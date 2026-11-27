; x264++ — original AVX2 4x4 quantisation / dequantisation (Win64 NASM)
;
;   int  x264pp_quant4x4_avx2 (const int16_t *coef, int16_t *qcoef,
;                             int16_t *dqcoef, const uint16_t *qp_table,
;                             int qp, const uint8_t *zigzag);
;   void x264pp_dequant4x4_avx2(const int16_t *qcoef, int16_t *dqcoef,
;                              const uint16_t *qp_table, int qp,
;                              const uint8_t *zigzag);
;
; Contract (documented in src/x86/ABI.md): qp_table[qp] is a Q16 multiply
; factor; the dead-zone bias is half of it.  Quantisation is the x264/quant.c
; rule q = sign(d)*((|d| + mf/2)*mf >> 16).  Reconstruction uses the H.264
; levelScale vector {10,11,13,14,16,18} and shift qp/6-4, including the
; round-to-nearest bias for negative shifts.  zigzag[i] is the natural index
; of scan position i (layout of src/core/dct.h:x264pp_zigzag4): qcoef is
; scan-ordered, dqcoef natural-ordered.
;
; AVX2 performs the sign-magnitude quantiser; the zig-zag scatter and the
; dequantisation are short exact-integer sequences (AVX2 has no byte gather
; and the C reference saturates in 32 bits before narrowing).

default rel

section .rodata align=16
align 16
ls_table: dw 10, 11, 13, 14, 16, 18

section .text

; ---------------------------------------------------------------------------
; dequant_block (internal): RCX=qcoef, RDX=dqcoef, R8D=qp, R9=zigzag.
; Writes 16 natural-order coefficients with 32-bit arithmetic and saturation.
; ---------------------------------------------------------------------------
dequant_block:
    sub       rsp, 56
    mov       [rsp+40], rdx                  ; dqcoef
    mov       [rsp+48], rcx                  ; qcoef
    mov       eax, r8d
    xor       edx, edx
    mov       ecx, 6
    div       ecx                            ; EAX=qp/6, EDX=qp%6
    sub       eax, 4
    mov       [rsp+32], eax                  ; s = qp/6 - 4
    lea       r10, [rel ls_table]
    movzx     r11d, word [r10+rdx*2]         ; levelScale
    mov       r8, [rsp+48]                   ; qcoef pointer
    xor       r10d, r10d                     ; i
.dqb_loop:
    movzx     edx, byte [r9+r10]             ; zigzag[i]
    movsx     eax, word [r8+rdx*2]           ; level (signed)
    imul      eax, r11d                      ; level * levelScale
    mov       ecx, [rsp+32]
    test      ecx, ecx
    jns       .dqb_left
    neg       ecx
    mov       edx, 1
    dec       ecx
    shl       edx, cl                        ; 1 << (-s - 1)
    add       eax, edx
    mov       ecx, [rsp+32]
    neg       ecx
    sar       eax, cl
    jmp       .dqb_clip
.dqb_left:
    test      ecx, ecx
    jz        .dqb_clip
    shl       eax, cl
.dqb_clip:
    cmp       eax, 32767
    jle       .dqb_low
    mov       eax, 32767
.dqb_low:
    cmp       eax, -32768
    jge       .dqb_store
    mov       eax, -32768
.dqb_store:
    mov       rdx, [rsp+40]
    mov       [rdx+r10*2], ax
    inc       r10d
    cmp       r10d, 16
    jl        .dqb_loop
    add       rsp, 56
    ret

; ---------------------------------------------------------------------------
; quant4x4
; ---------------------------------------------------------------------------
global x264pp_quant4x4_avx2
x264pp_quant4x4_avx2:
    ; Win64: RCX=coef, RDX=qcoef, R8=dqcoef, R9=qp_table,
    ;        [rsp+40 at entry]=qp, [rsp+48]=zigzag
    sub       rsp, 104
    mov       [rsp+64], r8                   ; dqcoef
    mov       eax, [rsp+144]                 ; qp
    mov       [rsp+72], eax
    mov       rax, [rsp+152]                 ; zigzag
    mov       [rsp+80], rax
    mov       [rsp+96], rdx                  ; qcoef

    vmovdqu   ymm0, [rcx]                    ; 16 residual words
    vmovdqa   ymm5, ymm0
    mov       eax, [rsp+72]
    lea       r10, [r9+rax*2]
    vpbroadcastw ymm1, word [r10]            ; mf
    vpsrlw    ymm2, ymm1, 1                  ; bias = mf/2
    vpabsw    ymm0, ymm5
    vpaddw    ymm0, ymm0, ymm2
    vpmulhuw  ymm0, ymm0, ymm1               ; unsigned magnitude
    vpxor     ymm4, ymm4, ymm4
    vpcmpgtw  ymm3, ymm4, ymm5               ; -1 where coef < 0
    vpxor     ymm0, ymm0, ymm3
    vpsubw    ymm0, ymm0, ymm3               ; signed levels
    vmovdqu   [rsp+32], ymm0

    mov       r10, rdx
    mov       r11, [rsp+80]
    xor       ecx, ecx
    xor       r8d, r8d
.q_store_loop:
    movzx     eax, word [rsp+32+rcx*2]
    movzx     edx, byte [r11+rcx]
    mov       [r10+rdx*2], ax
    test      eax, eax
    jz        .q_next
    inc       r8d
.q_next:
    inc       ecx
    cmp       ecx, 16
    jl        .q_store_loop

    mov       [rsp+88], r8d                  ; nonzero count (r8 is volatile)
    mov       rcx, [rsp+96]                  ; qcoef
    mov       rdx, [rsp+64]                  ; dqcoef
    mov       r8d, [rsp+72]                  ; qp
    mov       r9, [rsp+80]                   ; zigzag
    call      dequant_block

    mov       eax, [rsp+88]                  ; nonzero count
    add       rsp, 104
    ret

; ---------------------------------------------------------------------------
; dequant4x4
; ---------------------------------------------------------------------------
global x264pp_dequant4x4_avx2
x264pp_dequant4x4_avx2:
    ; Win64: RCX=qcoef, RDX=dqcoef, R8=qp_table, R9=qp, [rsp+40]=zigzag
    sub       rsp, 40
    mov       r8d, r9d                       ; qp
    mov       r9, [rsp+80]                   ; zigzag
    call      dequant_block
    add       rsp, 40
    ret
