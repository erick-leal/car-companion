#include "connectivity.h"
#include "connectivity_secrets.h"
#include "storage.h"
#include "state_store.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_mac.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_https_ota.h"
#include "esp_app_desc.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>

static const char *TAG = "connectivity";

/* Cada cuanto la tarea de fondo intenta sincronizar (si hay algo pendiente).
 * Elegido con el usuario el 24 ago, despues de medir ~5% de bateria del M5
 * gastado en menos de 10 minutos prendido sin conectar a nada: en vez de
 * dejar el radio WiFi siempre activo esperando reconectar, se prende solo
 * por una ventana acotada cada este intervalo (o al tocar el boton de sync
 * en Viaje) y se apaga apenas termina el intento. Ver attempt_sync_window(). */
#define SYNC_CHECK_INTERVAL_MS (12 * 60 * 1000)

/* Maximo tiempo con el radio WiFi prendido por intento, conectado o no —
 * si el WiFi de casa no aparece en esta ventana, se apaga igual y se
 * reintenta en el proximo ciclo. */
#define WIFI_CONNECT_WINDOW_MS (20 * 1000)

/* Maximo de viajes por POST — evita un cuerpo JSON gigante de una sola vez
 * si el dispositivo estuvo semanas sin pasar cerca del WiFi de casa. Se van
 * a mandar de a tandas, una por chequeo, hasta ponerse al dia. */
#define SYNC_BATCH_MAX 20

static bool s_wifi_connected = false;
static char s_device_uid[13]; // MAC de 6 bytes en hex, sin separadores, +'\0'

/* --- Utilidades de tiempo real ---
 *
 * storage.h documenta que trip_record_t.start_time_s es tiempo desde el
 * boot (esp_timer), NO hora de pared, porque sin WiFi no habia forma de
 * saber la hora real. Ahora que connectivity trae WiFi, tambien sincroniza
 * la hora por SNTP — eso resuelve esa limitacion, pero solo para el momento
 * de armar el JSON de sync (no tocamos el formato on-disk de trips.bin,
 * menos riesgo de romper algo que ya funciona). */

static bool time_is_valid(void)
{
    /* Si SNTP nunca sincronizo, time(NULL) da un valor cercano al epoch
     * (1970). Cualquier fecha real de hoy es muchisimo mayor — se usa
     * ~nov 2023 como piso, con margen de sobra. */
    return time(NULL) > 1700000000;
}

static void format_iso8601(time_t epoch_s, char *out, size_t out_size)
{
    struct tm tm_utc;
    gmtime_r(&epoch_s, &tm_utc);
    strftime(out, out_size, "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
}

static void on_sntp_time_synced(struct timeval *tv)
{
    char buf[32];
    format_iso8601(tv->tv_sec, buf, sizeof(buf));
    ESP_LOGI(TAG, "hora sincronizada por SNTP: %s", buf);

    /* Publicado para que storage pueda guardar una hora real aproximada
     * junto a cada viaje que cierre de aca en adelante en este arranque
     * (ver nota grande en state_store.h) — sin esto, un viaje grabado en
     * un arranque anterior y recien sincronizado despues quedaba con la
     * fecha del arranque actual, no la real (bug real, 23-24 ago). */
    int64_t offset_s = (int64_t)tv->tv_sec - (esp_timer_get_time() / 1000000);
    state_store_set_wall_clock_offset(offset_s);

    /* Completa la fecha real de cualquier viaje que ya haya cerrado en este
     * mismo arranque antes de que SNTP alcanzara a sincronizar (bug real
     * visto en el auto el 24 ago: si el dispositivo se reiniciaba entre que
     * el viaje cerraba y que se sincronizaba, la fecha reconstruida en el
     * momento del sync ya no era recuperable). No hace falta esperar a que
     * haya un sync pendiente para esto. */
    storage_backfill_recorded_at_epoch();
}

/* --- WiFi + SNTP --- */

/* Backoff de reconexion (encontrado en auditoria, 24 ago): antes de esto,
 * cada WIFI_EVENT_STA_DISCONNECTED disparaba un esp_wifi_connect()
 * inmediato. Fuera del alcance del WiFi de casa (el 95% del tiempo de uso
 * real, manejando), eso es un bucle intento-fallo continuo que gasta CPU y
 * bateria, y mantiene al driver de WiFi trabajando en la misma RAM interna
 * que ya se pelea con NimBLE/LVGL/mbedTLS. Backoff exponencial simple
 * (1s, 2s, 4s... tope 30s), via un esp_timer para no bloquear la tarea del
 * event loop con un delay (esp_wifi_connect() se llama desde ahi mismo). */
#define RECONNECT_BACKOFF_MAX_S 30
static esp_timer_handle_t s_reconnect_timer;
static int s_reconnect_backoff_s = 1;

static void reconnect_timer_cb(void *arg)
{
    esp_wifi_connect();
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect(); // primer intento, sin backoff
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_wifi_connected) {
            ESP_LOGW(TAG, "WiFi desconectado (normal manejando, fuera de rango de casa), reintentando...");
        }
        s_wifi_connected = false;
        ESP_LOGI(TAG, "WiFi: reintentando conectar en %ds", s_reconnect_backoff_s);
        esp_timer_start_once(s_reconnect_timer, (uint64_t)s_reconnect_backoff_s * 1000000ULL);
        s_reconnect_backoff_s *= 2;
        if (s_reconnect_backoff_s > RECONNECT_BACKOFF_MAX_S) {
            s_reconnect_backoff_s = RECONNECT_BACKOFF_MAX_S;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_wifi_connected = true;
        s_reconnect_backoff_s = 1; // conexion lograda, el proximo corte vuelve a empezar desde 1s
        ESP_LOGI(TAG, "WiFi conectado (IP obtenida), sincronizando hora por SNTP...");
    }
}

static void init_device_uid(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_device_uid, sizeof(s_device_uid), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* Solo inicializa el stack de WiFi (driver, netif, handlers, SNTP) — NO
 * prende el radio (eso es wifi_radio_start(), llamado a demanda por cada
 * ventana de sync). Se llama una sola vez, en connectivity_init(). */
static esp_err_t wifi_init_once(void)
{
    esp_err_t err = nvs_flash_init(); // WiFi necesita NVS para calibracion; idempotente si obd_driver ya lo inicializo
    /* Nada de ESP_ERROR_CHECK en esta funcion a proposito: eso llama a
     * abort() y reinicia TODO el dispositivo con cualquier fallo. Ya se vio
     * en hardware real (23 ago): esp_wifi_init() fallando por RAM interna
     * justa (compartida con NimBLE + LVGL) tiraba abajo el gauge OBD
     * completo — justo lo que connectivity.h dice que no puede pasar.
     * Cada paso se chequea y, si falla, se loguea fuerte y se corta ahi con
     * un error normal — el resto del firmware sigue andando igual. */
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init fallo: %s", esp_err_to_name(err));
        return err;
    }

    const esp_timer_create_args_t reconnect_timer_args = {
        .callback = &reconnect_timer_cb,
        .name = "wifi_reconnect",
    };
    err = esp_timer_create(&reconnect_timer_args, &s_reconnect_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_create (reconexion WiFi) fallo: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_netif_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_init fallo: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { // INVALID_STATE = ya existia, no es un error real acá
        ESP_LOGE(TAG, "esp_event_loop_create_default fallo: %s", esp_err_to_name(err));
        return err;
    }
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wifi_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init fallo (%s) — sin RAM interna suficiente? ver CONFIG_ESP_WIFI_*_BUFFER_NUM en sdkconfig.defaults", esp_err_to_name(err));
        return err;
    }

    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    if (err == ESP_OK) {
        err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_event_handler_register fallo: %s", esp_err_to_name(err));
        return err;
    }

    wifi_config_t sta_cfg = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
        },
    };
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no se pudo configurar WiFi STA: %s", esp_err_to_name(err));
        return err;
    }
    // NO esp_wifi_start() aca -- eso lo hace wifi_radio_start() a demanda, ver mas abajo

    /* wait_for_sync=false: no bloquear el arranque esperando la hora. La
     * tarea de fondo de sync ya chequea time_is_valid() antes de usarla.
     * sync_cb solo loguea confirmacion — sin esto no habia forma de saber
     * desde el log si SNTP realmente habia sincronizado o no. */
    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    sntp_cfg.wait_for_sync = false;
    sntp_cfg.sync_cb = &on_sntp_time_synced;
    err = esp_netif_sntp_init(&sntp_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_sntp_init fallo: %s (sin hora real no se puede armar started_at/ended_at para sync)", esp_err_to_name(err));
        // no return: el resto de WiFi quedo bien configurado, vale la pena seguir aunque SNTP haya fallado
    }

    return ESP_OK;
}

/* Prende el radio WiFi para un intento de sync (ver attempt_sync_window).
 * esp_wifi_start() dispara WIFI_EVENT_STA_START -> esp_wifi_connect()
 * automatico via wifi_event_handler. */
static void wifi_radio_start(void)
{
    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_start fallo (%s), no se pudo prender el radio para este intento de sync", esp_err_to_name(err));
        return;
    }
    /* BUG DE BATERIA REAL ENCONTRADO EN USO (24 ago): sin esto, mientras el
     * radio esta prendido queda en WIFI_PS_NONE (siempre activo al 100%,
     * el default cuando CONFIG_PM_ENABLE esta apagado). WIFI_PS_MIN_MODEM
     * deja que duerma entre balizas del punto de acceso una vez conectado
     * — el unico costo real es un poco mas de latencia, que no importa acá
     * (proceso de fondo sin apuro). Se re-aplica cada vez que se prende el
     * radio (no se sabe con certeza si el modo persiste entre un
     * esp_wifi_stop()/esp_wifi_start(), mejor no asumir). No se chequea el
     * rc: si falla, WiFi sigue andando en el modo anterior, no amerita
     * cortar el intento de sync por esto. */
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
}

/* Apaga el radio WiFi al terminar un intento de sync (exitoso o no). */
static void wifi_radio_stop(void)
{
    esp_timer_stop(s_reconnect_timer); // cancela cualquier reintento de backoff que hubiera quedado pendiente
    esp_wifi_stop();
    s_wifi_connected = false;
    s_reconnect_backoff_s = 1; // arrancar fresco la proxima ventana, no arrastrar el backoff de esta
}

/* --- Cliente HTTP: POST JSON con Bearer opcional, junta la respuesta en un
 * buffer del llamador via el event handler (esp_http_client no da acceso
 * directo al cuerpo con esp_http_client_perform solo). --- */

typedef struct {
    char   *buf;
    size_t  buf_size;
    size_t  written;
    bool    truncated; // ver nota en http_post_json — encontrado en auditoria, 24 ago
} http_resp_ctx_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA || evt->user_data == NULL) {
        return ESP_OK;
    }
    http_resp_ctx_t *ctx = (http_resp_ctx_t *)evt->user_data;
    if (ctx->buf == NULL || ctx->buf_size == 0) return ESP_OK;

    size_t space = ctx->buf_size - ctx->written - 1; // -1 para el '\0' final
    size_t to_copy = (size_t)evt->data_len < space ? (size_t)evt->data_len : space;
    if ((size_t)evt->data_len > space) {
        ctx->truncated = true; // se descarta el resto en silencio, ver log en http_post_json
    }
    if (to_copy > 0) {
        memcpy(ctx->buf + ctx->written, evt->data, to_copy);
        ctx->written += to_copy;
        ctx->buf[ctx->written] = '\0';
    }
    return ESP_OK;
}

static esp_err_t http_post_json(const char *path, const char *bearer_token, const char *body,
                                 char *resp_buf, size_t resp_buf_size, int *out_status)
{
    char url[192];
    snprintf(url, sizeof(url), "%s%s", BACKEND_BASE_URL, path);

    http_resp_ctx_t ctx = { .buf = resp_buf, .buf_size = resp_buf_size, .written = 0 };
    if (resp_buf != NULL && resp_buf_size > 0) resp_buf[0] = '\0';

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 8000,
        .event_handler = http_event_handler,
        .user_data = &ctx,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "no se pudo crear el cliente HTTP para %s", path);
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    char auth_header[560];
    if (bearer_token != NULL) {
        snprintf(auth_header, sizeof(auth_header), "Bearer %s", bearer_token);
        esp_http_client_set_header(client, "Authorization", auth_header);
    }
    esp_http_client_set_post_field(client, body, (int)strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    if (out_status != NULL) *out_status = status;
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP POST %s fallo (transporte): %s", path, esp_err_to_name(err));
        return err;
    }
    if (ctx.truncated) {
        /* Bug real encontrado en auditoria (24 ago): antes de este chequeo,
         * una respuesta mas larga que el buffer local se cortaba en
         * silencio -- el error resultante (ej. un cJSON_Parse fallando
         * mas arriba) apuntaba al backend ("la respuesta no es JSON
         * valido") cuando el problema real es que ESTE buffer se quedo
         * corto. Pasa si el backend agrega un campo al payload sin que el
         * firmware se entere. */
        ESP_LOGW(TAG, "la respuesta de %s no entra en el buffer local de %u bytes (se trunco) -- "
                      "si algo falla parseandola despues de este log, el buffer es sospechoso #1",
                 path, (unsigned)resp_buf_size);
    }
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "HTTP POST %s devolvio status %d: %s", path, status,
                 (resp_buf != NULL && resp_buf[0] != '\0') ? resp_buf : "(sin cuerpo)");
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* Igual que http_post_json pero GET, sin body ni token -- lo unico que la
 * usa hoy es connectivity_check_ota() contra /firmware/latest, que es
 * publico (ver firmware.ts en el backend). */
static esp_err_t http_get_json(const char *path, char *resp_buf, size_t resp_buf_size)
{
    char url[192];
    snprintf(url, sizeof(url), "%s%s", BACKEND_BASE_URL, path);

    http_resp_ctx_t ctx = { .buf = resp_buf, .buf_size = resp_buf_size, .written = 0 };
    if (resp_buf != NULL && resp_buf_size > 0) resp_buf[0] = '\0';

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 8000,
        .event_handler = http_event_handler,
        .user_data = &ctx,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "no se pudo crear el cliente HTTP para %s", path);
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP GET %s fallo (transporte): %s", path, esp_err_to_name(err));
        return err;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "HTTP GET %s devolvio status %d", path, status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* --- Armado del JSON de viajes + sync real ---
 *
 * Ya NO hay login ni registro por HTTP acá (bug de diseño arreglado el
 * 24 ago): antes, cada intento de sync hacia POST /auth/login con el
 * email/contraseña REAL del usuario (guardados en connectivity_secrets.h)
 * para sacar un JWT, y despues POST /devices para confirmar el registro —
 * dos viajes de red de mas, y la contraseña real de la cuenta viviendo en
 * la flash del M5. Ahora el firmware usa DEVICE_TOKEN directo como Bearer:
 * un token propio de este dispositivo (generado una vez desde una maquina
 * humana autenticada, ver connectivity_secrets.example.h), revocable sin
 * tocar la cuenta, que nunca fue la contraseña real. El registro del
 * dispositivo tambien pasa a ser una accion humana (POST /devices con el
 * JWT del usuario, no algo que el firmware repita solo). */

/* Devuelve false si no se pudo armar/agregar el objeto (tipicamente sin
 * memoria) — el llamador NO debe contar ese viaje como sincronizado en ese
 * caso (bug real encontrado en auditoria, 24 ago: antes de este chequeo, un
 * cJSON_CreateObject() fallido en OOM se contaba igual en `added`, y
 * storage_mark_trips_synced() marcaba el viaje como sincronizado aunque
 * nunca se mando al backend — perdida de datos definitiva y silenciosa). */
static bool add_trip_to_array(cJSON *trips_array, const trip_record_t *rec, int64_t boot_epoch_s)
{
    cJSON *t = cJSON_CreateObject();
    if (t == NULL) {
        ESP_LOGE(TAG, "sin memoria para armar el JSON de un viaje, se reintenta el proximo ciclo de sync");
        return false;
    }

    /* Preferimos recorded_at_epoch_s (hora real guardada por storage cuando
     * cerro el viaje, ver storage.h) por sobre reconstruirla con el
     * boot_epoch del arranque ACTUAL — ese boot_epoch solo es correcto si
     * el viaje paso en este mismo arranque; para un viaje de un arranque
     * anterior que recien se sincroniza ahora, da una fecha incorrecta
     * (bug real, 23-24 ago: dos viajes de dias distintos quedaron con la
     * misma fecha). recorded_at_epoch_s==0 solo pasa en viajes guardados
     * antes de esta funcionalidad, o si SNTP nunca habia sincronizado
     * cuando ese viaje en particular cerro — ahi no queda otra que la
     * aproximacion vieja. */
    int64_t start_epoch_s = rec->recorded_at_epoch_s != 0
        ? (int64_t)rec->recorded_at_epoch_s
        : boot_epoch_s + rec->start_time_s;

    char started[32], ended[32];
    format_iso8601((time_t)start_epoch_s, started, sizeof(started));
    format_iso8601((time_t)(start_epoch_s + rec->duration_s), ended, sizeof(ended));
    cJSON_AddStringToObject(t, "started_at", started);
    cJSON_AddStringToObject(t, "ended_at", ended);
    cJSON_AddNumberToObject(t, "distance_km", rec->distance_km);
    cJSON_AddNumberToObject(t, "max_rpm", rec->max_rpm);

    /* avg_consumption en L/100km (unidad definida el 24 ago, ver
     * docs/api-contract.md — el backend ya aceptaba/devolvia el campo sin
     * cambios, solo faltaba que el firmware lo calculara). Se omite (no se
     * manda el campo) si distance_km es 0 (division invalida) o si
     * fuel_used_l es 0: ese ultimo caso no es "consumio cero combustible en
     * todo el viaje", es que el vehiculo no expone el PID de caudal (0x5E)
     * y fuel_used_l nunca se acumulo — mismo criterio "0 = sin dato" que ya
     * usa el resto del codebase para PIDs no soportados. */
    if (rec->distance_km > 0.0f && rec->fuel_used_l > 0.0f) {
        double avg_consumption_l_100km = (double)rec->fuel_used_l / (double)rec->distance_km * 100.0;
        cJSON_AddNumberToObject(t, "avg_consumption", avg_consumption_l_100km);
    }

    /* dtc_codes se omite a proposito: trip_record_t no guarda los codigos
     * DTC de cada viaje individual (solo un bool check_engine_seen) —
     * mandarlo ahora seria inventar un dato. Habria que decidir si vale la
     * pena guardar los codigos reales por viaje en storage para esto (ver
     * docs/api-contract.md). */

    cJSON_AddItemToArray(trips_array, t);
    return true;
}

esp_err_t connectivity_sync_trip_history(void)
{
    if (!s_wifi_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!time_is_valid()) {
        return ESP_ERR_INVALID_STATE; // SNTP todavia no sincronizo, no podemos armar started_at/ended_at reales
    }

    uint32_t pending;
    esp_err_t err = storage_get_pending_sync_count(&pending);
    if (err != ESP_OK || pending == 0) {
        return ESP_OK; // nada para hacer, no es un error
    }

    uint32_t total;
    storage_get_trip_count(&total);
    uint32_t synced_count = total - pending;
    uint32_t batch_end = synced_count + pending;
    if (batch_end - synced_count > SYNC_BATCH_MAX) {
        batch_end = synced_count + SYNC_BATCH_MAX;
    }

    char now_str[32];
    format_iso8601(time(NULL), now_str, sizeof(now_str));
    ESP_LOGI(TAG, "sync: armando %lu viaje(s) (hora local del dispositivo: %s, RAM interna libre=%u, bloque contiguo mas grande=%u)",
             (unsigned long)pending, now_str,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    int64_t boot_epoch_s = time(NULL) - (esp_timer_get_time() / 1000000);

    cJSON *root = cJSON_CreateObject();
    cJSON *trips_array = root != NULL ? cJSON_AddArrayToObject(root, "trips") : NULL;
    if (root == NULL || trips_array == NULL) {
        ESP_LOGE(TAG, "sin memoria para armar el JSON de sync, se reintenta el proximo ciclo");
        if (root != NULL) cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "device_uid", s_device_uid);

    /* Se corta en el primer viaje que no se pudo agregar (en vez de
     * saltearlo y seguir con el siguiente): storage_mark_trips_synced solo
     * entiende "los primeros N viajes desde el cursor", no un conjunto con
     * huecos — si el viaje 3 fallara por OOM pero el 4 se agregara bien,
     * marcar "4 sincronizados" incluiria al 3 (que nunca se mando) como
     * si lo estuviera. Bug real evitado en auditoria (24 ago). */
    uint32_t added = 0;
    for (uint32_t i = synced_count; i < batch_end; i++) {
        trip_record_t rec;
        if (storage_get_trip(i, &rec) != ESP_OK) break;
        if (!add_trip_to_array(trips_array, &rec, boot_epoch_s)) break;
        added++;
    }

    char *body_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body_str == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char resp[256];
    err = http_post_json("/api/v1/sync/trips", DEVICE_TOKEN, body_str, resp, sizeof(resp), NULL);
    free(body_str);

    if (err == ESP_OK) {
        storage_mark_trips_synced(synced_count + added);
        ESP_LOGI(TAG, "sync: %lu viaje(s) sincronizados con el backend", (unsigned long)added);
    } else {
        ESP_LOGW(TAG, "sync: fallo el envio de viajes (%s), reintenta el proximo chequeo", esp_err_to_name(err));
    }
    return err;
}

/* --- Tarea de fondo --- */

/* Prende el radio WiFi, espera hasta WIFI_CONNECT_WINDOW_MS a que conecte, y
 * si conecta intenta sincronizar — apaga el radio al final pase lo que
 * pase (conectó o no, sincronizó bien o no). No prende el radio para nada
 * si no hay ningun viaje pendiente: ese chequeo es local, no necesita red. */
static void attempt_sync_window(void)
{
    uint32_t pending;
    if (storage_get_pending_sync_count(&pending) != ESP_OK || pending == 0) {
        state_store_set_sync_status(SYNC_STATUS_NOTHING_PENDING);
        return; // no vale la pena gastar bateria prendiendo el radio
    }

    ESP_LOGI(TAG, "prendiendo WiFi para intentar sincronizar (%lu viaje(s) pendiente(s))...", (unsigned long)pending);
    state_store_set_sync_status(SYNC_STATUS_IN_PROGRESS);
    wifi_radio_start();

    int64_t deadline_us = esp_timer_get_time() + (int64_t)WIFI_CONNECT_WINDOW_MS * 1000;
    while (!s_wifi_connected && esp_timer_get_time() < deadline_us) {
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    if (s_wifi_connected) {
        /* time_is_valid() puede seguir en false un instante despues de
         * conectar (SNTP todavia no completo su primer request) — un poco
         * de margen extra acá antes de darlo por perdido en esta ventana.
         * 12s, no 5: visto en el auto real (24 ago) que 5s no siempre
         * alcanza aunque el WiFi ya conecto rapido y sobra presupuesto en
         * WIFI_CONNECT_WINDOW_MS (20s total) para esperar un poco mas. */
        int64_t sntp_deadline_us = esp_timer_get_time() + 12LL * 1000000;
        while (!time_is_valid() && esp_timer_get_time() < sntp_deadline_us) {
            vTaskDelay(pdMS_TO_TICKS(300));
        }
        esp_err_t err = connectivity_sync_trip_history();
        if (err == ESP_OK) {
            state_store_set_sync_status(SYNC_STATUS_OK);
        } else {
            if (err == ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "sync: WiFi conecto pero la hora SNTP no llego a tiempo, se reintenta la proxima ventana");
            }
            state_store_set_sync_status(SYNC_STATUS_ERROR);
        }

        /* Reusa esta misma ventana de WiFi para chequear firmware nuevo --
         * no vale la pena prender el radio aparte solo para esto. Si hay
         * OTA para aplicar y el OBD no esta conectado, connectivity_check_ota
         * reinicia el dispositivo sola y esta funcion nunca vuelve. */
        connectivity_check_ota();
    } else {
        ESP_LOGI(TAG, "no se encontro el WiFi de casa en %ds, apagando el radio hasta el proximo intento",
                 WIFI_CONNECT_WINDOW_MS / 1000);
        state_store_set_sync_status(SYNC_STATUS_NO_WIFI);
    }

    wifi_radio_stop();
}

/* El loop principal revisa cada SYNC_CHECK_INTERVAL_MS, pero tambien
 * consulta el buzon de "sincronizar ahora" (boton manual en la pantalla de
 * Viaje) cada 1s — asi un toque del usuario no tiene que esperar hasta el
 * proximo ciclo automatico para que se note. */
static void connectivity_task(void *arg)
{
    (void)arg;
    int64_t last_periodic_check_us = 0;
    while (1) {
        bool manual_requested = state_store_consume_sync_request();
        int64_t now = esp_timer_get_time();
        bool periodic_due = (now - last_periodic_check_us) >= (int64_t)SYNC_CHECK_INTERVAL_MS * 1000;

        if (manual_requested || periodic_due) {
            attempt_sync_window();
            last_periodic_check_us = esp_timer_get_time();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t connectivity_init(void)
{
    init_device_uid();

    esp_err_t err = wifi_init_once();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no se pudo inicializar WiFi/SNTP (%s) — el dispositivo sigue funcionando standalone", esp_err_to_name(err));
        return err;
    }

    /* 12KB, no 6KB: un handshake TLS real (verificacion de firma RSA/ECDSA
     * contra el cert bundle) necesita bastante stack — con 6KB fallaba en
     * hardware real con "PK verify failed" durante el handshake (23 ago),
     * sintoma clasico de stack corto en mbedTLS (la corrupcion silenciosa
     * de la pila arruina el resultado de la operacion criptografica en vez
     * de crashear limpio). */
    BaseType_t ok = xTaskCreate(connectivity_task, "connectivity", 12288, NULL, 3, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "no se pudo crear la tarea de connectivity");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "connectivity_init OK (device_uid=%s, backend=%s)", s_device_uid, BACKEND_BASE_URL);
    return ESP_OK;
}

/* Implementado el 24 ago, llamada desde attempt_sync_window() (ver mas
 * abajo) reusando la misma ventana de WiFi que ya esta abierta para el
 * sync de viajes -- no vale la pena prender el radio aparte solo para
 * chequear version. */
esp_err_t connectivity_check_ota(void)
{
    if (!s_wifi_connected) {
        return ESP_ERR_INVALID_STATE;
    }

    char resp[256];
    esp_err_t err = http_get_json("/api/v1/firmware/latest", resp, sizeof(resp));
    if (err != ESP_OK) {
        return err; // sin firmware publicado (404) o error de red -- nada que hacer, no es grave
    }

    cJSON *root = cJSON_Parse(resp);
    if (root == NULL) {
        ESP_LOGW(TAG, "OTA: respuesta de /firmware/latest no es JSON valido");
        return ESP_FAIL;
    }
    cJSON *version_item = cJSON_GetObjectItem(root, "version");
    cJSON *url_item = cJSON_GetObjectItem(root, "url");
    if (!cJSON_IsString(version_item) || !cJSON_IsString(url_item)) {
        ESP_LOGW(TAG, "OTA: /firmware/latest no trajo version/url validos");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    const esp_app_desc_t *running = esp_app_get_description();
    if (strcmp(running->version, version_item->valuestring) == 0) {
        cJSON_Delete(root); // ya estamos en la ultima version, nada que hacer
        return ESP_OK;
    }

    /* No aplicar OTA con el OBD conectado: mitad de un viaje real no es
     * momento de reiniciar el M5 (se corta el gauge, y un viaje en curso
     * que no llego a checkpoint reciente perderia mas de lo necesario) --
     * se reintenta solo en la proxima ventana con WiFi, cuando el auto ya
     * no este conectado. */
    vehicle_state_t vs;
    state_store_get(&vs);
    if (vs.data_valid) {
        ESP_LOGI(TAG, "OTA: firmware nuevo disponible (%s, actual %s) pero el OBD esta conectado -- se aplica la proxima vez que no lo este",
                 version_item->valuestring, running->version);
        cJSON_Delete(root);
        return ESP_OK;
    }

    char ota_url[256];
    snprintf(ota_url, sizeof(ota_url), "%s", url_item->valuestring);
    ESP_LOGW(TAG, "OTA: aplicando firmware %s (actual %s) desde %s ...",
             version_item->valuestring, running->version, ota_url);
    cJSON_Delete(root); // liberar antes de la descarga larga, no hace falta mas

    esp_http_client_config_t ota_http_config = {
        .url = ota_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000, // el binario pesa ~1.5MB, mas margen que un POST chico
    };
    esp_https_ota_config_t ota_config = {
        .http_config = &ota_http_config,
    };
    esp_err_t ota_err = esp_https_ota(&ota_config);
    if (ota_err == ESP_OK) {
        ESP_LOGW(TAG, "OTA: descarga y escritura exitosas, reiniciando para arrancar la version nueva...");
        esp_restart(); // no vuelve
    }
    ESP_LOGE(TAG, "OTA: fallo (%s) -- se sigue con la version actual, se reintenta la proxima ventana", esp_err_to_name(ota_err));
    return ota_err;
}
