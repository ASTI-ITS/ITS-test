#ifndef MCP2515_TRANSPORT_H
#define MCP2515_TRANSPORT_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/spi_master.h"

/*
 * Raw CAN ID flags.
 *
 * Bit 31 = Extended 29-bit CAN identifier
 * Bit 30 = Remote transmission request
 *
 * Lower 29 bits contain the actual CAN identifier.
 */
#define MCP2515_CAN_EFF_FLAG  0x80000000UL
#define MCP2515_CAN_RTR_FLAG  0x40000000UL
#define MCP2515_CAN_ID_MASK   0x1FFFFFFFUL

typedef enum {
    CAN_BITRATE_125K  = 125000,
    CAN_BITRATE_250K  = 250000,
    CAN_BITRATE_500K  = 500000,
    CAN_BITRATE_1000K = 1000000
} can_bitrate_t;

typedef struct {
    spi_device_handle_t spi;

    int cs_pin;

    uint32_t oscillator_hz;

    can_bitrate_t bitrate;

} mcp2515_transport_t;


bool mcp2515_transport_init_bus(
    mcp2515_transport_t *transport,
    spi_host_device_t host,
    int sck_pin,
    int miso_pin,
    int mosi_pin,
    int cs_pin,
    uint32_t oscillator_hz
);


bool mcp2515_transport_begin(
    mcp2515_transport_t *transport,
    can_bitrate_t bitrate
);


bool mcp2515_transport_send(
    mcp2515_transport_t *transport,
    uint16_t id,
    const uint8_t *data,
    uint8_t length
);


/*
 * uint32_t is intentional.
 *
 * It allows the transport to return:
 *
 * 11-bit standard CAN
 * 29-bit extended CAN
 * RTR frames
 */
bool mcp2515_transport_receive(
    mcp2515_transport_t *transport,
    uint32_t *id,
    uint8_t *data,
    uint8_t *length
);


#endif
