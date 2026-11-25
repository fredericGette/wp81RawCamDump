# Sensor microcontroller patch (0107A302.dcc), 2026-11-25

The rear sensor (SMIA model 0x07A3, revision 2) contains an 8051-compatible microcontroller. At each
power on, `qccamsensor8930.sys` writes section 0 of `C:\Windows\System32\Drivers\0107A302.dcc` to the
sensor over I2C, including 694 bytes of 8051 code at `0xF800–0xFAB5`, a 40-byte table at `0xFBD7` and
`0xFB00 = 0x51` (format: Lumia520 `qccamsensor8930.md`, "CDCC data").

The 8051 data space (`MOVX @DPTR`) is the sensor register map: the code reads and writes SMIA and
manufacturer registers directly (`0x0205`, `0x0C1A`, `0x3036`, `0x3047`, `0x30F0`…).

Disassembly: [patch_F800_disasm.txt](patch_F800_disasm.txt) (IDA, raw binary, processor 8051).

## Structure

About 15 routines separated by `RET` padding. Most end with an `LJMP` into the ROM (`0x1D4F`, `0x16E1`,
`0x167E`, `0x190B`, `0x03DE`, `0x11C1`, `0x18AC`, `0x0A16`, `0x1E8F`, `0x1747`, `0x18D9`, `0x1FB1`): each
replaces the start of a ROM function, then continues in the ROM. Helpers `0xFA60` (load 16 bits from
`[7A:7B]`) and `0xFA6C` (add A to the pointer `7A:7B`). ROM routines called: `0x0947` (copy ?), `0x2122`,
`0x2293`, `0x224F`, `0x17D8`, `0x1B9B` (arithmetic ?), `0x12A4`.

Three times the bytes `90 FB 85 74 00 A5` (`MOV DPTR,#0xFB85` / `MOV A,#0` / `0xA5`): `0xA5` is not a
standard 8051 opcode, so either a vendor-specific instruction or data.

| Entry | Effect (from the code) | When it runs (measured) |
|---|---|---|
| `0xF800` | Picks one of 5 entries of the `0xFBD7` table from the analog gain code (register `0x0205` against `0x80/0xC0/0xE0/0xF0` = 2x/4x/8x/16x, confirmed by reading `0x0204` at each gain), computes 8 16-bit values from `0x3072…` and the table, writes them to `0x3062–0x3071` | Not seen: `0x3062…` stays `0x0020` at every gain |
| `0xF90F` | `0x3047 = 98`, `0x3036 = 1E`, `0x3042 = 24`, then variants depending on RAM `0x53`, `0x55` and `0x300E` | **At stream start** (sensor command 2): `0x3047` 80 → 98, `0x3036` 23 → 1E |
| `0xF972` | Tests `0x0C1A`, `0x3368`, `0x3038`, `0x303A` | ? |
| `0xF9B4` | `0x3E44 = 00`, calls `0x12A4` | **At power on**, after the upload (the .dcc writes `0x3E44 = 01`, it reads `00`) |
| `0xF9DF` | Writes `04` to `0xF62C/34/3C` or `0xF5C0/C8/D0` depending on RAM `0x44` | ? |
| `0xFA08` | If bit 2 of `0x0C1A` is clear: `0x30F8–F9 = 0`, bit 3 of `0x30F1` and `0x30F0` set | **At stream start**: `0x30F0` 01 00 → 09 08, `0x30F8` FF FE → 00 00 |
| `0xFA28` | Waits on `0x3442` bit 0 / RAM `0x46` bit 2, `0x361E = FF` | ? |
| `0xFA48`, `0xFA8A`, `0xFA9D`, `0xFAAB`, `0xFAB2` | Short hooks around `0xFB84/85` | ? |
| `0xFA76` | `0x35B5` low bits = 01, calls `0x1F5F` | Not seen (`0x35B5` stays `10`) |

The registers were read with `wp81RawCamDump i2c` / scripts `docs/scripts/probe_gain_regs.txt` and
`probe_patch_traces.txt`.

## Reading the sensor memory

- The patch area (`0xF800…`, `0xFB00`, `0xFBD7`) reads back as zeros over I2C, as do the ROM addresses
  called by the patch (`0x2122`, `0x17D8`): the code memory is not visible from I2C.
- Manufacturer registers written by the .dcc read back normally (`0x3002`, `0x3016`, `0x3045`, `0x35D8`
  table); `0x6006–0x6008` (written `02 02 02`) reads `00`.

## Scratch registers (tested 2026-11-25)

Script `docs/scripts/probe_scratch_regs.txt`: test pattern written after CONFIG_RESOLUTION, read back,
then read again while streaming (0.5 s, 1.5 s) and after the stream stop. The sensor is power-cycled at the
end, which reloads the .dcc values.

| Block | Width | Behaviour | Usable |
|---|---|---|---|
| `0x35F5–0x3614` (32 bytes) | 8 bits | Written pattern read back exactly; unchanged while streaming and after stop | **Yes** |
| `0x35D8–0x35EF` (24 bytes) | 10 bits (16-bit registers, high byte bits 0-1 only) | `A0 A7 AE B5…` reads `00 A7 02 B5…` | No |
| `0x3062–0x3071` (16 bytes) | 12 bits (high byte bits 0-3 only) | Overwritten while streaming (`00B9` x 8): used by the sensor (gain routine `0xF800` ?) | No |
| `0x3072–0x3081` (16 bytes) | ? | Writes ignored, reads `00` (earlier `00A7…`): read-only / volatile | No |

`0x35F5–0x3614` is part of a .dcc table (`0x35F5–0x3615`), so using it changes that table until the next
sensor power on: fine while dumping (no capture). The patch reaches the `0x35xx` registers with `MOVX`
(routine `0xFA76` writes `0x35B5`), so the 8051 sees the same block.

Proposed use: `0x35F5–0x35F6` = address to dump (big-endian, written over I2C by the phone),
`0x35F7–0x3614` = 30 bytes of code memory copied by the patch.

## Dumping the ROM: possible approach (not done)

Routine `0xF90F` runs at each stream start and only writes 3 registers before continuing in the ROM
(`LJMP 0x1D4F` after setting `DPTR = 0x3088`). A replacement could copy N bytes of code memory
(`MOVC A,@A+DPTR`) from an address taken from a scratch register into a readable register block, then do
what the original does and jump to `0x1D4F`. Loop from the phone: write the address, start the stream,
read the block, stop the stream.

Unknowns and risks:

- How the ROM reaches the patch routines (hook table not found: the `0xFBD7` table is calibration data).
  Only the effects are known, so a replacement must keep the entry address `0xF90F`.
- Whether `MOVC` reads the ROM (code and data spaces are unified at `0xF800`, since code written with
  I2C data writes is executed; unknown for the ROM range), and which register block can hold the data
  (found: `0x35F5–0x3614`, see above).
- The patch RAM can't be read back to check an upload.
- A wrong patch can hang the sensor (recoverable by a sensor power cycle, or a phone reboot). The sensor
  OTP / NVM (registers `0x0A00–0x0A0C`, used by the .dcc sections 1, 3, 4) holds the module calibration
  and could be programmed by a faulty routine: the dumper must only read code memory and write the
  chosen scratch registers.
