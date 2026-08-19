# Roadmap — Car Companion

- [ ] **Fase 0 — Setup y aprendizaje base**
  - [ ] Fundamentos de C/C++ embebido y ESP-IDF
  - [x] Adaptador OBD comprado (Vgate vLinker MC+)
  - [x] M5Stack comprado — llegó un **Core2** (ESP32 clásico) en vez del CoreS3
    (ESP32-S3) planeado; se decidió seguir con el Core2 para no bloquear
    Fase 1, ver "Estado actual" abajo
  - [ ] Cable OBD-II de extensión para banco de pruebas

- [ ] **Fase 1 — Prototipo funcional**
  - [ ] `obd_driver`: conexión BLE con el Vgate, comandos AT básicos
  - [ ] `pid_engine`: leer PIDs estándar (RPM, velocidad, temp motor, voltaje)
  - [ ] `ui`: pantalla principal básica en LVGL
  - [ ] Probar en el Maxus T60 real, mapear PIDs propietarios (ver `pid-mapping.md`, por crear)

- [x] **Fase 2 — MVP completo (backend)**
  - [ ] Todas las pantallas del MVP (viaje, DTC, diagnóstico, historial) — pendiente, es firmware
  - [ ] Migrar a Sunton CYD — pendiente, depende del hardware
  - [x] Backend mínimo (auth + sync) — desplegado en Railway, probado end-to-end
  - [ ] OTA básico funcionando — el endpoint existe (`/firmware/latest`), falta lógica en el firmware

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

**Backend en producción, firmware pendiente de hardware.** El backend
(Node/TS/Postgres) está desplegado en Railway y probado de punta a punta:
login, emparejamiento de dispositivo, sync de viajes con DTC, y consulta de
historial — todo contra la base de datos real, no un mock. URL:
https://car-companion-production.up.railway.app

El firmware tiene `state_store` y `pid_engine` implementados y testeados
(sin hardware). `obd_driver` (BLE/NimBLE) ya compila y corre en hardware
real (Core2, target `esp32`): bootea sin panics, inicializa BLE y escanea
buscando el Vgate por nombre (probado el 19 ago). Falta confirmar los UUIDs
reales de servicio/característica del vLinker MC+ con nRF Connect (los del
código son placeholders) y probar la conexión completa en el Maxus T60.

Hay una maqueta visual (no funcional) de la pantalla principal en
`docs/mockups/main-screen-mockup.html`, como referencia de diseño para
cuando se escriba la UI real en LVGL.
