#include "common.h"
#include "battle/hud_e.h"
#include "battle/battle.h"
#include "battle/battle_setup.h"
#include "sys/pad.h"
#include "sys/pad_watch.h"
#include "sys/adx.h"
#include "sys/snd.h"

/*
 * Pause request, 0x22F998-0x22FC40. Not part of the HUD: it remembers how many pads may pause the battle (2 in
 * split screen, else 1) and which pad did. BtlPause_CheckOpen / BtlPause_CheckStart are transition conditions of
 * the battle sequence tables (gBtlSeqTbl*); they read the pads' game button words directly.
 */

#define BTL_PAUSE_START 0x1000 /* START in Pad.gamePressed */

extern BtlPause gBtlPause;

#ifdef PORT
/* Training mod (port/src/plat_training.c): in the training mode (battle mode 6) SELECT records, START plays the
   recording back, and the pause menu opens with L2 + D-pad down. */
#include "battle/btl_char_member.h"
#include "plat_training.h"
#include "plat_replays.h"
extern s32 BattleReplay_IsActive(void);
extern s32 Jukebox_OnSelect(void); /* port/src/gs/jukebox.c */
extern BtlMemberChr *BtlChar_Get(s32 side);
#endif

/* Returns the work. */
BtlPause *BtlPause_GetWork(void) {
    return &gBtlPause;
}

/* Round reset: pad 0 owns the pause. */
void BtlPause_Reset(void) {
    BtlPause_GetWork()->pad = 0;
#ifdef PORT
    Port_TrainBattleReset(Battle_GetMode() == PORT_TRAIN_MODE);
    Port_RepBegin(BattleReplay_IsActive());
#endif
}

/* Battle start: sets the number of pads that may pause. */
void BtlPause_Init(s32 padCount) {
    BtlPause_GetWork()->padCount = padCount;
    BtlPause_Reset();
}

/* Battle end: nothing. */
void BtlPause_Term(void) {
#ifdef PORT
    Port_TrainBattleEnd();
    Port_RepEnd();
#endif
}

/* Sequence condition "open the pause menu": 1 when one of the pads pressed START (that pad becomes the owner).
   Otherwise, outside mode 7, a pad that was pulled out (PadWatch_GetMissing) becomes the owner and the battle is
   paused at once (BATTLE_FLAG_PAUSE | BATTLE_FLAG_PAUSE_MENU, the streams and sound group 4 are paused), but 0 is
   returned. In mode 7 the pad watch is not asked; pad 0's START counts once more (the owner is not
   changed). */
s32 BtlPause_CheckOpen(void) {
    BtlPause *work = BtlPause_GetWork();
    s32 port;
    s32 i;

#ifdef PORT
    if (Battle_GetMode() == PORT_TRAIN_MODE && Port_TrainActive()) {
        /* Training: START no longer pauses (it plays the recording); L2 + D-pad down opens the pause menu. */
        if (Port_TrainPoll(gPad[0].gamePressed, gPad[0].gameHeld)) {
            work->pad = 0;
            return 1;
        }
        if (PadWatch_GetMissing(&port)) {
            work->pad = port;
            Battle_GetWork()->flags |= BATTLE_FLAG_PAUSE | BATTLE_FLAG_PAUSE_MENU;
            Adx_PauseSeVoice();
            Snd_SetPause(4, 1);
        }
        return 0;
    }
#endif
    for (i = 0; i < work->padCount; i++) {
        if (gPad[i].gamePressed & BTL_PAUSE_START) {
            work->pad = i;
            return 1;
        }
    }
    if (Battle_GetMode() != 7) {
#ifdef PORT
        /* (online nothing stops the fight, btl_seq.c: a pad pulled out does not either) */
        extern int Port_NetActive(void);
        if (Port_NetActive()) {
            return 0;
        }
#endif
        if (PadWatch_GetMissing(&port)) {
            work->pad = port;
            Battle_GetWork()->flags |= BATTLE_FLAG_PAUSE | BATTLE_FLAG_PAUSE_MENU;
            Adx_PauseSeVoice();
            Snd_SetPause(4, 1);
            return 0;
        }
    } else if (gPad[0].gamePressed & BTL_PAUSE_START) {
        return 1;
    }
    return 0;
}

/* Sequence condition "START pressed": while a loaded replay plays only pad 0 counts, else any pad of the list. */
s32 BtlPause_CheckStart(void) {
    BtlPause *work = BtlPause_GetWork();
    s32 i;

    if (BattleReplay_IsActive() && BattleReplay_IsLoaded()) {
        if (gPad[0].gamePressed & BTL_PAUSE_START) {
            return 1;
        }
        return 0;
    }
    for (i = 0; i < work->padCount; i++) {
        if (gPad[i].gamePressed & BTL_PAUSE_START) {
            return 1;
        }
    }
    return 0;
}

/* Returns the number of pads that may pause. */
s32 BtlPause_GetPadCount(void) {
    return BtlPause_GetWork()->padCount;
}

/* Sets the pad that owns the pause. */
void BtlPause_SetPad(s32 pad) {
    BtlPause_GetWork()->pad = pad;
}

/* The pad that drives the pause menu: the owner, or 0 while a loaded replay plays. */
s32 BtlPause_GetMenuPad(void) {
    BtlPause *work = BtlPause_GetWork();

    if (BattleReplay_IsActive() && BattleReplay_IsLoaded()) {
        return 0;
    }
    return work->pad;
}

/* The pad that owns the pause. */
s32 BtlPause_GetPad(void) {
    return BtlPause_GetWork()->pad;
}

#ifdef PORT
/* Replays of the online matches: once per frame of the fight, the life of both sides (per mille) for the graph. */
void Port_RepTick(void) {
    s32 side;
    s32 hp[2];

    if (!Port_RepRecording()) {
        return;
    }
    for (side = 0; side < 2; side++) {
        BtlMemberChr *chr = BtlChar_Get(side);
        BtlMemberGauge *g;

        hp[side] = 1000;
        if (chr != NULL) {
            g = BtlMember_GetActiveGauge(chr);
            if (g->healthMax > 0) {
                hp[side] = g->health * 1000 / g->healthMax;
            }
        }
    }
    Port_RepSample(hp[0], hp[1]);
}

/* Offline fights other than the training: SELECT turns P2's automatic Z Counter on / off. */
void Port_TrainSelectPoll(void) {
    if (gPad[0].gamePressed & 0x2000) { /* PADG_SELECT */
        if (!Jukebox_OnSelect()) { /* with the music player's "SELECT = next song" on, SELECT goes to it, in every mode, online too */
            Port_AutoZSelect();
        }
    }
}

/* Training mod: puts life, ki and blast of a fighter at a percentage of their maximum. */
static void PortTrain_SetGauge(s32 side, s32 stat, s32 pct) {
    BtlMemberChr *chr = BtlChar_Get(side);
    BtlMemberGauge *g;
    s32 v;

    if (chr == NULL) {
        return;
    }
    g = BtlMember_GetActiveGauge(chr);
    if (pct < 0) {
        pct = 0;
    }
    if (pct > 100) {
        pct = 100;
    }
    switch (stat) {
    case 0:
        v = g->healthMax * pct / 100;
        g->health = v < 1 ? 1 : v; /* never 0: a KO would end the training */
        break;
    case 1:
        g->ki = g->kiMax * pct / 100;
        break;
    default:
        g->blast = g->blastMax * pct / 100;
        break;
    }
}

/* Training mod: once per frame of the fight state (paused or not), applies what the F1 window asked for. */
void Port_TrainGaugeTick(void) {
    s32 side;
    s32 stat;

    if (Battle_GetMode() != PORT_TRAIN_MODE) {
        return;
    }
    if (gPortTrainRefill) {
        gPortTrainRefill = 0;
        for (side = 0; side < 2; side++) {
            for (stat = 0; stat < 3; stat++) {
                PortTrain_SetGauge(side, stat, 100);
            }
        }
    }
    for (side = 0; side < 2; side++) {
        for (stat = 0; stat < 3; stat++) {
            if (gPortTrainGauge[side].apply[stat] || gPortTrainGauge[side].hold[stat]) {
                gPortTrainGauge[side].apply[stat] = 0;
                PortTrain_SetGauge(side, stat, gPortTrainGauge[side].value[stat]);
            }
        }
    }
}
#endif
