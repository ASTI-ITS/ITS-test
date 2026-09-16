#include "MCP2515Transport.h"

#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* MCP2515 SPI commands */
#define MCP_CMD_RESET           0xC0
#define MCP_CMD_READ            0x03
#define MCP_CMD_WRITE           0x02
#define MCP_CMD_BIT_MODIFY      0x05
#define MCP_CMD_RTS_TX0         0x81

/* MCP2515 registers */
#define MCP_CANSTAT             0x0E
#define MCP_CANCTRL             0x0F
#define MCP_CNF3                0x28
#define MCP_CNF2                0x29

#define MCP_CNF1                0x2A
#define MCP_CANINTE             0x2B
#define MCP_CANINTF             0x2C

#define MCP_TXB0CTRL            0x30
#define MCP_TXB0SIDH            0x31

#define MCP_RXB0CTRL            0x60
#define MCP_RXB0SIDH            0x61
#define MCP_RXB1CTRL            0x70
#define MCP_RXB1SIDH            0x71

#define MCP_CANINTF_RX0IF       0x01
#define MCP_CANINTF_RX1IF       0x02

#define MCP_TXREQ               0x08
#define MCP_TXERR               0x10
#define MCP_MLOA                0x20
#define MCP_ABTF                0x40

#define MCP_MODE_MASK           0xE0
#define MCP_MODE_CONFIG         0x80
#define MCP_MODE_NORMAL         0x00

#define MCP_MAX_SPI_HZ          8000000

typedef struct {
    uint8_t cnf1;
    uint8_t cnf2;
    uint8_t cnf3;
} mcp_timing_t;

static bool spi_transfer(
    mcp2515_transport_t *t,
    const uint8_t *tx,
    uint8_t *rx,
    size_t len
) {
    if (!t || !t->spi || !tx || len == 0) return false;

    spi_transaction_t tr = {
        .length = len * 8,
        .tx_buffer = tx,
        .rx_buffer = rx
    };
    return spi_device_polling_transmit(t->spi, &tr) == ESP_OK;
}

static bool write_regs(
    mcp2515_transport_t *t,
    uint8_t address,
    const uint8_t *data,
    size_t len
) {
    if (!data || len > 13) return false;

    uint8_t tx[15] = {MCP_CMD_WRITE, address};
    memcpy(&tx[2], data, len);
    return spi_transfer(t, tx, NULL, len + 2);
}

static bool read_regs(
    mcp2515_transport_t *t,
    uint8_t address,
    uint8_t *data,
    size_t len
) {
    if (!data || len > 13) return false;

    uint8_t tx[15] = {MCP_CMD_READ, address};
    uint8_t rx[15] = {0};
    if (!spi_transfer(t, tx, rx, len + 2)) return false;

    memcpy(data, &rx[2], len);
    return true;
}

static bool write_reg(mcp2515_transport_t *t, uint8_t address, uint8_t value) {
    return write_regs(t, address, &value, 1);
}

static bool read_reg(mcp2515_transport_t *t, uint8_t address, uint8_t *value) {
    return read_regs(t, address, value, 1);
}

static bool bit_modify(
    mcp2515_transport_t *t,
    uint8_t address,
    uint8_t mask,
    uint8_t value
) {
    uint8_t tx[4] = {MCP_CMD_BIT_MODIFY, address, mask, value};
    return spi_transfer(t, tx, NULL, sizeof(tx));
}


static bool configure_receive_any(mcp2515_transport_t *t) {
    /*
     * RXM1:RXM0 = 11 -> turn masks/filters off and receive any message.
     * RXB0 also enables BUKT so a full RXB0 can roll over into RXB1.
     *
     * No vehicle CAN identifier is rejected by MCP2515 acceptance filters.
     */
    if (!write_reg(t, MCP_RXB0CTRL, 0x64)) return false;
    if (!write_reg(t, MCP_RXB1CTRL, 0x60)) return false;
    return true;
}

static bool reset_chip(mcp2515_transport_t *t) {
    const uint8_t command = MCP_CMD_RESET;
    if (!spi_transfer(t, &command, NULL, 1)) return false;
    vTaskDelay(pdMS_TO_TICKS(5));
    return true;
}

static bool set_mode(mcp2515_transport_t *t, uint8_t mode) {
    if (!bit_modify(t, MCP_CANCTRL, MCP_MODE_MASK, mode)) return false;

    const int64_t deadline = esp_timer_get_time() + 10000;
    do {
        uint8_t status = 0;
        if (!read_reg(t, MCP_CANSTAT, &status)) return false;
        if ((status & MCP_MODE_MASK) == mode) return true;
    } while (esp_timer_get_time() < deadline);

    return false;
}

/*
 * These timing values match the common MCP2515 8 MHz table used by
 * arduino-mcp2515. A separate 16 MHz table is provided so the actual
 * oscillator is explicit instead of relying on a library default.
 */
static bool timing_for(
    uint32_t osc_hz,
    can_bitrate_t bitrate,
    mcp_timing_t *timing
) {
    if (!timing) return false;

    if (osc_hz == 8000000UL) {
        switch (bitrate) {
            case CAN_BITRATE_125K:  *timing = (mcp_timing_t){0x01, 0xB1, 0x85}; return true;
            case CAN_BITRATE_250K:  *timing = (mcp_timing_t){0x00, 0xB1, 0x85}; return true;
            case CAN_BITRATE_500K:
                /* User-selected 8 MHz / 500 kbit/s timing:
                 * CNF1=0x00: SJW=1 TQ, BRP=0
                 * CNF2=0x91: BTLMODE=1, SAM=0, PHSEG1=3 TQ, PROPSEG=2 TQ
                 * CNF3=0x01: PHSEG2=2 TQ
                 * Total = 1 + 2 + 3 + 2 = 8 TQ -> 500 kbit/s at 8 MHz.
                 */
                *timing = (mcp_timing_t){0x00, 0x91, 0x01};
                return true;
            case CAN_BITRATE_1000K: *timing = (mcp_timing_t){0x00, 0x80, 0x80}; return true;
            default: return false;
        }
    }

    if (osc_hz == 16000000UL) {
        switch (bitrate) {
            case CAN_BITRATE_125K:  *timing = (mcp_timing_t){0x03, 0xF0, 0x86}; return true;
            case CAN_BITRATE_250K:  *timing = (mcp_timing_t){0x41, 0xF1, 0x85}; return true;
            case CAN_BITRATE_500K:  *timing = (mcp_timing_t){0x00, 0xF0, 0x86}; return true;
            case CAN_BITRATE_1000K: *timing = (mcp_timing_t){0x00, 0xD0, 0x82}; return true;
            default: return false;
        }
    }

    return false;
}

bool mcp2515_transport_init_bus(
    mcp2515_transport_t *transport,
    spi_host_device_t host,
    int sck_pin,
    int miso_pin,
    int mosi_pin,
    int cs_pin,
    uint32_t oscillator_hz
) {
    if (!transport) return false;
    memset(transport, 0, sizeof(*transport));

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = mosi_pin,
        .miso_io_num = miso_pin,
        .sclk_io_num = sck_pin,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 16
    };

    esp_err_t err = spi_bus_initialize(host, &bus_cfg, SPI_DMA_DISABLED);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return false;

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = MCP_MAX_SPI_HZ,
        .mode = 0,
        .spics_io_num = cs_pin,
        .queue_size = 1
    };

    if (spi_bus_add_device(host, &dev_cfg, &transport->spi) != ESP_OK) return false;

    transport->cs_pin = cs_pin;
    transport->oscillator_hz = oscillator_hz;
    return true;
}

bool mcp2515_transport_begin(
    mcp2515_transport_t *transport,
    can_bitrate_t bitrate
) {
    if (!transport || !transport->spi) return false;

    mcp_timing_t timing;
    if (!timing_for(transport->oscillator_hz, bitrate, &timing)) return false;

    if (!reset_chip(transport)) return false;
    if (!set_mode(transport, MCP_MODE_CONFIG)) return false;

    /* Write the bit timing registers explicitly in the requested form. */
    if (!write_reg(transport, MCP_CNF1, timing.cnf1)) return false;
    if (!write_reg(transport, MCP_CNF2, timing.cnf2)) return false;
    if (!write_reg(transport, MCP_CNF3, timing.cnf3)) return false;

    if (!configure_receive_any(transport)) return false;

    if (!write_reg(transport, MCP_CANINTF, 0x00)) return false;
    if (!write_reg(transport, MCP_CANINTE, 0x03)) return false;

    if (!set_mode(transport, MCP_MODE_NORMAL)) return false;

    transport->bitrate = bitrate;
    return true;
}

bool mcp2515_transport_send(
    mcp2515_transport_t *transport,
    uint16_t id,
    const uint8_t *data,
    uint8_t length
) {
    if (!transport || !data || length > 8 || id > 0x7FF) return false;

    const int64_t free_deadline = esp_timer_get_time() + 3000;
    uint8_t ctrl = 0;
    do {
        if (!read_reg(transport, MCP_TXB0CTRL, &ctrl)) return false;
        if ((ctrl & MCP_TXREQ) == 0) break;
    } while (esp_timer_get_time() < free_deadline);

    if (ctrl & MCP_TXREQ) return false;

    uint8_t frame[13] = {0};
    frame[0] = (uint8_t)(id >> 3);
    frame[1] = (uint8_t)((id & 0x07) << 5);
    frame[2] = 0;
    frame[3] = 0;
    frame[4] = length & 0x0F;
    memcpy(&frame[5], data, length);

    if (!write_reg(transport, MCP_TXB0CTRL, 0x00)) return false;
    if (!write_regs(transport, MCP_TXB0SIDH, frame, 5 + length)) return false;

    const uint8_t rts = MCP_CMD_RTS_TX0;
    if (!spi_transfer(transport, &rts, NULL, 1)) return false;

    const int64_t done_deadline = esp_timer_get_time() + 5000;
    do {
        if (!read_reg(transport, MCP_TXB0CTRL, &ctrl)) return false;
        if ((ctrl & MCP_TXREQ) == 0) {
            return (ctrl & (MCP_TXERR | MCP_ABTF)) == 0;
        }
    } while (esp_timer_get_time() < done_deadline);

    return false;
}

static bool receive_from_buffer(
    mcp2515_transport_t *transport,
    uint8_t sidh_address,
    uint8_t intf_mask,
    uint32_t *id,
    uint8_t *data,
    uint8_t *length
) {
    uint8_t raw[13] = {0};
    if (!read_regs(transport, sidh_address, raw, sizeof(raw))) return false;

    const bool extended = (raw[1] & 0x08U) != 0;
    const bool rtr = (raw[4] & 0x40U) != 0;

    *length = raw[4] & 0x0FU;
    if (*length > 8) *length = 8;

    if (extended) {
        uint32_t eid =
            ((uint32_t)raw[0] << 21) |
            ((uint32_t)(raw[1] & 0xE0U) << 13) |
            ((uint32_t)(raw[1] & 0x03U) << 16) |
            ((uint32_t)raw[2] << 8) |
            (uint32_t)raw[3];
        *id = (eid & MCP2515_CAN_ID_MASK) | MCP2515_CAN_EFF_FLAG;
    } else {
        *id = ((uint32_t)raw[0] << 3) | (raw[1] >> 5);
    }

    if (rtr) *id |= MCP2515_CAN_RTR_FLAG;

    if (!rtr) memcpy(data, &raw[5], *length);

    (void)bit_modify(transport, MCP_CANINTF, intf_mask, 0x00);
    return true;
}

bool mcp2515_transport_receive(
    mcp2515_transport_t *transport,
    uint32_t *id,
    uint8_t *data,
    uint8_t *length
) {
    if (!transport || !id || !data || !length) return false;

    uint8_t intf = 0;
    if (!read_reg(transport, MCP_CANINTF, &intf)) return false;

    if (intf & MCP_CANINTF_RX0IF) {
        if (receive_from_buffer(
                transport, MCP_RXB0SIDH, MCP_CANINTF_RX0IF, id, data, length)) {
            return true;
        }
    }

    if (intf & MCP_CANINTF_RX1IF) {
        if (receive_from_buffer(
                transport, MCP_RXB1SIDH, MCP_CANINTF_RX1IF, id, data, length)) {
            return true;
        }
    }

    return false;
}
