# Dirty-side QA audit of the per-file rebuild specifications

**Auditor role:** dirty-side QA. **Scope:** `docs/rebuild/*.md` (one spec per reference source file), checked against `docs/rebuild/README.md` (mandatory 11-section skeleton + copyright rule) and `docs/rebuild/INDEX.md`. Reference source at `E:\_x264_dirty_stash\x264-source` was **read only**; no other file was modified.

**Audit snapshot / caveat:** the tree was being written concurrently during this audit. Observation sequence: 215 → 220 → 225 → 226 → 237 → 240 → 245 → 249 → **250** spec files; `INDEX.md` was itself regenerated mid-audit (progress line changed from `127 / 261` to `149 / 261`). All counts below are from the **final pass** and are explicitly marked with that snapshot size. Re-run this audit after writers stop.

**Line-count convention:** INDEX uses *non-blank* lines (`Measure-Object -Line`). I report `nb` = non-blank lines (comparable with INDEX) and `phys` = physical lines as seen in an editor. README depth targets (250–500 core, 120–300 infra, ≥60 trivial header) are interpreted against `phys`.

---

## 1. Executive summary

| Check | Result (snapshot: 250 docs) |
| --- | --- |
| Dirty-side marker present | **PASS — 0 / 250 missing** |
| All 11 mandatory sections present | **FAIL — 52 / 250 docs** (all missing `Header`; 20 also missing `References`) |
| Below 60 non-blank lines | **FAIL — ~97 / 250 docs** (worst: 30 nb / 52 phys) |
| Copyright spot-check (26 docs sampled) | **PASS — no verbatim source, no copied comments, no code blocks found** |
| Section 8 concrete optimization tips | **WEAK — 21 docs** with no optimization language at all |
| Section 8 references | **WEAK — 51 docs** have no references *inside* section 8 (most do have a separate section 9) |
| Section 10 verification checklist | **PASS — 0 missing; 1 thin** (`common/ppc/predict.h.md`) |
| I1 (CAVLC I-slice) critical docs missing | **PASS — 0 missing**, but 7 are below depth target |
| INDEX staleness | **FAIL at audit start; regenerated mid-audit; 12 source files still have no spec** |

**Bottom line:** the skeleton defect is systematic (four architecture families use 9- or 10-section templates), and the thinness defect is concentrated in architecture ports and peripheral infrastructure. The I1-critical C/CABAC/CAVLC core is structurally complete and clean of copied code, but `encoder/cavlc.c.md`, the frame/param structures, and the VLC table docs are too thin to implement from at the required depth.

---

## 2. Mandatory 11 sections and dirty-side marker

### 2.1 Marker — PASS
Every one of the 250 spec files contains the phrase `dirty-side` / `Dirty-side` (typically "Dirty-side behavioural specification"). No remediation needed.

### 2.2 Skeleton — FAIL: 52 docs (final snapshot)

All 52 use a **shortened numbered template**, so the section numbers do not even line up with README's numbering. Detection was by section *name*, not number, so variant headings such as `## 1. Header / identity` (`common/bitstream.c.md`) and `## 10. Verification checklist (observable conformance checks)` were accepted.

| Family | Docs | Missing | Root cause |
| --- | --- | --- | --- |
| `common/arm/*.md` | 18 | `Header` only | Uses a 10-section template: `1 Purpose, 2 Public interface, 3 State, 4 Algorithm, 5 Dependencies, 6 Constants, 7 Performance notes, 8 References, 9 Verification checklist, 10 Open questions`. No Header block and all numbers shifted by −1. |
| `common/loongarch/*.md` | 8 | `Header`, `References` | Uses a 9-section template (same as mips). References appear only as an inline `**References:**` bullet inside section 7/8, not as a dedicated section. |
| `common/mips/*.md` | 13 | `Header`, `References` | Same 9-section template. |
| `common/ppc/*.md` | 13 | `Header` only | Uses a 10-section template with a dedicated References section. |

Full list (paths relative to `docs/rebuild/`; `nb`/`phys` = non-blank/physical lines at final pass):

- **arm, missing `Header` (18):** `common/arm/asm.S.md` (42/68), `common/arm/bitstream.h.md` (47/74), `common/arm/bitstream-a.S.md` (42/65), `common/arm/cpu-a.S.md` (41/63), `common/arm/dct.h.md` (51/76), `common/arm/dct-a.S.md` (51/82), `common/arm/deblock.h.md` (53/80), `common/arm/deblock-a.S.md` (46/74), `common/arm/mc.h.md` (48/74), `common/arm/mc-a.S.md` (55/87), `common/arm/mc-c.c.md` (50/79), `common/arm/pixel.h.md` (57/84), `common/arm/pixel-a.S.md` (54/85), `common/arm/predict.h.md` (54/81), `common/arm/predict-a.S.md` (48/73), `common/arm/predict-c.c.md` (44/69), `common/arm/quant.h.md` (54/80), `common/arm/quant-a.S.md` (49/76).
- **loongarch, missing `Header` + `References` (8):** `common/loongarch/dct.h.md` (54/80), `common/loongarch/deblock.h.md` (39/64), `common/loongarch/mc.h.md` (56/83), `common/loongarch/mc-c.c.md` (52/80), `common/loongarch/pixel.h.md` (42/72), `common/loongarch/pixel-c.c.md` (51/…), `common/loongarch/predict.h.md` (48/73), `common/loongarch/quant.h.md` (47/72).
- **mips, missing `Header` + `References` (13):** `common/mips/dct.h.md` (47/72), `common/mips/dct-c.c.md` (79/119), `common/mips/deblock.h.md` (43/68), `common/mips/deblock-c.c.md` (85/128), `common/mips/macros.h.md` (69/96), `common/mips/mc.h.md` (42/67), `common/mips/mc-c.c.md` (80/119), `common/mips/pixel.h.md` (40/71), `common/mips/pixel-c.c.md` (77/115), `common/mips/predict.h.md` (46/70), `common/mips/predict-c.c.md` (85/129), `common/mips/quant.h.md` (43/67), `common/mips/quant-c.c.md` (76/120).
- **ppc, missing `Header` (13):** `common/ppc/dct.c.md` (45/73), `common/ppc/dct.h.md` (41/65), `common/ppc/deblock.c.md` (39/65), `common/ppc/deblock.h.md` (39/63), `common/ppc/mc.c.md` (54/88), `common/ppc/mc.h.md` (37/62), `common/ppc/pixel.c.md` (51/82), `common/ppc/pixel.h.md` (**30/52**), `common/ppc/ppccommon.h.md` (44/70), `common/ppc/predict.c.md` (42/…), `common/ppc/predict.h.md` (**33/56**), `common/ppc/quant.c.md` (39/…), `common/ppc/quant.h.md` (39/62).

Note: `common/arm/*` and `common/ppc/*` *do* contain a dedicated References section; they only need the Header block added and renumbering. `common/mips/*` and `common/loongarch/*` need both.

---

## 3. Depth / thinness

### 3.1 Counts
- **~97 / 250 docs are below 60 non-blank lines** (README: "Trivial headers: at least 60 lines with full interface semantics").
- **7 docs are below 40 non-blank lines** (final pass): `common/ppc/pixel.h.md` (30/52), `common/ppc/predict.h.md` (33/56), `tools/digress/scm/__init__.py.md` (36/62), `common/ppc/mc.h.md` (37/62), `common/ppc/quant.h.md` (39/62), `filters/video/internal.h.md` (39/64), `common/aarch64/mc.h.md` (39/64), plus `common/ppc/deblock.h.md` (39/63), `common/ppc/deblock.c.md` (39/65), `common/loongarch/deblock.h.md` (39/64), `common/aarch64/pixel-a-common.S.md` (39/64).
- Concentration: architecture ports (arm 18/18, ppc 13/13, mips ~10/13, loongarch ~7/8, aarch64 ~14/30) and peripheral infra (filters/video, output/, input/, tools/digress/scm).

### 3.2 Core files below README depth target (250–500 phys)
These are marked "done" in INDEX but are far below the stated core target:

| Doc | phys / nb | Target | Comment |
| --- | --- | --- | --- |
| `encoder/cavlc.c.md` | 146 / 88 | 250–500 | **Most important I1 gap.** Entropy coder for I-slices described in less than 60% of minimum target. |
| `encoder/cabac.c.md` | 93 / … | 250–500 | CABAC writer also short. |
| `encoder/api.c.md` | 60 / … | 120–300 (infra) | Borderline. |
| `common/frame.c.md` | 99 / 61 | 120–300 | I1 frame structures. |
| `common/frame.h.md` | 86 / 54 | 120–300 | I1 frame structures. |
| `common/common.h.md` | 93 / 59 | 120–300 | I1 param/t context. |
| `common/tables.c.md` | 110 / 70 | 120–300 | Holds CAVLC VLC table facts. |
| `common/tables.h.md` | 75 / 49 | 120–300 | Same. |
| `encoder/set.h.md` | 110 / 68 | 120–300 | SPS/PPS structures. |
| `encoder/macroblock.h.md` | 139 / 94 | 120–300 (nb basis) | Slightly under on nb basis. |

### 3.3 INDEX staleness
- At audit start INDEX reported `127 / 261` complete; it was regenerated during the audit to `149 / 261` and still showed only **250 spec files on disk vs 261 source paths**.
- **12 source files still have no spec** (all `pending`): `common/loongarch/dct-a.S`, `common/loongarch/deblock-a.S`, `common/loongarch/loongson_asm.S`, `common/loongarch/loongson_util.S`, `common/loongarch/mc-a.S`, `common/loongarch/pixel-a.S`, `common/loongarch/predict-a.S`, `common/loongarch/quant-a.S`, `common/loongarch/sad-a.S`, `tools/bash-autocomplete.sh`, `tools/countquant_x264.pl`, `tools/test_x264.py`.
- One doc on disk is **not in INDEX**: `common/x86/cabac.h.md` (170 nb — a substantial, well-formed doc). INDEX generation needs to pick it up or the source skeleton entry is missing.
- Several docs that INDEX previously marked `pending` were written during the audit and are **very thin** (e.g. `common/ppc/predict.c.md` 42 nb, `common/ppc/quant.c.md` 39 nb, `common/loongarch/mc-c.c.md` 52 nb). These should not be counted as "done" without expansion.

---

## 4. Copyright spot-check (README rule: no verbatim source, near-verbatim pseudocode, or copied comments)

### 4.1 Method
I sampled **26 docs** spanning scalar C cores, headers, entropy coders, SIMD C and assembly, and one non-x86 port, and compared each against its corresponding stashed source:

`common/dct.c`, `common/bitstream.c`, `common/cabac.c`, `common/macroblock.c`, `common/mc.c`, `common/predict.c`, `common/quant.c`, `common/pixel.c`, `common/deblock.c`, `common/mvpred.c`, `common/frame.c`, `common/set.c`, `common/tables.c`, `common/common.c`, `encoder/cavlc.c`, `encoder/cabac.c`, `encoder/set.c`, `encoder/macroblock.c`, `encoder/rdo.c`, `encoder/encoder.c`, `x264.h`, `common/x86/dct-a.asm`, `common/x86/quant-a.asm`, `common/x86/predict-c.c`, `common/mips/dct-c.c`, `common/aarch64/mc-c.c`.

Checks applied: (a) whitespace-stripped, lower-cased exact source lines ≥45 chars present in the doc; (b) whitespace-stripped source comment blocks ≥60 chars present in the doc; (c) doc prose lines ≥70 chars present verbatim in source; (d) all fenced code blocks in **all 250 docs** scanned for C/assembly statement lines (`for/if/while/#define/#include/return/memcpy/static/…`). I also manually read `common/dct.c.md`, `encoder/cavlc.c.md`, `encoder/set.c.md`, `common/aarch64/mc.h.md`, `common/arm/asm.S.md` and `common/mips/dct-c.c.md`.

### 4.2 Result — PASS for the sampled set
- **0 copied comment blocks.** No source comment text appears in any sampled doc.
- **0 docs with ≥4 C/assembly statement lines inside fenced code blocks** (all 250 docs scanned).
- Only exact matches found were **function signatures**, which are explicitly allowed facts: `common/pixel.c.md` (`x264_pixel_init`), `common/deblock.c.md` (`x264_deblock_init`), `common/set.c.md` (`x264_cqm_parse_file`), `encoder/cabac.c.md` (3 signatures), `encoder/encoder.c.md` (`x264_encoder_open`), `common/x86/predict-c.c.md` (5 init signatures), `common/aarch64/mc-c.c.md` (`x264_mc_init_aarch64`). Also one exact match in `common/deblock.c.md` of a numeric QP-step list (`8,9,10,12,13,15,…`) — a numeric constant, allowed.
- Manual reading confirms behaviour is expressed in original prose with original variable names, e.g. `common/dct.c.md` §5.1 derives the 4×4 butterfly as `p=a+d, q=b+c, r=a-d, s=b-c; y0=p+q, y1=2r+s, …` — an equation, not copied code; `encoder/cavlc.c.md` §5.5 restates the bit layout in its own words.

**Allowed vs disallowed (applied throughout):** names/signatures, constants, numeric tables, bit layouts, scan permutations, equations, normative profile/level rules = facts, allowed. Copying the source's expressions, statement order, comment wording, or identifier choices verbatim = disallowed. No instance of the latter was found.

### 4.3 Limitation / UNKNOWN
Near-verbatim pseudocode with systematically renamed identifiers and reworded comments can evade substring matching. This was checked manually only for the six docs named above; the remaining **224 docs not manually line-reviewed** are **UNKNOWN** with respect to that subtler form. Automated evidence strongly suggests the corpus is clean.

---

## 5. Section 8 and section 10 quality

### 5.1 Section 8 — concrete optimization tips
**21 docs have no optimization language in section 8 at all** (no "optimize/faster/speed/cache/SIMD/prefetch/branch/alignment/tip/lever"):

`autocomplete.c.md`, `common/aarch64/asm-offsets.h.md`, `common/aarch64/dct.h.md`, `common/aarch64/pixel.h.md`, `common/aarch64/predict-c.c.md`, `common/arm/cpu-a.S.md`, `common/loongarch/dct.h.md`, `common/loongarch/deblock.h.md`, `common/loongarch/pixel.h.md`, `common/loongarch/predict.h.md`, `common/loongarch/quant.h.md`, `common/mips/dct.h.md`, `common/mips/pixel.h.md`, `common/mips/predict.h.md`, `common/mips/quant.h.md`, `common/win32thread.h.md`, `input/ffms.c.md`, `input/raw.c.md`, `output/matroska_ebml.h.md`, `output/output.h.md`, `output/raw.c.md`.

None of these are I1-critical C files, so this is a compliance/debt item rather than an I1 blocker.

### 5.2 Section 8 — public references
**51 docs' section 8 contains no citation/public-reference language.** Most of these *do* have a proper standalone `## 9. References`, so this is a strict reading of README ("concrete, **referenced** optimization tips"): the tips are not tied to literature in-place. Notable names include `common/quant.h.md`, `common/x86/dct.h.md`, `common/x86/predict*.md`, `encoder/macroblock.c.md/.h.md`, `encoder/rdo.c.md`, `encoder/set.c.md/.h.md`, `encoder/me.h.md`, `encoder/ratecontrol.h.md`, `encoder/slicetype*.md`, `common/arm/*`, `common/ppc/*`, `common/loongarch/*`, `common/mips/*`. `common/quant.h.md`, `encoder/macroblock.c.md` and `encoder/rdo.c.md` are I1-adjacent and should get in-place citations.

### 5.3 Section 10 — verification checklist
- **PASS:** 0 docs missing the section. Variant heading `## 10. Verification checklist (observable conformance checks)` in `common/bitstream.c.md` is fine.
- **1 thin:** `common/ppc/predict.h.md` — checklist body only 198 chars / 3 bullets, in a doc of 33 nb / 56 phys; it lacks observable checks such as "compare emitted prediction samples bit-for-bit against scalar C", guard-byte overread checks, and SIMD-vs-C equivalence over random neighbours.
- The I1-critical docs have good observable checks, e.g. `encoder/set.c.md` §10 (independent SPS parse, bit-for-bit RBSP comparison, boundary level tests) and `encoder/cavlc.c.md` §10 (bit-for-bit stream comparison, decode with independent decoder, residual edge cases).

---

## 6. I1 critical-path assessment (CAVLC I-slice encoder)

Required areas from the goal: SPS/PPS derivation, macroblock, DCT/quant, prediction, bitstream/CAVLC, frame/param structures.

| Area | Spec | nb / phys | 11 sections? | Verdict for I1 |
| --- | --- | --- | --- | --- |
| SPS/PPS | `encoder/set.c.md` | 109 / 170 | yes | **Ready** — detailed derivation + bit layout + verification. |
| SPS/PPS structs | `encoder/set.h.md` | 68 / 110 | yes | Thin vs 120–300 target; expand. |
| Macroblock | `common/macroblock.c.md` | 219 / 302 | yes | **Ready** (core target met). |
| Macroblock structs | `common/macroblock.h.md` | 128 / 179 | yes | OK; slightly under on nb basis. |
| MB encode | `encoder/macroblock.c.md` | 167 / 249 | yes | **Ready**; add in-place references in §8. |
| MB encode structs | `encoder/macroblock.h.md` | 94 / 139 | yes | Expand to ≥120 nb. |
| DCT | `common/dct.c.md` | 263 / 337 | yes | **Ready** — equations, scans, verification. |
| DCT types | `common/dct.h.md` | 120 / 157 | yes | Ready. |
| Quant | `common/quant.c.md` | 238 / 298 | yes | **Ready**. |
| Quant types | `common/quant.h.md` | 120 / 159 | yes | Ready; §8 lacks in-place refs. |
| Prediction | `common/predict.c.md` | 243 / 298 | yes | **Ready**. |
| Prediction types | `common/predict.h.md` | 117 / 160 | yes | Ready. |
| Bitstream | `common/bitstream.c.md` | 169 / 232 | yes | **Ready**. |
| Bitstream types | `common/bitstream.h.md` | 130 / 180 | yes | Ready. |
| CAVLC | `encoder/cavlc.c.md` | **88 / 146** | yes | **THIN — top I1 risk.** Half the core depth target; I-slice mb_type/CBP/VLC detail is compressed into §5.5 and the exact VLC tables are deliberately deferred to the spec. |
| Frame structs | `common/frame.c.md` | **61 / 99** | yes | Thin; below infra target. |
| Frame structs | `common/frame.h.md` | **54 / 86** | yes | Thin; below infra target. |
| Param/context | `x264.h.md` | 179 / 250 | yes | Ready. |
| Param/context | `common/common.h.md` | **59 / 93** | yes | Thin; below infra target. |
| VLC tables | `common/tables.c.md` | **70 / 110** | yes | Thin; CAVLC table contents/derivation need public-spec pointers. |
| VLC tables | `common/tables.h.md` | **49 / 75** | yes | Thin; type/`vlc_t` semantics under-documented. |
| Integration | `encoder/encoder.c.md` | 115 / 182 | yes | Adequate. |
| RD | `encoder/rdo.c.md` | 159 / 236 | yes | Adequate. |
| Analysis | `encoder/analyse.c.md` | 195 / 302 | yes | Ready. |

**Missing:** none of the above are missing. **Still thin (7):** `encoder/cavlc.c.md`, `common/frame.c.md`, `common/frame.h.md`, `common/common.h.md`, `common/tables.c.md`, `common/tables.h.md`, `encoder/set.h.md` (plus `encoder/macroblock.h.md` borderline).

**Do not mistake architecture files for I1 blockers:** `common/aarch64/predict*.md`, `common/mips/predict*.md`, `common/ppc/predict*.md` and the x86 SIMD variants are dispatch/SIMD contracts — the clean increment can implement scalar C first from the generic `common/predict.c.md` / `common/dct.c.md` / `common/quant.c.md` docs and fill SIMD later.

---

## 7. Prioritized remediation list

**P0 — before the next clean implementation increment (I1)**
1. **Expand `encoder/cavlc.c.md` from 146 to ≥250 phys / ≥200 nb.** Add a complete I-slice syntax walkthrough (I_16x16 vs I_4x4/I_8x8, chroma mode, CBP, QP wrap), the exact `coeff_token`/`level`/`total_zeros`/`run_before` *derivation* with pointers to H.264 Tables 9-5…9-10, and at least 3 concrete bit-vector verification cases. No source text may be pasted.
2. **Expand the frame/param structures:** `common/frame.c.md` 99→≥120 phys, `common/frame.h.md` 86→≥120 phys, `common/common.h.md` 93→≥120 phys. These define the storage the I-slice encoder writes into.
3. **Expand VLC table docs:** `common/tables.h.md` 75→≥120 phys and `common/tables.c.md` 110→≥120 phys; specify `vlc_t` semantics, index derivation (`nC`, `total-1`, `trailing`, suffix_length) and where the clean team must obtain table *values* from the public standard (facts allowed, source text not).
4. **Expand `encoder/set.h.md`** 110→≥120 phys (SPS/PPS struct field ownership and invariants) and **`encoder/macroblock.h.md`** 139 phys / 94 nb to ≥120 nb.

**P1 — skeleton compliance (systematic, mechanical)**
5. Add a README-conformant `## 1. Header` block (source path, one-sentence role, dirty-side marker) and renumber to 11 sections in the **52** arm/loongarch/mips/ppc docs listed in §2.2. Do not change any other content.
6. Add a standalone `## 9. References` section (moving the inline `**References:**` bullets out of §7/8) to the **20** mips + loongarch docs flagged `[Header, References]`.
7. Regenerate `docs/rebuild/INDEX.md` after writers stop; add the missing `common/x86/cabac.h` row and stop counting the 8 newly written but sub-60-line ppc/loongarch docs as complete.

**P2 — section quality**
8. Add concrete optimization tips to the **21** docs in §5.1 (prioritize `common/aarch64/predict-c.c.md`, `common/loongarch/predict.h.md`, `common/mips/predict.h.md` if SIMD ports are in scope).
9. Add in-place public citations to the I1-adjacent §8s of `common/quant.h.md`, `encoder/macroblock.c.md`, `encoder/macroblock.h.md`, `encoder/rdo.c.md`, `encoder/set.c.md`, `encoder/set.h.md` (§5.2).
10. Expand the verification checklist of `common/ppc/predict.h.md` (198 chars / 3 bullets) to ≥5 observable checks including SIMD-vs-C bit-exactness and guard-byte overread tests.

**P3 — completeness / debt**
11. Write the **12** source-file specs still absent (§3.3), starting with `tools/test_x264.py`, `tools/checkasm*` (if any remain) and loongarch assembly helpers. Keep them out of "done" until ≥60 nb.
12. Re-run the copyright substring scan on the **224 docs not manually reviewed** (§4.3) before declaring the corpus copyright-clean in a signed-off sense; also re-run after all concurrent writers finish, since new text may have been added after this snapshot.

---

## 8. UNKNOWN items

- Whether the concurrent writers will add further docs after this snapshot — all counts are point-in-time.
- Whether the 224 docs not manually line-reviewed contain near-verbatim pseudocode with renamed identifiers (automated substring scan found none, but this form is not fully machine-detectable).
- Exact physical line counts for `common/loongarch/pixel-c.c.md`, `common/ppc/predict.c.md`, `common/ppc/quant.c.md` at the final instant (files were changing during the pass); their non-blank counts (51, 42, 39) are stable enough to classify them as thin.
- Whether `common/x86/cabac.h.md` should be added to the INDEX skeleton or removed from the docs tree; INDEX lists no corresponding source row.
