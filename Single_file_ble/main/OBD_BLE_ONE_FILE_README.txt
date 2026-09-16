Single-file ESP-IDF source: OBD_BLE_ONE_FILE.c

Target: XIAO ESP32-S3 + MCP2515 with 8 MHz crystal
SPI: CS=5 SCK=7 MISO=8 MOSI=9
CAN: 500 kbit/s, exact timing CNF1=0x00 CNF2=0x91 CNF3=0x01
CAN RX: no MCP2515 acceptance filtering; RXB0/RXB1 receive all frames.
BLE: Nordic UART Service UUIDs, device name OBDII, no pairing/passkey.

Put the C file at:
  main/main.c

Your main/CMakeLists.txt should include:
  idf_component_register(SRCS "main.c"
      INCLUDE_DIRS "."
      REQUIRES bt esp_driver_spi nvs_flash)

Enable NimBLE as the Bluetooth host in menuconfig.

Build:
  idf.py fullclean
  idf.py set-target esp32s3
  idf.py build
  idf.py -p /dev/ttyUSB0 flash monitor

Phone BLE:
  Service: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
  RX:      6E400002-B5A3-F393-E0A9-E50E24DCCA9E
  TX:      6E400003-B5A3-F393-E0A9-E50E24DCCA9E

Commands:
  ATZ, ATI, ATSP6, ATDP, ATH0, ATH1, ATS0, ATS1, ATCAF0, ATCAF1
  ATMA = raw CAN monitor; streams every received frame
  ATMT = stop raw monitor
  0100, 010C, 010D, 0105, 0111, 03, 04, 0902
  ATCS <ID> <DLC> <DATA...> = raw CAN transmit

Note: BLE throughput is much lower than a busy vehicle CAN bus. The firmware
therefore accepts every CAN frame at the MCP2515 but only streams every frame
over BLE when ATMA is active. When ATMA is off, frames remain accepted by
hardware and are drained locally so they cannot fill the software queue.
