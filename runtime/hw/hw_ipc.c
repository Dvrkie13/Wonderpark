/* Hollywood IPC (0x0D000000): how the PPC talks to IOS.
 *
 * The game's IPC library writes a request's physical address to PPCMSG and
 * sets X1 in PPCCTRL. IOS acknowledges (Y2), handles the request, writes the
 * result into it (result at +4, the original command at +8, IPC_REPLY at +0),
 * puts its address in ARMMSG and sets Y1. Both raise the Hollywood IPC
 * interrupt when enabled (IY2/IY1), which reaches the CPU as PI cause 14. The
 * game's own interrupt handler then runs the callbacks and frees the request.
 * Here IOS answers at once (runtime/ios/ios.c). Requests that only complete on
 * an event (/dev/stm/eventhook) are never answered, as on a console where
 * nothing presses the power button. */
#include "../core/wp.h"

int32_t ios_handle(uint32_t req);

#define IPC_PENDING 0x7FFFFFFF
#define IPC_REPLY 8

enum { X1 = 1u << 0, Y2 = 1u << 1, Y1 = 1u << 2, X2 = 1u << 3, IY1 = 1u << 4, IY2 = 1u << 5 };
#define HOLLYWOOD_IPC_IRQ (1u << 30)

static u32 s_ppcmsg, s_ctrl, s_armmsg;
static u32 s_irq_flag, s_irq_mask; /* 0x30 / 0x34 (PPC side) */

#define REPLY_QUEUE 64
static u32 s_replies[REPLY_QUEUE];
static unsigned s_head, s_tail;

static void update_irq(void) {
    if ((s_ctrl & Y2 && s_ctrl & IY2) || (s_ctrl & Y1 && s_ctrl & IY1)) s_irq_flag |= HOLLYWOOD_IPC_IRQ;
    hw_pi_set(PI_INT_IPC, (s_irq_flag & s_irq_mask) != 0);
}

static void deliver_reply(void) {
    if (s_ctrl & Y1 || s_head == s_tail) return; /* the previous reply isn't taken yet */
    s_armmsg = s_replies[s_head++ % REPLY_QUEUE];
    s_ctrl |= Y1;
    update_irq();
}

static void execute(void) {
    u32 req = s_ppcmsg;
    u32 cmd = LD32(req);
    int32_t result = ios_handle(req);
    WP_LOG("ipc", "request %08X cmd %u fd %d -> %d", req, cmd, (int32_t)LD32(req + 8), result);
    s_ctrl |= Y2; /* acknowledged */
    if (result != IPC_PENDING) {
        ST32(req + 4, (u32)result);
        ST32(req + 8, cmd);
        ST32(req + 0, IPC_REPLY);
        if (s_tail - s_head >= REPLY_QUEUE) wp_fatal("IPC reply queue full");
        s_replies[s_tail++ % REPLY_QUEUE] = req;
    }
    update_irq();
}

static u32 ipc_read(u32 a, int size) {
    (void)size;
    switch (a & 0xFFFFu) {
    case 0x00: return s_ppcmsg;
    case 0x04: return s_ctrl;
    case 0x08: return s_armmsg;
    case 0x30: return s_irq_flag;
    case 0x34: return s_irq_mask;
    default:
        WP_LOG("ipc", "read %08X", a);
        return 0;
    }
}

static void ipc_write(u32 a, u32 v, int size) {
    (void)size;
    switch (a & 0xFFFFu) {
    case 0x00: s_ppcmsg = v; break;
    case 0x04:
        /* Y1/Y2 are cleared by writing 1; IY1/IY2 are plain enables. */
        s_ctrl = (s_ctrl & ~(IY1 | IY2)) | (v & (IY1 | IY2));
        if (v & Y2) s_ctrl &= ~Y2;
        if (v & Y1) s_ctrl &= ~Y1;
        if (v & X1) execute();
        if (v & X2) deliver_reply(); /* the PPC took the last reply: the next may come */
        update_irq();
        break;
    case 0x30: s_irq_flag &= ~v; update_irq(); break;
    case 0x34: s_irq_mask = v; update_irq(); break;
    default:
        WP_LOG("ipc", "write %08X = %08X", a, v);
        break;
    }
}

void hw_ipc_tick(void) {
    deliver_reply();
}

void hw_ipc_init(void) { wp_mmio_register(0x0D000000u, 0x100u, ipc_read, ipc_write); }
