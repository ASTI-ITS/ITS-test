#include "OBD2.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "PIDTable.h"
#include "esp_timer.h"

#define OBD_FUNCTIONAL_REQUEST 0x7DF
#define OBD_RESPONSE_MIN       0x7E8
#define OBD_RESPONSE_MAX       0x7EF

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static uint16_t min_u16(uint16_t a, uint16_t b) {
    return a < b ? a : b;
}

static void diagnostic_clear(diagnostic_data_t *data) {
    if (data) memset(data, 0, sizeof(*data));
}

static void reset_protocol_state(obd2_t *obd) {
    obd->request_active = false;
    obd->non_obd_request = false;

    obd->echo = true;
    obd->headers = false;
    obd->spaces = true;
    obd->line_feeds = false;
    obd->adaptive_timing = true;
    obd->allow_long_messages = false;
    obd->can_auto_formatting = true;
    obd->can_flow_control = true;
    obd->memory_enabled = true;
    obd->automatic_protocol = true;

    obd->request_id = OBD_FUNCTIONAL_REQUEST;
    obd->response_filter = 0;
    obd->flow_control_header = 0;
    obd->flow_control_mode = 0;
    obd->timeout_ms = 1000;

    obd->iso_expected = 0;
    obd->iso_length = 0;
    obd->iso_sequence = 1;
    obd->iso_source_id = 0;
}

bool obd_response_pool_init(
    obd_response_pool_t *pool,
    obd_response_t *slots,
    uint8_t slot_count,
    QueueHandle_t free_slots,
    QueueHandle_t ready_slots
) {
    if (!pool || !slots || slot_count == 0 || !free_slots || !ready_slots) {
        return false;
    }

    pool->slots = slots;
    pool->slot_count = slot_count;
    pool->free_slots = free_slots;
    pool->ready_slots = ready_slots;

    for (uint8_t i = 0; i < slot_count; ++i) {
        if (xQueueSend(free_slots, &i, 0) != pdTRUE) return false;
    }

    return true;
}

static void queue_response(obd2_t *obd, const char *text) {
    if (!obd || !obd->response_pool) return;

    obd_response_pool_t *pool = obd->response_pool;
    uint8_t slot = 0;

    /* Never wait for BLE from the high-priority OBD/CAN task. */
    if (xQueueReceive(pool->free_slots, &slot, 0) != pdTRUE) return;
    if (slot >= pool->slot_count) return;

    obd_response_t *response = &pool->slots[slot];
    response->line_feeds = obd->line_feeds;
    snprintf(response->text, sizeof(response->text), "%s", text ? text : "");

    /* Queue only one byte. Return the slot if the ready queue is unexpectedly full. */
    if (xQueueSend(pool->ready_slots, &slot, 0) != pdTRUE) {
        (void)xQueueSend(pool->free_slots, &slot, 0);
    }
}

static void set_response(obd2_t *obd, const char *text) {
    if (!obd) return;

    if (obd->echo_prefix[0]) {
        snprintf(
            obd->response_buffer,
            sizeof(obd->response_buffer),
            "%s\r%s",
            obd->echo_prefix,
            text ? text : ""
        );
    } else {
        snprintf(
            obd->response_buffer,
            sizeof(obd->response_buffer),
            "%s",
            text ? text : ""
        );
    }

    queue_response(obd, obd->response_buffer);
}

static bool parse_hex(const char *text, uint8_t *bytes, uint8_t *length) {
    if (!text || !bytes || !length) return false;

    size_t n = strlen(text);
    *length = 0;

    if (n == 0 || (n & 1U) || n > 14) return false;

    for (size_t i = 0; i < n; i += 2) {
        if (!isxdigit((unsigned char)text[i]) ||
            !isxdigit((unsigned char)text[i + 1])) {
            return false;
        }

        char pair[3] = {text[i], text[i + 1], '\0'};
        bytes[(*length)++] = (uint8_t)strtoul(pair, NULL, 16);
    }

    return true;
}

static bool parse_non_obd_request(const char *command, uint16_t *response_id) {
    if (!command || !response_id || strlen(command) != 4) return false;

    if (command[0] < 'A' || command[0] > 'Z' ||
        command[1] < 'A' || command[1] > 'Z' ||
        !isxdigit((unsigned char)command[2]) ||
        !isxdigit((unsigned char)command[3])) {
        return false;
    }

    switch (command[0]) {
        case 'N':
            if ((command[1] == 'N' || command[1] == 'V') &&
                command[2] == '1' &&
                (command[3] == '3' || command[3] == '4')) {
                *response_id = 0x60D;
                return true;
            }
            return false;

        case 'M':
            if ((command[1] == 'O' || command[1] == 'M') &&
                command[2] == '2' &&
                (command[3] == '3' || command[3] == '5')) {
                *response_id = 0x208;
                return true;
            }
            return false;

        case 'T':
            /* Toyota mappings were placeholders in the supplied source. */
            return false;

        default:
            return false;
    }
}

static bool send_request(obd2_t *obd, const uint8_t *payload, uint8_t length) {
    if (!obd || !payload || length == 0 || length > 7) return false;

    uint8_t frame[8] = {0};
    frame[0] = length;
    memcpy(&frame[1], payload, length);

    if (!mcp2515_transport_send(obd->can, obd->request_id, frame, 8)) {
        set_response(obd, "CAN ERROR");
        return false;
    }

    obd->request_active = true;
    obd->request_started_ms = now_ms();
    obd->iso_expected = 0;
    obd->iso_length = 0;
    obd->iso_sequence = 1;
    return true;
}

static void send_flow_control(obd2_t *obd, uint16_t source_id) {
    if (!obd || !obd->can_flow_control) return;

    uint8_t frame[8] = {0x30, 0x00, 0x00, 0, 0, 0, 0, 0};
    uint16_t target =
        (obd->flow_control_mode == 1 && obd->flow_control_header)
        ? obd->flow_control_header
        : ((source_id >= OBD_RESPONSE_MIN && source_id <= OBD_RESPONSE_MAX)
            ? (uint16_t)(source_id - 8)
            : obd->request_id);

    (void)mcp2515_transport_send(obd->can, target, frame, 8);
}

static void update_caches(obd2_t *obd, const uint8_t *payload, uint16_t length) {
    if (!obd || !payload || length == 0) return;

    uint8_t service = (uint8_t)(payload[0] - 0x40);

    if ((service == 0x01 || service == 0x02) && length >= 3) {
        uint8_t pid = payload[1];

        if (service == 0x01) {
            if ((pid % 0x20) == 0 && length >= 6) {
                uint32_t bitmap =
                    ((uint32_t)payload[2] << 24) |
                    ((uint32_t)payload[3] << 16) |
                    ((uint32_t)payload[4] << 8) |
                    payload[5];

                mode01_data_set_supported_pid(&obd->vehicle_data, pid, bitmap);

                for (uint8_t bit = 0;
                     bit < 32 && ((uint16_t)pid + bit + 1) < 256;
                     ++bit) {
                    obd->vehicle_data.supported_pid[pid + bit + 1] =
                        (bitmap & (0x80000000UL >> bit)) != 0;
                }
            } else {
                decoder_decode(
                    &obd->decoder,
                    0x01,
                    pid,
                    &payload[2],
                    (uint8_t)(length - 2)
                );
            }
        } else {
            obd->diagnostics.freeze_frame_length =
                (uint8_t)min_u16(sizeof(obd->diagnostics.freeze_frame), length);
            memcpy(
                obd->diagnostics.freeze_frame,
                payload,
                obd->diagnostics.freeze_frame_length
            );
        }
    } else if (service == 0x03 || service == 0x07 || service == 0x0A) {
        uint16_t *target =
            service == 0x03 ? obd->diagnostics.stored :
            service == 0x07 ? obd->diagnostics.pending :
                              obd->diagnostics.permanent;

        uint8_t *count =
            service == 0x03 ? &obd->diagnostics.stored_count :
            service == 0x07 ? &obd->diagnostics.pending_count :
                              &obd->diagnostics.permanent_count;

        *count = 0;

        for (uint16_t i = 1;
             i + 1 < length && *count < OBD_MAX_DTCS;
             i += 2) {
            uint16_t code = ((uint16_t)payload[i] << 8) | payload[i + 1];
            if (code) target[(*count)++] = code;
        }
    } else if (service == 0x04) {
        diagnostic_clear(&obd->diagnostics);
    } else if (service == 0x09 && length >= 3) {
        uint8_t pid = payload[1];

        uint16_t start = (pid == 0x02 && length > 3) ? 3 : 2;
        char *target =
            pid == 0x02 ? obd->diagnostics.vin :
                          obd->diagnostics.calibration_id;

        size_t capacity =
            pid == 0x02 ? sizeof(obd->diagnostics.vin) :
                          sizeof(obd->diagnostics.calibration_id);

        if (pid == 0x02 || pid == 0x04) {
            size_t out = 0;
            for (uint16_t i = start; i < length && out + 1 < capacity; ++i) {
                if (isprint(payload[i])) target[out++] = (char)payload[i];
            }
            target[out] = '\0';
        }
    }

    obd->diagnostics.last_update_ms = now_ms();
}

static void format_non_obd_frame(
    obd2_t *obd,
    uint16_t source_id,
    const uint8_t *data,
    uint8_t length,
    const char *prefix
) {
    if (!obd || !data) return;

    size_t out = 0;
    obd->response_buffer[0] = '\0';

    if (prefix && prefix[0]) {
        out += snprintf(
            obd->response_buffer,
            sizeof(obd->response_buffer),
            "%s\r",
            prefix
        );
    }

    out += snprintf(
        &obd->response_buffer[out],
        sizeof(obd->response_buffer) - out,
        "%03X %u",
        source_id,
        length
    );

    for (uint8_t i = 0;
         i < length && out + 4 < sizeof(obd->response_buffer);
         ++i) {
        out += snprintf(
            &obd->response_buffer[out],
            sizeof(obd->response_buffer) - out,
            " %02X",
            data[i]
        );
    }

    queue_response(obd, obd->response_buffer);
}

static void append_byte(obd2_t *obd, size_t *out, uint8_t value, bool separator) {
    if (!obd || !out) return;

    if (separator && obd->spaces && *out + 1 < sizeof(obd->response_buffer)) {
        obd->response_buffer[(*out)++] = ' ';
        obd->response_buffer[*out] = '\0';
    }

    if (*out + 3 < sizeof(obd->response_buffer)) {
        *out += snprintf(
            &obd->response_buffer[*out],
            sizeof(obd->response_buffer) - *out,
            "%02X",
            value
        );
    }
}

static void begin_frame(obd2_t *obd, size_t *out, uint16_t source_id) {
    if (!obd || !out || !obd->headers) return;

    *out += snprintf(
        &obd->response_buffer[*out],
        sizeof(obd->response_buffer) - *out,
        "%03X%s",
        source_id,
        obd->spaces ? " " : ""
    );
}

static void format_payload(
    obd2_t *obd,
    uint16_t source_id,
    const uint8_t *payload,
    uint16_t length,
    const char *prefix
) {
    if (!obd || !payload) return;

    size_t out = 0;
    obd->response_buffer[0] = '\0';

    if (prefix && prefix[0]) {
        out += snprintf(
            obd->response_buffer,
            sizeof(obd->response_buffer),
            "%s\r",
            prefix
        );
    }

    if (!obd->headers && obd->can_auto_formatting) {
        for (uint16_t i = 0;
             i < length && out + 3 < sizeof(obd->response_buffer);
             ++i) {
            if (i && obd->spaces) {
                obd->response_buffer[out++] = ' ';
                obd->response_buffer[out] = '\0';
            }

            out += snprintf(
                &obd->response_buffer[out],
                sizeof(obd->response_buffer) - out,
                "%02X",
                payload[i]
            );
        }

        queue_response(obd, obd->response_buffer);
        return;
    }

    begin_frame(obd, &out, source_id);

    if (length <= 7) {
        append_byte(obd, &out, (uint8_t)length, false);
        for (uint16_t i = 0; i < length; ++i) {
            append_byte(obd, &out, payload[i], true);
        }
        for (uint8_t i = (uint8_t)(length + 1); i < 8; ++i) {
            append_byte(obd, &out, 0x00, true);
        }

        queue_response(obd, obd->response_buffer);
        return;
    }

    append_byte(obd, &out, (uint8_t)(0x10 | ((length >> 8) & 0x0F)), false);
    append_byte(obd, &out, (uint8_t)(length & 0xFF), true);

    uint16_t payload_offset = 0;
    uint16_t first_count = min_u16(6, length);

    for (; payload_offset < first_count; ++payload_offset) {
        append_byte(obd, &out, payload[payload_offset], true);
    }

    uint8_t sequence = 1;

    while (payload_offset < length && out + 8 < sizeof(obd->response_buffer)) {
        obd->response_buffer[out++] = '\r';
        obd->response_buffer[out] = '\0';

        begin_frame(obd, &out, source_id);
        append_byte(obd, &out, (uint8_t)(0x20 | (sequence++ & 0x0F)), false);

        uint8_t frame_bytes = 0;
        for (;
             frame_bytes < 7 && payload_offset < length;
             ++frame_bytes, ++payload_offset) {
            append_byte(obd, &out, payload[payload_offset], true);
        }

        for (; frame_bytes < 7; ++frame_bytes) {
            append_byte(obd, &out, 0x00, true);
        }
    }

    queue_response(obd, obd->response_buffer);
}

static void process_payload(
    obd2_t *obd,
    uint16_t source_id,
    const uint8_t *payload,
    uint16_t length
) {
    if (!obd || !payload || length == 0) return;

    if (payload[0] == 0x7F) {
        if (length >= 3 && payload[2] == 0x78) {
            obd->request_started_ms = now_ms();
            return;
        }

        format_payload(
            obd,
            source_id,
            payload,
            length,
            obd->obd_echo_prefix
        );
        obd->request_active = false;
        return;
    }

    if (!obd->request_active ||
        payload[0] != (uint8_t)(obd->requested_service + 0x40)) {
        return;
    }

    if (obd->requested_has_pid &&
        length > 1 &&
        payload[1] != obd->requested_pid) {
        return;
    }

    update_caches(obd, payload, length);
    format_payload(
        obd,
        source_id,
        payload,
        length,
        obd->obd_echo_prefix
    );
    obd->request_active = false;
}

static void finish_no_data(obd2_t *obd) {
    obd->request_active = false;
    set_response(obd, "NO DATA");
}

static void finish_non_obd_no_data(obd2_t *obd) {
    obd->non_obd_request = false;

    if (obd->non_obd_echo_prefix[0]) {
        snprintf(
            obd->response_buffer,
            sizeof(obd->response_buffer),
            "%s\rNO DATA",
            obd->non_obd_echo_prefix
        );
        queue_response(obd, obd->response_buffer);
    } else {
        queue_response(obd, "NO DATA");
    }
}

static bool parse_u32_hex(const char *text, uint32_t *value) {
    if (!text || !value || !*text) return false;

    char *end = NULL;
    unsigned long parsed = strtoul(text, &end, 16);
    if (!end || *end != '\0') return false;

    *value = (uint32_t)parsed;
    return true;
}

static bool process_at(obd2_t *obd, const char *cmd) {
    if (!obd || !cmd) return false;

    if (!strcmp(cmd, "ATZ") || !strcmp(cmd, "ATWS") || !strcmp(cmd, "ATD")) {
        reset_protocol_state(obd);
        set_response(
            obd,
            (!strcmp(cmd, "ATZ") || !strcmp(cmd, "ATWS"))
                ? "ELM327 v1.5"
                : "OK"
        );
        return true;
    }

    if (!strcmp(cmd, "ATI"))  { set_response(obd, "ELM327 v1.5"); return true; }
    if (!strcmp(cmd, "AT@1")) { set_response(obd, "OBD_BLE ESP32"); return true; }
    if (!strcmp(cmd, "AT@2")) { set_response(obd, "OBD_BLE"); return true; }

    if (!strcmp(cmd, "ATDP")) {
        if (obd->bitrate == CAN_BITRATE_500K) {
            set_response(
                obd,
                obd->automatic_protocol
                    ? "AUTO, ISO 15765-4 (CAN 11/500)"
                    : "ISO 15765-4 (CAN 11/500)"
            );
        } else {
            set_response(
                obd,
                obd->automatic_protocol
                    ? "AUTO, CAN 11/1000"
                    : "CAN 11/1000"
            );
        }
        return true;
    }

    if (!strcmp(cmd, "ATDPN")) {
        set_response(
            obd,
            obd->bitrate == CAN_BITRATE_500K
                ? (obd->automatic_protocol ? "A6" : "6")
                : "USER1"
        );
        return true;
    }

    if (!strcmp(cmd, "ATRV"))  { set_response(obd, "12.0V"); return true; }
    if (!strcmp(cmd, "ATIGN")) { set_response(obd, "ON"); return true; }

    if (!strcmp(cmd, "ATPC")) {
        obd->request_active = false;
        obd->non_obd_request = false;
        set_response(obd, "OK");
        return true;
    }

    if (!strcmp(cmd, "ATFE") || !strcmp(cmd, "ATAR") || !strcmp(cmd, "ATBI")) {
        set_response(obd, "OK");
        return true;
    }

    if (!strcmp(cmd, "ATE0") || !strcmp(cmd, "ATE1")) {
        obd->echo = cmd[3] == '1';
        set_response(obd, "OK");
        return true;
    }

    if (!strcmp(cmd, "ATH0") || !strcmp(cmd, "ATH1")) {
        obd->headers = cmd[3] == '1';
        set_response(obd, "OK");
        return true;
    }

    if (!strcmp(cmd, "ATS0") || !strcmp(cmd, "ATS1")) {
        obd->spaces = cmd[3] == '1';
        set_response(obd, "OK");
        return true;
    }

    if (!strcmp(cmd, "ATL0") || !strcmp(cmd, "ATL1")) {
        obd->line_feeds = cmd[3] == '1';
        set_response(obd, "OK");
        return true;
    }

    if (!strcmp(cmd, "ATM0") || !strcmp(cmd, "ATM1")) {
        obd->memory_enabled = cmd[3] == '1';
        set_response(obd, "OK");
        return true;
    }

    if (!strcmp(cmd, "ATAT0") || !strcmp(cmd, "ATAT1") || !strcmp(cmd, "ATAT2")) {
        obd->adaptive_timing = strcmp(cmd, "ATAT0") != 0;
        set_response(obd, "OK");
        return true;
    }

    if (!strcmp(cmd, "ATAL") || !strcmp(cmd, "ATAL1")) {
        obd->allow_long_messages = true;
        set_response(obd, "OK");
        return true;
    }

    if (!strcmp(cmd, "ATNL") || !strcmp(cmd, "ATAL0")) {
        obd->allow_long_messages = false;
        set_response(obd, "OK");
        return true;
    }

    if (!strcmp(cmd, "ATCAF0") || !strcmp(cmd, "ATCAF1")) {
        obd->can_auto_formatting = cmd[5] == '1';
        set_response(obd, "OK");
        return true;
    }

    if (!strcmp(cmd, "ATCFC0") || !strcmp(cmd, "ATCFC1")) {
        obd->can_flow_control = cmd[5] == '1';
        set_response(obd, "OK");
        return true;
    }

    if (!strcmp(cmd, "ATSP0") || !strcmp(cmd, "ATTP0")) {
        obd->automatic_protocol = true;
        set_response(obd, "OK");
        return true;
    }

    if (!strcmp(cmd, "ATSP6") || !strcmp(cmd, "ATTP6")) {
        obd->automatic_protocol = false;
        set_response(obd, "OK");
        return true;
    }

    if (!strncmp(cmd, "ATST", 4) &&
        (strlen(cmd) == 5 || strlen(cmd) == 6)) {
        uint32_t value = 0;

        if (parse_u32_hex(&cmd[4], &value) && value > 0) {
            uint32_t timeout = value * 4U;
            if (timeout < OBD_MIN_RESPONSE_TIMEOUT_MS) {
                timeout = OBD_MIN_RESPONSE_TIMEOUT_MS;
            }
            if (timeout > 5000U) timeout = 5000U;

            obd->timeout_ms = (uint16_t)timeout;
            set_response(obd, "OK");
            return true;
        }
    }

    if (!strncmp(cmd, "ATSH", 4) &&
        (strlen(cmd) == 7 || strlen(cmd) == 11)) {
        uint32_t value = 0;

        if (parse_u32_hex(&cmd[4], &value) && value <= 0x7FF) {
            obd->request_id = (uint16_t)value;
            set_response(obd, "OK");
            return true;
        }
    }

    if (!strncmp(cmd, "ATCRA", 5) && strlen(cmd) == 8) {
        uint32_t value = 0;

        if (parse_u32_hex(&cmd[5], &value) && value <= 0x7FF) {
            obd->response_filter = (uint16_t)value;
            set_response(obd, "OK");
            return true;
        }
    }

    if (!strcmp(cmd, "ATCRA")) {
        obd->response_filter = 0;
        set_response(obd, "OK");
        return true;
    }

    if (!strncmp(cmd, "ATFCSH", 6) && strlen(cmd) == 9) {
        uint32_t value = 0;

        if (parse_u32_hex(&cmd[6], &value) && value <= 0x7FF) {
            obd->flow_control_header = (uint16_t)value;
            set_response(obd, "OK");
            return true;
        }
    }

    if (!strcmp(cmd, "ATFCSM0") || !strcmp(cmd, "ATFCSM1")) {
        obd->flow_control_mode = (uint8_t)(cmd[6] - '0');
        set_response(obd, "OK");
        return true;
    }

    set_response(obd, "?");
    return false;
}

bool obd2_init(
    obd2_t *obd,
    mcp2515_transport_t *can,
    obd_response_pool_t *response_pool,
    can_bitrate_t bitrate
) {
    if (!obd || !can || !response_pool) return false;

    memset(obd, 0, sizeof(*obd));
    obd->can = can;
    obd->response_pool = response_pool;
    obd->bitrate = bitrate;

    mode01_data_clear(&obd->vehicle_data);
    diagnostic_clear(&obd->diagnostics);
    pid_table_init();
    decoder_init(&obd->decoder, &obd->vehicle_data);
    reset_protocol_state(obd);

    return mcp2515_transport_begin(can, bitrate);
}

bool obd2_command(obd2_t *obd, const char *input) {
    if (!obd || !input) return false;

    char cmd[128];
    size_t out = 0;

    for (size_t i = 0; input[i] && out + 1 < sizeof(cmd); ++i) {
        unsigned char c = (unsigned char)input[i];

        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        cmd[out++] = (char)toupper(c);
    }
    cmd[out] = '\0';

    if (cmd[0] == '\0') {
        if (obd->last_command[0] == '\0') {
            obd->echo_prefix[0] = '\0';
            set_response(obd, "");
            return true;
        }

        snprintf(cmd, sizeof(cmd), "%s", obd->last_command);
    } else {
        snprintf(obd->last_command, sizeof(obd->last_command), "%s", cmd);
    }

    snprintf(
        obd->echo_prefix,
        sizeof(obd->echo_prefix),
        "%s",
        obd->echo ? cmd : ""
    );

    uint16_t non_obd_id = 0;
    bool is_non_obd = parse_non_obd_request(cmd, &non_obd_id);

    if (!strncmp(cmd, "AT", 2)) {
        return process_at(obd, cmd);
    }

    if (is_non_obd) {
        if (obd->non_obd_request) {
            set_response(obd, "BUSY");
            return false;
        }

        snprintf(
            obd->non_obd_echo_prefix,
            sizeof(obd->non_obd_echo_prefix),
            "%s",
            obd->echo_prefix
        );

        obd->non_obd_request = true;
        obd->non_obd_response_id = non_obd_id;
        obd->non_obd_request_started_ms = now_ms();
        return true;
    }

    if (obd->request_active) {
        set_response(obd, "BUSY");
        return false;
    }

    uint8_t payload[7];
    uint8_t length = 0;

    if (!parse_hex(cmd, payload, &length) || length == 0) {
        set_response(obd, "?");
        return false;
    }

    uint8_t service = payload[0];

    if (!(service == 0x01 || service == 0x02 || service == 0x03 ||
          service == 0x04 || service == 0x07 || service == 0x09 ||
          service == 0x0A)) {
        set_response(obd, "?");
        return false;
    }

    obd->requested_service = service;
    obd->requested_has_pid = length > 1;
    obd->requested_pid = obd->requested_has_pid ? payload[1] : 0;

    snprintf(
        obd->obd_echo_prefix,
        sizeof(obd->obd_echo_prefix),
        "%s",
        obd->echo_prefix
    );

    return send_request(obd, payload, length);
}

bool obd2_poll(obd2_t *obd) {
    if (!obd || !obd->can) return false;

    bool consumed_any = false;
    uint16_t id = 0;
    uint8_t frame[8];
    uint8_t length = 0;

    /*
     * Timing-critical path:
     * - drain MCP2515 completely
     * - no BLE notify
     * - no printf/logging
     * - no heap allocation
     */
    while (mcp2515_transport_receive(obd->can, &id, frame, &length)) {
        consumed_any = true;

        if (obd->non_obd_request && id == obd->non_obd_response_id) {
            format_non_obd_frame(
                obd,
                id,
                frame,
                length > 8 ? 8 : length,
                obd->non_obd_echo_prefix
            );
            obd->non_obd_request = false;
            continue;
        }

        if (!obd->request_active ||
            id < OBD_RESPONSE_MIN ||
            id > OBD_RESPONSE_MAX ||
            length < 2) {
            continue;
        }

        if (obd->response_filter && id != obd->response_filter) continue;

        uint8_t type = frame[0] >> 4;

        if (type == 0) {
            uint8_t payload_length = frame[0] & 0x0F;
            if (payload_length > (uint8_t)(length - 1)) {
                payload_length = (uint8_t)(length - 1);
            }

            process_payload(obd, id, &frame[1], payload_length);
        } else if (type == 1) {
            obd->iso_expected =
                ((uint16_t)(frame[0] & 0x0F) << 8) | frame[1];

            if (obd->iso_expected > sizeof(obd->iso_buffer)) {
                set_response(obd, "BUFFER FULL");
                obd->request_active = false;
                continue;
            }

            obd->iso_source_id = id;
            obd->iso_length =
                min_u16((uint16_t)(length - 2), obd->iso_expected);

            memcpy(obd->iso_buffer, &frame[2], obd->iso_length);
            obd->iso_sequence = 1;
            send_flow_control(obd, id);
        } else if (type == 2 &&
                   id == obd->iso_source_id &&
                   obd->iso_expected) {
            if ((frame[0] & 0x0F) != (obd->iso_sequence & 0x0F)) continue;

            obd->iso_sequence++;

            uint16_t amount = min_u16(
                (uint16_t)(length - 1),
                (uint16_t)(obd->iso_expected - obd->iso_length)
            );

            memcpy(
                &obd->iso_buffer[obd->iso_length],
                &frame[1],
                amount
            );
            obd->iso_length += amount;

            if (obd->iso_length >= obd->iso_expected) {
                process_payload(
                    obd,
                    id,
                    obd->iso_buffer,
                    obd->iso_expected
                );
                obd->iso_expected = 0;
                obd->iso_length = 0;
            }
        }
    }

    uint32_t now = now_ms();

    if (obd->request_active &&
        (uint32_t)(now - obd->request_started_ms) >= obd->timeout_ms) {
        finish_no_data(obd);
    }

    if (obd->non_obd_request &&
        (uint32_t)(now - obd->non_obd_request_started_ms) >= obd->timeout_ms) {
        finish_non_obd_no_data(obd);
    }

    return consumed_any;
}

bool obd2_busy(const obd2_t *obd) {
    return obd && (obd->request_active || obd->non_obd_request);
}

bool obd2_line_feeds_enabled(const obd2_t *obd) {
    return obd && obd->line_feeds;
}

mode01_data_t *obd2_get_data(obd2_t *obd) {
    return obd ? &obd->vehicle_data : NULL;
}

diagnostic_data_t *obd2_get_diagnostics(obd2_t *obd) {
    return obd ? &obd->diagnostics : NULL;
}
