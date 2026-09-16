#include "PIDTable.h"

pid_info_t pid_table[TOTAL_PIDS];

static void set_pid(
    uint8_t pid,
    const char *name,
    const char *unit,
    uint8_t length,
    uint16_t interval_ms,
    pid_formula_t formula
) {
    pid_table[pid] = (pid_info_t){
        .pid = pid,
        .name = name,
        .unit = unit,
        .length = length,
        .interval_ms = interval_ms,
        .formula = formula,
        .supported = false
    };
}

void pid_table_init(void) {
    for (int i = 0; i < TOTAL_PIDS; ++i) {
        set_pid((uint8_t)i, "Unknown", "", 0, PID_5000MS, FORMULA_NONE);
    }

    set_pid(0x00, "Supported PID 01-20", "", 4, PID_5000MS, FORMULA_NONE);

    set_pid(0x04, "Engine Load", "%", 1, PID_200MS, FORMULA_PERCENT);
    set_pid(0x05, "Coolant Temperature", "C", 1, PID_200MS, FORMULA_TEMP);
    set_pid(0x06, "Short Fuel Trim Bank1", "%", 1, PID_500MS, FORMULA_FUEL_TRIM);
    set_pid(0x07, "Long Fuel Trim Bank1", "%", 1, PID_500MS, FORMULA_FUEL_TRIM);
    set_pid(0x0A, "Fuel Pressure", "kPa", 1, PID_500MS, FORMULA_PRESSURE);
    set_pid(0x0B, "MAP Pressure", "kPa", 1, PID_200MS, FORMULA_PRESSURE);
    set_pid(0x0C, "Engine RPM", "rpm", 2, PID_50MS, FORMULA_RPM);
    set_pid(0x0D, "Vehicle Speed", "km/h", 1, PID_50MS, FORMULA_BYTE_A);
    set_pid(0x0E, "Timing Advance", "deg", 1, PID_500MS, FORMULA_TIMING);
    set_pid(0x0F, "Intake Temperature", "C", 1, PID_500MS, FORMULA_TEMP);
    set_pid(0x10, "MAF", "g/s", 2, PID_200MS, FORMULA_MAF);
    set_pid(0x11, "Throttle Position", "%", 1, PID_50MS, FORMULA_PERCENT);

    set_pid(0x2F, "Fuel Level", "%", 1, PID_1000MS, FORMULA_PERCENT);
    set_pid(0x31, "Distance Since Clear", "km", 2, PID_1000MS, FORMULA_UINT16);
    set_pid(0x42, "Control Voltage", "V", 2, PID_200MS, FORMULA_VOLTAGE);
    set_pid(0x46, "Ambient Temperature", "C", 1, PID_1000MS, FORMULA_TEMP);
    set_pid(0x5C, "Engine Oil Temperature", "C", 1, PID_1000MS, FORMULA_TEMP);
    set_pid(0x5E, "Engine Fuel Rate", "L/h", 2, PID_1000MS, FORMULA_FUEL_RATE);
    set_pid(0x61, "Driver Demand Torque", "%", 1, PID_500MS, FORMULA_TORQUE);
    set_pid(0x62, "Actual Engine Torque", "%", 1, PID_500MS, FORMULA_TORQUE);
}
