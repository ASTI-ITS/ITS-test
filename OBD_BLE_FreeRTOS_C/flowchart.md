# OBD_BLE_FreeRTOS Flowchart

This file shows the runtime flow of the ESP-IDF / FreeRTOS firmware in [OBD_BLE_FreeRTOS_C](.).

## High-level architecture

```mermaid
graph TD
    A[app_main loads saved transport from NVS; fresh devices default to BLE] --> B{Selected transport?}
    B -- BLE --> C[Client connects and pairs over secure BLE]
    B -- Wi-Fi --> D[Client joins OBDII_WIFI and opens TCP port 35000]
    C --> E[BLE GATT RX callback]
    D --> F[Wi-Fi TCP server]
    E --> G[Copy command into shared s_command_queue]
    F --> G
    G --> H[OBD task on core 1]
    H --> I[obd2_command]
    I --> J{Command type?}
    J -- ATCM --> K[Toggle saved transport in NVS and queue confirmation]
    K --> L[Wait for response delivery, then restart]
    L --> A
    J -- Other AT --> M[Update adapter setting and build local response]
    J -- Standard OBD --> N[Build and send standard CAN request]
    J -- Custom non-OBD --> O[Store expected response ID and timeout]
    O --> P[No custom CAN request is sent]
    N --> Q[OBD task polls MCP2515]
    P --> Q
    Q --> R{Matching frame or timeout?}
    R -- Standard response --> S[Reassemble ISO-TP and format response]
    R -- Custom response --> T[Format raw CAN ID, DLC, and data]
    R -- Timeout --> U[Build NO DATA response]
    M --> V[Copy response to static response slot]
    S --> V
    T --> V
    U --> V
    V --> W[Zero-wait enqueue to debug log queue]
    V --> X[Queue response slot index]
    X --> Y{Active transport?}
    Y -- BLE --> Z[BLE TX adds ELM framing and sends MTU chunks]
    Y -- Wi-Fi --> AA[Wi-Fi TCP sends ELM-framed response]
```

## Detailed startup flow

```mermaid
graph TD
    A[app_main] --> B[Initialize NVS and load saved transport; default BLE]
    B --> C[Create static queues and response pools]
    C --> D[Initialize MCP2515 SPI and OBD state]
    D --> E{Saved transport?}
    E -- BLE --> F[Initialize BLE and start BLE TX task]
    F --> G[Start NimBLE host]
    E -- Wi-Fi --> H[Start OBDII_WIFI AP and TCP server]
    G --> I[Start log and OBD tasks]
    H --> I
    I --> J[Selected transport ready]
```

## OBD task loop

```mermaid
graph TD
    A[Start obd_task] --> B[obd2_poll]
    B --> C{Any incoming CAN data?}
    C -- Yes --> D[Read CAN frame]
    D --> E[ISO-TP / response parsing]
    E --> F[Update OBD state and caches]
    F --> G[Queue prepared response]
    C -- No --> H[No new work]
    H --> I{Any command in shared s_command_queue?}
    I -- Yes --> J[obd2_command with queued command]
    J --> K{AT, standard OBD, or custom command?}
    K -- ATCM --> AC[Toggle saved transport and queue confirmation]
    AC --> AD[Wait for response delivery, then restart]
    K -- Other AT --> AE[Update setting and queue local response]
    K -- OBD/custom --> L{Standard OBD or custom command?}
    L -- Standard OBD --> M[Build and send standard CAN request]
    M --> N[obd2_poll]
    N --> O[Continue loop]
    L -- Custom --> AA[Record expected CAN response ID and timeout]
    AA --> AB[No custom CAN request is transmitted]
    AB --> O
    I -- No --> P{obd2_busy?}
    P -- Yes --> Q[taskYIELD]
    P -- No --> R{did_work?}
    R -- No --> S[vTaskDelay 1 tick]
    R -- Yes --> Q
```

## BLE / Wi-Fi RX / TX path

```mermaid
graph TD
    A[Client sends a command over BLE or Wi-Fi TCP] --> B{Input transport?}
    B -- BLE --> C[BLE GATT callback collects command]
    B -- Wi-Fi --> D[Wi-Fi server collects command through CR/LF]
    C --> E[Queue command for OBD task]
    D --> E
    E --> F[OBD task processes command]
    F --> G{ATCM command?}
    G -- Yes --> H[Save other transport and queue confirmation]
    H --> I[Deliver reply, restart, and reconnect]
    G -- No --> J[Build normal AT or OBD response]
    J --> K[Queue response slot and debug copy]
    K --> L{Active transport?}
    L -- BLE --> M[BLE TX adds ELM framing and sends MTU chunks]
    L -- Wi-Fi --> N[Wi-Fi server adds ELM framing and sends TCP response]
```

## CAN send / receive path

```mermaid
graph TD
    A[obd2_command] --> B{Is request standard OBD or custom branch?}
    B -- Standard OBD --> C[Build 11-bit CAN request]
    B -- Custom non-OBD --> D[Map command to expected response ID]
    D --> E[Store response ID and start timeout]
    E --> X[No custom CAN request is transmitted]
    C --> F[mcp2515_transport_send]
    F --> G[Wait for TXB0 clear]
    G --> H[Write SIDH/SIDL/DLC/data to MCP2515 TX buffer]
    H --> I[Send RTS to transmit]
    I --> J[Wait until TX request clears]
    J --> K[Return success/failure]

    K --> L[Vehicle responds]
    L --> M[obd2_poll polls MCP2515]
    M --> N[Read CANINTF and RXB0/RXB1]
    N --> O[Decode CAN ID + DLC + data]
    O --> P[Process frame in OBD task]
    P --> Q[Reassemble ISO-TP and format response]
```

## Request parsing flow

```mermaid
graph TD
    A[Command string arrives] --> B[Normalize case and whitespace]
    B --> C{AT command?}
    C -- Yes --> D{Is it ATCM?}
    D -- Yes --> E[Save opposite mode and queue confirmation]
    E --> F[Deliver response, then restart]
    D -- No --> G[Process other AT setting locally]
    C -- No --> H{Matches custom non-OBD mapping?}
    H -- Yes --> I[Enter custom response wait path]
    H -- No --> J{Valid supported OBD service and hex payload?}
    J -- Yes --> K[Build standard OBD request]
    K --> L[Send over CAN]
    J -- No --> M[Queue question-mark error]
```

## Custom / non-OBD command process

```mermaid
graph TD
    A[Custom command arrives over BLE or Wi-Fi] --> B[Normalize to uppercase and remove whitespace]
    B --> C{Four characters and known mapping?}
    C -- No --> D[Fall through to hex OBD parsing and return question mark if invalid]
    C -- Yes --> E[Select expected response CAN ID]
    E --> F{Custom request already pending?}
    F -- Yes --> G[Queue BUSY response]
    F -- No --> H[Save echo, response ID, and start timeout]
    H --> I[No CAN request frame is sent by this code path]
    I --> J[OBD task polls incoming CAN frames]
    J --> K{Frame ID matches expected response ID?}
    K -- Yes --> L[Format raw CAN ID, DLC, and data bytes]
    L --> M[Queue response and serial debug copy]
    K -- No --> N{Timeout expired?}
    N -- No --> J
    N -- Yes --> O[Queue NO DATA]

    E -. NN12 or NV12 .-> P[Expect CAN ID 0x180]
    E -. NN14 or NV14 .-> Q[Expect CAN ID 0x60D]
    E -. MO23, MM23, MO25, or MM25 .-> R[Expect CAN ID 0x208]
```

## Key execution model

- `app_main()` initializes NVS, loads the saved transport (BLE by default), then starts that transport.
- The `obd_can` task owns all CAN and OBD protocol state. It is pinned to core 1.
- BLE GATT and Wi-Fi TCP input both feed the shared command queue.
- BLE notifications and Wi-Fi TCP responses use the same response pool and ELM framing.
- `ATCM` saves the other transport, queues a confirmation, then restarts into that mode.
- The low-priority `obd_serial_log` task prints response copies from a static queue.
- Debug queue writes use zero wait; a full queue drops a log message rather than stalling CAN processing.
- Commands are moved into a FreeRTOS queue instead of calling CAN from a transport callback.
- Responses are stored in a fixed-size pool and queued by slot index to avoid repeated heap allocations.

## File map

- [main/app_main.c](main/app_main.c) — startup, queue creation, task creation, system init
- [main/Transport/MCP2515Transport.c](main/Transport/MCP2515Transport.c) — SPI + MCP2515 send/receive logic
- [main/Core/OBD2.c](main/Core/OBD2.c) — request parsing, ISO-TP handling, PID logic, cache updates
- [main/BLE/ELM327_BLE.c](main/BLE/ELM327_BLE.c) — BLE command receive path and TX notification path
- [main/WiFi/WiFiTransport.c](main/WiFi/WiFiTransport.c) — Wi-Fi AP, TCP command receive, and response path
- The serial logger task is implemented in [main/app_main.c](main/app_main.c).

## Summary

The firmware is designed as a single-owner OBD system:

1. BLE or Wi-Fi receives a command.
2. The command is queued.
3. The OBD task classifies the command. `ATCM` toggles the saved transport and restarts; standard OBD requests are sent on CAN; custom commands only store an expected response ID and timeout.
4. The MCP2515 is polled for incoming frames. Standard replies use ISO-TP; matching custom replies are formatted as raw CAN data.
5. The response is queued for the active transport and copied to the serial logger without waiting.
6. The active BLE or Wi-Fi transport sends the reply to the client.

This structure avoids locking around the MCP2515 and keeps timing-sensitive CAN work off the BLE and Wi-Fi transport paths.
