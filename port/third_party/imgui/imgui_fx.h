// Interface effects of this build (the F1 window and all its tabs): smooth hover / press / focus animations, glows, ripples,
// and the hooks that play the interface's sounds. Included by imgui.cpp and imgui_widgets.cpp, which call it where a widget
// is drawn, so every widget of the window gets the same treatment without touching its call site.
// The sounds and the window's backdrop live in port/src/gs/ui_fx.cpp. gImFxVisual = 0 gives the plain widgets back.
#pragma once
#include "imgui.h"
#include "imgui_internal.h"
#include <math.h>

extern int gImFxVisual;       // 1: animations and glows (ui_fx.cpp; the F1 window's "Interface" section)
extern float gImFxAccent[3];  // the accent colour (0..1), the player's choice (ui_fx.cpp)
void ImFx_Sound(int kind);    // ui_fx.cpp: plays one of the interface's sounds (volume 0 = none)

enum { IMFX_HOVER = 1, IMFX_CLICK = 2, IMFX_ON = 3, IMFX_OFF = 4, IMFX_SLIDE = 5, IMFX_TAB = 6, IMFX_OPEN = 7, IMFX_CLOSE = 8, IMFX_MOVE = 9 };

namespace ImFx {

static int sSuppressClick = 0;   // a widget that plays its own sound (check box, tab) holds the generic click back
static ImGuiID sLastHover = 0;

static inline float Clamp01(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }

// A value that eases to its target (speed: how fast, ~10 slow, ~25 snappy). Starts at the target: nothing pops in on the first frame.
static inline float Anim(ImGuiID id, int slot, float target, float speed)
{
    if (!gImFxVisual)
        return target;
    ImGuiStorage* st = ImGui::GetStateStorage();
    ImGuiID key = ImHashData(&slot, sizeof(slot), id);
    float* v = st->GetFloatRef(key, target);
    float k = 1.0f - expf(-speed * ImGui::GetIO().DeltaTime);
    *v += (target - *v) * k;
    if (fabsf(target - *v) < 0.002f)
        *v = target;
    return *v;
}

static inline ImVec4 Mix(const ImVec4& a, const ImVec4& b, float t) { return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t); }
static inline ImVec4 AccentVec(float a) { return ImVec4(gImFxAccent[0], gImFxAccent[1], gImFxAccent[2], a); }
static inline ImVec4 AccentHiVec(float a) { return ImVec4(gImFxAccent[0] + (1.0f - gImFxAccent[0]) * 0.40f, gImFxAccent[1] + (1.0f - gImFxAccent[1]) * 0.40f, gImFxAccent[2] + (1.0f - gImFxAccent[2]) * 0.40f, a); }
static inline ImU32 Accent(float a) { return ImGui::ColorConvertFloat4ToU32(AccentVec(Clamp01(a))); }
static inline ImU32 AccentHi(float a) { return ImGui::ColorConvertFloat4ToU32(AccentHiVec(Clamp01(a))); }

// Soft glow around a rectangle: a few rings that fade out.
static inline void Glow(ImDrawList* dl, const ImRect& bb, float rounding, float amount, float spread = 5.0f)
{
    if (amount <= 0.01f)
        return;
    for (int i = 1; i <= 4; i++)
    {
        float d = spread * (float)i / 4.0f;
        dl->AddRect(ImVec2(bb.Min.x - d, bb.Min.y - d), ImVec2(bb.Max.x + d, bb.Max.y + d), Accent(amount * 0.30f / (float)i), rounding + d, 0, 2.0f);
    }
}

// Press ripple: starts where the pointer was (the middle for a pad), grows and fades.
static inline void RippleStart(ImGuiID id, const ImRect& bb)
{
    ImGuiContext& g = *GImGui;
    ImGuiStorage* st = ImGui::GetStateStorage();
    int s = 0;
    st->SetFloat(ImHashData(&s, sizeof(s), id ^ 0x51ED), (float)g.Time);
    ImVec2 p = (g.NavCursorVisible && g.NavId == id) ? bb.GetCenter() : g.IO.MousePos;
    s = 1; st->SetFloat(ImHashData(&s, sizeof(s), id ^ 0x51ED), p.x);
    s = 2; st->SetFloat(ImHashData(&s, sizeof(s), id ^ 0x51ED), p.y);
}
static inline void RippleDraw(ImDrawList* dl, const ImRect& bb, ImGuiID id)
{
    ImGuiContext& g = *GImGui;
    ImGuiStorage* st = ImGui::GetStateStorage();
    int s = 0;
    float t0 = st->GetFloat(ImHashData(&s, sizeof(s), id ^ 0x51ED), -10.0f);
    float age = (float)g.Time - t0;
    if (age < 0.0f || age > 0.55f)
        return;
    s = 1; float px = st->GetFloat(ImHashData(&s, sizeof(s), id ^ 0x51ED), 0.0f);
    s = 2; float py = st->GetFloat(ImHashData(&s, sizeof(s), id ^ 0x51ED), 0.0f);
    float k = age / 0.55f;
    float e = 1.0f - (1.0f - k) * (1.0f - k);
    float r = e * ImMax(bb.GetWidth(), bb.GetHeight()) * 1.15f;
    dl->PushClipRect(bb.Min, bb.Max, true);
    dl->AddCircleFilled(ImVec2(px, py), r, IM_COL32(255, 255, 255, (int)((1.0f - k) * 70.0f)), 32);
    dl->PopClipRect();
}

// What every clickable shape gets on top of its own fill: a soft gloss on the upper half, a glowing accent edge that follows
// the hover / focus, and the press ripple.
static inline void Decorate(ImDrawList* dl, const ImRect& bb, float rounding, ImGuiID id, float hover, float press)
{
    if (!gImFxVisual)
        return;
    float h = bb.GetHeight();
    dl->AddRectFilled(ImVec2(bb.Min.x + 1.0f, bb.Min.y + 1.0f), ImVec2(bb.Max.x - 1.0f, bb.Min.y + h * 0.5f), IM_COL32(255, 255, 255, (int)(14.0f + 26.0f * hover)), rounding, ImDrawFlags_RoundCornersTop);
    if (hover > 0.01f)
    {
        Glow(dl, bb, rounding, hover * (1.0f - press * 0.4f), 4.0f);
        dl->AddRect(bb.Min, bb.Max, Accent(0.35f + 0.65f * hover), rounding, 0, 1.5f);
    }
    RippleDraw(dl, bb, id);
}

// The sound of the pointer coming onto a widget, once per widget.
static inline void HoverSound(ImGuiID id, bool hovered)
{
    ImGuiContext& g = *GImGui;
    if (hovered && !g.NavCursorVisible)
    {
        if (id != sLastHover)
        {
            sLastHover = id;
            ImFx_Sound(IMFX_HOVER);
        }
    }
    else if (!hovered && sLastHover == id)
        sLastHover = 0;
}

} // namespace ImFx
