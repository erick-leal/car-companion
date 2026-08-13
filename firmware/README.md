# Firmware — Car Companion

Firmware para ESP32-S3 usando **ESP-IDF** (no Arduino IDE) + **LVGL** para la interfaz.
Placa de prototipo: M5Stack CoreS3.

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

# 3. Configurar el target
cd firmware
idf.py set-target esp32s3

# 4. Compilar
idf.py build

# 5. Flashear (con el CoreS3 conectado por USB-C)
idf.py -p /dev/ttyUSB0 flash monitor
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

## Próximo paso concreto

1. Implementar en `obd_driver` el emparejamiento BLE con el Vgate vLinker MC+ y el envío
   de comandos AT básicos (`ATZ`, `ATE0`, `0100`) para confirmar el protocolo detectado.
2. Loguear por UART las respuestas crudas mientras conduces el Maxus T60, para empezar
   a mapear qué PIDs estándar responde y cuáles no (ver `docs/pid-mapping.md`).
