import { Pool } from "pg";

// TODO: mover a variables de entorno (.env, ver .env.example) antes de cualquier commit real.
export const pool = new Pool({
  connectionString: process.env.DATABASE_URL,
});
