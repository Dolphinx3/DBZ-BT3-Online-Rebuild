/* Training mod (modo "Entrenamiento"): record a sequence of P1's inputs, replay it on the dummy (or on P1), and set
   life / ki / blast of both fighters from the F1 window.

   plat_training.c holds the data and the logic (integers only: it is compiled with the PS2 float model).
   The game side (src/battle/hud_e_d.c, src/battle/btl_input.c) calls in under #ifdef PORT; the F1 window
   (gs/ui.cpp, "Entrenamiento" tab) calls the Port_TrainUi_* functions from the window's thread. */
#ifndef PLAT_TRAINING_H
#define PLAT_TRAINING_H

#ifdef __cplusplus
extern "C" {
#endif

#define PORT_TRAIN_MODE 6        /* Battle_GetMode() of the training fight */
#define PORT_TRAIN_SLOTS 16      /* recordings kept */
#define PORT_TRAIN_NAME 32       /* name length incl. the terminator */
#define PORT_TRAIN_FRAMES 7200   /* longest recording, in simulated frames */

enum { PORT_TRAIN_IDLE = 0, PORT_TRAIN_RECORDING = 1, PORT_TRAIN_PLAYING = 2 };

/* ---- the game's side -------------------------------------------------------------------------------------- */
void Port_TrainBattleReset(int training);              /* a fight starts / restarts (training: 1 in the training mode): stops recording and playback */
void Port_TrainBattleEnd(void);                        /* the fight is over */
int Port_AutoZRoll(int player);                        /* auto Z Counter: 1 = counter this hit (offline only, player 0 / 1; the caller has checked the game's own conditions) */
int Port_CamSelectOff(void);                          /* 1 = SELECT does not step the camera (offline) */
void Port_AutoZSelect(void);                           /* SELECT outside the training fight: P2 auto Z Counter on / off */
int Port_TrainActive(void);                           /* 1 = a training fight (not online) is running */
int Port_TrainPoll(unsigned pressed, unsigned held);   /* once per fight frame with pad 0's game buttons (PADG_*); 1 = open the pause menu */
int Port_TrainFilter(int player, unsigned *buttons, unsigned char *stick); /* BtlInput_BuildRecord: 1 = buttons / stick were replaced */

/* Gauge requests (the game's side applies them, in hud_e_d.c: Port_TrainGaugeTick). */
typedef struct PortTrainGauge {
    int value[3];  /* life, ki, blast: percent of the maximum, 0..100 (life is kept at 1 or more) */
    int hold[3];   /* 1 = put the value back every frame */
    int apply[3];  /* 1 = set it once, now (cleared by the game's side) */
} PortTrainGauge;
extern volatile PortTrainGauge gPortTrainGauge[2];     /* [0] = P1 (left), [1] = P2 (right) */
extern volatile int gPortTrainRefill;                  /* 1 = fill life, ki and blast of both, once */

/* ---- the F1 window's side --------------------------------------------------------------------------------- */
typedef struct PortTrainInfo {
    int state;          /* PORT_TRAIN_* */
    int frames;         /* recording: frames so far; playing: frames played */
    int total;          /* playing: frames of the recording */
    int slot;           /* the selected recording, -1 = none */
    int count;          /* recordings kept */
    int active;         /* a training fight is running */
} PortTrainInfo;

void Port_TrainUi_Info(PortTrainInfo *out);
int Port_TrainUi_SlotInfo(int slot, char *name, int nameSize, int *frames); /* 1 = the slot exists */
void Port_TrainUi_Select(int slot);
void Port_TrainUi_Rename(int slot, const char *name);
int Port_TrainUi_Duplicate(int slot);   /* returns the new slot, -1 when full */
void Port_TrainUi_Delete(int slot);
void Port_TrainUi_Move(int slot, int delta);
void Port_TrainUi_Play(void);           /* starts / stops playback of the selected recording */
void Port_TrainUi_Record(void);         /* starts / stops recording */
void Port_TrainUi_AutoZGet(int side, int *enabled, int *pct);   /* side 0 = P1 (left), 1 = P2 (right) */
void Port_TrainUi_AutoZSet(int side, int enabled, int pct);
int Port_TrainUi_Loop(int set, int value);      /* set = 0: read; 1: write */
int Port_TrainUi_Target(int set, int value);    /* who plays the recording: 0 = P2 (the dummy), 1 = P1 */

#ifdef __cplusplus
}
#endif
#endif
