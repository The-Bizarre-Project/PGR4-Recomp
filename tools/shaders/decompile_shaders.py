"""Decompile dumped PGR4 shader containers to HLSL with PGR4's XenosRecomp.

For every <hash>.<vs|ps>.bin under the inputs (as written by dump_shaders.py),
runs XenosRecomp in single-file mode, which recompiles the Xenos microcode and
writes self-contained HLSL (shader_common.h is embedded). Each HLSL file is
then compiled with DXC using the same arguments XenosRecomp uses for the
D3D12 cache (-T vs_6_0/ps_6_0, or lib_6_3 for shaders that read spec
constants, -HV 2021 -all-resources-bound ...), plus
-DPGR4_RECOMP, so shaders XenosRecomp can't translate correctly show up here
instead of at runtime.

Results:
    <out>/<hash>.<vs|ps>.hlsl        recompiled shader
    <out>/errors/<hash>.<vs|ps>.txt  XenosRecomp crash/assert or DXC errors
    <out>/report.tsv                 one row per shader: recompile/compile status

Usage:
    python decompile_shaders.py <out_dir> <bin_dir> [<bin_dir> ...]
        [--xenosrecomp PATH] [--dxc PATH] [--include PATH] [-j N] [--no-compile]
"""
import argparse
import concurrent.futures as cf
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
XR_ROOT = os.path.join(ROOT, "thirdparty", "XenosRecomp")
DEFAULT_XR = os.path.join(ROOT, "out", "build", "xenosrecomp", "XenosRecomp", "XenosRecomp.exe")
DEFAULT_DXC = os.path.join(XR_ROOT, "thirdparty", "dxc-bin", "bin", "x64", "dxc.exe")
DEFAULT_INCLUDE = os.path.join(XR_ROOT, "XenosRecomp", "shader_common.h")


def iter_bins(dirs):
    for d in dirs:
        for root, _, names in os.walk(d):
            for n in sorted(names):
                if n.endswith((".vs.bin", ".ps.bin")):
                    yield os.path.join(root, n)


def run_one(args, path):
    stem = os.path.basename(path)[: -len(".bin")]  # <hash>.<vs|ps>
    is_ps = stem.endswith(".ps")
    hlsl = os.path.join(args.out_dir, stem + ".hlsl")
    err_path = os.path.join(args.out_dir, "errors", stem + ".txt")

    r = subprocess.run([args.xenosrecomp, path, hlsl, args.include],
                       capture_output=True, text=True, errors="replace", timeout=300)
    if r.returncode != 0 or not os.path.exists(hlsl) or os.path.getsize(hlsl) == 0:
        with open(err_path, "w", encoding="utf-8") as f:
            f.write(f"XenosRecomp exit {r.returncode}\n{r.stdout}\n{r.stderr}")
        return stem, "recompile_failed", "-"

    if args.no_compile:
        return stem, "ok", "skipped"

    # XenosRecomp builds shaders that read spec constants as a DXIL library
    # (lib_6_3) and links the constants in at pipeline creation.
    with open(hlsl, encoding="utf-8", errors="replace") as f:
        uses_spec = "g_SpecConstants() &" in f.read()
    target = "lib_6_3" if uses_spec else ("ps_6_0" if is_ps else "vs_6_0")
    cmd = [args.dxc, "-T", target, "-HV", "2021",
           "-all-resources-bound", "-Wno-ignored-attributes", "-Qstrip_reflect",
           "-Qstrip_debug", "-DPGR4_RECOMP", "-Fo", os.devnull, hlsl]
    c = subprocess.run(cmd, capture_output=True, text=True, errors="replace", timeout=300)
    if c.returncode != 0:
        with open(err_path, "w", encoding="utf-8") as f:
            f.write(" ".join(cmd) + "\n\n" + c.stdout + c.stderr)
        return stem, "ok", "compile_failed"
    return stem, "ok", "ok"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out_dir")
    ap.add_argument("bin_dirs", nargs="+")
    ap.add_argument("--xenosrecomp", default=DEFAULT_XR)
    ap.add_argument("--dxc", default=DEFAULT_DXC)
    ap.add_argument("--include", default=DEFAULT_INCLUDE)
    ap.add_argument("-j", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--no-compile", action="store_true")
    args = ap.parse_args()

    for tool in (args.xenosrecomp, args.include) + (() if args.no_compile else (args.dxc,)):
        if not os.path.exists(tool):
            sys.exit(f"missing: {tool}")
    os.makedirs(os.path.join(args.out_dir, "errors"), exist_ok=True)

    bins = list(iter_bins(args.bin_dirs))
    rows = []
    with cf.ThreadPoolExecutor(max_workers=args.j) as pool:
        futs = {pool.submit(run_one, args, p): p for p in bins}
        for i, fut in enumerate(cf.as_completed(futs), 1):
            try:
                rows.append(fut.result())
            except subprocess.TimeoutExpired:
                rows.append((os.path.basename(futs[fut])[:-4], "timeout", "-"))
            if i % 100 == 0 or i == len(bins):
                print(f"{i}/{len(bins)}")

    rows.sort()
    with open(os.path.join(args.out_dir, "report.tsv"), "w", encoding="utf-8") as f:
        f.write("shader\trecompile\tcompile\n")
        for r in rows:
            f.write("\t".join(r) + "\n")

    rec_fail = sum(r[1] != "ok" for r in rows)
    comp_fail = sum(r[2] == "compile_failed" for r in rows)
    print(f"{len(rows)} shaders: {len(rows) - rec_fail} recompiled, {rec_fail} recompile failures, "
          f"{comp_fail} DXC failures (see {os.path.join(args.out_dir, 'errors')})")


if __name__ == "__main__":
    main()
