# Mod de Entrenamiento (sobre Tenkaichi3Decomp v0.1.39.6)

Solo en modo Entrenamiento (battle mode 6):
- SELECT: iniciar/detener grabacion de P1 (con mensaje).
- START: reproducir/detener la grabacion elegida (por defecto la juega P2).
- L3 (al soltarlo, sin R3): abrir el menu de Pausa (L3+R3 sigue siendo el cambio de personaje).
- F1 > pestana "Entrenamiento": gestionar grabaciones (renombrar, duplicar, eliminar, subir, bajar, repetir),
  y Vida / Ki / Blast de P1 y P2 por separado (Aplicar, Fijar, Rellenar todo).
Grabaciones en bt3_training.bin.

Compilar para Windows: port/release/build.sh (Docker) -> port/build_release/el zip de release
o subir a GitHub y ejecutar el workflow "release".

Archivos del mod: port/src/plat_training.c, port/include/plat_training.h, port/src/gs/ui.cpp,
src/battle/hud_e_d.c, src/battle/btl_input.c, src/battle/btl_seq.c (cambios dentro de #ifdef PORT).

## Contra-Z automatica (F1 > Entrenamiento, al final)
Por lado (P1 izquierda / P2 derecha): casilla + deslizador de probabilidad 0-100 %.
Funciona en todos los modos offline (entrenamiento, duelo vs CPU, historia, torneo...); nunca online ni en repeticiones.
Usa las mismas condiciones que la Contra-Z manual del juego (Arriba + Rush): el golpe llega, el personaje esta libre
(animacion neutra), es el primer golpe del ataque y pasaron 30 cuadros desde la ultima. Con el rival lejos no hay golpe,
asi que no intenta nada. 100 % = todo golpe contrarrestable; 50 % = tirada de probabilidad (generador propio,
no toca el aleatorio del juego). Archivos: src/battle/btl_char_hit.c, port/src/plat_training.c.

## SELECT fuera del entrenamiento (todos los modos offline)
SELECT ya no cambia la camara (A/B/C): activa / desactiva la Contra-Z automatica del P2, con su porcentaje, y muestra un mensaje.
En el modo Entrenamiento SELECT sigue grabando. Online no cambia nada. Archivos: src/battle/btl_char_cam_modes.c,
src/battle/btl_seq.c, src/battle/hud_e_d.c, port/src/plat_training.c.

## Historia: oponentes que si se mueven (sin Broly)
Modo Historia (battle mode 1, offline): la armadura propia del oponente CPU (bit 4 de los parametros del personaje, un +1 fijo)
ya no cuenta, ni contra golpes ni contra ki blasts. La armadura que dan habilidades o hechizos (Falso Coraje, habilidades,
skills potenciadas) NO se toca. Broly (ids 131-133) queda como el juego. Archivos: btl_char_mgr.c (Port_StoryMovable),
btl_char_hit.c, btl_char_coll_b.c.

## Online: menos paquetes al esperar
En espera de la entrada del rival, el reenvio pasaba de 250 paquetes/s; ahora es 1/4 del ping (4-20 ms). El formato en el cable
es el mismo (compatible con la version oficial). Archivo: port/src/gs/net.c (resend_gap_ns).

## Replays de partidas online (F1 > Replays)
Se guardan solas las partidas online (casual y salas personalizadas) en replays/ (archivo .bt3rep en el formato de replay del
juego + .meta con jugadores, resultado, duracion, semilla y la grafica de ventaja). Se conservan las ultimas 30.
La pestana lista fecha, tipo, rival, resultado y duracion; historial de victorias/derrotas contra cada rival; grafica de
ventaja (arriba tu, abajo el rival); Reproducir abre una segunda ventana del juego con BT3_REPLAY + BT3_REPLAY_SEED.
Pendiente: reproducir desde un punto de la grafica; pagina "Replays" del menu online del juego (hoy el gestor esta en F1).
Archivos: port/src/plat_replays.c, port/include/plat_replays.h, src/battle/hud_e_d.c, battle_load.c, btl_seq.c,
port/src/headless.c, port/src/gs/ui.cpp.

## Replays: series FT y diseno
La pestana agrupa los combates en series (mismos jugadores y lados, menos de 20 min entre uno y otro). Cada serie es una
tarjeta: "Juanser (Player 1) vs MonoRabioso (Player 2)", insignia FT n (n = victorias del que va ganando), marcador, tipo
(Casual / Personalizada), numero de combates, duracion total y fecha. Al elegirla: lista de combates con ganador y
duracion, Reproducir / Eliminar combate / Eliminar serie y la grafica de ventaja del combate elegido.

## Replays sin limite de duracion
El bloque de replay del propio juego guarda 9000 cuadros por luchador (5 min a 30 Hz) y luego deja de grabar. Ahora cada
replay online guarda ademas la entrada completa de los dos luchadores en <replay>.bt3inp (6 bytes por cuadro y luchador:
30 min = unos 650 KB), indexada por el blank del netcode (a prueba de rollback). Al reproducir, el juego usa ese archivo
(BT3_REPLAY_LONG) en lugar del bloque de 5 min. La grafica se adapta a cualquier duracion (al llenarse conserva una de cada
dos muestras). Limite practico: 3 horas por pelea. El tiempo limite de la PARTIDA online no se toca (sRules, lobby: igual
que la version oficial). Archivos: port/src/plat_replays.c, src/battle/btl_input.c, btl_replay.c.

## Replays: arreglos de grabacion y agrupacion
- La decision de grabar y los datos de la sesion (apodos, lados, tipo, semilla) se toman DURANTE la pelea, con la conexion
  activa: antes se decidia al inicio (la sesion aun podia no estar lista, y por eso a veces no grababa tras volver a
  seleccion de personaje) y los nombres se leian al final (ya vacios).
- Una revancha dentro de la misma batalla guarda primero la pelea anterior. Dos peleas en el mismo segundo no se pisan.
- Series: mismos dos jugadores (en cualquier lado), mismo tipo y como mucho 20 min entre el fin de una pelea y el inicio
  de la siguiente; las victorias se cuentan por nombre.

## Replays: nombres, cronometro, "X WINS" y saltar a un punto
- La copia que reproduce una replay recibe los nombres (BT3_REPLAY_P0/P1) y el largo (BT3_REPLAY_FRAMES): los nombres salen
  bajo las barras (antes solo se dibujaban con conexion online) y un cronometro "transcurrido / total" abajo al centro,
  activable en F1 > Replays ("Mostrar el tiempo...", ajuste rep_clock).
- Historial: "<nombre> WINS" en cada combate y en cada serie (EMPATE si van igualados).
- Clic en la grafica: reproduce desde ese momento (BT3_REPLAY_SEEK=<cuadro>): el juego corre hasta alli como los cuadros de
  un rollback (sin imagen, sin sonido, sin esperar: gPortResim en plat_stub.c) y luego sigue normal.
- La copia de la replay arranca con BT3_FRAME_SKIP=0 (hipotesis para las barras que cambiaban de forma: un tick que se salta
  deja un HUD a medio dibujar).

## Replays: barra de control y velocidad
En la copia que reproduce una replay aparece (con el raton, 3,5 s sin moverlo la oculta) una barra como la de un video:
progreso arrastrable/clicable, <<10 s / 10 s>>, velocidades x0.25 x0.5 x0.75 x1 x1.25 x1.5 x2. La velocidad parte de la
original de la partida (guardada en el .meta, speed=: 114 % es su x1). Adelante: el juego corre sin mostrar hasta ese punto
(Port_RepSeekTo); atras: se vuelve a calcular la pelea desde el inicio en una copia nueva (solo se calcula hacia adelante).
Archivos: port/src/plat_replays.c, plat_stub.c (Port_SpeedMilli), gs/ui.cpp.

## Nuevo estilo del menu F1
style() rehecho (paleta, redondeos, espaciado, barras mas finas), la ventana aparece con fundido y el contenido de cada
pestana entra con fundido suave (tab()/tab_end()). Solo estilo: ninguna funcion cambia.

## Correcciones (barra de replay y menu)
- Error "Missing PopStyleVar()" en la pestana Controls: esa pestana usa tab() con el cierre en otra linea y no se cerraba con
  tab_end(); corregido (todas las pestanas de tab() cierran con tab_end()).
- Boton Pausa/Play en la barra: levanta la pausa del propio juego (BATTLE_FLAG_PAUSE + sonido) desde BtlSeqFight_Update.
- Ir hacia atras ya no cierra el juego ni muestra el menu de fin de replay: se levanta BATTLE_FLAG_RESTART (el "ver de nuevo"
  del propio juego: Battle_Restart rebobina las pistas y reinicia la secuencia) y la pelea corre sin mostrar hasta el cuadro pedido.
- La replay abierta desde F1 arranca con el HUD completo (modo 1) y desde el punto de vista de quien la grabo (BT3_REPLAY_ME).

## HUD que alterna entre 4:3 y 16:9 (replays rapidas y online a 114 %)
Causa (por lectura del codigo): el hilo del juego escribe gGsMainFbp (el buffer "del cuadro actual", en sceGsSwapDBuff) al
empezar su SIGUIENTE cuadro mientras el hilo de render aun ejecuta la lista del anterior; el HUD va al final de la lista y el
render lo comparaba con el buffer equivocado (gs_draw.c: `fbp == gGsMainFbp`), asi que ese cuadro no se estrechaba a 16:9
(arte 4:3 estirado). Mas facil cuanto mas adelantado va el juego sobre la imagen (replay x1.25+, online a 114 %).
Arreglo: gs_core.c copia el buffer al entregar la lista (sJobFbp) y el render lo lee de gGsMainFbpR (gs_sony.c); gs_draw.c,
gs_gl.c y gs_gpu.c leen gGsMainFbpR (#define tras el include). Sin render thread: mismo valor que antes.

## Rendimiento de las replays (auditoria)
- Quitado BT3_FRAME_SKIP=0 de la copia que reproduce: el salto de dibujo es la forma que tiene el port de seguir el ritmo en un
  equipo lento y yo lo habia desactivado por una hipotesis equivocada (la causa del HUD era la carrera de gGsMainFbp).
- Dos copias del juego comparten la maquina: la que reproduce corre con prioridad "por encima de lo normal" y la principal baja a
  "por debajo" mientras la replay esta abierta (SetPriorityClass, solo Windows); vuelve a normal al cerrarse (rep_child_poll).
- Lo que la copia de la replay consulta cada cuadro (activa, nombres, largo, lado, ritmo base) se lee del entorno una sola vez
  (getenv recorre todo el entorno en cada llamada).
- Captura online: los datos de sesion se miran una vez cada 64 llamadas, no en cada cuadro de cada luchador.

## Rediseno del menu F1 (efectos y sonidos)
Todos los widgets de F1 y de sus pestanas se dibujan ahora con efectos, sin tocar cada llamada: se parchearon los widgets de
ImGui incluido (port/third_party/imgui: imgui_widgets.cpp, imgui.cpp + imgui_fx.h). Botones, casillas, selectables, sliders,
pestanas y el cursor del mando tienen animacion suave de hover / enfoque / pulsacion (brillo, borde de acento, onda al pulsar,
marca que "salta", barra de acento, linea de pestana que crece, cursor pulsante para el mando). Fondo animado de la ventana
(luces que derivan, barrido de luz, borde que respira): port/src/gs/ui_fx.cpp. Sonidos generados por codigo (sin archivos) en
una salida propia: hover, mover el cursor del mando, clic, activar/desactivar, deslizar, cambio de pestana, abrir/cerrar.
Ajustes: F1 > Misc > Interfaz ("Efectos visuales", "Volumen de los sonidos del menu"; ui_fx y ui_sfx). Con los efectos
apagados los widgets vuelven a ser los de siempre. Mando: la navegacion de ImGui no se toco.

## Color del menu e imagen de fondo (F1 > Misc > Interfaz)
- Color de acento: 8 colores listos (se pueden elegir con el mando) y un selector libre con rueda de color; todo el menu lo sigue
  (pestanas, botones, casillas, sliders, listas, brillos, cursor del mando, fondo animado y borde). Ajuste ui_accent. style()
  vuelve a ejecutarse al cambiarlo (sStyleDirty).
- Imagen de fondo: "Elegir imagen..." abre el selector de archivos del sistema (PNG, JPG, BMP, GIF o TGA; stb_image en
  port/third_party/stb, dominio publico); se recorta para llenar la ventana y se oscurece con un deslizador (ui_wall_dim) para
  que el texto se lea; las imagenes grandes se reducen a 2048 px. La ruta se guarda en ui_wallpaper.txt junto a los ajustes;
  "Quitar imagen" la borra. Archivos: gs/ui.cpp, gs/ui_image.cpp, gs/ui_fx.cpp.

## Reproductor de musica interno (F1 > Musica)
gs/jukebox.c (+ jukebox.h): reproduce MP3 / WAV / FLAC de una carpeta elegida (selector de carpetas del sistema; la carpeta se
guarda en ui_music.txt). Motor: el miniaudio del port (snd_ma.c: PortMa_Engine). Opciones (ajustes jb_*): volumen propio, orden
aleatorio (sin repetir las ultimas 8), Fade In y Fade Out por separado, duracion del fundido (1-10 s), Crossfade (propia opcion:
el Fade Out de una cancion suena a la vez que el Fade In de la siguiente; sin ella no se cruzan), igualar volumen, empezar al abrir
el juego, SELECT = siguiente cancion, nombre de la cancion (con efectos; opcion para dejarlo siempre visible) y visualizador
(64 barras, FFT de lo que suena, con reflejo, picos, destello con el bajo; tamano ajustable).
Igualar volumen: un hilo de fondo decodifica cada cancion una vez, mide su nivel medio (bloques de 400 ms sobre -50 dBFS) y su
pico y calcula una ganancia hacia -17 dBFS sin pasar de fondo de escala; se guarda en jukebox_cache.txt.
SELECT: con la opcion activa, en combate (todos los modos, online incluido) SELECT pasa a la siguiente cancion y no cambia la camara
(Port_CamSelectOff); en Entrenamiento sigue grabando. Solo es musica: no toca la simulacion.
La musica propia del juego no se toca (se baja en las opciones del propio juego).

## Opacidad del menu F1
F1 > Misc > Interfaz > "Opacidad del fondo del menu" (ui_panel): 0-100 %; con 0 el panel es transparente.

## Correccion: tamano del menu al mover la opacidad
style() se vuelve a ejecutar mientras un deslizador pide cambios (opacidad, color); dejaba el menu sin la escala elegida
("Menu Size") hasta el cuadro siguiente, asi que la ventana saltaba entre dos tamanos. Ahora style() termina volviendo a aplicar la
escala (ui_scale_apply) y conserva el tamano de fuente.

## Visualizador y nombre de la cancion: posicion y tamano libres (F1 > Musica)
Visualizador: ancho 10-100 %, alto 10-200 %, posicion horizontal y vertical 0-100 % del espacio libre (puede cubrir toda la
pantalla). Nombre: tamano 40-300 %, posicion horizontal y vertical. Ambos: "Mover con el raton" muestra un marco que se arrastra
(el elemento se ve aunque no suene musica) y "Restablecer". Ajustes jb_viz_x/y/w/size, jb_name_x/y/scale.

## Pantalla dividida (solo en replays) y SELECT en todos los modos
- Hallazgo: el online ya corre como la pelea de dos jugadores del juego (modo dividido por dentro, con sus dos camaras) y solo
  muestra la vista local a pantalla completa (btl_cam.c, BtlCam_UpdateOverride + Port_NetView). El codigo que DIBUJA consume el
  generador de la C que comparte la pelea y el checksum de la pelea incluye su estado (docs/netplay_notes.md): dibujar dos vistas
  en una sola maquina consumiria mas numeros que el cliente oficial y se desincroniza. Por eso NO se activa en partidas en vivo.
- En replays si (nadie mas depende de ello): F1 > Replays > "Reproducir las replays en pantalla dividida" (rep_split), boton
  "Dividida / Una vista" en la barra de la replay y el atajo L1+L2+R1+R2 a la vez (opcion rep_split_key). Solo cambia que
  BtlCam_UpdateOverride deje de forzar una vista (Port_SplitWanted, plat_replays.c). Solo puede diferir el resultado de la replay
  donde un doble KO lo decide ese generador.
- SELECT = siguiente cancion se procesa ahora en Battle_Loop (battle.c), una vez por cuadro, en todos los modos salvo Entrenamiento
  (online, offline, personalizadas, historia, replays).
- Visualizador: la linea base puede llegar al borde inferior (0 = parte alta de las barras arriba, 100 = pie abajo); el marco de
  arrastre es solo el de las barras.
