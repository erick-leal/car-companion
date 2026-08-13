import { Router } from "express";

export const firmwareRouter = Router();

// El firmware consulta esto (connectivity_check_ota) para saber si hay
// una versión nueva antes de descargar el binario.
firmwareRouter.get("/latest", (_req, res) => {
  res.status(501).json({ error: "not implemented" });
});
