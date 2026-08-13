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

## Pendiente de definir

- Modelo de PIDs propietarios por vehículo (para cuando se soporte más de un modelo).
- Rate limiting de sync.
- Formato exacto de autenticación del dispositivo (token de larga duración vs. por sesión).
