// The F1 window's sounds and backdrop (the widgets' own animations are in port/third_party/imgui/imgui_fx.h).
//
// Sounds: short tones made here (no files): a soft tick when the pointer comes onto something, a rounder one when the pad's
// cursor moves, a click, a rising / falling pair for a switch, a sweep between tabs, a chord when the window opens. They play
// on a stream of their own on the default output, so the game's sound is left alone. Volume 0 = none.
// Backdrop: slow drifting light behind the window, a sweep of light now and then, a lit header and edge.
#include <SDL3/SDL.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "imgui.h"
#include "imgui_fx.h"

extern "C" {
int Port_Setting(const char *name, int def);
void Port_SettingSave(const char *name, int value);
void Port_SettingsWrite(void);
}

int gImFxVisual = 1;
float gImFxAccent[3] = {1.0f, 150.0f / 255.0f, 40.0f / 255.0f}; // the default: orange
static int sSfxVol = 55;     // 0..100
static bool sInit = false, sFailed = false;
static SDL_AudioStream *sStream = NULL;
static const int kRate = 48000;
static const int kKinds = 10;
static int16_t *sBank[kKinds];
static int sBankLen[kKinds];
static Uint64 sLastPlay[kKinds];
static bool sLoaded = false;

static void settings_load() {
    if (sLoaded) {
        return;
    }
    sLoaded = true;
    gImFxVisual = Port_Setting("ui_fx", 1) != 0;
    sSfxVol = Port_Setting("ui_sfx", 55);
    int packed = Port_Setting("ui_accent", 0xFF9628);
    gImFxAccent[0] = (float)((packed >> 16) & 255) / 255.0f;
    gImFxAccent[1] = (float)((packed >> 8) & 255) / 255.0f;
    gImFxAccent[2] = (float)(packed & 255) / 255.0f;
    if (sSfxVol < 0 || sSfxVol > 100) {
        sSfxVol = 55;
    }
}

void UiFx_GetAccent(float *rgb) { settings_load(); rgb[0] = gImFxAccent[0]; rgb[1] = gImFxAccent[1]; rgb[2] = gImFxAccent[2]; }
void UiFx_SetAccent(float r, float g, float b) {
    settings_load();
    gImFxAccent[0] = r < 0 ? 0 : r > 1 ? 1 : r;
    gImFxAccent[1] = g < 0 ? 0 : g > 1 ? 1 : g;
    gImFxAccent[2] = b < 0 ? 0 : b > 1 ? 1 : b;
    Port_SettingSave("ui_accent", ((int)(gImFxAccent[0] * 255.0f + 0.5f) << 16) | ((int)(gImFxAccent[1] * 255.0f + 0.5f) << 8) | (int)(gImFxAccent[2] * 255.0f + 0.5f));
    Port_SettingsWrite();
}
int UiFx_Visual() { settings_load(); return gImFxVisual; }
int UiFx_Volume() { settings_load(); return sSfxVol; }
void UiFx_SetVisual(int on) { settings_load(); gImFxVisual = on != 0; Port_SettingSave("ui_fx", gImFxVisual); Port_SettingsWrite(); }
void UiFx_SetVolume(int v) {
    settings_load();
    sSfxVol = v < 0 ? 0 : v > 100 ? 100 : v;
    if (sStream != NULL) {
        SDL_SetAudioStreamGain(sStream, (float)sSfxVol / 100.0f);
    }
    Port_SettingSave("ui_sfx", sSfxVol);
    Port_SettingsWrite();
}

// A tone that glides from f0 to f1, with a fast attack and a smooth fade, and a little of its octave for body.
static void tone(int16_t *out, int n, int offset, float f0, float f1, float vol, float attack, float decayPow) {
    float phase = 0.0f;
    for (int i = 0; i < n; i++) {
        float t = (float)i / (float)n;
        float f = f0 + (f1 - f0) * t;
        phase += 2.0f * 3.14159265f * f / (float)kRate;
        float env = (t < attack ? t / attack : 1.0f) * powf(1.0f - t, decayPow);
        float s = (sinf(phase) + 0.25f * sinf(phase * 2.0f)) * env * vol;
        int v = (int)out[offset + i] + (int)(s * 32000.0f);
        out[offset + i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
}

static int16_t *make(int kind, int *len) {
    int n = 0;
    switch (kind) {
    case IMFX_HOVER: n = kRate * 28 / 1000; break;
    case IMFX_MOVE: n = kRate * 45 / 1000; break;
    case IMFX_CLICK: n = kRate * 90 / 1000; break;
    case IMFX_ON: case IMFX_OFF: n = kRate * 130 / 1000; break;
    case IMFX_SLIDE: n = kRate * 22 / 1000; break;
    case IMFX_TAB: n = kRate * 140 / 1000; break;
    case IMFX_OPEN: n = kRate * 320 / 1000; break;
    case IMFX_CLOSE: n = kRate * 220 / 1000; break;
    default: return NULL;
    }
    int16_t *b = (int16_t *)calloc((size_t)n, sizeof(int16_t));
    if (b == NULL) {
        return NULL;
    }
    switch (kind) {
    case IMFX_HOVER: tone(b, n, 0, 2100.0f, 2300.0f, 0.10f, 0.08f, 2.0f); break;
    case IMFX_MOVE: tone(b, n, 0, 1050.0f, 1500.0f, 0.16f, 0.06f, 1.7f); break;
    case IMFX_CLICK: tone(b, n / 2, 0, 640.0f, 980.0f, 0.22f, 0.05f, 1.5f); tone(b, n / 2, n / 2, 980.0f, 1420.0f, 0.20f, 0.05f, 1.8f); break;
    case IMFX_ON: tone(b, n / 2, 0, 620.0f, 700.0f, 0.20f, 0.05f, 1.4f); tone(b, n / 2, n / 2, 930.0f, 1000.0f, 0.22f, 0.05f, 1.8f); break;
    case IMFX_OFF: tone(b, n / 2, 0, 930.0f, 860.0f, 0.20f, 0.05f, 1.4f); tone(b, n / 2, n / 2, 620.0f, 560.0f, 0.20f, 0.05f, 1.8f); break;
    case IMFX_SLIDE: tone(b, n, 0, 1650.0f, 1750.0f, 0.09f, 0.1f, 1.8f); break;
    case IMFX_TAB: tone(b, n, 0, 380.0f, 1100.0f, 0.17f, 0.12f, 1.6f); break;
    case IMFX_OPEN:
        tone(b, n, 0, 523.0f, 523.0f, 0.12f, 0.10f, 1.5f);
        tone(b, n * 9 / 10, n / 10, 659.0f, 659.0f, 0.12f, 0.10f, 1.5f);
        tone(b, n * 8 / 10, n * 2 / 10, 784.0f, 784.0f, 0.12f, 0.10f, 1.5f);
        break;
    case IMFX_CLOSE:
        tone(b, n, 0, 784.0f, 523.0f, 0.14f, 0.06f, 1.6f);
        tone(b, n * 8 / 10, n * 2 / 10, 659.0f, 392.0f, 0.10f, 0.06f, 1.6f);
        break;
    }
    *len = n;
    return b;
}

static void sound_init() {
    sInit = true;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        sFailed = true;
        return;
    }
    SDL_AudioSpec spec;
    SDL_zero(spec);
    spec.format = SDL_AUDIO_S16;
    spec.channels = 1;
    spec.freq = kRate;
    sStream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (sStream == NULL) {
        sFailed = true;
        return;
    }
    SDL_SetAudioStreamGain(sStream, (float)sSfxVol / 100.0f);
    SDL_ResumeAudioStreamDevice(sStream);
    for (int k = 1; k < kKinds; k++) {
        sBank[k] = make(k, &sBankLen[k]);
    }
}

void ImFx_Sound(int kind) {
    settings_load();
    if (sSfxVol <= 0 || sFailed || kind < 1 || kind >= kKinds) {
        return;
    }
    if (!sInit) {
        sound_init();
        if (sFailed) {
            return;
        }
    }
    if (sBank[kind] == NULL) {
        return;
    }
    // (a hover or a pad step is not played again before its own length, and the stream never holds more than a few sounds)
    Uint64 now = SDL_GetTicks();
    Uint64 gap = kind == IMFX_HOVER ? 45 : kind == IMFX_MOVE ? 30 : kind == IMFX_SLIDE ? 40 : 0;
    if (now - sLastPlay[kind] < gap || SDL_GetAudioStreamQueued(sStream) > kRate / 2 * (int)sizeof(int16_t)) {
        return;
    }
    sLastPlay[kind] = now;
    SDL_PutAudioStreamData(sStream, sBank[kind], sBankLen[kind] * (int)sizeof(int16_t));
}

// A soft round light: a fan of triangles whose middle is tinted and whose rim is clear, so the falloff is smooth (no rings).
static void orb(ImDrawList *dl, ImVec2 c, float r, int rr, int gg, int bb, int alpha) {
    const int N = 40;
    const ImVec2 uv = dl->_Data->TexUvWhitePixel;
    dl->PrimReserve(N * 3, N + 1);
    ImDrawIdx base = (ImDrawIdx)dl->_VtxCurrentIdx;
    dl->PrimWriteVtx(c, uv, IM_COL32(rr, gg, bb, alpha));
    for (int i = 0; i < N; i++) {
        float a = 6.2831853f * (float)i / (float)N;
        dl->PrimWriteVtx(ImVec2(c.x + cosf(a) * r, c.y + sinf(a) * r), uv, IM_COL32(rr, gg, bb, 0));
    }
    for (int i = 0; i < N; i++) {
        dl->PrimWriteIdx(base);
        dl->PrimWriteIdx((ImDrawIdx)(base + 1 + i));
        dl->PrimWriteIdx((ImDrawIdx)(base + 1 + (i + 1) % N));
    }
}

// The window's backdrop, drawn into its own draw list right after Begin (behind everything that comes after).
void UiFx_Backdrop(ImDrawList *dl, ImVec2 mn, ImVec2 mx, float rounding, bool hasWallpaper) {
    if (!gImFxVisual) {
        return;
    }
    settings_load();
    const int ar = (int)(gImFxAccent[0] * 255.0f), ag = (int)(gImFxAccent[1] * 255.0f), ab = (int)(gImFxAccent[2] * 255.0f);
    float hh, ss, vv, r2, g2, b2, r3, g3, b3;
    ImGui::ColorConvertRGBtoHSV(gImFxAccent[0], gImFxAccent[1], gImFxAccent[2], hh, ss, vv);
    ImGui::ColorConvertHSVtoRGB(fmodf(hh + 0.55f, 1.0f), ss, vv, r2, g2, b2);
    ImGui::ColorConvertHSVtoRGB(fmodf(hh + 0.92f, 1.0f), ss, vv, r3, g3, b3);
    float t = (float)ImGui::GetTime();
    float w = mx.x - mn.x, h = mx.y - mn.y;
    dl->PushClipRect(mn, mx, true);
    // a lighter band at the top, fading down: the header
    dl->AddRectFilledMultiColor(mn, ImVec2(mx.x, mn.y + 90.0f), IM_COL32(ar, ag, ab, 34), IM_COL32(ar, ag, ab, 22), IM_COL32(ar, ag, ab, 0), IM_COL32(ar, ag, ab, 0));
    // slow drifting lights (concentric discs make a soft falloff)
    struct Orb { float fx, fy, sp, r; ImU32 rgb; } orbs[3] = {
        {0.22f, 0.20f, 0.07f, 0.46f, IM_COL32(ar, ag, ab, 0)},
        {0.80f, 0.70f, 0.05f, 0.52f, IM_COL32((int)(r2 * 255.0f), (int)(g2 * 255.0f), (int)(b2 * 255.0f), 0)},
        {0.55f, 0.95f, 0.09f, 0.40f, IM_COL32((int)(r3 * 255.0f), (int)(g3 * 255.0f), (int)(b3 * 255.0f), 0)},
    };
    for (int i = 0; i < (hasWallpaper ? 1 : 3); i++) { // (over a wallpaper only the accent's own light: the picture is the background)
        float cx = mn.x + w * (orbs[i].fx + 0.12f * sinf(t * orbs[i].sp * 6.0f + (float)i * 2.1f));
        float cy = mn.y + h * (orbs[i].fy + 0.10f * cosf(t * orbs[i].sp * 5.0f + (float)i * 1.3f));
        float r = ImMax(w, h) * orbs[i].r;
        ImU32 rgb = orbs[i].rgb;
        int rr = (int)(rgb & 0xFF), gg = (int)((rgb >> 8) & 0xFF), bb = (int)((rgb >> 16) & 0xFF);
        orb(dl, ImVec2(cx, cy), r, rr, gg, bb, 44);
    }
    // a band of light that crosses the window every so often
    float cycle = fmodf(t, 9.0f) / 3.2f;
    if (cycle < 1.0f) {
        float x = mn.x - 80.0f + (w + 160.0f) * cycle;
        dl->AddRectFilledMultiColor(ImVec2(x - 40.0f, mn.y), ImVec2(x, mx.y), IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 14), IM_COL32(255, 255, 255, 14), IM_COL32(255, 255, 255, 0));
        dl->AddRectFilledMultiColor(ImVec2(x, mn.y), ImVec2(x + 40.0f, mx.y), IM_COL32(255, 255, 255, 14), IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 14));
    }
    dl->PopClipRect();
    // the edge: a thin accent line that breathes, and a bright line under the header
    float pulse = 0.55f + 0.25f * sinf(t * 1.6f);
    dl->AddRect(mn, mx, IM_COL32(ar, ag, ab, (int)(pulse * 120.0f)), rounding, 0, 1.5f);
    dl->AddLine(ImVec2(mn.x + rounding, mn.y + 1.0f), ImVec2(mx.x - rounding, mn.y + 1.0f), IM_COL32(ar + (255 - ar) * 4 / 10, ag + (255 - ag) * 4 / 10, ab + (255 - ab) * 4 / 10, (int)(pulse * 200.0f)), 2.0f);
}
