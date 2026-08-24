# Firmware — Car Companion

Firmware usando **ESP-IDF** (no Arduino IDE) + **LVGL** para la interfaz.

Placa de prototipo actual: **M5Stack Core2** (ESP32 clásico, no ESP32-S3). Se
había planeado usar el CoreS3, pero llegó un Core2 (posible confusión al
comprar) y se decidió seguir con este para no bloquear la validación de
`obd_driver` — ver nota en `docs/roadmap.md`. Esto implica:
- `idf.py set-target esp32` (no `esp32s3`).
- Del PSRAM de 8MB del Core2, el ESP32 clásico solo mapea 4MB (limitación de
  espacio de direcciones, no pasa en el S3) — visto en el log de boot, no es
  un bug, es esperable.
- Si más adelante se consigue el CoreS3 real, la lógica de `obd_driver`,
  `pid_engine` y `state_store` es portable sin cambios; lo que sí cambiaría
  es todo lo específico de placa (PMIC, driver de pantalla) cuando se escriba `ui`.

## Por qué ESP-IDF y no Arduino

Arduino framework es más simple para empezar, pero para un producto comercial con
requisitos de rendimiento ("muy rápida") y bajo consumo, ESP-IDF te da control real
sobre FreeRTOS (tareas, prioridades, colas), gestión de energía y particiones de
memoria. Vale la pena la curva de aprendizaje extra ahora, antes de tener miles de
líneas de firmware que migrar después.

## Setup

```bash
# 1. Instalar ESP-IDF v5.x (una sola vez)
git clone -b v5.3 --recursive https://github.com/espressif/esp-idf.git ~/esp-idf
cd ~/esp-idf && ./install.sh esp32s3

# 2. Activar el entorno (cada vez que abras una terminal nueva)
. ~/esp-idf/export.sh

# 3. Configurar el target (esp32 para el Core2 actual; esp32s3 si es CoreS3)
cd firmware
idf.py set-target esp32

# 4. Compilar
idf.py build

# 5. Flashear (con la placa conectada por USB)
idf.py -p /dev/ttyUSB0 flash monitor
# en Windows, el puerto es tipo COM3 (ver Administrador de dispositivos)
# en macOS suele ser /dev/cu.usbserial-XXXX o /dev/cu.wchusbserialXXXX (ver "ls /dev/cu.*")
```

### Ojo con `sdkconfig` al clonar o hacer pull

`firmware/sdkconfig` está en `.gitignore` (es correcto: es un archivo
generado). Los ajustes que el proyecto necesita viven en
**`sdkconfig.defaults`**, que ESP-IDF solo aplica cuando `sdkconfig` todavía
no existe. Si ya tenías un `sdkconfig` de antes, los defaults nuevos NO se
aplican solos y vas a ver bugs raros (colores invertidos, fuentes que faltan,
crashes por memoria de LVGL). Para forzar que se regenere:

```bash
cd firmware
rm -f sdkconfig
idf.py set-target esp32
```

### Troubleshooting en macOS sin Homebrew

Instalado y probado en una Mac sin Homebrew (19 ago) — dos problemas que van a
volver a aparecer si se repite el setup en otra máquina así:

- **`"cmake" must be available on the PATH`** al correr `idf.py set-target`:
  en macOS/Linux (a diferencia de Windows) `install.sh` NO baja `cmake` ni
  `ninja`, asume que ya están instalados (normalmente vía
  `brew install cmake ninja`). Si no querés instalar Homebrew, alternativa
  más liviana — instalarlos directo en el venv de ESP-IDF:
  ```bash
  ~/.espressif/python_env/idf5.3_py3.9_env/bin/python -m pip install cmake ninja
  ```
  Quedan en el `bin/` del venv, que `export.sh` ya agrega al PATH.

- **`idf.py` dice que falta un paquete Python al azar** (`construct`,
  `ruamel.yaml`, `importlib_metadata`, `pyclang` — cambia cada vez que
  corrés el comando) pese a que `install.sh` terminó bien: es un bug real de
  `ruamel.yaml` con el `importlib.metadata` del Python 3.9 de Apple (Xcode
  CLT) — pip genera la carpeta de metadata como `ruamel_yaml-x.y.z.dist-info`
  (guión bajo) pero ese `importlib.metadata` busca `ruamel.yaml-x.y.z.dist-info`
  (con punto) y no la encuentra. El nombre de paquete que reporta el error es
  además engañoso (hay un bug de variable en
  `tools/check_python_dependencies.py` de ESP-IDF que loguea el paquete
  incorrecto). Fix — symlink con el nombre que espera:
  ```bash
  cd ~/.espressif/python_env/idf5.3_py3.9_env/lib/python3.9/site-packages
  ln -s ruamel_yaml-*.dist-info "$(ls -d ruamel_yaml-*.dist-info | sed 's/ruamel_yaml/ruamel.yaml/')"
  ```

## Estructura de componentes

Cada capa de la arquitectura es un **componente ESP-IDF independiente**, con su propia
interfaz pública (`include/`) y su implementación privada. Esto es intencional: te
obliga a no mezclar, por ejemplo, lógica de parsing de PIDs con código de LVGL.

```
firmware/
├── main/                      # Punto de entrada, arranca las tareas de FreeRTOS
├── components/
│   ├── obd_driver/            # Habla con el Vgate vLinker MC+ por BLE (SPP/GATT + protocolo ELM327)
│   ├── pid_engine/            # Traduce respuestas OBD crudas a valores con significado (RPM, boost, etc.)
│   ├── state_store/           # Estado central en memoria que alimenta la UI (patrón pub/sub simple)
│   ├── ui/                    # Pantallas LVGL — consumen state_store, nunca hablan con obd_driver directo
│   ├── connectivity/          # WiFi provisioning, OTA, sync HTTP con el backend
│   └── storage/               # Persistencia local (NVS / SD) de historial antes de sincronizar
```

### Regla de dependencias (importante para que esto escale)

```
obd_driver → pid_engine → state_store → ui
                              ↑
                       connectivity / storage
```

`ui` **nunca** debe importar `obd_driver` directamente. Todo pasa por `state_store`.
Esto es lo que te permite, más adelante, cambiar de adaptador OBD o agregar soporte
para otro vehículo sin tocar una sola pantalla.

**Excepción deliberada (22 ago):** `ui` sí importa `storage` directamente para
leer el historial de viajes (pantalla Viaje). La regla de arriba protege contra
que `ui` dependa de *cómo se adquieren* los datos del auto (BLE, PIDs, timing);
leer una lista de registros ya persistidos es una preocupación distinta —no hay
"cómo se llega al dato" que filtrar, es lectura de un archivo. Forzarlo a pasar
por `state_store` (que modela el estado *actual* del vehículo, no un historial)
hubiera significado inventar un mecanismo de paginación ahí solo para esto. Si
en algún momento esto se siente mal (ej. varias pantallas necesitan lo mismo),
vale la pena revisar. Ver `pid_engine`/`state_store` para el patrón de "buzón"
que sí se usa para *acciones* puntuales (ej. pedir una lectura de DTC).

## Estado actual (Fase 1 en progreso)

- ✅ `state_store` — implementado (mutex, setters, suscriptores).
- ✅ `pid_engine` — parseo real de PIDs estándar (RPM, velocidad, temp, carga,
  temp admisión, MAP/boost aproximado, voltaje). Lógica de conversión aislada
  en `pid_math.c/h` sin dependencias de ESP-IDF, testeada en
  `host_tests/test_pid_math.c` (corre con `gcc` normal, sin hardware — ver
  ese archivo para el comando exacto, y el workflow de CI en
  `.github/workflows/firmware-host-tests.yml`).
- ⚠️ `obd_driver` — implementado con NimBLE (escaneo, conexión, descubrimiento
  de servicio/característica, suscripción a notificaciones, envío de comandos
  ELM327). Revisado y endurecido (14 ago): se corrigió un bug de falsos
  errores en el descubrimiento de servicios, y se agregó un timeout real por
  comando (antes, si el adaptador no respondía, el driver quedaba trabado
  para siempre — ahora se recupera solo a los 3s).

  **Compila y corre en hardware real desde el 19 ago** (Core2, target `esp32`).
  Al compilar por primera vez aparecieron dos errores reales, ya corregidos:
  - `#include "host/ble_gattc.h"` no existe en esta versión de NimBLE — las
    funciones `ble_gattc_*` están declaradas en `host/ble_gatt.h`.
  - `s_response_len` se usaba en `on_response_timeout()` antes de estar
    declarada más abajo en el archivo — se movió la declaración del buffer
    de respuesta arriba de esa función.

  El log de boot confirma `obd_driver_init OK` y que arranca el escaneo BLE
  buscando "vLinker" sin panics.

  **UUIDs BLE confirmados (19 ago) con nRF Connect** contra el adaptador real
  ("vLinker MC-IOS"): servicio `18F0`, notify `2AF0`, write `2AF1`. Se
  verificó escribiendo `ATZ` (hex `41545A0D`) en `2AF1` con notificaciones
  activas en `2AF0`, y el adaptador respondió `ELM327 v2.2` — banner de boot
  estándar, confirma que es el canal correcto. Ya cargados en
  `components/obd_driver/include/obd_driver_config.h`.

  **Probado end-to-end en el Maxus T60 el 19 ago — funciona.** El firmware
  conecta solo, descubre servicio/características por UUID (ya no por
  nombre — el vLinker no anuncia su nombre en el BLE advertising/scan
  response, nRF Connect lo mostraba porque lo lee por GATT recién después de
  conectar; el filtro de discovery ahora matchea por el UUID de servicio
  `0x18F0` en vez de por nombre, ver `gap_event_handler` en `obd_driver.c`),
  manda la secuencia de init ELM327 y arranca el polling de PIDs. Log real
  visto en el auto (llave en contacto, motor apagado):
  `RPM=0 vel=0km/h coolant=36C bat=12.5V carga=0% MAP=101kPa CEL=off`.

  En el camino se encontraron y corrigieron dos bugs reales de concurrencia
  que solo aparecían con hardware real respondiendo (no en la lógica pura
  de `pid_math`):
  - `s_cmd_mutex` era un mutex real de FreeRTOS, tomado en una tarea y
    liberado desde otra (la tarea de NimBLE al llegar una respuesta, o el
    timer de timeout) — un mutex real trackea dueño para priority
    inheritance y esto crasheaba el firmware (`assert xTaskPriorityDisinherit`)
    apenas llegaba la primera respuesta real del adaptador. Cambiado a
    semáforo binario.
  - La secuencia de init (`ATZ`/`ATE0`/`ATL0`/`ATSP0`) marcaba `s_ready = true`
    de forma optimista apenas mandaba los 4 comandos, sin esperar las
    respuestas — el polling de PIDs (que arranca apenas `s_ready` es true)
    se pisaba con las respuestas de init todavía en vuelo. Ahora es una
    cadena asíncrona, un comando a la vez, y `s_ready` solo se pone en true
    después de la última respuesta.

  **Confirmado con motor encendido (19 ago):** RPM se movió en la zona de
  ralentí del diésel (~700-1100, con picos al acelerar) y volvió a 0 al
  apagar. Bonus: el voltaje de batería subió de ~12V (motor apagado) a
  ~13.6-14.3V (alternador cargando con motor prendido) — confirma en forma
  independiente que los datos son reales y no un valor fijo/basura.
  `obd_driver` queda validado end-to-end contra hardware real.

  **PIDs ampliados (19 ago):** se agregó una consulta de "descubrimiento"
  (modo 01, PID `0x00`/`0x20`/`0x40`/`0x60` — el estándar para preguntarle a
  la ECU qué PIDs soporta) que corre una vez al conectar y loguea el
  bitmask crudo. Con eso se confirmó que el Maxus T60 soporta, además de los
  6 originales: `0x01` (monitor status / check engine real, antes
  hardcodeado en "off"), `0x11` (posición del acelerador), `0x23`
  (**presión de riel de combustible**, común-rail diesel), `0x33`
  (barométrica), `0x46` (temperatura ambiente), `0x5E` (caudal de
  combustible L/h). Ya implementados y agregados al poll list — fórmulas en
  `pid_math.c` con tests en `host_tests/test_pid_math.c`.

  De paso se corrigió `boost_pressure_kpa`: antes guardaba el MAP crudo sin
  compensar (con el comentario original ya advirtiendo que eso no era boost
  real); ahora se calcula MAP − barométrica de verdad
  (`update_boost_estimate()` en `pid_engine.c`). Confirmado con motor
  apagado: `boost=-1kPa` (correcto, ~0 con el motor parado) en vez del
  MAP crudo (~101kPa) que mostraba antes.

  No apareció nada de boost de turbo real ni EGT en el bitmask estándar —
  como se esperaba, eso sigue siendo un PID propietario del fabricante,
  pendiente de ingeniería inversa a futuro (ver `docs/pid-mapping.md`).

  **Lo que falta:** limpiar el logging de debug agregado para diagnosticar
  el escaneo (`disc: addr=...` por cada dispositivo BLE cercano, bastante
  ruidoso — útil para debug, no para uso normal) antes de considerar esto
  terminado del todo. Después de eso, el siguiente paso lógico es empezar
  `ui` (pantallas LVGL), ya con datos reales confirmados para mostrar.
- ✅ `ui` — implementada y **funcionando en la pantalla real** (19-20 ago).
  Usa LVGL (`lvgl/lvgl`) + driver de panel (`espressif/esp_lcd_ili9341`,
  managed components vía `components/ui/idf_component.yml`) sobre el Core2:
  - `core2_power.c` — init del AXP192 (PMIC) por I2C. El Core2, a diferencia
    del M5Stack clásico, alimenta la lógica y el backlight de la pantalla a
    través de rieles del AXP192 (no un pin directo), y el reset del panel es
    un GPIO expandido del AXP192 (GPIO4), no un pin del ESP32. Sin este paso
    la pantalla queda apagada aunque el SPI esté bien. La secuencia de
    registros está calcada de `AXP192::begin()` de la librería oficial de
    M5Stack, **no de memoria** — ver la advertencia de abajo.
  - `core2_display.c` — bus SPI + panel ILI9342C (320x240).
  - `core2_touch.c` — panel táctil capacitivo FT6336U (comparte el bus I2C
    con el AXP192; no reinstala el driver de I2C, solo lee registros).
  - `ui.c` — `lv_init`, buffers de dibujo, tarea de tick/refresco con mutex
    propio, dashboard (arco de RPM + tarjetas de valores con código de
    colores) y menú táctil con navegación a pantallas secundarias.

- ✅ `storage` — implementado (22 ago), primer pedazo real de Fase 2.
  Agrega una partición FAT dedicada de ~14MB (`firmware/partitions.csv` — el
  Core2 tiene 16MB de flash y la tabla "single app" default solo usaba
  ~1MB, dejaba el resto sin particionar) y arma el historial de viajes
  solo, suscrito a `state_store`: un viaje empieza cuando el RPM pasa a ser
  mayor que 0 con datos válidos, y termina si el OBD se desconecta o el
  motor queda en 0 RPM sostenido 2 minutos (para no cortar el viaje en cada
  semáforo). Guarda distancia (integrada de velocidad), combustible usado
  (integrado del caudal), RPM máximo, temp máxima, batería mínima y si hubo
  CEL. Viajes de menos de 30s se descartan. **Ojo con el orden de
  `app_main.c`**: `storage_init()` se suscribe a `state_store` internamente,
  así que tiene que ir DESPUÉS de `state_store_init()` — al revés crashea
  (mutex nulo), se probó en hardware real el 22 ago.

  `start_time_s` de cada viaje sigue siendo segundos desde el arranque del
  ESP32 en el archivo on-disk (no se tocó ese formato) — pero `connectivity`
  (ver más abajo) ya resuelve la hora real **al armar el JSON de sync**, vía
  SNTP + `boot_epoch = time(NULL) - esp_timer_get_time()/1e6`.

  Confirmado en hardware real: la partición monta, el filesystem se
  formatea solo en el primer arranque, no rompe nada del resto del sistema.
  **No confirmado todavía:** el ciclo completo de un viaje real (arrancar,
  manejar, apagar, ver el registro guardado) — falta probarlo la próxima
  vez que se maneje el auto con esto flasheado.
- ✅ `connectivity` — implementado el 23 ago (WiFi + sync HTTP con el
  backend). Ver sección dedicada más abajo.

### Trampas de bring-up del Core2 (costaron una tarde entera, no repetirlas)

1. **`swap_xy` NO va en el Core2 — este fue EL bug de fondo.** El driver que
   se reusa es el del ILI9341 (nativo 240x320 vertical, que sí necesita el
   swap para quedar horizontal), pero el Core2 monta un **ILI9342C, que es
   nativamente 320x240 horizontal** — confirmado en la fuente de M5GFX
   (`Panel_ILI9342.hpp`: `memory_width=320, memory_height=240`). Poner
   `esp_lcd_panel_swap_xy(panel, true)` sobre un panel que ya es horizontal
   deja al controlador direccionando 240 columnas mientras se le mandan 320
   píxeles por fila: cada fila se desborda 80 píxeles y la imagen se corre
   acumulativamente.

   Síntomas exactos, por si vuelven a aparecer: rellenos de color sólido
   **perfectos** (correrse no se nota si todo es del mismo color), texto y
   detalle fino corridos/fantasma, y una franja inferior gris con textura de
   píxeles (que es GRAM que nunca se llegaba a escribir, no un borde físico
   del panel).

   **Cómo se encontró, que es lo reutilizable:** volcando por el log, como
   arte ASCII, el contenido del buffer de LVGL justo antes de mandarlo por
   SPI (`disp_flush_cb`). En el log se leía `RPM: --` perfecto, lo que probó
   de una que LVGL renderizaba bien y que el bug estaba sí o sí en el camino
   buffer→panel. Eso cortó el ciclo de probar-y-mirar-la-foto, que ya llevaba
   muchas iteraciones sin converger. Si algo visual vuelve a fallar, hacer
   esto **primero**, antes de cambiar configuraciones a ver si pega.

2. **Nunca escribir un registro del AXP192 entero sin leerlo antes.** La
   primera versión hacía `write(0x12, 0xFF)` para "encender todo" y de paso
   encendió el LDO3, que en el Core2 es **el motor de vibración** — quedó
   vibrando sin parar y sobrevivía al apagado con el botón (el AXP192 tiene
   dominio de respaldo). No alcanza con "no tocarlo": hay que apagar ese bit
   explícitamente. Usar siempre read-modify-write (`axp192_rmw`).

3. `CONFIG_LV_COLOR_16_SWAP=y` es necesario (el panel espera los bytes de
   cada píxel RGB565 al revés de como LVGL los arma por default).

4. La tarea de LVGL usa `vTaskDelay(pdMS_TO_TICKS(10))`, no 5: con
   `CONFIG_FREERTOS_HZ=100` (tick de 10ms), `pdMS_TO_TICKS(5)` redondea a 0
   ticks, la tarea nunca cede CPU y salta el watchdog.

5. `lv_disp_flush_ready()` va en el callback `on_color_trans_done` del panel,
   **no** al volver de `esp_lcd_panel_draw_bitmap()` (que solo encola la
   transferencia DMA, no la completa).

## `avg_consumption` definido y calculado (24 ago)

Quedaba pendiente desde que se armó `connectivity` (ver
`docs/api-contract.md`): el campo `avg_consumption` del payload de sync
existía en el schema del backend pero sin unidad definida, así que el
firmware lo omitía. Definido como **L/100km** (estándar en Chile, más
intuitivo que km/L para un diesel: menor número = más eficiente). El
backend no necesitó ningún cambio — `POST`/`GET /sync/trips` ya
insertaban/devolvían la columna `avg_consumption NUMERIC(6,2)` sin asumir
una unidad, solo faltaba que el firmware la calculara.

`connectivity.c` ahora la computa por viaje como `fuel_used_l / distance_km
* 100`, y **omite el campo** (no lo manda) si `distance_km == 0` o si
`fuel_used_l == 0` — este último caso no es "el viaje gastó cero
combustible", es que el vehículo no expone el PID de caudal (0x5E) y
`fuel_used_l` nunca se acumuló durante el viaje (mismo criterio "0 = sin
dato" que ya usa el resto del codebase para PIDs no soportados). Confirmado
en hardware que compila y arranca limpio — **falta confirmar con un viaje
real** que el número calculado tenga sentido (comparar contra el consumo
real conocido del Maxus).

## Consumo de batería (24 ago)

Erick midió ~5% de batería del M5 en menos de 10 minutos, prendido pero sin
estar conectado al auto ni al WiFi de casa — el caso que más importa en la
práctica (el dispositivo va a pasar la mayor parte del tiempo prendido en
esa situación: guardado, o en el auto pero apagado). Dos causas reales
encontradas:

- **Escaneo BLE al 100% de duty cycle, para siempre**: `start_scan()` pasaba
  `itvl=0, window=0` a NimBLE, que los reemplaza por sus defaults de "scan
  rápido" (`BLE_GAP_SCAN_FAST_INTERVAL_MIN` = `BLE_GAP_SCAN_FAST_WINDOW` =
  30ms) — con intervalo igual a ventana, el radio BLE escanea sin parar,
  literalmente el 100% del tiempo, mientras no haya adaptador conectado.
  Cambiado al perfil `SLOW1` que ya trae NimBLE para descubrimiento en
  segundo plano (11.25ms de escaneo cada 1280ms, ~1% de duty cycle en vez de
  100%). Costo: hasta ~1.3s más para encontrar el adaptador al subirse al
  auto — imperceptible comparado con el ahorro el resto del tiempo.
- **WiFi sin modo de ahorro de energía**: sin `esp_wifi_set_ps()`, el radio
  de WiFi queda en `WIFI_PS_NONE` (siempre activo) — tanto conectado como
  reintentando en loop. Agregado `WIFI_PS_MIN_MODEM` (el radio duerme entre
  balizas del punto de acceso), único costo real: algo más de latencia en la
  sync, que ya corre en segundo plano sin apuro.

**Rediseño más grande, decidido con el usuario (24 ago)**: el radio WiFi
ahora queda **apagado por default**, no solo con ahorro de energía. Antes,
`connectivity_init()` prendía WiFi al arrancar y lo dejaba activo para
siempre (reintentando conectar todo el tiempo que no hay WiFi de casa
cerca, que es la mayoría del uso real). Ahora `wifi_init_once()` solo
configura el stack de WiFi sin prender el radio; una tarea de fondo
(`attempt_sync_window()`) lo prende por una ventana acotada
(`WIFI_CONNECT_WINDOW_MS`, 20s) cada `SYNC_CHECK_INTERVAL_MS` (~12min)
**solo si hay algún viaje pendiente de sincronizar** (chequeo local,
`storage_get_pending_sync_count()`, no necesita el radio prendido), o al
tocar el botón de sync en Viaje — y lo apaga de nuevo al terminar el
intento, haya conectado o no. Confirmado en hardware: con 0 viajes
pendientes, el radio WiFi ni siquiera se prende al arrancar (antes, cada
boot mostraba `wifi:mode : sta` y `connected with ...` de entrada; ahora
esas líneas no aparecen para nada).

**Falta confirmar en el auto real**: (1) que el ciclo automático de ~12min
efectivamente prenda el radio cuando hay un viaje real pendiente y lo
sincronice bien; (2) volver a medir cuánta batería baja en 10 minutos sin
conectar a nada, para comparar contra el ~5%/10min original.

**Quedó afuera a propósito, por ser más riesgoso**: habilitar
`CONFIG_PM_ENABLE` + tickless idle dejaría que el CPU baje de frecuencia o
entre en light-sleep en momentos ociosos — probablemente el ahorro más
grande de todos, pero light-sleep con BLE+WiFi activos a la vez necesita
soporte específico de coexistencia y puede introducir problemas de timing
sutiles (respuestas BLE perdidas, desconexiones) que no vale la pena
arriesgar sin poder probarlo con calma en el auto real. Si el consumo sigue
siendo alto después de este cambio, es el siguiente lugar a mirar.

Otro sospechoso menor no tocado: el backlight de la pantalla está fijo a
2.80V (`DCDC3` en `core2_power.c`), sin lógica de brillo — bajarlo ahorraría
algo pero podría afectar la visibilidad al sol dentro del auto, así que no
se tocó sin pedirlo explícitamente.

## Auditoria de memoria y casos borde (24 ago)

Repaso completo de todos los componentes buscando fugas de memoria, stacks
justos, condiciones de carrera entre tareas, y fallas silenciosas — con el
sistema completo (BLE+WiFi+LVGL+TLS) ya andando, la RAM interna quedó
demostradamente justa (ver la sección de TLS más abajo), así que valía la
pena revisar todo con esa lente. Arreglado:

- **Condicion de carrera real en `storage`**: `on_state_change` corre tanto
  desde la tarea del host de NimBLE (los setters normales de PIDs) como
  desde la tarea de `pid_engine` (via `state_store_set_disconnected()`), sin
  ningun mutex — dos llamadas casi simultaneas podian guardar el mismo viaje
  dos veces, o pisarse escribiendo `maint.bin`/`trips.bin` al mismo tiempo
  (FATFS no tiene lock propio acá, `CONFIG_FATFS_FS_LOCK=0`). Agregado un
  mutex propio de `storage` que protege el viaje en curso, `s_maint`, y el
  cursor de sync — todas las funciones públicas y `on_state_change` lo
  toman.
- **Stack de la tarea de NimBLE demasiado justo**: los callbacks de notify
  bajan por toda la cadena `obd_driver → pid_engine → state_store →
  storage (puede escribir a FAT) + ui (toma el mutex de LVGL)`, todo en el
  stack de *esa* tarea — 4KB (default) era poco margen para eso. Subido a
  8KB. Se agregó además `uxTaskGetStackHighWaterMark()` al log de salud
  (`pid_engine`, cada 5min) para las tareas de `pid_engine` y de NimBLE —
  antes no había ningún dato medido, solo la estimación de esta auditoría.
- **El init del ELM327 podía quedar colgado para siempre**: si el ELM327
  nunca respondía al primer paso (`ATZ`) — el código ya admitía que no
  espera confirmación del write del CCCD antes de mandarlo — no había
  reintento: `obd_driver_is_connected()` quedaba en `false` para siempre
  aunque el enlace BLE siguiera vivo. Agregado reintento (hasta 3 veces) y,
  si se agotan, `ble_gap_terminate()` para forzar una reconexión completa en
  vez de quedar colgado.
- **Los flags "Leyendo.../Borrando..." de Fallas podían quedar pegados**: si
  el envío del comando fallaba, o si se enviaba bien pero la respuesta nunca
  llegaba (el timeout de 3s de `obd_driver` se resuelve en silencio), nada
  apagaba `dtc_read_in_progress`/`dtc_clear_in_progress` — la pantalla
  quedaba mostrando "Leyendo..." indefinidamente. Arreglado en los dos
  casos: se revierte el flag si el envío falla, y `poll_task` tiene un
  watchdog de 5s que lo fuerza a apagarse si nunca llegó respuesta.
- **Códigos de retorno de NimBLE ignorados**: `ble_gap_connect`,
  `ble_gattc_write_no_rsp_flat`, `ble_gattc_disc_svc_by_uuid` y
  `ble_gattc_disc_all_chrs` no chequeaban su `rc` — una falla ahí dejaba al
  dispositivo sin OBD hasta un reboot, sin ningún log que lo explicara.
  Todos ahora loguean y, donde corresponde (`ble_gap_connect`), reintentan
  el escaneo.
- **`connectivity`: token JWT truncado en silencio, y viajes marcados como
  sincronizados sin haberse mandado**: si la respuesta HTTP no entraba en el
  buffer local, se cortaba sin avisar (un token truncado da un 401 sin
  ninguna pista de por qué); y si `cJSON_CreateObject()` fallaba por falta
  de memoria armando el JSON de un viaje, ese viaje igual se contaba como
  sincronizado y `storage_mark_trips_synced` lo marcaba como tal — pérdida
  de datos definitiva y silenciosa. Ambos casos ahora loguean y, en el
  segundo, se corta el batch en el primer viaje que falla (mantiene el
  cursor de sync contiguo).
- **Reconexión WiFi sin backoff**: cada desconexión disparaba un
  `esp_wifi_connect()` inmediato — fuera del alcance del WiFi de casa (la
  mayor parte del tiempo real de uso), eso es un bucle intento-fallo
  continuo gastando CPU/batería en la misma RAM interna que ya está
  compartida con NimBLE/LVGL/mbedTLS. Agregado backoff exponencial simple
  (1s → 2s → 4s... tope 30s, reseteado al reconectar).
- **`ui`: patrón `off += snprintf(...)` sin acotar**: en las pantallas de
  Fallas y Diagnóstico, si algún campo alguna vez creciera más de lo
  esperado, `off` podía superar el tamaño del buffer y la siguiente llamada
  pasaría un tamaño negativo (interpretado como un `size_t` gigante) —
  escritura fuera del buffer. No explotable con los tamaños actuales, pero
  sí una trampa para el próximo campo agregado. Reemplazado por un helper
  `buf_append()` que nunca deja que `off` supere el tamaño del buffer.

**Quedó pendiente, documentado a propósito para no meter una reescritura
grande sin poder probarla en el auto real:**
- Un viaje en curso vive solo en RAM hasta que termina — un corte de
  energía en medio de un viaje largo lo pierde entero (junto con los km que
  debían descontarse de aceite/filtro). Un checkpoint periódico a flash
  arreglaría esto, pero es una función nueva de por sí, mejor como su propia
  sesión.
- El watchdog de comando de `obd_driver` puede, en un caso borde raro (una
  respuesta tardía llegando justo después de que ya se armó el siguiente
  comando), entregarle a un callback datos de un comando anterior.
  Ya se mitigó la parte más común (bytes viejos pegándose al inicio de la
  respuesta siguiente, con un reset de buffer al armar cada comando), pero
  una correlación completa por número de secuencia es un cambio más
  invasivo al protocolo interno — se dejó afuera de esta pasada.

## `connectivity`: sync de viajes al backend (23 ago)

Implementado el componente `connectivity` (hasta ahora solo un stub sin
usar). Flujo: WiFi STA + SNTP arrancan en `connectivity_init()`; una tarea
de fondo chequea cada 30s si hay WiFi conectado y viajes pendientes — si no
hay nada que hacer, el chequeo es gratis (cero llamados de red). Cuando
corresponde: login contra `/auth/login` (credenciales de
`connectivity_secrets.h`, no versionado) para sacar un JWT fresco, registro
idempotente del dispositivo (`device_uid` = MAC en hex) contra
`/devices`, y `POST /sync/trips` con hasta `SYNC_BATCH_MAX` (20) viajes por
tanda. `storage` ahora guarda un cursor real (`/storage/sync.bin`,
`storage_mark_trips_synced`) — antes `storage_get_pending_sync_count`
siempre devolvía el total, nunca se había sincronizado nada.

**Setup manual necesario (no lo puedo hacer yo por vos):** el archivo
`firmware/components/connectivity/include/connectivity_secrets.h` ya existe
(con valores de ejemplo, para que compile) — completalo con tu WiFi de casa
y tu email/contraseña reales del backend (ese archivo no se sube a git).
**No me pases la contraseña por acá** — editalo vos directo con un editor de
texto, parado en la raíz del repo:
```bash
open -e firmware/components/connectivity/include/connectivity_secrets.h
```

**Decisiones tomadas con el usuario (23 ago, ver conversación):**
- Auth: login hardcodeado (no token de dispositivo separado — eso queda
  pendiente, ver `docs/api-contract.md`).
- Disparo: automático en segundo plano, no un botón manual.

**Bugs reales encontrados armando esto (primera tanda, con credenciales de
ejemplo — WiFi nunca llegaba a conectar de verdad):**
- **IRAM overflow al linkear** (`iram0_0_seg overflowed by 6920 bytes`):
  BLE + WiFi + mbedTLS ya están justos de IRAM en el ESP32 clásico.
  Arreglado bajando `CONFIG_ESP32_REV_MIN` de "v0.0" (default) a "v3.0" —
  el M5 real es v3.1 (confirmado en el log de boot) y esto desactiva
  `SPIRAM_CACHE_WORKAROUND`, un fix de compilador que el propio Kconfig
  documenta como "no requerido para el rev 3 en adelante".
- **Crash real en hardware: `esp_wifi_init()` fallaba con "malloc buffer
  fail"** (RAM interna agotada, compartida entre NimBLE + LVGL + WiFi) y
  el código lo envolvía en `ESP_ERROR_CHECK` — eso llama a `abort()` y
  **reinicia todo el dispositivo**, justo lo que `connectivity.h` dice que
  nunca puede pasar ("debe seguir funcionando como gauge OBD standalone").
  Doble fix: (1) se sacaron todos los `ESP_ERROR_CHECK` de
  `wifi_and_sntp_start()`, cada paso ahora loguea y devuelve un error
  normal si falla; (2) se bajaron los buffers de WiFi
  (`CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM` y compañía, ver
  `sdkconfig.defaults`) de los defaults pensados para "solo WiFi" a valores
  chicos que alcanzan para sync HTTP ocasional. Confirmado en hardware:
  arranca limpio, reintenta conectar WiFi sin crashear cuando la red no
  existe (probado a propósito con credenciales de ejemplo).
- El backend en Railway (`/health`, `/auth/login` con credenciales
  incorrectas) responde exactamente como documenta `docs/api-contract.md` —
  confirmado con `curl` desde afuera del firmware, así que el contrato que
  asume `connectivity.c` es correcto.

**Segunda tanda de bugs reales (23 ago, mismo día): con WiFi de casa real
conectando de verdad, aparecieron tres problemas nuevos en el handshake TLS
mismo** — ver el punto 10 de "Próximo paso concreto" más abajo para el
detalle completo (RAM insuficiente en el handshake, stack corto en la tarea,
y la causa de fondo real: fragmentación de RAM interna, resuelta moviendo
mbedTLS a PSRAM). **Confirmado en hardware real: WiFi conecta, SNTP
sincroniza, y el login HTTPS contra el backend llega hasta la respuesta de
la aplicación** (400 por una contraseña de menos de 8 caracteres en
`connectivity_secrets.h` — dato de Erick, no un bug). **Confirmado el
24 ago: con la contraseña corregida, login + registro + sync de los 2
viajes de prueba funcionó de punta a punta** — verificado consultando
`GET /sync/trips` directo al backend con `curl`, los datos (distancia,
RPM máximo) coinciden exactamente con lo que mostraba la pantalla de Viaje.

**Bug real encontrado en esa misma verificación**: los dos viajes
sincronizados aparecían con la **misma fecha** en el backend, aunque eran
viajes distintos. Causa: `connectivity` reconstruía `started_at`/`ended_at`
sumando `start_time_s` (segundos desde el boot) al offset de hora real
**del arranque en que se sincroniza**, no del arranque en que el viaje
realmente pasó — válido solo si sync y viaje ocurren en el mismo arranque.
Como estos dos viajes eran de antes de que existiera `connectivity` y
recién se sincronizaron ahora (arranque distinto), la fecha reconstruida
era una aproximación incorrecta para ambos.

Arreglado agregando `trip_record_t.recorded_at_epoch_s` (hora real UNIX
aproximada, guardada por `storage` al cerrar el viaje si `connectivity` ya
había sincronizado por SNTP en ese arranque — 0 si no). Para pasarle esa
hora a `storage` sin crear una dependencia nueva ida y vuelta entre
componentes, se agregó `state_store_set_wall_clock_offset()` /
`state_store_get_wall_clock_offset()`: un valor de "reloj de pared" en
`state_store`, separado de `vehicle_state_t` (no es estado del vehículo, no
dispara `notify_subscribers`), que `connectivity` fija apenas SNTP
sincroniza y `storage` lee al cerrar cada viaje. `connectivity.c` ahora
prefiere `recorded_at_epoch_s` sobre la reconstrucción vieja cuando está
disponible.

**Efecto secundario aceptado**: el campo nuevo cambió el tamaño de
`trip_record_t`, así que `trips.bin` en el formato viejo ya no calza en
offsets fijos — `storage_init()` lo detecta (tamaño de archivo no múltiplo
del tamaño actual del struct) y reinicia el archivo con un `ESP_LOGW`. Los
2 viajes de prueba en el M5 se perdieron localmente con este cambio (ya
estaban sincronizados en el backend de todos modos, así que el dato real no
se perdió). Confirmado en hardware: arranca limpio después de la migración.

**Botón manual de sync (23 ago)**: la pantalla de Viaje suma un botón solo
con el ícono de sincronizar (arriba a la derecha, `LV_SYMBOL_REFRESH`, sin
texto — mismo criterio que el ícono de LEER en Fallas), que dispara
`state_store_request_sync()`. Mismo patrón "buzón" que la lectura/borrado
de DTC: `connectivity_task` lo consulta cada 1s (barato, un booleano) además
de su chequeo automático cada 30s, así un toque manual no tiene que esperar
todo el ciclo. Layout de Viaje también se reordenó: `VOLVER` y `◀ / ▶` bajan
al pie de la pantalla, dejando la esquina superior derecha libre para el
botón de sync (pedido real del 23 ago: se sentía apretado arriba).

## Robustez para manejo real (22 ago)

Repaso pensando en errores típicos de una manejada real (no del escritorio):
caída de BLE al arrancar el motor (el Vgate se resetea con la baja de
tensión del arranque), túneles/estacionamientos, adaptador que nunca se
enchufa, flash llena o corrupta. Encontrado y arreglado:

- **Bug real: `data_valid` nunca volvía a `false`.** Una vez que llegaba la
  primera lectura OBD, `state_store` marcaba `data_valid=true` para siempre
  — no existía ningún setter que lo volviera a `false`. Si el OBD se
  desconectaba a mitad de una manejada (escenario normal, no raro), el
  dashboard se quedaba mostrando "OBD OK" con los últimos datos conocidos
  como si siguieran siendo en vivo. Peor: `storage` usa justo ese flag para
  cerrar un viaje cuando el OBD se desconecta (ver `storage.c`) — esa rama
  nunca se ejecutaba, así que todos los viajes terminaban solo por el
  timeout de 15min de inactividad, nunca por desconexión real. Arreglado con
  `state_store_set_disconnected()`, llamado desde `pid_engine` apenas
  detecta la transición conectado→desconectado.
- **Logs de diagnóstico para revisar después de manejar**, todos vía
  `ESP_LOGW`/`ESP_LOGI` con tag `pid_engine`, capturables con la técnica de
  `docs/guia-de-pruebas.md`:
  - `"OBD desconectado, reintentando..."` — cada vez que se cae la conexión.
  - `"todavia sin conexion OBD (Xs)"` — cada ~60s mientras sigue sin
    conectar (ya sea porque nunca conectó desde el arranque, o después de
    una caída real). Sirve para distinguir "sigue buscando el adaptador" de
    "se colgó en silencio" al revisar el log de una manejada.
  - `"salud: heap_libre=X min_heap_visto=Y obd_conectado=Z uptime=Ns"` — cada
    5min, para pescar un memory leak lento en una manejada larga sin tener
    que estar mirando la pantalla en el momento.
- **Escrituras a flash (`storage.c`) ahora verifican el resultado de
  `fwrite`** en vez de asumir que siempre funciona — si la partición se
  queda sin espacio o hay un error real de flash, ahora queda un
  `ESP_LOGE` explicando qué se perdió (el viaje en curso, o el
  odómetro/último cambio de aceite) en vez de fallar en silencio.

## Próximo paso concreto

1. ~~Instalar ESP-IDF y correr `idf.py build`~~ — hecho el 19 ago.
2. ~~Confirmar los UUIDs BLE reales del Vgate con nRF Connect~~ — hecho.
   Servicio `18F0` / notify `2AF0` / write `2AF1`.
3. ~~Probar `obd_driver` end-to-end en el Maxus T60~~ — hecho, con motor
   apagado y encendido. Datos reales confirmados.
4. ~~Ampliar los PIDs y hacer que `ui` muestre los datos en la pantalla~~ —
   hecho el 19-20 ago (12 PIDs, dashboard con arco de RPM + tarjetas, menú
   táctil).
5. ~~Ampliar el dashboard (velocidad como dato héroe) + batería del M5~~ —
   hecho el 22 ago.
6. ~~`storage`: historial de viajes en flash~~ — hecho el 22 ago, ver arriba.
   **Falta probar un viaje real completo** (queda pendiente para la próxima
   vez que se maneje el auto).
7. **Pantallas secundarias:**
   - ~~**Viaje**~~ — hecho el 22 ago: lee `storage_get_trip*` y navega entre
     viajes guardados (prev/next). **Falta probar con un viaje real completo**
     (hasta ahora solo se vio el estado "SIN VIAJES").
   - ~~**Fallas (DTC)**~~ — hecho el 22 ago: lee códigos con modo 03 y ahora
     también los borra con modo 04 (mismo patrón "buzón" en `state_store`,
     duplicado para `dtc_clear`). Si el ECU tenía una falla realmente activa
     (no solo histórica), el código puede volver a aparecer en la próxima
     lectura — borrar limpia el registro, no la causa. **Falta probar de
     punta a punta con un código real** (leer, borrar, confirmar que
     desaparece o vuelve si la falla sigue activa).
   - ~~**Diagnóstico**~~ — hecho el 22 ago: PIDs crudos que no tienen tarjeta
     en el dashboard (carga motor, temp admisión/ambiente, presión
     barométrica, presión de riel) + estado de conexión y hace cuánto llegó
     el último dato. Solo lectura, se actualiza sola vía `state_store`.
   - ~~**Mantenimiento**~~ — hecho el 22 ago, **rediseñado el mismo día**
     tras la primera manejada de prueba real. Versión 1 (odómetro propio +
     intervalo fijo) resultó inútil en la práctica: el odómetro de este
     dispositivo arranca en 0 y no tiene forma de saber cuánto le quedaba
     realmente al auto (Erick ya había hecho el cambio de aceite antes de
     instalar esto, con ~20.000km reales restantes — el sistema marcaba
     "faltan 9981 km" sin sentido). Rediseño: `storage` ahora trackea
     `oil_km_remaining` y `filter_km_remaining` como contadores
     **independientes** (aceite y filtro no siempre se cambian juntos) que
     se pueden **ajustar a mano** desde la pantalla con botones -1K/-100/
     +100/+1K, para calibrar contra el odómetro real del auto. Cada viaje
     real resta su distancia de ambos contadores; un botón resetea uno al
     intervalo completo (`STORAGE_OIL_CHANGE_INTERVAL_KM` /
     `STORAGE_FILTER_CHANGE_INTERVAL_KM`, ambos 10.000 km de partida,
     ajustables). El cambio de formato de `/storage/maint.bin` hace que el
     archivo viejo no calce en tamaño — se detecta y se reinicia con un
     `ESP_LOGW` explicando por qué (confirmado en hardware: perdió el
     odómetro propio acumulado de 18.6km, pero **no** los viajes guardados,
     que viven en un archivo aparte).

     **Bug de UX real encontrado el mismo día usando la v1 en el auto:**
     Erick ajustó el contador a "faltan 1000km" con los botones de paso,
     tocó lo que entonces se llamaba "LISTO" (pensando que confirmaba el
     ajuste) y el contador saltó de vuelta a 10.000km — el botón estaba
     pegado a los de ajuste, con el mismo color y un nombre ambiguo. Fix
     (decidido con el usuario vía preguntas de diseño): el botón de reset
     ahora vive en su propia fila separada, en rojo (`CAMBIO HOY` en vez de
     `LISTO`), y pide **doble toque** — el primero solo arma el botón
     ("SEGURO? TOCA") por 5s, recién el segundo confirma; si no se toca de
     nuevo a tiempo se desarma solo. Ver `confirm_btn_event_cb` en `ui.c`.
     **Falta probar en la pantalla real** que los botones entran bien en
     320px de ancho y que el flujo de doble toque se siente natural (todo
     esto se compiló y flasheó, pero no se vio a ojo en el M5 todavía).
8. Probar el dashboard **manejando** (hasta ahora solo se validó detenido:
   velocidad siempre 0 y boost sin carga de turbo real).
9. PIDs propietarios del Maxus (boost real de turbo, EGT) — no están en el
   bitmask estándar, requieren ingeniería inversa. Ver `docs/pid-mapping.md`.
10. ~~`connectivity`: WiFi + sync de viajes al backend~~ — hecho el 23 ago,
   **confirmado funcionando de punta a punta en hardware real el 24 ago**:
   WiFi conecta, SNTP sincroniza, login + registro + sync de viajes
   funcionan, verificado consultando el backend directo con `curl` (los
   datos coinciden con lo que mostraba la pantalla de Viaje). Falta todavía
   decidir el token de dispositivo separado (ver `docs/api-contract.md`) y
   definir la unidad de `avg_consumption`.

   **Tres bugs reales más, encontrados recién con WiFi de verdad** (los del
   22-23 ago con credenciales de ejemplo solo cubrían "WiFi nunca conecta"
   — con una red real detrás, aparecieron problemas nuevos en el handshake
   TLS mismo):
   - **RAM interna insuficiente en el handshake TLS**: `mbedtls_ssl_setup
     returned -0x7F00` (`MBEDTLS_ERR_SSL_ALLOC_FAILED`) — los buffers TLS
     por default (16KB in + 4KB out) no entraban con BLE+WiFi+LVGL ya
     instalados. Bajado el buffer de entrada a 8KB y habilitado
     `MBEDTLS_DYNAMIC_BUFFER` (aloca solo cuando hace falta, libera después).
   - **Stack insuficiente en la tarea de connectivity**: con el problema de
     RAM anterior resuelto, el handshake avanzaba pero fallaba con "PK
     verify failed" (síntoma clásico de stack corto en mbedTLS: la
     verificación de firma RSA/ECDSA corrompe la pila en silencio en vez de
     crashear limpio). Subido el stack de la tarea de 6KB a 12KB.
   - **La causa de fondo real, encontrada con logging temporal en DEBUG +
     `heap_caps_get_largest_free_block`**: no era falta de RAM interna total
     (quedaban ~27KB libres) sino **fragmentación** — el bloque contiguo más
     grande disponible era de solo 12KB, y el handshake real necesita ir
     pidiendo varios bloques que se fragmentan entre sí compitiendo con
     NimBLE+LVGL+WiFi en la misma RAM interna. Arreglado de raíz moviendo
     las allocaciones de mbedTLS a PSRAM (`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`,
     4MB libres sin competencia) en vez de seguir ajustando tamaños de
     buffer a ciegas.
   - **Bug real #4, encontrado verificando el sync con `curl` (24 ago)**:
     dos viajes distintos aparecían con la misma fecha en el backend —
     `connectivity` reconstruía la hora sumando el offset de hora real
     **del arranque en que sincroniza**, no del arranque en que el viaje
     pasó (solo correcto si ambos coinciden). Arreglado con
     `trip_record_t.recorded_at_epoch_s` (hora real guardada por `storage`
     al cerrar cada viaje, vía `state_store_set/get_wall_clock_offset()`)
     — ver sección dedicada más arriba. Efecto secundario: cambió el
     tamaño de `trip_record_t`, así que se perdió el historial local viejo
     (ya estaba sincronizado en el backend, dato real a salvo).
10. Rotación automática 180° por acelerómetro — **se probó y se sacó (20
   ago)**. El Core2 trae un MPU6886 (acelerómetro+giroscopio, confirmado en
   la fuente de M5Stack) en el mismo bus I2C que el AXP192/táctil, y se
   calibró en hardware real: sostenido normal el eje Y da `~+0.99g`, dado
   vuelta 180° da `~-1.01g` — separación clara, sin ambigüedad. El problema
   fue que `esp_lcd_panel_mirror()` cambiaba el registro MADCTL del panel
   (confirmado por log que la detección de orientación funcionaba) pero la
   pantalla no rotaba visualmente. No se investigó por qué — quedó afuera
   para no seguir iterando ese día. Si se retoma: probablemente haga falta
   forzar un redibujado completo (`lv_obj_invalidate` de toda la pantalla)
   después de cambiar el mirror, en vez de asumir que el panel reinterpreta
   la GRAM ya escrita on-the-fly.
