#ifndef ELM327_BLE_H
#define ELM327_BLE_H

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "../Core/OBD2.h"

#define ELM_BLE_RX_BUFFER 128
#define ELM_BLE_TX_MAX_PAYLOAD 244
#define ELM_BLE_COMMAND_QUEUE_SIZE 16

typedef struct {
    char text[ELM_BLE_RX_BUFFER];
} elm_command_t;

typedef struct {
    QueueHandle_t command_queue;
    obd_response_pool_t *response_pool;

    volatile bool connected;
    volatile uint16_t conn_handle;
    volatile uint16_t notification_payload;

    uint16_t rx_value_handle;
    uint16_t tx_value_handle;

    char incoming[ELM_BLE_RX_BUFFER];
    uint16_t incoming_length;
} elm327_ble_t;

bool elm327_ble_init(
    elm327_ble_t *ble,
    QueueHandle_t command_queue,
    obd_response_pool_t *response_pool,
    const char *device_name
);

/* Blocking NimBLE host task entry. */
void elm327_ble_host_task(void *param);
void elm327_ble_start_host(void);

/* Sends queued OBD responses without ever blocking the CAN/OBD task. */
void elm327_ble_tx_task(void *param);

bool elm327_ble_is_connected(const elm327_ble_t *ble);

#endif
