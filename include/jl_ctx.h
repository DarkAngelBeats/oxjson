/* Shared state of the jl loads kernel (src/jl_kernel.asm) and its C sink
 * (src/jl_loads.c). Offsets are mirrored in include/jl_ctx.inc; the C side
 * checks them with static asserts. No Python types here. */
#ifndef JL_CTX_H
#define JL_CTX_H

#include <stddef.h>
#include <stdint.h>

#define JL_OK 1
#define JL_E_MEMORY (-4)
#define JL_E_INVALID (-5)
#define JL_E_NUMBER (-14)
#define JL_E_UNEXPECTED (-16)
#define JL_E_EMPTY (-18)

/* frame: 16 bytes = byte offset of the first value in vs, kind (0 array, 1 object) */
typedef struct {
    size_t vstart;
    size_t kind;
} JlFrame;

typedef struct JlCtx {
    const uint8_t *start; /*  0 */
    const uint8_t *end;   /*  8  (end[0] is a NUL sentinel) */
    void **vs;            /* 16  value stack base */
    void **vs_end;        /* 24  base + capacity */
    void **vsp;           /* 32  top (valid at kernel exit / grow calls) */
    JlFrame *fs;          /* 40 */
    JlFrame *fs_end;      /* 48 */
    JlFrame *fsp;         /* 56 */
    int32_t err;          /* 64 */
    int32_t pad0;         /* 68 */
    size_t err_pos;       /* 72 */
    const uint8_t *pos;   /* 80  hand-off position from C helpers */
    double tmpd;          /* 88  strtod result */
    uint8_t *sbuf;        /* 96  unescape scratch */
    size_t scap;          /* 104 */
    void *kc;             /* 112 key cache table (64-byte entries, see jl_loads.c) */
    size_t pad1;          /* 120 */
    void *small_vs[256];  /* 128 */
    JlFrame small_fs[64];
} JlCtx;

/* kernel: 1 = ok (vs[0] is the root), 0 = error (err / err_pos set,
 * vs[0..vsp) still owned by the caller) */
int jl_run(JlCtx *c);

/* sink, called by the kernel (platform C ABI) */
void *jl_mk_str(const uint8_t *p, size_t n, uint32_t flags);   /* bit0: has byte >= 0x80 */
/* key: returns a new reference; a key-cache hit found by the kernel is pushed
 * as a borrowed pointer with bit 0 set (jl_mk_dict and cleanup know the tag) */
void *jl_mk_key(const uint8_t *p, size_t n, uint32_t flags);   /* bit0 as above, bit1: 32 bytes at p readable */
void *jl_mk_i64(int64_t v);
void *jl_mk_u64(uint64_t v);
void *jl_mk_f64(double d);
void *jl_mk_lit(uint32_t which); /* JL_LIT_* */
void *jl_mk_list(void **items, size_t n);
void *jl_mk_dict(void **kv, size_t npairs);
int jl_grow_vs(JlCtx *c);
int jl_grow_fs(JlCtx *c);
/* string with an escape or a control byte: s = first content byte, seg = first
 * special byte. On success returns the object and sets c->pos after the
 * closing quote; on failure NULL with c->err / c->err_pos. */
void *jl_str_escaped(JlCtx *c, const uint8_t *s, const uint8_t *seg, uint32_t flags); /* flags bit0: high byte seen, bit1: key */
int jl_strtod(const uint8_t *p, size_t n, double *out);

#define JL_LIT_NULL 0
#define JL_LIT_TRUE 1
#define JL_LIT_FALSE 2
#define JL_LIT_EMPTY_DICT 6
#define JL_LIT_EMPTY_LIST 7

#endif
