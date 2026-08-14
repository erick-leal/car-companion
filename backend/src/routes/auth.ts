import { Router } from "express";
import bcrypt from "bcryptjs";
import { z } from "zod";
import { pool } from "../db/pool.js";
import { signToken } from "../middleware/auth.js";

export const authRouter = Router();

const credentialsSchema = z.object({
  email: z.string().email(),
  password: z.string().min(8, "la contraseña debe tener al menos 8 caracteres"),
});

authRouter.post("/register", async (req, res) => {
  const parsed = credentialsSchema.safeParse(req.body);
  if (!parsed.success) {
    return res.status(400).json({ error: parsed.error.flatten() });
  }
  const { email, password } = parsed.data;

  const existing = await pool.query("SELECT id FROM users WHERE email = $1", [email]);
  if (existing.rowCount && existing.rowCount > 0) {
    return res.status(409).json({ error: "ya existe una cuenta con ese email" });
  }

  const passwordHash = await bcrypt.hash(password, 10);
  const result = await pool.query(
    "INSERT INTO users (email, password_hash) VALUES ($1, $2) RETURNING id",
    [email, passwordHash]
  );
  const userId = result.rows[0].id as number;

  return res.status(201).json({ token: signToken(userId) });
});

authRouter.post("/login", async (req, res) => {
  const parsed = credentialsSchema.safeParse(req.body);
  if (!parsed.success) {
    return res.status(400).json({ error: parsed.error.flatten() });
  }
  const { email, password } = parsed.data;

  const result = await pool.query(
    "SELECT id, password_hash FROM users WHERE email = $1",
    [email]
  );
  if (result.rowCount === 0) {
    return res.status(401).json({ error: "credenciales inválidas" });
  }

  const { id, password_hash } = result.rows[0];
  const valid = await bcrypt.compare(password, password_hash);
  if (!valid) {
    return res.status(401).json({ error: "credenciales inválidas" });
  }

  return res.json({ token: signToken(id) });
});
