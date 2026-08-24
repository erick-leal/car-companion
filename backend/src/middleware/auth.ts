import type { Request, Response, NextFunction } from "express";
import jwt from "jsonwebtoken";
import crypto from "node:crypto";
import { pool } from "../db/pool.js";

/**
 * Dos formas de autenticación en este backend, para dos clientes distintos:
 *
 * - `requireAuth` (JWT de usuario): para un humano operando desde su propia
 *   máquina — login, listar/borrar dispositivos, ver el historial de viajes.
 * - `requireDeviceAuth` (token de dispositivo, ver 002_device_tokens.sql):
 *   para el firmware, que sube viajes sin intervención humana. Nunca conoce
 *   el email/contraseña real del usuario — solo un token propio del
 *   dispositivo, de larga duración, generado una vez desde una máquina
 *   humana autenticada (ver POST /devices y POST /devices/:id/token en
 *   routes/devices.ts) y copiado a connectivity_secrets.h.
 */

const JWT_SECRET = process.env.JWT_SECRET;
if (!JWT_SECRET) {
  // Fallamos rápido y explícito en vez de arrancar con un secreto vacío/inseguro.
  throw new Error("JWT_SECRET no está definido (ver backend/.env.example)");
}

export interface AuthedRequest extends Request {
  userId?: number;
}

export function requireAuth(req: AuthedRequest, res: Response, next: NextFunction) {
  const header = req.headers.authorization;
  if (!header?.startsWith("Bearer ")) {
    return res.status(401).json({ error: "falta el header Authorization: Bearer <token>" });
  }

  const token = header.slice("Bearer ".length);
  try {
    const payload = jwt.verify(token, JWT_SECRET as string) as { userId: number };
    req.userId = payload.userId;
    next();
  } catch {
    return res.status(401).json({ error: "token inválido o expirado" });
  }
}

export function signToken(userId: number): string {
  return jwt.sign({ userId }, JWT_SECRET as string, { expiresIn: "30d" });
}

/** 256 bits de entropía en hex (64 caracteres) — token de dispositivo. */
export function generateDeviceToken(): string {
  return crypto.randomBytes(32).toString("hex");
}

/** sha256, no bcrypt: un token aleatorio de alta entropía no necesita el
 * costo computacional de bcrypt (eso es para compensar contraseñas de baja
 * entropía elegidas por humanos). */
export function hashDeviceToken(token: string): string {
  return crypto.createHash("sha256").update(token).digest("hex");
}

export interface AuthedDeviceRequest extends Request {
  deviceId?: number;
}

export async function requireDeviceAuth(req: AuthedDeviceRequest, res: Response, next: NextFunction) {
  const header = req.headers.authorization;
  if (!header?.startsWith("Bearer ")) {
    return res.status(401).json({ error: "falta el header Authorization: Bearer <token>" });
  }

  const token = header.slice("Bearer ".length);
  const result = await pool.query("SELECT id FROM devices WHERE token_hash = $1", [hashDeviceToken(token)]);
  if (result.rowCount === 0) {
    return res.status(401).json({ error: "token de dispositivo inválido" });
  }
  req.deviceId = result.rows[0].id;
  next();
}
