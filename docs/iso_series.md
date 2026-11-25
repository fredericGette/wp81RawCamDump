# ISO series, 2026-11-25

Same scene as the [exposure series](exposure_series.md) (desk, monitor), exposure 0.25 s (read back 250060 us),
lens 171 (infinity), on-sensor corrections off. `wp81RawCamDump capture -e 250000 -i ISO -f 171`, files
`samples/iso_ISO.raw` (not in git).

Method: 4x4 block means of each frame regressed against the ISO 100 frame, over the 218897 blocks with no
saturated pixel at ISO 1600. The slope is the real gain ratio; the intercept gives the black level of that
frame (value when the ISO 100 frame is at its black, 193.19). Noise: standard deviation of G1 - G2 in each
2x2 cell divided by sqrt(2) (includes a little scene texture).

| ISO | gain_q8 asked / read | Measured gain | Black level | Noise (raw units) | Saturated (>= 1000) |
|---|---|---|---|---|---|
| 100 | 256 / 256 | x1.000 | 193.2 | 2.2 | 0.00% |
| 200 | 512 / 512 | x2.012 | 187.2 | 4.4 | 0.00% |
| 400 | 1024 / 1024 | x4.021 | 186.7 | 8.7 | 0.20% |
| 800 | 2048 / 2048 | x8.024 | 190.1 | 17.3 | 1.43% |
| 1600 | 4096 / 4096 | x15.952 | 198.5 | 33.1 | 17.56% |

Conclusions:

- The analog gain is applied exactly (within 0.6%) from 1x to 16x: ISO 1600 is accepted by
  `qccamsensor8930.sys` (command 67) without capping.
- The noise doubles with each gain step: at these signal levels it's dominated by the shot noise amplified by
  the analog gain (no read-noise floor visible at 1x).
- **The black level depends on the gain** (186.7 to 198.5): with the on-sensor black level correction off, the
  pedestal isn't regulated. For accurate work, take a dark frame (lens covered) at the same gain and exposure;
  `raw2png --black auto` (0.1 percentile) is only an estimate, a little low at high gain because of the noise.
