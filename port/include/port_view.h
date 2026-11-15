#ifndef PORT_VIEW_H
#define PORT_VIEW_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VIEW_MODE_P1    = 0, /* Camara completa Jugador 1 */
    VIEW_MODE_P2    = 1, /* Camara completa Jugador 2 */
    VIEW_MODE_SPLIT = 2  /* Pantalla dividida (Split-Screen) */
} PortViewMode;

typedef enum {
    ROLE_ACTIVE_P1  = 0, /* Jugador local activo en lado 0 */
    ROLE_ACTIVE_P2  = 1, /* Jugador local activo en lado 1 */
    ROLE_SPECTATOR  = 2  /* Espectador online, replay o CPU vs CPU */
} PortViewRole;

/* Inicializacion y consulta de estado */
void PortView_BattleReset(void);
PortViewRole PortView_GetRole(void);
PortViewMode PortView_GetMode(void);
void PortView_SetMode(PortViewMode mode);
void PortView_CycleNext(void);
const char *PortView_GetName(PortViewMode mode);

/* Consulta para el motor del juego */
int PortView_GetOverride(void);      /* 0: P1, 1: P2, -1: Pantalla dividida */
int PortView_IsSplitActive(void);    /* 1 si la pantalla dividida esta activa */
int Port_BattleIsRunning(void);      /* 1 si el combate esta en ejecucion */

/* Opciones de configuracion */
int PortView_GetAllowLocal(void);
void PortView_SetAllowLocal(int allow);
int PortView_GetShortcutEnabled(void);
void PortView_SetShortcutEnabled(int enabled);
int PortView_GetDefaultSplit(void);
void PortView_SetDefaultSplit(int split);

#ifdef __cplusplus
}
#endif

#endif /* PORT_VIEW_H */
