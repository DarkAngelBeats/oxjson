; Offsets of JlCtx (include/jl_ctx.h). The C side static-asserts them.
JLC_START   equ 0
JLC_END     equ 8
JLC_VS      equ 16
JLC_VSEND   equ 24
JLC_VSP     equ 32
JLC_FS      equ 40
JLC_FSEND   equ 48
JLC_FSP     equ 56
JLC_ERR     equ 64
JLC_ERRPOS  equ 72
JLC_POS     equ 80
JLC_TMPD    equ 88
JLC_KC      equ 112
JL_KC_BITS  equ 11

JL_E_MEMORY     equ -4
JL_E_INVALID    equ -5
JL_E_NUMBER     equ -14
JL_E_UNEXPECTED equ -16
JL_E_EMPTY      equ -18

JL_LIT_NULL       equ 0
JL_LIT_TRUE       equ 1
JL_LIT_FALSE      equ 2
JL_LIT_EMPTY_DICT equ 6
JL_LIT_EMPTY_LIST equ 7
