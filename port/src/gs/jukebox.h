/* The internal music player (F1 > Musica): plays the mp3 / wav / flac files of a folder of the player's choice with its own
   volume, fade in / out (and an optional crossfade), random order, a loudness measured per song so they all play at a similar
   level, and the data for the visualiser. A sound-only feature: it touches nothing of the game's simulation, so it is the same
   in every offline and online mode. gs/jukebox.c; the window and the overlays are in gs/ui.cpp. */
#ifndef JUKEBOX_H
#define JUKEBOX_H
#ifdef __cplusplus
extern "C" {
#endif

#define JB_BANDS 64

void Jukebox_Frame(void);                          /* once per frame: fades, the end of a song, the next one */
void Jukebox_SetFolder(const char *path);          /* scans it (and its sub folders, one level) */
const char *Jukebox_Folder(void);
int Jukebox_Count(void);
const char *Jukebox_Name(int i);
int Jukebox_Current(void);                         /* the track playing (the newest if two overlap), -1 none */
int Jukebox_Playing(void);                         /* 1 playing, 0 stopped or paused */
void Jukebox_Play(int i);
void Jukebox_Next(void);
void Jukebox_Prev(void);
void Jukebox_Stop(void);
void Jukebox_PauseToggle(void);
int Jukebox_Paused(void);
void Jukebox_Seek(float fraction);
float Jukebox_Position(float *seconds, float *total); /* 0..1 */
int Jukebox_Analysing(int *done, int *total);      /* 1 while the loudness of the songs is being measured */
float Jukebox_TrackGain(int i);                    /* the multiplier the loudness measurement gives (1 = not measured yet) */

/* Options (kept in the settings: jb_*). get: set = 0. */
enum { JB_VOLUME, JB_RANDOM, JB_FADE_IN, JB_FADE_OUT, JB_CROSSFADE, JB_SELECT_NEXT, JB_NORMALISE, JB_NAME, JB_NAME_ALWAYS, JB_VIZ, JB_VIZ_SIZE, JB_AUTOPLAY, JB_FADE_SECS, JB_VIZ_X, JB_VIZ_Y, JB_VIZ_W, JB_NAME_X, JB_NAME_Y, JB_NAME_SCALE, JB_COUNT };
int Jukebox_Opt(int which, int set, int value);
void Jukebox_OptQuiet(int which, int value);       /* set without writing the settings file (while dragging); Jukebox_Opt(.., 1, ..) writes */

int Jukebox_OnSelect(void);                        /* the pad's SELECT in a fight: 1 = it went to "next song" */
int Jukebox_SelectOn(void);                        /* SELECT is the next song (the camera does not use it) */

/* The visualiser: JB_BANDS levels 0..1 of what is playing now (log spaced, 40 Hz .. 16 kHz); returns the bass level 0..1. */
float Jukebox_Spectrum(float *bands);
/* The name of the song that began last and how long ago (seconds), for the overlay. */
const char *Jukebox_NowName(double *age);

/* Testable pieces (no sound device): the loudness of a file and the spectrum of a block of samples. */
int Jukebox_Measure(const char *path, float *rmsDb, float *peak, double *seconds);
void Jukebox_FftBands(const float *mono2048, float *bands, int n, int rate);

#ifdef __cplusplus
}
#endif
#endif
