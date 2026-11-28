#!/usr/bin/env python3
"""Independent Annex-B H.264 bitstream diagnostic (Python stdlib only).

Parses a .264 file bit-by-bit and reports where syntax diverges from the
normative H.264 semantics (ITU-T Rec. H.264, clauses 7.3, 9.1 and 9.2).
Tables below were derived from the published standard (Table 9-4, 9-5,
9-7..9-10); no encoder source code is consulted or embedded.

Usage:
    python cert/bitstream_diag.py FILE.264 [--max-mbs N]

Output: SPS/PPS/slice-header fields with RBSP bit offsets and absolute file
byte numbers, then macroblock-layer syntax (I_16x16 and I_NxN/I_4x4) with
per-residual-block offsets.  The first invalid VLC / impossible mb_type /
failed residual block is reported with bit, byte and absolute file offsets.
Exit status is 1 if a syntax error was found, 0 otherwise.
"""

from __future__ import annotations

import argparse
import sys


class DiagError(Exception):
    """A syntax error at a precise bit position."""

    def __init__(self, message, bit, reader=None, detail=""):
        self.message = message
        self.bit = bit
        self.detail = detail
        self.reader = reader
        super().__init__(message)

    def location(self):
        if self.reader is None:
            return "bit %d" % self.bit
        r = self.reader
        byte = self.bit // 8
        abs_byte = r.byte_map[byte] if byte < len(r.byte_map) else None
        where = "rbsp bit %d (byte %d, bit-in-byte %d" % (self.bit, byte, self.bit % 8)
        if abs_byte is not None:
            where += ", file byte %d/0x%02X" % (abs_byte, abs_byte)
        where += ")"
        if self.detail:
            where += " " + self.detail
        return where


class BitReader:
    """MSB-first bit reader over an RBSP byte array."""

    def __init__(self, data: bytes, byte_map=None):
        self.data = data
        self.pos = 0
        self.byte_map = byte_map if byte_map is not None else list(range(len(data)))

    @property
    def nbits(self):
        return len(self.data) * 8

    def u(self, n):
        if n == 0:
            return 0
        if self.pos + n > self.nbits:
            raise DiagError("truncated RBSP while reading u(%d)" % n, self.pos, self)
        value = 0
        for _ in range(n):
            value = (value << 1) | ((self.data[self.pos >> 3] >> (7 - (self.pos & 7))) & 1)
            self.pos += 1
        return value

    def u1(self):
        return self.u(1)

    def ue(self):
        start = self.pos
        zeros = 0
        while True:
            if self.pos >= self.nbits:
                raise DiagError("truncated RBSP in ue(v)", start, self)
            b = self.u1()
            if b:
                break
            zeros += 1
            if zeros > 31:
                raise DiagError("implausibly long ue(v) prefix", start, self)
        return (1 << zeros) - 1 + self.u(zeros)

    def se(self):
        code_num = self.ue()
        if code_num & 1:
            return (code_num + 1) // 2
        return -(code_num // 2)

    def te(self, max_value):
        # ITU-T H.264 9.1: when the range is {0,1}, codeNum = !b.
        if max_value == 1:
            return 1 - self.u1()
        return self.ue()

    def align_byte(self, label="alignment"):
        start = self.pos
        while self.pos % 8:
            if self.u1() != 0:
                raise DiagError("nonzero %s bit" % label, self.pos - 1, self)
        return start

    def more_rbsp_data(self):
        """True if nontrivial (non-trailing) data remains (7.3.2.11)."""
        if self.pos >= self.nbits:
            return False
        first_one = None
        for p in range(self.pos, self.nbits):
            if (self.data[p >> 3] >> (7 - (p & 7))) & 1:
                first_one = p
                break
        if first_one is None:
            return False
        # rbsp_trailing_bits(): a single 1 followed by zeroes to a byte boundary.
        if all(
            ((self.data[p >> 3] >> (7 - (p & 7))) & 1) == 0
            for p in range(first_one + 1, self.nbits)
        ) and self.nbits - first_one <= 8:
            return False
        return True

    def peek_bits(self, n):
        save = self.pos
        try:
            return "".join(str(self.u1()) for _ in range(min(n, self.nbits - self.pos)))
        finally:
            self.pos = save


# ---------------------------------------------------------------------------
# Normative VLC tables (ITU-T H.264 clause 9.2, Tables 9-4..9-10).
# Each entry is (value(s)..., codeword); codewords were checked prefix-free.
# Format: (TotalCoeff, TrailingOnes, code) for coeff_token; (value, code) for
# the rest.  CT_0..3: nC 0..1, 2..3, 4..7, >=8; CT_4: nC=-1; CT_5: nC=-2.
# ---------------------------------------------------------------------------
CT_0 = [
    (0,0,'1'),(1,0,'000101'),(1,1,'01'),(2,0,'00000111'),
    (2,1,'000100'),(2,2,'001'),(3,0,'000000111'),(3,1,'00000110'),
    (3,2,'0000101'),(3,3,'00011'),(4,0,'0000000111'),(4,1,'000000110'),
    (4,2,'00000101'),(4,3,'000011'),(5,0,'00000000111'),(5,1,'0000000110'),
    (5,2,'000000101'),(5,3,'0000100'),(6,0,'0000000001111'),(6,1,'00000000110'),
    (6,2,'0000000101'),(6,3,'00000100'),(7,0,'0000000001011'),(7,1,'0000000001110'),
    (7,2,'00000000101'),(7,3,'000000100'),(8,0,'0000000001000'),(8,1,'0000000001010'),
    (8,2,'0000000001101'),(8,3,'0000000100'),(9,0,'00000000001111'),(9,1,'00000000001110'),
    (9,2,'0000000001001'),(9,3,'00000000100'),(10,0,'00000000001011'),(10,1,'00000000001010'),
    (10,2,'00000000001101'),(10,3,'0000000001100'),(11,0,'000000000001111'),(11,1,'000000000001110'),
    (11,2,'00000000001001'),(11,3,'00000000001100'),(12,0,'000000000001011'),(12,1,'000000000001010'),
    (12,2,'000000000001101'),(12,3,'00000000001000'),(13,0,'0000000000001111'),(13,1,'000000000000001'),
    (13,2,'000000000001001'),(13,3,'000000000001100'),(14,0,'0000000000001011'),(14,1,'0000000000001110'),
    (14,2,'0000000000001101'),(14,3,'000000000001000'),(15,0,'0000000000000111'),(15,1,'0000000000001010'),
    (15,2,'0000000000001001'),(15,3,'0000000000001100'),(16,0,'0000000000000100'),(16,1,'0000000000000110'),
    (16,2,'0000000000000101'),(16,3,'0000000000001000'),
]
CT_1 = [
    (0,0,'11'),(1,0,'001011'),(1,1,'10'),(2,0,'000111'),
    (2,1,'00111'),(2,2,'011'),(3,0,'0000111'),(3,1,'001010'),
    (3,2,'001001'),(3,3,'0101'),(4,0,'00000111'),(4,1,'000110'),
    (4,2,'000101'),(4,3,'0100'),(5,0,'00000100'),(5,1,'0000110'),
    (5,2,'0000101'),(5,3,'00110'),(6,0,'000000111'),(6,1,'00000110'),
    (6,2,'00000101'),(6,3,'001000'),(7,0,'00000001111'),(7,1,'000000110'),
    (7,2,'000000101'),(7,3,'000100'),(8,0,'00000001011'),(8,1,'00000001110'),
    (8,2,'00000001101'),(8,3,'0000100'),(9,0,'000000001111'),(9,1,'00000001010'),
    (9,2,'00000001001'),(9,3,'000000100'),(10,0,'000000001011'),(10,1,'000000001110'),
    (10,2,'000000001101'),(10,3,'00000001100'),(11,0,'000000001000'),(11,1,'000000001010'),
    (11,2,'000000001001'),(11,3,'00000001000'),(12,0,'0000000001111'),(12,1,'0000000001110'),
    (12,2,'0000000001101'),(12,3,'000000001100'),(13,0,'0000000001011'),(13,1,'0000000001010'),
    (13,2,'0000000001001'),(13,3,'0000000001100'),(14,0,'0000000000111'),(14,1,'00000000001011'),
    (14,2,'0000000000110'),(14,3,'0000000001000'),(15,0,'00000000001001'),(15,1,'00000000001000'),
    (15,2,'00000000001010'),(15,3,'0000000000001'),(16,0,'00000000000111'),(16,1,'00000000000110'),
    (16,2,'00000000000101'),(16,3,'00000000000100'),
]
CT_2 = [
    (0,0,'1111'),(1,0,'001111'),(1,1,'1110'),(2,0,'001011'),
    (2,1,'01111'),(2,2,'1101'),(3,0,'001000'),(3,1,'01100'),
    (3,2,'01110'),(3,3,'1100'),(4,0,'0001111'),(4,1,'01010'),
    (4,2,'01011'),(4,3,'1011'),(5,0,'0001011'),(5,1,'01000'),
    (5,2,'01001'),(5,3,'1010'),(6,0,'0001001'),(6,1,'001110'),
    (6,2,'001101'),(6,3,'1001'),(7,0,'0001000'),(7,1,'001010'),
    (7,2,'001001'),(7,3,'1000'),(8,0,'00001111'),(8,1,'0001110'),
    (8,2,'0001101'),(8,3,'01101'),(9,0,'00001011'),(9,1,'00001110'),
    (9,2,'0001010'),(9,3,'001100'),(10,0,'000001111'),(10,1,'00001010'),
    (10,2,'00001101'),(10,3,'0001100'),(11,0,'000001011'),(11,1,'000001110'),
    (11,2,'00001001'),(11,3,'00001100'),(12,0,'000001000'),(12,1,'000001010'),
    (12,2,'000001101'),(12,3,'00001000'),(13,0,'0000001101'),(13,1,'000000111'),
    (13,2,'000001001'),(13,3,'000001100'),(14,0,'0000001001'),(14,1,'0000001100'),
    (14,2,'0000001011'),(14,3,'0000001010'),(15,0,'0000000101'),(15,1,'0000001000'),
    (15,2,'0000000111'),(15,3,'0000000110'),(16,0,'0000000001'),(16,1,'0000000100'),
    (16,2,'0000000011'),(16,3,'0000000010'),
]
CT_3 = [
    (0,0,'000011'),(1,0,'000000'),(1,1,'000001'),(2,0,'000100'),
    (2,1,'000101'),(2,2,'000110'),(3,0,'001000'),(3,1,'001001'),
    (3,2,'001010'),(3,3,'001011'),(4,0,'001100'),(4,1,'001101'),
    (4,2,'001110'),(4,3,'001111'),(5,0,'010000'),(5,1,'010001'),
    (5,2,'010010'),(5,3,'010011'),(6,0,'010100'),(6,1,'010101'),
    (6,2,'010110'),(6,3,'010111'),(7,0,'011000'),(7,1,'011001'),
    (7,2,'011010'),(7,3,'011011'),(8,0,'011100'),(8,1,'011101'),
    (8,2,'011110'),(8,3,'011111'),(9,0,'100000'),(9,1,'100001'),
    (9,2,'100010'),(9,3,'100011'),(10,0,'100100'),(10,1,'100101'),
    (10,2,'100110'),(10,3,'100111'),(11,0,'101000'),(11,1,'101001'),
    (11,2,'101010'),(11,3,'101011'),(12,0,'101100'),(12,1,'101101'),
    (12,2,'101110'),(12,3,'101111'),(13,0,'110000'),(13,1,'110001'),
    (13,2,'110010'),(13,3,'110011'),(14,0,'110100'),(14,1,'110101'),
    (14,2,'110110'),(14,3,'110111'),(15,0,'111000'),(15,1,'111001'),
    (15,2,'111010'),(15,3,'111011'),(16,0,'111100'),(16,1,'111101'),
    (16,2,'111110'),(16,3,'111111'),
]
CT_4 = [
    (0,0,'01'),(1,0,'000111'),(1,1,'1'),(2,0,'000100'),
    (2,1,'000110'),(2,2,'001'),(3,0,'000011'),(3,1,'0000011'),
    (3,2,'0000010'),(3,3,'000101'),(4,0,'000010'),(4,1,'00000011'),
    (4,2,'00000010'),(4,3,'0000000'),(5,0,''),(5,1,''),
    (5,2,''),(5,3,''),(6,0,''),(6,1,''),
    (6,2,''),(6,3,''),(7,0,''),(7,1,''),
    (7,2,''),(7,3,''),(8,0,''),(8,1,''),
    (8,2,''),(8,3,''),(9,0,''),(9,1,''),
    (9,2,''),(9,3,''),(10,0,''),(10,1,''),
    (10,2,''),(10,3,''),(11,0,''),(11,1,''),
    (11,2,''),(11,3,''),(12,0,''),(12,1,''),
    (12,2,''),(12,3,''),(13,0,''),(13,1,''),
    (13,2,''),(13,3,''),(14,0,''),(14,1,''),
    (14,2,''),(14,3,''),(15,0,''),(15,1,''),
    (15,2,''),(15,3,''),(16,0,''),(16,1,''),
    (16,2,''),(16,3,''),
]
CT_5 = [
    (0,0,'1'),(1,0,'0001111'),(1,1,'01'),(2,0,'0001110'),
    (2,1,'0001101'),(2,2,'001'),(3,0,'000000111'),(3,1,'0001100'),
    (3,2,'0001011'),(3,3,'00001'),(4,0,'000000110'),(4,1,'000000101'),
    (4,2,'0001010'),(4,3,'000001'),(5,0,'0000000111'),(5,1,'0000000110'),
    (5,2,'000000100'),(5,3,'0001001'),(6,0,'00000000111'),(6,1,'00000000110'),
    (6,2,'0000000101'),(6,3,'0001000'),(7,0,'000000000111'),(7,1,'000000000110'),
    (7,2,'00000000101'),(7,3,'0000000100'),(8,0,'0000000000111'),(8,1,'000000000101'),
    (8,2,'000000000100'),(8,3,'00000000100'),(9,0,''),(9,1,''),
    (9,2,''),(9,3,''),(10,0,''),(10,1,''),
    (10,2,''),(10,3,''),(11,0,''),(11,1,''),
    (11,2,''),(11,3,''),(12,0,''),(12,1,''),
    (12,2,''),(12,3,''),(13,0,''),(13,1,''),
    (13,2,''),(13,3,''),(14,0,''),(14,1,''),
    (14,2,''),(14,3,''),(15,0,''),(15,1,''),
    (15,2,''),(15,3,''),(16,0,''),(16,1,''),
    (16,2,''),(16,3,''),
]
TZ4_1 = [
    (0,'1'),(1,'011'),(2,'010'),(3,'0011'),
    (4,'0010'),(5,'00011'),(6,'00010'),(7,'000011'),
    (8,'000010'),(9,'0000011'),(10,'0000010'),(11,'00000011'),
    (12,'00000010'),(13,'000000011'),(14,'000000010'),(15,'000000001'),
]
TZ4_2 = [
    (0,'111'),(1,'110'),(2,'101'),(3,'100'),
    (4,'011'),(5,'0101'),(6,'0100'),(7,'0011'),
    (8,'0010'),(9,'00011'),(10,'00010'),(11,'000011'),
    (12,'000010'),(13,'000001'),(14,'000000'),
]
TZ4_3 = [
    (0,'0101'),(1,'111'),(2,'110'),(3,'101'),
    (4,'0100'),(5,'0011'),(6,'100'),(7,'011'),
    (8,'0010'),(9,'00011'),(10,'00010'),(11,'000001'),
    (12,'00001'),(13,'000000'),
]
TZ4_4 = [
    (0,'00011'),(1,'111'),(2,'0101'),(3,'0100'),
    (4,'110'),(5,'101'),(6,'100'),(7,'0011'),
    (8,'011'),(9,'0010'),(10,'00010'),(11,'00001'),
    (12,'00000'),
]
TZ4_5 = [
    (0,'0101'),(1,'0100'),(2,'0011'),(3,'111'),
    (4,'110'),(5,'101'),(6,'100'),(7,'011'),
    (8,'0010'),(9,'00001'),(10,'0001'),(11,'00000'),
]
TZ4_6 = [
    (0,'000001'),(1,'00001'),(2,'111'),(3,'110'),
    (4,'101'),(5,'100'),(6,'011'),(7,'010'),
    (8,'0001'),(9,'001'),(10,'000000'),
]
TZ4_7 = [
    (0,'000001'),(1,'00001'),(2,'101'),(3,'100'),
    (4,'011'),(5,'11'),(6,'010'),(7,'0001'),
    (8,'001'),(9,'000000'),
]
TZ4_8 = [
    (0,'000001'),(1,'0001'),(2,'00001'),(3,'011'),
    (4,'11'),(5,'10'),(6,'010'),(7,'001'),
    (8,'000000'),
]
TZ4_9 = [
    (0,'000001'),(1,'000000'),(2,'0001'),(3,'11'),
    (4,'10'),(5,'001'),(6,'01'),(7,'00001'),
]
TZ4_10 = [
    (0,'00001'),(1,'00000'),(2,'001'),(3,'11'),
    (4,'10'),(5,'01'),(6,'0001'),
]
TZ4_11 = [
    (0,'0000'),(1,'0001'),(2,'001'),(3,'010'),
    (4,'1'),(5,'011'),
]
TZ4_12 = [
    (0,'0000'),(1,'0001'),(2,'01'),(3,'1'),
    (4,'001'),
]
TZ4_13 = [
    (0,'000'),(1,'001'),(2,'1'),(3,'01'),
]
TZ4_14 = [
    (0,'00'),(1,'01'),(2,'1'),
]
TZ4_15 = [
    (0,'0'),(1,'1'),
]
TZC_1 = [
    (0,'1'),(1,'01'),(2,'001'),(3,'000'),
]
TZC_2 = [
    (0,'1'),(1,'01'),(2,'00'),
]
TZC_3 = [
    (0,'1'),(1,'0'),
]
RB_1 = [
    (0,'1'),(1,'0'),
]
RB_2 = [
    (0,'1'),(1,'01'),(2,'00'),
]
RB_3 = [
    (0,'11'),(1,'10'),(2,'01'),(3,'00'),
]
RB_4 = [
    (0,'11'),(1,'10'),(2,'01'),(3,'001'),
    (4,'000'),
]
RB_5 = [
    (0,'11'),(1,'10'),(2,'011'),(3,'010'),
    (4,'001'),(5,'000'),
]
RB_6 = [
    (0,'11'),(1,'000'),(2,'001'),(3,'011'),
    (4,'010'),(5,'101'),(6,'100'),
]
RB_7 = [
    (0,'111'),(1,'110'),(2,'101'),(3,'100'),
    (4,'011'),(5,'010'),(6,'001'),(7,'0001'),
    (8,'00001'),(9,'000001'),(10,'0000001'),(11,'00000001'),
    (12,'000000001'),(13,'0000000001'),(14,'00000000001'),
]
CBP_I = [47, 31, 15, 0, 23, 27, 29, 30, 7, 11, 13, 14, 39, 43, 45, 46, 16, 3, 5, 10, 12, 19, 21, 26, 28, 35, 37, 42, 44, 1, 2, 4, 8, 17, 18, 20, 24, 6, 9, 22, 25, 32, 33, 34, 36, 40, 38, 41]
CBP_P = [0, 16, 1, 2, 4, 8, 32, 3, 5, 10, 12, 15, 47, 7, 11, 13, 14, 6, 9, 31, 35, 37, 42, 44, 33, 34, 36, 40, 39, 43, 45, 46, 17, 18, 20, 24, 19, 21, 26, 28, 23, 27, 29, 30, 22, 25, 38, 41]



def _vlc(entries):
    return {code: values for *values, code in entries if code}


COEFF_TOKEN = [_vlc(CT_0), _vlc(CT_1), _vlc(CT_2), _vlc(CT_3), _vlc(CT_4), _vlc(CT_5)]
TZ4 = {k: _vlc(v) for k, v in {
    1: TZ4_1, 2: TZ4_2, 3: TZ4_3, 4: TZ4_4, 5: TZ4_5, 6: TZ4_6, 7: TZ4_7,
    8: TZ4_8, 9: TZ4_9, 10: TZ4_10, 11: TZ4_11, 12: TZ4_12, 13: TZ4_13,
    14: TZ4_14, 15: TZ4_15,
}.items()}
TZC = {k: _vlc(v) for k, v in {1: TZC_1, 2: TZC_2, 3: TZC_3}.items()}
RUN_BEFORE = {k: _vlc(v) for k, v in {
    1: RB_1, 2: RB_2, 3: RB_3, 4: RB_4, 5: RB_5, 6: RB_6, 7: RB_7,
}.items()}


def nc_table(nc):
    # nC=-1/-2 are the dedicated chroma-DC contexts (Table 9-5), not
    # ordinary neighbour counts; they must map to CT_4 / CT_5.
    if nc == -1:
        return COEFF_TOKEN[4]
    if nc == -2:
        return COEFF_TOKEN[5]
    if nc < 2:
        return COEFF_TOKEN[0]
    if nc < 4:
        return COEFF_TOKEN[1]
    if nc < 8:
        return COEFF_TOKEN[2]
    return COEFF_TOKEN[3]


def read_vlc(br, table, what, start=None, max_bits=24):
    """Decode one prefix VLC; returns (values_tuple, codeword, end_bit)."""
    if start is None:
        start = br.pos
    acc = ""
    for _ in range(max_bits):
        if br.pos >= br.nbits:
            break
        acc += str(br.u1())
        hit = table.get(acc)
        if hit is not None:
            return hit, acc, br.pos
    raise DiagError(
        "invalid %s VLC" % what, start, br,
        detail="(bits %r)" % (acc if acc else br.peek_bits(max_bits)),
    )


# ---------------------------------------------------------------------------
# Annex B / NAL helpers
# ---------------------------------------------------------------------------

class Nal:
    def __init__(self, index, nal_ref_idc, nal_type, ebsp, ebsp_abs, sc_offset):
        self.index = index
        self.nal_ref_idc = nal_ref_idc
        self.type = nal_type
        self.ebsp = ebsp
        self.ebsp_abs = ebsp_abs
        self.sc_offset = sc_offset
        self.rbsp, self.byte_map = unescape(ebsp, ebsp_abs)


def split_annex_b(data):
    starts = []
    i = 0
    while i + 2 < len(data):
        if data[i] == 0 and data[i + 1] == 0 and data[i + 2] == 1:
            begin = i - 1 if i > 0 and data[i - 1] == 0 else i
            starts.append((begin, i + 3))
            i += 3
        else:
            i += 1
    nals = []
    for k, (start, body) in enumerate(starts):
        end = starts[k + 1][0] if k + 1 < len(starts) else len(data)
        ebsp = bytearray(data[body:end])
        abs_list = list(range(body, end))
        while ebsp and ebsp[-1] == 0:  # trailing_zero_8bits
            ebsp.pop()
            abs_list.pop()
        if not ebsp:
            continue
        header = ebsp[0]
        nal = Nal(
            k, (header >> 5) & 3, header & 0x1F,
            bytes(ebsp[1:]), abs_list[1:], start,
        )
        nal.forbidden = (header >> 7) & 1
        if nal.forbidden:
            raise DiagError("forbidden_zero_bit is 1", 0, None)
        nals.append(nal)
    return nals


def unescape(ebsp, abs_list):
    out = bytearray()
    abs_out = []
    zeros = 0
    for i, b in enumerate(ebsp):
        if zeros == 2 and b == 3:  # emulation_prevention_three_byte
            zeros = 0
            continue
        out.append(b)
        abs_out.append(abs_list[i])
        zeros = zeros + 1 if b == 0 else 0
    return bytes(out), abs_out


# ---------------------------------------------------------------------------
# Printing helpers
# ---------------------------------------------------------------------------

class Out:
    def __init__(self, stream=sys.stdout):
        self.stream = stream

    def line(self, text=""):
        print(text, file=self.stream)

    def field(self, br, name, value, start, note=""):
        byte = start // 8
        abs_byte = br.byte_map[byte] if byte < len(br.byte_map) else None
        loc = "bit %5d..%-5d" % (start, max(start, br.pos - 1))
        if abs_byte is not None:
            loc += " file %5d/0x%02X#%d" % (abs_byte, abs_byte, start % 8)
        suffix = ("  " + note) if note else ""
        self.line("    %-40s = %-12s @%s%s" % (name, str(value), loc, suffix))


OUT = Out()


def want(condition, message, br, start):
    if not condition:
        raise DiagError(message, start, br)


# ---------------------------------------------------------------------------
# Parameter sets and slice header
# ---------------------------------------------------------------------------

HIGH_PROFILES = {100, 110, 122, 244, 44, 83, 86, 118, 128}


def scaling_list(br, size, name):
    last_scale = 8
    next_scale = 8
    for j in range(size):
        if next_scale != 0:
            delta = br.se()
            next_scale = (last_scale + delta + 256) % 256
        last_scale = next_scale if next_scale != 0 else last_scale
    return last_scale


def parse_sps(br):
    sps = {}
    b = br.pos
    sps["profile_idc"] = br.u(8); OUT.field(br, "profile_idc", sps["profile_idc"], b)
    for i in range(6):
        b = br.pos
        sps["constraint_set%d_flag" % i] = br.u1(); OUT.field(br, "constraint_set%d_flag" % i, sps["constraint_set%d_flag" % i], b)
    b = br.pos
    reserved = br.u(2); OUT.field(br, "reserved_zero_2bits", reserved, b)
    b = br.pos
    sps["level_idc"] = br.u(8); OUT.field(br, "level_idc", sps["level_idc"], b)
    b = br.pos
    sps["id"] = br.ue(); OUT.field(br, "seq_parameter_set_id", sps["id"], b)
    want(sps["id"] <= 31, "seq_parameter_set_id > 31", br, b)
    sps["chroma_format_idc"] = 1
    sps["separate_colour_plane_flag"] = 0
    sps["bit_depth_luma_minus8"] = 0
    sps["bit_depth_chroma_minus8"] = 0
    if sps["profile_idc"] in HIGH_PROFILES:
        b = br.pos
        sps["chroma_format_idc"] = br.ue(); OUT.field(br, "chroma_format_idc", sps["chroma_format_idc"], b)
        want(sps["chroma_format_idc"] <= 3, "chroma_format_idc > 3", br, b)
        if sps["chroma_format_idc"] == 3:
            b = br.pos
            sps["separate_colour_plane_flag"] = br.u1(); OUT.field(br, "separate_colour_plane_flag", sps["separate_colour_plane_flag"], b)
        b = br.pos
        sps["bit_depth_luma_minus8"] = br.ue(); OUT.field(br, "bit_depth_luma_minus8", sps["bit_depth_luma_minus8"], b)
        b = br.pos
        sps["bit_depth_chroma_minus8"] = br.ue(); OUT.field(br, "bit_depth_chroma_minus8", sps["bit_depth_chroma_minus8"], b)
        b = br.pos
        sps["qpprime_y_zero_transform_bypass_flag"] = br.u1(); OUT.field(br, "qpprime_y_zero_transform_bypass_flag", sps["qpprime_y_zero_transform_bypass_flag"], b)
        b = br.pos
        matrix_present = br.u1(); OUT.field(br, "seq_scaling_matrix_present_flag", matrix_present, b)
        if matrix_present:
            count = 8 if sps["chroma_format_idc"] != 3 else 12
            for i in range(count):
                b = br.pos
                present = br.u1(); OUT.field(br, "seq_scaling_list_present_flag[%d]" % i, present, b)
                if present:
                    scaling_list(br, 16 if i < 6 else 64, "SPS scaling list %d" % i)
    b = br.pos
    sps["log2_max_frame_num_minus4"] = br.ue(); OUT.field(br, "log2_max_frame_num_minus4", sps["log2_max_frame_num_minus4"], b)
    want(sps["log2_max_frame_num_minus4"] <= 12, "log2_max_frame_num_minus4 > 12", br, b)
    b = br.pos
    sps["pic_order_cnt_type"] = br.ue(); OUT.field(br, "pic_order_cnt_type", sps["pic_order_cnt_type"], b)
    want(sps["pic_order_cnt_type"] <= 2, "pic_order_cnt_type > 2", br, b)
    if sps["pic_order_cnt_type"] == 0:
        b = br.pos
        sps["log2_max_pic_order_cnt_lsb_minus4"] = br.ue(); OUT.field(br, "log2_max_pic_order_cnt_lsb_minus4", sps["log2_max_pic_order_cnt_lsb_minus4"], b)
        want(sps["log2_max_pic_order_cnt_lsb_minus4"] <= 12, "log2_max_pic_order_cnt_lsb_minus4 > 12", br, b)
    elif sps["pic_order_cnt_type"] == 1:
        b = br.pos
        sps["delta_pic_order_always_zero_flag"] = br.u1(); OUT.field(br, "delta_pic_order_always_zero_flag", sps["delta_pic_order_always_zero_flag"], b)
        for name in ("offset_for_non_ref_pic", "offset_for_top_to_bottom_field"):
            b = br.pos
            v = br.se(); OUT.field(br, name, v, b)
        b = br.pos
        cycle = br.ue(); OUT.field(br, "num_ref_frames_in_pic_order_cnt_cycle", cycle, b)
        for i in range(cycle):
            b = br.pos
            v = br.se(); OUT.field(br, "offset_for_ref_frame[%d]" % i, v, b)
    b = br.pos
    sps["max_num_ref_frames"] = br.ue(); OUT.field(br, "max_num_ref_frames", sps["max_num_ref_frames"], b)
    b = br.pos
    sps["gaps_in_frame_num_value_allowed_flag"] = br.u1(); OUT.field(br, "gaps_in_frame_num_value_allowed_flag", sps["gaps_in_frame_num_value_allowed_flag"], b)
    b = br.pos
    sps["pic_width_in_mbs_minus1"] = br.ue(); OUT.field(br, "pic_width_in_mbs_minus1", sps["pic_width_in_mbs_minus1"], b)
    b = br.pos
    sps["pic_height_in_map_units_minus1"] = br.ue(); OUT.field(br, "pic_height_in_map_units_minus1", sps["pic_height_in_map_units_minus1"], b)
    b = br.pos
    sps["frame_mbs_only_flag"] = br.u1(); OUT.field(br, "frame_mbs_only_flag", sps["frame_mbs_only_flag"], b)
    if not sps["frame_mbs_only_flag"]:
        b = br.pos
        sps["mb_adaptive_frame_field_flag"] = br.u1(); OUT.field(br, "mb_adaptive_frame_field_flag", sps["mb_adaptive_frame_field_flag"], b)
    b = br.pos
    sps["direct_8x8_inference_flag"] = br.u1(); OUT.field(br, "direct_8x8_inference_flag", sps["direct_8x8_inference_flag"], b)
    b = br.pos
    sps["frame_cropping_flag"] = br.u1(); OUT.field(br, "frame_cropping_flag", sps["frame_cropping_flag"], b)
    if sps["frame_cropping_flag"]:
        for name in ("frame_crop_left_offset", "frame_crop_right_offset", "frame_crop_top_offset", "frame_crop_bottom_offset"):
            b = br.pos
            v = br.ue(); OUT.field(br, name, v, b)
    b = br.pos
    vui_present = br.u1(); OUT.field(br, "vui_parameters_present_flag", vui_present, b)
    if vui_present:
        parse_vui(br, sps)
    sps["PicWidthInMbs"] = sps["pic_width_in_mbs_minus1"] + 1
    sps["ChromaArrayType"] = 0 if sps["separate_colour_plane_flag"] else sps["chroma_format_idc"]
    return sps


def hrd_parameters(br):
    b = br.pos
    cpb_cnt = br.ue(); OUT.field(br, "cpb_cnt_minus1", cpb_cnt, b)
    b = br.pos
    v = br.u(4); OUT.field(br, "bit_rate_scale", v, b)
    b = br.pos
    v = br.u(4); OUT.field(br, "cpb_size_scale", v, b)
    for i in range(cpb_cnt + 1):
        b = br.pos
        v = br.ue(); OUT.field(br, "bit_rate_value_minus1[%d]" % i, v, b)
        b = br.pos
        v = br.ue(); OUT.field(br, "cpb_size_value_minus1[%d]" % i, v, b)
        b = br.pos
        v = br.u1(); OUT.field(br, "cbr_flag[%d]" % i, v, b)
    for name in ("initial_cpb_removal_delay_length_minus1", "cpb_removal_delay_length_minus1",
                 "dpb_output_delay_length_minus1", "time_offset_length"):
        b = br.pos
        v = br.u(5); OUT.field(br, name, v, b)


def parse_vui(br, sps):
    b = br.pos
    v = br.u1(); OUT.field(br, "aspect_ratio_info_present_flag", v, b)
    if v:
        b = br.pos
        idc = br.u(8); OUT.field(br, "aspect_ratio_idc", idc, b)
        if idc == 255:
            b = br.pos
            x = br.u(16); OUT.field(br, "sar_width", x, b)
            b = br.pos
            x = br.u(16); OUT.field(br, "sar_height", x, b)
    b = br.pos
    v = br.u1(); OUT.field(br, "overscan_info_present_flag", v, b)
    if v:
        b = br.pos
        x = br.u1(); OUT.field(br, "overscan_appropriate_flag", x, b)
    b = br.pos
    v = br.u1(); OUT.field(br, "video_signal_type_present_flag", v, b)
    if v:
        b = br.pos
        x = br.u(3); OUT.field(br, "video_format", x, b)
        b = br.pos
        x = br.u1(); OUT.field(br, "video_full_range_flag", x, b)
        b = br.pos
        x = br.u1(); OUT.field(br, "colour_description_present_flag", x, b)
        if x:
            for name in ("colour_primaries", "transfer_characteristics", "matrix_coefficients"):
                b = br.pos
                y = br.u(8); OUT.field(br, name, y, b)
    b = br.pos
    v = br.u1(); OUT.field(br, "chroma_loc_info_present_flag", v, b)
    if v:
        for name in ("chroma_sample_loc_type_top_field", "chroma_sample_loc_type_bottom_field"):
            b = br.pos
            x = br.ue(); OUT.field(br, name, x, b)
    b = br.pos
    v = br.u1(); OUT.field(br, "timing_info_present_flag", v, b)
    if v:
        for name in ("num_units_in_tick", "time_scale"):
            b = br.pos
            x = br.u(32); OUT.field(br, name, x, b)
        b = br.pos
        x = br.u1(); OUT.field(br, "fixed_frame_rate_flag", x, b)
    b = br.pos
    nal_hrd = br.u1(); OUT.field(br, "nal_hrd_parameters_present_flag", nal_hrd, b)
    if nal_hrd:
        hrd_parameters(br)
    b = br.pos
    vcl_hrd = br.u1(); OUT.field(br, "vcl_hrd_parameters_present_flag", vcl_hrd, b)
    if vcl_hrd:
        hrd_parameters(br)
    if nal_hrd or vcl_hrd:
        b = br.pos
        x = br.u1(); OUT.field(br, "low_delay_hrd_flag", x, b)
    b = br.pos
    x = br.u1(); OUT.field(br, "pic_struct_present_flag", x, b)
    b = br.pos
    v = br.u1(); OUT.field(br, "bitstream_restriction_flag", v, b)
    if v:
        for name, desc in (
            ("motion_vectors_over_pic_boundaries_flag", "u1"),
            ("max_bytes_per_pic_denom", "ue"),
            ("max_bits_per_mb_denom", "ue"),
            ("log2_max_mv_length_horizontal", "ue"),
            ("log2_max_mv_length_vertical", "ue"),
            ("max_num_reorder_frames", "ue"),
            ("max_dec_frame_buffering", "ue"),
        ):
            b = br.pos
            x = br.u1() if desc == "u1" else br.ue()
            OUT.field(br, name, x, b)


def parse_pps(br, spss):
    pps = {}
    b = br.pos
    pps["id"] = br.ue(); OUT.field(br, "pic_parameter_set_id", pps["id"], b)
    want(pps["id"] <= 255, "pic_parameter_set_id > 255", br, b)
    b = br.pos
    pps["sps_id"] = br.ue(); OUT.field(br, "seq_parameter_set_id", pps["sps_id"], b)
    sps = spss.get(pps["sps_id"])
    if sps is None:
        raise DiagError("PPS references unknown SPS %d" % pps["sps_id"], b, br)
    pps["sps"] = sps
    for name in ("entropy_coding_mode_flag", "bottom_field_pic_order_in_frame_present_flag"):
        b = br.pos
        pps[name] = br.u1(); OUT.field(br, name, pps[name], b)
    b = br.pos
    groups = br.ue(); OUT.field(br, "num_slice_groups_minus1", groups, b)
    want(groups <= 7, "num_slice_groups_minus1 > 7", br, b)
    if groups > 0:
        b = br.pos
        map_type = br.ue(); OUT.field(br, "slice_group_map_type", map_type, b)
        # Slice groups are out of the usage model; report rather than guess.
        raise DiagError("slice groups (num_slice_groups_minus1 > 0) not supported by this diagnostic", b, br)
    for name in ("num_ref_idx_l0_default_active_minus1", "num_ref_idx_l1_default_active_minus1"):
        b = br.pos
        v = br.ue(); OUT.field(br, name, v, b)
        pps[name] = v
    b = br.pos
    pps["weighted_pred_flag"] = br.u1(); OUT.field(br, "weighted_pred_flag", pps["weighted_pred_flag"], b)
    b = br.pos
    pps["weighted_bipred_idc"] = br.u(2); OUT.field(br, "weighted_bipred_idc", pps["weighted_bipred_idc"], b)
    want(pps["weighted_bipred_idc"] <= 2, "weighted_bipred_idc > 2", br, b)
    for name in ("pic_init_qp_minus26", "pic_init_qs_minus26", "chroma_qp_index_offset"):
        b = br.pos
        v = br.se(); OUT.field(br, name, v, b)
        pps[name] = v
    for name in ("deblocking_filter_control_present_flag", "constrained_intra_pred_flag", "redundant_pic_cnt_present_flag"):
        b = br.pos
        pps[name] = br.u1(); OUT.field(br, name, pps[name], b)
    if br.more_rbsp_data():
        b = br.pos
        pps["transform_8x8_mode_flag"] = br.u1(); OUT.field(br, "transform_8x8_mode_flag", pps["transform_8x8_mode_flag"], b)
        b = br.pos
        present = br.u1(); OUT.field(br, "pic_scaling_matrix_present_flag", present, b)
        if present:
            count = 6 + 2 * pps["transform_8x8_mode_flag"] if sps["chroma_format_idc"] != 3 else 6 + 6 * pps["transform_8x8_mode_flag"]
            for i in range(count):
                b = br.pos
                have = br.u1(); OUT.field(br, "pic_scaling_list_present_flag[%d]" % i, have, b)
                if have:
                    scaling_list(br, 16 if i < 6 else 64, "PPS scaling list %d" % i)
        b = br.pos
        pps["second_chroma_qp_index_offset"] = br.se(); OUT.field(br, "second_chroma_qp_index_offset", pps["second_chroma_qp_index_offset"], b)
    else:
        pps["transform_8x8_mode_flag"] = 0
        pps["second_chroma_qp_index_offset"] = pps["chroma_qp_index_offset"]
    return pps


def parse_slice_header(br, nal, spss, ppss):
    sh = {}
    b = br.pos
    sh["first_mb_in_slice"] = br.ue(); OUT.field(br, "first_mb_in_slice", sh["first_mb_in_slice"], b)
    b = br.pos
    sh["slice_type"] = br.ue(); OUT.field(br, "slice_type", sh["slice_type"], b, note="(%d mod 5)" % (sh["slice_type"] % 5))
    want(sh["slice_type"] <= 9, "impossible slice_type %d (> 9)" % sh["slice_type"], br, b)
    b = br.pos
    pps_id = br.ue(); OUT.field(br, "pic_parameter_set_id", pps_id, b)
    pps = ppss.get(pps_id)
    if pps is None:
        raise DiagError("slice references unknown PPS %d" % pps_id, b, br)
    sps = pps["sps"]
    sh["pps"] = pps
    sh["sps"] = sps
    if sps["separate_colour_plane_flag"]:
        b = br.pos
        v = br.u(2); OUT.field(br, "colour_plane_id", v, b)
    b = br.pos
    frame_num_bits = sps["log2_max_frame_num_minus4"] + 4
    v = br.u(frame_num_bits); OUT.field(br, "frame_num", v, b, note="u(%d)" % frame_num_bits)
    field_pic_flag = 0
    if not sps["frame_mbs_only_flag"]:
        b = br.pos
        field_pic_flag = br.u1(); OUT.field(br, "field_pic_flag", field_pic_flag, b)
        if field_pic_flag:
            b = br.pos
            v = br.u1(); OUT.field(br, "bottom_field_flag", v, b)
    sh["field_pic_flag"] = field_pic_flag
    if nal.type == 5:
        b = br.pos
        v = br.ue(); OUT.field(br, "idr_pic_id", v, b)
        want(v <= 65535, "idr_pic_id > 65535", br, b)
    if sps["pic_order_cnt_type"] == 0:
        bits = sps["log2_max_pic_order_cnt_lsb_minus4"] + 4
        b = br.pos
        v = br.u(bits); OUT.field(br, "pic_order_cnt_lsb", v, b, note="u(%d)" % bits)
        if pps["bottom_field_pic_order_in_frame_present_flag"] and not field_pic_flag:
            b = br.pos
            v = br.se(); OUT.field(br, "delta_pic_order_cnt_bottom", v, b)
    elif sps["pic_order_cnt_type"] == 1 and not sps.get("delta_pic_order_always_zero_flag", 0):
        b = br.pos
        v = br.se(); OUT.field(br, "delta_pic_order_cnt[0]", v, b)
        if pps["bottom_field_pic_order_in_frame_present_flag"] and not field_pic_flag:
            b = br.pos
            v = br.se(); OUT.field(br, "delta_pic_order_cnt[1]", v, b)
    if pps["redundant_pic_cnt_present_flag"]:
        b = br.pos
        v = br.ue(); OUT.field(br, "redundant_pic_cnt", v, b)
    slice_type = sh["slice_type"] % 5
    if slice_type == 1:
        b = br.pos
        v = br.u1(); OUT.field(br, "direct_spatial_mv_pred_flag", v, b)
    active0 = pps["num_ref_idx_l0_default_active_minus1"] + 1
    active1 = pps["num_ref_idx_l1_default_active_minus1"] + 1
    if slice_type in (0, 1, 3):
        b = br.pos
        override = br.u1(); OUT.field(br, "num_ref_idx_active_override_flag", override, b)
        if override:
            b = br.pos
            active0 = br.ue() + 1
            OUT.field(br, "num_ref_idx_l0_active_minus1", active0 - 1, b)
            if slice_type == 1:
                b = br.pos
                active1 = br.ue() + 1
                OUT.field(br, "num_ref_idx_l1_active_minus1", active1 - 1, b)
    # ref_pic_list_modification()
    if slice_type != 2 and slice_type != 4:
        b = br.pos
        flag = br.u1(); OUT.field(br, "ref_pic_list_modification_flag_l0", flag, b)
        if flag:
            while True:
                b = br.pos
                op = br.ue(); OUT.field(br, "modification_of_pic_nums_idc", op, b)
                if op in (0, 1):
                    b = br.pos
                    v = br.ue(); OUT.field(br, "abs_diff_pic_num_minus1", v, b)
                elif op == 2:
                    b = br.pos
                    v = br.ue(); OUT.field(br, "long_term_pic_num", v, b)
                if op == 3:
                    break
    if slice_type == 1:
        b = br.pos
        flag = br.u1(); OUT.field(br, "ref_pic_list_modification_flag_l1", flag, b)
        if flag:
            while True:
                b = br.pos
                op = br.ue(); OUT.field(br, "modification_of_pic_nums_idc_l1", op, b)
                if op in (0, 1):
                    b = br.pos
                    v = br.ue(); OUT.field(br, "abs_diff_pic_num_minus1_l1", v, b)
                elif op == 2:
                    b = br.pos
                    v = br.ue(); OUT.field(br, "long_term_pic_num_l1", v, b)
                if op == 3:
                    break
    if ((pps["weighted_pred_flag"] and slice_type in (0, 3)) or
            (pps["weighted_bipred_idc"] == 1 and slice_type == 1)):
        b = br.pos
        v = br.ue(); OUT.field(br, "luma_log2_weight_denom", v, b)
        if sps["ChromaArrayType"] != 0:
            b = br.pos
            v = br.ue(); OUT.field(br, "chroma_log2_weight_denom", v, b)
        for lst, count in (("l0", active0), ("l1", active1 if slice_type == 1 else 0)):
                for idx in range(count):
                    b = br.pos
                    have = br.u1(); OUT.field(br, "luma_weight_%s_flag[%d]" % (lst, idx), have, b)
                    if have:
                        for part in ("weight", "offset"):
                            b = br.pos
                            v = br.se(); OUT.field(br, "luma_%s_%s[%d]" % (part, lst, idx), v, b)
                    b = br.pos
                    have = br.u1(); OUT.field(br, "chroma_weight_%s_flag[%d]" % (lst, idx), have, b)
                    if have:
                        for j in range(2):
                            for part in ("weight", "offset"):
                                b = br.pos
                                v = br.se(); OUT.field(br, "chroma_%s_%s[%d][%d]" % (part, lst, idx, j), v, b)
    if nal.nal_ref_idc != 0:
        if nal.type == 5:
            for name in ("no_output_of_prior_pics_flag", "long_term_reference_flag"):
                b = br.pos
                v = br.u1(); OUT.field(br, name, v, b)
        else:
            b = br.pos
            adaptive = br.u1(); OUT.field(br, "adaptive_ref_pic_marking_mode_flag", adaptive, b)
            if adaptive:
                while True:
                    b = br.pos
                    op = br.ue(); OUT.field(br, "memory_management_control_operation", op, b)
                    if op in (1, 3):
                        b = br.pos
                        v = br.ue(); OUT.field(br, "difference_of_pic_nums_minus1", v, b)
                    if op == 2:
                        b = br.pos
                        v = br.ue(); OUT.field(br, "long_term_pic_num", v, b)
                    if op in (3, 6):
                        b = br.pos
                        v = br.ue(); OUT.field(br, "long_term_frame_idx", v, b)
                    if op == 4:
                        b = br.pos
                        v = br.ue(); OUT.field(br, "max_long_term_frame_idx_plus1", v, b)
                    if op == 0:
                        break
    if pps["entropy_coding_mode_flag"] and slice_type not in (2, 4):
        b = br.pos
        v = br.ue(); OUT.field(br, "cabac_init_idc", v, b)
        want(v <= 2, "cabac_init_idc > 2", br, b)
    b = br.pos
    sh["slice_qp_delta"] = br.se(); OUT.field(br, "slice_qp_delta", sh["slice_qp_delta"], b)
    sh["QPY"] = 26 + pps["pic_init_qp_minus26"] + sh["slice_qp_delta"]
    want(0 <= sh["QPY"] <= 51, "QPY out of range: %d" % sh["QPY"], br, b)
    if slice_type in (3, 4):
        if slice_type == 3:
            b = br.pos
            v = br.u1(); OUT.field(br, "sp_for_switch_flag", v, b)
        b = br.pos
        v = br.se(); OUT.field(br, "slice_qs_delta", v, b)
    if pps["deblocking_filter_control_present_flag"]:
        b = br.pos
        disable = br.ue(); OUT.field(br, "disable_deblocking_filter_idc", disable, b)
        want(disable <= 2, "disable_deblocking_filter_idc > 2", br, b)
        if disable != 1:
            for name in ("slice_alpha_c0_offset_div2", "slice_beta_offset_div2"):
                b = br.pos
                v = br.se(); OUT.field(br, name, v, b)
                want(-6 <= v <= 6, "%s out of range" % name, br, b)
    return sh


# ---------------------------------------------------------------------------
# Macroblock layer (CAVLC, I slices)
# ---------------------------------------------------------------------------

class MbContext:
    def __init__(self, sps, pps, slice_qpy):
        self.sps = sps
        self.pps = pps
        self.qpy = slice_qpy
        self.mb_count = 0
        # Per-MB total coefficients of 4x4 luma blocks and chroma AC blocks.
        self.luma_tc = [0] * 16
        self.chroma_tc = [[0] * 4, [0] * 4]
        self.prev_luma_tc = [0] * 16
        self.prev_chroma_tc = [[0] * 4, [0] * 4]
        self.prev_pcm = False
        self.mb_addr = 0
        # nC needs the MB to the left (addr-1) AND the MB above
        # (addr-PicWidthInMbs); a single "previous MB" cache cannot serve
        # both.  Keep the completed count arrays of every MB in the slice.
        self.tc_hist = {}

    def finish_mb(self, pcm=False, luma=None, chroma=None):
        luma = list(luma if luma is not None else self.luma_tc)
        chroma = [list(c) for c in (chroma if chroma is not None else self.chroma_tc)]
        self.prev_luma_tc = luma
        self.prev_chroma_tc = chroma
        self.tc_hist[self.mb_addr] = (luma, chroma)
        self.prev_pcm = pcm


def neighbour_nc(ctx, kind, idx, mb_addr, comp=0):
    """nC per H.264 9.2.1 / 6.4.11.4-6 for progressive 4:2:0 frames."""
    width = ctx.sps["PicWidthInMbs"]
    if kind == "chroma_ac":
        cur = ctx.chroma_tc[comp]
        side = 2
        # 4:2:0 chroma blocks are laid out 0 1 / 2 3.  The left neighbour
        # of 0 and 2 is the right column of the MB to the left (1 and 3);
        # the left neighbour of 1 and 3 is 0 and 2 of the current MB.
        left_idx = (1, 0, 3, 2)[idx]
        # Bottom-row blocks (2,3) look up to 0,1 of the current MB;
        # top-row blocks look up to 2,3 of the MB above.
        top_idx = (2, 3, 0, 1)[idx]

        def hist_block(addr, block):
            h = ctx.tc_hist.get(addr)
            return h[1][comp][block] if h is not None else 0
    else:
        cur = ctx.luma_tc
        side = 4
        row, col = divmod(idx, side)
        left_idx = (idx - 1) if col else (row * side + side - 1)
        top_idx = (idx - side) if row else ((side - 1) * side + idx)

        def hist_block(addr, block):
            h = ctx.tc_hist.get(addr)
            return h[0][block] if h is not None else 0

    n = []
    if (idx % side != 0) or (mb_addr % width != 0):
        if idx % side == 0:                      # MB to the left
            n.append(16 if ctx.prev_pcm else hist_block(mb_addr - 1, left_idx))
        else:
            n.append(cur[left_idx])
    if (idx >= side) or (mb_addr >= width):
        if idx < side:                           # MB above
            n.append(16 if ctx.prev_pcm else hist_block(mb_addr - width, top_idx))
        else:
            n.append(cur[top_idx])
    if len(n) == 2:
        return (n[0] + n[1] + 1) >> 1
    return n[0] if n else 0


def decode_residual_block(br, ctx, nc, max_num_coeff, label):
    start = br.pos
    table = nc_table(nc)
    (total, t1), token_code, after_token = read_vlc(br, table, "coeff_token(nC=%d)" % nc, start)
    OUT.line("    %-22s coeff_token=(TotalCoeff=%d,TrailingOnes=%d) code=%s @bit %d..%d"
             % (label, total, t1, token_code, start, after_token - 1))
    if max_num_coeff == 15 and total == 16:
        # A conforming bitstream cannot have TotalCoeff=16 here (9.2.1).
        # Some encoders still emit the 16-coefficient token for a 15-coeff
        # Intra16x16 AC block; report it but keep decoding so the rest of
        # the picture can be diagnosed.
        OUT.line("    %-22s WARNING: TotalCoeff=16 forbidden when maxNumCoeff=15" % label)
    elif total > max_num_coeff:
        raise DiagError("TotalCoeff=%d exceeds maxNumCoeff=%d" % (total, max_num_coeff), start, br)
    if total == 0:
        OUT.line("    %-22s empty block" % label)
        return 0
    want(t1 <= min(3, total), "TrailingOnes=%d > min(3,TotalCoeff) for %s" % (t1, label), br, start)
    levels = []
    for i in range(t1):
        b = br.pos
        sign = br.u1()
        levels.append(1 if sign == 0 else -1)
        OUT.line("    %-22s trailing_ones_sign_flag[%d]=%s @bit %d" % (label, i, "-" if sign else "+", b))
    suffix_length = 1 if (total > 10 and t1 < 3) else 0
    for i in range(t1, total):
        b = br.pos
        zeros = 0
        while True:
            if br.pos >= br.nbits or zeros > 31:
                raise DiagError("truncated/invalid level_prefix for %s" % label, b, br)
            bit = br.u1()
            if bit:
                break
            zeros += 1
        level_prefix = zeros
        if level_prefix == 14 and suffix_length == 0:
            suffix_size = 4
        elif level_prefix >= 15:
            suffix_size = level_prefix - 3
        else:
            suffix_size = suffix_length
        suffix = br.u(suffix_size)
        level_code = (min(15, level_prefix) << suffix_length) + suffix
        if level_prefix >= 15 and suffix_length == 0:
            level_code += 15
        if level_prefix >= 16:
            level_code += (1 << (level_prefix - 3)) - 4096
        if i == t1 and t1 < 3:
            level_code += 2
        level = (level_code + 2) >> 1 if level_code % 2 == 0 else (-level_code - 1) >> 1
        levels.append(level)
        OUT.line("    %-22s level[%d]=%d (prefix=%d,suffix=%d,len=%d) @bit %d..%d"
                 % (label, i, level, level_prefix, suffix, suffix_size, b, br.pos - 1))
        if suffix_length == 0:
            suffix_length = 1
        if abs(level) > (3 << (suffix_length - 1)) and suffix_length < 6:
            suffix_length += 1
    if total >= max_num_coeff:
        zeros_left = 0
        OUT.line("    %-22s total_zeros inferred 0 (TotalCoeff>=maxNumCoeff)" % label)
    else:
        b = br.pos
        zeros_table = TZC if max_num_coeff == 4 else TZ4
        if total not in zeros_table:
            raise DiagError("no total_zeros table for TotalCoeff=%d" % total, b, br)
        (zeros_left,), code, _ = read_vlc(br, zeros_table[total], "total_zeros(%d)" % total, b)
        OUT.line("    %-22s total_zeros=%d code=%s @bit %d..%d" % (label, zeros_left, code, b, br.pos - 1))
    runs = []
    for i in range(total - 1):
        if zeros_left == 0:
            runs.append(0)
            continue
        b = br.pos
        cls = min(zeros_left, 7)
        (run,), code, _ = read_vlc(br, RUN_BEFORE[cls], "run_before(zerosLeft=%d)" % zeros_left, b)
        if run > zeros_left:
            raise DiagError("run_before=%d > zerosLeft=%d for %s" % (run, zeros_left, label), b, br)
        zeros_left -= run
        runs.append(run)
        OUT.line("    %-22s run_before[%d]=%d code=%s @bit %d..%d" % (label, i, run, code, b, br.pos - 1))
    runs.append(zeros_left)
    OUT.line("    %-22s DONE total=%d levels=%s runs=%s @bit %d..%d"
             % (label, total, levels[:16], runs[:16], start, br.pos - 1))
    return total


def parse_residual(br, ctx, mb_type, cbp_luma, cbp_chroma):
    sps = ctx.sps
    chroma_type = sps["ChromaArrayType"]
    if mb_type != 0:  # Intra_16x16: DC block first, then 15 AC blocks
        for blk in (0,):
            nc = neighbour_nc(ctx, "luma", blk, ctx.mb_addr)
            tc = decode_residual_block(br, ctx, nc, 16, "Intra16x16DC")
        for blk in range(1, 16):
            if not ((cbp_luma >> (blk // 4)) & 1):
                ctx.luma_tc[blk] = 0
                continue
            nc = neighbour_nc(ctx, "luma", blk, ctx.mb_addr)
            ctx.luma_tc[blk] = decode_residual_block(br, ctx, nc, 15, "Intra16x16AC[%d]" % blk)
        return
    for blk in range(16):
        if not ((cbp_luma >> (blk // 4)) & 1):
            ctx.luma_tc[blk] = 0
            continue
        nc = neighbour_nc(ctx, "luma", blk, ctx.mb_addr)
        ctx.luma_tc[blk] = decode_residual_block(br, ctx, nc, 16, "Luma4x4[%d]" % blk)
    if chroma_type == 0:
        return
    # Chroma DC is present whenever CodedBlockPatternChroma is non-zero
    # (values 1, 2 and 3), not just when its low bit is set.
    if cbp_chroma != 0:
        for comp in (0, 1):
            decode_residual_block(br, ctx, -1 if chroma_type == 1 else -2, 4,
                                  "%sDC" % ("Cb" if comp == 0 else "Cr"))
    if cbp_chroma & 2:
        for comp in (0, 1):
            for blk in range(4):
                nc = neighbour_nc(ctx, "chroma_ac", blk, ctx.mb_addr, comp)
                ctx.chroma_tc[comp][blk] = decode_residual_block(
                    br, ctx, nc, 15, "%sAC[%d]" % ("Cb" if comp == 0 else "Cr", blk))


def parse_macroblock(br, ctx):
    start = br.pos
    mb_addr = ctx.mb_addr
    OUT.line("  MB %d (addr %d):" % (ctx.mb_count, mb_addr))
    b = br.pos
    mb_type = br.ue()
    OUT.field(br, "mb_type", mb_type, b, note="I slice: 0=I_NxN, 1..24=I_16x16, 25=I_PCM")
    want(mb_type <= 25, "impossible mb_type %d in I slice (valid 0..25)" % mb_type, br, b)
    if mb_type == 25:  # I_PCM
        align = br.align_byte("pcm_alignment_zero_bit")
        OUT.line("    I_PCM alignment @bit %d..%d" % (align, br.pos - 1))
        sample_bits = (256 * (8 + ctx.sps["bit_depth_luma_minus8"]) +
                       2 * 8 * 8 * (8 + ctx.sps["bit_depth_chroma_minus8"]))
        if br.pos + sample_bits > br.nbits:
            raise DiagError("truncated I_PCM samples", br.pos, br)
        OUT.line("    I_PCM samples: %d bits @bit %d..%d (file byte %s)"
                 % (sample_bits, br.pos, br.pos + sample_bits - 1,
                    br.byte_map[br.pos // 8] if br.pos // 8 < len(br.byte_map) else "?"))
        br.pos += sample_bits
        ctx.finish_mb(pcm=True)
        ctx.mb_count += 1
        ctx.mb_addr += 1
        return
    if mb_type == 0:  # I_NxN
        if ctx.pps["transform_8x8_mode_flag"]:
            b = br.pos
            flag = br.u1()
            OUT.field(br, "transform_size_8x8_flag", flag, b)
            if flag:
                raise DiagError("Intra_8x8 (transform_size_8x8_flag=1) not supported by this diagnostic", b, br)
        b = br.pos
        modes = []
        for blk in range(16):
            prev = br.u1()
            if prev:
                # Mode is inferred from neighbouring samples (8.3.1); this
                # syntax-only reader cannot derive its numeric value.
                modes.append("P")
            else:
                rem = br.u(3)
                if rem >= 8:
                    raise DiagError("rem_intra4x4_pred_mode >= 8", br.pos - 3, br)
                modes.append(str(rem))
        OUT.line("    intra4x4 pred modes[16]=%s @bit %d..%d"
                 % (",".join(modes), b, br.pos - 1))
        if ctx.sps["ChromaArrayType"] != 0:
            b = br.pos
            chroma_mode = br.ue()
            OUT.field(br, "intra_chroma_pred_mode", chroma_mode, b)
            want(chroma_mode <= 3, "intra_chroma_pred_mode > 3", br, b)
        b = br.pos
        code_num = br.ue()  # me(v): Exp-Golomb codeNum, then Table 9-4 mapping
        if code_num >= len(CBP_I):
            raise DiagError("coded_block_pattern codeNum=%d out of range" % code_num, b, br)
        cbp = CBP_I[code_num]
        code = "".join(str((br.data[p >> 3] >> (7 - (p & 7))) & 1) for p in range(b, br.pos))
        OUT.line("    coded_block_pattern=%d (codeNum=%d, me code=%s) @bit %d..%d"
                 % (cbp, code_num, code, b, br.pos - 1))
        cbp_luma = cbp & 15
        cbp_chroma = cbp >> 4
        OUT.line("    CodedBlockPatternLuma=%d CodedBlockPatternChroma=%d" % (cbp_luma, cbp_chroma))
    else:  # I_16x16
        idx = mb_type - 1
        pred_mode = idx % 4
        cbp_chroma = (idx // 4) % 3
        cbp_luma = 15 if (idx // 4) >= 3 else 0
        OUT.line("    Intra16x16PredMode=%d CodedBlockPatternLuma=%d CodedBlockPatternChroma=%d (inferred from mb_type)"
                 % (pred_mode, cbp_luma, cbp_chroma))
    # 7.3.5: mb_qp_delta and residual() are also present for every
    # Intra_16x16 macroblock, even when both CBP fields are zero.
    if cbp_luma or cbp_chroma or mb_type != 0:
        b = br.pos
        delta = br.se()
        OUT.field(br, "mb_qp_delta", delta, b)
        new_qpy = (ctx.qpy + delta + 52) % 52
        if not 0 <= new_qpy <= 51:
            raise DiagError("QPY out of range after mb_qp_delta: %d" % new_qpy, b, br)
        ctx.qpy = new_qpy
        OUT.line("    QPY=%d" % new_qpy)
        parse_residual(br, ctx, mb_type, cbp_luma, cbp_chroma)
    ctx.finish_mb(pcm=False)
    ctx.mb_count += 1
    ctx.mb_addr += 1


def parse_slice_data(br, nal, sh, max_mbs, ctx):
    pps = sh["pps"]
    if pps["entropy_coding_mode_flag"]:
        OUT.line("  [entropy_coding_mode_flag=1: CAVLC macroblock decode not applicable; stopping]")
        return
    slice_type = sh["slice_type"] % 5
    if slice_type not in (2, 4):
        OUT.line("  [slice_type=%d (P/B/SP): only I-slice macroblock layer is decoded; stopping]" % slice_type)
        return
    if max_mbs is not None and ctx.mb_count >= max_mbs:
        return
    ctx.mb_addr = sh["first_mb_in_slice"]
    while br.more_rbsp_data():
        if max_mbs is not None and ctx.mb_count >= max_mbs:
            OUT.line("  [--max-mbs %d reached; stopping]" % max_mbs)
            break
        try:
            parse_macroblock(br, ctx)
        except DiagError:
            raise
    OUT.line("  slice data: %d macroblock(s) decoded, %d bits consumed" % (ctx.mb_count, br.pos))


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------

def diagnose(path, max_mbs=None):
    with open(path, "rb") as f:
        data = f.read()
    OUT.line("FILE %s (%d bytes)" % (path, len(data)))
    nals = split_annex_b(data)
    OUT.line("NAL units: %d" % len(nals))
    spss, ppss = {}, {}
    errors = []
    mb_total = 0
    for nal in nals:
        OUT.line("")
        OUT.line("NAL %d: type=%d nal_ref_idc=%d start_code_byte=%d payload_bytes=%d"
                 % (nal.index, nal.type, nal.nal_ref_idc, nal.sc_offset, len(nal.ebsp)))
        br = BitReader(nal.rbsp, nal.byte_map)
        try:
            if nal.type == 7:
                sps = parse_sps(br)
                spss[sps["id"]] = sps
                OUT.line("  SPS parsed: %d bits consumed" % br.pos)
            elif nal.type == 8:
                pps = parse_pps(br, spss)
                ppss[pps["id"]] = pps
                OUT.line("  PPS parsed: %d bits consumed" % br.pos)
            elif nal.type in (1, 5):
                sh = parse_slice_header(br, nal, spss, ppss)
                OUT.line("  slice header parsed: %d bits consumed" % br.pos)
                ctx = MbContext(sh["sps"], sh["pps"], sh["QPY"])
                parse_slice_data(br, nal, sh, max_mbs, ctx)
                mb_total += ctx.mb_count
            else:
                OUT.line("  [NAL type %d skipped]" % nal.type)
        except DiagError as e:
            errors.append(e)
            OUT.line("")
            OUT.line("ERROR: %s" % e.message)
            OUT.line("       %s" % e.location())
            if nal.type in (1, 5):
                # Keep scanning subsequent parameter sets / NAL units.
                continue
    OUT.line("")
    OUT.line("SUMMARY: NALs=%d SPS=%d PPS=%d macroblocks=%d errors=%d"
             % (len(nals), len(spss), len(ppss), mb_total, len(errors)))
    if errors:
        OUT.line("FIRST ERROR: %s -- %s" % (errors[0].message, errors[0].location()))
    return errors


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("file", help="Annex-B H.264 (.264/.h264) file")
    ap.add_argument("--max-mbs", type=int, default=None, metavar="N",
                    help="decode at most N macroblocks in total (0 = headers only)")
    args = ap.parse_args(argv)
    errors = diagnose(args.file, args.max_mbs)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
