# Arquitectura — Car Companion

## Visión general

```
┌─────────────────────┐      BLE/Bluetooth       ┌──────────────────────────┐
│  Adaptador OBD-II    │ ───────────────────────▶ │  Dispositivo (ESP32-S3)  │
│  (Vgate vLinker MC+) │                           │  Firmware C/C++ + LVGL  │
└─────────────────────┘                           └──────────┬───────────────┘
                                                              │ WiFi (OTA, sync)
                                                              ▼
                                              ┌───────────────────────────┐
                                              │  Backend propio (Node.js) │
                                              │  API REST + Auth + OTA    │
                                              │  PostgreSQL               │
                                              └──────────┬────────────────┘
                                                          │
                                                          ▼
                                              ┌───────────────────────────┐
                                              │  App móvil (React Native) │
                                              └───────────────────────────┘
```

## Capas del firmware

```
obd_driver → pid_engine → state_store → ui
                              ↑
                       connectivity / storage
```

- **obd_driver**: habla BLE con el Vgate. No interpreta datos.
- **pid_engine**: traduce respuestas crudas a valores con significado (RPM, boost, etc.).
  Separa PIDs estándar (SAE J1979) de PIDs propietarios por vehículo.
- **state_store**: único punto de verdad del estado del vehículo en memoria (pub/sub).
- **ui**: pantallas LVGL, solo lee de `state_store`, nunca de `obd_driver` directo.
- **connectivity**: WiFi provisioning, OTA, sync con backend. No bloqueante.
- **storage**: persistencia local (NVS/SD) del historial mientras no hay conexión.

**Regla de oro**: ninguna capa se salta otra. Esto es lo que permite, más adelante,
cambiar de adaptador OBD, soportar otro vehículo, o cambiar de pantalla sin
reescribir todo el firmware.

## Errores comunes a evitar (recordatorio)

- No empezar por el PCB propio antes de validar en dev board.
- No prometer GPS + app + backend + plugins en v1.
- No mezclar lógica de UI con lógica de parsing de PIDs.
- Diseñar pensando en el problema térmico desde el día 1 (carcasa cerrada + sol).

Ver el análisis completo de producto en el documento de análisis inicial del proyecto.
