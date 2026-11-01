# PGR4 shader tools

PGR4's shaders are D3D shader containers embedded in `default.xex`. None of
the files under `PGR4-Files` contain uncompressed containers. Of the 247 shaders Xenia
captured at runtime (`shader_dump/`), 116 of 119 pixel shaders match an xex
container byte for byte, and 116 of 128 vertex shaders match except for the
vertex-fetch dwords D3D patches at bind time. 3 PS and 4 VS have no xex match
yet.

## 1. Dump the decrypted xex image (needs IDA + ida-pro-mcp on :13337)

```bash
python tools/shaders/dump_xex_image.py shaders/xex/default.image.bin
```

## 2. Dump shader containers

```bash
python tools/shaders/dump_shaders.py shaders/bin/xex shaders/xex/default.image.bin
python tools/shaders/dump_shaders.py shaders/bin/assets ../PGR4-Files
```

Writes `<hash>.<vs|ps>.bin` plus `manifest.tsv` (where each one was found).
With `pip install xxhash` the hash is XXH3_64, matching XenosRecomp's cache
keys. Without it a BLAKE2b-64 is used.

## 3. Decompile to HLSL

Build PGR4's XenosRecomp with the PGR4 guards on:

```bash
cmake -S thirdparty/XenosRecomp -B out/build/xenosrecomp -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DXENOS_RECOMP_PGR4=ON
cmake --build out/build/xenosrecomp --target XenosRecomp
python tools/shaders/decompile_shaders.py shaders/hlsl shaders/bin/xex
```

Each shader becomes `shaders/hlsl/<hash>.<vs|ps>.hlsl` and is compiled with
DXC the same way the D3D12 cache is built (with `-DPGR4_RECOMP`). Failures go
to `shaders/hlsl/errors/`, and `report.tsv` lists every shader's status.

## XenosRecomp changes (all behind `PGR4_RECOMP`)

| Problem in PGR4 shaders | Change |
| --- | --- |
| Vertex texture fetch uses fetch slots 16-19 (`s16`..`s19` undeclared) | VS fetch slot `16+n` maps to constant-table sampler `n` |
| VS texture fetch used `Sample` (invalid outside PS) | VS fetches call `tfetchLevel*` (`SampleLevel`, mip 0) |
| 1D textures (`MatrixPaletteSampler`, `fresnel`) had no descriptor heap | 1D is a Width x 1 2D texture: `tfetch1D` on the 2D heap |
| NORMAL0 / TANGENT0-1 interpolators undeclared | Added to the shared interpolator table (VS out = PS in) |
| Same vertex element declared twice (`iPosition1`) | Each usage/index declared once, all fetch addresses kept |
| PS bools (address 128+) and unnamed bools (`b253`) undeclared | All bool branches read a 256-bit `g_BoolConstantWords` block at SharedConstants c18-c19 |
| `r63` written, only r0-r31 declared | 64 temp registers |
| `cube` took its source register as is, dropping the operand swizzle and negate (`-r2.zzxy`): every reflection sampled the opposite way (the Enzo's lamps and windows reflected the ground) | The direction is the swizzled, negated src0's z, w, x |

The renderer must fill `g_BoolConstantWords` (VS bools in bits 0-127, PS
bools in 128-255) and create 1D textures as height-1 2D textures.
