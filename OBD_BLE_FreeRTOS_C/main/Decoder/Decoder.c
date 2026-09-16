#include "Decoder.h"

#include "../Core/Formula.h"
#include "../Core/PIDTable.h"
#include "esp_timer.h"

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void update_data(mode01_data_t *data, uint8_t pid, float value) {
    if (!data) return;

    switch (pid) {
        case 0x04: data->engine_load = value; break;
        case 0x05: data->coolant_temp = (int16_t)value; break;
        case 0x06: data->short_fuel_trim_bank1 = value; break;
        case 0x07: data->long_fuel_trim_bank1 = value; break;
        case 0x0A: data->fuel_pressure = (uint16_t)value; break;
        case 0x0B: data->intake_manifold_pressure = (uint8_t)value; break;
        case 0x0C: data->rpm = (uint16_t)value; break;
        case 0x0D: data->speed = (uint8_t)value; break;
        case 0x0E: data->timing_advance = value; break;
        case 0x0F: data->intake_air_temp = (int16_t)value; break;
        case 0x10: data->maf = value; break;
        case 0x11: data->throttle = value; break;
        case 0x2F: data->fuel_level = value; break;
        case 0x31: data->distance_since_clear = (uint16_t)value; break;
        case 0x42: data->control_module_voltage = value; break;
        case 0x46: data->ambient_air_temp = (int16_t)value; break;
        case 0x47: data->absolute_throttle = value; break;
        case 0x5C: data->engine_oil_temp = (int16_t)value; break;
        case 0x5E: data->engine_fuel_rate = value; break;
        case 0x61: data->driver_demand_torque = value; break;
        case 0x62: data->actual_engine_torque = value; break;
        default: break;
    }
}

void decoder_init(decoder_t *decoder, mode01_data_t *storage) {
    if (!decoder) return;
    decoder->data = storage;
}

void decoder_decode(
    decoder_t *decoder,
    uint8_t mode,
    uint8_t pid,
    const uint8_t *payload,
    uint8_t length
) {
    if (!decoder || !decoder->data || mode != 0x01 || !payload) return;
    if (pid_table[pid].formula == FORMULA_NONE) return;
    if (length < pid_table[pid].length) return;

    float value = 0.0f;

    switch (pid_table[pid].formula) {
        case FORMULA_BYTE_A:    value = formula_byte_a(payload); break;
        case FORMULA_UINT16:    value = formula_uint16(payload); break;
        case FORMULA_RPM:       value = formula_rpm(payload); break;
        case FORMULA_TEMP:      value = formula_temperature(payload); break;
        case FORMULA_PERCENT:   value = formula_percentage(payload); break;
        case FORMULA_FUEL_TRIM: value = formula_fuel_trim(payload); break;
        case FORMULA_MAF:       value = formula_maf(payload); break;
        case FORMULA_VOLTAGE:   value = formula_voltage(payload); break;
        case FORMULA_PRESSURE:  value = formula_pressure(payload); break;
        case FORMULA_TIMING:    value = formula_timing_advance(payload); break;
        case FORMULA_FUEL_RATE: value = formula_fuel_rate(payload); break;
        case FORMULA_TORQUE:    value = formula_torque(payload); break;
        default: return;
    }

    update_data(decoder->data, pid, value);
    decoder->data->last_update_ms = now_ms();
}
