#!/usr/bin/env python3
"""Reproducible real-video A/B benchmark: oracle/x264.exe vs build/x264pp.exe.

The clips come from the user-provided corpus (``D:\\Videos`` by default).  A
curated, hash-pinned subset is listed in ``cert/real_clips.json``: one ready
1920x1080 I420 prefix (Fortnite) plus several MP4 containers.  Containers are
decoded with ffmpeg to a *bounded* number of leading frames as raw I420 and
cached (with a SHA256 manifest) outside the repository.

For every clip x preset the harness runs one discarded warm-up per encoder and
``--reps`` timed repetitions, A/B interleaved with the order flipped on each
repetition.  It records median wall-clock / fps, output size and hash, decodes
each bitstream with ffmpeg, and measures PSNR/SSIM against the exact source
I420 when ffmpeg is available.

The candidate is published as certified **only if it exists, runs, and decodes
cleanly on every case**.  A missing or invalid candidate never fabricates a
speedup: the oracle results are still written as a clearly labelled
reference-only table.  Nothing here writes to ``src/`` or ``spec/``.

Examples (PowerShell, from the repository root):

    python cert\\real_bench.py                                  # default 3 clips x 3 presets
    python cert\\real_bench.py --frames 8 --reps 2 --warmups 1  # quick smoke run
    python cert\\real_bench.py --frames 128 --reps 5            # full real-corpus run
    python cert\\real_bench.py --clips fortnite-raw-1080p --presets medium
    python cert\\real_bench.py --discover                       # list curated clips

Exit codes: 0 completed, 1 partial (some encodes failed), 2 blocked (missing
oracle, ffmpeg or candidate).  The JSON ``status`` / ``candidate_certified``
fields are authoritative; a blocked run still produces the reference table.
"""

from __future__ import annotations

import argparse
import collections
import datetime as dt
import hashlib
import json
import os
import platform
import re
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import verify

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent
SCHEMA_VERSION = 1

# Published, already-established reference anchors (see benchmarks.md / README.md).
# These are historical measurements, not numbers produced by this run.
DOCUMENTED_ANCHORS = [
    {"case": "640x360, 120 f, medium, CRF 23, 6 threads (synthetic testsrc2)",
     "fps": 609.14, "source": "benchmarks.md / README.md"},
    {"case": "1920x1080, 8 f, ultrafast, CRF 23, 6 threads (Fortnite I420)",
     "fps": 126.98, "source": "benchmarks.md / README.md"},
    {"case": "1920x1080, 8 f, medium, CRF 23, 6 threads (Fortnite I420)",
     "fps": 40.61, "source": "benchmarks.md / README.md"},
    {"case": "1920x1080, 8 f, veryslow, CRF 23, 6 threads (Fortnite I420)",
     "fps": 13.84, "source": "benchmarks.md / README.md"},
]


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
    """Make MinGW-built x264.exe runnable from plain Python/PowerShell."""
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
        text = p.stdout.decode("utf-8", "replace").strip()
        if p.returncode != 0:
            return f"<invalid: exit {p.returncode}: {text[-200:]}>"
        return next((ln.strip() for ln in text.splitlines() if ln.strip()), "<empty>")
    except Exception as exc:  # diagnostic only
        return f"<unavailable: {exc}>"


def fmt(value, spec=".2f") -> str:
    return "—" if value is None else format(value, spec)


# ---------------------------------------------------------------------------
# Clip manifest / materialisation
# ---------------------------------------------------------------------------

def load_clips(manifest_path: Path, corpus_dir: Path, only: list[str] | None) -> tuple[list[dict], list[str]]:
    warnings = []
    data = json.loads(manifest_path.read_text(encoding="utf-8"))
    clips = []
    for raw in data.get("clips", []):
        if only and raw["id"] not in only:
            continue
        clip = dict(raw)
        clip["source"] = (corpus_dir / clip["rel"]).resolve()
        clips.append(clip)
    if only:
        known = {c["id"] for c in data.get("clips", [])}
        for cid in only:
            if cid not in known:
                warnings.append(f"unknown --clips id {cid!r} (ignored)")
    return clips, warnings


def _cache_key(clip: dict, frames: int, source_sha: str) -> str:
    payload = {
        "id": clip["id"], "rel": clip["rel"], "kind": clip["kind"],
        "width": clip["width"], "height": clip["height"], "fps": clip["fps"],
        "frames": frames, "source_sha256": source_sha,
    }
    return hashlib.sha256(json.dumps(payload, sort_keys=True).encode("utf-8")).hexdigest()[:20]


def materialize_clip(clip: dict, frames: int, assets_dir: Path, ffmpeg: Path | None,
                     timeout: float) -> dict:
    """Return (path + provenance) for a bounded I420 clip; cached by SHA256."""
    width, height = int(clip["width"]), int(clip["height"])
    if width % 2 or height % 2:
        raise RuntimeError(f"I420 requires even dimensions, got {width}x{height}")
    frame_bytes = width * height * 3 // 2
    expected = frames * frame_bytes
    src = clip["source"]
    if not src.is_file():
        raise RuntimeError(f"source clip missing: {src}")
    source_sha = sha256_file(src)
    expected_source_sha = clip.get("source_sha256")
    sha_match = expected_source_sha in (None, source_sha)

    key = _cache_key(clip, frames, source_sha)
    cache = assets_dir.resolve() / key
    cache.mkdir(parents=True, exist_ok=True)
    out = cache / "clip.yuv"
    manifest = cache / "manifest.json"
    meta = {
        "id": clip["id"], "label": clip["label"], "kind": clip["kind"],
        "source": str(src), "source_rel": clip["rel"],
        "source_sha256": source_sha, "source_sha256_expected": expected_source_sha,
        "source_sha256_match": sha_match, "width": width, "height": height,
        "fps": clip["fps"], "frames": frames, "expected_bytes": expected,
        "frame_bytes": frame_bytes, "file": str(out),
    }

    if manifest.is_file() and out.is_file():
        try:
            cached = json.loads(manifest.read_text(encoding="utf-8"))
            if (cached.get("clip_sha256") == sha256_file(out)
                    and cached.get("expected_bytes") == expected):
                meta.update(reused=True, clip_sha256=cached["clip_sha256"],
                            decode_command=cached.get("decode_command"))
                if not sha_match:
                    meta["warning"] = ("source SHA256 differs from the pinned value; "
                                       "cache key followed the observed hash")
                return meta
        except (OSError, ValueError, KeyError):
            pass  # regenerate

    if clip["kind"] == "raw":
        available = clip.get("available_frames")
        if available is not None and int(available) < frames:
            raise RuntimeError(f"{clip['id']}: {frames}f requested but only {available}f available")
        with open(src, "rb") as f:
            data = f.read(expected)
        if len(data) != expected:
            raise RuntimeError(f"{clip['id']}: raw file has {len(data) // frame_bytes}f, need {frames}f")
        out.write_bytes(data)
        command = f"python: read first {frames} frames ({expected} bytes) from {src.name}"
    else:
        if ffmpeg is None:
            raise RuntimeError("ffmpeg is required to decode container clips")
        command_args = [ffmpeg, "-hide_banner", "-nostats", "-loglevel", "error", "-y",
                        "-i", str(src), "-frames:v", str(frames), "-an", "-sn", "-dn",
                        "-pix_fmt", "yuv420p", "-f", "rawvideo", str(out)]
        p = subprocess.run([str(x) for x in command_args], stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, timeout=timeout)
        command = " ".join(str(x) for x in command_args)
        if p.returncode != 0:
            raise RuntimeError(f"ffmpeg decode failed: {p.stderr.decode('utf-8', 'replace').strip()[-400:]}")
    size = out.stat().st_size
    if size != expected:
        actual = size // frame_bytes
        out.unlink(missing_ok=True)
        raise RuntimeError(
            f"{clip['id']}: decoded {actual}f ({size} bytes), expected {frames}f ({expected} bytes)")
    meta.update(reused=False, clip_sha256=sha256_file(out), decode_command=command)
    manifest.write_text(json.dumps(meta, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    if not sha_match:
        meta["warning"] = "source SHA256 differs from the pinned value in cert/real_clips.json"
    return meta


# ---------------------------------------------------------------------------
# Encoding / measurement
# ---------------------------------------------------------------------------

def encode_args(case: dict, clip: Path, out: Path, frames: int) -> list[str]:
    """Byte-identical argv for both encoders except the -o path."""
    return [
        "--demuxer", "raw", "--input-csp", "i420",
        "--input-res", f"{case['width']}x{case['height']}",
        "--fps", str(case["fps"]),
        "--preset", case["preset"], "--crf", str(case["crf"]),
        "--threads", str(case["threads"]), "--frames", str(frames),
        # --no-progress (rather than --quiet) keeps real error text on stderr
        # when a binary fails, while avoiding terminal progress redraws.
        "--no-progress", "-o", str(out), str(clip),
    ]


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


def encode_ok(run: dict, out: Path) -> tuple[bool, str]:
    if run["timed_out"]:
        return False, run["stderr"]
    if run["returncode"] != 0:
        return False, f"exit {run['returncode']}: {run['stderr'][-300:]}"
    if not out.is_file() or out.stat().st_size == 0:
        return False, "encoder exited 0 but produced no output"
    return True, ""


def timing_stats(times: list[float], frames: int) -> dict:
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


def benchmark_case(case: dict, clip_path: Path, clip_meta: dict, args,
                   ffmpeg: Path | None, work: Path, candidate_ok: bool) -> dict:
    cid = case["case_id"]
    # Windows filenames cannot contain '|' (and other punctuation); keep case
    # ids human-readable in JSON but make the temporary bitstream names legal.
    token = re.sub(r"[^A-Za-z0-9_.-]+", "_", cid)
    rec = {
        "case_id": cid, "clip_id": clip_meta["id"], "preset": case["preset"],
        "settings": {k: case[k] for k in
                     ("width", "height", "fps", "crf", "threads", "frames")},
        "clip": {"file": Path(clip_meta["file"]).name,
                 "source": clip_meta["source_rel"],
                 "source_sha256": clip_meta["source_sha256"],
                 "clip_sha256": clip_meta["clip_sha256"],
                 "reused": clip_meta["reused"]},
        "failures": [], "status": "ok", "certified": False,
        "oracle": {"times_ms": []},
        "candidate": {"times_ms": [], "availability": "present" if candidate_ok else "missing"},
    }
    if not clip_meta.get("source_sha256_match", True):
        rec["failures"].append("clip_source_sha256_mismatch")

    labels = ["oracle"] + (["candidate"] if candidate_ok else [])
    binaries = {"oracle": args.oracle, "candidate": args.candidate}
    sizes = {label: [] for label in labels}
    hashes = {label: [] for label in labels}
    last_out = {label: None for label in labels}

    def do_encode(label: str, out: Path) -> dict:
        run = run_encode(binaries[label], encode_args(case, clip_path, out, case["frames"]),
                         args.timeout)
        ok, err = encode_ok(run, out)
        if not ok:
            rec["failures"].append(f"{label}_encode: {err}")
        return run, ok

    try:
        for k in range(args.warmups):
            for label in labels:
                warm = work / f"{token}_warm{k}_{label}.264"
                _, ok = do_encode(label, warm)
                warm.unlink(missing_ok=True)
                if not ok:
                    break
        for rep in range(args.reps):
            order = labels if rep % 2 == 0 else list(reversed(labels))
            for label in order:
                out = work / f"{token}_rep{rep}_{label}.264"
                run, ok = do_encode(label, out)
                if not ok:
                    continue
                rec[label]["times_ms"].append(run["ms"])
                sizes[label].append(out.stat().st_size)
                hashes[label].append(sha256_file(out))
                last_out[label] = out
    except OSError as exc:
        rec["failures"].append(f"io_error: {exc}")

    for label in labels:
        rec[label].update(timing_stats(rec[label]["times_ms"], case["frames"]))
        rec[label]["sizes_bytes"] = sizes[label]
        rec[label]["hash_full"] = (collections.Counter(hashes[label]).most_common(1)[0][0]
                                   if hashes[label] else None)
        if sizes[label]:
            rec[label]["size_bytes"] = int(statistics.median(sizes[label]))

    # Decode validity (hard requirement) and quality vs the exact source I420.
    for label in labels:
        out = last_out[label]
        if out is None:
            rec[label]["decode"] = {"ok": False, "frames": None, "error": "no output produced"}
            rec["failures"].append(f"{label}_decode: no output produced")
            continue
        if ffmpeg is None:
            rec[label]["decode"] = {"ok": None, "frames": None,
                                    "error": "ffmpeg unavailable; decode not checked"}
            rec["failures"].append(f"{label}_decode: ffmpeg unavailable")
            continue
        ok, frames, err = verify.decode_check(ffmpeg, out, case["frames"], args.timeout)
        rec[label]["decode"] = {"ok": ok, "frames": frames, "error": err, "file": out.name}
        if not ok:
            rec["failures"].append(f"{label}_decode: {err}")
        if not args.no_quality:
            q = verify.measure_quality(ffmpeg, clip_path, out, case["width"], case["height"],
                                       case["fps"], work, args.timeout)
            rec[label]["quality"] = q
            if q.get("error"):
                rec["warnings"] = rec.get("warnings", []) + [f"{label}_quality: {q['error']}"]

    o, c = rec["oracle"], rec["candidate"]
    if o.get("median_ms") and c.get("median_ms"):
        rec["speedup"] = round(o["median_ms"] / c["median_ms"], 4)
    else:
        rec["speedup"] = None
    if o.get("size_bytes") and c.get("size_bytes"):
        rec["size_delta_pct"] = round(100.0 * (c["size_bytes"] - o["size_bytes"]) / o["size_bytes"], 3)
    else:
        rec["size_delta_pct"] = None

    if not candidate_ok:
        rec["status"] = "reference-only"
    elif rec["failures"]:
        rec["status"] = "fail"
    else:
        rec["status"] = "pass"
        rec["certified"] = True
    return rec


# ---------------------------------------------------------------------------
# Reports
# ---------------------------------------------------------------------------

def generate_markdown(result: dict) -> str:
    cases = result.get("cases", [])
    oracle_rows = []
    ab_rows = []
    quality_rows = []
    for c in cases:
        s = c["settings"]
        o = c["oracle"]
        cand = c["candidate"]
        dec = o.get("decode", {})
        oracle_rows.append(
            f"| {c['clip_id']} | {s['width']}x{s['height']} | {s['frames']} | "
            f"{c['preset']} | {fmt(o.get('fps'))} | {fmt(o.get('median_ms'), '.1f')} | "
            f"{'yes' if dec.get('ok') else ('not checked' if dec.get('ok') is None else 'no')} | "
            f"{fmt((o.get('quality') or {}).get('psnr_db'), '.2f')} | "
            f"{fmt((o.get('quality') or {}).get('ssim'), '.4f')} |")

        availability = cand.get("availability", "missing")
        if availability == "missing":
            cfps, speed, cdec, status = "n/a (binary missing)", "—", "n/a", "NOT CERTIFIED"
        elif availability == "invalid":
            cfps = fmt(cand.get("fps"))
            speed, cdec, status = "—", "n/a", "NOT CERTIFIED (invalid candidate)"
        else:
            cfps = fmt(cand.get("fps"))
            ok = cand.get("decode", {}).get("ok")
            # Never present a ratio as a speedup unless the candidate decoded
            # cleanly; a failed decode is recorded in the JSON but unpublished.
            speed = (fmt(c.get("speedup"), ".3f") + "x") if c.get("certified") else "—"
            cdec = "yes" if ok else ("not checked" if ok is None else "no")
            status = "certified" if c.get("certified") else "NOT CERTIFIED"
        ab_rows.append(
            f"| {c['clip_id']} | {c['preset']} | {fmt(o.get('fps'))} | {cfps} | "
            f"{speed} | {cdec} | {status} |")

        if cand.get("quality") or o.get("quality"):
            qo, qc = o.get("quality") or {}, cand.get("quality") or {}
            quality_rows.append(
                f"| {c['clip_id']} | {c['preset']} | "
                f"{fmt(qo.get('psnr_db'), '.2f')} | {fmt(qo.get('ssim'), '.4f')} | "
                f"{fmt(qc.get('psnr_db'), '.2f') if availability not in ('missing', 'invalid') else 'n/a'} | "
                f"{fmt(qc.get('ssim'), '.4f') if availability not in ('missing', 'invalid') else 'n/a'} |")

    lines = [
        "# Real-video A/B benchmark", "",
        f"_Generated: {result['generated_at_utc']} (run `{result['run_id']}`)_", "",
        f"**Status:** {result['status']} — "
        f"**candidate {'CERTIFIED' if result['candidate_certified'] else 'NOT CERTIFIED'}**", "",
        f"**Publishable speedups:** "
        f"{'yes' if result['publication']['publish_speeds'] else 'no'} "
        f"({result['publication']['reason']})", "",
    ]
    if result.get("blocked_reason"):
        lines += ["## Blocked", "", result["blocked_reason"], ""]
    for w in result.get("warnings", []):
        lines.append(f"- **Warning:** {w}")
    if result.get("warnings"):
        lines.append("")

    lines += [
        "## Reference-only table (oracle x264, black box)", "",
        "| Clip | Resolution | Frames | Preset | Oracle fps (median) | Median ms | Decode | PSNR dB | SSIM |",
        "|---|---|---:|---|---:|---:|:---:|---:|---:|",
    ] + (oracle_rows or ["| _no oracle cases measured_ | — | — | — | — | — | — | — | — |"]) + [
        "", "## A/B table (reference vs candidate)", "",
        "| Clip | Preset | Oracle fps | Candidate fps | Speedup | Candidate decode | Certification |",
        "|---|---|---:|---:|---:|:---:|---|",
    ] + (ab_rows or ["| _no cases measured_ | — | — | — | — | — | — |"]) + [
        "", "The candidate column stays `n/a` / `NOT CERTIFIED` unless the candidate binary "
        "exists, runs and decodes cleanly on **every** case. A measured ratio is shown only "
        "for a cleanly decoding candidate; no speedup is inferred from a failed or absent binary.", "",
    ]
    if quality_rows:
        lines += ["## Quality vs exact source I420 (ffmpeg PSNR / SSIM)", "",
                  "| Clip | Preset | Oracle PSNR dB | Oracle SSIM | Candidate PSNR dB | Candidate SSIM |",
                  "|---|---|---:|---:|---:|---:|"] + quality_rows + [""]

    lines += ["## Documented reference anchors (historical, not re-derived here)", "",
              "| Case | fps | Source |", "|---|---:|---|"]
    lines += [f"| {a['case']} | {a['fps']:.2f} | `{a['source']}` |"
              for a in result["reference_anchors"]]
    lines += ["", "These four numbers are the already-established reference measurements "
              "published in `benchmarks.md` / `README.md`. The table above contains this "
              "run's fresh oracle measurements and is the authoritative result for this run.", ""]

    summary = result.get("summary", {})
    lines += ["## Summary", "",
              f"- Cases: {summary.get('total_cases', 0)}; "
              f"oracle-decoded: {summary.get('oracle_decoded_cases', 0)}; "
              f"candidate-certified: {summary.get('certified_cases', 0)}",
              f"- Presets: {', '.join(result['settings']['presets'])}; "
              f"frames/case: {result['settings']['frames']}; "
              f"reps: {result['settings']['reps']}; warm-ups: {result['settings']['warmups']}",
              f"- Machine: {result['machine'].get('processor', 'unknown')}, "
              f"{result['machine'].get('cpu_count')} logical CPUs", ""]
    return "\n".join(lines)


def write_outputs(result: dict, results_dir: Path, run_id: str) -> None:
    results_dir.mkdir(parents=True, exist_ok=True)
    payload = json.dumps(result, indent=2, ensure_ascii=False) + "\n"
    (results_dir / "latest.json").write_text(payload, encoding="utf-8")
    (results_dir / "runs").mkdir(parents=True, exist_ok=True)
    (results_dir / "runs" / f"{run_id}.json").write_text(payload, encoding="utf-8")
    (results_dir / "latest.md").write_text(generate_markdown(result), encoding="utf-8")
    print(f"[out] {results_dir / 'latest.json'}")
    print(f"[out] {results_dir / 'latest.md'}")
    print(f"[out] {results_dir / 'runs' / (run_id + '.json')}")


# ---------------------------------------------------------------------------
# CLI / main
# ---------------------------------------------------------------------------

def parse_args(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--oracle", type=Path, default=REPO_ROOT / "oracle" / "x264.exe")
    ap.add_argument("--candidate", type=Path, default=REPO_ROOT / "build" / "x264pp.exe")
    ap.add_argument("--ffmpeg", type=Path, default=None)
    ap.add_argument("--corpus-dir", type=Path, default=Path(r"D:\Videos"))
    ap.add_argument("--clips-json", type=Path, default=SCRIPT_DIR / "real_clips.json")
    ap.add_argument("--clips", nargs="+", default=None, help="restrict to these clip ids")
    ap.add_argument("--presets", nargs="+", default=["ultrafast", "medium", "veryslow"])
    ap.add_argument("--frames", type=int, default=8, help="bounded frames per clip (default 8)")
    ap.add_argument("--fps", default=None, help="override clip fps (e.g. 30 or 30000/1001)")
    ap.add_argument("--crf", type=int, default=23)
    ap.add_argument("--threads", type=int, default=6)
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--warmups", type=int, default=1)
    ap.add_argument("--timeout", type=float, default=600.0)
    ap.add_argument("--assets-dir", type=Path,
                    default=Path(tempfile.gettempdir()) / "x264pp-realbench-assets")
    ap.add_argument("--work-dir", type=Path,
                    default=Path(tempfile.gettempdir()) / "x264pp-realbench-work")
    ap.add_argument("--results-dir", type=Path, default=SCRIPT_DIR / "results" / "real")
    ap.add_argument("--no-quality", action="store_true", help="skip PSNR/SSIM measurement")
    ap.add_argument("--keep-outputs", action="store_true")
    ap.add_argument("--discover", action="store_true", help="print curated clips and exit")
    args = ap.parse_args(argv)
    if args.frames < 1 or args.reps < 1 or args.warmups < 0 or args.threads < 1:
        ap.error("--frames/--reps/--threads must be >= 1 and --warmups >= 0")
    return args


def main(argv=None) -> int:
    args = parse_args(argv)
    args.oracle = args.oracle.resolve()
    args.candidate = args.candidate.resolve()

    clips, load_warnings = load_clips(args.clips_json.resolve(), args.corpus_dir.resolve(),
                                      args.clips)
    if args.discover:
        for c in clips:
            exists = "yes" if c["source"].is_file() else "NO"
            print(f"{c['id']}: {c['width']}x{c['height']} @{c['fps']} "
                  f"{c['kind']:9s} exists={exists} {c['source']}")
        return 0

    run_id = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    mingw_added = ensure_mingw_path()
    ffmpeg = verify.find_ffmpeg(args.ffmpeg)
    warnings = list(load_warnings)
    if not clips:
        warnings.append("no clips selected")

    result = {
        "schema_version": SCHEMA_VERSION,
        "generated_at_utc": now_utc(), "run_id": run_id,
        "status": "ok", "candidate_certified": False, "blocked_reason": None,
        "publication": {"publish_speeds": False,
                        "reason": "not yet determined"},
        "machine": {"platform": platform.platform(), "processor": platform.processor(),
                    "cpu_count": os.cpu_count(), "python": platform.python_version()},
        "binaries": {
            "oracle": {"path": str(args.oracle), "exists": args.oracle.is_file(),
                       "version": tool_version(args.oracle) if args.oracle.is_file() else None,
                       "sha256": sha256_file(args.oracle) if args.oracle.is_file() else None},
            "candidate": {"path": str(args.candidate), "exists": args.candidate.is_file(),
                          "version": None, "sha256": None},
        },
        "reference_anchors": DOCUMENTED_ANCHORS,
        "settings": {
            "corpus_dir": str(args.corpus_dir.resolve()),
            "clips": [c["id"] for c in clips], "presets": list(args.presets),
            "frames": args.frames, "fps_override": args.fps,
            "crf": args.crf, "threads": args.threads, "reps": args.reps,
            "warmups": args.warmups, "timeout_s": args.timeout,
            "quality": not args.no_quality,
            "ffmpeg": str(ffmpeg) if ffmpeg else None,
            "mingw_path_prepended": mingw_added,
            "assets_dir": str(args.assets_dir.resolve()),
            "timing": "wall clock incl. process startup + file write; warm-up discarded; "
                      "A/B interleaved with alternating order per repetition",
            "cli_shape": "x264-compatible raw I420; identical argv for both encoders except -o",
        },
        "clips": [], "cases": [], "summary": {}, "warnings": warnings,
    }

    blocked = []
    if ffmpeg is None:
        blocked.append("ffmpeg not found (use --ffmpeg or set FFMPEG)")
    oracle_ok = args.oracle.is_file()
    if not oracle_ok:
        blocked.append(f"oracle binary missing: {args.oracle}")

    candidate_ok = False
    candidate_state = "missing"
    if not args.candidate.is_file():
        blocked.append(f"candidate binary missing: {args.candidate} (NOT CERTIFIED)")
    else:
        version = tool_version(args.candidate)
        if version.startswith("<"):
            blocked.append(f"candidate binary invalid: {version} (NOT CERTIFIED)")
            candidate_state = "invalid"
        else:
            candidate_ok = True
            candidate_state = "present"
            result["binaries"]["candidate"].update(
                version=version, sha256=sha256_file(args.candidate))
    result["settings"]["candidate_state"] = candidate_state

    if blocked:
        result["blocked_reason"] = "; ".join(blocked)
        result["publication"] = {"publish_speeds": False,
                                 "reason": "NOT CERTIFIED — " + result["blocked_reason"]}
        print("[status] NOT CERTIFIED — " + result["blocked_reason"], file=sys.stderr)

    work = args.work_dir.resolve()
    work.mkdir(parents=True, exist_ok=True)

    for clip in clips:
        case_common = {
            "width": clip["width"], "height": clip["height"],
            "fps": args.fps if args.fps is not None else clip["fps"],
            "crf": args.crf, "threads": args.threads, "frames": args.frames,
        }
        try:
            meta = materialize_clip(clip, args.frames, args.assets_dir, ffmpeg, args.timeout)
        except (RuntimeError, OSError, subprocess.TimeoutExpired) as exc:
            warnings.append(f"{clip['id']}: clip materialisation failed: {exc}")
            rec = {"case_id": f"{clip['id']}|<clip>", "clip_id": clip["id"],
                   "preset": None, "settings": case_common,
                   "status": "fail", "certified": False,
                   "failures": [f"clip_materialisation: {exc}"],
                   "oracle": {"times_ms": [], "fps": None, "decode": {"ok": False}},
                   "candidate": {"times_ms": [], "availability": candidate_state}}
            result["cases"].append(rec)
            print(f"[case] {clip['id']} status=fail: {exc}")
            continue
        result["clips"].append(meta)
        if meta.get("warning"):
            warnings.append(f"{clip['id']}: {meta['warning']}")
        clip_path = Path(meta["file"])

        for preset in args.presets:
            case = dict(case_common)
            case["preset"] = preset
            case["case_id"] = f"{clip['id']}|{preset}"
            if not oracle_ok:
                rec = {"case_id": case["case_id"], "clip_id": clip["id"],
                       "preset": preset, "settings": case_common,
                       "status": "fail", "certified": False,
                       "failures": ["oracle binary missing"],
                       "oracle": {"times_ms": [], "fps": None, "decode": {"ok": False}},
                       "candidate": {"times_ms": [], "availability": candidate_state}}
                result["cases"].append(rec)
                continue
            rec = benchmark_case(case, clip_path, meta, args, ffmpeg, work, candidate_ok)
            if not candidate_ok:
                rec["candidate"]["availability"] = candidate_state
            result["cases"].append(rec)
            print(f"[case] {case['case_id']} status={rec['status']} "
                  f"oracle_fps={fmt(rec['oracle'].get('fps'))} "
                  f"cand_fps={fmt(rec['candidate'].get('fps'))} "
                  f"decode={rec['oracle'].get('decode', {}).get('ok')}/"
                  f"{rec['candidate'].get('decode', {}).get('ok')} "
                  f"failures={'; '.join(rec['failures']) or '—'}")

    cases = result["cases"]
    passing = [c for c in cases if c.get("certified")]
    certified_all = bool(candidate_ok and passing and len(passing) == len(cases) and not blocked)
    result["candidate_certified"] = certified_all
    speedups = [c["speedup"] for c in passing if c.get("speedup") is not None]
    result["summary"] = {
        "total_cases": len(cases),
        "oracle_measured_cases": sum(1 for c in cases if c["oracle"].get("median_ms") is not None),
        "oracle_decoded_cases": sum(1 for c in cases
                                    if c["oracle"].get("decode", {}).get("ok") is True),
        "candidate_measured_cases": sum(1 for c in cases
                                        if c["candidate"].get("median_ms") is not None),
        "certified_cases": len(passing),
        "median_speedup": round(statistics.median(speedups), 4) if speedups else None,
        "by_preset": {},
    }
    for preset in args.presets:
        pc = [c for c in cases if c.get("preset") == preset and c["oracle"].get("fps")]
        if pc:
            result["summary"]["by_preset"][preset] = {
                "oracle_fps_median": round(statistics.median([c["oracle"]["fps"] for c in pc]), 2),
                "candidate_fps_median": (
                    round(statistics.median([c["candidate"]["fps"] for c in pc
                                             if c["candidate"].get("fps")]), 2)
                    if any(c["candidate"].get("fps") for c in pc) else None),
                "certified_cases": sum(1 for c in pc if c.get("certified")),
            }
    if certified_all:
        result["status"] = "ok"
        result["publication"] = {
            "publish_speeds": True,
            "reason": "candidate decoded cleanly on every real-video case"}
    elif blocked or not candidate_ok:
        result["status"] = "blocked"
        result["publication"] = {"publish_speeds": False,
                                 "reason": "NOT CERTIFIED — " + (result["blocked_reason"] or
                                                                "candidate unavailable")}
    else:
        result["status"] = "partial"
        result["publication"] = {
            "publish_speeds": False,
            "reason": "NOT CERTIFIED — candidate failed one or more decode/encode checks"}

    write_outputs(result, args.results_dir.resolve(), run_id)
    if not args.keep_outputs:
        for f in work.glob("*.264"):
            f.unlink(missing_ok=True)
    print(f"[status] {result['status']} candidate_certified={result['candidate_certified']} "
          f"publish={result['publication']['publish_speeds']}")
    if args.no_quality:
        print("[note] PSNR/SSIM measurement skipped (--no-quality)")
    return 2 if result["status"] == "blocked" else (0 if result["status"] == "ok" else 1)


if __name__ == "__main__":
    sys.exit(main())
