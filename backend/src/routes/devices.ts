import { Router } from "express";
import { z } from "zod";
import { pool } from "../db/pool.js";
import { requireAuth, type AuthedRequest } from "../middleware/auth.js";

export const devicesRouter = Router();
devicesRouter.use(requireAuth);

const registerDeviceSchema = z.object({
  device_uid: z.string().min(4), // ej. la MAC del ESP32
  name: z.string().min(1).optional(),
});

/** Emparejar un dispositivo físico con el usuario autenticado. */
devicesRouter.post("/", async (req: AuthedRequest, res) => {
  const parsed = registerDeviceSchema.safeParse(req.body);
  if (!parsed.success) {
    return res.status(400).json({ error: parsed.error.flatten() });
  }
  const { device_uid, name } = parsed.data;

  const existing = await pool.query("SELECT id, user_id FROM devices WHERE device_uid = $1", [device_uid]);
  if (existing.rowCount && existing.rowCount > 0) {
    if (existing.rows[0].user_id !== req.userId) {
      return res.status(409).json({ error: "ese dispositivo ya está emparejado con otra cuenta" });
    }
    return res.json({ id: existing.rows[0].id, device_uid, already_registered: true });
  }

  const result = await pool.query(
    `INSERT INTO devices (user_id, device_uid, name)
     VALUES ($1, $2, COALESCE($3, 'Mi vehículo'))
     RETURNING id, device_uid, name, created_at`,
    [req.userId, device_uid, name ?? null]
  );
  return res.status(201).json(result.rows[0]);
});

/** Listar los dispositivos del usuario autenticado. */
devicesRouter.get("/", async (req: AuthedRequest, res) => {
  const result = await pool.query(
    "SELECT id, device_uid, name, firmware_version, last_seen_at FROM devices WHERE user_id = $1 ORDER BY created_at",
    [req.userId]
  );
  return res.json(result.rows);
});
