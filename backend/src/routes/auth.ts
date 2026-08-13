import { Router } from "express";

export const authRouter = Router();

// TODO: registro/login de usuarios (dueños del dispositivo) + emparejamiento
// device_id <-> user_id para que el dispositivo pueda subir datos autenticado.
authRouter.post("/register", (_req, res) => {
  res.status(501).json({ error: "not implemented" });
});

authRouter.post("/login", (_req, res) => {
  res.status(501).json({ error: "not implemented" });
});
