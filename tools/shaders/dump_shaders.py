"""Dump Xbox 360 D3D shader containers out of PGR4's game files.

Scans every input file (and directory, recursively) for the container header
XenosRecomp recognises, using the same acceptance test as XenosRecomp's
main.cpp:

    (flags & 0xFFFFFF00) == 0x102A1100
    virtualSize + physicalSize fits in the rest of the file
    field1C == 0 and field20 == 0

Each unique container is written as <out>/<hash>.<vs|ps>.bin. With the
'xxhash' package installed the hash is XXH3_64 over the whole container, the
same key XenosRecomp and the runtime shader cache use; without it a BLAKE2b-64
of the container is used, which is stable but does not match cache keys. A manifest.tsv records where
every container was found.

default.xex is encrypted/compressed on disk, so it is not scanned directly:
pass the decrypted image produced by dump_xex_image.py instead.

Usage:
    python dump_shaders.py <out_dir> <input> [<input> ...]
"""
import argparse
import mmap
import os
import struct
import sys

try:
    import xxhash
except ImportError:
    xxhash = None
import hashlib

MAGIC_PREFIX = b"\x10\x2a\x11"
HEADER = struct.Struct(">9I")  # ShaderContainer, 0x24 bytes

# IDA databases sit next to default.xex and are locked while IDA has them open.
SKIP_EXTS = {".id0", ".id1", ".id2", ".nam", ".til", ".i64", ".idb"}


def iter_files(paths):
    for p in paths:
        if os.path.isdir(p):
            for root, _, names in os.walk(p):
                for n in sorted(names):
                    if os.path.splitext(n)[1].lower() not in SKIP_EXTS:
                        yield os.path.join(root, n)
        elif os.path.isfile(p):
            yield p


def scan(path):
    """Yield (offset, flags, container_bytes) for every container in path."""
    size = os.path.getsize(path)
    if size <= HEADER.size:
        return
    with open(path, "rb") as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as m:
        pos = 0
        while True:
            i = m.find(MAGIC_PREFIX, pos)
            if i < 0 or i + HEADER.size > size:
                return
            # XenosRecomp walks in 4-byte steps from the file start.
            if i % 4:
                pos = i + 1
                continue
            (flags, vsize, psize, _fc, _ct, _dt, _so, f1c, f20) = HEADER.unpack_from(m, i)
            data_size = vsize + psize
            if (flags & 0xFFFFFF00) == 0x102A1100 and 0 < data_size <= size - i and f1c == 0 and f20 == 0:
                yield i, flags, m[i : i + data_size]
                pos = i + data_size
            else:
                pos = i + 4


def kind_of(flags):
    # Same test as ShaderRecompiler::recompile: bit 0 clear = pixel shader.
    return "ps" if (flags & 0x1) == 0 else "vs"


def hash64(blob):
    if xxhash is not None:
        return xxhash.xxh3_64_intdigest(blob)
    return int.from_bytes(hashlib.blake2b(blob, digest_size=8).digest(), "big")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out_dir")
    ap.add_argument("inputs", nargs="+")
    args = ap.parse_args()

    os.makedirs(args.out_dir, exist_ok=True)
    seen = {}
    rows = []
    scanned = 0
    for path in iter_files(args.inputs):
        scanned += 1
        found = 0
        try:
            hits = list(scan(path))
        except OSError as e:
            print(f"skip   {path}: {e.strerror}")
            continue
        for off, flags, blob in hits:
            h = hash64(blob)
            name = f"{h:016X}.{kind_of(flags)}.bin"
            if h not in seen:
                with open(os.path.join(args.out_dir, name), "wb") as f:
                    f.write(blob)
                seen[h] = name
            rows.append((name, path, off, len(blob)))
            found += 1
        if found:
            print(f"{found:5d}  {path}")

    with open(os.path.join(args.out_dir, "manifest.tsv"), "w", encoding="utf-8") as f:
        f.write("shader\tsource\toffset\tsize\n")
        for name, path, off, size in rows:
            f.write(f"{name}\t{path}\t0x{off:X}\t{size}\n")

    vs = sum(1 for n in seen.values() if n.endswith(".vs.bin"))
    print(f"scanned {scanned} files: {len(rows)} containers, {len(seen)} unique ({vs} VS, {len(seen) - vs} PS)")


if __name__ == "__main__":
    main()
