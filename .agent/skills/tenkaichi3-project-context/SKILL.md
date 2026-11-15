---
name: tenkaichi3-project-context
description: >-
  Proporciona contexto exhaustivo, arquitectura del motor, mapa del repositorio,
  reglas de decompilación, plataforma PC, mod de entrenamiento y flujos de trabajo
  de Dragon Ball Z: Budokai Tenkaichi 3 (Tenkaichi3Decomp / BT3-Decompiled).
  Usar esta skill siempre que se trabaje, analice, modifique, descompile o depure
  cualquier parte de este repositorio.
---

# Dragon Ball Z: Budokai Tenkaichi 3 - Contexto del Proyecto

Este documento reúne la guía técnica y conceptual completa para entender el proyecto **Tenkaichi3Decomp**, su arquitectura híbrida (decompilación + port nativo a PC), sus módulos clave, el **Mod de Entrenamiento y Replays**, y las reglas críticas para desarrolladores y agentes de IA.

---

## 1. Misión y Naturaleza del Proyecto

- **¿Qué es?**: Un port nativo para PC de **Dragon Ball Z: Budokai Tenkaichi 3** (PlayStation 2, versión USA NTSC `SLUS-21678`).
- **No es un emulador**: El código del juego fue obtenido mediante decompilación inversa bit a bit (`BT3-Decompiled`) del binario original de PS2 en lenguaje C. Ese código se compila nativamente en la arquitectura del host (x86-64, ARM64 en macOS).
- **Capa de Abstracción PC (`port/`)**: Sustituye el hardware original de la PS2 (Graphics Synthesizer, SPU2, IOP, CDVD, Emotion Engine DMA) por APIs modernas:
  - **Gráficos**: Vulkan como backend principal y OpenGL 3.3 como fallback secundario (con soporte Metal en macOS experimental). Interfaz de configuración mediante **Dear ImGui** (accesible con `F1` o `START + SELECT`).
  - **Audio**: Decodificación de audio ADX y emulación del protocolo `SOUNDS.IRX`.
  - **Entrada**: SDL3 con soporte para mandos y teclado totalmente remapeables.
  - **Netplay**: Multijugador online determinista con **Rollback netcode**, matchmaking mediante Cloudflare Workers y retransmisión TURN.
  - **Mejoras**: Resoluciones internas 1x a 8x, aspecto panorámico 16:9 / 21:9 sin deformar el combate, interpolación de movimiento fluido a 60/120/240 FPS, carga de mapas (`.unk`) y canciones externas (`.mp3`/`.adx`), y mods en Lua.
- **Datos y Legalidad**: El repositorio **no contiene datos protegidos por derechos de autor**. Requiere una imagen de disco (`.iso`) legítima para extraer tablas y assets.

---

## 2. Mapa del Repositorio y Directorios Clave

| Carpeta / Archivo | Propósito y Contenido |
| :--- | :--- |
| `src/` | Código decompilado del juego (C para ee-gcc 2.96). Incluye `battle/` (combate, personajes, IA, efectos, HUD), `sys/` (heap, archivos, pad, matemáticas, guardado), `menu/` y `cri/`. |
| `include/` | Cabeceras C con estructuras de datos, enumeraciones y prototipos exactos del juego (`battle/`, `sys/`, `menu/`, `cri/`, `port/`). |
| `port/` | Todo el código exclusivo del port de PC: capa de render (`port/src/gs/`), entrada, sonido, netplay, gestor de replays (`plat_replays.c`), mod de entrenamiento (`plat_training.c`), compatibilidad (`port_compat.h`), scripts de empaquetado y extracción (`port/tools/`). |
| `docs/` | Documentación técnica exhaustiva del motor, decompilación y subsistemas del juego. |
| `scripts/` | Herramientas de automatización y decompilación (`fdiff.py`, `progress.py`, `permute.py`, `apply_names.py`, etc.). |
| `config/` | Archivos de símbolos (`config/symbols/*.txt`, `symbol_addrs.txt`) y configuraciones de particionado splat (`SLUS_216.78.yaml`, `DBZP.yaml`). |
| `MOD_ENTRENAMIENTO.md` | Especificación completa del Mod de Entrenamiento, Contra-Z automática y Sistema de Replays extendido. |
| `install.py` / `configure.py` | Scripts de configuración e instalación a partir de la ISO. |

---

## 3. Subdirectorios en `docs/`, `include/` y `scripts/`

### 3.1. Documentación Clave (`docs/`)
- `docs/agent_rules.md`: **Reglas obligatorias** para cualquier agente de IA o colaborador que trabaje en el código decompilado.
- `docs/decomp_guide.md`: Guía de decompilación, peculiaridades del compilador Sony `ee-gcc 2.96 -O2 -G8` y técnicas de matching.
- `docs/status.md`: Estado de integración de módulos decompilados y funciones `INCLUDE_ASM`.
- `docs/netplay_notes.md`: Bitácora técnica sobre determinismo, estados de la máquina, rollback y protocolo de red.
- `docs/systems/`: Guías individuales de cada subsistema:
  - `fighter.md`: Estructura del objeto luchador (`0x1600` bytes), fases de actualización, hit-stop, cambios de personaje.
  - `combat.md`: Banderas de combate, colisiones, daño, barras de ki/vida, choques (clashes), congelamiento de tiempo.
  - `battle.md`: Ciclo de vida de la batalla, secuencias, modos de batalla.
  - `ai.md`: Lógica del oponente controlado por la CPU, árbol de reglas y pad virtual.
  - `graphics.md` & `effects_stage.md`: Renderizado GS, ordenamiento de capas (OT), efectos de pantalla y partículas.
  - `input.md` & `audio.md`: Mapeo de pad y audio streaming / IRX.
- `docs/port/`:
  - `README.md`: Bitácora de desarrollo del port a PC, registro de parches y decisiones arquitectónicas.
  - `mods.md`, `adding_stages.md`, `adding_songs.md`: Extensiones de contenido.

### 3.2. Cabeceras (`include/`)
- `include/common.h`, `types.h`: Tipos de datos fundamentales (`s8`, `u8`, `s16`, `u16`, `s32`, `u32`, `s64`, `u64`, `f32`).
- `include/battle/`: Todas las definiciones del combate (`battle.h`, `btl_char.h`, `btl_hit.h`, `btl_action.h`, `btl_seq.h`, `btl_ai.h`, etc.).
- `include/sys/`: Subsistemas base (`heap.h`, `file.h`, `pad.h`, `math3d.h`, `save.h`, `snd.h`, `dma.h`).
- `include/port/`: Cabeceras del port (`port_compat.h`, `plat_training.h`, `plat_replays.h`).

### 3.3. Herramientas de Scripting (`scripts/`)
- `scripts/fdiff.py`: **Herramienta principal de diffing**. Compara las instrucciones compiladas de una función contra el ensamblador original.
  ```bash
  python3 scripts/fdiff.py src/<modulo>.c [nombre_funcion]
  ```
- `scripts/progress.py`: Reporta el porcentaje actual de decompilación del juego.
- `scripts/permute.py`: Búsqueda heurística y permutaciones de código C para resolver diferencias de ensamblador.
- `scripts/apply_names.py`: Renombra símbolos en el código fuente basándose en los archivos de `config/symbols/`.

---

## 4. Convenciones del Motor de Juego (PS2 Engine Internals)

1. **Frecuencia de Simulación**:
   - El juego corre a **30 FPS lógicos fijos** (1 tick de simulación cada ~33.3 ms).
   - El renderizador de PC puede dibujar a 60/120/240 FPS interpolando posiciones, pero la física y el estado del combate siempre avanzan a 30 Hz.
2. **Sistema de Coordenadas y Ángulos**:
   - Eje vertical: **+Y apunta hacia abajo**.
   - Los ángulos se expresan en **radianes** (o enteros de punto fijo en ciertas tablas).
3. **Estructura del Personaje (`Fighter Object`)**:
   - Tamaño: exactamente **`0x1600` bytes**.
   - Manejo de banderas de estado:
     - Comprobación: `BtlChar_TestFlag(chr, flag)`
     - Activación: `BtlChar_SetFlag(chr, flag)` / `BtlChar_SetHeldFlag(chr, flag)`
     - Limpieza: `BtlChar_ClearFlag(chr, flag)`
     - Acceso al objeto de batalla: `BtlChar_GetObj(chr)`
4. **Tipos y Compatibilidad de 64-bit**:
   - En la PS2 (`ee-gcc`), `long` es de 64 bits.
   - En PC (`port_compat.h`), `s64` y `u64` se redefinen como `long long` / `unsigned long long`.
   - Modificaciones al código original de `src/` o `include/` **deben** estar encerradas en `#ifdef PORT ... #endif` para no romper la decompilación idéntica de PS2.

---

## 5. El Mod de Entrenamiento y Replays (`MOD_ENTRENAMIENTO.md`)

Este fork incluye un mod avanzado sobre la base de Tenkaichi3Decomp v0.1.39.6:

1. **Grabación y Reproducción en Entrenamiento** (`battle mode 6`):
   - `SELECT`: Inicia o detiene la grabación del Player 1 (con aviso en pantalla).
   - `START`: Reproduce o detiene la grabación activa asignándola al Player 2.
   - `L3` (al soltarlo): Abre el menú de Pausa.
   - Pestaña `F1 > Entrenamiento`: Administra grabaciones (renombrar, duplicar, ordenar, repetir, guardado en `bt3_training.bin`), y control manual/bloqueo de Vida, Ki y Blast de P1 y P2.
2. **Contra-Z Automática (Auto Z-Counter)**:
   - Configurable en `F1 > Entrenamiento` con casilla y deslizador de probabilidad (0 a 100%).
   - Disponible en todos los modos offline; **deshabilitada estrictamente en online y replays**.
   - Usa un generador pseudoaleatorio propio para no alterar la semilla determinista del juego original.
   - Fuera del modo entrenamiento, `SELECT` activa/desactiva la Contra-Z del P2 en combate offline.
3. **Ajustes de Modo Historia**:
   - Oponentes de la CPU ya no tienen la superarmadura pasiva fija (bit 4 de parámetros) en modo historia (`Port_StoryMovable`), permitiendo combates dinámicos contra la IA (Broly mantiene su comportamiento original).
4. **Sistema Avanzado de Replays Online**:
   - Guardado automático de combates online en la carpeta `replays/`:
     - Archivo `.bt3rep`: Estado original del replay.
     - Archivo `.meta`: Metadatos (nombres, duración, semilla, historial).
     - Archivo `.bt3inp`: Registro cuadro a cuadro de entradas (6 bytes por cuadro por luchador), permitiendo **duración ilimitada** sin el límite de 5 minutos (9000 cuadros) de la PS2 original.
   - Pestaña `F1 > Replays`:
     - Agrupación inteligente por series **FT n** (First To n) entre los mismos jugadores.
     - Gráfica de ventaja (barra de vida a lo largo del tiempo).
     - Reproducción interactiva con barra de control temporal (estilo reproductor de vídeo): saltar a cualquier punto mediante rebobinado y simulación rápida (`gPortResim`), pausar, y cambiar velocidad (x0.25 a x2.0).
     - Sistema de Keyframing (Snapshots de Rollback): Búfer circular de estados intermedios guardados con `state_save`/`state_load`. Al retroceder o saltar en la línea de tiempo, restaura el snapshot más cercano en 0.3 ms y simula únicamente el tramo remanente (< 50 ms), eliminando los bloqueos $O(N)$ de reiniciar desde el cuadro 0.
     - Optimización Gráfica a Altas Velocidades (>1x): Supresión automática de interpolación de movimiento (`gsInterp`), throttling proactivo de presentación visual a 30 FPS constantes a x1.5 y x2.0 (ahorrando 50% de carga GPU sin perder precisión física ni de entradas) y frame skip adaptativo (`BT3_FRAME_SKIP=1`).
     - Bypass Ultrarrápido en Resimulación (`gPortResim`): Ruta directa en `Port_VBlank()` que omite hooks de accesibilidad, paneles laterales, mods y trazado durante el avance rápido en replays y rollbacks online, reduciendo la sobrecarga de VBlank de ~85 µs a < 2 µs (~40x de aceleración).
     - Pacing de Alta Precisión: `vblank_wait()` (`plat_stub.c`) usa espera híbrida en todas las plataformas. Windows: `timeBeginPeriod(1)` + temporizador de alta resolución (`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`) hasta 0.4 ms antes y spin con `YieldProcessor()`. Linux/macOS: `nanosleep` hasta 0.25 ms (1 ms en macOS) antes y spin con `pause`/`yield`, eliminando el sobre-sueño que a x2.0 (blanks de 8.3 ms) causaba jitter.
     - Zero-Copy del `.bt3inp`: `Port_RepLongPlay()` (`plat_replays.c`) mapea el archivo de entradas en solo-lectura (`CreateFileMappingA`/`MapViewOfFile` en Windows, `mmap` + `MADV_SEQUENTIAL` en POSIX); los arrays de cada jugador son vistas dentro del mapeo (botones leídos con `memcpy` por posible desalineación cuando n0 es impar). Fallback: una sola lectura del archivo completo. Las variables de `plat_replays.c` son datos del host (no están en `PORT_STATE` de `make_state.py`), por eso los keyframes guardan `sPos` vía `Port_RepGetPos/SetPos`.
5. **Correcciones Gráficas en Render**:
   - Solución a la condición de carrera en `gGsMainFbp` para garantizar que el HUD mantenga la relación de aspecto 16:9 durante replays a alta velocidad o partidas online a 114% de velocidad (`gs_core.c`, `gs_sony.c`, `gs_draw.c`).

---

## 6. Reglas Críticas para Agentes y Desarrolladores

Al trabajar en este proyecto, sigue rigurosamente estas reglas:

1. **Aislamiento de la decompilación de PS2**:
   - No ejecutes `configure.py`, `ninja` o `git` de forma arbitraria en tareas paralelas.
   - La única herramienta segura para validar funciones decompiladas es `python3 scripts/fdiff.py <archivo.c> [funcion]`.
   - No toques archivos en `asm/`, `build/`, ni alteres los YAML de `config/` a menos que estés integrando explícitamente un módulo terminado.
2. **Capa PC y Directivas `#ifdef PORT`**:
   - Cualquier ajuste necesario para que el código fuente del juego compile o funcione en PC debe protegerse con `#ifdef PORT ... #endif`.
   - Todo código nuevo de la plataforma de PC, UI de ImGui, replays o entrenamiento pertenece a `port/src/` y `port/include/`.
3. **Determinismo en Simulación y Netplay**:
   - Jamás uses funciones de tiempo del sistema (`time()`, `clock()`) o números aleatorios de la librería estándar del sistema operativo (`rand()`, `std::mt19937`) dentro del código de simulación del combate (`src/battle/`). Romper el determinismo causa desincronizaciones fatales en el netcode de rollback y desincroniza los replays.
   - Emplea únicamente los generadores pseudoaleatorios del motor o el estado explícito `gPortVu0R`.
4. **Compilación del Port**:
   - Compilación para Windows: mediante Docker con `port/release/build.sh` o el workflow de GitHub Actions `release`.
   - Compilación local completa desde código fuente: `python3 install.py --iso <ruta_iso>`.

