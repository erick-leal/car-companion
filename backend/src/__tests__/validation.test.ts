import { test } from "node:test";
import assert from "node:assert/strict";
import { credentialsSchema, registerDeviceSchema, tripSchema, syncPayloadSchema } from "../validation.js";

test("credentialsSchema acepta email y password válidos", () => {
  const result = credentialsSchema.safeParse({ email: "a@b.com", password: "password123" });
  assert.equal(result.success, true);
});

test("credentialsSchema rechaza email inválido", () => {
  const result = credentialsSchema.safeParse({ email: "no-es-un-email", password: "password123" });
  assert.equal(result.success, false);
});

test("credentialsSchema rechaza password corta (menos de 8 caracteres)", () => {
  const result = credentialsSchema.safeParse({ email: "a@b.com", password: "1234567" });
  assert.equal(result.success, false);
});

test("registerDeviceSchema acepta device_uid solo (name es opcional)", () => {
  const result = registerDeviceSchema.safeParse({ device_uid: "AA:BB:CC:11:22:33" });
  assert.equal(result.success, true);
});

test("registerDeviceSchema rechaza device_uid muy corto", () => {
  const result = registerDeviceSchema.safeParse({ device_uid: "AB" });
  assert.equal(result.success, false);
});

test("tripSchema acepta un viaje completo con DTCs", () => {
  const result = tripSchema.safeParse({
    started_at: "2026-08-14T10:00:00Z",
    ended_at: "2026-08-14T10:45:00Z",
    distance_km: 32.5,
    max_rpm: 3200,
    dtc_codes: ["P0299"],
  });
  assert.equal(result.success, true);
});

test("tripSchema acepta avg_consumption null (vehículo sin flujo de combustible por OBD)", () => {
  const result = tripSchema.safeParse({
    started_at: "2026-08-14T10:00:00Z",
    ended_at: "2026-08-14T10:45:00Z",
    avg_consumption: null,
  });
  assert.equal(result.success, true);
});

test("tripSchema rechaza started_at con formato de fecha inválido", () => {
  const result = tripSchema.safeParse({
    started_at: "14 de agosto",
    ended_at: "2026-08-14T10:45:00Z",
  });
  assert.equal(result.success, false);
});

test("tripSchema rechaza distance_km negativo", () => {
  const result = tripSchema.safeParse({
    started_at: "2026-08-14T10:00:00Z",
    ended_at: "2026-08-14T10:45:00Z",
    distance_km: -5,
  });
  assert.equal(result.success, false);
});

test("syncPayloadSchema acepta un payload con múltiples viajes", () => {
  const result = syncPayloadSchema.safeParse({
    device_uid: "AA:BB:CC:11:22:33",
    trips: [
      { started_at: "2026-08-14T10:00:00Z", ended_at: "2026-08-14T10:45:00Z" },
      { started_at: "2026-08-14T11:00:00Z", ended_at: "2026-08-14T11:20:00Z" },
    ],
  });
  assert.equal(result.success, true);
  assert.equal(result.data?.trips.length, 2);
});

test("syncPayloadSchema acepta trips vacío (sync sin nada pendiente)", () => {
  const result = syncPayloadSchema.safeParse({ device_uid: "AA:BB:CC:11:22:33", trips: [] });
  assert.equal(result.success, true);
});
