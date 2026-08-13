#pragma once
#include "esp_err.h"

/**
 * ui — pantallas LVGL. Consume state_store exclusivamente vía suscripción.
 * NUNCA debe incluir obd_driver.h — si una pantalla necesita un dato nuevo,
 * ese dato se agrega primero a vehicle_state_t en state_store.h.
 */

esp_err_t ui_init(void);

/** Pantallas del MVP — cada una vive en su propio .c dentro de screens/ (por crear). */
void ui_show_main_screen(void);
void ui_show_trip_stats_screen(void);
void ui_show_dtc_screen(void);
void ui_show_diagnostics_screen(void);
void ui_show_maintenance_screen(void);
