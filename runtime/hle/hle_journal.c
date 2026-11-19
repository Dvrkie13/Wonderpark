/* The F1 notebook's Journal (settings_ui.cpp): what the save says, and what
 * Wonderpark counts from now on, per save file, in journal.json in the data
 * folder (%APPDATA%\Wonderpark):
 *   - play time per day (game time: only while the game runs, not paused);
 *   - attractions played, each (the scene entered with its AttractionId,
 *     hle_scene.c);
 *   - milestones with their day: the first friend made (and who), each area
 *     first reached, 50 / 100 / 150 / 194 friends.
 * Friends and areas already in a save when Wonderpark first sees it are
 * taken as they are (no milestones for them): the Journal counts "since
 * Wonderpark was installed".
 *
 * From the loaded save in memory (the GD copy at the pointer at 0x804BA694,
 * as hle_balance.c and hle_friends.c read it): play time in seconds at
 * +0x1470 (the game's "Select a Saved Game" shows it as h:mm), berries at
 * +0x1474, area and zone at +0x14A6. Which file is loaded: the last GD<n>
 * the game opened (ios/nand.c, g_nand_last_slot), a save written being
 * certain.
 *
 * Not counted yet (shown "Coming soon"): jumps, Thunderbolts, photos,
 * distance, attraction scores and medals: their hooks aren't found yet.
 *
 * The file is read once at start (main.c, before the game runs) and written
 * on a thread of its own (never the game's), at most every 20 s and when
 * the game closes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#endif

#include "hle.h"
#include "hle_journal.h"

#define SAVE_PTR 0x804BA694u
#define SAVE_SIZE 5344u
#define PLAY_TIME 0x1470u
#define BERRIES 0x1474u
#define AREA 0x14A6u
#define POKEMON 198
#define FIRST 4 /* 0-3: the partners */
#define POINTS_ENTRY 0x80413100u
#define POINTS_AT 0x28u
#define FRIEND_POINTS 100

const char* wp_data_dir(void); /* paths.c */
int hle_scene(void);           /* hle_scene.c */
int hle_minigame(void);
int g_nand_last_slot = -1;     /* ios/nand.c: the last GD<n> opened (saved to: certain) */

static JournalSlot sSlots[JOURNAL_SLOTS];
static int sLoaded, sDirty;
static double sLastWrite, sLastTick, sLastScan;
static int sLastScene = -1;
static CRITICAL_SECTION sLock;

static void today(char out[11]) {
    time_t t = time(NULL);
    struct tm lt;
#ifdef _WIN32
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    strftime(out, 11, "%Y-%m-%d", &lt);
}

/* ---- journal.json: a fixed layout, written and read by this file only. */

static void path_of(char* out, size_t n) { snprintf(out, n, "%s/journal.json", wp_data_dir()); }

static char* serialize(void) {
    size_t cap = 1 << 16, len = 0;
    char* s = (char*)malloc(cap);
    int i, k;
#define PUT(...) do { int w_ = snprintf(s + len, cap - len, __VA_ARGS__); if (w_ > 0) len += (size_t)w_ < cap - len ? (size_t)w_ : cap - len - 1; } while (0)
    if (!s) return NULL;
    PUT("{\n  \"version\": 1,\n  \"slots\": [\n");
    for (i = 0; i < JOURNAL_SLOTS; i++) {
        JournalSlot* j = &sSlots[i];
        PUT("    {\"slot\": %d, \"seen\": %d,\n     \"days\": [", i, j->seen);
        for (k = 0; k < j->day_count; k++) PUT("%s[\"%s\", %d]", k ? ", " : "", j->days[k].day, j->days[k].seconds);
        PUT("],\n     \"attractions\": [%d, %d, %d, %d],\n     \"friends\": [", j->attractions[0], j->attractions[1], j->attractions[2],
            j->attractions[3]);
        for (k = 0; k < j->friend_count; k++) PUT("%s%d", k ? ", " : "", j->friends[k]);
        PUT("],\n     \"areas\": [");
        for (k = 0; k < j->area_count; k++) PUT("%s%d", k ? ", " : "", j->areas[k]);
        PUT("],\n     \"milestones\": [");
        for (k = 0; k < j->milestone_count; k++)
            PUT("%s[\"%s\", %d, %d]", k ? ", " : "", j->milestones[k].day, j->milestones[k].kind, j->milestones[k].value);
        PUT("]}%s\n", i + 1 < JOURNAL_SLOTS ? "," : "");
    }
    PUT("  ]\n}\n");
#undef PUT
    return s;
}

/* The reader: numbers and strings in order, by position in the layout above. */
static const char* skip_to(const char* p, const char* key) {
    const char* q = p ? strstr(p, key) : NULL;
    return q ? q + strlen(key) : NULL;
}
static const char* next_num(const char* p, int* v) {
    while (*p && !(*p == '-' || (*p >= '0' && *p <= '9'))) {
        if (*p == ']') return NULL;
        p++;
    }
    if (!*p) return NULL;
    *v = (int)strtol(p, (char**)&p, 10);
    return p;
}
static const char* next_str(const char* p, char out[11]) {
    const char* q;
    while (*p && *p != '"') {
        if (*p == ']') return NULL;
        p++;
    }
    if (!*p || !(q = strchr(p + 1, '"')) || q - p - 1 > 10) return NULL;
    memcpy(out, p + 1, (size_t)(q - p - 1));
    out[q - p - 1] = 0;
    return q + 1;
}
/* In a list of lists: past the next inner '[', or NULL at the list's end. */
static const char* next_pair(const char* p) {
    while (*p && *p != '[' && *p != ']') p++;
    return *p == '[' ? p + 1 : NULL;
}
static void parse(const char* s) {
    const char* p = s;
    int i;
    for (i = 0; i < JOURNAL_SLOTS && (p = skip_to(p, "\"slot\":")); i++) {
        JournalSlot* j = &sSlots[i];
        const char *q, *end = strstr(p, "]}");
        int v;
        if (!end) break;
        memset(j, 0, sizeof(*j));
        if ((q = skip_to(p, "\"seen\":")) && q < end) j->seen = atoi(q);
        if ((q = skip_to(p, "\"days\": [")) && q < end)
            while (j->day_count < JOURNAL_DAYS && (q = next_pair(q)) != NULL) {
                JournalDay* d = &j->days[j->day_count];
                const char* r = next_str(q, d->day);
                if (!r || !(r = next_num(r, &d->seconds))) break;
                j->day_count++;
                q = strchr(r, ']');
                if (!q) break;
                q++;
            }
        if ((q = skip_to(p, "\"attractions\": [")) && q < end)
            for (v = 0; v < 4 && q; v++) q = next_num(q, &j->attractions[v]);
        if ((q = skip_to(p, "\"friends\": [")) && q < end)
            while (j->friend_count < POKEMON && (q = next_num(q, &v))) j->friends[j->friend_count++] = (unsigned char)v;
        if ((q = skip_to(p, "\"areas\": [")) && q < end)
            while (j->area_count < JOURNAL_AREAS && (q = next_num(q, &v))) j->areas[j->area_count++] = (unsigned short)v;
        if ((q = skip_to(p, "\"milestones\": [")) && q < end)
            while (j->milestone_count < JOURNAL_MILESTONES && (q = next_pair(q)) != NULL) {
                JournalMilestone* m = &j->milestones[j->milestone_count];
                const char* r = next_str(q, m->day);
                if (!r || !(r = next_num(r, &m->kind)) || !(r = next_num(r, &m->value))) break;
                j->milestone_count++;
                q = strchr(r, ']');
                if (!q) break;
                q++;
            }
        p = end;
    }
}

void hle_journal_init(void) {
    char path[600];
    FILE* f;
    InitializeCriticalSection(&sLock);
    path_of(path, sizeof(path));
    if ((f = fopen(path, "rb")) != NULL) {
        long n;
        char* s;
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n > 0 && n < (8 << 20) && (s = (char*)malloc((size_t)n + 1)) != NULL) {
            if (fread(s, 1, (size_t)n, f) == (size_t)n) {
                s[n] = 0;
                parse(s);
            }
            free(s);
        }
        fclose(f);
    }
    sLoaded = 1;
}

/* Written on a thread of its own, from a copy. */
static unsigned __stdcall write_thread(void* arg) {
    char path[600], tmp[620];
    char* s = (char*)arg;
    FILE* f;
    path_of(path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    if ((f = fopen(tmp, "wb")) != NULL) {
        int ok = fwrite(s, 1, strlen(s), f) == strlen(s);
        if (fclose(f) == 0 && ok) MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING);
    }
    free(s);
    return 0;
}
static void write_now(int wait) {
    char* s;
    HANDLE h;
    EnterCriticalSection(&sLock);
    s = serialize();
    sDirty = 0;
    LeaveCriticalSection(&sLock);
    if (!s) return;
    h = (HANDLE)_beginthreadex(NULL, 0, write_thread, s, 0, NULL);
    if (h) {
        if (wait) WaitForSingleObject(h, 5000);
        CloseHandle(h);
    } else {
        free(s);
    }
}
void hle_journal_flush(void) {
    if (sLoaded && sDirty) write_now(1);
}

static void add_milestone(JournalSlot* j, int kind, int value) {
    JournalMilestone* m;
    if (j->milestone_count >= JOURNAL_MILESTONES) return;
    m = &j->milestones[j->milestone_count++];
    today(m->day);
    m->kind = kind;
    m->value = value;
    WP_LOG("journal", "milestone %d %d", kind, value);
}

static int has_friend(const JournalSlot* j, int p) {
    int k;
    for (k = 0; k < j->friend_count; k++)
        if (j->friends[k] == p) return 1;
    return 0;
}
static int has_area(const JournalSlot* j, int a) {
    int k;
    for (k = 0; k < j->area_count; k++)
        if (j->areas[k] == a) return 1;
    return 0;
}

/* Each game frame (SceneManager::RunBegin, hle_scene.c). */
void hle_journal_frame(void) {
    double now = wp_wall_seconds(), dt;
    u32 save = LD32(SAVE_PTR);
    int slot = g_nand_last_slot, scene = hle_scene();
    JournalSlot* j;
    if (!sLoaded) return;
    dt = sLastTick > 0 ? now - sLastTick : 0;
    sLastTick = now;
    if (dt > .25) dt = .25; /* a pause or a load: not play time */
    if (slot < 0 || slot >= JOURNAL_SLOTS || !wp_mem_ptr(save, SAVE_SIZE)) return;
    /* Only in the park, the lobby and the attractions (scenes 0, 5, 4). */
    if (scene != 0 && scene != 4 && scene != 5) {
        sLastScene = scene;
        return;
    }
    EnterCriticalSection(&sLock);
    j = &sSlots[slot];
    {
        /* Play time, today's. */
        static double carry;
        char day[11];
        int k;
        carry += dt;
        if (carry >= 1) {
            int whole = (int)carry;
            carry -= whole;
            today(day);
            for (k = 0; k < j->day_count && strcmp(j->days[k].day, day); k++) {}
            if (k == j->day_count) {
                if (j->day_count == JOURNAL_DAYS) {
                    memmove(j->days, j->days + 1, sizeof(j->days[0]) * (JOURNAL_DAYS - 1));
                    k = --j->day_count;
                }
                strcpy(j->days[k].day, day);
                j->days[k].seconds = 0;
                j->day_count++;
            }
            j->days[k].seconds += whole;
            sDirty = 1;
        }
    }
    if (scene == 4 && sLastScene != 4) {
        int a = hle_minigame();
        if (a >= 1 && a <= 4) j->attractions[a - 1]++, sDirty = 1;
    }
    sLastScene = scene;
    if (now - sLastScan >= 1) {
        /* Friends and areas: what's new since last time. */
        int p, area = (int)LD16(save + AREA), first = !j->seen, before = j->friend_count;
        static const int goals[4] = {50, 100, 150, 194};
        sLastScan = now;
        for (p = FIRST; p < POKEMON; p++) {
            u32 at = save + LD32(POINTS_ENTRY + p * 4) * 8 + POINTS_AT;
            if (at + 2 > save + SAVE_SIZE || LD16(at) < FRIEND_POINTS || has_friend(j, p)) continue;
            if (j->friend_count < POKEMON) j->friends[j->friend_count++] = (unsigned char)p;
            if (!first && j->friend_count == 1) add_milestone(j, JOURNAL_FIRST_FRIEND, p);
            sDirty = 1;
        }
        if (!first)
            for (p = 0; p < 4; p++)
                if (before < goals[p] && j->friend_count >= goals[p]) add_milestone(j, JOURNAL_FRIENDS, goals[p]);
        if (area && (area >> 8) >= 1 && (area >> 8) <= 9 && !has_area(j, area)) {
            if (j->area_count < JOURNAL_AREAS) j->areas[j->area_count++] = (unsigned short)area;
            if (!first) add_milestone(j, JOURNAL_AREA, area);
            sDirty = 1;
        }
        if (first) j->seen = 1, sDirty = 1;
    }
    LeaveCriticalSection(&sLock);
    if (sDirty && now - sLastWrite > 20) {
        sLastWrite = now;
        write_now(0);
    }
}

/* ---- For the Journal's pages (settings_ui.cpp, its own thread). */

int hle_journal_slot(void) { return g_nand_last_slot >= 0 && g_nand_last_slot < JOURNAL_SLOTS ? g_nand_last_slot : -1; }
int hle_journal_copy(int slot, JournalSlot* out) {
    if (slot < 0 || slot >= JOURNAL_SLOTS || !sLoaded) return 0;
    EnterCriticalSection(&sLock);
    *out = sSlots[slot];
    LeaveCriticalSection(&sLock);
    return 1;
}
/* From the loaded save: play time (seconds), berries; -1 with none. */
int hle_journal_save_value(int which) {
    u32 save = LD32(SAVE_PTR);
    if (!wp_mem_ptr(save, SAVE_SIZE)) return -1;
    return (int)LD32(save + (which == 0 ? PLAY_TIME : BERRIES));
}
