import { Router } from "express";
import { pool } from "../db/pool.js";
import { requireAuth, requireDeviceAuth, type AuthedRequest, type AuthedDeviceRequest } from "../middleware/auth.js";
import { syncPayloadSchema } from "../validation.js";

export const syncRouter = Router();

/**
 * El firmware sube su historial de viajes autenticado con su propio token
 * de dispositivo (requireDeviceAuth, ver 002_device_tokens.sql) — no con el
 * login del usuario. `device_uid` sigue en el payload por compatibilidad y
 * como dato informativo, pero ya no se usa para resolver a qué dispositivo
 * pertenece: eso lo determina el token en sí.
 */
syncRouter.post("/trips", requireDeviceAuth, async (req: AuthedDeviceRequest, res) => {
  const parsed = syncPayloadSchema.safeParse(req.body);
  if (!parsed.success) {
    return res.status(400).json({ error: parsed.error.flatten() });
  }
  const { trips } = parsed.data;
  const deviceId = req.deviceId as number;

  const client = await pool.connect();
  const insertedIds: number[] = [];
  try {
    await client.query("BEGIN");

    for (const trip of trips) {
      const tripResult = await client.query(
        `INSERT INTO trips (device_id, started_at, ended_at, distance_km, avg_consumption, max_rpm, vehicle_vin)
         VALUES ($1, $2, $3, $4, $5, $6, $7)
         RETURNING id`,
        [deviceId, trip.started_at, trip.ended_at, trip.distance_km ?? null,
         trip.avg_consumption ?? null, trip.max_rpm ?? null, trip.vehicle_vin ?? null]
      );
      const tripId = tripResult.rows[0].id;
      insertedIds.push(tripId);

      for (const code of trip.dtc_codes ?? []) {
        await client.query(
          "INSERT INTO dtc_codes (trip_id, code) VALUES ($1, $2)",
          [tripId, code]
        );
      }
    }

    await client.query(
      "UPDATE devices SET last_seen_at = now() WHERE id = $1",
      [deviceId]
    );

    await client.query("COMMIT");
  } catch (err) {
    await client.query("ROLLBACK");
    throw err;
  } finally {
    client.release();
  }

  return res.status(201).json({ synced_trip_ids: insertedIds });
});

/** Historial de viajes del usuario (para la futura app móvil / dashboard) — login de usuario, no token de dispositivo. */
syncRouter.get("/trips", requireAuth, async (req: AuthedRequest, res) => {
  const result = await pool.query(
    `SELECT t.id, t.started_at, t.ended_at, t.distance_km, t.avg_consumption, t.max_rpm, t.vehicle_vin,
            d.name AS device_name
     FROM trips t
     JOIN devices d ON d.id = t.device_id
     WHERE d.user_id = $1
     ORDER BY t.started_at DESC
     LIMIT 100`,
    [req.userId]
  );
  return res.json(result.rows);
});

/** Códigos DTC de un viaje puntual (para la pantalla de diagnóstico) — login de usuario. */
syncRouter.get("/trips/:id/dtc", requireAuth, async (req: AuthedRequest, res) => {
  const result = await pool.query(
    `SELECT dc.code, dc.description, dc.created_at
     FROM dtc_codes dc
     JOIN trips t ON t.id = dc.trip_id
     JOIN devices d ON d.id = t.device_id
     WHERE t.id = $1 AND d.user_id = $2`,
    [req.params.id, req.userId]
  );
  return res.json(result.rows);
});
