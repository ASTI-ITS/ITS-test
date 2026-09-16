#ifndef OBD2_H
#define OBD2_H

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "../Transport/MCP2515Transport.h"
#include "../Data/Mode01Data.h"
#include "../Decoder/Decoder.h"

#define OBD_RESPONSE_BUFFER_SIZE 1600
#define OBD_RESPONSE_QUEUE_SIZE 4
#define OBD_MAX_DTCS 32
#define OBD_MIN_RESPONSE_TIMEOUT_MS 200

typedef struct {
    uint16_t stored[OBD_MAX_DTCS];
    uint16_t pending[OBD_MAX_DTCS];
    uint16_t permanent[OBD_MAX_DTCS];
    uint8_t stored_count;
    uint8_t pending_count;
    uint8_t permanent_count;
    uint8_t freeze_frame[64];
    uint8_t freeze_frame_length;
    char vin[18];
    char calibration_id[33];
    uint32_t last_update_ms;
} diagnostic_data_t;

typedef struct {
    char text[OBD_RESPONSE_BUFFER_SIZE];
    bool line_feeds;
} obd_response_t;

/*
 * Static response pool. FreeRTOS queues carry only uint8_t slot indexes,
 * never a 1600-byte response object. This keeps queue critical sections short
 * and avoids large stack temporaries in the high-priority CAN task.
 */
typedef struct {
    obd_response_t *slots;
    uint8_t slot_count;
    QueueHandle_t free_slots;
    QueueHandle_t ready_slots;
} obd_response_pool_t;

typedef struct {
    mcp2515_transport_t *can;
    decoder_t decoder;
    mode01_data_t vehicle_data;
    diagnostic_data_t diagnostics;

    obd_response_pool_t *response_pool;

    char response_buffer[OBD_RESPONSE_BUFFER_SIZE];
    char echo_prefix[32];
    char obd_echo_prefix[32];
    char non_obd_echo_prefix[32];
    char last_command[128];

    bool request_active;
    bool non_obd_request;
    uint16_t non_obd_response_id;
    uint32_t non_obd_request_started_ms;

    bool echo;
    bool headers;
    bool spaces;
    bool line_feeds;
    bool adaptive_timing;
    bool allow_long_messages;
    bool can_auto_formatting;
    bool can_flow_control;
    bool memory_enabled;
    bool automatic_protocol;

    uint16_t request_id;
    uint16_t response_filter;
    uint16_t flow_control_header;
    uint8_t flow_control_mode;

    uint16_t timeout_ms;
    uint32_t request_started_ms;
    uint8_t requested_service;
    uint8_t requested_pid;
    bool requested_has_pid;

    uint8_t iso_buffer[512];
    uint16_t iso_expected;
    uint16_t iso_length;
    uint8_t iso_sequence;
    uint16_t iso_source_id;

    can_bitrate_t bitrate;
} obd2_t;

bool obd_response_pool_init(
    obd_response_pool_t *pool,
    obd_response_t *slots,
    uint8_t slot_count,
    QueueHandle_t free_slots,
    QueueHandle_t ready_slots
);

bool obd2_init(
    obd2_t *obd,
    mcp2515_transport_t *can,
    obd_response_pool_t *response_pool,
    can_bitrate_t bitrate
);

bool obd2_command(obd2_t *obd, const char *command);

/* Drains all currently pending MCP2515 frames and checks request timeouts.
 * Returns true when at least one CAN frame was consumed.
 */
bool obd2_poll(obd2_t *obd);

bool obd2_busy(const obd2_t *obd);
bool obd2_line_feeds_enabled(const obd2_t *obd);
mode01_data_t *obd2_get_data(obd2_t *obd);
diagnostic_data_t *obd2_get_diagnostics(obd2_t *obd);

#endif
