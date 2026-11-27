; x264++ — original AVX2 intra prediction (Win64 NASM)
;
;   void x264pp_intra4x4_avx2 (uint8_t *dst, intptr_t stride,
;                              const uint8_t *above, const uint8_t *left, int mode);
;   void x264pp_intra16x16_avx2(...)
;
; Neighbour convention: above[0] = top-left corner, above[1+k] = T[k];
; left[0] = same corner, left[1+k] = L[k].  Mode numbering is the H.264
; intra4x4 / intra16x16 numbering.  Equations are the published H.264 rules
; (F1(a,b)=(a+b+1)>>1, F2(a,b,c)=(a+2b+c+2)>>2) as summarised in
; docs/rebuild/common/predict.c.md §5.1/§5.4 and predict-a.asm.md §5.12.
;
; The directional 4x4 modes use a compact descriptor table: per output pixel
; (a,b,c,kind) where kind 0 = copy N[a], 1 = F1(N[a],N[b]), 2 = F2(N[a],N[b],N[c]).
; N[] = [corner, T0..T7, L0..L3] is assembled on the stack once per call.

default rel

section .rodata align=16
align 16

; N indices: 0=lt, 1..8=T0..T7, 9..12=L0..L3
tbl_ddl:
    db 1,2,3,2,   2,3,4,2,   3,4,5,2,   4,5,6,2
    db 2,3,4,2,   3,4,5,2,   4,5,6,2,   5,6,7,2
    db 3,4,5,2,   4,5,6,2,   5,6,7,2,   6,7,8,2
    db 4,5,6,2,   5,6,7,2,   6,7,8,2,   7,8,8,2

tbl_ddr:
    db 9,0,1,2,   0,1,2,2,   1,2,3,2,   2,3,4,2
    db 10,9,0,2,  9,0,1,2,   0,1,2,2,   1,2,3,2
    db 11,10,9,2, 10,9,0,2,  9,0,1,2,   0,1,2,2
    db 12,11,10,2,11,10,9,2, 10,9,0,2,  9,0,1,2

tbl_vr:
    db 0,1,0,1,   1,2,0,1,   2,3,0,1,   3,4,0,1
    db 9,0,1,2,   0,1,2,2,   1,2,3,2,   2,3,4,2
    db 0,9,10,2,  0,1,0,1,   1,2,0,1,   2,3,0,1
    db 9,10,11,2, 9,0,1,2,   0,1,2,2,   1,2,3,2

tbl_hd:
    db 0,9,0,1,   9,0,1,2,   0,1,2,2,   1,2,3,2
    db 9,10,0,1,  0,9,10,2,  0,9,0,1,   9,0,1,2
    db 10,11,0,1, 9,10,11,2, 9,10,0,1,  0,9,10,2
    db 11,12,0,1, 10,11,12,2,10,11,0,1, 9,10,11,2

tbl_vl:
    db 1,2,0,1,   3,4,0,1,   5,6,0,1,   7,8,0,1
    db 5,6,0,1,   1,2,3,2,   2,3,4,2,   3,4,5,2
    db 4,5,6,2,   1,2,0,1,   2,3,0,1,   3,4,0,1
    db 5,6,7,2,   5,6,0,1,   1,2,3,2,   2,3,4,2

tbl_hu:
    db 9,10,0,1,  9,10,11,2, 10,11,0,1, 10,11,12,2
    db 10,11,0,1, 10,11,12,2,11,12,0,1, 11,12,12,2
    db 11,12,0,1, 11,12,12,2,12,0,0,0,  12,0,0,0
    db 12,0,0,0,  12,0,0,0,  12,0,0,0,  12,0,0,0

section .text

; ---------------------------------------------------------------------------
; 4x4 luma intra prediction.
; ---------------------------------------------------------------------------
global x264pp_intra4x4_avx2
x264pp_intra4x4_avx2:
    ; Win64: RCX=dst, RDX=stride, R8=above, R9=left, [rsp+40 at entry]=mode
    sub       rsp, 88
    mov       [rsp+48], rcx                 ; dst
    mov       [rsp+56], rdx                 ; stride
    mov       [rsp+72], r8                  ; above
    mov       [rsp+80], r9                  ; left
    mov       r9d, [rsp+128]                ; mode (5th arg, on stack)
    mov       [rsp+64], r9d

    xor       r10d, r10d
.top_copy:                                  ; N[0..8] = above[0..8]
    movzx     r11d, byte [r8+r10]
    mov       [rsp+32+r10], r11b
    inc       r10d
    cmp       r10d, 9
    jl        .top_copy

    mov       r9, [rsp+80]                  ; left
    xor       r10d, r10d
.left_copy:                                 ; N[9..12] = left[1..4]
    movzx     r11d, byte [r9+r10+1]
    mov       [rsp+41+r10], r11b
    inc       r10d
    cmp       r10d, 4
    jl        .left_copy

    cmp       dword [rsp+64], 0
    je        .mode_v
    cmp       dword [rsp+64], 1
    je        .mode_h
    cmp       dword [rsp+64], 2
    je        .mode_dc
    cmp       dword [rsp+64], 3
    je        .mode_ddl
    cmp       dword [rsp+64], 4
    je        .mode_ddr
    cmp       dword [rsp+64], 5
    je        .mode_vr
    cmp       dword [rsp+64], 6
    je        .mode_hd
    cmp       dword [rsp+64], 7
    je        .mode_vl
    cmp       dword [rsp+64], 8
    je        .mode_hu
    jmp       .mode_dc

.mode_v:
    vmovq     xmm0, qword [r8+1]             ; T0..T3
    mov       rax, [rsp+48]
    mov       rdx, [rsp+56]
    mov       ecx, 4
.v_loop:
    vmovd     dword [rax], xmm0
    add       rax, rdx
    dec       ecx
    jnz       .v_loop
    jmp       .done

.mode_h:
    mov       rax, [rsp+48]
    mov       rcx, [rsp+56]                  ; stride
    xor       r11d, r11d
.h_loop:
    vpbroadcastb xmm0, byte [r9+r11+1]       ; R9 = left[]
    vmovd     dword [rax], xmm0
    add       rax, rcx
    inc       r11d
    cmp       r11d, 4
    jl        .h_loop
    jmp       .done

.mode_dc:
    xor       eax, eax
    mov       r10d, 1                        ; N[1..4] = T0..T3
    mov       ecx, 4
.dc_top:
    movzx     r11d, byte [rsp+32+r10]
    add       eax, r11d
    inc       r10d
    dec       ecx
    jnz       .dc_top
    mov       r10d, 9                        ; N[9..12] = L0..L3
    mov       ecx, 4
.dc_left:
    movzx     r11d, byte [rsp+32+r10]
    add       eax, r11d
    inc       r10d
    dec       ecx
    jnz       .dc_left
    add       eax, 4
    shr       eax, 3
    mov       [rsp+84], al
    vpbroadcastb xmm0, byte [rsp+84]         ; VEX form (GPR form needs EVEX)
    mov       rax, [rsp+48]
    mov       rdx, [rsp+56]
    mov       ecx, 4
.dc_store:
    vmovd     dword [rax], xmm0
    add       rax, rdx
    dec       ecx
    jnz       .dc_store
    jmp       .done

.mode_ddl:
    lea       r10, [rel tbl_ddl]
    jmp       .generic
.mode_ddr:
    lea       r10, [rel tbl_ddr]
    jmp       .generic
.mode_vr:
    lea       r10, [rel tbl_vr]
    jmp       .generic
.mode_hd:
    lea       r10, [rel tbl_hd]
    jmp       .generic
.mode_vl:
    lea       r10, [rel tbl_vl]
    jmp       .generic
.mode_hu:
    lea       r10, [rel tbl_hu]

.generic:
    xor       r11d, r11d                     ; i = y*4+x
.gen_loop:
    movzx     eax, byte [r10+r11*4]
    movzx     ecx, byte [r10+r11*4+1]
    movzx     edx, byte [r10+r11*4+2]
    movzx     r8d, byte [r10+r11*4+3]
    movzx     eax, byte [rsp+32+rax]
    test      r8d, r8d
    jz        .gen_store
    movzx     ecx, byte [rsp+32+rcx]
    cmp       r8d, 1
    je        .gen_f1
    movzx     edx, byte [rsp+32+rdx]
    add       ecx, ecx
    add       eax, ecx
    add       eax, edx
    add       eax, 2
    shr       eax, 2
    jmp       .gen_store
.gen_f1:
    add       eax, ecx
    add       eax, 1
    shr       eax, 1
.gen_store:
    mov       r9, [rsp+48]
    mov       rcx, r11
    shr       rcx, 2
    imul      rcx, [rsp+56]
    mov       edx, r11d
    and       edx, 3
    add       r9, rcx
    mov       [r9+rdx], al
    inc       r11d
    cmp       r11d, 16
    jl        .gen_loop

.done:
    add       rsp, 88
    ret

; ---------------------------------------------------------------------------
; 16x16 luma intra prediction (V / H / DC / plane).
; ---------------------------------------------------------------------------
global x264pp_intra16x16_avx2
x264pp_intra16x16_avx2:
    ; Win64: RCX=dst, RDX=stride, R8=above, R9=left, [rsp+40 at entry]=mode
    sub       rsp, 144
    mov       [rsp+32], rcx                  ; dst
    mov       [rsp+40], rdx                  ; stride
    mov       [rsp+128], r8                  ; above (outside Tbuf/Lbuf)
    mov       [rsp+132], r9                  ; left

    movzx     eax, byte [r8]                 ; corner
    mov       [rsp+56], al
    mov       [rsp+64], al                   ; Tbuf[0]
    mov       [rsp+96], al                   ; Lbuf[0]
    xor       r10d, r10d
.copy16:
    movzx     eax, byte [r8+r10+1]
    mov       [rsp+65+r10], al               ; Tbuf[1+i] = T[i]
    movzx     eax, byte [r9+r10+1]
    mov       [rsp+97+r10], al               ; Lbuf[1+i] = L[i]
    inc       r10d
    cmp       r10d, 16
    jl        .copy16

    mov       r9d, [rsp+184]                 ; mode (5th arg)
    mov       [rsp+48], r9d
    cmp       dword [rsp+48], 0
    je        .p16_v
    cmp       dword [rsp+48], 1
    je        .p16_h
    cmp       dword [rsp+48], 2
    je        .p16_dc
    cmp       dword [rsp+48], 3
    je        .p16_plane
    jmp       .p16_dc

.p16_v:
    vmovdqu   xmm0, [r8+1]
    mov       rax, [rsp+32]
    mov       rdx, [rsp+40]
    mov       ecx, 16
.p16_v_loop:
    vmovdqu   [rax], xmm0
    add       rax, rdx
    dec       ecx
    jnz       .p16_v_loop
    jmp       .p16_done

.p16_h:
    mov       rax, [rsp+32]
    mov       rcx, [rsp+40]                  ; stride
    mov       r9, [rsp+132]                  ; left[]
    xor       r11d, r11d
.p16_h_loop:
    vpbroadcastb xmm0, byte [r9+r11+1]
    vmovdqu   [rax], xmm0
    add       rax, rcx
    inc       r11d
    cmp       r11d, 16
    jl        .p16_h_loop
    jmp       .p16_done

.p16_dc:
    xor       eax, eax
    xor       r10d, r10d
.p16_dc_t:
    movzx     r11d, byte [rsp+65+r10]
    add       eax, r11d
    inc       r10d
    cmp       r10d, 16
    jl        .p16_dc_t
    xor       r10d, r10d
.p16_dc_l:
    movzx     r11d, byte [rsp+97+r10]
    add       eax, r11d
    inc       r10d
    cmp       r10d, 16
    jl        .p16_dc_l
    add       eax, 16
    shr       eax, 5
    mov       [rsp+8], al
    vpbroadcastb xmm0, byte [rsp+8]          ; VEX form (GPR form needs EVEX)
    mov       rax, [rsp+32]
    mov       rdx, [rsp+40]
    mov       ecx, 16
.p16_dc_store:
    vmovdqu   [rax], xmm0
    add       rax, rdx
    dec       ecx
    jnz       .p16_dc_store
    jmp       .p16_done

.p16_plane:
    ; H = sum (i+1)*(T[8+i]-T[6-i]); V = same for L; T[-1]=corner.
    xor       r8d, r8d
    xor       r9d, r9d
    xor       r10d, r10d
.grad_loop:
    lea       r11d, [r10+1]
    mov       eax, r11d
    movzx     ecx, byte [rsp+64+9+r10]
    mov       edx, 7
    sub       edx, r10d
    movzx     edx, byte [rsp+64+rdx]
    sub       ecx, edx
    imul      eax, ecx
    add       r8d, eax
    mov       eax, r11d
    movzx     ecx, byte [rsp+96+9+r10]
    mov       edx, 7
    sub       edx, r10d
    movzx     edx, byte [rsp+96+rdx]
    sub       ecx, edx
    imul      eax, ecx
    add       r9d, eax
    inc       r10d
    cmp       r10d, 8
    jl        .grad_loop

    mov       eax, r8d
    imul      eax, 5
    add       eax, 32
    sar       eax, 6
    mov       [rsp+16], eax                  ; b
    mov       eax, r9d
    imul      eax, 5
    add       eax, 32
    sar       eax, 6
    mov       [rsp+24], eax                  ; c

    movzx     eax, byte [rsp+64+16]          ; T[15]
    movzx     ecx, byte [rsp+96+16]          ; L[15]
    add       eax, ecx
    shl       eax, 4                         ; a = 16*(L15+T15)
    mov       ecx, [rsp+16]
    imul      ecx, 7
    sub       eax, ecx
    mov       ecx, [rsp+24]
    imul      ecx, 7
    sub       eax, ecx
    add       eax, 16                        ; start
    mov       [rsp+48], eax

    xor       r10d, r10d                     ; y
.plane_y:
    mov       eax, [rsp+48]
    mov       ecx, r10d
    imul      ecx, [rsp+24]                  ; y*c
    add       eax, ecx
    mov       [rsp+56], eax                  ; row base
    xor       r11d, r11d                     ; x
.plane_x:
    mov       eax, [rsp+56]
    mov       ecx, r11d
    imul      ecx, [rsp+16]                  ; x*b
    add       eax, ecx
    sar       eax, 5
    cmp       eax, 0
    jge       .plane_nonneg
    xor       eax, eax
.plane_nonneg:
    cmp       eax, 255
    jle       .plane_clipped
    mov       eax, 255
.plane_clipped:
    mov       r8b, al                        ; clipped value
    mov       rdx, [rsp+32]
    mov       rcx, r10
    imul      rcx, [rsp+40]
    add       rdx, rcx
    mov       [rdx+r11], r8b
    inc       r11d
    cmp       r11d, 16
    jl        .plane_x
    inc       r10d
    cmp       r10d, 16
    jl        .plane_y

.p16_done:
    add       rsp, 144
    ret
