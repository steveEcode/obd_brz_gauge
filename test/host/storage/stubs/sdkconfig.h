#pragma once
/* Match IDF: disabled booleans are absent, not defined to zero. */
#ifdef STORAGE_TEST_HAS_SLOT
#define CONFIG_OBD_SD_SLOT_SDMMC_1BIT 1
#else
#define CONFIG_OBD_SD_SLOT_NONE 1
#endif
#define CONFIG_OBD_HW_VERSION_V1_WAVESHARE 1
