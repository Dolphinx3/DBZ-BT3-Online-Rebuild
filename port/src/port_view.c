#include "port_view.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int Port_NetActive(void);
extern int Port_NetSpectating(void);
extern int Port_NetMe(void);
extern const char *Port_NetName(int side);
extern int Port_RepPlayActive(void);
extern const char *Port_RepPlayName(int side);
extern int Port_RepMySide(void);
extern int Port_SplitGet(void);
extern int BattleReplay_IsActive(void);
extern int BattleSide_GetControl(int side); /* 0: pad (humano), 2: CPU */
extern int Battle_GetMode(void);
extern void *Battle_GetWork(void);

extern int Port_Setting(const char *name, int def);
extern void Port_SettingSave(const char *name, int value);
extern void Port_SettingsWrite(void);
extern void Port_UiNotice(const char *text);

static PortViewMode sCurrentMode = VIEW_MODE_P1;

static const char *side_player_name(int side) {
    const char *n = Port_NetName(side);
    if (n != NULL && n[0] != '\0') {
        return n;
    }
    n = Port_RepPlayName(side);
    if (n != NULL && n[0] != '\0') {
        return n;
    }
    return "";
}

int Port_BattleIsRunning(void) {
    /* BattleWork +0x19F8 is 'running' (s32) */
    void *w = Battle_GetWork();
    if (w == NULL) {
        return 0;
    }
    return *((int *)((char *)w + 0x19F8)) != 0;
}

int PortView_GetAllowLocal(void) {
    return Port_Setting("view_split_local", 1) != 0;
}

void PortView_SetAllowLocal(int allow) {
    Port_SettingSave("view_split_local", allow ? 1 : 0);
    Port_SettingsWrite();
}

int PortView_GetShortcutEnabled(void) {
    return Port_Setting("view_shortcut_on", 1) != 0;
}

void PortView_SetShortcutEnabled(int enabled) {
    Port_SettingSave("view_shortcut_on", enabled ? 1 : 0);
    Port_SettingsWrite();
}

int PortView_GetDefaultSplit(void) {
    return Port_Setting("view_split_default", 0) != 0;
}

void PortView_SetDefaultSplit(int split) {
    Port_SettingSave("view_split_default", split ? 1 : 0);
    Port_SettingsWrite();
}

PortViewRole PortView_GetRole(void) {
    if (BattleReplay_IsActive() || Port_NetSpectating()) {
        return ROLE_SPECTATOR;
    }
    if (Port_NetActive()) {
        return Port_NetMe() == 1 ? ROLE_ACTIVE_P2 : ROLE_ACTIVE_P1;
    }
    /* Modo local */
    if (BattleSide_GetControl(0) != 0 && BattleSide_GetControl(1) != 0) {
        return ROLE_SPECTATOR; /* CPU vs CPU o demo */
    }
    if (BattleSide_GetControl(0) != 0 && BattleSide_GetControl(1) == 0) {
        return ROLE_ACTIVE_P2; /* Jugador en pad 2 */
    }
    return ROLE_ACTIVE_P1;     /* Jugador en pad 1 */
}

static PortViewMode sanitize_mode(PortViewMode mode, PortViewRole role) {
    int isLocal = !Port_NetActive() && !BattleReplay_IsActive() && !Port_NetSpectating();
    int allowSplit = (!isLocal) || PortView_GetAllowLocal();

    /* Caso especial: combate local 1P vs 2P humano en el mismo PC siempre permite split */
    if (isLocal && BattleSide_GetControl(0) == 0 && BattleSide_GetControl(1) == 0) {
        allowSplit = 1;
    }

    switch (role) {
    case ROLE_ACTIVE_P1:
        if (mode == VIEW_MODE_SPLIT) {
            return allowSplit ? VIEW_MODE_SPLIT : VIEW_MODE_P1;
        }
        return VIEW_MODE_P1; /* Bloquea terminantemente la vista completa del rival (P2) */

    case ROLE_ACTIVE_P2:
        if (mode == VIEW_MODE_SPLIT) {
            return allowSplit ? VIEW_MODE_SPLIT : VIEW_MODE_P2;
        }
        return VIEW_MODE_P2; /* Bloquea terminantemente la vista completa del rival (P1) */

    case ROLE_SPECTATOR:
    default:
        if (mode == VIEW_MODE_SPLIT && !allowSplit && isLocal) {
            return VIEW_MODE_P1;
        }
        if (mode < VIEW_MODE_P1 || mode > VIEW_MODE_SPLIT) {
            return VIEW_MODE_P1;
        }
        return mode;
    }
}

void PortView_BattleReset(void) {
    PortViewRole role = PortView_GetRole();
    int defaultSplit = PortView_GetDefaultSplit();
    int isLocal = !Port_NetActive() && !BattleReplay_IsActive() && !Port_NetSpectating();

    /* Caso especial: Espectador online con BT3_SPEC_VIEWS o vista ya elegida */
    if (Port_NetSpectating() && getenv("BT3_SPEC_VIEWS") != NULL) {
        return;
    }

    /* Caso especial: Replays del port respetan su flag de inicio (rep_split / lado grabado) */
    if (Port_RepPlayActive()) {
        if (Port_SplitGet() || defaultSplit) {
            sCurrentMode = VIEW_MODE_SPLIT;
        } else {
            sCurrentMode = Port_RepMySide() == 1 ? VIEW_MODE_P2 : VIEW_MODE_P1;
        }
        return;
    }

    /* Caso especial: Local 1P vs 2P humano en la misma pantalla siempre inicia en Split */
    if (isLocal && BattleSide_GetControl(0) == 0 && BattleSide_GetControl(1) == 0) {
        sCurrentMode = VIEW_MODE_SPLIT;
        return;
    }

    if (defaultSplit && ((!isLocal) || PortView_GetAllowLocal())) {
        sCurrentMode = VIEW_MODE_SPLIT;
    } else {
        sCurrentMode = (role == ROLE_ACTIVE_P2) ? VIEW_MODE_P2 : VIEW_MODE_P1;
    }
    sCurrentMode = sanitize_mode(sCurrentMode, role);
}

PortViewMode PortView_GetMode(void) {
    return sanitize_mode(sCurrentMode, PortView_GetRole());
}

void PortView_SetMode(PortViewMode mode) {
    sCurrentMode = sanitize_mode(mode, PortView_GetRole());
}

void PortView_CycleNext(void) {
    PortViewRole role = PortView_GetRole();
    PortViewMode cur = PortView_GetMode();
    int isLocal = !Port_NetActive() && !BattleReplay_IsActive() && !Port_NetSpectating();
    int allowSplit = (!isLocal) || PortView_GetAllowLocal();

    if (isLocal && BattleSide_GetControl(0) == 0 && BattleSide_GetControl(1) == 0) {
        allowSplit = 1;
    }

    switch (role) {
    case ROLE_ACTIVE_P1:
        if (!allowSplit) {
            Port_UiNotice("Pantalla dividida deshabilitada en combates locales");
            return;
        }
        sCurrentMode = (cur == VIEW_MODE_P1) ? VIEW_MODE_SPLIT : VIEW_MODE_P1;
        Port_UiNotice(sCurrentMode == VIEW_MODE_SPLIT ? "Vista: Pantalla Dividida" : "Vista: Camara Propia (P1)");
        break;

    case ROLE_ACTIVE_P2:
        if (!allowSplit) {
            Port_UiNotice("Pantalla dividida deshabilitada en combates locales");
            return;
        }
        sCurrentMode = (cur == VIEW_MODE_P2) ? VIEW_MODE_SPLIT : VIEW_MODE_P2;
        Port_UiNotice(sCurrentMode == VIEW_MODE_SPLIT ? "Vista: Pantalla Dividida" : "Vista: Camara Propia (P2)");
        break;

    case ROLE_SPECTATOR:
        if (!allowSplit && isLocal) {
            sCurrentMode = (cur == VIEW_MODE_P1) ? VIEW_MODE_P2 : VIEW_MODE_P1;
        } else {
            sCurrentMode = (PortViewMode)((cur + 1) % 3);
        }
        if (sCurrentMode == VIEW_MODE_P1) {
            Port_UiNotice(side_player_name(0)[0] != '\0' ? side_player_name(0) : "Vista: Jugador 1");
        } else if (sCurrentMode == VIEW_MODE_P2) {
            Port_UiNotice(side_player_name(1)[0] != '\0' ? side_player_name(1) : "Vista: Jugador 2");
        } else {
            Port_UiNotice("Vista: Pantalla Dividida (Ambos)");
        }
        break;
    }
}

const char *PortView_GetName(PortViewMode mode) {
    static char buf[64];
    switch (mode) {
    case VIEW_MODE_P1:
        if (side_player_name(0)[0] != '\0') {
            snprintf(buf, sizeof(buf), "Jugador 1 (%s)", side_player_name(0));
            return buf;
        }
        return "Jugador 1";
    case VIEW_MODE_P2:
        if (side_player_name(1)[0] != '\0') {
            snprintf(buf, sizeof(buf), "Jugador 2 (%s)", side_player_name(1));
            return buf;
        }
        return "Jugador 2";
    case VIEW_MODE_SPLIT:
    default:
        return "Pantalla dividida (Ambos)";
    }
}

int PortView_GetOverride(void) {
    static int forced = -2;
    if (forced == -2) {
        const char *e = getenv("BT3_VIEW");
        forced = e != NULL ? atoi(e) : -1;
    }
    if (forced >= 0) {
        return forced & 1;
    }
    PortViewMode mode = PortView_GetMode();
    if (mode == VIEW_MODE_SPLIT) {
        return -1; /* Split screen layout */
    }
    return (int)mode; /* 0: P1, 1: P2 */
}

int PortView_IsSplitActive(void) {
    return PortView_GetMode() == VIEW_MODE_SPLIT;
}
