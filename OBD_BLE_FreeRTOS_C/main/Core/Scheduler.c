#include "Scheduler.h"

#include <string.h>

#include "PIDTable.h"
#include "esp_timer.h"

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

void scheduler_init(scheduler_t *scheduler) {
    if (!scheduler) return;
    memset(scheduler, 0, sizeof(*scheduler));
    scheduler->state = DISCOVERY_PID_00;
}

void scheduler_begin(scheduler_t *scheduler, mode01_data_t *data) {
    if (!scheduler) return;
    scheduler_init(scheduler);
    scheduler->vehicle_data = data;
}

uint8_t scheduler_discovery_pid(const scheduler_t *scheduler) {
    if (!scheduler) return 0;

    switch (scheduler->state) {
        case DISCOVERY_PID_00: return 0x00;
        case DISCOVERY_PID_20: return 0x20;
        case DISCOVERY_PID_40: return 0x40;
        case DISCOVERY_PID_60: return 0x60;
        case DISCOVERY_PID_80: return 0x80;
        default: return 0;
    }
}

bool scheduler_discovery_complete(const scheduler_t *scheduler) {
    return scheduler && scheduler->state == SCHEDULER_RUNNING;
}

static void add_pid(scheduler_t *scheduler, uint8_t pid) {
    if (!scheduler || scheduler->pid_count >= MAX_ACTIVE_PIDS) return;
    if (pid_table[pid].formula == FORMULA_NONE) return;

    pid_request_t *request = &scheduler->pid_list[scheduler->pid_count++];
    request->pid = pid;
    request->interval_ms = pid_table[pid].interval_ms;
    request->priority =
        request->interval_ms <= 100 ? 3 :
        request->interval_ms <= 500 ? 2 : 1;
    request->last_request_ms = 0;
}

static void build_pid_list(scheduler_t *scheduler) {
    if (!scheduler || !scheduler->vehicle_data) return;

    scheduler->pid_count = 0;
    scheduler->current_index = 0;

    for (uint16_t pid = 1; pid < 256; ++pid) {
        if (scheduler->vehicle_data->supported_pid[pid]) {
            add_pid(scheduler, (uint8_t)pid);
        }
    }
}

void scheduler_process_discovery(scheduler_t *scheduler) {
    if (!scheduler || !scheduler->vehicle_data) return;

    switch (scheduler->state) {
        case DISCOVERY_PID_00: scheduler->state = DISCOVERY_PID_20; break;
        case DISCOVERY_PID_20: scheduler->state = DISCOVERY_PID_40; break;
        case DISCOVERY_PID_40: scheduler->state = DISCOVERY_PID_60; break;
        case DISCOVERY_PID_60: scheduler->state = DISCOVERY_PID_80; break;
        case DISCOVERY_PID_80:
            build_pid_list(scheduler);
            scheduler->state = SCHEDULER_RUNNING;
            break;
        default: break;
    }
}

bool scheduler_request_ready(scheduler_t *scheduler) {
    if (!scheduler || scheduler->state != SCHEDULER_RUNNING ||
        scheduler->pid_count == 0) {
        return false;
    }

    pid_request_t *request = &scheduler->pid_list[scheduler->current_index];
    return (uint32_t)(now_ms() - request->last_request_ms) >= request->interval_ms;
}

uint8_t scheduler_next_pid(scheduler_t *scheduler) {
    if (!scheduler || scheduler->pid_count == 0) return 0;

    pid_request_t *request = &scheduler->pid_list[scheduler->current_index];
    request->last_request_ms = now_ms();
    uint8_t pid = request->pid;

    scheduler->current_index++;
    if (scheduler->current_index >= scheduler->pid_count) {
        scheduler->current_index = 0;
    }

    return pid;
}
