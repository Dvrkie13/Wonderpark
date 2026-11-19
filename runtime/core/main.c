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

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "wp.h"

#define GAME_ID "S2LE01"
#define DEFAULT_DOL "game/sys/main.dol"

void wp_devices_init(void);
int hle_dvd_init(const char* root);
void hle_dvd_boot_setup(void);
void hle_nand_set_root(const char* root);
void hle_nand_set_tmp_root(const char* root);
void settings_load(void);
void gx_init(void);
void input_init(void);
void audio_init(void);
void wp_hostprof_start(void);
void wp_stallwatch_start(void);
void wp_logpipe_start(void);
void wp_logpipe_drain(void);
void wp_watch_init(void);
void wp_run(CPUState* c, unsigned long long max_slices);

/* MEM1 and MEM2, with write watch on Windows (the GPU's texture cache uses
 * it to see which memory changed). Same layout cpu_init/cpu_alloc_mem2 give. */
static int alloc_memory(CPUState* c) {
    memset(c, 0, sizeof(*c));
    c->ram_size = GC_MAIN_RAM_SIZE;
    c->mem2_size = WII_MEM2_SIZE;
#ifdef _WIN32
    c->ram = (u8*)VirtualAlloc(NULL, c->ram_size, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
    c->mem2 = (u8*)VirtualAlloc(NULL, c->mem2_size, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
#else
    c->ram = (u8*)calloc(1, c->ram_size);
    c->mem2 = (u8*)calloc(1, c->mem2_size);
#endif
    if (!c->ram || !c->mem2) { fprintf(stderr, "wonderpark: out of memory\n"); return 0; }
    return 1;
}

#ifdef _WIN32
/* A host crash (an access violation in generated or runtime code): say
 * where the guest was before Windows ends the process. */
static LONG WINAPI crash_filter(EXCEPTION_POINTERS* e) {
    fprintf(stderr, "wonderpark: host exception %08lX at %p (address %p)\n", e->ExceptionRecord->ExceptionCode,
            e->ExceptionRecord->ExceptionAddress,
            e->ExceptionRecord->NumberParameters > 1 ? (void*)e->ExceptionRecord->ExceptionInformation[1] : NULL);
    if (g_cpu) {
        fprintf(stderr, "  guest pc %08X lr %08X r1 %08X\n", g_cpu->pc, g_cpu->lr, g_cpu->gpr[1]);
        wp_backtrace(g_cpu);
    }
    wp_logpipe_drain();
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

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

    /* The BSS range covers the small-data sections too (.sdata/.sdata2 sit
     * between .sbss pieces), so clear it before the sections are copied. */
    {
        uint32_t bss = be32(d + 0xD8), bss_size = be32(d + 0xDC);
        uint8_t* dst = wp_mem_ptr(bss, bss_size);
        if (dst) memset(dst, 0, bss_size);
    }
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
    const char* dol = DEFAULT_DOL;
    unsigned long long max_slices = 0;
    int prepare_shaders = 0; /* --prepare-shaders */
    uint32_t entry = 0;
    const char* env;
    int i;

    wp_logpipe_start(); /* log lines never block the game (logpipe.c) */
#ifdef WP_PGO_GENERATE
    {
        /* An instrumented build (build_game.py --pgo-generate): the profile
         * runtime comes from a MinGW clang, whose own write-at-exit is
         * registered by MinGW's start-up code, never run here. */
        extern void __llvm_profile_initialize_file(void); /* reads LLVM_PROFILE_FILE */
        extern int __llvm_profile_write_file(void);
        __llvm_profile_initialize_file();
        atexit((void (*)(void))__llvm_profile_write_file);
    }
#endif
    /* Wonderpark [main.dol] [--headless] [--exit-after <seconds>] */
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--headless")) g_wp_headless = 1;
        else if (!strcmp(argv[i], "--prepare-shaders")) prepare_shaders = g_wp_headless = 1;
        else if (!strcmp(argv[i], "--exit-after") && i + 1 < argc) g_wp_exit_after = atof(argv[++i]);
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("Wonderpark [path/to/sys/main.dol] [--headless] [--exit-after <seconds>]\n"
                   "  --headless          no visible window, no sound device, no input (automated runs)\n"
                   "  --exit-after N      stop after N seconds and print a summary\n"
                   "  --prepare-shaders   compile and draw with the shaders once, then exit (the build's last step)\n"
                   "Settings: settings.ini in %%APPDATA%%\\Wonderpark; WONDERPARK_<KEY> overrides any of them.\n");
            return 0;
        } else if (argv[i][0] != '-') dol = argv[i];
    }
    if (prepare_shaders) {
        /* The build's "Preparing shaders" step (builder/pipeline.py): the
         * game's own boot, hidden and silent, at x10 for 8 s (through the
         * title). The graphics driver compiles each new shader and draw
         * state the first time it sees one (on AMD ~0.4 s each, three times
         * by the title) and keeps them in its own cache, so the player's
         * first start doesn't pause. A data folder of its own (in the cache
         * folder), no mods, no preloading: saves and settings untouched. */
        char data[1100];
        snprintf(data, sizeof(data), "%s/prepare", wp_cache_dir());
#ifdef _WIN32
#define WP_SETENV(k, v) _putenv_s(k, v)
#else
#define WP_SETENV(k, v) setenv(k, v, 1)
#endif
        WP_SETENV("WONDERPARK_DATA_DIR", data);
        WP_SETENV("WONDERPARK_GAME_SPEED", "10");
        WP_SETENV("WONDERPARK_NO_MODS", "1");
        WP_SETENV("WONDERPARK_FAST_LOADING", "0");
        if (g_wp_exit_after <= 0) g_wp_exit_after = 8;
    }
    if ((env = getenv("WONDERPARK_MAX_SLICES")) != NULL) max_slices = strtoull(env, NULL, 10);
    if (g_wp_headless) fprintf(stderr, "wonderpark: headless\n");

#ifdef _WIN32
    SetUnhandledExceptionFilter(crash_filter);
    {
        /* No power throttling ("efficiency mode"): Windows 11 runs a
         * process without a visible window in front at lower clocks and
         * ignores its timer requests, and the game then falls below 30 fps
         * (in the background, and every --headless run). */
        PROCESS_POWER_THROTTLING_STATE pt;
        memset(&pt, 0, sizeof(pt));
        pt.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        pt.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
        pt.StateMask = 0;
        SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof(pt));
    }
#endif
    if (!alloc_memory(&cpu)) return 1;
    g_cpu = &cpu;
    wp_cpu_host_init(&cpu);
    wp_mmio_init(&cpu);
    wp_devices_init();

    if (!load_dol(&cpu, dol, &entry)) return 1;
    setup_low_memory();
    {
        /* The disc's files next to the extracted main.dol (<game>/sys/main.dol
         * -> <game>); the emulated NAND, with the saves, in the data folder
         * (paths.c), its /tmp in the cache folder. */
        char root[512], nand[1100];
        char* p;
        snprintf(root, sizeof(root), "%s", dol);
        for (p = root; *p; p++) if (*p == '\\') *p = '/';
        if ((p = strstr(root, "/sys/main.dol")) != NULL) *p = '\0';
        {
            settings_load();
            /* The mods switched on in the launcher (host_mods.h): their
             * files take the place of the disc's, so before the disc opens. */
            extern void wp_mods_init(void);
            wp_mods_init();
            /* The Journal (hle_journal.c): read now, before the game runs;
             * written again at exit. */
            extern void hle_journal_init(void);
            extern void hle_journal_flush(void);
            hle_journal_init();
            atexit(hle_journal_flush);
        }
        if (hle_dvd_init(root) != 0) return 1;
        hle_dvd_boot_setup();
        snprintf(nand, sizeof(nand), "%s/nand", wp_data_dir());
        wp_make_dirs(nand);
        hle_nand_set_root(nand);
        snprintf(nand, sizeof(nand), "%s/nandtmp", wp_cache_dir());
        hle_nand_set_tmp_root(nand);
        fprintf(stderr, "wonderpark: saves and settings in %s\n", wp_data_dir());
    }

    cpu.pc = entry;
    cpu.msr = 0x00002032u;  /* FP, IR, DR, RI */
    cpu.hid2 = 0xE0000000u; /* paired singles, locked cache, write-gather pipe */

    fprintf(stderr, "wonderpark: %s loaded, entry %08X\n", dol, entry);

    /* (Settings are read before the disc opens: Voice language picks its files, dvd.c.) */
    {
        /* Fast loading: the disc's files into RAM in the background (dvd.c). */
        extern void hle_dvd_preload_start(void);
        hle_dvd_preload_start();
    }
    gx_init(); /* opens the window and the renderer (runtime/gpu) */
    input_init();
    if (!g_wp_headless) audio_init();

    wp_hostprof_start();
    wp_stallwatch_start();
    wp_watch_init();
    wp_run(&cpu, max_slices);
    return 2;
}
