#!/usr/bin/env python3
"""Reproducible speed / bit-identity benchmark for upstream x264 vs x264++.

The harness generates (or reuses) a deterministic I420 clip, encodes it with
both CLIs using *identical* options at every preset, times warm-up + N
repetitions, and reports per-preset median wall-clock time, frames/sec,
speed ratio, size delta and SHA256 identity. Results go to JSON + Markdown.

Only Python's standard library is required; numpy is optional and only used
to generate the clip faster (see generate_clip.py).

Examples:
    python bench/bench.py --generate-only
    python bench/bench.py --reps 5 --threads 6
    python bench/bench.py --upstream ../build/upstream/x264.exe \
                          --opt ../build/opt/x264.exe --reps 7
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
import statistics
import subprocess
import sys
import time
from pathlib import Path

import generate_clip

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent
DEFAULT_PRESETS = ["ultrafast", "superfast", "veryfast", "faster", "fast",
                   "medium", "slow", "slower", "veryslow"]
SCHEMA_VERSION = 1


# ---------------------------------------------------------------------------
# Clip management
# ---------------------------------------------------------------------------

def manifest_path_for(clip: Path) -> Path:
    return clip.with_suffix(clip.suffix + ".manifest.json")


def ensure_clip(clip: Path, width: int, height: int, frames: int,
                fps: int, seed: int, no_generate: bool) -> dict:
    """Reuse a matching verified clip, otherwise generate it."""
    expected = {
        "generator_version": generate_clip.GENERATOR_VERSION,
        "backend": "numpy" if generate_clip.np is not None else "pure-python",
        "numpy_version": generate_clip.np.__version__ if generate_clip.np else None,
        "width": width, "height": height, "frames": frames,
        "fps": fps, "seed": seed,
    }
    mp = manifest_path_for(clip)
    if clip.is_file() and mp.is_file():
        try:
            m = json.loads(mp.read_text(encoding="utf-8"))
            if (all(m.get(k) == v for k, v in expected.items())
                    and m.get("sha256") == generate_clip.sha256_file(clip)
                    and m.get("size_bytes") == clip.stat().st_size):
                print(f"[clip] reusing {clip} (sha256={m['sha256'][:16]}...)")
                return m
            print("[clip] existing clip does not match requested parameters "
                  "or failed SHA256 verification; regenerating")
        except (ValueError, OSError, KeyError) as exc:
            print(f"[clip] cannot read manifest ({exc}); regenerating")
    if no_generate:
        raise RuntimeError(f"clip {clip} is missing/stale and --no-generate was set")
    print(f"[clip] generating {width}x{height}, {frames} frames, seed={seed} "
          f"(backend: {expected['backend']}) ...")
    m = generate_clip.build_manifest(clip, width, height, frames, fps, seed)
    mp.write_text(json.dumps(m, indent=2) + "\n", encoding="utf-8")
    print(f"[clip] wrote {clip} ({m['size_bytes']:,} bytes, "
          f"sha256={m['sha256'][:16]}...)")
    return m


# ---------------------------------------------------------------------------
# Executing x264
# ---------------------------------------------------------------------------

def x264_version(exe: Path) -> str:
    try:
        p = subprocess.run([str(exe), "--version"], stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, timeout=30)
        for line in p.stdout.decode("utf-8", "replace").splitlines():
            if line.strip():
                return line.strip()
    except Exception as exc:  # noqa: BLE001 - diagnostic only
        return f"<unavailable: {exc}>"
    return "<unknown>"


def x264_args(preset: str, crf: float, threads: int, keyint: int,
              min_keyint: int, scenecut: int, fps: int, width: int,
              height: int, clip: Path, out: Path, extra: list[str]) -> list[str]:
    """Identical settings for both encoders; only the output path differs."""
    return [
        "--demuxer", "raw",
        "--input-csp", "i420",
        "--input-res", f"{width}x{height}",
        "--fps", str(fps),
        "--preset", preset,
        "--crf", str(crf),
        "--threads", str(threads),
        "--keyint", str(keyint),
        "--min-keyint", str(min_keyint),
        "--scenecut", str(scenecut),   # 0 => disable adaptive I-frames
        # PSNR/SSIM are opt-in in this x264 source (--psnr/--ssim); omitting
        # them keeps the work disabled without relying on --no-psnr/--no-ssim,
        # which older/newer builds may spell differently.
        "--quiet",
    ] + list(extra) + ["-o", str(out), str(clip)]


def run_encode(exe: Path, args: list[str], timeout: float) -> tuple[float, str]:
    t0 = time.perf_counter()
    try:
        p = subprocess.run([str(exe)] + args, stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, timeout=timeout)
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError(f"encode timed out after {timeout:g}s") from exc
    elapsed_ms = (time.perf_counter() - t0) * 1000.0
    if p.returncode != 0:
        err = (p.stderr or p.stdout or b"").decode("utf-8", "replace").strip()
        raise RuntimeError(f"x264 exited with code {p.returncode}: {err[-1000:]}")
    return elapsed_ms, (p.stderr or b"").decode("utf-8", "replace")


def ensure_mingw_path() -> list[str]:
    """Make MinGW-built CLIs runnable when called from plain PowerShell.

    x264.exe built with MSYS2 MinGW needs libgcc/libwinpthread DLLs from
    mingw64/bin. Prepend that directory to PATH for child processes if it is
    not already there. Returns the directories that were added.
    """
    added = []
    current = os.environ.get("PATH", "").split(os.pathsep)
    for candidate in (Path(r"C:\msys64\mingw64\bin"), Path(r"C:\msys64\usr\bin")):
        if candidate.is_dir() and str(candidate) not in current:
            os.environ["PATH"] = str(candidate) + os.pathsep + os.environ.get("PATH", "")
            current.insert(0, str(candidate))
            added.append(str(candidate))
    return added


def full_sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def video_only_sha256(path: Path) -> tuple[str, bool]:
    """SHA256 of the Annex-B stream with SEI NALs (type 6) removed.

    x264 writes an SEI 'encoding settings' string that embeds the build
    version. If the two builds advertise different version strings, the full
    files differ even when the coded video is bit-identical. This derived
    hash ignores that NAL so the two kinds of difference can be told apart.
    Start codes are canonicalised to 00 00 01 and trailing_zero_8bits dropped.
    """
    data = path.read_bytes()
    n = len(data)
    starts = []
    i = 0
    while i < n - 2:
        if data[i] == 0 and data[i + 1] == 0 and data[i + 2] == 1:
            starts.append(i)
            i += 3
        else:
            i += 1
    if not starts:
        return hashlib.sha256(data).hexdigest(), False

    out = bytearray()
    for idx, pos in enumerate(starts):
        end = starts[idx + 1] if idx + 1 < len(starts) else n
        body = data[pos + 3:end].rstrip(b"\x00")
        if not body:
            continue
        nal_type = body[0] & 0x1F
        if nal_type == 6:  # SEI
            continue
        out += b"\x00\x00\x01"
        out += body
    return hashlib.sha256(out).hexdigest(), True


# ---------------------------------------------------------------------------
# Benchmark
# ---------------------------------------------------------------------------

def bench_preset(preset: str, exe_up: Path, exe_opt: Path, args: argparse.Namespace,
                 clip: Path, manifest: dict, work: Path) -> dict:
    rec = {
        "preset": preset,
        "status": "ok",
        "upstream": {"times_ms": [], "hashes_full": [], "hashes_video_only": [],
                     "sizes_bytes": []},
        "opt": {"times_ms": [], "hashes_full": [], "hashes_video_only": [],
                "sizes_bytes": []},
    }

    def common(exe, out):
        return x264_args(preset, args.crf, args.threads, args.keyint,
                         args.min_keyint, args.scenecut, manifest["fps"],
                         manifest["width"], manifest["height"], clip, out,
                         args.extra_arg)

    try:
        # One warm-up per encoder (output discarded).
        warm = work / f"warm_{preset}.264"
        run_encode(exe_up, common(exe_up, warm), args.timeout)
        run_encode(exe_opt, common(exe_opt, warm), args.timeout)
        warm.unlink(missing_ok=True)

        # Interleave A/B per repetition to cancel thermal / cache drift.
        for rep in range(args.reps):
            for label, exe in (("upstream", exe_up), ("opt", exe_opt)):
                out = work / f"{label}_{preset}.264"
                ms, _ = run_encode(exe, common(exe, out), args.timeout)
                slot = rec[label]
                slot["times_ms"].append(ms)
                slot["hashes_full"].append(full_sha256(out))
                slot["hashes_video_only"].append(video_only_sha256(out)[0])
                slot["sizes_bytes"].append(out.stat().st_size)
    except (RuntimeError, OSError) as exc:
        rec["status"] = "failed"
        rec["error"] = str(exc)
        # Keep whatever measurements completed so failures are diagnosable.
        if not rec["upstream"]["times_ms"] and not rec["opt"]["times_ms"]:
            return rec

    for label in ("upstream", "opt"):
        slot = rec[label]
        times = slot["times_ms"]
        if not times:
            slot["median_ms"] = None
            slot["fps"] = None
            continue
        slot["median_ms"] = round(statistics.median(times), 3)
        slot["mean_ms"] = round(statistics.fmean(times), 3)
        slot["min_ms"] = round(min(times), 3)
        slot["max_ms"] = round(max(times), 3)
        slot["fps"] = round(manifest["frames"] / (slot["median_ms"] / 1000.0), 2)
        slot["times_ms"] = [round(x, 3) for x in times]
        slot["hash_full"] = collections.Counter(slot["hashes_full"]).most_common(1)[0][0]
        slot["hash_video_only"] = collections.Counter(slot["hashes_video_only"]).most_common(1)[0][0]
        slot["reps_identical_full"] = len(set(slot["hashes_full"])) == 1
        slot["reps_identical_video_only"] = len(set(slot["hashes_video_only"])) == 1
        slot["size_bytes"] = int(statistics.median(slot["sizes_bytes"]))
        for k in ("hashes_full", "hashes_video_only", "sizes_bytes"):
            slot.pop(k)

    u, o = rec["upstream"], rec["opt"]
    if u.get("median_ms") and o.get("median_ms"):
        rec["speedup"] = round(u["median_ms"] / o["median_ms"], 4)
        rec["size_delta_bytes"] = o["size_bytes"] - u["size_bytes"]
        rec["size_delta_pct"] = (round(100.0 * rec["size_delta_bytes"] / u["size_bytes"], 3)
                                 if u["size_bytes"] else None)
        rec["identical_full"] = u["hash_full"] == o["hash_full"]
        rec["identical_video_only"] = u["hash_video_only"] == o["hash_video_only"]
    else:
        rec["speedup"] = None
        rec["identical_full"] = None
        rec["identical_video_only"] = None
    return rec


def write_json(result: dict, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"[out] JSON  -> {path}")


def fmt_ms(v):
    return "n/a" if v is None else f"{v:,.1f}"


def fmt_num(v, spec=".2f"):
    return "n/a" if v is None else format(v, spec)


def write_markdown(result: dict, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    s = result["settings"]
    c = result["clip"]
    lines = [
        "# x264++ benchmark results", "",
        f"_Generated {result['generated_at_utc']}_", "",
        "## Configuration", "",
        f"- Clip: {c['width']}x{c['height']}, {c['frames']} frames, "
        f"{c['fps']} fps, seed {c['seed']}, I420 (`{c['file']}`)",
        f"- CRF {s['crf']}; threads {s['threads']}; keyint {s['keyint']}; "
        f"min-keyint {s['min_keyint']}; scenecut {s['scenecut']} (disabled); "
        f"PSNR/SSIM computation off; raw I420 demuxer",
        f"- {s['reps']} timed repetitions per preset after one warm-up; "
        "A/B interleaved per repetition; median reported",
        f"- Upstream: `{result['encoders']['upstream']['version']}`",
        f"- Optimized: `{result['encoders']['opt']['version']}`",
        "", "## Per-preset results", "",
        "| Preset | Upstream median (ms) | Opt median (ms) | Speedup (up/opt) | "
        "Upstream fps | Opt fps | Size Δ (bytes) | Full SHA256 | Video-only SHA256 |",
        "|---|---:|---:|---:|---:|---:|---:|:---:|:---:|",
    ]
    if result["status"] == "blocked":
        lines.append("| _(not run)_ | | | | | | | | |")
    else:
        for r in result["results"]:
            u, o = r["upstream"], r["opt"]
            def yn(v):
                return "n/a" if v is None else ("✅ identical" if v else "❌ different")
            lines.append(
                f"| {r['preset']} | {fmt_ms(u.get('median_ms'))} | "
                f"{fmt_ms(o.get('median_ms'))} | {fmt_num(r.get('speedup'))} | "
                f"{fmt_num(u.get('fps'))} | {fmt_num(o.get('fps'))} | "
                f"{'n/a' if r.get('size_delta_bytes') is None else format(r['size_delta_bytes'], '+d')} | "
                f"{yn(r.get('identical_full'))} | {yn(r.get('identical_video_only'))} |"
            )
    sm = result.get("summary", {})
    lines += ["", "## Summary", ""]
    if result["status"] == "blocked":
        lines.append(f"Benchmark not run: {result.get('blocked_reason', 'unknown reason')}.")
    else:
        lines.append(f"- Presets completed: {sm.get('completed', 0)}/{len(s['presets'])}")
        lines.append(f"- Median speedup across presets: {fmt_num(sm.get('median_speedup'))}x")
        lines.append(f"- Geometric-mean speedup: {fmt_num(sm.get('geomean_speedup'))}x")
        lines.append(f"- Full-file SHA256 identical on every preset: {sm.get('all_identical_full')}")
        lines.append(f"- Video-only SHA256 identical on every preset: {sm.get('all_identical_video_only')}")
    lines += [
        "", "## Notes", "",
        "- Speedup = upstream median wall-clock / optimized median wall-clock; "
        "values > 1 mean the optimized build is faster.",
        "- Timings include process startup and writing the .264 to disk; the "
        "same command shape is used for both encoders.",
        "- `Full SHA256` hashes the complete .264 file. x264 writes an SEI "
        "build-version string by default, so differing version strings make "
        "this hash differ even when coded video is identical. `Video-only "
        "SHA256` strips SEI NALs before hashing to isolate real bitstream "
        "differences. If full differs but video-only matches, the difference "
        "is metadata/version only.",
        "- Encoders are deterministic across repetitions by design; the "
        "harness verifies this internally and records `reps_identical_*` "
        "flags in the JSON.",
        "", "Raw machine-readable data: `bench_result.json`.", "",
    ]
    path.write_text("\n".join(lines), encoding="utf-8")
    print(f"[out] Markdown -> {path}")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--upstream", type=Path, default=REPO_ROOT / "build/upstream/x264.exe")
    ap.add_argument("--opt", type=Path, default=REPO_ROOT / "build/opt/x264.exe")
    ap.add_argument("--presets", nargs="+", default=DEFAULT_PRESETS)
    ap.add_argument("--crf", type=float, default=23.0)
    ap.add_argument("--threads", type=int, default=6)
    ap.add_argument("--keyint", type=int, default=250)
    ap.add_argument("--min-keyint", type=int, default=25)
    ap.add_argument("--scenecut", type=int, default=0)
    ap.add_argument("--reps", type=int, default=5)
    ap.add_argument("--fps", type=int, default=30)
    ap.add_argument("--width", type=int, default=640)
    ap.add_argument("--height", type=int, default=360)
    ap.add_argument("--frames", type=int, default=240)
    ap.add_argument("--seed", type=int, default=1337)
    ap.add_argument("--clip", type=Path, default=None,
                    help="override clip path (default: bench/assets/clip_<wxh>_<n>f.yuv)")
    ap.add_argument("--assets-dir", type=Path, default=SCRIPT_DIR / "assets")
    ap.add_argument("--work-dir", type=Path, default=SCRIPT_DIR / "temp")
    ap.add_argument("--out-json", type=Path, default=SCRIPT_DIR / "results" / "bench_result.json")
    ap.add_argument("--out-md", type=Path, default=SCRIPT_DIR / "results" / "score_table.md")
    ap.add_argument("--timeout", type=float, default=900.0, help="per-encode timeout in seconds")
    ap.add_argument("--extra-arg", action="append", default=[],
                    help="extra x264 CLI arg applied identically to both encoders (repeatable)")
    ap.add_argument("--no-generate", action="store_true",
                    help="fail instead of regenerating a missing/stale clip")
    ap.add_argument("--keep-outputs", action="store_true",
                    help="keep per-run .264 files in the work directory")
    ap.add_argument("--generate-only", action="store_true",
                    help="only generate/verify the clip, then exit (no encoders needed)")
    args = ap.parse_args(argv)

    if args.reps < 1:
        ap.error("--reps must be >= 1")
    if args.clip is None:
        args.clip = args.assets_dir / f"clip_{args.width}x{args.height}_{args.frames}f.yuv"
    args.clip = args.clip.resolve()

    manifest = ensure_clip(args.clip, args.width, args.height, args.frames,
                           args.fps, args.seed, args.no_generate)
    if args.generate_only:
        print(json.dumps(manifest, indent=2))
        return 0

    args.upstream = args.upstream.resolve()
    args.opt = args.opt.resolve()
    missing = [str(p) for p in (args.upstream, args.opt) if not p.is_file()]
    mingw_added = ensure_mingw_path()
    result = {
        "schema_version": SCHEMA_VERSION,
        "generated_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds"),
        "status": "ok",
        "machine": {
            "platform": platform.platform(),
            "processor": platform.processor(),
            "cpu_count": os.cpu_count(),
        },
        "clip": manifest,
        "settings": {
            "upstream_exe": str(args.upstream),
            "opt_exe": str(args.opt),
            "presets": args.presets,
            "crf": args.crf,
            "threads": args.threads,
            "keyint": args.keyint,
            "min_keyint": args.min_keyint,
            "scenecut": args.scenecut,
            "psnr_ssim": "disabled (neither --psnr nor --ssim is passed)",
            "reps": args.reps,
            "warmup_runs": 1,
            "order": "A/B interleaved per repetition",
            "extra_args": args.extra_arg,
            "mingw_path_prepended": mingw_added,
        },
        "encoders": {
            "upstream": {"path": str(args.upstream), "version": None},
            "opt": {"path": str(args.opt), "version": None},
        },
        "results": [],
        "summary": {},
        "notes": [
            "Wall-clock includes process startup and output file write; identical "
            "command shapes are used for both encoders.",
            "SEI-stripped SHA256 isolates coded-video differences from x264 build "
            "version strings embedded in SEI metadata.",
        ],
    }

    if missing:
        result["status"] = "blocked"
        result["blocked_reason"] = "missing executable(s): " + ", ".join(missing)
        print(f"[error] {result['blocked_reason']}", file=sys.stderr)
        print("[hint] build both CLIs, or point --upstream/--opt at existing binaries",
              file=sys.stderr)
        write_json(result, args.out_json.resolve())
        write_markdown(result, args.out_md.resolve())
        return 2

    result["encoders"]["upstream"]["version"] = x264_version(args.upstream)
    result["encoders"]["opt"]["version"] = x264_version(args.opt)
    print(f"[enc] upstream: {result['encoders']['upstream']['version']}")
    print(f"[enc] opt:      {result['encoders']['opt']['version']}")

    work = args.work_dir.resolve()
    work.mkdir(parents=True, exist_ok=True)
    for preset in args.presets:
        print(f"[bench] preset={preset} ...")
        rec = bench_preset(preset, args.upstream, args.opt, args, args.clip,
                           manifest, work)
        result["results"].append(rec)
        status = rec["status"]
        print(f"        status={status} speedup={rec.get('speedup')} "
              f"full_hash_match={rec.get('identical_full')} "
              f"video_hash_match={rec.get('identical_video_only')}")

    done = [r for r in result["results"] if r["status"] == "ok" and r.get("speedup")]
    if len(done) != len(result["results"]):
        result["status"] = "partial"
    speedups = [r["speedup"] for r in done]
    if speedups:
        result["summary"] = {
            "completed": len(done),
            "median_speedup": round(statistics.median(speedups), 4),
            "geomean_speedup": round(math.exp(statistics.fmean(
                [math.log(x) for x in speedups])), 4),
            "all_identical_full": all(r.get("identical_full") for r in done),
            "all_identical_video_only": all(r.get("identical_video_only") for r in done),
        }
    else:
        result["summary"] = {"completed": 0, "median_speedup": None,
                             "geomean_speedup": None,
                             "all_identical_full": None,
                             "all_identical_video_only": None}

    write_json(result, args.out_json.resolve())
    write_markdown(result, args.out_md.resolve())
    if not args.keep_outputs:
        for f in work.glob("*.264"):
            f.unlink(missing_ok=True)
    return 0 if len(done) == len(args.presets) else 1


if __name__ == "__main__":
    sys.exit(main())
