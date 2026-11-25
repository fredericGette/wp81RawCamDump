# Sensor mask-ROM dumper (validated 2026-11-25)

Dumps the rear sensor's 8051 mask ROM through the `0xF90F` stream-start hook: a
replacement routine copies code memory (`MOVC A,@A+DPTR`) into a readable scratch
register block, which the phone reads back over I2C. See
`../../docs/sensor_patch/dumper_F90F.txt` for the routine and
`../../docs/sensor_patch/README.md` for the patch background.

## What works

- `MOVC` reads the real mask ROM. The dump at `0x0000` is a textbook 8051 vector
  table (`LJMP 0x0AB7` reset + interrupt vectors at `0x0003/0x000B/0x0013/0x001B/0x0023/0x002B`,
  8 bytes apart, each a valid `LJMP`), and the code calls the same ROM routines
  the `.dcc` patch calls (`0x224F`, `0x225E`, ...).
- **Full 16 KB dumped, complete: `rom_0000_3FFF.bin`** (coverage
  `rom_0000_3FFF.cov`, 1 byte per address, all 1). The ROM is 16 KB
  (`0x0000-0x3FFF`), mirrored 4x across the 64 KB code space (A14/A15 not decoded).
  Code occupies roughly `0x0000-0x27FF`; `0x2800-0x3FFF` is `0x20`-filled data.
  **Every byte is MOVC-confirmed — 0 missing.** Pass 1 left 192 bytes on poison
  cells as `0x00`; the second pass (`dump_pass2.py`) recovered all of them (274
  real bytes re-read across 37 candidate blocks, 0 unrecoverable; the remaining
  zeros are genuine ROM content). md5 `bf9e19d7a1dd03b07dd10622208820b6`.
- **But 23 blocks of 8 bytes are stale (found 2026-11-25).** They read
  `55 1D 4C 16 C7 1A A0 18`, the stock `.dcc` value of `0x35F7-0x35FE`: the
  dumper didn't run on that stream start, and the old driver accepted the
  read-back. 13 are in the code (`0150 01C0 03E8 09E8 0B78 0BB8 0E98 1850 1CB8
  1CD8 1F80 2060 22A0`), 10 in the filler. Also suspicious: `24C0` (`FB` x 8),
  `3200`, `35E0`, `3A20`, `3AF8`. Both scripts now reject stale read-backs (see
  below). The md5 above is of that first dump.
- **Fixed on the phone (2026-11-25):** `dump_pass2.py rom_0000_3FFF.bin --add 0x24C0
  0x3200 0x35E0 0x3A20 0x3AF8` re-read 29 blocks in 76 s (232 bytes, 0
  unrecoverable, 1 stale read rejected and retried). The 23 stale blocks are
  fixed; `24C0` read `FB` x 8 again (genuine); the 4 others became the filler
  pattern. Then `0x28F8` (`20` x 8, not stale but off the filler pattern) was
  re-read alone and became the pattern. Current md5
  `9fbba61337189c444d267004daccbf6c`; the filler is now one 32-byte pattern
  everywhere. `0x28F8` shows that pass 1 can return wrong bytes that aren't
  stale: the code area has been read only once.

## Hard-won constraints (why the driver is the way it is)

1. **The source address can't be re-patched mid-session.** After the first
   stream start latches the hook code, later I2C writes to `0xF8xx/0xF9xx` do not
   change what runs (re-patch of the `MOV DPTR,#SRC` immediate is frozen;
   re-upload is ignored). So the address is baked into the routine and the sensor
   is **power-cycled per chunk** (`sensor 1`/`sensor 0` reloads the `.dcc`,
   restoring the stock `0xF90F`, then the dumper is re-uploaded). The first
   stream after a fresh upload always honours the baked-in address.
2. **The scratch block is not inert RAM.** Writing arbitrary code bytes into
   `0x3600-0x3614` (the top of the `0x35F5-0x3614` block) hangs the sensor, and
   even in `0x35F7-0x35FE` a few byte values at a few cells hang it. So each
   chunk copies only **8 bytes** into `0x35F7-0x35FE`, and a chunk whose payload
   still hits a poison value is retried then marked missing (second pass needed).
3. **The sensor wedges intermittently after ~25-33 power cycles in one session**,
   and a wedge can block a sensor command. So each app session dumps a short
   batch (`BATCH=14`), every ssh call has a hard timeout, and on a wedge the
   driver kills the stuck app, bounces the kernel driver and resumes.

Throughput ~4.6 bytes/s (dominated by the per-chunk `.dcc` reload). The full 16 KB
ROM took ~60 min (3571 s), with 147 sensor wedges all auto-recovered.

## Running

Driver service `wp81rawcam` must be started and no camera app open. From the PC
(needs ssh/scp to the phone at the IP in `dump_driver.py`):

```
python dump_driver.py <start> <end> <out.bin>     # resumable; writes out.bin + out.bin.cov
# e.g. first 4 KB:
python dump_driver.py 0x0000 0x1000 rom.bin
```

Re-running continues where the coverage map left off.

### Second pass (`dump_pass2.py`) — recovering poison-cell bytes

A hung chunk in pass 1 leaves all 8 of its bytes `0x00`, so the missing bytes
show up as 8-aligned all-zero blocks. The second pass re-reads each such block:
first a fast 8-byte retry (genuine-zero blocks confirm in one power-cycle), then,
for blocks that hang again, **each byte individually at a single safe scratch
cell**, sweeping `0x35F7 → 0x35F8 … 0x35FE → 0x35F5/F6` only if a byte still
hangs. The win: pass 1 hung because one byte of the 8 landed on a poison *cell*
(`0x35F7 + offset`); reading bytes one at a time at the fixed cell `0x35F7` dodges
it. On the real ROM, all 274 missing bytes recovered (only 9 needed `0x35F8`), 0
unrecoverable. Run: `python dump_pass2.py <out.bin>` (writes only cells that were
`0x00` and come back non-zero, so genuine zeros are left intact).

### Stale read-backs

The `.dcc` writes the scratch cells at every power on (`0x35F5-0x35FE` =
`22 19 55 1D 4C 16 C7 1A A0 18`). If the dumper doesn't run on a stream start,
the read-back is these stock values, not ROM. Now:

- `dump_driver.py` rejects a chunk equal to `55 1D 4C 16 C7 1A A0 18`: it stays
  uncovered and is retried like a hang (`stale=` in the progress line).
- `dump_pass2.py` also selects the blocks already stored as that pattern, and
  checks every read: an 8-byte read equal to the stock values is rejected, and a
  single-byte read equal to the stock value of its cell moves to the next cell
  (the 10 stock values are all different, so a genuine byte equal to one is read
  at the next cell). Stale and extra blocks are overwritten with the validated
  bytes (any value, `0x00` included); zero blocks keep the old rule.

```
python dump_pass2.py <out.bin>                      # zero + stale blocks
python dump_pass2.py <out.bin> --add 0x24C0 0x3200  # + these 8-byte blocks
python dump_pass2.py <out.bin> 0x0150 0x01C0        # only these blocks
```

The log (`<out.bin>.pass2.log`) lists the stale blocks found, every recovered
byte, and the unrecoverable addresses.

- `gen_dump.py` — standalone generator for a single offline dump script (no
  auto-recovery); handy for manual runs with `wp81RawCamDump run`.
- `dump_driver.py` — the resilient driver (batches, timeouts, wedge recovery).
- `dump_pass2.py` — the poison-cell recovery pass (above); imports `dump_driver`.
