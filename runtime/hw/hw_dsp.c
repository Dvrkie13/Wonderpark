/* The DSP (0x0C005000): mailboxes, control/status, ARAM DMA and AI DMA, with
 * the DSP's side done at a high level (as Dolphin's DSP HLE does):
 *
 *  - Boot: the SDK's task manager (__DSP_boot_task) waits for the boot ROM's
 *    0x8071FEED, sends the ucode's addresses in 0x80F3xxxx mails, and the
 *    ucode then reports DSP_INIT (0xDCD10000) with an interrupt.
 *  - AX ucode: a 0xBABE<size> mail announces a command list whose address
 *    follows; the frame is run (mixing comes with the audio step) and the
 *    ucode yields (DSP_YIELD, 0xDCD10002). The task manager answers
 *    MAIL_RESUME (0xCDD10000), which gets DSP_RESUME (0xDCD10001), and the
 *    OS's __DSPHandler calls AX's resume callback.
 *  - AI DMA: AX's output buffer is "played" at 32 kHz; each buffer raises
 *    AIDINT, and the OS's AI handler calls AX's frame callback.
 *
 * CSR (0x0A): 0 RES, 1 PIINT, 2 HALT, 3 AIDINT, 4 AIDINTMSK, 5 ARINT,
 * 6 ARINTMSK, 7 DSPINT, 8 DSPINTMSK, 9 DSPDMA, 11 DSPINIT. Status bits
 * (3, 5, 7) are cleared by writing 1. */
#include <string.h>

#include "../core/wp.h"

enum { CSR_RES = 1 << 0, CSR_HALT = 1 << 2, CSR_AIDINT = 1 << 3, CSR_AIDMSK = 1 << 4, CSR_ARINT = 1 << 5,
       CSR_ARMSK = 1 << 6, CSR_DSPINT = 1 << 7, CSR_DSPMSK = 1 << 8, CSR_DMA = 1 << 9, CSR_INIT = 1 << 11 };

#define DSP_INIT 0xDCD10000u
#define DSP_RESUME 0xDCD10001u
#define DSP_YIELD 0xDCD10002u
#define MAIL_RESUME 0xCDD10000u
#define MAIL_NEW_UCODE 0xCDD10001u
#define MAIL_RESET 0xCDD10002u
#define MAIL_CONTINUE 0xCDD10003u

static u16 s_regs[0x80 / 2];
#define R(off) s_regs[(off) / 2]
static u16 s_csr;
static u16 s_to_dsp_hi;

/* Mail from the DSP: a queue; the head is visible in 0x04/0x06. */
#define MAILQ 16
static struct { u32 mail; int irq; } s_out[MAILQ];
static unsigned s_oh, s_ot;
static int s_out_visible; /* head shown to the CPU (valid bit set) */

typedef enum { ST_ROM, ST_BOOT_MAILS, ST_UCODE } State;
static State s_state = ST_ROM;
static int s_boot_mails;      /* address words still to come after a 0x80F3xxxx */
static int s_cmdlist_next;    /* the next mail is a command list's address */
static u32 s_cmdlist_size;
static int s_upload_setup;    /* after MAIL_NEW_UCODE: absorb the upload mails */
u32 g_dsp_frames;

/* Called once per AX frame with the command list's address (hw audio hooks in here). */
void (*g_dsp_ax_frame)(u32 cmdlist, u32 size);

static void update_pi(void) {
    int on = ((s_csr & CSR_AIDINT) && (s_csr & CSR_AIDMSK)) || ((s_csr & CSR_ARINT) && (s_csr & CSR_ARMSK)) ||
             ((s_csr & CSR_DSPINT) && (s_csr & CSR_DSPMSK));
    hw_pi_set(PI_INT_DSP, on);
}

static void show_next_mail(void) {
    if (s_out_visible || s_oh == s_ot) return;
    s_out_visible = 1;
    if (s_out[s_oh % MAILQ].irq) {
        s_csr |= CSR_DSPINT;
        update_pi();
    }
}

static void push_mail(u32 mail, int irq) {
    if (s_ot - s_oh >= MAILQ) { WP_WARN("DSP mail queue full, dropping %08X", mail); return; }
    s_out[s_ot % MAILQ].mail = mail;
    s_out[s_ot % MAILQ].irq = irq;
    s_ot++;
    WP_LOG("dsp", "DSP -> CPU %08X%s", mail, irq ? " (interrupt)" : "");
    show_next_mail();
}

static void reset_dsp(void) {
    s_state = ST_ROM;
    s_oh = s_ot = 0;
    s_out_visible = 0;
    s_boot_mails = s_cmdlist_next = s_upload_setup = 0;
}

static void mail_from_cpu(u32 mail) {
    WP_LOG("dsp", "CPU -> DSP %08X", mail);
    switch (s_state) {
    case ST_ROM:
    case ST_BOOT_MAILS:
        /* __DSP_boot_task: 0x80F3A001 iram_mmem, 0x80F3C002 iram_addr,
         * 0x80F3A002 iram_len, 0x80F3B002 dram_mmem? , 0x80F3D001 init vector. */
        if (s_boot_mails) {
            if (--s_boot_mails == 0 && s_state == ST_BOOT_MAILS && s_upload_setup == 2) {
                /* The init vector arrived: the ucode runs and says hello. */
                s_upload_setup = 0;
                s_state = ST_UCODE;
                push_mail(DSP_INIT, 1);
            }
            return;
        }
        if ((mail & 0xFFFF0000u) == 0x80F30000u) {
            s_state = ST_BOOT_MAILS;
            s_boot_mails = 1;
            if (mail == 0x80F3D001u) s_upload_setup = 2; /* last one */
            return;
        }
        WP_LOG("dsp", "boot ROM: unexpected mail %08X", mail);
        return;
    case ST_UCODE:
        if (s_cmdlist_next) {
            s_cmdlist_next = 0;
            g_dsp_frames++;
            if (g_dsp_ax_frame) g_dsp_ax_frame(mail, s_cmdlist_size);
            push_mail(DSP_YIELD, 1);
            return;
        }
        if (s_upload_setup) {
            /* MAIL_NEW_UCODE: the same 0x80F3 upload sequence follows. */
            if ((mail & 0xFFFF0000u) == 0x80F30000u) {
                s_state = ST_BOOT_MAILS;
                s_boot_mails = 1;
                s_upload_setup = mail == 0x80F3D001u ? 2 : 1;
            }
            return;
        }
        if ((mail & 0xFFFF0000u) == 0xBABE0000u) { s_cmdlist_next = 1; s_cmdlist_size = mail & 0xFFFF; return; }
        switch (mail) {
        case MAIL_RESUME: push_mail(DSP_RESUME, 1); return;
        case MAIL_NEW_UCODE: s_upload_setup = 1; return;
        case MAIL_RESET: reset_dsp(); return;
        case MAIL_CONTINUE: return; /* a command list follows */
        default: WP_LOG("dsp", "AX ucode: unhandled mail %08X", mail); return;
        }
    }
}

/* ---- AI DMA: AX's mixed output, read at 32 kHz. */
static uint64_t s_ai_next, s_ai_period;
void (*g_dsp_ai_buffer)(u32 addr, u32 bytes); /* audio output (comes with the audio step) */

static void ai_update(void) {
    u32 blocks = R(0x36) & 0x7FFF;
    if ((R(0x36) & 0x8000) && blocks) {
        uint64_t period = (uint64_t)blocks * 32 / 4 * TB_HZ / 32000;
        if (!s_ai_period || period != s_ai_period) { s_ai_period = period; s_ai_next = wp_host_time() + period; }
    } else {
        s_ai_period = 0;
    }
}

void hw_dsp_tick(uint64_t now) {
    if (s_ai_period && (int64_t)(now - s_ai_next) >= 0) {
        u32 addr = (u32)R(0x30) << 16 | R(0x32), bytes = (u32)(R(0x36) & 0x7FFF) * 32;
        s_ai_next += s_ai_period;
        if ((int64_t)(now - s_ai_next) > (int64_t)(TB_HZ / 4)) s_ai_next = now + s_ai_period;
        if (g_dsp_ai_buffer) g_dsp_ai_buffer(addr, bytes);
        s_csr |= CSR_AIDINT;
        update_pi();
    }
}

static u32 dsp_read(u32 a, int size) {
    u32 off = a & 0x7Eu;
    if (size == 4) return dsp_read(a, 2) << 16 | dsp_read(a + 2, 2);
    switch (off) {
    case 0x00: return s_to_dsp_hi & 0x7FFF; /* CPU mail is taken at once */
    case 0x02: return 0;
    case 0x04:
        if (s_state == ST_ROM && s_oh == s_ot) push_mail(0x8071FEEDu, 0); /* the boot ROM's greeting */
        return s_out_visible ? 0x8000u | (s_out[s_oh % MAILQ].mail >> 16) : 0;
    case 0x06:
        if (s_out_visible) {
            u32 m = s_out[s_oh % MAILQ].mail;
            s_oh++;
            s_out_visible = 0;
            show_next_mail();
            return m & 0xFFFF;
        }
        return 0;
    case 0x0A: return s_csr;
    case 0x3A: return R(0x36) & 0x7FFF; /* AI DMA blocks left: report a full buffer */
    default: return R(off);
    }
}

static void dsp_write(u32 a, u32 v, int size) {
    u32 off = a & 0x7Eu;
    if (size == 4) { dsp_write(a, v >> 16, 2); dsp_write(a + 2, v & 0xFFFF, 2); return; }
    switch (off) {
    case 0x00: s_to_dsp_hi = (u16)v; break;
    case 0x02: mail_from_cpu((u32)s_to_dsp_hi << 16 | (v & 0xFFFF)); break;
    case 0x0A: {
        u16 ack = (u16)(v & (CSR_AIDINT | CSR_ARINT | CSR_DSPINT));
        u16 was_halted = s_csr & CSR_HALT;
        s_csr = (u16)((s_csr & (CSR_AIDINT | CSR_ARINT | CSR_DSPINT) & ~ack) |
                      (v & ~(CSR_AIDINT | CSR_ARINT | CSR_DSPINT | CSR_RES | CSR_DMA)));
        if (v & CSR_RES) reset_dsp();
        if (was_halted && !(s_csr & CSR_HALT)) WP_LOG("dsp", "DSP running");
        update_pi();
        break;
    }
    case 0x2A: /* ARAM DMA count low: the transfer happens at once */
        R(off) = (u16)v;
        s_csr |= CSR_ARINT;
        update_pi();
        break;
    case 0x36: R(off) = (u16)v; ai_update(); break;
    default: R(off) = (u16)v; break;
    }
}

void hw_dsp_init(void) { wp_mmio_register(0x0C005000u, 0x80u, dsp_read, dsp_write); }
