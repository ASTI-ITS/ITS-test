# Nissan NV350 CAN Signal Monitor

This project is a small ESP-IDF firmware for an ESP32-S3 that reads CAN traffic from a Microchip MCP2515 CAN controller over SPI and decodes a handful of Nissan NV350 vehicle signals.

The code is intentionally compact and single-file: the full application logic lives in `main/Nissan_NV350_nonOBDII.c`.

## What the firmware does

At runtime the board:

- initializes the SPI bus to talk to the MCP2515,
- configures the MCP2515 for 500 kbps CAN,
- reads incoming CAN frames from the MCP2515 receive buffers,
- decodes selected CAN IDs into interpreted vehicle signals,
- stores the most recent values in a shared `TransmittedData_t` structure,
- prints the decoded values every 250 ms on a background task.

It focuses on the following signals:

- vehicle speed,
- engine RPM,
- coolant temperature,
- left/right indicators,
- hazard warning detection,
- door status,
- brake indicator,
- rear heated screen,
- side/main/full beam status,
- air-conditioning unit state.

Unknown CAN IDs are logged once and ignored for decoding, which helps identify new Nissan message IDs without breaking the program.

## Hardware setup

The code is written for a XIAO ESP32-S3 connected to an MCP2515 module.

### SPI pins

| Signal | ESP32-S3 GPIO |
|---|---:|
| SCK | 7 |
| MISO | 8 |
| MOSI | 9 |
| CS | 5 |

### CAN configuration

- SPI host: `SPI2_HOST`
- MCP2515 oscillator: `8 MHz`
- CAN bitrate: `500 kbps`
- MCP2515 in SPI mode 0

The firmware assumes the MCP2515 is connected to the vehicle CAN bus and that the bus is a standard 11-bit OBD-style network.

## Main control flow

The program starts in `app_main()`:

1. `SPI_Init()` initializes the ESP32 SPI peripheral.
2. `CANBus_Init()` resets the MCP2515, puts it into configuration mode, sets the bitrate, enables receive mode, and returns it to normal operating mode.
3. `CAN_Reader_Task()` runs continuously and drains CAN frames from the MCP2515.
4. `CAN_Print_Task()` reads the latest decoded values and prints them to stdout.

### CAN reader task

`CAN_Reader_Task()` loops forever, repeatedly calling `CANBus_Read()`. Each call:

- reads one pending MCP2515 RX message,
- extracts the CAN ID and data bytes,
- matches the frame against known IDs,
- decodes the payload into the current signal state,
- stores the result in `currentSensorData`.

The code limits each reader loop to a small batch of frames (`MAX_CAN_FRAMES_PER_LOOP`) so a busy CAN bus does not monopolize the CPU.

### Signal decoding

`CANBus_Read()` switches on the CAN ID and calls the appropriate processor function:

| CAN ID | Parsed value |
|---|---|
| `0x354` | speed |
| `0x23D` | RPM |
| `0x551` | coolant temperature |
| `0x60D` | indicator and door signals |
| `0x180` | brake |
| `0x625` | rear heated screen and beam settings |
| `0x35D` | AC state |

The decoder uses a few helper functions such as endian conversion and bit extraction to turn raw bytes into meaningful values.

## Indicator behavior

The code includes a small state machine for turn signals.

- left/right blinking is detected from CAN bits in the `0x60D` message,
- a hazard signal is recognized when both left and right indicators are active,
- indicator state times out after `3000 ms` if no new valid signal is received.

This prevents stale turn-signal readings from remaining active forever.

## Project layout

```text
Nissan_NV350_nonOBDII/
├── CMakeLists.txt
├── sdkconfig
├── sdkconfig.old
├── build/
└── main/
    └── Nissan_NV350_nonOBDII.c
```

The project builds with ESP-IDF using the standard `project()` CMake pattern.

## Building and flashing

From inside this folder:

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

Adjust the serial port as needed for your environment.

## Typical output

The print task emits output similar to:

```text
Speed: 42 | RPM: 1800 | Coolant: 88 | Left: 0 | Right: 1 | Brake: 0 | Driver Door: 0 | Passenger Door: 0 | Rear Door: 0 | Side Beam: 0 | Main Beam: 0 | Full Beam: 0 | Rear Heated Screen: 0 | AC: 1
```

This is useful for confirming that the MCP2515 is receiving the expected CAN traffic and that the signal mapping is working.

## Important notes

- The code is written for a specific Nissan message set and is not a generic OBD-II scanner.
- It only decodes the CAN IDs defined in the source; other IDs are logged as unknown.
- If your vehicle uses different bit positions or different message IDs, the decode logic will need to be tuned.
- The firmware is designed as a signal observer and debug tool, not as a full ECU communication stack.

## Debugging tips

- Watch the serial monitor for `Unmapped CAN ID=...` messages to discover new frame IDs.
- Verify the MCP2515 wiring and the CAN bus ground before chasing incorrect values.
- Confirm the board is running at 500 kbps and that the MCP2515 oscillator matches your hardware.
- If no frames are decoded, check whether the vehicle bus is actually active and whether the CAN transceiver is connected properly.
