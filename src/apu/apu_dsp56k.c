/*
 * apu_dsp56k.c -- a small DSP56300 interpreter (apu_dsp56k.h).
 *
 * Written from Motorola's DSP56300 Family Manual (instruction encodings
 * and their arithmetic) and from the effects programs of a title's GP
 * image, which say how the GP's DMA engine is driven. No emulator's code.
 */
#include "apu_dsp56k.h"

#include <string.h>

/* ── arithmetic ─────────────────────────────────────────────────────── */

static inline int64_t w56(int64_t a)
{
    return (int64_t)((uint64_t)a << 8) >> 8;
}

static inline int32_t sx24(uint32_t v)
{
    return (int32_t)(v << 8) >> 8;
}

static inline int32_t lim24(int64_t a)
{
    int64_t hi = a >> 47;
    if (hi > 0) return 0x7FFFFF;
    if (hi < -1) return -0x800000;
    return (int32_t)(a >> 24);
}

static inline int64_t ld(int32_t v)             /* a 24-bit value into A1 */
{
    return (int64_t)v * 16777216;
}

/* Convergent rounding of the accumulator at the A1 / A0 boundary. */
static inline int64_t rnd56(int64_t a)
{
    a = w56(a + 0x800000);
    if (((uint64_t)a & 0xFFFFFF) == 0)
        a &= ~(int64_t)0x1000000;
    return (int64_t)((uint64_t)a & ~(uint64_t)0xFFFFFF);
}

/* ── registers by their codes ───────────────────────────────────────── */

/* The six-bit register code (DDDDDD); the five-bit one of the parallel
 * moves is its low 32 entries. */
enum { RG_X0 = 4, RG_X1, RG_Y0, RG_Y1, RG_A0, RG_B0, RG_A2, RG_B2, RG_A1, RG_B1, RG_A, RG_B };

static int reg_get(dsp56k *d, unsigned c, int32_t *v)
{
    switch (c) {
    case RG_X0: *v = d->x0; return 1;
    case RG_X1: *v = d->x1; return 1;
    case RG_Y0: *v = d->y0; return 1;
    case RG_Y1: *v = d->y1; return 1;
    case RG_A0: *v = sx24((uint32_t)d->a & 0xFFFFFF); return 1;
    case RG_B0: *v = sx24((uint32_t)d->b & 0xFFFFFF); return 1;
    case RG_A2: *v = (int32_t)(d->a >> 48); return 1;
    case RG_B2: *v = (int32_t)(d->b >> 48); return 1;
    case RG_A1: *v = sx24((uint32_t)(d->a >> 24) & 0xFFFFFF); return 1;
    case RG_B1: *v = sx24((uint32_t)(d->b >> 24) & 0xFFFFFF); return 1;
    case RG_A:  *v = lim24(d->a); return 1;
    case RG_B:  *v = lim24(d->b); return 1;
    default:
        if (c >= 16 && c < 24) { *v = sx24(d->r[c - 16]); return 1; }
        if (c >= 24 && c < 32) { *v = sx24(d->n[c - 24]); return 1; }
        if (c >= 32 && c < 40) { *v = sx24(d->m[c - 32]); return 1; }
        return 0;
    }
}

static int64_t set_part(int64_t acc, int shift, int bits, int32_t v)
{
    uint64_t mask = (((uint64_t)1 << bits) - 1) << shift;
    return w56((int64_t)(((uint64_t)acc & ~mask) | (((uint64_t)(uint32_t)v << shift) & mask)));
}

static int reg_set(dsp56k *d, unsigned c, int32_t v)
{
    v = sx24((uint32_t)v);
    switch (c) {
    case RG_X0: d->x0 = v; return 1;
    case RG_X1: d->x1 = v; return 1;
    case RG_Y0: d->y0 = v; return 1;
    case RG_Y1: d->y1 = v; return 1;
    case RG_A0: d->a = set_part(d->a, 0, 24, v); return 1;
    case RG_B0: d->b = set_part(d->b, 0, 24, v); return 1;
    case RG_A2: d->a = set_part(d->a, 48, 8, v); return 1;
    case RG_B2: d->b = set_part(d->b, 48, 8, v); return 1;
    case RG_A1: d->a = set_part(d->a, 24, 24, v); return 1;
    case RG_B1: d->b = set_part(d->b, 24, 24, v); return 1;
    case RG_A:  d->a = ld(v); return 1;
    case RG_B:  d->b = ld(v); return 1;
    default:
        if (c >= 16 && c < 24) { d->r[c - 16] = (uint32_t)v & 0xFFFFFF; return 1; }
        if (c >= 24 && c < 32) { d->n[c - 24] = (uint32_t)v & 0xFFFFFF; return 1; }
        if (c >= 32 && c < 40) { d->m[c - 32] = (uint32_t)v & 0xFFFFFF; return 1; }
        return 0;
    }
}

/* ── memory ─────────────────────────────────────────────────────────── */

#define PERIPH_DMA_NEXT 0xFFFFD4u       /* the DMA list's first block */
#define PERIPH_DMA_CTL  0xFFFFD6u       /* 1 / 2: freeze on / off; 3 / 4: stop / start */
#define PERIPH_IRQ      0xFFFFC5u       /* bit 7: the list ran to its end; written to clear */

static int xrd(dsp56k *d, uint32_t a, int32_t *v)
{
    a &= 0xFFFFFF;
    if (a < DSP56K_X_WORDS) { *v = d->x[a]; return 1; }
    if (a == PERIPH_DMA_CTL) { *v = (int32_t)d->dma_ctl; return 1; }
    if (a == PERIPH_IRQ) { *v = (int32_t)d->irq; return 1; }
    if (a == PERIPH_DMA_NEXT) { *v = (int32_t)d->dma_next; return 1; }
    return 0;
}

/* The DMA list: each block moves its words between X memory and its
 * circular buffer in GP scratch, and its position moves on by what it
 * moved. */
static int dma_run(dsp56k *d)
{
    uint32_t blk = d->dma_next & 0x3FFF;
    for (int guard = 0; guard < 256; guard++) {
        uint32_t next, ctl, count, addr, pos, base, last;
        if (blk + 7 > DSP56K_X_WORDS)
            return 0;
        next = (uint32_t)d->x[blk] & 0xFFFFFF;
        ctl = (uint32_t)d->x[blk + 1] & 0xFFFFFF;
        count = (uint32_t)d->x[blk + 2] & 0xFFFFFF;
        addr = (uint32_t)d->x[blk + 3] & 0xFFFFFF;
        pos = (uint32_t)d->x[blk + 4] & 0xFFFFFF;
        base = (uint32_t)d->x[blk + 5] & 0xFFFFFF;
        last = (uint32_t)d->x[blk + 6] & 0xFFFFFF;
        if (count > 0x400 || addr + count > DSP56K_X_WORDS)
            return 0;
        for (uint32_t k = 0; k < count; k++) {
            if (ctl & 2) {
                uint32_t w = (uint32_t)d->x[addr + k] & 0xFFFFFF;
                if (d->mem_write) d->mem_write(d->ctx, base + pos, &w, 1);
            } else {
                d->x[addr + k] = sx24(d->mem_read ? d->mem_read(d->ctx, base + pos) : 0);
            }
            pos += 4;
            if (pos > last)
                pos = 0;
        }
        d->x[blk + 4] = (int32_t)pos;
        if (next & 0x4000)
            break;
        blk = next & 0x3FFF;
    }
    d->irq |= 0x80;
    return 1;
}

static int xwr(dsp56k *d, uint32_t a, int32_t v)
{
    a &= 0xFFFFFF;
    if (a < DSP56K_X_WORDS) { d->x[a] = sx24((uint32_t)v); return 1; }
    if (a == PERIPH_DMA_NEXT) { d->dma_next = (uint32_t)v & 0xFFFFFF; return 1; }
    if (a == PERIPH_IRQ) { d->irq &= ~((uint32_t)v & 0xFFFFFF); return 1; }
    if (a == PERIPH_DMA_CTL) {
        switch ((uint32_t)v & 0xFFFFFF) {
        case 1: d->dma_ctl |= 0x10; return 1;
        case 2: d->dma_ctl &= ~0x10u; return 1;
        case 3: d->dma_ctl |= 0x08; return 1;
        case 4: d->dma_ctl &= ~0x08u; return dma_run(d);
        default: return 0;
        }
    }
    return 0;
}

/* The effective address of MMMRRR, with its update; modes 0..5 and 7.
 * Linear addressing only. */
static int ea_addr(dsp56k *d, unsigned mmmrrr, uint32_t *a)
{
    unsigned m = mmmrrr >> 3, r = mmmrrr & 7;
    switch (m) {
    case 0: *a = d->r[r]; d->r[r] = (d->r[r] - d->n[r]) & 0xFFFFFF; return 1;
    case 1: *a = d->r[r]; d->r[r] = (d->r[r] + d->n[r]) & 0xFFFFFF; return 1;
    case 2: *a = d->r[r]; d->r[r] = (d->r[r] - 1) & 0xFFFFFF; return 1;
    case 3: *a = d->r[r]; d->r[r] = (d->r[r] + 1) & 0xFFFFFF; return 1;
    case 4: *a = d->r[r]; return 1;
    case 5: *a = (d->r[r] + d->n[r]) & 0xFFFFFF; return 1;
    case 7: d->r[r] = (d->r[r] - 1) & 0xFFFFFF; *a = d->r[r]; return 1;
    default: return 0;
    }
}

/* ── the data ALU ───────────────────────────────────────────────────── */

static void mul_ops(dsp56k *d, unsigned qqq, int32_t *s1, int32_t *s2)
{
    switch (qqq) {
    case 0: *s1 = d->x0; *s2 = d->x0; break;
    case 1: *s1 = d->y0; *s2 = d->y0; break;
    case 2: *s1 = d->x1; *s2 = d->x0; break;
    case 3: *s1 = d->y1; *s2 = d->y0; break;
    case 4: *s1 = d->x0; *s2 = d->y1; break;
    case 5: *s1 = d->y0; *s2 = d->x0; break;
    case 6: *s1 = d->x1; *s2 = d->y0; break;
    default: *s1 = d->y1; *s2 = d->x1; break;
    }
}

static void cmp56(dsp56k *d, int64_t dst, int64_t src)
{
    /* Both fit 56 bits, so the difference fits 64: no overflow to track. */
    int64_t r = dst - src;
    d->lt = r < 0;
    d->eq = r == 0;
}

static int cond(const dsp56k *d, unsigned cc)
{
    switch (cc) {
    case 0x1: return !d->lt;            /* GE */
    case 0x2: return !d->eq;            /* NE */
    case 0x7: return !d->lt && !d->eq;  /* GT */
    case 0x9: return d->lt;             /* LT */
    case 0xA: return d->eq;             /* EQ */
    case 0xF: return d->lt || d->eq;    /* LE */
    default: return -1;
    }
}

/* The opcode byte of a parallel-move instruction. */
static int alu(dsp56k *d, unsigned op)
{
    int64_t *dst = (op & 8) ? &d->b : &d->a;
    int64_t oth = (op & 8) ? d->a : d->b;

    if (op & 0x80) {
        int32_t s1, s2;
        int64_t p;
        mul_ops(d, (op >> 4) & 7, &s1, &s2);
        p = (int64_t)s1 * s2 * 2;
        if (op & 4) p = -p;
        if (op & 2) p += *dst;          /* MAC / MACR */
        p = w56(p);
        if (op & 1) p = rnd56(p);       /* MPYR / MACR */
        *dst = p;
        return 1;
    }
    switch (op & 0xF7) {
    case 0x00: return 1;                                /* MOVE */
    case 0x01: *dst = oth; return 1;                    /* TFR */
    case 0x03: cmp56(d, *dst, 0); return 1;             /* TST */
    case 0x05: cmp56(d, *dst, oth); return 1;           /* CMP */
    case 0x10: *dst = w56(*dst + oth); return 1;        /* ADD */
    case 0x11: *dst = rnd56(*dst); return 1;            /* RND */
    case 0x13: *dst = 0; return 1;                      /* CLR */
    case 0x14: *dst = w56(*dst - oth); return 1;        /* SUB */
    case 0x22: *dst = *dst >> 1; return 1;              /* ASR */
    case 0x26: if (*dst < 0) *dst = w56(-*dst); return 1; /* ABS */
    case 0x32: *dst = w56(*dst * 2); return 1;          /* ASL */
    case 0x36: *dst = w56(-*dst); return 1;             /* NEG */
    default: break;
    }
    if ((op & 0x40)) {
        int32_t s;
        switch ((op >> 4) & 7) {
        case 4: s = d->x0; break;
        case 5: s = d->y0; break;
        case 6: s = d->x1; break;
        default: s = d->y1; break;
        }
        switch (op & 7) {
        case 0: *dst = w56(*dst + ld(s)); return 1;     /* ADD */
        case 1: *dst = ld(s); return 1;                 /* TFR */
        case 4: *dst = w56(*dst - ld(s)); return 1;     /* SUB */
        case 5: cmp56(d, *dst, ld(s)); return 1;        /* CMP */
        default: return 0;
        }
    }
    return 0;
}

/* The move field (bits 23..8) of a parallel-move instruction; the ALU
 * operation has run. Sources are read before the operation on hardware;
 * the programs run here never move a register the same instruction's
 * operation changes except as its documented result, which is handled by
 * taking the source values first (pre). */
struct pre { int have; int32_t v; };

static int pmove_pre(dsp56k *d, uint32_t w, struct pre *p)
{
    unsigned hi = w >> 8;
    p->have = 0;
    if ((hi & 0xFC00) == 0x2000 && ((hi >> 5) & 0x1F) >= 4) {      /* register to register */
        p->have = reg_get(d, (hi >> 5) & 0x1F, &p->v);
        return p->have;
    }
    if ((hi & 0xC000) == 0x4000 && !(hi & 0x0800) && (hi & 0x40) && !(hi & 0x80)) {
        /* register to X:ea */
        p->have = reg_get(d, ((hi >> 9) & 0x18) | ((hi >> 8) & 7), &p->v);
        return p->have;
    }
    return 1;
}

static int pmove_post(dsp56k *d, uint32_t w, uint32_t ext, const struct pre *p, int *len, int *skip)
{
    unsigned hi = w >> 8;
    uint32_t a;
    int32_t v;

    if (hi == 0x2000)
        return 1;
    if ((hi & 0xFFE0) == 0x2040)                                /* address update */
        return ea_addr(d, hi & 0x1F, &a);
    if ((hi & 0xFFF0) == 0x2020) {                              /* IFcc: the operation is conditional */
        (void)skip;
        return 1;
    }
    if ((hi & 0xFC00) == 0x2000 && ((hi >> 5) & 0x1F) >= 4)
        return p->have && reg_set(d, hi & 0x1F, p->v);
    if ((hi & 0xE000) == 0x2000) {                              /* #xx to a register */
        unsigned r = (hi >> 8) & 0x1F;
        int32_t imm = (int32_t)(hi & 0xFF);
        if ((r >= RG_X0 && r <= RG_Y1) || r == RG_A || r == RG_B)
            imm = (int32_t)((uint32_t)imm << 16);               /* a fraction, left justified */
        return reg_set(d, r, imm);
    }
    if ((hi & 0xC000) == 0x4000 && !(hi & 0x0800)) {            /* X: with a register */
        unsigned r = ((hi >> 9) & 0x18) | ((hi >> 8) & 7);
        if ((hi & 0xF400) == 0x4000)
            return 0;                                           /* L: moves */
        if (!(hi & 0x40))
            return 0;                                           /* X:aa */
        if ((hi & 0x3F) == 0x34) {                              /* #xxxxxx */
            *len = 2;
            return (hi & 0x80) && reg_set(d, r, (int32_t)ext);
        }
        if ((hi & 0x3F) == 0x30) {                              /* absolute */
            *len = 2;
            a = ext;
        } else if (!ea_addr(d, hi & 0x3F, &a)) {
            return 0;
        }
        if (hi & 0x80)
            return xrd(d, a, &v) && reg_set(d, r, v);
        return p->have && xwr(d, a, p->v);
    }
    return 0;
}

/* ── the run ────────────────────────────────────────────────────────── */

#define STACK 16

int dsp56k_run(dsp56k *d, uint32_t pc, uint32_t end, unsigned long max_steps)
{
    struct { uint32_t la, start, lc; } loop[STACK];
    uint32_t ret[STACK];
    int nloop = 0, nret = 0;
    unsigned long steps = 0;

#define FAULT() do { d->fault_pc = pc; d->fault_word = w; d->steps += steps; return 1; } while (0)

    while (pc != end) {
        uint32_t w, ext, b2, b1, b0, next;
        int len = 1;
        int32_t v;

        if (pc >= d->pn || ++steps > max_steps) { w = 0xFFFFFFFF; FAULT(); }
        w = d->p[pc] & 0xFFFFFF;
        ext = pc + 1 < d->pn ? d->p[pc + 1] & 0xFFFFFF : 0;
        b2 = w >> 16; b1 = (w >> 8) & 0xFF; b0 = w & 0xFF;
        next = 0xFFFFFFFF;

        if (w >> 20) {
            /* A data ALU operation with a parallel move. */
            struct pre p;
            int skip = 0;
            if ((w >> 8) >= 0x8000 || (w >> 20) == 1)
                FAULT();                                        /* X:Y: and X:R class I moves */
            if (((w >> 8) & 0xFFF0) == 0x2020) {
                int c = cond(d, (w >> 8) & 15);
                if (c < 0) FAULT();
                skip = !c;
            }
            if (!pmove_pre(d, w, &p)) FAULT();
            if (!skip && !alu(d, b0)) FAULT();
            if (!pmove_post(d, w, ext, &p, &len, &skip)) FAULT();
        } else if (w == 0x000000) {
            /* NOP */
        } else if (w == 0x00000C) {                             /* RTS */
            if (!nret) FAULT();
            next = ret[--nret];
        } else if ((w & 0xFFFFC0) == 0x0D1080 || (w & 0xFFFFC0) == 0x0D10C0) {
            len = 2;                                            /* BSR / BRA long */
            if (!(w & 0x40)) {
                if (nret == STACK) FAULT();
                ret[nret++] = pc + 2;
            }
            next = (pc + ext) & 0xFFFFFF;
        } else if ((w & 0xFFFFF0) == 0x0D1040) {                /* Bcc long */
            int c = cond(d, w & 15);
            if (c < 0) FAULT();
            len = 2;
            if (c) next = (pc + ext) & 0xFFFFFF;
        } else if (b2 == 0x05 && (b1 & 0xFC) >= 0x08 && (b1 & 0xF0) == 0x00 && !(b0 & 0x20)) {
            /* BSR (0000 10aa aa0a aaaa) and BRA (0000 11aa aa0a aaaa), short */
            uint32_t disp = ((w >> 1) & 0x1E0) | (w & 0x1F);
            int32_t s = (int32_t)(disp << 23) >> 23;
            if (!(b1 & 0x04)) {
                if (nret == STACK) FAULT();
                ret[nret++] = pc + 1;
            }
            next = (uint32_t)((int32_t)pc + s) & 0xFFFFFF;
        } else if (b2 == 0x06 && ((b0 & 0xF0) == 0x90 || ((b1 & 0xC0) == 0xC0 && b0 == 0x10))) {
            /* DOR #xxx / DOR S: the loop ends with the instruction at pc + ext */
            uint32_t lc;
            len = 2;
            if ((b0 & 0xF0) == 0x90) {
                lc = b1 | ((b0 & 15) << 8);
            } else {
                if (!reg_get(d, b1 & 0x3F, &v)) FAULT();
                lc = (uint32_t)v & 0xFFFFFF;
            }
            if (nloop == STACK) FAULT();
            if (lc == 0) {
                next = (pc + ext + 1) & 0xFFFFFF;               /* a count of zero skips the loop */
            } else {
                loop[nloop].la = (pc + ext) & 0xFFFFFF;
                loop[nloop].start = pc + 2;
                loop[nloop].lc = lc;
                nloop++;
            }
        } else if (b2 == 0x0C && (b1 & 0xC0) == 0xC0 && (b0 & 0xC0) == 0x80) {
            /* BRCLR / BRSET #n,register */
            len = 2;
            if (!reg_get(d, b1 & 0x3F, &v)) FAULT();
            if ((((uint32_t)v >> (b0 & 0x1F)) & 1) == ((b0 >> 5) & 1))
                next = (pc + ext) & 0xFFFFFF;
        } else if (b2 == 0x0C && (b1 & 0xC0) == 0xC0 && (b0 & 0x80) == 0) {
            /* BRCLR / BRSET #n,X:<<pp: the programs wait on these */
            len = 2;
            if (b0 & 0x40) FAULT();
            if (!xrd(d, 0xFFFFC0 | (b1 & 0x3F), &v)) FAULT();
            if ((((uint32_t)v >> (b0 & 0x1F)) & 1) == ((b0 >> 5) & 1))
                FAULT();                                        /* it would wait for ever here */
        } else if (b2 == 0x0A && (b1 & 0xC0) == 0xC0 && (b0 & 0xC0) == 0x40) {
            /* BCLR / BSET #n,register */
            if (!reg_get(d, b1 & 0x3F, &v)) FAULT();
            if (b0 & 0x20) v |= (int32_t)(1u << (b0 & 0x1F));
            else v &= ~(int32_t)(1u << (b0 & 0x1F));
            if (!reg_set(d, b1 & 0x3F, v)) FAULT();
        } else if (b2 == 0x0A && (b1 & 0xF8) == 0x70 && (b0 & 0x80)) {
            /* MOVE X:(Rn + xxxx) with a register */
            uint32_t a = (d->r[b1 & 7] + ext) & 0xFFFFFF;
            len = 2;
            if (b0 & 0x40) {
                if (!xrd(d, a, &v) || !reg_set(d, b0 & 0x3F, v)) FAULT();
            } else {
                if (!reg_get(d, b0 & 0x3F, &v) || !xwr(d, a, v)) FAULT();
            }
        } else if ((w & 0xFE00A0) == 0x020080) {
            /* MOVE X:(Rn + xxx) with a register, seven-bit displacement */
            uint32_t disp = (((w >> 11) & 0x3F) << 1) | ((w >> 6) & 1);
            uint32_t a = (uint32_t)((int32_t)d->r[(w >> 8) & 7] + ((int32_t)(disp << 25) >> 25)) & 0xFFFFFF;
            if (w & 0x10) {
                if (!xrd(d, a, &v) || !reg_set(d, w & 15, v)) FAULT();
            } else {
                if (!reg_get(d, w & 15, &v) || !xwr(d, a, v)) FAULT();
            }
        } else if ((w & 0xFFC0F0) == 0x014080 || (w & 0xFFFFF0) == 0x0140C0) {
            /* ADD / SUB / CMP / AND / OR #, on A1 */
            int64_t *dst = (w & 8) ? &d->b : &d->a;
            int64_t imm;
            if ((w & 0xFFFFF0) == 0x0140C0) { len = 2; imm = ld(sx24(ext)); }
            else imm = ld((int32_t)((w >> 8) & 0x3F));
            switch (w & 7) {
            case 0: *dst = w56(*dst + imm); break;
            case 4: *dst = w56(*dst - imm); break;
            case 5: cmp56(d, *dst, imm); break;
            case 6: *dst = (int64_t)((uint64_t)*dst & ((uint64_t)imm | ~(uint64_t)0xFFFFFF000000ull)); break;
            case 2: *dst = (int64_t)((uint64_t)*dst | ((uint64_t)imm & 0xFFFFFF000000ull)); break;
            default: FAULT();
            }
        } else if ((w & 0xFFFF00) == 0x0C1D00 || (w & 0xFFFF00) == 0x0C1C00) {
            /* ASL / ASR #n,S,D */
            int64_t s = (w & 0x80) ? d->b : d->a;
            unsigned n = (w >> 1) & 0x3F;
            s = (w & 0x0100) ? w56((int64_t)((uint64_t)s << n)) : s >> n;
            if (w & 1) d->b = s; else d->a = s;
        } else if ((w & 0xFFFFC0) == 0x0C1E40) {
            /* ASL S1,S2,D: the count from a register */
            static const uint8_t rc[8] = { 0, 0, RG_A1, RG_B1, RG_X0, RG_Y0, RG_X1, RG_Y1 };
            int64_t s = (w & 0x10) ? d->b : d->a;
            unsigned sss = (w >> 1) & 7;
            if (sss < 2 || !reg_get(d, rc[sss], &v)) FAULT();
            v &= 0x3F;
            s = v >= 56 ? 0 : w56((int64_t)((uint64_t)s << v));
            if (w & 1) d->b = s; else d->a = s;
        } else if ((w & 0xFFFFC0) == 0x0141C0) {
            /* MPYI / MPYRI / MACI / MACRI #xxxxxx,S,D */
            static const uint8_t rc[4] = { RG_X0, RG_Y0, RG_X1, RG_Y1 };
            int64_t *dst = (w & 8) ? &d->b : &d->a, p;
            len = 2;
            if (!reg_get(d, rc[(w >> 4) & 3], &v)) FAULT();
            p = (int64_t)sx24(ext) * v * 2;
            if (w & 4) p = -p;
            if (w & 2) p += *dst;
            p = w56(p);
            if (w & 1) p = rnd56(p);
            *dst = p;
        } else if (b2 == 0x05 && b1 == 0xF4 && (b0 & 0xE0) == 0x20) {
            /* MOVEC #xxxxxx,register */
            len = 2;
            if (!reg_set(d, 0x20 | (b0 & 0x1F), (int32_t)ext)) FAULT();
        } else if (b2 == 0x04 && (b1 & 0x40) && (b0 & 0xE0) == 0xA0) {
            /* MOVEC between a control register and a register */
            if (b1 & 0x80) {
                if (!reg_get(d, b1 & 0x3F, &v) || !reg_set(d, 0x20 | (b0 & 0x1F), v)) FAULT();
            } else {
                if (!reg_get(d, 0x20 | (b0 & 0x1F), &v) || !reg_set(d, b1 & 0x3F, v)) FAULT();
            }
        } else if ((w & 0xFFC000) == 0x040000) {
            /* LUA (Rn + aa),D */
            uint32_t disp = (((w >> 11) & 7) << 4) | ((w >> 4) & 0xF);
            uint32_t a = (uint32_t)((int32_t)d->r[(w >> 8) & 7] + ((int32_t)(disp << 25) >> 25)) & 0xFFFFFF;
            if (w & 8) d->n[w & 7] = a; else d->r[w & 7] = a;
        } else if (b2 == 0x08 && (b1 & 0x40) && (b0 & 0x80)) {
            /* MOVEP to X:<<pp from #xxxxxx */
            if ((b1 & 0x3F) != 0x34 || (b1 & 0x80) == 0 || (b0 & 0x40)) FAULT();
            len = 2;
            if (!xwr(d, 0xFFFFC0 | (b0 & 0x3F), (int32_t)ext)) FAULT();
        } else if (b2 == 0x08 && (b1 & 0xC0) == 0xC0 && (b0 & 0xC0) == 0x00) {
            /* MOVEP register to X:<<pp */
            if (!reg_get(d, b1 & 0x3F, &v) || !xwr(d, 0xFFFFC0 | (b0 & 0x3F), v)) FAULT();
        } else {
            FAULT();
        }

        if (next == 0xFFFFFFFF)
            next = pc + (uint32_t)len;
        /* The end of a loop: the instruction that holds its last address. */
        while (nloop && next == pc + (uint32_t)len && pc <= loop[nloop - 1].la &&
               pc + (uint32_t)len > loop[nloop - 1].la) {
            if (--loop[nloop - 1].lc) {
                next = loop[nloop - 1].start;
            } else {
                nloop--;
                /* An enclosing loop may end on the same instruction. */
                continue;
            }
            break;
        }
        pc = next;
    }
    d->steps += steps;
    return 0;
#undef FAULT
}
