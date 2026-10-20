; jl kernel: one-pass JSON -> Python objects through the C sink (src/jl_loads.c).
;
; int jl_run(JlCtx *c)            platform C ABI (Win64 or SysV), one source.
;
; State in callee-saved registers (both ABIs):
;   r12 = ctx       r13 = p (cursor)    r14 = end (end[0] is NUL)
;   r15 = vsp       rbx = vs_end        rbp = fsp (next free 16-byte frame)
; Frame: [0] = byte offset of the first value in vs, [8] = kind (0 array, 1 object).
; A list / dict is built once at its closing bracket from the value-stack slice.
; Only ymm0..ymm5 are used (xmm6..15 are callee-saved on Win64). vzeroupper
; before every C call.
;
; The sink owns Python: no PyObject layout here, objects are opaque pointers.

default rel

%include "jl_ctx.inc"

%ifidn __OUTPUT_FORMAT__, win64
  %define A1 rcx
  %define A2 rdx
  %define A3 r8
  %define A4 r9
  %define A1d ecx
  %define A3d r8d
  %define A4d r9d
  %macro CC 1
        vzeroupper
        call %1
  %endmacro
  %macro EXPORT 1
        global %1
  %endmacro
%else
  %define A1 rdi
  %define A2 rsi
  %define A3 rdx
  %define A4 rcx
  %define A1d edi
  %define A3d edx
  %define A4d ecx
  %macro CC 1
        vzeroupper
        call %1 wrt ..plt
  %endmacro
  %macro EXPORT 1
        global %1:function hidden
  %endmacro
%endif

extern jl_mk_str
extern jl_mk_key
extern jl_mk_i64
extern jl_mk_u64
extern jl_mk_f64
extern jl_mk_lit
extern jl_mk_list
extern jl_mk_dict
extern jl_grow_vs
extern jl_grow_fs
extern jl_str_escaped
extern jl_strtod
extern jl_drop

section .rodata
align 32
jl_v_quote:  times 32 db '"'
jl_v_bslash: times 32 db 0x5C
jl_v_1f:     times 32 db 0x1F
jl_p10:
    dq 1.0e0, 1.0e1, 1.0e2, 1.0e3, 1.0e4, 1.0e5, 1.0e6, 1.0e7
    dq 1.0e8, 1.0e9, 1.0e10, 1.0e11, 1.0e12, 1.0e13, 1.0e14, 1.0e15
    dq 1.0e16, 1.0e17, 1.0e18, 1.0e19, 1.0e20, 1.0e21, 1.0e22
align 16
jl_signbit:  dq 0x8000000000000000, 0
align 32
jl_ones32:   times 32 db 0xFF
             times 32 db 0x00
jl_v_sp:     times 32 db ' '
jl_v_nl:     times 32 db 10
jl_v_cr:     times 32 db 13
jl_v_tab:    times 32 db 9
; vector number digits
jl_v_30:     times 16 db '0'
jl_v_09:     times 16 db 9
jl_nshuf:    times 16 db 0x80     ; [jl_nshuf+n]: right-align n bytes, zero the rest
             db 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
jl_m10:      times 8 db 10, 1
jl_m100:     times 4 dw 100, 1
jl_m10000:   times 4 dw 10000, 1
align 8
jl_p10u:
    dq 1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000
    dq 1000000000, 10000000000, 100000000000, 1000000000000, 10000000000000
    dq 100000000000000, 1000000000000000, 10000000000000000
%include "jl_pow5.inc"

section .text

; ---------------------------------------------------------------- helpers
; skip JSON whitespace at r13 (clobbers eax)
jl_skip_ws:
.l:
    movzx eax, byte [r13]
    cmp al, ' '
    je .adv
    cmp al, 10
    je .nl
    cmp al, 13
    je .nl
    cmp al, 9
    je .adv
    ret
.adv:
    inc r13
    cmp byte [r13], ' '
    ja .ret                       ; one space, then text: the common case
    jmp .l
.ret:
    ret
.nl:
    inc r13
    ; after a line break comes indentation: 32 bytes at a time
    mov rax, r14
    sub rax, r13
    cmp rax, 32
    jb .l
    vmovdqu ymm0, [r13]
    vpcmpeqb ymm1, ymm0, [jl_v_sp]
    vpcmpeqb ymm2, ymm0, [jl_v_nl]
    vpor ymm1, ymm1, ymm2
    vpcmpeqb ymm2, ymm0, [jl_v_cr]
    vpor ymm1, ymm1, ymm2
    vpcmpeqb ymm2, ymm0, [jl_v_tab]
    vpor ymm1, ymm1, ymm2
    vpmovmskb eax, ymm1
    not eax
    test eax, eax
    jz .all32
    tzcnt eax, eax
    add r13, rax
    ret
.all32:
    add r13, 32
    jmp .l

%macro SKIPWS 0
    cmp byte [r13], ' '
    ja %%done
    call jl_skip_ws
%%done:
%endmacro

; push rax on the value stack (grows through the sink)
%macro PUSHV 0
    cmp r15, rbx
    jb %%ok
    mov [rsp+32], rax
    mov [r12+JLC_VSP], r15
    mov A1, r12
    CC jl_grow_vs
    test eax, eax
    jnz jl_run.grow_fail
    mov r15, [r12+JLC_VSP]
    mov rbx, [r12+JLC_VSEND]
    mov rax, [rsp+32]
%%ok:
    mov [r15], rax
    add r15, 8
%endmacro

; eight ASCII digits in rax -> value in rax (clobbers rcx, rdx)
%macro PARSE8 0
    mov rcx, 0x3030303030303030
    sub rax, rcx
    lea rcx, [rax+rax*4]
    add rcx, rcx
    shr rax, 8
    add rax, rcx
    mov rcx, rax
    shr rcx, 16
    mov rdx, 0x000000FF000000FF
    and rax, rdx
    and rcx, rdx
    mov rdx, 0x000F424000000064
    imul rax, rdx
    mov rdx, 0x0000271000000001
    imul rcx, rdx
    add rax, rcx
    shr rax, 32
%endmacro

; 16 digit values (0..9) in %1, most significant first -> value in rax (clobbers %1, rdx)
%macro DIG16 1
    vpmaddubsw %1, %1, [jl_m10]
    vpmaddwd %1, %1, [jl_m100]
    vpackusdw %1, %1, %1
    vpmaddwd %1, %1, [jl_m10000]
    vmovq rax, %1
    mov edx, eax
    shr rax, 32
    imul rdx, rdx, 100000000
    add rax, rdx
%endmacro

; digits at %1: xmm0 = byte - '0', ecx = count of leading digits (0..16)
%macro DIGRUN 1
    vmovdqu xmm0, %1
    vpsubb xmm0, xmm0, [jl_v_30]
    vpminub xmm1, xmm0, [jl_v_09]
    vpcmpeqb xmm1, xmm1, xmm0
    vpmovmskb eax, xmm1
    not eax
    tzcnt ecx, eax
%endmacro

; ZF=1 when the 8 bytes in rax are all ASCII digits (clobbers rcx, rdx)
%macro IS8DIG 0
    mov rcx, 0x0606060606060606
    add rcx, rax
    mov rdx, 0xF0F0F0F0F0F0F0F0
    and rcx, rdx
    shr rcx, 4
    and rdx, rax
    or rcx, rdx
    mov rdx, 0x3333333333333333
    cmp rcx, rdx
%endmacro

; String at r13 (opening quote). %1 = 0 value, 1 key. On success rax = object,
; r13 after the closing quote; falls through. Errors leave jl_run.
%macro STRING 1
    lea r10, [r13+1]
    mov rax, r14
    sub rax, r10
    cmp rax, 32
    jae %%vec
    mov eax, r10d
    and eax, 4095
    cmp eax, 4096-32
    ja %%long
%%vec:
    vmovdqu ymm0, [r10]
    vpcmpeqb ymm1, ymm0, [jl_v_quote]
    vpcmpeqb ymm2, ymm0, [jl_v_bslash]
    vpmaxub ymm3, ymm0, [jl_v_1f]
    vpcmpeqb ymm3, ymm3, [jl_v_1f]
    vpor ymm1, ymm1, ymm2
    vpor ymm1, ymm1, ymm3
    vpmovmskb eax, ymm1
    test eax, eax
    jz %%long
    tzcnt ecx, eax
    lea rdx, [r10+rcx]
    cmp rdx, r14
    jae %%long
    cmp byte [rdx], '"'
    jne %%long
    vpmovmskb r8d, ymm0
%if %1
    ; ASCII key of at most 32 bytes: look it up in the key cache here
    mov r11d, 1
    shl r11, cl
    dec r11
    test r8, r11
    jnz %%kc_skip
    lea r11, [jl_ones32 + 32]
    sub r11, rcx
    vpand ymm1, ymm0, [r11]       ; key bytes, zero padded
    vmovq rax, xmm1
    vpextrq r9, xmm1, 1
    mov r11, 0x9E3779B97F4A7C15
    imul rax, r11
    mov r8, 0xC2B2AE3D27D4EB4F
    imul r9, r8
    xor rax, r9
    mov r9, 0x165667B19E3779F9
    imul r9, rcx
    xor rax, r9
    cmp ecx, 16
    jbe %%kc_h
    vextracti128 xmm2, ymm1, 1
    vmovq r9, xmm2
    mov r8, 0x85EBCA77C2B2AE63
    imul r9, r8
    xor rax, r9
    vpextrq r9, xmm2, 1
    mov r8, 0x27D4EB2F165667C5
    imul r9, r8
    xor rax, r9
%%kc_h:
    mov r9, rax
    shr r9, 29
    xor rax, r9
    imul rax, r11
    shr rax, 64 - JL_KC_BITS
    shl rax, 6
    add rax, [r12+JLC_KC]
    cmp dword [rax+40], ecx
    jne %%kc_miss
    vpcmpeqb ymm2, ymm1, [rax]
    vpmovmskb r9d, ymm2
    cmp r9d, -1
    jne %%kc_miss
    mov rax, [rax+32]
    test rax, rax
    jz %%kc_miss
    vzeroupper
    or rax, 1                     ; borrowed from the cache
    lea r13, [rdx+1]
    jmp %%ok
%%kc_miss:
    vpmovmskb r8d, ymm0
%%kc_skip:
%endif
    vzeroupper
    mov r11d, ecx                 ; n
    mov r9d, 1
    shl r9, cl
    dec r9
    and r8, r9                    ; high bits before the quote
    xor r9d, r9d
    test r8, r8
    setnz r9b
%if %1
    or r9d, 2                     ; 32 bytes at s are readable
%endif
    lea r13, [rdx+1]
    mov A3d, r9d
    mov A2, r11
    mov A1, r10
%if %1
    CC jl_mk_key
%else
    CC jl_mk_str
%endif
    test rax, rax
    jnz %%ok
    jmp strict near jl_run.e_str_back   ; r10 did not survive the call
    times 3 nop                         ; block stays 8 bytes
%%long:
    mov r11, r10                  ; cursor
    xor r9d, r9d                  ; high bits seen
%%lloop:
    mov rax, r14
    sub rax, r11
    cmp rax, 32
    jb %%ltail
    vmovdqu ymm0, [r11]
    vpcmpeqb ymm1, ymm0, [jl_v_quote]
    vpcmpeqb ymm2, ymm0, [jl_v_bslash]
    vpmaxub ymm3, ymm0, [jl_v_1f]
    vpcmpeqb ymm3, ymm3, [jl_v_1f]
    vpor ymm1, ymm1, ymm2
    vpor ymm1, ymm1, ymm3
    vpmovmskb eax, ymm1
    vpmovmskb r8d, ymm0
    test eax, eax
    jnz %%lfound
    or r9d, r8d
    add r11, 32
    jmp %%lloop
%%lfound:
    tzcnt ecx, eax
    mov edx, 1
    shl rdx, cl
    dec rdx
    and r8, rdx
    or r9d, r8d
    add r11, rcx
    jmp %%lspecial
%%ltail:
    cmp r11, r14
    jae %%unterminated
    movzx eax, byte [r11]
    cmp al, '"'
    je %%lspecial
    cmp al, 0x5C
    je %%lspecial
    cmp al, 0x20
    jb %%lspecial
    shr eax, 7
    or r9d, eax
    inc r11
    jmp %%ltail
%%lspecial:
    vzeroupper
    cmp r11, r14
    jae %%unterminated
    cmp byte [r11], '"'
    jne %%escaped
    lea r13, [r11+1]
    mov rax, r11
    sub rax, r10                  ; n
    xor ecx, ecx
    test r9d, r9d
    setnz cl
    mov A3d, ecx
    mov A2, rax
    mov A1, r10
%if %1
    CC jl_mk_key
%else
    CC jl_mk_str
%endif
    test rax, rax
    jnz %%ok
    jmp strict near jl_run.e_str_back   ; r10 did not survive the call
    times 3 nop                         ; block stays 8 bytes
%%escaped:
    xor eax, eax
    test r9d, r9d
    setnz al
%if %1
    or eax, 2
%endif
    mov A4d, eax
    mov A3, r11
    mov A2, r10
    mov A1, r12
    CC jl_str_escaped
    test rax, rax
    jz jl_run.err_set
    mov r13, [r12+JLC_POS]
    jmp %%ok
%%unterminated:
    vzeroupper
    mov r13, r10
    jmp jl_run.e_invalid
%%ok:
%endmacro

; ---------------------------------------------------------------- Eisel-Lemire
; in : rcx = w (!= 0), rdx = q (signed), r11d = negative
; out: eax = 1 ok (r8 = double bits), eax = -1 overflow
; clobbers rax, rcx, rdx, r8; preserves the rest
jl_el:
    cmp rdx, JL_POW5_QMIN
    jl .zero
    cmp rdx, JL_POW5_QMAX
    jg .ovf
    push rbx
    push rsi
    push rdi
    push r9
    push r10
    mov rsi, rdx                  ; q
    bsr rax, rcx
    mov edi, 63
    sub edi, eax                  ; lz
    mov rax, rcx
    mov ecx, edi
    shl rax, cl                   ; w <<= lz
    mov rbx, rax
    lea rdx, [rsi - JL_POW5_QMIN]
    shl rdx, 4
    lea r10, [jl_pow5_tab]
    add r10, rdx
    mul qword [r10]               ; rdx:rax = w * hi5
    mov r8d, edx
    and r8d, 0x1FF
    cmp r8d, 0x1FF
    jne .noref
    mov r9, rax                   ; lo
    mov r8, rdx                   ; hi
    mov rax, rbx
    mul qword [r10+8]             ; rdx = high(w * lo5)
    add r9, rdx
    adc r8, 0
    mov rax, r9
    mov rdx, r8
.noref:
    ; rdx = hi, rax = lo
    mov r9, rax                   ; lo
    mov r10, rdx                  ; hi
    mov r8, rdx
    shr r8, 63                    ; upper
    lea ecx, [r8d+9]
    mov rbx, rdx
    shr rbx, cl                   ; mant
    ; p2 = ((217706 * q) >> 16) + 63 + upper - lz + 1023
    mov rax, rsi
    imul rax, rax, 217706
    sar rax, 16
    add rax, 63 + 1023
    add rax, r8
    sub rax, rdi
    test rax, rax
    jle .sub
    ; ties to even when the product is exact
    cmp r9, 1
    ja .round
    cmp rsi, -4
    jl .round
    cmp rsi, 23
    jg .round
    mov rdx, rbx
    and edx, 3
    cmp edx, 1
    jne .round
    mov rdx, rbx
    shl rdx, cl                   ; cl = upper + 9
    cmp rdx, r10
    jne .round
    and rbx, -2
.round:
    mov rdx, rbx
    and edx, 1
    add rbx, rdx
    shr rbx, 1
    mov rdx, 1
    shl rdx, 53
    cmp rbx, rdx
    jb .norm
    shr rdx, 1
    mov rbx, rdx                  ; 1 << 52
    inc rax
.norm:
    cmp rax, 0x7FF
    jge .ovf_pop
    shl rax, 52
    mov rdx, 1
    shl rdx, 52
    dec rdx
    and rbx, rdx
    or rax, rbx
    jmp .sign
.sub:
    ; subnormal: shift = -p2 + 1
    mov rcx, 1
    sub rcx, rax
    cmp rcx, 64
    jae .zero_pop
    shr rbx, cl
    mov rdx, rbx
    and edx, 1
    add rbx, rdx
    shr rbx, 1
    xor eax, eax
    mov rdx, 1
    shl rdx, 52
    cmp rbx, rdx
    jb .subn
    mov eax, 1
.subn:
    shl rax, 52
    dec rdx
    and rbx, rdx
    or rax, rbx
.sign:
    mov r8, rax
    test r11d, r11d
    jz .okpop
    bts r8, 63
.okpop:
    pop r10
    pop r9
    pop rdi
    pop rsi
    pop rbx
    mov eax, 1
    ret
.zero_pop:
    pop r10
    pop r9
    pop rdi
    pop rsi
    pop rbx
.zero:
    xor r8d, r8d
    test r11d, r11d
    jz .z1
    bts r8, 63
.z1:
    mov eax, 1
    ret
.ovf_pop:
    pop r10
    pop r9
    pop rdi
    pop rsi
    pop rbx
.ovf:
    mov eax, -1
    ret

; ---------------------------------------------------------------- jl_run
EXPORT jl_run
jl_run:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    push rdi
    push rsi
    sub rsp, 40                   ; 32 shadow (Win64) + 8 spare, rsp % 16 == 0
    mov r12, A1
    mov r13, [r12+JLC_START]
    mov r14, [r12+JLC_END]
    mov r15, [r12+JLC_VSP]
    mov rbx, [r12+JLC_VSEND]
    mov rbp, [r12+JLC_FSP]
    mov rax, r14
    sub rax, r13
    cmp rax, 3
    jb .nobom
    cmp word [r13], 0xBBEF
    jne .nobom
    cmp byte [r13+2], 0xBF
    je strict near .e_invalid     ; UTF-8 BOM is not JSON; 6 bytes, same as the old skip
.nobom:
    SKIPWS
    cmp r13, r14
    jae .e_empty

.value:
    SKIPWS
.value_nows:
    movzx eax, byte [r13]
    lea rcx, [.jt]
    movsxd rax, dword [rcx+rax*4]
    add rax, rcx
    jmp rax

; ---- string value
.str:
    STRING 0
    jmp .have

; ---- containers
.obj_open:
    inc r13
    SKIPWS
    cmp byte [r13], '}'
    je .obj_empty
    cmp rbp, [r12+JLC_FSEND]
    jae .obj_grow
.obj_push:
    mov rax, r15
    sub rax, [r12+JLC_VS]
    mov [rbp], rax
    mov qword [rbp+8], 1
    add rbp, 16
    jmp .key_nows
.obj_grow:
    mov [r12+JLC_FSP], rbp
    mov A1, r12
    CC jl_grow_fs
    test eax, eax
    jnz .e_memory
    mov rbp, [r12+JLC_FSP]
    jmp .obj_push
.obj_empty:
    inc r13
    mov A1d, JL_LIT_EMPTY_DICT
    CC jl_mk_lit
    jmp .have_chk

.arr_open:
    inc r13
    SKIPWS
    cmp byte [r13], ']'
    je .arr_empty
    cmp rbp, [r12+JLC_FSEND]
    jae .arr_grow
.arr_push:
    mov rax, r15
    sub rax, [r12+JLC_VS]
    mov [rbp], rax
    mov qword [rbp+8], 0
    add rbp, 16
    jmp .value_nows
.arr_grow:
    mov [r12+JLC_FSP], rbp
    mov A1, r12
    CC jl_grow_fs
    test eax, eax
    jnz .e_memory
    mov rbp, [r12+JLC_FSP]
    jmp .arr_push
.arr_empty:
    inc r13
    mov A1d, JL_LIT_EMPTY_LIST
    CC jl_mk_lit
    jmp .have_chk

; ---- literals
.lit_t:
    mov rax, r14
    sub rax, r13
    cmp rax, 4
    jb .e_unexpected
    cmp dword [r13], 'true'
    jne .e_unexpected
    add r13, 4
    mov A1d, JL_LIT_TRUE
    CC jl_mk_lit
    jmp .have_chk
.lit_f:
    mov rax, r14
    sub rax, r13
    cmp rax, 5
    jb .e_unexpected
    cmp dword [r13], 'fals'
    jne .e_unexpected
    cmp byte [r13+4], 'e'
    jne .e_unexpected
    add r13, 5
    mov A1d, JL_LIT_FALSE
    CC jl_mk_lit
    jmp .have_chk
.lit_n:
    mov rax, r14
    sub rax, r13
    cmp rax, 4
    jb .e_unexpected
    cmp dword [r13], 'null'
    jne .e_unexpected
    add r13, 4
    mov A1d, JL_LIT_NULL
    CC jl_mk_lit
    jmp .have_chk
; NaN / Infinity are not JSON. The 120 bytes their literals took stay as
; padding so .num and the value loop keep their offsets.
    times 120 nop

; ---- numbers
; r10 = token start, edi = negative, r9 = w, r11d = digits in w, rsi = exp10,
; r8d bit0 = truncated digits, bit1 = float syntax
.num:
    mov r10, r13
    xor edi, edi
    cmp byte [r13], '-'
    jne .n_start
    mov edi, 1
    inc r13
.n_start:
    xor r9d, r9d
    xor r11d, r11d
    xor esi, esi
    xor r8d, r8d
    movzx eax, byte [r13]
    sub eax, '0'
    cmp eax, 9
    ja .n_notdigit
    test eax, eax
    jnz .n_int8
    inc r13
    movzx eax, byte [r13]
    sub eax, '0'
    cmp eax, 9
    jbe .e_number
    jmp .n_frac_chk
.n_int8:
    mov rax, r14
    sub rax, r13
    cmp rax, 8
    jb .n_int1
    cmp r11d, 11
    ja .n_int1
    mov rax, [r13]
    IS8DIG
    jne .n_int1
    mov rax, [r13]
    PARSE8
    imul r9, r9, 100000000
    add r9, rax
    add r11d, 8
    add r13, 8
    jmp .n_int8
.n_int1:
    movzx eax, byte [r13]
    sub eax, '0'
    cmp eax, 9
    ja .n_frac_chk
    cmp r11d, 19
    jae .n_int_extra
    lea r9, [r9+r9*4]
    lea r9, [rax+r9*2]
    inc r11d
    inc r13
    jmp .n_int1
.n_int_extra:                     ; digit past the 19 in r9; block stays 16 bytes
    jmp strict near .n_int20
    times 11 nop

.n_frac_chk:
    cmp byte [r13], '.'
    jne .n_exp_chk
    inc r13
    or r8d, 2
    movzx eax, byte [r13]
    sub eax, '0'
    cmp eax, 9
    ja .e_number
    test r11d, r11d
    jnz .n_frac8
.n_fz:
    cmp byte [r13], '0'
    jne .n_frac8
    dec rsi
    inc r13
    jmp .n_fz
.n_frac8:
    mov rax, r14
    sub rax, r13
    cmp rax, 8
    jb .n_frac1
    cmp r11d, 11
    ja .n_frac1
    mov rax, [r13]
    IS8DIG
    jne .n_frac1
    mov rax, [r13]
    PARSE8
    imul r9, r9, 100000000
    add r9, rax
    add r11d, 8
    sub rsi, 8
    add r13, 8
    jmp .n_frac8
.n_frac1:
    movzx eax, byte [r13]
    sub eax, '0'
    cmp eax, 9
    ja .n_exp_chk
    cmp r11d, 19
    jae .n_frac_extra
    lea r9, [r9+r9*4]
    lea r9, [rax+r9*2]
    inc r11d
    dec rsi
    inc r13
    jmp .n_frac1
.n_frac_extra:
    test eax, eax
    setnz cl
    or r8b, cl
    inc r13
    jmp .n_frac1

.n_exp_chk:
    movzx eax, byte [r13]
    or eax, 0x20
    cmp eax, 'e'
    jne .n_fin
    inc r13
    or r8d, 2
    xor ecx, ecx
    movzx eax, byte [r13]
    cmp al, '+'
    je .n_esign
    cmp al, '-'
    jne .n_edig0
    mov ecx, 1
.n_esign:
    inc r13
.n_edig0:
    movzx eax, byte [r13]
    sub eax, '0'
    cmp eax, 9
    ja .e_number
    xor edx, edx
.n_edig:
    movzx eax, byte [r13]
    sub eax, '0'
    cmp eax, 9
    ja .n_edone
    cmp edx, 100000
    jae .n_eskip
    imul edx, edx, 10
    add edx, eax
.n_eskip:
    inc r13
    jmp .n_edig
.n_edone:
    test ecx, ecx
    jz .n_eadd
    neg rdx
.n_eadd:
    add rsi, rdx

.n_fin:
    test r8d, 3
    jnz .n_float
    test rsi, rsi
    jnz .n_float
    cmp r11d, 18
    ja .n_int19
    mov rax, r9
    test edi, edi
    jz .n_mki
    neg rax
.n_mki:
    mov A1, rax
    CC jl_mk_i64
    jmp .have_chk
.n_int19:                         ; block stays 61 bytes
    jmp strict near .n_big19
    times 56 nop

.n_float:
    test r9, r9
    jz .n_fzero
    test r8d, 1
    jnz .n_el
    mov rax, 1
    shl rax, 53
    cmp r9, rax
    ja .n_el
    cmp rsi, -22
    jl .n_el
    cmp rsi, 22
    jg .n_el
    vcvtsi2sd xmm0, xmm0, r9
    lea rax, [jl_p10]
    test rsi, rsi
    js .n_div
    vmulsd xmm0, xmm0, [rax+rsi*8]
    jmp .n_sign
.n_div:
    neg rsi
    vdivsd xmm0, xmm0, [rax+rsi*8]
.n_sign:
    test edi, edi
    jz .n_mkf
    vxorpd xmm0, xmm0, [jl_signbit]
.n_mkf:
    CC jl_mk_f64
    jmp .have_chk
.n_fzero:
    vxorpd xmm0, xmm0, xmm0
    jmp .n_sign
.n_el:
    mov [rsp+32], r8              ; flags (jl_el returns bits in r8)
    mov rcx, r9
    mov rdx, rsi
    mov r11d, edi
    call jl_el
    cmp eax, 1
    jne .e_number
    test byte [rsp+32], 1         ; truncated: w + 1 must give the same double
    jz .n_el_ok
    mov [rsp+32], r8
    lea rcx, [r9+1]
    mov rdx, rsi
    mov r11d, edi
    call jl_el
    cmp eax, 1
    jne .n_strtod
    cmp r8, [rsp+32]
    jne .n_strtod
.n_el_ok:
    vmovq xmm0, r8
    CC jl_mk_f64
    jmp .have_chk
.n_strtod:
    mov A2, r13
    sub A2, r10
    mov A1, r10
    lea A3, [r12+JLC_TMPD]
    CC jl_strtod
    test eax, eax
    jnz .e_number
    vmovsd xmm0, [r12+JLC_TMPD]
    vmovq rax, xmm0
    shr rax, 52
    and eax, 0x7FF
    cmp eax, 0x7FF
    je .e_number
    CC jl_mk_f64
    jmp .have_chk
.n_notdigit:                      ; '-' without a digit; block stays 63 bytes
    jmp strict near .e_number
    times 58 nop

; ---- a value is in rax
.have_chk:
    test rax, rax
    jz .e_memory
.have:
    PUSHV
    cmp rbp, [r12+JLC_FS]
    je .done
    SKIPWS
    movzx eax, byte [r13]
    cmp qword [rbp-8], 0
    jne .in_obj
    cmp al, ','
    jne .arr_close
    inc r13
    jmp .value
.arr_close:
    cmp al, ']'
    jne .e_invalid
    inc r13
    mov A1, [rbp-16]
    add A1, [r12+JLC_VS]
    mov A2, r15
    sub A2, A1
    shr A2, 3
    mov r15, A1
    sub rbp, 16
    CC jl_mk_list
    jmp .have_chk
.in_obj:
    cmp al, ','
    jne .obj_close
    inc r13
    jmp .key
.obj_close:
    cmp al, '}'
    jne .e_invalid
    inc r13
    mov A1, [rbp-16]
    add A1, [r12+JLC_VS]
    mov A2, r15
    sub A2, A1
    shr A2, 4
    mov r15, A1
    sub rbp, 16
    CC jl_mk_dict
    jmp .have_chk

; ---- key
.key:
    SKIPWS
.key_nows:
    cmp byte [r13], '"'
    jne .e_invalid
    STRING 1
    PUSHV
    SKIPWS
    cmp byte [r13], ':'
    jne .e_invalid
    inc r13
    jmp .value

; ---- end
.done:
    SKIPWS
    cmp r13, r14
    jne .e_unexpected
    mov [r12+JLC_VSP], r15
    mov [r12+JLC_FSP], rbp
    mov eax, 1
    jmp .ret

.grow_fail:
    mov A1, [rsp+32]
    CC jl_drop
    jmp .e_memory

.e_number:
    mov eax, JL_E_NUMBER
    jmp .err_at
.e_invalid:
    mov eax, JL_E_INVALID
    jmp .err_at
.e_unexpected:
    mov eax, JL_E_UNEXPECTED
    jmp .err_at
.e_empty:
    mov eax, JL_E_EMPTY
    jmp .err_at
.e_memory:
    mov eax, JL_E_MEMORY
.err_at:
    mov [r12+JLC_ERR], eax
    mov rax, r13
    sub rax, [r12+JLC_START]
    mov [r12+JLC_ERRPOS], rax
.err_set:
    mov [r12+JLC_VSP], r15
    mov [r12+JLC_FSP], rbp
    xor eax, eax
.ret:
    vzeroupper
    add rsp, 40
    pop rsi
    pop rdi
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    ret

; ---- a clean string failed in the sink (invalid UTF-8). r13 is after its
; closing quote and the text has no other quote: walk back to the first byte.
.e_str_back:
    dec r13
.e_str_back1:
    dec r13
    cmp byte [r13], '"'
    jne .e_str_back1
    inc r13
    jmp .e_invalid

; ---- cold integer tails (kept after ret: the value loop does not move)
; 19 digits in r9, integer syntax. Up to 2^64-1 stays an int.
.n_big19:
    test edi, edi
    jnz .n_big19neg
    mov A1, r9
    CC jl_mk_u64
    jmp .have_chk
.n_big19neg:
    mov rax, r9
    neg rax
    jns .n_float                  ; below INT64_MIN
    mov A1, rax
    CC jl_mk_i64
    jmp .have_chk
; eax = digit past the 19 in r9. A positive 20-digit integer that fits in
; 64 bits and ends here stays an int; anything else continues as a float.
.n_int20:
    test rsi, rsi
    jnz .n_int_more
    test edi, edi
    jnz .n_int_more
    movzx ecx, byte [r13+1]
    lea edx, [rcx-'0']
    cmp edx, 9
    jbe .n_int_more
    cmp ecx, '.'
    je .n_int_more
    or ecx, 0x20
    cmp ecx, 'e'
    je .n_int_more
    mov ecx, eax
    mov rax, r9
    mov edx, 10
    mul rdx
    jc .n_int20_no
    add rax, rcx
    jc .n_int20_no
    inc r13
    mov A1, rax
    CC jl_mk_u64
    jmp .have_chk
.n_int20_no:
    mov eax, ecx
.n_int_more:
    inc rsi
    test eax, eax
    setnz cl
    or r8b, cl
    inc r13
    jmp .n_int1

; ---- numbers, fast path: 1..15 integer digits, an optional fraction, 19
; significant digits in all, no exponent. One- and two-digit integer parts go
; byte by byte (short counters and indexes; their length is predictable),
; longer digit runs as 16-byte vectors (no branch per digit). Fills the .num
; registers and joins it at .n_fin; anything else is parsed by .num from the
; token start.
.nfast:
    mov rax, r14
    sub rax, r13
    cmp rax, 56                   ; the vectors read up to 49 bytes past the token start
    jb .num
    mov r10, r13
    xor edi, edi
    cmp byte [r13], '-'
    jne .nf_int
    mov edi, 1
    inc r13
.nf_int:
    movzx r9d, byte [r13]
    sub r9d, '0'
    cmp r9d, 9
    ja .nf_slow
    movzx eax, byte [r13+1]
    sub eax, '0'
    cmp eax, 9
    ja .nf_int1
    test r9d, r9d
    jz .nf_slow                   ; leading zero
    movzx ecx, byte [r13+2]
    sub ecx, '0'
    cmp ecx, 9
    ja .nf_int2
    DIGRUN [r13]
    cmp ecx, 15
    ja .nf_slow
    lea rdx, [jl_nshuf]
    vpshufb xmm0, xmm0, [rdx+rcx]
    DIG16 xmm0
    mov r9, rax
    mov r11d, ecx
    add r13, rcx
    jmp .nf_int_done
.nf_int2:
    lea r9, [r9+r9*4]
    lea r9, [rax+r9*2]
    mov r11d, 2
    add r13, 2
    jmp .nf_int_done
.nf_int1:
    xor r11d, r11d
    test r9d, r9d
    setnz r11b                    ; a leading 0 is not a significant digit
    inc r13
.nf_int_done:
    xor esi, esi
    xor r8d, r8d
    movzx eax, byte [r13]
    cmp al, '.'
    je .nf_frac
    or eax, 0x20
    cmp eax, 'e'
    je .nf_slow
    jmp .n_fin
.nf_frac:
    DIGRUN [r13+1]
    lea edx, [rcx-1]
    cmp edx, 14
    ja .nf_frac16                 ; no digit, or 16
    lea edx, [rcx+r11]
    cmp edx, 19
    ja .nf_slow
    mov r11d, edx
    mov esi, ecx
    lea rdx, [jl_nshuf]
    vpshufb xmm0, xmm0, [rdx+rcx]
    lea rdx, [jl_p10u]
    imul r9, [rdx+rcx*8]
    DIG16 xmm0
    add r9, rax
    lea r13, [r13+rsi+1]
.nf_frac_done:
    neg rsi
    mov r8d, 2
    movzx eax, byte [r13]
    or eax, 0x20
    cmp eax, 'e'
    je .nf_slow
    jmp .n_fin
.nf_frac16:
    test ecx, ecx
    jz .nf_slow
    lea edx, [r11+16]
    cmp edx, 19
    ja .nf_slow
    DIG16 xmm0                    ; 16 digits; up to 3 more below
    mov rsi, rax
    DIGRUN [r13+17]
    lea edx, [rcx+r11+16]
    cmp edx, 19
    ja .nf_slow
    mov r11d, edx
    lea rdx, [jl_nshuf]
    vpshufb xmm0, xmm0, [rdx+rcx]
    lea rdx, [jl_p10u]
    imul r9, [rdx+16*8]
    add r9, rsi
    imul r9, [rdx+rcx*8]
    DIG16 xmm0
    add r9, rax
    lea r13, [r13+rcx+17]
    lea esi, [rcx+16]
    jmp .nf_frac_done
.nf_slow:
    mov r13, r10
    jmp .num

; ---- first-byte dispatch (relative offsets, position independent)
align 4
.jt:
%assign jl_i 0
%rep 256
  %if jl_i == '"'
    dd .str - .jt
  %elif jl_i == '{'
    dd .obj_open - .jt
  %elif jl_i == '['
    dd .arr_open - .jt
  %elif jl_i == '-' || (jl_i >= '0' && jl_i <= '9')
    dd .nfast - .jt
  %elif jl_i == 't'
    dd .lit_t - .jt
  %elif jl_i == 'f'
    dd .lit_f - .jt
  %elif jl_i == 'n'
    dd .lit_n - .jt
  %else
    dd .e_invalid - .jt
  %endif
  %assign jl_i jl_i+1
%endrep

%ifidn __OUTPUT_FORMAT__, elf64
section .note.GNU-stack noalloc noexec nowrite progbits
%endif
