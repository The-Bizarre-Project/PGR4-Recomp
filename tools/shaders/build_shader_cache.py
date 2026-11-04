#!/usr/bin/env python3
"""Build the PGR4 native renderer's shader cache (shader model 5.1 DXBC).

Takes the XenosRecomp HLSL from decompile_shaders.py (shaders/hlsl) and the
matching containers from dump_shaders.py (shaders/bin/xex), rewrites each
shader for fxc with shader_common_sm51.h and packs the DXBC into one file the
renderer loads at boot:

  header  : 'PGR4SC51', u32 version, u32 entry count
  entries : u64 key, u32 blob offset, u32 blob size, u32 stage (1 PS; VS: 0, | 2 reads the vertex index, | 4 indexed vertex fetch),
            u32 input offset, u32 input count, u32 sampler register mask
  inputs  : per VS input u8 usage (D3DDECLUSAGE), u8 usage index, u8 kind
            (0 float4, 1 uint4 float bits, 2 uint4 integers), u8 reserved
  blobs   : DXBC, 16-byte aligned

key is XXH64 (seed 0) over the whole container (virtualSize + physicalSize
bytes), which is what the renderer hashes in D3DDevice_CreateVertexShader /
CreatePixelShader. All values are little-endian.

  python tools/shaders/build_shader_cache.py --out <build>/shader_cache_sm51.bin
"""
import argparse
import concurrent.futures
import os
import re
import shutil
import struct
import subprocess
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))

USAGES = {
    "POSITION": 0, "BLENDWEIGHT": 1, "BLENDINDICES": 2, "NORMAL": 3, "PSIZE": 4,
    "TEXCOORD": 5, "TANGENT": 6, "BINORMAL": 7, "TESSFACTOR": 8, "POSITIONT": 9,
    "COLOR": 10, "FOG": 11, "DEPTH": 12, "SAMPLE": 13,
}

# --- XXH64 ------------------------------------------------------------------
P1, P2, P3, P4, P5 = (0x9E3779B185EBCA87, 0xC2B2AE3D27D4EB4F, 0x165667B19E3779F9,
                      0x85EBCA77C2B2AE63, 0x27D4EB2F165667C5)
M = 0xFFFFFFFFFFFFFFFF


def _rotl(x, r):
    return ((x << r) | (x >> (64 - r))) & M


def _round(acc, lane):
    acc = (acc + lane * P2) & M
    return (_rotl(acc, 31) * P1) & M


def _merge(acc, val):
    acc ^= _round(0, val)
    return (acc * P1 + P4) & M


def xxh64(data, seed=0):
    n = len(data)
    i = 0
    if n >= 32:
        v1, v2, v3, v4 = (seed + P1 + P2) & M, (seed + P2) & M, seed, (seed - P1) & M
        while i + 32 <= n:
            a, b, c, d = struct.unpack_from("<4Q", data, i)
            v1, v2, v3, v4 = _round(v1, a), _round(v2, b), _round(v3, c), _round(v4, d)
            i += 32
        h = (_rotl(v1, 1) + _rotl(v2, 7) + _rotl(v3, 12) + _rotl(v4, 18)) & M
        for v in (v1, v2, v3, v4):
            h = _merge(h, v)
    else:
        h = (seed + P5) & M
    h = (h + n) & M
    while i + 8 <= n:
        (k,) = struct.unpack_from("<Q", data, i)
        h ^= _round(0, k)
        h = (_rotl(h, 27) * P1 + P4) & M
        i += 8
    if i + 4 <= n:
        (k,) = struct.unpack_from("<I", data, i)
        h ^= (k * P1) & M
        h = (_rotl(h, 23) * P2 + P3) & M
        i += 4
    while i < n:
        h ^= (data[i] * P5) & M
        h = (_rotl(h, 11) * P1) & M
        i += 1
    h ^= h >> 33
    h = (h * P2) & M
    h ^= h >> 29
    h = (h * P3) & M
    h ^= h >> 32
    return h


assert xxh64(b"") == 0xEF46DB3751D8E999

# --- HLSL rewrite -----------------------------------------------------------
SHARED_TAIL = (
    "uint g_Booleans : packoffset(c16.x);\n"
    "\tuint g_SwappedTexcoords : packoffset(c16.y);\n"
    "\tfloat2 g_HalfPixelOffset : packoffset(c16.z);\n"
    "\tfloat g_AlphaThreshold : packoffset(c17.x);\n"
    "\tuint g_SpecConstantsValue : packoffset(c17.y);\n"
    "\tuint g_InputModes : packoffset(c17.z);\n"
    "\tint g_BaseVertex : packoffset(c17.w);\n"
    "\tuint4 g_BoolConstantWords[2] : packoffset(c18);\n"
    "\tuint4 g_FetchLayouts[8] : packoffset(c20);\n"
    "\tfloat2 g_NdcScale : packoffset(c28.x);\n"
    "\tfloat2 g_PixelScale : packoffset(c28.z);\n"
    "\tfloat g_UiSqueeze : packoffset(c29.x);\n"
    "\tfloat3 g_OutputRange : packoffset(c29.y);\n"
    "\tfloat g_DepthOffset : packoffset(c30.x);\n"
    "\tfloat g_UiSqueezeY : packoffset(c30.y);\n"
    "\tfloat g_UiEdgeShiftY : packoffset(c30.z);\n"
    "\tuint g_InputMissing : packoffset(c30.w);"
)


PS_MAIN = re.compile(r"^void main\((.*?)\)\n\{", re.M | re.S)


def clamp_pixel_output(body):
    """Keep oC0 inside what the guest render target can hold (renderer.cpp,
    c29.yzw: low, high, alpha high; high 0 = unclamped). The Xenos stores
    2_10_10_10_FLOAT as 0..31.875 and the fixed 16-bit formats as -32..32, so
    an overflowing pixel saturates there; the host's float16 copies would
    keep it, up to infinity, and one such pixel in the HDR scene drives the
    auto-exposure to black. main() runs the shader as xenos_main() and then
    clamps."""
    m = PS_MAIN.search(body)
    if not m or "out float4 oC0 : SV_Target0" not in m.group(1):
        return body
    params = m.group(1)
    names = [p.split(":")[0].split()[-1] for p in params.split(",") if p.strip()]
    wrapper = ("\nvoid main(" + params + ")\n{\n\txenos_main(" + ", ".join(names) + ");\n"
               "\tif (g_OutputRange.y > 0.0)\n\t{\n"
               "\t\toC0 = select(isnan(oC0), 0.0, oC0);\n"
               "\t\toC0.rgb = clamp(oC0.rgb, g_OutputRange.x, g_OutputRange.y);\n"
               "\t\toC0.a = clamp(oC0.a, g_OutputRange.x, g_OutputRange.z);\n"
               "\t}\n}\n")
    return body[:m.start()] + "void xenos_main(" + params + ")\n{" + body[m.end():] + wrapper


CONST_CBUFFER = re.compile(r"^cbuffer (VertexShaderConstants|PixelShaderConstants) : register\((\w+), (\w+)\)\n\{\n(.*?)^\};\n",
                           re.M | re.S)
CONST_MEMBER = re.compile(r"^\tfloat4 (\w+)(?:\[(\d+)\])? : packoffset\(c(\d+)\);$")
# XenosRecomp's guard, signed in HLSL generated before PGR4_RECOMP made it unsigned.
CONST_INDEXED = re.compile(r"^#define (\w+)\(INDEX\) select\((?:\(INDEX\)|uint\(INDEX\)) < (\d+)u?, "
                           r"\1\[min\((?:INDEX|uint\(INDEX\)), \d+u?\)\], 0\.0\)$")


def flatten_constant_buffers(body):
    """Declare the float constants as one 256-register array.

    Xenos relative addressing (c[a0 + n]) reaches past the end of the array
    the game named, into the following registers; XenosRecomp bounds the
    index with the register file (`select(INDEX < N, ...)`), but the HLSL
    array keeps its declared size, and D3D12 reads 0 for any constant past
    the end of the declared cbuffer. Every named constant becomes a macro
    over the raw register array instead. The index is compared unsigned, so
    a negative one (a0 from a NaN) reads 0: signed, it passed the bound and
    indexed the root CBV backwards, an unbounded read that faults on AMD
    (NVIDIA bounds constant reads).
    """
    offsets = {}

    def rewrite(m):
        name, reg, space, members = m.groups()
        raw = "g_%sRaw" % name
        defines = []
        for line in members.split("\n"):
            if not line.strip():
                continue
            mm = CONST_MEMBER.match(line)
            if mm:
                member, count, offset = mm.groups()
                if count is None:
                    defines.append("#define %s %s[%s]" % (member, raw, offset))
                else:
                    offsets[member] = offset
                continue
            mi = CONST_INDEXED.match(line)
            if mi:
                member, bound = mi.groups()
                defines.append("#define %s(INDEX) select(uint(INDEX) < %su, %s[%s + min(uint(INDEX), %du)], 0.0)"
                               % (member, bound, raw, offsets[member], int(bound) - 1))
                continue
            raise ValueError("unexpected constant declaration: " + line)
        # c[a0 + n] on a register the game did not declare as an array.
        defines.append("#define g_RelativeConstant(INDEX) select(uint(INDEX) < 256u, %s[min(uint(INDEX), 255u)], 0.0)"
                       % raw)
        return ("cbuffer %s : register(%s, %s)\n{\n\tfloat4 %s[256];\n};\n" % (name, reg, space, raw)
                + "\n".join(defines) + "\n")

    return CONST_CBUFFER.sub(rewrite, body)


LOAD_ONE = re.compile(r"\babs\(r\d+\.[xyzw]\) >= 0\.0\b")


def to_sm51(hlsl, common, sm51, is_pixel):
    """Rewrite XenosRecomp HLSL (shader_common.h embedded at the top) for fxc."""
    hlsl = hlsl.replace("\r\n", "\n")
    if not hlsl.startswith(common):
        raise ValueError("HLSL does not start with the PGR4_RECOMP shader_common.h")
    body = hlsl[len(common):]
    # Only the D3D branch of the constant declarations is wanted.
    body = re.sub(r"#ifdef __spirv__\n.*?\n#else\n(.*?)\n#endif\n", r"\1\n", body, count=1, flags=re.S)
    body = body.replace("#ifndef __spirv__\n", "").replace('[shader("pixel")]\n#endif\n', "")
    body = body.replace('[shader("vertex")]\n#endif\n', "")
    body = re.sub(r"\[\[vk::[^\]]*\]\]\s*", "", body)
    body = body.replace("#ifdef __spirv__\n\tin bool iFace : SV_IsFrontFace\n#else\n\tin uint iFace : SV_IsFrontFace\n#endif\n",
                        "\tin bool iFace : SV_IsFrontFace\n")
    body = body.replace("DEFINE_SHARED_CONSTANTS();", SHARED_TAIL)
    body = body.replace("g_SpecConstants()", "g_SpecConstantsValue")
    # Screen position (Xenos vPos) in guest pixels: at an internal resolution
    # above 720p, SV_Position counts host pixels (renderer.cpp, c28.z).
    body = body.replace("(iPos.xy - 0.5)", "(iPos.xy * g_PixelScale - 0.5)")
    # `ps = abs(rN) >= 0.0` is the Xenos compiler's way of loading 1.0 (667
    # shaders). fxc assumes no NaN and folds it to 1; dxc's strict IEEE (-Gis)
    # gives 0 for a NaN register, and the crowd's w came out 0 (a corner at
    # infinity: black slabs and wedges across the scene on DXIL GPUs).
    body = LOAD_ONE.sub("1.0", body)
    body = flatten_constant_buffers(body)
    # fxc will not expand `#define g_X(INDEX) ... g_X[...]`; rename the array.
    for name in set(re.findall(r"#define (\w+)\(INDEX\) select\((?:\(INDEX\)|uint\(INDEX\)) < \d+u?, \1\[", body)):
        body = re.sub(r"\b%s\[" % re.escape(name), name + "_cb[", body)
    body = dedupe_sampler_offsets(body)
    body = unswitch_control_flow(body)
    if is_pixel:
        # Alpha test: the Xenos compares in fixed function, where a NaN alpha
        # fails and the pixel is killed; clip() keeps NaN (NaN < 0 is false),
        # which let whole crowd-sprite quads through as black.
        body = body.replace("clip(oC0.w - g_AlphaThreshold);",
                            "if (isnan(oC0.w) || oC0.w < g_AlphaThreshold) discard;")
        body = clamp_pixel_output(body)
    if not is_pixel:
        body = decode_vertex_inputs(body)
        body = seed_vertex_index(body)
    if not is_pixel:
        # Ultrawide (renderer.cpp, c29.x): the renderer squeezes 2D draws by
        # scaling their PosScale; a vertex that lands on a squeezed screen
        # corner belongs to a full-screen quad (backdrop, tint, pasted scene)
        # and is widened back out to the window's corner.
        epilogue = "\toPos.xy = oPos.xy * g_NdcScale + g_HalfPixelOffset * oPos.w;\n"
        if epilogue not in body:
            raise ValueError("vertex shader without the g_NdcScale epilogue")
        # Constant polygon offset (renderer.cpp, c30.x), in depth units like
        # the Xenos's; kept short of the near plane, since hardware bias is
        # applied after clipping.
        # A NaN in any vertex culls the whole primitive on the Xenos, and PGR4
        # relies on it (its crowd sprites get NaN corners to drop them). Host
        # GPUs rasterize such triangles stretched to infinity ("the void"). A
        # NaN vertex moves to the screen centre with a clip distance of
        # -1e30 against +1 at the others, which clips all of every triangle
        # it is in but a sliver too thin to cover a pixel. Bit test: fxc may
        # fold isnan away.
        # Last output: the pixel shaders' inputs must stay a prefix of the
        # vertex shader's outputs.
        main = body.find("void main(")
        start = body.find("\n{\n", main)
        if main < 0 or start < 0 or body[start - 1] != ")":
            raise ValueError("vertex shader main not found")
        body = (body[:start - 1] + ",\n\tout float oCullNaN : SV_ClipDistance0)\n{\n\toCullNaN = 1.0;\n" +
                body[start + 3:])
        # A NaN interpolant blackens its whole triangle (PGR4's 250 GTO body:
        # the Xenos gives 0 for 0 * Inf where the host gives NaN). Patching
        # every multiply that way instead made the scene flicker.
        interpolants = "".join(
            f"\t{name} = select((asuint({name}) & 0x7FFFFFFF) > 0x7F800000, 0.0, {name});\n"
            for name in re.findall(r"out float4 (o(?:TexCoord\d+|Color\d|Normal0|Tangent\d)) :", body))
        body = body.replace(epilogue, epilogue + interpolants +
                            "\tif (any((asuint(oPos) & 0x7FFFFFFF) >= 0x7F800000))\n\t{\n"
                            "\t\toPos = float4(0.0, 0.0, 0.0, 1.0);\n\t\toCullNaN = -1e30;\n\t}\n")
        body = body.replace(epilogue,
                            "\tif (g_DepthOffset != 0.0) oPos.z = min(oPos.z + g_DepthOffset * oPos.w, abs(oPos.w));\n"
                            "\tif (g_UiSqueeze < 1.0 && "
                            "abs(abs(oPos.x) - g_UiSqueeze * abs(oPos.w)) <= 0.005 * abs(oPos.w) && "
                            "abs(abs(oPos.y) - abs(oPos.w)) <= 0.005 * abs(oPos.w)) oPos.x /= g_UiSqueeze;\n"
                            "\tif (g_UiSqueezeY < 1.0 && "
                            "abs(abs(oPos.y) - g_UiSqueezeY * abs(oPos.w)) <= 0.005 * abs(oPos.w) && "
                            "abs(abs(oPos.x) - abs(oPos.w)) <= 0.005 * abs(oPos.w)) oPos.y /= g_UiSqueezeY;\n"
                            # A race HUD batch across the thirds (renderer.cpp, c30.z): each
                            # vertex in the (squeezed) top or bottom third moves to that edge.
                            "\tif (g_UiEdgeShiftY != 0.0 && abs(oPos.y) > g_UiSqueezeY / 3.0 * abs(oPos.w)) "
                            "oPos.y += sign(oPos.y) * g_UiEdgeShiftY * abs(oPos.w);\n"
                            + epilogue)
    if not is_pixel:
        # VS samplers get their own shared constants (see shader_common_sm51.h).
        body = body.replace("cbuffer SharedConstants : register(b2, space4)",
                            "cbuffer SharedConstants : register(b3, space4)")
    return sm51 + body


PACKED = re.compile(r"^\t(float4|uint) (\w+)(?:\[(\d+)\])? : packoffset\((c\d+(?:\.[xyzw])?)\);\n", re.M)


def dedupe_sampler_offsets(body):
    """Some shaders name one constant or sampler register twice; fxc rejects
    the shared packoffset, so the later name becomes a macro for the first
    (the larger array keeps the slot)."""
    first = {}
    for m in PACKED.finditer(body):
        kind, name, count, offset = m.group(1), m.group(2), int(m.group(3) or 0), m.group(4)
        key = (kind, offset)
        if key not in first or count > first[key][1]:
            first[key] = (name, count)
    aliases = []

    def repl(m):
        name, offset = m.group(2), m.group(4)
        keep = first[(m.group(1), offset)][0]
        if keep == name:
            return m.group(0)
        aliases.append(f"#define {name} {keep}\n")
        return ""

    body = PACKED.sub(repl, body)
    if aliases:
        body = body.replace("cbuffer ", "".join(aliases) + "cbuffer ", 1)
    return body


def unswitch_control_flow(body):
    """XenosRecomp lowers non-structured control flow to `while (true) {
    switch (pc) { case N: ... } }` with fall-through cases, which fxc rejects.
    Each case becomes `if (pc <= N)`: jumps always assign pc and `continue`,
    and `break` still leaves the loop, so the order of execution is unchanged."""
    start = body.find("\t\tswitch (pc)\n\t\t{\n")
    if start < 0:
        return body
    end = body.find("\n\t\t}\n\t\tbreak;\n\t}", start)
    if end < 0:
        raise ValueError("unterminated switch (pc)")
    inner = body[start + len("\t\tswitch (pc)\n\t\t{\n"):end]
    first = [True]

    def case(m):
        prefix = "" if first[0] else "\t\t}\n"
        first[0] = False
        return f"{prefix}\t\tif (pc <= {m.group(1)})\n\t\t{{"

    loop_open = "\twhile (true)\n\t{\n"
    loop_head = body.rfind(loop_open, 0, start)
    tail = body[end:]
    if (loop_head >= 0 and loop_head + len(loop_open) == start and forward_only(inner)
            and tail.startswith("\n\t\t}\n\t\tbreak;\n\t}\n}")):
        # Forward jumps only: no loop is needed, which keeps fxc from forcing
        # an unroll around the gradient fetches. Skipped blocks fail their
        # `pc <=` test, and leaving the loop is leaving main.
        inner = re.sub(r"(pc = \d+;)\n(\s*)continue;", r"\1", inner)
        inner = re.sub(r"\bbreak;", "return;", inner)
        inner = re.sub(r"^\t\tcase (\d+):", case, inner, flags=re.M)
        return body[:loop_head] + "\t{\n" + inner + "\n\t\t}\n\t}\n}" + tail[len("\n\t\t}\n\t\tbreak;\n\t}\n}"):]
    inner = re.sub(r"^\t\tcase (\d+):", case, inner, flags=re.M)
    return body[:start] + "\t\t{\n" + inner + "\n\t\t}" + body[end:]


def forward_only(inner):
    """Every jump targets a later case and ends its case (nothing runs after
    the `continue` inside that case)."""
    parts = re.split(r"^\t\tcase (\d+):", inner, flags=re.M)
    for i in range(1, len(parts), 2):
        index, code = int(parts[i]), parts[i + 1]
        for m in re.finditer(r"pc = (\d+);\n\s*continue;", code):
            if int(m.group(1)) <= index:
                return False
            if code[m.end():].strip("\t\n }"):
                return False
    return True


VS_INPUT = re.compile(r"^\tin (float4|uint4) (i\w+) : ([A-Z]+)(\d+),$", re.M)


def input_kind(kind, sem):
    """0: float4 input, 1: uint4 holding float bits (NORMAL/TANGENT/BINORMAL,
    decoded by tfetchR11G11B10), 2: uint4 used as integers (BLENDINDICES)."""
    if kind == "float4":
        return 0
    return 2 if sem == "BLENDINDICES" else 1


def vs_inputs(hlsl):
    out = []
    hlsl = re.sub(r"\[\[vk::[^\]]*\]\]\s*", "", hlsl.replace("\r\n", "\n"))
    for kind, _, sem, idx in VS_INPUT.findall(hlsl):
        if sem not in USAGES:
            raise ValueError(f"unknown vertex input semantic {sem}")
        out.append((USAGES[sem], int(idx), input_kind(kind, sem)))
    return out


DECODERS = ("pgr4InputFloat", "pgr4InputBits", "pgr4InputIndex")


def decode_vertex_inputs(body):
    """Every VS input is read as raw uint4 and decoded per draw from
    g_InputModes (2 bits per input, see shader_common_sm51.h): DXGI has no
    scaled integer vertex formats, and PGR4 feeds SHORT4/UBYTE4 data to float4
    inputs."""
    decls = []
    ordinals = {}

    def repl(m):
        kind, name, sem, idx = m.groups()
        n = len(decls)
        ordinals[name] = n
        decoded = "float4" if kind == "float4" else "uint4"
        decls.append(f"\t{decoded} {name} = {DECODERS[input_kind(kind, sem)]}({name}_raw, {n});\n")
        return f"\tin uint4 {name}_raw : {sem}{idx},"

    body = VS_INPUT.sub(repl, body)
    if decls:
        head = body.index(")\n{\n", body.index("void main(")) + len(")\n{\n")
        body = body[:head] + "".join(decls) + body[head:]

    # Indexed vertex fetches read the stream through the input's layout in
    # g_FetchLayouts (input ordinal = declaration order, as for g_InputModes).
    def vfetch(m):
        name = m.group(1)
        if name not in ordinals:
            raise ValueError("PGR4_VFETCH of undeclared input " + name)
        return f"pgr4VFetchInput({name}, {ordinals[name]}, "

    return re.sub(r"PGR4_VFETCH\((\w+), ", vfetch, body)


# A use of the macro on an input (the HLSL also carries its #define).
VFETCH_USE = re.compile(r"PGR4_VFETCH\(i\w+, ")


def reads_vertex_index(body):
    """Whether the VS reads r0.x before writing it. Xenos preloads r0.x with
    the vertex index; shaders that instance from it (instance =
    floor(index / N), fetch index = index mod N) need the wrapped vertex
    streams the renderer builds for them."""
    body = body[body.index("float ps = 0.0;"):]
    written = set()
    reads_x = re.compile(r"\br0\.[xyzw]*x|\br0\b(?!\.)")
    for line in body.split("\n"):
        line = line.strip()
        m = re.match(r"(\w+)(?:\.([xyzw]+))?\s*=\s*(.*);$", line)
        rhs = m.group(3) if m else line
        if "x" not in written and reads_x.search(rhs):
            return True
        if m and m.group(1) == "r0":
            written |= set(m.group(2) or "xyzw")
            if "x" in written:
                return False
    return False


def seed_vertex_index(body):
    """r0.x starts as the vertex index, as on Xenos. SV_VertexID leaves out
    the draw's base vertex, which Xenos includes (VGT_INDX_OFFSET)."""
    head = body.index("void main(\n") + len("void main(\n")
    body = body[:head] + "\tin uint iVertexId : SV_VertexID,\n" + body[head:]
    return body.replace("\tfloat4 r0 = 0.0;\n",
                        "\tfloat4 r0 = float4(float(int(iVertexId) + g_BaseVertex), 0.0, 0.0, 0.0);\n", 1)


def sampler_mask(hlsl):
    mask = 0
    for row, comp in re.findall(r"_SamplerDescriptorIndex : packoffset\(c(\d+)\.([xyzw])\)", hlsl):
        mask |= 1 << ((int(row) - 12) * 4 + "xyzw".index(comp))
    return mask


RELATIVE_CONSTANT = re.compile(r"\w+\(\d+ \+ a[0L]\)")


def constant_regs(hlsl):
    """Float constant registers the shader can read: the end of the highest
    declaration in its Vertex/PixelShaderConstants cbuffer. A relative read
    (c[a0 + n], c[aL + n]) can reach any register past its array, like the
    Xenos's, so those shaders get the whole file: the root CBV is not
    bounded, and a read past the upload returned another draw's constants."""
    if RELATIVE_CONSTANT.search(hlsl):
        return 256
    m = re.search(r"^cbuffer (?:Vertex|Pixel)ShaderConstants\b.*?\n\{(.*?)\n\}", hlsl, re.S | re.M)
    if not m:
        return 0
    end = 0
    for count, reg in re.findall(r"float4 \w+(?:\[(\d+)\])? : packoffset\(c(\d+)", m.group(1)):
        end = max(end, int(reg) + (int(count) if count else 1))
    return min(end, 256)


SCREEN_2D = re.compile(r"^\tfloat4 (?:PosScale|g_vPosScale) : packoffset\(c45\);$", re.M)


def compile_one(stem, hlsl_dir, bin_dir, work, fxc, common, sm51, dxc=None):
    with open(os.path.join(hlsl_dir, stem + ".hlsl"), encoding="utf-8", newline="") as f:
        hlsl = f.read()
    with open(os.path.join(bin_dir, stem + ".bin"), "rb") as f:
        container = f.read()
    is_pixel = stem.endswith(".ps")
    try:
        src = to_sm51(hlsl, common, sm51, is_pixel)
    except ValueError as e:
        return stem, None, str(e)
    src_path = os.path.join(work, stem + ".sm51.hlsl")
    obj_path = os.path.join(work, stem + (".dxil" if dxc else ".dxbc"))
    with open(src_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(src)
    if dxc:
        # Same source and binding layout as the fxc build, compiled to DXIL
        # (SM 6.0). dxc signs the output when dxil.dll sits beside it.
        # -Gis: strict IEEE float like fxc. Without it dxc may assume no
        # inf/NaN, and the HDR luminance chain (log of a black pixel is -inf)
        # flickered the scene's exposure on DXIL GPUs.
        cmd = [dxc, "-nologo", "-T", "ps_6_0" if is_pixel else "vs_6_0", "-E", "main", "-O3", "-Gis",
               "-HV", "2018", "-Qstrip_debug", "-Fo", obj_path, src_path]
    else:
        cmd = [fxc, "/nologo", "/T", "ps_5_1" if is_pixel else "vs_5_1", "/E", "main", "/O3",
               "/Qstrip_debug", "/Qstrip_priv", "/Fo", obj_path, src_path]
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if proc.returncode != 0:
        return stem, None, proc.stdout.decode(errors="replace").strip()
    with open(obj_path, "rb") as f:
        dxbc = f.read()
    # Stage: 1 PS; VS flags 2 = reads the vertex index, 4 = indexed vertex fetch,
    # 8 = screen-space 2D (places vertices with PGR4's PosScale at c45: UI).
    stage = 1 if is_pixel else ((2 if reads_vertex_index(hlsl) else 0) | (4 if VFETCH_USE.search(hlsl) else 0) |
                                (8 if SCREEN_2D.search(hlsl) else 0))
    # Bits 8-16: float constant registers read, plus one (0 = unknown, all 256).
    stage |= (constant_regs(hlsl) + 1) << 8
    return stem, (xxh64(container), stage, dxbc, [] if is_pixel else vs_inputs(hlsl), sampler_mask(src)), None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--hlsl", default=os.path.join(ROOT, "shaders", "hlsl"))
    ap.add_argument("--bin", nargs="+", default=[os.path.join(ROOT, "shaders", "bin", "xex"),
                                                  os.path.join(ROOT, "shaders", "bin", "tu"),
                                                  os.path.join(ROOT, "shaders", "bin", "runtime"),
                                                  # Geometry Wars (gw.xex) builds its shaders at runtime too.
                                                  os.path.join(ROOT, "shaders", "bin", "gw")])
    ap.add_argument("--common", default=os.path.join(ROOT, "thirdparty", "XenosRecomp", "XenosRecomp", "shader_common.h"))
    ap.add_argument("--sm51", default=os.path.join(os.path.dirname(__file__), "shader_common_sm51.h"))
    ap.add_argument("--fxc", default=shutil.which("fxc") or os.path.join(
        os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"), "Windows Kits", "10", "bin", "10.0.26100.0", "x64", "fxc.exe"))
    ap.add_argument("--work", default=os.path.join(ROOT, "shaders", "sm51"))
    ap.add_argument("--out", required=True)
    ap.add_argument("--dxil", action="store_true",
                    help="compile with dxc to DXIL (SM 6.0) instead of fxc to DXBC (SM 5.1)")
    ap.add_argument("--dxc", default=os.path.join(ROOT, "thirdparty", "XenosRecomp", "thirdparty", "dxc-bin",
                                                  "bin", "x64", "dxc.exe"))
    ap.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 4)
    args = ap.parse_args()

    if args.dxil and not os.path.isfile(args.dxc):
        sys.exit(f"dxc not found: {args.dxc}")
    if not args.dxil and not os.path.isfile(args.fxc):
        sys.exit(f"fxc not found: {args.fxc}")
    with open(args.common, encoding="utf-8") as f:
        common = f.read().replace("\r\n", "\n")
    with open(args.sm51, encoding="utf-8") as f:
        sm51 = f.read().replace("\r\n", "\n")
    os.makedirs(args.work, exist_ok=True)
    err_dir = os.path.join(args.work, "errors")
    shutil.rmtree(err_dir, ignore_errors=True)

    # Container dir for each HLSL stem: the XEX dump, the title update's XEX
    # dump, then runtime-built shaders.
    bin_for = {}
    for n in sorted(os.listdir(args.hlsl)):
        if n.endswith((".vs.hlsl", ".ps.hlsl")):
            for d in args.bin:
                if os.path.isfile(os.path.join(d, n[:-5] + ".bin")):
                    bin_for[n[:-5]] = d
                    break
    stems = sorted(bin_for)
    entries, failed = [], []
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        futs = [pool.submit(compile_one, s, args.hlsl, bin_for[s], args.work, args.fxc, common, sm51,
                            args.dxc if args.dxil else None) for s in stems]
        for fut in concurrent.futures.as_completed(futs):
            stem, entry, error = fut.result()
            if entry:
                entries.append(entry)
            else:
                failed.append(stem)
                os.makedirs(err_dir, exist_ok=True)
                with open(os.path.join(err_dir, stem + ".txt"), "w", encoding="utf-8") as f:
                    f.write(error)

    entries.sort(key=lambda e: e[0])
    header = struct.pack("<8sII", b"PGR4SC60" if args.dxil else b"PGR4SC51", 1, len(entries))
    entry_size = 32
    inputs_blob = bytearray()
    input_offsets = []
    for _, _, _, inputs, _ in entries:
        input_offsets.append(len(inputs_blob))
        for usage, index, is_uint in inputs:
            inputs_blob += struct.pack("<BBBB", usage, index, is_uint, 0)
    inputs_base = len(header) + entry_size * len(entries)
    blob_base = (inputs_base + len(inputs_blob) + 15) & ~15
    table = bytearray()
    blobs = bytearray()
    for (key, stage, dxbc, inputs, samplers), in_off in zip(entries, input_offsets):
        table += struct.pack("<QIIIIII", key, blob_base + len(blobs), len(dxbc), stage,
                             inputs_base + in_off, len(inputs), samplers)
        blobs += dxbc
        blobs += b"\0" * (-len(blobs) & 15)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "wb") as f:
        f.write(header + table + inputs_blob + b"\0" * (blob_base - inputs_base - len(inputs_blob)) + blobs)

    vs = sum(1 for e in entries if (e[1] & 0xFF) != 1)
    print(f"{len(entries)} shaders packed ({vs} VS, {len(entries) - vs} PS), {len(failed)} failed -> {args.out}")
    if failed:
        print(f"{'dxc' if args.dxil else 'fxc'} errors in {err_dir}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
