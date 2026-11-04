/* Host interrupts: guest callbacks the runtime has to call "from an
 * interrupt" (Wii Remote reports and completions, device timers), run the
 * way the OS runs interrupt handlers, without nesting host calls:
 *
 *   1. at a slice boundary with MSR[EE] set: an external-interrupt entry;
 *      the interrupted state is saved in OSCurrentContext (all GPRs, CR,
 *      LR, CTR, XER, SRR0/1, GQRs) and marked as saved by an exception;
 *   2. OSClearContext + OSSetCurrentContext(scratch context on the stack),
 *      as the OS's handlers do around user callbacks, so the scheduler
 *      won't switch threads inside them;
 *   3. each callback, as a plain guest call;
 *   4. OSClearContext(scratch) + OSSetCurrentContext(the thread's context);
 *   5. __OSReschedule (which may switch to a thread a callback woke, the
 *      saved context being an exception one), then OSLoadContext.
 *
 * Every step is an ordinary guest call that "returns" to a magic address
 * the dispatcher recognises; the sequence is only in progress while
 * interrupts are off and the scratch context is current, so nothing else
 * can interleave. */
#include <string.h>

#include "hle.h"

/* S2LE01 (config/S2LE01/symbols.txt) */
#define OS_SET_CURRENT_CONTEXT 0x8028E950u
#define OS_LOAD_CONTEXT 0x8028EA40u
#define OS_CLEAR_CONTEXT 0x8028EB90u
#define OS_RESCHEDULE 0x80296170u
#define OS_CURRENT_CONTEXT 0x800000D4u
#define OS_CURRENT_CONTEXT_PHYS 0x800000C0u

#define CTX_SIZE 0x2C8u

#define QUEUE 64
static struct { u32 fn, r3, r4; } s_q[QUEUE];
static unsigned s_qh, s_qt;

enum { IDLE, CLEAR_SCRATCH, SET_SCRATCH, CALLBACKS, CLEAR_SCRATCH2, SET_THREAD };
static int s_step = IDLE;
static u32 s_thread_ctx, s_scratch, s_frame_sp;

void wp_irq_post(u32 fn, u32 r3, u32 r4) {
    if (!fn) return;
    if (s_qt - s_qh >= QUEUE) {
        WP_WARN("host interrupt queue full; dropping a callback to %08X", fn);
        return;
    }
    s_q[s_qt % QUEUE].fn = fn;
    s_q[s_qt % QUEUE].r3 = r3;
    s_q[s_qt % QUEUE].r4 = r4;
    s_qt++;
}

int wp_irq_pending(void) { return s_qh != s_qt; }

static void guest_call(CPUState* c, u32 fn, u32 r3, u32 r4) {
    c->gpr[1] = s_frame_sp;
    c->gpr[3] = r3;
    c->gpr[4] = r4;
    c->lr = WP_MAGIC_IRQ_STEP;
    c->pc = fn;
}

/* Step 1: called by the dispatcher between slices. */
int wp_irq_begin(CPUState* c) {
    u32 ctx, i;
    if (s_step != IDLE || !wp_irq_pending() || !(c->msr & 0x8000u)) return 0;
    ctx = LD32(OS_CURRENT_CONTEXT);
    if (!ctx || !wp_mem_ptr(ctx, CTX_SIZE)) return 0;

    /* Hardware entry for an external interrupt. */
    c->srr0 = c->pc;
    c->srr1 = c->msr & 0x87C0FFFFu;
    c->msr = (c->msr & ~0x0004EF37u) | 0x30u; /* EE PR FP FE0 SE BE FE1 IP? RI LE off; IR DR on (the stub's rfi) */

    /* What __OSEVStart and OS_EXCEPTION_SAVE_GPRS store. */
    for (i = 0; i < 32; i++) ST32(ctx + 4 * i, c->gpr[i]);
    ST32(ctx + 0x80, c->cr);
    ST32(ctx + 0x84, c->lr);
    ST32(ctx + 0x88, c->ctr);
    ST32(ctx + 0x8C, c->xer);
    ST32(ctx + 0x198, c->srr0);
    ST32(ctx + 0x19C, c->srr1);
    for (i = 1; i < 8; i++) ST32(ctx + 0x1A4 + 4 * i, c->gqr[i]);
    ST16(ctx + 0x1A2, LD16(ctx + 0x1A2) | 2); /* OS_CONTEXT_STATE_EXC */

    s_thread_ctx = ctx;
    /* A frame below the interrupted stack for the scratch context and the calls. */
    s_scratch = ((c->gpr[1] - CTX_SIZE - 0x40) & ~7u);
    s_frame_sp = (s_scratch - 0x100) & ~15u;
    ST32(s_frame_sp, c->gpr[1]); /* back chain */
    s_step = CLEAR_SCRATCH;
    guest_call(c, OS_CLEAR_CONTEXT, s_scratch, 0);
    WP_LOG("irq", "host interrupt at %08X: %u callback(s)", c->srr0, s_qt - s_qh);
    return 1;
}

/* Steps 2-5: the dispatcher calls this when pc reaches WP_MAGIC_IRQ_STEP
 * (a step's guest call returned) or WP_MAGIC_LOAD_CONTEXT. */
void wp_irq_step(CPUState* c) {
    if (c->pc == WP_MAGIC_LOAD_CONTEXT) {
        /* __OSReschedule kept this thread: resume it. */
        c->gpr[3] = LD32(OS_CURRENT_CONTEXT);
        c->pc = OS_LOAD_CONTEXT;
        return;
    }
    switch (s_step) {
    case CLEAR_SCRATCH:
        s_step = SET_SCRATCH;
        guest_call(c, OS_SET_CURRENT_CONTEXT, s_scratch, 0);
        return;
    case SET_SCRATCH:
    case CALLBACKS:
        if (wp_irq_pending()) {
            u32 fn = s_q[s_qh % QUEUE].fn, r3 = s_q[s_qh % QUEUE].r3, r4 = s_q[s_qh % QUEUE].r4;
            s_qh++;
            s_step = CALLBACKS;
            guest_call(c, fn, r3, r4);
            return;
        }
        s_step = CLEAR_SCRATCH2;
        guest_call(c, OS_CLEAR_CONTEXT, s_scratch, 0);
        return;
    case CLEAR_SCRATCH2:
        s_step = SET_THREAD;
        guest_call(c, OS_SET_CURRENT_CONTEXT, s_thread_ctx, 0);
        return;
    case SET_THREAD:
        s_step = IDLE;
        c->gpr[1] = s_frame_sp;
        c->lr = WP_MAGIC_LOAD_CONTEXT;
        c->pc = OS_RESCHEDULE;
        return;
    default:
        wp_fatal("host interrupt step with nothing in progress");
    }
}

/* ---- host timers: periodic host callbacks (device reports), on host time. */
#define TIMERS 8
static struct { int on; uint64_t next, period; void (*fire)(void); } s_timers[TIMERS];

void wp_timer_start(int id, uint64_t period, void (*fire)(void)) {
    s_timers[id].period = period ? period : 1;
    s_timers[id].fire = fire;
    if (!s_timers[id].on) s_timers[id].next = wp_host_time() + s_timers[id].period;
    s_timers[id].on = 1;
}

void wp_timer_stop(int id) { s_timers[id].on = 0; }

void wp_timers_tick(uint64_t now) {
    int i;
    for (i = 0; i < TIMERS; i++) {
        if (!s_timers[i].on || (int64_t)(now - s_timers[i].next) < 0) continue;
        s_timers[i].next += s_timers[i].period;
        if ((int64_t)(now - s_timers[i].next) > (int64_t)(TB_HZ / 4)) s_timers[i].next = now + s_timers[i].period;
        s_timers[i].fire();
    }
}
