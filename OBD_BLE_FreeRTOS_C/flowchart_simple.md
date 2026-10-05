# OBD_BLE_FreeRTOS Simple Flowchart

This file shows the same system flow as the main flowchart, but with easier wording and fewer technical terms.

## Big-picture view

```mermaid
graph TD
    A[Device reads saved transport; fresh devices default to BLE] --> B{Selected connection?}
    B -- BLE --> C[Phone pairs over encrypted BLE]
    B -- Wi-Fi --> D[Phone joins OBDII_WIFI and connects to TCP port 35000]
    C --> E[Selected transport puts command in shared queue]
    D --> E
    E --> F[Main OBD task on core 1]
    F --> G[Read and understand the command]
    G --> H{What kind of command?}
    H -- ATCM --> I[Save the other transport and queue confirmation]
    I --> J[Send reply, restart, then wait for reconnect]
    J --> A
    H -- Other AT --> K[Update setting and answer locally]
    H -- Normal OBD --> L[Format as CAN request and send to ECU]
    H -- Extra command --> M[Store expected response ID and timeout]
    M --> N[No extra request is sent]
    L --> O[Check for vehicle ECU replies]
    N --> O
    O --> P{Was a reply found or timed out?}
    P -- Reply found --> Q[Format CAN response]
    P -- Extra reply found --> R[Show raw message details]
    P -- No reply --> S[Return NO DATA]
    K --> T[Save answer in a response slot]
    Q --> T
    R --> T
    S --> T
    T --> U[Copy response to the log]
    T --> V[Send response over selected transport]
    V --> W[Add CR/CRLF and prompt]
    W --> X[BLE notification or Wi-Fi TCP response]
```

## Startup flow

```mermaid
graph TD
    A[Program starts] --> B[Read saved transport; default to BLE]
    B --> C[Create queues and response storage]
    C --> D[Start vehicle interface and initialize OBD]
    D --> E{Saved connection mode?}
    E -- BLE --> F[Start BLE and secure pairing]
    E -- Wi-Fi --> G[Start Wi-Fi access point and TCP server]
    F --> H[Start log and OBD tasks]
    G --> H
    H --> I[Ready for a client]
```

## Main task loop

```mermaid
graph TD
    A[Start vehicle task] --> B[Check for new vehicle data]
    B --> C{Any new message?}
    C -- Yes --> D[Read the message]
    D --> E[Break down the message]
    E --> F[Update the current state]
    F --> G[Prepare a reply]
    C -- No --> H[Nothing new yet]
    H --> I{Any command waiting?}
    I -- Yes --> J[Handle the queued command]
    J --> K{ATCM, other AT, or OBD?}
    K -- ATCM --> AA[Save other mode and queue confirmation]
    AA --> AB[Deliver reply, restart, and reconnect]
    K -- Other AT --> AC[Update setting and answer locally]
    K -- OBD --> L{Normal OBD or extra command?}
    L -- Normal OBD --> M[Build and send the vehicle request]
    M --> N[Check again for new data]
    N --> O[Keep looping]
    L -- Extra command --> P[Remember the reply ID and timeout]
    P --> Q[Do not send a new request here]
    Q --> O
    I -- No --> R{Is the system busy?}
    R -- Yes --> S[Give other work a moment]
    R -- No --> T{Did we do useful work?}
    T -- No --> U[Wait a tiny moment]
    T -- Yes --> S
```

## BLE and Wi-Fi send/receive path

```mermaid
graph TD
    A[Phone sends command over BLE or Wi-Fi TCP] --> B[Collect command until CR/LF]
    B --> C[Put command in shared waiting list]
    C --> D[Main task takes and processes it]
    D --> E{Is it ATCM?}
    E -- Yes --> F[Save other mode and queue confirmation]
    F --> G[Deliver reply, restart, and reconnect]
    E -- No --> H[Create response in fixed response space]
    H --> I[Copy response to log and response queues]
    I --> J[Selected transport adds ELM framing]
    J --> K[BLE notification or Wi-Fi TCP response]
```

## Vehicle request and reply path

```mermaid
graph TD
    A[Main task receives a request] --> B{Normal OBD or extra command?}
    B -- Normal OBD --> C[Build the vehicle message]
    B -- Extra command --> D[Store the expected reply ID]
    D --> E[Wait for the matching reply]
    E --> X[No second request is sent here]
    C --> F[Send it to the vehicle interface]
    F --> G[Wait until the chip is ready]
    G --> H[Write the message to the chip]
    H --> I[Send the transmit request]
    I --> J[Wait for the message to finish]
    J --> K[Done]

    K --> L[Vehicle answers]
    L --> M[Check the vehicle interface for new data]
    M --> N[Read the message ID and content]
    N --> O[Break down the data]
    O --> P[Make sense of the reply]
    P --> Q[Build a clean answer for the app]
```

## Request checking flow

```mermaid
graph TD
    A[Command arrives] --> B[Clean up the text]
    B --> C{Is it an AT command?}
    C -- Yes --> D{Is it ATCM?}
    D -- Yes --> E[Save other transport and queue confirmation]
    E --> F[Reply, restart, and reconnect]
    D -- No --> G[Handle other AT settings locally]
    C -- No --> H{Does it match a special extra command?}
    H -- Yes --> I[Follow the special-response path]
    H -- No --> J{Is it a valid supported command?}
    J -- Yes --> K[Build the normal request]
    K --> L[Send it to the vehicle]
    J -- No --> M[Return a question-mark error]
```

## Extra command path

```mermaid
graph TD
    A[Extra command comes in] --> B[Fix the text case and spaces]
    B --> C{Is it a short known command?}
    C -- No --> D[Try normal OBD parsing and return an error if invalid]
    C -- Yes --> E[Pick the reply ID to expect]
    E --> F{Is another request still waiting?}
    F -- Yes --> G[Send busy response]
    F -- No --> H[Save the command, reply ID, and time limit]
    H --> I[No message is sent by this path]
    I --> J[Check for incoming vehicle messages]
    J --> K{Did a message match the expected ID?}
    K -- Yes --> L[Format the raw data into a reply]
    L --> M[Send it back to the app and log it]
    K -- No --> N{Has time run out?}
    N -- No --> J
    N -- Yes --> O[Return NO DATA]

    E -. Example: one command expects ID 0x180 .-> P[Wait for the matching vehicle reply]
    E -. Example: another expects ID 0x60D .-> Q[Wait for that reply]
    E -. Example: others expect ID 0x208 .-> R[Wait for that reply]
```

## Simple execution model

- The program reads the saved transport from NVS; a fresh device defaults to BLE.
- The BLE stack is configured for secure pairing and bonding.
- Wi-Fi mode starts the `OBDII_WIFI` access point and TCP server.
- The main task checks for new messages from the vehicle.
- BLE and Wi-Fi clients both send commands to the same waiting list.
- The main task reads the command and decides what to do.
- `ATCM` saves the other transport, sends a confirmation, then restarts into it.
- Normal vehicle requests are sent to the car.
- Extra commands mostly remember what reply to look for and wait.
- When a reply arrives, it is cleaned up and sent back through the selected transport.
- A small background task writes logs without slowing the vehicle work.

## Main files

- [main/app_main.c](main/app_main.c) — startup, queue setup, and task setup
- [main/Transport/MCP2515Transport.c](main/Transport/MCP2515Transport.c) — vehicle interface and message transfer
- [main/Core/OBD2.c](main/Core/OBD2.c) — command handling and reply building
- [main/BLE/ELM327_BLE.c](main/BLE/ELM327_BLE.c) — Bluetooth command and notification flow
- [main/WiFi/WiFiTransport.c](main/WiFi/WiFiTransport.c) — Wi-Fi access point and TCP command/response flow

## Summary

The system works in a simple order:

1. The device starts its saved transport; BLE is the fresh-device default.
2. The command is placed in a waiting list.
3. The main task handles AT settings, uses `ATCM` to switch transports, or sends a normal/special request.
4. The vehicle side checks for replies.
5. The response is prepared and returned over BLE or Wi-Fi.
6. A small log task copies the result without slowing the main work.

This keeps the system smooth and avoids stopping the vehicle work while a transport is active.
