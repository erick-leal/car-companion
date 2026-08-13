# Roadmap — Car Companion

- [ ] **Fase 0 — Setup y aprendizaje base**
  - [ ] Fundamentos de C/C++ embebido y ESP-IDF
  - [x] Adaptador OBD comprado (Vgate vLinker MC+)
  - [ ] M5Stack CoreS3 comprado
  - [ ] Cable OBD-II de extensión para banco de pruebas

- [ ] **Fase 1 — Prototipo funcional**
  - [ ] `obd_driver`: conexión BLE con el Vgate, comandos AT básicos
  - [ ] `pid_engine`: leer PIDs estándar (RPM, velocidad, temp motor, voltaje)
  - [ ] `ui`: pantalla principal básica en LVGL
  - [ ] Probar en el Maxus T60 real, mapear PIDs propietarios (ver `pid-mapping.md`, por crear)

- [ ] **Fase 2 — MVP completo**
  - [ ] Todas las pantallas del MVP (viaje, DTC, diagnóstico, historial)
  - [ ] Migrar a Sunton CYD
  - [ ] Backend mínimo (auth + sync)
  - [ ] OTA básico funcionando

- [ ] **Fase 3 — Validación con usuarios reales**
  - [ ] 5–10 unidades a beta testers
  - [ ] Definir PCB v1

- [ ] **Fase 4 — Primeras 100 unidades**
  - [ ] Fabricar PCB propia + carcasa
  - [ ] App móvil (React Native) v1
  - [ ] Pricing y canal de venta

- [ ] **Fase 5 — Producción y lanzamiento comercial**
  - [ ] Ajustar BOM para volumen
  - [ ] Soporte, garantía, logística
  - [ ] Roadmap de funciones futuras según demanda real

## Estado actual

**Fase 0, en progreso.** Repositorio inicializado, estructura del monorepo lista,
firmware con esqueleto de componentes (todo en TODO). Siguiente paso: comprar/activar
el M5Stack CoreS3 y hacer el primer `obd_driver_init()` real contra el Vgate.
