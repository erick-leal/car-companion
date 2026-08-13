# Hardware — Car Companion

Esquemáticos, BOM y notas de diseño de PCB/carcasa.

> No hay PCB propia todavía. Fase actual: prototipo sobre M5Stack CoreS3 (`firmware/`).
> Esta carpeta se activa en la Fase 3–4 de la hoja de ruta, cuando se congele el
> set de sensores/PIDs soportado y se pase a diseño de PCB propia.

## Estructura futura

```
hardware/
├── schematics/     # KiCad — esquemático y layout de PCB
├── bom/             # Bill of materials por versión (v1.0, v1.1...)
└── enclosure/        # Modelos 3D de la carcasa (STL/STEP)
```

## Referencia de prototipo actual

- Placa: M5Stack CoreS3 (ESP32-S3, pantalla táctil 2", USB-C)
- Adaptador OBD: Vgate vLinker MC+ (BLE)
- Vehículo de prueba: Maxus T60 2021 Diesel
