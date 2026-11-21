/* The Journal's counts (hle_journal.c), for the F1 notebook's Journal book
 * (runtime/host/settings_ui.cpp). */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define JOURNAL_SLOTS 2
#define JOURNAL_DAYS 60
#define JOURNAL_AREAS 64
#define JOURNAL_MILESTONES 64

enum {
    JOURNAL_FIRST_FRIEND = 1, /* value: the Pokémon (the Pad's number, hle_friends.c) */
    JOURNAL_AREA = 2,         /* value: area << 8 | zone (Cove Town 0x0102) */
    JOURNAL_FRIENDS = 3,      /* value: 50, 100, 150 or 194 */
};

typedef struct {
    char day[11]; /* YYYY-MM-DD, local */
    int seconds;
} JournalDay;

typedef struct {
    char day[11];
    int kind, value;
} JournalMilestone;

typedef struct {
    int seen; /* Wonderpark has seen this save file (what it had then: no milestones) */
    JournalDay days[JOURNAL_DAYS];
    int day_count;
    int attractions[4]; /* times played: Cake Contraption, Dance Inferno, Power Bash, Flight of Fancy */
    unsigned char friends[198];
    int friend_count;
    unsigned short areas[JOURNAL_AREAS];
    int area_count;
    JournalMilestone milestones[JOURNAL_MILESTONES];
    int milestone_count;
    /* From the Wii Remote buttons the game gets, in the park outside story
     * scenes and talks: jumps (a press of 2) and Thunderbolts (1 held half
     * a second or more, then let go: the charged move). */
    int jumps, thunderbolts;
} JournalSlot;

void hle_journal_init(void);   /* main.c, before the game runs: reads journal.json */
void hle_journal_flush(void);  /* writes it if anything changed (at exit) */
void hle_journal_frame(void);  /* each game frame */

/* The save file loaded (0, 1), -1 if not known yet. */
int hle_journal_slot(void);
/* A copy of a save file's journal; 0 if none. */
int hle_journal_copy(int slot, JournalSlot* out);
/* From the loaded save: 0 play time (seconds), 1 berries; -1 with none loaded. */
int hle_journal_save_value(int which);

#ifdef __cplusplus
}
#endif
