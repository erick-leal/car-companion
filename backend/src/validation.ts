import { z } from "zod";

/**
 * Todos los esquemas de validación en un solo lugar, separados de las rutas
 * que los usan. Igual que hicimos con pid_math en el firmware: esto es
 * lógica pura (sin Express, sin base de datos) que se puede testear directo,
 * sin levantar un servidor ni una conexión a Postgres — ver
 * src/__tests__/validation.test.ts.
 */

export const credentialsSchema = z.object({
  email: z.string().email(),
  password: z.string().min(8, "la contraseña debe tener al menos 8 caracteres"),
});

export const registerDeviceSchema = z.object({
  device_uid: z.string().min(4), // ej. la MAC del ESP32
  name: z.string().min(1).optional(),
});

export const tripSchema = z.object({
  started_at: z.string().datetime(),
  ended_at: z.string().datetime(),
  distance_km: z.number().nonnegative().optional(),
  avg_consumption: z.number().nonnegative().nullable().optional(), // null si el vehículo no expone flujo de combustible
  max_rpm: z.number().int().nonnegative().optional(),
  dtc_codes: z.array(z.string()).optional(),
  // VIN del vehículo conectado al momento de este viaje (25 ago) — permite
  // distinguir de qué auto son los datos si el mismo dispositivo se usa en
  // más de un vehículo. Se omite si el auto no respondió modo 09 (adaptador
  // o vehículo viejo que no lo soporta) — nunca inventado.
  vehicle_vin: z.string().length(17).optional(),
});

export const syncPayloadSchema = z.object({
  device_uid: z.string().min(4),
  trips: z.array(tripSchema),
});
