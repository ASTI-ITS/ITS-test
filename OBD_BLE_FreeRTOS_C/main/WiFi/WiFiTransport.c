#include "WiFiTransport.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "sys/time.h"

#include "../BLE/ELM327_BLE.h"

static const char *TAG = "WIFI_TRANSPORT";

typedef struct {
    QueueHandle_t command_queue;
    obd_response_pool_t *response_pool;
} wifi_server_context_t;

static wifi_server_context_t s_server_context;

static void queue_wifi_command(QueueHandle_t command_queue, const char *command) {
    if (!command_queue || !command) return;

    elm_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    snprintf(cmd.text, sizeof(cmd.text), "%.127s", command);

    /* Trim trailing spaces/newlines before handing it to the OBD task. */
    char *trim_end = cmd.text + strlen(cmd.text);
    while (trim_end > cmd.text &&
           (trim_end[-1] == '\r' || trim_end[-1] == '\n' ||
            trim_end[-1] == ' ' || trim_end[-1] == '\t')) {
        *--trim_end = '\0';
    }

    if (cmd.text[0] != '\0') {
        (void)xQueueSend(command_queue, &cmd, 0);
    }
}

static bool send_all(int socket_fd, const char *data, size_t length) {
    size_t sent = 0;
    while (sent < length) {
        int result = send(socket_fd, data + sent, length - sent, 0);
        if (result <= 0) return false;
        sent += (size_t)result;
    }
    return true;
}

static bool send_response_byte(
    int socket_fd,
    char *buffer,
    size_t capacity,
    size_t *used,
    char value
) {
    buffer[(*used)++] = value;
    if (*used == capacity) {
        if (!send_all(socket_fd, buffer, *used)) return false;
        *used = 0;
    }
    return true;
}

static bool send_elm_response(int socket_fd, const obd_response_t *response) {
    char buffer[256];
    size_t used = 0;

    for (size_t i = 0; response->text[i] != '\0'; ++i) {
        if (response->text[i] == '\r' || response->text[i] == '\n') {
            if (!send_response_byte(socket_fd, buffer, sizeof(buffer), &used, '\r')) {
                return false;
            }
            if (response->line_feeds &&
                !send_response_byte(socket_fd, buffer, sizeof(buffer), &used, '\n')) {
                return false;
            }
            if (response->text[i] == '\r' && response->text[i + 1] == '\n') ++i;
        } else if (!send_response_byte(
                       socket_fd, buffer, sizeof(buffer), &used, response->text[i])) {
            return false;
        }
    }

    if (!send_response_byte(socket_fd, buffer, sizeof(buffer), &used, '\r')) return false;
    if (response->line_feeds &&
        !send_response_byte(socket_fd, buffer, sizeof(buffer), &used, '\n')) {
        return false;
    }
    if (!send_response_byte(socket_fd, buffer, sizeof(buffer), &used, '>')) return false;
    return used == 0 || send_all(socket_fd, buffer, used);
}

static bool send_ready_responses(
    int socket_fd,
    obd_response_pool_t *response_pool
) {
    uint8_t slot;
    while (xQueueReceive(response_pool->ready_slots, &slot, 0) == pdTRUE) {
        bool sent = slot < response_pool->slot_count &&
                    send_elm_response(socket_fd, &response_pool->slots[slot]);
        if (slot < response_pool->slot_count) {
            (void)xQueueSend(response_pool->free_slots, &slot, portMAX_DELAY);
        }
        if (!sent) return false;
    }
    return true;
}

static void wifi_socket_task(void *arg) {
    wifi_server_context_t *context = (wifi_server_context_t *)arg;
    if (!context || !context->command_queue || !context->response_pool) {
        vTaskDelete(NULL);
        return;
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (server_fd < 0) {
        ESP_LOGE(TAG, "socket() failed");
        vTaskDelete(NULL);
        return;
    }

    int reuse = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(WIFI_SOCKET_PORT),
    };

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) != 0) {
        ESP_LOGE(TAG, "bind() failed on port %d: errno=%d", WIFI_SOCKET_PORT, errno);
        close(server_fd);
        vTaskDelete(NULL);
        return;
    }

    if (listen(server_fd, 1) != 0) {
        ESP_LOGE(TAG, "listen() failed: errno=%d", errno);
        close(server_fd);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Wi‑Fi socket listening on %s:%d", "0.0.0.0", WIFI_SOCKET_PORT);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            ESP_LOGE(TAG, "accept() failed: errno=%d", errno);
            continue;
        }

        struct timeval receive_timeout = {
            .tv_sec = 0,
            .tv_usec = 100000,
        };
        (void)setsockopt(
            client_fd,
            SOL_SOCKET,
            SO_RCVTIMEO,
            &receive_timeout,
            sizeof(receive_timeout)
        );

        char buffer[256];
        char command[128];
        size_t command_length = 0;
        bool previous_was_cr = false;
        bool connected = true;
        while (connected) {
            int received = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
            if (received > 0) {
                for (int i = 0; i < received; ++i) {
                    char value = buffer[i];
                    if (value == '\r') {
                        command[command_length] = '\0';
                        queue_wifi_command(context->command_queue, command);
                        command_length = 0;
                        previous_was_cr = true;
                    } else if (value == '\n') {
                        if (!previous_was_cr) {
                            command[command_length] = '\0';
                            queue_wifi_command(context->command_queue, command);
                            command_length = 0;
                        }
                        previous_was_cr = false;
                    } else {
                        previous_was_cr = false;
                        if (command_length < sizeof(command) - 1) {
                            command[command_length++] = value;
                        }
                    }
                }
            } else if (received == 0) {
                connected = false;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                ESP_LOGW(TAG, "recv() failed: errno=%d", errno);
                connected = false;
            }

            if (!send_ready_responses(client_fd, context->response_pool)) {
                connected = false;
            }
        }

        close(client_fd);
    }
}

bool wifi_transport_init_ap(void) {
    esp_netif_init();
    esp_event_loop_create_default();

    esp_netif_t *netif = esp_netif_create_default_wifi_ap();
    if (netif == NULL) {
        ESP_LOGE(TAG, "Failed to create default Wi‑Fi AP netif");
        return false;
    }

    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&wifi_init_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed");
        return false;
    }

    if (esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_storage failed");
        return false;
    }

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = "OBDII_WIFI",
            .ssid_len = sizeof("OBDII_WIFI") - 1,
            .channel = 1,
            .password = "12345678",
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    if (strlen((const char *)wifi_config.ap.password) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    if (esp_wifi_set_mode(WIFI_MODE_AP) != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode failed");
        return false;
    }

    if (esp_wifi_set_config(WIFI_IF_AP, &wifi_config) != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config failed");
        return false;
    }

    if (esp_wifi_start() != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed");
        return false;
    }

    ESP_LOGI(TAG, "Wi‑Fi AP started: SSID=%s", wifi_config.ap.ssid);
    return true;
}

bool wifi_transport_start_server(QueueHandle_t command_queue, obd_response_pool_t *response_pool) {
    if (!command_queue || !response_pool) {
        return false;
    }

    s_server_context.command_queue = command_queue;
    s_server_context.response_pool = response_pool;

    BaseType_t ok = xTaskCreate(
        wifi_socket_task,
        "wifi_socket",
        4096,
        &s_server_context,
        5,
        NULL
    );

    return ok == pdPASS;
}
