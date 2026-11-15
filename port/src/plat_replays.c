/*
 * Replays of the online matches. See plat_replays.h.
 *
 * What is kept is the game's own replay block (gBattleReplay: the match's setup and both fighters' input, frame by
 * frame: what an online match synchronises anyway), in a file in the format of a replay on the memory card, which
 * the harness already plays (BT3_REPLAY, headless.c). The match's seed is kept in the .meta file: the game's replay
 * block has none, and a copy of the match plays out the same only with the generators seeded the same.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif
#include "plat_replays.h"
#include "port_view.h"

extern int Port_NetActive(void);
extern int Port_NetSpectating(void);
extern int Port_NetMe(void);
extern unsigned Port_NetTick(void);
extern int Port_NetSpeedMilli(void);
extern const char *Port_NetName(int player);
extern unsigned Port_NetSeed(void);
extern int Lobby_Active(void);
extern void Port_RepCaptureBlock(void *dst); /* src/battle/battle_load.c: a copy of gBattleReplay */
extern int Port_Setting(const char *name, int def);
extern void Port_SettingSave(const char *name, int value);
extern void Port_SettingsWrite(void);
extern void Port_UiNotice(const char *text);
#ifdef _WIN32
extern unsigned long __stdcall GetModuleFileNameA(void *module, char *name, unsigned long size);
#endif

#define DIR_NAME "replays"
#define FILE_SIZE 0x1AC00
#define FILE_HEADER 0x38
#define BLOCK_SIZE 0x1ABA8
#define DEFAULT_KEEP 30

/* ---- the fight being recorded ------------------------------------------------------------------------------
   The game's own replay block keeps 9000 frames of each fighter (5 minutes at 30 Hz) and then stops recording: a fight
   of half an hour would play back only its first five minutes. So both fighters' input is also kept here, without a
   limit but the memory (a frame is 6 bytes; half an hour is about 650 KB), in a file next to the replay (.bt3inp).
   It is stored by the blank (Port_NetTick) the game is in, not by a count of calls: a rollback runs a blank again and
   the second time overwrites the first, so what is kept is what was finally played. The blanks the game does not step
   (it steps at 30 Hz) stay empty and are left out when the file is written. */
#define MAX_TICKS (60u * 60u * 180u) /* three hours of blanks */

static unsigned sPos[2];   /* playback of a long replay: the next frame of each fighter */
static unsigned sPlayed;   /* frames of fighter 0 played so far (a replay being watched) */
static int sPaused;        /* the transport bar's pause */
static int sRestartTo = -1;/* the transport bar asked to start again (and run to this frame) */
static int sViewPending;   /* the first frame of a replay: set the viewer up */
static int sKeepView;      /* a restart from the transport bar: the HUD mode and the side watched stay as they were */
static unsigned sLongN[2]; /* its frames */
static unsigned *sLongBtn[2];
static unsigned char *sLongStk[2];
static int sLongTried;

static int sOn;            /* this fight is an online match that will be kept */
static long long sBase = -1; /* the blank of the first input */
static unsigned sMaxT;     /* the last blank seen, from sBase */
static unsigned sCap;
static unsigned *sBtn[2];
static unsigned char *sStk[2];
static unsigned char *sVal[2];
static int sStep = 30;     /* blanks between two samples of the graph */
static int sCount;         /* samples of the graph */
static short sG[2][PORT_REP_GRAPH];
static int sLast[2];

static int grow(unsigned need) {
    unsigned cap = sCap ? sCap : 4096, p;

    if (need >= MAX_TICKS) {
        return 0;
    }
    if (need < sCap) {
        return 1;
    }
    while (cap <= need) {
        cap *= 2;
    }
    if (cap > MAX_TICKS) {
        cap = MAX_TICKS;
    }
    for (p = 0; p < 2; p++) {
        unsigned *b = (unsigned *)realloc(sBtn[p], (size_t)cap * sizeof(unsigned));
        unsigned char *k, *v;

        if (b == NULL) {
            return 0;
        }
        sBtn[p] = b;
        k = (unsigned char *)realloc(sStk[p], (size_t)cap * 2);
        if (k == NULL) {
            return 0;
        }
        sStk[p] = k;
        v = (unsigned char *)realloc(sVal[p], cap);
        if (v == NULL) {
            return 0;
        }
        sVal[p] = v;
        memset(sVal[p] + sCap, 0, cap - sCap);
    }
    sCap = cap;
    return 1;
}

int Port_RepUi_Auto(int set, int value) {
    if (set) {
        Port_SettingSave("rep_auto", value != 0);
        Port_SettingsWrite();
    }
    return Port_Setting("rep_auto", 1) != 0;
}

int Port_RepRecording(void) {
    return sOn;
}

static void save_current(void);

/* The session facts are taken while the fight is played, when the connection certainly is there: at the start it may not
   be yet, and at the end it may be gone (the names were empty in the saved replays, and a second fight in the same run
   was not kept). */
static int sNetSeen;          /* the connection was up during this fight */
static char sName[2][PORT_REP_NAME];
static int sMe, sCustom;
static unsigned sSeedVal;
static int sSpeedVal = 1000;  /* the match's pace, 1000 = the game's own: the replay's "x1" */

static void see_session(void) {
    static unsigned calls;
    int p;

    if (!Port_NetActive() || Port_NetSpectating()) {
        return;
    }
    if (sNetSeen && sName[0][0] && sName[1][0] && (calls++ & 63u) != 0u) {
        return; /* all of it is known: looked at again once in a while, not on every call (two per frame) */
    }
    sNetSeen = 1;
    for (p = 0; p < 2; p++) {
        const char *n = Port_NetName(p);

        if (n != NULL && n[0] != '\0') {
            snprintf(sName[p], PORT_REP_NAME, "%s", n);
        }
    }
    sMe = Port_NetMe() == 1 ? 1 : 0;
    sCustom = Lobby_Active() != 0;
    sSeedVal = Port_NetSeed();
    sSpeedVal = Port_NetSpeedMilli();
}

void Port_RepBegin(int replayActive) {
    int p;

    if (sOn && sNetSeen) {
        save_current(); /* a rematch in the same battle: the fight that was being played is kept first */
    }
    sOn = !replayActive && Port_RepUi_Auto(0, 0);
    sNetSeen = 0;
    sName[0][0] = sName[1][0] = '\0';
    sBase = -1;
    sMaxT = 0;
    sStep = 30;
    sCount = 0;
    sLast[0] = sLast[1] = 1000;
    for (p = 0; p < 2; p++) {
        if (sVal[p] != NULL) {
            memset(sVal[p], 0, sCap);
        }
    }
    sPos[0] = sPos[1] = 0;
    sPlayed = 0;
    sPaused = 0;
    sViewPending = replayActive && Port_RepPlayActive() && !sKeepView;
    sKeepView = 0;
}

/* One fighter's input of one frame (btl_input.c, as the game takes it for its own replay). */
void Port_RepInput(int player, unsigned buttons, const unsigned char *stick) {
    unsigned idx;

    if (!sOn || player < 0 || player > 1 || !Port_NetActive()) {
        return;
    }
    see_session();
    if (sBase < 0) {
        sBase = (long long)Port_NetTick();
    }
    if ((long long)Port_NetTick() < sBase) {
        return;
    }
    idx = (unsigned)((long long)Port_NetTick() - sBase);
    if (!grow(idx)) {
        return;
    }
    sBtn[player][idx] = buttons;
    sStk[player][idx * 2] = stick[0];
    sStk[player][idx * 2 + 1] = stick[1];
    sVal[player][idx] = 1;
    if (idx > sMaxT) {
        sMaxT = idx;
    }
}

/* Every sample of the graph is the life of both sides at a blank; when the graph is full it keeps every other sample
   and takes them twice as far apart, so any length fits (the time of a sample is worked out from the length at the end). */
void Port_RepSample(int hp0, int hp1) {
    long long t;
    unsigned idx;
    int slot, i;

    if (!sOn || !Port_NetActive()) {
        return;
    }
    see_session();
    if (hp0 < 0) { hp0 = 0; }
    if (hp1 < 0) { hp1 = 0; }
    if (hp0 > 1000) { hp0 = 1000; }
    if (hp1 > 1000) { hp1 = 1000; }
    sLast[0] = hp0;
    sLast[1] = hp1;
    t = (long long)Port_NetTick();
    if (sBase < 0) {
        sBase = t;
    }
    if (t < sBase) {
        return;
    }
    idx = (unsigned)(t - sBase);
    if (idx % (unsigned)sStep != 0) {
        return;
    }
    slot = (int)(idx / (unsigned)sStep);
    while (slot >= PORT_REP_GRAPH) {
        for (i = 0; i < PORT_REP_GRAPH / 2; i++) {
            sG[0][i] = sG[0][i * 2];
            sG[1][i] = sG[1][i * 2];
        }
        sCount = (sCount + 1) / 2;
        sStep *= 2;
        slot = (int)(idx / (unsigned)sStep);
    }
    sG[0][slot] = (short)hp0;
    sG[1][slot] = (short)hp1;
    if (slot + 1 > sCount) {
        sCount = slot + 1;
    }
}

static void make_dir(void) {
#ifdef _WIN32
    _mkdir(DIR_NAME);
#else
    mkdir(DIR_NAME, 0755);
#endif
}

static int by_name_desc(const void *a, const void *b) {
    return strcmp((const char *)b, (const char *)a);
}

/* The base names (without the extension) of the .meta files in the folder, newest first. */
static int list_bases(char (*out)[96], int max) {
    DIR *d = opendir(DIR_NAME);
    struct dirent *e;
    int n = 0;

    if (d == NULL) {
        return 0;
    }
    while ((e = readdir(d)) != NULL && n < max) {
        size_t len = strlen(e->d_name);

        if (len > 5 && len < 90 && strcmp(e->d_name + len - 5, ".meta") == 0) {
            memcpy(out[n], e->d_name, len - 5);
            out[n][len - 5] = '\0';
            n++;
        }
    }
    closedir(d);
    qsort(out, (size_t)n, 96, by_name_desc);
    return n;
}

static void remove_base(const char *base) {
    char path[256];

    snprintf(path, sizeof(path), DIR_NAME "/%s.meta", base);
    remove(path);
    snprintf(path, sizeof(path), DIR_NAME "/%s.bt3rep", base);
    remove(path);
    snprintf(path, sizeof(path), DIR_NAME "/%s.bt3inp", base);
    remove(path);
}

/* The oldest ones go when there are more than rep_keep (30). */
static void rotate(void) {
    static char bases[256][96];
    int n, keep = Port_Setting("rep_keep", DEFAULT_KEEP), i;

    if (keep < 1) {
        keep = DEFAULT_KEEP;
    }
    n = list_bases(bases, 256);
    for (i = keep; i < n; i++) {
        remove_base(bases[i]);
    }
}

/* The whole input of both fighters, without the game's 9000 frame limit: <base>.bt3inp. Frames only, in order (the
   blanks the game did not step are left out): "BT3I", version 1, the two counts, then for each fighter its buttons and
   its sticks. */
static void write_inputs(const char *base) {
    char path[256];
    FILE *fp;
    unsigned head[4], p, t;

    snprintf(path, sizeof(path), DIR_NAME "/%s.bt3inp", base);
    fp = fopen(path, "wb");
    if (fp == NULL) {
        return;
    }
    head[0] = 0x49335442u; /* "BT3I" */
    head[1] = 1u;
    for (p = 0; p < 2; p++) {
        unsigned n = 0;

        for (t = 0; t <= sMaxT; t++) {
            n += sVal[p][t];
        }
        head[2 + p] = n;
    }
    fwrite(head, sizeof(head), 1, fp);
    for (p = 0; p < 2; p++) {
        for (t = 0; t <= sMaxT; t++) {
            if (sVal[p][t]) {
                fwrite(&sBtn[p][t], sizeof(unsigned), 1, fp);
            }
        }
        for (t = 0; t <= sMaxT; t++) {
            if (sVal[p][t]) {
                fwrite(&sStk[p][t * 2], 1, 2, fp);
            }
        }
    }
    fclose(fp);
}

/* BtlReplay_Play (src/battle/btl_replay.c): in a copy started with BT3_REPLAY_LONG=<file.bt3inp> the input comes from
   the whole recording; 0 when there is none, or it has ended (the game's own code then ends the replay). */
/* The copy that plays a replay (BT3_REPLAY): the players' names, the fight's length and how far it has come, for the names
   under the gauges and the clock (gs/ui.cpp). BT3_REPLAY_P0 / _P1 / _FRAMES are set by the window that opened it. */

/* What a copy that plays a replay is asked every frame (by the window, the pace, the HUD) is read from the environment once:
   getenv scans the whole environment every time, on Windows too. */
static int sPlayAct = -1;
static char sPlayNames[2][PORT_REP_NAME];
static int sPlayTotal, sPlayMe, sPlayBase = 1000;

/* The process's priority (Windows): the copy that plays a replay above normal, the other below while it is open. */
#ifdef _WIN32
extern void *__stdcall GetCurrentProcess(void);
extern int __stdcall SetPriorityClass(void *process, unsigned long priorityClass);
#endif
void Port_RepSetPriority(int level) {
#ifdef _WIN32
    SetPriorityClass(GetCurrentProcess(), level > 0 ? 0x8000ul : level < 0 ? 0x4000ul : 0x20ul);
#else
    (void)level;
#endif
}

static int sSplit;          /* the replay is shown in split screen (the game's own two-player view) */

int Port_RepPlayActive(void) {
    if (sPlayAct < 0) {
        const char *v;

        sPlayAct = getenv("BT3_REPLAY") != NULL;
        if (sPlayAct) {
            Port_RepSetPriority(1);
            v = getenv("BT3_REPLAY_P0");
            snprintf(sPlayNames[0], PORT_REP_NAME, "%s", v != NULL ? v : "");
            v = getenv("BT3_REPLAY_P1");
            snprintf(sPlayNames[1], PORT_REP_NAME, "%s", v != NULL ? v : "");
            v = getenv("BT3_REPLAY_FRAMES");
            sPlayTotal = v != NULL ? atoi(v) : 0;
            v = getenv("BT3_REPLAY_SPLIT");
            sSplit = v != NULL && atoi(v) != 0;
            v = getenv("BT3_REPLAY_ME");
            sPlayMe = v != NULL && atoi(v) == 1;
            v = getenv("BT3_REPLAY_SPEED");
            sPlayBase = v != NULL ? atoi(v) : 1000;
            if (sPlayBase < 200) {
                sPlayBase = 1000;
            }
        }
    }
    return sPlayAct;
}

const char *Port_RepPlayName(int side) {
    return (side == 0 || side == 1) && Port_RepPlayActive() ? sPlayNames[side] : "";
}

void Port_RepPlayInfo(int *frame, int *total) {
    Port_RepPlayActive();
    *frame = (int)sPlayed;
    *total = sPlayTotal;
}

/* Playing a replay from a point (BT3_REPLAY_SEEK=<frame>, or the transport bar's jump forward): until the fight has come
   to that frame its blanks are run the way a rollback's are (plat_stub.c, Port_VBlank): not shown, no sound, not waited for. */
static long sSeekTarget = -1;

int Port_RepSeeking(void) {
    if (sSeekTarget < 0) {
        const char *t = getenv("BT3_REPLAY_SEEK");

        sSeekTarget = t != NULL ? atol(t) : 0;
    }
    return sSeekTarget > 0 && (long)sPlayed < sSeekTarget;
}

void Port_RepSeekTo(int frame) { /* forward only: going back starts the fight again (the window does that) */
    Port_RepSeeking();
    sSeekTarget = frame > 0 ? frame : 0;
}

/* Pause (the transport bar's button: the game's own pause flag is raised from the fight's update, btl_seq.c), starting the
   fight again from the first frame (going back: the fight is only computed forward; headless.c runs the replay again in
   this same window and the transport bar's target is run to as a jump forward), and the first frame of a replay: the
   whole HUD and the point of view of whoever recorded it (BT3_REPLAY_ME, the side that player had). */

void Port_RepPauseSet(int on) {
    sPaused = on != 0;
}

int Port_RepPaused(void) {
    return sPaused;
}

void Port_RepRestart(int frame) {
    sRestartTo = frame > 0 ? frame : 0;
    sPaused = 0;
}

int Port_RepRestartPending(void) {
    return sRestartTo >= 0;
}

int Port_RepRestartTake(void) { /* the frame to run to after the fight starts again; clears the request */
    int f = sRestartTo;

    sRestartTo = -1;
    sKeepView = 1;
    return f < 0 ? 0 : f;
}

int Port_RepViewTake(void) { /* 1 once at the start of every replay: the viewer is set up (hud mode, side) */
    int p = sViewPending;

    sViewPending = 0;
    return p;
}

int Port_RepMySide(void) {
    Port_RepPlayActive();
    return sPlayMe;
}

/* The speed the replay plays at: a percent of the pace it was played at (the match's, saved with it: 114% is its x1). */
static int sSpeedPct = 100;

void Port_RepSpeedSet(int percent) {
    sSpeedPct = percent < 25 ? 25 : percent > 200 ? 200 : percent;
}

int Port_RepSpeedGet(void) {
    return sSpeedPct;
}

int Port_RepSpeedMilli(void) {
    if (!Port_RepPlayActive()) {
        return 0;
    }
    return (int)((long)sPlayBase * sSpeedPct / 100);
}

/* Split screen in a replay. An online match runs as the game's two-player fight with its two cameras and shows only the local
   player's view (btl_cam.c, BtlCam_UpdateOverride); a replay is shown the same way by default. Drawing the two views draws more
   random numbers from the generator the fight shares with the drawing code, so this is only ever done in a replay (nobody
   else's fight depends on it, and the replay's own result can differ only where a double KO is decided by that generator): never
   in a live match, online or not. */
int Port_SplitWanted(void) {
    return Port_RepPlayActive() && (sSplit || PortView_IsSplitActive());
}
void Port_SplitSet(int on) {
    sSplit = on != 0;
    PortView_SetMode(sSplit ? VIEW_MODE_SPLIT : (Port_RepMySide() == 1 ? VIEW_MODE_P2 : VIEW_MODE_P1));
}
int Port_SplitGet(void) {
    return Port_RepPlayActive() ? (sSplit || PortView_IsSplitActive()) : 0;
}
/* The pad's L1 + L2 + R1 + R2 held together (the order does not matter) toggles it, if the player asked for that shortcut. */
void Port_SplitKeys(unsigned held) {
    static int was;
    unsigned all = 0x00004000u | 0x00008000u | 0x00010000u | 0x00020000u; /* PADG_L1 | L2 | R1 | R2 */
    int now = (held & all) == all;

    if (now && !was && Port_RepPlayActive() && Port_Setting("rep_split_key", 1) != 0) {
        PortView_CycleNext();
        sSplit = PortView_IsSplitActive();
    }
    was = now;
}

int Port_RepSelfExe(char *exe, int exeSize) {
#ifdef _WIN32
    return GetModuleFileNameA(NULL, exe, (unsigned long)exeSize) != 0;
#else
    long n = readlink("/proc/self/exe", exe, (size_t)exeSize - 1);

    if (n <= 0) {
        return 0;
    }
    exe[n] = '\0';
    return 1;
#endif
}

int Port_RepUi_Clock(int set, int value) {
    if (set) {
        Port_SettingSave("rep_clock", value != 0);
        Port_SettingsWrite();
    }
    return Port_Setting("rep_clock", 1) != 0;
}

int Port_RepLongPlay(int player, unsigned *buttons, unsigned char *stick) {
    if (player == 0) {
        sPlayed++; /* (counted for every replay, a long one or not: one call per frame of fighter 0) */
    }
    if (!sLongTried) {
        const char *path = getenv("BT3_REPLAY_LONG");
        FILE *fp = path != NULL ? fopen(path, "rb") : NULL;
        unsigned head[4], p;

        sLongTried = 1;
        if (fp != NULL && fread(head, sizeof(head), 1, fp) == 1 && head[0] == 0x49335442u && head[1] == 1u) {
            for (p = 0; p < 2; p++) {
                sLongBtn[p] = (unsigned *)malloc((head[2 + p] + 1) * sizeof(unsigned));
                sLongStk[p] = (unsigned char *)malloc((head[2 + p] + 1) * 2);
                if (sLongBtn[p] == NULL || sLongStk[p] == NULL ||
                    fread(sLongBtn[p], sizeof(unsigned), head[2 + p], fp) != head[2 + p] ||
                    fread(sLongStk[p], 2, head[2 + p], fp) != head[2 + p]) {
                    free(sLongBtn[p]);
                    free(sLongStk[p]);
                    sLongBtn[p] = NULL;
                    sLongStk[p] = NULL;
                    head[2 + p] = 0;
                }
                sLongN[p] = head[2 + p];
            }
        }
        if (fp != NULL) {
            fclose(fp);
        }
    }
    if (player < 0 || player > 1 || sLongBtn[player] == NULL || sPos[player] >= sLongN[player]) {
        return 0;
    }
    *buttons = sLongBtn[player][sPos[player]];
    stick[0] = sLongStk[player][sPos[player] * 2];
    stick[1] = sLongStk[player][sPos[player] * 2 + 1];
    sPos[player]++;
    return 1;
}

static void save_current(void) {
    static unsigned char buf[FILE_SIZE];
    char base[96], path[256];
    FILE *fp;
    time_t now = time(NULL);
    struct tm *tm;
    int me, winner, i, hpMe, hpOpp;
    unsigned frames = 0, t;

    sOn = 0;
    if (sBase >= 0 && sVal[0] != NULL) {
        for (t = 0; t <= sMaxT; t++) {
            frames += sVal[0][t];
        }
    }
    if (frames < 30) { /* nothing to keep: a fight of less than a second */
        return;
    }
    memset(buf, 0, sizeof(buf));
    Port_RepCaptureBlock(buf + FILE_HEADER);
    {
        int any = 0;
        for (i = 0; i < 8; i++) {
            any |= buf[FILE_HEADER + i];
        }
        if (!any) { /* the block carries no setup: not a fight the game can play back */
            return;
        }
    }
    make_dir();
    tm = localtime(&now);
    snprintf(base, sizeof(base), "%04d%02d%02d_%02d%02d%02d", tm != NULL ? tm->tm_year + 1900 : 0, tm != NULL ? tm->tm_mon + 1 : 0,
             tm != NULL ? tm->tm_mday : 0, tm != NULL ? tm->tm_hour : 0, tm != NULL ? tm->tm_min : 0, tm != NULL ? tm->tm_sec : 0);
    {
        char stem[96];
        int n;

        snprintf(stem, sizeof(stem), "%s", base);
        for (n = 2; n < 100; n++) { /* two fights in the same second do not overwrite each other */
            snprintf(path, sizeof(path), DIR_NAME "/%s.meta", base);
            fp = fopen(path, "r");
            if (fp == NULL) {
                break;
            }
            fclose(fp);
            snprintf(base, sizeof(base), "%s_%d", stem, n);
        }
    }
    snprintf(path, sizeof(path), DIR_NAME "/%s.bt3rep", base);
    fp = fopen(path, "wb");
    if (fp == NULL) {
        return;
    }
    fwrite(buf, 1, sizeof(buf), fp);
    fclose(fp);
    write_inputs(base);

    me = sMe;
    hpMe = sLast[me];
    hpOpp = sLast[1 - me];
    winner = hpMe > hpOpp ? me : hpMe < hpOpp ? 1 - me : -1;
    snprintf(path, sizeof(path), DIR_NAME "/%s.meta", base);
    fp = fopen(path, "w");
    if (fp == NULL) {
        return;
    }
    fprintf(fp, "v=1\ntime=%lld\ntype=%s\nme=%d\np0=%s\np1=%s\nwinner=%d\nframes=%d\nseed=%u\nspeed=%d\n", (long long)now,
            sCustom ? "custom" : "casual", me, sName[0][0] ? sName[0] : "Player 1", sName[1][0] ? sName[1] : "Player 2", winner, (int)frames, sSeedVal, sSpeedVal);
    /* the time one sample of the graph stands for, in milliseconds: the blanks between two samples out of the blanks
       of the whole fight, over the fight's length (30 frames a second) */
    fprintf(fp, "gms=%d\n", (int)((long long)sStep * (long long)frames * 1000 / 30 / (long long)(sMaxT + 1)));
    fprintf(fp, "g0=");
    for (i = 0; i < sCount; i++) {
        fprintf(fp, "%s%d", i ? "," : "", sG[0][i]);
    }
    fprintf(fp, "\ng1=");
    for (i = 0; i < sCount; i++) {
        fprintf(fp, "%s%d", i ? "," : "", sG[1][i]);
    }
    fprintf(fp, "\n");
    fclose(fp);
    rotate();
    Port_UiNotice("Replay guardada");
}

void Port_RepEnd(void) {
    if (sOn && sNetSeen) {
        save_current();
    }
    sOn = 0;
    sNetSeen = 0;
}

/* ---- the list --------------------------------------------------------------------------------------------- */
static PortRepInfo sList[PORT_REP_MAX];
static unsigned sSeed[PORT_REP_MAX];
static int sListN;

static void parse_list(char *s, short *out, int *n) {
    int k = 0;

    while (*s != '\0' && k < PORT_REP_GRAPH) {
        char *end;
        long v = strtol(s, &end, 10);

        if (end == s) {
            break;
        }
        out[k++] = (short)v;
        s = end;
        if (*s == ',') {
            s++;
        }
    }
    *n = k;
}

static int load_meta(const char *base, PortRepInfo *r, unsigned *seed) {
    static char line[PORT_REP_GRAPH * 6 + 64];
    static short g0[PORT_REP_GRAPH], g1[PORT_REP_GRAPH];
    char path[256], p0[PORT_REP_NAME] = "", p1[PORT_REP_NAME] = "";
    FILE *fp;
    int n0 = 0, n1 = 0, me = 0, frames = 0, i;

    snprintf(path, sizeof(path), DIR_NAME "/%s.meta", base);
    fp = fopen(path, "r");
    if (fp == NULL) {
        return 0;
    }
    memset(r, 0, sizeof(*r));
    r->won = -1;
    r->graphStepMs = 500;
    r->speedMilli = 1000;
    *seed = 0;
    while (fgets(line, sizeof(line), fp) != NULL) {
        size_t len = strlen(line);

        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (strncmp(line, "time=", 5) == 0) {
            r->when = atoll(line + 5);
        } else if (strncmp(line, "type=", 5) == 0) {
            r->custom = strcmp(line + 5, "custom") == 0;
        } else if (strncmp(line, "me=", 3) == 0) {
            me = atoi(line + 3) == 1;
        } else if (strncmp(line, "p0=", 3) == 0) {
            snprintf(p0, sizeof(p0), "%s", line + 3);
        } else if (strncmp(line, "p1=", 3) == 0) {
            snprintf(p1, sizeof(p1), "%s", line + 3);
        } else if (strncmp(line, "winner=", 7) == 0) {
            r->won = atoi(line + 7);
        } else if (strncmp(line, "frames=", 7) == 0) {
            frames = atoi(line + 7);
        } else if (strncmp(line, "seed=", 5) == 0) {
            *seed = (unsigned)strtoul(line + 5, NULL, 10);
        } else if (strncmp(line, "speed=", 6) == 0) {
            r->speedMilli = atoi(line + 6);
        } else if (strncmp(line, "gms=", 4) == 0) {
            r->graphStepMs = atoi(line + 4);
        } else if (strncmp(line, "g0=", 3) == 0) {
            parse_list(line + 3, g0, &n0);
        } else if (strncmp(line, "g1=", 3) == 0) {
            parse_list(line + 3, g1, &n1);
        }
    }
    fclose(fp);
    r->winSide = r->won;
    r->mySide = me;
    snprintf(r->p[0], sizeof(r->p[0]), "%s", p0);
    snprintf(r->p[1], sizeof(r->p[1]), "%s", p1);
    r->won = r->won < 0 ? -1 : (r->won == me ? 1 : 0);
    snprintf(r->me, sizeof(r->me), "%s", me ? p1 : p0);
    snprintf(r->opp, sizeof(r->opp), "%s", me ? p0 : p1);
    r->seconds = frames / 30;
    r->graphN = n0 < n1 ? n0 : n1;
    for (i = 0; i < r->graphN; i++) {
        r->graph[i] = (short)((me ? g1[i] : g0[i]) - (me ? g0[i] : g1[i]));
    }
    snprintf(r->file, sizeof(r->file), "%s", base);
    return 1;
}

int Port_RepUi_Scan(void) {
    static char bases[PORT_REP_MAX][96];
    int n = list_bases(bases, PORT_REP_MAX), i;

    sListN = 0;
    for (i = 0; i < n; i++) {
        if (load_meta(bases[i], &sList[sListN], &sSeed[sListN])) {
            sListN++;
        }
    }
    return sListN;
}

int Port_RepUi_Get(int i, PortRepInfo *out) {
    if (i < 0 || i >= sListN) {
        return 0;
    }
    *out = sList[i];
    return 1;
}

void Port_RepUi_Delete(int i) {
    if (i >= 0 && i < sListN) {
        remove_base(sList[i].file);
    }
}

void Port_RepUi_History(const char *opp, int *wins, int *losses) {
    int i;

    *wins = *losses = 0;
    for (i = 0; i < sListN; i++) {
        if (strcmp(sList[i].opp, opp) == 0) {
            if (sList[i].won == 1) {
                (*wins)++;
            } else if (sList[i].won == 0) {
                (*losses)++;
            }
        }
    }
}

int Port_RepUi_Play(int i, char *file, int fileSize, char *longFile, int longSize, unsigned *seed, char *exe, int exeSize) {
    if (i < 0 || i >= sListN) {
        return 0;
    }
    snprintf(file, (size_t)fileSize, DIR_NAME "/%s.bt3rep", sList[i].file);
    snprintf(longFile, (size_t)longSize, DIR_NAME "/%s.bt3inp", sList[i].file);
    {
        FILE *fp = fopen(longFile, "rb");

        if (fp != NULL) {
            fclose(fp);
        } else {
            longFile[0] = '\0'; /* a replay without it: the game's own block is all there is */
        }
    }
    *seed = sSeed[i];
#ifdef _WIN32
    if (GetModuleFileNameA(NULL, exe, (unsigned long)exeSize) == 0) {
        return 0;
    }
#else
    {
        long n = readlink("/proc/self/exe", exe, (size_t)exeSize - 1);

        if (n <= 0) {
            return 0;
        }
        exe[n] = '\0';
    }
#endif
    return 1;
}
