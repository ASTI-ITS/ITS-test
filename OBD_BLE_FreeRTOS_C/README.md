# OBD_BLE FreeRTOS C conversion

This is a **pure C / ESP-IDF FreeRTOS** conversion of the supplied Arduino/C++
OBD_BLE project. The logical source layout is preserved from the original
include paths:

```text
OBD_BLE_FreeRTOS_C/
├── CMakeLists.txt
├── sdkconfig.defaults
└── main/
    ├── CMakeLists.txt
    ├── app_main.c
    ├── BLE/
    │   ├── BLEServer.h
    │   ├── ELM327_BLE.c
   │   └── ELM327_BLE.h
    ├── Core/
    │   ├── Formula.h
    │   ├── OBD2.c
    │   ├── OBD2.h
    │   ├── PIDTable.c
            │   └── PIDTable.h
    ├── Data/
    │   ├── Mode01Data.c
    │   └── Mode01Data.h
    ├── Decoder/
    │   ├── Decoder.c
    │   └── Decoder.h
    └── Transport/
        ├── MCP2515Transport.c
            └── MCP2515Transport.h
```

## Architecture

The conversion intentionally removes C++ classes, `String`, Arduino `loop()`,
NimBLE-Arduino callbacks, and the C++ `mcp2515` object.

Runtime tasks:

1. **OBD/CAN task** (core 1, priority 20)
   - Owns all MCP2515 SPI access and all mutable OBD protocol state.
   - Drains pending CAN frames.
   - Reassembles ISO-TP.
   - Parses queued BLE commands.
   - Never performs BLE notification or serial printing.

2. **NimBLE host task** (ESP-IDF/NimBLE)
   - Handles GAP/GATT and secure BLE pairing.
   - Starts bonding and authenticated pairing on connect.
   - RX callback copies complete ELM commands into a FreeRTOS queue.
   - Never calls the CAN driver directly.

3. **BLE TX task** (core 0, priority 8)
   - Blocks on the OBD response queue.
   - Adds CR/CRLF and `>` framing.
   - Sends notifications in MTU-sized chunks.

4. **Serial debug task** (core 0, priority 2)
   - Prints formatted OBD responses with the `OBD_OUT` log tag.
   - Receives copies through a bounded static queue; the OBD/CAN task never
     waits for serial output, and debug messages are dropped if that queue fills.

This single-owner design avoids a mutex around the MCP2515 and avoids blocking
the timing-critical receive path on BLE.

## How a request works

At startup, `app_main.c` initializes NVS, creates static command/response
queues and a fixed response pool, starts the MCP2515 SPI transport, initializes
the OBD protocol and decoder, and registers the BLE GATT service. It then
starts the OBD/CAN, BLE TX, and serial debug tasks before starting the NimBLE
host.

The request and response path is:

1. A BLE client connects to the adapter and completes a secure pairing and
   bonding exchange. The BLE stack is configured for MITM-protected pairing
   with encrypted communication and stored bonding keys.
2. After pairing, the client writes an ELM-style command to the Nordic UART
   Service RX characteristic. CR/LF terminates a command; a write boundary
   also accepts clients that omit a terminator.
3. The NimBLE callback copies the command into a queue and returns. The
   OBD/CAN task consumes it. AT commands update adapter settings or return a
   local response; OBD commands are encoded and sent through the MCP2515.
4. Standard OBD requests use CAN ID `0x7DF`; responses are accepted from
   `0x7E8` through `0x7EF`. The OBD task drains received frames, handles
   ISO-TP single- and multi-frame responses (sending flow control when needed),
   updates the data/diagnostic caches, and formats the response.
5. The response is placed in a fixed response-pool slot and copied to the
   non-blocking debug queue. The BLE TX task
   consumes that slot, adds CR or CRLF and the ELM `>` prompt, splits the text
   to fit the negotiated BLE MTU, sends notifications, and returns the slot.

Requests are driven by commands from the BLE client. The firmware does not
currently discover all supported PIDs and poll them periodically in the
background. `PIDTable` and `Decoder` describe and decode supported PID data
when a response is received.

## Pins

Preserved from the supplied sketch:

| MCP2515 | ESP32-S3 GPIO |
|---|---:|
| CS | 5 |
| SCK | 7 |
| MISO | 8 |
| MOSI | 9 |

## CAN bitrate and oscillator

The application currently uses 500 kbit/s for standard 11-bit OBD-II CAN:

```c
#define APP_CAN_BITRATE CAN_BITRATE_500K
```

The MCP2515 oscillator is configured explicitly:

```c
#define MCP2515_OSC_HZ 8000000UL
```

Change it to `16000000UL` if your module has a 16 MHz crystal.

## Build

Requires ESP-IDF with NimBLE enabled.

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

`sdkconfig.defaults` sets the FreeRTOS tick to 1000 Hz and enables NimBLE.

## Preserved protocol features

- Nordic UART Service BLE UUIDs
- BLE device name `OBDII`
- Secure pairing with MITM protection, bonding, and encrypted transport
- MTU-aware TX chunking up to 244 bytes
- ELM-style echo, CR/CRLF, and `>` prompt
- `010C`, `010D`, `0902`, Mode 02/03/04/07/09/0A
- 11-bit OBD request/response IDs
- ISO-TP single frame / first frame / consecutive frame handling
- Flow-control frame generation
- PID cache and decoder
- VIN / calibration ID cache
- Stored, pending, and permanent DTC caches

Custom raw-broadcast commands currently map as follows:

| Commands | CAN ID |
|---|---:|
| `NN12`, `NV12` | `0x180` |
| `NN14`, `NV14` | `0x60D` |
| `MO23`, `MM23`, `MO25`, `MM25` | `0x208` |

Other custom commands, including the placeholder Toyota branch, are unsupported.

## Optimization choices

- Static FreeRTOS queues and a fixed response pool: no runtime heap fragmentation.
- Response queues carry one-byte slot indexes instead of copying 1600-byte response objects.
- Fixed-size C strings: no Arduino `String` heap churn.
- One owner of OBD state and MCP2515 SPI: no transport mutex and no races.
- No printing in the CAN drain loop.
- BLE notification moved to a lower-priority task on the other core.
- BLE TX streams MTU-sized chunks from a 244-byte scratch buffer instead of building another 1600-byte packet on the task stack.
- MCP2515 SPI uses polling transactions for small deterministic transfers.
- CAN receive drains both MCP2515 RX buffers before the task idles.
- MCP2515 hardware filters reject unrelated bus traffic:
  `0x7E8..0x7EF`, `0x60D`, and `0x208` are accepted.
- 1 ms FreeRTOS tick fallback when the bus is idle.

## Important MCP2515 note

The original design does not provide an MCP2515 `INT` GPIO. Therefore this
conversion preserves polling. For very high raw bus utilization, wiring the
MCP2515 `INT` pin to an ESP32 GPIO and waking the OBD task with a direct task
notification is the next optimization; it avoids both idle polling and RX
latency.
