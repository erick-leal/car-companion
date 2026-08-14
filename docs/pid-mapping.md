# Mapeo de PIDs — Maxus T60 2021 Diesel

> Este documento se llena con datos reales del vehículo, no se puede completar
> desde el escritorio. Ir agregando filas a medida que se prueba en el auto
> con `idf.py monitor` (ver firmware/README.md, Fase 1).

## PIDs estándar (SAE J1979) — confirmados en el vehículo

| PID | Nombre | Soportado | Notas |
|---|---|---|---|
| 0x0C | RPM | ⏳ por probar | |
| 0x0D | Velocidad | ⏳ por probar | |
| 0x05 | Temp. refrigerante | ⏳ por probar | |
| 0x04 | Carga motor | ⏳ por probar | |
| 0x0F | Temp. aire admisión | ⏳ por probar | |
| 0x0B | MAP (presión múltiple) | ⏳ por probar | Ver nota en `pid_engine.c`: no es boost directo |

## PIDs propietarios — por descubrir

Objetivo: encontrar cómo el Maxus/SAIC expone presión de turbo real (boost) y
temperatura de gases de escape (EGT), si es que los expone por OBD-II en
absoluto. Método sugerido:

1. Conseguir (si es posible) el manual de taller o service manual del T60 —
   a veces incluye la lista de PIDs propietarios para diagnóstico.
2. Si no hay manual: usar un CAN sniffer (ej. módulo MCP2515 + otro ESP32, o
   herramienta dedicada) conectado al bus CAN del OBD-II, y comparar tramas
   mientras se acelera con el turbo trabajando, buscando un valor que suba/baje
   de forma correlacionada con la sensación de boost real.
3. Documentar acá cada PID propietario encontrado: código (modo + PID),
   fórmula de conversión, y bajo qué condiciones se confirmó.

## Plantilla para PIDs propietarios confirmados

```
PID propietario: 22XXXX (modo 22, típico de PIDs extendidos UDS)
Nombre: <por definir>
Fórmula: <por definir>
Confirmado el: <fecha>
Condiciones de prueba: <ej. "motor caliente, ralentí vs. 3000 RPM">
```
