#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

...........................this is the error...........#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "driver/spi_master.h"
#include "driver/gpio.h"

#include "esp_err.h"
#include "esp_log.h"


// ============================================================
// XIAO ESP32-S3 PIN CONFIGURATION
//
// Original Arduino:
// SPI.begin(7, 8, 9, 5);
//
// SCK  = GPIO 7
// MISO = GPIO 8
// MOSI = GPIO 9
// CS   = GPIO 5
// LED  = GPIO 4
// ============================================================

#define MCP2515_SCK        GPIO_NUM_7
#define MCP2515_MISO       GPIO_NUM_8
#define MCP2515_MOSI       GPIO_NUM_9
#define MCP2515_CS         GPIO_NUM_5

#define LED_PIN            GPIO_NUM_4


// ============================================================
// OBD-II
// ============================================================

#define OBD_REQUEST_ID       0x7DF
#define OBD_RESPONSE_ID      0x7E8

#define OBD_SERVICE_CURRENT  0x01
#define OBD_PID_RPM          0x0C

#define REQUEST_INTERVAL_MS  50


// ============================================================
// MCP2515 COMMANDS
// ============================================================

#define MCP_RESET          0xC0
#define MCP_READ           0x03
#define MCP_WRITE          0x02
#define MCP_BIT_MODIFY     0x05
#define MCP_RTS_TX0        0x81


// ============================================================
// MCP2515 REGISTERS
// ============================================================

// Filter registers
#define MCP_RXF0SIDH       0x00
#define MCP_RXF1SIDH       0x04
#define MCP_RXF2SIDH       0x08
#define MCP_RXF3SIDH       0x10
#define MCP_RXF4SIDH       0x14
#define MCP_RXF5SIDH       0x18

// Mask registers
#define MCP_RXM0SIDH       0x20
#define MCP_RXM1SIDH       0x24

// CAN configuration
#define MCP_CANSTAT        0x0E
#define MCP_CANCTRL        0x0F

#define MCP_CNF3           0x28
#define MCP_CNF2           0x29
#define MCP_CNF1           0x2A

#define MCP_CANINTE        0x2B
#define MCP_CANINTF        0x2C
#define MCP_EFLG           0x2D


// TX Buffer 0
#define MCP_TXB0CTRL       0x30
#define MCP_TXB0SIDH       0x31
#define MCP_TXB0SIDL       0x32
#define MCP_TXB0EID8       0x33
#define MCP_TXB0EID0       0x34
#define MCP_TXB0DLC        0x35
#define MCP_TXB0D0         0x36


// RX Buffer 0
#define MCP_RXB0CTRL       0x60
#define MCP_RXB0SIDH       0x61
#define MCP_RXB0SIDL       0x62
#define MCP_RXB0EID8       0x63
#define MCP_RXB0EID0       0x64
#define MCP_RXB0DLC        0x65
#define MCP_RXB0D0         0x66


// RX Buffer 1
#define MCP_RXB1CTRL       0x70
#define MCP_RXB1SIDH       0x71
#define MCP_RXB1SIDL       0x72
#define MCP_RXB1EID8       0x73
#define MCP_RXB1EID0       0x74
#define MCP_RXB1DLC        0x75
#define MCP_RXB1D0         0x76


// ============================================================
// MCP2515 FLAGS
// ============================================================

#define MCP_RX0IF          0x01
#define MCP_RX1IF          0x02

#define MCP_TXREQ          0x08


// ============================================================
// MCP2515 OPERATING MODES
// ============================================================

#define MCP_MODE_MASK      0xE0

#define MCP_MODE_NORMAL    0x00
#define MCP_MODE_SLEEP     0x20
#define MCP_MODE_LOOPBACK  0x40
#define MCP_MODE_LISTEN    0x60
#define MCP_MODE_CONFIG    0x80


// ============================================================
// CAN FRAME
// ============================================================

typedef struct
{
    uint32_t id;
    uint8_t dlc;
    uint8_t data[8];

} can_frame_t;


// ============================================================
// GLOBALS
// ============================================================

static spi_device_handle_t mcp_spi;

static SemaphoreHandle_t mcp_mutex;

static const char *TAG = "OBD2";


// ============================================================
// SPI TRANSFER
// ============================================================

static esp_err_t mcp_spi_transfer(
    const uint8_t *tx_data,
    uint8_t *rx_data,
    size_t length)
{
    spi_transaction_t trans;

    memset(&trans, 0, sizeof(trans));

    trans.length = length * 8;
    trans.tx_buffer = tx_data;
    trans.rx_buffer = rx_data;

    return spi_device_transmit(
        mcp_spi,
        &trans
    );
}


// ============================================================
// MCP2515 RESET
// ============================================================

static void mcp_reset(void)
{
    uint8_t command = MCP_RESET;

    ESP_ERROR_CHECK(
        mcp_spi_transfer(
            &command,
            NULL,
            1
        )
    );

    // MCP2515 requires time after reset
    vTaskDelay(pdMS_TO_TICKS(10));
}


// ============================================================
// READ ONE REGISTER
// ============================================================

static uint8_t mcp_read_register(uint8_t address)
{
    uint8_t tx[3] = {
        MCP_READ,
        address,
        0x00
    };

    uint8_t rx[3] = {0};

    ESP_ERROR_CHECK(
        mcp_spi_transfer(
            tx,
            rx,
            sizeof(tx)
        )
    );

    return rx[2];
}


// ============================================================
// WRITE ONE REGISTER
// ============================================================

static void mcp_write_register(
    uint8_t address,
    uint8_t value)
{
    uint8_t tx[3] = {
        MCP_WRITE,
        address,
        value
    };

    ESP_ERROR_CHECK(
        mcp_spi_transfer(
            tx,
            NULL,
            sizeof(tx)
        )
    );
}


// ============================================================
// WRITE MULTIPLE REGISTERS
// ============================================================

static void mcp_write_registers(
    uint8_t address,
    const uint8_t *data,
    uint8_t length)
{
    // MCP2515 CAN frame is max 8 bytes.
    // Buffer gives enough space for command + address + data.
    uint8_t tx[16] = {0};

    if (length > 14)
    {
        return;
    }

    tx[0] = MCP_WRITE;
    tx[1] = address;

    memcpy(
        &tx[2],
        data,
        length
    );

    ESP_ERROR_CHECK(
        mcp_spi_transfer(
            tx,
            NULL,
            length + 2
        )
    );
}


// ============================================================
// READ MULTIPLE REGISTERS
// ============================================================

static void mcp_read_registers(
    uint8_t address,
    uint8_t *data,
    uint8_t length)
{
    uint8_t tx[16] = {0};
    uint8_t rx[16] = {0};

    if (length > 14)
    {
        return;
    }

    tx[0] = MCP_READ;
    tx[1] = address;

    ESP_ERROR_CHECK(
        mcp_spi_transfer(
            tx,
            rx,
            length + 2
        )
    );

    memcpy(
        data,
        &rx[2],
        length
    );
}


// ============================================================
// BIT MODIFY
// ============================================================

static void mcp_bit_modify(
    uint8_t address,
    uint8_t mask,
    uint8_t value)
{
    uint8_t tx[4] = {
        MCP_BIT_MODIFY,
        address,
        mask,
        value
    };

    ESP_ERROR_CHECK(
        mcp_spi_transfer(
            tx,
            NULL,
            sizeof(tx)
        )
    );
}


// ============================================================
// SET MCP2515 MODE
// ============================================================

static bool mcp_set_mode(uint8_t mode)
{
    mcp_bit_modify(
        MCP_CANCTRL,
        MCP_MODE_MASK,
        mode
    );

    for (int i = 0; i < 20; i++)
    {
        uint8_t status;

        status = mcp_read_register(
            MCP_CANSTAT
        );

        if ((status & MCP_MODE_MASK) == mode)
        {
            return true;
        }

        vTaskDelay(1);
    }

    return false;
}


// ============================================================
// SET STANDARD 11-BIT CAN ID
//
// Converts:
//      0x7E8
//
// to MCP2515:
//      SIDH
//      SIDL
//      EID8
//      EID0
// ============================================================

static void mcp_set_standard_id(
    uint8_t address,
    uint16_t id)
{
    uint8_t buffer[4];

    buffer[0] = (uint8_t)(id >> 3);

    buffer[1] =
        (uint8_t)((id & 0x07) << 5);

    buffer[2] = 0x00;
    buffer[3] = 0x00;

    mcp_write_registers(
        address,
        buffer,
        sizeof(buffer)
    );
}


// ============================================================
// MCP2515 INITIALIZATION
// ============================================================

static bool mcp2515_init(void)
{
    ESP_LOGI(TAG, "Resetting MCP2515");

    mcp_reset();


    // --------------------------------------------------------
    // Enter configuration mode
    // --------------------------------------------------------

    if (!mcp_set_mode(MCP_MODE_CONFIG))
    {
        ESP_LOGE(
            TAG,
            "Failed to enter MCP2515 configuration mode"
        );

        return false;
    }

    ESP_LOGI(
        TAG,
        "MCP2515 configuration mode OK"
    );


    // --------------------------------------------------------
    // CAN SPEED
    //
    // 1 Mbps
    // MCP2515 oscillator = 8 MHz
    //
    // Same settings used by:
    // autowp/arduino-mcp2515
    //
    // CNF1 = 0x00
    // CNF2 = 0x80
    // CNF3 = 0x80
    // --------------------------------------------------------

    mcp_write_register(
        MCP_CNF1,
        0x00
    );

    mcp_write_register(
        MCP_CNF2,
        0x91
    );

    mcp_write_register(
        MCP_CNF3,
        0x01
    );


    // --------------------------------------------------------
    // Hardware mask 0
    //
    // 0x7FF means compare all 11 bits.
    // --------------------------------------------------------

    mcp_set_standard_id(
        MCP_RXM0SIDH,
        0x7FF
    );


    // --------------------------------------------------------
    // Hardware mask 1
    // --------------------------------------------------------

    mcp_set_standard_id(
        MCP_RXM1SIDH,
        0x7FF
    );


    // --------------------------------------------------------
    // Filter 0
    //
    // Accept only ID 0x7E8
    // --------------------------------------------------------

    mcp_set_standard_id(
        MCP_RXF0SIDH,
        OBD_RESPONSE_ID
    );


    // Filter 1
    mcp_set_standard_id(
        MCP_RXF1SIDH,
        OBD_RESPONSE_ID
    );


    // Filter 2
    mcp_set_standard_id(
        MCP_RXF2SIDH,
        OBD_RESPONSE_ID
    );


    // Filter 3
    mcp_set_standard_id(
        MCP_RXF3SIDH,
        OBD_RESPONSE_ID
    );


    // Filter 4
    mcp_set_standard_id(
        MCP_RXF4SIDH,
        OBD_RESPONSE_ID
    );


    // Filter 5
    mcp_set_standard_id(
        MCP_RXF5SIDH,
        OBD_RESPONSE_ID
    );


    // --------------------------------------------------------
    // RX buffer configuration
    //
    // RXM = 00
    // Use masks and filters.
    // --------------------------------------------------------

    mcp_write_register(
        MCP_RXB0CTRL,
        0x00
    );

    mcp_write_register(
        MCP_RXB1CTRL,
        0x00
    );


    // --------------------------------------------------------
    // Clear interrupt flags
    // --------------------------------------------------------

    mcp_write_register(
        MCP_CANINTF,
        0x00
    );


    // --------------------------------------------------------
    // Enter NORMAL mode
    // --------------------------------------------------------

    if (!mcp_set_mode(MCP_MODE_NORMAL))
    {
        ESP_LOGE(
            TAG,
            "Failed to enter MCP2515 normal mode"
        );

        return false;
    }


    ESP_LOGI(
        TAG,
        "MCP2515 initialized: 1 Mbps"
    );

    return true;
}


// ============================================================
// SEND CAN MESSAGE
// ============================================================

static bool mcp_send_message(
    const can_frame_t *frame)
{
    uint8_t txctrl;

    txctrl =
        mcp_read_register(
            MCP_TXB0CTRL
        );


    // --------------------------------------------------------
    // Check TX buffer
    // --------------------------------------------------------

    if (txctrl & MCP_TXREQ)
    {
        // TX buffer still busy
        return false;
    }


    // --------------------------------------------------------
    // Standard 11-bit CAN identifier
    // --------------------------------------------------------

    uint8_t id_data[4];

    id_data[0] =
        (uint8_t)(frame->id >> 3);

    id_data[1] =
        (uint8_t)((frame->id & 0x07) << 5);

    id_data[2] = 0x00;
    id_data[3] = 0x00;


    mcp_write_registers(
        MCP_TXB0SIDH,
        id_data,
        sizeof(id_data)
    );


    // --------------------------------------------------------
    // DLC
    // --------------------------------------------------------

    uint8_t dlc = frame->dlc;

    if (dlc > 8)
    {
        dlc = 8;
    }

    mcp_write_register(
        MCP_TXB0DLC,
        dlc
    );


    // --------------------------------------------------------
    // CAN DATA
    // --------------------------------------------------------

    mcp_write_registers(
        MCP_TXB0D0,
        frame->data,
        dlc
    );


    // --------------------------------------------------------
    // Request To Send - TX buffer 0
    // --------------------------------------------------------

    uint8_t command =
        MCP_RTS_TX0;

    ESP_ERROR_CHECK(
        mcp_spi_transfer(
            &command,
            NULL,
            1
        )
    );


    return true;
}


// ============================================================
// READ RX BUFFER
// ============================================================

static bool mcp_read_rx_buffer(
    uint8_t sidh_address,
    uint8_t dlc_address,
    uint8_t data_address,
    can_frame_t *frame)
{
    uint8_t id_data[2];

    mcp_read_registers(
        sidh_address,
        id_data,
        2
    );


    // --------------------------------------------------------
    // Convert MCP2515 SIDH/SIDL back to 11-bit ID
    // --------------------------------------------------------

    frame->id =
        ((uint16_t)id_data[0] << 3) |
        (id_data[1] >> 5);


    // --------------------------------------------------------
    // DLC
    // --------------------------------------------------------

    frame->dlc =
        mcp_read_register(
            dlc_address
        ) & 0x0F;


    if (frame->dlc > 8)
    {
        frame->dlc = 8;
    }


    // --------------------------------------------------------
    // CAN DATA
    // --------------------------------------------------------

    if (frame->dlc > 0)
    {
        mcp_read_registers(
            data_address,
            frame->data,
            frame->dlc
        );
    }


    return true;
}


// ============================================================
// READ CAN MESSAGE
// ============================================================

static bool mcp_read_message(
    can_frame_t *frame)
{
    uint8_t flags;

    flags =
        mcp_read_register(
            MCP_CANINTF
        );


    // --------------------------------------------------------
    // RX BUFFER 0
    // --------------------------------------------------------

    if (flags & MCP_RX0IF)
    {
        mcp_read_rx_buffer(
            MCP_RXB0SIDH,
            MCP_RXB0DLC,
            MCP_RXB0D0,
            frame
        );


        // Clear RX0IF
        mcp_bit_modify(
            MCP_CANINTF,
            MCP_RX0IF,
            0x00
        );


        return true;
    }


    // --------------------------------------------------------
    // RX BUFFER 1
    // --------------------------------------------------------

    if (flags & MCP_RX1IF)
    {
        mcp_read_rx_buffer(
            MCP_RXB1SIDH,
            MCP_RXB1DLC,
            MCP_RXB1D0,
            frame
        );


        // Clear RX1IF
        mcp_bit_modify(
            MCP_CANINTF,
            MCP_RX1IF,
            0x00
        );


        return true;
    }


    return false;
}


// ============================================================
// OBD-II REQUEST TASK
//
// Arduino equivalent:
//
// if (millis() - lastRequest >= 50)
// {
//     mcp2515.sendMessage(&txFrame);
// }
//
// FreeRTOS handles the timing.
// ============================================================

static void obd_request_task(
    void *pvParameters)
{
    can_frame_t txFrame = {

        .id = OBD_REQUEST_ID,

        .dlc = 8,

        .data = {
            0x02,       // Number of following bytes
            0x01,       // Mode 01 - Current Data
            0x0C,       // PID 0C - Engine RPM
            0x00,
            0x00,
            0x00,
            0x00,
            0x00
        }
    };


    TickType_t lastWakeTime;

    lastWakeTime =
        xTaskGetTickCount();


    while (1)
    {
        // Same behavior as original:
        // request sent -> LED LOW

        gpio_set_level(
            LED_PIN,
            0
        );


        // ----------------------------------------------------
        // MCP2515 is shared with RX task.
        // Lock SPI access.
        // ----------------------------------------------------

        if (xSemaphoreTake(
                mcp_mutex,
                pdMS_TO_TICKS(20))
            == pdTRUE)
        {
            bool sent;

            sent =
                mcp_send_message(
                    &txFrame
                );


            xSemaphoreGive(
                mcp_mutex
            );


            if (!sent)
            {
                ESP_LOGW(
                    TAG,
                    "TX buffer busy"
                );
            }
        }


        // ----------------------------------------------------
        // Exactly one request every 50 ms
        //
        // 20 requests / second
        // ----------------------------------------------------

        xTaskDelayUntil(
            &lastWakeTime,
            pdMS_TO_TICKS(
                REQUEST_INTERVAL_MS
            )
        );
    }
}


// ============================================================
// CAN RECEIVE TASK
// ============================================================

static void can_receive_task(
    void *pvParameters)
{
    can_frame_t rxFrame;

    uint16_t engineRPM;


    while (1)
    {
        bool received = false;


        // ----------------------------------------------------
        // Access shared MCP2515
        // ----------------------------------------------------

        if (xSemaphoreTake(
                mcp_mutex,
                pdMS_TO_TICKS(20))
            == pdTRUE)
        {
            received =
                mcp_read_message(
                    &rxFrame
                );


            xSemaphoreGive(
                mcp_mutex
            );
        }


        // ----------------------------------------------------
        // Process CAN frame
        // ----------------------------------------------------

        if (received)
        {
            // Hardware filter already limits reception
            // to 0x7E8, but checking again is safer.

            if (
                rxFrame.id == OBD_RESPONSE_ID &&
                rxFrame.dlc >= 5 &&
                rxFrame.data[1] == 0x41 &&
                rxFrame.data[2] == OBD_PID_RPM
            )
            {
                // ------------------------------------------------
                // OBD-II RPM formula:
                //
                // RPM = ((A * 256) + B) / 4
                //
                // A = data[3]
                // B = data[4]
                // ------------------------------------------------

                engineRPM =
                    (
                        ((uint16_t)rxFrame.data[3] << 8)
                        |
                        rxFrame.data[4]
                    ) / 4;


                printf(
                    "Engine RPM: %u\n",
                    engineRPM
                );


                // Same as original:
                // valid RPM -> LED HIGH

                gpio_set_level(
                    LED_PIN,
                    1
                );
            }
        }


        // One RTOS tick.
        //
        // Avoids continuously occupying CPU while still
        // checking MCP2515 frequently.

        vTaskDelay(1);
    }
}


// ============================================================
// SPI INITIALIZATION
// ============================================================

static void spi_init(void)
{
    // --------------------------------------------------------
    // Configure SPI bus
    // --------------------------------------------------------

    spi_bus_config_t bus_config = {

        .mosi_io_num = MCP2515_MOSI,
        .miso_io_num = MCP2515_MISO,
        .sclk_io_num = MCP2515_SCK,

        .quadwp_io_num = -1,
        .quadhd_io_num = -1,

        .max_transfer_sz = 32
    };


    ESP_ERROR_CHECK(
        spi_bus_initialize(
            SPI2_HOST,
            &bus_config,
            SPI_DMA_CH_AUTO
        )
    );


    // --------------------------------------------------------
    // Add MCP2515 to SPI bus
    //
    // MCP2515 supports SPI mode 0.
    //
    // SPI clock = 8 MHz.
    // --------------------------------------------------------

    spi_device_interface_config_t device_config = {

        .clock_speed_hz = 8 * 1000 * 1000,

        .mode = 0,

        .spics_io_num = MCP2515_CS,

        .queue_size = 1
    };


    ESP_ERROR_CHECK(
        spi_bus_add_device(
            SPI2_HOST,
            &device_config,
            &mcp_spi
        )
    );


    ESP_LOGI(
        TAG,
        "SPI initialized"
    );

    ESP_LOGI(
        TAG,
        "SCK=%d MISO=%d MOSI=%d CS=%d",
        MCP2515_SCK,
        MCP2515_MISO,
        MCP2515_MOSI,
        MCP2515_CS
    );
}


// ============================================================
// APPLICATION ENTRY POINT
// ============================================================

void app_main(void)
{
    printf("\n");
    printf("=============================\n");
    printf(" ESP32-S3 MCP2515 OBD2 RPM\n");
    printf("=============================\n\n");


    // ========================================================
    // GPIO INITIALIZATION
    // ========================================================

    ESP_ERROR_CHECK(
        gpio_set_direction(
            LED_PIN,
            GPIO_MODE_OUTPUT
        )
    );


    ESP_ERROR_CHECK(
        gpio_set_level(
            LED_PIN,
            0
        )
    );


    printf("1. GPIO Ready\n");


    // ========================================================
    // SPI
    // ========================================================

    spi_init();

    printf("2. SPI Begun\n");


    // ========================================================
    // MUTEX
    //
    // Both FreeRTOS tasks communicate with the same MCP2515.
    // ========================================================

    mcp_mutex =
        xSemaphoreCreateMutex();


    if (mcp_mutex == NULL)
    {
        ESP_LOGE(
            TAG,
            "Failed to create MCP2515 mutex"
        );

        return;
    }


    // ========================================================
    // MCP2515
    // ========================================================

    printf("3. Initializing MCP2515\n");


    if (!mcp2515_init())
    {
        ESP_LOGE(
            TAG,
            "MCP2515 initialization failed"
        );

        return;
    }


    printf("4. MCP2515 Initialization Complete\n");


    // ========================================================
    // CREATE OBD REQUEST TASK
    // ========================================================

    BaseType_t result;


    result =
        xTaskCreate(
            obd_request_task,
            "obd_request_task",
            4096,
            NULL,
            5,
            NULL
        );


    if (result != pdPASS)
    {
        ESP_LOGE(
            TAG,
            "Failed to create OBD request task"
        );

        return;
    }


    // ========================================================
    // CREATE CAN RECEIVE TASK
    //
    // Receive task is slightly higher priority so an incoming
    // CAN response can be handled promptly.
    // ========================================================

    result =
        xTaskCreate(
            can_receive_task,
            "can_receive_task",
            4096,
            NULL,
            6,
            NULL
        );


    if (result != pdPASS)
    {
        ESP_LOGE(
            TAG,
            "Failed to create CAN receive task"
        );

        return;
    }


    printf("5. FreeRTOS Tasks Started\n");
    printf("6. Waiting for OBD-II RPM response...\n\n");
}