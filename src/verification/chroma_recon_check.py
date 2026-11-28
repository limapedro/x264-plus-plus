#!/usr/bin/env python3
"""Controlled chroma-DC reconstruction self-check for the gated I_16x16 path.

Encodes one 16x16 I420 frame with a known 4x4-quadrant chroma pattern, decodes
it with ffmpeg, and checks that the decoder's chroma quadrants reproduce the
pattern at QP 23 (slice_qp_delta = 0).  This catches a Hadamard index/order or
power-of-two scaling error: the pattern is asymmetric, so swapping the two
chroma DC frequency indices or multiplying the levels by 4/16 changes the
decoded quadrants dramatically.

Usage (from the repository root):

    python src/verification/chroma_recon_check.py \
        --encoder build-gate/x264pp.exe

The encoder must be built with the gated CAVLC path and a zero slice QP delta:

    make -f src/Makefile BUILD_DIR=build-gate \
        CPPFLAGS="-DX264PP_FORCE_PCM=0 -DX264PP_SLICE_QP_DELTA=0"

Only the encoder binary, ffmpeg and the Python standard library are used.
No reference-encoder source is consulted.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile

W = H = 16
CW, CH = W // 2, H // 2

# Top-left, top-right, bottom-left, bottom-right 4x4 quadrants of an 8x8
# chroma block.  Asymmetric enough to detect f01/f10 swaps and scaling errors.
U_PATTERN = (64, 128, 192, 64)
V_PATTERN = (128, 64, 192, 128)
TOLERANCE = 4  # integer quantisation at QP 23 is within +-2


def write_pattern(path: str) -> None:
    y = bytearray([128]) * (W * H)
    u = bytearray(CW * CH)
    v = bytearray(CW * CH)
    for by in range(2):
        for bx in range(2):
            uq = U_PATTERN[by * 2 + bx]
            vq = V_PATTERN[by * 2 + bx]
            for j in range(4):
                for i in range(4):
                    off = (by * 4 + j) * CW + bx * 4 + i
                    u[off] = uq
                    v[off] = vq
    with open(path, "wb") as f:
        f.write(y)
        f.write(u)
        f.write(v)


def quadrant_means(path: str) -> tuple[list[int], list[int]]:
    data = open(path, "rb").read()
    plane = W * H
    u = data[plane:plane + CW * CH]
    v = data[plane + CW * CH:plane + 2 * CW * CH]

    def quad(p: bytes) -> list[int]:
        return [p[0 * CW + 0], p[0 * CW + 4],
                p[4 * CW + 0], p[4 * CW + 4]]

    return quad(u), quad(v)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--encoder", required=True, help="path to x264pp.exe")
    ap.add_argument("--ffmpeg", default="ffmpeg")
    args = ap.parse_args()

    with tempfile.TemporaryDirectory(prefix="x264pp-chroma-") as td:
        src = os.path.join(td, "pattern.yuv")
        stream = os.path.join(td, "pattern.264")
        dec = os.path.join(td, "decoded.yuv")
        write_pattern(src)

        subprocess.run([
            args.encoder, "--demuxer", "raw", "--input-csp", "i420",
            "--input-res", "%dx%d" % (W, H), "--fps", "25",
            "--preset", "medium", "--threads", "1", "--frames", "1",
            "--no-deblock", "--no-progress", "--crf", "23",
            "-o", stream, src,
        ], check=True, stdout=subprocess.DEVNULL)
        subprocess.run([
            args.ffmpeg, "-y", "-hide_banner", "-loglevel", "error",
            "-i", stream, "-pix_fmt", "yuv420p", "-f", "rawvideo", dec,
        ], check=True)

        u, v = quadrant_means(dec)

    print("expected U quadrants:", U_PATTERN)
    print("decoded  U quadrants:", u)
    print("expected V quadrants:", V_PATTERN)
    print("decoded  V quadrants:", v)

    err_u = [abs(a - b) for a, b in zip(u, U_PATTERN)]
    err_v = [abs(a - b) for a, b in zip(v, V_PATTERN)]
    worst = max(err_u + err_v)
    print("max absolute chroma error:", worst, "(tolerance %d)" % TOLERANCE)

    if worst > TOLERANCE:
        print("FAIL: chroma DC reconstruction does not match the source pattern",
              file=sys.stderr)
        return 1
    print("PASS: chroma reconstruction is neither reordered nor scaled")
    return 0


if __name__ == "__main__":
    sys.exit(main())
