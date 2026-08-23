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
      "avg_consumption": 0.0, // null si el vehículo no expone flujo de combustible por OBD
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
- **Unidad de `avg_consumption` sin definir** (23 ago): el campo existe en
  `syncPayloadSchema` pero no dice si es L/100km, km/L, o algo distinto —
  el firmware por ahora lo omite del payload de sync (es opcional) en vez
  de inventar una unidad. Definir esto antes de que algo (app móvil,
  dashboard) empiece a mostrarlo.
- `trip_record_t` en el firmware no guarda los códigos DTC de cada viaje
  individual (solo un `bool check_engine_seen`), así que `dtc_codes` en el
  payload de sync también se omite por ahora — habría que decidir si vale
  la pena guardar los códigos reales por viaje en `storage` para esto.
