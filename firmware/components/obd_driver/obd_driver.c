#include "obd_driver.h"
#include "obd_driver_config.h"
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gattc.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"

#include <string.h>

static const char *TAG = "obd_driver";

/* --- Estado de conexión --- */
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_write_char_handle = 0;
static uint16_t s_notify_char_handle = 0;
static uint16_t s_notify_cccd_handle = 0;
static bool s_ready = false; // true recien despues de completar ATZ/ATE0 (ver obd_send_init_sequence)

/* --- Comando en vuelo (ver nota en el header: uno a la vez) --- */
static SemaphoreHandle_t s_cmd_mutex;
static obd_response_cb_t s_pending_cb = NULL;
static void *s_pending_ctx = NULL;

/* Buffer de acumulacion de respuesta: el ELM327 puede mandar la respuesta en
 * varios paquetes BLE (notify), termina con el prompt '>' cuando esta completa. */
#define RESPONSE_BUF_SIZE 256
static uint8_t s_response_buf[RESPONSE_BUF_SIZE];
static size_t s_response_len = 0;

static void start_scan(void);
static void obd_send_init_sequence(void);

/* ---------------------------------------------------------------------
 * Descubrimiento de características (una vez conectados)
 * --------------------------------------------------------------------- */

static int on_chr_disc(uint16_t conn_handle,
                        const struct ble_gatt_error *error,
                        const struct ble_gatt_chr *chr,
                        void *arg)
{
    if (error->status != 0 && error->status != BLE_HS_EDONE) {
        ESP_LOGE(TAG, "error descubriendo caracteristicas: %d", error->status);
        return 0;
    }
    if (chr == NULL) {
        /* BLE_HS_EDONE: se termino de listar. Si encontramos ambas, suscribimos. */
        if (s_write_char_handle && s_notify_char_handle) {
            ESP_LOGI(TAG, "caracteristicas encontradas (write=0x%04x, notify=0x%04x)",
                     s_write_char_handle, s_notify_char_handle);
            /* La característica notify normalmente trae el CCCD en el handle
             * siguiente (val_handle + 1) — así es en la gran mayoría de stacks
             * BLE, pero no es garantía absoluta del estándar. Si la suscripción
             * falla en hardware real, este es el primer lugar a revisar
             * (se puede resolver de forma robusta con un descriptor-discovery,
             * omitido acá para no sobrecomplicar el v1). */
            uint16_t cccd_val = 1; // 0x0001 = habilitar notificaciones
            int rc = ble_gattc_write_flat(conn_handle, s_notify_cccd_handle,
                                           &cccd_val, sizeof(cccd_val), NULL, NULL);
            if (rc != 0) {
                ESP_LOGE(TAG, "fallo al suscribirse a notificaciones: %d", rc);
            } else {
                /* No esperamos confirmación del write de CCCD antes de mandar
                 * el init ELM327 (simplificación de v1) — si en hardware real
                 * el adaptador pierde el primer ATZ por timing, agregar acá
                 * un pequeño delay o mover esto al callback de confirmación
                 * del write (ble_gattc_write_flat acepta uno como 5to arg). */
                obd_send_init_sequence();
            }
        } else {
            ESP_LOGE(TAG, "no se encontraron las caracteristicas esperadas "
                           "(revisar obd_driver_config.h)");
        }
        return 0;
    }

    if (chr->uuid.u.type == BLE_UUID_TYPE_16) {
        uint16_t uuid16 = BLE_UUID16(&chr->uuid)->value;
        if (uuid16 == OBD_BLE_WRITE_CHAR_UUID) {
            s_write_char_handle = chr->val_handle;
        } else if (uuid16 == OBD_BLE_NOTIFY_CHAR_UUID) {
            s_notify_char_handle = chr->val_handle;
            s_notify_cccd_handle = chr->val_handle + 1; // ver nota arriba
        }
    }
    return 0;
}

static int on_svc_disc(uint16_t conn_handle,
                        const struct ble_gatt_error *error,
                        const struct ble_gatt_svc *service,
                        void *arg)
{
    if (error->status != 0) {
        ESP_LOGE(TAG, "error descubriendo el servicio OBD (uuid configurado en "
                       "obd_driver_config.h): %d", error->status);
        return 0;
    }
    ESP_LOGI(TAG, "servicio OBD encontrado, descubriendo caracteristicas...");
    ble_gattc_disc_all_chrs(conn_handle, service->start_handle, service->end_handle,
                             on_chr_disc, NULL);
    return 0;
}

static void discover_service(uint16_t conn_handle)
{
    ble_uuid16_t svc_uuid = BLE_UUID16_INIT(OBD_BLE_SERVICE_UUID);
    ble_gattc_disc_svc_by_uuid(conn_handle, &svc_uuid.u, on_svc_disc, NULL);
}

/* ---------------------------------------------------------------------
 * Envio de la secuencia de init ELM327 apenas queda todo suscripto
 * --------------------------------------------------------------------- */

static void obd_write_raw(const char *cmd)
{
    if (s_write_char_handle == 0) {
        ESP_LOGE(TAG, "obd_write_raw: no hay characteristic de escritura todavia");
        return;
    }
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%s\r", cmd); // ELM327 espera CR al final
    ble_gattc_write_no_rsp_flat(s_conn_handle, s_write_char_handle, buf, n);
}

static void obd_send_init_sequence(void)
{
    /* ATZ (reset), ATE0 (echo off — clave, si no el ELM327 nos devuelve el
     * comando que mandamos antes de la respuesta real), ATL0 (sin saltos de
     * linea extra), ATSP0 (autodetectar protocolo del vehiculo). */
    ESP_LOGI(TAG, "enviando secuencia de init ELM327...");
    obd_write_raw("ATZ");
    // TODO: hay que esperar la respuesta de cada AT antes de mandar el
    // siguiente (mismo mecanismo de "un comando en vuelo" que send_command).
    // Simplificado por ahora; si en hardware real se pisan, encolar con delay.
    obd_write_raw("ATE0");
    obd_write_raw("ATL0");
    obd_write_raw("ATSP0");
    s_ready = true; // optimista; ver TODO arriba para hacerlo robusto de verdad
}

/* ---------------------------------------------------------------------
 * GAP event handler
 * --------------------------------------------------------------------- */

static int gap_event_handler(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
        case BLE_GAP_EVENT_DISC: {
            /* Filtramos por nombre del dispositivo anunciado. */
            struct ble_hs_adv_fields fields;
            ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data);

            if (fields.name != NULL &&
                strncmp((char *)fields.name, OBD_BLE_DEVICE_NAME, strlen(OBD_BLE_DEVICE_NAME)) == 0) {
                ESP_LOGI(TAG, "encontrado adaptador OBD, conectando...");
                ble_gap_disc_cancel();
                ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &event->disc.addr, 5000, NULL,
                                 gap_event_handler, NULL);
            }
            return 0;
        }

        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                s_conn_handle = event->connect.conn_handle;
                ESP_LOGI(TAG, "conectado (handle=%d), descubriendo servicios...", s_conn_handle);
                discover_service(s_conn_handle);
            } else {
                ESP_LOGE(TAG, "fallo la conexion (status=%d), reintentando escaneo",
                          event->connect.status);
                start_scan();
            }
            return 0;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGW(TAG, "desconectado (reason=%d), reintentando escaneo",
                     event->disconnect.reason);
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            s_write_char_handle = 0;
            s_notify_char_handle = 0;
            s_ready = false;
            start_scan();
            return 0;

        case BLE_GAP_EVENT_NOTIFY_RX: {
            /* Llega un pedazo de la respuesta ELM327. Acumulamos hasta ver '>'. */
            uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
            if (s_response_len + len < RESPONSE_BUF_SIZE) {
                ble_hs_mbuf_to_flat(event->notify_rx.om, &s_response_buf[s_response_len],
                                     len, NULL);
                s_response_len += len;
            } else {
                ESP_LOGE(TAG, "buffer de respuesta OBD lleno, descartando");
                s_response_len = 0;
                return 0;
            }

            if (s_response_len > 0 && s_response_buf[s_response_len - 1] == '>') {
                /* Un pedido en particular considera que la característica de escritura
                 * quedó lista al recibir la respuesta a la secuencia de init;
                 * mientras eso no esté hecho de forma robusta (ver TODO en
                 * obd_send_init_sequence), simplemente despachamos al callback
                 * pendiente si existe. */
                if (s_pending_cb != NULL) {
                    s_pending_cb(s_response_buf, s_response_len, s_pending_ctx);
                    s_pending_cb = NULL;
                    s_pending_ctx = NULL;
                    xSemaphoreGive(s_cmd_mutex);
                }
                s_response_len = 0;
            }
            return 0;
        }

        default:
            return 0;
    }
}

static void start_scan(void)
{
    struct ble_gap_disc_params disc_params = {0};
    disc_params.passive = 0;
    disc_params.itvl = 0;
    disc_params.window = 0;
    disc_params.filter_duplicates = 1;

    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &disc_params,
                           gap_event_handler, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_disc fallo: %d", rc);
    } else {
        ESP_LOGI(TAG, "escaneando adaptadores BLE (buscando '%s')...", OBD_BLE_DEVICE_NAME);
    }
}

static void on_sync(void)
{
    ble_svc_gap_device_name_set("car-companion");
    start_scan();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE se reinicio, razon: %d", reason);
}

static void nimble_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t obd_driver_init(void)
{
    s_cmd_mutex = xSemaphoreCreateMutex();
    if (s_cmd_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = nvs_flash_init(); // NimBLE guarda bonding/config en NVS
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(nimble_port_init());

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    nimble_port_freertos_init(nimble_host_task);

    ESP_LOGI(TAG, "obd_driver_init OK — UUIDs configurados en obd_driver_config.h "
                   "(CONFIRMAR contra el adaptador real con nRF Connect)");
    return ESP_OK;
}

esp_err_t obd_driver_send_command(const char *command, obd_response_cb_t cb, void *ctx)
{
    if (!obd_driver_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Espera a que no haya otro comando en vuelo. Timeout generoso porque el
     * ELM327 puede tardar (sobre todo autodetectando protocolo la primera vez). */
    if (xSemaphoreTake(s_cmd_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGE(TAG, "timeout esperando turno para enviar comando '%s'", command);
        return ESP_ERR_TIMEOUT;
    }

    s_pending_cb = cb;
    s_pending_ctx = ctx;
    obd_write_raw(command);
    /* El mutex se libera en BLE_GAP_EVENT_NOTIFY_RX cuando llega la respuesta
     * completa (o queda tomado hasta el próximo timeout si el adaptador no
     * responde — aceptable para v1, revisar si se ve en campo). */
    return ESP_OK;
}

bool obd_driver_is_connected(void)
{
    return s_conn_handle != BLE_HS_CONN_HANDLE_NONE && s_ready;
}
