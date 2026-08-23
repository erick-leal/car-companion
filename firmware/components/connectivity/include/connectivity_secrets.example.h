#pragma once

/**
 * Plantilla de connectivity_secrets.h — copiar este archivo a
 * "connectivity_secrets.h" (mismo directorio) y completar con los datos
 * reales. Ese archivo NO se sube a git (ver .gitignore) porque tiene tu
 * contraseña real del backend.
 *
 * Por que la contraseña va en el firmware y no un token: el backend hoy
 * solo tiene JWT de usuario (30 dias, via /auth/login), no un token de
 * dispositivo separado — ver docs/api-contract.md. El firmware hace login
 * el mismo antes de cada sync para sacar un token fresco, asi no hay que
 * manejar expiracion a mano. Riesgo real y aceptado para este proyecto
 * personal: alguien con acceso fisico al M5 (y las herramientas para leer
 * su flash) podria extraer esta contraseña. Si eso te preocupa, la
 * alternativa es agregar un endpoint de token de dispositivo al backend
 * (mas trabajo, mas seguro) — ver conversacion del 23 ago.
 *
 *   cp connectivity_secrets.example.h connectivity_secrets.h
 */

#define WIFI_SSID     "tu-red-wifi"
#define WIFI_PASSWORD "tu-clave-wifi"

#define BACKEND_BASE_URL "https://car-companion-production.up.railway.app"
#define BACKEND_EMAIL     "tu-email@ejemplo.com"
#define BACKEND_PASSWORD  "tu-contrasena-del-backend"
