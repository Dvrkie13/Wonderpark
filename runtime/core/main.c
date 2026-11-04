/* Wonderpark core: loads the player's main.dol and runs DolRecomp's
 * generated code from a dispatcher loop.
 *
 * This is the Phase 2 step-2 skeleton: enough to compile, link and start
 * executing. Hardware (GPU, DSP/AI, DVD, IOS, Wii Remote) is not wired up
 * yet; MMIO accesses are logged and ignored, so the game is expected to stop
 * early. The reference runtime's HLE and host layers get ported onto this in
 * later steps.
 *
 * No game data is compiled in: main.dol is read from the extracted disc at
 * startup.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "S2LE01.h" /* generated: CPUState, dispatch helpers */

#define GAME_ID "S2LE01"
#define DEFAULT_DOL "game/sys/main.dol"

/* ---------------------------------------------------------------- memory */

static void st32(CPUState* c, uint32_t a, uint32_t v) { mem_write32(c, a, v); }
static void st16(CPUState* c, uint32_t a, uint16_t v) { mem_write16(c, a, v); }

static uint32_t be32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static int load_dol(CPUState* c, const char* path, uint32_t* entry) {
    FILE* f = fopen(path, "rb");
    uint8_t* d;
    long len;
    int i;
    if (!f) { fprintf(stderr, "wonderpark: can't open %s\n", path); return 0; }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0x100 || !(d = (uint8_t*)malloc((size_t)len))) { fclose(f); return 0; }
    if (fread(d, 1, (size_t)len, f) != (size_t)len) { fclose(f); free(d); return 0; }
    fclose(f);

    for (i = 0; i < 18; i++) {
        uint32_t off = be32(d + i * 4), addr = be32(d + 0x48 + i * 4), size = be32(d + 0x90 + i * 4);
        if (!size) continue;
        if (off + (uint64_t)size > (uint64_t)len || (addr & 0x3FFFFFFFu) + (uint64_t)size > c->ram_size) {
            fprintf(stderr, "wonderpark: bad DOL section %d\n", i);
            free(d);
            return 0;
        }
        memcpy(c->ram + (addr & 0x3FFFFFFFu), d + off, size);
    }
    {
        uint32_t bss = be32(d + 0xD8), bss_size = be32(d + 0xDC);
        if ((bss & 0x3FFFFFFFu) + (uint64_t)bss_size <= c->ram_size)
            memset(c->ram + (bss & 0x3FFFFFFFu), 0, bss_size);
    }
    *entry = be32(d + 0xE0);
    free(d);
    return 1;
}

/* What the system menu leaves in low memory before jumping to the game.
 * Values follow the reference runtime (MildRain479/PokePark-Wiicompiled,
 * recomp/boot/boot.c, CC0), which follows Dolphin's HLE boot. */
static void setup_low_memory(CPUState* c) {
    memcpy(c->ram, GAME_ID, 6);
    st32(c, 0x80000018, 0x5D1C9EA3); /* Wii disc magic */
    st32(c, 0x80000020, 0x0D15EA5E); /* boot magic */
    st32(c, 0x80000024, 0x00000001);
    st32(c, 0x80000028, 0x01800000); /* MEM1 size */
    st32(c, 0x8000002C, 0x00000023); /* console type: retail Wii */
    st32(c, 0x80000030, 0x00000000); /* arena lo: set by the OS */
    st32(c, 0x80000034, 0x817FEC60); /* arena hi */
    st32(c, 0x800000CC, 0x00000000); /* video mode: NTSC */
    st32(c, 0x800000F0, 0x01800000); /* simulated memory size */
    st32(c, 0x800000F8, 0x0E7BE2C0); /* bus clock, 243 MHz */
    st32(c, 0x800000FC, 0x2B73A840); /* CPU clock, 729 MHz */
    st32(c, 0x800030D8, 0x00000000); /* system time */
    st16(c, 0x800030E6, 0x0000);     /* retail console */
    st32(c, 0x80003100, 0x01800000); /* MEM1 physical size */
    st32(c, 0x80003104, 0x01800000); /* MEM1 simulated size */
    st32(c, 0x80003108, 0x817FEC60); /* end of usable MEM1 */
    st32(c, 0x8000310C, 0x00000000);
    st32(c, 0x80003110, 0x817FEC60); /* MEM1 arena end */
    st32(c, 0x80003118, 0x04000000); /* MEM2 physical size */
    st32(c, 0x8000311C, 0x04000000); /* MEM2 simulated size */
    st32(c, 0x80003120, 0x94000000); /* end of MEM2 */
    st32(c, 0x80003124, 0x90000800); /* usable MEM2 start */
    st32(c, 0x80003128, 0x93FE0000); /* usable MEM2 end */
    st32(c, 0x80003130, 0x93FE0000); /* IOS IPC buffer start */
    st32(c, 0x80003134, 0x94000000); /* IOS IPC buffer end */
    st32(c, 0x80003138, 0x00000011); /* Hollywood revision */
    st32(c, 0x80003140, 0x00090204); /* IOS version: IOS9 v516 */
    st32(c, 0x80003144, 0x00062507); /* IOS build date */
    st32(c, 0x80003158, 0x0000FF16); /* DDR vendor */
    st32(c, 0x80003188, 0x00090204); /* IOS version the apploader saw */
}

/* ---------------------------------------------------------------- MMIO */

static unsigned long long g_mmio_reads, g_mmio_writes;
static int g_mmio_verbose;

static void mmio_note(const char* what, uint32_t ea, uint64_t v, uint8_t size) {
    if (g_mmio_verbose)
        fprintf(stderr, "mmio %s%u %08X = %llX\n", what, size * 8u, ea, (unsigned long long)v);
}

static u64 mmio_read(CPUState* c, u32 ea, u8 size) {
    (void)c;
    g_mmio_reads++;
    mmio_note("rd", ea, 0, size);
    return 0;
}

static void mmio_write(CPUState* c, u32 ea, u64 v, u8 size) {
    (void)c;
    g_mmio_writes++;
    mmio_note("wr", ea, v, size);
}

/* ---------------------------------------------------------------- system instructions
 *
 * DolRecomp leaves supervisor SPR moves and cache maintenance to the host:
 * the generated code calls the fallback and returns to the dispatcher, so
 * the handler must emulate the instruction and move pc past it. */

static u32 g_spr[1024]; /* SPRs with no dedicated CPUState field (BATs, HID0/1, SPRGs, DEC, PMCs, ...) */

static u32 spr_read(CPUState* c, u16 spr, u32 cia) {
    (void)c; (void)cia;
    if (spr == 287) return 0x00087102u; /* PVR: Broadway */
    return g_spr[spr & 1023];
}

static void spr_write(CPUState* c, u16 spr, u32 value, u32 cia) {
    (void)c; (void)cia;
    g_spr[spr & 1023] = value;
}

/* SPRs that live in CPUState itself (DolRecomp's ppc_mtspr/ppc_mfspr own them). */
static int spr_has_field(u16 spr) {
    switch (spr) {
    case 1: case 8: case 9: case 18: case 19: case 26: case 27: case 282: case 284: case 285:
    case 912: case 913: case 914: case 915: case 916: case 917: case 918: case 919: case 920:
        return 1;
    default:
        return 0;
    }
}

static void system_instruction(CPUState* c, u32 raw, u32 cia) {
    u32 op = raw >> 26, xo = (raw >> 1) & 0x3FF, rs = (raw >> 21) & 31;
    u16 spr = (u16)(((raw >> 16) & 0x1F) | ((raw >> 6) & 0x3E0));
    u32 before = c->exception;
    if (op == 31) {
        switch (xo) {
        case 467: /* mtspr */
            if (spr_has_field(spr)) ppc_mtspr(c, spr, c->gpr[rs], cia);
            else spr_write(c, spr, c->gpr[rs], cia);
            break;
        case 339: /* mfspr */
            c->gpr[rs] = spr_has_field(spr) ? ppc_mfspr(c, spr, cia) : spr_read(c, spr, cia);
            break;
        case 86: case 54: case 470: case 278: case 246: case 982: break; /* dcbf dcbst dcbi dcbt dcbtst icbi: no caches to manage */
        default: goto unsupported;
        }
        if (c->exception == before) c->pc = cia + 4;
        return;
    }
unsupported:
    fprintf(stderr, "wonderpark: unsupported instruction %08X at %08X\n", raw, cia);
    ppc_program_exception(c, PPC_PROGRAM_ILLEGAL, cia);
}

/* ---------------------------------------------------------------- run */

#define SLICE_CYCLES 20000

int main(int argc, char** argv) {
    static CPUState cpu;
    const char* dol = argc > 1 ? argv[1] : DEFAULT_DOL;
    unsigned long long slices = 0, max_slices = 0;
    uint32_t entry = 0;
    const char* env;

    if ((env = getenv("WONDERPARK_MAX_SLICES")) != NULL) max_slices = strtoull(env, NULL, 10);
    g_mmio_verbose = getenv("WONDERPARK_MMIO_LOG") != NULL;

    if (!cpu_init(&cpu) || !cpu_alloc_mem2(&cpu, WII_MEM2_SIZE)) return 1;
    cpu.external_read = mmio_read;
    cpu.external_write = mmio_write;
    cpu.instruction_fallback = system_instruction;
    cpu.spr_read = spr_read;
    cpu.spr_write = spr_write;

    if (!load_dol(&cpu, dol, &entry)) return 1;
    setup_low_memory(&cpu);

    cpu.pc = entry;
    cpu.msr = 0x00002032u;  /* FP, IR, DR, RI */
    cpu.hid2 = 0xE0000000u; /* paired singles, locked cache, write-gather pipe */

    fprintf(stderr, "wonderpark: %s loaded, entry %08X\n", dol, entry);

    for (;;) {
        cpu.downcount = SLICE_CYCLES;
        if (!dolrecomp_call(&cpu, cpu.pc)) {
            fprintf(stderr, "wonderpark: no recompiled code at %08X (lr %08X)\n", cpu.pc, cpu.lr);
            break;
        }
        /* Time base: 60.75 MHz, one tick per 12 CPU cycles (729 MHz). Driven by
         * the guest cycles each slice used; host-time pacing comes with the
         * interrupt/timer work in the boot-to-title step. */
        cpu.timebase += (u64)((SLICE_CYCLES - cpu.downcount) / 12 + 1);
        if (cpu.exception) {
            fprintf(stderr, "wonderpark: exception %08X (program %08X) at %08X, srr0 %08X lr %08X\n",
                    cpu.exception, cpu.program_exception, cpu.pc, cpu.srr0, cpu.lr);
            break;
        }
        if (max_slices && ++slices >= max_slices) {
            fprintf(stderr, "wonderpark: stopped after %llu slices at %08X\n", slices, cpu.pc);
            break;
        }
    }
    fprintf(stderr, "wonderpark: %llu MMIO reads, %llu MMIO writes\n", g_mmio_reads, g_mmio_writes);
    cpu_free(&cpu);
    return 2; /* not expected to get further than this yet */
}
