/*
 * The internal music player. See jukebox.h.
 *
 * Sound: the port's one miniaudio engine (snd_ma.c). Each song that plays is a "deck": a decoder, a data source that copies
 * what passes into a ring for the visualiser, and an ma_sound. Two decks can overlap (the crossfade, a manual skip). The
 * volume of a deck is the player's volume x the loudness gain of its song x the deck's envelope (the fade in / out), set once
 * per frame (Jukebox_Frame).
 *
 * Loudness: a worker thread decodes every song once (22 kHz, stereo) and measures its average level over the stretches that
 * are not silence (400 ms blocks above -50 dBFS) and its peak; the gain brings every song to the same average level without
 * letting the peak clip. The results are kept in jukebox_cache.txt, so a folder is measured once.
 *
 * None of this is read by the game's simulation: music only.
 */
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#include "../../third_party/miniaudio/miniaudio.h"

#include <dirent.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "jukebox.h"

extern void *PortMa_Engine(void); /* snd_ma.c: the ma_engine (made on first use), NULL without a sound device */
extern int Port_Setting(const char *name, int def);
extern void Port_SettingSave(const char *name, int value);
extern void Port_SettingsWrite(void);

#define MAX_TRACKS 1024
#define TARGET_RMS_DB (-17.0f) /* the average level every song is brought to */
#define FOLDER_FILE "ui_music.txt"
#define CACHE_FILE "jukebox_cache.txt"
#define RING 8192

typedef struct Track {
    char path[512];
    char name[96];
    long long size;
    long long mtime;
    float rmsDb, peak, gain;
    double seconds;
    int analysed; /* 1 measured, -1 could not be read */
} Track;

static Track sTr[MAX_TRACKS];
static int sN;
static char sFolder[512];
static pthread_mutex_t sLock = PTHREAD_MUTEX_INITIALIZER;
static int sThreadOn;
static volatile int sRescan;     /* the worker restarts its pass */
static volatile int sPriority[4] = {-1, -1, -1, -1}; /* songs to measure first (the one playing, the next ones) */

/* ---- the options ----------------------------------------------------------------------------------------------- */
static const char *kOptName[JB_COUNT] = {"jb_vol", "jb_random", "jb_fade_in", "jb_fade_out", "jb_crossfade", "jb_select", "jb_norm", "jb_name",
                                         "jb_name_always", "jb_viz", "jb_viz_size", "jb_autoplay", "jb_fade_secs", "jb_viz_x", "jb_viz_y", "jb_viz_w",
                                         "jb_name_x", "jb_name_y", "jb_name_scale"};
static const int kOptDef[JB_COUNT] = {60, 0, 1, 1, 0, 0, 1, 1, 0, 0, 50, 0, 3, 50, 95, 62, 2, 60, 100};
static int sOpt[JB_COUNT];
static int sOptRead;

static void opts_read(void) {
    if (!sOptRead) {
        int i;

        for (i = 0; i < JB_COUNT; i++) {
            sOpt[i] = Port_Setting(kOptName[i], kOptDef[i]);
        }
        sOptRead = 1;
    }
}

int Jukebox_Opt(int which, int set, int value) {
    if (which < 0 || which >= JB_COUNT) {
        return 0;
    }
    opts_read();
    if (set) {
        sOpt[which] = value;
        Port_SettingSave(kOptName[which], value);
        Port_SettingsWrite();
    }
    return sOpt[which];
}

void Jukebox_OptQuiet(int which, int value) {
    if (which >= 0 && which < JB_COUNT) {
        opts_read();
        sOpt[which] = value;
    }
}

/* ---- measuring ------------------------------------------------------------------------------------------------- */
int Jukebox_Measure(const char *path, float *rmsDb, float *peak, double *seconds) {
    enum { RATE = 22050, BLOCK = RATE * 400 / 1000 };
    ma_decoder dec;
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 2, RATE);
    static __thread float buf[4096 * 2];
    double blockSum = 0.0, gated = 0.0, pk = 0.0;
    ma_uint64 blockN = 0, gatedN = 0, total = 0;

    if (ma_decoder_init_file(path, &cfg, &dec) != MA_SUCCESS) {
        return 0;
    }
    for (;;) {
        ma_uint64 got = 0, i;

        ma_decoder_read_pcm_frames(&dec, buf, 4096, &got);
        if (got == 0) {
            break;
        }
        for (i = 0; i < got; i++) {
            float l = buf[i * 2], r = buf[i * 2 + 1];
            double a = fabs((double)l), b = fabs((double)r);

            if (a > pk) { pk = a; }
            if (b > pk) { pk = b; }
            blockSum += (double)l * (double)l + (double)r * (double)r;
            if (++blockN >= BLOCK) {
                double ms = blockSum / (2.0 * (double)blockN);

                if (ms > 1e-5) { /* above -50 dBFS: music, not the silence between its parts */
                    gated += ms;
                    gatedN++;
                }
                blockSum = 0.0;
                blockN = 0;
            }
        }
        total += got;
    }
    ma_decoder_uninit(&dec);
    *rmsDb = gatedN > 0 ? (float)(10.0 * log10(gated / (double)gatedN)) : -90.0f;
    *peak = (float)pk;
    *seconds = (double)total / (double)RATE;
    return total > 0;
}

static float gain_for(float rmsDb, float peak) {
    float g = powf(10.0f, (TARGET_RMS_DB - rmsDb) / 20.0f);

    if (g > 4.0f) { g = 4.0f; }
    if (g < 0.15f) { g = 0.15f; }
    if (peak > 0.001f && g * peak > 0.98f) {
        g = 0.98f / peak; /* never over full scale */
    }
    return g;
}

/* ---- the cache and the worker --------------------------------------------------------------------------------- */
static void cache_save(void) {
    FILE *fp = fopen(CACHE_FILE, "w");
    int i;

    if (fp == NULL) {
        return;
    }
    for (i = 0; i < sN; i++) {
        if (sTr[i].analysed == 1) {
            fprintf(fp, "%lld|%lld|%.2f|%.4f|%.2f|%s\n", sTr[i].size, sTr[i].mtime, sTr[i].rmsDb, sTr[i].peak, sTr[i].seconds, sTr[i].path);
        }
    }
    fclose(fp);
}

static void cache_apply(void) { /* sLock held */
    FILE *fp = fopen(CACHE_FILE, "r");
    char line[900];

    if (fp == NULL) {
        return;
    }
    while (fgets(line, sizeof(line), fp) != NULL) {
        long long size, mtime;
        float rms, pk;
        double sec;
        char *bar = line;
        int field, i;

        if (sscanf(line, "%lld|%lld|%f|%f|%lf|", &size, &mtime, &rms, &pk, &sec) != 5) {
            continue;
        }
        for (field = 0; field < 5 && bar != NULL; field++) {
            bar = strchr(bar, '|');
            bar = bar != NULL ? bar + 1 : NULL;
        }
        if (bar == NULL) {
            continue;
        }
        bar[strcspn(bar, "\r\n")] = '\0';
        for (i = 0; i < sN; i++) {
            if (sTr[i].analysed == 0 && sTr[i].size == size && sTr[i].mtime == mtime && strcmp(sTr[i].path, bar) == 0) {
                sTr[i].rmsDb = rms;
                sTr[i].peak = pk;
                sTr[i].seconds = sec;
                sTr[i].gain = gain_for(rms, pk);
                sTr[i].analysed = 1;
            }
        }
    }
    fclose(fp);
}

/* The worker's wait between looks for work (a timed wait on a condition nobody signals: portable where nanosleep is not). */
static void nap(int ms) {
    static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    static pthread_cond_t c = PTHREAD_COND_INITIALIZER;
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += (long)ms * 1000000L;
    ts.tv_sec += ts.tv_nsec / 1000000000L;
    ts.tv_nsec %= 1000000000L;
    pthread_mutex_lock(&m);
    pthread_cond_timedwait(&c, &m, &ts);
    pthread_mutex_unlock(&m);
}

static void *worker(void *arg) {
    int sinceSave = 0;

    (void)arg;
    for (;;) {
        int pick = -1, i;
        char path[512];
        float rms = 0.0f, pk = 0.0f;
        double sec = 0.0;
        int ok;

        pthread_mutex_lock(&sLock);
        for (i = 0; i < 4 && pick < 0; i++) {
            int p = sPriority[i];
            if (p >= 0 && p < sN && sTr[p].analysed == 0) {
                pick = p;
            }
        }
        for (i = 0; i < sN && pick < 0; i++) {
            if (sTr[i].analysed == 0) {
                pick = i;
            }
        }
        if (pick >= 0) {
            snprintf(path, sizeof(path), "%s", sTr[pick].path);
        }
        pthread_mutex_unlock(&sLock);
        if (pick < 0) {
            if (sinceSave > 0) {
                pthread_mutex_lock(&sLock);
                cache_save();
                pthread_mutex_unlock(&sLock);
                sinceSave = 0;
            }
            nap(150);
            continue;
        }
        ok = Jukebox_Measure(path, &rms, &pk, &sec);
        pthread_mutex_lock(&sLock);
        if (pick < sN && strcmp(sTr[pick].path, path) == 0) { /* (the folder did not change meanwhile) */
            if (ok) {
                sTr[pick].rmsDb = rms;
                sTr[pick].peak = pk;
                sTr[pick].seconds = sec;
                sTr[pick].gain = gain_for(rms, pk);
                sTr[pick].analysed = 1;
                sinceSave++;
            } else {
                sTr[pick].analysed = -1;
            }
            if (sinceSave >= 5) {
                cache_save();
                sinceSave = 0;
            }
        }
        pthread_mutex_unlock(&sLock);
    }
    return NULL;
}

int Jukebox_Analysing(int *done, int *total) {
    int i, d = 0, t = 0;

    for (i = 0; i < sN; i++) {
        t++;
        if (sTr[i].analysed != 0) {
            d++;
        }
    }
    if (done != NULL) { *done = d; }
    if (total != NULL) { *total = t; }
    return d < t;
}

float Jukebox_TrackGain(int i) {
    return i >= 0 && i < sN && sTr[i].analysed == 1 ? sTr[i].gain : 1.0f;
}

/* ---- the folder ------------------------------------------------------------------------------------------------ */
static int has_ext(const char *name) {
    size_t l = strlen(name);
    static const char *exts[] = {".mp3", ".wav", ".flac"};
    int e;

    for (e = 0; e < 3; e++) {
        size_t el = strlen(exts[e]);
        if (l > el) {
            size_t k;
            int same = 1;
            for (k = 0; k < el; k++) {
                char c = name[l - el + k];
                if (c >= 'A' && c <= 'Z') { c = (char)(c + 32); }
                if (c != exts[e][k]) { same = 0; }
            }
            if (same) {
                return 1;
            }
        }
    }
    return 0;
}

static int by_name(const void *a, const void *b) {
    const Track *x = (const Track *)a, *y = (const Track *)b;
    const char *p = x->name, *q = y->name;

    for (; *p && *q; p++, q++) {
        int c = *p, d = *q;
        if (c >= 'A' && c <= 'Z') { c += 32; }
        if (d >= 'A' && d <= 'Z') { d += 32; }
        if (c != d) {
            return c - d;
        }
    }
    return (int)(unsigned char)*p - (int)(unsigned char)*q;
}

static void scan_dir(const char *dir, int depth) { /* sLock held */
    DIR *d = opendir(dir);
    struct dirent *e;

    if (d == NULL) {
        return;
    }
    while ((e = readdir(d)) != NULL && sN < MAX_TRACKS) {
        char path[512];
        struct stat st;

        if (e->d_name[0] == '.') {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (stat(path, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (depth < 1) {
                scan_dir(path, depth + 1);
            }
        } else if (has_ext(e->d_name)) {
            Track *t = &sTr[sN];
            size_t n, i;

            memset(t, 0, sizeof(*t));
            snprintf(t->path, sizeof(t->path), "%s", path);
            snprintf(t->name, sizeof(t->name), "%s", e->d_name);
            n = strlen(t->name);
            if (n > 4 && strchr(t->name + n - 5, '.') != NULL) {
                t->name[strrchr(t->name, '.') - t->name] = '\0';
            }
            for (i = 0; t->name[i] != '\0'; i++) {
                if (t->name[i] == '_') {
                    t->name[i] = ' ';
                }
            }
            t->size = (long long)st.st_size;
            t->mtime = (long long)st.st_mtime;
            t->gain = 1.0f;
            sN++;
        }
    }
    closedir(d);
}

/* ---- the decks ------------------------------------------------------------------------------------------------- */
typedef struct TapDS {
    ma_data_source_base base;
    ma_decoder *dec;
} TapDS;

static float sRing[RING];
static volatile unsigned sRingPos;

static ma_result tap_read(ma_data_source *p, void *out, ma_uint64 n, ma_uint64 *read) {
    TapDS *t = (TapDS *)p;
    ma_uint64 got = 0, i;
    ma_result r = ma_decoder_read_pcm_frames(t->dec, out, n, &got);
    const float *f = (const float *)out;

    for (i = 0; i < got; i++) { /* the mono mix of what is about to be heard, for the visualiser (the decks add into one ring) */
        unsigned pos = sRingPos++ & (RING - 1);
        sRing[pos] = (f[i * 2] + f[i * 2 + 1]) * 0.5f;
    }
    *read = got;
    return r;
}
static ma_result tap_seek(ma_data_source *p, ma_uint64 frame) { return ma_decoder_seek_to_pcm_frame(((TapDS *)p)->dec, frame); }
static ma_result tap_format(ma_data_source *p, ma_format *f, ma_uint32 *ch, ma_uint32 *rate, ma_channel *map, size_t cap) {
    TapDS *t = (TapDS *)p;
    *f = ma_format_f32;
    *ch = 2;
    *rate = t->dec->outputSampleRate;
    if (map != NULL) {
        ma_channel_map_init_standard(ma_standard_channel_map_default, map, cap, 2);
    }
    return MA_SUCCESS;
}
static ma_result tap_cursor(ma_data_source *p, ma_uint64 *c) { return ma_decoder_get_cursor_in_pcm_frames(((TapDS *)p)->dec, c); }
static ma_result tap_length(ma_data_source *p, ma_uint64 *len) { (void)p; (void)len; return MA_NOT_IMPLEMENTED; } /* (the length comes from the measuring) */
static ma_data_source_vtable sTapVt = {tap_read, tap_seek, tap_format, tap_cursor, tap_length, NULL, 0};

typedef struct Deck {
    int used;
    int track;
    int phase;       /* 0 fading in / playing, 1 fading out */
    float env;       /* 0..1: the fade */
    float fadeSecs;  /* how long this deck's fade-out takes */
    ma_decoder dec;
    TapDS ds;
    ma_sound snd;
} Deck;

static Deck sDeck[2];
static int sCur = -1;          /* the song that began last */
static int sPending = -1;      /* a song to start when the decks are quiet (a manual skip without a crossfade) */
static int sNextStarted;       /* the crossfade has started the next song already */
static int sPaused;
static int sWasStopped = 1;
static char sNowName[96];
static double sNameT0 = -100.0;
static int sHist[16];
static int sHistN;
static unsigned sRnd = 0x9E3779B1u;

static double clock_s(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static float user_amp(void) {
    float v = (float)Jukebox_Opt(JB_VOLUME, 0, 0) / 100.0f;
    return v * v;
}

static float deck_gain(const Deck *d) {
    return Jukebox_Opt(JB_NORMALISE, 0, 0) ? Jukebox_TrackGain(d->track) : 1.0f;
}

static void deck_free(Deck *d) {
    if (d->used) {
        ma_sound_uninit(&d->snd);
        ma_data_source_uninit(&d->ds.base);
        ma_decoder_uninit(&d->dec);
        d->used = 0;
    }
}

static int deck_start(int idx, int fadeIn) {
    ma_engine *eng = (ma_engine *)PortMa_Engine();
    int di, i;
    Deck *d;
    ma_decoder_config cfg;
    ma_data_source_config dsc;
    int n = 0;

    if (eng == NULL || idx < 0 || idx >= sN) {
        return 0;
    }
    di = !sDeck[0].used ? 0 : !sDeck[1].used ? 1 : (sDeck[0].phase ? 0 : 1);
    d = &sDeck[di];
    deck_free(d);
    cfg = ma_decoder_config_init(ma_format_f32, 2, ma_engine_get_sample_rate(eng));
    if (ma_decoder_init_file(sTr[idx].path, &cfg, &d->dec) != MA_SUCCESS) {
        sTr[idx].analysed = sTr[idx].analysed == 0 ? -1 : sTr[idx].analysed;
        return 0;
    }
    d->ds.dec = &d->dec;
    dsc = ma_data_source_config_init();
    dsc.vtable = &sTapVt;
    if (ma_data_source_init(&dsc, &d->ds.base) != MA_SUCCESS) {
        ma_decoder_uninit(&d->dec);
        return 0;
    }
    if (ma_sound_init_from_data_source(eng, &d->ds, 0, NULL, &d->snd) != MA_SUCCESS) {
        ma_data_source_uninit(&d->ds.base);
        ma_decoder_uninit(&d->dec);
        return 0;
    }
    ma_sound_set_spatialization_enabled(&d->snd, MA_FALSE);
    d->used = 1;
    d->track = idx;
    d->phase = 0;
    d->env = fadeIn ? 0.0f : 1.0f;
    d->fadeSecs = (float)Jukebox_Opt(JB_FADE_SECS, 0, 0);
    ma_sound_set_volume(&d->snd, d->env * user_amp() * deck_gain(d));
    ma_sound_start(&d->snd);
    sCur = idx;
    snprintf(sNowName, sizeof(sNowName), "%s", sTr[idx].name);
    sNameT0 = clock_s();
    sNextStarted = 0;
    sWasStopped = 0;
    for (i = 15; i > 0; i--) {
        sHist[i] = sHist[i - 1];
    }
    sHist[0] = idx;
    if (sHistN < 16) { sHistN++; }
    pthread_mutex_lock(&sLock);
    sPriority[0] = idx;
    for (i = 1; i < 4; i++) { sPriority[i] = -1; }
    n = sN;
    pthread_mutex_unlock(&sLock);
    sPriority[1] = n > 1 ? (idx + 1) % n : -1; /* (the next one is measured next) */
    return 1;
}

static int pick_next(int cur) {
    int i, tries;

    if (sN <= 0) {
        return -1;
    }
    if (sN == 1) {
        return 0;
    }
    if (!Jukebox_Opt(JB_RANDOM, 0, 0)) {
        return (cur + 1 + sN) % sN;
    }
    for (tries = 0; tries < 40; tries++) {
        int c, recent = 0, lim = sN > 9 ? 8 : sN - 1;

        sRnd = sRnd * 1664525u + 1013904223u;
        c = (int)((sRnd >> 8) % (unsigned)sN);
        if (c == cur) {
            continue;
        }
        for (i = 0; i < sHistN && i < lim; i++) {
            if (sHist[i] == c) { recent = 1; }
        }
        if (!recent || tries > 30) {
            return c;
        }
    }
    return (cur + 1) % sN;
}

static int any_deck(void) { return sDeck[0].used || sDeck[1].used; }

static void begin_fade_out(Deck *d, float secs) {
    if (d->used && d->phase == 0) {
        d->phase = 1;
        d->fadeSecs = secs;
    }
}

/* A song by the player's own choice (the list, "next", "previous"): the one playing fades out (a short fade; at once without
   the fade option) and the new one comes in, over it with the crossfade, after it without. */
static void go_to(int idx) {
    int fo = Jukebox_Opt(JB_FADE_OUT, 0, 0), cross = Jukebox_Opt(JB_CROSSFADE, 0, 0), fi = Jukebox_Opt(JB_FADE_IN, 0, 0);
    int d;

    sPaused = 0;
    sPending = -1;
    if (!any_deck()) {
        deck_start(idx, fi);
        return;
    }
    if (!fo) {
        for (d = 0; d < 2; d++) { deck_free(&sDeck[d]); }
        deck_start(idx, fi);
        return;
    }
    for (d = 0; d < 2; d++) { begin_fade_out(&sDeck[d], 0.7f); }
    if (cross) {
        deck_start(idx, fi);
    } else {
        sPending = idx;
    }
}

void Jukebox_Play(int i) { if (i >= 0 && i < sN) { go_to(i); } }
void Jukebox_Next(void) { if (sN > 0) { go_to(pick_next(sCur >= 0 ? sCur : -1)); } }
void Jukebox_Prev(void) {
    if (sN > 0) {
        go_to(sHistN > 1 ? sHist[1] : (sCur - 1 + sN) % sN);
        if (sHistN > 1) { /* (going back does not leave the song it came from first in the history) */
            int k;
            for (k = 0; k + 2 < 16 && k + 2 < sHistN; k++) { sHist[k] = sHist[k + 2]; }
            sHistN = sHistN > 2 ? sHistN - 2 : 0;
        }
    }
}
void Jukebox_Stop(void) {
    int d;

    for (d = 0; d < 2; d++) { deck_free(&sDeck[d]); }
    sPending = -1;
    sPaused = 0;
    sWasStopped = 1;
}
void Jukebox_PauseToggle(void) {
    int d;

    if (!any_deck()) {
        if (sN > 0) { go_to(sCur >= 0 ? sCur : pick_next(-1)); }
        return;
    }
    sPaused = !sPaused;
    for (d = 0; d < 2; d++) {
        if (sDeck[d].used) {
            if (sPaused) { ma_sound_stop(&sDeck[d].snd); } else { ma_sound_start(&sDeck[d].snd); }
        }
    }
}
int Jukebox_Paused(void) { return sPaused; }
int Jukebox_Playing(void) { return any_deck() && !sPaused; }
int Jukebox_Current(void) { return any_deck() ? sCur : -1; }
int Jukebox_Count(void) { return sN; }
const char *Jukebox_Name(int i) { return i >= 0 && i < sN ? sTr[i].name : ""; }
const char *Jukebox_Folder(void) { return sFolder; }
const char *Jukebox_NowName(double *age) {
    *age = clock_s() - sNameT0;
    return sNowName;
}

float Jukebox_Position(float *seconds, float *total) {
    int d, best = -1;
    ma_uint64 cur = 0;
    float pos, len;

    for (d = 0; d < 2; d++) {
        if (sDeck[d].used && (best < 0 || sDeck[d].track == sCur)) { best = d; }
    }
    if (best < 0) {
        if (seconds != NULL) { *seconds = 0.0f; }
        if (total != NULL) { *total = 0.0f; }
        return 0.0f;
    }
    ma_sound_get_cursor_in_pcm_frames(&sDeck[best].snd, &cur);
    {
        ma_engine *eng = (ma_engine *)PortMa_Engine();
        pos = eng != NULL ? (float)cur / (float)ma_engine_get_sample_rate(eng) : 0.0f;
    }
    len = (float)sTr[sDeck[best].track].seconds;
    if (seconds != NULL) { *seconds = pos; }
    if (total != NULL) { *total = len; }
    return len > 0.0f ? (pos / len > 1.0f ? 1.0f : pos / len) : 0.0f;
}

void Jukebox_Seek(float fraction) {
    int d;

    for (d = 0; d < 2; d++) {
        if (sDeck[d].used && sDeck[d].track == sCur && sTr[sCur].seconds > 0.0) {
            ma_engine *eng = (ma_engine *)PortMa_Engine();
            if (eng != NULL) {
                fraction = fraction < 0.0f ? 0.0f : fraction > 0.999f ? 0.999f : fraction;
                ma_sound_seek_to_pcm_frame(&sDeck[d].snd, (ma_uint64)((double)fraction * sTr[sCur].seconds * (double)ma_engine_get_sample_rate(eng)));
            }
        }
    }
}

static void folder_restore(void);
static volatile int sNextReq;

/* Once per frame: the fades, the end of a song, the next one. */
void Jukebox_Frame(void) {
    static double last = 0.0;
    double now = clock_s(), dt = now - last;
    int d, fi, fo, cross;

    last = now;
    if (dt < 0.0 || dt > 0.1) {
        dt = 0.016;
    }
    opts_read();
    folder_restore(); /* (the folder of the last run, once) */
    if (sNextReq) {
        sNextReq = 0;
        Jukebox_Next();
    }
    if (!sThreadOn && sN > 0) { /* the measuring starts with the first folder */
        pthread_t th;
        sThreadOn = 1;
        if (pthread_create(&th, NULL, worker, NULL) == 0) {
            pthread_detach(th);
        }
    }
    if (sWasStopped && !any_deck() && sN > 0 && Jukebox_Opt(JB_AUTOPLAY, 0, 0) && sNameT0 < 0.0) {
        sNameT0 = 0.0; /* (once) */
        go_to(pick_next(-1));
    }
    fi = Jukebox_Opt(JB_FADE_IN, 0, 0);
    fo = Jukebox_Opt(JB_FADE_OUT, 0, 0);
    cross = Jukebox_Opt(JB_CROSSFADE, 0, 0);
    for (d = 0; d < 2; d++) {
        Deck *k = &sDeck[d];
        double secs;
        ma_uint64 cur = 0;
        int ended;
        float target;

        if (!k->used) {
            continue;
        }
        ended = ma_sound_at_end(&k->snd) ? 1 : 0;
        if (!sPaused) {
            if (k->phase == 0) {
                float inSecs = (float)Jukebox_Opt(JB_FADE_SECS, 0, 0);
                if (fi && inSecs > 0.1f) {
                    k->env += (float)dt / inSecs;
                    if (k->env > 1.0f) { k->env = 1.0f; }
                } else {
                    k->env = 1.0f;
                }
                secs = sTr[k->track].seconds;
                if (fo && secs > 0.0) { /* the fade out begins as many seconds before the end as it lasts */
                    ma_engine *eng = (ma_engine *)PortMa_Engine();
                    float rate = eng != NULL ? (float)ma_engine_get_sample_rate(eng) : 48000.0f;
                    float fs = (float)Jukebox_Opt(JB_FADE_SECS, 0, 0);
                    ma_sound_get_cursor_in_pcm_frames(&k->snd, &cur);
                    if ((double)cur / (double)rate >= secs - (double)fs - 0.05) {
                        begin_fade_out(k, fs);
                    }
                }
            }
            if (k->phase == 1) {
                k->env -= (float)dt / (k->fadeSecs > 0.1f ? k->fadeSecs : 0.1f);
                if (k->env < 0.0f) { k->env = 0.0f; }
                if (cross && !sNextStarted && sPending < 0 && !(sDeck[1 - d].used)) { /* the crossfade: the next one comes in as this goes out */
                    sNextStarted = 1;
                    deck_start(pick_next(k->track), fi);
                }
            }
        }
        target = k->env * user_amp() * deck_gain(k);
        ma_sound_set_volume(&k->snd, target); /* (every frame: the steps are a few thousandths, not heard) */
        if (k->used && (ended || (k->phase == 1 && k->env <= 0.0f))) {
            int wasLast = !sDeck[1 - d].used;
            int finished = k->track;
            deck_free(k);
            if (wasLast && sPending < 0 && !sNextStarted && !sPaused) {
                deck_start(pick_next(finished), fi); /* the song ended and nothing is coming: the next one */
            }
        }
    }
    if (sPending >= 0 && !any_deck()) {
        int p = sPending;
        sPending = -1;
        deck_start(p, fi);
    }
}

void Jukebox_SetFolder(const char *path) {
    int i;

    Jukebox_Stop();
    pthread_mutex_lock(&sLock);
    snprintf(sFolder, sizeof(sFolder), "%s", path != NULL ? path : "");
    sN = 0;
    sCur = -1;
    sHistN = 0;
    for (i = 0; i < 4; i++) { sPriority[i] = -1; }
    if (sFolder[0] != '\0') {
        size_t l = strlen(sFolder);
        while (l > 1 && (sFolder[l - 1] == '/' || sFolder[l - 1] == '\\')) { sFolder[--l] = '\0'; }
        scan_dir(sFolder, 0);
        qsort(sTr, (size_t)sN, sizeof(Track), by_name);
        cache_apply();
    }
    {
        FILE *fp = fopen(FOLDER_FILE, "w");
        if (fp != NULL) {
            fprintf(fp, "%s\n", sFolder);
            fclose(fp);
        }
    }
    pthread_mutex_unlock(&sLock);
}

/* The folder kept from the last run, read once. */
static void folder_restore(void) {
    static int done;
    FILE *fp;
    char line[512];

    if (done) {
        return;
    }
    done = 1;
    fp = fopen(FOLDER_FILE, "r");
    if (fp != NULL) {
        if (fgets(line, sizeof(line), fp) != NULL) {
            line[strcspn(line, "\r\n")] = '\0';
            if (line[0] != '\0') {
                fclose(fp);
                Jukebox_SetFolder(line);
                return;
            }
        }
        fclose(fp);
    }
}

/* ---- SELECT ------------------------------------------------------------------------------------------------------- */
/* (Asked from the game's thread: the request is taken by Jukebox_Frame, on the window's thread, where the decks are handled.) */
int Jukebox_SelectOn(void) {
    return sOptRead && sOpt[JB_SELECT_NEXT] && sN > 0;
}
int Jukebox_OnSelect(void) {
    if (!Jukebox_SelectOn()) {
        return 0;
    }
    sNextReq = 1;
    return 1;
}

/* ---- the spectrum ------------------------------------------------------------------------------------------------- */
static void fft(float *re, float *im, int n) {
    int i, j = 0, k;

    for (i = 1; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) { j ^= bit; }
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (k = 2; k <= n; k <<= 1) {
        float ang = -6.2831853f / (float)k;
        float wr = cosf(ang), wi = sinf(ang);
        for (i = 0; i < n; i += k) {
            float cr = 1.0f, ci = 0.0f;
            int m;
            for (m = 0; m < k / 2; m++) {
                float ur = re[i + m], ui = im[i + m];
                float vr = re[i + m + k / 2] * cr - im[i + m + k / 2] * ci;
                float vi = re[i + m + k / 2] * ci + im[i + m + k / 2] * cr;
                re[i + m] = ur + vr; im[i + m] = ui + vi;
                re[i + m + k / 2] = ur - vr; im[i + m + k / 2] = ui - vi;
                { float nr = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = nr; }
            }
        }
    }
}

void Jukebox_FftBands(const float *x, float *bands, int n, int rate) {
    static float re[2048], im[2048], win[2048];
    static int init;
    int i, b;

    if (!init) {
        for (i = 0; i < 2048; i++) { win[i] = 0.5f - 0.5f * cosf(6.2831853f * (float)i / 2047.0f); }
        init = 1;
    }
    for (i = 0; i < 2048; i++) { re[i] = x[i] * win[i]; im[i] = 0.0f; }
    fft(re, im, 2048);
    for (b = 0; b < n; b++) {
        float f0 = 40.0f * powf(16000.0f / 40.0f, (float)b / (float)n), f1 = 40.0f * powf(16000.0f / 40.0f, (float)(b + 1) / (float)n);
        int k0 = (int)(f0 * 2048.0f / (float)rate), k1 = (int)(f1 * 2048.0f / (float)rate), k;
        float acc = 0.0f, db;
        if (k1 <= k0) { k1 = k0 + 1; }
        if (k1 > 1023) { k1 = 1023; }
        for (k = k0; k < k1; k++) {
            float m = sqrtf(re[k] * re[k] + im[k] * im[k]) / 512.0f; /* a full scale sine reads about 1 */
            if (m > acc) { acc = m; }
        }
        db = 20.0f * log10f(acc + 1e-6f) + 3.0f * log2f((f0 + f1) / 400.0f > 0.1f ? (f0 + f1) / 400.0f : 0.1f); /* (a little tilt up: music falls off with pitch) */
        bands[b] = (db + 66.0f) / 60.0f;
        if (bands[b] < 0.0f) { bands[b] = 0.0f; }
        if (bands[b] > 1.0f) { bands[b] = 1.0f; }
    }
}

float Jukebox_Spectrum(float *bands) {
    float x[2048], bass = 0.0f;
    unsigned end = sRingPos, i;
    int b;
    ma_engine *eng = (ma_engine *)PortMa_Engine();

    for (i = 0; i < 2048; i++) { x[i] = sRing[(end - 2048 + i) & (RING - 1)]; }
    Jukebox_FftBands(x, bands, JB_BANDS, eng != NULL ? (int)ma_engine_get_sample_rate(eng) : 48000);
    for (b = 0; b < 6; b++) { bass += bands[b]; }
    return bass / 6.0f;
}
