#ifndef MODE01_DATA_H
#define MODE01_DATA_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool supported_pid[256];

    uint32_t supported_pid_00;
    uint32_t supported_pid_20;
    uint32_t supported_pid_40;
    uint32_t supported_pid_60;
    uint32_t supported_pid_80;

    uint8_t monitor_status;
    uint8_t fuel_system_status[2];
    float engine_load;
    int16_t coolant_temp;
    float short_fuel_trim_bank1;
    float long_fuel_trim_bank1;
    float short_fuel_trim_bank2;
    float long_fuel_trim_bank2;
    uint16_t fuel_pressure;
    uint8_t intake_manifold_pressure;
    uint16_t rpm;
    uint8_t speed;
    float timing_advance;
    int16_t intake_air_temp;
    float maf;
    float throttle;
    uint8_t secondary_air_status;
    uint8_t oxygen_sensor_location;
    float oxygen_voltage[8];
    float oxygen_fuel_trim[8];
    uint8_t obd_standard;
    uint8_t oxygen_sensor_location_b;
    uint8_t auxiliary_input_status;
    uint16_t engine_run_time;

    uint16_t distance_with_mil;
    float commanded_egr;
    float egr_error;
    float evap_purge;
    float fuel_level;
    uint16_t distance_since_clear;
    uint8_t barometric_pressure;
    int16_t catalyst_temperature[4];

    float control_module_voltage;
    int16_t ambient_air_temp;
    float absolute_load;
    float commanded_air_fuel_ratio;
    float relative_throttle;
    float absolute_throttle;
    float accelerator_pedal;
    float accelerator_pedal2;
    float commanded_throttle;
    uint16_t time_run_with_mil;
    uint16_t time_since_clear;
    uint8_t fuel_type;
    float ethanol_fuel_percentage;
    int16_t evap_pressure;
    float relative_accelerator_pedal;
    float hybrid_battery_remaining;
    int16_t engine_oil_temp;
    float fuel_injection_timing;
    float engine_fuel_rate;

    float driver_demand_torque;
    float actual_engine_torque;
    float reference_torque;

    uint32_t last_update_ms;
} mode01_data_t;

void mode01_data_clear(mode01_data_t *data);
void mode01_data_set_supported_pid(mode01_data_t *data, uint8_t pid, uint32_t bitmap);

#endif
