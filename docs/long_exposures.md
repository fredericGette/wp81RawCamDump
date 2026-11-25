# Long exposures, 2026-11-25

ISO 100, lens 171 (infinity), on-sensor corrections off. Reference: 0.5 s frame of the same scene taken
right after (`samples/long_ref_500000.raw`), the phone having moved since the [exposure series](exposure_series.md).
Method: 4x4 block means regressed against the reference, over the blocks with no saturated pixel.

| Asked | exposure_read_us | Expected vs 0.5 s | Measured | Correlation | Saturated (>= 1000) |
|---|---|---|---|---|---|
| 3 s | 3000720 | x6.000 | x5.967 | 0.9972 | 3.7% |
| 4 s | 4000960 | x8.000 | x7.891 | 0.9965 | 13.6% |
| 6 s | 6001440 | x12.000 | x12.085 | 0.9962 | 35.7% |
| 8 s | **6428220** | x12.854 | x12.810 | 0.9980 | 37.3% |

Conclusions:

- Linear up to the maximum (within 1.4%, scene very bright: up to 37% of the pixels saturated).
- **Maximum exposure 6428220 us (6.43 s)**: `qccamsensor8930.sys` caps longer requests silently (the cap
  noted in wp81camdump: 0xFFFF lines). The frame brightness follows the read-back value, not the request.
  wp81RawCamDump now prints a warning when `exposure_read_us` is shorter than asked.
- Each capture takes about 3 frame periods plus the skipped frames (`-d`, default 2): ~25 s at 6.4 s.
  `-d 1` is enough once the exposure is set (the new value applies at the next frame start).
