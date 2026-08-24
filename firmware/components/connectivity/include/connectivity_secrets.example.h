#pragma once

/**
 * Plantilla de connectivity_secrets.h — copiar este archivo a
 * "connectivity_secrets.h" (mismo directorio) y completar con los datos
 * reales. Ese archivo NO se sube a git (ver .gitignore) porque tiene tu
 * WiFi y el token de tu dispositivo.
 *
 * DEVICE_TOKEN (agregado 24 ago, reemplaza a BACKEND_EMAIL/BACKEND_PASSWORD):
 * un token propio de ESTE dispositivo, no tu contraseña real de la cuenta.
 * El firmware ya no hace login — solo usa este token como Bearer directo
 * para sincronizar. Se genera una sola vez desde tu propia máquina
 * (autenticada con tu login real), nunca desde el firmware:
 *
 *   1. Conseguir un JWT de usuario:
 *      curl -s -X POST https://car-companion-production.up.railway.app/api/v1/auth/login \
 *        -H "Content-Type: application/json" \
 *        -d '{"email":"tu-email","password":"tu-contraseña"}'
 *      (copiar el valor de "token" de la respuesta)
 *
 *   2. Pedir/regenerar el token de este dispositivo con ese JWT (reemplazar
 *      TU-JWT y DEVICE_UID — la MAC del ESP32 en hex, la ves en el log de
 *      boot como "device_uid=..."):
 *      curl -s -X POST https://car-companion-production.up.railway.app/api/v1/devices \
 *        -H "Authorization: Bearer TU-JWT" \
 *        -H "Content-Type: application/json" \
 *        -d '{"device_uid":"DEVICE_UID"}'
 *      (copiar el valor de "device_token" de la respuesta — solo se muestra
 *      esta vez; si el dispositivo ya estaba registrado sin token, este
 *      mismo llamado se lo genera. Si ya tiene uno y lo perdiste, usar
 *      POST /api/v1/devices/:id/token en su lugar para regenerarlo)
 *
 *   3. Pegar ese valor acá abajo en DEVICE_TOKEN.
 *
 * Riesgo real y aceptado para este proyecto personal: alguien con acceso
 * físico al M5 (y las herramientas para leer su flash) podría extraer este
 * token — pero a diferencia de la contraseña real, este token se puede
 * revocar/regenerar sin afectar la cuenta (POST /devices/:id/token), y
 * nunca fue tu contraseña real en primer lugar.
 *
 *   cp connectivity_secrets.example.h connectivity_secrets.h
 */

#define WIFI_SSID     "tu-red-wifi"
#define WIFI_PASSWORD "tu-clave-wifi"

#define BACKEND_BASE_URL "https://car-companion-production.up.railway.app"
#define DEVICE_TOKEN      "tu-token-de-dispositivo"
