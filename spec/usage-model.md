# Markov usage model for statistical certification

Status: **specification of the test-case population**, not a claim about measured real-world traffic. No usage telemetry was available to the specification team; all weights and stickiness parameters are documented engineering priors and are marked **UNKNOWN** where they assert real-world frequency. The model is fully specified so certification can draw reproducible, independent-enough test cases and report the realized marginals.

## 1. State definition

A test case is a composite first-order Markov state

```
S_t = ( R_t , N_t , P_t , C_t , T_t , K_t )
```

| Symbol | Dimension | Values (code → meaning) |
|---|---|---|
| `R` | Resolution | `R1`=320×180, `R2`=640×360, `R3`=1280×720 |
| `N` | Frame count at 30 fps | `N1`=1, `N2`=10, `N3`=30, `N4`=120 |
| `P` | Preset | `P1`=ultrafast, `P2`=veryfast, `P3`=fast, `P4`=medium, `P5`=slow |
| `C` | CRF | `C1`=18, `C2`=23, `C3`=28, `C4`=32 |
| `T` | Thread count | `T1`=1, `T2`=2, `T3`=4, `T4`=8 |
| `K` | Content type | `K1`=synthetic, `K2`=natural-like, `K3`=noisy, `K4`=static |

All encodings are progressive 8-bit 4:2:0, raw YUV420p input, `.264` Annex B output, frame rate 30. These choices match the probe setup in `spec/probes/measure-quality.ps1`; other chroma formats/bit depths are **UNKNOWN** for this usage model.

### 1.1 Deterministic content generators

The content label is mapped to a deterministic ffmpeg `lavfi` source so that any certifier can reproduce the exact input bytes:

| `K` | ffmpeg source |
|---|---|
| synthetic | `testsrc2=size=WxH:rate=30` |
| natural-like | `mandelbrot=size=WxH:rate=30:maxiter=256:end_pts=10` |
| noisy | `testsrc2=size=WxH:rate=30,noise=alls=25:allf=t+u` |
| static | `color=c=0x303030:s=WxH:r=30,drawbox=x=0:y=0:w=iw/2:h=ih/2:color=0x808080:t=fill` |

`mandelbrot` is a detailed moving fractal used as a **proxy** for natural imagery; whether it is representative of real camera footage is **UNKNOWN**. The generators live in `spec/probes/measure-quality.ps1`; media must be written to a temp directory outside the repo.

## 2. Transition law

The chain is factored. For each dimension `d ∈ {R,N,P,C,T,K}`, with current value `i`, next value `j`:

```
Pr( S_{t+1,d} = j | S_{t,d} = i ) = λ_d · 1[j = i] + (1 − λ_d) · w_d(j)
```

The six dimensions transition independently conditional on `S_t`. This is a legitimate first-order Markov chain: the distribution of `S_{t+1}` depends only on `S_t`, and every entry is positive (the chain is ergodic). Because the initial distribution is chosen as the product of the `w_d` (below) and the kernel preserves `w_d`, the chain is **stationary from t=0**; no burn-in is required.

### 2.1 Stationary / initial weights `w_d`

| `R` | w | `N` | w | `P` | w | `C` | w | `T` | w | `K` | w |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 320×180 | 0.35 | 1 | 0.10 | ultrafast | 0.08 | 18 | 0.10 | 1 | 0.25 | synthetic | 0.25 |
| 640×360 | 0.45 | 10 | 0.25 | veryfast | 0.17 | 23 | 0.40 | 2 | 0.35 | natural-like | 0.35 |
| 1280×720 | 0.20 | 30 | 0.40 | fast | 0.20 | 28 | 0.35 | 4 | 0.30 | noisy | 0.25 |
| — | — | 120 | 0.25 | medium | 0.40 | 32 | 0.15 | 8 | 0.10 | static | 0.15 |

### 2.2 Stickiness `λ_d`

| Dimension | `R` | `N` | `P` | `C` | `T` | `K` |
|---|---|---|---|---|---|---|
| `λ` | 0.75 | 0.60 | 0.45 | 0.60 | 0.80 | 0.50 |

A high `λ` means consecutive cases tend to hold that dimension fixed (a campaign explores one setting at a time); a low `λ` means deliberate switching.

### 2.3 Explicit transition matrices (row = current, column = next)

`R` (λ=0.75):

| from ↓ / to → | 320×180 | 640×360 | 1280×720 |
|---|---|---|---|
| 320×180 | 0.8375 | 0.1125 | 0.0500 |
| 640×360 | 0.0875 | 0.8625 | 0.0500 |
| 1280×720 | 0.0875 | 0.1125 | 0.8000 |

`N` (λ=0.60):

| from ↓ / to → | 1 | 10 | 30 | 120 |
|---|---|---|---|---|
| 1 | 0.64 | 0.10 | 0.16 | 0.10 |
| 10 | 0.04 | 0.70 | 0.16 | 0.10 |
| 30 | 0.04 | 0.10 | 0.76 | 0.10 |
| 120 | 0.04 | 0.10 | 0.16 | 0.70 |

`P` (λ=0.45):

| from ↓ / to → | ultrafast | veryfast | fast | medium | slow |
|---|---|---|---|---|---|
| ultrafast | 0.4940 | 0.0935 | 0.1100 | 0.2200 | 0.0825 |
| veryfast | 0.0440 | 0.5435 | 0.1100 | 0.2200 | 0.0825 |
| fast | 0.0440 | 0.0935 | 0.5600 | 0.2200 | 0.0825 |
| medium | 0.0440 | 0.0935 | 0.1100 | 0.6700 | 0.0825 |
| slow | 0.0440 | 0.0935 | 0.1100 | 0.2200 | 0.5325 |

`C` (λ=0.60):

| from ↓ / to → | 18 | 23 | 28 | 32 |
|---|---|---|---|---|
| 18 | 0.64 | 0.16 | 0.14 | 0.06 |
| 23 | 0.04 | 0.76 | 0.14 | 0.06 |
| 28 | 0.04 | 0.16 | 0.74 | 0.06 |
| 32 | 0.04 | 0.16 | 0.14 | 0.66 |

`T` (λ=0.80):

| from ↓ / to → | 1 | 2 | 4 | 8 |
|---|---|---|---|---|
| 1 | 0.85 | 0.07 | 0.06 | 0.02 |
| 2 | 0.05 | 0.87 | 0.06 | 0.02 |
| 4 | 0.05 | 0.07 | 0.86 | 0.02 |
| 8 | 0.05 | 0.07 | 0.06 | 0.82 |

`K` (λ=0.50):

| from ↓ / to → | synthetic | natural-like | noisy | static |
|---|---|---|---|---|
| synthetic | 0.625 | 0.175 | 0.125 | 0.075 |
| natural-like | 0.125 | 0.675 | 0.125 | 0.075 |
| noisy | 0.125 | 0.175 | 0.625 | 0.075 |
| static | 0.125 | 0.175 | 0.125 | 0.575 |

All rows sum to 1. The stationary distribution of the composite chain is `π(S) = w_R(R)·w_N(N)·w_P(P)·w_C(C)·w_T(T)·w_K(K)`.

## 3. Rationale (documented priors)

1. **Resolution (R):** 640×360 is the most common compromise between meaningful bitstream complexity and fast certification runs (0.45); 320×180 covers smoke/regression cases (0.35); 1280×720 is included to exercise level/DPB/VUI scaling but is expensive (0.20). High stickiness (0.75) models batches run at one resolution.
2. **Frame count (N):** 30 frames (1 s at 30 fps) is the workhorse (0.40); 120 frames exercises long-GOP/rate-control drift (0.25); 10 frames supports quick sweeps (0.25); 1 frame supports header/syntax checks (0.10). Moderate stickiness (0.60) groups similar-length runs.
3. **Preset (P):** `medium` is the reference default and therefore the most tested (0.40); fast-side presets (ultrafast/veryfast/fast = 0.45 combined) matter for the speed-first goal; `slow` (0.15) is a quality anchor. Lowest stickiness (0.45) reflects that preset sweeps are a common experiment.
4. **CRF (C):** 23 and 28 together cover 75% of the population, matching typical constant-quality choices; 18 is a high-quality anchor (0.10), 32 a low-bitrate stress case (0.15). Stickiness 0.60 matches CRF sweeps where only one quality point changes.
5. **Threads (T):** single/dual/quad-thread runs cover the common CI and desktop cases (0.90 combined); 8 threads is a scaling stress case (0.10). Highest stickiness (0.80) reflects that a machine is usually configured once per campaign. Black-box checks showed thread count can leave PSNR/size unchanged at small resolutions but can shift a 640×360 natural encode by ~0.43% in bytes and ~0.02 dB in PSNR (see `quality-envelope.md`), so it is a real, if small, factor.
6. **Content (K):** natural-like (0.35) and synthetic (0.25) dominate; noisy (0.25) stresses rate control and deblocking; static (0.15) stresses skip/PCM-like behaviour and very small outputs. Stickiness 0.50 allows content changes often enough to cover artifact classes.
7. **UNKNOWN:** none of these weights are measured from deployment telemetry or from certification logs. If the certification team collects realized frequencies, it must state whether the priors were revised; any revised model is a **new** specification version, not a silent change.

## 4. Drawing test cases

Algorithm (any CSPRNG or well-seeded PCG/Mersenne Twister is acceptable; the seed must be published):

1. Pick seed `s`, set `t = 0`.
2. Draw each component of `S_0` independently from its `w_d` row (inverse-CDF sampling). This makes the chain stationary from the first sample.
3. For `t = 1 … M−1`, for each dimension `d`:
   - draw `U ~ Uniform(0,1)`;
   - if `U ≤ λ_d`, set `S_{t+1,d} = S_{t,d}`;
   - else draw `S_{t+1,d}` from `w_d` (inverse CDF).
   - Equivalently, sample the row of the explicit matrix in §2.3.
4. Map `S_t` to an input and command:

```
W,H  = R_t
F    = N_t
source string from §1.1 with size=WxH:rate=30
ffmpeg -f lavfi -i "<source>" -frames:v F -pix_fmt yuv420p -f rawvideo case_t.yuv
x264 --input-res WxH --fps 30 --preset <P_t> --crf <C_t> --threads <T_t> -o case_t.264 case_t.yuv
```

   Run the candidate implementation with the **identical** command shape and the **identical** YUV file. Use the same `--keyint`/GOP defaults unless a later spec says otherwise.
5. Record for every case: `s`, `t`, the six state values, SHA-256 of `case_t.yuv`, SHA-256 of both encoded files, measured size/PSNR/SSIM, and any decoder errors.
6. If a generated input or encode fails, redraw that case (advance `t`) and report the failure; do not silently substitute a hand-picked case.
7. **Coverage guardrail:** for `M ≥ 100`, report the realized marginal frequency of every value in every dimension. If a value never occurs, either draw a fresh batch with a new published seed or explicitly label the run as *stratified* after forcing one occurrence and reweight the aggregate statistics. A stratified/forced draw is not a pure sample from `π` and must be declared.
8. Suggested run sizes: ≥100 cases for an initial smoke certification, ≥300 for a final statistical claim. The model does not itself fix `M` or the confidence level; that is the certification team's choice.

## 5. What the model does and does not claim

- It defines a **population of test cases**, not an encoder behaviour guarantee.
- It does **not** specify pass/fail thresholds; those are in `quality-envelope.md`.
- It does **not** assert that any dimension is independent in the real world; the factorization is an explicit modelling simplification.
- All numeric weights and `λ` values are **UNVERIFIED priors (UNKNOWN)**. Reproducibility, not empirical representativeness, is the property this model guarantees.
