/*
 * Tiny Code Generator for QEMU
 *
 * Copyright (c) 2008 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/* define it to use liveness analysis (better code) */
#define USE_LIVENESS_ANALYSIS
#define USE_TCG_OPTIMIZATIONS

#include "config.h"

/* Define to jump the ELF file used to communicate with GDB.  */
#undef DEBUG_JIT

#if !defined(CONFIG_DEBUG_TCG) && !defined(NDEBUG)
/* define it to suppress various consistency checks (faster) */
#define NDEBUG
#endif

#include "qemu-common.h"
#include "qemu/host-utils.h"
#include "qemu/timer.h"

/* Note: the long term plan is to reduce the dependencies on the QEMU
   CPU definitions. Currently they are used for qemu_ld/st
   instructions */
#define NO_CPU_IO_DEFS
#include "cpu.h"

#include "tcg-op.h"

#if UINTPTR_MAX == UINT32_MAX
# define ELF_CLASS  ELFCLASS32
#else
# define ELF_CLASS  ELFCLASS64
#endif
#ifdef HOST_WORDS_BIGENDIAN
# define ELF_DATA   ELFDATA2MSB
#else
# define ELF_DATA   ELFDATA2LSB
#endif

#include "elf.h"

/* Forward declarations for functions declared in tcg-target.c and used here. */
static void tcg_target_init(TCGContext *s);
static void tcg_target_qemu_prologue(TCGContext *s);
static void patch_reloc(tcg_insn_unit *code_ptr, int type,
                        intptr_t value, intptr_t addend);

/* The CIE and FDE header definitions will be common to all hosts.  */
typedef struct {
    //uint32_t QEMU_ALIGN(sizeof(void *), len);
    uint32_t QEMU_ALIGN(8, len);
    uint32_t id;
    uint8_t version;
    char augmentation[1];
    uint8_t code_align;
    uint8_t data_align;
    uint8_t return_column;
} DebugFrameCIE;

QEMU_PACK( typedef struct {
//  uint32_t QEMU_ALIGN(sizeof(void *), len);
    uint32_t QEMU_ALIGN(8, len);
    uint32_t cie_offset;
    uintptr_t func_start;
    uintptr_t func_len;
}) DebugFrameFDEHeader;

QEMU_PACK( typedef struct {
    DebugFrameCIE cie;
    DebugFrameFDEHeader fde;
}) DebugFrameHeader;

/* Forward declarations for functions declared and used in tcg-target.c. */
static int target_parse_constraint(TCGArgConstraint *ct, const char **pct_str);
static void tcg_out_ld(TCGContext *s, TCGType type, TCGReg ret, TCGReg arg1,
                       intptr_t arg2);
static void tcg_out_mov(TCGContext *s, TCGType type, TCGReg ret, TCGReg arg);
static void tcg_out_movi(TCGContext *s, TCGType type,
                         TCGReg ret, tcg_target_long arg);
static void tcg_out_op(TCGContext *s, TCGOpcode opc, const TCGArg *args,
                       const int *const_args);
static void tcg_out_st(TCGContext *s, TCGType type, TCGReg arg, TCGReg arg1,
                       intptr_t arg2);
static void tcg_out_call(TCGContext *s, tcg_insn_unit *target);
static int tcg_target_const_match(tcg_target_long val, TCGType type,
                                  const TCGArgConstraint *arg_ct);
static void tcg_out_tb_init(TCGContext *s);
static void tcg_out_tb_finalize(TCGContext *s);


TCGOpDef tcg_op_defs_org[] = {
#define DEF(s, oargs, iargs, cargs, flags) { #s, oargs, iargs, cargs, iargs + oargs + cargs, flags },
#include "tcg-opc.h"
#undef DEF
};

#if TCG_TARGET_INSN_UNIT_SIZE == 1
static QEMU_UNUSED_FUNC inline void tcg_out8(TCGContext *s, uint8_t v)
{
    *s->code_ptr++ = v;
}

static QEMU_UNUSED_FUNC inline void tcg_patch8(tcg_insn_unit *p,
                                                      uint8_t v)
{
    *p = v;
}
#endif

#if TCG_TARGET_INSN_UNIT_SIZE <= 2
static QEMU_UNUSED_FUNC inline void tcg_out16(TCGContext *s, uint16_t v)
{
    if (TCG_TARGET_INSN_UNIT_SIZE == 2) {
        *s->code_ptr++ = (tcg_insn_unit)v;
    } else {
        tcg_insn_unit *p = s->code_ptr;
        memcpy(p, &v, sizeof(v));
        s->code_ptr = p + (2 / TCG_TARGET_INSN_UNIT_SIZE);
    }
}

static QEMU_UNUSED_FUNC inline void tcg_patch16(tcg_insn_unit *p,
                                                       uint16_t v)
{
    if (TCG_TARGET_INSN_UNIT_SIZE == 2) {
        *p = (tcg_insn_unit)v;
    } else {
        memcpy(p, &v, sizeof(v));
    }
}
#endif

#if TCG_TARGET_INSN_UNIT_SIZE <= 4
static QEMU_UNUSED_FUNC inline void tcg_out32(TCGContext *s, uint32_t v)
{
    if (TCG_TARGET_INSN_UNIT_SIZE == 4) {
        *s->code_ptr++ = v;
    } else {
        tcg_insn_unit *p = s->code_ptr;
        memcpy(p, &v, sizeof(v));
        s->code_ptr = p + (4 / TCG_TARGET_INSN_UNIT_SIZE);
    }
}

static QEMU_UNUSED_FUNC inline void tcg_patch32(tcg_insn_unit *p,
                                                       uint32_t v)
{
    if (TCG_TARGET_INSN_UNIT_SIZE == 4) {
        *p = v;
    } else {
        memcpy(p, &v, sizeof(v));
    }
}
#endif

#if TCG_TARGET_INSN_UNIT_SIZE <= 8
static QEMU_UNUSED_FUNC inline void tcg_out64(TCGContext *s, uint64_t v)
{
    if (TCG_TARGET_INSN_UNIT_SIZE == 8) {
        *s->code_ptr++ = (tcg_insn_unit)v;
    } else {
        tcg_insn_unit *p = s->code_ptr;
        memcpy(p, &v, sizeof(v));
        s->code_ptr = p + (8 / TCG_TARGET_INSN_UNIT_SIZE);
    }
}

static QEMU_UNUSED_FUNC inline void tcg_patch64(tcg_insn_unit *p,
                                                       uint64_t v)
{
    if (TCG_TARGET_INSN_UNIT_SIZE == 8) {
        *p = (tcg_insn_unit)v;
    } else {
        memcpy(p, &v, sizeof(v));
    }
}
#endif

/* label relocation processing */

static void tcg_out_reloc(TCGContext *s, tcg_insn_unit *code_ptr, int type,
                          int label_index, intptr_t addend)
{
    TCGLabel *l;
    TCGRelocation *r;

    l = &s->labels[label_index];
    if (l->has_value) {
        /* FIXME: This may break relocations on RISC targets that
           modify instruction fields in place.  The caller may not have
           written the initial value.  */
        patch_reloc(code_ptr, type, l->u.value, addend);
    } else {
        /* add a new relocation entry */
        r = tcg_malloc(s, sizeof(TCGRelocation));
        r->type = type;
        r->ptr = code_ptr;
        r->addend = addend;
        r->next = l->u.first_reloc;
        l->u.first_reloc = r;
    }
}

static void tcg_out_label(TCGContext *s, int label_index, tcg_insn_unit *ptr)
{
    TCGLabel *l = &s->labels[label_index];
    intptr_t value = (intptr_t)ptr;
    TCGRelocation *r;

    assert(!l->has_value);

    for (r = l->u.first_reloc; r != NULL; r = r->next) {
        patch_reloc(r->ptr, r->type, value, r->addend);
    }

    l->has_value = 1;
    l->u.value_ptr = ptr;
}

int gen_new_label(TCGContext *s)
{
    int idx;
    TCGLabel *l;

    if (s->nb_labels >= TCG_MAX_LABELS)
        tcg_abort();
    idx = s->nb_labels++;
    l = &s->labels[idx];
    l->has_value = 0;
    l->u.first_reloc = NULL;
    return idx;
}

#include "tcg-target.c"

/* pool based memory allocation */
void *tcg_malloc_internal(TCGContext *s, int size)
{
    TCGPool *p;
    int pool_size;

    if (size > TCG_POOL_CHUNK_SIZE) {
        /* big malloc: insert a new pool (XXX: could optimize) */
        p = g_malloc(sizeof(TCGPool) + size);
        p->size = size;
        p->next = s->pool_first_large;
        s->pool_first_large = p;
        return p->data;
    } else {
        p = s->pool_current;
        if (!p) {
            p = s->pool_first;
            if (!p)
                goto new_pool;
        } else {
            if (!p->next) {
            new_pool:
                pool_size = TCG_POOL_CHUNK_SIZE;
                p = g_malloc(sizeof(TCGPool) + pool_size);
                p->size = pool_size;
                p->next = NULL;
                if (s->pool_current)
                    s->pool_current->next = p;
                else
                    s->pool_first = p;
            } else {
                p = p->next;
            }
        }
    }
    s->pool_current = p;
    s->pool_cur = p->data + size;
    s->pool_end = p->data + p->size;
    return p->data;
}

void tcg_pool_reset(TCGContext *s)
{
    TCGPool *p, *t;
    for (p = s->pool_first_large; p; p = t) {
        t = p->next;
        g_free(p);
    }
    s->pool_first_large = NULL;
    s->pool_cur = s->pool_end = NULL;
    s->pool_current = NULL;
}

typedef struct TCGHelperInfo {
    void *func;
    const char *name;
    unsigned flags;
    unsigned sizemask;
} TCGHelperInfo;

#include "exec/helper-proto.h"

static const TCGHelperInfo all_helpers[] = {
#include "exec/helper-tcg.h"
};



void tcg_context_init(TCGContext *s)
{
    int op, total_args, n, i;
    TCGOpDef *def;
    TCGArgConstraint *args_ct;
    int *sorted_args;
    GHashTable *helper_table;

    memset(s, 0, sizeof(*s));
    s->nb_globals = 0;

    // copy original tcg_op_defs_org for private usage
    s->tcg_op_defs = g_malloc(sizeof(tcg_op_defs_org));
    memcpy(s->tcg_op_defs, tcg_op_defs_org, sizeof(tcg_op_defs_org));

    /* Count total number of arguments and allocate the corresponding
       space */
    total_args = 0;
    for(op = 0; op < NB_OPS; op++) {
        def = &s->tcg_op_defs[op];
        n = def->nb_iargs + def->nb_oargs;
        total_args += n;
    }

    args_ct = g_malloc(sizeof(TCGArgConstraint) * total_args);
    sorted_args = g_malloc(sizeof(int) * total_args);

    for(op = 0; op < NB_OPS; op++) {
        def = &s->tcg_op_defs[op];
        def->args_ct = args_ct;
        def->sorted_args = sorted_args;
        n = def->nb_iargs + def->nb_oargs;
        sorted_args += n;
        args_ct += n;
    }

    /* Register helpers.  */
    /* Use g_direct_hash/equal for direct pointer comparisons on func.  */
    s->helpers = helper_table = g_hash_table_new(NULL, NULL);

    for (i = 0; i < ARRAY_SIZE(all_helpers); ++i) {
        g_hash_table_insert(helper_table, (gpointer)all_helpers[i].func,
                            (gpointer)&all_helpers[i]);
    }

    tcg_target_init(s);
}

void tcg_prologue_init(TCGContext *s)
{
    /* init global prologue and epilogue */
    s->code_buf = s->code_gen_prologue;
    s->code_ptr = s->code_buf;
    tcg_target_qemu_prologue(s);
    flush_icache_range((uintptr_t)s->code_buf, (uintptr_t)s->code_ptr);

#ifdef DEBUG_DISAS
    if (qemu_loglevel_mask(CPU_LOG_TB_OUT_ASM)) {
        size_t size = tcg_current_code_size(s);
        qemu_log("PROLOGUE: [size=%zu]\n", size);
        qemu_log("\n");
        qemu_log_flush();
    }
#endif
}

void tcg_set_frame(TCGContext *s, int reg, intptr_t start, intptr_t size)
{
    s->frame_start = start;
    s->frame_end = start + size;
    s->frame_reg = reg;
}

void tcg_func_start(TCGContext *s)
{
    tcg_pool_reset(s);
    s->nb_temps = s->nb_globals;

    /* No temps have been previously allocated for size or locality.  */
    memset(s->free_temps, 0, sizeof(s->free_temps));

    s->nb_labels = 0;
    s->current_frame_offset = s->frame_start;

#ifdef CONFIG_DEBUG_TCG
    s->goto_tb_issue_mask = 0;
#endif

	s->gen_first_op_idx = 0;
	s->gen_last_op_idx = -1;
	s->gen_next_op_idx = 0;

    s->be = tcg_malloc(s, sizeof(TCGBackendData));
}

static inline void tcg_temp_alloc(TCGContext *s, int n)
{
    if (n > TCG_MAX_TEMPS)
        tcg_abort();
}

static inline int tcg_global_reg_new_internal(TCGContext *s, TCGType type, int reg,
                                              const char *name)
{
    TCGTemp *ts;
    int idx;

#if TCG_TARGET_REG_BITS == 32
    if (type != TCG_TYPE_I32)
        tcg_abort();
#endif
    if (tcg_regset_test_reg(s->reserved_regs, reg))
        tcg_abort();
    idx = s->nb_globals;
    tcg_temp_alloc(s, s->nb_globals + 1);
    ts = &s->temps[s->nb_globals];
    ts->base_type = type;
    ts->type = type;
    ts->fixed_reg = 1;
    ts->reg = reg;
    ts->name = name;
    s->nb_globals++;
    tcg_regset_set_reg(s->reserved_regs, reg);
    return idx;
}

TCGv_i32 tcg_global_reg_new_i32(TCGContext *s, int reg, const char *name)
{
    int idx;

    idx = tcg_global_reg_new_internal(s, TCG_TYPE_I32, reg, name);
    return MAKE_TCGV_I32(idx);
}

TCGv_i64 tcg_global_reg_new_i64(TCGContext *s, int reg, const char *name)
{
    int idx;

    idx = tcg_global_reg_new_internal(s, TCG_TYPE_I64, reg, name);
    return MAKE_TCGV_I64(idx);
}

static inline int tcg_global_mem_new_internal(TCGContext *s, TCGType type, int reg,
                                              intptr_t offset,
                                              const char *name)
{
    TCGTemp *ts;
    int idx;

    idx = s->nb_globals;
#if TCG_TARGET_REG_BITS == 32
    if (type == TCG_TYPE_I64) {
        char buf[64];
        tcg_temp_alloc(s, s->nb_globals + 2);
        ts = &s->temps[s->nb_globals];
        ts->base_type = type;
        ts->type = TCG_TYPE_I32;
        ts->fixed_reg = 0;
        ts->mem_allocated = 1;
        ts->mem_reg = reg;
#ifdef HOST_WORDS_BIGENDIAN
        ts->mem_offset = offset + 4;
#else
        ts->mem_offset = offset;
#endif
        pstrcpy(buf, sizeof(buf), name);
        pstrcat(buf, sizeof(buf), "_0");
        ts->name = g_strdup(buf);
        ts++;

        ts->base_type = type;
        ts->type = TCG_TYPE_I32;
        ts->fixed_reg = 0;
        ts->mem_allocated = 1;
        ts->mem_reg = reg;
#ifdef HOST_WORDS_BIGENDIAN
        ts->mem_offset = offset;
#else
        ts->mem_offset = offset + 4;
#endif
        pstrcpy(buf, sizeof(buf), name);
        pstrcat(buf, sizeof(buf), "_1");
        ts->name = g_strdup(buf);

        s->nb_globals += 2;
    } else
#endif
    {
        tcg_temp_alloc(s, s->nb_globals + 1);
        ts = &s->temps[s->nb_globals];
        ts->base_type = type;
        ts->type = type;
        ts->fixed_reg = 0;
        ts->mem_allocated = 1;
        ts->mem_reg = reg;
        ts->mem_offset = offset;
        ts->name = name;
        s->nb_globals++;
    }
    return idx;
}

TCGv_i32 tcg_global_mem_new_i32(TCGContext *s, int reg, intptr_t offset, const char *name)
{
    int idx = tcg_global_mem_new_internal(s, TCG_TYPE_I32, reg, offset, name);
    return MAKE_TCGV_I32(idx);
}

TCGv_i64 tcg_global_mem_new_i64(TCGContext *s, int reg, intptr_t offset, const char *name)
{
    int idx = tcg_global_mem_new_internal(s, TCG_TYPE_I64, reg, offset, name);
    return MAKE_TCGV_I64(idx);
}

static inline int tcg_temp_new_internal(TCGContext *s, TCGType type, int temp_local)
{
    TCGTemp *ts;
    int idx, k;

    k = type + (temp_local ? TCG_TYPE_COUNT : 0);
    idx = find_first_bit(s->free_temps[k].l, TCG_MAX_TEMPS);
    if (idx < TCG_MAX_TEMPS) {
        /* There is already an available temp with the right type.  */
        clear_bit(idx, s->free_temps[k].l);

        ts = &s->temps[idx];
        ts->temp_allocated = 1;
        assert(ts->base_type == type);
        assert(ts->temp_local == temp_local);
    } else {
        idx = s->nb_temps;
#if TCG_TARGET_REG_BITS == 32
        if (type == TCG_TYPE_I64) {
            tcg_temp_alloc(s, s->nb_temps + 2);
            ts = &s->temps[s->nb_temps];
            ts->base_type = type;
            ts->type = TCG_TYPE_I32;
            ts->temp_allocated = 1;
            ts->temp_local = temp_local;
            ts->name = NULL;
            ts++;
            ts->base_type = type;
            ts->type = TCG_TYPE_I32;
            ts->temp_allocated = 1;
            ts->temp_local = temp_local;
            ts->name = NULL;
            s->nb_temps += 2;
        } else
#endif
        {
            tcg_temp_alloc(s, s->nb_temps + 1);
            ts = &s->temps[s->nb_temps];
            ts->base_type = type;
            ts->type = type;
            ts->temp_allocated = 1;
            ts->temp_local = temp_local;
            ts->name = NULL;
            s->nb_temps++;
        }
    }

#if defined(CONFIG_DEBUG_TCG)
    s->temps_in_use++;
#endif
    return idx;
}

TCGv_i32 tcg_temp_new_internal_i32(TCGContext *s, int temp_local)
{
    int idx;

    idx = tcg_temp_new_internal(s, TCG_TYPE_I32, temp_local);
    return MAKE_TCGV_I32(idx);
}

TCGv_i64 tcg_temp_new_internal_i64(TCGContext *s, int temp_local)
{
    int idx;

    idx = tcg_temp_new_internal(s, TCG_TYPE_I64, temp_local);
    return MAKE_TCGV_I64(idx);
}

static void tcg_temp_free_internal(TCGContext *s, int idx)
{
    TCGTemp *ts;
    int k;

#if defined(CONFIG_DEBUG_TCG)
    s->temps_in_use--;
    if (s->temps_in_use < 0) {
        fprintf(stderr, "More temporaries freed than allocated!\n");
    }
#endif

    assert(idx >= s->nb_globals && idx < s->nb_temps);
    ts = &s->temps[idx];
    assert(ts->temp_allocated != 0);
    ts->temp_allocated = 0;

    k = ts->base_type + (ts->temp_local ? TCG_TYPE_COUNT : 0);
    set_bit(idx, s->free_temps[k].l);
}

void tcg_temp_free_i32(TCGContext *s, TCGv_i32 arg)
{
    tcg_temp_free_internal(s, GET_TCGV_I32(arg));
}

void tcg_temp_free_i64(TCGContext *s, TCGv_i64 arg)
{
    tcg_temp_free_internal(s, GET_TCGV_I64(arg));
}

TCGv_i32 tcg_const_i32(TCGContext *s, int32_t val)
{
    TCGv_i32 t0;
    t0 = tcg_temp_new_i32(s);
    tcg_gen_movi_i32(s, t0, val);
    return t0;
}

TCGv_i64 tcg_const_i64(TCGContext *s, int64_t val)
{
    TCGv_i64 t0;
    t0 = tcg_temp_new_i64(s);
    tcg_gen_movi_i64(s, t0, val);
    return t0;
}

TCGv_i32 tcg_const_local_i32(TCGContext *s, int32_t val)
{
    TCGv_i32 t0;
    t0 = tcg_temp_local_new_i32(s);
    tcg_gen_movi_i32(s, t0, val);
    return t0;
}

TCGv_i64 tcg_const_local_i64(TCGContext *s, int64_t val)
{
    TCGv_i64 t0;
    t0 = tcg_temp_local_new_i64(s);
    tcg_gen_movi_i64(s, t0, val);
    return t0;
}

#if defined(CONFIG_DEBUG_TCG)
void tcg_clear_temp_count(TCGContext *s)
{
    s->temps_in_use = 0;
}

int tcg_check_temp_count(TCGContext *s)
{
    if (s->temps_in_use) {
        /* Clear the count so that we don't give another
         * warning immediately next time around.
         */
        s->temps_in_use = 0;
        return 1;
    }
    return 0;
}
#endif

/* Note: we convert the 64 bit args to 32 bit and do some alignment
   and endian swap. Maybe it would be better to do the alignment
   and endian swap in tcg_reg_alloc_call(). */
void tcg_gen_callN(TCGContext *s, void *func, TCGArg ret,
                   int nargs, TCGArg *args)
{
	int i, real_args, nb_rets, pi;
	unsigned sizemask, flags;
    TCGHelperInfo *info;
	TCGOp *op;

    info = g_hash_table_lookup(s->helpers, (gpointer)func);
    flags = info->flags;
    sizemask = info->sizemask;

#if defined(__sparc__) && !defined(__arch64__) \
    && !defined(CONFIG_TCG_INTERPRETER)
    /* We have 64-bit values in one register, but need to pass as two
       separate parameters.  Split them.  */
    int orig_sizemask = sizemask;
    int orig_nargs = nargs;
    TCGv_i64 retl, reth;
	TCGArg split_args[MAX_OPC_PARAM];

    TCGV_UNUSED_I64(retl);
    TCGV_UNUSED_I64(reth);
    if (sizemask != 0) {
        for (i = real_args = 0; i < nargs; ++i) {
            int is_64bit = sizemask & (1 << (i+1)*2);
            if (is_64bit) {
                TCGv_i64 orig = MAKE_TCGV_I64(args[i]);
                TCGv_i32 h = tcg_temp_new_i32(s);
                TCGv_i32 l = tcg_temp_new_i32(s);
                tcg_gen_extr_i64_i32(l, h, orig);
                split_args[real_args++] = GET_TCGV_I32(h);
                split_args[real_args++] = GET_TCGV_I32(l);
            } else {
                split_args[real_args++] = args[i];
            }
        }
        nargs = real_args;
        args = split_args;
        sizemask = 0;
    }
#elif defined(TCG_TARGET_EXTEND_ARGS) && TCG_TARGET_REG_BITS == 64
    for (i = 0; i < nargs; ++i) {
        int is_64bit = sizemask & (1 << (i+1)*2);
        int is_signed = sizemask & (2 << (i+1)*2);
        if (!is_64bit) {
            TCGv_i64 temp = tcg_temp_new_i64(s);
            TCGv_i64 orig = MAKE_TCGV_I64(args[i]);
            if (is_signed) {
                tcg_gen_ext32s_i64(s, temp, orig);
            } else {
                tcg_gen_ext32u_i64(s, temp, orig);
            }
            args[i] = GET_TCGV_I64(temp);
        }
    }
#endif /* TCG_TARGET_EXTEND_ARGS */

	i = s->gen_next_op_idx;
	tcg_debug_assert(i < OPC_BUF_SIZE);
	s->gen_op_buf[0].prev = i;
	s->gen_next_op_idx = i + 1;
	op = &s->gen_op_buf[i];

	/* Set links for sequential allocation during translation.  */
	memset(op, 0, offsetof(TCGOp, args));
	op->opc = INDEX_op_call;
	op->prev = i - 1;
	op->next = i + 1;

	pi = 0;

    if (ret != TCG_CALL_DUMMY_ARG) {
#if defined(__sparc__) && !defined(__arch64__) \
    && !defined(CONFIG_TCG_INTERPRETER)
        if (orig_sizemask & 1) {
            /* The 32-bit ABI is going to return the 64-bit value in
               the %o0/%o1 register pair.  Prepare for this by using
               two return temporaries, and reassemble below.  */
            retl = tcg_temp_new_i64(s);
            reth = tcg_temp_new_i64(s);
			op->args[pi++] = GET_TCGV_I64(reth);
			op->args[pi++] = GET_TCGV_I64(retl);
            nb_rets = 2;
        } else {
			op->args[pi++] = ret;
            nb_rets = 1;
        }
#else
		if (TCG_TARGET_REG_BITS < 64 && (sizemask & 1)) {
#ifdef HOST_WORDS_BIGENDIAN
			op->args[pi++] = ret + 1;
			op->args[pi++] = ret;
#else
			op->args[pi++] = ret;
			op->args[pi++] = ret + 1;
#endif
			nb_rets = 2;
		}
		else {
			op->args[pi++] = ret;
			nb_rets = 1;
		}
#endif
    } else {
        nb_rets = 0;
    }
	op->callo = nb_rets;
    real_args = 0;
    for (i = 0; i < nargs; i++) {
        int is_64bit = sizemask & (1 << (i+1)*2);
        if (TCG_TARGET_REG_BITS < 64 && is_64bit) {
#ifdef TCG_TARGET_CALL_ALIGN_ARGS
            /* some targets want aligned 64 bit args */
            if (real_args & 1) {
				op->args[pi++] = TCG_CALL_DUMMY_ARG;
                real_args++;
            }
#endif
        /* If stack grows up, then we will be placing successive
           arguments at lower addresses, which means we need to
           reverse the order compared to how we would normally
           treat either big or little-endian.  For those arguments
           that will wind up in registers, this still works for
           HPPA (the only current STACK_GROWSUP target) since the
           argument registers are *also* allocated in decreasing
           order.  If another such target is added, this logic may
           have to get more complicated to differentiate between
           stack arguments and register arguments.  */
#if defined(HOST_WORDS_BIGENDIAN) != defined(TCG_TARGET_STACK_GROWSUP)
			op->args[pi++] = args[i] + 1;
			op->args[pi++] = args[i];
#else
			op->args[pi++] = args[i];
			op->args[pi++] = args[i] + 1;
#endif
            real_args += 2;
            continue;
        }

		op->args[pi++] = args[i];
		real_args++;
	}
	op->args[pi++] = (uintptr_t)func;
	op->args[pi++] = flags;
	op->calli = real_args;

	/* Make sure the fields didn't overflow.  */
	tcg_debug_assert(op->calli == real_args);
	tcg_debug_assert(pi <= ARRAY_SIZE(op->args));


#if defined(__sparc__) && !defined(__arch64__) \
    && !defined(CONFIG_TCG_INTERPRETER)
    /* Free all of the parts we allocated above.  */
    for (i = real_args = 0; i < orig_nargs; ++i) {
        int is_64bit = orig_sizemask & (1 << (i+1)*2);
        if (is_64bit) {
            TCGv_i32 h = MAKE_TCGV_I32(args[real_args++]);
            TCGv_i32 l = MAKE_TCGV_I32(args[real_args++]);
            tcg_temp_free_i32(s, h);
            tcg_temp_free_i32(s, l);
        } else {
            real_args++;
        }
    }
    if (orig_sizemask & 1) {
        /* The 32-bit ABI returned two 32-bit pieces.  Re-assemble them.
           Note that describing these as TCGv_i64 eliminates an unnecessary
           zero-extension that tcg_gen_concat_i32_i64 would create.  */
        tcg_gen_concat32_i64(MAKE_TCGV_I64(ret), retl, reth);
        tcg_temp_free_i64(s, retl);
        tcg_temp_free_i64(s, reth);
    }
#elif defined(TCG_TARGET_EXTEND_ARGS) && TCG_TARGET_REG_BITS == 64
    for (i = 0; i < nargs; ++i) {
        int is_64bit = sizemask & (1 << (i+1)*2);
        if (!is_64bit) {
            TCGv_i64 temp = MAKE_TCGV_I64(args[i]);
            tcg_temp_free_i64(s, temp);
        }
    }
#endif /* TCG_TARGET_EXTEND_ARGS */
}

// Unicorn engine
// check if the last memory access was invalid
// if so, we jump to the block epilogue to quit immediately.
void check_exit_request(TCGContext *tcg_ctx)
{
    TCGv_i32 flag;

    flag = tcg_temp_new_i32(tcg_ctx);
    tcg_gen_ld_i32(tcg_ctx, flag, tcg_ctx->cpu_env,
            offsetof(CPUState, tcg_exit_req) - ENV_OFFSET);
    tcg_gen_brcondi_i32(tcg_ctx, TCG_COND_NE, flag, 0, tcg_ctx->exitreq_label);
    tcg_temp_free_i32(tcg_ctx, flag);
}


static void tcg_reg_alloc_start(TCGContext *s)
{
    int i;
    TCGTemp *ts;
    for(i = 0; i < s->nb_globals; i++) {
        ts = &s->temps[i];
        if (ts->fixed_reg) {
            ts->val_type = TEMP_VAL_REG;
        } else {
            ts->val_type = TEMP_VAL_MEM;
        }
    }
    for(i = s->nb_globals; i < s->nb_temps; i++) {
        ts = &s->temps[i];
        if (ts->temp_local) {
            ts->val_type = TEMP_VAL_MEM;
        } else {
            ts->val_type = TEMP_VAL_DEAD;
        }
        ts->mem_allocated = 0;
        ts->fixed_reg = 0;
    }
    for(i = 0; i < TCG_TARGET_NB_REGS; i++) {
        s->reg_to_temp[i] = -1;
    }
}

static char *tcg_get_arg_str_idx(TCGContext *s, char *buf, int buf_size,
                                 int idx)
{
    TCGTemp *ts;

    assert(idx >= 0 && idx < s->nb_temps);
    ts = &s->temps[idx];
    if (idx < s->nb_globals) {
        pstrcpy(buf, buf_size, ts->name);
    } else {
        if (ts->temp_local)
            snprintf(buf, buf_size, "loc%d", idx - s->nb_globals);
        else
            snprintf(buf, buf_size, "tmp%d", idx - s->nb_globals);
    }
    return buf;
}

char *tcg_get_arg_str_i32(TCGContext *s, char *buf, int buf_size, TCGv_i32 arg)
{
    return tcg_get_arg_str_idx(s, buf, buf_size, GET_TCGV_I32(arg));
}

char *tcg_get_arg_str_i64(TCGContext *s, char *buf, int buf_size, TCGv_i64 arg)
{
    return tcg_get_arg_str_idx(s, buf, buf_size, GET_TCGV_I64(arg));
}

/* Find helper name.  */
static inline const char *tcg_find_helper(TCGContext *s, uintptr_t val)
{
    const char *ret = NULL;
    if (s->helpers) {
        TCGHelperInfo *info = g_hash_table_lookup(s->helpers, (gpointer)val);
        if (info) {
            ret = info->name;
        }
    }
    return ret;
}

static const char * const cond_name[] =
{
#ifdef _MSC_VER
    "never",	// TCG_COND_NEVER
    "always",	// TCG_COND_ALWAYS
    "lt",		// TCG_COND_LT
    "ge",		// TCG_COND_GE
    "ltu",		// TCG_COND_LTU
    "geu",		// TCG_COND_GEU
    NULL,		// n/a
    NULL,		// n/a
    "eq",		// TCG_COND_EQ
    "ne",		// TCG_COND_NE
    "le",		// TCG_COND_LE
    "gt",		// TCG_COND_GT
    "leu",		// TCG_COND_LEU
    "gtu",		// TCG_COND_GTU
    NULL,		// n/a
    NULL,		// n/a
#else
    [TCG_COND_NEVER] = "never",
    [TCG_COND_ALWAYS] = "always",
    [TCG_COND_EQ] = "eq",
    [TCG_COND_NE] = "ne",
    [TCG_COND_LT] = "lt",
    [TCG_COND_GE] = "ge",
    [TCG_COND_LE] = "le",
    [TCG_COND_GT] = "gt",
    [TCG_COND_LTU] = "ltu",
    [TCG_COND_GEU] = "geu",
    [TCG_COND_LEU] = "leu",
    [TCG_COND_GTU] = "gtu"
#endif
};

static const char * const ldst_name[] =
{
#ifdef _MSC_VER
    "ub",	// MO_UB
#  ifdef HOST_WORDS_BIGENDIAN
    "beuw",		// MO_BEUW
    "beul",		// MO_BEUL
    "beq",		// MO_BEQ
    "sb",		// MO_SB
    "besw",		// MO_BESW
    "besl",		// MO_BESL
    NULL,		// n/a
    NULL,		// n/a
    "leuw",		// MO_LEUW
    "leul",		// MO_LEUL
    "leq",		// MO_LEQ
    NULL,		// n/a
    "lesw",		// MO_LESW
    "lesl",		// MO_LESL
    NULL,		// n/a
#  else // !HOST_WORDS_BIGENDIAN
    "leuw",		// MO_LEUW
    "leul",		// MO_LEUL
    "leq",		// MO_LEQ
    "sb",		// MO_SB
    "lesw",		// MO_LESW
    "lesl",		// MO_LESL
    NULL,		// n/a
    NULL,		// n/a
    "beuw",		// MO_BEUW
    "beul",		// MO_BEUL
    "beq",		// MO_BEQ
    NULL,		// n/a
    "besw",		// MO_BESW
    "besl",		// MO_BESL
    NULL,		// n/a
#  endif // HOST_WORDS_BIGENDIAN

#else //_MSC_VER
    [MO_UB]   = "ub",
    [MO_SB]   = "sb",
    [MO_LEUW] = "leuw",
    [MO_LESW] = "lesw",
    [MO_LEUL] = "leul",
    [MO_LESL] = "lesl",
    [MO_LEQ]  = "leq",
    [MO_BEUW] = "beuw",
    [MO_BESW] = "besw",
    [MO_BEUL] = "beul",
    [MO_BESL] = "besl",
    [MO_BEQ]  = "beq",
#endif // _MSC_VER
};

void tcg_dump_ops(TCGContext *s)
{
	char buf[128];
	TCGOp *op;
	int oi;

	for (oi = s->gen_first_op_idx; oi >= 0; oi = op->next) {
		int i, k, nb_oargs, nb_iargs, nb_cargs;
		const TCGOpDef *def;
		const TCGArg *args;
		TCGOpcode c;

		op = &s->gen_op_buf[oi];
		c = op->opc;
		def = &s->tcg_op_defs[c];
		args = op->args;

		if (c == INDEX_op_debug_insn_start) {
			uint64_t pc;
#if TARGET_LONG_BITS > TCG_TARGET_REG_BITS
			pc = ((uint64_t)args[1] << 32) | args[0];
#else
			pc = args[0];
#endif
			if (oi != s->gen_first_op_idx) {
				qemu_log("\n");
			}
			qemu_log(" ---- 0x%" PRIx64, pc);
		}
		else if (c == INDEX_op_call) {
			/* variable number of arguments */
			nb_oargs = op->callo;
			nb_iargs = op->calli;
			nb_cargs = def->nb_cargs;

			/* function name, flags, out args */
			qemu_log(" %s %s,$0x%" TCG_PRIlx ",$%d", def->name,
				tcg_find_helper(s, args[nb_oargs + nb_iargs]),
				args[nb_oargs + nb_iargs + 1], nb_oargs);
			for (i = 0; i < nb_oargs; i++) {
				qemu_log(",%s", tcg_get_arg_str_idx(s, buf, sizeof(buf),
					args[i]));
			}
			for (i = 0; i < nb_iargs; i++) {
				TCGArg arg = args[nb_oargs + i];
				const char *t = "<dummy>";
				if (arg != TCG_CALL_DUMMY_ARG) {
					t = tcg_get_arg_str_idx(s, buf, sizeof(buf), arg);
				}
				qemu_log(",%s", t);
			}
		}
		else {
			qemu_log(" %s ", def->name);

			nb_oargs = def->nb_oargs;
			nb_iargs = def->nb_iargs;
			nb_cargs = def->nb_cargs;

			k = 0;
			for (i = 0; i < nb_oargs; i++) {
				if (k != 0) {
					qemu_log(",");
				}
				qemu_log("%s", tcg_get_arg_str_idx(s, buf, sizeof(buf),
					args[k++]));
			}
			for (i = 0; i < nb_iargs; i++) {
				if (k != 0) {
					qemu_log(",");
				}
				qemu_log("%s", tcg_get_arg_str_idx(s, buf, sizeof(buf),
					args[k++]));
			}
			switch (c) {
			case INDEX_op_brcond_i32:
			case INDEX_op_setcond_i32:
			case INDEX_op_movcond_i32:
			case INDEX_op_brcond2_i32:
			case INDEX_op_setcond2_i32:
			case INDEX_op_brcond_i64:
			case INDEX_op_setcond_i64:
			case INDEX_op_movcond_i64:
				if (args[k] < ARRAY_SIZE(cond_name) && cond_name[args[k]]) {
					qemu_log(",%s", cond_name[args[k++]]);
				}
				else {
					qemu_log(",$0x%" TCG_PRIlx, args[k++]);
				}
				i = 1;
				break;
			case INDEX_op_qemu_ld_i32:
			case INDEX_op_qemu_st_i32:
			case INDEX_op_qemu_ld_i64:
			case INDEX_op_qemu_st_i64:
				if (args[k] < ARRAY_SIZE(ldst_name) && ldst_name[args[k]]) {
					qemu_log(",%s", ldst_name[args[k++]]);
				}
				else {
					qemu_log(",$0x%" TCG_PRIlx, args[k++]);
				}
				i = 1;
				break;
			default:
				i = 0;
				break;
			}
			for (; i < nb_cargs; i++) {
				if (k != 0) {
					qemu_log(",");
				}
				qemu_log("$0x%" TCG_PRIlx, args[k++]);
			}
		}
		qemu_log("\n");
	}
}

/* we give more priority to constraints with less registers */
static int get_constraint_priority(const TCGOpDef *def, int k)
{
    const TCGArgConstraint *arg_ct;

    int i, n;
    arg_ct = &def->args_ct[k];
    if (arg_ct->ct & TCG_CT_ALIAS) {
        /* an alias is equivalent to a single register */
        n = 1;
    } else {
        if (!(arg_ct->ct & TCG_CT_REG))
            return 0;
        n = 0;
        for(i = 0; i < TCG_TARGET_NB_REGS; i++) {
            if (tcg_regset_test_reg(arg_ct->u.regs, i))
                n++;
        }
    }
    return TCG_TARGET_NB_REGS - n + 1;
}

/* sort from highest priority to lowest */
static void sort_constraints(TCGOpDef *def, int start, int n)
{
    int i, j, p1, p2, tmp;

    for(i = 0; i < n; i++)
        def->sorted_args[start + i] = start + i;
    if (n <= 1)
        return;
    for(i = 0; i < n - 1; i++) {
        for(j = i + 1; j < n; j++) {
            p1 = get_constraint_priority(def, def->sorted_args[start + i]);
            p2 = get_constraint_priority(def, def->sorted_args[start + j]);
            if (p1 < p2) {
                tmp = def->sorted_args[start + i];
                def->sorted_args[start + i] = def->sorted_args[start + j];
                def->sorted_args[start + j] = tmp;
            }
        }
    }
}

static void process_op_defs(TCGContext *s)
{
	TCGOpcode op;

	for (op = 0; op < NB_OPS; op++) {
		TCGOpDef *def = &s->tcg_op_defs[op];
		const TCGTargetOpDef *tdefs;
		TCGType type;
		int i, nb_args;

		if (def->flags & TCG_OPF_NOT_PRESENT) {
			continue;
		}

		nb_args = def->nb_iargs + def->nb_oargs;
		if (nb_args == 0) {
			continue;
		}

		tdefs = tcg_target_op_def(op);
		/* Missing TCGTargetOpDef entry. */
		tcg_debug_assert(tdefs != NULL);

		type = (def->flags & TCG_OPF_64BIT ? TCG_TYPE_I64 : TCG_TYPE_I32);
		for (i = 0; i < nb_args; i++) {
			const char *ct_str = tdefs->args_ct_str[i];
			/* Incomplete TCGTargetOpDef entry. */
			tcg_debug_assert(ct_str != NULL);

			def->args_ct[i].u.regs = 0;
			def->args_ct[i].ct = 0;
			while (*ct_str != '\0') {
				switch (*ct_str) {
				case '0' ... '9':
				{
					int oarg = *ct_str - '0';
					tcg_debug_assert(ct_str == tdefs->args_ct_str[i]);
					tcg_debug_assert(oarg < def->nb_oargs);
					tcg_debug_assert(def->args_ct[oarg].ct & TCG_CT_REG);
					/* TCG_CT_ALIAS is for the output arguments.
					The input is tagged with TCG_CT_IALIAS. */
					def->args_ct[i] = def->args_ct[oarg];
					def->args_ct[oarg].ct |= TCG_CT_ALIAS;
					def->args_ct[oarg].alias_index = i;
					def->args_ct[i].ct |= TCG_CT_IALIAS;
					def->args_ct[i].alias_index = oarg;
				}
				ct_str++;
				break;
				case '&':
					def->args_ct[i].ct |= TCG_CT_NEWREG;
					ct_str++;
					break;
				case 'i':
					def->args_ct[i].ct |= TCG_CT_CONST;
					ct_str++;
					break;
				default:
					ct_str = target_parse_constraint(&def->args_ct[i],
						ct_str, type);
					/* Typo in TCGTargetOpDef constraint. */
					tcg_debug_assert(ct_str != NULL);
				}
			}
		}

		/* TCGTargetOpDef entry with too much information? */
		tcg_debug_assert(i == TCG_MAX_OP_ARGS || tdefs->args_ct_str[i] == NULL);

		/* sort the constraints (XXX: this is just an heuristic) */
		sort_constraints(def, 0, def->nb_oargs);
		sort_constraints(def, def->nb_oargs, def->nb_iargs);
	}
}

void tcg_op_remove(TCGContext *s, TCGOp *op)
{
	int next = op->next;
	int prev = op->prev;

	/* We should never attempt to remove the list terminator.  */
	tcg_debug_assert(op != &s->gen_op_buf[0]);

	s->gen_op_buf[next].prev = prev;
	s->gen_op_buf[prev].next = next;

	memset(op, 0, sizeof(*op));

#ifdef CONFIG_PROFILER
	s->del_op_count++;
#endif
}

TCGOp *tcg_op_insert_before(TCGContext *s, TCGOp *old_op,
	TCGOpcode opc, int nargs)
{
	int oi = s->gen_next_op_idx;
	int prev = old_op->prev;
	int next = old_op - s->gen_op_buf;
	TCGOp *new_op;

	tcg_debug_assert(oi < OPC_BUF_SIZE);
	s->gen_next_op_idx = oi + 1;

	new_op = &s->gen_op_buf[oi];
	*new_op = (TCGOp) {
		.opc = opc,
			.prev = prev,
			.next = next
	};
	s->gen_op_buf[prev].next = oi;
	old_op->prev = oi;

	return new_op;
}

TCGOp *tcg_op_insert_after(TCGContext *s, TCGOp *old_op,
	TCGOpcode opc, int nargs)
{
	int oi = s->gen_next_op_idx;
	int prev = old_op - s->gen_op_buf;
	int next = old_op->next;
	TCGOp *new_op;

	tcg_debug_assert(oi < OPC_BUF_SIZE);
	s->gen_next_op_idx = oi + 1;

	new_op = &s->gen_op_buf[oi];
	*new_op = (TCGOp) {
		.opc = opc,
			.prev = prev,
			.next = next
	};
	s->gen_op_buf[next].prev = oi;
	old_op->next = oi;

	return new_op;
}

/* Liveness analysis : update the opc_arg_life array to tell if a
given input arguments is dead. Instructions updating dead
temporaries are removed. */
static void liveness_pass_1(TCGContext *s, uint8_t *temp_state)
{
	int nb_globals = s->nb_globals;
	int oi, oi_prev;

	tcg_la_func_end(s, temp_state);

	for (oi = s->gen_op_buf[0].prev; oi != 0; oi = oi_prev) {
		int i, nb_iargs, nb_oargs;
		TCGOpcode opc_new, opc_new2;
		bool have_opc_new2;
		TCGLifeData arg_life = 0;
		TCGArg arg;

		TCGOp * const op = &s->gen_op_buf[oi];
		TCGArg * const args = op->args;
		TCGOpcode opc = op->opc;
		const TCGOpDef *def = &s->tcg_op_defs[opc];

		oi_prev = op->prev;

		switch (opc) {
		case INDEX_op_call:
		{
			int call_flags;

			nb_oargs = op->callo;
			nb_iargs = op->calli;
			call_flags = args[nb_oargs + nb_iargs + 1];

			/* pure functions can be removed if their result is unused */
			if (call_flags & TCG_CALL_NO_SIDE_EFFECTS) {
				for (i = 0; i < nb_oargs; i++) {
					arg = args[i];
					if (temp_state[arg] != TS_DEAD) {
						goto do_not_remove_call;
					}
				}
				goto do_remove;
			}
			else {
			do_not_remove_call:

				/* output args are dead */
				for (i = 0; i < nb_oargs; i++) {
					arg = args[i];
					if (temp_state[arg] & TS_DEAD) {
						arg_life |= DEAD_ARG << i;
					}
					if (temp_state[arg] & TS_MEM) {
						arg_life |= SYNC_ARG << i;
					}
					temp_state[arg] = TS_DEAD;
				}

				if (!(call_flags & (TCG_CALL_NO_WRITE_GLOBALS |
					TCG_CALL_NO_READ_GLOBALS))) {
					/* globals should go back to memory */
					memset(temp_state, TS_DEAD | TS_MEM, nb_globals);
				}
				else if (!(call_flags & TCG_CALL_NO_READ_GLOBALS)) {
					/* globals should be synced to memory */
					for (i = 0; i < nb_globals; i++) {
						temp_state[i] |= TS_MEM;
					}
				}

				/* record arguments that die in this helper */
				for (i = nb_oargs; i < nb_iargs + nb_oargs; i++) {
					arg = args[i];
					if (arg != TCG_CALL_DUMMY_ARG) {
						if (temp_state[arg] & TS_DEAD) {
							arg_life |= DEAD_ARG << i;
						}
					}
				}
				/* input arguments are live for preceding opcodes */
				for (i = nb_oargs; i < nb_iargs + nb_oargs; i++) {
					arg = args[i];
					if (arg != TCG_CALL_DUMMY_ARG) {
						temp_state[arg] &= ~TS_DEAD;
					}
				}
			}
		}
		break;
		case INDEX_op_insn_start:
			break;
		case INDEX_op_discard:
			/* mark the temporary as dead */
			temp_state[args[0]] = TS_DEAD;
			break;

		case INDEX_op_add2_i32:
			opc_new = INDEX_op_add_i32;
			goto do_addsub2;
		case INDEX_op_sub2_i32:
			opc_new = INDEX_op_sub_i32;
			goto do_addsub2;
		case INDEX_op_add2_i64:
			opc_new = INDEX_op_add_i64;
			goto do_addsub2;
		case INDEX_op_sub2_i64:
			opc_new = INDEX_op_sub_i64;
		do_addsub2:
			nb_iargs = 4;
			nb_oargs = 2;
			/* Test if the high part of the operation is dead, but not
			the low part.  The result can be optimized to a simple
			add or sub.  This happens often for x86_64 guest when the
			cpu mode is set to 32 bit.  */
			if (temp_state[args[1]] == TS_DEAD) {
				if (temp_state[args[0]] == TS_DEAD) {
					goto do_remove;
				}
				/* Replace the opcode and adjust the args in place,
				leaving 3 unused args at the end.  */
				op->opc = opc = opc_new;
				args[1] = args[2];
				args[2] = args[4];
				/* Fall through and mark the single-word operation live.  */
				nb_iargs = 2;
				nb_oargs = 1;
			}
			goto do_not_remove;

		case INDEX_op_mulu2_i32:
			opc_new = INDEX_op_mul_i32;
			opc_new2 = INDEX_op_muluh_i32;
			have_opc_new2 = TCG_TARGET_HAS_muluh_i32;
			goto do_mul2;
		case INDEX_op_muls2_i32:
			opc_new = INDEX_op_mul_i32;
			opc_new2 = INDEX_op_mulsh_i32;
			have_opc_new2 = TCG_TARGET_HAS_mulsh_i32;
			goto do_mul2;
		case INDEX_op_mulu2_i64:
			opc_new = INDEX_op_mul_i64;
			opc_new2 = INDEX_op_muluh_i64;
			have_opc_new2 = TCG_TARGET_HAS_muluh_i64;
			goto do_mul2;
		case INDEX_op_muls2_i64:
			opc_new = INDEX_op_mul_i64;
			opc_new2 = INDEX_op_mulsh_i64;
			have_opc_new2 = TCG_TARGET_HAS_mulsh_i64;
			goto do_mul2;
		do_mul2:
			nb_iargs = 2;
			nb_oargs = 2;
			if (temp_state[args[1]] == TS_DEAD) {
				if (temp_state[args[0]] == TS_DEAD) {
					/* Both parts of the operation are dead.  */
					goto do_remove;
				}
				/* The high part of the operation is dead; generate the low. */
				op->opc = opc = opc_new;
				args[1] = args[2];
				args[2] = args[3];
			}
			else if (temp_state[args[0]] == TS_DEAD && have_opc_new2) {
				/* The low part of the operation is dead; generate the high. */
				op->opc = opc = opc_new2;
				args[0] = args[1];
				args[1] = args[2];
				args[2] = args[3];
			}
			else {
				goto do_not_remove;
			}
			/* Mark the single-word operation live.  */
			nb_oargs = 1;
			goto do_not_remove;

		default:
			/* XXX: optimize by hardcoding common cases (e.g. triadic ops) */
			nb_iargs = def->nb_iargs;
			nb_oargs = def->nb_oargs;

			/* Test if the operation can be removed because all
			its outputs are dead. We assume that nb_oargs == 0
			implies side effects */
			if (!(def->flags & TCG_OPF_SIDE_EFFECTS) && nb_oargs != 0) {
				for (i = 0; i < nb_oargs; i++) {
					if (temp_state[args[i]] != TS_DEAD) {
						goto do_not_remove;
					}
				}
			do_remove:
				tcg_op_remove(s, op);
			}
			else {
			do_not_remove:
				/* output args are dead */
				for (i = 0; i < nb_oargs; i++) {
					arg = args[i];
					if (temp_state[arg] & TS_DEAD) {
						arg_life |= DEAD_ARG << i;
					}
					if (temp_state[arg] & TS_MEM) {
						arg_life |= SYNC_ARG << i;
					}
					temp_state[arg] = TS_DEAD;
				}

				/* if end of basic block, update */
				if (def->flags & TCG_OPF_BB_END) {
					tcg_la_bb_end(s, temp_state);
				}
				else if (def->flags & TCG_OPF_SIDE_EFFECTS) {
					/* globals should be synced to memory */
					for (i = 0; i < nb_globals; i++) {
						temp_state[i] |= TS_MEM;
					}
				}

				/* record arguments that die in this opcode */
				for (i = nb_oargs; i < nb_oargs + nb_iargs; i++) {
					arg = args[i];
					if (temp_state[arg] & TS_DEAD) {
						arg_life |= DEAD_ARG << i;
					}
				}
				/* input arguments are live for preceding opcodes */
				for (i = nb_oargs; i < nb_oargs + nb_iargs; i++) {
					temp_state[args[i]] &= ~TS_DEAD;
				}
			}
			break;
		}
		op->life = arg_life;
	}
}

/* Liveness analysis: Convert indirect regs to direct temporaries.  */
static bool liveness_pass_2(TCGContext *s, uint8_t *temp_state)
{
	int nb_globals = s->nb_globals;
	int16_t *dir_temps;
	int i, oi, oi_next;
	bool changes = false;

	dir_temps = tcg_malloc(nb_globals * sizeof(int16_t));
	memset(dir_temps, 0, nb_globals * sizeof(int16_t));

	/* Create a temporary for each indirect global.  */
	for (i = 0; i < nb_globals; ++i) {
		TCGTemp *its = &s->temps[i];
		if (its->indirect_reg) {
			TCGTemp *dts = tcg_temp_alloc(s);
			dts->type = its->type;
			dts->base_type = its->base_type;
			dir_temps[i] = temp_idx(s, dts);
		}
	}

	memset(temp_state, TS_DEAD, nb_globals);

	for (oi = s->gen_op_buf[0].next; oi != 0; oi = oi_next) {
		TCGOp *op = &s->gen_op_buf[oi];
		TCGArg *args = op->args;
		TCGOpcode opc = op->opc;
		const TCGOpDef *def = &s->tcg_op_defs[opc];
		TCGLifeData arg_life = op->life;
		int nb_iargs, nb_oargs, call_flags;
		TCGArg arg, dir;

		oi_next = op->next;

		if (opc == INDEX_op_call) {
			nb_oargs = op->callo;
			nb_iargs = op->calli;
			call_flags = args[nb_oargs + nb_iargs + 1];
		}
		else {
			nb_iargs = def->nb_iargs;
			nb_oargs = def->nb_oargs;

			/* Set flags similar to how calls require.  */
			if (def->flags & TCG_OPF_BB_END) {
				/* Like writing globals: save_globals */
				call_flags = 0;
			}
			else if (def->flags & TCG_OPF_SIDE_EFFECTS) {
				/* Like reading globals: sync_globals */
				call_flags = TCG_CALL_NO_WRITE_GLOBALS;
			}
			else {
				/* No effect on globals.  */
				call_flags = (TCG_CALL_NO_READ_GLOBALS |
					TCG_CALL_NO_WRITE_GLOBALS);
			}
		}

		/* Make sure that input arguments are available.  */
		for (i = nb_oargs; i < nb_iargs + nb_oargs; i++) {
			arg = args[i];
			/* Note this unsigned test catches TCG_CALL_ARG_DUMMY too.  */
			if (arg < nb_globals) {
				dir = dir_temps[arg];
				if (dir != 0 && temp_state[arg] == TS_DEAD) {
					TCGTemp *its = &s->temps[arg];
					TCGOpcode lopc = (its->type == TCG_TYPE_I32
						? INDEX_op_ld_i32
						: INDEX_op_ld_i64);
					TCGOp *lop = tcg_op_insert_before(s, op, lopc, 3);
					TCGArg *largs = lop->args;

					largs[0] = dir;
					largs[1] = temp_idx(s, its->mem_base);
					largs[2] = its->mem_offset;

					/* Loaded, but synced with memory.  */
					temp_state[arg] = TS_MEM;
				}
			}
		}

		/* Perform input replacement, and mark inputs that became dead.
		No action is required except keeping temp_state up to date
		so that we reload when needed.  */
		for (i = nb_oargs; i < nb_iargs + nb_oargs; i++) {
			arg = args[i];
			if (arg < nb_globals) {
				dir = dir_temps[arg];
				if (dir != 0) {
					args[i] = dir;
					changes = true;
					if (IS_DEAD_ARG(i)) {
						temp_state[arg] = TS_DEAD;
					}
				}
			}
		}

		/* Liveness analysis should ensure that the following are
		all correct, for call sites and basic block end points.  */
		if (call_flags & TCG_CALL_NO_READ_GLOBALS) {
			/* Nothing to do */
		}
		else if (call_flags & TCG_CALL_NO_WRITE_GLOBALS) {
			for (i = 0; i < nb_globals; ++i) {
				/* Liveness should see that globals are synced back,
				that is, either TS_DEAD or TS_MEM.  */
				tcg_debug_assert(dir_temps[i] == 0
					|| temp_state[i] != 0);
			}
		}
		else {
			for (i = 0; i < nb_globals; ++i) {
				/* Liveness should see that globals are saved back,
				that is, TS_DEAD, waiting to be reloaded.  */
				tcg_debug_assert(dir_temps[i] == 0
					|| temp_state[i] == TS_DEAD);
			}
		}

		/* Outputs become available.  */
		for (i = 0; i < nb_oargs; i++) {
			arg = args[i];
			if (arg >= nb_globals) {
				continue;
			}
			dir = dir_temps[arg];
			if (dir == 0) {
				continue;
			}
			args[i] = dir;
			changes = true;

			/* The output is now live and modified.  */
			temp_state[arg] = 0;

			/* Sync outputs upon their last write.  */
			if (NEED_SYNC_ARG(i)) {
				TCGTemp *its = &s->temps[arg];
				TCGOpcode sopc = (its->type == TCG_TYPE_I32
					? INDEX_op_st_i32
					: INDEX_op_st_i64);
				TCGOp *sop = tcg_op_insert_after(s, op, sopc, 3);
				TCGArg *sargs = sop->args;

				sargs[0] = dir;
				sargs[1] = temp_idx(s, its->mem_base);
				sargs[2] = its->mem_offset;

				temp_state[arg] = TS_MEM;
			}
			/* Drop outputs that are dead.  */
			if (IS_DEAD_ARG(i)) {
				temp_state[arg] = TS_DEAD;
			}
		}
	}

	return changes;
}


void tcg_add_target_add_op_defs(TCGContext *s, const TCGTargetOpDef *tdefs)
{
    TCGOpcode op;
    TCGOpDef *def;
    const char *ct_str;
    int i, nb_args;

    for(;;) {
        if (tdefs->op == (TCGOpcode)-1)
            break;
        op = tdefs->op;
        assert((unsigned)op < NB_OPS);
        def = &s->tcg_op_defs[op];
#if defined(CONFIG_DEBUG_TCG)
        /* Duplicate entry in op definitions? */
        assert(!def->used);
        def->used = 1;
#endif
        nb_args = def->nb_iargs + def->nb_oargs;
        for(i = 0; i < nb_args; i++) {
            ct_str = tdefs->args_ct_str[i];
            /* Incomplete TCGTargetOpDef entry? */
            assert(ct_str != NULL);
            tcg_regset_clear(def->args_ct[i].u.regs);
            def->args_ct[i].ct = 0;
            if (ct_str[0] >= '0' && ct_str[0] <= '9') {
                int oarg;
                oarg = ct_str[0] - '0';
                assert(oarg < def->nb_oargs);
                assert(def->args_ct[oarg].ct & TCG_CT_REG);
                /* TCG_CT_ALIAS is for the output arguments. The input
                   argument is tagged with TCG_CT_IALIAS. */
                def->args_ct[i] = def->args_ct[oarg];
                def->args_ct[oarg].ct = TCG_CT_ALIAS;
                def->args_ct[oarg].alias_index = i;
                def->args_ct[i].ct |= TCG_CT_IALIAS;
                def->args_ct[i].alias_index = oarg;
            } else {
                for(;;) {
                    if (*ct_str == '\0')
                        break;
                    switch(*ct_str) {
                    case 'i':
                        def->args_ct[i].ct |= TCG_CT_CONST;
                        ct_str++;
                        break;
                    default:
                        if (target_parse_constraint(&def->args_ct[i], &ct_str) < 0) {
                            fprintf(stderr, "Invalid constraint '%s' for arg %d of operation '%s'\n",
                                    ct_str, i, def->name);
                            exit(1);
                        }
                    }
                }
            }
        }

        /* TCGTargetOpDef entry with too much information? */
        assert(i == TCG_MAX_OP_ARGS || tdefs->args_ct_str[i] == NULL);

        /* sort the constraints (XXX: this is just an heuristic) */
        sort_constraints(def, 0, def->nb_oargs);
        sort_constraints(def, def->nb_oargs, def->nb_iargs);

#if 0
        {
            int i;

            printf("%s: sorted=", def->name);
            for(i = 0; i < def->nb_oargs + def->nb_iargs; i++)
                printf(" %d", def->sorted_args[i]);
            printf("\n");
        }
#endif
        tdefs++;
    }

#if defined(CONFIG_DEBUG_TCG)
    i = 0;
    for (op = 0; op < ARRAY_SIZE(s->tcg_op_defs); op++) {
        const TCGOpDef *def = &s->tcg_op_defs[op];
        if (def->flags & TCG_OPF_NOT_PRESENT) {
            /* Wrong entry in op definitions? */
            if (def->used) {
                fprintf(stderr, "Invalid op definition for %s\n", def->name);
                i = 1;
            }
        } else {
            /* Missing entry in op definitions? */
            if (!def->used) {
                fprintf(stderr, "Missing op definition for %s\n", def->name);
                i = 1;
            }
        }
    }
    if (i == 1) {
        tcg_abort();
    }
#endif
}

#ifdef USE_LIVENESS_ANALYSIS

/* set a nop for an operation using 'nb_args' */
static inline void tcg_set_nop(TCGContext *s, uint16_t *opc_ptr,
                               TCGArg *args, int nb_args)
{
    if (nb_args == 0) {
        *opc_ptr = INDEX_op_nop;
    } else {
        *opc_ptr = INDEX_op_nopn;
        args[0] = nb_args;
        args[nb_args - 1] = nb_args;
    }
}

/* liveness analysis: end of function: all temps are dead, and globals
   should be in memory. */
static inline void tcg_la_func_end(TCGContext *s, uint8_t *dead_temps,
                                   uint8_t *mem_temps)
{
    memset(dead_temps, 1, s->nb_temps);
    memset(mem_temps, 1, s->nb_globals);
    memset(mem_temps + s->nb_globals, 0, s->nb_temps - s->nb_globals);
}

/* liveness analysis: end of basic block: all temps are dead, globals
   and local temps should be in memory. */
static inline void tcg_la_bb_end(TCGContext *s, uint8_t *dead_temps,
                                 uint8_t *mem_temps)
{
    int i;

    memset(dead_temps, 1, s->nb_temps);
    memset(mem_temps, 1, s->nb_globals);
    for(i = s->nb_globals; i < s->nb_temps; i++) {
        mem_temps[i] = s->temps[i].temp_local;
    }
}

/*
    Unicorn: for brcond, we should refresh liveness states for TCG globals
*/
static inline void tcg_la_br_end(TCGContext *s, uint8_t *mem_temps)
{
    int i;
    memset(mem_temps, 1, s->nb_globals);
    for(i = s->nb_globals; i < s->nb_temps; i++) {
        mem_temps[i] = s->temps[i].temp_local;
    }
}

/* Liveness analysis : update the opc_dead_args array to tell if a
   given input arguments is dead. Instructions updating dead
   temporaries are removed. */
static void tcg_liveness_analysis(TCGContext *s)
{
	uint8_t *dead_temps, *mem_temps;
	int oi, oi_prev, nb_ops;

	nb_ops = s->gen_next_op_idx;
	s->op_dead_args = tcg_malloc(s,nb_ops * sizeof(uint16_t));
	s->op_sync_args = tcg_malloc(s,nb_ops * sizeof(uint8_t));

	dead_temps = tcg_malloc(s,s->nb_temps);
	mem_temps = tcg_malloc(s,s->nb_temps);
	tcg_la_func_end(s, dead_temps, mem_temps);

	for (oi = s->gen_last_op_idx; oi >= 0; oi = oi_prev) {
		int i, nb_iargs, nb_oargs;
		TCGOpcode opc_new, opc_new2;
		bool have_opc_new2;
		uint16_t dead_args;
		uint8_t sync_args;
		TCGArg arg;

		TCGOp * const op = &s->gen_op_buf[oi];
		TCGArg * const args = &s->gen_opparam_buf[op->args];
		TCGOpcode opc = op->opc;
		const TCGOpDef *def = &s->tcg_op_defs[opc];

		oi_prev = op->prev;

		switch (opc) {
		case INDEX_op_call:
		{
			int call_flags;

			nb_oargs = op->callo;
			nb_iargs = op->calli;
			call_flags = args[nb_oargs + nb_iargs + 1];

			/* pure functions can be removed if their result is unused */
			if (call_flags & TCG_CALL_NO_SIDE_EFFECTS) {
				for (i = 0; i < nb_oargs; i++) {
					arg = args[i];
					if (!dead_temps[arg] || mem_temps[arg]) {
						goto do_not_remove_call;
					}
				}
				goto do_remove;
			}
			else {
			do_not_remove_call:

				/* output args are dead */
				dead_args = 0;
				sync_args = 0;
				for (i = 0; i < nb_oargs; i++) {
					arg = args[i];
					if (dead_temps[arg]) {
						dead_args |= (1 << i);
					}
					if (mem_temps[arg]) {
						sync_args |= (1 << i);
					}
					dead_temps[arg] = 1;
					mem_temps[arg] = 0;
				}

				if (!(call_flags & TCG_CALL_NO_READ_GLOBALS)) {
					/* globals should be synced to memory */
					memset(mem_temps, 1, s->nb_globals);
				}
				if (!(call_flags & (TCG_CALL_NO_WRITE_GLOBALS |
					TCG_CALL_NO_READ_GLOBALS))) {
					/* globals should go back to memory */
					memset(dead_temps, 1, s->nb_globals);
				}

				/* input args are live */
				for (i = nb_oargs; i < nb_iargs + nb_oargs; i++) {
					arg = args[i];
					if (arg != TCG_CALL_DUMMY_ARG) {
						if (dead_temps[arg]) {
							dead_args |= (1 << i);
						}
						dead_temps[arg] = 0;
					}
				}
				s->op_dead_args[oi] = dead_args;
				s->op_sync_args[oi] = sync_args;
			}
		}
		break;
		case INDEX_op_debug_insn_start:
		case INDEX_op_nop:
		case INDEX_op_end:
			break;
		case INDEX_op_discard:
			/* mark the temporary as dead */
			dead_temps[args[0]] = 1;
			mem_temps[args[0]] = 0;
			break;

		case INDEX_op_add2_i32:
			opc_new = INDEX_op_add_i32;
			goto do_addsub2;
		case INDEX_op_sub2_i32:
			opc_new = INDEX_op_sub_i32;
			goto do_addsub2;
		case INDEX_op_add2_i64:
			opc_new = INDEX_op_add_i64;
			goto do_addsub2;
		case INDEX_op_sub2_i64:
			opc_new = INDEX_op_sub_i64;
		do_addsub2:
			nb_iargs = 4;
			nb_oargs = 2;
			/* Test if the high part of the operation is dead, but not
			the low part.  The result can be optimized to a simple
			add or sub.  This happens often for x86_64 guest when the
			cpu mode is set to 32 bit.  */
			if (dead_temps[args[1]] && !mem_temps[args[1]]) {
				if (dead_temps[args[0]] && !mem_temps[args[0]]) {
					goto do_remove;
				}
				/* Replace the opcode and adjust the args in place,
				leaving 3 unused args at the end.  */
				op->opc = opc = opc_new;
				args[1] = args[2];
				args[2] = args[4];
				/* Fall through and mark the single-word operation live.  */
				nb_iargs = 2;
				nb_oargs = 1;
			}
			goto do_not_remove;

		case INDEX_op_mulu2_i32:
			opc_new = INDEX_op_mul_i32;
			opc_new2 = INDEX_op_muluh_i32;
			have_opc_new2 = TCG_TARGET_HAS_muluh_i32;
			goto do_mul2;
		case INDEX_op_muls2_i32:
			opc_new = INDEX_op_mul_i32;
			opc_new2 = INDEX_op_mulsh_i32;
			have_opc_new2 = TCG_TARGET_HAS_mulsh_i32;
			goto do_mul2;
		case INDEX_op_mulu2_i64:
			opc_new = INDEX_op_mul_i64;
			opc_new2 = INDEX_op_muluh_i64;
			have_opc_new2 = TCG_TARGET_HAS_muluh_i64;
			goto do_mul2;
		case INDEX_op_muls2_i64:
			opc_new = INDEX_op_mul_i64;
			opc_new2 = INDEX_op_mulsh_i64;
			have_opc_new2 = TCG_TARGET_HAS_mulsh_i64;
			goto do_mul2;
		do_mul2:
			nb_iargs = 2;
			nb_oargs = 2;
			if (dead_temps[args[1]] && !mem_temps[args[1]]) {
				if (dead_temps[args[0]] && !mem_temps[args[0]]) {
					/* Both parts of the operation are dead.  */
					goto do_remove;
				}
				/* The high part of the operation is dead; generate the low. */
				op->opc = opc = opc_new;
				args[1] = args[2];
				args[2] = args[3];
			}
			else if (have_opc_new2 && dead_temps[args[0]]
				&& !mem_temps[args[0]]) {
				/* The low part of the operation is dead; generate the high. */
				op->opc = opc = opc_new2;
				args[0] = args[1];
				args[1] = args[2];
				args[2] = args[3];
			}
			else {
				goto do_not_remove;
			}
			/* Mark the single-word operation live.  */
			nb_oargs = 1;
			goto do_not_remove;

		default:
			/* XXX: optimize by hardcoding common cases (e.g. triadic ops) */
			nb_iargs = def->nb_iargs;
			nb_oargs = def->nb_oargs;

			/* Test if the operation can be removed because all
			its outputs are dead. We assume that nb_oargs == 0
			implies side effects */
			if (!(def->flags & TCG_OPF_SIDE_EFFECTS) && nb_oargs != 0) {
				for (i = 0; i < nb_oargs; i++) {
					arg = args[i];
					if (!dead_temps[arg] || mem_temps[arg]) {
						goto do_not_remove;
					}
				}
			do_remove:
				op->opc = INDEX_op_nop;
#ifdef CONFIG_PROFILER
				s->del_op_count++;
#endif
			}
			else {
			do_not_remove:
				/* output args are dead */
				dead_args = 0;
				sync_args = 0;
				for (i = 0; i < nb_oargs; i++) {
					arg = args[i];
					if (dead_temps[arg]) {
						dead_args |= (1 << i);
					}
					if (mem_temps[arg]) {
						sync_args |= (1 << i);
					}
					dead_temps[arg] = 1;
					mem_temps[arg] = 0;
				}

				/* if end of basic block, update */
				if (def->flags & TCG_OPF_BB_END) {
					tcg_la_bb_end(s, dead_temps, mem_temps);
				}
				else if (def->flags & TCG_OPF_SIDE_EFFECTS) {
					/* globals should be synced to memory */
					memset(mem_temps, 1, s->nb_globals);
				}

				/* input args are live */
				for (i = nb_oargs; i < nb_oargs + nb_iargs; i++) {
					arg = args[i];
					if (dead_temps[arg]) {
						dead_args |= (1 << i);
					}
					dead_temps[arg] = 0;
				}
				s->op_dead_args[oi] = dead_args;
				s->op_sync_args[oi] = sync_args;
				}
			break;
			}
		}
	}
#else
/* dummy liveness analysis */
static void tcg_liveness_analysis(TCGContext *s)
{
    int nb_ops;
    nb_ops = s->gen_opc_ptr - s->gen_opc_buf;

    s->op_dead_args = tcg_malloc(s, nb_ops * sizeof(uint16_t));
    memset(s->op_dead_args, 0, nb_ops * sizeof(uint16_t));
    s->op_sync_args = tcg_malloc(s, nb_ops * sizeof(uint8_t));
    memset(s->op_sync_args, 0, nb_ops * sizeof(uint8_t));
}
#endif

#ifndef NDEBUG
static void dump_regs(TCGContext *s)
{
    TCGTemp *ts;
    int i;
    char buf[64];

    for(i = 0; i < s->nb_temps; i++) {
        ts = &s->temps[i];
        printf("  %10s: ", tcg_get_arg_str_idx(s, buf, sizeof(buf), i));
        switch(ts->val_type) {
        case TEMP_VAL_REG:
            printf("%s", tcg_target_reg_names[ts->reg]);
            break;
        case TEMP_VAL_MEM:
            printf("%d(%s)", (int)ts->mem_offset, tcg_target_reg_names[ts->mem_reg]);
            break;
        case TEMP_VAL_CONST:
            printf("$0x%" TCG_PRIlx, ts->val);
            break;
        case TEMP_VAL_DEAD:
            printf("D");
            break;
        default:
            printf("???");
            break;
        }
        printf("\n");
    }

    for(i = 0; i < TCG_TARGET_NB_REGS; i++) {
        if (s->reg_to_temp[i] >= 0) {
            printf("%s: %s\n",
                   tcg_target_reg_names[i],
                   tcg_get_arg_str_idx(s, buf, sizeof(buf), s->reg_to_temp[i]));
        }
    }
}

static void check_regs(TCGContext *s)
{
    int reg, k;
    TCGTemp *ts;
    char buf[64];

    for(reg = 0; reg < TCG_TARGET_NB_REGS; reg++) {
        k = s->reg_to_temp[reg];
        if (k >= 0) {
            ts = &s->temps[k];
            if (ts->val_type != TEMP_VAL_REG ||
                ts->reg != reg) {
                printf("Inconsistency for register %s:\n",
                       tcg_target_reg_names[reg]);
                goto fail;
            }
        }
    }
    for(k = 0; k < s->nb_temps; k++) {
        ts = &s->temps[k];
        if (ts->val_type == TEMP_VAL_REG &&
            !ts->fixed_reg &&
            s->reg_to_temp[ts->reg] != k) {
                printf("Inconsistency for temp %s:\n",
                       tcg_get_arg_str_idx(s, buf, sizeof(buf), k));
        fail:
                printf("reg state:\n");
                dump_regs(s);
                tcg_abort();
        }
    }
}
#endif

static void temp_allocate_frame(TCGContext *s, int temp)
{
    TCGTemp *ts;
    ts = &s->temps[temp];
#if !(defined(__sparc__) && TCG_TARGET_REG_BITS == 64)
    /* Sparc64 stack is accessed with offset of 2047 */
    s->current_frame_offset = (s->current_frame_offset +
                               (tcg_target_long)sizeof(tcg_target_long) - 1) &
        ~(sizeof(tcg_target_long) - 1);
#endif
    if (s->current_frame_offset + (tcg_target_long)sizeof(tcg_target_long) >
        s->frame_end) {
        tcg_abort();
    }
    ts->mem_offset = s->current_frame_offset;
    ts->mem_reg = s->frame_reg;
    ts->mem_allocated = 1;
    s->current_frame_offset += sizeof(tcg_target_long);
}

/* sync register 'reg' by saving it to the corresponding temporary */
static inline void tcg_reg_sync(TCGContext *s, int reg)
{
    TCGTemp *ts;
    int temp;

    temp = s->reg_to_temp[reg];
    ts = &s->temps[temp];
    assert(ts->val_type == TEMP_VAL_REG);
    if (!ts->mem_coherent && !ts->fixed_reg) {
        if (!ts->mem_allocated) {
            temp_allocate_frame(s, temp);
        }
        tcg_out_st(s, ts->type, reg, ts->mem_reg, ts->mem_offset);
    }
    ts->mem_coherent = 1;
}

/* free register 'reg' by spilling the corresponding temporary if necessary */
static void tcg_reg_free(TCGContext *s, int reg)
{
    int temp;

    temp = s->reg_to_temp[reg];
    if (temp != -1) {
        tcg_reg_sync(s, reg);
        s->temps[temp].val_type = TEMP_VAL_MEM;
        s->reg_to_temp[reg] = -1;
    }
}

/* Allocate a register belonging to reg1 & ~reg2 */
static int tcg_reg_alloc(TCGContext *s, TCGRegSet reg1, TCGRegSet reg2)
{
    int i, reg;
    TCGRegSet reg_ct;

    tcg_regset_andnot(reg_ct, reg1, reg2);

    /* first try free registers */
    for(i = 0; i < ARRAY_SIZE(tcg_target_reg_alloc_order); i++) {
        reg = tcg_target_reg_alloc_order[i];
        if (tcg_regset_test_reg(reg_ct, reg) && s->reg_to_temp[reg] == -1)
            return reg;
    }

    /* XXX: do better spill choice */
    for(i = 0; i < ARRAY_SIZE(tcg_target_reg_alloc_order); i++) {
        reg = tcg_target_reg_alloc_order[i];
        if (tcg_regset_test_reg(reg_ct, reg)) {
            tcg_reg_free(s, reg);
            return reg;
        }
    }

    tcg_abort();
}

/* mark a temporary as dead. */
static inline void temp_dead(TCGContext *s, int temp)
{
    TCGTemp *ts;

    ts = &s->temps[temp];
    if (!ts->fixed_reg) {
        if (ts->val_type == TEMP_VAL_REG) {
            s->reg_to_temp[ts->reg] = -1;
        }
        if (temp < s->nb_globals || ts->temp_local) {
            ts->val_type = TEMP_VAL_MEM;
        } else {
            ts->val_type = TEMP_VAL_DEAD;
        }
    }
}

/* sync a temporary to memory. 'allocated_regs' is used in case a
   temporary registers needs to be allocated to store a constant. */
static inline void temp_sync(TCGContext *s, int temp, TCGRegSet allocated_regs)
{
    TCGTemp *ts;

    ts = &s->temps[temp];
    if (!ts->fixed_reg) {
        switch(ts->val_type) {
        case TEMP_VAL_CONST:
            ts->reg = tcg_reg_alloc(s, (TCGRegSet)s->tcg_target_available_regs[ts->type],
                                    allocated_regs);
            ts->val_type = TEMP_VAL_REG;
            s->reg_to_temp[ts->reg] = temp;
            ts->mem_coherent = 0;
            tcg_out_movi(s, ts->type, ts->reg, ts->val);
            /* fallthrough*/
        case TEMP_VAL_REG:
            tcg_reg_sync(s, ts->reg);
            break;
        case TEMP_VAL_DEAD:
        case TEMP_VAL_MEM:
            break;
        default:
            tcg_abort();
        }
    }
}

/* save a temporary to memory. 'allocated_regs' is used in case a
   temporary registers needs to be allocated to store a constant. */
static inline void temp_save(TCGContext *s, int temp, TCGRegSet allocated_regs)
{
#ifdef USE_LIVENESS_ANALYSIS
    /* The liveness analysis already ensures that globals are back
       in memory. Keep an assert for safety. */
    assert(s->temps[temp].val_type == TEMP_VAL_MEM || s->temps[temp].fixed_reg);
#else
    temp_sync(s, temp, allocated_regs);
    temp_dead(s, temp);
#endif
}

/* save globals to their canonical location and assume they can be
   modified be the following code. 'allocated_regs' is used in case a
   temporary registers needs to be allocated to store a constant. */
static void save_globals(TCGContext *s, TCGRegSet allocated_regs)
{
    int i;

    for(i = 0; i < s->nb_globals; i++) {
        temp_save(s, i, allocated_regs);
    }
}

/* sync globals to their canonical location and assume they can be
   read by the following code. 'allocated_regs' is used in case a
   temporary registers needs to be allocated to store a constant. */
static void sync_globals(TCGContext *s, TCGRegSet allocated_regs)
{
    int i;

    for (i = 0; i < s->nb_globals; i++) {
#ifdef USE_LIVENESS_ANALYSIS
        assert(s->temps[i].val_type != TEMP_VAL_REG || s->temps[i].fixed_reg ||
               s->temps[i].mem_coherent);
#else
        temp_sync(s, i, allocated_regs);
#endif
    }
}

/* at the end of a basic block, we assume all temporaries are dead and
   all globals are stored at their canonical location. */
static void tcg_reg_alloc_bb_end(TCGContext *s, TCGRegSet allocated_regs)
{
    TCGTemp *ts;
    int i;

    for(i = s->nb_globals; i < s->nb_temps; i++) {
        ts = &s->temps[i];
        if (ts->temp_local) {
            temp_save(s, i, allocated_regs);
        } else {
#ifdef USE_LIVENESS_ANALYSIS
            /* The liveness analysis already ensures that temps are dead.
               Keep an assert for safety. */
            assert(ts->val_type == TEMP_VAL_DEAD);
#else
            temp_dead(s, i);
#endif
        }
    }

    save_globals(s, allocated_regs);
}

#define IS_DEAD_ARG(n) ((dead_args >> (n)) & 1)
#define NEED_SYNC_ARG(n) ((sync_args >> (n)) & 1)

static void tcg_reg_alloc_movi(TCGContext *s, const TCGArg *args,
                               uint16_t dead_args, uint8_t sync_args)
{
    TCGTemp *ots;
    tcg_target_ulong val;

    ots = &s->temps[args[0]];
    val = args[1];

    if (ots->fixed_reg) {
        /* for fixed registers, we do not do any constant
           propagation */
        tcg_out_movi(s, ots->type, ots->reg, val);
    } else {
        /* The movi is not explicitly generated here */
        if (ots->val_type == TEMP_VAL_REG)
            s->reg_to_temp[ots->reg] = -1;
        ots->val_type = TEMP_VAL_CONST;
        ots->val = val;
    }
    if (NEED_SYNC_ARG(0)) {
        temp_sync(s, args[0], s->reserved_regs);
    }
    if (IS_DEAD_ARG(0)) {
        temp_dead(s, args[0]);
    }
}

static void tcg_reg_alloc_mov(TCGContext *s, const TCGOpDef *def,
                              const TCGArg *args, uint16_t dead_args,
                              uint8_t sync_args)
{
    TCGRegSet allocated_regs;
    TCGTemp *ts, *ots;
    TCGType otype, itype;

    tcg_regset_set(allocated_regs, s->reserved_regs);
    ots = &s->temps[args[0]];
    ts = &s->temps[args[1]];

    /* Note that otype != itype for no-op truncation.  */
    otype = ots->type;
    itype = ts->type;

    /* If the source value is not in a register, and we're going to be
       forced to have it in a register in order to perform the copy,
       then copy the SOURCE value into its own register first.  That way
       we don't have to reload SOURCE the next time it is used. */
    if (((NEED_SYNC_ARG(0) || ots->fixed_reg) && ts->val_type != TEMP_VAL_REG)
        || ts->val_type == TEMP_VAL_MEM) {
        ts->reg = tcg_reg_alloc(s, (TCGRegSet)s->tcg_target_available_regs[itype],
                                allocated_regs);
        if (ts->val_type == TEMP_VAL_MEM) {
            tcg_out_ld(s, itype, ts->reg, ts->mem_reg, ts->mem_offset);
            ts->mem_coherent = 1;
        } else if (ts->val_type == TEMP_VAL_CONST) {
            tcg_out_movi(s, itype, ts->reg, ts->val);
            ts->mem_coherent = 0;
        }
        s->reg_to_temp[ts->reg] = args[1];
        ts->val_type = TEMP_VAL_REG;
    }

    if (IS_DEAD_ARG(0) && !ots->fixed_reg) {
        /* mov to a non-saved dead register makes no sense (even with
           liveness analysis disabled). */
        assert(NEED_SYNC_ARG(0));
        /* The code above should have moved the temp to a register. */
        assert(ts->val_type == TEMP_VAL_REG);
        if (!ots->mem_allocated) {
            temp_allocate_frame(s, args[0]);
        }
        tcg_out_st(s, otype, ts->reg, ots->mem_reg, ots->mem_offset);
        if (IS_DEAD_ARG(1)) {
            temp_dead(s, args[1]);
        }
        temp_dead(s, args[0]);
    } else if (ts->val_type == TEMP_VAL_CONST) {
        /* propagate constant */
        if (ots->val_type == TEMP_VAL_REG) {
            s->reg_to_temp[ots->reg] = -1;
        }
        ots->val_type = TEMP_VAL_CONST;
        ots->val = ts->val;
    } else {
        /* The code in the first if block should have moved the
           temp to a register. */
        assert(ts->val_type == TEMP_VAL_REG);
        if (IS_DEAD_ARG(1) && !ts->fixed_reg && !ots->fixed_reg) {
            /* the mov can be suppressed */
            if (ots->val_type == TEMP_VAL_REG) {
                s->reg_to_temp[ots->reg] = -1;
            }
            ots->reg = ts->reg;
            temp_dead(s, args[1]);
        } else {
            if (ots->val_type != TEMP_VAL_REG) {
                /* When allocating a new register, make sure to not spill the
                   input one. */
                tcg_regset_set_reg(allocated_regs, ts->reg);
                ots->reg = tcg_reg_alloc(s, (TCGRegSet)s->tcg_target_available_regs[otype],
                                         allocated_regs);
            }
            tcg_out_mov(s, otype, ots->reg, ts->reg);
        }
        ots->val_type = TEMP_VAL_REG;
        ots->mem_coherent = 0;
        s->reg_to_temp[ots->reg] = args[0];
        if (NEED_SYNC_ARG(0)) {
            tcg_reg_sync(s, ots->reg);
        }
    }
}

static void tcg_reg_alloc_op(TCGContext *s,
                             const TCGOpDef *def, TCGOpcode opc,
                             const TCGArg *args, uint16_t dead_args,
                             uint8_t sync_args)
{
    TCGRegSet allocated_regs;
    int i, k, nb_iargs, nb_oargs, reg;
    TCGArg arg;
    const TCGArgConstraint *arg_ct;
    TCGTemp *ts;
    TCGArg new_args[TCG_MAX_OP_ARGS];
    int const_args[TCG_MAX_OP_ARGS];

    nb_oargs = def->nb_oargs;
    nb_iargs = def->nb_iargs;

    /* copy constants */
    memcpy(new_args + nb_oargs + nb_iargs,
           args + nb_oargs + nb_iargs,
           sizeof(TCGArg) * def->nb_cargs);

    /* satisfy input constraints */
    tcg_regset_set(allocated_regs, s->reserved_regs);
    for(k = 0; k < nb_iargs; k++) {
        i = def->sorted_args[nb_oargs + k];
        arg = args[i];
        arg_ct = &def->args_ct[i];
        ts = &s->temps[arg];
        if (ts->val_type == TEMP_VAL_MEM) {
            reg = tcg_reg_alloc(s, arg_ct->u.regs, allocated_regs);
            tcg_out_ld(s, ts->type, reg, ts->mem_reg, ts->mem_offset);
            ts->val_type = TEMP_VAL_REG;
            ts->reg = reg;
            ts->mem_coherent = 1;
            s->reg_to_temp[reg] = arg;
        } else if (ts->val_type == TEMP_VAL_CONST) {
            if (tcg_target_const_match(ts->val, ts->type, arg_ct)) {
                /* constant is OK for instruction */
                const_args[i] = 1;
                new_args[i] = ts->val;
                goto iarg_end;
            } else {
                /* need to move to a register */
                reg = tcg_reg_alloc(s, arg_ct->u.regs, allocated_regs);
                tcg_out_movi(s, ts->type, reg, ts->val);
                ts->val_type = TEMP_VAL_REG;
                ts->reg = reg;
                ts->mem_coherent = 0;
                s->reg_to_temp[reg] = arg;
            }
        }
        assert(ts->val_type == TEMP_VAL_REG);
        if (arg_ct->ct & TCG_CT_IALIAS) {
            if (ts->fixed_reg) {
                /* if fixed register, we must allocate a new register
                   if the alias is not the same register */
                if (arg != args[arg_ct->alias_index])
                    goto allocate_in_reg;
            } else {
                /* if the input is aliased to an output and if it is
                   not dead after the instruction, we must allocate
                   a new register and move it */
                if (!IS_DEAD_ARG(i)) {
                    goto allocate_in_reg;
                }
            }
        }
        reg = ts->reg;
        if (tcg_regset_test_reg(arg_ct->u.regs, reg)) {
            /* nothing to do : the constraint is satisfied */
        } else {
        allocate_in_reg:
            /* allocate a new register matching the constraint
               and move the temporary register into it */
            reg = tcg_reg_alloc(s, arg_ct->u.regs, allocated_regs);
            tcg_out_mov(s, ts->type, reg, ts->reg);
        }
        new_args[i] = reg;
        const_args[i] = 0;
        tcg_regset_set_reg(allocated_regs, reg);
    iarg_end: ;
    }

    /* mark dead temporaries and free the associated registers */
    for (i = nb_oargs; i < nb_oargs + nb_iargs; i++) {
        if (IS_DEAD_ARG(i)) {
            temp_dead(s, args[i]);
        }
    }

    if (def->flags & TCG_OPF_BB_END) {
        tcg_reg_alloc_bb_end(s, allocated_regs);
    } else {
        if (def->flags & TCG_OPF_CALL_CLOBBER) {
            /* XXX: permit generic clobber register list ? */
            for(reg = 0; reg < TCG_TARGET_NB_REGS; reg++) {
                if (tcg_regset_test_reg(s->tcg_target_call_clobber_regs, reg)) {
                    tcg_reg_free(s, reg);
                }
            }
        }
        if (def->flags & TCG_OPF_SIDE_EFFECTS) {
            /* sync globals if the op has side effects and might trigger
               an exception. */
            sync_globals(s, allocated_regs);
        }

        /* satisfy the output constraints */
        tcg_regset_set(allocated_regs, s->reserved_regs);
        for(k = 0; k < nb_oargs; k++) {
            i = def->sorted_args[k];
            arg = args[i];
            arg_ct = &def->args_ct[i];
            ts = &s->temps[arg];
            if (arg_ct->ct & TCG_CT_ALIAS) {
                reg = new_args[arg_ct->alias_index];
            } else {
                /* if fixed register, we try to use it */
                reg = ts->reg;
                if (ts->fixed_reg &&
                    tcg_regset_test_reg(arg_ct->u.regs, reg)) {
                    goto oarg_end;
                }
                reg = tcg_reg_alloc(s, arg_ct->u.regs, allocated_regs);
            }
            tcg_regset_set_reg(allocated_regs, reg);
            /* if a fixed register is used, then a move will be done afterwards */
            if (!ts->fixed_reg) {
                if (ts->val_type == TEMP_VAL_REG) {
                    s->reg_to_temp[ts->reg] = -1;
                }
                ts->val_type = TEMP_VAL_REG;
                ts->reg = reg;
                /* temp value is modified, so the value kept in memory is
                   potentially not the same */
                ts->mem_coherent = 0;
                s->reg_to_temp[reg] = arg;
            }
        oarg_end:
            new_args[i] = reg;
        }
    }

    /* emit instruction */
    tcg_out_op(s, opc, new_args, const_args);

    /* move the outputs in the correct register if needed */
    for(i = 0; i < nb_oargs; i++) {
        ts = &s->temps[args[i]];
        reg = new_args[i];
        if (ts->fixed_reg && ts->reg != reg) {
            tcg_out_mov(s, ts->type, ts->reg, reg);
        }
        if (NEED_SYNC_ARG(i)) {
            tcg_reg_sync(s, reg);
        }
        if (IS_DEAD_ARG(i)) {
            temp_dead(s, args[i]);
        }
    }
}

#ifdef TCG_TARGET_STACK_GROWSUP
#define STACK_DIR(x) (-(x))
#else
#define STACK_DIR(x) (x)
#endif

static void tcg_reg_alloc_call(TCGContext *s, int nb_oargs, int nb_iargs,
	const TCGArg * const args, uint16_t dead_args,
	uint8_t sync_args)
{
	int flags, nb_regs, i, reg;
    TCGArg arg;
    TCGTemp *ts;
    intptr_t stack_offset;
    size_t call_stack_size;
    tcg_insn_unit *func_addr;
    int allocate_args;
    TCGRegSet allocated_regs;

	func_addr = (tcg_insn_unit *)(intptr_t)args[nb_oargs + nb_iargs];
	flags = args[nb_oargs + nb_iargs + 1];

	nb_regs = ARRAY_SIZE(tcg_target_call_iarg_regs);
	if (nb_regs > nb_iargs) {
		nb_regs = nb_iargs;
	}

    /* assign stack slots first */
    call_stack_size = (nb_iargs - nb_regs) * sizeof(tcg_target_long);
    call_stack_size = (call_stack_size + TCG_TARGET_STACK_ALIGN - 1) &
        ~(TCG_TARGET_STACK_ALIGN - 1);
    allocate_args = (call_stack_size > TCG_STATIC_CALL_ARGS_SIZE);
    if (allocate_args) {
        /* XXX: if more than TCG_STATIC_CALL_ARGS_SIZE is needed,
           preallocate call stack */
        tcg_abort();
    }

    stack_offset = TCG_TARGET_CALL_STACK_OFFSET;
    for(i = nb_regs; i < nb_iargs; i++) {
        arg = args[nb_oargs + i];
#ifdef TCG_TARGET_STACK_GROWSUP
        stack_offset -= sizeof(tcg_target_long);
#endif
        if (arg != TCG_CALL_DUMMY_ARG) {
            ts = &s->temps[arg];
            if (ts->val_type == TEMP_VAL_REG) {
                tcg_out_st(s, ts->type, ts->reg, TCG_REG_CALL_STACK, stack_offset);
            } else if (ts->val_type == TEMP_VAL_MEM) {
                reg = tcg_reg_alloc(s, (TCGRegSet)s->tcg_target_available_regs[ts->type],
                                    s->reserved_regs);
                /* XXX: not correct if reading values from the stack */
                tcg_out_ld(s, ts->type, reg, ts->mem_reg, ts->mem_offset);
                tcg_out_st(s, ts->type, reg, TCG_REG_CALL_STACK, stack_offset);
            } else if (ts->val_type == TEMP_VAL_CONST) {
                reg = tcg_reg_alloc(s, (TCGRegSet)s->tcg_target_available_regs[ts->type],
                                    s->reserved_regs);
                /* XXX: sign extend may be needed on some targets */
                tcg_out_movi(s, ts->type, reg, ts->val);
                tcg_out_st(s, ts->type, reg, TCG_REG_CALL_STACK, stack_offset);
            } else {
                tcg_abort();
            }
        }
#ifndef TCG_TARGET_STACK_GROWSUP
        stack_offset += sizeof(tcg_target_long);
#endif
    }

    /* assign input registers */
    tcg_regset_set(allocated_regs, s->reserved_regs);
    for(i = 0; i < nb_regs; i++) {
        arg = args[nb_oargs + i];
        if (arg != TCG_CALL_DUMMY_ARG) {
            ts = &s->temps[arg];
            reg = tcg_target_call_iarg_regs[i];
            tcg_reg_free(s, reg);
            if (ts->val_type == TEMP_VAL_REG) {
                if (ts->reg != reg) {
                    tcg_out_mov(s, ts->type, reg, ts->reg);
                }
            } else if (ts->val_type == TEMP_VAL_MEM) {
                tcg_out_ld(s, ts->type, reg, ts->mem_reg, ts->mem_offset);
            } else if (ts->val_type == TEMP_VAL_CONST) {
                /* XXX: sign extend ? */
                tcg_out_movi(s, ts->type, reg, ts->val);
            } else {
                tcg_abort();
            }
            tcg_regset_set_reg(allocated_regs, reg);
        }
    }

    /* mark dead temporaries and free the associated registers */
    for(i = nb_oargs; i < nb_iargs + nb_oargs; i++) {
        if (IS_DEAD_ARG(i)) {
            temp_dead(s, args[i]);
        }
    }

    /* clobber call registers */
    for(reg = 0; reg < TCG_TARGET_NB_REGS; reg++) {
        if (tcg_regset_test_reg(s->tcg_target_call_clobber_regs, reg)) {
            tcg_reg_free(s, reg);
        }
    }

    /* Save globals if they might be written by the helper, sync them if
       they might be read. */
    if (flags & TCG_CALL_NO_READ_GLOBALS) {
        /* Nothing to do */
    } else if (flags & TCG_CALL_NO_WRITE_GLOBALS) {
        sync_globals(s, allocated_regs);
    } else {
        save_globals(s, allocated_regs);
    }

    tcg_out_call(s, func_addr);

    /* assign output registers and emit moves if needed */
    for(i = 0; i < nb_oargs; i++) {
        arg = args[i];
        ts = &s->temps[arg];
        reg = tcg_target_call_oarg_regs[i];
        assert(s->reg_to_temp[reg] == -1);

        if (ts->fixed_reg) {
            if (ts->reg != reg) {
                tcg_out_mov(s, ts->type, ts->reg, reg);
            }
        } else {
            if (ts->val_type == TEMP_VAL_REG) {
                s->reg_to_temp[ts->reg] = -1;
            }
            ts->val_type = TEMP_VAL_REG;
            ts->reg = reg;
            ts->mem_coherent = 0;
            s->reg_to_temp[reg] = arg;
            if (NEED_SYNC_ARG(i)) {
                tcg_reg_sync(s, reg);
            }
            if (IS_DEAD_ARG(i)) {
                temp_dead(s, args[i]);
            }
        }
    }

}

#ifdef CONFIG_PROFILER

static void dump_op_count(void)
{
    int i;

    for(i = INDEX_op_end; i < NB_OPS; i++) {
        qemu_log("%s %" PRId64 "\n", s->tcg_op_defs[i].name, tcg_table_op_count[i]);
    }
}
#endif


static inline int tcg_gen_code_common(TCGContext *s,
                                      tcg_insn_unit *gen_code_buf,
                                      long search_pc)
{
	int oi, oi_next;

#ifdef DEBUG_DISAS
    if (unlikely(qemu_loglevel_mask(CPU_LOG_TB_OP))) {
        qemu_log("OP:\n");
        tcg_dump_ops(s);
        qemu_log("\n");
    }
#endif

#ifdef CONFIG_PROFILER
    s->opt_time -= profile_getclock();
#endif

#ifdef USE_TCG_OPTIMIZATIONS
	tcg_optimize(s);
#endif

#ifdef CONFIG_PROFILER
    s->opt_time += profile_getclock();
    s->la_time -= profile_getclock();
#endif

    tcg_liveness_analysis(s);

#ifdef CONFIG_PROFILER
    s->la_time += profile_getclock();
#endif

#ifdef DEBUG_DISAS
    if (unlikely(qemu_loglevel_mask(CPU_LOG_TB_OP_OPT))) {
        qemu_log("OP after optimization and liveness analysis:\n");
        tcg_dump_ops(s);
        qemu_log("\n");
    }
#endif

    tcg_reg_alloc_start(s);

    s->code_buf = gen_code_buf;
    s->code_ptr = gen_code_buf;

    tcg_out_tb_init(s);

	for (oi = s->gen_first_op_idx; oi >= 0; oi = oi_next) {
		TCGOp * const op = &s->gen_op_buf[oi];
		TCGArg * const args = &s->gen_opparam_buf[op->args];
		TCGOpcode opc = op->opc;
		const TCGOpDef *def = &s->tcg_op_defs[opc];
		uint16_t dead_args = s->op_dead_args[oi];
		uint8_t sync_args = s->op_sync_args[oi];

		oi_next = op->next;
#ifdef CONFIG_PROFILER
		tcg_table_op_count[opc]++;
#endif

		switch (opc) {
        case INDEX_op_mov_i32:
        case INDEX_op_mov_i64:
			tcg_reg_alloc_mov(s, def, args, dead_args, sync_args);
            break;
        case INDEX_op_movi_i32:
        case INDEX_op_movi_i64:
			tcg_reg_alloc_movi(s, args, dead_args, sync_args);
            break;
        case INDEX_op_debug_insn_start:
            /* debug instruction */
            break;
        case INDEX_op_nop:
            break;
        case INDEX_op_discard:
            temp_dead(s, args[0]);
            break;
        case INDEX_op_set_label:
            tcg_reg_alloc_bb_end(s, s->reserved_regs);
            tcg_out_label(s, args[0], s->code_ptr);
            break;
        case INDEX_op_call:
			tcg_reg_alloc_call(s, op->callo, op->calli, args,
				dead_args, sync_args);
			break;
        default:
            /* Sanity check that we've not introduced any unhandled opcodes. */
            if (def->flags & TCG_OPF_NOT_PRESENT) {
                tcg_abort();
            }
            /* Note: in order to speed up the code, it would be much
               faster to have specialized register allocator functions for
               some common argument patterns */
			tcg_reg_alloc_op(s, def, opc, args, dead_args, sync_args);
            break;
        }
		if (search_pc >= 0 && search_pc < tcg_current_code_size(s)) {
			return oi;
		}
#ifndef NDEBUG
        check_regs(s);
#endif
    }
    /* Generate TB finalization at the end of block */
    tcg_out_tb_finalize(s);
    return -1;
}

int tcg_gen_code(TCGContext *s, TranslationBlock *tb)
{
	int i, oi, oi_next, num_insns;

#ifdef CONFIG_PROFILER
	{
		int n;

		n = s->gen_op_buf[0].prev + 1;
		s->op_count += n;
		if (n > s->op_count_max) {
			s->op_count_max = n;
		}

		n = s->nb_temps;
		s->temp_count += n;
		if (n > s->temp_count_max) {
			s->temp_count_max = n;
}
	}
#endif

#ifdef DEBUG_DISAS
	if (unlikely(qemu_loglevel_mask(CPU_LOG_TB_OP)
		&& qemu_log_in_addr_range(tb->pc))) {
		qemu_log_lock();
		qemu_log("OP:\n");
		tcg_dump_ops(s);
		qemu_log("\n");
		qemu_log_unlock();
	}
#endif

#ifdef CONFIG_PROFILER
	s->opt_time -= profile_getclock();
#endif

#ifdef USE_TCG_OPTIMIZATIONS
	tcg_optimize(s);
#endif

#ifdef CONFIG_PROFILER
	s->opt_time += profile_getclock();
	s->la_time -= profile_getclock();
#endif

	{
		uint8_t *temp_state = tcg_malloc(s->nb_temps + s->nb_indirects);

		liveness_pass_1(s, temp_state);

		if (s->nb_indirects > 0) {
#ifdef DEBUG_DISAS
			if (unlikely(qemu_loglevel_mask(CPU_LOG_TB_OP_IND)
				&& qemu_log_in_addr_range(tb->pc))) {
				qemu_log_lock();
				qemu_log("OP before indirect lowering:\n");
				tcg_dump_ops(s);
				qemu_log("\n");
				qemu_log_unlock();
			}
#endif
			/* Replace indirect temps with direct temps.  */
			if (liveness_pass_2(s, temp_state)) {
				/* If changes were made, re-run liveness.  */
				liveness_pass_1(s, temp_state);
			}
		}
	}

#ifdef CONFIG_PROFILER
	s->la_time += profile_getclock();
#endif

#ifdef DEBUG_DISAS
	if (unlikely(qemu_loglevel_mask(CPU_LOG_TB_OP_OPT)
		&& qemu_log_in_addr_range(tb->pc))) {
		qemu_log_lock();
		qemu_log("OP after optimization and liveness analysis:\n");
		tcg_dump_ops(s);
		qemu_log("\n");
		qemu_log_unlock();
	}
#endif

	tcg_reg_alloc_start(s);

	s->code_buf = tb->tc.ptr;
	s->code_ptr = tb->tc.ptr;

#ifdef TCG_TARGET_NEED_LDST_LABELS
	s->ldst_labels = NULL;
#endif
#ifdef TCG_TARGET_NEED_POOL_LABELS
	s->pool_labels = NULL;
#endif

	num_insns = -1;
	for (oi = s->gen_op_buf[0].next; oi != 0; oi = oi_next) {
		TCGOp * const op = &s->gen_op_buf[oi];
		TCGArg * const args = op->args;
		TCGOpcode opc = op->opc;
		const TCGOpDef *def = &tcg_op_defs[opc];
		TCGLifeData arg_life = op->life;

		oi_next = op->next;
#ifdef CONFIG_PROFILER
		tcg_table_op_count[opc]++;
#endif

		switch (opc) {
		case INDEX_op_mov_i32:
		case INDEX_op_mov_i64:
			tcg_reg_alloc_mov(s, def, args, arg_life);
			break;
		case INDEX_op_movi_i32:
		case INDEX_op_movi_i64:
			tcg_reg_alloc_movi(s, args, arg_life);
			break;
		case INDEX_op_insn_start:
			if (num_insns >= 0) {
				s->gen_insn_end_off[num_insns] = tcg_current_code_size(s);
			}
			num_insns++;
			for (i = 0; i < TARGET_INSN_START_WORDS; ++i) {
				target_ulong a;
#if TARGET_LONG_BITS > TCG_TARGET_REG_BITS
				a = ((target_ulong)args[i * 2 + 1] << 32) | args[i * 2];
#else
				a = args[i];
#endif
				s->gen_insn_data[num_insns][i] = a;
			}
			break;
		case INDEX_op_discard:
			temp_dead(s, &s->temps[args[0]]);
			break;
		case INDEX_op_set_label:
			tcg_reg_alloc_bb_end(s, s->reserved_regs);
			tcg_out_label(s, arg_label(args[0]), s->code_ptr);
			break;
		case INDEX_op_call:
			tcg_reg_alloc_call(s, op->callo, op->calli, args, arg_life);
			break;
		default:
			/* Sanity check that we've not introduced any unhandled opcodes. */
			tcg_debug_assert(tcg_op_supported(opc));
			/* Note: in order to speed up the code, it would be much
			faster to have specialized register allocator functions for
			some common argument patterns */
			tcg_reg_alloc_op(s, def, opc, args, arg_life);
			break;
		}
#ifdef CONFIG_DEBUG_TCG
		check_regs(s);
#endif
		/* Test for (pending) buffer overflow.  The assumption is that any
		one operation beginning below the high water mark cannot overrun
		the buffer completely.  Thus we can test for overflow after
		generating code without having to check during generation.  */
		if (unlikely((void *)s->code_ptr > s->code_gen_highwater)) {
			return -1;
		}
	}
	tcg_debug_assert(num_insns >= 0);
	s->gen_insn_end_off[num_insns] = tcg_current_code_size(s);

	/* Generate TB finalization at the end of block */
#ifdef TCG_TARGET_NEED_LDST_LABELS
	if (!tcg_out_ldst_finalize(s)) {
		return -1;
	}
#endif
#ifdef TCG_TARGET_NEED_POOL_LABELS
	if (!tcg_out_pool_finalize(s)) {
		return -1;
	}
#endif

	/* flush instruction cache */
	flush_icache_range((uintptr_t)s->code_buf, (uintptr_t)s->code_ptr);

	return tcg_current_code_size(s);
}

/* Return the index of the micro operation such as the pc after is <
   offset bytes from the start of the TB.  The contents of gen_code_buf must
   not be changed, though writing the same values is ok.
   Return -1 if not found. */
int tcg_gen_code_search_pc(TCGContext *s, tcg_insn_unit *gen_code_buf,
                           long offset)
{
    return tcg_gen_code_common(s, gen_code_buf, offset);
}

#ifdef CONFIG_PROFILER
void tcg_dump_info(FILE *f, fprintf_function cpu_fprintf)
{
#if 0
    TCGContext *s = &tcg_ctx;
    int64_t tot;

    tot = s->interm_time + s->code_time;
    cpu_fprintf(f, "JIT cycles          %" PRId64 " (%0.3f s at 2.4 GHz)\n",
                tot, tot / 2.4e9);
    cpu_fprintf(f, "translated TBs      %" PRId64 " (aborted=%" PRId64 " %0.1f%%)\n",
                s->tb_count,
                s->tb_count1 - s->tb_count,
                s->tb_count1 ? (double)(s->tb_count1 - s->tb_count) / s->tb_count1 * 100.0 : 0);
    cpu_fprintf(f, "avg ops/TB          %0.1f max=%d\n",
                s->tb_count ? (double)s->op_count / s->tb_count : 0, s->op_count_max);
    cpu_fprintf(f, "deleted ops/TB      %0.2f\n",
                s->tb_count ?
                (double)s->del_op_count / s->tb_count : 0);
    cpu_fprintf(f, "avg temps/TB        %0.2f max=%d\n",
                s->tb_count ?
                (double)s->temp_count / s->tb_count : 0,
                s->temp_count_max);

    cpu_fprintf(f, "cycles/op           %0.1f\n",
                s->op_count ? (double)tot / s->op_count : 0);
    cpu_fprintf(f, "cycles/in byte      %0.1f\n",
                s->code_in_len ? (double)tot / s->code_in_len : 0);
    cpu_fprintf(f, "cycles/out byte     %0.1f\n",
                s->code_out_len ? (double)tot / s->code_out_len : 0);
    if (tot == 0)
        tot = 1;
    cpu_fprintf(f, "  gen_interm time   %0.1f%%\n",
                (double)s->interm_time / tot * 100.0);
    cpu_fprintf(f, "  gen_code time     %0.1f%%\n",
                (double)s->code_time / tot * 100.0);
    cpu_fprintf(f, "optim./code time    %0.1f%%\n",
                (double)s->opt_time / (s->code_time ? s->code_time : 1)
                * 100.0);
    cpu_fprintf(f, "liveness/code time  %0.1f%%\n",
                (double)s->la_time / (s->code_time ? s->code_time : 1) * 100.0);
    cpu_fprintf(f, "cpu_restore count   %" PRId64 "\n",
                s->restore_count);
    cpu_fprintf(f, "  avg cycles        %0.1f\n",
                s->restore_count ? (double)s->restore_time / s->restore_count : 0);

    dump_op_count();
#endif
}
#else
void tcg_dump_info(FILE *f, fprintf_function cpu_fprintf)
{
    cpu_fprintf(f, "[TCG profiler not compiled]\n");
}
#endif
