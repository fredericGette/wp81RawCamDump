# Camera app photo, wp81rawcamlog trace (2026-11-25)

Source: `etw_camera_photo.txt` (20682 requests). Camera app opened, autofocus, photo, preview restarted.
The app wasn't closed before the recording stopped: no final POWER_OFF in the trace.

## Sequence of qccamavs8930.sys

1. Probe (twice): sensor queries 25…50, `0` POWER_ON (platform ACTIVATE, POWER_ON, I2C clock 400 kHz),
   sensor detection and CDCC upload over I2C (patch at 0xF800…0xFAB7, 0xFB00, 0xFBD7…, tuning 0x3xxx), `6`
   chromatix, VFE `1` POWER_ON, `101` SET_BRACKETING_CALLBACK, `51`/`52`/`53` NVM, `68` identity, `59`, `57`,
   VFE `3` RESET, `23` modes, VFE `2` POWER_OFF, sensor `1` POWER_OFF.
2. Preview: VFE `REGISTER_MESSAGE_EVENT`, sensor `45`, VFE `11` ISPIF, `7` OPERATION, `42` RDI, `9` CAMIF,
   crop / scalers / skip / clamp, `8` AXI_OUTPUT, the pipeline blocks (linearization, roll-off 3D,
   demosaic, DBPC, DBCC, WB, CLF, CCM, LA, gamma, chroma, ASF, MCE, skin), statistics starts, sensor `11`
   QUERY_CONFIG, VFE `4` START, sensor `2` START_STREAM, VFE `149` START_CAPTURE_ACK_REQUEST, sensor `29`
   EXPOSURE_CONFIG, then per frame: sensor `54` SET_AEC_RESULTS, `13`, `17`…`20`, `28` embedded data, VFE
   updates and buffer enqueues (output 1, RDI0, statistics).
3. Photo: autofocus (`56` SET_LENS_POSITION ×7), then the still is made **offline** by the VFE from memory:
   `12` AXI_INPUT_CONFIG, `7` OPERATION_CONFIG (input source 2), `8` AXI_OUTPUT_CONFIG (output 2: NV12
   2592x1456), pipeline configs, `103` START_OFFLINE_PROCESS, `109` STOP, `3` RESET, sensor `73`, flushes.
   No sensor still mode and no VFE CAPTURE in this session.
4. Preview restarted (same as 2).

## Structures confirmed

**Sensor 45 CONFIG_RESOLUTION** (40 bytes), preview: `20 03 00 00 C0 01 00 00 01 00 00 00 01 00 00 00
01 00 00 00 01 00 00 00 00 03 00 00 84 03 00 00 xx xx xx xx 35 82 00 00`

| Offset | Value | Meaning |
|---|---|---|
| 0x00 | 800 | width |
| 0x04 | 448 | height |
| 0x08, 0x0C, 0x10 | 1, 1, 1 | ? |
| 0x14 | 1 | op mode (1 preview; 2 still in the old wp81camdump trace) |
| 0x18 | 768 | initial analog gain 8.8 (= 3.0), same as the first EXPOSURE_CONFIG |
| 0x1C | 900 | initial line count, same as the first EXPOSURE_CONFIG |
| 0x20 | varies | uninitialised ? (different in each call) |
| 0x24 | 33333 | max frame interval (µs) |

**Sensor 29 EXPOSURE_CONFIG**: `00 03 00 00 84 03 00 00` = gain 768 (8.8), 900 lines.

**Sensor 11 QUERY_CONFIG**: in `800, 448, 2, 1, 33333` → response (20 bytes) `0, 2014, 2024, 2750, 1288`.

**VFE 11 ISPIF_CONFIG**: RDI input 0, PIX input 0, `pixCID[0]` = 1, `rdiCID[1]` = 1, PIX rate 167040012,
RDI0 rate 1670400.

**VFE 7 OPERATION_CONFIG**: raw size 0, output 1 type 1, RDI0 type 1, input source 0, pixel pattern 3,
+0x2C 3, +0x30 0, +0x34 1 (composite statistics), +0x38 2.

**VFE 8 AXI_OUTPUT_CONFIG**: burst 2, statistics class 1, +0x08 33322; output 1 NV12 800x448
(format 7); **RDI0 path** at 0xBC: `10 05 03 00 03 00 54 06 18 00 00 00 01 00 00 00 2A 82 00 00 00`:

| Offset | Value | Meaning |
|---|---|---|
| 0x00 | 1296 | width |
| 0x02 | 3 | height (lines) |
| 0x04 | 3 | buffer height |
| 0x06 | 1620 | stride in bytes (1296 × 10 / 8); row increment (1620 + 7) / 8 = 203 words |
| 0x08 | 24 | output format |
| 0x0C | 1 | raw data size (10 bits) |
| 0x10 | 33322 | frame interval (µs) |

**VFE 4 START**: `00 00 00 00`. Order: START, then sensor START_STREAM, then START_CAPTURE_ACK_REQUEST.

These values are used by the capture recipe (`BuildCaptureScript` in `app/src/main.cpp`).
