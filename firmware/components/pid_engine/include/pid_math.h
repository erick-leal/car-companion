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
