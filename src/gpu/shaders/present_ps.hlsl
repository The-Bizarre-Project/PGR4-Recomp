// Front buffer blit: samples the guest front buffer's host copy, then maps
// each 8-bit channel through the guest's gamma ramp (256x1, identity when
// the game set none), as the 360's display controller does at scan-out.
//
// HDR output (gHdr.x > 0, an FP16 scRGB swapchain): the 360 sent an SDR
// signal to a TV with a 2.2 gamma. It is decoded to linear light, the game's
// white set at paper white, and the brightest part of the image lifted
// towards the display's peak (by luminance, so hues stay), as Windows' Auto
// HDR does for SDR games.
Texture2D    gSource  : register(t0, space0);
Texture2D    gGamma   : register(t1, space0);
SamplerState gSampler : register(s0, space1);

cbuffer HdrConstants : register(b0, space2) {
  float gPaperScale;    // nits / 80; 0 = SDR output
  float gHighlightGain; // white goes to (1 + gain) x paper white
  float gHighlightPower;
  float gFxaa;           // 1: FXAA on the source before the gamma ramp
};

// FXAA (after Timothy Lottes' FXAA, the compact span-based form): on edges found by luma
// contrast, blends along the edge's direction with samples up to kSpanMax texels away.
static const float kSpanMax = 8.0;
static const float kReduceMul = 1.0 / 8.0;
static const float kReduceMin = 1.0 / 128.0;

float Luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

float3 Fxaa(float2 uv) {
  float w, h;
  gSource.GetDimensions(w, h);
  const float2 texel = 1.0 / float2(w, h);
  const float3 nw = gSource.SampleLevel(gSampler, uv + float2(-1.0, -1.0) * texel, 0.0).rgb;
  const float3 ne = gSource.SampleLevel(gSampler, uv + float2(1.0, -1.0) * texel, 0.0).rgb;
  const float3 sw = gSource.SampleLevel(gSampler, uv + float2(-1.0, 1.0) * texel, 0.0).rgb;
  const float3 se = gSource.SampleLevel(gSampler, uv + float2(1.0, 1.0) * texel, 0.0).rgb;
  const float3 m = gSource.SampleLevel(gSampler, uv, 0.0).rgb;
  const float l_nw = Luma(nw), l_ne = Luma(ne), l_sw = Luma(sw), l_se = Luma(se), l_m = Luma(m);
  const float l_min = min(l_m, min(min(l_nw, l_ne), min(l_sw, l_se)));
  const float l_max = max(l_m, max(max(l_nw, l_ne), max(l_sw, l_se)));
  float2 dir = float2(-((l_nw + l_ne) - (l_sw + l_se)), (l_nw + l_sw) - (l_ne + l_se));
  const float reduce = max((l_nw + l_ne + l_sw + l_se) * (0.25 * kReduceMul), kReduceMin);
  const float rcp_min = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
  dir = clamp(dir * rcp_min, -kSpanMax, kSpanMax) * texel;
  const float3 a = 0.5 * (gSource.SampleLevel(gSampler, uv + dir * (1.0 / 3.0 - 0.5), 0.0).rgb +
                          gSource.SampleLevel(gSampler, uv + dir * (2.0 / 3.0 - 0.5), 0.0).rgb);
  const float3 b = a * 0.5 + 0.25 * (gSource.SampleLevel(gSampler, uv - dir * 0.5, 0.0).rgb +
                                     gSource.SampleLevel(gSampler, uv + dir * 0.5, 0.0).rgb);
  const float l_b = Luma(b);
  return (l_b < l_min || l_b > l_max) ? a : b;
}

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
  const float3 color = gFxaa > 0.5 ? Fxaa(uv) : gSource.SampleLevel(gSampler, uv, 0.0).rgb;
  const int3 index = int3(round(saturate(color) * 255.0));
  const float3 display = float3(gGamma.Load(int3(index.r, 0, 0)).r, gGamma.Load(int3(index.g, 0, 0)).g,
                                gGamma.Load(int3(index.b, 0, 0)).b);
  if (gPaperScale <= 0.0) {
    return float4(display, 1.0);
  }
  const float3 linear_rgb = pow(saturate(display), 2.2);
  const float luma = dot(linear_rgb, float3(0.2126, 0.7152, 0.0722));
  const float lift = 1.0 + gHighlightGain * pow(luma, gHighlightPower);
  return float4(linear_rgb * (lift * gPaperScale), 1.0);
}
