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

  **Limitación real, no resuelta:** sin `connectivity` (WiFi/NTP) no hay
  hora de pared confiable — `start_time_s` de cada viaje es segundos desde
  el arranque del ESP32, no una fecha real. Se resuelve cuando
  `connectivity` sincronice con el backend (que sí sabe la hora real de
  cuándo llegó cada sync).

  Confirmado en hardware real: la partición monta, el filesystem se
  formatea solo en el primer arranque, no rompe nada del resto del sistema.
  **No confirmado todavía:** el ciclo completo de un viaje real (arrancar,
  manejar, apagar, ver el registro guardado) — falta probarlo la próxima
  vez que se maneje el auto con esto flasheado.
- ❌ `connectivity` — sigue sin implementar (WiFi, sync con el backend).

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
   - ~~**Fallas (DTC)**~~ — hecho el 22 ago: lee códigos con modo 03 vía el
     patrón "buzón" en `state_store` (ver arriba). Falta borrarlos (modo 04) —
     no implementado todavía, no hay botón para eso. **Falta probar leyendo
     un código real** (hasta ahora solo se vieron los estados "sin fallas" y
     "sin conexión OBD").
   - ~~**Diagnóstico**~~ — hecho el 22 ago: PIDs crudos que no tienen tarjeta
     en el dashboard (carga motor, temp admisión/ambiente, presión
     barométrica, presión de riel) + estado de conexión y hace cuánto llegó
     el último dato. Solo lectura, se actualiza sola vía `state_store`.
   - **Mantenimiento** — service, filtros, aceite. Puede reusar `storage`.
     Pendiente: necesita decisiones de producto primero (¿qué intervalos?
     ¿cómo se resetea un contador de service?).
8. Probar el dashboard **manejando** (hasta ahora solo se validó detenido:
   velocidad siempre 0 y boost sin carga de turbo real).
9. PIDs propietarios del Maxus (boost real de turbo, EGT) — no están en el
   bitmask estándar, requieren ingeniería inversa. Ver `docs/pid-mapping.md`.
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
