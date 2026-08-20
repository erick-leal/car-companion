#pragma once

/**
 * obd_driver_config.h — UUIDs BLE del Vgate vLinker MC+, CONFIRMADOS el 19 ago
 * 2026 con nRF Connect contra el adaptador real (dispositivo "vLinker MC-IOS").
 *
 * Verificación: se activaron notificaciones en 2AF0 (CCCD 0x0100), se escribió
 * ATZ + CR en hex (41545A0D) en 2AF1, y 2AF0 devolvió:
 *   0D0D 454C 4D33 3237 2076 322E 320D 0D3E  →  "\r\rELM327 v2.2\r\r>"
 * que es el banner estándar de boot del ELM327. Servicio 18F0 confirmado como
 * el canal real (el otro candidato relevado, servicio 128-bit
 * E7810A71-73AE-499D-8C15-FAA9AEF0C3F2 con característica única
 * BEF8D6C9-9C21-4C9E-B632-BD58C1009F9F, no se probó — no hizo falta una vez
 * que este candidato respondió).
 */

#define OBD_BLE_DEVICE_NAME "vLinker"   // aparece como "vLinker MC-IOS" al escanear

#define OBD_BLE_SERVICE_UUID        0x18F0
#define OBD_BLE_NOTIFY_CHAR_UUID    0x2AF0
#define OBD_BLE_WRITE_CHAR_UUID     0x2AF1
