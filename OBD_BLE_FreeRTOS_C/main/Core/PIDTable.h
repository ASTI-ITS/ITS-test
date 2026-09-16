#ifndef PID_TABLE_H
#define PID_TABLE_H

#include <stdbool.h>
#include <stdint.h>

#define TOTAL_PIDS 256

typedef enum {
    PID_50MS   = 50,
    PID_100MS  = 100,
    PID_200MS  = 200,
    PID_500MS  = 500,
    PID_1000MS = 1000,
    PID_5000MS = 5000
} pid_interval_t;

typedef enum {
    FORMULA_NONE = 0,
    FORMULA_BYTE_A,
    FORMULA_UINT16,
    FORMULA_RPM,
    FORMULA_TEMP,
    FORMULA_PERCENT,
    FORMULA_FUEL_TRIM,
    FORMULA_MAF,
    FORMULA_VOLTAGE,
    FORMULA_PRESSURE,
    FORMULA_TIMING,
    FORMULA_FUEL_RATE,
    FORMULA_TORQUE
} pid_formula_t;

typedef struct {
    uint8_t pid;
    const char *name;
    const char *unit;
    uint8_t length;
    uint16_t interval_ms;
    pid_formula_t formula;
    bool supported;
} pid_info_t;

extern pid_info_t pid_table[TOTAL_PIDS];

void pid_table_init(void);

#endif
