import { Router } from "express";
import { pool } from "../db/pool.js";

export const firmwareRouter = Router();

/**
 * El firmware (connectivity_check_ota) consulta esto para saber si hay una
 * versión más nueva antes de descargar el binario. Sin auth: el dispositivo
 * todavía no tiene un token propio en v1 (ver TODO en docs/api-contract.md
 * sobre autenticación de dispositivo vs. de usuario).
 */
firmwareRouter.get("/latest", async (_req, res) => {
  const result = await pool.query(
    "SELECT version, url, sha256, notes FROM firmware_releases ORDER BY released_at DESC LIMIT 1"
  );
  if (result.rowCount === 0) {
    return res.status(404).json({ error: "no hay ninguna versión de firmware publicada todavía" });
  }
  return res.json(result.rows[0]);
});
