# docs

- [camera_photo_analysis.md](camera_photo_analysis.md): sequence and structures of qccamavs8930.sys, from the logger trace `etw_camera_photo.txt`.
- [exposure_series.md](exposure_series.md): linearity 0.125 s to 2 s.
- [iso_series.md](iso_series.md): analog gain 1x to 16x, black level per gain.
- [long_exposures.md](long_exposures.md): 3 s to 8 s, cap at 6.43 s.
- [camera_after_rawcam.md](camera_after_rawcam.md): Camera app trace after wp81RawCamDump sessions (`etw_camera_after_rawcam.txt`).
- `capture_*.txt`, `debug_noframe_run1.txt`: console output of the first test runs on the phone.
- [sensor_patch/](sensor_patch/README.md): disassembly and behaviour of the 8051 patch uploaded from the .dcc file.
- `scripts/`: scripts used for the step-by-step tests (`wp81RawCamDump run FILE`).
