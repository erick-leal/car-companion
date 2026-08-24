import { Router } from "express";
import { pool } from "../db/pool.js";
import { requireAuth, generateDeviceToken, hashDeviceToken, type AuthedRequest } from "../middleware/auth.js";
import { registerDeviceSchema } from "../validation.js";

export const devicesRouter = Router();
devicesRouter.use(requireAuth);

/**
 * Emparejar un dispositivo físico con el usuario autenticado.
 *
 * Devuelve `device_token` en texto plano SOLO en la respuesta de esta
 * llamada (o de POST /:id/token) — el backend nunca vuelve a poder mostrar
 * el valor real, solo guarda su hash (ver 002_device_tokens.sql). Ese
 * token es lo que va en `connectivity_secrets.h` del firmware, no la
 * contraseña real de la cuenta.
 */
devicesRouter.post("/", async (req: AuthedRequest, res) => {
  const parsed = registerDeviceSchema.safeParse(req.body);
  if (!parsed.success) {
    return res.status(400).json({ error: parsed.error.flatten() });
  }
  const { device_uid, name } = parsed.data;

  const existing = await pool.query(
    "SELECT id, user_id, token_hash FROM devices WHERE device_uid = $1",
    [device_uid]
  );
  if (existing.rowCount && existing.rowCount > 0) {
    if (existing.rows[0].user_id !== req.userId) {
      return res.status(409).json({ error: "ese dispositivo ya está emparejado con otra cuenta" });
    }
    if (existing.rows[0].token_hash) {
      // Ya tiene token de un registro anterior — no se puede volver a mostrar
      // (solo se guarda el hash). Usar POST /:id/token si se perdió.
      return res.json({ id: existing.rows[0].id, device_uid, already_registered: true });
    }
    // Dispositivo registrado antes de que existiera token_hash (migración
    // 002) — generar uno ahora, es la única vez que se puede mostrar.
    const token = generateDeviceToken();
    await pool.query("UPDATE devices SET token_hash = $1 WHERE id = $2", [
      hashDeviceToken(token),
      existing.rows[0].id,
    ]);
    return res.json({ id: existing.rows[0].id, device_uid, already_registered: true, device_token: token });
  }

  const token = generateDeviceToken();
  const result = await pool.query(
    `INSERT INTO devices (user_id, device_uid, name, token_hash)
     VALUES ($1, $2, COALESCE($3, 'Mi vehículo'), $4)
     RETURNING id, device_uid, name, created_at`,
    [req.userId, device_uid, name ?? null, hashDeviceToken(token)]
  );
  return res.status(201).json({ ...result.rows[0], device_token: token });
});

/**
 * Regenerar el token de un dispositivo (se perdió el original, o rotación
 * de seguridad de rutina). Invalida cualquier token anterior de inmediato.
 */
devicesRouter.post("/:id/token", async (req: AuthedRequest, res) => {
  const token = generateDeviceToken();
  const result = await pool.query(
    "UPDATE devices SET token_hash = $1 WHERE id = $2 AND user_id = $3 RETURNING id, device_uid",
    [hashDeviceToken(token), req.params.id, req.userId]
  );
  if (result.rowCount === 0) {
    return res.status(404).json({ error: "dispositivo no encontrado" });
  }
  return res.json({ id: result.rows[0].id, device_uid: result.rows[0].device_uid, device_token: token });
});

/** Listar los dispositivos del usuario autenticado. */
devicesRouter.get("/", async (req: AuthedRequest, res) => {
  const result = await pool.query(
    "SELECT id, device_uid, name, firmware_version, last_seen_at FROM devices WHERE user_id = $1 ORDER BY created_at",
    [req.userId]
  );
  return res.json(result.rows);
});

/** Desemparejar un dispositivo (ej. si vendés el auto o cambiás de adaptador). */
devicesRouter.delete("/:id", async (req: AuthedRequest, res) => {
  const result = await pool.query(
    "DELETE FROM devices WHERE id = $1 AND user_id = $2 RETURNING id",
    [req.params.id, req.userId]
  );
  if (result.rowCount === 0) {
    return res.status(404).json({ error: "dispositivo no encontrado" });
  }
  return res.status(204).send();
});
