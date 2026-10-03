/*
 * MCPX APU MMIO Hook - VEH instruction decoder for APU register access
 *
 * Reuses the same x86-64 instruction decoder pattern as nv2a_mmio_hook.c
 * but routes reads/writes through the MCPX APU register handlers.
 */

#include "apu.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

/* Global APU state pointer -- referenced from main.c regardless of which
 * platform's MMIO hook is active, so define it before the #if guard. */
MCPXAPUState *g_apu_state = NULL;

/* The MMIO hook is a Win32-VEH x86-64 instruction decoder. On Linux the
 * equivalent goes through sigaction + ucontext_t (Stage 2 / main.c). For
 * now the whole body is Windows-only so apu_emu links on Debian. */
#if defined(_WIN32)
#include <windows.h>

/* APU MMIO base in Xbox VA space */
#define APU_MMIO_BASE  0xFE800000u
#define APU_MMIO_SIZE  0x00080000u  /* 512KB */

/* (g_apu_state is defined above, outside the Win32 guard) */

/* Statistics */
static int g_apu_mmio_read_count = 0;
static int g_apu_mmio_write_count = 0;
static int g_apu_mmio_decode_fail = 0;

/* The value the decoder last handed the model, so the RECOMP_APU_TRACE
 * histogram can show what was written (a read-back of a PIO method offset
 * does not return it). Racy across faulting threads; diagnostic only. */
static volatile uint32_t s_last_wval;
static void trace_mmio_write(MCPXAPUState *s, uint32_t off, uint64_t v,
                             unsigned size)
{
    s_last_wval = (uint32_t)v;
    (mcpx_apu_mmio_write)(s, off, v, size);
}
#define mcpx_apu_mmio_write(s, o, v, z) trace_mmio_write((s), (o), (v), (z))

/* ============================================================
 * x86-64 register access helpers (same as NV2A hook)
 * ============================================================ */

static uint64_t *ctx_reg64(PCONTEXT ctx, int reg)
{
    switch (reg & 0xF) {
    case 0:  return (uint64_t*)&ctx->Rax;
    case 1:  return (uint64_t*)&ctx->Rcx;
    case 2:  return (uint64_t*)&ctx->Rdx;
    case 3:  return (uint64_t*)&ctx->Rbx;
    case 4:  return (uint64_t*)&ctx->Rsp;
    case 5:  return (uint64_t*)&ctx->Rbp;
    case 6:  return (uint64_t*)&ctx->Rsi;
    case 7:  return (uint64_t*)&ctx->Rdi;
    case 8:  return (uint64_t*)&ctx->R8;
    case 9:  return (uint64_t*)&ctx->R9;
    case 10: return (uint64_t*)&ctx->R10;
    case 11: return (uint64_t*)&ctx->R11;
    case 12: return (uint64_t*)&ctx->R12;
    case 13: return (uint64_t*)&ctx->R13;
    case 14: return (uint64_t*)&ctx->R14;
    case 15: return (uint64_t*)&ctx->R15;
    default: return NULL;
    }
}

static int decode_modrm_len(const uint8_t *ip, int has_rex_b)
{
    uint8_t modrm = *ip;
    int mod = (modrm >> 6) & 3;
    int rm = (modrm & 7) | (has_rex_b ? 8 : 0);
    int len = 1;

    if (mod == 3) return 1;
    if ((rm & 7) == 4) len += 1; /* SIB */
    if (mod == 0 && (rm & 7) == 5) len += 4; /* disp32 */
    else if (mod == 1) len += 1;
    else if (mod == 2) len += 4;
    return len;
}

/* ============================================================
 * ALU forms against a register (TEST/CMP/ADD/SUB/OR/AND/XOR)
 *
 * Generated code polls and masks registers with more than plain moves:
 * DirectSound tests a status bit with `test dword [mem], imm32`, and a C
 * compiler folds `x += REG` or `if (REG & m)` into an ALU op with a memory
 * operand. Each is one read (and for the read-modify-write forms one write)
 * of the register, then the flags a following jcc reads: ZF, SF, CF, OF.
 * PF/AF are left alone; nothing branches on them after a register read.
 * ADC/SBB and the F6/F7 forms other than TEST are not decoded (they fail
 * visibly instead).
 * ============================================================ */

enum { ALU_ADD = 0, ALU_OR = 1, ALU_AND = 4, ALU_SUB = 5, ALU_XOR = 6,
       ALU_CMP = 7, ALU_TEST = 8 };

static uint64_t size_mask(int size)
{
    return size >= 8 ? ~0ULL : (1ULL << (size * 8)) - 1;
}

/* a OP b at the given width; sets ZF/SF/CF/OF in ctx. */
static uint64_t alu_op(PCONTEXT ctx, int op, uint64_t a, uint64_t b, int size)
{
    uint64_t m = size_mask(size), sign = 1ULL << (size * 8 - 1), r;
    int cf = 0, of = 0;

    a &= m; b &= m;
    switch (op) {
    case ALU_ADD:
        r = (a + b) & m;
        cf = r < a;
        of = ((~(a ^ b) & (a ^ r)) & sign) != 0;
        break;
    case ALU_SUB: case ALU_CMP:
        r = (a - b) & m;
        cf = a < b;
        of = (((a ^ b) & (a ^ r)) & sign) != 0;
        break;
    case ALU_OR:  r = a | b; break;
    case ALU_XOR: r = a ^ b; break;
    default:      r = a & b; break;   /* AND, TEST */
    }
    ctx->EFlags &= ~(0x0001u | 0x0040u | 0x0080u | 0x0800u);
    if (cf) ctx->EFlags |= 0x0001u;
    if (r == 0) ctx->EFlags |= 0x0040u;
    if (r & sign) ctx->EFlags |= 0x0080u;
    if (of) ctx->EFlags |= 0x0800u;
    return r;
}

/* Write an ALU result to a general register with x86-64 width rules: 32-bit
 * writes zero the upper half, 8/16-bit writes merge, and an 8-bit register
 * 4-7 without a REX prefix is AH/CH/DH/BH. */
static void set_reg_sized(PCONTEXT ctx, int reg, uint64_t v, int size,
                          int has_rex)
{
    if (size == 1 && !has_rex && reg >= 4 && reg < 8) {
        uint64_t *d = ctx_reg64(ctx, reg - 4);
        *d = (*d & ~0xFF00ULL) | ((v & 0xFF) << 8);
        return;
    }
    uint64_t *d = ctx_reg64(ctx, reg);
    if (size == 1)      *d = (*d & ~0xFFULL) | (v & 0xFF);
    else if (size == 2) *d = (*d & ~0xFFFFULL) | (v & 0xFFFF);
    else if (size == 4) *d = v & 0xFFFFFFFFULL;
    else                *d = v;
}

static uint64_t get_reg_sized(PCONTEXT ctx, int reg, int size, int has_rex)
{
    if (size == 1 && !has_rex && reg >= 4 && reg < 8)
        return (*ctx_reg64(ctx, reg - 4) >> 8) & 0xFF;
    return *ctx_reg64(ctx, reg) & size_mask(size);
}

/* ============================================================
 * Instruction decoder for APU MMIO access
 * ============================================================ */

static bool apu_decode_and_handle(PCONTEXT ctx, uint32_t mmio_offset, int is_write)
{
    const uint8_t *ip = (const uint8_t *)ctx->Rip;
    if (!g_apu_state) return false;

    int prefix_len = 0;
    int has_66 = 0;
    int rex = 0, has_rex = 0;

    while (1) {
        uint8_t b = ip[prefix_len];
        if (b == 0x66) { has_66 = 1; prefix_len++; }
        else if (b == 0xF2 || b == 0xF3) { prefix_len++; }
        else if (b >= 0x40 && b <= 0x4F) { rex = b; has_rex = 1; prefix_len++; }
        else break;
    }

    int rex_w = has_rex && (rex & 0x08);
    int rex_r = has_rex && (rex & 0x04);
    int rex_b = has_rex && (rex & 0x01);

    const uint8_t *opcode = ip + prefix_len;
    int access_size = 4;
    if (has_66) access_size = 2;
    if (rex_w) access_size = 8;

    /* MOV r/m, r (write: 88/89) */
    if (opcode[0] == 0x89 || opcode[0] == 0x88) {
        if (opcode[0] == 0x88) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t val = *ctx_reg64(ctx, reg);
        mcpx_apu_mmio_write(g_apu_state, mmio_offset, val, access_size);
        ctx->Rip += prefix_len + 1 + modrm_len;
        g_apu_mmio_write_count++;
        return true;
    }

    /* MOV r/m, imm32 (write: C7 /0) */
    if (opcode[0] == 0xC7) {
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        uint32_t imm = *(uint32_t *)(opcode + 1 + modrm_len);
        mcpx_apu_mmio_write(g_apu_state, mmio_offset, imm, access_size);
        ctx->Rip += prefix_len + 1 + modrm_len + 4;
        g_apu_mmio_write_count++;
        return true;
    }

    /* MOV r/m8, imm8 (write: C6 /0) */
    if (opcode[0] == 0xC6) {
        access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        uint8_t imm = *(opcode + 1 + modrm_len);
        mcpx_apu_mmio_write(g_apu_state, mmio_offset, imm, 1);
        ctx->Rip += prefix_len + 1 + modrm_len + 1;
        g_apu_mmio_write_count++;
        return true;
    }

    /* MOV r, r/m (read: 8A/8B) */
    if (opcode[0] == 0x8B || opcode[0] == 0x8A) {
        if (opcode[0] == 0x8A) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t val = mcpx_apu_mmio_read(g_apu_state, mmio_offset, access_size);
        uint64_t *dst = ctx_reg64(ctx, reg);
        if (access_size == 1) *dst = (*dst & ~0xFFULL) | (val & 0xFF);
        else if (access_size == 2) *dst = (*dst & ~0xFFFFULL) | (val & 0xFFFF);
        else if (access_size == 4) *dst = val & 0xFFFFFFFF;
        else *dst = val;
        ctx->Rip += prefix_len + 1 + modrm_len;
        g_apu_mmio_read_count++;
        return true;
    }

    /* MOVZX r32, r/m8 (0F B6) */
    if (opcode[0] == 0x0F && opcode[1] == 0xB6) {
        access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 2, rex_b);
        int reg = ((opcode[2] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t val = mcpx_apu_mmio_read(g_apu_state, mmio_offset, 1);
        *ctx_reg64(ctx, reg) = val & 0xFF;
        ctx->Rip += prefix_len + 2 + modrm_len;
        g_apu_mmio_read_count++;
        return true;
    }

    /* MOVZX r32, r/m16 (0F B7) */
    if (opcode[0] == 0x0F && opcode[1] == 0xB7) {
        access_size = 2;
        int modrm_len = decode_modrm_len(opcode + 2, rex_b);
        int reg = ((opcode[2] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t val = mcpx_apu_mmio_read(g_apu_state, mmio_offset, 2);
        *ctx_reg64(ctx, reg) = val & 0xFFFF;
        ctx->Rip += prefix_len + 2 + modrm_len;
        g_apu_mmio_read_count++;
        return true;
    }

    /* TEST r/m, r (84/85) - read */
    if (opcode[0] == 0x85 || opcode[0] == 0x84) {
        if (opcode[0] == 0x84) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t mem_val = mcpx_apu_mmio_read(g_apu_state, mmio_offset, access_size);
        uint64_t reg_val = *ctx_reg64(ctx, reg);
        uint64_t result = mem_val & reg_val;
        ctx->EFlags &= ~(0x0001 | 0x0040 | 0x0080 | 0x0800);
        if (result == 0) ctx->EFlags |= 0x0040;
        if (result & (1ULL << (access_size * 8 - 1))) ctx->EFlags |= 0x0080;
        ctx->Rip += prefix_len + 1 + modrm_len;
        g_apu_mmio_read_count++;
        return true;
    }

    /* CMP r/m, r (38/39) */
    if (opcode[0] == 0x39 || opcode[0] == 0x38) {
        if (opcode[0] == 0x38) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t mem_val = mcpx_apu_mmio_read(g_apu_state, mmio_offset, access_size);
        uint64_t reg_val = *ctx_reg64(ctx, reg);
        if (access_size <= 4) {
            mem_val &= (1ULL << (access_size * 8)) - 1;
            reg_val &= (1ULL << (access_size * 8)) - 1;
        }
        uint64_t result = mem_val - reg_val;
        ctx->EFlags &= ~(0x0001 | 0x0040 | 0x0080 | 0x0800);
        if (result == 0) ctx->EFlags |= 0x0040;
        if (mem_val < reg_val) ctx->EFlags |= 0x0001;
        if (result & (1ULL << (access_size * 8 - 1))) ctx->EFlags |= 0x0080;
        ctx->Rip += prefix_len + 1 + modrm_len;
        g_apu_mmio_read_count++;
        return true;
    }

    /* OR r/m, r (08/09) */
    if (opcode[0] == 0x09 || opcode[0] == 0x08) {
        if (opcode[0] == 0x08) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t mem_val = mcpx_apu_mmio_read(g_apu_state, mmio_offset, access_size);
        uint64_t reg_val = *ctx_reg64(ctx, reg);
        mcpx_apu_mmio_write(g_apu_state, mmio_offset, mem_val | reg_val, access_size);
        ctx->Rip += prefix_len + 1 + modrm_len;
        g_apu_mmio_write_count++;
        return true;
    }

    /* AND r/m, r (20/21) */
    if (opcode[0] == 0x21 || opcode[0] == 0x20) {
        if (opcode[0] == 0x20) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t mem_val = mcpx_apu_mmio_read(g_apu_state, mmio_offset, access_size);
        uint64_t reg_val = *ctx_reg64(ctx, reg);
        mcpx_apu_mmio_write(g_apu_state, mmio_offset, mem_val & reg_val, access_size);
        ctx->Rip += prefix_len + 1 + modrm_len;
        g_apu_mmio_write_count++;
        return true;
    }

    /* Group 1 immediate: ADD/OR/AND/SUB/XOR/CMP r/m, imm (80/81/83) */
    if (opcode[0] == 0x80 || opcode[0] == 0x81 || opcode[0] == 0x83) {
        int op = (opcode[1] >> 3) & 7;
        if (op != 2 && op != 3) {                    /* not ADC/SBB */
            int modrm_len = decode_modrm_len(opcode + 1, rex_b);
            const uint8_t *immp = opcode + 1 + modrm_len;
            int imm_len;
            uint64_t imm;
            if (opcode[0] == 0x80) {
                access_size = 1; imm_len = 1; imm = immp[0];
            } else if (opcode[0] == 0x83) {
                imm_len = 1; imm = (uint64_t)(int64_t)(int8_t)immp[0];
            } else if (access_size == 2) {
                imm_len = 2; imm = *(const uint16_t *)immp;
            } else {
                imm_len = 4; imm = (uint64_t)(int64_t)*(const int32_t *)immp;
            }
            uint64_t mem_val = mcpx_apu_mmio_read(g_apu_state, mmio_offset, access_size);
            uint64_t r = alu_op(ctx, op, mem_val, imm, access_size);
            g_apu_mmio_read_count++;
            if (op != ALU_CMP) {
                mcpx_apu_mmio_write(g_apu_state, mmio_offset, r, access_size);
                g_apu_mmio_write_count++;
            }
            ctx->Rip += prefix_len + 1 + modrm_len + imm_len;
            return true;
        }
    }

    /* TEST r/m, imm (F6 /0, F7 /0; /1 is an alias) */
    if ((opcode[0] == 0xF6 || opcode[0] == 0xF7)
            && ((opcode[1] >> 3) & 7) <= 1) {
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        const uint8_t *immp = opcode + 1 + modrm_len;
        int imm_len;
        uint64_t imm;
        if (opcode[0] == 0xF6) {
            access_size = 1; imm_len = 1; imm = immp[0];
        } else if (access_size == 2) {
            imm_len = 2; imm = *(const uint16_t *)immp;
        } else {
            imm_len = 4; imm = (uint64_t)(int64_t)*(const int32_t *)immp;
        }
        uint64_t mem_val = mcpx_apu_mmio_read(g_apu_state, mmio_offset, access_size);
        alu_op(ctx, ALU_TEST, mem_val, imm, access_size);
        ctx->Rip += prefix_len + 1 + modrm_len + imm_len;
        g_apu_mmio_read_count++;
        return true;
    }

    /* op r, r/m: ADD/OR/AND/SUB/XOR/CMP with the register as destination
     * (02/03 0A/0B 22/23 2A/2B 32/33 3A/3B) -- one read */
    {
        static const int8_t rd_op[64] = {
            [0x02] = ALU_ADD + 1, [0x03] = ALU_ADD + 1,
            [0x0A] = ALU_OR + 1,  [0x0B] = ALU_OR + 1,
            [0x22] = ALU_AND + 1, [0x23] = ALU_AND + 1,
            [0x2A] = ALU_SUB + 1, [0x2B] = ALU_SUB + 1,
            [0x32] = ALU_XOR + 1, [0x33] = ALU_XOR + 1,
            [0x3A] = ALU_CMP + 1, [0x3B] = ALU_CMP + 1,
        };
        if (opcode[0] < 64 && rd_op[opcode[0]]) {
            int op = rd_op[opcode[0]] - 1;
            if (!(opcode[0] & 1)) access_size = 1;
            int modrm_len = decode_modrm_len(opcode + 1, rex_b);
            int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
            uint64_t mem_val = mcpx_apu_mmio_read(g_apu_state, mmio_offset, access_size);
            uint64_t reg_val = get_reg_sized(ctx, reg, access_size, has_rex);
            uint64_t r = alu_op(ctx, op, reg_val, mem_val, access_size);
            if (op != ALU_CMP)
                set_reg_sized(ctx, reg, r, access_size, has_rex);
            ctx->Rip += prefix_len + 1 + modrm_len;
            g_apu_mmio_read_count++;
            return true;
        }
    }

    /* op r/m, r: ADD/SUB/XOR with the register as destination of a
     * read-modify-write (00/01 28/29 30/31); OR/AND/CMP are above */
    if (opcode[0] == 0x00 || opcode[0] == 0x01 || opcode[0] == 0x28
            || opcode[0] == 0x29 || opcode[0] == 0x30 || opcode[0] == 0x31) {
        int op = opcode[0] < 0x28 ? ALU_ADD : opcode[0] < 0x30 ? ALU_SUB : ALU_XOR;
        if (!(opcode[0] & 1)) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t mem_val = mcpx_apu_mmio_read(g_apu_state, mmio_offset, access_size);
        uint64_t reg_val = get_reg_sized(ctx, reg, access_size, has_rex);
        uint64_t r = alu_op(ctx, op, mem_val, reg_val, access_size);
        mcpx_apu_mmio_write(g_apu_state, mmio_offset, r, access_size);
        ctx->Rip += prefix_len + 1 + modrm_len;
        g_apu_mmio_read_count++;
        g_apu_mmio_write_count++;
        return true;
    }

    /* Unrecognized */
    g_apu_mmio_decode_fail++;
    if (g_apu_mmio_decode_fail <= 20) {
        fprintf(stderr, "[APU] MMIO decode fail at RIP=%p offset=0x%X: %02X %02X %02X %02X %02X %02X\n",
                (void*)ctx->Rip, mmio_offset, ip[0], ip[1], ip[2], ip[3], ip[4], ip[5]);
        fflush(stderr);
    }
    return false;
}

/* ============================================================
 * Public API (called from VEH in main.c)
 * ============================================================ */

/* RECOMP_APU_TRACE statistics.
 *
 * The 400-line trace shows the first accesses only, and the decoder's own
 * counters were never printed, so neither the steady-state rate nor which
 * GP/EP registers a title writes could be read from a log. With the trace on,
 * every access is tallied per dword offset and direction, and a reporter
 * thread prints a stats line every 10 s and the per-offset table every 30 s
 * (periodic, because a watchdog exit or a SIGINT never reaches atexit):
 *
 *   [APUMMIO-STATS] t=..s reads=N (+n/s) writes=N (+n/s) fails=N handler=..us
 *   [APUMMIO-HIST] 0xOFFSET r=N w=N lastw=VALUE
 *
 * handler is the time spent inside the decoder and model per access; the
 * exception round trip around it is not visible from here (see
 * apu_hook_fault_bench). */
#define APU_HIST_SLOTS (APU_MMIO_SIZE / 4)
static int s_trace = -1;               /* getenv once, not per access */
static volatile LONG s_hist_r[APU_HIST_SLOTS], s_hist_w[APU_HIST_SLOTS];
static volatile uint32_t s_hist_lastw[APU_HIST_SLOTS];
static volatile LONG64 s_tot_r, s_tot_w, s_handler_ticks;
static volatile LONG s_reporter_started, s_bench_active;
static LARGE_INTEGER s_qpf;

static void apu_trace_report(int with_table, double t)
{
    static LONG64 prev_r, prev_w;
    static double prev_t;
    LONG64 r = s_tot_r, w = s_tot_w, ticks = s_handler_ticks;
    double dt = t - prev_t > 0 ? t - prev_t : 1;
    fprintf(stderr, "[APUMMIO-STATS] t=%.1fs reads=%lld (+%.0f/s) writes=%lld"
            " (+%.0f/s) fails=%d handler=%.2fus/access\n", t,
            (long long)r, (r - prev_r) / dt, (long long)w, (w - prev_w) / dt,
            g_apu_mmio_decode_fail,
            (r + w) ? (double)ticks * 1e6 / (double)s_qpf.QuadPart / (double)(r + w)
                    : 0.0);
    prev_r = r; prev_w = w; prev_t = t;
    if (with_table) {
        for (uint32_t i = 0; i < APU_HIST_SLOTS; i++)
            if (s_hist_r[i] || s_hist_w[i])
                fprintf(stderr, "[APUMMIO-HIST] 0x%05X r=%ld w=%ld lastw=%08X\n",
                        i * 4, (long)s_hist_r[i], (long)s_hist_w[i],
                        s_hist_lastw[i]);
    }
    fflush(stderr);
}

static DWORD WINAPI apu_trace_reporter(LPVOID unused)
{
    LARGE_INTEGER t0, now;
    unsigned k = 0;
    (void)unused;
    QueryPerformanceCounter(&t0);
    for (;;) {
        Sleep(10000);
        QueryPerformanceCounter(&now);
        apu_trace_report(++k % 3 == 0,
                         (double)(now.QuadPart - t0.QuadPart) / (double)s_qpf.QuadPart);
    }
    return 0;
}

static int apu_trace_on(void)
{
    if (s_trace < 0) {
        s_trace = getenv("RECOMP_APU_TRACE") != NULL;
        QueryPerformanceFrequency(&s_qpf);
    }
    return s_trace;
}

bool apu_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                          uint32_t fault_xbox_va, int is_write)
{
    uint32_t mmio_offset = fault_xbox_va - APU_MMIO_BASE;
    LARGE_INTEGER a, b;
    int trace = apu_trace_on() && !s_bench_active;

    (void)fault_addr;
    if (trace)
        QueryPerformanceCounter(&a);
    bool ok = apu_decode_and_handle(ctx, mmio_offset, is_write);
    if (!trace)
        return ok;
    QueryPerformanceCounter(&b);
    InterlockedExchangeAdd64(&s_handler_ticks, b.QuadPart - a.QuadPart);

    if (mmio_offset < APU_MMIO_SIZE) {
        uint32_t slot = mmio_offset >> 2;
        if (is_write) {
            InterlockedIncrement64(&s_tot_w);
            InterlockedIncrement(&s_hist_w[slot]);
            s_hist_lastw[slot] = s_last_wval;
        } else {
            InterlockedIncrement64(&s_tot_r);
            InterlockedIncrement(&s_hist_r[slot]);
        }
    }
    if (!s_reporter_started && !InterlockedExchange(&s_reporter_started, 1)) {
        HANDLE h = CreateThread(NULL, 0, apu_trace_reporter, NULL, 0, NULL);
        if (h)
            CloseHandle(h);
    }

    /* What the title actually asks the APU for. The DSPs are stubbed here, so
     * a title that waits on one waits forever, and the only way to work out
     * what it is waiting for is to see the register traffic that precedes the
     * wait. */
    {
        static unsigned n;
        if (n++ < 400) {
            /* The value as well as the offset: finding which register carries
             * the command-block address means recognising the address when it
             * goes past, and an offset alone never shows it. A write shows the
             * value written; a read, what the model returned. */
            uint64_t v = is_write ? s_last_wval
                       : g_apu_state
                       ? mcpx_apu_mmio_read(g_apu_state, mmio_offset, 4) : 0;
            fprintf(stderr, "  [APUMMIO] %s 0x%05X = %08X%s\n",
                    is_write ? "write" : "read ", mmio_offset,
                    (uint32_t)v, ok ? "" : "  (decode failed)");
        }
    }
    return ok;
}

/* RECOMP_APU_FAULT_BENCH=<n>: the full cost of one trapped access.
 *
 * Time n reads of NV1BA0_PIO_FREE (0xFE820010, read-only, the register
 * DirectSound polls most) through the same page fault, VEH and decoder a
 * guest access takes, from the calling thread. Call it after the VEH is
 * installed and g_apu_state is set. The accesses are left out of the
 * RECOMP_APU_TRACE tallies. Prints one line and returns the microseconds per
 * access (0 if it did not run). */
double apu_hook_fault_bench(unsigned n)
{
    extern ptrdiff_t xbox_GetMemoryOffset(void);
    volatile uint32_t *p;
    LARGE_INTEGER f, a, b;
    uint32_t sink = 0;
    double us;

    if (!g_apu_state || !n)
        return 0;
    p = (volatile uint32_t *)((uintptr_t)xbox_GetMemoryOffset() + 0xFE820010u);
    QueryPerformanceFrequency(&f);
    InterlockedExchange(&s_bench_active, 1);
    QueryPerformanceCounter(&a);
    /* A plain MOV load: left to itself the compiler folds the read into
     * `add r32, [mem]`, a form the decoder does not handle. */
    for (unsigned i = 0; i < n; i++) {
        uint32_t v;
        __asm__ volatile("movl (%1), %0" : "=r"(v) : "r"(p) : "memory");
        sink += v;
    }
    QueryPerformanceCounter(&b);
    InterlockedExchange(&s_bench_active, 0);
    us = (double)(b.QuadPart - a.QuadPart) * 1e6 / (double)f.QuadPart / n;
    fprintf(stderr, "[APU] fault bench: %u trapped reads of PIO_FREE, %.2f us"
            " each (fault + VEH + decode + model; value 0x%X)\n",
            n, us, sink / n);
    return us;
}

#endif /* _WIN32 */
