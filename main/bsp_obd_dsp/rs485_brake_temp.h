#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Start the RS485 brake temperature sampling task (Modbus RTU)
void rs485_brake_temp_start(void);

// Pause / resume the polling loop (e.g. during BLE OTA to avoid RS485 blocking the CPU)
void rs485_brake_temp_pause(void);
void rs485_brake_temp_resume(void);

#ifdef __cplusplus
}
#endif
