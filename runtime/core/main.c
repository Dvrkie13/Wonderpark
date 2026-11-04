/* Wonderpark: loads the player's main.dol and runs DolRecomp's generated
 * code from a dispatcher loop.
 *
 * No game data is compiled in: main.dol is read from the extracted disc at
 * startup.
 *
 * Environment (development):
 *   WONDERPARK_MAX_SLICES=N   stop after N dispatcher slices
 *   WONDERPARK_LOG=cats       log categories (mmio, exc, dec, pi, ipc, ..., all)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wp.h"

#define GAME_ID "S2LE01"
#define DEFAULT_DOL "game/sys/main.dol"

void wp_devices_init(void);
void wp_run(CPUState* c, unsigned long long max_slices);

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
        uint8_t* dst;
        if (!size) continue;
        if (off + (uint64_t)size > (uint64_t)len || !(dst = wp_mem_ptr(addr, size))) {
            fprintf(stderr, "wonderpark: bad DOL section %d\n", i);
            free(d);
            return 0;
        }
        memcpy(dst, d + off, size);
    }
    {
        uint32_t bss = be32(d + 0xD8), bss_size = be32(d + 0xDC);
        uint8_t* dst = wp_mem_ptr(bss, bss_size);
        if (dst) memset(dst, 0, bss_size);
    }
    *entry = be32(d + 0xE0);
    free(d);
    return 1;
}

/* What the system menu leaves in low memory before jumping to the game.
 * Values follow the reference runtime (MildRain479/PokePark-Wiicompiled,
 * recomp/boot/boot.c, CC0), which follows Dolphin's HLE boot. */
static void setup_low_memory(void) {
    memcpy(wp_mem_ptr(0x80000000u, 6), GAME_ID, 6);
    ST32(0x80000018, 0x5D1C9EA3); /* Wii disc magic */
    ST32(0x80000020, 0x0D15EA5E); /* boot magic */
    ST32(0x80000024, 0x00000001);
    ST32(0x80000028, 0x01800000); /* MEM1 size */
    ST32(0x8000002C, 0x00000023); /* console type: retail Wii */
    ST32(0x80000030, 0x00000000); /* arena lo: set by the OS */
    ST32(0x80000034, 0x817FEC60); /* arena hi */
    ST32(0x800000CC, 0x00000000); /* video mode: NTSC */
    ST32(0x800000F0, 0x01800000); /* simulated memory size */
    ST32(0x800000F8, 0x0E7BE2C0); /* bus clock, 243 MHz */
    ST32(0x800000FC, 0x2B73A840); /* CPU clock, 729 MHz */
    ST32(0x800030D8, 0x00000000); /* system time */
    ST16(0x800030E6, 0x0000);     /* retail console */
    ST32(0x80003100, 0x01800000); /* MEM1 physical size */
    ST32(0x80003104, 0x01800000); /* MEM1 simulated size */
    ST32(0x80003108, 0x817FEC60); /* end of usable MEM1 */
    ST32(0x8000310C, 0x00000000);
    ST32(0x80003110, 0x817FEC60); /* MEM1 arena end */
    ST32(0x80003118, 0x04000000); /* MEM2 physical size */
    ST32(0x8000311C, 0x04000000); /* MEM2 simulated size */
    ST32(0x80003120, 0x94000000); /* end of MEM2 */
    ST32(0x80003124, 0x90000800); /* usable MEM2 start */
    ST32(0x80003128, 0x93FE0000); /* usable MEM2 end */
    ST32(0x80003130, 0x93FE0000); /* IOS IPC buffer start */
    ST32(0x80003134, 0x94000000); /* IOS IPC buffer end */
    ST32(0x80003138, 0x00000011); /* Hollywood revision */
    ST32(0x80003140, 0x00090204); /* IOS version: IOS9 v516 */
    ST32(0x80003144, 0x00062507); /* IOS build date */
    ST32(0x80003158, 0x0000FF16); /* DDR vendor */
    ST32(0x80003188, 0x00090204); /* IOS version the apploader saw */
}

int main(int argc, char** argv) {
    static CPUState cpu;
    const char* dol = argc > 1 ? argv[1] : DEFAULT_DOL;
    unsigned long long max_slices = 0;
    uint32_t entry = 0;
    const char* env;

    if ((env = getenv("WONDERPARK_MAX_SLICES")) != NULL) max_slices = strtoull(env, NULL, 10);

    if (!cpu_init(&cpu) || !cpu_alloc_mem2(&cpu, WII_MEM2_SIZE)) return 1;
    g_cpu = &cpu;
    wp_cpu_host_init(&cpu);
    wp_mmio_init(&cpu);
    wp_devices_init();

    if (!load_dol(&cpu, dol, &entry)) return 1;
    setup_low_memory();

    cpu.pc = entry;
    cpu.msr = 0x00002032u;  /* FP, IR, DR, RI */
    cpu.hid2 = 0xE0000000u; /* paired singles, locked cache, write-gather pipe */

    fprintf(stderr, "wonderpark: %s loaded, entry %08X\n", dol, entry);

    wp_run(&cpu, max_slices);
    cpu_free(&cpu);
    return 2;
}
