"""Write PGR4's decrypted, decompressed default.xex image to a flat file.

default.xex on disk is encrypted and compressed, so shader containers in it
can't be found by scanning the file. IDA (with the ida-pro-mcp plugin serving
on 127.0.0.1:13337 and default.xex loaded) already holds the loaded image;
this asks IDA to write every segment into one file laid out by virtual
address, starting at the image base. Gaps between segments are zero-filled.

Usage:
    python dump_xex_image.py <out.bin> [--url http://127.0.0.1:13337/mcp]

Then:
    python dump_shaders.py <out_dir> <out.bin>
"""
import argparse
import json
import os
import sys
import urllib.request

IDA_CODE = r"""
import ida_bytes, ida_nalt, ida_segment
base = ida_nalt.get_imagebase()
segs = []
n = ida_segment.get_segm_qty()
for i in range(n):
    s = ida_segment.getnseg(i)
    segs.append((s.start_ea, s.end_ea, ida_segment.get_segm_name(s)))
end = max(e for _, e, _ in segs)
with open(OUT_PATH, "wb") as f:
    f.truncate(end - base)
    for start, stop, name in segs:
        data = ida_bytes.get_bytes(start, stop - start) or b""
        f.seek(start - base)
        f.write(data)
print(hex(base), hex(end), [(hex(a), hex(b), nm) for a, b, nm in segs])
"""


def call(url, tool, arguments):
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                       "params": {"name": tool, "arguments": arguments}}).encode()
    req = urllib.request.Request(url, body, {"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=600) as r:
        reply = json.load(r)
    if "error" in reply:
        sys.exit(f"IDA MCP error: {reply['error']}")
    text = "\n".join(c.get("text", "") for c in reply["result"].get("content", []))
    return json.loads(text) if text.startswith("{") else {"stdout": text}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out")
    ap.add_argument("--url", default="http://127.0.0.1:13337/mcp")
    args = ap.parse_args()

    out = os.path.abspath(args.out)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    health = call(args.url, "server_health", {})
    print(f"IDA: {health.get('module')} base={health.get('imagebase')}")
    result = call(args.url, "py_eval", {"code": f"OUT_PATH = {out!r}\n" + IDA_CODE})
    if result.get("stderr"):
        sys.exit(result["stderr"])
    print(result.get("stdout", "").strip())
    print(f"wrote {out} ({os.path.getsize(out)} bytes)")


if __name__ == "__main__":
    main()
