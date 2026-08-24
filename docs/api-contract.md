# Contrato de API — firmware ↔ backend ↔ app móvil

> Borrador inicial. Versionar cada cambio acá antes de tocar código en `firmware/connectivity`
> o `backend/src/routes`. El objetivo es que ningún cambio de formato sea "accidental".

## Convenciones

- Base URL: `/api/v1`
- Auth: Bearer token (por definir: JWT emitido en `/auth/login`)
- Formato: JSON

## Endpoints (estado: no implementados, solo contrato)

### `POST /api/v1/auth/register`
Registro de usuario dueño del dispositivo.

### `POST /api/v1/auth/login`
Devuelve token de sesión.

### `GET /api/v1/devices`
Lista los dispositivos emparejados del usuario autenticado.

### `DELETE /api/v1/devices/:id`
Desemparejar un dispositivo (ej. se vendió el auto, se cambió de adaptador).

### `POST /api/v1/sync/trips`
El firmware sube el historial de viajes acumulado offline.

```jsonc
{
  "device_id": "string",
  "trips": [
    {
      "started_at": "ISO8601",
      "ended_at": "ISO8601",
      "distance_km": 0.0,
      "avg_consumption": 0.0, // L/100km (definido 24 ago) — omitido/null si el vehículo no expone flujo de combustible por OBD
      "max_rpm": 0,
      "dtc_codes": ["P0299"]
    }
  ]
}
```

### `GET /api/v1/firmware/latest`
El firmware consulta si hay una versión nueva antes de aplicar OTA.

```jsonc
{
  "version": "0.2.0",
  "url": "https://.../firmware-0.2.0.bin",
  "sha256": "..."
}
```

### `GET /api/v1/sync/trips/:id/dtc`
Códigos de falla (DTC) de un viaje puntual — para la pantalla de diagnóstico.

## Pendiente de definir

- Modelo de PIDs propietarios por vehículo (para cuando se soporte más de un modelo).
- Rate limiting de sync.
- Formato exacto de autenticación del dispositivo (token de larga duración vs. por sesión).
  **Estado real (23 ago):** el firmware (`connectivity`) usa por ahora login de
  usuario (email/contraseña hardcodeados en `connectivity_secrets.h`, no
  versionado) contra `/auth/login` para sacar un JWT fresco antes de cada
  sync. Es un atajo consciente para v1 — la alternativa prolija (token de
  dispositivo de larga duración, sin exponer la contraseña real del
  usuario) queda pendiente.
- ~~Unidad de `avg_consumption` sin definir~~ — **definida el 24 ago:
  L/100km** (litros cada 100km — el estándar usado en Chile para consumo de
  combustible, más intuitivo que km/L para un vehículo diesel: menor número
  = más eficiente). El backend no necesitó ningún cambio (`POST/GET
  /sync/trips` ya insertaban/devolvían la columna `avg_consumption
  NUMERIC(6,2)` sin asumir una unidad) — solo faltaba que el firmware la
  calculara y mandara. `connectivity.c` la computa por viaje como
  `fuel_used_l / distance_km * 100`, y la omite (no manda el campo) si
  `distance_km == 0` o si `fuel_used_l == 0` — este último caso es el mismo
  criterio "0 = sin dato" que ya usa el resto del codebase para PIDs no
  soportados: si el vehículo no expone el PID de caudal de combustible
  (0x5E), `fuel_used_l` queda en 0.0 durante todo el viaje, así que un 0.0
  ahí no es un consumo real, es "nunca se pudo medir".
- `trip_record_t` en el firmware no guarda los códigos DTC de cada viaje
  individual (solo un `bool check_engine_seen`), así que `dtc_codes` en el
  payload de sync también se omite por ahora — habría que decidir si vale
  la pena guardar los códigos reales por viaje en `storage` para esto.
