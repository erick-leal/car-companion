import "dotenv/config";
import express from "express";
import { authRouter } from "./routes/auth.js";
import { syncRouter } from "./routes/sync.js";
import { firmwareRouter } from "./routes/firmware.js";

const app = express();
app.use(express.json());

// Contrato de API versionado — ver docs/api-contract.md antes de romper algo acá.
app.use("/api/v1/auth", authRouter);
app.use("/api/v1/sync", syncRouter);
app.use("/api/v1/firmware", firmwareRouter);

app.get("/health", (_req, res) => res.json({ ok: true }));

const port = process.env.PORT ?? 3000;
app.listen(port, () => {
  console.log(`car-companion-backend escuchando en :${port}`);
});
