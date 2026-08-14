import { Router } from "express";
import { z } from "zod";
import { pool } from "../db/pool.js";
import { requireAuth, type AuthedRequest } from "../middleware/auth.js";

export const syncRouter = Router();
syncRouter.use(requireAuth);

// Contrato esperado del firmware, ver docs/api-contract.md.
const tripSchema = z.object({
  started_at: z.string().datetime(),
  ended_at: z.string().datetime(),
  distance_km: z.number().nonnegative().optional(),
  avg_consumption: z.number().nonnegative().nullable().optional(), // null si el vehículo no expone flujo de combustible
  max_rpm: z.number().int().nonnegative().optional(),
  dtc_codes: z.array(z.string()).optional(),
});

const syncPayloadSchema = z.object({
  device_uid: z.string().min(4),
  trips: z.array(tripSchema),
});

syncRouter.post("/trips", async (req: AuthedRequest, res) => {
  const parsed = syncPayloadSchema.safeParse(req.body);
  if (!parsed.success) {
    return res.status(400).json({ error: parsed.error.flatten() });
  }
  const { device_uid, trips } = parsed.data;

  const deviceResult = await pool.query(
    "SELECT id FROM devices WHERE device_uid = $1 AND user_id = $2",
    [device_uid, req.userId]
  );
  if (deviceResult.rowCount === 0) {
    return res.status(404).json({ error: "dispositivo no encontrado o no pertenece a este usuario" });
  }
  const deviceId = deviceResult.rows[0].id;

  const client = await pool.connect();
  const insertedIds: number[] = [];
  try {
    await client.query("BEGIN");

    for (const trip of trips) {
      const tripResult = await client.query(
        `INSERT INTO trips (device_id, started_at, ended_at, distance_km, avg_consumption, max_rpm)
         VALUES ($1, $2, $3, $4, $5, $6)
         RETURNING id`,
        [deviceId, trip.started_at, trip.ended_at, trip.distance_km ?? null,
         trip.avg_consumption ?? null, trip.max_rpm ?? null]
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

/** Historial de viajes del usuario (para la futura app móvil / dashboard). */
syncRouter.get("/trips", async (req: AuthedRequest, res) => {
  const result = await pool.query(
    `SELECT t.id, t.started_at, t.ended_at, t.distance_km, t.avg_consumption, t.max_rpm,
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
