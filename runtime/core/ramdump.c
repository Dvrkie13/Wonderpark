/* WONDERPARK_RAMDUMP="<frame>:<file>,..." (or "t<seconds>:<file>"): at the
 * game's <frame>-th finished frame, a copy of guest memory (MEM1 24 MB, then MEM2 64 MB) is written to
 * <file>, for finding where the game keeps a value (compare two dumps).
 * The copy is taken on the game thread (a memcpy) and written by a thread
 * of its own: the game never waits for the disk. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wp.h"

#define MEM1_SIZE 0x01800000u
#define MEM2_SIZE 0x04000000u

typedef struct { char path[512]; uint8_t* data; } Dump;

static DWORD WINAPI writer(void* arg) {
    Dump* d = (Dump*)arg;
    FILE* f = fopen(d->path, "wb");
    if (f) {
        fwrite(d->data, 1, MEM1_SIZE + MEM2_SIZE, f);
        fclose(f);
        fprintf(stderr, "ramdump: wrote %s\n", d->path);
    }
    free(d->data);
    free(d);
    return 0;
}

double wp_wall_seconds(void);

/* "t<seconds>:<file>" instead takes it at the first frame finished that
 * many seconds after start (lines up with WONDERPARK_PAD_SCRIPT's times). */
void wp_ramdump_frame(unsigned frame) {
    static int parsed;
    static struct { unsigned frame; double t; int done; char path[512]; } items[8];
    static int count;
    int i;
    if (!parsed) {
        const char* e = getenv("WONDERPARK_RAMDUMP");
        parsed = 1;
        while (e && *e && count < 8) {
            const char* c = strchr(e, ':');
            const char* end = strchr(e, ',');
            size_t n;
            if (!c) break;
            items[count].t = *e == 't' ? strtod(e + 1, NULL) : -1;
            items[count].frame = (unsigned)strtoul(*e == 'f' || *e == 't' ? e + 1 : e, NULL, 10);
            n = (size_t)((end ? end : e + strlen(e)) - (c + 1));
            if (n >= sizeof(items[count].path)) n = sizeof(items[count].path) - 1;
            memcpy(items[count].path, c + 1, n);
            items[count].path[n] = 0;
            count++;
            e = end ? end + 1 : NULL;
        }
    }
    for (i = 0; i < count; i++) {
        if (items[i].t >= 0 ? !items[i].done && wp_wall_seconds() >= items[i].t : items[i].frame == frame) {
            items[i].done = 1;
            Dump* d = (Dump*)malloc(sizeof(Dump));
            uint8_t* m1 = wp_mem_ptr(0x80000000u, MEM1_SIZE);
            uint8_t* m2 = wp_mem_ptr(0x90000000u, MEM2_SIZE);
            HANDLE t;
            if (!d || !m1 || !m2 || !(d->data = (uint8_t*)malloc(MEM1_SIZE + MEM2_SIZE))) {
                free(d);
                continue;
            }
            memcpy(d->data, m1, MEM1_SIZE);
            memcpy(d->data + MEM1_SIZE, m2, MEM2_SIZE);
            snprintf(d->path, sizeof(d->path), "%s", items[i].path);
            t = CreateThread(NULL, 0, writer, d, 0, NULL);
            if (t) CloseHandle(t);
        }
    }
}
