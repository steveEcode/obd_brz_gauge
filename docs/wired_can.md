# Direct wired CAN OBD-II

This branch adds an optional direct ISO 15765-4 CAN source while preserving the
existing Bluetooth ELM327 path. The source is selected on the OBD SOURCE screen
and stored in NVS. Existing installations remain on Bluetooth by default.

## Tested hardware

- ESP32-S3-Touch-LCD-1.85C V2.0
- SN65HVD230 3.3 V CAN transceiver module

| ESP32-S3-Touch-LCD-1.85C V2.0 | SN65HVD230 board |
| --- | --- |
| 3V3 | 3.3V |
| GND | GND |
| GPIO43 / UART TXD | CAN TX |
| GPIO44 / UART RXD | CAN RX |

| SN65HVD230 screw terminal | OBD-II connector |
| --- | --- |
| CANH | Pin 6 (CAN High) |
| CANL | Pin 14 (CAN Low) |
| GND | Pin 4 or 5 (ground) |

Power the display using its normal protected supply. Do not connect OBD pin 16
directly to a 3.3 V input. The transceiver is powered from 3.3 V and all grounds
must be common.

Most vehicles already provide termination at both ends of the vehicle CAN bus.
Do not add another 120 ohm terminator for a short OBD stub unless measurements
show that the test network is unterminated. With vehicle power off, a correctly
terminated bus normally measures about 60 ohms between CANH and CANL.

## Supported direct-CAN operation

- ISO 15765-4 11-bit, 500 kbit/s (protocol 6)
- ISO 15765-4 29-bit, 500 kbit/s (protocol 7)
- ISO 15765-4 11-bit, 250 kbit/s (protocol 8)
- ISO 15765-4 29-bit, 250 kbit/s (protocol 9)
- Mode 01 PIDs: load, coolant temperature, RPM, speed, intake temperature,
  throttle position, control-module voltage, commanded equivalence ratio and
  oil temperature

The implementation sends single-frame functional/physical OBD requests and
accepts single-frame Mode 01 replies. Manufacturer-specific multi-frame Mode 22
and non-CAN OBD protocols are still handled only by the existing ELM327 path.

## Runtime behavior

- CAN and BLE are mutually exclusive so they do not compete for resources.
- `NO SIGNAL` follows fresh data from the selected source.
- CAN is considered stale after eight seconds without valid replies.
- Switching back to BLE re-enables manual scanning without rebooting.
- A manually selected BLE connection suppresses the original connection sweep,
  which otherwise looks like a device restart.
