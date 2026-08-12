#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Start the oil pressure sampling task (direct ESP32 ADC connection).
void oil_pressure_start(void);

#ifdef __cplusplus
}
#endif
