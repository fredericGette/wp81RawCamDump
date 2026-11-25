# wp81RawCamDump

Raw Bayer dump of the Nokia Lumia 520 rear camera (Windows Phone 8.1), **without the manufacturing mode**
and **without the VFE pixel pipeline**. The sensor frame goes CSI → ISPIF → VFE write master RDI0 → memory:
no black level subtraction, no lens shading (roll-off) correction, no bad pixel/cluster correction, no 3A.
Exposure time, analog gain (ISO) and lens position are forced by the application.

> **Status (2026-11-25): working on the Lumia 520.** The SMIA colour bars (`-t 2`) and a real scene were
> captured: 2600x1948 MIPI RAW10, row stride 3256 bytes, Bayer order GBRG, on-sensor corrections off.
> See [Results](#results) and [Hardware rules](#hardware-rules-learned-the-hard-way).

Compared with [wp81camdump](../wp81camdump) (`-r`), which switches `ManufacturingMode` on so that
`qccamavs8930.sys` exposes its "PT" raw pins, and gets the VFE *ideal raw* output (already corrected).

## Components

| Path | What |
|---|---|
| `driver/rawcam` | `wp81rawcam.sys`: legacy WDM control driver (`\\.\wp81rawcam`), unloadable. Opens the sensor, VFE and camera platform drivers through their device interfaces, like `qccamavs8930.sys`, and relays their internal IOCTLs. Owns the VFE message queue while a session is open, and maps frame buffers in the VFE IOMMU (SMMU contexts 13 and 14) with `qcvcm8930.sys`. |
| `driver/rawcamlog` | `wp81rawcamlog.sys`: KMDF upper filter, ETW logger only. Logs every internal IOCTL sent to the sensor, VFE and platform drivers, with full input and response buffers and decoded I2C transfers. Used to record the sequences of `qccamavs8930.sys`. Not needed for a capture. |
| `driver/common/rawcam_ioctl.h` | Interface between the application and `wp81rawcam.sys`. |
| `app` | `wp81RawCamDump.exe`: console application. The capture logic lives here, as scripts. |
| `tools/raw2png.py` | Converts a raw dump to PNG on the PC (from wp81camdump, plus `stride=auto`). |
| `docs/` | Recorded traces and decoded sequences. |

```
wp81RawCamDump.exe ──DeviceIoControl──> wp81rawcam.sys ──internal IOCTLs──> qccamsensor8930.sys  (sensor, CSI)
                                              │                         ──> qccamvfe8930.sys     (VFE, ISPIF)
                                              │                         ──> qccamplatform8930.sys (I2C)
                                              └── VcmCreate/Assoc/Reserve/Back ──> qcvcm8930.sys ──> qcsmmu8930.sys
```

Reference documentation of the drivers: `Lumia520/content/drivers/` (`qccamsensor8930.md`,
`qccamvfe8930.md`, `qccamavs8930.md`, `qccamplatform8930.md`, `qcvcm8930.md`, `qcsmmu8930.md`).

## Build

LLVM (clang-cl, lld-link, llvm-lib), CMake, Ninja, Windows Kits 8.1 and Windows Phone Kits 8.1 at their default
locations, and the VS 2012 WPSDK CRT (as for wp81camdump).

```
cd driver\rawcam     && cmake --preset arm32-wdm     && cmake --build build   # build\wp81rawcam.sys
cd driver\rawcamlog  && cmake --preset arm32-kmdf    && cmake --build build   # build\wp81rawcamlog.sys
cd app               && cmake --preset arm32-windows && cmake --build build   # build\wp81RawCamDump.exe
```

`wp81rawcam.sys` imports `qcvcm8930.sys`: its import library is generated from `driver/rawcam/qcvcm8930.def`.

Both drivers must be [test-signed](https://github.com/fredericGette/wp81documentation/blob/main/DriverBuilding/README.md#signing-on-the-computer):

The certificate is `CN=Wp81TestSigning` in the user store `My` (created once with PowerShell, since
`makecert` isn't installed: `New-SelfSignedCertificate -Subject "CN=Wp81TestSigning" -Type CodeSigningCert
-KeyAlgorithm RSA -KeyLength 2048 -HashAlgorithm SHA256 -CertStoreLocation Cert:\CurrentUser\My`). Sign from
PowerShell or cmd, not Git Bash, which rewrites the `/s` arguments into paths:

```
"C:\Program Files (x86)\Windows Kits\10\bin\10.0.19041.0\x64\signtool.exe" sign /fd sha256 /ph /s My /n Wp81TestSigning driver\rawcam\build\wp81rawcam.sys driver\rawcamlog\build\wp81rawcamlog.sys
```

Rebuilding a driver removes its signature: sign again after each build. On the PC, `Get-AuthenticodeSignature`
reports the chain as untrusted (the certificate isn't in the machine root store). That doesn't matter on the
phone, which runs in test-signing mode.

## Install (on the phone, over telnet)

Copy the files to `C:\Data\USERS\Public\Documents` (shared Documents folder).

### Control driver

```
sc create wp81rawcam type= kernel binPath= C:\Data\USERS\Public\Documents\wp81rawcam.sys
sc start wp81rawcam
```

It is a legacy driver: `sc stop wp81rawcam` / `sc start wp81rawcam` replace it without a reboot (close the
application first). It must be started again after each reboot.

### Logger (optional, Phase 0)

1. Copy `wp81rawcamlog.sys` to `C:\Windows\System32\drivers\`.
2. Service:
   ```
   reg add HKLM\SYSTEM\CurrentControlSet\Services\wp81rawcamlog /v Type /t REG_DWORD /d 1
   reg add HKLM\SYSTEM\CurrentControlSet\Services\wp81rawcamlog /v Start /t REG_DWORD /d 3
   reg add HKLM\SYSTEM\CurrentControlSet\Services\wp81rawcamlog /v ErrorControl /t REG_DWORD /d 1
   reg add HKLM\SYSTEM\CurrentControlSet\Services\wp81rawcamlog /v ImagePath /t REG_EXPAND_SZ /d \SystemRoot\System32\drivers\wp81rawcamlog.sys
   ```
3. Find the device instances and add the filter to each one:
   ```
   reg query HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\QCOM0710
   reg query HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\QCOM0700
   reg query HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\QCOM0775
   reg add "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\QCOM0710\<instance>" /v UpperFilters /t REG_MULTI_SZ /d wp81rawcamlog
   (same for QCOM0700 = VFE and QCOM0775 = camera platform)
   ```
   Check first that the instance has no `UpperFilters` value already (append to it if it has).
4. Reboot.

To remove it: delete the three `UpperFilters` values and reboot.

## Phase 0: record what the camera driver does

The capture recipe was built from the driver analysis and from the `qccamavs` traces of wp81camdump. A few
values are still guesses (see [Open points](#open-points)). Record the real sequences with the logger:

```
wp81debug.exe etw {0B6F5E42-1C3D-4D2B-9C8E-5A7D21F0C3A1} > C:\Data\USERS\Public\Documents\avs_photo.txt
```

then, in another telnet session:

1. a normal photo with the Camera app (or `wp81camdump -o x.jpg`);
2. a raw capture with `wp81camdump -r` (ideal raw path, still mode);
3. optionally `wp81camdump -r -e 1000000 -i 400` (manual exposure and gain through the diagnostic property).

Stop the capture, copy the text files to `docs/`.

Log format (one ETW string event per line):

```
REQ #12 VFE I 0x23241B n=7 OPERATION_CONFIG in=60 out=16 caller=K irql=0 qpc=0x1234ABCD
REQ #12 in+0000: 01 00 00 00 01 00 00 00 ...
CPL #12 VFE n=7 OPERATION_CONFIG status=0x00000000 STATUS_SUCCESS info=0 dt=35 result=0 resp=0/0
REQ #40 PLATFORM I 0x23200F I2C_WRITE in=36 out=0 caller=K irql=0 qpc=...
REQ #40 i2c slave=0x20 flags=0x002 reg=0x0202 count=2 data=03 E8
```

- Sensor and VFE commands show `n` (`((0x8FF + n) << 2) | 0x230003`) and their name, the input bytes, the
  camera result (`result`, byte 0x0C of the response descriptor) and the response bytes (`resp+`).
- `VFE REGISTER_MESSAGE_EVENT` (0x232003) shows the client event and the message queue address.
- `PLATFORM` I2C transfers are decoded (slave, register, data): this is every sensor register write.
- `dt` and `qpc` are performance counter ticks; the frequency is in the `Driver loaded` / `Filter attached` events.
- Buffers are dumped up to 4096 bytes, in lines of 64 bytes.

ETW provider `WP81-RawCamLog` `{0B6F5E42-1C3D-4D2B-9C8E-5A7D21F0C3A1}`: keyword 0x1 requests, 0x2 completions,
0x4 diagnostics, 0x8 I2C. The control driver writes its own traces to `WP81-RawCam`
`{6D1C2B7A-4E8F-4A63-B1D2-8F0E5C3A9B47}`.

The VFE and sensor drivers have their own (more verbose) ETW traces, see wp81camdump: set
`HKLM\System\CurrentControlSet\Services\qccamavs\Tracing` `TraceEnable` = 1 and `CoreDbgEnable` = 1, reboot,
and record provider `{62774a98-5c63-43e1-8b13-3c5e8958e414}`. They print the RDI write master settings
(`RDI: pSizeBufferCfg->rowIncrement`, …) during a wp81RawCamDump capture too.

## Usage

**Close every application that uses the camera first.** The driver attaches its own page tables to the VFE
IOMMU contexts, which `qccamavs8930.sys` only uses while a camera application is open.

```
wp81RawCamDump info                         # identity, lens details, modes (modesN.bin), SMIA registers
wp81RawCamDump capture -e 100000 -i 100 -v  # 1/10 s, ISO 100, verbose
wp81RawCamDump capture -e 2000000 -i 400 -f 800 -o C:\Data\Users\Public\Documents\night.raw
wp81RawCamDump capture -t 2                 # sensor colour bars: checks the unpacking and the Bayer order
wp81RawCamDump capture --sensor-correction     # keep the on-sensor defect / black level correction
wp81RawCamDump capture --dump-script > capture.txt   # edit, then:
wp81RawCamDump run capture.txt -v
wp81RawCamDump cmd sensor 57 resp=36        # one sensor command (sensor powered on/off around it)
wp81RawCamDump i2c r 0x0000 4               # SMIA model id / revision / manufacturer
```

| Option | |
|---|---|
| `-o FILE` | output (default `C:\Data\Users\Public\Documents\rawcam.raw`) and sidecar `FILE.txt` |
| `-e US` | exposure time in µs (sensor command 67 MANUAL_EXPOSURE). Max 6428220 µs: longer requests are capped by the sensor driver (warning printed) |
| `-i ISO` | 100…1600 → analog gain Q8 = ISO × 256 / 100 (same mapping as wp81camdump) |
| `-f POS` | lens position (sensor command 56 SET_LENS_POSITION), read back with command 55. ~800 = 40 cm, 900–1000 = a few metres (wp81camdump) |
| `-d N` | frames dropped before the captured one (`rdiNumberOfFramesToDrop` of VFE CAPTURE), so the new exposure is in effect |
| `-t N` | SMIA test pattern (register 0x0600): 1 solid colour, 2 colour bars, 3 fade to grey, 4 PN9 |
| `--sensor-correction` | keeps the SMIA on-sensor corrections. By default 0 is written to 0x0B00 (shading), 0x0B04 (black level), 0x0B05 (mapped defects), 0x0B06 (single defects); 0x0B04 and 0x0B05 are 1 after power on |
| `--size WxH` | resolution asked to the sensor (CONFIG_RESOLUTION, still op mode). Default 2592x1944 |
| `--rdi WxH` | size of the frame received on RDI0. Default: read from the SMIA output size registers (0x034C) after the mode change (2600x1948) |
| `--format N`, `--cid N`, `--rate KBPS` | RDI0 output format (24), ISPIF CID of the pixel data (0), RDI data rate (1670400) |
| `-v` | prints every command, its input, result and response, and every VFE message |

### Output

- `FILE`: the whole frame buffer as the VFE wrote it, filled with `0xCD` before the capture (so a partial
  frame is visible). Nothing is converted.
- `FILE.txt`: `format`, `width`, `height`, `stride=auto`, `fill`, `order`, exposure / gain / lens (asked and
  read back), sensor model, dropped frames, test pattern, VFE message id.

On the PC:

```
python tools/raw2png.py rawcam.raw              # half-resolution colour preview
python tools/raw2png.py rawcam.raw --gray       # raw values, to check the Bayer order
python tools/raw2png.py rawcam.raw --planes     # one false-colour PNG per Bayer channel
```

`stride=auto` estimates the row increment from the end of the written data and prints it.

## Script language

`capture` and `info` generate a script; `run` executes one from a file. One command per line, `#` starts a
comment, `?VAR command` runs the command only if `VAR` is not 0.

| Command | |
|---|---|
| `set NAME EXPR` | variable. EXPR: numbers (decimal / 0x hex) and `$VAR`s joined by `+ - * /`, left to right |
| `open sensor vfe platform smmu` / `open all` | starts the session (devices, VFE message queue, VCM) |
| `close` | ends the session: VFE stop / power off, sensor stop / power off, queue flush, SMMU unmapped |
| `sensor N BYTES... [resp=N] [nocheck]` | sensor command `n`. Fails unless status and camera result are 0 (`nocheck`) |
| `vfe N BYTES... [resp=N] [nocheck]` | VFE command `n` |
| `platform CODE [V0 [V1]]` | buffer-less platform IOCTLs (power, I2C clock 0x232013, GPIO 0x232017) |
| `i2cr REG LEN [slave=S]` / `i2cw REG BYTES...` | sensor registers (16-bit address, slave 0x20). Read sets `I2C0`…`I2C3`, `I2CV` |
| `alloc NAME SIZE [fill=B]` | frame buffer: `NAME` = device address, `NAME_IDX`, `NAME_SIZE` |
| `waitframe NAME... timeout=MS` | waits for a VFE message that contains one of the buffer addresses; sets `FRAME`, `FRAME_IDX`, `FRAME_SIZE`, `FRAME_MSG` |
| `wait MS` | prints the next VFE message |
| `save NAME FILE [LEN]` | writes a frame buffer (`save FRAME file` after `waitframe`) |
| `getu8/getu16/getu32 NAME OFFSET` | reads the last response |
| `dumpresp FILE`, `echo TEXT`, `sleep MS`, `fail TEXT` | |

BYTES: `0a0b0c` raw hex bytes, `u8:EXPR` `u16:EXPR` `u32:EXPR` little endian, `zN` N zero bytes.
After each command: `STATUS`, `RESULT`, `RESP_LEN`.

## How the capture works

1. **Sensor** (`qccamsensor8930.sys`, interface `{5e34e1c5-…}`): `0` power on, `68` identity, `45`
   CONFIG_RESOLUTION in still mode (2592x1944), optional SMIA register writes, `67` MANUAL_EXPOSURE
   (exposure µs + gain 8.8; applied directly to the sensor, without 3A), `56` SET_LENS_POSITION.
2. **VFE** (`qccamvfe8930.sys`, interface `{3e9c0fdb-…}`): `1` power on, `3` reset,
   `11` ISPIF_CONFIG (pixel CID → RDI, no PIX interface), `7` OPERATION_CONFIG (only RDI0, snapshot →
   ISPIF mode "RDI only": CAMIF and the pixel pipeline are never started), `42` RDI_CONFIG, `8`
   AXI_OUTPUT_CONFIG (RDI0 path only, format 24, 10-bit), `118` two RDI0 buffers.
3. `2` sensor start stream, VFE `102` CAPTURE (1 frame after `DROP` dropped ones), `149` capture ack, then
   wait for the VFE message carrying one of the buffer addresses.
4. Teardown: VFE `109` stop, `3` reset, `136` RDI0 flush, `2` power off; sensor `3` stop stream, `1` power
   off. The frame is read back through the driver's own non-cached mapping.

The frame buffers are allocated like `CameraBufferPool::AllocateSMMUMemory` of `qccamavs8930.sys`
(`MmAllocatePagesForMdlEx` + `VcmReserve(0x3E0)` + `VcmBack`), in a VCM created like its `CreateVcm`
(`VcmCreate(0x4000, 0x80000000)`, `VcmAssoc` + `VcmActivate` on contexts 13 and 14). `qccamavs8930.sys`
creates its VCM in `AcquireFilterResources` and frees it in `ReleaseFilterResources`, so the contexts are
free while no camera application runs.

### Facts used (from the wp81camdump `qccamavs` traces)

- `qccamavs8930.sys` already uses RDI0 in every session, for the 2 lines of SMIA embedded data:
  ISPIF `pixCID[0]` (image, RAW10) and `rdiCID[1]` (embedded data), RDI0 path type 1 (continuous),
  `rawOutputFormat` 24, `rawDataSize` 1, `RDI interface frequency set 1670400` (2 lanes × 835.2 Mbps).
  wp81RawCamDump routes CID 0 (the image) to RDI0 instead.
- Full mode: `vt_pix_clk_freq` 167.04 MHz, `line_length_pck` 2750 (16.46 µs per line), 2 MIPI lanes,
  sensor model 0x07A3, I2C address 0x20.
- Snapshot sequence of `qccamavs8930.sys`: CAMIF_CONFIG, ISPIF_CONFIG, OPERATION_CONFIG, AXI_OUTPUT_CONFIG, …,
  CAPTURE, START_CAPTURE_ACK_REQUEST, (frame), STOP, RESET, FLUSH.

## Results

First captures (`samples/`, not in git):

- `capture -t 2`: SMIA colour bars, correct colours and order with `order=GBRG` → packing (MIPI RAW10, 5 bytes per
  4 pixels), stride (3256 = (3250 + 7) / 8 × 8) and Bayer order verified.
- `capture -e 66666 -i 100`: real scene. `exposure_read_us` 66680, gain 256 (1×). Black pedestal ~191–192
  in all four channels (on-sensor black level correction 0x0B04 off), uncorrected hot pixels present.
- Frame done = VFE **message id 10**, payload: `+0x00` frame id, `+0x04` buffer device address,
  `+0x08` 0x1E6C, `+0x0C` 0x079B0004, `+0x10`/`+0x18` timestamp.
- The VFE reports a frame only when another buffer is queued to replace it ("Output Frame is dropped"
  otherwise, and the write master keeps the old addresses): `waitframe ... requeue=118` keeps one buffer
  in the queue, so a reported frame is never overwritten.

## Hardware rules learned the hard way

Each of these hung the phone (bus lockup → watchdog reset, ~4 minutes, no crash dump):

1. **The VFE IOMMU (SMMU contexts 13/14) is only clocked while the VFE is powered.** `VcmActivate` before
   VFE POWER_ON hangs. The driver attaches the VCM at the first frame allocation (after POWER_ON) and
   detaches it before POWER_OFF.
2. **VFE POWER_OFF switches the clocks off even when it reports a failure** (result 12 = ISPIF reset
   failed): no VFE command may follow it. The driver counts any POWER_OFF that reached the driver as done.
3. **The ISPIF needs the sensor's CSI clocks**: VFE RESET (and the ISPIF reset inside POWER_OFF) fails
   with 12 unless the sensor is powered (sensor 0) first. Order: sensor on → VFE on → … → VFE off → sensor off.

And one silent failure: the VFE message event must be registered with a non-zero input length (16), or
`qccamvfe8930.sys` stores a NULL event and never posts a message.

## Open points

Settled by the trace of a Camera app photo ([docs/camera_photo_analysis.md](docs/camera_photo_analysis.md)):
the layouts of CONFIG_RESOLUTION, ISPIF_CONFIG, OPERATION_CONFIG and of the AXI RDI path (width, height,
buffer height, stride), burst 2, pixel pattern 3, and the START → START_STREAM → START_CAPTURE_ACK_REQUEST
order with RDI0 in continuous mode. The recipe now follows that sequence.

Still open:

- Op mode 2 (still) + 2592x1944 gives the full 2600x1948 sensor output: confirmed. Other modes (binned 1288x968,
  `--size`) untested.
- Exposures up to the sensor driver cap of 6428220 us verified linear: [docs/long_exposures.md](docs/long_exposures.md).
- Lens positions (`-f`): range from GET_LENS_DETAILS: infinity 171, hyperfocal 283, near end 820.
- Exposure (0.125–2 s, linear) and analog gain (1×–16×, exact) verified: [docs/exposure_series.md](docs/exposure_series.md),
  [docs/iso_series.md](docs/iso_series.md). The black level depends on the gain (186.7–198.5).
- The Camera app works after wp81RawCamDump sessions without reboot: [docs/camera_after_rawcam.md](docs/camera_after_rawcam.md).

## Safety notes

- The VFE writes to the frame buffers by DMA through the SMMU. The driver unmaps them only after VFE STOP and
  POWER_OFF. If the VFE can't be stopped, rebooting is the safe option.
- A camera application started while a session is open would attach its own page tables to the same IOMMU
  contexts. Don't do that.
- Closing the application (or a crash) closes the handle, and the driver tears the session down.
