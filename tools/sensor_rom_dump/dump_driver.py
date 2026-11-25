#!/usr/bin/env python3
# Resilient sensor-ROM dumper. Each app session dumps a short batch of
# power-cycle chunks (kept under the ~25-cycle wedge threshold). A hard timeout
# guards every ssh call; on timeout/failure the stuck app is killed, the driver
# restarted, and the range resumed. Never blocks the phone and always makes
# progress (a chunk that fails RETRIES times is marked missing and skipped).
# Resumable: writes <bin> and <bin>.cov (1 byte/addr coverage).
import sys, re, subprocess, time, os

IP = "192.168.1.34"
REMOTE = "C:/Data/Users/Public/Documents"
CHUNK = 8
BATCH = 14            # chunks per session (< wedge threshold ~25)
RETRIES = 3          # per-batch attempts before shrinking/skipping
CALL_TIMEOUT = 45    # s, hard limit per app session
CONFIG = ("sensor 45 u32:2592 u32:1944 u32:1 u32:1 u32:1 u32:2 "
          "u32:256 u32:4050 u32:0 u32:83333")
C1 = ("7C 35 7D F7 78 00 E8 90 {hi:02X} {lo:02X} 93 FB 8D 82 8C 83 EB F0 0D "
      "BD 00 01 0C 08 B8 {cnt:02X} EB 90 30 47 74 98")
C2 = "F0 90 30 36 74 1E F0 90 30 42 74 24 F0 E5 53 60 06 78 2B 76 01 80 04 78 2B E4 F6 90 30 88 02 1D"
HERE = os.path.dirname(os.path.abspath(__file__))

# Stock values of the scratch cells 0x35F5..0x35FE, written by the .dcc at every
# power on (ETW trace: reg=0x35F5 data=22 19 55 1D 4C 16 C7 1A, 0x35FD=A0 18).
# A chunk that reads back these values means the dumper did NOT run on that
# stream start: the bytes are stale scratch content, not ROM.
STOCK = {0x35F5 + i: v for i, v in enumerate(bytes.fromhex("2219551D4C16C71AA018"))}
STALE = bytes(STOCK[0x35F7 + i] for i in range(CHUNK))   # 55 1D 4C 16 C7 1A A0 18
stale_hits = 0       # chunks rejected as stale (for the progress line)

def is_stale(payload, dest=0x35F7):
    # True when every byte equals the stock value of its scratch cell.
    return all(v == STOCK.get(dest + i) for i, v in enumerate(payload))

def sh(args, timeout=None):
    try:
        r = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
        return r.stdout + r.stderr, False
    except subprocess.TimeoutExpired as e:
        return (e.stdout or "") + (e.stderr or ""), True

def ssh(cmd, timeout):
    return sh(["ssh", "-o", "ConnectTimeout=8", IP, cmd], timeout=timeout)

def scp(local, name):
    sh(["scp", local, f"{IP}:{REMOTE}/{name}"], timeout=20)

def recover():
    # Kill any stuck app, bounce the driver, settle.
    ssh("cmd /c kill wp81RawCamDump.exe", 15)
    ssh("cmd /c sc stop wp81rawcam", 25)
    time.sleep(1.5)
    ssh("cmd /c sc start wp81rawcam", 25)
    time.sleep(1.5)

def gen(addrs, path, cnt=CHUNK):
    L = ["open sensor platform"]
    for a in addrs:
        L += ["sensor 0", "sleep 8", CONFIG,
              "i2cw 0xF90F " + C1.format(hi=(a >> 8) & 0xFF, lo=a & 0xFF, cnt=cnt),
              "i2cw 0xF92F " + C2, "i2cw 0xF94F 4F",
              "sensor 2", "sleep 15", "sensor 3",
              f"echo @{a:04X}", "i2cr 0x35F5 32", "sensor 1", "sleep 6"]
    L.append("close")
    open(path, "w", newline="\n").write("\n".join(L) + "\n")

def parse(out, cnt=CHUNK):
    global stale_hits
    res, cur = {}, None
    for line in out.splitlines():
        m = re.search(r"@([0-9A-Fa-f]{4})", line)
        if m:
            cur = int(m.group(1), 16); continue
        m = re.search(r"i2c 0x35F5:\s*((?:[0-9A-Fa-f]{2}\s*)+)", line)
        if m and cur is not None:
            b = [int(x, 16) for x in m.group(1).split()]
            payload = b[2:2 + cnt]
            if len(payload) == cnt and is_stale(payload):
                stale_hits += 1          # dumper didn't run: leave uncovered -> retried
            else:
                for i, v in enumerate(payload):
                    res[cur + i] = v
            cur = None
    return res

def run_batch(addrs, cnt=CHUNK):
    gen(addrs, os.path.join(HERE, "batch.txt"), cnt)
    scp(os.path.join(HERE, "batch.txt"), "batch.txt")
    out, to = ssh(f"cmd /c cd {REMOTE} && wp81RawCamDump.exe run batch.txt", CALL_TIMEOUT)
    return parse(out, cnt), to, ("0xC0000001" in out)

def main():
    start, end, binpath = int(sys.argv[1], 0), int(sys.argv[2], 0), sys.argv[3]
    data = bytearray(open(binpath, "rb").read()) if os.path.exists(binpath) else bytearray()
    cov = bytearray(open(binpath + ".cov", "rb").read()) if os.path.exists(binpath + ".cov") else bytearray()
    for buf in (data, cov):
        if len(buf) < end: buf.extend(b"\x00" * (end - len(buf)))

    def save():
        open(binpath, "wb").write(data); open(binpath + ".cov", "wb").write(cov)

    addr, t0, hangs, missing = start, time.time(), 0, 0
    while addr < end:
        if cov[addr]: addr += 1; continue
        addrs = list(range(addr, min(addr + BATCH * CHUNK, end), CHUNK))
        progressed = False
        for attempt in range(RETRIES):
            res, to, err = run_batch(addrs)
            for a, v in res.items():
                if a < end and not cov[a]:
                    data[a] = v; cov[a] = 1; progressed = True
            if to or err:
                hangs += 1; recover()
            # recompute pending addresses
            addrs = [a for a in addrs if not all(cov[a + i] for i in range(min(CHUNK, end - a)))]
            if not addrs: break
        save()
        if not progressed:
            # this lead chunk is stubborn: mark its bytes missing, skip it, keep going
            for i in range(min(CHUNK, end - addr)):
                if not cov[addr + i]: cov[addr + i] = 1; missing += 1
            recover()
        while addr < end and cov[addr]:
            addr += 1
        cvd = sum(cov[start:end]); tot = end - start
        el = time.time() - t0
        print(f"@0x{addr:04X} {cvd}/{tot} hangs={hangs} missing={missing} stale={stale_hits} "
              f"{cvd/max(1e-9,el):.1f}B/s {el:.0f}s", flush=True)
    save()
    print(f"DONE 0x{start:04X}-0x{end:04X} covered={sum(cov[start:end])}/{end-start} "
          f"hangs={hangs} missing={missing} stale={stale_hits} {time.time()-t0:.0f}s")

if __name__ == "__main__":
    main()
