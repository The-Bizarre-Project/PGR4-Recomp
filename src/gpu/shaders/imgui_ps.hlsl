// ImGui overlay pixel shader. Shader model 5.1 (fxc) so it runs on every
// D3D12 GPU, including SM 5.1-only drivers (e.g. GT 710 on 391.35) that
// can't create DXIL pipelines or survive large bindless tables. Each ImGui
// texture has its own one-entry descriptor set bound per draw.
//
// HDR output (gPaperScale > 0): the overlay's sRGB colours are decoded and
// drawn at paper white into the FP16 scRGB swapchain, like the game image.
Texture2D    gTexture : register(t0, space0);
SamplerState gSampler : register(s0, space1);

cbuffer HdrConstants : register(b1, space2) {
  float gPaperScale; // nits / 80; 0 = SDR output
  float3 gUnused;
};

struct PS_IN {
  float4 pos : SV_Position;
  float4 col : COLOR0;
  float2 uv  : TEXCOORD0;
};

float4 main(PS_IN i) : SV_Target {
  // ImGui textures are single-mip.
  float4 c = gTexture.SampleLevel(gSampler, i.uv, 0.0) * i.col;
  if (gPaperScale > 0.0) {
    c.rgb = pow(saturate(c.rgb), 2.2) * gPaperScale;
  }
  return c;
}
