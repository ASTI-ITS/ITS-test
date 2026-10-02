# OBD_BLE_FreeRTOS Simple Flowchart

This file shows the same system flow as the main flowchart, but with easier wording and fewer technical terms.

## Big-picture view

```mermaid
graph TD
    A[Phone app starts BLE connection] --> B[Secure pairing and bonding]
    B --> C[Encrypted BLE link ready]
    C --> D[Phone app sends a command]
    D --> E[Bluetooth receives it]
    E --> F[Save command in waiting list]
    F --> G[Main task on second core core 1_]
    G --> H[Read and understand the command]
    H --> I{What kind of command?}
    I -- AT --> J[Answer it right away]
    I -- Normal OBD --> K[Format as CAN request frame and send to vehicle ECU]
    I -- Extra command --> L[Store expected response ID and timeout]
    L --> M[No extra request is sent]
    K --> N[Check for vehicle ECU replies]
    M --> N
    N --> O{Was a reply found or timed out?}
    O -- Reply found --> P[Format CAN response]
    O -- Extra reply found --> Q[Show raw message details]
    O -- No reply --> R[Return NO DATA]
    J --> S[Save answer in a response slot]
    P --> S
    Q --> S
    R --> S
    S --> T[Quickly send a copy to the log]
    T --> U[Small debug print task]
    U --> V[Write log line]
    S --> W[Send response slot number]
    W --> X[Bluetooth send task on first core _core 0_]
    X --> Y[Add end markers CR/CRLF and prompt]
    Y --> Z[Split into small Bluetooth packets]
    Z --> AA[Phone app / scanner]
```

## Startup flow

```mermaid
graph TD
    A[Program starts] --> B[Set up saved settings/config]
    B --> C[Create static FreeRTOS queues]
    C --> D[Prepare reply and log storage _pools_]
    D --> E[Start the vehicle connection]
    E --> F[Set up chip and bus _SPI.begin_]
    F --> G[Initialize the OBD system]
    G --> H[Set vehicle timing and receive mode]
    H --> I[Start Bluetooth]
    I --> J[Activate secure BLE pairing settings]
    J --> K[Start the log task]
    K --> L[Start the main vehicle task]
    L --> M[Start the Bluetooth send task]
    M --> N[Begin Bluetooth host]
    N --> O[Pairing-ready and ready to connect]
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
    J --> K[Check what the command means]
    K --> L{Normal OBD or extra command?}
    L -- Normal OBD --> M[Build and send the vehicle request]
    M --> N[Check again for new data]
    N --> O[Keep looping]
    L -- Extra command --> AA[Remember the reply ID and timeout]
    AA --> AB[Do not send a new request here]
    AB --> O
    I -- No --> P{Is the system busy?}
    P -- Yes --> Q[Give other work a moment]
    P -- No --> R{Did we do useful work?}
    R -- No --> S[Wait a tiny moment]
    R -- Yes --> Q
```

## Bluetooth send/receive path

```mermaid
graph TD
    A[Phone sends AT or PID command] --> B[Bluetooth receives the message]
    B --> C[Keep collecting the message]
    C --> D[Put it in the waiting list]
    D --> E[Main task takes the command]
    E --> F[Understand and process it]
    F --> G[Create the answer in a fixed response space]
    G --> H[Quick copy to the log list]
    H --> I[Small log task writes it]
    G --> J[Send the response slot number]
    J --> K[Bluetooth send task picks it up]
    K --> L[Add end markers and prompt]
    L --> M[Split it into small pieces]
    M --> N[Send to the phone]
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
    C -- Yes --> D[Handle it locally and return answer]
    C -- No --> E{Does it match a special extra command?}
    E -- Yes --> F[Follow the special-response path]
    E -- No --> H{Is it a valid supported command?}
    H -- Yes --> I[Build the normal request]
    I --> J[Send it to the vehicle]
    H -- No --> K[Return a question-mark error]
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

- The program starts by setting up saved settings.
- The BLE stack is configured for secure pairing and bonding.
- The main task checks for new messages from the vehicle.
- Bluetooth receives commands only after an encrypted, paired connection is established.
- The main task reads the command and decides what to do.
- Normal vehicle requests are sent to the car.
- Extra commands mostly remember what reply to look for and wait.
- When a reply arrives, it is cleaned up and sent back to the phone over the secure link.
- A small background task writes logs without slowing the vehicle work.

## Main files

- [main/app_main.c](main/app_main.c) — startup, queue setup, and task setup
- [main/Transport/MCP2515Transport.c](main/Transport/MCP2515Transport.c) — vehicle interface and message transfer
- [main/Core/OBD2.c](main/Core/OBD2.c) — command handling and reply building
- [main/BLE/ELM327_BLE.c](main/BLE/ELM327_BLE.c) — Bluetooth command and notification flow

## Summary

The system works in a simple order:

1. The phone connects and completes secure pairing.
2. The command is placed in a waiting list.
3. The main task decides if it is a normal request or a special request.
4. The vehicle side checks for replies.
5. The response is prepared and returned to the app over the encrypted BLE link.
6. A small log task copies the result without slowing the main work.

This keeps the system smooth and avoids stopping the vehicle work while Bluetooth is active.
