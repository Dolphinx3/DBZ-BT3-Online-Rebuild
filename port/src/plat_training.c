/*
 * Training mod ("Entrenamiento"), the data and the logic. Integers only (this file is compiled with the PS2 float
 * model and with the game's 32-bit pointer rules: nothing here touches a game structure).
 *
 *   SELECT            starts / stops recording P1's inputs (a message tells which).
 *   START (the pause) plays the selected recording on the dummy (P2), or on P1 (see the F1 window); START again stops.
 *   L2 + D-pad down   opens the game's pause menu (START used to).
 *
 * The inputs are recorded where the game builds a fighter's frame of input (BtlInput_BuildRecord, the same place
 * the game's own replay hooks in): the button word and the stick bytes after the key configuration and the stick
 * merge. A recording therefore is the game's own input, frame by frame, and playing it back replaces the
 * fighter's input of that frame and rebuilds the commands, exactly as the game's replay does.
 *
 * The recordings are kept in bt3_training.bin next to the program (written at every change).
 * The F1 window ("Entrenamiento" tab, gs/ui.cpp) uses the Port_TrainUi_* functions from its own thread: one mutex
 * guards the recordings.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat_training.h"

extern int Port_NetActive(void);
extern int BtlGame_IsReplay(void); /* src/battle/hud_0.c: a saved replay is being played */
extern void Port_UiNotice(const char *text); /* gs/ui.cpp: a line over the picture for a few seconds */
extern int Port_Setting(const char *name, int def);
extern void Port_SettingSave(const char *name, int value);
extern void Port_SettingsWrite(void);

/* The game's button words, as far as this file needs them. */
#define PADG_DOWN 0x00000004     /* include/sys/pad.h, Pad.gamePressed / gameHeld */
#define PADG_START 0x00001000
#define PADG_SELECT 0x00002000
#define PADG_L2 0x00008000
#define PADG_L3 0x00040000
#define PADG_R3 0x00080000
#define BTLB_SELECT 0x00800000   /* not recorded: it is the key that starts / stops the recording */
#define STICK_CENTER 0x7F

#define FILE_NAME "bt3_training.bin"
#define FILE_MAGIC 0x54335442u   /* "BT3T" */
#define FILE_VERSION 1u

typedef struct Slot {
    char name[PORT_TRAIN_NAME];
    int frames;
    unsigned *buttons;        /* [frames] */
    unsigned char *stick;     /* [frames * 2] */
} Slot;

volatile PortTrainGauge gPortTrainGauge[2];
volatile int gPortTrainRefill;

static pthread_mutex_t sLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t sOnce = PTHREAD_ONCE_INIT;
static Slot sSlot[PORT_TRAIN_SLOTS];
static int sCount;
static int sSel = -1;
static int sActive;      /* a training fight is running */
static int sState;       /* PORT_TRAIN_* */
static int sRecFrames;   /* frames recorded so far */
static int sPos;         /* playback position */
static int sPlaySlot = -1;
static int sLoop;        /* 1 = start again at the end */
static int sTarget;      /* 0 = the recording plays on P2, 1 = on P1 */
static int sL3Down;      /* L3 is held (it opens the pause menu when released alone) */
static int sL3Chord;     /* R3 came with it: L3 + R3 is the character switch, not the pause */
static int sAutoZ[2];     /* automatic Z Counter, per side */
static int sAutoZPct[2] = {100, 100};
static unsigned sRollSeed = 0x2545F491u;
static unsigned sRecBtn[PORT_TRAIN_FRAMES];
static unsigned char sRecStick[PORT_TRAIN_FRAMES][2];

/* ---- messages ----------------------------------------------------------------------------------------------- */

static void say(const char *fmt, const char *a, int n, int m) {
    char text[120];

    snprintf(text, sizeof(text), fmt, a != NULL ? a : "", n, m);
    Port_UiNotice(text);
}

/* ---- the file ----------------------------------------------------------------------------------------------- */

static void file_save(void) { /* sLock held */
    FILE *fp = fopen(FILE_NAME, "wb");
    unsigned head[3];
    int i;

    if (fp == NULL) {
        return;
    }
    head[0] = FILE_MAGIC;
    head[1] = FILE_VERSION;
    head[2] = (unsigned)sCount;
    fwrite(head, sizeof(head), 1, fp);
    for (i = 0; i < sCount; i++) {
        unsigned n = (unsigned)sSlot[i].frames;

        fwrite(sSlot[i].name, PORT_TRAIN_NAME, 1, fp);
        fwrite(&n, sizeof(n), 1, fp);
        if (n > 0) {
            fwrite(sSlot[i].buttons, sizeof(unsigned), n, fp);
            fwrite(sSlot[i].stick, 2, n, fp);
        }
    }
    fclose(fp);
}

static void slot_free(Slot *s) {
    free(s->buttons);
    free(s->stick);
    memset(s, 0, sizeof(*s));
}

static void file_load(void) {
    FILE *fp = fopen(FILE_NAME, "rb");
    unsigned head[3];
    unsigned i;

    sLoop = Port_Setting("train_loop", 0) != 0;
    sTarget = Port_Setting("train_target", 0) != 0;
    if (fp == NULL) {
        return;
    }
    if (fread(head, sizeof(head), 1, fp) != 1 || head[0] != FILE_MAGIC || head[1] != FILE_VERSION) {
        fclose(fp);
        return;
    }
    for (i = 0; i < head[2] && sCount < PORT_TRAIN_SLOTS; i++) {
        Slot *s = &sSlot[sCount];
        unsigned n;

        if (fread(s->name, PORT_TRAIN_NAME, 1, fp) != 1 || fread(&n, sizeof(n), 1, fp) != 1 || n > PORT_TRAIN_FRAMES) {
            break;
        }
        s->name[PORT_TRAIN_NAME - 1] = '\0';
        s->buttons = (unsigned *)malloc((n + 1) * sizeof(unsigned));
        s->stick = (unsigned char *)malloc((n + 1) * 2);
        if (s->buttons == NULL || s->stick == NULL || fread(s->buttons, sizeof(unsigned), n, fp) != n ||
            fread(s->stick, 2, n, fp) != n) {
            slot_free(s);
            break;
        }
        s->frames = (int)n;
        sCount++;
    }
    fclose(fp);
    sSel = sCount > 0 ? 0 : -1;
}

static void init(void) {
    int i;
    char key[24];

    file_load();
    for (i = 0; i < 2; i++) {
        snprintf(key, sizeof(key), "autoz%d", i + 1);
        sAutoZ[i] = Port_Setting(key, 0) != 0;
        snprintf(key, sizeof(key), "autoz%d_pct", i + 1);
        sAutoZPct[i] = Port_Setting(key, 100);
        if (sAutoZPct[i] < 0 || sAutoZPct[i] > 100) {
            sAutoZPct[i] = 100;
        }
    }
}

/* ---- recording and playback (sLock held) --------------------------------------------------------------------- */

static void default_name(char *out) {
    int n, i, used;

    for (n = 1; n < 1000; n++) {
        used = 0;
        snprintf(out, PORT_TRAIN_NAME, "Grabacion %d", n);
        for (i = 0; i < sCount; i++) {
            if (strcmp(sSlot[i].name, out) == 0) {
                used = 1;
            }
        }
        if (!used) {
            return;
        }
    }
}

static void play_stop(int silent) {
    if (sState == PORT_TRAIN_PLAYING) {
        sState = PORT_TRAIN_IDLE;
        sPos = 0;
        sPlaySlot = -1;
        if (!silent) {
            say("Reproduccion detenida", NULL, 0, 0);
        }
    }
}

static void record_stop(int silent) {
    Slot *s;

    if (sState != PORT_TRAIN_RECORDING) {
        return;
    }
    sState = PORT_TRAIN_IDLE;
    if (sRecFrames <= 0) {
        if (!silent) {
            say("Grabacion vacia: descartada", NULL, 0, 0);
        }
        return;
    }
    if (sCount >= PORT_TRAIN_SLOTS) {
        say("Limite de grabaciones: borra alguna", NULL, 0, 0);
        return;
    }
    s = &sSlot[sCount];
    memset(s, 0, sizeof(*s));
    s->buttons = (unsigned *)malloc((size_t)sRecFrames * sizeof(unsigned));
    s->stick = (unsigned char *)malloc((size_t)sRecFrames * 2);
    if (s->buttons == NULL || s->stick == NULL) {
        slot_free(s);
        say("Sin memoria para guardar la grabacion", NULL, 0, 0);
        return;
    }
    memcpy(s->buttons, sRecBtn, (size_t)sRecFrames * sizeof(unsigned));
    memcpy(s->stick, sRecStick, (size_t)sRecFrames * 2);
    s->frames = sRecFrames;
    default_name(s->name);
    sSel = sCount++;
    file_save();
    say("Grabacion guardada: %s (%d cuadros, %d s)", s->name, s->frames, (s->frames + 29) / 30);
}

static void record_start(void) {
    if (sState == PORT_TRAIN_PLAYING) {
        play_stop(1);
    }
    sState = PORT_TRAIN_RECORDING;
    sRecFrames = 0;
    say("GRABANDO... (SELECT para detener)", NULL, 0, 0);
}

static void play_start(void) {
    if (sState == PORT_TRAIN_RECORDING) {
        say("Deten la grabacion primero (SELECT)", NULL, 0, 0);
        return;
    }
    if (sSel < 0 || sSel >= sCount || sSlot[sSel].frames <= 0) {
        say("No hay grabacion. SELECT para grabar", NULL, 0, 0);
        return;
    }
    sState = PORT_TRAIN_PLAYING;
    sPos = 0;
    sPlaySlot = sSel;
    say("Reproduciendo: %s (START para detener)", sSlot[sSel].name, 0, 0);
}

/* ---- the game's side ------------------------------------------------------------------------------------------ */

void Port_TrainBattleReset(int training) {
    pthread_once(&sOnce, init);
    pthread_mutex_lock(&sLock);
    record_stop(1);
    play_stop(1);
    sActive = training != 0 && !Port_NetActive(); /* never in an online fight */
    sL3Down = 0;
    pthread_mutex_unlock(&sLock);
}

void Port_TrainBattleEnd(void) {
    Port_TrainBattleReset(0);
}

/* Automatic Z Counter. The game's Z Counter is Up + Rush: it opens a one frame window (fighter + 0x1070) that
   BtlHit_CheckGuard looks at for the first hit of an attack on a fighter in a neutral animation, and then waits
   30 frames. The caller (btl_char_hit.c) has already checked all that; this says whether the fighter is set to
   counter it and whether the chance (percent) comes up. The roll has its own generator: the game's is never drawn
   from. Never online (matchmaking or any session) and never while a replay plays. */
int Port_AutoZRoll(int player) {
    int pct;

    pthread_once(&sOnce, init);
    if (player < 0 || player > 1 || !sAutoZ[player] || Port_NetActive() || BtlGame_IsReplay()) {
        return 0;
    }
    pct = sAutoZPct[player];
    if (pct >= 100) {
        return 1;
    }
    sRollSeed = sRollSeed * 1664525u + 1013904223u;
    return (int)((sRollSeed >> 16) % 100u) < pct;
}

void Port_TrainUi_AutoZGet(int side, int *enabled, int *pct) {
    pthread_once(&sOnce, init);
    side &= 1;
    *enabled = sAutoZ[side];
    *pct = sAutoZPct[side];
}

void Port_TrainUi_AutoZSet(int side, int enabled, int pct) {
    char key[24];

    pthread_once(&sOnce, init);
    side &= 1;
    if (pct < 0) {
        pct = 0;
    }
    if (pct > 100) {
        pct = 100;
    }
    sAutoZ[side] = enabled != 0;
    sAutoZPct[side] = pct;
    snprintf(key, sizeof(key), "autoz%d", side + 1);
    Port_SettingSave(key, enabled != 0);
    snprintf(key, sizeof(key), "autoz%d_pct", side + 1);
    Port_SettingSave(key, pct);
    Port_SettingsWrite();
}

/* SELECT no longer steps the camera in the offline modes (online it stays the game's). */
extern int Jukebox_SelectOn(void); /* gs/jukebox.c: SELECT is the music player's "next song" */
int Port_CamSelectOff(void) {
    return !Port_NetActive() || Jukebox_SelectOn();
}

/* Outside the training fight (offline, not a replay) SELECT turns P2's automatic Z Counter on / off. */
void Port_AutoZSelect(void) {
    int en, pct;
    char text[80];

    pthread_once(&sOnce, init);
    if (Port_NetActive() || BtlGame_IsReplay()) {
        return;
    }
    en = sAutoZ[1];
    pct = sAutoZPct[1];
    Port_TrainUi_AutoZSet(1, !en, pct);
    snprintf(text, sizeof(text), !en ? "Contra-Z automatica P2: ACTIVADA (%d%%)" : "Contra-Z automatica P2: DESACTIVADA (%d%%)", pct);
    Port_UiNotice(text);
}

int Port_TrainActive(void) {
    return sActive;
}

int Port_TrainPoll(unsigned pressed, unsigned held) {
    int open = 0;

    pthread_once(&sOnce, init);
    pthread_mutex_lock(&sLock);
    if (sActive) {
        if (pressed & PADG_SELECT) {
            if (sState == PORT_TRAIN_RECORDING) {
                record_stop(0);
            } else {
                record_start();
            }
        }
        if (pressed & PADG_START) {
            if (sState == PORT_TRAIN_PLAYING) {
                play_stop(0);
            } else {
                play_start();
            }
        }
        /* The pause menu: L3, when it is let go without R3 (L3 + R3 stays the game's character switch). */
        if (pressed & PADG_L3) {
            sL3Down = 1;
            sL3Chord = 0;
        }
        if (sL3Down) {
            if (held & PADG_R3) {
                sL3Chord = 1;
            }
            if (!(held & PADG_L3)) {
                sL3Down = 0;
                if (!sL3Chord) {
                    open = 1;
                }
            }
        }
    }
    pthread_mutex_unlock(&sLock);
    return open;
}

int Port_TrainFilter(int player, unsigned *buttons, unsigned char *stick) {
    int changed = 0;

    if (!sActive || sState == PORT_TRAIN_IDLE) {
        return 0;
    }
    pthread_mutex_lock(&sLock);
    if (sState == PORT_TRAIN_RECORDING && player == 0) {
        if (sRecFrames < PORT_TRAIN_FRAMES) {
            sRecBtn[sRecFrames] = *buttons & ~(unsigned)BTLB_SELECT;
            sRecStick[sRecFrames][0] = stick[0];
            sRecStick[sRecFrames][1] = stick[1];
            sRecFrames++;
        } else {
            record_stop(0); /* full: what was recorded is kept */
        }
    } else if (sState == PORT_TRAIN_PLAYING && player == (sTarget ? 0 : 1)) {
        Slot *s = (sPlaySlot >= 0 && sPlaySlot < sCount) ? &sSlot[sPlaySlot] : NULL;

        if (s == NULL || s->frames <= 0) {
            play_stop(1);
        } else {
            if (sPos >= s->frames) {
                if (sLoop) {
                    sPos = 0;
                } else {
                    play_stop(1);
                    say("Reproduccion terminada", NULL, 0, 0);
                    s = NULL;
                }
            }
            if (s != NULL) {
                *buttons = s->buttons[sPos];
                stick[0] = s->stick[sPos * 2];
                stick[1] = s->stick[sPos * 2 + 1];
                sPos++;
                changed = 1;
            }
        }
    }
    pthread_mutex_unlock(&sLock);
    return changed;
}

/* ---- the F1 window's side -------------------------------------------------------------------------------------- */

void Port_TrainUi_Info(PortTrainInfo *out) {
    pthread_once(&sOnce, init);
    pthread_mutex_lock(&sLock);
    out->state = sState;
    out->frames = sState == PORT_TRAIN_RECORDING ? sRecFrames : sPos;
    out->total = (sState == PORT_TRAIN_PLAYING && sPlaySlot >= 0 && sPlaySlot < sCount) ? sSlot[sPlaySlot].frames : 0;
    out->slot = sSel;
    out->count = sCount;
    out->active = sActive;
    pthread_mutex_unlock(&sLock);
}

int Port_TrainUi_SlotInfo(int slot, char *name, int nameSize, int *frames) {
    int ok = 0;

    pthread_once(&sOnce, init);
    pthread_mutex_lock(&sLock);
    if (slot >= 0 && slot < sCount) {
        snprintf(name, (size_t)nameSize, "%s", sSlot[slot].name);
        *frames = sSlot[slot].frames;
        ok = 1;
    }
    pthread_mutex_unlock(&sLock);
    return ok;
}

void Port_TrainUi_Select(int slot) {
    pthread_mutex_lock(&sLock);
    if (slot >= 0 && slot < sCount) {
        sSel = slot;
    }
    pthread_mutex_unlock(&sLock);
}

void Port_TrainUi_Rename(int slot, const char *name) {
    pthread_mutex_lock(&sLock);
    if (slot >= 0 && slot < sCount && name != NULL && name[0] != '\0') {
        snprintf(sSlot[slot].name, PORT_TRAIN_NAME, "%s", name);
        file_save();
    }
    pthread_mutex_unlock(&sLock);
}

int Port_TrainUi_Duplicate(int slot) {
    int made = -1;

    pthread_mutex_lock(&sLock);
    if (slot >= 0 && slot < sCount && sCount < PORT_TRAIN_SLOTS) {
        Slot *src = &sSlot[slot];
        Slot *dst = &sSlot[sCount];
        size_t n = (size_t)src->frames;

        memset(dst, 0, sizeof(*dst));
        dst->buttons = (unsigned *)malloc((n + 1) * sizeof(unsigned));
        dst->stick = (unsigned char *)malloc((n + 1) * 2);
        if (dst->buttons != NULL && dst->stick != NULL) {
            memcpy(dst->buttons, src->buttons, n * sizeof(unsigned));
            memcpy(dst->stick, src->stick, n * 2);
            dst->frames = src->frames;
            {
                char base[PORT_TRAIN_NAME];

                snprintf(base, sizeof(base), "%.23s", src->name);
                snprintf(dst->name, PORT_TRAIN_NAME, "%.23s (copia)", base);
            }
            made = sCount++;
            sSel = made;
            file_save();
        } else {
            slot_free(dst);
        }
    }
    pthread_mutex_unlock(&sLock);
    return made;
}

void Port_TrainUi_Delete(int slot) {
    int i;

    pthread_mutex_lock(&sLock);
    if (slot >= 0 && slot < sCount) {
        play_stop(1);
        slot_free(&sSlot[slot]);
        for (i = slot; i < sCount - 1; i++) {
            sSlot[i] = sSlot[i + 1];
        }
        memset(&sSlot[sCount - 1], 0, sizeof(Slot));
        sCount--;
        if (sSel >= sCount) {
            sSel = sCount - 1;
        }
        file_save();
    }
    pthread_mutex_unlock(&sLock);
}

void Port_TrainUi_Move(int slot, int delta) {
    int to = slot + delta;

    pthread_mutex_lock(&sLock);
    if (slot >= 0 && slot < sCount && to >= 0 && to < sCount) {
        Slot tmp = sSlot[slot];

        play_stop(1);
        sSlot[slot] = sSlot[to];
        sSlot[to] = tmp;
        sSel = to;
        file_save();
    }
    pthread_mutex_unlock(&sLock);
}

void Port_TrainUi_Play(void) {
    pthread_mutex_lock(&sLock);
    if (!sActive) {
        say("Solo funciona en el modo Entrenamiento", NULL, 0, 0);
    } else if (sState == PORT_TRAIN_PLAYING) {
        play_stop(0);
    } else {
        play_start();
    }
    pthread_mutex_unlock(&sLock);
}

void Port_TrainUi_Record(void) {
    pthread_mutex_lock(&sLock);
    if (!sActive) {
        say("Solo funciona en el modo Entrenamiento", NULL, 0, 0);
    } else if (sState == PORT_TRAIN_RECORDING) {
        record_stop(0);
    } else {
        record_start();
    }
    pthread_mutex_unlock(&sLock);
}

int Port_TrainUi_Loop(int set, int value) {
    pthread_once(&sOnce, init);
    if (set) {
        sLoop = value != 0;
        Port_SettingSave("train_loop", sLoop);
        Port_SettingsWrite();
    }
    return sLoop;
}

int Port_TrainUi_Target(int set, int value) {
    pthread_once(&sOnce, init);
    if (set) {
        sTarget = value != 0;
        Port_SettingSave("train_target", sTarget);
        Port_SettingsWrite();
    }
    return sTarget;
}
