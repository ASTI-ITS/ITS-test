#ifndef WIFI_TRANSPORT_H
#define WIFI_TRANSPORT_H

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "../Core/OBD2.h"

#define WIFI_SOCKET_PORT 35000

bool wifi_transport_init_ap(void);
bool wifi_transport_start_server(QueueHandle_t command_queue, obd_response_pool_t *response_pool);

#endif
