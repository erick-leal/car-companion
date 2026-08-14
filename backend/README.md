# Backend — Car Companion

API REST (Node.js + TypeScript + Express + PostgreSQL).

## Setup

```bash
cd backend
npm install
cp .env.example .env   # y completar DATABASE_URL (ver notas en ese archivo)
npm run dev
```

## Producción

Desplegado en Railway: https://car-companion-production.up.railway.app
(root directory del servicio configurado como `backend/`, variables
`DATABASE_URL` y `JWT_SECRET` seteadas ahí — ver `.env.example` para el detalle).

## Tests

```bash
npm run test
```

Corre los tests de los esquemas de validación (`src/validation.ts`) sin
necesitar base de datos ni servidor levantado — son lógica pura, igual que
`pid_math` en el firmware. Corren automáticamente en cada push (ver
`.github/workflows/backend-tests.yml`).

## Estructura

```
src/
├── index.ts          # entry point, monta los routers
├── validation.ts      # esquemas zod (compartidos por rutas y tests, sin dependencias externas)
├── routes/            # un archivo por recurso (auth, devices, sync, firmware)
├── middleware/         # auth (JWT)
├── __tests__/          # tests de validation.ts (npm run test)
└── db/                 # conexión, queries y migraciones de PostgreSQL
```

Ver [`docs/api-contract.md`](../docs/api-contract.md) para el contrato esperado
entre este backend y el firmware (`connectivity` component).
