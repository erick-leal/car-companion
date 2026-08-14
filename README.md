# Car Companion

Dispositivo inteligente para vehículos que lee datos en tiempo real vía OBD-II (Bluetooth)
y los muestra en una pantalla dedicada, con app móvil y backend propio.

> Estado: **prototipo temprano**. Vehículo de desarrollo: Maxus T60 2021 Diesel.
> Adaptador OBD de referencia: Vgate vLinker MC+ (BLE).
> Placa de prototipo: M5Stack CoreS3 (ESP32-S3).

Ver el análisis completo de producto y arquitectura en [`docs/architecture.md`](docs/architecture.md)
y la hoja de ruta en [`docs/roadmap.md`](docs/roadmap.md).

## Estructura del monorepo

```
car-companion/
├── firmware/       # Firmware ESP32-S3 (ESP-IDF + LVGL) — el dispositivo en sí
├── backend/        # API REST (Node.js + TypeScript + PostgreSQL)
├── mobile-app/     # App móvil (React Native)
├── hardware/       # Esquemáticos, BOM, notas de diseño de PCB/carcasa
└── docs/           # Arquitectura, roadmap, decisiones de diseño (ADRs)
```

## Por qué esta separación

Cada carpeta es un proyecto independiente con su propio ciclo de vida y build.
El contrato entre `firmware/` y `backend/` (formato de sync, autenticación, OTA)
se define como un esquema versionado en `docs/api-contract.md` — cualquier cambio
ahí es un cambio deliberado, no un acoplamiento accidental de código.

## Empezar

Cada subcarpeta tiene su propio `README.md` con instrucciones de setup:

- [`firmware/README.md`](firmware/README.md) — requiere ESP-IDF v5.x
- [`backend/README.md`](backend/README.md) — requiere Node.js 20+
- [`mobile-app/README.md`](mobile-app/README.md) — requiere React Native / Expo
- [`hardware/README.md`](hardware/README.md) — BOM y notas de diseño

## Licencia

Por definir. Mientras tanto, todo el código es propietario / no licenciado públicamente.
