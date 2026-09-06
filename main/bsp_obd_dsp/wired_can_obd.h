#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Direct ISO 15765-4 OBD-II through ESP32-S3 TWAI and SN65HVD230.
 * GPIO43 (UART TXD) -> CAN TX, GPIO44 (UART RXD) <- CAN RX.
 * A short RPM probe is made first; false means the selected wired source is waiting for ECU data.
 */
bool wired_can_obd_start(void);
void wired_can_obd_stop(void);
bool wired_can_obd_is_active(void);
/* True only while valid ECU replies are still arriving. */
bool wired_can_obd_has_fresh_data(void);

#ifdef __cplusplus
}
#endif

