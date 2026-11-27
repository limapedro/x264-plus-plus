; x264++ — original AVX2 4x4 integer transforms (Win64 NASM)
;
;   void x264pp_dct4x4_avx2     (const int16_t *src, int16_t *dst);
;   void x264pp_idct4x4_avx2    (const int16_t *src, int16_t *dst);
;   void x264pp_hadamard4x4_avx2(const int16_t *src, int16_t *dst);
;
; Behaviour is the H.264 clause 8.5 separable transform implemented by
; src/core/dct.c: 16 signed words in raster order -> 16 signed words out.
; No pixel add/clip is performed by these pure transform kernels.
;
; All temporaries are XMM4..XMM9; XMM6..XMM9 are saved/restored per Win64.
; The low four word lanes of XMM0..XMM3 hold the four rows of the block.

default rel

section .rodata align=16
align 16
pw_one: times 8 dw 1
pw_32:  times 8 dw 32

section .text

; ---------------------------------------------------------------------------
; Forward 1-D butterfly on one row register:
;   [a,b,c,d] -> [p+q, 2r+s, p-q, r-2s]
;   p=a+d, q=b+c, r=a-d, s=b-c   (H.264 8.5.1)
; ---------------------------------------------------------------------------
%macro FWD1_ROW 1
    vpshuflw  xmm4, %1, 01Bh                 ; reversed [d,c,b,a]
    vpaddw    xmm5, %1, xmm4                 ; [p0,q0,q0,p0]
    vpsubw    xmm6, %1, xmm4                 ; [r0,s0,-s0,-r0]
    vpshuflw  xmm7, xmm5, 04Eh               ; swapped [q0,p0,p0,q0]
    vpaddw    xmm4, xmm5, xmm7               ; out0 = p0+q0 (broadcast)
    vpsubw    xmm5, xmm5, xmm7               ; [p-q, -(p-q), ...]
    vpshuflw  xmm5, xmm5, 000h               ; out2 = p-q (broadcast)
    vpshuflw  xmm8, xmm6, 000h               ; r (broadcast)
    vpshuflw  xmm6, xmm6, 055h               ; s (broadcast)
    vpaddw    xmm9, xmm8, xmm8
    vpaddw    xmm9, xmm9, xmm6               ; out1 = 2r+s
    vpaddw    xmm6, xmm6, xmm6
    vpsubw    xmm6, xmm8, xmm6               ; out3 = r-2s
    vpblendw  %1, %1, xmm4, 001h
    vpblendw  %1, %1, xmm9, 002h
    vpblendw  %1, %1, xmm5, 004h
    vpblendw  %1, %1, xmm6, 008h
    vmovq     %1, %1                         ; discard high 64 bits
%endmacro

; ---------------------------------------------------------------------------
; Inverse 1-D butterfly on one row register:
;   u=a+c, v=a-c, w=b+(d>>1), z=(b>>1)-d
;   [a,b,c,d] -> [u+w, v+z, v-z, u-w]  (H.264 8.5.2)
; ---------------------------------------------------------------------------
%macro INV1_ROW 1
    vpshuflw  xmm4, %1, 04Eh                 ; [c,d,a,b]
    vpaddw    xmm5, %1, xmm4                 ; [a+c,b+d,c+a,d+b]
    vpsubw    xmm6, %1, xmm4                 ; [a-c,b-d,c-a,d-b]
    vpshuflw  xmm5, xmm5, 000h               ; u
    vpshuflw  xmm6, xmm6, 000h               ; v
    vpshuflw  xmm4, %1, 055h                 ; b (broadcast)
    vpshuflw  xmm7, %1, 0FFh                 ; d (broadcast)
    vpsraw    xmm8, xmm7, 1                  ; d>>1
    vpaddw    xmm8, xmm4, xmm8               ; w = b + (d>>1)
    vpsraw    xmm4, xmm4, 1                  ; b>>1
    vpsubw    xmm9, xmm4, xmm7               ; z = (b>>1) - d
    vpaddw    xmm4, xmm5, xmm8               ; out0 = u+w
    vpaddw    xmm7, xmm6, xmm9               ; out1 = v+z
    vpblendw  %1, %1, xmm4, 001h
    vpblendw  %1, %1, xmm7, 002h
    vpsubw    xmm4, xmm6, xmm9               ; out2 = v-z
    vpsubw    xmm7, xmm5, xmm8               ; out3 = u-w
    vpblendw  %1, %1, xmm4, 004h
    vpblendw  %1, %1, xmm7, 008h
    vmovq     %1, %1
%endmacro

; ---------------------------------------------------------------------------
; Unscaled Hadamard 1-D butterfly:
;   [a,b,c,d] -> [(a+d)+(b+c), (a+c)-(b+d), (a+d)-(b+c), (a-c)-(b-d)]
; ---------------------------------------------------------------------------
%macro HAD1_ROW 1
    ; (s0,s1,d0,d1) = (x0+x3, x1+x2, x0-x3, x1-x2)
    ; out = [s0+s1, d0+d1, s0-s1, d0-d1]  (Walsh-Hadamard 4 point)
    vpshuflw  xmm4, %1, 01Bh                 ; [x3,x2,x1,x0]
    vpaddw    xmm6, %1, xmm4                 ; s = [s0,s1,s1,s0]
    vpsubw    xmm7, %1, xmm4                 ; d = [d0,d1,-d1,-d0]
    vpshuflw  xmm4, xmm6, 000h               ; s0
    vpshuflw  xmm5, xmm6, 055h               ; s1
    vpaddw    xmm6, xmm4, xmm5               ; out0
    vpsubw    xmm8, xmm4, xmm5               ; out2
    vpshuflw  xmm4, xmm7, 000h               ; d0
    vpshuflw  xmm5, xmm7, 055h               ; d1
    vpaddw    xmm7, xmm4, xmm5               ; out1
    vpsubw    xmm9, xmm4, xmm5               ; out3
    vpblendw  %1, %1, xmm6, 001h               ; out0 = s0+s1
    vpblendw  %1, %1, xmm8, 002h               ; out1 = s0-s1
    vpblendw  %1, %1, xmm7, 004h               ; out2 = d0+d1
    vpblendw  %1, %1, xmm9, 008h               ; out3 = d0-d1
    vmovq     %1, %1
%endmacro

; ---------------------------------------------------------------------------
; 4x4 word transpose: XMM0..XMM3 (rows) -> XMM0..XMM3 (columns).
; Uses XMM4..XMM7 as scratch.
; ---------------------------------------------------------------------------
%macro TRANSPOSE4 0
    ; Each row register carries four words in its low qword (upper qword is
    ; zero).  Interleave rows 0/1 and 2/3 as word pairs, then extract the
    ; four columns from the two resulting dword pairs.
    vmovdqa   xmm4, xmm0
    vpunpcklwd xmm4, xmm4, xmm1             ; A = [a0,b0,a1,b1,a2,b2,a3,b3]
    vmovdqa   xmm6, xmm2
    vpunpcklwd xmm6, xmm6, xmm3             ; B = [c0,d0,c1,d1,c2,d2,c3,d3]

    vmovdqa   xmm0, xmm4
    vpunpckldq xmm0, xmm0, xmm6             ; low qword = column 0, high = 1
    vmovdqa   xmm7, xmm0
    vpsrldq   xmm7, xmm7, 8
    vmovq     xmm0, xmm0                    ; column 0
    vmovdqa   xmm1, xmm7                    ; column 1

    vmovdqa   xmm2, xmm4
    vpunpckhdq xmm2, xmm2, xmm6             ; low qword = column 2, high = 3
    vmovdqa   xmm7, xmm2
    vpsrldq   xmm7, xmm7, 8
    vmovq     xmm2, xmm2                    ; column 2
    vmovdqa   xmm3, xmm7                    ; column 3
%endmacro

%macro DCT_PROLOGUE 0
    sub       rsp, 120
    movdqu    [rsp+32], xmm6
    movdqu    [rsp+48], xmm7
    movdqu    [rsp+64], xmm8
    movdqu    [rsp+80], xmm9
%endmacro

%macro DCT_EPILOGUE 0
    movdqu    xmm6, [rsp+32]
    movdqu    xmm7, [rsp+48]
    movdqu    xmm8, [rsp+64]
    movdqu    xmm9, [rsp+80]
    add       rsp, 120
    ret
%endmacro

; ---------------------------------------------------------------------------
; Forward 4x4 DCT.
; ---------------------------------------------------------------------------
global x264pp_dct4x4_avx2
x264pp_dct4x4_avx2:
    DCT_PROLOGUE
    vmovq     xmm0, qword [rcx]
    vmovq     xmm1, qword [rcx+8]
    vmovq     xmm2, qword [rcx+16]
    vmovq     xmm3, qword [rcx+24]
    FWD1_ROW  xmm0
    FWD1_ROW  xmm1
    FWD1_ROW  xmm2
    FWD1_ROW  xmm3
    TRANSPOSE4
    FWD1_ROW  xmm0
    FWD1_ROW  xmm1
    FWD1_ROW  xmm2
    FWD1_ROW  xmm3
    TRANSPOSE4
    vmovq     qword [rdx], xmm0
    vmovq     qword [rdx+8], xmm1
    vmovq     qword [rdx+16], xmm2
    vmovq     qword [rdx+24], xmm3
    DCT_EPILOGUE

; ---------------------------------------------------------------------------
; Inverse 4x4 DCT (+32, >>6; no destination read or clipping).
; ---------------------------------------------------------------------------
global x264pp_idct4x4_avx2
x264pp_idct4x4_avx2:
    ; Exact integer semantics of src/core/dct.c inv1() are kept in GPRs;
    ; AVX2 moves the whole block in and out.  This avoids any ambiguity in
    ; the rounding of the two >>1 stages for negative coefficients.
    sub       rsp, 136
    mov       [rsp+128], rdx                 ; keep dst across GPR temporaries
    vmovdqu   ymm0, [rcx]
    vmovdqu   [rsp+32], ymm0                 ; src[16]

    xor       r10d, r10d                     ; byte offset of column i
.idct_cols:
    movsx     eax, word [rsp+32+r10]         ; a
    movsx     ecx, word [rsp+40+r10]         ; b
    movsx     edx, word [rsp+48+r10]         ; c
    movsx     r8d, word [rsp+56+r10]         ; d
    lea       r9d, [rax+rdx]                 ; u
    sub       eax, edx                       ; v
    mov       edx, ecx
    sar       edx, 1                         ; b>>1
    mov       r11d, r8d
    sar       r11d, 1                        ; d>>1
    add       ecx, r11d                      ; w
    sub       edx, r8d                       ; z
    lea       r11d, [r9+rcx]
    mov       [rsp+64+r10], r11w             ; tmp[0*4+i]
    lea       r11d, [rax+rdx]
    mov       [rsp+72+r10], r11w             ; tmp[1*4+i]
    sub       eax, edx
    mov       [rsp+80+r10], ax               ; tmp[2*4+i]
    sub       r9d, ecx
    mov       [rsp+88+r10], r9w              ; tmp[3*4+i]
    add       r10d, 2
    cmp       r10d, 8
    jl        .idct_cols

    xor       r10d, r10d                     ; byte offset of row y
.idct_rows:
    movsx     eax, word [rsp+64+r10]         ; a
    movsx     ecx, word [rsp+66+r10]         ; b
    movsx     edx, word [rsp+68+r10]         ; c
    movsx     r8d, word [rsp+70+r10]         ; d
    lea       r9d, [rax+rdx]                 ; u
    sub       eax, edx                       ; v
    mov       edx, ecx
    sar       edx, 1
    mov       r11d, r8d
    sar       r11d, 1
    add       ecx, r11d                      ; w
    sub       edx, r8d                       ; z
    lea       r11d, [r9+rcx]
    add       r11d, 32
    sar       r11d, 6
    mov       [rsp+96+r10], r11w
    lea       r11d, [rax+rdx]
    add       r11d, 32
    sar       r11d, 6
    mov       [rsp+98+r10], r11w
    sub       eax, edx
    add       eax, 32
    sar       eax, 6
    mov       [rsp+100+r10], ax
    sub       r9d, ecx
    add       r9d, 32
    sar       r9d, 6
    mov       [rsp+102+r10], r9w
    add       r10d, 8
    cmp       r10d, 32
    jl        .idct_rows

    vmovdqu   ymm4, [rsp+96]
    mov       rdx, [rsp+128]
    vmovdqu   [rdx], ymm4
    vzeroupper
    add       rsp, 136
    ret

; ---------------------------------------------------------------------------
; 4x4 Hadamard: two unscaled butterflies with (v+1)>>1 on the second pass.
; Matches x264pp_hadamard4() in src/core/dct.c.
; ---------------------------------------------------------------------------
global x264pp_hadamard4x4_avx2
x264pp_hadamard4x4_avx2:
    DCT_PROLOGUE
    vmovq     xmm0, qword [rcx]
    vmovq     xmm1, qword [rcx+8]
    vmovq     xmm2, qword [rcx+16]
    vmovq     xmm3, qword [rcx+24]
    HAD1_ROW  xmm0
    HAD1_ROW  xmm1
    HAD1_ROW  xmm2
    HAD1_ROW  xmm3
    TRANSPOSE4
    HAD1_ROW  xmm0
    HAD1_ROW  xmm1
    HAD1_ROW  xmm2
    HAD1_ROW  xmm3
    vpaddw    xmm0, xmm0, [pw_one]
    vpaddw    xmm1, xmm1, [pw_one]
    vpaddw    xmm2, xmm2, [pw_one]
    vpaddw    xmm3, xmm3, [pw_one]
    vpsraw    xmm0, xmm0, 1
    vpsraw    xmm1, xmm1, 1
    vpsraw    xmm2, xmm2, 1
    vpsraw    xmm3, xmm3, 1
    TRANSPOSE4
    vmovq     qword [rdx], xmm0
    vmovq     qword [rdx+8], xmm1
    vmovq     qword [rdx+16], xmm2
    vmovq     qword [rdx+24], xmm3
    DCT_EPILOGUE
