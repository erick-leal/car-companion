#include "ui.h"
#include "core2_power.h"
#include "core2_display.h"
#include "core2_touch.h"
#include "state_store.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lvgl.h"
#include <stdio.h>

static const char *TAG = "ui";

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_panel_io;
static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t s_disp_drv;

/* LVGL no es thread-safe: todo llamado a su API (incluido lv_timer_handler,
 * que corre en la tarea de tick) tiene que estar serializado con este mutex.
 * state_store notifica desde la tarea de pid_engine, no la de LVGL, asi que
 * el callback de abajo (on_state_change) tambien lo toma antes de tocar
 * cualquier lv_obj_t. */
static SemaphoreHandle_t s_lvgl_mutex;

static bool lvgl_lock(uint32_t timeout_ms)
{
    return xSemaphoreTake(s_lvgl_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

static void lvgl_unlock(void)
{
    xSemaphoreGive(s_lvgl_mutex);
}

/* --- Pantalla principal: dashboard con arco de RPM + tarjetas de valores ---
 *
 * Layout pensado para los 320x240 del Core2:
 *   - barra superior (22px): titulo + estado de conexion + testigo CHECK
 *   - izquierda: arco de RPM (dato "heroe", el que se mira de reojo manejando)
 *   - derecha: tres tarjetas apiladas (velocidad, refrigerante, bateria)
 *   - abajo: tres tarjetas (boost, acelerador, consumo)
 */

/* Paleta: fondo oscuro para no encandilar de noche en el auto. */
#define COL_BG        lv_color_hex(0x0D1117)
#define COL_CARD      lv_color_hex(0x161B22)
#define COL_CAPTION   lv_color_hex(0x8B949E)
#define COL_VALUE     lv_color_hex(0xE6EDF3)
#define COL_ACCENT    lv_color_hex(0x58A6FF)
#define COL_OK        lv_color_hex(0x3FB950)
#define COL_WARN      lv_color_hex(0xD29922)
#define COL_DANGER    lv_color_hex(0xF85149)

/* Calibrado para el diesel del Maxus T60, que corta bastante antes que un
 * motor a gasolina. Si se prueba en un auto a gasolina (ej. el MG3), subir
 * a ~7000 o el arco se llena antes de tiempo. */
#define RPM_MAX 5000

static lv_obj_t *s_arc_rpm;
static lv_obj_t *s_lbl_rpm_val;
static lv_obj_t *s_lbl_speed_val;
static lv_obj_t *s_lbl_coolant_val;
static lv_obj_t *s_lbl_batt_val;
static lv_obj_t *s_lbl_boost_val;
static lv_obj_t *s_lbl_thr_val;
static lv_obj_t *s_lbl_fuel_val;
static lv_obj_t *s_lbl_status;
static lv_obj_t *s_lbl_cel;

/* true solo mientras el dashboard esta en pantalla. Al cambiar de pantalla
 * lv_obj_clean() destruye esos labels y los punteros de arriba quedan
 * colgando — on_state_change (que corre desde la tarea de pid_engine, no
 * desde la de LVGL) tiene que saber que no debe tocarlos. */
static bool s_dashboard_active;
static bool s_subscribed;

/* Las funciones build_* asumen que el lock de LVGL YA esta tomado: se llaman
 * tanto desde los wrappers publicos (que lo toman) como desde callbacks de
 * eventos de botones, que corren dentro de lv_timer_handler y por lo tanto ya
 * estan bajo el lock. Tomarlo de nuevo ahi colgaria todo, porque el mutex de
 * FreeRTOS no es reentrante. */
static void build_main_screen(void);
static void build_menu_screen(void);

/* Crea una tarjeta con titulo chico arriba y valor grande abajo. Devuelve el
 * label del valor, que es lo unico que se actualiza despues. */
static lv_obj_t *make_card(lv_obj_t *parent, int x, int y, int w, int h,
                            const char *caption, const lv_font_t *value_font)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_bg_color(card, COL_CARD, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 6, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cap = lv_label_create(card);
    lv_obj_set_style_text_font(cap, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(cap, COL_CAPTION, 0);
    lv_label_set_text(cap, caption);
    lv_obj_align(cap, LV_ALIGN_TOP_LEFT, 7, 3);

    lv_obj_t *val = lv_label_create(card);
    lv_obj_set_style_text_font(val, value_font, 0);
    lv_obj_set_style_text_color(val, COL_VALUE, 0);
    lv_label_set_text(val, "--");
    lv_obj_align(val, LV_ALIGN_BOTTOM_LEFT, 7, -3);
    return val;
}

static void on_state_change(const vehicle_state_t *state, void *ctx)
{
    (void)ctx;
    if (!lvgl_lock(50)) {
        return; // se pierde esta actualizacion puntual antes que bloquear pid_engine
    }
    if (!s_dashboard_active) {
        /* Estamos en el menu u otra pantalla: los widgets del dashboard ya
         * no existen. Salir sin tocar nada. */
        lvgl_unlock();
        return;
    }

    char buf[32];

    bool engine_running = state->rpm > 0;

    /* RPM: arco + numero al centro. Zonas estimadas para el diesel del T60
     * (ralenti ~800, uso normal hasta ~2500-3000, forzando el motor de ahi a
     * ~3500-4000, y de ahi al corte). Si el manual del Maxus da el corte real
     * exacto, ajustar RPM_MAX y estos umbrales a eso en vez de la estimacion. */
    uint16_t rpm = state->rpm > RPM_MAX ? RPM_MAX : state->rpm;
    lv_arc_set_value(s_arc_rpm, rpm);
    snprintf(buf, sizeof(buf), "%u", state->rpm);
    lv_label_set_text(s_lbl_rpm_val, buf);
    lv_color_t rpm_col = state->rpm > 3500 ? COL_DANGER
                       : state->rpm > 2500 ? COL_WARN
                                            : COL_OK;
    lv_obj_set_style_arc_color(s_arc_rpm, rpm_col, LV_PART_INDICATOR);
    lv_obj_set_style_text_color(s_lbl_rpm_val, rpm_col, 0);

    snprintf(buf, sizeof(buf), "%u", state->speed_kmh);
    lv_label_set_text(s_lbl_speed_val, buf);

    /* Refrigerante: la mayoria de los motores opera en 90-105C. Entrando en
     * temperatura (frio, recien arrancado) tambien cuenta como verde: no es
     * una falla, es esperable. Sobre 105C es sobrecalentamiento real. Solo
     * verde/ambar/rojo, sin un cuarto color para no perder el criterio de
     * "un color = un nivel de atencion" en toda la pantalla. */
    snprintf(buf, sizeof(buf), "%d C", state->coolant_temp_c);
    lv_label_set_text(s_lbl_coolant_val, buf);
    lv_obj_set_style_text_color(s_lbl_coolant_val,
                                 state->coolant_temp_c > 105 ? COL_DANGER :
                                 state->coolant_temp_c > 95  ? COL_WARN : COL_OK, 0);

    /* Bateria: el rango "normal" depende de si el motor esta prendido o no,
     * y ese es justo el dato que mas le importa a un mecanico:
     *   - Motor apagado: bateria en reposo, 12.4-12.9V es sano, <11.8V
     *     ya es bateria floja/vieja.
     *   - Motor prendido: el alternador deberia estar cargando a 13.2-14.8V.
     *     Menos de eso con el motor andando es indicio real de que el
     *     alternador no esta cargando (correa, regulador, etc) — mas de
     *     14.8V sostenido es sobrecarga (regulador de voltaje fallando). */
    snprintf(buf, sizeof(buf), "%.1f V", state->battery_voltage);
    lv_label_set_text(s_lbl_batt_val, buf);
    lv_color_t batt_col;
    if (engine_running) {
        batt_col = (state->battery_voltage < 13.0f || state->battery_voltage > 14.8f) ? COL_DANGER
                 : (state->battery_voltage < 13.2f) ? COL_WARN
                                                     : COL_OK;
    } else {
        batt_col = (state->battery_voltage < 11.8f) ? COL_DANGER
                 : (state->battery_voltage < 12.4f) ? COL_WARN
                 : (state->battery_voltage <= 12.9f) ? COL_OK
                                                      : COL_VALUE; // >12.9V en reposo: raro, no aventuramos veredicto
    }
    lv_obj_set_style_text_color(s_lbl_batt_val, batt_col, 0);

    snprintf(buf, sizeof(buf), "%d", state->boost_pressure_kpa);
    lv_label_set_text(s_lbl_boost_val, buf);

    snprintf(buf, sizeof(buf), "%u%%", state->throttle_pct);
    lv_label_set_text(s_lbl_thr_val, buf);

    snprintf(buf, sizeof(buf), "%.1f", state->fuel_rate_lph);
    lv_label_set_text(s_lbl_fuel_val, buf);

    /* Estado de conexion: se deduce de state_store (data_valid), NO se
     * consulta a obd_driver — `ui` nunca debe depender de esa capa
     * (ver regla de dependencias en firmware/README.md). */
    lv_label_set_text(s_lbl_status, state->data_valid ? "OBD OK" : "SIN OBD");
    lv_obj_set_style_text_color(s_lbl_status, state->data_valid ? COL_OK : COL_CAPTION, 0);

    lv_label_set_text(s_lbl_cel, state->check_engine_on ? "CHECK" : "");

    lvgl_unlock();
}

static void menu_open_event_cb(lv_event_t *e)
{
    (void)e;
    build_menu_screen(); // ya estamos bajo el lock (evento de LVGL)
}

static void build_main_screen(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    s_dashboard_active = true;
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* --- Barra superior: boton de menu + estado --- */
    lv_obj_t *menu_btn = lv_btn_create(scr);
    lv_obj_set_size(menu_btn, 74, 22);
    lv_obj_set_pos(menu_btn, 6, 2);
    lv_obj_set_style_bg_color(menu_btn, COL_CARD, 0);
    lv_obj_set_style_radius(menu_btn, 5, 0);
    lv_obj_set_style_shadow_width(menu_btn, 0, 0);
    lv_obj_add_event_cb(menu_btn, menu_open_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *menu_lbl = lv_label_create(menu_btn);
    lv_obj_set_style_text_font(menu_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(menu_lbl, COL_ACCENT, 0);
    lv_label_set_text(menu_lbl, LV_SYMBOL_LIST " MENU");
    lv_obj_center(menu_lbl);

    s_lbl_cel = lv_label_create(scr);
    lv_obj_set_style_text_font(s_lbl_cel, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_cel, COL_DANGER, 0);
    lv_label_set_text(s_lbl_cel, ""); // vacio hasta que el auto reporte falla
    lv_obj_align(s_lbl_cel, LV_ALIGN_TOP_RIGHT, -74, 4);

    s_lbl_status = lv_label_create(scr);
    lv_obj_set_style_text_font(s_lbl_status, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_status, COL_CAPTION, 0);
    lv_label_set_text(s_lbl_status, "SIN OBD");
    lv_obj_align(s_lbl_status, LV_ALIGN_TOP_RIGHT, -8, 4);

    /* --- Arco de RPM (izquierda) --- */
    s_arc_rpm = lv_arc_create(scr);
    lv_obj_set_size(s_arc_rpm, 116, 116);
    lv_obj_set_pos(s_arc_rpm, 6, 26);
    lv_arc_set_rotation(s_arc_rpm, 135);
    lv_arc_set_bg_angles(s_arc_rpm, 0, 270);
    lv_arc_set_range(s_arc_rpm, 0, RPM_MAX);
    lv_arc_set_value(s_arc_rpm, 0);
    lv_obj_remove_style(s_arc_rpm, NULL, LV_PART_KNOB); // sin perilla: es indicador, no control
    lv_obj_clear_flag(s_arc_rpm, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_arc_rpm, 9, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc_rpm, 9, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc_rpm, COL_CARD, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc_rpm, COL_OK, LV_PART_INDICATOR);

    s_lbl_rpm_val = lv_label_create(scr);
    lv_obj_set_style_text_font(s_lbl_rpm_val, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_lbl_rpm_val, COL_OK, 0);
    lv_label_set_text(s_lbl_rpm_val, "0");
    lv_obj_align(s_lbl_rpm_val, LV_ALIGN_TOP_LEFT, 6 + 58 - 30, 26 + 44);
    lv_obj_set_width(s_lbl_rpm_val, 60);
    lv_obj_set_style_text_align(s_lbl_rpm_val, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *rpm_cap = lv_label_create(scr);
    lv_obj_set_style_text_font(rpm_cap, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(rpm_cap, COL_CAPTION, 0);
    lv_label_set_text(rpm_cap, "RPM");
    lv_obj_set_width(rpm_cap, 60);
    lv_obj_set_style_text_align(rpm_cap, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(rpm_cap, LV_ALIGN_TOP_LEFT, 6 + 58 - 30, 26 + 74);

    /* --- Columna derecha --- */
    const int rx = 132, rw = 182;
    s_lbl_speed_val   = make_card(scr, rx, 26,  rw, 40, "VELOCIDAD  km/h", &lv_font_montserrat_20);
    s_lbl_coolant_val = make_card(scr, rx, 70,  rw, 34, "REFRIGERANTE",    &lv_font_montserrat_20);
    s_lbl_batt_val    = make_card(scr, rx, 108, rw, 34, "BATERIA",         &lv_font_montserrat_20);

    /* --- Fila inferior --- */
    const int by = 150, bh = 46, bw = 98;
    s_lbl_boost_val = make_card(scr, 6,   by, bw, bh, "BOOST kPa",  &lv_font_montserrat_20);
    s_lbl_thr_val   = make_card(scr, 110, by, bw, bh, "ACELERADOR", &lv_font_montserrat_20);
    s_lbl_fuel_val  = make_card(scr, 214, by, bw, bh, "COMB. L/h",  &lv_font_montserrat_20);
}

/* --- Pantallas secundarias (todavia sin contenido real) --- */

static void back_to_menu_event_cb(lv_event_t *e)
{
    (void)e;
    build_menu_screen();
}

static void build_placeholder_screen(const char *titulo, const char *detalle)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    s_dashboard_active = false;
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = lv_btn_create(scr);
    lv_obj_set_size(back, 88, 24);
    lv_obj_set_pos(back, 6, 4);
    lv_obj_set_style_bg_color(back, COL_CARD, 0);
    lv_obj_set_style_radius(back, 5, 0);
    lv_obj_set_style_shadow_width(back, 0, 0);
    lv_obj_add_event_cb(back, back_to_menu_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_lbl = lv_label_create(back);
    lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(back_lbl, COL_ACCENT, 0);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT " VOLVER");
    lv_obj_center(back_lbl);

    lv_obj_t *t = lv_label_create(scr);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(t, COL_VALUE, 0);
    lv_label_set_text(t, titulo);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t *d = lv_label_create(scr);
    lv_obj_set_style_text_font(d, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(d, COL_CAPTION, 0);
    lv_label_set_text(d, detalle);
    lv_obj_align(d, LV_ALIGN_CENTER, 0, 14);
}

/* --- Menu --- */

typedef enum {
    MENU_DASHBOARD = 0,
    MENU_VIAJE,
    MENU_FALLAS,
    MENU_DIAGNOSTICO,
    MENU_MANTENIMIENTO,
} menu_item_t;

static void menu_item_event_cb(lv_event_t *e)
{
    menu_item_t item = (menu_item_t)(intptr_t)lv_event_get_user_data(e);
    switch (item) {
        case MENU_DASHBOARD:
            build_main_screen();
            break;
        case MENU_VIAJE:
            build_placeholder_screen("VIAJE", "Distancia, consumo promedio, duracion");
            break;
        case MENU_FALLAS:
            build_placeholder_screen("FALLAS", "Codigos DTC leidos del vehiculo");
            break;
        case MENU_DIAGNOSTICO:
            build_placeholder_screen("DIAGNOSTICO", "PIDs crudos y estado del adaptador");
            break;
        case MENU_MANTENIMIENTO:
            build_placeholder_screen("MANTENIMIENTO", "Service, filtros, aceite");
            break;
    }
}

static void add_menu_item(lv_obj_t *list, const char *icono, const char *texto, menu_item_t item)
{
    lv_obj_t *btn = lv_list_add_btn(list, icono, texto);
    lv_obj_set_style_bg_color(btn, COL_CARD, 0);
    lv_obj_set_style_text_color(btn, COL_VALUE, 0);
    lv_obj_set_style_text_font(btn, &lv_font_montserrat_20, 0);
    lv_obj_add_event_cb(btn, menu_item_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)item);
}

static void build_menu_screen(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    s_dashboard_active = false;
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *list = lv_list_create(scr);
    lv_obj_set_size(list, 308, 232);
    lv_obj_center(list);
    lv_obj_set_style_bg_color(list, COL_BG, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_row(list, 4, 0);

    add_menu_item(list, LV_SYMBOL_HOME,    "Tablero",       MENU_DASHBOARD);
    add_menu_item(list, LV_SYMBOL_GPS,     "Viaje",         MENU_VIAJE);
    add_menu_item(list, LV_SYMBOL_WARNING, "Fallas",        MENU_FALLAS);
    add_menu_item(list, LV_SYMBOL_SETTINGS,"Diagnostico",   MENU_DIAGNOSTICO);
    add_menu_item(list, LV_SYMBOL_EDIT,    "Mantenimiento", MENU_MANTENIMIENTO);
}

/* --- Wrappers publicos: estos SI toman el lock (se llaman desde otras
 *     tareas, como app_main), a diferencia de los build_* de arriba. --- */

void ui_show_main_screen(void)
{
    if (!lvgl_lock(1000)) {
        ESP_LOGE(TAG, "ui_show_main_screen: no se pudo tomar el lock de LVGL");
        return;
    }
    build_main_screen();
    lvgl_unlock();

    /* Una sola suscripcion en toda la vida del programa: state_store tiene un
     * maximo de suscriptores y volver al dashboard desde el menu llamaria
     * aca de nuevo. */
    if (!s_subscribed) {
        s_subscribed = true;
        state_store_subscribe(on_state_change, NULL);
    }
}

void ui_show_menu_screen(void)
{
    if (!lvgl_lock(1000)) return;
    build_menu_screen();
    lvgl_unlock();
}

static void show_placeholder_locked(const char *titulo, const char *detalle)
{
    if (!lvgl_lock(1000)) return;
    build_placeholder_screen(titulo, detalle);
    lvgl_unlock();
}

void ui_show_trip_stats_screen(void)
{
    show_placeholder_locked("VIAJE", "Distancia, consumo promedio, duracion");
}
void ui_show_dtc_screen(void)
{
    show_placeholder_locked("FALLAS", "Codigos DTC leidos del vehiculo");
}
void ui_show_diagnostics_screen(void)
{
    show_placeholder_locked("DIAGNOSTICO", "PIDs crudos y estado del adaptador");
}
void ui_show_maintenance_screen(void)
{
    show_placeholder_locked("MANTENIMIENTO", "Service, filtros, aceite");
}

/* --- Flush de LVGL hacia el panel real ---
 * esp_lcd_panel_draw_bitmap encola la transferencia SPI y vuelve enseguida:
 * NO significa que el dato ya salio por el bus. Si le avisamos a LVGL que
 * el flush esta listo apenas volvemos de esa llamada (en vez de esperar a
 * que el SPI realmente termine), LVGL reusa el buffer para el siguiente
 * frame mientras el DMA todavia esta mandando el anterior — la cola de
 * transacciones se satura y el driver se queda esperando eternamente
 * (esto era lo que disparaba el watchdog: la tarea de LVGL nunca cedia CPU
 * porque quedaba trabada ahi adentro). El aviso real tiene que venir del
 * callback on_color_trans_done, que dispara cuando el hardware confirma que
 * termino de mandar los bytes. */
static void disp_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)drv->user_data;
    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_map);
    // lv_disp_flush_ready se llama desde on_color_trans_done, no aca.
}

/* --- Entrada tactil ---
 * Se llama desde lv_timer_handler (o sea, ya bajo el lock de LVGL).
 * La pantalla esta espejada en ambos ejes (ver core2_display.c), asi que la
 * coordenada cruda del FT6336U hay que invertirla para que coincida con lo
 * que se ve. Se loguea el primer toque de cada pulsacion para poder calibrar
 * con datos si algun eje quedara al reves. */
static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;
    static bool was_pressed = false;
    uint16_t rx = 0, ry = 0;

    if (core2_touch_read(&rx, &ry)) {
        int x = (CORE2_LCD_WIDTH - 1) - (int)rx;
        int y = (CORE2_LCD_HEIGHT - 1) - (int)ry;
        if (!was_pressed) {
            was_pressed = true;
            ESP_LOGI(TAG, "touch crudo=(%u,%u) -> pantalla=(%d,%d)", rx, ry, x, y);
        }
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x >= CORE2_LCD_WIDTH)  x = CORE2_LCD_WIDTH - 1;
        if (y >= CORE2_LCD_HEIGHT) y = CORE2_LCD_HEIGHT - 1;
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        was_pressed = false;
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static bool on_color_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    lv_disp_drv_t *drv = (lv_disp_drv_t *)user_ctx;
    lv_disp_flush_ready(drv);
    return false; // no hace falta pedir un context-switch desde esta ISR
}

/* Tarea de tick + refresco: LVGL necesita que alguien llame lv_tick_inc()
 * periodicamente y lv_timer_handler() para que efectivamente dibuje. */
static void lvgl_tick_task(void *arg)
{
    /* 10ms, no 5: con CONFIG_FREERTOS_HZ=100 (tick de 10ms) de este proyecto,
     * pdMS_TO_TICKS(5) redondea a 0 ticks — vTaskDelay(0) no espera de verdad,
     * asi que la tarea quedaba en un loop casi cerrado a prioridad 4 sin
     * ceder tiempo a la tarea idle. Eso era lo que disparaba el watchdog
     * (visto en hardware real el 19 ago: iter subia a cientos de miles en
     * segundos, y IDLE0/IDLE1 se moria de hambre). */
    const uint32_t tick_ms = 10;
    while (1) {
        lv_tick_inc(tick_ms);
        if (lvgl_lock(50)) {
            lv_timer_handler();
            lvgl_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(tick_ms));
    }
}

esp_err_t ui_init(void)
{
    ESP_ERROR_CHECK(core2_power_init());
    ESP_ERROR_CHECK(core2_display_init(&s_panel, &s_panel_io));

    s_lvgl_mutex = xSemaphoreCreateMutex();
    if (s_lvgl_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    lv_init();

    /* Buffers de dibujo en RAM INTERNA con capacidad DMA, no en PSRAM.
     *
     * El DMA del SPI lee la memoria directamente, mientras que la CPU escribe
     * los pixeles pasando por la cache. Con el buffer en PSRAM esos dos
     * caminos no estan garantizados coherentes en el ESP32 clasico: el DMA
     * puede leer datos viejos (del frame anterior) mezclados con los nuevos.
     * Eso da exactamente el efecto "fantasma" de texto superpuesto que se
     * vio en hardware real el 19-20 ago — y explica por que los rellenos de
     * color solido salian perfectos: si el dato viejo y el nuevo son el
     * mismo color, la incoherencia es invisible.
     *
     * Cuesta ~25KB de RAM interna (un solo buffer de 40 lineas), que el
     * Core2 tiene de sobra incluso con BLE andando. */
    const size_t buf_pixels = CORE2_LCD_WIDTH * 40;
    lv_color_t *buf1 = heap_caps_malloc(buf_pixels * sizeof(lv_color_t),
                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (buf1 == NULL) {
        ESP_LOGE(TAG, "no se pudo reservar buffer de LVGL en RAM interna DMA");
        return ESP_ERR_NO_MEM;
    }
    /* Log de diagnostico: confirma con datos (no por suposicion) donde quedo
     * el buffer y si el DMA puede leerlo de verdad. */
    ESP_LOGI(TAG, "buffer LVGL: %u bytes, dma_capable=%d, interna=%d, psram=%d",
             (unsigned)(buf_pixels * sizeof(lv_color_t)),
             esp_ptr_dma_capable(buf1),
             esp_ptr_internal(buf1),
             esp_ptr_external_ram(buf1));

    lv_disp_draw_buf_init(&s_draw_buf, buf1, NULL, buf_pixels);

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = CORE2_LCD_WIDTH;
    s_disp_drv.ver_res = CORE2_LCD_HEIGHT;
    s_disp_drv.flush_cb = disp_flush_cb;
    s_disp_drv.draw_buf = &s_draw_buf;
    s_disp_drv.user_data = s_panel;
    lv_disp_drv_register(&s_disp_drv);

    /* Tactil: si el chip no responde la UI igual arranca, solo que sin
     * navegacion — mejor eso que no bootear por un periferico de entrada. */
    if (core2_touch_init() == ESP_OK) {
        static lv_indev_drv_t indev_drv;
        lv_indev_drv_init(&indev_drv);
        indev_drv.type = LV_INDEV_TYPE_POINTER;
        indev_drv.read_cb = touch_read_cb;
        lv_indev_drv_register(&indev_drv);
    } else {
        ESP_LOGW(TAG, "sin tactil: la UI va a funcionar pero no se va a poder navegar");
    }

    const esp_lcd_panel_io_callbacks_t io_cbs = {
        .on_color_trans_done = on_color_trans_done,
    };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(s_panel_io, &io_cbs, &s_disp_drv));

    /* 10240, no 4096: el render de texto (decodificar glifos de la fuente)
     * usa bastante mas stack que pintar rectangulos solidos — con 4096 esta
     * tarea probablemente desbordaba stack silenciosamente al dibujar
     * labels (corrompe memoria vecina sin panic limpio necesariamente),
     * mientras que los tests con solo rectangulos de color solido nunca
     * tocaban ese codigo y por eso salian perfectos. Visto en hardware real
     * el 19-20 ago. */
    BaseType_t ok = xTaskCreate(lvgl_tick_task, "lvgl_tick", 10240, NULL, 4, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "ui_init OK");
    return ESP_OK;
}
