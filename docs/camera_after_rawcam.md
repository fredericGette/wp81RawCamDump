# Camera app after wp81RawCamDump sessions, 2026-11-25

Trace: `etw_camera_after_rawcam.txt` (logger). The phone wasn't rebooted after about 15 wp81RawCamDump
captures (colour bars, scenes, exposure and ISO series). The Camera app was opened, previewed and used for
photos: preview and photos OK (user report).

- No request failed at the IRP level (no error NTSTATUS).
- `qccamavs8930.sys` registers its VFE message event again at preview start (`REGISTER_MESSAGE_EVENT in=16`,
  result 0, same queue `0x89284CD0` as wp81RawCamDump), so the event left by wp81rawcam is replaced.
- Same full sequence as before any wp81RawCamDump session (probe power cycles, CONFIG_RESOLUTION, ISPIF,
  OPERATION, AXI, pipeline, START, START_STREAM), ending with the normal VFE and sensor POWER_OFF.
- Non-zero camera results are the same as in the reference trace (`etw_camera_photo.txt`): sensor queries
  9, 21, 26…49 → 20 (not supported), VFE updates → 26 (update pending). New: 12 × GET_DEMOSAIC_INFO → 1
  (rejected, wrong state), all right after START_OFFLINE_PROCESS, i.e. while the VFE reprocesses a photo;
  this session took 4 photos (4 offline processes) against 1 in the reference.

Conclusion: a wp81RawCamDump session leaves the camera stack usable without reboot.
