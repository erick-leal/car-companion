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
  para siempre — ahora se recupera solo a los 3s). **Sigue sin compilar ni
  probar contra hardware real todavía.** Antes de flashear, hay que:
  1. Confirmar los UUIDs BLE reales del Vgate vLinker MC+ con la app nRF Connect
     (instrucciones en `components/obd_driver/include/obd_driver_config.h`).
  2. Compilar con `idf.py build` (requiere tener ESP-IDF instalado, ver setup arriba)
     y corregir lo que el compilador marque — es código escrito a mano contra la
     API de NimBLE, sin haber pasado por el toolchain real todavía.
- ❌ `ui` — sigue en stub. Es el siguiente paso lógico una vez que `obd_driver`
  esté conectando de verdad (para ver los datos en pantalla, no solo por log).
- ❌ `connectivity` / `storage` (contenido real) — quedan para Fase 2.

## Próximo paso concreto

1. Instalar ESP-IDF y correr `idf.py build` — vas a encontrar errores de
   compilación reales en `obd_driver.c` (es la parte más compleja y menos
   probada de todo el firmware). Es normal y esperado; son buenos primeros
   errores para aprender ESP-IDF/NimBLE.
2. En paralelo, sin esperar a resolver BLE: correr los tests de `pid_math`
   (`cd host_tests && gcc ...`) para confirmar que las fórmulas de conversión
   están bien — eso ya está verificado y no cambia aunque BLE tarde en andar.
3. Confirmar los UUIDs del Vgate con nRF Connect (ver `obd_driver_config.h`).
4. Flashear al CoreS3 y mirar `idf.py monitor` mientras conduces el Maxus T60,
   para ver qué PIDs responde de verdad (algunos, como boost real, probablemente
   no estén en el estándar — ver `docs/pid-mapping.md`, por crear con los
   resultados de esa prueba).
