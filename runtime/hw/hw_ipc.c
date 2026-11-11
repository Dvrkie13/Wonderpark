/* Hollywood IPC (0x0D000000): how the PPC talks to IOS.
 *
 * The game's IPC library writes a request's physical address to PPCMSG and
 * sets X1 in PPCCTRL. IOS acknowledges (Y2), handles the request, writes the
 * result into it (result at +4, the original command at +8, IPC_REPLY at +0),
 * puts its address in ARMMSG and sets Y1. Both raise the Hollywood IPC
 * interrupt when enabled (IY2/IY1), which reaches the CPU as PI cause 14. The
 * game's own interrupt handler then runs the callbacks and frees the request.
 *
 * Here IOS (runtime/ios/ios.c) runs on a thread of its own, as on a Wii,
 * where it is another processor: requests are handled in the order sent,
 * and the game thread goes on meanwhile. Its disc and NAND requests read
 * and write host files, and on a busy hard drive one can take a second or
 * more, a frozen game if the game thread waited for it. Each reply is
 * picked up at the next device tick (hw_ipc_tick), and a reply wakes the
 * idle loop's sleep (wp_wake_event). WONDERPARK_SYNC_IOS=1 handles requests
 * on the game thread at once instead. Requests that only complete on an
 * event (/dev/stm/eventhook) are never answered, as on a console where
 * nothing presses the power button. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include <stdlib.h>

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

/* IOS has written the reply into the request: queue it for the PPC. */
static void complete(u32 req, int32_t result) {
    WP_LOG("ipc", "request %08X (cmd %u) -> %d", req, LD32(req + 8), result);
    if (s_tail - s_head >= REPLY_QUEUE) wp_fatal("IPC reply queue full");
    s_replies[s_tail++ % REPLY_QUEUE] = req;
}

/* Handles a request and writes the reply into it; 0 if it never completes. */
static int handle(u32 req, int32_t* result) {
    u32 cmd = LD32(req);
    *result = ios_handle(req);
    if (*result == IPC_PENDING) return 0;
    ST32(req + 4, (u32)*result);
    ST32(req + 8, cmd);
    ST32(req + 0, IPC_REPLY);
    return 1;
}

#ifdef _WIN32
/* The IOS thread: requests in (s_work), replies out (s_done). */
#define WORK_QUEUE 64
static CRITICAL_SECTION s_lock;
static HANDLE s_work_event, s_wake;
static u32 s_work[WORK_QUEUE], s_done[WORK_QUEUE];
static int32_t s_done_result[WORK_QUEUE];
static unsigned s_work_head, s_work_tail, s_done_head, s_done_tail;
static volatile LONG s_busy;
static int s_sync = -1;

static DWORD WINAPI ios_thread(LPVOID arg) {
    (void)arg;
    for (;;) {
        u32 req;
        int32_t result;
        WaitForSingleObject(s_work_event, INFINITE);
        for (;;) {
            EnterCriticalSection(&s_lock);
            if (s_work_head == s_work_tail) {
                s_busy = 0;
                LeaveCriticalSection(&s_lock);
                break;
            }
            req = s_work[s_work_head++ % WORK_QUEUE];
            LeaveCriticalSection(&s_lock);
            if (!handle(req, &result)) continue;
            EnterCriticalSection(&s_lock);
            s_done[s_done_tail % WORK_QUEUE] = req;
            s_done_result[s_done_tail++ % WORK_QUEUE] = result;
            LeaveCriticalSection(&s_lock);
            SetEvent(s_wake);
        }
    }
    return 0;
}

/* At exit: let a save being written finish (up to 2 s). */
static void ios_thread_finish(void) {
    int i;
    for (i = 0; i < 2000 && s_busy; i++) Sleep(1);
}

void* wp_wake_event(void) { return s_wake; }

/* Ends the idle loop's sleep (an IOS reply, a GPU interrupt). */
void wp_wake(void) {
    if (s_wake) SetEvent(s_wake);
}

static void collect(void) {
    if (s_done_head == s_done_tail) return; /* read without the lock: at worst picked up next tick */
    EnterCriticalSection(&s_lock);
    while (s_done_head != s_done_tail) {
        complete(s_done[s_done_head % WORK_QUEUE], s_done_result[s_done_head % WORK_QUEUE]);
        s_done_head++;
    }
    LeaveCriticalSection(&s_lock);
}
#else
void* wp_wake_event(void) { return NULL; }
void wp_wake(void) {}
static void collect(void) {}
#endif

static void execute(void) {
    u32 req = s_ppcmsg;
    int32_t result;
    s_ctrl |= Y2; /* acknowledged */
#ifdef _WIN32
    if (!s_sync) {
        EnterCriticalSection(&s_lock);
        if (s_work_tail - s_work_head >= WORK_QUEUE) wp_fatal("IPC request queue full");
        s_work[s_work_tail++ % WORK_QUEUE] = req;
        s_busy = 1;
        LeaveCriticalSection(&s_lock);
        SetEvent(s_work_event);
        update_irq();
        return;
    }
#endif
    if (handle(req, &result)) complete(req, result);
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
    collect();
    deliver_reply();
}

void hw_ipc_init(void) {
    wp_mmio_register(0x0D000000u, 0x100u, ipc_read, ipc_write);
#ifdef _WIN32
    {
        const char* e = getenv("WONDERPARK_SYNC_IOS");
        s_sync = e && *e && *e != '0';
        s_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
        if (!s_sync) {
            InitializeCriticalSection(&s_lock);
            s_work_event = CreateEventA(NULL, FALSE, FALSE, NULL);
            CreateThread(NULL, 0, ios_thread, NULL, 0, NULL);
            atexit(ios_thread_finish);
        }
    }
#endif
}
