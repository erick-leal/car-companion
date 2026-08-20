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
#include "host/ble_gatt.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "esp_timer.h"

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

/* BUG ENCONTRADO EN REVISION: si el adaptador nunca responde a un comando
 * (se desconectó a mitad de camino, se colgó, etc.), la versión anterior
 * dejaba el mutex tomado para siempre — cada llamada siguiente a
 * obd_driver_send_command habría esperado 2s y fallado con timeout, sin
 * recuperarse nunca aunque el adaptador volviera a responder después.
 * Este timer es el watchdog que libera el turno si no llega respuesta a
 * tiempo, para que un solo comando perdido no mate el driver entero. */
#define COMMAND_TIMEOUT_MS 3000
static esp_timer_handle_t s_response_timeout_timer;

/* Buffer de acumulacion de respuesta: el ELM327 puede mandar la respuesta en
 * varios paquetes BLE (notify), termina con el prompt '>' cuando esta completa. */
#define RESPONSE_BUF_SIZE 256
static uint8_t s_response_buf[RESPONSE_BUF_SIZE];
static size_t s_response_len = 0;

static void on_response_timeout(void *arg)
{
    ESP_LOGW(TAG, "timeout esperando respuesta OBD, liberando turno para el siguiente comando");
    s_pending_cb = NULL;
    s_pending_ctx = NULL;
    s_response_len = 0;
    xSemaphoreGive(s_cmd_mutex);
}

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
    /* BUG ENCONTRADO EN REVISION: NimBLE llama este callback una última vez
     * al terminar la búsqueda con error->status == BLE_HS_EDONE y service ==
     * NULL — eso NO es un error, es la señal normal de "no hay más
     * resultados". La versión anterior lo trataba como error y logueaba un
     * falso positivo cada vez que el servicio SÍ se encontraba bien
     * (create-y-log-error-en-el-camino-feliz es confuso para debuggear).
     * Mismo patrón que ya está bien hecho en on_chr_disc, ahora unificado acá. */
    if (error->status != 0 && error->status != BLE_HS_EDONE) {
        ESP_LOGE(TAG, "error descubriendo el servicio OBD (uuid configurado en "
                       "obd_driver_config.h): %d", error->status);
        return 0;
    }
    if (service == NULL) {
        /* Fin normal de la búsqueda. Si nunca entramos al branch de abajo,
         * quiere decir que el UUID configurado en obd_driver_config.h no
         * coincide con ningún servicio del adaptador — revisar ahí primero. */
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

/* ATZ (reset), ATE0 (echo off — clave, si no el ELM327 nos devuelve el
 * comando que mandamos antes de la respuesta real), ATL0 (sin saltos de
 * linea extra), ATSP0 (autodetectar protocolo del vehiculo). */
static const char *const s_init_cmds[] = { "ATZ", "ATE0", "ATL0", "ATSP0" };
#define INIT_CMD_COUNT (sizeof(s_init_cmds) / sizeof(s_init_cmds[0]))
static size_t s_init_step;

static void send_next_init_step(void);

static void on_init_step_response(const uint8_t *data, size_t len, void *ctx)
{
    /* No usamos la respuesta en si (solo confirma que el comando anterior
     * fue procesado) antes de mandar el siguiente — evita que las 4
     * respuestas del init se pisen entre si o con el primer poll de PIDs
     * de pid_engine, que arranca apenas obd_driver_is_connected() da true. */
    s_init_step++;
    send_next_init_step();
}

static void send_next_init_step(void)
{
    if (s_init_step >= INIT_CMD_COUNT) {
        s_ready = true; // recien ahora: las 4 respuestas de init ya llegaron
        ESP_LOGI(TAG, "secuencia de init ELM327 completa, listo para PIDs");
        return;
    }
    /* Reusamos el mismo canal de "respuesta pendiente" que obd_driver_send_command,
     * pero sin tomar s_cmd_mutex: en este punto s_ready todavia es false, asi que
     * obd_driver_is_connected() da false y pid_engine no puede llamar a
     * obd_driver_send_command todavia (esa funcion chequea is_connected antes de
     * tocar el mutex) — no hay con quien pisarse. */
    s_pending_cb = on_init_step_response;
    s_pending_ctx = NULL;
    obd_write_raw(s_init_cmds[s_init_step]);
    esp_timer_start_once(s_response_timeout_timer, COMMAND_TIMEOUT_MS * 1000ULL);
}

static void obd_send_init_sequence(void)
{
    ESP_LOGI(TAG, "enviando secuencia de init ELM327...");
    s_init_step = 0;
    send_next_init_step();
}

/* ---------------------------------------------------------------------
 * GAP event handler
 * --------------------------------------------------------------------- */

static int gap_event_handler(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
        case BLE_GAP_EVENT_DISC: {
            /* Filtramos principalmente por el UUID de servicio 0x18F0 (confirmado
             * en el auto con nRF Connect el 19 ago): el vLinker MC+ no manda su
             * nombre en el advertising/scan-response, solo se lee por GATT
             * despues de conectar (asi lo mostraba nRF Connect) — matchear por
             * nombre solo no encontraba nunca el adaptador. Se deja el chequeo
             * por nombre como fallback por si algun otro adaptador OBD si lo
             * anuncia. */
            struct ble_hs_adv_fields fields;
            ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data);

            bool matched_by_uuid = false;
            for (int i = 0; i < fields.num_uuids16; i++) {
                if (fields.uuids16[i].value == OBD_BLE_SERVICE_UUID) {
                    matched_by_uuid = true;
                    break;
                }
            }
            bool matched_by_name = fields.name != NULL &&
                strncmp((char *)fields.name, OBD_BLE_DEVICE_NAME, strlen(OBD_BLE_DEVICE_NAME)) == 0;

            if (matched_by_uuid || matched_by_name) {
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
            /* Si había un comando esperando respuesta cuando se desconectó,
             * liberar el turno acá también — si no, obd_driver_send_command
             * queda bloqueado esperando un mutex que nadie va a soltar hasta
             * el timeout de 3s (funciona igual, pero es innecesariamente lento
             * justo cuando ya sabemos que no va a llegar nada). */
            esp_timer_stop(s_response_timeout_timer);
            if (s_pending_cb != NULL) {
                s_pending_cb = NULL;
                s_pending_ctx = NULL;
                s_response_len = 0;
                xSemaphoreGive(s_cmd_mutex);
            }
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
                esp_timer_stop(s_response_timeout_timer); // llegó a tiempo, cancelar el watchdog
                if (s_pending_cb != NULL) {
                    /* Limpiar los globales ANTES de invocar el callback: si es
                     * on_init_step_response, dispara el siguiente paso del init
                     * y vuelve a setear s_pending_cb desde adentro — si lo
                     * limpiaramos despues, pisariamos ese nuevo valor y la
                     * cadena de init se cortaria en el primer paso. */
                    obd_response_cb_t cb = s_pending_cb;
                    void *ctx = s_pending_ctx;
                    s_pending_cb = NULL;
                    s_pending_ctx = NULL;
                    cb(s_response_buf, s_response_len, ctx);
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
    /* En 0 a proposito: el vLinker manda su UUID de servicio (lo que
     * usamos para matchear) en un paquete que el controlador puede tratar
     * como "duplicado" del mismo address si se filtra — con filtro en 1 el
     * adaptador nunca se encontraba en las pruebas reales del 19 ago. */
    disc_params.filter_duplicates = 0;

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
    /* Semaforo BINARIO, no mutex: se toma en la tarea que llama a
     * obd_driver_send_command pero se libera desde la tarea de NimBLE (al
     * llegar la respuesta por notify) o desde el timer de timeout — un mutex
     * real de FreeRTOS trackea el dueño para priority inheritance y explota
     * (assert xTaskPriorityDisinherit) si se libera desde otra tarea o sin
     * haberse tomado antes. Visto en hardware real el 19 ago al recibir la
     * primera respuesta del vLinker. */
    s_cmd_mutex = xSemaphoreCreateBinary();
    if (s_cmd_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(s_cmd_mutex); // arranca "disponible" (a diferencia de un mutex, el binario arranca tomado)

    const esp_timer_create_args_t timer_args = {
        .callback = &on_response_timeout,
        .name = "obd_resp_timeout",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_response_timeout_timer));

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

    ESP_LOGI(TAG, "obd_driver_init OK — UUIDs confirmados contra el vLinker MC+ "
                   "(servicio 18F0/notify 2AF0/write 2AF1)");
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
    esp_timer_start_once(s_response_timeout_timer, COMMAND_TIMEOUT_MS * 1000ULL);
    /* El mutex se libera en dos lugares posibles ahora: BLE_GAP_EVENT_NOTIFY_RX
     * si llega respuesta a tiempo (que además cancela este timer), o
     * on_response_timeout si no llega — así un comando perdido no deja el
     * driver trabado para siempre. */
    return ESP_OK;
}

bool obd_driver_is_connected(void)
{
    return s_conn_handle != BLE_HS_CONN_HANDLE_NONE && s_ready;
}
