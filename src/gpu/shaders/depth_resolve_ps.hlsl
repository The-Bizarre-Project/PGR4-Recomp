// MSAA depth to a single-sample depth texture (renderer.cpp ResolveMsaaDepth): D3D12 cannot
// resolve depth formats, so the first sample of each pixel is written as the depth.
Texture2DMS<float> gDepth : register(t0, space0);

float main(float4 pos : SV_Position) : SV_Depth {
  return gDepth.Load(int2(pos.xy), 0);
}
