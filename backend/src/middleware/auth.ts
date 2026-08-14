import type { Request, Response, NextFunction } from "express";
import jwt from "jsonwebtoken";

/**
 * Middleware de autenticación por JWT. Usado en cualquier ruta que requiera
 * saber "quién" está pidiendo (o qué dispositivo, si extendemos esto a
 * tokens de dispositivo más adelante — ver TODO en routes/sync.ts).
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
