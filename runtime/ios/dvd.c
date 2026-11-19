/* Ported from MildRain479/PokePark-Wiicompiled recomp/patches/hle_dvd.c (CC0). */
/* The disc, served from files extracted from the user's own copy (see
 * recomp/README.md). The game's DVD library is left as is: it reads the
 * data partition through /dev/di (hle_ios.c), and those reads are mapped
 * back to files through the disc's file table (FST). On Android the disc
 * can instead be read straight from the user's disc image (disc_image.c),
 * whose data partition is read at the same offsets. */
#include <stdlib.h>
#include <string.h>

#include "ref_compat.h"
#include "host_settings.h"
#include "host_mods.h"

#ifndef _WIN32
#define _strdup strdup
#define _fseeki64 fseeko
#define _ftelli64 ftello
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

typedef struct Region {
    uint64_t offset, size; /* in the data partition */
    char* path;            /* host file */
    uint8_t* volatile data; /* the whole file in RAM, once preloaded (fast loading) */
} Region;

static char sRoot[512];

/* The extracted disc's folder (game/), for reading the game's own menu art
 * at run time (the Options page's PC rows, settings_ui.cpp). */
const char* dvd_root(void) { return sRoot; }
static Region* sRegions;
static int sRegionCount, sRegionCap;
static uint8_t sBootBin[0x440];
static uint8_t* sFst;
static uint32_t sFstSize;
/* A disc image's partition, read at an offset, and its TMD. */
static int (*sImageRead)(uint64_t offset, void* out, uint32_t len);
static uint8_t* sTmd;
static uint32_t sTmdSize;

static uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void add_region(uint64_t offset, uint64_t size, const char* path) {
    if (sRegionCount == sRegionCap) {
        sRegionCap = sRegionCap ? sRegionCap * 2 : 512;
        sRegions = (Region*)realloc(sRegions, sRegionCap * sizeof(Region));
    }
    sRegions[sRegionCount].offset = offset;
    sRegions[sRegionCount].size = size;
    sRegions[sRegionCount].path = _strdup(path);
    sRegions[sRegionCount].data = NULL;
    sRegionCount++;
}

static uint8_t* read_file(const char* path, uint32_t* size) {
    FILE* f = fopen(path, "rb");
    uint8_t* data;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = (uint8_t*)malloc(n ? n : 1);
    if (fread(data, 1, n, f) != (size_t)n) {
        free(data);
        data = NULL;
    }
    fclose(f);
    if (size) *size = (uint32_t)n;
    return data;
}

static void put_be32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
}

/* Voice language (Audio; [audio] voice_language 1, Japanese): the Pokémon
 * voices from the player's own Japanese disc, copied out of it once by
 * tools/extract_voices.py (the launcher runs it when that disc is dropped
 * on it) into <game>/voices/ja/, the same paths as the disc's. Such a file
 * takes the place of the US one, as a mod's does (a different size: the
 * FST is changed), so from the next start. Only banks whose cue names and
 * numbers match the US ones are copied (docs/handoff/VOICE-LANGUAGE.md);
 * the other Pokémon's shared voice bank stays English. */
static int sVoicesReady;
/* Whether the Japanese voices are set up (looked at once, as the disc
 * opens: never a disk access while the F1 tab draws). */
int dvd_voices_ready(void) { return sVoicesReady; }

static int voice_file(const char* rel, char* out, size_t n, uint64_t* size) {
    FILE* f;
    if (g_x[X_VOICE_LANGUAGE] != 1 || strncmp(rel, "Sound/", 6) != 0) return 0;
    snprintf(out, n, "%s/voices/ja/%s", sRoot, rel);
    if (!(f = fopen(out, "rb"))) return 0;
    _fseeki64(f, 0, SEEK_END);
    *size = (uint64_t)_ftelli64(f);
    fclose(f);
    WP_LOG("mods", "disc file %s: Japanese voices", rel);
    return 1;
}

/* Walks the FST, adding a region for every file. Wii FSTs store file
 * offsets divided by 4. A file an enabled mod replaces (host_mods.h) is
 * read from the mod instead: the FST (which the game reads from memory) gets
 * its size, and if it grew, a new place past the end of the disc's data. */
static void add_fst_files(void) {
    uint32_t count = be32(sFst + 8);
    const char* names = (const char*)sFst + count * 12;
    uint32_t dir_end[64];
    char dir_path[64][256];
    int depth = 0, replaced = 0;
    uint32_t i;
    uint64_t next_free = 0;
    for (i = 1; i < count; i++) {
        const uint8_t* e = sFst + i * 12;
        uint64_t end = ((uint64_t)be32(e + 4) << 2) + be32(e + 8);
        if (!e[0] && end > next_free) next_free = end;
    }
    next_free = (next_free + 0x7FFF) & ~0x7FFFull;
    strcpy(dir_path[0], "files");
    dir_end[0] = count;
    for (i = 1; i < count; i++) {
        const uint8_t* e = sFst + i * 12;
        const char* name = names + (be32(e) & 0xFFFFFF);
        char path[512];
        while (depth > 0 && i >= dir_end[depth]) depth--;
        if (e[0]) {
            if (depth + 1 < 64) {
                depth++;
                snprintf(dir_path[depth], sizeof(dir_path[depth]), "%s/%s", dir_path[depth - 1], name);
                dir_end[depth] = be32(e + 8);
            }
        } else {
            char rel[512], mod[600];
            uint64_t size;
            snprintf(rel, sizeof(rel), "%s/%s", dir_path[depth] + (depth ? 6 : 5), name); /* past "files/" */
            if ((wp_mod_file(depth ? rel : name, mod, sizeof(mod), &size) || voice_file(depth ? rel : name, mod, sizeof(mod), &size)) &&
                size < 0xFFFFFFFFull) {
                uint8_t* w = sFst + i * 12;
                uint64_t at = (uint64_t)be32(w + 4) << 2;
                if (size > be32(w + 8)) {
                    at = next_free;
                    next_free = (next_free + size + 0x7FFF) & ~0x7FFFull;
                    put_be32(w + 4, (uint32_t)(at >> 2));
                }
                put_be32(w + 8, (uint32_t)size);
                add_region(at, size, mod);
                replaced++;
                WP_LOG("mods", "disc file %s from %s", depth ? rel : name, mod);
                continue;
            }
            snprintf(path, sizeof(path), "%s/%s/%s", sRoot, dir_path[depth], name);
            add_region((uint64_t)be32(e + 4) << 2, be32(e + 8), path);
        }
    }
    if (replaced) fprintf(stderr, "disc: %d file%s from mods or the Japanese voices\n", replaced, replaced == 1 ? "" : "s");
}

int hle_dvd_init(const char* root) {
    uint8_t* boot;
    char path[600];
    uint32_t size;
    strncpy(sRoot, root, sizeof(sRoot) - 1);
    snprintf(path, sizeof(path), "%s/sys/boot.bin", sRoot);
    boot = read_file(path, &size);
    if (!boot || size < sizeof(sBootBin)) {
        HLE_WARN("no disc at %s (extract it with dtk; see recomp/README.md)", sRoot);
        return -1;
    }
    memcpy(sBootBin, boot, sizeof(sBootBin));
    free(boot);
    snprintf(path, sizeof(path), "%s/sys/fst.bin", sRoot);
    sFst = read_file(path, &sFstSize);
    if (!sFst) return -1;
    /* The system area, then every file. */
    snprintf(path, sizeof(path), "%s/sys/boot.bin", sRoot);
    add_region(0, 0x440, path);
    snprintf(path, sizeof(path), "%s/sys/bi2.bin", sRoot);
    add_region(0x440, 0x2000, path);
    snprintf(path, sizeof(path), "%s/sys/apploader.img", sRoot);
    add_region(0x2440, 0x3B000, path);
    snprintf(path, sizeof(path), "%s/sys/fst.bin", sRoot);
    add_region((uint64_t)be32(sBootBin + 0x424) << 2, sFstSize, path);
    {
        FILE* v;
        snprintf(path, sizeof(path), "%s/voices/ja/voices.txt", sRoot);
        if ((v = fopen(path, "rb")) != NULL) sVoicesReady = 1, fclose(v);
    }
    add_fst_files();
    HLE_LOG("disc: %d files from %s", sRegionCount, sRoot);
    return 0;
}

int hle_dvd_init_image(const uint8_t* boot, const uint8_t* fst, uint32_t fstSize, const uint8_t* tmd,
                       uint32_t tmdSize, int (*read)(uint64_t offset, void* out, uint32_t len)) {
    memcpy(sBootBin, boot, sizeof(sBootBin));
    sFst = (uint8_t*)malloc(fstSize);
    memcpy(sFst, fst, fstSize);
    sFstSize = fstSize;
    if (tmd && tmdSize) {
        sTmd = (uint8_t*)malloc(tmdSize);
        memcpy(sTmd, tmd, tmdSize);
        sTmdSize = tmdSize;
    }
    sImageRead = read;
    HLE_LOG("disc: from a disc image, %u files", be32(sFst + 8));
    return 0;
}

/* What the apploader leaves in memory: the disc header at 0x80000000 and
 * the FST at the top of MEM1, just below the arena. */
void hle_dvd_boot_setup(void) {
    uint32_t arena_hi = LD32(0x80000034);
    uint32_t fst_addr = (arena_hi - sFstSize) & ~31u;
    if (!sFst) return;
    memcpy(MA(0x80000000), sBootBin, 0x20);
    memcpy(MA(fst_addr), sFst, sFstSize);
    ST32(0x80000034, fst_addr); /* arena hi */
    ST32(0x80000038, fst_addr); /* FST */
    ST32(0x8000003C, sFstSize); /* max FST size */
    ST32(0x80003110, fst_addr); /* MEM1 arena end */
}

/* Movies (Movie/*.mo) are streamed from disc while they play: the last one
 * read, and when (host time), tell whether one is playing. For the debug
 * overlay (and skip-movies, not built yet); WONDERPARK_LOG=movie logs each one's start. */
static char sMovie[64];
static uint64_t sMovieAt;

static void note_movie(const char* path) {
    const char* m = strstr(path, "/Movie/");
    if (!m) return;
    m += 7;
    if (strcmp(sMovie, m) != 0 || wp_host_time() - sMovieAt > 5 * TB_HZ) {
        snprintf(sMovie, sizeof(sMovie), "%s", m);
        WP_LOG("movie", "%.1f s: %s starts", (double)wp_host_time() / TB_HZ, sMovie);
    }
    sMovieAt = wp_host_time();
}

/* The movie being played (read in the last 5 s; it streams in bursts), or NULL. */
const char* wp_movie_playing(void) {
    return sMovie[0] && wp_host_time() - sMovieAt < 5 * TB_HZ ? sMovie : NULL;
}

/* Disc reads in the last second, their time, and how many came from RAM
 * (RECOMP_FPS's stats line). */
unsigned g_cnt_disc_reads, g_cnt_disc_ram;
double g_disc_ms, g_disc_max_ms;

static int read_regions(uint64_t offset, uint32_t len, uint32_t dst);

#ifdef _WIN32
/* ---- Reading the extracted files
 *
 * The game's reads (on the IOS thread) and the preloader's go through
 * these. WONDERPARK_DISC_UNCACHED=1 (tests) reads past Windows' file cache,
 * every read from the drive, as on a cold start: for timing loads on a hard
 * drive without having to restart the PC. */
static int uncached(void) {
    static int v = -1;
    if (v < 0) v = getenv("WONDERPARK_DISC_UNCACHED") != NULL;
    return v;
}

static HANDLE open_file(const char* path) {
    return CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                       uncached() ? FILE_FLAG_NO_BUFFERING : FILE_FLAG_SEQUENTIAL_SCAN, NULL);
}

/* len bytes at pos of an open file into out; 1 if all were read. */
static int file_read(HANDLE h, uint64_t pos, uint8_t* out, uint32_t len) {
    DWORD got;
    if (!uncached()) {
        LARGE_INTEGER p;
        p.QuadPart = (LONGLONG)pos;
        while (len) {
            DWORD n = len > (1u << 30) ? (1u << 30) : len;
            if (!SetFilePointerEx(h, p, NULL, FILE_BEGIN) || !ReadFile(h, out, n, &got, NULL) || got == 0) return 0;
            out += got, len -= got, p.QuadPart += got;
        }
        return 1;
    }
    {
        /* Unbuffered: whole aligned sectors into an aligned buffer (one a
         * thread: the IOS thread's and the preloader's). */
        static __declspec(thread) uint8_t* buf;
        static __declspec(thread) uint32_t cap;
        uint64_t a = pos & ~4095ull, b = (pos + len + 4095) & ~4095ull;
        LARGE_INTEGER p;
        if (b - a > cap) {
            if (buf) VirtualFree(buf, 0, MEM_RELEASE);
            cap = (uint32_t)(b - a);
            buf = (uint8_t*)VirtualAlloc(NULL, cap, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        }
        p.QuadPart = (LONGLONG)a;
        if (!SetFilePointerEx(h, p, NULL, FILE_BEGIN) || !ReadFile(h, buf, (DWORD)(b - a), &got, NULL) ||
            got < pos + len - a)
            return 0;
        memcpy(out, buf + (pos - a), len);
        return 1;
    }
}

/* The IOS thread's open files, so a file streamed in small reads (music,
 * movies) isn't opened again for each. */
#define OPEN_FILES 8
static struct { int region; HANDLE h; unsigned used; } sOpen[OPEN_FILES];
static unsigned sOpenClock;

static HANDLE region_handle(int i) {
    int k, old = 0;
    for (k = 0; k < OPEN_FILES; k++) {
        if (sOpen[k].h && sOpen[k].region == i) {
            sOpen[k].used = ++sOpenClock;
            return sOpen[k].h;
        }
        if (!sOpen[k].h || sOpen[k].used < sOpen[old].used) old = k;
    }
    if (sOpen[old].h) CloseHandle(sOpen[old].h);
    sOpen[old].h = open_file(sRegions[i].path);
    if (sOpen[old].h == INVALID_HANDLE_VALUE) sOpen[old].h = NULL;
    sOpen[old].region = i;
    sOpen[old].used = ++sOpenClock;
    return sOpen[old].h;
}

/* ---- Fast loading (the Fast loading setting)
 *
 * PokePark 2 reads each area's archives as it enters it, and on a hard
 * drive (spinning, often busy) a cold read took 250 ms or more. With fast
 * loading on, a thread reads the disc's files into RAM at start: all but
 * the movies and the streamed music and voices (Movie/, *.awb), 949 files,
 * 284 MB. First what every scene needs (Archive/, Common/, Message/, the
 * sound banks and so on), then the areas' files (Field/, Pokemon/,
 * Gimmick/, Attraction/), each group in disc order, so the drive mostly
 * reads straight through. The game's own reads come first: the preloader
 * waits while one is going on, reading 1 MB at a time, and a file not in
 * RAM yet is read from the drive as before. Once a file is in RAM, its
 * reads are copies from memory. */
static volatile LONG sGameReading;

static int preload_group(const char* path) {
    static const char* const kAreas[] = {"/files/Field/", "/files/Pokemon/", "/files/Gimmick/", "/files/Attraction/"};
    size_t k;
    if (strstr(path, "/files/Movie/")) return -1;
    if (strlen(path) > 4 && strcmp(path + strlen(path) - 4, ".awb") == 0) return -1;
    for (k = 0; k < sizeof(kAreas) / sizeof(kAreas[0]); k++)
        if (strstr(path, kAreas[k])) return 1;
    return 0;
}

static int preload_order(const void* a, const void* b) {
    const Region *x = &sRegions[*(const int*)a], *y = &sRegions[*(const int*)b];
    int gx = preload_group(x->path), gy = preload_group(y->path);
    if (gx != gy) return gx - gy;
    return x->offset < y->offset ? -1 : x->offset > y->offset;
}

static DWORD WINAPI preload_thread(LPVOID arg) {
    int* order = (int*)malloc((size_t)sRegionCount * sizeof(int));
    int n = 0, i;
    unsigned files = 0;
    uint64_t bytes = 0;
    double t0 = wp_wall_seconds();
    (void)arg;
    for (i = 0; i < sRegionCount; i++)
        if (sRegions[i].size && preload_group(sRegions[i].path) >= 0) order[n++] = i;
    qsort(order, (size_t)n, sizeof(int), preload_order);
    for (i = 0; i < n; i++) {
        Region* r = &sRegions[order[i]];
        uint8_t* buf = (uint8_t*)malloc((size_t)r->size);
        HANDLE h = open_file(r->path);
        uint64_t at = 0;
        int ok = buf && h != INVALID_HANDLE_VALUE;
        while (ok && at < r->size) {
            uint32_t chunk = r->size - at > (1u << 20) ? (1u << 20) : (uint32_t)(r->size - at);
            while (sGameReading) Sleep(1); /* the game's reads first */
            ok = file_read(h, at, buf + at, chunk);
            at += chunk;
        }
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        if (!ok) {
            free(buf);
            continue;
        }
        InterlockedExchangePointer((void* volatile*)&r->data, buf);
        files++;
        bytes += r->size;
        if (i + 1 < n && preload_group(r->path) != preload_group(sRegions[order[i + 1]].path))
            fprintf(stderr, "disc: fast loading: what every scene needs in RAM after %.1f s\n", wp_wall_seconds() - t0);
    }
    fprintf(stderr, "disc: fast loading: %u files, %.0f MB in RAM after %.1f s\n", files, bytes / 1048576.0,
            wp_wall_seconds() - t0);
    free(order);
    return 0;
}

/* Starts fast loading (main.c, once the settings are read). */
void hle_dvd_preload_start(void) {
    HANDLE t;
    if (sImageRead || !sRegionCount || !g_x[X_FAST_LOADING]) return;
    t = CreateThread(NULL, 0, preload_thread, NULL, 0, NULL);
    if (!t) return;
    SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL);
    CloseHandle(t);
}
#else
void hle_dvd_preload_start(void) {}
#endif

/* Copies len bytes at a data-partition offset to guest memory. */
int hle_dvd_read(uint64_t offset, uint32_t len, uint32_t dst) {
    double t0 = wp_wall_seconds(), ms;
    int ok;
#ifdef _WIN32
    InterlockedIncrement(&sGameReading);
#endif
    ok = read_regions(offset, len, dst);
#ifdef _WIN32
    InterlockedDecrement(&sGameReading);
#endif
    ms = (wp_wall_seconds() - t0) * 1000.0;
    g_cnt_disc_reads++;
    g_disc_ms += ms;
    if (ms > g_disc_max_ms) g_disc_max_ms = ms;
    return ok;
}

static int read_regions(uint64_t offset, uint32_t len, uint32_t dst) {
    uint64_t end = offset + len;
    int i, ok = 1, ram = 1;
    if (sImageRead) {
        HLE_LOG("disc read %08llX+%X", (unsigned long long)offset, (unsigned)len);
        if (sImageRead(offset, MA(dst), len)) return 1;
        memset(MA(dst), 0, len);
        HLE_WARN("disc read %08llX+%X failed", (unsigned long long)offset, (unsigned)len);
        return 0;
    }
    memset(MA(dst), 0, len);
    for (i = 0; i < sRegionCount; i++) {
        const Region* r = &sRegions[i];
        uint64_t lo = offset > r->offset ? offset : r->offset;
        uint64_t hi = end < r->offset + r->size ? end : r->offset + r->size;
        uint8_t* data;
        if (lo >= hi) continue;
        note_movie(r->path);
        if (strstr(r->path, "_streamfiles.awb")) {
            extern void jukebox_note_read(const char* path, unsigned long long offset); /* host/jukebox.cpp: heard */
            jukebox_note_read(r->path, lo - r->offset);
        }
        if (wp_log_on("mods") && strstr(r->path, "/voices/ja/")) WP_LOG("mods", "read %s", r->path);
        if ((data = r->data) != NULL) {
            memcpy(MA(dst + (uint32_t)(lo - offset)), data + (lo - r->offset), (size_t)(hi - lo));
            continue;
        }
        ram = 0;
#ifdef _WIN32
        {
            HANDLE h = region_handle(i);
            if (!h) {
                HLE_WARN("disc read needs %s, which is missing", r->path);
                ok = 0;
                continue;
            }
            if (!file_read(h, lo - r->offset, MA(dst + (uint32_t)(lo - offset)), (uint32_t)(hi - lo))) ok = 0;
        }
#else
        {
            FILE* f = fopen(r->path, "rb");
            if (!f) {
                HLE_WARN("disc read needs %s, which is missing", r->path);
                ok = 0;
                continue;
            }
            _fseeki64(f, (int64_t)(lo - r->offset), SEEK_SET);
            if (fread(MA(dst + (uint32_t)(lo - offset)), 1, (size_t)(hi - lo), f) != hi - lo) ok = 0;
            fclose(f);
        }
#endif
        HLE_LOG("disc read %08llX+%X: %s", (unsigned long long)lo, (unsigned)(hi - lo),
                strncmp(r->path, sRoot, strlen(sRoot)) == 0 ? r->path + strlen(sRoot) + 1 : r->path); /* a mod's file: in full */
    }
    if (ram) g_cnt_disc_ram++;
    return ok;
}

/* The partition's TMD, which DVDLowOpenPartition returns. */
uint32_t hle_dvd_tmd(uint32_t dst, uint32_t max) {
    char path[600];
    uint32_t size;
    uint8_t* tmd;
    if (sTmd) {
        size = sTmdSize < max ? sTmdSize : max;
        memcpy(MA(dst), sTmd, size);
        return size;
    }
    snprintf(path, sizeof(path), "%s/tmd.bin", sRoot);
    tmd = read_file(path, &size);
    if (!tmd) return 0;
    if (size > max) size = max;
    memcpy(MA(dst), tmd, size);
    free(tmd);
    return size;
}

const uint8_t* hle_dvd_disc_id(void) { return sBootBin; }
