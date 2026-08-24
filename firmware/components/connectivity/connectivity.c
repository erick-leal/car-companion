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

/* Cada cuanto la tarea de fondo revisa si hay algo para sincronizar. Barato
 * de chequear seguido: si no hay WiFi conectado o no hay viajes pendientes,
 * connectivity_sync_trip_history() vuelve al toque sin ningun llamado de
 * red — el costo real (login + POST) solo se paga cuando de verdad hace
 * falta. */
#define SYNC_CHECK_INTERVAL_MS (30 * 1000)

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

static esp_err_t wifi_and_sntp_start(void)
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
    if (err == ESP_OK) err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no se pudo arrancar WiFi STA: %s", esp_err_to_name(err));
        return err;
    }

    /* wait_for_sync=false: no bloquear el arranque esperando la hora. La
     * tarea de fondo de sync ya chequea time_is_valid() antes de usarla.
     * sync_cb solo loguea confirmacion — sin esto no habia forma de saber
     * desde el log si SNTP realmente habia sincronizado o no. */
    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    sntp_cfg.wait_for_sync = false;
    sntp_cfg.sync_cb = &on_sntp_time_synced;
    err = esp_netif_sntp_init(&sntp_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_sntp_init fallo: %s (WiFi sigue andando, pero sin hora real no se puede armar started_at/ended_at para sync)", esp_err_to_name(err));
        // no return: WiFi ya arranco bien, vale la pena seguir aunque SNTP haya fallado
    }

    return ESP_OK;
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
         * silencio -- el error resultante (ej. cJSON_Parse fallando en
         * http_login) apuntaba al backend ("la respuesta no es JSON
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

/* --- Login + registro de dispositivo --- */

static esp_err_t http_login(char *token_out, size_t token_out_size)
{
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "email", BACKEND_EMAIL);
    cJSON_AddStringToObject(body, "password", BACKEND_PASSWORD);
    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (body_str == NULL) return ESP_ERR_NO_MEM;

    char resp[512];
    esp_err_t err = http_post_json("/api/v1/auth/login", NULL, body_str, resp, sizeof(resp), NULL);
    free(body_str);
    if (err != ESP_OK) return err;

    cJSON *parsed = cJSON_Parse(resp);
    if (parsed == NULL) {
        ESP_LOGE(TAG, "login: la respuesta no es JSON valido");
        return ESP_FAIL;
    }
    cJSON *token_item = cJSON_GetObjectItem(parsed, "token");
    if (!cJSON_IsString(token_item)) {
        ESP_LOGE(TAG, "login: la respuesta no tiene un campo 'token' de texto");
        cJSON_Delete(parsed);
        return ESP_FAIL;
    }
    size_t token_len = strlen(token_item->valuestring);
    if (token_len >= token_out_size) {
        /* No solo cortarlo en silencio: un token JWT truncado da un 401
         * "invalido" sin ninguna pista de por que (encontrado en auditoria,
         * 24 ago) -- mejor fallar ahora con la causa clara. */
        ESP_LOGE(TAG, "login: el token (%u bytes) no entra en el buffer de %u bytes, se descarta en vez de truncarlo",
                 (unsigned)token_len, (unsigned)token_out_size);
        cJSON_Delete(parsed);
        return ESP_FAIL;
    }
    strncpy(token_out, token_item->valuestring, token_out_size - 1);
    token_out[token_out_size - 1] = '\0';
    cJSON_Delete(parsed);
    return ESP_OK;
}

static esp_err_t http_register_device(const char *token)
{
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "device_uid", s_device_uid);
    cJSON_AddStringToObject(body, "name", "Car Companion (Maxus T60)");
    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (body_str == NULL) return ESP_ERR_NO_MEM;

    char resp[256];
    int status = 0;
    esp_err_t err = http_post_json("/api/v1/devices", token, body_str, resp, sizeof(resp), &status);
    free(body_str);

    if (err == ESP_OK) return ESP_OK; // nuevo registro o "already_registered", ambos 2xx segun devices.ts
    if (status == 409) {
        ESP_LOGE(TAG, "registro de dispositivo: device_uid %s ya pertenece a otra cuenta del backend", s_device_uid);
    }
    return err;
}

/* --- Armado del JSON de viajes + sync real --- */

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
    /* avg_consumption y dtc_codes se omiten a proposito: avg_consumption no
     * tiene unidad definida todavia en el contrato (ver docs/api-contract.md,
     * TODO agregado el 23 ago) y trip_record_t no guarda los codigos DTC de
     * cada viaje (solo un bool check_engine_seen) — mandar cualquiera de los
     * dos ahora seria inventar un dato, mejor omitirlos (son opcionales en
     * el schema del backend) hasta definirlos bien. */

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
    ESP_LOGI(TAG, "sync: intentando login (hora local del dispositivo: %s, RAM interna libre=%u, bloque contiguo mas grande=%u)",
             now_str,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    char token[512];
    err = http_login(token, sizeof(token));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sync: no se pudo hacer login (%s), reintenta el proximo chequeo", esp_err_to_name(err));
        return err;
    }

    err = http_register_device(token);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sync: no se pudo registrar/confirmar el dispositivo (%s)", esp_err_to_name(err));
        return err;
    }

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
    err = http_post_json("/api/v1/sync/trips", token, body_str, resp, sizeof(resp), NULL);
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

/* El loop principal revisa cada SYNC_CHECK_INTERVAL_MS (30s), pero tambien
 * consulta el buzon de "sincronizar ahora" (boton manual en la pantalla de
 * Viaje) cada 1s — asi un toque del usuario no tiene que esperar hasta 30s
 * para que se note. Consultar el buzon cada 1s es gratis (un booleano), el
 * costo real (login+POST) solo se paga cuando de verdad corresponde. */
static void connectivity_task(void *arg)
{
    (void)arg;
    int64_t last_periodic_check_us = 0;
    while (1) {
        bool manual_requested = state_store_consume_sync_request();
        int64_t now = esp_timer_get_time();
        bool periodic_due = (now - last_periodic_check_us) >= (int64_t)SYNC_CHECK_INTERVAL_MS * 1000;

        if (manual_requested || periodic_due) {
            esp_err_t sync_err = connectivity_sync_trip_history(); // no-op instantaneo y gratis si no hay WiFi o no hay nada pendiente
            /* Si devolvio ESP_ERR_INVALID_STATE (sin WiFi o sin hora SNTP
             * todavia — visto en hardware real el 23 ago: SNTP tardo ~33s,
             * mas que el intervalo de 30s), NO actualizamos el timer: el
             * proximo tick de 1s vuelve a intentar en vez de esperar otros
             * 30s completos de mas por "gastar" el turno en algo que ni
             * siquiera llego a intentar la red. */
            if (sync_err != ESP_ERR_INVALID_STATE) {
                last_periodic_check_us = now;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t connectivity_init(void)
{
    init_device_uid();

    esp_err_t err = wifi_and_sntp_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no se pudo arrancar WiFi/SNTP (%s) — el dispositivo sigue funcionando standalone", esp_err_to_name(err));
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

esp_err_t connectivity_check_ota(void)
{
    // TODO: GET a /api/v1/firmware/latest, comparar version, esp_https_ota si aplica.
    // Queda afuera de la primera version de connectivity (23 ago) -- el foco
    // fue sync de viajes. Ver firmware/README.md "Proximo paso concreto".
    ESP_LOGW(TAG, "connectivity_check_ota: no implementado todavia");
    return ESP_OK;
}
