#pragma once

/**
 * obd_driver_config.h — valores específicos del adaptador BLE que hay que
 * CONFIRMAR con el Vgate vLinker MC+ real antes de que obd_driver conecte.
 *
 * No encontré los UUIDs de servicio/característica del vLinker MC+ documentados
 * públicamente (los clones ELM327 no siempre los publican) — no los voy a
 * inventar. Cómo descubrirlos en 10 minutos:
 *
 *   1. Instala "nRF Connect" (Android/iOS) — app gratuita de Nordic Semiconductor.
 *   2. Enchufa el vLinker MC+ al OBD-II del auto (o a un banco de pruebas 12V).
 *   3. En nRF Connect, escanea BLE y conéctate al dispositivo (aparece como
 *      "vLinker MC" o similar).
 *   4. En la lista de servicios, vas a ver típicamente un servicio "custom"
 *      (no un UUID estándar de Bluetooth SIG) con 2 características: una con
 *      propiedad WRITE (o WRITE NO RESPONSE) y otra con propiedad NOTIFY.
 *      Esa es la pareja "escribir comando / recibir respuesta" tipo UART.
 *   5. Copiá esos 3 UUIDs (servicio, característica write, característica notify)
 *      acá abajo, reemplazando los placeholders.
 *
 * Muchos adaptadores ELM327-BLE clonados usan el patrón de servicio "FFF0" con
 * características "FFF1" (notify) / "FFF2" (write), o el Nordic UART Service
 * (6E400001-...). Dejo esos dos casos comentados como punto de partida más
 * probable, pero NO asumas que son correctos sin confirmarlos con nRF Connect.
 */

#define OBD_BLE_DEVICE_NAME "vLinker"   // nombre que aparece al escanear (confirmar substring exacto)

/* --- Opción A: patrón FFF0/FFF1/FFF2 (común en clones baratos) --- */
// #define OBD_BLE_SERVICE_UUID        0xFFF0
// #define OBD_BLE_NOTIFY_CHAR_UUID    0xFFF1
// #define OBD_BLE_WRITE_CHAR_UUID     0xFFF2

/* --- Opción B: Nordic UART Service (común en módulos basados en nRF) --- */
// #define OBD_BLE_SERVICE_UUID_128     "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
// #define OBD_BLE_NOTIFY_CHAR_UUID_128 "6E400003-B5A3-F393-E0A9-E50E24DCCA9E" // TX del periférico = notify para nosotros
// #define OBD_BLE_WRITE_CHAR_UUID_128  "6E400002-B5A3-F393-E0A9-E50E24DCCA9E" // RX del periférico = write para nosotros

/* --- PLACEHOLDER ACTIVO: reemplazar con lo que confirmes en nRF Connect --- */
#define OBD_BLE_SERVICE_UUID        0xFFF0
#define OBD_BLE_NOTIFY_CHAR_UUID    0xFFF1
#define OBD_BLE_WRITE_CHAR_UUID     0xFFF2
