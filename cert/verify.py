#!/usr/bin/env python3
"""ffmpeg-based decode and quality verification for certification outputs.

Only the decoder is used as an external oracle for "valid H.264"; no encoder
source is consulted.  PSNR/SSIM are measured only when a quality envelope in
``spec/quality-envelope.md`` mentions or defines thresholds.
"""

from __future__ import annotations

import math
import os
import re
import shutil
import subprocess
from pathlib import Path

DEFAULT_FFMPEG = Path(r"C:\ffmpeg\ffmpeg-master-latest-win64-gpl\bin\ffmpeg.exe")


def find_ffmpeg(explicit=None):
    candidates = ([explicit] if explicit else []) + [
        os.environ.get("FFMPEG"), str(DEFAULT_FFMPEG), shutil.which("ffmpeg")]
    for c in candidates:
        if not c:
            continue
        p = Path(c)
        if p.is_file():
            return p.resolve()
    return None


def _run(cmd, timeout, cwd=None):
    return subprocess.run([str(x) for x in cmd], stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, timeout=timeout, cwd=cwd)


def decode_check(ffmpeg: Path, out: Path, expected_frames: int,
                 timeout: float = 300.0):
    """Decode every frame; return (ok, decoded_frames, error_text)."""
    cmd = [ffmpeg, "-hide_banner", "-nostats", "-v", "error",
           "-progress", "pipe:1", "-i", out, "-map", "0:v:0",
           "-f", "null", "-"]
    try:
        p = _run(cmd, timeout)
    except subprocess.TimeoutExpired:
        return False, None, f"decode timed out after {timeout:g}s"
    frames = [int(m.group(1)) for m in
              re.finditer(r"^frame=(\d+)\s*$", p.stdout.decode("utf-8", "replace"), re.M)]
    decoded = max(frames) if frames else None
    err = p.stderr.decode("utf-8", "replace").strip()
    ok = p.returncode == 0 and not err and decoded == expected_frames
    if not ok and not err:
        if decoded != expected_frames:
            err = f"decoded {decoded} frames, expected {expected_frames}"
        else:
            err = f"ffmpeg exit code {p.returncode}"
    return ok, decoded, err


def measure_quality(ffmpeg: Path, clip: Path, out: Path, width: int, height: int,
                    fps: int, work_dir: Path, timeout: float = 300.0) -> dict:
    """Measure PSNR and SSIM of `out` against the raw source `clip`."""
    result = {"psnr_db": None, "ssim": None, "error": None}
    filters = (("psnr", "psnr_db", r"average:([0-9.]+|inf)"),
               ("ssim", "ssim", r"All:([0-9.]+|inf)"))
    for name, key, pattern in filters:
        log = f"cert_{name}.log"
        logpath = work_dir / log
        try:
            logpath.unlink(missing_ok=True)
        except OSError:
            pass
        # stats_file is a bare filename: ffmpeg treats ':' inside a filter as
        # a separator, so an absolute Windows path with 'C:' would break it.
        filter_arg = f"[1:v][0:v]{name}=stats_file={log}"
        cmd = [ffmpeg, "-hide_banner", "-nostats", "-v", "info",
               "-f", "rawvideo", "-pixel_format", "yuv420p",
               "-video_size", f"{width}x{height}", "-framerate", str(fps),
               "-i", clip, "-i", out,
               "-filter_complex", filter_arg, "-an", "-f", "null", "-"]
        try:
            p = _run(cmd, timeout, cwd=work_dir)
        except subprocess.TimeoutExpired:
            result["error"] = f"{name} measurement timed out"
            continue
        stderr = p.stderr.decode("utf-8", "replace")
        match = re.search(pattern, stderr)
        if p.returncode != 0 and not match:
            result["error"] = f"{name} failed: {stderr.strip()[-300:]}"
            continue
        if match:
            value = float(match.group(1))
            if not math.isfinite(value):
                value = 99.0  # ffmpeg prints "inf" for an identical stream
            result[key] = round(value, 4)
        elif logpath.is_file():
            lines = [ln for ln in logpath.read_text(encoding="utf-8",
                                                     errors="replace").splitlines() if ln.strip()]
            values = []
            for ln in lines:
                nums = re.findall(r"[0-9]+\.[0-9]+", ln)
                if nums:
                    values.append(float(nums[-1] if name == "ssim" else nums[-3]))
            if values:
                result[key] = round(sum(values) / len(values), 4)
        else:
            result["error"] = f"{name} produced no summary: {stderr.strip()[-300:]}"
    return result


def stream_info(ffmpeg: Path, path: Path, timeout: float = 120.0) -> dict:
    """Parse the first Video stream line using ffmpeg only (no ffprobe needed)."""
    cmd = [ffmpeg, "-hide_banner", "-i", path, "-f", "null", "-"]
    try:
        p = _run(cmd, timeout)
    except subprocess.TimeoutExpired:
        return {"error": "stream probe timed out"}
    stderr = p.stderr.decode("utf-8", "replace")
    line = next((ln for ln in stderr.splitlines() if "Video:" in ln), None)
    if not line:
        return {"error": "no Video stream found", "raw": stderr.strip()[-300:]}
    info = {"video_line": line.strip()}
    m = re.search(r"Video:\s*([A-Za-z0-9_]+)", line)
    if m:
        info["codec_name"] = m.group(1).lower()
    m = re.search(r"\b(yuv420p|yuv422p|yuv444p|nv12)\b", line)
    if m:
        info["pix_fmt"] = m.group(1)
    m = re.search(r"\b(\d{2,5})x(\d{2,5})\b", line)
    if m:
        info["width"], info["height"] = int(m.group(1)), int(m.group(2))
    return info


def load_quality_envelope(path: Path) -> dict:
    """Read absolute thresholds or the spec's relative-to-reference bands."""
    info = {"path": str(path), "status": "absent", "psnr_db_min": None,
            "ssim_min": None, "bands": None, "warnings": []}
    if not path.is_file():
        info["warnings"].append(
            f"{path} is absent; PSNR/SSIM are not measured and no quality claim is made.")
        return info
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as exc:
        info["status"] = "unreadable"
        info["warnings"].append(f"Cannot read {path}: {exc}")
        return info

    def num(pattern, label):
        m = re.search(pattern, text, re.I)
        return float(m.group(1)) if m else None

    bands = {
        "psnr_lower_db": num(r"P_ref\s*[−–-]\s*([0-9.]+)\s*dB\s*[≤<]=?\s*P_c", "psnr low"),
        "psnr_upper_db": num(r"P_c\s*[≤<]=?\s*P_ref\s*\+\s*([0-9.]+)\s*dB", "psnr high"),
        "ssim_lower": num(r"S_ref\s*[−–-]\s*([0-9.]+)\s*[≤<]=?\s*S_c", "ssim low"),
        "ssim_upper": num(r"S_c\s*[≤<]=?\s*min\(1\.0,\s*S_ref\s*\+\s*([0-9.]+)\)", "ssim high"),
        "size_lower_ratio": num(r"([0-9.]+)\s*[≤<]=?\s*B_c\s*/\s*B_ref", "size low"),
        "size_upper_ratio": num(r"B_c\s*/\s*B_ref\s*[≤<]=?\s*([0-9.]+)", "size high"),
    }
    if all(v is not None for v in bands.values()):
        info["bands"] = bands
        info["status"] = "relative bands"
        return info

    comparison = r"(?:>=|≥|at least|minimum|min\.?|no less than)"
    p = re.search(r"\bpsnr\b[^\n]{0,60}?" + comparison +
                  r"\s*([0-9]+(?:\.[0-9]+)?)\s*(?:db)?", text, re.I)
    s = re.search(r"\bssim\b[^\n]{0,60}?" + comparison +
                  r"\s*([0-9]*\.?[0-9]+)", text, re.I)
    if p:
        info["psnr_db_min"] = float(p.group(1))
    if s:
        info["ssim_min"] = float(s.group(1))
    if info["psnr_db_min"] is not None or info["ssim_min"] is not None:
        info["status"] = "absolute thresholds"
    elif "psnr" in text.lower() or "ssim" in text.lower():
        info["status"] = "mentioned (no numeric contract)"
        info["warnings"].append(
            "Quality file mentions PSNR/SSIM but no parseable numeric contract; "
            "no quality pass/fail gate is applied.")
    else:
        info["status"] = "absent"
    return info


def evaluate_quality(candidate: dict, reference: dict, candidate_size, reference_size,
                     envelope: dict, width: int, height: int, stream: dict) -> list[str]:
    """Return failures against the envelope for one case."""
    failures = []
    if stream.get("error"):
        failures.append("stream probe: " + stream["error"])
    else:
        if stream.get("codec_name") != "h264":
            failures.append(f"codec {stream.get('codec_name')!r} != h264")
        if stream.get("pix_fmt") != "yuv420p":
            failures.append(f"pix_fmt {stream.get('pix_fmt')!r} != yuv420p")
        if (stream.get("width"), stream.get("height")) != (width, height):
            failures.append(
                f"decoded size {stream.get('width')}x{stream.get('height')} "
                f"!= {width}x{height}")
    bands = envelope.get("bands")
    if bands:
        tests = [
            ("PSNR", candidate.get("psnr_db"), reference.get("psnr_db"),
             bands["psnr_lower_db"], bands["psnr_upper_db"], "dB"),
            ("SSIM", candidate.get("ssim"), reference.get("ssim"),
             bands["ssim_lower"], bands["ssim_upper"], ""),
        ]
        for name, got, ref, low, high, unit in tests:
            if got is None or ref is None:
                failures.append(f"{name} not measured")
                continue
            delta = got - ref
            if delta < -low or delta > high:
                failures.append(
                    f"{name} delta {delta:+.4f}{unit} outside "
                    f"[-{low}{unit}, +{high}{unit}]")
        if candidate_size and reference_size:
            ratio = candidate_size / reference_size
            if ratio < bands["size_lower_ratio"] or ratio > bands["size_upper_ratio"]:
                failures.append(
                    f"size ratio {ratio:.3f} outside "
                    f"[{bands['size_lower_ratio']}, {bands['size_upper_ratio']}]")
        else:
            failures.append("size ratio not measurable")
        return failures

    if envelope.get("status") in ("absolute thresholds", "absolute"):
        for name, key, minimum in (("PSNR", "psnr_db", envelope.get("psnr_db_min")),
                                   ("SSIM", "ssim", envelope.get("ssim_min"))):
            if minimum is not None:
                value = candidate.get(key)
                if value is None:
                    failures.append(f"{name} not measured")
                elif value < minimum:
                    failures.append(f"{name} {value:g} < required {minimum:g}")
    return failures
