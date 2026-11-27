#!/usr/bin/env python3
"""Independent certification harness for the clean-room x264++ encoder.

Draws cases from ``spec/usage-model.md`` (or the documented built-in fallback,
clearly flagged), runs the black-box oracle and the candidate with identical
CLI settings, times warm-up + interleaved repetitions, verifies candidate
output with ffmpeg, optionally measures PSNR/SSIM against a quality envelope,
writes machine-readable results under ``cert/results/`` and regenerates only
the current-score table in ``score.md``.

Nothing here writes to ``src/`` or ``spec/``.

Examples (PowerShell):
    python cert\\certify.py                             # full certification
    python cert\\certify.py --cases 12 --reps 5          # more samples
    python cert\\certify.py --self-test --cases 2 --reps 2 --frames-cap 60
    python cert\\certify.py --force-fallback --cases 3   # ignore the spec file
"""

from __future__ import annotations

import argparse
import collections
import datetime as dt
import hashlib
import json
import math
import os
import platform
import random
import re
import statistics
import subprocess
import sys
import tempfile
import time
import traceback
from pathlib import Path

import clipgen
import usage_model
import verify

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent
DEFAULT_PRESETS = ["ultrafast", "superfast", "veryfast", "faster", "fast",
                   "medium", "slow", "slower", "veryslow"]
SCHEMA_VERSION = 1
SELFTEST_BANNER = "SELF-TEST ONLY — oracle used as candidate; NO numbers may be published"


# ---------------------------------------------------------------------------
# General helpers
# ---------------------------------------------------------------------------

def now_utc() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds")


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def ensure_mingw_path() -> list[str]:
    """Make MinGW-built x264.exe runnable from plain PowerShell."""
    added = []
    current = os.environ.get("PATH", "").split(os.pathsep)
    for candidate in (Path(r"C:\msys64\mingw64\bin"), Path(r"C:\msys64\usr\bin")):
        if candidate.is_dir() and str(candidate) not in current:
            os.environ["PATH"] = str(candidate) + os.pathsep + os.environ.get("PATH", "")
            current.insert(0, str(candidate))
            added.append(str(candidate))
    return added


def tool_version(exe: Path) -> str:
    try:
        p = subprocess.run([str(exe), "--version"], stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, timeout=30)
        for line in p.stdout.decode("utf-8", "replace").splitlines():
            if line.strip():
                return line.strip()
    except Exception as exc:  # diagnostic only
        return f"<unavailable: {exc}>"
    return "<unknown>"


def encode_args(case: dict, clip: Path, out: Path, extra_args: list[str]) -> list[str]:
    """Identical settings for both encoders; only executable and -o differ."""
    return [
        "--demuxer", "raw",
        "--input-csp", "i420",
        "--input-res", f"{case['width']}x{case['height']}",
        "--fps", str(case["fps"]),
        "--preset", case["preset"],
        "--crf", str(case["crf"]),
        "--threads", str(case["threads"]),
        "--keyint", str(case["keyint"]),
        "--min-keyint", str(case["min_keyint"]),
        "--scenecut", str(case["scenecut"]),
        "--quiet",
    ] + list(case.get("extra_args", [])) + list(extra_args) + ["-o", str(out), str(clip)]


def run_encode(exe: Path, argv: list[str], timeout: float) -> dict:
    t0 = time.perf_counter()
    try:
        p = subprocess.run([str(exe)] + argv, stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, timeout=timeout)
    except subprocess.TimeoutExpired:
        return {"ms": (time.perf_counter() - t0) * 1000.0, "returncode": None,
                "stderr": f"encode timed out after {timeout:g}s", "timed_out": True}
    return {"ms": (time.perf_counter() - t0) * 1000.0, "returncode": p.returncode,
            "stderr": (p.stderr or p.stdout).decode("utf-8", "replace").strip(),
            "timed_out": False}


def _stats(times: list[float], frames: int) -> dict:
    if not times:
        return {"times_ms": [], "median_ms": None, "mean_ms": None,
                "min_ms": None, "max_ms": None, "fps": None}
    med = statistics.median(times)
    return {
        "times_ms": [round(x, 3) for x in times],
        "median_ms": round(med, 3),
        "mean_ms": round(statistics.fmean(times), 3),
        "min_ms": round(min(times), 3),
        "max_ms": round(max(times), 3),
        "fps": round(frames / (med / 1000.0), 2) if med > 0 else None,
    }


def _run_ok(run: dict, out: Path) -> tuple[bool, str]:
    if run["timed_out"]:
        return False, run["stderr"]
    if run["returncode"] != 0:
        return False, f"exit {run['returncode']}: {run['stderr'][-400:]}"
    if not out.is_file() or out.stat().st_size == 0:
        return False, "encoder exited 0 but produced no output"
    return True, ""


# ---------------------------------------------------------------------------
# Per-case measurement
# ---------------------------------------------------------------------------

def benchmark_case(case: dict, manifest: dict, args, clip: Path,
                   work: Path, ffmpeg: Path, envelope: dict) -> dict:
    rec = {
        "case_id": case["case_id"], "state": case["state"], "preset": case["preset"],
        "settings": {k: case[k] for k in
                     ("width", "height", "frames", "fps", "crf", "threads",
                      "keyint", "min_keyint", "scenecut", "content", "seed", "extra_args")},
        "attribute_sources": case["attribute_sources"],
        "clip": {"file": clip.name, "sha256": manifest["sha256"],
                 "content_kind": manifest["content_kind"],
                 "content_guessed": manifest["content_guessed"],
                 "reused": manifest["reused"]},
        "failures": [], "status": "pass",
        "oracle": {"times_ms": []}, "candidate": {"times_ms": []},
    }
    binaries = {"oracle": args.oracle, "candidate": args.candidate}
    extra = list(args.extra_arg)

    def encode(label, out):
        argv = encode_args(case, clip, out, extra)
        run = run_encode(binaries[label], argv, args.timeout)
        ok, err = _run_ok(run, out)
        if not ok:
            rec["failures"].append(f"{label}_encode: {err}")
            rec[label].setdefault("encode_error", err)
        return run, ok

    last_out = {"oracle": None, "candidate": None}
    sizes = {"oracle": [], "candidate": []}
    hashes = {"oracle": [], "candidate": []}
    try:
        # Warm-up: discarded, never timed.
        for k in range(args.warmups):
            for label in ("oracle", "candidate"):
                warm = work / f"{case['case_id']}_warm{k}_{label}.264"
                _, ok = encode(label, warm)
                warm.unlink(missing_ok=True)
                if not ok:
                    return _finish_case(rec, case, None, None, ffmpeg, envelope,
                                        work, args, clip)

        for rep in range(args.reps):
            # Interleave A/B; flip direction each rep to cancel drift.
            order = ("oracle", "candidate") if rep % 2 == 0 else ("candidate", "oracle")
            for label in order:
                out = work / f"{case['case_id']}_rep{rep}_{label}.264"
                run, ok = encode(label, out)
                if not ok:
                    continue
                rec[label]["times_ms"].append(run["ms"])
                sizes[label].append(out.stat().st_size)
                hashes[label].append(sha256_file(out))
                last_out[label] = out
    except OSError as exc:
        rec["failures"].append(f"io_error: {exc}\n{traceback.format_exc(limit=3)}")

    return _finish_case(rec, case, last_out, {"sizes": sizes, "hashes": hashes},
                        ffmpeg, envelope, work, args, clip)


def _finish_case(rec: dict, case: dict, outputs, raw, ffmpeg: Path,
                 envelope: dict, work: Path, args, clip: Path) -> dict:
    sizes = raw["sizes"] if raw else {"oracle": [], "candidate": []}
    hashes = raw["hashes"] if raw else {"oracle": [], "candidate": []}
    for label in ("oracle", "candidate"):
        rec[label].update(_stats(rec[label]["times_ms"], case["frames"]))
        rec[label]["sizes_bytes"] = sizes[label]
        rec[label]["hash_full"] = (collections.Counter(hashes[label]).most_common(1)[0][0]
                                   if hashes[label] else None)
        rec[label]["reps_identical"] = len(set(hashes[label])) <= 1
        if sizes[label]:
            rec[label]["size_bytes"] = int(statistics.median(sizes[label]))

    candidate_out = outputs.get("candidate") if outputs else None
    oracle_out = outputs.get("oracle") if outputs else None
    if candidate_out is not None:
        ok, frames, err = verify.decode_check(ffmpeg, candidate_out, case["frames"],
                                              args.timeout)
        rec["candidate"]["decode"] = {"ok": ok, "frames": frames, "error": err,
                                      "file": candidate_out.name}
        if not ok:
            rec["failures"].append("candidate_decode: " + err)
    else:
        rec["candidate"]["decode"] = {"ok": False, "frames": None,
                                      "error": "no candidate output"}

    if oracle_out is not None:
        ok, frames, err = verify.decode_check(ffmpeg, oracle_out, case["frames"], args.timeout)
        rec["oracle"]["decode"] = {"ok": ok, "frames": frames, "error": err,
                                   "file": oracle_out.name}
        if not ok:
            rec["failures"].append("oracle_decode: " + err)

    measure_quality = envelope.get("status") in (
        "relative bands", "absolute thresholds", "mentioned (no numeric contract)")
    rec["checks"] = {"validity": True, "quality": None}
    if candidate_out is not None:
        stream = verify.stream_info(ffmpeg, candidate_out, args.timeout)
        rec["candidate"]["stream"] = stream
        if stream.get("error"):
            rec["failures"].append("validity: " + stream["error"])
        else:
            if stream.get("codec_name") != "h264":
                rec["failures"].append("validity: codec is not h264")
            if stream.get("pix_fmt") != "yuv420p":
                rec["failures"].append("validity: pix_fmt is not yuv420p")
            if (stream.get("width"), stream.get("height")) != (case["width"], case["height"]):
                rec["failures"].append("validity: decoded resolution mismatch")

    if measure_quality and candidate_out is not None:
        candidate_metrics = verify.measure_quality(
            ffmpeg, clip, candidate_out, case["width"], case["height"], case["fps"],
            work, args.timeout)
        reference_metrics = None
        if oracle_out is not None:
            reference_metrics = verify.measure_quality(
                ffmpeg, clip, oracle_out, case["width"], case["height"], case["fps"],
                work, args.timeout)
        rec["quality"] = candidate_metrics
        rec["quality"]["envelope_status"] = envelope["status"]
        rec["quality"]["reference"] = reference_metrics
        failures = verify.evaluate_quality(
            candidate_metrics, reference_metrics or {},
            rec["candidate"].get("size_bytes"), rec["oracle"].get("size_bytes"),
            envelope, case["width"], case["height"],
            rec["candidate"].get("stream", {}))
        for failure in failures:
            rec["failures"].append("quality: " + failure)
        rec["checks"]["quality"] = not failures

    validity_prefixes = ("oracle_encode", "candidate_encode", "oracle_decode",
                         "candidate_decode", "validity")
    rec["checks"]["validity"] = not any(
        f.startswith(validity_prefixes) for f in rec["failures"])

    o, c = rec["oracle"], rec["candidate"]
    if o.get("median_ms") and c.get("median_ms"):
        rec["speedup"] = round(o["median_ms"] / c["median_ms"], 4)
    else:
        rec["speedup"] = None
    if o.get("size_bytes") and c.get("size_bytes") is not None:
        rec["size_delta_bytes"] = c["size_bytes"] - o["size_bytes"]
        rec["size_delta_pct"] = round(100.0 * rec["size_delta_bytes"] / o["size_bytes"], 3)
    else:
        rec["size_delta_bytes"] = None
        rec["size_delta_pct"] = None

    rec["exposure_s"] = round((sum(o["times_ms"]) + sum(c["times_ms"])) / 1000.0, 3)
    if rec["failures"]:
        rec["status"] = "fail"
    return rec


# ---------------------------------------------------------------------------
# Reliability (confidence / MTTF intent)
# ---------------------------------------------------------------------------

def chi2_ppf(p: float, k: float) -> float:
    """Wilson-Hilferty approximation; adequate for reporting intent."""
    z = statistics.NormalDist().inv_cdf(p)
    return k * (1.0 - 2.0 / (9.0 * k) + z * math.sqrt(2.0 / (9.0 * k))) ** 3


def reliability_from_log(path: Path) -> dict:
    records = []
    if path.is_file():
        for line in path.read_text(encoding="utf-8").splitlines():
            if not line.strip():
                continue
            try:
                r = json.loads(line)
            except json.JSONDecodeError:
                continue
            if r.get("mode") == "certification":
                records.append(r)
    if not records:
        return {"status": "no certification samples yet", "cases": 0,
                "failures": 0, "exposure_s": 0.0, "confidence": 0.95,
                "mttf_point_estimate_s": None, "interval_s": None,
                "interpretation": (
                    "MTTF intent only: no certification sample has been logged; "
                    "no reliability claim is made.")}
    exposure = sum(float(r.get("exposure_s", 0.0)) for r in records)
    failures = sum(1 for r in records if r.get("failures"))
    out = {"status": "estimated", "cases": len(records), "failures": failures,
           "exposure_s": round(exposure, 3), "confidence": 0.95,
           "model": "exponential failures per executed case; Wilson-Hilferty chi-square CI"}
    if failures == 0:
        lower = exposure / (-math.log(0.05)) if exposure > 0 else None
        out.update(mttf_point_estimate_s=None,
                   interval_s={"lower_95pct_s": round(lower, 3)} if lower else None,
                   interpretation=(
                       "Zero observed failures: one-sided 95% lower bound on MTTF "
                       "only. Not a demonstrated MTTF; more samples are required."))
    else:
        lo = 2.0 * exposure / chi2_ppf(0.975, 2 * failures + 2)
        hi = 2.0 * exposure / chi2_ppf(0.025, 2 * failures)
        out.update(mttf_point_estimate_s=round(exposure / failures, 3),
                   interval_s={"lower_95pct_s": round(lo, 3),
                               "upper_95pct_s": round(hi, 3)},
                   interpretation=(
                       "Illustrative exponential MTTF estimate from cumulative "
                       "certification samples; too few failures/samples for a claim."))
    return out


# ---------------------------------------------------------------------------
# score.md table regeneration (only the current-score table is touched)
# ---------------------------------------------------------------------------

SCORE_HEADER = ["Preset", "Reference (fps)", "x264++ (fps)", "Speedup",
                "Valid H.264", "Size delta", "Notes"]
SCORE_ALIGN = ["---", "---:", "---:", "---:", "---:", "---:", "---"]


def _fmt(v, spec=".2f"):
    return "—" if v is None else format(v, spec)


def generate_score_table(result: dict, blocked: bool) -> list[str]:
    by_preset = collections.defaultdict(list)
    for case in result.get("cases", []):
        if case.get("status") == "pass" and case.get("speedup"):
            by_preset[case["preset"]].append(case)
    presets = list(DEFAULT_PRESETS)
    for p in by_preset:
        if p not in presets:
            presets.append(p)
    reps = result["settings"]["reps"]
    model_note = "; fallback usage model" if result["usage_model"]["fallback_used"] else ""
    if result["settings"]["cases_requested"] < 100:
        model_note += "; SMOKE sample (<100 cases)"
    rows = []
    for preset in presets:
        cases = by_preset.get(preset, [])
        if blocked:
            rows.append([preset, "—", "—", "—", "—", "—",
                         "Not certified yet — no candidate binary"])
            continue
        if not cases:
            failed = [c for c in result.get("cases", []) if c["preset"] == preset]
            reason = ("no case passed" if failed else "no case sampled")
            rows.append([preset, "—", "—", "—", "—", "—",
                         f"Not certified for this preset ({reason})"])
            continue
        oracle_ms = statistics.median([c["oracle"]["median_ms"] for c in cases])
        candidate_ms = statistics.median([c["candidate"]["median_ms"] for c in cases])
        speedup = oracle_ms / candidate_ms if candidate_ms else None
        fps_o = statistics.median([c["oracle"]["fps"] for c in cases if c["oracle"]["fps"]])
        fps_c = statistics.median([c["candidate"]["fps"] for c in cases if c["candidate"]["fps"]])
        decoded = sum(1 for c in cases if c["candidate"].get("decode", {}).get("ok"))
        size_deltas = [c["size_delta_pct"] for c in cases if c.get("size_delta_pct") is not None]
        size = (f"{statistics.median(size_deltas):+.1f}%"
                if size_deltas else "—")
        note = f"{len(cases)} case(s) × {reps} reps"
        metrics = [c.get("quality", {}) for c in cases if c.get("quality")]
        if metrics:
            psnrs = [m["psnr_db"] for m in metrics if m.get("psnr_db") is not None]
            ssims = [m["ssim"] for m in metrics if m.get("ssim") is not None]
            if psnrs:
                note += f"; PSNR {statistics.median(psnrs):.1f} dB"
            if ssims:
                note += f"; SSIM {statistics.median(ssims):.3f}"
        rows.append([preset, _fmt(fps_o), _fmt(fps_c), _fmt(speedup, ".3f") + "x",
                     f"{decoded}/{len(cases)}", size, note + model_note])

    lines = ["| " + " | ".join(SCORE_HEADER) + " |",
             "| " + " | ".join(SCORE_ALIGN) + " |"]
    lines += ["| " + " | ".join(r) + " |" for r in rows]
    return lines


def update_score_table(score_path: Path, table: list[str]) -> bool:
    """Replace only the first Markdown table under '## Current score'."""
    try:
        text = score_path.read_text(encoding="utf-8")
    except OSError as exc:
        print(f"[error] cannot read {score_path}: {exc}", file=sys.stderr)
        return False
    newline = "\r\n" if "\r\n" in text else "\n"
    had_final = text.endswith(("\n", "\r"))
    lines = text.splitlines()
    heading = next((i for i, ln in enumerate(lines)
                    if ln.strip().lower().startswith("## current score")), None)
    if heading is None:
        print(f"[error] no '## Current score' heading in {score_path}; table not updated",
              file=sys.stderr)
        return False
    sep = re.compile(r"^\|[\s:|-]+\|$")
    start = None
    for i in range(heading + 1, len(lines) - 1):
        if lines[i].strip().startswith("|") and sep.match(lines[i + 1].strip()):
            start = i
            break
    if start is None:
        print(f"[error] no current-score table in {score_path}; table not updated",
              file=sys.stderr)
        return False
    end = start
    while end < len(lines) and lines[end].strip().startswith("|"):
        end += 1
    lines[start:end] = table
    out = newline.join(lines)
    if had_final and not out.endswith(("\n", "\r")):
        out += newline
    with score_path.open("w", encoding="utf-8", newline="") as f:
        f.write(out)
    return True


# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------

def write_outputs(result: dict, results_dir: Path, run_id: str, args) -> None:
    results_dir.mkdir(parents=True, exist_ok=True)
    payload = json.dumps(result, indent=2) + "\n"
    (results_dir / "latest.json").write_text(payload, encoding="utf-8")
    (results_dir / "runs").mkdir(parents=True, exist_ok=True)
    (results_dir / "runs" / f"{run_id}.json").write_text(payload, encoding="utf-8")
    lines = [
        "# Certification report", "",
        f"_Generated: {result['generated_at_utc']} (run `{run_id}`)_", "",
        f"**Status:** {result['status']} — "
        f"{'speeds publishable' if result['publication']['publish_speeds'] else 'not certified yet / not publishable'}",
        "", "## Provenance", "",
        f"- Mode: `{result['mode']}`",
        f"- Usage model: `{result['usage_model']['source']}` "
        f"(fallback: {result['usage_model']['fallback_used']})",
        f"- Quality envelope: `{result['quality_envelope']['status']}`",
        ""
    ]
    for w in result["warnings"]:
        lines.append(f"- **Warning:** {w}")
    if result["warnings"]:
        lines.append("")
    if result.get("blocked_reason"):
        lines += ["## Blocked", "", result["blocked_reason"], ""]
    summary = result.get("summary", {})
    lines += ["## Summary", "",
              f"- Cases: {summary.get('passing_cases', 0)} passed / "
              f"{summary.get('total_cases', 0)} executed",
              f"- Preset speedups: median {_fmt(summary.get('median_speedup'), '.3f')}x, "
              f"geomean {_fmt(summary.get('geomean_speedup'), '.3f')}x",
              f"- Validity: {_fmt(summary.get('validity_rate'), '.1%')}; "
              f"quality-band compliance: "
              f"{_fmt(summary.get('quality_compliance_rate'), '.1%')}; "
              f"envelope: {summary.get('envelope_passed')}",
              "- Realized marginals (spec §4.3): "
              + "; ".join(f"{k}: {v}" for k, v in
                          summary.get("realized_marginals", {}).items()),
              "", "## Reliability", "",
              result["reliability"].get("interpretation", "n/a"), ""]
    if result.get("cases"):
        lines += ["## Cases", "",
                  "| Case | Preset | Resolution | Frames | Speedup | Decode | Failures |",
                  "|---|---|---:|---:|---:|:---:|---|"]
        for c in result["cases"]:
            s = c["settings"]
            lines.append(
                f"| {c['case_id']} | {c['preset']} | {s['width']}x{s['height']} | "
                f"{s['frames']} | {_fmt(c.get('speedup'), '.3f')} | "
                f"{'yes' if c['candidate'].get('decode', {}).get('ok') else 'no'} | "
                f"{'; '.join(c['failures']) or '—'} |")
        lines.append("")
    (results_dir / "latest.md").write_text("\n".join(lines), encoding="utf-8")
    print(f"[out] {results_dir / 'latest.json'}")
    print(f"[out] {results_dir / 'latest.md'}")


def failed_clip_record(case: dict, reason: str) -> dict:
    return {
        "case_id": case["case_id"], "state": case["state"],
        "preset": case["preset"],
        "settings": {k: case[k] for k in
                     ("width", "height", "frames", "fps", "crf", "threads",
                      "keyint", "min_keyint", "scenecut", "content", "seed", "extra_args")},
        "attribute_sources": case["attribute_sources"],
        "failures": [reason], "status": "fail",
        "checks": {"validity": False, "quality": None},
        "oracle": {"times_ms": [], "decode": {"ok": False, "error": "not attempted"}},
        "candidate": {"times_ms": [], "decode": {"ok": False, "error": "not attempted"}},
        "speedup": None, "exposure_s": 0.0,
    }


def log_case(path: Path, result: dict, case: dict) -> None:
    record = {
        "utc": now_utc(), "run_id": result["run_id"], "mode": result["mode"],
        "case_id": case["case_id"], "preset": case["preset"],
        "status": case["status"], "failures": case["failures"],
        "exposure_s": case.get("exposure_s", 0.0),
    }
    with open(path, "a", encoding="utf-8") as f:
        f.write(json.dumps(record) + "\n")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def parse_args(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--oracle", type=Path, default=REPO_ROOT / "oracle" / "x264.exe")
    ap.add_argument("--candidate", type=Path, default=REPO_ROOT / "build" / "x264pp.exe")
    ap.add_argument("--ffmpeg", type=Path, default=None)
    ap.add_argument("--usage-model", type=Path, default=REPO_ROOT / "spec" / "usage-model.md")
    ap.add_argument("--quality-envelope", type=Path,
                    default=REPO_ROOT / "spec" / "quality-envelope.md")
    ap.add_argument("--cases", type=int, default=9, help="random cases to sample (default 9)")
    ap.add_argument("--reps", type=int, default=3, help="timed reps per case (default 3)")
    ap.add_argument("--warmups", type=int, default=1, help="discarded warm-up runs per encoder")
    ap.add_argument("--seed", type=int, default=20261127, help="sampling RNG seed")
    ap.add_argument("--presets", nargs="+", default=None, help="restrict sampled presets")
    ap.add_argument("--frames-cap", type=int, default=None, help="cap sampled frame counts")
    ap.add_argument("--timeout", type=float, default=600.0, help="per-command timeout (s)")
    # The usage model requires generated media to live outside the repository.
    ap.add_argument("--assets-dir", type=Path,
                    default=Path(tempfile.gettempdir()) / "x264pp-cert-assets")
    ap.add_argument("--work-dir", type=Path, default=SCRIPT_DIR / "temp")
    ap.add_argument("--results-dir", type=Path, default=SCRIPT_DIR / "results")
    ap.add_argument("--score", type=Path, default=REPO_ROOT / "score.md")
    ap.add_argument("--no-score-update", action="store_true")
    ap.add_argument("--force-fallback", action="store_true",
                    help="ignore the spec and use the built-in usage profile")
    ap.add_argument("--self-test", action="store_true",
                    help="use the oracle as candidate; never publish or update score.md")
    ap.add_argument("--extra-arg", action="append", default=[],
                    help="extra CLI arg passed identically to both encoders")
    ap.add_argument("--keep-outputs", action="store_true")
    args = ap.parse_args(argv)
    if args.cases < 1 or args.reps < 1 or args.warmups < 0:
        ap.error("--cases/--reps must be >= 1 and --warmups >= 0")
    return args


def main(argv=None) -> int:
    args = parse_args(argv)
    args.oracle = args.oracle.resolve()
    args.candidate = args.candidate.resolve()
    args.usage_model = args.usage_model.resolve()
    args.quality_envelope = args.quality_envelope.resolve()
    args.score = args.score.resolve()

    mode = "selftest" if args.self_test else "certification"
    results_dir = args.results_dir.resolve()
    if args.self_test:
        results_dir = results_dir / "selftest"
        print("=" * 78)
        print(SELFTEST_BANNER)
        print("=" * 78)
    run_id = (dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
              + f"-{args.seed}")
    mingw_added = ensure_mingw_path()
    ffmpeg = verify.find_ffmpeg(args.ffmpeg)

    model, prov = usage_model.load_usage_model(args.usage_model, args.force_fallback)
    rng = random.Random(args.seed)
    cases, sample_warnings = usage_model.sample_cases(
        model, args.cases, rng, args.seed, args.presets, args.frames_cap)
    envelope = verify.load_quality_envelope(args.quality_envelope)

    result = {
        "schema_version": SCHEMA_VERSION,
        "generated_at_utc": now_utc(), "run_id": run_id, "mode": mode,
        "status": "ok", "certified": False, "blocked_reason": None,
        "publication": {"publish_speeds": False, "reason": "not yet determined"},
        "machine": {"platform": platform.platform(), "processor": platform.processor(),
                    "cpu_count": os.cpu_count(), "python": platform.python_version()},
        "binaries": {
            "oracle": {"path": str(args.oracle), "exists": args.oracle.is_file(),
                       "version": tool_version(args.oracle) if args.oracle.is_file() else None,
                       "sha256": sha256_file(args.oracle) if args.oracle.is_file() else None},
            "candidate": {"path": str(args.candidate),
                          "exists": args.candidate.is_file(), "version": None,
                          "sha256": None},
        },
        "usage_model": prov,
        "quality_envelope": envelope,
        "settings": {
            "cases_requested": args.cases, "reps": args.reps,
            "warmups": args.warmups, "sampling_seed": args.seed,
            "presets_filter": args.presets, "frames_cap": args.frames_cap,
            "timeout_s": args.timeout, "extra_args": args.extra_arg,
            "mingw_path_prepended": mingw_added,
            "timing": "wall clock incl. startup + file write; warm-up discarded; "
                      "A/B interleaved with alternating order per repetition",
            "cli_shape": "x264-compatible; identical argv for both encoders except -o",
        },
        "cases": [], "summary": {}, "warnings": [], "notes": [],
    }
    result["warnings"] = list(prov.get("warnings", [])) + sample_warnings + list(
        envelope.get("warnings", []))
    if args.cases < 100:
        result["warnings"].append(
            f"Only {args.cases} case(s) requested; spec §4 suggests >=100 for an "
            "initial certification batch. Treat this as a smoke run.")
    if prov["fallback_used"]:
        print("[warn] usage model unavailable/unparsed: using built-in fallback profile "
              "(flagged in all outputs)")
    for w in result["warnings"]:
        print("[warn] " + w)

    # Missing binaries: report and stop. No numbers are fabricated.
    missing = []
    if not args.oracle.is_file():
        missing.append(f"oracle binary missing: {args.oracle}")
    candidate_path = args.oracle if args.self_test else args.candidate
    if args.self_test:
        args.candidate = candidate_path
    if not candidate_path.is_file():
        missing.append(f"candidate binary missing: {args.candidate}")
    if ffmpeg is None:
        missing.append("ffmpeg not found (set --ffmpeg or FFMPEG)")
    if not args.score.is_file():
        missing.append(f"score file missing: {args.score}")
    if missing:
        reason = "; ".join(missing)
        result.update(status="blocked", blocked_reason=reason)
        result["publication"] = {"publish_speeds": False,
                                 "reason": "not certified yet — " + reason}
        result["reliability"] = reliability_from_log(results_dir.parent / "test-log.jsonl")
        result["summary"] = {"total_cases": 0, "passing_cases": 0,
                             "median_speedup": None, "geomean_speedup": None}
        write_outputs(result, results_dir, run_id, args)
        if not args.no_score_update and not args.self_test and args.score.is_file():
            if update_score_table(args.score, generate_score_table(result, blocked=True)):
                print(f"[out] updated current-score table in {args.score}")
        print("[status] NOT CERTIFIED YET — " + reason, file=sys.stderr)
        return 2

    result["binaries"]["candidate"] = {
        "path": str(candidate_path), "exists": True,
        "version": tool_version(candidate_path),
        "sha256": sha256_file(candidate_path),
        "self_test_substitute": str(candidate_path) if args.self_test else None,
    }
    if args.self_test:
        result["publication"] = {"publish_speeds": False, "reason": SELFTEST_BANNER}

    work = args.work_dir.resolve()
    work.mkdir(parents=True, exist_ok=True)
    log_path = results_dir.parent / "test-log.jsonl"
    content_sources = prov.get("content_sources") or {}
    use_lavfi = bool(model.get("factored") and content_sources)
    result["settings"]["clip_generator"] = (
        f"ffmpeg lavfi sources from {args.usage_model} §1.1" if use_lavfi
        else "cert/clipgen.py deterministic analytic generator")
    if use_lavfi:
        result["usage_model"]["content_sources"] = content_sources

    for case in cases:
        print(f"[case] {case['case_id']} state={case['state']} "
              f"{case['width']}x{case['height']} {case['frames']}f "
              f"preset={case['preset']} crf={case['crf']} ...")
        try:
            if use_lavfi:
                source = content_sources.get(case["content"])
                if not source:
                    raise RuntimeError(
                        f"no lavfi source defined in the usage model for content "
                        f"{case['content']!r}")
                manifest = clipgen.ensure_ffmpeg_clip(
                    args.assets_dir.resolve(), case, ffmpeg, source, args.timeout)
            else:
                manifest = clipgen.ensure_clip(args.assets_dir.resolve(), case)
        except (RuntimeError, OSError) as exc:
            rec = failed_clip_record(case, "clip_generation: " + str(exc))
            result["cases"].append(rec)
            if mode == "certification":
                log_case(log_path, result, rec)
            print(f"       status=fail clip_generation: {exc}")
            continue
        if manifest.get("content_guessed"):
            w = (f"{case['case_id']}: content {case['content']!r} not recognised; "
                 f"generated '{manifest['content_kind']}' material")
            result["warnings"].append(w)
            print("[warn] " + w)
        clip = args.assets_dir.resolve() / manifest["file"]
        rec = benchmark_case(case, manifest, args, clip, work, ffmpeg, envelope)
        result["cases"].append(rec)
        if mode == "certification":
            log_case(log_path, result, rec)
        print(f"       status={rec['status']} speedup={rec.get('speedup')} "
              f"decode={rec['candidate'].get('decode', {}).get('ok')} "
              f"failures={'; '.join(rec['failures']) or '—'}")

    passing = [c for c in result["cases"] if c["status"] == "pass" and c.get("speedup")]
    speedups = [c["speedup"] for c in passing]
    total = len(result["cases"])
    validity_ok = sum(1 for c in result["cases"] if c.get("checks", {}).get("validity"))
    quality_checked = [c for c in result["cases"]
                       if c.get("checks", {}).get("quality") is not None]
    quality_ok = sum(1 for c in quality_checked if c["checks"]["quality"])
    validity_rate = validity_ok / total if total else None
    quality_rate = quality_ok / len(quality_checked) if quality_checked else None
    # spec/quality-envelope.md §4.3: 100% validity, >=95% band compliance.
    envelope_ok = (validity_rate == 1.0
                   and (quality_rate is None or quality_rate >= 0.95))
    dimensions = {
        "resolution": lambda c: f"{c['settings']['width']}x{c['settings']['height']}",
        "frames": lambda c: c["settings"]["frames"],
        "preset": lambda c: c["preset"],
        "crf": lambda c: c["settings"]["crf"],
        "threads": lambda c: c["settings"]["threads"],
        "content": lambda c: c["settings"]["content"],
        "state": lambda c: c["state"],
    }
    realized = {
        dim: dict(collections.Counter(str(fn(c)) for c in result["cases"]))
        for dim, fn in dimensions.items()
    }
    result["summary"] = {
        "realized_marginals": realized,
        "total_cases": total, "passing_cases": len(passing),
        "failed_cases": total - len(passing),
        "validity_rate": round(validity_rate, 4) if validity_rate is not None else None,
        "quality_compliance_rate": round(quality_rate, 4) if quality_rate is not None else None,
        "aggregate_envelope_rule": "100% validity and >=95% band compliance (spec §4.3)",
        "envelope_passed": envelope_ok,
        "median_speedup": round(statistics.median(speedups), 4) if speedups else None,
        "geomean_speedup": (round(math.exp(statistics.fmean([math.log(x) for x in speedups])), 4)
                            if speedups else None),
    }
    result["reliability"] = reliability_from_log(log_path)
    if mode == "selftest":
        result["status"] = "selftest"
        result["certified"] = False
    elif envelope_ok and passing:
        result["status"] = "ok"
        result["certified"] = True
        result["publication"] = {
            "publish_speeds": True,
            "reason": ("spec §4.3 met: 100% validity, "
                       f"{result['summary']['quality_compliance_rate']} band compliance"),
        }
    else:
        result["status"] = "partial"
        result["certified"] = False
        result["publication"] = {
            "publish_speeds": bool(passing),
            "reason": ("partial: only presets with all cases passing are shown; "
                       "check per-preset Notes; run is not fully certified"),
        }

    write_outputs(result, results_dir, run_id, args)
    if not args.no_score_update and not args.self_test:
        if update_score_table(args.score, generate_score_table(result, blocked=False)):
            print(f"[out] updated current-score table in {args.score}")
    if not args.keep_outputs:
        for f in work.glob("*.264"):
            f.unlink(missing_ok=True)
    print(f"[status] {result['status']} (certified={result['certified']}, "
          f"publish={result['publication']['publish_speeds']})")
    return 0 if result["status"] in ("ok", "selftest") else 1


if __name__ == "__main__":
    sys.exit(main())
