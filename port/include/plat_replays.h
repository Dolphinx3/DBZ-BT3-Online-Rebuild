/* Replays of the online matches (matchmaking and the lobby's rooms): every one is kept in replays/ as the game's own
   replay save file (0x1AC00 bytes: what BT3_REPLAY reads) next to a small text file (.meta) with who played, who won,
   how long it was and the course of the two fighters' life (the advantage graph). The newest rep_keep (30) are kept.
   plat_replays.c: integers and files only. The game's side (hud_e_d.c, battle_load.c) calls Port_Rep*; the F1 window
   ("Replays" tab, gs/ui.cpp) calls Port_RepUi_*. */
#ifndef PLAT_REPLAYS_H
#define PLAT_REPLAYS_H

#ifdef __cplusplus
extern "C" {
#endif

#define PORT_REP_MAX 64        /* entries the list can show */
#define PORT_REP_NAME 32
#define PORT_REP_GRAPH 720     /* samples of the graph (when it is full it keeps every other one: any length fits) */

/* ---- the game's side ---- */
void Port_RepBegin(int replayActive);          /* a fight starts */
void Port_RepSample(int hp0, int hp1);         /* once per frame of the fight: life of both sides, 0..1000 */
void Port_RepInput(int player, unsigned buttons, const unsigned char *stick); /* one fighter's input of one frame: kept without the game's 9000 frame limit */
void Port_RepPauseSet(int on);                                  /* the transport bar's pause button */
int Port_RepPaused(void);
void Port_RepRestart(int frame);                                /* start the fight again from its first frame and run to this one */
int Port_RepRestartPending(void);
int Port_RepRestartTake(void);                                  /* the frame to run to once it has started again (clears the request) */
int Port_RepViewTake(void);                                     /* 1 once per replay: the viewer's first frame */
int Port_RepMySide(void);                                       /* the side whoever recorded the replay played (0 / 1) */
void Port_RepSeekTo(int frame);                                 /* run on (not shown) up to this frame of the fight; a frame already played does nothing */
void Port_RepSpeedSet(int percent);                             /* 25..200: of the pace the replay was played at */
int Port_RepSpeedGet(void);
int Port_RepSpeedMilli(void);                                   /* the pace for plat_stub.c (1000 = the game's), 0 when this copy plays no replay */
int Port_RepSelfExe(char *exe, int exeSize);                    /* the path of this program */
int Port_SplitWanted(void);                                     /* a replay is to be shown in split screen (btl_cam.c) */
void Port_SplitSet(int on);
int Port_SplitGet(void);
void Port_SplitKeys(unsigned held);                             /* the shortcut L1 + L2 + R1 + R2 (the game's thread, each frame of a replay) */
int Port_RepSeeking(void);                                      /* playing a replay up to the frame it was asked to start from */
void Port_RepSetPriority(int level);                              /* -1 below normal, 0 normal, 1 above (Windows) */
int Port_RepPlayActive(void);                                  /* this copy plays a replay (BT3_REPLAY) */
const char *Port_RepPlayName(int side);                         /* the name of a side in the replay being played ("" if unknown) */
void Port_RepPlayInfo(int *frame, int *total);                  /* frames played so far / frames of the fight (30 a second) */
int Port_RepUi_Clock(int set, int value);                       /* show the clock while a replay plays (default on) */
int Port_RepLongPlay(int player, unsigned *buttons, unsigned char *stick);   /* playback of the whole recording (BT3_REPLAY_LONG); 1 = filled */
void Port_RepEnd(void);                        /* the fight is over: the replay is saved if it was an online match */
int Port_RepRecording(void);

/* ---- the F1 window's side ---- */
typedef struct PortRepInfo {
    char file[96];                 /* the replay file */
    char me[PORT_REP_NAME], opp[PORT_REP_NAME];
    char p[2][PORT_REP_NAME];      /* the names of side 0 (Player 1: the host) and side 1 (Player 2) */
    int winSide;                   /* 0 / 1 the side that won, -1 draw / unknown */
    int mySide;                    /* the side this copy played */
    int custom;                    /* 0 casual (matchmaking), 1 custom (a room: by code or address) */
    int won;                       /* 1 won, 0 lost, -1 draw / unknown */
    int seconds;
    long long when;                /* unix time */
    int graphN;
    int speedMilli;                /* the pace of the match it was played at (1000 or 1200): the replay's "x1" */
    int graphStepMs;               /* the time one sample of the graph stands for */
    short graph[PORT_REP_GRAPH];   /* my life minus the opponent's, -1000..1000 (above 0: I was ahead) */
} PortRepInfo;

int Port_RepUi_Scan(void);                                    /* reads the folder; returns the entries, newest first */
int Port_RepUi_Get(int i, PortRepInfo *out);
void Port_RepUi_Delete(int i);
int Port_RepUi_Play(int i, char *file, int fileSize, char *longFile, int longSize, unsigned *seed, char *exe, int exeSize); /* what the window needs to open the replay in a second copy of the game (BT3_REPLAY = file, BT3_REPLAY_LONG = longFile when not empty, BT3_REPLAY_SEED = seed); 1 = ok */
void Port_RepUi_History(const char *opp, int *wins, int *losses);
int Port_RepUi_Auto(int set, int value);                      /* record online matches (default on) */

#ifdef __cplusplus
}
#endif
#endif
