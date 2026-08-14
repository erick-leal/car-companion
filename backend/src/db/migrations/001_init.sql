-- 001_init.sql — esquema inicial de Car Companion
-- Aplicar con: psql $DATABASE_URL -f src/db/migrations/001_init.sql

CREATE TABLE IF NOT EXISTS users (
    id            SERIAL PRIMARY KEY,
    email         TEXT NOT NULL UNIQUE,
    password_hash TEXT NOT NULL,
    created_at    TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- Un "device" es un dispositivo físico (una pantalla Car Companion). Un usuario
-- puede tener más de uno (ej. uno por auto). device_uid es el identificador
-- único de hardware (ej. la MAC del ESP32) que el firmware manda al emparejar.
CREATE TABLE IF NOT EXISTS devices (
    id                SERIAL PRIMARY KEY,
    user_id           INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    device_uid        TEXT NOT NULL UNIQUE,
    name              TEXT NOT NULL DEFAULT 'Mi vehículo',
    firmware_version  TEXT,
    created_at        TIMESTAMPTZ NOT NULL DEFAULT now(),
    last_seen_at      TIMESTAMPTZ
);

CREATE TABLE IF NOT EXISTS trips (
    id               SERIAL PRIMARY KEY,
    device_id        INTEGER NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
    started_at       TIMESTAMPTZ NOT NULL,
    ended_at         TIMESTAMPTZ NOT NULL,
    distance_km      NUMERIC(8,2),
    avg_consumption  NUMERIC(6,2), -- NULL si el vehículo no expone flujo de combustible por OBD
    max_rpm          INTEGER,
    created_at       TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS dtc_codes (
    id          SERIAL PRIMARY KEY,
    trip_id     INTEGER NOT NULL REFERENCES trips(id) ON DELETE CASCADE,
    code        TEXT NOT NULL,   -- ej. "P0299"
    description TEXT,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- El firmware consulta esta tabla (GET /api/v1/firmware/latest) para saber
-- si hay una versión nueva antes de aplicar OTA.
CREATE TABLE IF NOT EXISTS firmware_releases (
    id           SERIAL PRIMARY KEY,
    version      TEXT NOT NULL UNIQUE,
    url          TEXT NOT NULL,
    sha256       TEXT NOT NULL,
    notes        TEXT,
    released_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS idx_trips_device_id ON trips(device_id);
CREATE INDEX IF NOT EXISTS idx_dtc_codes_trip_id ON dtc_codes(trip_id);
CREATE INDEX IF NOT EXISTS idx_devices_user_id ON devices(user_id);
