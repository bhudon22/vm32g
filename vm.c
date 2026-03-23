/*
 * vm.c — 32-bit Forth VM: memory helpers, dictionary, inner interpreter,
 *        outer interpreter, and REPL.
 *
 * Read vm.h first for the full design overview.
 *
 * Key differences from the 16-bit vm/:
 *   - Cells are int32_t; addresses are uint32_t
 *   - read32/write32 handle 4-byte little-endian values
 *   - vm_run() uses computed goto instead of switch — faster dispatch
 *   - STATE and BASE live in memory (ADDR_STATE, ADDR_BASE), not the struct
 *   - Dictionary headers are 6 bytes before the name (link=4, flags=1, len=1)
 *   - F_PRIMITIVE words are inlined by the compiler (no CALL overhead)
 *   - F_HIDDEN hides a word during its own definition
 */

#include "vm.h"
#include "gfx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

/* =========================================================================
 * Compile-time loop stack — shared between vm_run (DO/LOOP handlers) and
 * vm_eval (colon handler resets it at the start of each new definition).
 * ========================================================================= */
static uint32_t cloop_skip[16];   /* ?DO forward-skip patch address (0 = plain DO) */
static uint32_t cloop_chain[16];  /* LEAVE linked-list head (0 = empty)            */
static uint32_t cloop_body[16];   /* loop body start address                       */
static int      cloop_depth = 0;  /* number of open DO loops being compiled        */

/* =========================================================================
 * Memory helpers — 32-bit little-endian read/write
 * ========================================================================= */

static uint32_t read32(VM *vm, uint32_t addr)
{
    return (uint32_t)vm->mem[addr]
         | ((uint32_t)vm->mem[addr + 1] << 8)
         | ((uint32_t)vm->mem[addr + 2] << 16)
         | ((uint32_t)vm->mem[addr + 3] << 24);
}

static void write32(VM *vm, uint32_t addr, uint32_t val)
{
    vm->mem[addr]     = (uint8_t)(val & 0xFF);
    vm->mem[addr + 1] = (uint8_t)((val >>  8) & 0xFF);
    vm->mem[addr + 2] = (uint8_t)((val >> 16) & 0xFF);
    vm->mem[addr + 3] = (uint8_t)((val >> 24) & 0xFF);
}

/* =========================================================================
 * Compilation helpers
 * ========================================================================= */

void emit_byte(VM *vm, uint8_t b)
{
    vm->mem[vm->here++] = b;
}

void emit_u32(VM *vm, uint32_t v)
{
    emit_byte(vm, (uint8_t)(v & 0xFF));
    emit_byte(vm, (uint8_t)((v >>  8) & 0xFF));
    emit_byte(vm, (uint8_t)((v >> 16) & 0xFF));
    emit_byte(vm, (uint8_t)((v >> 24) & 0xFF));
}

/* =========================================================================
 * Stack helpers
 *
 * dsp is the index of the current top-of-stack, or -1 when empty.
 * Stack cells are int32_t — signed, which is the natural type for
 * Forth cells (arithmetic is signed; truth values are -1 and 0).
 * ========================================================================= */

void dpush(VM *vm, int32_t v)
{
    if (vm->dsp >= STACK_DEPTH - 1) {
        fputs("stack overflow\n", stderr);
        return;
    }
    vm->ds[++vm->dsp] = v;
}

int32_t dpop(VM *vm)
{
    if (vm->dsp < 0) {
        fputs("stack underflow\n", stderr);
        return 0;
    }
    return vm->ds[vm->dsp--];
}

int32_t dpeek(VM *vm)
{
    if (vm->dsp < 0) {
        fputs("stack underflow\n", stderr);
        return 0;
    }
    return vm->ds[vm->dsp];
}

/* =========================================================================
 * Dictionary
 *
 * Memory layout of each entry:
 *
 *   Offset  Size  Field
 *   ------  ----  -----
 *   0       4     link    — address of previous entry (0 = end)
 *   4       1     flags   — F_IMMEDIATE, F_HIDDEN, F_PRIMITIVE
 *   5       1     namelen — byte count of name
 *   6       N     name    — raw bytes (not null-terminated)
 *   6+N     ...   body    — bytecode
 *
 * dict_create() writes the header and updates LATEST and HERE.
 * The caller emits bytecode at HERE to fill in the body.
 * ========================================================================= */

void dict_create(VM *vm, const char *name, int len, uint8_t flags)
{
    uint32_t entry = vm->here;

    emit_u32(vm, vm->latest);           /* link to previous entry */
    emit_byte(vm, flags);               /* flags */
    emit_byte(vm, (uint8_t)len);        /* name length */
    for (int i = 0; i < len; i++)
        emit_byte(vm, (uint8_t)name[i]);

    vm->latest = entry;
}

uint32_t dict_body(VM *vm, uint32_t entry)
{
    uint8_t namelen = vm->mem[entry + 5];
    return entry + 6 + namelen;         /* skip link(4)+flags(1)+len(1)+name(N) */
}

uint32_t dict_find(VM *vm, const char *name, int len)
{
    uint32_t e = vm->latest;
    while (e != 0) {
        uint8_t flags   = vm->mem[e + 4];
        uint8_t namelen = vm->mem[e + 5];

        if (!(flags & F_HIDDEN) &&
            namelen == (uint8_t)len &&
            memcmp(&vm->mem[e + 6], name, (size_t)len) == 0)
            return e;

        e = read32(vm, e);              /* follow link */
    }
    return 0;
}

/* =========================================================================
 * do_save_image — write VM image to "forth.img"
 *
 * Header layout (20 bytes):
 *   offset  0: 4 bytes  magic 'F','I','M','G'
 *   offset  4: 4 bytes  version = 1 (uint32_t, little-endian)
 *   offset  8: 4 bytes  here    (vm->here, uint32_t)
 *   offset 12: 4 bytes  latest  (vm->latest, uint32_t)
 *   offset 16: 4 bytes  turnkey_xt (0 = none)
 *   offset 20: here bytes  mem[0..here]
 *
 * Returns 0 on success, -1 on error.
 * ========================================================================= */

static int do_save_image(VM *vm, uint32_t turnkey_xt)
{
    FILE *f = fopen("forth.img", "wb");
    if (!f) {
        fprintf(stderr, "error: save-image: cannot open forth.img for writing\n");
        return -1;
    }

    /* Build the 20-byte header */
    uint8_t hdr[20];
    hdr[0] = 'F'; hdr[1] = 'I'; hdr[2] = 'M'; hdr[3] = 'G';

    uint32_t version = 1;
    memcpy(hdr + 4,  &version,     4);
    memcpy(hdr + 8,  &vm->here,    4);
    memcpy(hdr + 12, &vm->latest,  4);
    memcpy(hdr + 16, &turnkey_xt,  4);

    if (fwrite(hdr, 1, 20, f) != 20) {
        fprintf(stderr, "error: save-image: write error (header)\n");
        fclose(f);
        return -1;
    }

    /* Write mem[0..here] */
    if (fwrite(vm->mem, 1, vm->here, f) != vm->here) {
        fprintf(stderr, "error: save-image: write error (image body)\n");
        fclose(f);
        return -1;
    }

    fclose(f);
    return 0;
}

/* =========================================================================
 * vm_init — zero the VM, set defaults, register built-in words
 *
 * Primitives get F_PRIMITIVE in their flags. This tells the compiler to
 * inline the opcode byte directly instead of emitting OP_CALL + address.
 * This matters most for >R and R> — without inlining, the CALL itself
 * would push a return address and corrupt the return stack.
 * ========================================================================= */

static void def_prim(VM *vm, const char *name, uint8_t opcode)
{
    dict_create(vm, name, (int)strlen(name), F_PRIMITIVE);
    emit_byte(vm, opcode);
    emit_byte(vm, OP_RET);
}

/* IMMEDIATE word: executes during compilation rather than being inlined.
 * Body is just [opcode, RET] — same as a primitive, but flagged IMMEDIATE
 * so vm_eval() runs it instead of emitting it into the word under definition. */
static void def_imm(VM *vm, const char *name, uint8_t opcode)
{
    dict_create(vm, name, (int)strlen(name), F_IMMEDIATE);
    emit_byte(vm, opcode);
    emit_byte(vm, OP_RET);
}

/* Forth constant: body is [OP_LIT, value(4), OP_RET].
 * Executing the word pushes the value. */
static void def_const(VM *vm, const char *name, int32_t val)
{
    dict_create(vm, name, (int)strlen(name), 0);
    emit_byte(vm, OP_LIT);
    emit_u32(vm, (uint32_t)val);
    emit_byte(vm, OP_RET);
}

void vm_init(VM *vm)
{
    memset(vm, 0, sizeof(*vm));
    vm->dsp    = -1;
    vm->here   = DICT_BASE;
    vm->latest = 0;

    write32(vm, ADDR_STATE,   0);         /* interpret mode */
    write32(vm, ADDR_BASE,    10);        /* decimal */
    write32(vm, ADDR_IN,      0);
    write32(vm, ADDR_SRCLEN,  0);
    write32(vm, ADDR_HLD,     ADDR_PAD); /* picture hold pointer: init to pad */
    write32(vm, ADDR_SRCBASE, TIB_BASE); /* SOURCE base addr: TIB for normal input */

    /* Arithmetic */
    def_prim(vm, "+",      OP_ADD);
    def_prim(vm, "-",      OP_SUB);
    def_prim(vm, "*",      OP_MUL);
    def_prim(vm, "/",      OP_DIV);
    def_prim(vm, "negate", OP_NEGATE);
    def_prim(vm, "abs",    OP_ABS);
    def_prim(vm, "max",    OP_MAX);
    def_prim(vm, "min",    OP_MIN);
    def_prim(vm, "mod",    OP_MOD);
    def_prim(vm, "/mod",   OP_DIVMOD);
    def_prim(vm, "1+",  OP_ONEPLUS);
    def_prim(vm, "1-",  OP_ONEMINUS);
    def_prim(vm, "2*",  OP_TWOSTAR);
    def_prim(vm, "2/",  OP_TWOSLASH);
    def_prim(vm, "cell+",   OP_CELLPLUS);
    def_prim(vm, "cells",   OP_CELLS);
    def_prim(vm, "char+",   OP_CHARPLUS);
    def_prim(vm, "chars",   OP_CHARS);
    def_prim(vm, "aligned", OP_ALIGNED);
    def_prim(vm, "align",   OP_ALIGN);

    /* Turnkey image support */
    def_prim(vm, "bye",        OP_BYE);
    def_prim(vm, "save-image", OP_SAVEIMAGE);
    def_prim(vm, "turnkey",    OP_TURNKEY);
    def_prim(vm, "latest",     OP_LATEST);
    def_prim(vm, "set-here",   OP_SETHERE);
    def_prim(vm, "set-latest", OP_SETLATEST);

    /* Stack */
    def_prim(vm, "dup",    OP_DUP);
    def_prim(vm, "drop",   OP_DROP);
    def_prim(vm, "swap",   OP_SWAP);
    def_prim(vm, "over",   OP_OVER);
    def_prim(vm, "rot",    OP_ROT);
    def_prim(vm, "?dup",   OP_QDUP);
    def_prim(vm, "2swap",  OP_2SWAP);
    def_prim(vm, "2over",  OP_2OVER);
    def_prim(vm, "pick",   OP_PICK);
    def_prim(vm, "roll",   OP_ROLL);
    def_prim(vm, "depth",  OP_DEPTH);

    /* Comparison */
    def_prim(vm, "=",      OP_EQ);
    def_prim(vm, "<",      OP_LT);
    def_prim(vm, ">",      OP_GT);
    def_prim(vm, "0=",     OP_ZEROEQ);
    def_prim(vm, "0<",     OP_ZEROLT);
    def_prim(vm, "0>",     OP_ZEROGT);
    def_prim(vm, "<>",     OP_NEQ);
    def_prim(vm, "u<",     OP_ULT);
    def_prim(vm, "not",    OP_ZEROEQ);  /* not = 0= */
    def_const(vm, "true",  -1);
    def_const(vm, "false",  0);

    /* Bitwise */
    def_prim(vm, "and",    OP_AND);
    def_prim(vm, "or",     OP_OR);
    def_prim(vm, "xor",    OP_XOR);
    def_prim(vm, "invert", OP_INVERT);
    def_prim(vm, "lshift", OP_LSHIFT);
    def_prim(vm, "rshift", OP_RSHIFT);

    /* Memory */
    def_prim(vm, "@",      OP_FETCH);
    def_prim(vm, "!",      OP_STORE);
    def_prim(vm, "c@",     OP_CFETCH);
    def_prim(vm, "c!",     OP_CSTORE);
    def_prim(vm, "+!",     OP_PLUSST);
    def_prim(vm, "here",   OP_HERE);
    def_prim(vm, "allot",  OP_ALLOT);
    def_prim(vm, ",",      OP_COMMA);
    def_prim(vm, "c,",     OP_CCOMMA);
    def_prim(vm, "count",  OP_COUNT);
    def_prim(vm, "move",   OP_MOVE);
    def_prim(vm, "fill",   OP_FILL);

    /* VM variables — memory-mapped addresses as Forth constants */
    def_const(vm, "state", ADDR_STATE);
    def_const(vm, "base",  ADDR_BASE);

    /* Return stack */
    def_prim(vm, ">r",     OP_TOR);
    def_prim(vm, "r>",     OP_RFROM);
    def_prim(vm, "r@",     OP_RFETCH);

    /* I/O */
    def_prim(vm, "emit",   OP_EMIT);
    def_prim(vm, "key",    OP_KEY);
    def_prim(vm, ".",      OP_DOT);
    def_prim(vm, ".s",     OP_DOTS);
    def_prim(vm, "cr",     OP_CR);
    def_prim(vm, "space",  OP_SPACE);
    def_prim(vm, "type",   OP_TYPE);
    def_prim(vm, "accept", OP_ACCEPT);

    /* Control flow — runtime */
    def_prim(vm, "exit",   OP_EXIT);

    /* Control flow — compile-time IMMEDIATE */
    def_imm(vm, "if",      OP_IF);
    def_imm(vm, "then",    OP_THEN);
    def_imm(vm, "else",    OP_ELSE);
    def_imm(vm, "begin",   OP_BEGIN);
    def_imm(vm, "until",   OP_UNTIL);
    def_imm(vm, "while",   OP_WHILE);
    def_imm(vm, "repeat",  OP_REPEAT);
    def_imm(vm, "again",   OP_AGAIN);

    /* Compile state */
    def_imm(vm,  "[",         OP_LBRACKET);
    def_prim(vm, "]",         OP_RBRACKET);
    def_imm(vm,  "literal",   OP_LITERAL);
    def_prim(vm, "immediate", OP_SETIMM);

    /* Forth-defined words moved to kernel.fth */

    /* --- step 10: DO/LOOP --- */
    def_prim(vm, "i",      OP_I);
    def_prim(vm, "j",      OP_J);
    def_prim(vm, "unloop", OP_UNLOOP);
    def_imm(vm,  "do",     OP_DO);
    def_imm(vm,  "?do",    OP_QDO);
    def_imm(vm,  "loop",   OP_LOOP);
    def_imm(vm,  "+loop",  OP_PLOOP);
    def_imm(vm,  "leave",  OP_LEAVE);

    /* --- step 11: double-cell arithmetic --- */
    def_prim(vm, "s>d",    OP_S2D);
    def_prim(vm, "um*",    OP_UMSTAR);
    def_prim(vm, "m*",     OP_MSTAR);
    def_prim(vm, "um/mod", OP_UMDIVMOD);
    def_prim(vm, "fm/mod", OP_FMDIVMOD);
    def_prim(vm, "sm/rem", OP_SMDIVREM);
    def_prim(vm, "d+",     OP_DPLUS);
    def_prim(vm, "dnegate", OP_DNEGATE);

    /* --- step 13: file I/O --- */
    def_prim(vm, "include-file", OP_INCLUDE);
    def_prim(vm, "source",       OP_SOURCE);
    def_prim(vm, "parse-name",   OP_PARSENAME);
    def_prim(vm, "word",         OP_WORD);
    def_prim(vm, "char",         OP_CHAR);
    def_prim(vm, "find",         OP_FIND);
    def_prim(vm, "execute",      OP_EXECUTE);
    def_prim(vm, ">number",      OP_TONUMBER);
    /* --- step 12: pictured numeric output --- */
    def_const(vm, "hld", ADDR_HLD);
    def_const(vm, "pad", ADDR_PAD);

    def_prim(vm, "ud/mod", OP_UDDIVMOD);     /* ( lo hi u -- rem lo' hi' ) 64-bit quotient */
    /* --- step 13 prerequisites: >in --- */
    def_const(vm, ">in", ADDR_IN);

    /* --- step 13: quit, abort, evaluate --- */
    def_prim(vm, "quit", OP_QUIT);
    def_prim(vm, "evaluate", OP_EVALUATE);
    def_prim(vm, "create",   OP_CREATE);
    def_imm(vm,  "\\",           OP_BACKSLASH);    /* \ comment: real dict word for POSTPONE */
    def_prim(vm, "compile,",     OP_COMPILE_CALL); /* compile, ( xt -- ) emit CALL xt */
    def_prim(vm, "constant",     OP_CONSTANT);     /* constant: parsing word — creates constant */
    def_prim(vm, ":",            OP_COLON_RT);     /* runtime ':': parse name, create entry */
    def_imm(vm,  ";",            OP_SEMICOLON_RT); /* runtime ';': emit RET, un-hide, STATE=0 */
    def_imm(vm,  "does>",        OP_DOES_IMM);     /* DOES>: compile-time setup for defining words */
    /* --- step 14: environment?, source-id (moved to kernel.fth) --- */
}

/* =========================================================================
 * vm_run — inner interpreter (computed goto)
 *
 * The NEXT macro is the heart of the engine. It fetches the next opcode
 * byte from mem[ip++] and jumps directly to its handler label. There is
 * no loop-top branch — each handler's NEXT jumps straight to the next
 * handler. The CPU's branch predictor learns each opcode's target
 * independently, which is why this is faster than switch.
 *
 * vm_run() returns when:
 *   - OP_HALT is hit
 *   - OP_RET is hit with an empty return stack
 *     (the normal exit path for a top-level word call)
 *
 * The dispatch table (tbl[]) is static and initialized once. Any opcode
 * not in the table dispatches to l_unknown.
 * ========================================================================= */

#define NEXT  goto *tbl[vm->mem[vm->ip++]]

void vm_run(VM *vm)
{
    /* One-time initialization of the dispatch table */
    static void *tbl[256];
    static int ready = 0;
    if (!ready) {
        for (int i = 0; i < 256; i++) tbl[i] = &&l_unknown;
        tbl[OP_HALT]   = &&l_halt;
        tbl[OP_RET]    = &&l_ret;
        tbl[OP_CALL]   = &&l_call;
        tbl[OP_JMP]    = &&l_jmp;
        tbl[OP_JZ]     = &&l_jz;
        tbl[OP_LIT]    = &&l_lit;
        tbl[OP_ADD]    = &&l_add;
        tbl[OP_SUB]    = &&l_sub;
        tbl[OP_MUL]    = &&l_mul;
        tbl[OP_DIV]    = &&l_div;
        tbl[OP_DUP]    = &&l_dup;
        tbl[OP_DROP]   = &&l_drop;
        tbl[OP_SWAP]   = &&l_swap;
        tbl[OP_OVER]   = &&l_over;
        tbl[OP_EQ]     = &&l_eq;
        tbl[OP_LT]     = &&l_lt;
        tbl[OP_GT]     = &&l_gt;
        tbl[OP_ZEROEQ] = &&l_zeroeq;
        tbl[OP_EMIT]   = &&l_emit;
        tbl[OP_KEY]    = &&l_key;
        tbl[OP_DOT]    = &&l_dot;
        tbl[OP_DOTS]   = &&l_dots;
        tbl[OP_FETCH]  = &&l_fetch;
        tbl[OP_STORE]  = &&l_store;
        tbl[OP_TOR]    = &&l_tor;
        tbl[OP_RFROM]  = &&l_rfrom;
        tbl[OP_CR]     = &&l_cr;
        tbl[OP_SPACE]  = &&l_space;
        tbl[OP_IF]     = &&l_if;
        tbl[OP_THEN]   = &&l_then;
        tbl[OP_ELSE]   = &&l_else;
        tbl[OP_BEGIN]  = &&l_begin;
        tbl[OP_UNTIL]  = &&l_until;
        tbl[OP_WHILE]  = &&l_while;
        tbl[OP_REPEAT] = &&l_repeat;
        tbl[OP_NEGATE] = &&l_negate;
        tbl[OP_DEPTH]  = &&l_depth;
        tbl[OP_ABS]    = &&l_abs;
        tbl[OP_MAX]    = &&l_max;
        tbl[OP_MIN]    = &&l_min;
        tbl[OP_MOD]    = &&l_mod;
        tbl[OP_DIVMOD] = &&l_divmod;
        tbl[OP_AND]    = &&l_and;
        tbl[OP_OR]     = &&l_or;
        tbl[OP_XOR]    = &&l_xor;
        tbl[OP_INVERT] = &&l_invert;
        tbl[OP_LSHIFT] = &&l_lshift;
        tbl[OP_RSHIFT] = &&l_rshift;
        tbl[OP_ZEROLT] = &&l_zerolt;
        tbl[OP_ZEROGT] = &&l_zerogt;
        tbl[OP_NEQ]    = &&l_neq;
        tbl[OP_ULT]    = &&l_ult;
        tbl[OP_ROT]    = &&l_rot;
        tbl[OP_QDUP]   = &&l_qdup;
        tbl[OP_2SWAP]  = &&l_2swap;
        tbl[OP_2OVER]  = &&l_2over;
        tbl[OP_PICK]   = &&l_pick;
        tbl[OP_ROLL]   = &&l_roll;
        tbl[OP_RFETCH] = &&l_rfetch;
        tbl[OP_EXIT]   = &&l_exit;
        tbl[OP_AGAIN]  = &&l_again;
        tbl[OP_CFETCH] = &&l_cfetch;
        tbl[OP_CSTORE] = &&l_cstore;
        tbl[OP_PLUSST] = &&l_plusst;
        tbl[OP_HERE]      = &&l_here;
        tbl[OP_ALLOT]     = &&l_allot;
        tbl[OP_COMMA]     = &&l_comma;
        tbl[OP_CCOMMA]    = &&l_ccomma;
        tbl[OP_COUNT]     = &&l_count;
        tbl[OP_LBRACKET]  = &&l_lbracket;
        tbl[OP_RBRACKET]  = &&l_rbracket;
        tbl[OP_LITERAL]   = &&l_literal;
        tbl[OP_SETIMM]    = &&l_setimm;
        tbl[OP_TYPE]      = &&l_type;
        tbl[OP_ACCEPT]    = &&l_accept;
        tbl[OP_MOVE]      = &&l_move;
        tbl[OP_FILL]      = &&l_fill;
        tbl[OP_DO]      = &&l_do;
        tbl[OP_QDO]     = &&l_qdo;
        tbl[OP_LOOP]    = &&l_loop;
        tbl[OP_PLOOP]   = &&l_ploop;
        tbl[OP_LEAVE]   = &&l_leave;
        tbl[OP_DO_RT]   = &&l_do_rt;
        tbl[OP_QDO_RT]  = &&l_qdo_rt;
        tbl[OP_LOOP_RT] = &&l_loop_rt;
        tbl[OP_PLOOP_RT]= &&l_ploop_rt;
        tbl[OP_LEAVE_RT]= &&l_leave_rt;
        tbl[OP_I]       = &&l_i;
        tbl[OP_J]       = &&l_j;
        tbl[OP_UNLOOP]  = &&l_unloop;
        tbl[OP_S2D]     = &&l_s2d;
        tbl[OP_UMSTAR]  = &&l_umstar;
        tbl[OP_MSTAR]   = &&l_mstar;
        tbl[OP_UMDIVMOD] = &&l_umdivmod;
        tbl[OP_FMDIVMOD] = &&l_fmdivmod;
        tbl[OP_SMDIVREM] = &&l_smdivrem;
        tbl[OP_DPLUS]    = &&l_dplus;
        tbl[OP_DNEGATE]  = &&l_dnegate;
        tbl[OP_INCLUDE]   = &&l_include;
        tbl[OP_SOURCE]    = &&l_source;
        tbl[OP_PARSENAME] = &&l_parsename;
        tbl[OP_WORD] = &&l_word;
        tbl[OP_CHAR] = &&l_char;
        tbl[OP_FIND] = &&l_find;
        tbl[OP_EXECUTE]  = &&l_execute;
        tbl[OP_TONUMBER] = &&l_tonumber;
        tbl[OP_QUIT]     = &&l_quit;
        tbl[OP_EVALUATE] = &&l_evaluate;
        tbl[OP_CREATE]    = &&l_create;
        tbl[OP_BACKSLASH]    = &&l_backslash;
        tbl[OP_COMPILE_CALL] = &&l_compile_call;
        tbl[OP_CONSTANT]     = &&l_constant_prim;
        tbl[OP_COLON_RT]     = &&l_colon_rt;
        tbl[OP_SEMICOLON_RT] = &&l_semicolon_rt;
        tbl[OP_DOES_CALL]    = &&l_does_call;
        tbl[OP_DOES_SETUP]   = &&l_does_setup;
        tbl[OP_DOES_IMM]     = &&l_does_imm;
        tbl[OP_UDDIVMOD]     = &&l_uddivmod_ext;
        tbl[OP_ONEPLUS]  = &&l_oneplus;
        tbl[OP_ONEMINUS] = &&l_oneminus;
        tbl[OP_TWOSTAR]  = &&l_twostar;
        tbl[OP_TWOSLASH] = &&l_twoslash;
        tbl[OP_CELLPLUS] = &&l_cellplus;
        tbl[OP_CELLS]    = &&l_cells;
        tbl[OP_CHARPLUS] = &&l_charplus;
        tbl[OP_CHARS]    = &&l_chars;
        tbl[OP_ALIGNED]   = &&l_aligned;
        tbl[OP_ALIGN]     = &&l_align;
        tbl[OP_BYE]       = &&l_bye;
        tbl[OP_SAVEIMAGE] = &&l_saveimage;
        tbl[OP_TURNKEY]   = &&l_turnkey;
        tbl[OP_LATEST]    = &&l_latest;
        tbl[OP_SETHERE]   = &&l_sethere;
        tbl[OP_SETLATEST] = &&l_setlatest;
        ready = 1;
    }

    NEXT;   /* start dispatch */

/* --- Control flow ---------------------------------------------------- */

l_halt:
    return;

l_ret:
    if (vm->rsp == 0) return;
    vm->ip = (uint32_t)vm->rs[--vm->rsp];
    NEXT;

l_call: {
    uint32_t addr = read32(vm, vm->ip);
    vm->ip += 4;
    vm->rs[vm->rsp++] = (int32_t)vm->ip;
    vm->ip = addr;
    NEXT;
}

l_jmp: {
    uint32_t addr = read32(vm, vm->ip);
    vm->ip = addr;
    NEXT;
}

l_jz: {
    uint32_t addr = read32(vm, vm->ip);
    vm->ip += 4;
    if (dpop(vm) == 0) vm->ip = addr;
    NEXT;
}

l_lit: {
    int32_t v = (int32_t)read32(vm, vm->ip);
    vm->ip += 4;
    dpush(vm, v);
    NEXT;
}

/* --- Arithmetic ------------------------------------------------------- */

l_add:    { int32_t b = dpop(vm), a = dpop(vm); dpush(vm, a + b);  NEXT; }
l_sub:    { int32_t b = dpop(vm), a = dpop(vm); dpush(vm, a - b);  NEXT; }
l_mul:    { int32_t b = dpop(vm), a = dpop(vm); dpush(vm, a * b);  NEXT; }
l_negate: { dpush(vm, -dpop(vm)); NEXT; }
l_oneplus:  { vm->ds[vm->dsp] += 1; NEXT; }
l_oneminus: { vm->ds[vm->dsp] -= 1; NEXT; }
l_twostar:  { vm->ds[vm->dsp] *= 2; NEXT; }
l_twoslash: { vm->ds[vm->dsp] = (int32_t)vm->ds[vm->dsp] >> 1; NEXT; }
l_cellplus:  { vm->ds[vm->dsp] += 4; NEXT; }
l_cells:     { vm->ds[vm->dsp] *= 4; NEXT; }
l_charplus:  { vm->ds[vm->dsp] += 1; NEXT; }
l_chars:     { NEXT; }   /* no-op on byte-addressed machines */
l_aligned:   { vm->ds[vm->dsp] = (int32_t)(((uint32_t)vm->ds[vm->dsp] + 3u) & ~3u); NEXT; }
l_align: {
    uint32_t a = (vm->here + 3) & ~3;
    vm->here = a;
    NEXT;
}
l_bye:       { exit(0); }
l_saveimage: { do_save_image(vm, 0);           NEXT; }
l_turnkey:   { uint32_t xt = (uint32_t)dpop(vm); do_save_image(vm, xt); NEXT; }
l_latest:    { dpush(vm, (int32_t)vm->latest);          NEXT; }
l_sethere:   { vm->here   = (uint32_t)dpop(vm);        NEXT; }
l_setlatest: { vm->latest = (uint32_t)dpop(vm);        NEXT; }
l_depth:  { dpush(vm, (int32_t)(vm->dsp + 1)); NEXT; }
l_abs:    { int32_t n = dpop(vm); dpush(vm, n < 0 ? -n : n); NEXT; }
l_max:    { int32_t b = dpop(vm), a = dpop(vm); dpush(vm, a > b ? a : b); NEXT; }
l_min:    { int32_t b = dpop(vm), a = dpop(vm); dpush(vm, a < b ? a : b); NEXT; }
l_mod: {
    int32_t b = dpop(vm), a = dpop(vm);
    if (b == 0) { fputs("error: division by zero\n", stderr); dpush(vm, 0); NEXT; }
    dpush(vm, a % b);
    NEXT;
}
l_zerolt: { dpush(vm, dpop(vm) <  0 ? -1 : 0); NEXT; }
l_zerogt: { dpush(vm, dpop(vm) >  0 ? -1 : 0); NEXT; }
l_neq:    { int32_t b=dpop(vm), a=dpop(vm); dpush(vm, a != b ? -1 : 0); NEXT; }
l_ult:    { uint32_t b=(uint32_t)dpop(vm), a=(uint32_t)dpop(vm); dpush(vm, a < b ? -1 : 0); NEXT; }

l_rot: {
    /* ( a b c -- b c a ) */
    int32_t c = dpop(vm), b = dpop(vm), a = dpop(vm);
    dpush(vm, b); dpush(vm, c); dpush(vm, a);
    NEXT;
}

l_qdup: {
    /* ( n -- n n | 0 ) — dup only if non-zero */
    if (dpeek(vm) != 0) dpush(vm, dpeek(vm));
    NEXT;
}

l_2swap: {
    /* ( a b c d -- c d a b ) */
    int32_t d = dpop(vm), c = dpop(vm), b = dpop(vm), a = dpop(vm);
    dpush(vm, c); dpush(vm, d); dpush(vm, a); dpush(vm, b);
    NEXT;
}

l_2over: {
    /* ( a b c d -- a b c d a b ) — copy the pair two below TOS */
    if (vm->dsp < 3) { fputs("stack underflow\n", stderr); NEXT; }
    int32_t a = vm->ds[vm->dsp - 3];
    int32_t b = vm->ds[vm->dsp - 2];
    dpush(vm, a); dpush(vm, b);
    NEXT;
}

l_pick: {
    /* ( xu..x0 u -- xu..x0 xu ) — copy item u deep (0 = TOS) */
    int32_t u = dpop(vm);
    if (u < 0 || u > vm->dsp) { fputs("pick: out of range\n", stderr); dpush(vm, 0); NEXT; }
    dpush(vm, vm->ds[vm->dsp - u]);
    NEXT;
}

l_roll: {
    /* ( xu..x0 u -- xu-1..x0 xu ) — move item u deep to TOS */
    int32_t u = dpop(vm);
    if (u < 0 || u > vm->dsp) { fputs("roll: out of range\n", stderr); NEXT; }
    int idx = vm->dsp - u;
    int32_t val = vm->ds[idx];
    memmove(&vm->ds[idx], &vm->ds[idx + 1], (size_t)(vm->dsp - idx) * sizeof(int32_t));
    vm->ds[vm->dsp] = val;
    NEXT;
}

l_and:    { int32_t b=dpop(vm), a=dpop(vm); dpush(vm, a & b);  NEXT; }
l_or:     { int32_t b=dpop(vm), a=dpop(vm); dpush(vm, a | b);  NEXT; }
l_xor:    { int32_t b=dpop(vm), a=dpop(vm); dpush(vm, a ^ b);  NEXT; }
l_invert: { dpush(vm, ~dpop(vm)); NEXT; }
l_lshift: { int32_t u=dpop(vm), n=dpop(vm); dpush(vm, n << u); NEXT; }
l_rshift: { int32_t u=dpop(vm), n=dpop(vm); dpush(vm, (int32_t)((uint32_t)n >> u)); NEXT; }

l_divmod: {
    int32_t b = dpop(vm), a = dpop(vm);
    if (b == 0) { fputs("error: division by zero\n", stderr); dpush(vm, 0); dpush(vm, 0); NEXT; }
    dpush(vm, a % b);   /* remainder */
    dpush(vm, a / b);   /* quotient  */
    NEXT;
}
l_div: {
    int32_t b = dpop(vm), a = dpop(vm);
    if (b == 0) { fputs("error: division by zero\n", stderr); dpush(vm, 0); NEXT; }
    dpush(vm, a / b);
    NEXT;
}

/* --- Stack ------------------------------------------------------------ */

l_dup:  dpush(vm, dpeek(vm)); NEXT;
l_drop: dpop(vm);             NEXT;

l_swap: {
    int32_t b = dpop(vm), a = dpop(vm);
    dpush(vm, b); dpush(vm, a);
    NEXT;
}

l_over: {
    if (vm->dsp < 1) { fputs("stack underflow\n", stderr); NEXT; }
    dpush(vm, vm->ds[vm->dsp - 1]);
    NEXT;
}

/* --- Comparison (truth: -1 = true, 0 = false) ------------------------ */

l_eq:     { int32_t b=dpop(vm),a=dpop(vm); dpush(vm, a==b ? -1 : 0); NEXT; }
l_lt:     { int32_t b=dpop(vm),a=dpop(vm); dpush(vm, a< b ? -1 : 0); NEXT; }
l_gt:     { int32_t b=dpop(vm),a=dpop(vm); dpush(vm, a> b ? -1 : 0); NEXT; }
l_zeroeq: { dpush(vm, dpop(vm) == 0 ? -1 : 0); NEXT; }

/* --- I/O ------------------------------------------------------------- */

/* OP_EMIT */
l_emit: {
    char buf[2] = { (char)(dpop(vm) & 0xFF), '\0' };
    gfx_print(buf);
    NEXT;
}

l_key: {
    int c = getchar();
    dpush(vm, c == EOF ? -1 : (int32_t)(unsigned char)c);
    NEXT;
}

/* OP_DOT */
l_dot: {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d ", (int)dpop(vm));
    gfx_print(buf);
    NEXT;
}

/* OP_DOTS */
l_dots: {
    char buf[32];
    snprintf(buf, sizeof(buf), "<%d> ", vm->dsp + 1);
    gfx_print(buf);
    for (int i = 0; i <= vm->dsp; i++) {
        snprintf(buf, sizeof(buf), "%d ", (int)vm->ds[i]);
        gfx_print(buf);
    }
    NEXT;
}

/* --- Memory ---------------------------------------------------------- */

l_fetch: {
    uint32_t addr = (uint32_t)dpop(vm);
    dpush(vm, (int32_t)read32(vm, addr));
    NEXT;
}

l_store: {
    uint32_t addr = (uint32_t)dpop(vm);
    int32_t  val  = dpop(vm);
    write32(vm, addr, (uint32_t)val);
    NEXT;
}

/* --- Return stack ---------------------------------------------------- */

l_tor:   vm->rs[vm->rsp++] = dpop(vm);           NEXT;
l_rfrom: dpush(vm, vm->rs[--vm->rsp]);            NEXT;

/* --- Output shortcuts ------------------------------------------------ */

/* OP_CR */    l_cr:    gfx_print("\n"); NEXT;
/* OP_SPACE */ l_space: gfx_print(" ");  NEXT;

/* --- Compile-time control flow --------------------------------------- *
 *
 * These handlers run at compile time (IMMEDIATE words).  They emit JZ/JMP
 * opcodes into the word being compiled and pass patch addresses via the
 * data stack.  The compiled word never executes these opcodes — it only
 * sees the JZ/JMP instructions that were emitted.
 *
 * Stack diagrams show compile-time stack state, not runtime.
 */

l_if: {
    /* ( -- patch_addr )
     * Emit JZ with a zero placeholder.  Push the address of that
     * placeholder so THEN or ELSE can fill it in later. */
    emit_byte(vm, OP_JZ);
    uint32_t patch = vm->here;     /* address of the 4-byte placeholder */
    emit_u32(vm, 0);               /* placeholder — filled by THEN/ELSE */
    dpush(vm, (int32_t)patch);
    NEXT;
}

l_then: {
    /* ( patch_addr -- )
     * Fill in the placeholder left by IF or ELSE with the current HERE.
     * This makes the earlier JZ/JMP land right here (after THEN). */
    uint32_t patch = (uint32_t)dpop(vm);
    write32(vm, patch, vm->here);
    NEXT;
}

l_else: {
    /* ( if_patch -- else_patch )
     * Called between IF and THEN.
     * 1. Emit an unconditional JMP (to skip the else-branch) with a placeholder.
     * 2. Patch IF's JZ to jump here (the start of the else-branch).
     * 3. Push the JMP's placeholder for THEN to fill in. */
    uint32_t if_patch = (uint32_t)dpop(vm);
    emit_byte(vm, OP_JMP);
    uint32_t else_patch = vm->here;
    emit_u32(vm, 0);               /* placeholder for JMP target */
    write32(vm, if_patch, vm->here); /* patch IF's JZ to land here */
    dpush(vm, (int32_t)else_patch);
    NEXT;
}

l_begin: {
    /* ( -- loop_addr )
     * Push HERE as the loop-back target.  UNTIL will compile a JZ to it. */
    dpush(vm, (int32_t)vm->here);
    NEXT;
}

l_until: {
    /* ( loop_addr -- )
     * Emit JZ back to the address BEGIN pushed.
     * At runtime: pop TOS — if zero (condition false), jump back; otherwise fall through. */
    uint32_t loop_addr = (uint32_t)dpop(vm);
    emit_byte(vm, OP_JZ);
    emit_u32(vm, loop_addr);
    NEXT;
}

l_while: {
    /* ANS: ( C: dest -- orig dest )
     * Pops the loop-back dest (from BEGIN), emits JZ placeholder (orig),
     * then pushes orig below dest so REPEAT sees dest on top.
     * This ordering allows multiple WHILEs: each adds its orig below dest,
     * so REPEAT always pops dest then the innermost orig.
     * Outer WHILEs' origs are left for ELSE/THEN to resolve. */
    uint32_t dest = (uint32_t)dpop(vm);   /* pop begin_addr */
    emit_byte(vm, OP_JZ);
    uint32_t orig = vm->here;
    emit_u32(vm, 0);
    dpush(vm, (int32_t)orig);   /* push orig (JZ patch) */
    dpush(vm, (int32_t)dest);   /* push dest (begin_addr) back on top */
    NEXT;
}

l_repeat: {
    /* ANS: ( C: orig dest -- )
     * dest is on top (begin_addr), orig is just below (while's JZ patch).
     * Emits JMP back to dest, then patches orig's JZ to land after the JMP. */
    uint32_t dest       = (uint32_t)dpop(vm);  /* begin_addr */
    uint32_t while_orig = (uint32_t)dpop(vm);  /* JZ patch */
    emit_byte(vm, OP_JMP);
    emit_u32(vm, dest);
    write32(vm, while_orig, vm->here);   /* JZ exits to here (after JMP) */
    NEXT;
}

/* --- Step 5: R@ EXIT AGAIN ------------------------------------------ */

l_rfetch: {
    /* r@ ( -- n )  copy top of return stack without removing it */
    dpush(vm, vm->rs[vm->rsp - 1]);
    NEXT;
}

l_exit: {
    /* exit ( -- )  early return from word — identical to OP_RET */
    if (vm->rsp == 0) goto l_halt;
    vm->ip = (uint32_t)vm->rs[--vm->rsp];
    NEXT;
}

l_again: {
    /* again IMMEDIATE ( begin_addr -- )
     * Emit JMP back to the address BEGIN pushed — unconditional infinite loop. */
    uint32_t loop_addr = (uint32_t)dpop(vm);
    emit_byte(vm, OP_JMP);
    emit_u32(vm, loop_addr);
    NEXT;
}

/* --- Step 6: C@ C! +! ----------------------------------------------- */

l_cfetch: {
    uint32_t addr = (uint32_t)dpop(vm);
    dpush(vm, (int32_t)vm->mem[addr]);
    NEXT;
}

l_cstore: {
    uint32_t addr = (uint32_t)dpop(vm);
    int32_t  val  = dpop(vm);
    vm->mem[addr] = (uint8_t)(val & 0xFF);
    NEXT;
}

l_plusst: {
    uint32_t addr = (uint32_t)dpop(vm);
    int32_t  n    = dpop(vm);
    write32(vm, addr, read32(vm, addr) + (uint32_t)n);
    NEXT;
}

/* --- Step 7: HERE ALLOT , C, COUNT ---------------------------------- */

l_here: {
    dpush(vm, (int32_t)vm->here);
    NEXT;
}

l_allot: {
    vm->here += (uint32_t)dpop(vm);
    NEXT;
}

l_comma: {
    write32(vm, vm->here, (uint32_t)dpop(vm));
    vm->here += 4;
    NEXT;
}

l_ccomma: {
    vm->mem[vm->here++] = (uint8_t)(dpop(vm) & 0xFF);
    NEXT;
}

l_count: {
    uint32_t addr = (uint32_t)dpop(vm);
    dpush(vm, (int32_t)(addr + 1));
    dpush(vm, (int32_t)vm->mem[addr]);
    NEXT;
}

/* --- Step 8: [ ] LITERAL IMMEDIATE ---------------------------------- */

l_lbracket: {
    /* [ IMMEDIATE — switch to interpret mode */
    write32(vm, ADDR_STATE, 0);
    NEXT;
}

l_rbracket: {
    /* ] — switch to compile mode */
    write32(vm, ADDR_STATE, 1);
    NEXT;
}

l_literal: {
    /* literal IMMEDIATE — pop compile-time value, emit OP_LIT + value */
    int32_t val = dpop(vm);
    emit_byte(vm, OP_LIT);
    emit_u32(vm, (uint32_t)val);
    NEXT;
}

l_setimm: {
    /* immediate — set F_IMMEDIATE flag on the most recently defined word */
    vm->mem[vm->latest + 4] |= F_IMMEDIATE;
    NEXT;
}

/* --- Step 9: TYPE ACCEPT MOVE FILL ---------------------------------- */

/* OP_TYPE */
l_type: {
    int32_t  u    = dpop(vm);
    uint32_t addr = (uint32_t)dpop(vm);
    char buf[2] = { 0, 0 };
    for (int32_t i = 0; i < u; i++) {
        buf[0] = (char)vm->mem[addr + (uint32_t)i];
        gfx_print(buf);
    }
    NEXT;
}

l_accept: {
    /* accept ( c-addr +n -- +n )  read up to n chars from stdin into buffer */
    int32_t  n    = dpop(vm);
    uint32_t addr = (uint32_t)dpop(vm);
    int32_t  i    = 0;
    int      c;
    while (i < n && (c = getchar()) != EOF && c != '\n')
        vm->mem[addr + i++] = (uint8_t)c;
    dpush(vm, i);
    NEXT;
}

l_move: {
    /* move ( addr1 addr2 u -- )  copy u bytes; memmove handles overlaps */
    int32_t  u     = dpop(vm);
    uint32_t addr2 = (uint32_t)dpop(vm);
    uint32_t addr1 = (uint32_t)dpop(vm);
    if (u > 0)
        memmove(&vm->mem[addr2], &vm->mem[addr1], (size_t)u);
    NEXT;
}

l_fill: {
    /* fill ( c-addr u char -- )  fill u bytes at c-addr with char */
    int32_t  ch   = dpop(vm);
    int32_t  u    = dpop(vm);
    uint32_t addr = (uint32_t)dpop(vm);
    if (u > 0)
        memset(&vm->mem[addr], (int)(ch & 0xFF), (size_t)u);
    NEXT;
}

/* --- Step 10: DO/LOOP — compile-time IMMEDIATE handlers ---------------- *
 *
 * These run at compile time when the IMMEDIATE word is encountered.
 * They emit runtime opcodes (OP_DO_RT etc.) into the word being defined
 * and manage a 3-slot compile-time stack group:
 *
 *   ( skip_or_0   leave_chain   loop_body_addr )   <- TOS
 *
 *   skip_or_0   : address of ?DO's forward-skip placeholder (0 for plain DO)
 *   leave_chain : head of the LEAVE linked list (0 = empty)
 *   loop_body_addr : address of first byte after OP_DO_RT/OP_QDO_RT
 */

l_do: {
    /* do IMMEDIATE — push loop frame to cloop stack; nothing on data stack.
     * Separate from data stack so nested IF/THEN cannot corrupt loop info. */
    if (cloop_depth >= 16) { fputs("error: DO nesting too deep\n", stderr); goto l_halt; }
    emit_byte(vm, OP_DO_RT);
    cloop_skip[cloop_depth]  = 0;           /* plain DO: no ?DO skip patch */
    cloop_chain[cloop_depth] = 0;           /* leave chain: empty */
    cloop_body[cloop_depth]  = vm->here;    /* body starts right after DO_RT */
    cloop_depth++;
    NEXT;
}

l_qdo: {
    /* ?do IMMEDIATE — emit OP_QDO_RT + 4-byte skip placeholder; push cloop frame. */
    if (cloop_depth >= 16) { fputs("error: DO nesting too deep\n", stderr); goto l_halt; }
    emit_byte(vm, OP_QDO_RT);
    uint32_t skip_patch = vm->here;
    emit_u32(vm, 0);                /* placeholder: filled by LOOP/+LOOP */
    cloop_skip[cloop_depth]  = skip_patch;  /* address of skip placeholder */
    cloop_chain[cloop_depth] = 0;           /* leave chain: empty */
    cloop_body[cloop_depth]  = vm->here;    /* body starts after QDO_RT + 4-byte op */
    cloop_depth++;
    NEXT;
}

l_leave: {
    /* leave IMMEDIATE — emit OP_LEAVE_RT; thread into the innermost loop's
     * leave chain via cloop_chain[].  Data stack is untouched, so nested
     * IF/THEN inside a loop cannot corrupt the loop frame info. */
    int d = cloop_depth - 1;              /* index of innermost open loop */
    emit_byte(vm, OP_LEAVE_RT);
    emit_u32(vm, cloop_chain[d]);         /* chain: old head (0 = end) */
    cloop_chain[d] = vm->here - 4;        /* new chain head = address of that slot */
    NEXT;
}

l_loop: {
    /* loop IMMEDIATE — emit OP_LOOP_RT + branch-back; patch LEAVE chain and ?DO skip.
     * All loop frame info comes from cloop stack (not data stack). */
    int d = --cloop_depth;
    uint32_t loop_body   = cloop_body[d];
    uint32_t leave_chain = cloop_chain[d];
    uint32_t skip_patch  = cloop_skip[d];
    emit_byte(vm, OP_LOOP_RT);
    emit_u32(vm, loop_body);              /* branch-back target */
    /* patch all LEAVE exits to HERE */
    uint32_t patch = leave_chain;
    while (patch != 0) {
        uint32_t next = read32(vm, patch);
        write32(vm, patch, vm->here);
        patch = next;
    }
    /* patch ?DO forward skip */
    if (skip_patch != 0) write32(vm, skip_patch, vm->here);
    NEXT;
}

l_ploop: {
    /* +loop IMMEDIATE — same as l_loop but emits OP_PLOOP_RT.
     * All loop frame info comes from cloop stack (not data stack). */
    int d = --cloop_depth;
    uint32_t loop_body   = cloop_body[d];
    uint32_t leave_chain = cloop_chain[d];
    uint32_t skip_patch  = cloop_skip[d];
    emit_byte(vm, OP_PLOOP_RT);
    emit_u32(vm, loop_body);
    uint32_t patch = leave_chain;
    while (patch != 0) {
        uint32_t next = read32(vm, patch);
        write32(vm, patch, vm->here);
        patch = next;
    }
    if (skip_patch != 0) write32(vm, skip_patch, vm->here);
    NEXT;
}

/* --- Step 10: DO/LOOP — runtime opcodes -------------------------------- *
 *
 * These opcodes appear inside compiled words and execute at runtime.
 * Return stack layout during a loop:
 *   rs[rsp-2] = limit   (boundary)
 *   rs[rsp-1] = index   (current counter, TOS of RS)
 */

l_do_rt: {
    /* ( limit index -- )  push loop frame onto return stack */
    int32_t index = dpop(vm);
    int32_t limit = dpop(vm);
    vm->rs[vm->rsp++] = limit;
    vm->rs[vm->rsp++] = index;
    NEXT;
}

l_qdo_rt: {
    /* ( limit index -- )  like DO_RT but skip loop if limit == index
     * Operand: 4-byte exit address (patched by LOOP). */
    int32_t index = dpop(vm);
    int32_t limit = dpop(vm);
    uint32_t skip_addr = read32(vm, vm->ip);
    vm->ip += 4;
    if (limit == index) {
        vm->ip = skip_addr;
    } else {
        vm->rs[vm->rsp++] = limit;
        vm->rs[vm->rsp++] = index;
    }
    NEXT;
}

l_loop_rt: {
    /* Increment index; if index < limit, jump to body; else drop frame.
     * Operand: 4-byte branch-back address (body start). */
    uint32_t body = read32(vm, vm->ip);
    vm->ip += 4;
    int32_t index = ++vm->rs[vm->rsp - 1];
    int32_t limit =   vm->rs[vm->rsp - 2];
    if (index < limit) {
        vm->ip = body;
    } else {
        vm->rsp -= 2;           /* drop loop frame */
    }
    NEXT;
}

l_ploop_rt: {
    /* Pop n; add to index.  Exit when displaced index crosses zero.
     * Test: ((old - limit) ^ (new - limit)) < 0 means boundary crossed.
     * Operand: 4-byte branch-back address (body start). */
    uint32_t body = read32(vm, vm->ip);
    vm->ip += 4;
    int32_t n     = dpop(vm);
    int32_t limit = vm->rs[vm->rsp - 2];
    int32_t old   = vm->rs[vm->rsp - 1];
    int32_t new_  = old + n;
    vm->rs[vm->rsp - 1] = new_;
    if (((old - limit) ^ (new_ - limit)) < 0) {
        vm->rsp -= 2;           /* crossed boundary — drop frame */
    } else {
        vm->ip = body;          /* not done — jump back */
    }
    NEXT;
}

l_leave_rt: {
    /* Drop loop frame and jump to exit address.
     * Operand: 4-byte exit address (patched by LOOP from the leave chain). */
    uint32_t exit_addr = read32(vm, vm->ip);
    vm->ip += 4;                /* advance past operand (in case rsp check needed) */
    vm->rsp -= 2;               /* drop loop frame: limit and index */
    vm->ip = exit_addr;
    NEXT;
}

/* --- Step 10: I / J / UNLOOP --- */

l_i: {
    /* i ( -- n )  copy loop index (non-destructive) */
    dpush(vm, vm->rs[vm->rsp - 1]);
    NEXT;
}

l_j: {
    /* j ( -- n )  copy outer loop index (non-destructive) */
    dpush(vm, vm->rs[vm->rsp - 3]);
    NEXT;
}

l_unloop: {
    /* unloop ( -- )  drop loop frame — use before EXIT inside a loop */
    vm->rsp -= 2;
    NEXT;
}

l_s2d: {
    /* s>d ( n -- lo hi )  sign-extend single to double */
    int32_t n = dpop(vm);
    dpush(vm, n);
    dpush(vm, n < 0 ? -1 : 0);
    NEXT;
}

l_umstar: {
    /* um* ( u1 u2 -- lo hi )  unsigned 32×32→64 */
    uint64_t r = (uint64_t)(uint32_t)dpop(vm) * (uint64_t)(uint32_t)dpop(vm);
    dpush(vm, (int32_t)(r & 0xFFFFFFFF));
    dpush(vm, (int32_t)(r >> 32));
    NEXT;
}

l_mstar: {
    /* m* ( n1 n2 -- lo hi )  signed 32×32→64 */
    int64_t r = (int64_t)dpop(vm) * (int64_t)dpop(vm);
    dpush(vm, (int32_t)(r & 0xFFFFFFFF));
    dpush(vm, (int32_t)(r >> 32));
    NEXT;
}

l_umdivmod: {
    /* um/mod ( lo hi u -- rem quot )  unsigned 64÷32 */
    uint32_t u  = (uint32_t)dpop(vm);
    uint32_t hi = (uint32_t)dpop(vm);
    uint32_t lo = (uint32_t)dpop(vm);
    uint64_t ud = ((uint64_t)hi << 32) | lo;
    dpush(vm, (int32_t)(ud % u));   /* remainder */
    dpush(vm, (int32_t)(ud / u));   /* quotient on TOS */
    NEXT;
}

l_fmdivmod: {
    /* fm/mod ( lo hi n -- rem quot )  floored signed division */
    int32_t n  = dpop(vm);
    int32_t hi = dpop(vm);
    int32_t lo = dpop(vm);
    int64_t d  = ((int64_t)hi << 32) | (uint32_t)lo;
    int64_t q  = d / n;
    int64_t r  = d % n;
    /* floor toward -∞: adjust when remainder and divisor have opposite signs.
     * (r ^ n) < 0 tests this: XOR of two values with different sign bits is negative. */
    if (r != 0 && ((r ^ (int64_t)n) < 0)) { r += n; q--; }
    dpush(vm, (int32_t)r);
    dpush(vm, (int32_t)q);
    NEXT;
}

l_smdivrem: {
    /* sm/rem ( lo hi n -- rem quot )  symmetric (truncated) signed division */
    int32_t n  = dpop(vm);
    int32_t hi = dpop(vm);
    int32_t lo = dpop(vm);
    int64_t d  = ((int64_t)hi << 32) | (uint32_t)lo;
    dpush(vm, (int32_t)(d % n));   /* remainder */
    dpush(vm, (int32_t)(d / n));   /* quotient on TOS */
    NEXT;
}

l_dplus: {
    /* d+ ( lo1 hi1 lo2 hi2 -- lo hi )  double-cell addition */
    int32_t hi2 = dpop(vm);
    int32_t lo2 = dpop(vm);
    int32_t hi1 = dpop(vm);
    int32_t lo1 = dpop(vm);
    int64_t r = (((int64_t)hi1 << 32) | (uint32_t)lo1)
              + (((int64_t)hi2 << 32) | (uint32_t)lo2);
    dpush(vm, (int32_t)(r & 0xFFFFFFFF));
    dpush(vm, (int32_t)(r >> 32));
    NEXT;
}

l_dnegate: {
    /* dnegate ( lo hi -- lo' hi' )  two's complement of a double */
    int32_t hi = dpop(vm);
    int32_t lo = dpop(vm);
    int64_t r  = -(((int64_t)hi << 32) | (uint32_t)lo);
    dpush(vm, (int32_t)(r & 0xFFFFFFFF));
    dpush(vm, (int32_t)(r >> 32));
    NEXT;
}

l_include: {
    uint32_t len  = (uint32_t)dpop(vm);
    uint32_t addr = (uint32_t)dpop(vm);
    char path[256];
    if (len >= sizeof(path)) len = (uint32_t)sizeof(path) - 1;
    memcpy(path, vm->mem + addr, len);
    path[len] = '\0';
    /* Save TIB context — vm_load will call vm_eval which overwrites the TIB,
     * ADDR_IN, and ADDR_SRCLEN.  Restore them afterward so the outer vm_eval
     * loop can continue scanning the line that contained the include word. */
    uint32_t saved_in  = read32(vm, ADDR_IN);
    uint32_t saved_len = read32(vm, ADDR_SRCLEN);
    uint8_t  saved_tib[TIB_SIZE];
    memcpy(saved_tib, vm->mem + TIB_BASE, TIB_SIZE);
    uint32_t saved_ip  = vm->ip;  /* vm_eval clobbers ip; restore for NEXT */
    vm_load(vm, path);
    vm->ip = saved_ip;
    memcpy(vm->mem + TIB_BASE, saved_tib, TIB_SIZE);
    write32(vm, ADDR_SRCLEN, saved_len);
    write32(vm, ADDR_IN,     saved_in);
    NEXT;
}

l_source: {
    /* SOURCE ( -- c-addr u )
     * Returns the address and length of the current input source.
     * For normal input (keyboard/file), this is TIB_BASE and SRCLEN.
     * For EVALUATE, this is the original string address + its length. */
    dpush(vm, (int32_t)read32(vm, ADDR_SRCBASE));
    dpush(vm, (int32_t)read32(vm, ADDR_SRCLEN));
    NEXT;
}

l_parsename: {
    uint32_t in  = read32(vm, ADDR_IN);
    uint32_t len = read32(vm, ADDR_SRCLEN);
    uint8_t *buf = vm->mem + TIB_BASE;
    while (in < len && buf[in] <= ' ') in++;   /* skip whitespace */
    uint32_t start = in;
    while (in < len && buf[in] >  ' ') in++;   /* scan token */
    write32(vm, ADDR_IN, in);
    dpush(vm, (int32_t)(TIB_BASE + start));
    dpush(vm, (int32_t)(in - start));
    NEXT;
}

l_word: {
    uint8_t  delim = (uint8_t)dpop(vm);
    uint32_t in    = read32(vm, ADDR_IN);
    uint32_t slen  = read32(vm, ADDR_SRCLEN);
    uint8_t *buf   = vm->mem + TIB_BASE;
    /* Skip leading whitespace and/or leading delimiter chars.
     * Standard behaviour: if delim is BL, skip all blanks; otherwise skip
     * blanks AND leading delimiter chars (matches common Forth behaviour). */
    while (in < slen && (buf[in] <= ' ' || buf[in] == delim)) in++;
    uint32_t dest = vm->here + 1;                  /* +1: count byte at vm->here */
    uint32_t n = 0;
    while (in < slen && buf[in] != delim) {
        vm->mem[dest + n] = buf[in];
        n++; in++;
    }
    if (in < slen && buf[in] == delim) in++;       /* skip trailing delimiter */
    vm->mem[vm->here] = (uint8_t)n;                /* write count byte */
    write32(vm, ADDR_IN, in);
    /* Note: vm->here is NOT advanced — HERE is a transient scratch buffer for word */
    dpush(vm, (int32_t)vm->here);
    NEXT;
}

l_char: {
    uint32_t in  = read32(vm, ADDR_IN);
    uint32_t len = read32(vm, ADDR_SRCLEN);
    uint8_t *buf = vm->mem + TIB_BASE;
    while (in < len && buf[in] <= ' ') in++;
    uint8_t ch = (in < len) ? buf[in] : 0;
    while (in < len && buf[in] >  ' ') in++;
    write32(vm, ADDR_IN, in);
    dpush(vm, (int32_t)ch);
    NEXT;
}

l_find: {
    uint32_t caddr  = (uint32_t)dpop(vm);
    uint8_t  qlen   = vm->mem[caddr];
    uint8_t *qname  = vm->mem + caddr + 1;
    /* Case-fold the query name: dict stores lowercase, input may be uppercase */
    char qlow[256];
    int  qn = qlen > 255 ? 255 : (int)qlen;
    for (int i = 0; i < qn; i++)
        qlow[i] = (char)tolower((unsigned char)qname[i]);
    uint32_t entry  = vm->latest;
    while (entry) {
        uint8_t  flags   = vm->mem[entry + 4];
        uint8_t  namelen = vm->mem[entry + 5];
        uint8_t *name    = vm->mem + entry + 6;
        if (!(flags & F_HIDDEN) && namelen == (uint8_t)qn &&
            memcmp(name, qlow, (size_t)qn) == 0) {
            uint32_t xt = entry + 6 + namelen;    /* body address */
            dpush(vm, (int32_t)xt);
            dpush(vm, (flags & F_IMMEDIATE) ? 1 : -1);
            goto l_find_done;
        }
        entry = read32(vm, entry);                /* follow link */
    }
    dpush(vm, (int32_t)caddr);
    dpush(vm, 0);
    l_find_done: NEXT;
}

l_execute: {
    uint32_t xt = (uint32_t)dpop(vm);
    /* Push return address — same convention as l_call */
    vm->rs[vm->rsp++] = (int32_t)vm->ip;
    vm->ip = xt;
    NEXT;
}

l_tonumber: {
    uint32_t u    = (uint32_t)dpop(vm);
    uint32_t addr = (uint32_t)dpop(vm);
    uint32_t hi   = (uint32_t)dpop(vm);
    uint32_t lo   = (uint32_t)dpop(vm);
    uint32_t base = read32(vm, ADDR_BASE);
    uint64_t ud   = ((uint64_t)hi << 32) | lo;
    while (u > 0) {
        uint8_t c = vm->mem[addr];
        int digit;
        if      (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'z') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z') digit = c - 'A' + 10;
        else break;
        if ((uint32_t)digit >= base) break;
        ud = ud * base + (uint32_t)digit;
        addr++; u--;
    }
    dpush(vm, (int32_t)(uint32_t)(ud & 0xFFFFFFFF));
    dpush(vm, (int32_t)(uint32_t)(ud >> 32));
    dpush(vm, (int32_t)addr);
    dpush(vm, (int32_t)u);
    NEXT;
}

/* --- quit: escape to REPL top via longjmp ---------------------------- */

l_quit: {
    if (vm->quit_active) {
        write32(vm, ADDR_STATE, 0);
        write32(vm, ADDR_SRCID, 0);
        longjmp(vm->quit_jmp, 1);
    }
    NEXT;   /* safety: no-op if not inside vm_repl */
}

/* --- evaluate ( addr u -- ) ---------------------------------------------- */
l_evaluate: {
    uint32_t u    = (uint32_t)dpop(vm);
    uint32_t addr = (uint32_t)dpop(vm);
    /* save input state */
    uint8_t  saved_tib[TIB_SIZE];
    uint32_t saved_srclen  = read32(vm, ADDR_SRCLEN);
    uint32_t saved_in      = read32(vm, ADDR_IN);
    int32_t  saved_srcid   = (int32_t)read32(vm, ADDR_SRCID);
    uint32_t saved_srcbase = read32(vm, ADDR_SRCBASE);
    uint32_t save_len = (saved_srclen < TIB_SIZE) ? saved_srclen : TIB_SIZE;
    memcpy(saved_tib, vm->mem + TIB_BASE, save_len);
    /* evaluate the string */
    char buf[2048];
    if (u >= sizeof(buf)) {
        fputs("error: evaluate string too long\n", stderr);
        u = (uint32_t)sizeof(buf) - 1;
    }
    memcpy(buf, vm->mem + addr, u);
    buf[u] = '\0';
    write32(vm, ADDR_SRCID,   (uint32_t)(int32_t)-1);
    write32(vm, ADDR_SRCBASE, addr);   /* SOURCE returns original string addr */
    uint32_t saved_ip_ev = vm->ip;  /* vm_eval clobbers ip; restore for NEXT */
    vm_eval(vm, buf);
    vm->ip = saved_ip_ev;
    /* restore input state */
    memcpy(vm->mem + TIB_BASE, saved_tib, save_len);
    write32(vm, ADDR_SRCLEN,  saved_srclen);
    write32(vm, ADDR_IN,      saved_in);
    write32(vm, ADDR_SRCID,   (uint32_t)saved_srcid);
    write32(vm, ADDR_SRCBASE, saved_srcbase);
    NEXT;
}

/* --- \ ( -- ) line comment: advance >IN to end of line ------------------- */
l_backslash: {
    write32(vm, ADDR_IN, read32(vm, ADDR_SRCLEN));
    NEXT;
}

l_compile_call: {
    /* compile, ( xt -- )
     * Pops an execution token (body address) and emits OP_CALL + xt at HERE.
     * Used by POSTPONE to defer compilation of non-immediate words. */
    uint32_t xt = (uint32_t)dpop(vm);
    emit_byte(vm, OP_CALL);
    emit_u32(vm, xt);
    NEXT;
}

l_constant_prim: {
    /* constant ( n "name" -- )
     * Runtime version: reads the next whitespace-delimited name from TIB
     * via ADDR_IN, pops value from stack, creates a constant word.
     * This allows CONSTANT to be called from within a colon definition
     * (e.g., ": EQU CONSTANT ;"). */
    uint32_t in     = read32(vm, ADDR_IN);
    uint32_t srclen = read32(vm, ADDR_SRCLEN);
    while (in < srclen && vm->mem[TIB_BASE + in] <= ' ') in++;
    char     cname[256];
    int      clen = 0;
    while (in < srclen && vm->mem[TIB_BASE + in] > ' ' && clen < 255)
        cname[clen++] = (char)tolower((unsigned char)vm->mem[TIB_BASE + in++]);
    cname[clen] = '\0';
    write32(vm, ADDR_IN, in);
    if (clen == 0) { fputs("error: expected name after constant\n", stderr); NEXT; }
    int32_t cval = dpop(vm);
    dict_create(vm, cname, clen, 0);
    emit_byte(vm, OP_LIT);
    emit_u32(vm, (uint32_t)cval);
    emit_byte(vm, OP_RET);
    NEXT;
}

l_colon_rt: {
    /* Runtime ':' ( "name" -- )
     * Reads the next whitespace-delimited name from TIB, creates a hidden
     * dict entry, and sets STATE=1.  Used by defining words like NOP. */
    uint32_t in     = read32(vm, ADDR_IN);
    uint32_t srclen = read32(vm, ADDR_SRCLEN);
    while (in < srclen && vm->mem[TIB_BASE + in] <= ' ') in++;
    char     rname[256];
    int      rlen = 0;
    while (in < srclen && vm->mem[TIB_BASE + in] > ' ' && rlen < 255)
        rname[rlen++] = (char)tolower((unsigned char)vm->mem[TIB_BASE + in++]);
    rname[rlen] = '\0';
    write32(vm, ADDR_IN, in);
    if (rlen == 0) { fputs("error: expected name after :\n", stderr); NEXT; }
    dict_create(vm, rname, rlen, F_HIDDEN);
    write32(vm, ADDR_STATE, 1);
    NEXT;
}

l_semicolon_rt: {
    /* Runtime ';' ( -- )
     * Emits OP_RET at HERE, clears F_HIDDEN on the current definition,
     * and sets STATE=0.  Used by POSTPONE ; in defining words. */
    emit_byte(vm, OP_RET);
    vm->mem[vm->latest + 4] &= ~F_HIDDEN;
    write32(vm, ADDR_STATE, 0);
    NEXT;
}

l_does_call: {
    /* Body of a CREATE'd word: OP_DOES_CALL + 4-byte does_ptr.
     * Reads does_ptr operand, computes data_addr = ip (byte after operand),
     * pushes data_addr on data stack, then:
     *   - does_ptr != 0: tail-call the runtime code (ip = does_ptr)
     *   - does_ptr == 0: return to caller (pop return address from rs)       */
    uint32_t does_ptr  = read32(vm, vm->ip); vm->ip += 4;
    uint32_t data_addr = vm->ip;             /* data area starts right here  */
    dpush(vm, (int32_t)data_addr);
    if (does_ptr != 0) {
        vm->ip = does_ptr;                   /* tail-call: no rs push needed */
    } else {
        if (vm->rsp == 0) goto l_halt;
        vm->ip = (uint32_t)vm->rs[--vm->rsp];
    }
    NEXT;
}

l_does_setup: {
    /* Runtime DOES> setup: reads 4-byte runtime_addr operand, patches the
     * does_ptr slot (body+1) of vm->latest (the most recently CREATE'd word). */
    uint32_t runtime_addr = read32(vm, vm->ip); vm->ip += 4;
    uint32_t body = vm->latest + 6 + vm->mem[vm->latest + 5];
    write32(vm, body + 1, runtime_addr);     /* +1 skips the OP_DOES_CALL byte */
    NEXT;
}

l_does_imm: {
    /* IMMEDIATE compile-time action of DOES>.
     * Emits into the current word:
     *   OP_DOES_SETUP [runtime_addr] OP_RET
     * Patches runtime_addr to vm->here (the start of the does-runtime code).
     * Compilation continues for the runtime tokens after DOES>. */
    emit_byte(vm, OP_DOES_SETUP);
    uint32_t patch = vm->here;
    emit_u32(vm, 0);                         /* placeholder — patched next   */
    emit_byte(vm, OP_RET);                   /* end of creation-part         */
    write32(vm, patch, vm->here);            /* runtime code starts here     */
    NEXT;
}

l_uddivmod_ext: {
    /* ud/mod ( lo hi u -- rem lo' hi' )
     * Unsigned 64÷32 division yielding a 64-bit quotient and 32-bit remainder.
     * Needed by '#' for pictured numeric output of double-cell numbers. */
    uint32_t u  = (uint32_t)dpop(vm);
    uint32_t hi = (uint32_t)dpop(vm);
    uint32_t lo = (uint32_t)dpop(vm);
    uint64_t ud = ((uint64_t)hi << 32) | lo;
    uint64_t q  = ud / u;
    uint32_t r  = (uint32_t)(ud % u);
    dpush(vm, (int32_t)r);
    dpush(vm, (int32_t)(uint32_t)(q & 0xFFFFFFFFu));
    dpush(vm, (int32_t)(uint32_t)(q >> 32));
    NEXT;
}

/* --- create ( -- ) ------------------------------------------------------- */
l_create: {
    /* parse name from TIB */
    uint32_t in  = read32(vm, ADDR_IN);
    uint32_t len = read32(vm, ADDR_SRCLEN);
    uint8_t *buf = vm->mem + TIB_BASE;
    while (in < len && buf[in] <= ' ') in++;          /* skip whitespace */
    uint32_t start = in;
    while (in < len && buf[in] >  ' ') in++;          /* scan name */
    uint32_t nlen = in - start;
    write32(vm, ADDR_IN, in);
    if (nlen == 0 || nlen > 31) { fputs("error: create: bad name\n", stderr); NEXT; }
    /* lowercase the name */
    char name[32];
    for (uint32_t i = 0; i < nlen; i++)
        name[i] = (char)tolower((unsigned char)buf[start + i]);
    /* write dict header */
    uint32_t entry = vm->here;
    write32(vm, entry, vm->latest);
    vm->mem[entry+4] = 0;
    vm->mem[entry+5] = (uint8_t)nlen;
    memcpy(vm->mem + entry + 6, name, nlen);
    vm->here = entry + 6 + nlen;
    vm->latest = entry;
    /* body: OP_DOES_CALL [does_ptr=0]  (5 bytes)
     * Data area follows immediately at vm->here + 5 (= body + 5).
     * Calling the word pushes the data area address; DOES> can install a
     * runtime action by patching the does_ptr slot (body+1). */
    emit_byte(vm, OP_DOES_CALL);
    emit_u32(vm, 0);                   /* does_ptr = 0 (no DOES> code yet) */
    /* vm->here == data-area address now; caller uses allot/comma for storage */
    NEXT;
}

/* --- Unknown opcode -------------------------------------------------- */

l_unknown:
    fprintf(stderr, "error: unknown opcode 0x%02X at 0x%06X\n",
            (unsigned)vm->mem[vm->ip - 1], (unsigned)(vm->ip - 1));
    /* find which word contains this address */
    { uint32_t bad = vm->ip - 1;
      uint32_t e = vm->latest;
      while (e != 0) {
          if (e <= bad) {
              uint8_t nlen = vm->mem[e+5];
              uint32_t body = e + 6 + nlen;
              char name[64]; if (nlen > 63) nlen = 63;
              memcpy(name, vm->mem+e+6, nlen); name[nlen] = '\0';
              fprintf(stderr, "  in word '%s' (entry=0x%X body=0x%X offset=%d)\n",
                      name, e, body, (int)(bad - body));
              break;
          }
          uint32_t link = (uint32_t)(vm->mem[e]|(vm->mem[e+1]<<8)|(vm->mem[e+2]<<16)|(vm->mem[e+3]<<24));
          e = link;
      }
    }
    return;
}

#undef NEXT

/* =========================================================================
 * vm_eval — outer interpreter (one line of Forth source)
 *
 * For each token in the line:
 *   1. Special tokens: \ (comment), : (define), ; (end define)
 *   2. Dictionary lookup — if found:
 *        - Compiling + NOT immediate + primitive: inline the opcode byte
 *        - Compiling + NOT immediate:             emit OP_CALL + address
 *        - Otherwise (immediate or interpreting): execute now
 *   3. Number literal — push or compile OP_LIT
 *   4. Unknown: print error
 *
 * STATE (compiling flag) lives in memory at ADDR_STATE, not the struct.
 * This means Forth code can read it with `state @` when needed.
 * ========================================================================= */

static int next_token(const char **p, char *buf, int bufsz)
{
    while (**p == ' ' || **p == '\t') (*p)++;
    if (**p == '\0') return 0;

    int i = 0;
    while (**p != ' ' && **p != '\t' && **p != '\0') {
        if (i < bufsz - 1) buf[i++] = **p;
        (*p)++;
    }
    buf[i] = '\0';
    return 1;
}

int vm_eval(VM *vm, const char *line)
{
    const char *p = line;
    char token[256];

    /* Sync TIB for SOURCE, >IN, PARSE-NAME */
    {
        uint32_t _ll = (uint32_t)strlen(line);
        if (_ll > TIB_SIZE - 1) _ll = TIB_SIZE - 1;
        memcpy(vm->mem + TIB_BASE, line, _ll);
        write32(vm, ADDR_SRCLEN, _ll);
        write32(vm, ADDR_IN, 0);
    }

    for (;;) {
        /* Re-sync p — parsing words (WORD, PARSE-NAME) may have advanced ADDR_IN */
        p = line + read32(vm, ADDR_IN);
        if (!next_token(&p, token, sizeof(token))) break;
        write32(vm, ADDR_IN, (uint32_t)(p - line));
        /* Case-fold: dict names are stored lowercase; ANS files use uppercase */
        for (char *q = token; *q; q++) *q = (char)tolower((unsigned char)*q);

        /* ---- [IF] / [ELSE] / [THEN] skip state ---- */
        if (vm->if_skip > 0) {
            if      (strcmp(token, "[if]")   == 0) vm->if_skip++;
            else if (strcmp(token, "[else]") == 0 && vm->if_skip == 1 && vm->if_want_else) {
                vm->if_skip = 0; vm->if_want_else = 0;
            }
            else if (strcmp(token, "[then]") == 0) {
                if (--vm->if_skip == 0) vm->if_want_else = 0;
            }
            continue;
        }

        int compiling = (read32(vm, ADDR_STATE) != 0);

        /* Line comment — handled by the IMMEDIATE \ word in the dict.
         * Keep a fast-path break for the common case (no dict lookup needed). */
        if (strcmp(token, "\\") == 0) {
            write32(vm, ADDR_IN, read32(vm, ADDR_SRCLEN));
            break;
        }

        /* POSTPONE word — compile-time only.
         * ANS semantics:
         *   - IMMEDIATE word:     compile a direct call/inline (runs at compile time
         *                         when the enclosing word executes)
         *   - Non-immediate word: compile code that, when the enclosing word runs,
         *                         compiles a call to the target word.
         *                         For colon defs: OP_LIT body + OP_COMPILE_CALL
         *                         For primitives:  inline the opcode directly       */
        if (strcmp(token, "postpone") == 0 && compiling) {
            if (!next_token(&p, token, sizeof(token))) {
                fputs("error: expected word after postpone\n", stderr);
                return -1;
            }
            for (char *q = token; *q; q++) *q = (char)tolower((unsigned char)*q);
            write32(vm, ADDR_IN, (uint32_t)(p - line));
            uint32_t pe = dict_find(vm, token, (int)strlen(token));
            if (!pe) {
                fprintf(stderr, "error: postpone: word not found: %s\n", token);
                return -1;
            }
            uint32_t pbody  = dict_body(vm, pe);
            uint8_t  pflags = vm->mem[pe + 4];
            if (pflags & F_IMMEDIATE) {
                /* Immediate word: compile a direct call so it runs at compile time */
                if (pflags & F_PRIMITIVE) {
                    emit_byte(vm, vm->mem[pbody]);
                } else {
                    emit_byte(vm, OP_CALL);
                    emit_u32(vm, pbody);
                }
            } else {
                /* Non-immediate word: emit code that compiles a call at run time */
                if (pflags & F_PRIMITIVE) {
                    /* Inline the opcode — same as compile-time inlining */
                    emit_byte(vm, vm->mem[pbody]);
                } else {
                    emit_byte(vm, OP_LIT);
                    emit_u32(vm, pbody);
                    emit_byte(vm, OP_COMPILE_CALL);
                }
            }
            continue;
        }

        /* [IF] [ELSE] [THEN] — interpret-time conditionals */
        if (strcmp(token, "[if]") == 0) {
            int32_t cond = (int32_t)dpop(vm);
            if (!cond) { vm->if_skip = 1; vm->if_want_else = 1; }
            continue;
        }
        if (strcmp(token, "[else]") == 0) {
            /* reached [else] while executing the true branch — skip to [then] */
            vm->if_skip = 1; vm->if_want_else = 0;
            continue;
        }
        if (strcmp(token, "[then]") == 0) {
            continue;  /* no-op in executing mode */
        }

        /* Paren comment — skip everything up to and including ')' */
        if (strcmp(token, "(") == 0) {
            while (*p && *p != ')') p++;
            if (*p == ')') p++;
            write32(vm, ADDR_IN, (uint32_t)(p - line));
            continue;
        }

        /* -------------------------------------------------------------------
         * ":" — begin a new word definition
         *
         * Set F_HIDDEN so the word isn't visible during its own compilation
         * (prevents accidental recursion before ";" clears it).
         * ------------------------------------------------------------------- */
        if (strcmp(token, ":") == 0 && !compiling) {
            if (!next_token(&p, token, sizeof(token))) {
                fputs("error: expected word name after :\n", stderr);
                return -1;
            }
            for (char *q = token; *q; q++) *q = (char)tolower((unsigned char)*q);
            dict_create(vm, token, (int)strlen(token), F_HIDDEN);
            cloop_depth = 0;
            write32(vm, ADDR_STATE, 1);
            write32(vm, ADDR_IN, (uint32_t)(p - line));
            continue;
        }

        /* -------------------------------------------------------------------
         * ";" — end a word definition
         *
         * Emit OP_RET, clear F_HIDDEN on the entry, exit compile mode.
         * ------------------------------------------------------------------- */
        if (strcmp(token, ";") == 0) {
            if (!compiling) {
                fputs("error: ; without :\n", stderr);
                return -1;
            }
            emit_byte(vm, OP_RET);
            vm->mem[vm->latest + 4] &= ~F_HIDDEN;  /* reveal the word */
            write32(vm, ADDR_STATE, 0);
            continue;
        }

        /* -------------------------------------------------------------------
         * "variable" — define a named memory cell
         *
         * Creates a word whose body is: OP_LIT <data_addr> OP_RET
         * followed by 4 zero bytes for the data cell.
         * Executing the word pushes the data cell's address.
         * ------------------------------------------------------------------- */
        if (strcmp(token, "variable") == 0) {
            if (compiling) {
                fputs("error: variable inside definition\n", stderr);
                return -1;
            }
            if (!next_token(&p, token, sizeof(token))) {
                fputs("error: expected name after variable\n", stderr);
                return -1;
            }
            for (char *q = token; *q; q++) *q = (char)tolower((unsigned char)*q);
            dict_create(vm, token, (int)strlen(token), 0);
            uint32_t data_addr = vm->here + 6; /* after OP_LIT(1) + addr(4) + OP_RET(1) */
            emit_byte(vm, OP_LIT);
            emit_u32(vm, data_addr);
            emit_byte(vm, OP_RET);
            emit_u32(vm, 0);                   /* data cell, initialised to 0 */
            write32(vm, ADDR_IN, (uint32_t)(p - line));
            continue;
        }

        /* -------------------------------------------------------------------
         * "constant" — define a named compile-time constant
         *
         * Pops TOS as the value.  Creates a word whose body is:
         * OP_LIT <value> OP_RET.  Executing the word pushes the value.
         * ------------------------------------------------------------------- */
        if (strcmp(token, "constant") == 0 && !compiling) {
            if (!next_token(&p, token, sizeof(token))) {
                fputs("error: expected name after constant\n", stderr);
                return -1;
            }
            for (char *q = token; *q; q++) *q = (char)tolower((unsigned char)*q);
            int32_t val = dpop(vm);
            dict_create(vm, token, (int)strlen(token), 0);
            emit_byte(vm, OP_LIT);
            emit_u32(vm, (uint32_t)val);
            emit_byte(vm, OP_RET);
            write32(vm, ADDR_IN, (uint32_t)(p - line));
            continue;
        }

        /* -------------------------------------------------------------------
         * s" — string literal: ( "ccc" -- c-addr u )
         *
         * Reads raw characters up to the closing '"'.
         * Interpret: stores bytes at HERE (non-destructive), pushes addr+len.
         * Compile:   emits JMP-over-string inline, then LIT addr, LIT len.
         * ------------------------------------------------------------------- */
        if (strcmp(token, "s\"") == 0) {
            if (*p == ' ') p++;                 /* skip the mandatory space */
            char str[256];
            int  len = 0;
            while (*p && *p != '"' && len < (int)sizeof(str) - 1)
                str[len++] = *p++;
            if (*p == '"') p++;

            if (!compiling) {
                uint32_t addr = vm->here;       /* store at HERE non-destructively */
                for (int i = 0; i < len; i++)
                    vm->mem[addr + i] = (uint8_t)str[i];
                dpush(vm, (int32_t)addr);
                dpush(vm, len);
            } else {
                emit_byte(vm, OP_JMP);          /* jump over the string data */
                uint32_t patch    = vm->here;
                emit_u32(vm, 0);
                uint32_t str_addr = vm->here;
                for (int i = 0; i < len; i++)
                    emit_byte(vm, (uint8_t)str[i]);
                write32(vm, patch, vm->here);   /* patch JMP target */
                emit_byte(vm, OP_LIT);
                emit_u32(vm, str_addr);
                emit_byte(vm, OP_LIT);
                emit_u32(vm, (uint32_t)len);
            }
            write32(vm, ADDR_IN, (uint32_t)(p - line));
            continue;
        }

        /* -------------------------------------------------------------------
         * ." — print string literal
         *
         * Interpret: reads and prints characters directly.
         * Compile:   same as s" but also emits OP_TYPE.
         * ------------------------------------------------------------------- */
        if (strcmp(token, ".\"") == 0) {
            if (*p == ' ') p++;
            char str[256];
            int  len = 0;
            while (*p && *p != '"' && len < (int)sizeof(str) - 1)
                str[len++] = *p++;
            if (*p == '"') p++;

            if (!compiling) {
                char cbuf[2] = { 0, 0 };
                for (int i = 0; i < len; i++) {
                    cbuf[0] = str[i];
                    gfx_print(cbuf);
                }
            } else {
                emit_byte(vm, OP_JMP);
                uint32_t patch    = vm->here;
                emit_u32(vm, 0);
                uint32_t str_addr = vm->here;
                for (int i = 0; i < len; i++)
                    emit_byte(vm, (uint8_t)str[i]);
                write32(vm, patch, vm->here);
                emit_byte(vm, OP_LIT);
                emit_u32(vm, str_addr);
                emit_byte(vm, OP_LIT);
                emit_u32(vm, (uint32_t)len);
                emit_byte(vm, OP_TYPE);
            }
            write32(vm, ADDR_IN, (uint32_t)(p - line));
            continue;
        }

        /* -------------------------------------------------------------------
         * .( text) — IMMEDIATE: print text up to closing ')'
         * Always prints immediately (at parse time), even inside definitions.
         * ------------------------------------------------------------------- */
        if (strcmp(token, ".(") == 0) {
            if (*p == ' ') p++;
            { char cbuf[2] = { 0, 0 }; while (*p && *p != ')') { cbuf[0] = *p++; gfx_print(cbuf); } }
            if (*p == ')') p++;
            write32(vm, ADDR_IN, (uint32_t)(p - line));
            continue;
        }

        /* -------------------------------------------------------------------
         * [char] — compile-time: push ASCII value of next token's first char
         * ------------------------------------------------------------------- */
        if (strcmp(token, "[char]") == 0) {
            if (!next_token(&p, token, sizeof(token))) {
                fputs("error: expected char after [char]\n", stderr);
                return -1;
            }
            write32(vm, ADDR_IN, (uint32_t)(p - line));
            emit_byte(vm, OP_LIT);
            emit_u32(vm, (uint32_t)(uint8_t)token[0]);
            continue;
        }

        /* -------------------------------------------------------------------
         * ['] — compile-time: push xt of named word as a literal
         * ------------------------------------------------------------------- */
        if (strcmp(token, "[']") == 0) {
            if (!next_token(&p, token, sizeof(token))) {
                fputs("error: expected word after [']\n", stderr);
                return -1;
            }
            for (char *q = token; *q; q++) *q = (char)tolower((unsigned char)*q);
            write32(vm, ADDR_IN, (uint32_t)(p - line));
            uint32_t xt_entry = dict_find(vm, token, (int)strlen(token));
            if (!xt_entry) {
                fprintf(stderr, "error: ['] word not found: %s\n", token);
                return -1;
            }
            uint32_t xt = xt_entry + 6 + vm->mem[xt_entry + 5];
            emit_byte(vm, OP_LIT);
            emit_u32(vm, xt);
            continue;
        }

        /* -------------------------------------------------------------------
         * abort" — IMMEDIATE parsing: ( flag "msg" -- )
         *   If TOS is non-zero, print msg and call abort.
         *   Emit: JZ <over>, JMP <code>, <string data>, LIT addr, LIT len,
         *         CALL type, CALL abort, patch JZ target.
         * ------------------------------------------------------------------- */
        if (strcmp(token, "abort\"") == 0) {
            if (*p == ' ') p++;             /* skip mandatory space */
            const char *start = p;
            while (*p && *p != '"') p++;
            int slen = (int)(p - start);
            if (*p == '"') p++;
            write32(vm, ADDR_IN, (uint32_t)(p - line));

            emit_byte(vm, OP_JZ);
            uint32_t jz_patch  = vm->here;  emit_u32(vm, 0);
            emit_byte(vm, OP_JMP);
            uint32_t jmp_patch = vm->here;  emit_u32(vm, 0);
            uint32_t str_addr  = vm->here;
            memcpy(vm->mem + vm->here, start, (size_t)slen);
            vm->here += (uint32_t)slen;
            write32(vm, jmp_patch, vm->here);
            emit_byte(vm, OP_LIT); emit_u32(vm, str_addr);
            emit_byte(vm, OP_LIT); emit_u32(vm, (uint32_t)slen);
            /* CALL type */
            uint32_t type_entry = dict_find(vm, "type", 4);
            if (type_entry) {
                uint32_t type_body = type_entry + 6 + vm->mem[type_entry + 5];
                emit_byte(vm, OP_CALL); emit_u32(vm, type_body);
            }
            /* CALL abort */
            uint32_t abort_entry = dict_find(vm, "abort", 5);
            if (abort_entry) {
                uint32_t abort_body = abort_entry + 6 + vm->mem[abort_entry + 5];
                emit_byte(vm, OP_CALL); emit_u32(vm, abort_body);
            }
            write32(vm, jz_patch, vm->here);
            continue;
        }

        /* -------------------------------------------------------------------
         * recurse — compile-time: call the word currently being defined
         * ------------------------------------------------------------------- */
        if (strcmp(token, "recurse") == 0 && compiling) {
            uint32_t latest_namelen = vm->mem[vm->latest + 5];
            uint32_t body = vm->latest + 6 + latest_namelen;
            emit_byte(vm, OP_CALL);
            emit_u32(vm, body);
            continue;
        }

        /* -------------------------------------------------------------------
         * Dictionary lookup
         * ------------------------------------------------------------------- */
        uint32_t entry = dict_find(vm, token, (int)strlen(token));
        if (entry != 0) {
            uint8_t  flags = vm->mem[entry + 4];
            uint32_t body  = dict_body(vm, entry);

            if (compiling && !(flags & F_IMMEDIATE)) {
                if (flags & F_PRIMITIVE) {
                    /* Inline the opcode directly — avoids a CALL/RET pair.
                     * This is critical for >R and R> where CALL would push
                     * an extra return address and corrupt the return stack. */
                    emit_byte(vm, vm->mem[body]);
                } else {
                    emit_byte(vm, OP_CALL);
                    emit_u32(vm, body);
                }
            } else {
                /* Execute immediately.
                 * Sync ADDR_IN so parsing primitives (e.g. CONSTANT called
                 * from a colon definition) see the correct input position. */
                write32(vm, ADDR_IN, (uint32_t)(p - line));
                vm->ip  = body;
                vm->rsp = 0;
                vm_run(vm);
                /* Re-sync p from ADDR_IN in case the word advanced it
                 * (e.g. CONSTANT consumed the name token from TIB). */
                p = line + read32(vm, ADDR_IN);
            }
            continue;
        }

        /* -------------------------------------------------------------------
         * Number literal — respects current BASE; 0x prefix always forces hex
         * ------------------------------------------------------------------- */
        char *endp;
        long n;
        if (token[0] == '0' && (token[1] == 'x' || token[1] == 'X')) {
            n = strtol(token, &endp, 16);
        } else {
            int nbase = (int)read32(vm, ADDR_BASE);
            if (nbase < 2 || nbase > 36) nbase = 10;
            n = strtol(token, &endp, nbase);
        }
        if (*endp == '\0') {
            if (compiling) {
                emit_byte(vm, OP_LIT);
                emit_u32(vm, (uint32_t)(int32_t)n);
            } else {
                dpush(vm, (int32_t)n);
            }
            continue;
        }

        /* -------------------------------------------------------------------
         * Unknown token
         * ------------------------------------------------------------------- */
        fprintf(stderr, "error: unknown word '%s'\n", token);
        return -1;
    }

    return 0;
}

/* =========================================================================
 * vm_load — evaluate a file line by line
 * ========================================================================= */

int vm_load(VM *vm, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    char line[1024];
    int  lineno = 0;
    int  result = 0;

    while (fgets(line, sizeof(line), f)) {
        lineno++;
        line[strcspn(line, "\r\n")] = '\0';
        write32(vm, ADDR_SRCBASE, TIB_BASE);   /* SOURCE returns TIB for file input */
        if (vm_eval(vm, line) != 0) {
            fprintf(stderr, "%s:%d: error\n", path, lineno);
            result = -1;
            break;
        }
    }

    fclose(f);
    return result;
}

/* =========================================================================
 * vm_repl — interactive REPL
 * ========================================================================= */

void vm_repl(VM *vm)
{
    char line[1024];
    int  interactive = isatty(fileno(stdin));

    for (;;) {
        vm->quit_active = 1;
        if (setjmp(vm->quit_jmp) != 0) {
            vm->dsp = -1;
            vm->rsp = 0;
            write32(vm, ADDR_STATE, 0);
            write32(vm, ADDR_SRCID, 0);
            fputs("  (aborted)\n", stderr);
        }

        fputs(read32(vm, ADDR_STATE) ? "... " : "> ", stdout);
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) {
            putchar('\n');
            break;
        }

        line[strcspn(line, "\r\n")] = '\0';

        if (!interactive)
            printf("%s\n", line);

        if (strcmp(line, "bye") == 0) break;

        write32(vm, ADDR_SRCBASE, TIB_BASE);   /* SOURCE returns TIB for keyboard input */
        if (vm_eval(vm, line) == 0)
            puts("ok");
    }
}
