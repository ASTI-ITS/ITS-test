#ifndef OBD_SCHEDULER_H
#define OBD_SCHEDULER_H

#include <stdbool.h>
#include <stdint.h>

#include "../Data/Mode01Data.h"

#define MAX_ACTIVE_PIDS 64

typedef enum {
    DISCOVERY_PID_00 = 0,
    DISCOVERY_PID_20,
    DISCOVERY_PID_40,
    DISCOVERY_PID_60,
    DISCOVERY_PID_80,
    SCHEDULER_RUNNING
} scheduler_state_t;

typedef struct {
    uint8_t pid;
    uint16_t interval_ms;
    uint8_t priority;
    uint32_t last_request_ms;
} pid_request_t;

typedef struct {
    pid_request_t pid_list[MAX_ACTIVE_PIDS];
    uint8_t pid_count;
    uint8_t current_index;
    scheduler_state_t state;
    uint32_t last_discovery_ms;
    mode01_data_t *vehicle_data;
} scheduler_t;

void scheduler_init(scheduler_t *scheduler);
void scheduler_begin(scheduler_t *scheduler, mode01_data_t *data);
uint8_t scheduler_discovery_pid(const scheduler_t *scheduler);
bool scheduler_discovery_complete(const scheduler_t *scheduler);
void scheduler_process_discovery(scheduler_t *scheduler);
bool scheduler_request_ready(scheduler_t *scheduler);
uint8_t scheduler_next_pid(scheduler_t *scheduler);

#endif
