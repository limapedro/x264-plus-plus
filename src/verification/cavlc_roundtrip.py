#!/usr/bin/env python3
"""Independent CAVLC inverse + ffmpeg check for the standalone harness.

Imports the published VLC tables from cert/bitstream_diag.py (read-only).
Decodes the exact bit string emitted by x264pp_cavlc_residual and reconstructs
the coefficient vector per ITU-T H.264 clause 9.2.4.
"""
import os
import random
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.path.join(tempfile.gettempdir(), "x264pp-cavlc-verify")
os.makedirs(WORK, exist_ok=True)
sys.path.insert(0, os.path.abspath(os.path.join(HERE, "..", "..", "cert")))
import bitstream_diag as bd  # noqa: E402

HARNESS = os.environ.get("X264PP_HARNESS", os.path.join(HERE, "harness.exe"))
FFMPEG = r"C:\ffmpeg\ffmpeg-master-latest-win64-gpl\bin\ffmpeg.exe"


def cat(nc):
    if nc == -1:
        return 4
    if nc == -2:
        return 5
    if nc < 2:
        return 0
    if nc < 4:
        return 1
    if nc < 8:
        return 2
    return 3


class BR:
    def __init__(self, bits):
        self.s = bits
        self.p = 0

    def u1(self):
        b = int(self.s[self.p])
        self.p += 1
        return b

    def u(self, n):
        v = 0
        for _ in range(n):
            v = (v << 1) | self.u1()
        return v


def vlc(br, table):
    acc = ""
    for _ in range(40):
        if br.p >= len(br.s):
            raise ValueError("truncated VLC, acc=%r" % acc)
        acc += str(br.u1())
        if acc in table:
            return table[acc]
    raise ValueError("invalid VLC %r" % acc)


def decode(bits, n, nc):
    br = BR(bits)
    total, t1 = vlc(br, bd.COEFF_TOKEN[cat(nc)])
    if total == 0:
        return [0] * n, br.p
    if total > n or t1 > min(3, total):
        raise ValueError("bad total=%d t1=%d n=%d" % (total, t1, n))
    levels = []
    for _ in range(t1):
        levels.append(1 if br.u1() == 0 else -1)
    sl = 1 if (total > 10 and t1 < 3) else 0
    for i in range(t1, total):
        lp = 0
        while br.u1() == 0:
            lp += 1
        if lp == 14 and sl == 0:
            ss = 4
        elif lp >= 15:
            ss = lp - 3
        else:
            ss = sl
        suffix = br.u(ss)
        code = (min(15, lp) << sl) + suffix
        if lp >= 15 and sl == 0:
            code += 15
        if lp >= 16:
            code += (1 << (lp - 3)) - 4096
        if i == t1 and t1 < 3:
            code += 2
        level = (code + 2) >> 1 if code % 2 == 0 else (-code - 1) >> 1
        levels.append(level)
        if sl == 0:
            sl = 1
        if sl < 6 and abs(level) > (3 << (sl - 1)):
            sl += 1
    if total >= n:
        tz = 0
    else:
        table = bd.TZC[total] if n == 4 else bd.TZ4[total]
        (tz,) = vlc(br, table)
    zeros_left = tz
    runs = []
    for _ in range(total - 1):
        if zeros_left == 0:
            runs.append(0)
            continue
        (run,) = vlc(br, bd.RUN_BEFORE[min(zeros_left, 7)])
        if run > zeros_left:
            raise ValueError("run > zerosLeft")
        zeros_left -= run
        runs.append(run)
    runs.append(zeros_left)
    out = [0] * n
    pos = tz + total - 1 if total < n else n - 1
    for i in range(total):
        if pos < 0 or pos >= n:
            raise ValueError("coeff position out of range: %d" % pos)
        out[pos] = levels[i]
        pos -= runs[i] + 1
    if br.p != len(bits):
        raise ValueError("trailing bits: consumed %d of %d" % (br.p, len(bits)))
    return out, br.p


def load_bits(vectors, mode="i4"):
    vf = os.path.join(WORK, "bits_vectors.txt")
    with open(vf, "w") as f:
        for n, nc, v in vectors:
            f.write("%d %d %s\n" % (n, nc, " ".join(map(str, v))))
    p = subprocess.run([HARNESS, "--bits", vf, mode], capture_output=True, text=True)
    if p.returncode:
        raise RuntimeError("harness --bits failed: " + p.stderr)
    result = []
    for line in p.stdout.splitlines():
        line = line.strip()
        if not line:
            continue
        nbits_str, bits = line.split()
        result.append((int(nbits_str), bits))
    return result


def roundtrip(vectors, mode="i4"):
    bits_list = load_bits(vectors, mode)
    assert len(bits_list) == len(vectors), (len(bits_list), len(vectors))
    bad = []
    for idx, ((n, nc, expect), (nbits, bits)) in enumerate(zip(vectors, bits_list)):
        try:
            got, consumed = decode(bits, n, nc)
            if got != expect:
                bad.append((idx, expect, got, nbits, bits))
        except Exception as exc:
            bad.append((idx, expect, "EXC: %s" % exc, nbits, bits))
    return bad


def ffmpeg_check(vectors, mode="i4"):
    vf = os.path.join(WORK, "ff_vectors.txt")
    out = os.path.join(WORK, "ff.264")
    with open(vf, "w") as f:
        for n, nc, v in vectors:
            f.write("%d %d %s\n" % (n, nc, " ".join(map(str, v))))
    # ffmpeg_check always uses the single-block I_16x16 DC mode: it exercises
    # exactly the x264pp_cavlc_residual bit string with nC=0 and no neighbouring
    # non-zero-count context to model in the harness.
    subprocess.run([HARNESS, out, vf, "i16dc"], check=True, capture_output=True, text=True)
    p = subprocess.run([FFMPEG, "-hide_banner", "-v", "error", "-i", out,
                        "-f", "null", "-"], capture_output=True, text=True)
    return p.returncode, p.stderr, out


def battery():
    v = []
    v.append((16, 0, [0] * 15 + [3]))                       # 1 coeff, last pos
    v.append((16, 0, [3] + [0] * 15))                       # 1 coeff, first pos
    v.append((16, 0, [2] + [0] * 14 + [1]))                 # 2 coeffs, T1 + gap
    v.append((16, 0, [0] * 13 + [1, 1, 1]))                 # 3 trailing ones
    v.append((16, 0, [0] * 12 + [1, 0, 1, 1]))              # T1 across a gap
    v.append((16, 0, [0] * 12 + [1, 1, 1, 1]))              # 4 trailing ones
    v.append((16, 0, [7] + [0] * 14 + [-1]))                # level > 1 + gap
    v.append((16, 0, [0] * 15 + [-8]))                      # negative level
    v.append((16, 0, [1] * 16))                             # 16 coeffs
    v.append((16, 0, [0, 2, 0, 3, 0, 4, 0, 5, 0, 6, 0, 7, 0, 8, 0, 9]))
    v.append((16, 0, [0] * 16))                             # all zero
    return v


def randoms(count, seed=12345):
    rnd = random.Random(seed)
    out = []
    for _ in range(count):
        n = 16
        k = rnd.randint(0, n)
        idxs = rnd.sample(range(n), k)
        vec = [0] * n
        for i in idxs:
            mag = 1 if rnd.random() < 0.45 else rnd.randint(2, 40)
            vec[i] = mag if rnd.random() < 0.5 else -mag
        out.append((n, 0, vec))
    return out


def main():
    count = int(sys.argv[1]) if len(sys.argv) > 1 else 1000
    b = battery()
    bad = roundtrip(b)
    print("battery: %d vectors, %d mismatches" % (len(b), len(bad)))
    for x in bad[:5]:
        print("  FAIL", x)
    if bad:
        return 1
    rc, err, path = ffmpeg_check(b)
    print("battery ffmpeg: rc=%d stderr=%r" % (rc, err[:400]))
    if rc or err.strip():
        return 1
    r = randoms(count)
    bad = roundtrip(r)
    print("random roundtrip: %d vectors, %d mismatches" % (len(r), len(bad)))
    for x in bad[:5]:
        print("  FAIL", x)
    if bad:
        return 1
    rc, err, path = ffmpeg_check(r)
    print("random ffmpeg: rc=%d stderr=%r file=%s" % (rc, err[:400], path))
    if rc or err.strip():
        return 1
    print("ALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
