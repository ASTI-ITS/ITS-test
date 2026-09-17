/*
 * OBD_BLE_ONE_FILE.c
 *
 * ESP32-S3 + MCP2515 + NimBLE, single-file ESP-IDF application.
 *
 * Features
 * --------
 * - MCP2515, 8 MHz crystal, EXACT 500 kbit/s timing requested by user:
 *       CNF1 = 0x00
 *       CNF2 = 0x91
 *       CNF3 = 0x01
 * - NO MCP2515 CAN acceptance filtering. RXB0/RXB1 accept ALL CAN frames.
 * - Standard and extended CAN reception.
 * - NUS-compatible BLE service for phone applications.
 * - ELM327-style commands: ATZ, ATI, ATSP6, ATCAF0/1, ATH0/1, ATS0/1,
 *   ATMA, ATMT, 0100, 010C, 010D, 0105, 0111, 03, 04, 0902.
 * - ATMA streams every received raw CAN frame as ASCII.
 * - Single-frame OBD-II request/response and ISO-TP multi-frame receive.
 * - CAN RX is owned by one high-priority FreeRTOS task; BLE callback only
 *   copies commands into a queue.
 * - Static queues/buffers; no malloc in the CAN receive path.
 * - BLE notifications are fragmented to the negotiated ATT payload size.
 *
 * Hardware
 * --------
 * XIAO ESP32-S3:
 *   CS   GPIO5
 *   SCK  GPIO7
 *   MISO GPIO8
 *   MOSI GPIO9
 *
 * Build as main/main.c in an ESP-IDF project. CMake component requirements:
 *   bt, esp_driver_spi, nvs_flash
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

...............this is the error...........#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_att.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

/* ========================= USER CONFIG ========================= */

#define TAG                     "OBD_BLE"

#define MCP2515_CS_PIN          5
#define MCP2515_SCK_PIN         7
#define MCP2515_MISO_PIN        8
#define MCP2515_MOSI_PIN        9
#define MCP2515_OSC_HZ          8000000UL

/* Exact requested 8 MHz / 500 kbps MCP2515 timing. */
#define MCP_CNF1_500K           0x00
#define MCP_CNF2_500K           0x91
#define MCP_CNF3_500K           0x01

#define CAN_RX_QUEUE_LEN        256
#define CMD_QUEUE_LEN           16
#define BLE_TX_QUEUE_LEN        16
#define BLE_TX_MAX              1024
#define CMD_MAX                 128

#define OBD_REQUEST_ID          0x7DF
#define OBD_FIRST_RESPONSE_ID   0x7E8
#define OBD_LAST_RESPONSE_ID    0x7EF

#define OBD_TIMEOUT_MS          1000
#define BLE_MTU_DEFAULT         23
#define BLE_MAX_ATT_PAYLOAD     244

/* ========================= MCP2515 REGISTERS ========================= */

#define MCP_RESET               0xC0
#define MCP_READ                0x03
#define MCP_WRITE               0x02
#define MCP_BIT_MODIFY          0x05
#define MCP_READ_STATUS         0xA0
#define MCP_RTS_TX0             0x81

#define MCP_CANSTAT             0x0E
#define MCP_CANCTRL             0x0F
#define MCP_CNF3                0x28
#define MCP_CNF2                0x29
#define MCP_CNF1                0x2A
#define MCP_CANINTE             0x2B
#define MCP_CANINTF             0x2C
#define MCP_EFLG                0x2D

#define MCP_TXB0CTRL            0x30
#define MCP_TXB0SIDH            0x31
#define MCP_TXB0SIDL            0x32
#define MCP_TXB0EID8            0x33
#define MCP_TXB0EID0            0x34
#define MCP_TXB0DLC             0x35
#define MCP_TXB0D0              0x36

#define MCP_RXB0CTRL            0x60
#define MCP_RXB0SIDH            0x61
#define MCP_RXB1CTRL            0x70
#define MCP_RXB1SIDH            0x71

#define MCP_CANCTRL_REQOP_MASK  0xE0
#define MCP_MODE_CONFIG         0x80
#define MCP_MODE_NORMAL         0x00

#define MCP_CANINTF_RX0IF       0x01
#define MCP_CANINTF_RX1IF       0x02
#define MCP_CANINTF_TX0IF       0x04
#define MCP_CANINTF_ERRIF       0x20

#define MCP_TXB_TXREQ           0x08
#define MCP_EFLG_TXBO           0x20
#define MCP_EFLG_TXEP           0x10
#define MCP_EFLG_TXWAR          0x08

#define CAN_ID_EFF_FLAG         0x80000000UL
#define CAN_ID_RTR_FLAG         0x40000000UL
#define CAN_ID_MASK             0x1FFFFFFFUL

/* ========================= BLE NUS UUIDS ========================= */

static const ble_uuid128_t NUS_SERVICE_UUID =
    BLE_UUID128_INIT(
        0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
        0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);

static const ble_uuid128_t NUS_RX_UUID =
    BLE_UUID128_INIT(
        0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
        0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);

static const ble_uuid128_t NUS_TX_UUID =
    BLE_UUID128_INIT(
        0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
        0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

/* ========================= DATA TYPES ========================= */

typedef struct {
    uint32_t id;
    uint8_t dlc;
    uint8_t data[8];
} can_frame_t;

typedef struct {
    char text[CMD_MAX];
} command_t;

typedef struct {
    uint16_t len;
    uint8_t data[BLE_TX_MAX];
} ble_tx_packet_t;

/* ========================= GLOBALS ========================= */

static spi_device_handle_t s_spi;
static QueueHandle_t s_can_rx_queue;
static QueueHandle_t s_cmd_queue;
static QueueHandle_t s_ble_tx_queue;

/* Static queues: no heap allocation or fragmentation in the data path. */
static StaticQueue_t s_can_rx_q_storage;
static uint8_t s_can_rx_q_buffer[CAN_RX_QUEUE_LEN * sizeof(can_frame_t)];
static StaticQueue_t s_cmd_q_storage;
static uint8_t s_cmd_q_buffer[CMD_QUEUE_LEN * sizeof(command_t)];
static StaticQueue_t s_ble_tx_q_storage;
static uint8_t s_ble_tx_q_buffer[BLE_TX_QUEUE_LEN * sizeof(ble_tx_packet_t)];

static volatile uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_notify_enabled = false;
static volatile uint16_t s_att_payload = BLE_MTU_DEFAULT - 3;
static volatile bool s_raw_monitor = false;
static volatile bool s_raw_timestamp = false;

static uint8_t s_cmd_accum[CMD_MAX];
static size_t s_cmd_accum_len;

/* ========================= SMALL HELPERS ========================= */

static void str_upper_trim(char *s)
{
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) {
        s[--n] = '\0';
    }
    size_t start = 0;
    while (s[start] && isspace((unsigned char)s[start])) start++;
    if (start) memmove(s, s + start, strlen(s + start) + 1);
    for (size_t i = 0; s[i]; i++) {
        s[i] = (char)toupper((unsigned char)s[i]);
    }
}

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void append_hex(char *out, size_t cap, size_t *pos, uint8_t v)
{
    if (*pos + 2 >= cap) return;
    int n = snprintf(out + *pos, cap - *pos, "%02X", v);
    if (n > 0) *pos += (size_t)n;
}

/* ========================= SPI / MCP2515 ========================= */

static esp_err_t spi_cmd(const uint8_t *tx, uint8_t *rx, size_t n)
{
    spi_transaction_t t = {0};
    t.length = n * 8;
    t.tx_buffer = tx;
    t.rx_buffer = rx;
    return spi_device_transmit(s_spi, &t);
}

static bool mcp_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t tx[3] = {MCP_WRITE, reg, value};
    return spi_cmd(tx, NULL, sizeof(tx)) == ESP_OK;
}

static bool mcp_write_regs(uint8_t reg, const uint8_t *data, uint8_t len)
{
    uint8_t tx[16];
    if ((size_t)len + 2 > sizeof(tx)) return false;
    tx[0] = MCP_WRITE;
    tx[1] = reg;
    memcpy(&tx[2], data, len);
    return spi_cmd(tx, NULL, (size_t)len + 2) == ESP_OK;
}

static uint8_t mcp_read_reg(uint8_t reg)
{
    uint8_t tx[3] = {MCP_READ, reg, 0};
    uint8_t rx[3] = {0};
    if (spi_cmd(tx, rx, sizeof(tx)) != ESP_OK) return 0;
    return rx[2];
}

static bool mcp_read_regs(uint8_t reg, uint8_t *data, uint8_t len)
{
    uint8_t tx[16] = {0};
    uint8_t rx[16] = {0};
    if ((size_t)len + 2 > sizeof(tx)) return false;
    tx[0] = MCP_READ;
    tx[1] = reg;
    if (spi_cmd(tx, rx, (size_t)len + 2) != ESP_OK) return false;
    memcpy(data, &rx[2], len);
    return true;
}

static bool mcp_bit_modify(uint8_t reg, uint8_t mask, uint8_t value)
{
    uint8_t tx[4] = {MCP_BIT_MODIFY, reg, mask, value};
    return spi_cmd(tx, NULL, sizeof(tx)) == ESP_OK;
}

static bool mcp_reset(void)
{
    uint8_t tx = MCP_RESET;
    return spi_cmd(&tx, NULL, 1) == ESP_OK;
}

static bool mcp_set_mode(uint8_t mode)
{
    if (!mcp_bit_modify(MCP_CANCTRL, MCP_CANCTRL_REQOP_MASK, mode)) {
        return false;
    }

    for (int i = 0; i < 20; i++) {
        if ((mcp_read_reg(MCP_CANSTAT) & MCP_CANCTRL_REQOP_MASK) == mode) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return false;
}

static bool mcp_init(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num = MCP2515_MOSI_PIN,
        .miso_io_num = MCP2515_MISO_PIN,
        .sclk_io_num = MCP2515_SCK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 64,
    };

    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_DISABLED);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(err));
        return false;
    }

    spi_device_interface_config_t dev = {
        .clock_speed_hz = 8000000,
        .mode = 0,
        .spics_io_num = MCP2515_CS_PIN,
        .queue_size = 1,
        .flags = SPI_DEVICE_NO_DUMMY,
    };

    err = spi_bus_add_device(SPI2_HOST, &dev, &s_spi);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "SPI device init failed: %s", esp_err_to_name(err));
        return false;
    }

    if (!mcp_reset()) {
        ESP_LOGE(TAG, "MCP2515 reset failed");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(5));

    if (!mcp_set_mode(MCP_MODE_CONFIG)) {
        ESP_LOGE(TAG, "MCP2515 config mode failed");
        return false;
    }

    /* EXACT USER REQUEST: 8 MHz / 500 kbps. */
    if (!mcp_write_reg(MCP_CNF1, MCP_CNF1_500K) ||
        !mcp_write_reg(MCP_CNF2, MCP_CNF2_500K) ||
        !mcp_write_reg(MCP_CNF3, MCP_CNF3_500K)) {
        ESP_LOGE(TAG, "CAN timing setup failed");
        return false;
    }

    /*
     * NO FILTERING:
     * RXM1:RXM0 = 11 => receive any message, bypass filters/masks.
     * RXB0 BUKT = 1 => rollover RXB0 -> RXB1.
     */
    if (!mcp_write_reg(MCP_RXB0CTRL, 0x64) ||
        !mcp_write_reg(MCP_RXB1CTRL, 0x60)) {
        ESP_LOGE(TAG, "CAN receive mode setup failed");
        return false;
    }

    /* Clear stale interrupt/error flags and enable RX interrupts. */
    if (!mcp_write_reg(MCP_CANINTF, 0x00) ||
        !mcp_write_reg(MCP_CANINTE, MCP_CANINTF_RX0IF | MCP_CANINTF_RX1IF)) {
        return false;
    }

    if (!mcp_set_mode(MCP_MODE_NORMAL)) {
        ESP_LOGE(TAG, "MCP2515 normal mode failed");
        return false;
    }

    ESP_LOGI(TAG, "MCP2515: 8 MHz / 500 kbps / RX ALL");
    return true;
}

static bool mcp_read_rx_buffer(uint8_t start, can_frame_t *f)
{
    uint8_t r[13] = {0};
    if (!mcp_read_regs(start, r, sizeof(r))) return false;

    uint16_t sid = ((uint16_t)r[0] << 3) | (r[1] >> 5);
    bool ext = (r[1] & 0x08) != 0;
    bool rtr = (r[1] & 0x10) != 0 || (r[5] & 0x40) != 0;

    uint32_t id;
    if (!ext) {
        id = sid;
    } else {
        uint32_t eid = ((uint32_t)(r[1] & 0x03) << 16) |
                       ((uint32_t)r[2] << 8) |
                       r[3];
        id = ((uint32_t)sid << 18) | eid | CAN_ID_EFF_FLAG;
    }

    if (rtr) id |= CAN_ID_RTR_FLAG;

    f->id = id;
    f->dlc = r[4] & 0x0F;
    if (f->dlc > 8) f->dlc = 8;
    memcpy(f->data, &r[5], 8);
    return true;
}

static bool mcp_receive_one(can_frame_t *f)
{
    uint8_t flags = mcp_read_reg(MCP_CANINTF);

    if (flags & MCP_CANINTF_RX0IF) {
        bool ok = mcp_read_rx_buffer(MCP_RXB0SIDH, f);
        (void)mcp_bit_modify(MCP_CANINTF, MCP_CANINTF_RX0IF, 0);
        return ok;
    }

    if (flags & MCP_CANINTF_RX1IF) {
        bool ok = mcp_read_rx_buffer(MCP_RXB1SIDH, f);
        (void)mcp_bit_modify(MCP_CANINTF, MCP_CANINTF_RX1IF, 0);
        return ok;
    }

    return false;
}

static void mcp_drain_rx(void)
{
    can_frame_t f;

    /* Bound one pass so a stuck RX flag cannot starve BLE or the idle task. */
    for (int i = 0; i < 8 && mcp_receive_one(&f); i++) {
        (void)xQueueSend(s_can_rx_queue, &f, 0);
    }
}

static bool mcp_send_frame(uint32_t id, const uint8_t *data, uint8_t dlc)
{
    if (dlc > 8) dlc = 8;

    /* TXB0 is the only transmitter; wait until available. */
    for (int i = 0; i < 20; i++) {
        if (!(mcp_read_reg(MCP_TXB0CTRL) & MCP_TXB_TXREQ)) break;
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    if (mcp_read_reg(MCP_TXB0CTRL) & MCP_TXB_TXREQ) {
        return false;
    }

    uint8_t r[13] = {0};
    uint32_t raw = id & CAN_ID_MASK;
    bool ext = (id & CAN_ID_EFF_FLAG) != 0;
    bool rtr = (id & CAN_ID_RTR_FLAG) != 0;

    if (!ext) {
        r[0] = (uint8_t)(raw >> 3);
        r[1] = (uint8_t)((raw & 0x07) << 5);
    } else {
        uint16_t sid = (uint16_t)(raw >> 18);
        uint32_t eid = raw & 0x3FFFFUL;
        r[0] = (uint8_t)(sid >> 3);
        r[1] = (uint8_t)((sid & 0x07) << 5) | 0x08 |
                (uint8_t)(eid >> 16);
        r[2] = (uint8_t)(eid >> 8);
        r[3] = (uint8_t)eid;
    }

    r[4] = dlc | (rtr ? 0x40 : 0x00);
    memcpy(&r[5], data, dlc);

    if (!mcp_write_regs(MCP_TXB0SIDH, r, 13)) return false;

    uint8_t cmd = MCP_RTS_TX0;
    if (spi_cmd(&cmd, NULL, 1) != ESP_OK) return false;

    /* A missing ECU or CAN wiring fault leaves TXREQ set while the MCP2515
     * retries forever. Bound the wait so the BLE client receives a useful
     * error instead of waiting for the OBD timeout. */
    for (int i = 0; i < 100; i++) {
        uint8_t tx_status = mcp_read_reg(MCP_TXB0CTRL);
        if (!(tx_status & MCP_TXB_TXREQ)) return true;
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    uint8_t error_flags = mcp_read_reg(MCP_EFLG);
    ESP_LOGW(TAG, "CAN transmit timeout: EFLG=0x%02X%s%s%s",
             error_flags,
             (error_flags & MCP_EFLG_TXBO) ? " TXBUSOFF" : "",
             (error_flags & MCP_EFLG_TXEP) ? " TXERROR" : "",
             (error_flags & MCP_EFLG_TXWAR) ? " TXWARN" : "");
    return false;
}

/* ========================= BLE ========================= */

static uint16_t tx_val_handle;
static uint8_t own_addr_type;

static int gatt_access_cb(uint16_t conn_handle,
                          uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt,
                          void *arg);

static int gap_event(struct ble_gap_event *event, void *arg);

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &NUS_SERVICE_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &NUS_TX_UUID.u,
                .access_cb = gatt_access_cb,
                .val_handle = &tx_val_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY,
            },
            {
                .uuid = &NUS_RX_UUID.u,
                .access_cb = gatt_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {0}
        },
    },
    {0}
};

static void ble_advertise(void)
{
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));

    const char *name = "OBDII";
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = (ble_uuid128_t *)&NUS_SERVICE_UUID;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv fields rc=%d", rc);
        return;
    }

    struct ble_gap_adv_params params;
    memset(&params, 0, sizeof(params));
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER,
                           &params, gap_event, NULL);
    if (rc != 0) ESP_LOGE(TAG, "adv start rc=%d", rc);
}

static int gatt_access_cb(uint16_t conn_handle,
                          uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt,
                          void *arg)
{
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    size_t total = OS_MBUF_PKTLEN(ctxt->om);
    if (total == 0) return 0;

    if (total > CMD_MAX - 1) total = CMD_MAX - 1;

    command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    int rc = ble_hs_mbuf_to_flat(ctxt->om, cmd.text, total, NULL);
    if (rc != 0) return BLE_ATT_ERR_UNLIKELY;
    cmd.text[total] = '\0';

    /* Split command stream by CR/LF. */
    for (size_t i = 0; i < total; i++) {
        uint8_t c = (uint8_t)cmd.text[i];
        if (c == '\r' || c == '\n') {
            if (s_cmd_accum_len) {
                command_t q = {0};
                memcpy(q.text, s_cmd_accum, s_cmd_accum_len);
                q.text[s_cmd_accum_len] = '\0';
                (void)xQueueSend(s_cmd_queue, &q, 0);
                s_cmd_accum_len = 0;
            }
        } else if (s_cmd_accum_len < CMD_MAX - 1) {
            s_cmd_accum[s_cmd_accum_len++] = c;
        }
    }

    /* Some phone BLE terminals omit CR/LF.  Submit complete short ELM
     * commands immediately; commands sent with CR/LF use the normal path. */
    if (s_cmd_accum_len && s_cmd_accum_len < CMD_MAX - 1) {
        bool complete = false;

        if (s_cmd_accum_len >= 2 && s_cmd_accum[0] == 'A' &&
            s_cmd_accum[1] == 'T') {
            complete = (s_cmd_accum_len >= 3);
        } else if ((s_cmd_accum_len % 2) == 0 &&
                   s_cmd_accum_len >= 2 &&
                   s_cmd_accum_len <= 16) {
            complete = true;
            for (size_t i = 0; i < s_cmd_accum_len; i++) {
                if (!isxdigit((unsigned char)s_cmd_accum[i])) {
                    complete = false;
                    break;
                }
            }
        }

        if (complete) {
            command_t q = {0};
            memcpy(q.text, s_cmd_accum, s_cmd_accum_len);
            q.text[s_cmd_accum_len] = '\0';
            if (xQueueSend(s_cmd_queue, &q, 0) == pdTRUE) {
                s_cmd_accum_len = 0;
            }
        }
    }

    (void)conn_handle;
    return 0;
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_notify_enabled = false;

            /* Prefer a short connection interval for lower command/notification
             * latency. The phone remains free to accept or reject this request. */
            struct ble_gap_upd_params params;
            memset(&params, 0, sizeof(params));
            params.itvl_min = 6;   /* 7.5 ms */
            params.itvl_max = 12;  /* 15 ms */
            params.latency = 0;
            params.supervision_timeout = 400; /* 4 s */
            (void)ble_gap_update_params(event->connect.conn_handle, &params);

            ESP_LOGI(TAG, "BLE connected");
        } else {
            ble_advertise();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_notify_enabled = false;
        ESP_LOGI(TAG, "BLE disconnected");
        ble_advertise();
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == tx_val_handle) {
            s_notify_enabled = event->subscribe.cur_notify != 0;
        }
        break;

    case BLE_GAP_EVENT_MTU:
        if (event->mtu.value >= 23) {
            uint16_t mtu = event->mtu.value;
            if (mtu > 247) mtu = 247;
            s_att_payload = mtu - 3;
            if (s_att_payload > BLE_MAX_ATT_PAYLOAD)
                s_att_payload = BLE_MAX_ATT_PAYLOAD;
        }
        break;

    case BLE_GAP_EVENT_CONN_UPDATE:
        break;

    default:
        break;
    }
    return 0;
}

static void ble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void ble_sync(void)
{
    if (ble_hs_util_ensure_addr(0) != 0) {
        ESP_LOGE(TAG, "BLE address setup failed");
        return;
    }

    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto rc=%d", rc);
        return;
    }

    ble_advertise();
}

static bool ble_init(void)
{
    /* nimble_port_init owns controller, HCI, and host initialization. */
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE initialization failed: %s (%d)",
                 esp_err_to_name(err), (int)err);
        return false;
    }

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set("OBDII");

    int rc = ble_gatts_count_cfg(gatt_svcs);
    if (rc != 0) return false;

    rc = ble_gatts_add_svcs(gatt_svcs);
    if (rc != 0) return false;

    ble_hs_cfg.sync_cb = ble_sync;

    /* No pairing/passkey is required for a serial-style scanner. */
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_mitm = 0;

    /* Prefer 247-byte ATT MTU. Phones that only support 23-byte MTU still
     * work; s_att_payload is updated from the actual negotiated MTU. */
    (void)ble_att_set_preferred_mtu(247);

    nimble_port_freertos_init(ble_host_task);
    return true;
}

static void ble_send_raw(const uint8_t *data, size_t len)
{
    if (!s_notify_enabled || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;

    while (len) {
        size_t n = len;
        if (n > s_att_payload) n = s_att_payload;

        struct os_mbuf *om = ble_hs_mbuf_from_flat(data, n);
        if (!om) return;

        int rc = ble_gatts_notify_custom(s_conn_handle, tx_val_handle, om);
        if (rc != 0) return;

        data += n;
        len -= n;
    }
}

static void ble_queue_text(const char *text)
{
    if (!text) return;

    ble_tx_packet_t p;
    memset(&p, 0, sizeof(p));
    size_t n = strlen(text);
    if (n > BLE_TX_MAX) n = BLE_TX_MAX;
    memcpy(p.data, text, n);
    p.len = (uint16_t)n;
    (void)xQueueSend(s_ble_tx_queue, &p, 0);
}

static void ble_tx_task(void *arg)
{
    (void)arg;
    ble_tx_packet_t p;

    while (1) {
        if (xQueueReceive(s_ble_tx_queue, &p, portMAX_DELAY) == pdTRUE) {
            ble_send_raw(p.data, p.len);
        }
    }
}

/* ========================= ELM / OBD ========================= */

static bool id_is_obd_response(uint32_t id)
{
    uint32_t v = id & CAN_ID_MASK;
    if (id & CAN_ID_EFF_FLAG) return false;
    return v >= OBD_FIRST_RESPONSE_ID && v <= OBD_LAST_RESPONSE_ID;
}

static void format_can_ascii(const can_frame_t *f, char *out, size_t cap)
{
    size_t p = 0;
    uint32_t id = f->id & CAN_ID_MASK;

    if (f->id & CAN_ID_EFF_FLAG)
        p += (size_t)snprintf(out + p, cap - p, "%08lX", (unsigned long)id);
    else
        p += (size_t)snprintf(out + p, cap - p, "%03lX", (unsigned long)id);

    if (p < cap) p += (size_t)snprintf(out + p, cap - p, " %u", f->dlc);

    for (uint8_t i = 0; i < f->dlc && p < cap; i++) {
        if (p + 3 >= cap) break;
        p += (size_t)snprintf(out + p, cap - p, " %02X", f->data[i]);
    }

    if (p + 2 < cap) {
        out[p++] = '\r';
        out[p++] = '\r';
        out[p] = '\0';
    }
}

static void reply_prompt(const char *payload)
{
    char out[BLE_TX_MAX];
    int n = snprintf(out, sizeof(out), "%s\r\r>", payload ? payload : "");
    if (n > 0) ble_queue_text(out);
}

static bool hex_byte(const char *s, uint8_t *v)
{
    unsigned int x;
    if (sscanf(s, "%2x", &x) != 1 || x > 255) return false;
    *v = (uint8_t)x;
    return true;
}

static bool send_obd_request(uint8_t mode, uint8_t pid)
{
    uint8_t d[8] = {0};
    d[0] = 0x02;
    d[1] = mode;
    d[2] = pid;
    return mcp_send_frame(OBD_REQUEST_ID, d, 8);
}

static bool collect_obd_response(uint8_t mode, uint8_t pid,
                                 char *out, size_t cap)
{
    can_frame_t f;
    uint32_t deadline = now_ms() + OBD_TIMEOUT_MS;
    uint8_t iso[512];
    size_t iso_len = 0;
    size_t expected = 0;
    bool started = false;

    while ((int32_t)(deadline - now_ms()) > 0) {
        /* Keep draining MCP2515 while waiting; the CAN task remains the only
         * owner of the SPI device, so no mutex is needed. */
        mcp_drain_rx();

        while (xQueueReceive(s_can_rx_queue, &f, 0) == pdTRUE) {
            if (!id_is_obd_response(f.id)) continue;
            if (f.dlc == 0) continue;

            uint8_t pci_type = f.data[0] >> 4;

            if (pci_type == 0) {
                uint8_t len = f.data[0] & 0x0F;
                if (len > 7 || len + 1 > f.dlc) continue;

                if (len >= 2 && f.data[1] == (uint8_t)(mode + 0x40)) {
                    if (mode == 0x01 && len >= 2 && f.data[2] == pid) {
                        size_t p = 0;
                        for (uint8_t i = 1; i <= len && p + 3 < cap; i++) {
                            append_hex(out, cap, &p, f.data[i]);
                        }
                        out[p] = '\0';
                        return true;
                    }

                    size_t p = 0;
                    for (uint8_t i = 1; i <= len && p + 3 < cap; i++)
                        append_hex(out, cap, &p, f.data[i]);
                    out[p] = '\0';
                    return true;
                }
            } else if (pci_type == 1) {
                expected = ((size_t)(f.data[0] & 0x0F) << 8) | f.data[1];
                if (expected > sizeof(iso)) expected = sizeof(iso);
                iso_len = 0;
                size_t first = f.dlc - 2;
                if (first > expected) first = expected;
                memcpy(iso, &f.data[2], first);
                iso_len = first;
                started = true;

                /* Flow Control: continue receiving ISO-TP response. */
                uint8_t fc[8] = {0x30, 0x00, 0x00};
                (void)mcp_send_frame(0x7E0, fc, 8);
            } else if (pci_type == 2 && started) {
                size_t n = f.dlc - 1;
                if (iso_len + n > expected) n = expected - iso_len;
                if (n) {
                    memcpy(&iso[iso_len], &f.data[1], n);
                    iso_len += n;
                }
                if (iso_len >= expected) {
                    size_t p = 0;
                    for (size_t i = 0; i < iso_len && p + 3 < cap; i++)
                        append_hex(out, cap, &p, iso[i]);
                    out[p] = '\0';
                    return true;
                }
            }
        }
    }

    return false;
}

static void process_command(char *cmd)
{
    str_upper_trim(cmd);
    if (!cmd[0]) return;

    if (!strcmp(cmd, "ATZ") || !strcmp(cmd, "ATWS")) {
        s_raw_monitor = false;
        s_raw_timestamp = false;
        reply_prompt("ELM327 v1.5");
        return;
    }

    if (!strcmp(cmd, "ATI")) {
        reply_prompt("ELM327");
        return;
    }

    if (!strcmp(cmd, "ATSP6")) {
        reply_prompt("OK");
        return;
    }

    if (!strcmp(cmd, "ATD")) {
        s_raw_monitor = false;
        reply_prompt("OK");
        return;
    }

    if (!strcmp(cmd, "ATMA")) {
        s_raw_monitor = true;
        ble_queue_text("> ");
        return;
    }

    if (!strcmp(cmd, "ATMT")) {
        s_raw_monitor = false;
        reply_prompt("OK");
        return;
    }

    if (!strcmp(cmd, "ATH1")) {
        reply_prompt("OK");
        return;
    }

    if (!strcmp(cmd, "ATH0")) {
        reply_prompt("OK");
        return;
    }

    if (!strcmp(cmd, "ATS1") || !strcmp(cmd, "ATS0") ||
        !strcmp(cmd, "ATCAF1") || !strcmp(cmd, "ATCAF0")) {
        reply_prompt("OK");
        return;
    }

    if (!strcmp(cmd, "ATDP")) {
        reply_prompt("ISO 15765-4 (CAN 11/500)");
        return;
    }

    if (!strcmp(cmd, "ATSP0")) {
        reply_prompt("OK");
        return;
    }

    if (!strncmp(cmd, "ATSH", 4)) {
        reply_prompt("OK");
        return;
    }

    /* Raw CAN transmit: ATCS <hexid> <dlc> <bytes...> */
    if (!strncmp(cmd, "ATCS ", 5)) {
        unsigned long id;
        unsigned int dlc;
        char *p = cmd + 5;
        if (sscanf(p, "%lx %u", &id, &dlc) == 2 && dlc <= 8) {
            char *sp = strchr(p, ' ');
            if (sp) sp = strchr(sp + 1, ' ');
            uint8_t data[8] = {0};
            bool ok = true;
            if (sp) {
                sp++;
                for (unsigned int i = 0; i < dlc; i++) {
                    if (!hex_byte(sp + i * 3, &data[i])) {
                        ok = false;
                        break;
                    }
                }
            }
            if (ok && mcp_send_frame((uint32_t)id, data, (uint8_t)dlc)) {
                reply_prompt("OK");
                return;
            }
        }
        reply_prompt("ERROR");
        return;
    }

    /* Standard OBD request, e.g. 010C, 010D, 03, 0902. */
    size_t n = strlen(cmd);
    if (n >= 2 && n <= 16 && (n % 2) == 0) {
        uint8_t bytes[8] = {0};
        size_t nb = n / 2;
        bool ok = nb <= 8;
        for (size_t i = 0; ok && i < nb; i++) {
            ok = hex_byte(cmd + i * 2, &bytes[i]);
        }

        if (ok && nb >= 1) {
            uint8_t tx[8] = {0};
            tx[0] = (uint8_t)nb;
            memcpy(&tx[1], bytes, nb);

            if (!mcp_send_frame(OBD_REQUEST_ID, tx, 8)) {
                reply_prompt("CAN ERROR");
                return;
            }

            char response[512] = {0};
            if (collect_obd_response(bytes[0],
                                      nb > 1 ? bytes[1] : 0,
                                      response, sizeof(response))) {
                reply_prompt(response);
            } else {
                reply_prompt("NO DATA");
            }
            return;
        }
    }

    reply_prompt("?");
}

/* ========================= CAN TASK ========================= */

static void can_task(void *arg)
{
    (void)arg;
    command_t cmd;
    can_frame_t f;

    while (1) {
        /* Highest priority work: drain the MCP2515 RX buffers. */
        mcp_drain_rx();

        /* Raw monitor consumes every accepted CAN frame.  When monitor mode
         * is off, drain the software queue as well so a busy vehicle bus can
         * never back-pressure the MCP2515 receive path. Hardware filtering is
         * still disabled; ATMA is the mode that exposes every frame to BLE. */
        while (xQueueReceive(s_can_rx_queue, &f, 0) == pdTRUE) {
            if (s_raw_monitor) {
                char line[96];
                format_can_ascii(&f, line, sizeof(line));
                ble_queue_text(line);
            }
        }

        if (xQueueReceive(s_cmd_queue, &cmd, 0) == pdTRUE) {
            process_command(cmd.text);
            continue;
        }

        /* Let the idle task and BLE stack run when the CAN queue is empty. */
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

/* ========================= MAIN ========================= */

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }

    s_can_rx_queue = xQueueCreateStatic(
        CAN_RX_QUEUE_LEN,
        sizeof(can_frame_t),
        s_can_rx_q_buffer,
        &s_can_rx_q_storage
    );

    s_cmd_queue = xQueueCreateStatic(
        CMD_QUEUE_LEN,
        sizeof(command_t),
        s_cmd_q_buffer,
        &s_cmd_q_storage
    );

    s_ble_tx_queue = xQueueCreateStatic(
        BLE_TX_QUEUE_LEN,
        sizeof(ble_tx_packet_t),
        s_ble_tx_q_buffer,
        &s_ble_tx_q_storage
    );

    if (!s_can_rx_queue || !s_cmd_queue || !s_ble_tx_queue) {
        ESP_LOGE(TAG, "Queue creation failed");
        return;
    }

    if (!mcp_init()) {
        ESP_LOGE(TAG, "MCP2515 initialization failed");
        return;
    }

    if (!ble_init()) {
        ESP_LOGE(TAG, "BLE initialization failed");
        return;
    }

    if (xTaskCreatePinnedToCore(can_task, "CAN", 8192, NULL, 5, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "CAN task creation failed");
        return;
    }

    if (xTaskCreatePinnedToCore(ble_tx_task, "BLE_TX", 4096, NULL, 8, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "BLE TX task creation failed");
        return;
    }

    ESP_LOGI(TAG, "OBDII ready: MCP2515 8MHz / 500kbps / RX ALL");
}
