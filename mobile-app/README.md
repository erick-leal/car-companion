# Mobile App — Car Companion

App móvil (React Native) — consume el mismo backend que el dispositivo.

> No iniciar todavía. Según la hoja de ruta (`docs/roadmap.md`), esta carpeta se
> activa en la Fase 4 (primeras 100 unidades), después de validar firmware + backend
> con usuarios reales. Empezarla antes es la forma más común de perder foco en
> este tipo de proyecto (ver "Errores comunes" en docs/architecture.md).

## Setup (cuando corresponda)

```bash
npx create-expo-app@latest . --template blank-typescript
```

Reutiliza el contrato de API definido en `docs/api-contract.md` — mismo backend,
mismo modelo de datos que usa el sync del firmware.
