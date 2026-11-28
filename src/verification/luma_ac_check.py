#!/usr/bin/env python3
"""Controlled luma reconstruction self-check for the gated I_16x16 path.

Encodes four 16x16 I420 frames whose luma contains known DC/AC patterns,
decodes them with ffmpeg and compares every Y sample against the source.
This catches Hadamard sign/order errors, plane-predictor off-by-one errors and
wrong 4x4 AC dequant scaling: any of those makes the reconstructed luma drift
far from the intended pattern.

Usage (from the repository root):

    python src/verification/luma_ac_check.py --encoder build-gate0/x264pp.exe

The encoder must be built with the gated CAVLC path and a zero slice QP delta:

    make -f src/Makefile BUILD_DIR=build-gate0 \
        CPPFLAGS="-DX264PP_FORCE_PCM=0 -DX264PP_SLICE_QP_DELTA=0"
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile

W = H = 16
TOLERANCE = 12  # impulse clipping at QP23 reaches 10


def pattern(kind: str) -> bytearray:
    out = bytearray()
    for y in range(H):
        for x in range(W):
            if kind == "gradx":
                v = 128 + (x - 7) * 8
            elif kind == "grady":
                v = 128 + (y - 7) * 8
            elif kind == "check":
                v = 128 + (64 if ((x // 4 + y // 4) % 2) else -64)
            elif kind == "impulse":
                v = 128 + (48 if (x == 4 and y == 4) else 0)
            else:
                raise ValueError(kind)
            out.append(max(0, min(255, v)))
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--encoder", required=True)
    ap.add_argument("--ffmpeg", default="ffmpeg")
    args = ap.parse_args()

    worst_all = 0
    with tempfile.TemporaryDirectory(prefix="x264pp-luma-") as td:
        for kind in ("gradx", "grady", "check", "impulse"):
            src = os.path.join(td, kind + ".yuv")
            stream = os.path.join(td, kind + ".264")
            dec = os.path.join(td, kind + ".dec.yuv")
            y = pattern(kind)
            chroma = bytearray([128]) * (2 * 8 * 8)
            open(src, "wb").write(y + chroma)

            subprocess.run([
                args.encoder, "--demuxer", "raw", "--input-csp", "i420",
                "--input-res", "16x16", "--fps", "25", "--preset", "medium",
                "--threads", "1", "--frames", "1", "--no-deblock",
                "--no-progress", "--crf", "23", "-o", stream, src,
            ], check=True, stdout=subprocess.DEVNULL)
            subprocess.run([
                args.ffmpeg, "-y", "-hide_banner", "-loglevel", "error",
                "-i", stream, "-pix_fmt", "yuv420p", "-f", "rawvideo", dec,
            ], check=True)

            got = open(dec, "rb").read()[:W * H]
            err = [abs(a - b) for a, b in zip(y, got)]
            worst = max(err)
            worst_all = max(worst_all, worst)
            print("%-8s max error %2d  mean error %.3f" %
                  (kind, worst, sum(err) / len(err)))

    print("worst luma error:", worst_all, "(tolerance %d)" % TOLERANCE)
    if worst_all > TOLERANCE:
        print("FAIL: luma reconstruction does not match the source pattern",
              file=sys.stderr)
        return 1
    print("PASS: luma DC/AC reconstruction and plane prediction are consistent")
    return 0


if __name__ == "__main__":
    sys.exit(main())
