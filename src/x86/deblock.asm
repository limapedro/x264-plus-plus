; x264++ — original AVX2 deblocking edge filter (Win64 NASM)
;
;   void x264pp_deblock_edge_avx2(uint8_t *pix, intptr_t stride,
;                                 int alpha, int beta, const int8_t *bS,
;                                 int chroma, int vertical);
;
; Behaviour follows H.264 clause 8.7 / docs/rebuild/common/x86/deblock-a.asm.md
; §5.1-5.5 for one edge: p3 p2 p1 p0 | q0 q1 q2 q3, with the three threshold
; tests, the tc-clipped weak filter, and the strong intra filter when bS==4.
; bS==0 leaves a sample untouched.  For the inter filter tc is derived from
; alpha and bS as (alpha*bS)>>3 (this ABI carries no separate tc0 array).
; Vertical edges advance along the picture stride; horizontal edges advance
; in x and separate p/q by the stride.

default rel

section .text

%macro ABS_BYTE_DIFF 2                      ; eax = |%1 - %2| (byte locals)
    movzx   eax, byte [%1]
    movzx   edx, byte [%2]
    sub     eax, edx
    jge     %%abs_done
    neg     eax
%%abs_done:
%endmacro

global x264pp_deblock_edge_avx2
x264pp_deblock_edge_avx2:
    sub       rsp, 200
    mov       [rsp+32], rcx                 ; pix
    mov       [rsp+40], rdx                 ; stride
    mov       [rsp+48], r8d                 ; alpha
    mov       [rsp+52], r9d                 ; beta
    mov       rax, [rsp+240]
    mov       [rsp+56], rax                 ; bS
    mov       eax, [rsp+248]
    mov       [rsp+64], eax                 ; chroma
    mov       eax, [rsp+256]
    mov       [rsp+72], eax                 ; vertical

    mov       r10d, 8                        ; luma: 8 edge samples
    cmp       dword [rsp+64], 0
    je        .have_count
    mov       r10d, 4                        ; chroma: 4 samples
.have_count:
    mov       [rsp+160], r10d

.edge_loop:
    mov       r11, [rsp+32]
    mov       rdx, [rsp+40]
    cmp       dword [rsp+72], 0
    je        .horizontal_addr

    ; ---- vertical edge: p/q differ by one byte, samples by stride ----
    mov       rax, r10
    imul      rax, rdx
    add       r11, rax
    lea       rax, [r11-4]
    mov       [rsp+96], rax                  ; &p3
    lea       rax, [r11-3]
    mov       [rsp+104], rax                 ; &p2
    lea       rax, [r11-2]
    mov       [rsp+112], rax                 ; &p1
    lea       rax, [r11-1]
    mov       [rsp+120], rax                 ; &p0
    lea       rax, [r11]
    mov       [rsp+128], rax                 ; &q0
    lea       rax, [r11+1]
    mov       [rsp+136], rax                 ; &q1
    lea       rax, [r11+2]
    mov       [rsp+144], rax                 ; &q2
    lea       rax, [r11+3]
    mov       [rsp+152], rax                 ; &q3
    jmp       .load_pixels

.horizontal_addr:
    add       r11, r10                       ; pixel at column i
    lea       r8, [rdx+rdx]                  ; 2*stride
    lea       r9, [rdx+2*rdx]                ; 3*stride
    lea       rax, [r9+rdx]
    mov       rcx, r11
    sub       rcx, rax
    mov       [rsp+96], rcx                  ; &p3
    mov       rcx, r11
    sub       rcx, r9
    mov       [rsp+104], rcx                 ; &p2
    mov       rcx, r11
    sub       rcx, r8
    mov       [rsp+112], rcx                 ; &p1
    mov       rcx, r11
    sub       rcx, rdx
    mov       [rsp+120], rcx                 ; &p0
    mov       [rsp+128], r11                 ; &q0
    lea       rax, [r11+rdx]
    mov       [rsp+136], rax                 ; &q1
    lea       rax, [r11+r8]
    mov       [rsp+144], rax                 ; &q2
    lea       rax, [r11+r9]
    mov       [rsp+152], rax                 ; &q3

.load_pixels:
    mov       rax, [rsp+96]
    movzx     eax, byte [rax]
    mov       [rsp+80], al                   ; p3
    mov       rax, [rsp+104]
    movzx     eax, byte [rax]
    mov       [rsp+81], al                   ; p2
    mov       rax, [rsp+112]
    movzx     eax, byte [rax]
    mov       [rsp+82], al                   ; p1
    mov       rax, [rsp+120]
    movzx     eax, byte [rax]
    mov       [rsp+83], al                   ; p0
    mov       rax, [rsp+128]
    movzx     eax, byte [rax]
    mov       [rsp+84], al                   ; q0
    mov       rax, [rsp+136]
    movzx     eax, byte [rax]
    mov       [rsp+85], al                   ; q1
    mov       rax, [rsp+144]
    movzx     eax, byte [rax]
    mov       [rsp+86], al                   ; q2
    mov       rax, [rsp+152]
    movzx     eax, byte [rax]
    mov       [rsp+87], al                   ; q3

    mov       rax, [rsp+56]
    movsx     r8d, byte [rax+r10]            ; bS
    test      r8d, r8d
    jz        .next_edge

    ; |p0-q0| < alpha && |p1-p0| < beta && |q1-q0| < beta
    ABS_BYTE_DIFF rsp+83, rsp+84
    cmp       eax, [rsp+48]
    jge       .next_edge
    ABS_BYTE_DIFF rsp+82, rsp+83
    cmp       eax, [rsp+52]
    jge       .next_edge
    ABS_BYTE_DIFF rsp+85, rsp+84
    cmp       eax, [rsp+52]
    jge       .next_edge

    mov       eax, [rsp+48]
    imul      eax, r8d
    sar       eax, 3                         ; tc
    mov       [rsp+88], eax

    cmp       r8d, 4
    jne       .weak_filter
    cmp       dword [rsp+64], 0
    jne       .weak_filter                   ; chroma has no strong filter

    ; ---- strong intra luma filter ----
    movzx     eax, byte [rsp+81]             ; p2
    movzx     edx, byte [rsp+82]             ; p1
    add       eax, edx
    add       eax, edx
    movzx     edx, byte [rsp+83]             ; p0
    add       eax, edx
    add       eax, edx
    movzx     edx, byte [rsp+84]             ; q0
    add       eax, edx
    add       eax, edx
    movzx     edx, byte [rsp+85]             ; q1
    add       eax, edx
    add       eax, 4
    sar       eax, 3
    mov       [rsp+83], al                   ; p0'

    movzx     eax, byte [rsp+81]
    movzx     edx, byte [rsp+82]
    add       eax, edx
    movzx     edx, byte [rsp+83]
    add       eax, edx
    movzx     edx, byte [rsp+84]
    add       eax, edx
    add       eax, 2
    sar       eax, 2
    mov       [rsp+82], al                   ; p1'

    ABS_BYTE_DIFF rsp+81, rsp+83
    cmp       eax, [rsp+52]
    jge       .strong_q
    movzx     eax, byte [rsp+80]             ; p3
    add       eax, eax
    movzx     edx, byte [rsp+81]
    lea       eax, [eax+edx*2]
    add       eax, edx
    movzx     edx, byte [rsp+82]
    add       eax, edx
    movzx     edx, byte [rsp+83]
    add       eax, edx
    movzx     edx, byte [rsp+84]
    add       eax, edx
    add       eax, 4
    sar       eax, 3
    mov       [rsp+81], al                   ; p2'

.strong_q:
    movzx     eax, byte [rsp+86]             ; q2
    movzx     edx, byte [rsp+85]             ; q1
    add       eax, edx
    add       eax, edx
    movzx     edx, byte [rsp+84]             ; q0
    add       eax, edx
    add       eax, edx
    movzx     edx, byte [rsp+83]             ; p0 (already filtered)
    add       eax, edx
    add       eax, edx
    movzx     edx, byte [rsp+82]             ; p1 (already filtered)
    add       eax, edx
    add       eax, 4
    sar       eax, 3
    mov       [rsp+84], al                   ; q0'

    movzx     eax, byte [rsp+86]
    movzx     edx, byte [rsp+85]
    add       eax, edx
    movzx     edx, byte [rsp+84]
    add       eax, edx
    movzx     edx, byte [rsp+83]
    add       eax, edx
    add       eax, 2
    sar       eax, 2
    mov       [rsp+85], al                   ; q1'

    ABS_BYTE_DIFF rsp+86, rsp+84
    cmp       eax, [rsp+52]
    jge       .store_result
    movzx     eax, byte [rsp+87]             ; q3
    add       eax, eax
    movzx     edx, byte [rsp+86]
    lea       eax, [eax+edx*2]
    add       eax, edx
    movzx     edx, byte [rsp+85]
    add       eax, edx
    movzx     edx, byte [rsp+84]
    add       eax, edx
    movzx     edx, byte [rsp+83]
    add       eax, edx
    add       eax, 4
    sar       eax, 3
    mov       [rsp+86], al                   ; q2'
    jmp       .store_result

    ; ---- weak (normal) filter ----
.weak_filter:
    ABS_BYTE_DIFF rsp+81, rsp+83             ; |p2-p0|
    cmp       eax, [rsp+52]
    jge       .weak_q
    movzx     eax, byte [rsp+83]             ; p0
    movzx     edx, byte [rsp+84]             ; q0
    add       eax, edx
    add       eax, 1
    shr       eax, 1                         ; avg(p0,q0)
    movzx     edx, byte [rsp+81]             ; p2
    add       eax, edx
    add       eax, 1
    shr       eax, 1                         ; avg(p2, avg(p0,q0))
    mov       edx, [rsp+88]                  ; tc
    movzx     ecx, byte [rsp+82]             ; p1
    sub       ecx, edx
    cmp       eax, ecx
    jge       .weak_p1_clip_hi
    mov       eax, ecx
.weak_p1_clip_hi:
    movzx     ecx, byte [rsp+82]
    add       ecx, edx
    cmp       eax, ecx
    jle       .weak_p1_done
    mov       eax, ecx
.weak_p1_done:
    mov       [rsp+82], al
    inc       dword [rsp+88]                 ; tc+1 for the central filter

.weak_q:
    ABS_BYTE_DIFF rsp+86, rsp+84             ; |q2-q0|
    cmp       eax, [rsp+52]
    jge       .weak_delta
    movzx     eax, byte [rsp+84]
    movzx     edx, byte [rsp+83]
    add       eax, edx
    add       eax, 1
    shr       eax, 1
    movzx     edx, byte [rsp+86]
    add       eax, edx
    add       eax, 1
    shr       eax, 1
    mov       edx, [rsp+88]
    movzx     ecx, byte [rsp+85]
    sub       ecx, edx
    cmp       eax, ecx
    jge       .weak_q1_clip_hi
    mov       eax, ecx
.weak_q1_clip_hi:
    movzx     ecx, byte [rsp+85]
    add       ecx, edx
    cmp       eax, ecx
    jle       .weak_q1_done
    mov       eax, ecx
.weak_q1_done:
    mov       [rsp+85], al

.weak_delta:
    movzx     eax, byte [rsp+84]             ; q0
    movzx     edx, byte [rsp+83]             ; p0
    sub       eax, edx
    lea       eax, [eax*4]
    movzx     edx, byte [rsp+82]             ; p1 (possibly filtered)
    add       eax, edx
    movzx     edx, byte [rsp+85]             ; q1 (possibly filtered)
    sub       eax, edx
    add       eax, 4
    sar       eax, 3
    mov       edx, [rsp+88]
    mov       ecx, edx
    neg       ecx
    cmp       eax, ecx
    jge       .delta_hi
    mov       eax, ecx
.delta_hi:
    cmp       eax, edx
    jle       .delta_done
    mov       eax, edx
.delta_done:
    mov       edx, eax                       ; delta

    movzx     eax, byte [rsp+83]
    add       eax, edx
    cmp       eax, 0
    jge       .p0_nonneg
    xor       eax, eax
.p0_nonneg:
    cmp       eax, 255
    jle       .p0_clipped
    mov       eax, 255
.p0_clipped:
    mov       [rsp+83], al

    movzx     eax, byte [rsp+84]
    sub       eax, edx
    cmp       eax, 0
    jge       .q0_nonneg
    xor       eax, eax
.q0_nonneg:
    cmp       eax, 255
    jle       .q0_clipped
    mov       eax, 255
.q0_clipped:
    mov       [rsp+84], al

.store_result:
    mov       rax, [rsp+112]
    mov       dl, [rsp+82]
    mov       [rax], dl                      ; p1
    mov       rax, [rsp+120]
    mov       dl, [rsp+83]
    mov       [rax], dl                      ; p0
    mov       rax, [rsp+128]
    mov       dl, [rsp+84]
    mov       [rax], dl                      ; q0
    mov       rax, [rsp+136]
    mov       dl, [rsp+85]
    mov       [rax], dl                      ; q1

.next_edge:
    inc       r10d
    cmp       r10d, [rsp+160]
    jl        .edge_loop

.done:
    add       rsp, 200
    ret
