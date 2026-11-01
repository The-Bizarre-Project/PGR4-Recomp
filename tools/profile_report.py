"""Summarise profile.txt from the PGR4_PROFILE sampling profiler.

Usage: python tools/profile_report.py <build dir> [--thread TID] [--top N]
Resolves pgr4_recompiled.exe offsets against pgr4_recompiled.map (the
linker map beside the exe); other modules are listed by name only.
"""
import argparse
import bisect
import collections
import os
import re


IDLE_MODULES = {"ntdll.dll", "win32u.dll", "kernelbase.dll", "kernel32.dll"}


def load_map(path):
    syms = []
    base = 0x140000000
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = re.match(r"\s*Preferred load address is ([0-9a-fA-F]+)", line)
            if m:
                base = int(m.group(1), 16)
            m = re.match(r"\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s", line)
            if m:
                syms.append((int(m.group(2), 16) - base, m.group(1)))
    syms.sort()
    return [a for a, _ in syms], [n for _, n in syms]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("build")
    ap.add_argument("--thread", type=int)
    ap.add_argument("--top", type=int, default=40)
    args = ap.parse_args()
    addrs, names = load_map(os.path.join(args.build, "pgr4_recompiled.map"))
    per_thread = collections.Counter()
    idle = collections.Counter()
    funcs = collections.Counter()
    with open(os.path.join(args.build, "profile.txt")) as f:
        for line in f:
            tid, module, off, n = line.split()
            tid, off, n = int(tid), int(off, 16), int(n)
            # Samples parked in the OS (waits, sleeps) are idle time.
            if module.lower() in IDLE_MODULES:
                idle[tid] += n
                continue
            per_thread[tid] += n
            if args.thread and tid != args.thread:
                continue
            if module.lower() == "pgr4_recompiled.exe":
                i = bisect.bisect_right(addrs, off) - 1
                name = names[i] if i >= 0 else "?"
            else:
                name = module
            funcs[(tid if not args.thread else args.thread, name)] += n
    total = sum(per_thread.values())
    print("busy samples per thread (busy % of that thread's samples):")
    for tid, n in per_thread.most_common(12):
        print(f"  {tid:6} {n:8} {100 * n / (n + idle[tid]):5.1f}% busy")
    print()
    for (tid, name), n in funcs.most_common(args.top):
        print(f"{tid:6} {n:7} {100 * n / max(per_thread[tid], 1):5.1f}%  {name}")


if __name__ == "__main__":
    main()
