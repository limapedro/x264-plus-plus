; x264++ — original AVX2 distortion metrics (Win64 NASM)
;
; Public prototypes (src/x86/kernels.h):
;   int x264pp_sad{4x4,8x8,16x16}_avx2 (const uint8_t *a, intptr_t sa,
;                                       const uint8_t *b, intptr_t sb);
;   int x264pp_satd{4x4,8x8,16x16}_avx2(...)
;
; Behaviour from docs/rebuild/common/x86/sad-a.asm.md and pixel.c.md §5.3:
;   SAD  = sum |a-b| over the block (psadbw on unsigned bytes).
;   SATD = sum of absolute 4x4 Hadamard coefficients of the residual, with
;          the 1/2 metric scale applied per 8x4 group (two 4x4 blocks),
;          exactly as described for PIXEL_SATD_C in pixel.c.md.
;
; Win64: RCX=a, RDX=sa, R8=b, R9=sb.  Only volatile GPRs and XMM0..XMM7 are
; used; no callee-saved state is touched.

default rel

section .rodata align=16
align 16
pw_ones: times 8 dw 1

section .text

; ---------------------------------------------------------------------------
; reduce_xmm_qwords: XMM0 (two 64-bit lane sums) -> EAX, zero-extended.
; ---------------------------------------------------------------------------
%macro REDUCE_XMM 0
    vpsrldq   xmm1, xmm0, 8
    vpaddq    xmm0, xmm0, xmm1
    vmovd     eax, xmm0
%endmacro

; ---------------------------------------------------------------------------
; reduce_ymm_dwords: YMM0 -> EAX (sum of four 64-bit lanes).
; ---------------------------------------------------------------------------
%macro REDUCE_YMM 0
    vextracti128 xmm1, ymm0, 1
    vpaddq    xmm0, xmm0, xmm1
    REDUCE_XMM
%endmacro

; ---------------------------------------------------------------------------
; SAD 4x4 — two rows per psadbw (vpinsrd builds an 8-byte row pair).
; ---------------------------------------------------------------------------
global x264pp_sad4x4_avx2
x264pp_sad4x4_avx2:
    mov       rax, rcx
    mov       r10, r8
    vpxor     xmm0, xmm0, xmm0
    mov       ecx, 2
.sad4_loop:
    vmovd     xmm1, dword [rax]
    vpinsrd   xmm1, xmm1, dword [rax+rdx], 1
    vmovd     xmm2, dword [r10]
    vpinsrd   xmm2, xmm2, dword [r10+r9], 1
    vpsadbw   xmm1, xmm1, xmm2
    vpaddq    xmm0, xmm0, xmm1
    lea       rax, [rax+2*rdx]
    lea       r10, [r10+2*r9]
    dec       ecx
    jnz       .sad4_loop
    REDUCE_XMM
    ret

; ---------------------------------------------------------------------------
; SAD 8x8 — two 8-byte rows per psadbw, four iterations.
; ---------------------------------------------------------------------------
global x264pp_sad8x8_avx2
x264pp_sad8x8_avx2:
    mov       rax, rcx
    mov       r10, r8
    vpxor     xmm0, xmm0, xmm0
    mov       ecx, 4
.sad8_loop:
    vmovq     xmm1, qword [rax]
    vpinsrq   xmm1, xmm1, qword [rax+rdx], 1
    vmovq     xmm2, qword [r10]
    vpinsrq   xmm2, xmm2, qword [r10+r9], 1
    vpsadbw   xmm1, xmm1, xmm2
    vpaddq    xmm0, xmm0, xmm1
    lea       rax, [rax+2*rdx]
    lea       r10, [r10+2*r9]
    dec       ecx
    jnz       .sad8_loop
    REDUCE_XMM
    ret

; ---------------------------------------------------------------------------
; SAD 16x16 — one 32-byte row per psadbw, YMM accumulator.
; ---------------------------------------------------------------------------
global x264pp_sad16x16_avx2
x264pp_sad16x16_avx2:
    mov       rax, rcx
    mov       r10, r8
    vpxor     ymm0, ymm0, ymm0
    mov       ecx, 8                         ; 32-byte load covers two 16-pixel rows
.sad16_loop:
    ; Build two 16-byte rows per YMM without assuming stride == 16.
    vmovdqu   xmm1, [rax]
    vmovdqu   xmm2, [rax+rdx]
    vinserti128 ymm1, ymm1, xmm2, 1
    vmovdqu   xmm2, [r10]
    vmovdqu   xmm3, [r10+r9]
    vinserti128 ymm2, ymm2, xmm3, 1
    vpsadbw   ymm1, ymm1, ymm2
    vpaddq    ymm0, ymm0, ymm1
    lea       rax, [rax+2*rdx]
    lea       r10, [r10+2*r9]
    dec       ecx
    jnz       .sad16_loop
    REDUCE_YMM
    vzeroupper
    ret

; ---------------------------------------------------------------------------
; satd4_raw: RCX=a, RDX=sa, R8=b, R9=sb -> EAX = Σ|H4(a-b)| (unscaled).
; The 4x4 Hadamard is two 4-point butterflies separated by a word transpose;
; all intermediate values fit in signed 16-bit lanes for 8-bit input.
; ---------------------------------------------------------------------------
satd4_raw:
    sub       rsp, 72                     ; shadow + XMM6/XMM7 save, 16-byte aligned
    movdqu    [rsp+32], xmm6
    movdqu    [rsp+48], xmm7
    lea       r10, [rdx+2*rdx]            ; 3*sa
    lea       r11, [r9+2*r9]              ; 3*sb
    vmovd     xmm0, dword [rcx]           ; row 0
    vpmovzxbw xmm0, xmm0
    vmovd     xmm4, dword [r8]
    vpmovzxbw xmm4, xmm4
    vpsubw    xmm0, xmm0, xmm4
    vmovd     xmm1, dword [rcx+rdx]
    vpmovzxbw xmm1, xmm1
    vmovd     xmm4, dword [r8+r9]
    vpmovzxbw xmm4, xmm4
    vpsubw    xmm1, xmm1, xmm4
    vmovd     xmm2, dword [rcx+2*rdx]
    vpmovzxbw xmm2, xmm2
    vmovd     xmm4, dword [r8+2*r9]
    vpmovzxbw xmm4, xmm4
    vpsubw    xmm2, xmm2, xmm4
    vmovd     xmm3, dword [rcx+r10]
    vpmovzxbw xmm3, xmm3
    vmovd     xmm4, dword [r8+r11]
    vpmovzxbw xmm4, xmm4
    vpsubw    xmm3, xmm3, xmm4

    ; First 4-point butterfly across the four row registers.
    vmovdqa   xmm4, xmm0
    vpaddw    xmm4, xmm4, xmm3            ; s0 = r0+r3
    vpsubw    xmm0, xmm0, xmm3            ; d0 = r0-r3
    vmovdqa   xmm5, xmm1
    vpaddw    xmm5, xmm5, xmm2            ; s1 = r1+r2
    vpsubw    xmm1, xmm1, xmm2            ; d1 = r1-r2
    vmovdqa   xmm2, xmm4
    vpaddw    xmm2, xmm2, xmm5            ; t0 = s0+s1
    vpsubw    xmm4, xmm4, xmm5            ; t2 = s0-s1
    vmovdqa   xmm3, xmm0
    vpaddw    xmm3, xmm3, xmm1            ; t1 = d0+d1
    vpsubw    xmm0, xmm0, xmm1            ; t3 = d0-d1

    ; 4x4 word transpose: rows xmm2,xmm3,xmm4,xmm0 -> columns xmm7,xmm5,xmm2,xmm6
    vmovdqa   xmm5, xmm2
    vpunpcklwd xmm5, xmm5, xmm3           ; A_lo = [t0_0,t1_0,t0_1,t1_1]
    vmovdqa   xmm6, xmm4
    vpunpcklwd xmm6, xmm6, xmm0           ; B_lo = [t2_0,t3_0,t2_1,t3_1]
    vmovdqa   xmm7, xmm5
    vpunpckldq xmm7, xmm7, xmm6           ; column 0
    vpunpckhdq xmm5, xmm5, xmm6           ; column 1
    ; The high halves of the interleaved registers hold columns 1 and 3.
    vpsrldq   xmm6, xmm5, 8               ; column 3
    vmovq     xmm5, xmm5                  ; column 2
    vpsrldq   xmm4, xmm7, 8               ; column 1
    vmovq     xmm7, xmm7                  ; column 0
    vmovdqa   xmm2, xmm5                  ; column 2
    vmovdqa   xmm5, xmm4                  ; column 1

    ; Second 4-point butterfly across the column registers.
    vmovdqa   xmm3, xmm7
    vpaddw    xmm3, xmm3, xmm6            ; s0
    vpsubw    xmm7, xmm7, xmm6            ; d0
    vmovdqa   xmm4, xmm5
    vpaddw    xmm4, xmm4, xmm2            ; s1
    vpsubw    xmm5, xmm5, xmm2            ; d1
    vmovdqa   xmm2, xmm3
    vpaddw    xmm2, xmm2, xmm4            ; c0
    vpsubw    xmm3, xmm3, xmm4            ; c2
    vmovdqa   xmm0, xmm7
    vpaddw    xmm0, xmm0, xmm5            ; c1
    vpsubw    xmm7, xmm7, xmm5            ; c3

    vpabsw    xmm2, xmm2
    vpabsw    xmm0, xmm0
    vpabsw    xmm3, xmm3
    vpabsw    xmm7, xmm7
    vmovq     xmm2, xmm2                     ; discard duplicated high qwords
    vmovq     xmm0, xmm0
    vmovq     xmm3, xmm3
    vmovq     xmm7, xmm7
    vpmaddwd  xmm2, xmm2, [pw_ones]
    vpmaddwd  xmm0, xmm0, [pw_ones]
    vpmaddwd  xmm3, xmm3, [pw_ones]
    vpmaddwd  xmm7, xmm7, [pw_ones]
    vpaddd    xmm2, xmm2, xmm0
    vpaddd    xmm2, xmm2, xmm3
    vpaddd    xmm2, xmm2, xmm7
    vpsrldq   xmm0, xmm2, 8
    vpaddd    xmm2, xmm2, xmm0
    vpsrldq   xmm0, xmm2, 4
    vpaddd    xmm2, xmm2, xmm0
    vmovd     eax, xmm2
    movdqu    xmm6, [rsp+32]
    movdqu    xmm7, [rsp+48]
    add       rsp, 72
    ret

; ---------------------------------------------------------------------------
; satd8x4_half: sum two adjacent 4x4 blocks, then >>1 (8x4 SATD).
; ---------------------------------------------------------------------------
satd8x4_half:
    sub       rsp, 40
    mov       [rsp], rcx
    mov       [rsp+8], rdx
    mov       [rsp+16], r8
    mov       [rsp+24], r9

    mov       rcx, [rsp]
    mov       rdx, [rsp+8]
    mov       r8, [rsp+16]
    mov       r9, [rsp+24]
    call      satd4_raw
    mov       [rsp+32], eax                 ; r10 is volatile across the call

    mov       rcx, [rsp]
    add       rcx, 4
    mov       r8, [rsp+16]
    add       r8, 4
    mov       rdx, [rsp+8]
    mov       r9, [rsp+24]
    call      satd4_raw
    add       eax, [rsp+32]
    sar       eax, 1
    add       rsp, 40
    ret

; ---------------------------------------------------------------------------
; satd8x8_total: top 8x4 + bottom 8x4.
; ---------------------------------------------------------------------------
satd8x8_total:
    sub       rsp, 40
    mov       [rsp], rcx
    mov       [rsp+8], rdx
    mov       [rsp+16], r8
    mov       [rsp+24], r9

    mov       rcx, [rsp]
    mov       rdx, [rsp+8]
    mov       r8, [rsp+16]
    mov       r9, [rsp+24]
    call      satd8x4_half
    mov       [rsp+32], eax

    mov       rcx, [rsp]
    mov       rdx, [rsp+8]
    lea       rcx, [rcx+rdx*4]
    mov       r8, [rsp+16]
    mov       r9, [rsp+24]
    lea       r8, [r8+r9*4]
    call      satd8x4_half
    add       eax, [rsp+32]
    add       rsp, 40
    ret

; ---------------------------------------------------------------------------
; SATD public entry points.
; ---------------------------------------------------------------------------
global x264pp_satd4x4_avx2
x264pp_satd4x4_avx2:
    sub       rsp, 40
    call      satd4_raw
    sar       eax, 1
    add       rsp, 40
    ret

global x264pp_satd8x8_avx2
x264pp_satd8x8_avx2:
    sub       rsp, 40
    mov       [rsp], rcx
    mov       [rsp+8], rdx
    mov       [rsp+16], r8
    mov       [rsp+24], r9
    mov       rcx, [rsp]
    mov       rdx, [rsp+8]
    mov       r8, [rsp+16]
    mov       r9, [rsp+24]
    call      satd8x8_total
    add       rsp, 40
    ret

global x264pp_satd16x16_avx2
x264pp_satd16x16_avx2:
    sub       rsp, 40
    mov       [rsp], rcx
    mov       [rsp+8], rdx
    mov       [rsp+16], r8
    mov       [rsp+24], r9
    mov       dword [rsp+32], 0             ; accumulator (callee may clobber r10)

    ; (0,0) and (8,0)
    mov       rcx, [rsp]
    mov       rdx, [rsp+8]
    mov       r8, [rsp+16]
    mov       r9, [rsp+24]
    call      satd8x8_total
    add       [rsp+32], eax
    mov       rcx, [rsp]
    add       rcx, 8
    mov       rdx, [rsp+8]
    mov       r8, [rsp+16]
    add       r8, 8
    mov       r9, [rsp+24]
    call      satd8x8_total
    add       [rsp+32], eax

    ; (0,8) and (8,8)
    mov       rcx, [rsp]
    mov       rdx, [rsp+8]
    lea       rcx, [rcx+rdx*8]
    mov       r8, [rsp+16]
    mov       r9, [rsp+24]
    lea       r8, [r8+r9*8]
    call      satd8x8_total
    add       [rsp+32], eax
    mov       rcx, [rsp]
    mov       rdx, [rsp+8]
    lea       rcx, [rcx+rdx*8]
    add       rcx, 8
    mov       r8, [rsp+16]
    mov       r9, [rsp+24]
    lea       r8, [r8+r9*8]
    add       r8, 8
    call      satd8x8_total
    add       eax, [rsp+32]

    add       rsp, 40
    ret
