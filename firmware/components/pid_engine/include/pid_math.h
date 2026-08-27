#pragma once
#include <stdint.h>
#include <stddef.h>

/**
 * pid_math — funciones puras de parseo/conversión de PIDs OBD-II (SAE J1979).
 *
 * Deliberadamente sin ninguna dependencia de ESP-IDF (nada de esp_err.h,
 * FreeRTOS, logging). Esto permite testear las fórmulas con gcc normal en
 * cualquier máquina, sin instalar ESP-IDF ni tener el hardware conectado.
 * Ver firmware/host_tests/test_pid_math.c.
 */

/**
 * Convierte una respuesta ASCII de ELM327 (ej. "41 0C 1A F8") a bytes crudos.
 * Ignora espacios y corta en '>' (prompt final). Devuelve la cantidad de
 * bytes escritos en out, o -1 si el texto tiene un byte incompleto o inválido.
 */
int pid_math_ascii_hex_to_bytes(const char *ascii, uint8_t *out, size_t out_max);

/**
 * Quita los prefijos de numero de linea que ELM327 agrega en respuestas
 * multi-frame (modo 09, VIN incluido) por default: "0:", "1:", etc. — un
 * digito hex seguido de ':', sin espacio. Sin esto, pid_math_ascii_hex_to_bytes
 * falla al toparse con el ':' (no es digito hex ni espacio). Modifica ascii
 * en el lugar (nunca crece, solo se puede achicar).
 */
void pid_math_strip_frame_prefixes(char *ascii);

/** PID 0x0C — RPM = ((A*256)+B)/4 */
uint16_t pid_math_rpm(uint8_t a, uint8_t b);

/** PID 0x0D — Velocidad en km/h = A */
uint8_t pid_math_speed_kmh(uint8_t a);

/** PID 0x05 / 0x0F — Temperatura en °C = A - 40 (coolant e intake air usan la misma fórmula) */
int16_t pid_math_temp_c(uint8_t a);

/** PID 0x04 — Carga del motor en % = A * 100 / 255 */
uint8_t pid_math_engine_load_pct(uint8_t a);

/** PID 0x0B / 0x33 — Presión en kPa = A (MAP y barométrica usan la misma fórmula) */
uint8_t pid_math_map_kpa(uint8_t a);

/** PID 0x11 — Posición del acelerador en % = A * 100 / 255 */
uint8_t pid_math_throttle_pct(uint8_t a);

/** PID 0x23 — Presión de riel de combustible (diesel common-rail) en kPa = 10 * ((A*256)+B) */
uint32_t pid_math_fuel_rail_pressure_kpa(uint8_t a, uint8_t b);

/** PID 0x5E — Caudal de combustible en L/h = ((A*256)+B) / 20 */
float pid_math_fuel_rate_lph(uint8_t a, uint8_t b);

/** PID 0x01 — Estado de monitores: bit 7 de A = check engine (MIL) encendido */
int pid_math_mil_on(uint8_t a);

/**
 * Parsea la respuesta de texto de "ATRV" (ej. "12.6V") a voltios.
 * Devuelve 1 si pudo parsear (escribe en *out_volts), 0 si el texto es inválido.
 */
int pid_math_parse_atrv(const char *text, float *out_volts);

/**
 * Decodifica un codigo DTC (modo 03/07) de sus 2 bytes crudos al formato
 * estandar de 5 caracteres (ej. "P0301"). Formato SAE J2012:
 *   byte alto, bits 7:6 -> sistema (00=P poderplante, 01=C chasis,
 *                                    10=B carroceria, 11=U red)
 *   byte alto, bits 5:4 -> primer digito (0-3)
 *   byte alto, bits 3:0 -> segundo digito (0-F, hex)
 *   byte bajo           -> tercer y cuarto digito (cada nibble, hex)
 * out debe tener al menos 6 bytes (5 caracteres + '\0'). Devuelve 1 si el
 * par no es "0000" (que es relleno de "sin codigo", no un DTC real), 0 si
 * era relleno y no escribio nada en out.
 */
int pid_math_decode_dtc(uint8_t hi, uint8_t lo, char out[6]);

/**
 * Parsea una respuesta completa de modo 03 (bytes ya convertidos, ver
 * pid_math_ascii_hex_to_bytes) a una lista de codigos DTC de texto. Salta el
 * byte de header (0x43) si esta presente, y los pares de relleno "0000".
 * Devuelve la cantidad de codigos escritos en out_codes (hasta max_codes).
 */
int pid_math_parse_dtc_list(const uint8_t *bytes, int n, char out_codes[][6], int max_codes);

/**
 * Parsea la respuesta de modo 09 PID 02 (VIN) ya convertida a bytes (ver
 * pid_math_ascii_hex_to_bytes, despues de pid_math_strip_frame_prefixes).
 * Formato SAE J1979: header "49 02 01" (modo+0x40, PID, cantidad de items)
 * seguido de 17 bytes ASCII con el VIN -- se salta el header si esta
 * presente. out_vin debe tener al menos 18 bytes (17 + '\0'). Devuelve 1 si
 * se pudo juntar el VIN completo (17 caracteres), 0 si la respuesta vino
 * incompleta (adaptador/vehiculo no lo soporta, o se corto en el medio).
 */
int pid_math_parse_vin(const uint8_t *bytes, int n, char out_vin[18]);
