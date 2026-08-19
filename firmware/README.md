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
  buscando "vLinker" sin panics. **Lo que falta:**
  1. Confirmar los UUIDs BLE reales del Vgate vLinker MC+ con la app nRF Connect
     (instrucciones en `components/obd_driver/include/obd_driver_config.h`) —
     los que están en el código son placeholders, todavía no confirmados
     contra el adaptador real.
  2. Probar la conexión completa (GAP connect + discovery de servicio/característica
     + intercambio de comandos AT) en el Maxus T60 con los UUIDs ya confirmados.
- ❌ `ui` — sigue en stub. Es el siguiente paso lógico una vez que `obd_driver`
  esté conectando de verdad (para ver los datos en pantalla, no solo por log).
- ❌ `connectivity` / `storage` (contenido real) — quedan para Fase 2.

## Próximo paso concreto

1. ~~Instalar ESP-IDF y correr `idf.py build`~~ — hecho el 19 ago (target
   `esp32`, Core2). Compila y bootea sin panics.
2. Confirmar los UUIDs BLE reales del Vgate con nRF Connect (ver
   `obd_driver_config.h`) — pendiente, es lo que sigue. Enviar `ATZ` como
   texto a la característica write candidata y confirmar que responde algo
   tipo `ELM327 v...` en la característica notify antes de darla por buena.
3. Cargar esos UUIDs confirmados en `obd_driver_config.h`, recompilar y
   flashear al Core2 con `idf.py -p COMx flash monitor` mientras estás en el
   Maxus T60 (llave en contacto), para ver si conecta y qué PIDs responde de
   verdad (algunos, como boost real, probablemente no estén en el estándar —
   ver `docs/pid-mapping.md`, por crear con los resultados de esa prueba).
