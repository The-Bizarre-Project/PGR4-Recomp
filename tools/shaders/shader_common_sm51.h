#ifndef SHADER_COMMON_H_INCLUDED
#define SHADER_COMMON_H_INCLUDED

// PGR4-Recomp shader model 5.1 (FXC) variant of XenosRecomp's shader_common.h.
// build_shader_cache.py swaps this in for the shader_common.h text that the
// PGR4_RECOMP XenosRecomp build embeds at the top of every shader, then
// compiles the result with fxc. It must stay in sync with the PGR4_RECOMP
// path of shader_common.h. Differences:
// - Bounded per-draw descriptor tables instead of bindless heaps, so the
//   cache runs on every D3D12 GPU, resource binding tier 1 included. The
//   renderer writes table-local slots into the *_DescriptorIndex constants.
// - Vertex shaders read their own SharedConstants at b3 (the script retargets
//   the cbuffer), so VS and PS sampler registers never share a slot.
// - No HLSL 2021: select() is a ternary macro, empty-argument macros
//   (g_SpecConstants(), DEFINE_SHARED_CONSTANTS()) are expanded by the
//   script, and no `texture` identifiers.
// - The SharedConstants tail (c16-c19) mirrors pgr4::gpu::SharedConstants in
//   src/gpu/renderer.cpp byte for byte.

#define SPEC_CONSTANT_R11G11B10_NORMAL  (1 << 0)
#define SPEC_CONSTANT_ALPHA_TEST        (1 << 1)

#define PGR4_RECOMP 1
#define PGR4_SM51 1

#define FLT_MIN asfloat(0xff7fffff)
#define FLT_MAX asfloat(0x7f7fffff)

// HLSL 2021 select() is a component-wise ternary, which is what SM 5 `?:` does.
#define select(condition, trueValue, falseValue) ((condition) ? (trueValue) : (falseValue))

// All 256 Xenos bool constants (VS 0-127, PS 128-255), one bit each.
#define g_BoolConstantWord(INDEX) g_BoolConstantWords[(INDEX) >> 2][(INDEX) & 3]
#define xenosBool(N) ((g_BoolConstantWord((N) >> 5) >> ((N) & 31)) & 1)

// Table sizes; the renderer's pipeline layout uses the same counts.
Texture2D<float4> g_Texture2DDescriptorHeap[32] : register(t0, space0);
Texture3D<float4> g_Texture3DDescriptorHeap[8] : register(t0, space1);
TextureCube<float4> g_TextureCubeDescriptorHeap[8] : register(t0, space2);
SamplerState g_SamplerDescriptorHeap[16] : register(s0, space3);

uint2 getTexture2DDimensions(uint resourceDescriptorIndex)
{
    uint2 dimensions;
    g_Texture2DDescriptorHeap[resourceDescriptorIndex].GetDimensions(dimensions.x, dimensions.y);
    return dimensions;
}

float4 tfetch2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
    return g_Texture2DDescriptorHeap[resourceDescriptorIndex].Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex],
        texCoord + offset / float2(getTexture2DDimensions(resourceDescriptorIndex)));
}

float2 getWeights2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
    float2 weights = frac(texCoord * float2(getTexture2DDimensions(resourceDescriptorIndex)) + offset - 0.5);
    return select(isnan(texCoord), 0.0, weights);
}

float w0(float a) { return (1.0f / 6.0f) * (a * (a * (-a + 3.0f) - 3.0f) + 1.0f); }
float w1(float a) { return (1.0f / 6.0f) * (a * a * (3.0f * a - 6.0f) + 4.0f); }
float w2(float a) { return (1.0f / 6.0f) * (a * (a * (-3.0f * a + 3.0f) + 3.0f) + 1.0f); }
float w3(float a) { return (1.0f / 6.0f) * (a * a * a); }
float g0(float a) { return w0(a) + w1(a); }
float g1(float a) { return w2(a) + w3(a); }
float h0(float a) { return -1.0f + w1(a) / (w0(a) + w1(a)) + 0.5f; }
float h1(float a) { return 1.0f + w3(a) / (w2(a) + w3(a)) + 0.5f; }

float4 tfetch2DBicubic(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
    float2 dimensions = float2(getTexture2DDimensions(resourceDescriptorIndex));
    float x = texCoord.x * dimensions.x + offset.x - 0.5f;
    float y = texCoord.y * dimensions.y + offset.y - 0.5f;
    float px = floor(x);
    float py = floor(y);
    float fx = x - px;
    float fy = y - py;
    float g0x = g0(fx);
    float g1x = g1(fx);
    float h0x = h0(fx);
    float h1x = h1(fx);
    float h0y = h0(fy);
    float h1y = h1(fy);
    SamplerState s = g_SamplerDescriptorHeap[samplerDescriptorIndex];
    return
        g0(fy) * (g0x * g_Texture2DDescriptorHeap[resourceDescriptorIndex].Sample(s, float2(px + h0x, py + h0y) / dimensions) +
                  g1x * g_Texture2DDescriptorHeap[resourceDescriptorIndex].Sample(s, float2(px + h1x, py + h0y) / dimensions)) +
        g1(fy) * (g0x * g_Texture2DDescriptorHeap[resourceDescriptorIndex].Sample(s, float2(px + h0x, py + h1y) / dimensions) +
                  g1x * g_Texture2DDescriptorHeap[resourceDescriptorIndex].Sample(s, float2(px + h1x, py + h1y) / dimensions));
}

float4 tfetch3D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord)
{
    return g_Texture3DDescriptorHeap[resourceDescriptorIndex].Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord);
}

struct CubeMapData
{
    float3 cubeMapDirections[2];
    uint cubeMapIndex;
};

float4 tfetchCube(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord, inout CubeMapData cubeMapData)
{
    return g_TextureCubeDescriptorHeap[resourceDescriptorIndex].Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], cubeMapData.cubeMapDirections[texCoord.z]);
}

float2 pgr4Texture2DSize(uint resourceDescriptorIndex)
{
    return float2(getTexture2DDimensions(resourceDescriptorIndex));
}

// 1D fetch coordinate with the instruction's texel offset; denorm coordinates
// are in texels (skinning matrix palettes: bone * 3 + row).
float tfetch1DCoord(uint resourceDescriptorIndex, float texCoord, float offset, bool denorm)
{
    float width = pgr4Texture2DSize(resourceDescriptorIndex).x;
    return denorm ? (texCoord + offset) / width : texCoord + offset / width;
}

// 1D textures are created as Width x 1 2D textures and share the 2D table.
float4 tfetch1D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float texCoord, float offset, bool denorm)
{
    texCoord = tfetch1DCoord(resourceDescriptorIndex, texCoord, offset, denorm);
    return g_Texture2DDescriptorHeap[resourceDescriptorIndex].Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], float2(texCoord, 0.5));
}

// Vertex texture fetch has no derivatives, so vertex shaders sample mip 0.
float4 tfetchLevel1D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float texCoord, float offset, bool denorm)
{
    texCoord = tfetch1DCoord(resourceDescriptorIndex, texCoord, offset, denorm);
    return g_Texture2DDescriptorHeap[resourceDescriptorIndex].SampleLevel(g_SamplerDescriptorHeap[samplerDescriptorIndex], float2(texCoord, 0.5), 0.0);
}

float4 tfetchLevel2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
    return g_Texture2DDescriptorHeap[resourceDescriptorIndex].SampleLevel(g_SamplerDescriptorHeap[samplerDescriptorIndex],
        texCoord + offset / float2(getTexture2DDimensions(resourceDescriptorIndex)), 0.0);
}

float4 tfetchLevel3D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord)
{
    return g_Texture3DDescriptorHeap[resourceDescriptorIndex].SampleLevel(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord, 0.0);
}

float4 tfetchLevelCube(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord, inout CubeMapData cubeMapData)
{
    return g_TextureCubeDescriptorHeap[resourceDescriptorIndex].SampleLevel(g_SamplerDescriptorHeap[samplerDescriptorIndex], cubeMapData.cubeMapDirections[texCoord.z], 0.0);
}

// Explicit LOD (setTexLOD register and/or the instruction's bias) and
// computed LOD with the instruction's bias.
float4 tfetchLod1D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float texCoord, float offset, bool denorm, float lod)
{
    texCoord = tfetch1DCoord(resourceDescriptorIndex, texCoord, offset, denorm);
    return g_Texture2DDescriptorHeap[resourceDescriptorIndex].SampleLevel(g_SamplerDescriptorHeap[samplerDescriptorIndex], float2(texCoord, 0.5), lod);
}

float4 tfetchLod2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset, float lod)
{
    return g_Texture2DDescriptorHeap[resourceDescriptorIndex].SampleLevel(g_SamplerDescriptorHeap[samplerDescriptorIndex],
        texCoord + offset / float2(getTexture2DDimensions(resourceDescriptorIndex)), lod);
}

float4 tfetchLod3D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord, float lod)
{
    return g_Texture3DDescriptorHeap[resourceDescriptorIndex].SampleLevel(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord, lod);
}

float4 tfetchLodCube(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord, inout CubeMapData cubeMapData, float lod)
{
    return g_TextureCubeDescriptorHeap[resourceDescriptorIndex].SampleLevel(g_SamplerDescriptorHeap[samplerDescriptorIndex], cubeMapData.cubeMapDirections[texCoord.z], lod);
}

float4 tfetchBias1D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float texCoord, float offset, bool denorm, float bias)
{
    texCoord = tfetch1DCoord(resourceDescriptorIndex, texCoord, offset, denorm);
    return g_Texture2DDescriptorHeap[resourceDescriptorIndex].SampleBias(g_SamplerDescriptorHeap[samplerDescriptorIndex], float2(texCoord, 0.5), bias);
}

float4 tfetchBias2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset, float bias)
{
    return g_Texture2DDescriptorHeap[resourceDescriptorIndex].SampleBias(g_SamplerDescriptorHeap[samplerDescriptorIndex],
        texCoord + offset / float2(getTexture2DDimensions(resourceDescriptorIndex)), bias);
}

float4 tfetchBias3D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord, float bias)
{
    return g_Texture3DDescriptorHeap[resourceDescriptorIndex].SampleBias(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord, bias);
}

float4 tfetchBiasCube(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord, inout CubeMapData cubeMapData, float bias)
{
    return g_TextureCubeDescriptorHeap[resourceDescriptorIndex].SampleBias(g_SamplerDescriptorHeap[samplerDescriptorIndex], cubeMapData.cubeMapDirections[texCoord.z], bias);
}

// getGradients: Xenos packs { ddx(src.x), ddy(src.x), ddx(src.y), ddy(src.y) }.
float4 getGradients2D(float2 value)
{
    return float4(ddx_coarse(value.x), ddy_coarse(value.x), ddx_coarse(value.y), ddy_coarse(value.y));
}

// The flags are passed in by the tfetchR11G11B10 macro at the call site,
// where the shader's SharedConstants cbuffer is already declared.
float4 pgr4FetchR11G11B10(uint4 value, uint specConstants)
{
    if (specConstants & SPEC_CONSTANT_R11G11B10_NORMAL)
    {
        return float4(
            ((value.x & 0x00000400) ? -1.0 : 0.0) + ((value.x & 0x3FF) / 1024.0),
            ((value.x & 0x00200000) ? -1.0 : 0.0) + (((value.x >> 11) & 0x3FF) / 1024.0),
            ((value.x & 0x80000000) ? -1.0 : 0.0) + (((value.x >> 22) & 0x1FF) / 512.0),
            0.0);
    }
    return asfloat(value);
}
#define tfetchR11G11B10(VALUE) pgr4FetchR11G11B10((VALUE), g_SpecConstantsValue)

// Vertex inputs arrive as raw uint4 and are decoded per draw: g_InputModes
// (VS shared constants) holds 2 bits per input in declaration order, set by
// the renderer from the vertex declaration's element format:
// 0 = float bits (FLOAT/UNORM/SNORM formats), 1 = signed integers (SHORT*,
// BYTE4), 2 = unsigned integers (UBYTE4, USHORT*), 3 = signed normalized
// 10:10:10:2 in .x (DEC3N/DEC4N). Xenos converts these in the fetch, which no
// DXGI format does.
#define PGR4_INPUT_MODE(INDEX) ((g_InputModes >> ((INDEX) * 2)) & 3)
// Components the input's vertex format lacks (g_InputMissing, 2 bits per
// input): filled as the Xenos does, 0 for y and z, 1 for w. The input
// assembler's own fill is not used: for a uint4 input AMD's drivers under
// vkd3d-proton give integer 1 for a float format's w, which decodes to ~0.
#define PGR4_INPUT_MISSING(INDEX) ((g_InputMissing >> ((INDEX) * 2)) & 3)
#define pgr4InputFloat(RAW, INDEX) \
    pgr4PadInput(pgr4DecodeInput((RAW), PGR4_INPUT_MODE(INDEX)), PGR4_INPUT_MISSING(INDEX))
#define pgr4InputBits(RAW, INDEX) asuint(pgr4DecodeInput((RAW), PGR4_INPUT_MODE(INDEX)))
#define pgr4InputIndex(RAW, INDEX) pgr4DecodeIndex((RAW), PGR4_INPUT_MODE(INDEX))

float4 pgr4DecodeInput(uint4 raw, uint mode)
{
    if (mode == 1)
        return float4(asint(raw));
    if (mode == 2)
        return float4(raw);
    if (mode == 3)
    {
        int4 v = int4(int(raw.x << 22) >> 22, int(raw.x << 12) >> 22, int(raw.x << 2) >> 22, int(raw.x) >> 30);
        return max(float4(v) / float4(511.0, 511.0, 511.0, 1.0), -1.0);
    }
    return asfloat(raw);
}

float4 pgr4PadInput(float4 value, uint missing)
{
    return float4(value.x, missing > 2 ? 0.0 : value.y, missing > 1 ? 0.0 : value.z,
                  missing > 0 ? 1.0 : value.w);
}

uint4 pgr4DecodeIndex(uint4 raw, uint mode)
{
    if (mode == 0)
        return uint4(asfloat(raw));
    return raw;
}

// Vertex fetch indexed by a register other than r0.x (XenosRecomp's
// PGR4_VFETCH): the stream is read directly. The renderer binds every vertex
// stream as a raw buffer (endian-swapped, guest component order) and
// describes each VS input in g_FetchLayouts, two dwords per input in
// declaration order: x = valid | stream << 1 | byte offset << 5 |
// byte stride << 16, y = the D3DDECLTYPE (format, signed, integer, swizzle).
// Xenos takes component X of 16-bit pairs from the high half of the dword
// and X of 8-bit and 10-bit packs from the low bits.
ByteAddressBuffer g_VertexStreams[16] : register(t0, space4);

float pgr4Norm(uint bits, uint width, bool isSigned, bool isInteger)
{
    if (isSigned)
    {
        int v = int(bits << (32 - width)) >> (32 - width);
        return isInteger ? float(v) : max(float(v) / float((1u << (width - 1)) - 1), -1.0);
    }
    return isInteger ? float(bits) : float(bits) / float((1u << width) - 1);
}

float4 pgr4VFetch(float4 fallback, float index, uint rounded, uint2 layout)
{
    if ((layout.x & 1) == 0)
        return fallback;
    uint stream = (layout.x >> 1) & 15;
    uint offset = (layout.x >> 5) & 0x7FF;
    uint stride = layout.x >> 16;
    uint type = layout.y;
    uint vertex = uint(max(rounded != 0 ? floor(index + 0.5) : floor(index), 0.0));
    // Bounds-checked here, not left to the driver: the index is guest data
    // (the crowd's per-instance index), and a stray one put the read
    // gigabytes past the stream, a GPU page fault on AMD. Checked before the
    // multiply so it cannot wrap.
    uint streamBytes;
    g_VertexStreams[stream].GetDimensions(streamBytes);
    if (stride != 0 && vertex >= streamBytes / stride)
        return fallback;
    uint address = vertex * stride + offset;
    if (address + 4 > streamBytes)
        return fallback;
    bool isSigned = ((type >> 8) & 1) != 0;
    bool isInteger = ((type >> 9) & 1) != 0;
    float4 c = float4(0.0, 0.0, 0.0, 1.0);
    uint4 d = g_VertexStreams[stream].Load4(address);
    switch (type & 0x3F)
    {
    case 6: // 8_8_8_8
        c = float4(pgr4Norm(d.x & 0xFF, 8, isSigned, isInteger), pgr4Norm((d.x >> 8) & 0xFF, 8, isSigned, isInteger),
                   pgr4Norm((d.x >> 16) & 0xFF, 8, isSigned, isInteger), pgr4Norm(d.x >> 24, 8, isSigned, isInteger));
        break;
    case 7: // 2_10_10_10
        c = float4(pgr4Norm(d.x & 0x3FF, 10, isSigned, isInteger), pgr4Norm((d.x >> 10) & 0x3FF, 10, isSigned, isInteger),
                   pgr4Norm((d.x >> 20) & 0x3FF, 10, isSigned, isInteger), pgr4Norm(d.x >> 30, 2, isSigned, isInteger));
        break;
    // The crowd's per-sprite data (UDHEN3N / UHEND3). Without these the fetch
    // fell back to the input assembler, which reads the vertex's own record:
    // each corner of a sprite took another sprite's position (slabs).
    case 16: // 10_11_11: x 11 bits, y 11, z 10 (x lowest)
        c.xyz = float3(pgr4Norm(d.x & 0x7FF, 11, isSigned, isInteger), pgr4Norm((d.x >> 11) & 0x7FF, 11, isSigned, isInteger),
                       pgr4Norm(d.x >> 22, 10, isSigned, isInteger));
        break;
    case 17: // 11_11_10: x 10 bits, y 11, z 11 (x lowest)
        c.xyz = float3(pgr4Norm(d.x & 0x3FF, 10, isSigned, isInteger), pgr4Norm((d.x >> 10) & 0x7FF, 11, isSigned, isInteger),
                       pgr4Norm(d.x >> 21, 11, isSigned, isInteger));
        break;
    case 25: // 16_16
        c.xy = float2(pgr4Norm(d.x >> 16, 16, isSigned, isInteger), pgr4Norm(d.x & 0xFFFF, 16, isSigned, isInteger));
        break;
    case 26: // 16_16_16_16
        c = float4(pgr4Norm(d.x >> 16, 16, isSigned, isInteger), pgr4Norm(d.x & 0xFFFF, 16, isSigned, isInteger),
                   pgr4Norm(d.y >> 16, 16, isSigned, isInteger), pgr4Norm(d.y & 0xFFFF, 16, isSigned, isInteger));
        break;
    case 31: // 16_16_FLOAT
        c.xy = f16tof32(uint2(d.x >> 16, d.x));
        break;
    case 32: // 16_16_16_16_FLOAT
        c = f16tof32(uint4(d.x >> 16, d.x, d.y >> 16, d.y));
        break;
    case 36: // 32_FLOAT
        c.x = asfloat(d.x);
        break;
    case 37: // 32_32_FLOAT
        c.xy = asfloat(d.xy);
        break;
    case 57: // 32_32_32_FLOAT
        c.xyz = asfloat(d.xyz);
        break;
    case 38: // 32_32_32_32_FLOAT
        c = asfloat(d);
        break;
    default:
        return fallback;
    }
    // D3DDECLTYPE swizzle: 3 bits per component, 0-3 select, 4 = 0, 5 = 1.
    float4 r;
    [unroll] for (uint i = 0; i < 4; ++i)
    {
        uint sel = (type >> (10 + i * 3)) & 7;
        r[i] = sel < 4 ? c[sel] : (sel == 5 ? 1.0 : 0.0);
    }
    return r;
}
#define pgr4VFetchInput(VALUE, INPUT, INDEX, ROUNDED) \
    pgr4VFetch((VALUE), (INDEX), (ROUNDED), ((INPUT) & 1) ? g_FetchLayouts[(INPUT) >> 1].zw : g_FetchLayouts[(INPUT) >> 1].xy)

float4 tfetchTexcoord(uint swappedTexcoords, float4 value, uint semanticIndex)
{
    return (swappedTexcoords & (1u << semanticIndex)) != 0 ? value.yxwz : value;
}

float4 cube(float4 value, inout CubeMapData cubeMapData)
{
    uint index = cubeMapData.cubeMapIndex;
    cubeMapData.cubeMapDirections[index] = value.xyz;
    ++cubeMapData.cubeMapIndex;
    return float4(0.0, 0.0, 0.0, index);
}

float4 dst(float4 src0, float4 src1)
{
    float4 dest;
    dest.x = 1.0;
    dest.y = src0.y * src1.y;
    dest.z = src0.z;
    dest.w = src1.w;
    return dest;
}

float4 max4(float4 src0)
{
    return max(max(src0.x, src0.y), max(src0.z, src0.w));
}

float2 getPixelCoord(uint resourceDescriptorIndex, float2 texCoord)
{
    return float2(getTexture2DDimensions(resourceDescriptorIndex)) * texCoord;
}

float computeMipLevel(float2 pixelCoord)
{
    float2 dx = ddx(pixelCoord);
    float2 dy = ddy(pixelCoord);
    float deltaMaxSqr = max(dot(dx, dx), dot(dy, dy));
    return max(0.0, 0.5 * log2(deltaMaxSqr));
}

#endif
