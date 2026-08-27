-- vehicle_vin (25 ago): permite distinguir de qué auto es cada viaje si el
-- mismo dispositivo (M5 + adaptador OBD) se usa en más de un vehículo.
-- El firmware lo lee del auto (modo 09 PID 02) y lo manda con cada viaje
-- en POST /sync/trips -- ver connectivity.c y docs/api-contract.md.
-- Nullable: adaptadores/vehículos que no soportan modo 09 simplemente no
-- lo mandan, no es un dato inventado.
ALTER TABLE trips ADD COLUMN IF NOT EXISTS vehicle_vin TEXT;
CREATE INDEX IF NOT EXISTS idx_trips_vehicle_vin ON trips(vehicle_vin);
