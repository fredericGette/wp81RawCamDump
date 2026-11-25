# Exposure series, 2026-11-25

Same scene (desk, monitor), ISO 100 (gain 1x), lens 171 (infinity), on-sensor corrections off.
`wp81RawCamDump capture -e T -i 100 -f 171`, files `samples/series_T.raw` (not in git).
Statistics over the 98.2% of the pixels that are below 950 in the 2 s frame.

Linear fit of the mean raw value: **111.19 units/s, black (intercept) 193.19**.

| Exposure (s) | exposure_read_us | Mean raw | Above black | Fit residual | Ratio to 0.125 s | Saturated |
|---|---|---|---|---|---|---|
| 0.125 | 125020 | 206.89 | 13.70 | -0.20 | x1.00 | 0.00% |
| 0.250 | 250060 | 220.75 | 27.56 | -0.24 | x2.01 | 0.00% |
| 0.500 | 500110 | 248.74 | 55.55 | -0.05 | x4.05 | 0.00% |
| 1.000 | 1000230 | 305.23 | 112.04 | +0.85 | x8.18 | 0.20% |
| 2.000 | 2000480 | 415.19 | 222.00 | -0.37 | x16.20 | 1.44% |

Conclusions:

- The sensor response is linear from 0.125 s to 2 s (residuals below 1 raw unit), with no offset: the
  exposure is applied exactly as requested and read back.
- 2 s is beyond 0xFFFF lines of 16.46 us (1.08 s): `qccamsensor8930.sys` (command 67) extends the frame
  by itself; nothing extra is needed in the recipe.
- Black pedestal ~193 with the on-sensor black level correction off (0x0B04 = 0).
- Blue gets about 54% of the green signal under this (indoor, warm) light; G1 and G2 match within 1%.
