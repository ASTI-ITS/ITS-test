#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/spi_master.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"


// ============================================================
// XIAO ESP32-S3 + MCP2515 CONFIGURATION
// ============================================================

#define SPI_HOST                SPI2_HOST

#define SPI_SCK_PIN             7
#define SPI_MISO_PIN            8
#define SPI_MOSI_PIN            9
#define SPI_CS_PIN              5

#define MCP2515_CLOCK_HZ        8000000UL

#define CAN_BITRATE             500000UL

#define INDICATOR_TIMEOUT_MS    3000UL

// ============================================================
// WATCHDOG / TASK CONFIGURATION
// ============================================================

// Maximum number of CAN frames processed in one reader pass.
// Prevents a busy CAN bus from monopolizing the CPU.
#define MAX_CAN_FRAMES_PER_LOOP   16U

// Small delay guarantees that lower-priority tasks and the
// FreeRTOS idle/watchdog tasks get CPU time.
#define CAN_READER_DELAY_MS       10U

#define CAN_READER_PRIORITY       8
#define CAN_PRINT_PRIORITY        2

#define CAN_READER_STACK_SIZE     4096
#define CAN_PRINT_STACK_SIZE      4096


// ============================================================
// MCP2515 SPI COMMANDS
// ============================================================

#define MCP_RESET               0xC0
#define MCP_READ                0x03
#define MCP_WRITE               0x02
#define MCP_BIT_MODIFY          0x05


// ============================================================
// MCP2515 REGISTERS
// ============================================================

#define MCP_CANSTAT             0x0E
#define MCP_CANCTRL             0x0F

#define MCP_CNF3                0x28
#define MCP_CNF2                0x29
#define MCP_CNF1                0x2A

#define MCP_CANINTE             0x2B
#define MCP_CANINTF             0x2C

#define MCP_RXB0CTRL            0x60
#define MCP_RXB0SIDH            0x61

#define MCP_RXB1CTRL            0x70
#define MCP_RXB1SIDH            0x71


// ============================================================
// MCP2515 FLAGS
// ============================================================

#define MCP_RX0IF               0x01
#define MCP_RX1IF               0x02


// ============================================================
// MCP2515 MODES
// ============================================================

#define MCP_MODE_NORMAL         0x00
#define MCP_MODE_CONFIG         0x80


// ============================================================
// LOG TAG
// ============================================================

static const char *TAG = "CAN_BUS";


// ============================================================
// CAN FRAME
// ============================================================

typedef struct
{
    uint32_t can_id;
    uint8_t can_dlc;
    uint8_t data[8];
    bool extended;

} can_frame_t;


// ============================================================
// SIGNAL DATA STRUCTURE
// ============================================================

typedef struct
{
    uint8_t flag;

    uint8_t speed;
    uint16_t rpm;
    uint8_t coolant_temp;

    bool indicating_left;
    bool indicating_right;

    bool indicating_driver_door;
    bool indicating_passenger_door;
    bool indicating_rear_door;

    bool indicating_brake;

    bool indicating_side_beam;
    bool indicating_main_beam;
    bool indicating_full_beam;

    bool rear_heated_screen;
    bool ac_unit;

} TransmittedData_t;


// ============================================================
// GLOBAL DATA
// ============================================================

static TransmittedData_t currentSensorData = {0};

static bool canInitialized = false;

static spi_device_handle_t mcp_spi = NULL;

#define MAX_LOGGED_UNKNOWN_CAN_IDS 32U
static uint32_t loggedUnknownCanIds[MAX_LOGGED_UNKNOWN_CAN_IDS];
static size_t loggedUnknownCanIdCount = 0;


// ============================================================
// INDICATOR STATE MACHINE
// ============================================================

typedef enum
{
    INDICATOR_OFF,
    INDICATOR_LEFT,
    INDICATOR_RIGHT,
    INDICATOR_HAZARD

} IndicatorState;


static IndicatorState indicatorState =
    INDICATOR_OFF;


static int64_t lastIndicatorDetection = 0;


// ============================================================
// TIME FUNCTION
// ============================================================
//
// Equivalent to Arduino millis().
//
// Returns milliseconds since ESP32 startup.
// ============================================================

static uint32_t get_millis(void)
{
    return (uint32_t)(
        esp_timer_get_time() / 1000
    );
}


// ============================================================
// SPI TRANSFER
// ============================================================

static esp_err_t MCP2515_SPI_Transfer(
    const uint8_t *tx,
    uint8_t *rx,
    size_t length)
{
    if (mcp_spi == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (tx == NULL || length == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    spi_transaction_t transaction = {0};

    transaction.length =
        length * 8;

    transaction.tx_buffer =
        tx;

    transaction.rx_buffer =
        rx;

    /*
     * MCP2515 transactions are very small (1-15 bytes).
     * Use the normal synchronous driver transaction, but with
     * DMA disabled (see SPI_Init). This avoids DMA descriptor
     * allocation/free overhead for every register access.
     */
    esp_err_t ret = spi_device_transmit(
        mcp_spi,
        &transaction
    );

    return ret;
}


// ============================================================
// MCP2515 WRITE REGISTER
// ============================================================

static bool MCP2515_WriteRegister(
    uint8_t address,
    uint8_t value)
{
    uint8_t tx[3];

    tx[0] = MCP_WRITE;
    tx[1] = address;
    tx[2] = value;

    return MCP2515_SPI_Transfer(
        tx,
        NULL,
        sizeof(tx)
    ) == ESP_OK;
}


// ============================================================
// MCP2515 READ REGISTER
// ============================================================

static uint8_t MCP2515_ReadRegister(
    uint8_t address)
{
    uint8_t tx[3];
    uint8_t rx[3] = {0};

    tx[0] = MCP_READ;
    tx[1] = address;
    tx[2] = 0x00;

    if (MCP2515_SPI_Transfer(
            tx,
            rx,
            sizeof(tx)) != ESP_OK)
    {
        return 0;
    }

    return rx[2];
}


// ============================================================
// MCP2515 BIT MODIFY
// ============================================================

static bool MCP2515_BitModify(
    uint8_t address,
    uint8_t mask,
    uint8_t value)
{
    uint8_t tx[4];

    tx[0] = MCP_BIT_MODIFY;
    tx[1] = address;
    tx[2] = mask;
    tx[3] = value;

    return MCP2515_SPI_Transfer(
        tx,
        NULL,
        sizeof(tx)
    ) == ESP_OK;
}


// ============================================================
// MCP2515 RESET
// ============================================================

static void MCP2515_Reset(void)
{
    uint8_t command =
        MCP_RESET;

    MCP2515_SPI_Transfer(
        &command,
        NULL,
        1
    );

    vTaskDelay(
        pdMS_TO_TICKS(10)
    );
}


// ============================================================
// MCP2515 SET MODE
// ============================================================

static bool MCP2515_SetMode(
    uint8_t mode)
{
    MCP2515_BitModify(
        MCP_CANCTRL,
        0xE0,
        mode
    );

    vTaskDelay(
        pdMS_TO_TICKS(1)
    );

    uint8_t currentMode =
        MCP2515_ReadRegister(
            MCP_CANSTAT
        ) & 0xE0;

    return currentMode == mode;
}


// ============================================================
// MCP2515 CAN BITRATE
// ============================================================
//
// 8 MHz oscillator
// 500 kbps CAN
//
// CNF1 = 0x00
// CNF2 = 0x91
// CNF3 = 0x01
//
// ============================================================

static void MCP2515_SetBitrate500K(void)
{
    MCP2515_WriteRegister(
        MCP_CNF1,
        0x00
    );

    MCP2515_WriteRegister(
        MCP_CNF2,
        0x91
    );

    MCP2515_WriteRegister(
        MCP_CNF3,
        0x01
    );
}


// ============================================================
// MCP2515 RECEIVE CONFIGURATION
// ============================================================
//
// RXM = 11
//
// Receive all valid CAN messages.
// No CAN ID filtering.
// ============================================================

static void MCP2515_ConfigureReceive(void)
{
    MCP2515_WriteRegister(
        MCP_RXB0CTRL,
        0x60
    );

    MCP2515_WriteRegister(
        MCP_RXB1CTRL,
        0x60
    );

    MCP2515_WriteRegister(
        MCP_CANINTE,
        MCP_RX0IF | MCP_RX1IF
    );

    MCP2515_WriteRegister(
        MCP_CANINTF,
        0x00
    );
}


// ============================================================
// MCP2515 INITIALIZATION
// ============================================================

static bool MCP2515_Init(void)
{
    MCP2515_Reset();


    if (!MCP2515_SetMode(
            MCP_MODE_CONFIG))
    {
        ESP_LOGE(
            TAG,
            "Failed to enter configuration mode"
        );

        return false;
    }


    MCP2515_SetBitrate500K();


    MCP2515_ConfigureReceive();


    if (!MCP2515_SetMode(
            MCP_MODE_NORMAL))
    {
        ESP_LOGE(
            TAG,
            "Failed to enter normal mode"
        );

        return false;
    }


    ESP_LOGI(
        TAG,
        "MCP2515 initialized"
    );

    ESP_LOGI(
        TAG,
        "CAN bitrate: 500 kbps"
    );


    return true;
}


// ============================================================
// PROCESS ENDIAN
// ============================================================

static uint16_t Process_Endian(
    uint8_t msb,
    uint8_t lsb)
{
    return ((uint16_t)msb << 8) |
           lsb;
}


// ============================================================
// PROCESS BIT
// ============================================================

static bool Process_Bit(
    uint8_t byte,
    uint8_t bit_pos)
{
    return (byte >> bit_pos) & 0x01;
}


// ============================================================
// UPDATE PERSISTENT INDICATOR STATE
// ============================================================

static void Update_Indicator_State(
    bool leftDetected,
    bool rightDetected)
{
    /*
     * No valid indicator detection.
     *
     * Do not refresh timeout.
     */

    if (!leftDetected &&
        !rightDetected)
    {
        return;
    }


    /*
     * Valid detection refreshes
     * the 3-second timeout.
     */

    lastIndicatorDetection =
        get_millis();


    /*
     * Both indicators.
     *
     * Hazard.
     */

    if (leftDetected &&
        rightDetected)
    {
        indicatorState =
            INDICATOR_HAZARD;
    }


    /*
     * Left indicator.
     */

    else if (leftDetected)
    {
        indicatorState =
            INDICATOR_LEFT;
    }


    /*
     * Right indicator.
     */

    else if (rightDetected)
    {
        indicatorState =
            INDICATOR_RIGHT;
    }


    /*
     * Update public outputs.
     */

    currentSensorData.indicating_left =
        (
            indicatorState ==
            INDICATOR_LEFT
        ) ||
        (
            indicatorState ==
            INDICATOR_HAZARD
        );


    currentSensorData.indicating_right =
        (
            indicatorState ==
            INDICATOR_RIGHT
        ) ||
        (
            indicatorState ==
            INDICATOR_HAZARD
        );
}


// ============================================================
// CHECK INDICATOR TIMEOUT
// ============================================================

static void Check_Indicator_Timeout(void)
{
    if (indicatorState ==
        INDICATOR_OFF)
    {
        return;
    }


    uint32_t now =
        get_millis();


    uint32_t last =
        (uint32_t)
        lastIndicatorDetection;


    /*
     * Unsigned subtraction also handles
     * timer rollover.
     */

    if ((uint32_t)(
            now - last
        ) >= INDICATOR_TIMEOUT_MS)
    {
        indicatorState =
            INDICATOR_OFF;


        currentSensorData.indicating_left =
            false;

        currentSensorData.indicating_right =
            false;
    }
}


// ============================================================
// COOLANT TEMPERATURE
// CAN ID = 0x551
// ============================================================

static void Process_Coolant_Temp(
    const uint8_t *data)
{
    int16_t temperature =
        (int16_t)data[0] - 40;


    currentSensorData.coolant_temp =
        (uint8_t)temperature;
}


// ============================================================
// SPEED
// CAN ID = 0x354
// ============================================================

static void Process_Speed(
    const uint8_t *data)
{
    const uint8_t SPEED_LSB = 0;
    const uint8_t SPEED_MSB = 1;


    const float scale =
        0.01f;


    uint16_t raw_value =
        Process_Endian(
            data[SPEED_MSB],
            data[SPEED_LSB]
        );


    float speed_kmph =
        raw_value * scale;


    currentSensorData.speed =
        (uint8_t)speed_kmph;
}


// ============================================================
// INDICATORS AND DOORS
// CAN ID = 0x60D
// ============================================================

static void Process_Indicators(
    const uint8_t *data)
{
    /*
     * Turn indicators.
     */

    uint8_t light_byte =
        data[1];


    bool leftDetected =
        Process_Bit(
            light_byte,
            5
        );


    bool rightDetected =
        Process_Bit(
            light_byte,
            6
        );


    Update_Indicator_State(
        leftDetected,
        rightDetected
    );


    /*
     * Door signals.
     */

    uint8_t door_byte =
        data[0];


    currentSensorData
        .indicating_driver_door =
        Process_Bit(
            door_byte,
            4
        );


    currentSensorData
        .indicating_passenger_door =
        Process_Bit(
            door_byte,
            3
        );


    currentSensorData
        .indicating_rear_door =
        Process_Bit(
            door_byte,
            6
        );
}


// ============================================================
// BRAKE
// CAN ID = 0x180
// ============================================================

static void Process_Brake(
    const uint8_t *data)
{
    currentSensorData
        .indicating_brake =
        Process_Bit(
            data[2],
            0
        );
}


// ============================================================
// REAR HEATED SCREEN + LIGHT BEAMS
// CAN ID = 0x625
// ============================================================

static void Process_Rear_Heated_Wind_Screen(
    const uint8_t *data)
{
    currentSensorData
        .rear_heated_screen =
        Process_Bit(
            data[0],
            0
        );


    uint8_t beam_byte =
        data[1];


    currentSensorData
        .indicating_side_beam =
        Process_Bit(
            beam_byte,
            6
        );


    currentSensorData
        .indicating_main_beam =
        Process_Bit(
            beam_byte,
            5
        );


    currentSensorData
        .indicating_full_beam =
        Process_Bit(
            beam_byte,
            4
        );
}


// ============================================================
// AC
// CAN ID = 0x35D
// ============================================================

static void Process_AC_Unit(
    const uint8_t *data)
{
    currentSensorData.ac_unit =
        Process_Bit(
            data[0],
            0
        );
}


// ============================================================
// RPM
// CAN ID = 0x23D
// ============================================================

static void Process_RPM(
    const uint8_t *data)
{
    const uint8_t RPM_LSB = 3;
    const uint8_t RPM_MSB = 4;


    const uint16_t scale = 3;


    uint16_t raw_value =
        Process_Endian(
            data[RPM_MSB],
            data[RPM_LSB]
        );


    currentSensorData.rpm =
        raw_value * scale;
}


// ============================================================
// READ CAN FRAME FROM MCP2515
// ============================================================

static bool MCP2515_ReadMessage(
    can_frame_t *frame)
{
    uint8_t canintf =
        MCP2515_ReadRegister(
            MCP_CANINTF
        );


    uint8_t bufferAddress;
    uint8_t interruptFlag;


    /*
     * RX Buffer 0.
     */

    if (canintf & MCP_RX0IF)
    {
        bufferAddress =
            MCP_RXB0SIDH;

        interruptFlag =
            MCP_RX0IF;
    }


    /*
     * RX Buffer 1.
     */

    else if (canintf & MCP_RX1IF)
    {
        bufferAddress =
            MCP_RXB1SIDH;

        interruptFlag =
            MCP_RX1IF;
    }


    /*
     * Nothing received.
     */

    else
    {
        return false;
    }


    /*
     * Read:
     *
     * SIDH
     * SIDL
     * EID8
     * EID0
     * DLC
     * DATA0...DATA7
     */

    uint8_t tx[15];
    uint8_t rx[15];


    memset(
        tx,
        0,
        sizeof(tx)
    );

    memset(
        rx,
        0,
        sizeof(rx)
    );


    tx[0] =
        MCP_READ;

    tx[1] =
        bufferAddress;


    if (MCP2515_SPI_Transfer(
            tx,
            rx,
            sizeof(tx)
        ) != ESP_OK)
    {
        return false;
    }


    uint8_t sidh =
        rx[2];

    uint8_t sidl =
        rx[3];

    uint8_t eid8 =
        rx[4];

    uint8_t eid0 =
        rx[5];

    uint8_t dlc =
        rx[6];


    /*
     * Determine standard/extended frame.
     */

    frame->extended =
        (sidl & 0x08) != 0;


    if (frame->extended)
    {
        /*
         * 29-bit CAN identifier.
         */

        frame->can_id =
            ((uint32_t)sidh << 21) |
            ((uint32_t)(sidl & 0xE0) << 13) |
            ((uint32_t)(sidl & 0x03) << 16) |
            ((uint32_t)eid8 << 8) |
            eid0;
    }

    else
    {
        /*
         * 11-bit CAN identifier.
         */

        frame->can_id =
            ((uint32_t)sidh << 3) |
            ((sidl >> 5) & 0x07);
    }


    /*
     * DLC.
     */

    frame->can_dlc =
        dlc & 0x0F;


    if (frame->can_dlc > 8)
    {
        frame->can_dlc = 8;
    }


    /*
     * Copy CAN data.
     */

    for (int i = 0;
         i < frame->can_dlc;
         i++)
    {
        frame->data[i] =
            rx[7 + i];
    }


    /*
     * Clear RX interrupt flag.
     */

    if (!MCP2515_BitModify(
            MCP_CANINTF,
            interruptFlag,
            0
        ))
    {
        return false;
    }


    return true;
}


static void Log_Unknown_CAN_Frame(
    const can_frame_t *frame)
{
    for (size_t i = 0; i < loggedUnknownCanIdCount; i++)
    {
        if (loggedUnknownCanIds[i] == frame->can_id)
        {
            return;
        }
    }

    if (loggedUnknownCanIdCount >= MAX_LOGGED_UNKNOWN_CAN_IDS)
    {
        return;
    }

    char dataText[3U * 8U + 1U] = {0};
    size_t used = 0;

    for (uint8_t i = 0; i < frame->can_dlc; i++)
    {
        int written = snprintf(
            dataText + used,
            sizeof(dataText) - used,
            "%s%02X",
            i == 0 ? "" : " ",
            frame->data[i]
        );

        if (written < 0 || (size_t)written >= sizeof(dataText) - used)
        {
            return;
        }

        used += (size_t)written;
    }

    ESP_LOGI(
        TAG,
        "Unmapped CAN ID=0x%lX %s DLC=%u data=[%s]",
        (unsigned long)frame->can_id,
        frame->extended ? "extended" : "standard",
        frame->can_dlc,
        dataText
    );

    loggedUnknownCanIds[loggedUnknownCanIdCount++] = frame->can_id;
}


// ============================================================
// CAN BUS INITIALIZATION
// ============================================================

bool CANBus_Init(void)
{
    /*
     * Reset MCP2515.
     */

    MCP2515_Reset();


    /*
     * Enter configuration mode.
     */

    if (!MCP2515_SetMode(
            MCP_MODE_CONFIG))
    {
        return false;
    }


    /*
     * 500 kbps.
     */

    MCP2515_SetBitrate500K();


    /*
     * Receive all CAN IDs.
     */

    MCP2515_ConfigureReceive();


    /*
     * Enter normal mode.
     */

    if (!MCP2515_SetMode(
            MCP_MODE_NORMAL))
    {
        return false;
    }


    /*
     * Clear signal data.
     */

    memset(
        &currentSensorData,
        0,
        sizeof(currentSensorData)
    );


    indicatorState =
        INDICATOR_OFF;


    lastIndicatorDetection =
        0;


    canInitialized =
        true;


    return true;
}


// ============================================================
// READ AND DECODE CAN FRAME
// ============================================================

bool CANBus_Read(
    TransmittedData_t *out)
{
    if (out == NULL ||
        !canInitialized)
    {
        return false;
    }


    can_frame_t canMsg = {0};


    /*
     * Read one CAN message.
     */

    if (!MCP2515_ReadMessage(
            &canMsg))
    {
        return false;
    }


    /*
     * Validate DLC.
     */

    if (canMsg.can_dlc > 8)
    {
        return false;
    }


    /*
     * Only use 11-bit ID for
     * the signals defined here.
     */

    uint16_t can_id =
        canMsg.can_id & 0x7FF;


    switch (can_id)
    {
        // ------------------------------------------------
        // SPEED
        // ------------------------------------------------

        case 0x354:

            if (canMsg.can_dlc >= 2)
            {
                Process_Speed(
                    canMsg.data
                );
            }

            break;


        // ------------------------------------------------
        // RPM
        // ------------------------------------------------

        case 0x23D:

            if (canMsg.can_dlc >= 5)
            {
                Process_RPM(
                    canMsg.data
                );
            }

            break;


        // ------------------------------------------------
        // COOLANT
        // ------------------------------------------------

        case 0x551:

            if (canMsg.can_dlc >= 1)
            {
                Process_Coolant_Temp(
                    canMsg.data
                );
            }

            break;


        // ------------------------------------------------
        // INDICATORS + DOORS
        // ------------------------------------------------

        case 0x60D:

            if (canMsg.can_dlc >= 2)
            {
                Process_Indicators(
                    canMsg.data
                );
            }

            break;


        // ------------------------------------------------
        // BRAKE
        // ------------------------------------------------

        case 0x180:

            if (canMsg.can_dlc >= 3)
            {
                Process_Brake(
                    canMsg.data
                );
            }

            break;


        // ------------------------------------------------
        // REAR HEATED SCREEN + BEAMS
        // ------------------------------------------------

        case 0x625:

            if (canMsg.can_dlc >= 2)
            {
                Process_Rear_Heated_Wind_Screen(
                    canMsg.data
                );
            }

            break;


        // ------------------------------------------------
        // AC
        // ------------------------------------------------

        case 0x35D:

            if (canMsg.can_dlc >= 1)
            {
                Process_AC_Unit(
                    canMsg.data
                );
            }

            break;


        // ------------------------------------------------
        // OTHER CAN IDs
        // ------------------------------------------------

        default:

            Log_Unknown_CAN_Frame(&canMsg);

            break;
    }


    /*
     * Return latest signal state.
     */

    *out =
        currentSensorData;


    return true;
}


// ============================================================
// GET LATEST SIGNAL DATA
// ============================================================

void CANBus_GetData(
    TransmittedData_t *out)
{
    if (out == NULL)
    {
        return;
    }


    *out =
        currentSensorData;
}


// ============================================================
// CAN READER TASK
// ============================================================

static void CAN_Reader_Task(
    void *pvParameters)
{
    (void)pvParameters;

    TransmittedData_t signals;

    while (1)
    {
        uint8_t framesProcessed = 0;

        /*
         * Process several available CAN frames, but keep the
         * number bounded so a busy CAN bus cannot monopolize
         * the CPU indefinitely.
         */
        while (framesProcessed < MAX_CAN_FRAMES_PER_LOOP)
        {
            if (!CANBus_Read(&signals))
            {
                break;
            }

            framesProcessed++;

            /*
             * Yield periodically even while draining a busy CAN bus.
             * This is important because the MCP2515 is being polled
             * through the ESP-IDF SPI driver.
             */
            if ((framesProcessed & 0x03U) == 0U)
            {
                taskYIELD();
            }
        }

        /*
         * Indicator timeout must be checked even when no CAN
         * message arrives.
         */
        Check_Indicator_Timeout();

        /*
         * Get the latest signal state after timeout processing.
         */
        CANBus_GetData(&signals);

        /*
         * Always give FreeRTOS and the idle/watchdog tasks CPU time.
         */
        vTaskDelay(pdMS_TO_TICKS(CAN_READER_DELAY_MS));
    }
}


// ============================================================
// SIGNAL PRINT TASK
// ============================================================

static void CAN_Print_Task(
    void *pvParameters)
{
    (void)pvParameters;

    TransmittedData_t signals;

    while (1)
    {
        CANBus_GetData(&signals);

        printf(
            "Speed: %u | RPM: %u | Coolant: %u | Left: %d | Right: %d"
            " | Brake: %d | Driver Door: %d | Passenger Door: %d"
            " | Rear Door: %d | Side Beam: %d | Main Beam: %d"
            " | Full Beam: %d | Rear Heated Screen: %d | AC: %d\n",
            signals.speed,
            signals.rpm,
            signals.coolant_temp,
            signals.indicating_left,
            signals.indicating_right,
            signals.indicating_brake,
            signals.indicating_driver_door,
            signals.indicating_passenger_door,
            signals.indicating_rear_door,
            signals.indicating_side_beam,
            signals.indicating_main_beam,
            signals.indicating_full_beam,
            signals.rear_heated_screen,
            signals.ac_unit
        );

        vTaskDelay(pdMS_TO_TICKS(250));
    }
}


// ============================================================
// SPI INITIALIZATION
// ============================================================

static bool SPI_Init(void)
{
    spi_bus_config_t bus_config =
    {
        .mosi_io_num =
            SPI_MOSI_PIN,

        .miso_io_num =
            SPI_MISO_PIN,

        .sclk_io_num =
            SPI_SCK_PIN,

        .quadwp_io_num =
            -1,

        .quadhd_io_num =
            -1,

        .max_transfer_sz =
            32
    };


    /*
     * DMA is intentionally disabled. MCP2515 transactions are only
     * 1-15 bytes, so DMA provides little benefit and adds descriptor
     * allocation/free work inside the SPI driver.
     */
    esp_err_t ret =
        spi_bus_initialize(
            SPI_HOST,
            &bus_config,
            SPI_DMA_DISABLED
        );


    if (ret != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "SPI bus initialization failed"
        );

        return false;
    }


    /*
     * MCP2515 uses SPI Mode 0.
     *
     * 10 MHz SPI clock is used here.
     */

    spi_device_interface_config_t device_config =
    {
        .clock_speed_hz =
            10 * 1000 * 1000,

        .mode =
            0,

        .spics_io_num =
            SPI_CS_PIN,

        .queue_size =
            1
    };


    ret =
        spi_bus_add_device(
            SPI_HOST,
            &device_config,
            &mcp_spi
        );


    if (ret != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to add MCP2515 SPI device"
        );

        return false;
    }


    ESP_LOGI(
        TAG,
        "SPI initialized"
    );


    return true;
}


// ============================================================
// APP MAIN
// ============================================================

void app_main(void)
{
    /*
     * Initialize SPI.
     */

    if (!SPI_Init())
    {
        ESP_LOGE(
            TAG,
            "SPI initialization failed"
        );

        return;
    }


    /*
     * Initialize CAN.
     */

    canInitialized =
        CANBus_Init();


    if (!canInitialized)
    {
        ESP_LOGE(
            TAG,
            "CAN initialization failed"
        );

        return;
    }


    /*
     * CAN reader task.
     *
     * Priority = CAN_READER_PRIORITY
     */

    BaseType_t result =
        xTaskCreate(
            CAN_Reader_Task,
            "CAN_Reader",
            CAN_READER_STACK_SIZE,
            NULL,
            CAN_READER_PRIORITY,
            NULL
        );


    if (result != pdPASS)
    {
        ESP_LOGE(
            TAG,
            "Failed to create CAN reader task"
        );

        return;
    }


    result =
        xTaskCreate(
            CAN_Print_Task,
            "CAN_Print",
            CAN_PRINT_STACK_SIZE,
            NULL,
            CAN_PRINT_PRIORITY,
            NULL
        );


    if (result != pdPASS)
    {
        ESP_LOGE(
            TAG,
            "Failed to create CAN print task"
        );

        return;
    }


    ESP_LOGI(
        TAG,
        "CAN system started"
    );

    ESP_LOGI(
        TAG,
        "CAN bitrate: 500 kbps"
    );

    ESP_LOGI(
        TAG,
        "MCP2515 oscillator: 8 MHz"
    );
}