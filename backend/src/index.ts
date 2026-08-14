import "dotenv/config";
import express from "express";
import { authRouter } from "./routes/auth.js";
import { devicesRouter } from "./routes/devices.js";
import { syncRouter } from "./routes/sync.js";
import { firmwareRouter } from "./routes/firmware.js";

const app = express();
app.use(express.json());

// Contrato de API versionado — ver docs/api-contract.md antes de romper algo acá.
app.use("/api/v1/auth", authRouter);
app.use("/api/v1/devices", devicesRouter);
app.use("/api/v1/sync", syncRouter);
app.use("/api/v1/firmware", firmwareRouter);

app.get("/health", (_req, res) => res.json({ ok: true }));

// Manejador de errores centralizado — cualquier throw en una ruta (ej. una
// query de pg que falla) cae acá en vez de tumbar el proceso.
app.use((err: unknown, _req: express.Request, res: express.Response, _next: express.NextFunction) => {
  console.error(err);
  res.status(500).json({ error: "error interno del servidor" });
});

const port = process.env.PORT ?? 3000;
app.listen(port, () => {
  console.log(`car-companion-backend escuchando en :${port}`);
});
