// Single-sample depth into a depth texture (renderer.cpp ResolveMsaaDepth): D3D12 copies depth
// only as whole subresources, so a resolve into a larger texture (3-player wide split screen:
// 640x360 views into textures sized for the 1280x360 one) is drawn instead.
Texture2D<float> gDepth : register(t0, space0);

float main(float4 pos : SV_Position) : SV_Depth {
  return gDepth.Load(int3(pos.xy, 0));
}
