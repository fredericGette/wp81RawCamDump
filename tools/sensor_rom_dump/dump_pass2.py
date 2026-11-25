#!/usr/bin/env python3
# Second pass: re-read the 8-byte blocks that pass 1 got wrong.
#   - zero blocks : left 0x00 by poison-cell hangs in pass 1;
#   - stale blocks: read back as the stock .dcc scratch values
#                   (55 1D 4C 16 C7 1A A0 18): the dumper did not run on that
#                   stream start, so the bytes are not ROM;
#   - extra blocks: given on the command line (--add), e.g. suspicious chunks.
# For each block: first retry the normal 8-byte dumper (one power-cycle). Blocks
# that hang or come back stale again are recovered byte-by-byte, sweeping the
# destination scratch cell (0x35F7..0x35FE, then 0x35F5/F6) until a valid read.
# A byte read is accepted only if it differs from the stock value of the cell it
# was copied to; otherwise the next cell is tried (the stock values are all
# different, so a genuine byte equal to one cell's stock is read at the next).
#
#   python dump_pass2.py <out.bin>                      # zero + stale blocks
#   python dump_pass2.py <out.bin> --add 0x24C0 0x3200  # + these blocks
#   python dump_pass2.py <out.bin> 0x0150 0x01C0        # only these blocks
import sys, os, time
import re
import dump_driver as dd

# cell-sweep order: the pass-1 cells first, then the two "address" cells
DESTS = [0xF7, 0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xF5, 0xF6]
C1T = ("7C 35 7D {dl:02X} 78 00 E8 90 {hi:02X} {lo:02X} 93 FB 8D 82 8C 83 EB F0 "
       "0D BD 00 01 0C 08 B8 {cnt:02X} EB 90 30 47 74 98")
BATCH = 12

def chunk(src, dl, cnt):
    return ["sensor 0", "sleep 8", dd.CONFIG,
            "i2cw 0xF90F " + C1T.format(dl=dl, hi=(src >> 8) & 0xFF, lo=src & 0xFF, cnt=cnt),
            "i2cw 0xF92F " + dd.C2, "i2cw 0xF94F 4F",
            "sensor 2", "sleep 15", "sensor 3",
            f"echo @{src:04X}_{dl:02X}_{cnt:02X}", "i2cr 0x35F5 32", "sensor 1", "sleep 6"]

def gen(reqs, path):
    L = ["open sensor platform"]
    for (src, dl, cnt) in reqs:
        L += chunk(src, dl, cnt)
    L.append("close")
    open(path, "w", newline="\n").write("\n".join(L) + "\n")

def parse(out):
    # Returns {addr: value} for valid reads only, and the number of stale reads.
    res, cur, stale = {}, None, 0
    for line in out.splitlines():
        m = re.search(r"@([0-9A-Fa-f]{4})_([0-9A-Fa-f]{2})_([0-9A-Fa-f]{2})", line)
        if m:
            cur = (int(m.group(1), 16), int(m.group(2), 16), int(m.group(3), 16)); continue
        m = re.search(r"i2c 0x35F5:\s*((?:[0-9A-Fa-f]{2}\s*)+)", line)
        if m and cur:
            b = [int(x, 16) for x in m.group(1).split()]
            src, dl, cnt = cur
            first = dl - 0xF5
            payload = b[first:first + cnt]
            if len(payload) == cnt:
                if cnt == 1:
                    # single byte: reject only if it equals this cell's stock value
                    ok = payload[0] != dd.STOCK[0x3500 | dl]
                else:
                    ok = not dd.is_stale(payload, 0x3500 | dl)
                if ok:
                    for i, v in enumerate(payload):
                        res[src + i] = v
                else:
                    stale += 1
            cur = None
    return res, stale

def run(reqs):
    gen(reqs, os.path.join(dd.HERE, "p2.txt"))
    dd.scp(os.path.join(dd.HERE, "p2.txt"), "p2.txt")
    out, to = dd.ssh(f"cmd /c cd {dd.REMOTE} && wp81RawCamDump.exe run p2.txt", dd.CALL_TIMEOUT)
    res, stale = parse(out)
    return res, stale, to, ("0xC0000001" in out)

def zero_blocks(data):
    return [a for a in range(0, len(data), 8) if all(b == 0 for b in data[a:a + 8])]

def stale_blocks(data):
    return [a for a in range(0, len(data), 8) if bytes(data[a:a + 8]) == dd.STALE]

def main():
    binpath = sys.argv[1]
    args = sys.argv[2:]
    data = bytearray(open(binpath, "rb").read())
    if not args or args[0] == "--add":
        extra = [int(x, 0) & ~7 for x in args[1:]]
        zeros, stales = zero_blocks(data), stale_blocks(data)
    else:
        extra = [int(x, 0) & ~7 for x in args]     # only these block addrs
        zeros, stales = [], []
    # zero blocks: write back only non-zero values into cells that are 0 (genuine
    # zeros stay). Stale/extra blocks: the old bytes are wrong, overwrite them.
    replace = set(stales) | set(extra)
    blocks = sorted(set(zeros) | replace)
    log = open(binpath + ".pass2.log", "a")
    def say(s):
        print(s, flush=True); log.write(s + "\n"); log.flush()
    say(f"=== pass2 start {time.strftime('%H:%M:%S')} blocks={len(blocks)} "
        f"(zero={len(zeros)} stale={len(stales)} extra={len(extra)}) ===")
    if stales:
        say("stale blocks: " + " ".join(f"{a:04X}" for a in stales))

    t0 = time.time()
    recovered = {}        # addr -> value (validated read, may be 0)
    nstale = 0
    # Stage 1: fast 8-byte retry, batched
    hard = []             # blocks that hung or stayed stale -> per-byte stage
    i = 0
    while i < len(blocks):
        grp = blocks[i:i + BATCH]
        reqs = [(a, 0xF7, 8) for a in grp]
        res, st, to, err = run(reqs)
        nstale += st
        if to or err:
            dd.recover()
        for a in grp:
            got = [res.get(a + k) for k in range(8)]
            if all(v is not None for v in got):
                for k in range(8):
                    recovered[a + k] = got[k]
            else:
                hard.append(a)
        say(f"stage1 {i+len(grp)}/{len(blocks)} hard={len(hard)} stale_reads={nstale} {time.time()-t0:.0f}s")
        i += len(grp)

    # Stage 2: per-byte cell sweep for hung / still-stale blocks
    pend = {a + k: 0 for a in hard for k in range(8)}   # addr -> next DESTS index
    unrec = []
    while pend:
        batch = list(pend.items())[:BATCH]
        reqs = [(a, DESTS[di], 1) for a, di in batch]
        res, st, to, err = run(reqs)
        nstale += st
        if to or err:
            dd.recover()
        for a, di in batch:
            if a in res:
                recovered[a] = res[a]
                say(f"  recovered {a:04X}=0x{res[a]:02X} @cell 0x35{DESTS[di]:02X}")
                del pend[a]
            else:
                pend[a] = di + 1
                if pend[a] >= len(DESTS):
                    unrec.append(a); del pend[a]
                    say(f"  UNRECOVERED {a:04X} (all cells hang or stale)")
        say(f"stage2 pending={len(pend)} done={len(recovered)} unrec={len(unrec)} "
            f"stale_reads={nstale} {time.time()-t0:.0f}s")

    filled = replaced = 0
    for a, v in recovered.items():
        if (a & ~7) in replace:
            if data[a] != v:
                data[a] = v; replaced += 1
        elif data[a] == 0 and v != 0:
            data[a] = v; filled += 1
    open(binpath, "wb").write(data)
    left = stale_blocks(data)
    say(f"=== pass2 done: read {len(recovered)} bytes, filled {filled} zero cells, "
        f"replaced {replaced} bytes in stale/extra blocks, unrecoverable {len(unrec)}, "
        f"stale blocks left {len(left)} {time.time()-t0:.0f}s ===")
    if unrec:
        say("unrec addrs: " + " ".join(f"{a:04X}" for a in unrec))

if __name__ == "__main__":
    main()
