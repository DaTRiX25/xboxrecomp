/* The APU raises its interrupt the way the title's driver expects.
 *
 * The driver arms FETFORCE1 so that an idle voice traps the front end, and
 * resumes it from its ISR. This checks the device half of that: the trap
 * records the method and latches FETINTSTS, which asserts GINTSTS when IEN
 * allows; an ack while still trapped keeps the line up (level-triggered, so
 * the ISR is owed another call); resuming and acking drops it; a masked trap
 * keeps the line low and an unarmed one does not trap.
 *
 * A trap only sets set_irq; the frame thread turns that into the line. An
 * ISTS write runs the same update, so a write of 0 (acks nothing) stands in
 * for the frame thread here. */
#include "apu_state.h"
#include "apu.h"
#include "apu_regs.h"

#include <stdio.h>
#include <stdlib.h>

/* The runtime links against the title's dispatch; nothing here calls it. */
void *recomp_lookup(unsigned long address) { (void)address; abort(); }
void *recomp_lookup_manual(unsigned long address) { (void)address; abort(); }

static int failures;
#define CHECK(name, cond) \
    do { if (!(cond)) { printf("FAIL: %s\n", name); failures++; } } while (0)

#define VP(off) (0x20000u + (off))

static uint32_t reg(MCPXAPUState *d, uint32_t r) { return d->regs[r]; }
static int line(MCPXAPUState *d) { return !!(reg(d, NV_PAPU_ISTS) & NV_PAPU_ISTS_GINTSTS); }
static void settle(MCPXAPUState *d) { mcpx_apu_mmio_write(d, NV_PAPU_ISTS, 0, 4); }

int main(void)
{
    MCPXAPUState *d = (MCPXAPUState *)calloc(1, sizeof(*d));
    if (!d) return 2;
    qemu_mutex_init(&d->lock);
    qemu_cond_init(&d->cond);
    qemu_cond_init(&d->idle_cond);
    printf("apu_irq: running\n");

    /* The title's own sequence: IEN 0xD9, FETFORCE1 = SE2FE_IDLE_VOICE. */
    mcpx_apu_mmio_write(d, NV_PAPU_IEN, 0xD9, 4);
    mcpx_apu_mmio_write(d, NV_PAPU_FETFORCE1,
                        NV_PAPU_FETFORCE1_SE2FE_IDLE_VOICE, 4);
    settle(d);
    CHECK("quiet before any trap", !line(d));

    /* 1. An idle voice traps the front end and raises the line. */
    mcpx_apu_mmio_write(d, VP(SE2FE_IDLE_VOICE), 66, 4);
    CHECK("FECTL trapped",
          (reg(d, NV_PAPU_FECTL) & NV_PAPU_FECTL_FEMETHMODE)
              == NV_PAPU_FECTL_FEMETHMODE_TRAPPED);
    CHECK("trap reason requested",
          (reg(d, NV_PAPU_FECTL) & NV_PAPU_FECTL_FETRAPREASON)
              == NV_PAPU_FECTL_FETRAPREASON_REQUESTED);
    CHECK("FEDECMETH/PARAM name the voice",
          reg(d, NV_PAPU_FEDECMETH) == SE2FE_IDLE_VOICE
              && reg(d, NV_PAPU_FEDECPARAM) == 66);
    CHECK("trap asks for the line", d->set_irq);
    settle(d);
    CHECK("FETINTSTS latched", reg(d, NV_PAPU_ISTS) & NV_PAPU_ISTS_FETINTSTS);
    CHECK("line asserted", line(d));

    /* 2. The ISR acks before it resumes the front end: still trapped, so the
     *    ack re-latches and the line stays up for a second call. */
    mcpx_apu_mmio_write(d, NV_PAPU_ISTS, 0xFFFFFFFFu, 4);
    CHECK("ack while trapped keeps the line", line(d));

    /* 3. Resumed (FECTL free running) and acked: the line drops. */
    mcpx_apu_mmio_write(d, NV_PAPU_FECTL,
                        NV_PAPU_FECTL_FEMETHMODE_FREE_RUNNING, 4);
    mcpx_apu_mmio_write(d, NV_PAPU_ISTS, 0xFFFFFFFFu, 4);
    CHECK("resume + ack drops the line", !line(d));
    CHECK("ISTS clear", reg(d, NV_PAPU_ISTS) == 0);

    /* 4. Masked: the trap still latches its status but the line stays low. */
    mcpx_apu_mmio_write(d, NV_PAPU_IEN, 0x00, 4);
    mcpx_apu_mmio_write(d, VP(SE2FE_IDLE_VOICE), 67, 4);
    settle(d);
    CHECK("masked: status latched", reg(d, NV_PAPU_ISTS) & NV_PAPU_ISTS_FETINTSTS);
    CHECK("masked: line low", !line(d));

    /* 5. Not armed: an idle voice is just a method, no trap. */
    mcpx_apu_mmio_write(d, NV_PAPU_FECTL,
                        NV_PAPU_FECTL_FEMETHMODE_FREE_RUNNING, 4);
    mcpx_apu_mmio_write(d, NV_PAPU_ISTS, 0xFFFFFFFFu, 4);
    mcpx_apu_mmio_write(d, NV_PAPU_IEN, 0xD9, 4);
    mcpx_apu_mmio_write(d, NV_PAPU_FETFORCE1, 0, 4);
    mcpx_apu_mmio_write(d, VP(SE2FE_IDLE_VOICE), 68, 4);
    settle(d);
    CHECK("unarmed: no trap",
          (reg(d, NV_PAPU_FECTL) & NV_PAPU_FECTL_FEMETHMODE)
              == NV_PAPU_FECTL_FEMETHMODE_FREE_RUNNING);
    CHECK("unarmed: line low", !line(d));

    if (failures) { printf("apu_irq: %d failure(s)\n", failures); return 1; }
    printf("apu_irq: all passed\n");
    return 0;
}
