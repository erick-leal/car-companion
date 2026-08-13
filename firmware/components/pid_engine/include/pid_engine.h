#pragma once
#include "esp_err.h"

/**
 * pid_engine — traduce respuestas OBD crudas (obd_driver) en valores con significado
 * (RPM, velocidad, temperatura, etc.) y los escribe en state_store.
 *
 * Los PIDs estándar (SAE J1979) van en pid_table_standard.c (por implementar).
 * Los PIDs propietarios del Maxus T60 van en pid_table_maxus.c (por descubrir/reversear,
 * ver docs/pid-mapping.md) — mantenerlos separados para poder agregar otros vehículos
 * sin tocar el motor genérico.
 */

esp_err_t pid_engine_init(void);

/** Dispara la lectura periódica de todos los PIDs activos según la pantalla actual. */
esp_err_t pid_engine_start_polling(void);
