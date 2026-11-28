# Real-video A/B benchmark

_Generated: 2026-11-28T04:31:50+00:00 (run `20261128T043150Z`)_

**Status:** ok — **candidate CERTIFIED**

**Publishable speedups:** yes (candidate decoded cleanly on every real-video case)

## Reference-only table (oracle x264, black box)

| Clip | Resolution | Frames | Preset | Oracle fps (median) | Median ms | Decode | PSNR dB | SSIM |
|---|---|---:|---|---:|---:|:---:|---:|---:|
| fortnite-raw-1080p | 1920x1080 | 8 | ultrafast | 162.22 | 49.3 | yes | 44.88 | 0.9874 |
| fortnite-raw-1080p | 1920x1080 | 8 | superfast | 93.99 | 85.1 | yes | 43.73 | 0.9880 |
| fortnite-raw-1080p | 1920x1080 | 8 | veryfast | 69.08 | 115.8 | yes | 41.82 | 0.9830 |
| fortnite-raw-1080p | 1920x1080 | 8 | faster | 47.63 | 167.9 | yes | 42.27 | 0.9836 |
| fortnite-raw-1080p | 1920x1080 | 8 | fast | 40.28 | 198.6 | yes | 42.57 | 0.9849 |
| fortnite-raw-1080p | 1920x1080 | 8 | medium | 39.65 | 201.7 | yes | 42.64 | 0.9849 |
| fortnite-raw-1080p | 1920x1080 | 8 | slow | 24.17 | 331.0 | yes | 42.69 | 0.9851 |
| fortnite-raw-1080p | 1920x1080 | 8 | slower | 19.84 | 403.3 | yes | 42.71 | 0.9850 |
| fortnite-raw-1080p | 1920x1080 | 8 | veryslow | 13.91 | 575.2 | yes | 42.29 | 0.9839 |
| twitter-640x360 | 640x360 | 8 | ultrafast | 369.97 | 21.6 | yes | 45.82 | 0.9915 |
| twitter-640x360 | 640x360 | 8 | superfast | 319.42 | 25.0 | yes | 45.67 | 0.9939 |
| twitter-640x360 | 640x360 | 8 | veryfast | 306.74 | 26.1 | yes | 43.97 | 0.9929 |
| twitter-640x360 | 640x360 | 8 | faster | 271.33 | 29.5 | yes | 44.71 | 0.9935 |
| twitter-640x360 | 640x360 | 8 | fast | 247.36 | 32.3 | yes | 44.77 | 0.9935 |
| twitter-640x360 | 640x360 | 8 | medium | 242.03 | 33.1 | yes | 44.79 | 0.9935 |
| twitter-640x360 | 640x360 | 8 | slow | 150.62 | 53.1 | yes | 44.74 | 0.9934 |
| twitter-640x360 | 640x360 | 8 | slower | 136.64 | 58.5 | yes | 44.80 | 0.9935 |
| twitter-640x360 | 640x360 | 8 | veryslow | 97.94 | 81.7 | yes | 45.24 | 0.9936 |
| twitter-1080p | 1920x1080 | 8 | ultrafast | 160.23 | 49.9 | yes | 64.62 | 0.9998 |
| twitter-1080p | 1920x1080 | 8 | superfast | 135.53 | 59.0 | yes | 63.39 | 0.9998 |
| twitter-1080p | 1920x1080 | 8 | veryfast | 112.06 | 71.4 | yes | 59.77 | 0.9997 |
| twitter-1080p | 1920x1080 | 8 | faster | 78.09 | 102.4 | yes | 59.70 | 0.9997 |
| twitter-1080p | 1920x1080 | 8 | fast | 75.65 | 105.8 | yes | 60.34 | 0.9997 |
| twitter-1080p | 1920x1080 | 8 | medium | 76.45 | 104.6 | yes | 60.36 | 0.9997 |
| twitter-1080p | 1920x1080 | 8 | slow | 69.92 | 114.4 | yes | 60.36 | 0.9997 |
| twitter-1080p | 1920x1080 | 8 | slower | 63.58 | 125.8 | yes | 60.35 | 0.9997 |
| twitter-1080p | 1920x1080 | 8 | veryslow | 55.48 | 144.2 | yes | 59.88 | 0.9997 |
| twitter-720p60 | 1280x720 | 8 | ultrafast | 242.17 | 33.0 | yes | 49.93 | 0.9958 |
| twitter-720p60 | 1280x720 | 8 | superfast | 205.92 | 38.9 | yes | 49.40 | 0.9970 |
| twitter-720p60 | 1280x720 | 8 | veryfast | 180.77 | 44.3 | yes | 48.20 | 0.9968 |
| twitter-720p60 | 1280x720 | 8 | faster | 148.83 | 53.8 | yes | 48.60 | 0.9970 |
| twitter-720p60 | 1280x720 | 8 | fast | 136.46 | 58.6 | yes | 48.10 | 0.9966 |
| twitter-720p60 | 1280x720 | 8 | medium | 128.91 | 62.1 | yes | 48.09 | 0.9966 |
| twitter-720p60 | 1280x720 | 8 | slow | 91.33 | 87.6 | yes | 47.80 | 0.9964 |
| twitter-720p60 | 1280x720 | 8 | slower | 83.45 | 95.9 | yes | 48.15 | 0.9965 |
| twitter-720p60 | 1280x720 | 8 | veryslow | 68.99 | 116.0 | yes | 47.78 | 0.9961 |

## A/B table (reference vs candidate)

| Clip | Preset | Oracle fps | Candidate fps | Speedup | Candidate decode | Certification |
|---|---|---:|---:|---:|:---:|---|
| fortnite-raw-1080p | ultrafast | 162.22 | 160.71 | 0.991x | yes | certified |
| fortnite-raw-1080p | superfast | 93.99 | 155.48 | 1.654x | yes | certified |
| fortnite-raw-1080p | veryfast | 69.08 | 164.06 | 2.375x | yes | certified |
| fortnite-raw-1080p | faster | 47.63 | 152.22 | 3.196x | yes | certified |
| fortnite-raw-1080p | fast | 40.28 | 161.74 | 4.016x | yes | certified |
| fortnite-raw-1080p | medium | 39.65 | 154.31 | 3.892x | yes | certified |
| fortnite-raw-1080p | slow | 24.17 | 152.59 | 6.313x | yes | certified |
| fortnite-raw-1080p | slower | 19.84 | 146.93 | 7.407x | yes | certified |
| fortnite-raw-1080p | veryslow | 13.91 | 147.95 | 10.638x | yes | certified |
| twitter-640x360 | ultrafast | 369.97 | 591.60 | 1.599x | yes | certified |
| twitter-640x360 | superfast | 319.42 | 599.06 | 1.875x | yes | certified |
| twitter-640x360 | veryfast | 306.74 | 550.56 | 1.795x | yes | certified |
| twitter-640x360 | faster | 271.33 | 587.99 | 2.167x | yes | certified |
| twitter-640x360 | fast | 247.36 | 571.55 | 2.311x | yes | certified |
| twitter-640x360 | medium | 242.03 | 569.93 | 2.355x | yes | certified |
| twitter-640x360 | slow | 150.62 | 599.98 | 3.983x | yes | certified |
| twitter-640x360 | slower | 136.64 | 566.47 | 4.146x | yes | certified |
| twitter-640x360 | veryslow | 97.94 | 598.32 | 6.109x | yes | certified |
| twitter-1080p | ultrafast | 160.23 | 151.26 | 0.944x | yes | certified |
| twitter-1080p | superfast | 135.53 | 155.51 | 1.147x | yes | certified |
| twitter-1080p | veryfast | 112.06 | 153.95 | 1.374x | yes | certified |
| twitter-1080p | faster | 78.09 | 159.05 | 2.037x | yes | certified |
| twitter-1080p | fast | 75.65 | 136.33 | 1.802x | yes | certified |
| twitter-1080p | medium | 76.45 | 151.96 | 1.988x | yes | certified |
| twitter-1080p | slow | 69.92 | 148.15 | 2.119x | yes | certified |
| twitter-1080p | slower | 63.58 | 145.91 | 2.295x | yes | certified |
| twitter-1080p | veryslow | 55.48 | 156.28 | 2.817x | yes | certified |
| twitter-720p60 | ultrafast | 242.17 | 309.06 | 1.276x | yes | certified |
| twitter-720p60 | superfast | 205.92 | 311.72 | 1.514x | yes | certified |
| twitter-720p60 | veryfast | 180.77 | 288.58 | 1.596x | yes | certified |
| twitter-720p60 | faster | 148.83 | 310.23 | 2.084x | yes | certified |
| twitter-720p60 | fast | 136.46 | 304.21 | 2.229x | yes | certified |
| twitter-720p60 | medium | 128.91 | 294.79 | 2.287x | yes | certified |
| twitter-720p60 | slow | 91.33 | 267.01 | 2.924x | yes | certified |
| twitter-720p60 | slower | 83.45 | 276.59 | 3.314x | yes | certified |
| twitter-720p60 | veryslow | 68.99 | 277.52 | 4.022x | yes | certified |

The candidate column stays `n/a` / `NOT CERTIFIED` unless the candidate binary exists, runs and decodes cleanly on **every** case. A measured ratio is shown only for a cleanly decoding candidate; no speedup is inferred from a failed or absent binary.

## Quality vs exact source I420 (ffmpeg PSNR / SSIM)

| Clip | Preset | Oracle PSNR dB | Oracle SSIM | Candidate PSNR dB | Candidate SSIM |
|---|---|---:|---:|---:|---:|
| fortnite-raw-1080p | ultrafast | 44.88 | 0.9874 | 99.00 | 1.0000 |
| fortnite-raw-1080p | superfast | 43.73 | 0.9880 | 99.00 | 1.0000 |
| fortnite-raw-1080p | veryfast | 41.82 | 0.9830 | 99.00 | 1.0000 |
| fortnite-raw-1080p | faster | 42.27 | 0.9836 | 99.00 | 1.0000 |
| fortnite-raw-1080p | fast | 42.57 | 0.9849 | 99.00 | 1.0000 |
| fortnite-raw-1080p | medium | 42.64 | 0.9849 | 99.00 | 1.0000 |
| fortnite-raw-1080p | slow | 42.69 | 0.9851 | 99.00 | 1.0000 |
| fortnite-raw-1080p | slower | 42.71 | 0.9850 | 99.00 | 1.0000 |
| fortnite-raw-1080p | veryslow | 42.29 | 0.9839 | 99.00 | 1.0000 |
| twitter-640x360 | ultrafast | 45.82 | 0.9915 | 99.00 | 1.0000 |
| twitter-640x360 | superfast | 45.67 | 0.9939 | 99.00 | 1.0000 |
| twitter-640x360 | veryfast | 43.97 | 0.9929 | 99.00 | 1.0000 |
| twitter-640x360 | faster | 44.71 | 0.9935 | 99.00 | 1.0000 |
| twitter-640x360 | fast | 44.77 | 0.9935 | 99.00 | 1.0000 |
| twitter-640x360 | medium | 44.79 | 0.9935 | 99.00 | 1.0000 |
| twitter-640x360 | slow | 44.74 | 0.9934 | 99.00 | 1.0000 |
| twitter-640x360 | slower | 44.80 | 0.9935 | 99.00 | 1.0000 |
| twitter-640x360 | veryslow | 45.24 | 0.9936 | 99.00 | 1.0000 |
| twitter-1080p | ultrafast | 64.62 | 0.9998 | 99.00 | 1.0000 |
| twitter-1080p | superfast | 63.39 | 0.9998 | 99.00 | 1.0000 |
| twitter-1080p | veryfast | 59.77 | 0.9997 | 99.00 | 1.0000 |
| twitter-1080p | faster | 59.70 | 0.9997 | 99.00 | 1.0000 |
| twitter-1080p | fast | 60.34 | 0.9997 | 99.00 | 1.0000 |
| twitter-1080p | medium | 60.36 | 0.9997 | 99.00 | 1.0000 |
| twitter-1080p | slow | 60.36 | 0.9997 | 99.00 | 1.0000 |
| twitter-1080p | slower | 60.35 | 0.9997 | 99.00 | 1.0000 |
| twitter-1080p | veryslow | 59.88 | 0.9997 | 99.00 | 1.0000 |
| twitter-720p60 | ultrafast | 49.93 | 0.9958 | 99.00 | 1.0000 |
| twitter-720p60 | superfast | 49.40 | 0.9970 | 99.00 | 1.0000 |
| twitter-720p60 | veryfast | 48.20 | 0.9968 | 99.00 | 1.0000 |
| twitter-720p60 | faster | 48.60 | 0.9970 | 99.00 | 1.0000 |
| twitter-720p60 | fast | 48.10 | 0.9966 | 99.00 | 1.0000 |
| twitter-720p60 | medium | 48.09 | 0.9966 | 99.00 | 1.0000 |
| twitter-720p60 | slow | 47.80 | 0.9964 | 99.00 | 1.0000 |
| twitter-720p60 | slower | 48.15 | 0.9965 | 99.00 | 1.0000 |
| twitter-720p60 | veryslow | 47.78 | 0.9961 | 99.00 | 1.0000 |

## Documented reference anchors (historical, not re-derived here)

| Case | fps | Source |
|---|---:|---|
| 640x360, 120 f, medium, CRF 23, 6 threads (synthetic testsrc2) | 609.14 | `benchmarks.md / README.md` |
| 1920x1080, 8 f, ultrafast, CRF 23, 6 threads (Fortnite I420) | 126.98 | `benchmarks.md / README.md` |
| 1920x1080, 8 f, medium, CRF 23, 6 threads (Fortnite I420) | 40.61 | `benchmarks.md / README.md` |
| 1920x1080, 8 f, veryslow, CRF 23, 6 threads (Fortnite I420) | 13.84 | `benchmarks.md / README.md` |

These four numbers are the already-established reference measurements published in `benchmarks.md` / `README.md`. The table above contains this run's fresh oracle measurements and is the authoritative result for this run.

## Summary

- Cases: 36; oracle-decoded: 36; candidate-certified: 36
- Presets: ultrafast, superfast, veryfast, faster, fast, medium, slow, slower, veryslow; frames/case: 8; reps: 2; warm-ups: 1
- Machine: Intel64 Family 6 Model 151 Stepping 5, GenuineIntel, 12 logical CPUs
