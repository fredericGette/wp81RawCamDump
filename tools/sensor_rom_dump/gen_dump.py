#!/usr/bin/env python3
# Generate a power-cycle-per-chunk ROM dump script for the sensor 8051, and
# parse its output into a .bin. The dumper (dumper_v2, 30 bytes/chunk) is
# re-uploaded after every sensor power-on, with the source address baked into
# the MOV DPTR,#SRC immediate at 0xF917 (hi) / 0xF918 (lo).
import sys, re

CONFIG = ("sensor 45 u32:2592 u32:1944 u32:1 u32:1 u32:1 u32:2 "
          "u32:256 u32:4050 u32:0 u32:83333")
CHUNK = 8   # bytes/stream into 0x35F7..0x35FE only (0x3600+ is value-sensitive)

# dumper chunk1: SRC immediate at bytes [8]=hi [9]=lo (0xF917/0xF918), count byte
# after "B8" (CJNE R0,#CHUNK). Dest 0x35F7, stays below 0x3600 for CHUNK<=9.
C1 = "7C 35 7D F7 78 00 E8 90 {hi:02X} {lo:02X} 93 FB 8D 82 8C 83 EB F0 0D BD 00 01 0C 08 B8 {cnt:02X} EB 90 30 47 74 98"
C2 = "F0 90 30 36 74 1E F0 90 30 42 74 24 F0 E5 53 60 06 78 2B 76 01 80 04 78 2B E4 F6 90 30 88 02 1D"
C3 = "4F"
# Stock .dcc value of 0x35F7..0x35FE: read back when the dumper didn't run.
STALE = bytes.fromhex("551D4C16C71AA018")

def gen(start, count, path):
    lines = ["open sensor platform"]
    addr = start
    end = start + count
    while addr < end:
        lines += [
            "sensor 0",
            "sleep 10",
            CONFIG,
            "i2cw 0xF90F " + C1.format(hi=(addr >> 8) & 0xFF, lo=addr & 0xFF, cnt=CHUNK),
            "i2cw 0xF92F " + C2,
            "i2cw 0xF94F " + C3,
            "sensor 2",
            "sleep 20",
            "sensor 3",
            f"# @{addr:04X}",
            "i2cr 0x35F5 32",
            "sensor 1",
            "sleep 10",
        ]
        addr += CHUNK
    lines.append("close")
    open(path, "w", newline="\n").write("\n".join(lines) + "\n")
    return (end - start + CHUNK - 1)  # upper bound

def parse(out_path, bin_path, start, count):
    data = {}
    cur = None
    for line in open(out_path, encoding="utf-8", errors="replace"):
        m = re.search(r"@([0-9A-Fa-f]{4})", line)
        if m:
            cur = int(m.group(1), 16)
            continue
        m = re.search(r"i2c 0x35F5:\s*((?:[0-9A-Fa-f]{2}\s*)+)", line)
        if m and cur is not None:
            b = [int(x, 16) for x in m.group(1).split()]
            payload = b[2:2 + CHUNK]  # skip 0x35F5,0x35F6 echo
            if bytes(payload) == STALE:
                cur = None            # dumper didn't run: stock scratch, counted missing
                continue
            for i, v in enumerate(payload):
                data[cur + i] = v
            cur = None
    buf = bytearray()
    miss = 0
    for a in range(start, start + count):
        if a in data:
            buf.append(data[a])
        else:
            buf.append(0)
            miss += 1
    open(bin_path, "wb").write(buf)
    return len(data), miss

if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "gen":
        start = int(sys.argv[2], 0); count = int(sys.argv[3], 0); path = sys.argv[4]
        n = gen(start, count, path)
        print(f"generated {path}: 0x{start:04X}..0x{start+count-1:04X}")
    elif cmd == "parse":
        start = int(sys.argv[2], 0); count = int(sys.argv[3], 0)
        out_path = sys.argv[4]; bin_path = sys.argv[5]
        got, miss = parse(out_path, bin_path, start, count)
        print(f"parsed {got} bytes, {miss} missing, wrote {bin_path}")
