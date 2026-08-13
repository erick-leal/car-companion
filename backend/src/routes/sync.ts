import { Router } from "express";

export const syncRouter = Router();

// Recibe el historial de viajes pendiente que el firmware acumuló offline
// (ver firmware/components/storage). Ver docs/api-contract.md para el schema.
syncRouter.post("/trips", (_req, res) => {
  res.status(501).json({ error: "not implemented" });
});
