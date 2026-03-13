# Project Structure For Open-Source Release

## Recommended Core Directories

These directories are the main project payload and should be kept in the public repository:

- [main/app_obd_dsp](../main/app_obd_dsp): application logic and runtime data cache
- [main/bsp_obd_dsp](../main/bsp_obd_dsp): board support, BLE client, storage, LCD, touch, and peripheral drivers
- [main/export_path](../main/export_path): UI source exported from the design workflow
- [components](../components): custom ESP-IDF components when needed
- [tools](../tools): project helper scripts
- [CMakeLists.txt](../CMakeLists.txt): top-level ESP-IDF project definition
- [main/CMakeLists.txt](../main/CMakeLists.txt): main component source registration
- [main/idf_component.yml](../main/idf_component.yml): dependency declaration

## Directories That Can Stay Generated

These directories can remain in your local workspace, but they usually do not need to be maintained manually as hand-written source:

- [managed_components](../managed_components): auto-downloaded dependency sources
- [build](../build): generated build output

## What The Main Code Does

- [main/app_main.c](../main/app_main.c): initializes NVS, board peripherals, LCD, touch, LVGL, UI, BLE, and mileage statistics task
- [main/bsp_obd_dsp/elm327_ble_client.h](../main/bsp_obd_dsp/elm327_ble_client.h): declares the BLE OBD client interface and parsed data callbacks
- [main/app_obd_dsp/obd_data_cache.h](../main/app_obd_dsp/obd_data_cache.h): defines the shared OBD data cache access API

## Release Notes You Should Keep Visible

- Target board is Waveshare ESP32-S3-Touch-LCD-1.85
- Display path is based on ST77916 and CST816 related drivers
- BLE OBD support is centered on ELM327-compatible adapters
- Vehicle data is only validated on Subaru BRZ ZC6 at the moment

## Practical Suggestion

If you want to make the repository cleaner before publishing, keep the source directories above, keep the documentation under [docs](../docs), and exclude generated outputs such as [build](../build) from version control.