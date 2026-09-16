#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "Core/OBD2.h"
#include "BLE/ELM327_BLE.h"
#include "Transport/MCP2515Transport.h"

/*
 * Pin assignment preserved from the supplied XIAO ESP32-S3 sketch.
 */
#define MCP2515_CS_PIN          5
#define MCP2515_SCK_PIN         7
#define MCP2515_MISO_PIN        8
#define MCP2515_MOSI_PIN        9

/*
 * Set this to 16000000UL if your MCP2515 module has a 16 MHz crystal.
 */
#define MCP2515_OSC_HZ          8000000UL


#define APP_CAN_BITRATE         CAN_BITRATE_500K

#define OBD_TASK_STACK          8192
#define BLE_TX_TASK_STACK       4096
#define OBD_TASK_PRIORITY       20
#define BLE_TX_TASK_PRIORITY    8

static const char *TAG = "APP";

static mcp2515_transport_t s_can;
static obd2_t s_obd;
static elm327_ble_t s_ble;

static StaticQueue_t s_command_queue_storage;
static uint8_t s_command_queue_buffer[
    ELM_BLE_COMMAND_QUEUE_SIZE * sizeof(elm_command_t)
];

static obd_response_t s_response_slots[OBD_RESPONSE_QUEUE_SIZE];
static obd_response_pool_t s_response_pool;

static StaticQueue_t s_response_free_queue_storage;
static uint8_t s_response_free_queue_buffer[
    OBD_RESPONSE_QUEUE_SIZE * sizeof(uint8_t)
];

static StaticQueue_t s_response_ready_queue_storage;
static uint8_t s_response_ready_queue_buffer[
    OBD_RESPONSE_QUEUE_SIZE * sizeof(uint8_t)
];

static QueueHandle_t s_command_queue;
static QueueHandle_t s_response_free_queue;
static QueueHandle_t s_response_ready_queue;

static void obd_task(void *param) {
    obd2_t *obd = (obd2_t *)param;
    elm_command_t command;

    while (1) {
        bool did_work = obd2_poll(obd);

        /*
         * Drain all BLE commands already queued, but command execution stays
         * on this one task. That means all OBD protocol state and all MCP2515
         * SPI access are single-owner: no CAN mutex is required.
         */
        while (xQueueReceive(s_command_queue, &command, 0) == pdTRUE) {
            (void)obd2_command(obd, command.text);
            did_work = true;

            /*
             * Immediately drain CAN again after each command so a fast ECU
             * response is handled before processing another command.
             */
            (void)obd2_poll(obd);
        }

        /*
         * With CONFIG_FREERTOS_HZ=1000 this is a 1 ms idle backoff.
         * Do not delay while frames/commands are actively being processed.
         */
        if (obd2_busy(obd)) {
            /*
             * Stay responsive during a request / ISO-TP burst. Request timeout
             * is bounded, so this cannot starve the idle task indefinitely.
             */
            taskYIELD();
        } else if (!did_work) {
            vTaskDelay(1);
        } else {
            taskYIELD();
        }
    }
}

static void init_nvs(void) {
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }
}

void app_main(void) {
    init_nvs();

    s_command_queue = xQueueCreateStatic(
        ELM_BLE_COMMAND_QUEUE_SIZE,
        sizeof(elm_command_t),
        s_command_queue_buffer,
        &s_command_queue_storage
    );

    s_response_free_queue = xQueueCreateStatic(
        OBD_RESPONSE_QUEUE_SIZE,
        sizeof(uint8_t),
        s_response_free_queue_buffer,
        &s_response_free_queue_storage
    );

    s_response_ready_queue = xQueueCreateStatic(
        OBD_RESPONSE_QUEUE_SIZE,
        sizeof(uint8_t),
        s_response_ready_queue_buffer,
        &s_response_ready_queue_storage
    );

    if (!s_command_queue || !s_response_free_queue || !s_response_ready_queue) {
        ESP_LOGE(TAG, "Failed to create static FreeRTOS queues");
        return;
    }

    if (!obd_response_pool_init(
            &s_response_pool,
            s_response_slots,
            OBD_RESPONSE_QUEUE_SIZE,
            s_response_free_queue,
            s_response_ready_queue)) {
        ESP_LOGE(TAG, "Failed to initialize static response pool");
        return;
    }

    if (!mcp2515_transport_init_bus(
            &s_can,
            SPI2_HOST,
            MCP2515_SCK_PIN,
            MCP2515_MISO_PIN,
            MCP2515_MOSI_PIN,
            MCP2515_CS_PIN,
            MCP2515_OSC_HZ)) {
        ESP_LOGE(TAG, "MCP2515 SPI bus initialization failed");
        return;
    }

    if (!obd2_init(
            &s_obd,
            &s_can,
            &s_response_pool,
            APP_CAN_BITRATE)) {
        ESP_LOGE(TAG, "MCP2515/OBD initialization failed");
        return;
    }

    if (!elm327_ble_init(
            &s_ble,
            s_command_queue,
            &s_response_pool,
            "OBDII")) {
        ESP_LOGE(TAG, "NimBLE initialization failed");
        return;
    }

    BaseType_t ok;

    ok = xTaskCreatePinnedToCore(
        obd_task,
        "obd_can",
        OBD_TASK_STACK,
        &s_obd,
        OBD_TASK_PRIORITY,
        NULL,
        1
    );

    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create OBD/CAN task");
        return;
    }

    ok = xTaskCreatePinnedToCore(
        elm327_ble_tx_task,
        "ble_tx",
        BLE_TX_TASK_STACK,
        &s_ble,
        BLE_TX_TASK_PRIORITY,
        NULL,
        0
    );

    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create BLE TX task");
        return;
    }

    /*
     * NimBLE host runs independently. It receives GATT writes and only copies
     * commands into a FreeRTOS queue; it never calls OBD/CAN directly.
     */
    elm327_ble_start_host();

    ESP_LOGI(
        TAG,
        "Started OBD BLE scanner, CAN=%d bit/s, MCP2515 osc=%lu Hz",
        (int)APP_CAN_BITRATE,
        (unsigned long)MCP2515_OSC_HZ
    );
}
