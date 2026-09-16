#include "ELM327_BLE.h"

#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/task.h"

#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_uuid.h"
#include "nimble/nimble_port.h"
#include "host/util/util.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ELM327_BLE";

static elm327_ble_t *s_ble = NULL;
static uint8_t s_own_addr_type = 0;

/* UUIDs are encoded little-endian for NimBLE. */
static const ble_uuid128_t NUS_SERVICE_UUID =
    BLE_UUID128_INIT(
        0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
        0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e
    );

static const ble_uuid128_t NUS_RX_UUID =
    BLE_UUID128_INIT(
        0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
        0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e
    );

static const ble_uuid128_t NUS_TX_UUID =
    BLE_UUID128_INIT(
        0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
        0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e
    );

static void queue_command(bool allow_empty) {
    if (!s_ble || !s_ble->command_queue) return;
    if (s_ble->incoming_length == 0 && !allow_empty) return;

    elm_command_t command = {0};

    if (s_ble->incoming_length > 0) {
        memcpy(
            command.text,
            s_ble->incoming,
            s_ble->incoming_length
        );
    }
    command.text[s_ble->incoming_length] = '\0';

    /* Never block NimBLE host callback. */
    (void)xQueueSend(s_ble->command_queue, &command, 0);
    s_ble->incoming_length = 0;
}

static int gatt_access(
    uint16_t conn_handle,
    uint16_t attr_handle,
    struct ble_gatt_access_ctxt *ctxt,
    void *arg
) {
    (void)conn_handle;
    (void)arg;

    if (!s_ble || attr_handle != s_ble->rx_value_handle) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint16_t packet_len = OS_MBUF_PKTLEN(ctxt->om);
    if (packet_len == 0) {
        queue_command(false);
        return 0;
    }

    uint8_t buffer[ELM_BLE_RX_BUFFER];
    uint16_t copy_len = packet_len;
    if (copy_len > sizeof(buffer)) copy_len = sizeof(buffer);

    if (ble_hs_mbuf_to_flat(ctxt->om, buffer, copy_len, NULL) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    for (uint16_t i = 0; i < copy_len; ++i) {
        char c = (char)buffer[i];

        if (c == '\r' || c == '\n') {
            /*
             * CR always terminates a command. Empty CR repeats the previous
             * ELM command. Ignore an empty LF half of CRLF.
             */
            queue_command(c == '\r');
        } else if (c &&
                   s_ble->incoming_length < ELM_BLE_RX_BUFFER - 1) {
            s_ble->incoming[s_ble->incoming_length++] = c;
        }
    }

    /*
     * Preserve the original behavior: a BLE write boundary also terminates
     * a short ELM command for clients that omit CR/LF.
     */
    queue_command(false);
    return 0;
}

/*
 * ESP-IDF/NimBLE requires val_handle pointers to remain valid. Because the
 * service table above is const, create the same small service table here
 * with handles wired into the runtime context.
 */
static struct ble_gatt_chr_def s_chars[3];
static struct ble_gatt_svc_def s_services[2];

static void build_gatt_table(void) {
    memset(s_chars, 0, sizeof(s_chars));
    memset(s_services, 0, sizeof(s_services));

    s_chars[0].uuid = &NUS_RX_UUID.u;
    s_chars[0].access_cb = gatt_access;
    s_chars[0].flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP;
    s_chars[0].val_handle = &s_ble->rx_value_handle;

    s_chars[1].uuid = &NUS_TX_UUID.u;
    s_chars[1].access_cb = gatt_access;
    s_chars[1].flags = BLE_GATT_CHR_F_NOTIFY;
    s_chars[1].val_handle = &s_ble->tx_value_handle;

    s_services[0].type = BLE_GATT_SVC_TYPE_PRIMARY;
    s_services[0].uuid = &NUS_SERVICE_UUID.u;
    s_services[0].characteristics = s_chars;
}

static void advertise(void);

static int gap_event(
    struct ble_gap_event *event,
    void *arg
) {
    (void)arg;

    if (!s_ble) return 0;

    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                s_ble->connected = true;
                s_ble->conn_handle = event->connect.conn_handle;

                uint16_t mtu = ble_att_mtu(event->connect.conn_handle);
                s_ble->notification_payload =
                    mtu > 3 ? (uint16_t)(mtu - 3) : 20;

                if (s_ble->notification_payload < 20) {
                    s_ble->notification_payload = 20;
                }
                if (s_ble->notification_payload > ELM_BLE_TX_MAX_PAYLOAD) {
                    s_ble->notification_payload = ELM_BLE_TX_MAX_PAYLOAD;
                }

                struct ble_gap_upd_params params = {
                    .itvl_min = 6,
                    .itvl_max = 12,
                    .latency = 0,
                    .supervision_timeout = 200,
                    .min_ce_len = 0,
                    .max_ce_len = 0
                };

                (void)ble_gap_update_params(
                    event->connect.conn_handle,
                    &params
                );
            } else {
                advertise();
            }
            return 0;

        case BLE_GAP_EVENT_DISCONNECT:
            s_ble->connected = false;
            s_ble->conn_handle = BLE_HS_CONN_HANDLE_NONE;
            s_ble->notification_payload = 20;
            s_ble->incoming_length = 0;
            xQueueReset(s_ble->command_queue);
            advertise();
            return 0;

        case BLE_GAP_EVENT_MTU:
            if (event->mtu.conn_handle == s_ble->conn_handle) {
                uint16_t payload =
                    event->mtu.value > 3
                        ? (uint16_t)(event->mtu.value - 3)
                        : 20;

                if (payload < 20) payload = 20;
                if (payload > ELM_BLE_TX_MAX_PAYLOAD) {
                    payload = ELM_BLE_TX_MAX_PAYLOAD;
                }

                s_ble->notification_payload = payload;
            }
            return 0;

        default:
            return 0;
    }
}

static void advertise(void) {
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));

    fields.flags =
        BLE_HS_ADV_F_DISC_GEN |
        BLE_HS_ADV_F_BREDR_UNSUP;

    fields.uuids128 = (ble_uuid128_t *)&NUS_SERVICE_UUID;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    (void)ble_gap_adv_set_fields(&fields);

    const char *name = ble_svc_gap_device_name();
    struct ble_hs_adv_fields rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.name = (uint8_t *)name;
    rsp.name_len = strlen(name);
    rsp.name_is_complete = 1;
    (void)ble_gap_adv_rsp_set_fields(&rsp);

    struct ble_gap_adv_params params;
    memset(&params, 0, sizeof(params));
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    (void)ble_gap_adv_start(
        s_own_addr_type,
        NULL,
        BLE_HS_FOREVER,
        &params,
        gap_event,
        NULL
    );
}

static void on_sync(void) {
    if (ble_hs_util_ensure_addr(0) != 0) {
        ESP_LOGE(TAG, "No usable BLE address");
        return;
    }

    if (ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) {
        ESP_LOGE(TAG, "Failed to infer BLE address type");
        return;
    }

    advertise();
}

static void on_reset(int reason) {
    (void)reason;
    ESP_LOGW(TAG, "NimBLE reset, reason=%d", reason);
}

bool elm327_ble_init(
    elm327_ble_t *ble,
    QueueHandle_t command_queue,
    obd_response_pool_t *response_pool,
    const char *device_name
) {
    if (!ble || !command_queue || !response_pool || !device_name) return false;

    memset(ble, 0, sizeof(*ble));
    ble->command_queue = command_queue;
    ble->response_pool = response_pool;
    ble->conn_handle = BLE_HS_CONN_HANDLE_NONE;
    ble->notification_payload = 20;

    s_ble = ble;

    if (nimble_port_init() != ESP_OK) return false;

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    if (ble_svc_gap_device_name_set(device_name) != 0) return false;

    ble_att_set_preferred_mtu(247);

    build_gatt_table();

    if (ble_gatts_count_cfg(s_services) != 0) return false;
    if (ble_gatts_add_svcs(s_services) != 0) return false;

    return true;
}

void elm327_ble_host_task(void *param) {
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
    vTaskDelete(NULL);
}

void elm327_ble_start_host(void) {
    nimble_port_freertos_init(elm327_ble_host_task);
}

static bool notify_chunk(
    elm327_ble_t *ble,
    const char *data,
    size_t length
) {
    if (!ble || !data || length == 0 || !ble->connected) return false;

    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, length);
    if (!om) return false;

    return ble_gatts_notify_custom(
        ble->conn_handle,
        ble->tx_value_handle,
        om
    ) == 0;
}

static bool emit_byte(
    elm327_ble_t *ble,
    char *chunk,
    size_t chunk_capacity,
    size_t *used,
    char value
) {
    if (!ble || !chunk || !used || chunk_capacity == 0) return false;

    if (*used == chunk_capacity) {
        if (!notify_chunk(ble, chunk, *used)) return false;
        *used = 0;
    }

    chunk[(*used)++] = value;
    return true;
}

static void send_elm_response(
    elm327_ble_t *ble,
    const obd_response_t *response
) {
    if (!ble || !response || !ble->connected) return;

    /* Capture MTU-derived payload once so every chunk in this response matches. */
    size_t chunk_capacity = ble->notification_payload;
    if (chunk_capacity < 20) chunk_capacity = 20;
    if (chunk_capacity > ELM_BLE_TX_MAX_PAYLOAD) {
        chunk_capacity = ELM_BLE_TX_MAX_PAYLOAD;
    }

    char chunk[ELM_BLE_TX_MAX_PAYLOAD];
    size_t used = 0;
    const char *source = response->text;

    for (size_t i = 0; source[i]; ++i) {
        if (source[i] == '\r' || source[i] == '\n') {
            if (!emit_byte(ble, chunk, chunk_capacity, &used, '\r')) return;
            if (response->line_feeds &&
                !emit_byte(ble, chunk, chunk_capacity, &used, '\n')) return;

            if (source[i] == '\r' && source[i + 1] == '\n') ++i;
        } else if (!emit_byte(
                       ble, chunk, chunk_capacity, &used, source[i])) {
            return;
        }
    }

    if (!emit_byte(ble, chunk, chunk_capacity, &used, '\r')) return;
    if (response->line_feeds &&
        !emit_byte(ble, chunk, chunk_capacity, &used, '\n')) return;
    if (!emit_byte(ble, chunk, chunk_capacity, &used, '>')) return;

    if (used) (void)notify_chunk(ble, chunk, used);
}

void elm327_ble_tx_task(void *param) {
    elm327_ble_t *ble = (elm327_ble_t *)param;
    obd_response_pool_t *pool = ble ? ble->response_pool : NULL;
    uint8_t slot = 0;

    if (!pool) {
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        if (xQueueReceive(pool->ready_slots, &slot, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (slot < pool->slot_count) {
            send_elm_response(ble, &pool->slots[slot]);
        }

        /* Always recycle the slot, even when disconnected or notify fails. */
        (void)xQueueSend(pool->free_slots, &slot, portMAX_DELAY);
    }
}

bool elm327_ble_is_connected(const elm327_ble_t *ble) {
    return ble && ble->connected;
}
