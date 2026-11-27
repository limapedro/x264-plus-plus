#!/usr/bin/env python3
"""Deterministic raw I420 (YUV 4:2:0) clip generator for the x264++ benchmark.

No ffmpeg / external tools are used. The clip is generated from an integer
hash plus analytic spatial/temporal patterns, so the same arguments always
produce byte-identical output (on the same platform). numpy is used when
available for speed; a pure-Python fallback is provided so the script works
with a bare Python 3 install.

Usage:
    python generate_clip.py                      # default: 640x360, 240f, seed 1337
    python generate_clip.py --frames 120 --seed 7 --out assets/clip.yuv
    python generate_clip.py --print-manifest

The generator writes a sidecar <output>.manifest.json containing the exact
parameters and the SHA256 of the produced .yuv, which the benchmark runner
uses to decide whether to reuse or regenerate the clip.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import platform
import sys
from pathlib import Path

GENERATOR_VERSION = 1
DEFAULT_WIDTH = 640
DEFAULT_HEIGHT = 360
DEFAULT_FRAMES = 240
DEFAULT_FPS = 30
DEFAULT_SEED = 1337

try:
    import numpy as np
except ImportError:  # pragma: no cover - exercised only on bare Python
    np = None


# ---------------------------------------------------------------------------
# Content generation
# ---------------------------------------------------------------------------

def _noise32(x, y, t, seed):
    """Small deterministic integer hash, returns a byte 0..255 (pure Python)."""
    v = (x * 374761393 + y * 668265263 + t * 1442695041 + seed) & 0xFFFFFFFF
    v ^= v >> 13
    v = (v * 1274126177) & 0xFFFFFFFF
    v ^= v >> 16
    return v & 0xFF


def _noise_array(xs, ys, t, seed):
    """Vectorised version of _noise32. xs: (1,w), ys: (h,1) uint64 arrays."""
    mask = np.uint64(0xFFFFFFFFFFFFFFFF)
    v = (xs * np.uint64(374761393) + ys * np.uint64(668265263)
         + np.uint64(t) * np.uint64(1442695041) + np.uint64(seed)) & mask
    v ^= v >> np.uint64(13)
    v = (v * np.uint64(1274126177)) & mask
    v ^= v >> np.uint64(16)
    return (v & np.uint64(0xFF)).astype(np.float64)


def _plane_numpy(w, h, t, seed, is_chroma):
    """Return one uint8 plane as bytes (numpy fast path)."""
    xs = np.arange(w, dtype=np.uint64)[None, :]
    ys = np.arange(h, dtype=np.uint64)[:, None]
    xf = xs.astype(np.float64)
    yf = ys.astype(np.float64)
    nf = _noise_array(xs, ys, t, seed) - 127.5

    if not is_chroma:
        # Two drifting sine fields + spatial ramp + deterministic grain.
        val = (112.0
               + 38.0 * np.sin(xf / w * (2.0 * math.pi * 3.0) + t * 0.11
                               + yf / h * (2.0 * math.pi * 0.6))
               + 24.0 * np.sin(yf / h * (2.0 * math.pi * 2.0) - t * 0.07)
               + 26.0 * (xf / w) + 16.0 * (yf / h)
               + nf * 0.12)
        # Moving bright blob (non-trivial temporal motion).
        cx = (w * 0.5) + (w * 0.42) * math.sin(t * 0.045)
        cy = (h * 0.5) + (h * 0.34) * math.cos(t * 0.061)
        d2 = (xf - cx) ** 2 + (yf - cy) ** 2
        val += 58.0 * np.exp(-d2 / (2.0 * (h * 0.09) ** 2))
        # A moving rectangular feature with hard edges.
        bx = int((t * 9) % (w - 70))
        by = int((t * 5) % (h - 44))
        val[by:by + 44, bx:bx + 70] += 34.0
    else:
        # Chroma: colour cast that drifts over time + spatial interference.
        val = (128.0
               + 27.0 * np.sin(xf / w * (2.0 * math.pi * 2.0) + t * 0.043)
               + 15.0 * np.cos(yf / h * (2.0 * math.pi * 3.0) - t * 0.031)
               + nf * 0.09)
    return np.clip(val, 0, 255).astype(np.uint8).tobytes()


def _plane_python(w, h, t, seed, is_chroma, plane_index):
    """Pure-Python fallback. Slower but dependency-free."""
    buf = bytearray(w * h)
    two_pi = 2.0 * math.pi
    cx = (w * 0.5) + (w * 0.42) * math.sin(t * 0.045)
    cy = (h * 0.5) + (h * 0.34) * math.cos(t * 0.061)
    inv_sigma2 = 1.0 / (2.0 * (h * 0.09) ** 2)
    bx = int((t * 9) % (w - 70))
    by = int((t * 5) % (h - 44))
    for y in range(h):
        row = y * w
        yf = y / h
        sy = (math.cos(yf * two_pi * 3.0 - t * 0.031) if is_chroma
              else math.sin(yf * two_pi * 2.0 - t * 0.031))
        cy_d2 = (y - cy) ** 2
        in_band = by <= y < by + 44
        for x in range(w):
            n = _noise32(x, y, t, seed + plane_index * 7919) - 127.5
            xf = x / w
            if is_chroma:
                val = (128.0
                       + 27.0 * math.sin(xf * two_pi * 2.0 + t * 0.043)
                       + 15.0 * sy + n * 0.09)
            else:
                val = (112.0
                       + 38.0 * math.sin(xf * two_pi * 3.0 + t * 0.11
                                         + yf * two_pi * 0.6)
                       + 24.0 * math.sin(yf * two_pi * 2.0 - t * 0.07)
                       + 26.0 * xf + 16.0 * yf + n * 0.12)
                val += 58.0 * math.exp(-((x - cx) ** 2 + cy_d2) * inv_sigma2)
                if in_band and bx <= x < bx + 70:
                    val += 34.0
            if val < 0:
                val = 0
            elif val > 255:
                val = 255
            buf[row + x] = int(val)
    return bytes(buf)


def make_plane(w, h, t, seed, is_chroma=False, plane_index=0):
    if np is not None:
        # Vary the seed per plane so U and V differ on the numpy path too.
        return _plane_numpy(w, h, t, seed + plane_index * 7919, is_chroma)
    return _plane_python(w, h, t, seed, is_chroma, plane_index)


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def write_clip(path: Path, width: int, height: int, frames: int,
               seed: int) -> dict:
    """Write the I420 clip and return its manifest dict."""
    if width <= 0 or height <= 0 or width % 2 or height % 2:
        raise ValueError("width and height must be positive even integers")
    if frames <= 0:
        raise ValueError("frames must be positive")
    cw, ch = width // 2, height // 2

    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "wb") as f:
        for t in range(frames):
            f.write(make_plane(width, height, t, seed, False, 0))   # Y
            f.write(make_plane(cw, ch, t, seed, True, 1))           # U
            f.write(make_plane(cw, ch, t, seed, True, 2))           # V

    manifest = {
        "generator_version": GENERATOR_VERSION,
        "backend": "numpy" if np is not None else "pure-python",
        "width": width,
        "height": height,
        "frames": frames,
        "fps": None,  # filled in by caller / manifest writer
        "seed": seed,
        "format": "I420 / YUV420P (planar Y, U, V; 8-bit)",
        "file": path.name,
        "size_bytes": path.stat().st_size,
        "sha256": sha256_file(path),
        "numpy_version": np.__version__ if np is not None else None,
        "python_version": platform.python_version(),
        "content": ("Drifting 2D sine fields + spatial ramp + moving Gaussian "
                    "blob + moving rectangular patch + hash-based grain; "
                    "chroma has independent drifting patterns."),
    }
    return manifest


def build_manifest(clip_path: Path, width: int, height: int, frames: int,
                   fps: int, seed: int) -> dict:
    m = write_clip(clip_path, width, height, frames, seed)
    m["fps"] = fps
    return m


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--width", type=int, default=DEFAULT_WIDTH)
    ap.add_argument("--height", type=int, default=DEFAULT_HEIGHT)
    ap.add_argument("--frames", type=int, default=DEFAULT_FRAMES)
    ap.add_argument("--fps", type=int, default=DEFAULT_FPS)
    ap.add_argument("--seed", type=int, default=DEFAULT_SEED)
    ap.add_argument("--out", type=Path,
                    default=Path(__file__).resolve().parent / "assets" / "clip_640x360_240f.yuv")
    ap.add_argument("--print-manifest", action="store_true",
                    help="print the manifest JSON to stdout after generating")
    args = ap.parse_args(argv)

    out = args.out.resolve()
    manifest = build_manifest(out, args.width, args.height, args.frames,
                              args.fps, args.seed)
    mpath = out.with_suffix(out.suffix + ".manifest.json")
    mpath.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {out} ({manifest['size_bytes']:,} bytes, "
          f"sha256={manifest['sha256'][:16]}...)")
    print(f"wrote {mpath}")
    if args.print_manifest:
        print(json.dumps(manifest, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
