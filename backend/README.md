# Backend — Car Companion

API REST (Node.js + TypeScript + Express + PostgreSQL).

## Setup

```bash
cd backend
npm install
cp .env.example .env   # y completar DATABASE_URL
npm run dev
```

## Estructura

```
src/
├── index.ts        # entry point, monta los routers
├── routes/         # un archivo por recurso (auth, sync, firmware, ...)
├── services/        # lógica de negocio (vacío todavía)
└── db/              # conexión y queries a PostgreSQL
```

Ver [`docs/api-contract.md`](../docs/api-contract.md) para el contrato esperado
entre este backend y el firmware (`connectivity` component).
