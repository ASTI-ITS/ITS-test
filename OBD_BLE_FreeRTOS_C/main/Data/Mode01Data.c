#include "Mode01Data.h"

#include <string.h>

void mode01_data_clear(mode01_data_t *data) {
    if (data) memset(data, 0, sizeof(*data));
}

void mode01_data_set_supported_pid(mode01_data_t *data, uint8_t pid, uint32_t bitmap) {
    if (!data) return;

    switch (pid) {
        case 0x00: data->supported_pid_00 = bitmap; break;
        case 0x20: data->supported_pid_20 = bitmap; break;
        case 0x40: data->supported_pid_40 = bitmap; break;
        case 0x60: data->supported_pid_60 = bitmap; break;
        case 0x80: data->supported_pid_80 = bitmap; break;
        default: break;
    }
}
