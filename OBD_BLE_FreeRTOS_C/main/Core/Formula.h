#ifndef OBD_FORMULA_H
#define OBD_FORMULA_H

#include <stdint.h>

static inline float formula_byte_a(const uint8_t *p) {
    return p ? (float)p[0] : 0.0f;
}

static inline float formula_uint16(const uint8_t *p) {
    return p ? (float)(((uint16_t)p[0] << 8) | p[1]) : 0.0f;
}

static inline float formula_rpm(const uint8_t *p) {
    return formula_uint16(p) / 4.0f;
}

static inline float formula_temperature(const uint8_t *p) {
    return formula_byte_a(p) - 40.0f;
}

static inline float formula_percentage(const uint8_t *p) {
    return formula_byte_a(p) * 100.0f / 255.0f;
}

static inline float formula_fuel_trim(const uint8_t *p) {
    return (formula_byte_a(p) - 128.0f) * 100.0f / 128.0f;
}

static inline float formula_maf(const uint8_t *p) {
    return formula_uint16(p) / 100.0f;
}

static inline float formula_voltage(const uint8_t *p) {
    return formula_uint16(p) / 1000.0f;
}

static inline float formula_pressure(const uint8_t *p) {
    return formula_byte_a(p);
}

static inline float formula_timing_advance(const uint8_t *p) {
    return formula_byte_a(p) / 2.0f - 64.0f;
}

static inline float formula_fuel_rate(const uint8_t *p) {
    return formula_uint16(p) / 20.0f;
}

static inline float formula_torque(const uint8_t *p) {
    return formula_byte_a(p) - 125.0f;
}

#endif
