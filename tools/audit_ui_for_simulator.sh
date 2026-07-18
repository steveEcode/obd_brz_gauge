#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(
    cd "$(dirname "${BASH_SOURCE[0]}")/.."
    pwd
)"

OUTPUT_DIR="$ROOT_DIR/docs/offline_ui_audit"

mkdir -p "$OUTPUT_DIR"

cd "$ROOT_DIR"

{
    echo "=== DATE ==="
    date

    echo
    echo "=== CURRENT BRANCH ==="
    git branch --show-current

    echo
    echo "=== STATUS ==="
    git status

    echo
    echo "=== RECENT COMMITS ==="
    git log --oneline --decorate -8
} > "$OUTPUT_DIR/00_repository_state.txt"

find main/export_path \
    -maxdepth 2 \
    -type f \
    \( -name "*.c" -o -name "*.h" \) \
    | sort \
    > "$OUTPUT_DIR/01_exported_ui_files.txt"

grep -R -n '^#include' \
    main/export_path \
    --include="*.c" \
    --include="*.h" \
    > "$OUTPUT_DIR/02_all_ui_includes.txt" \
    || true

grep -R -nE \
    'esp_|freertos|driver/|nvs_|app_obd|ble_|espnow_|ST77916' \
    main/export_path \
    --include="*.c" \
    --include="*.h" \
    > "$OUTPUT_DIR/03_hardware_dependencies.txt" \
    || true

grep -R -nE \
    'device_role|ESPNOW_ROLE|espnow_|slave|master' \
    main \
    --include="*.c" \
    --include="*.h" \
    > "$OUTPUT_DIR/04_master_slave_references.txt" \
    || true

grep -R -nE \
    'nvs_[A-Za-z0-9_]+\(' \
    main/export_path \
    --include="*.c" \
    --include="*.h" \
    > "$OUTPUT_DIR/05_nvs_calls.txt" \
    || true

grep -R -nE \
    'elm327_[A-Za-z0-9_]+\(|ble_[A-Za-z0-9_]+\(' \
    main/export_path \
    --include="*.c" \
    --include="*.h" \
    > "$OUTPUT_DIR/06_ble_calls.txt" \
    || true

grep -nE \
    '^[[:space:]]*(void|bool|int|uint8_t|uint16_t|uint32_t|int16_t|int32_t|float)[[:space:]]+ui_[A-Za-z0-9_]+[[:space:]]*\(' \
    main/export_path/ui.c \
    > "$OUTPUT_DIR/07_ui_functions.txt" \
    || true

grep -R -nE \
    'ui_ScreenPage[A-Za-z0-9_]+_screen_init|void ui_init' \
    main/export_path \
    --include="*.c" \
    --include="*.h" \
    > "$OUTPUT_DIR/08_screen_initializers.txt" \
    || true

grep -R -nE \
    'rpm|RPM|speed|Speed|coolant|Coolant|oil_temp|vehicle_data|obd_data' \
    main/export_path \
    --include="*.c" \
    --include="*.h" \
    > "$OUTPUT_DIR/09_vehicle_data_references.txt" \
    || true

sed -n '1180,1320p' \
    main/export_path/ui.c \
    > "$OUTPUT_DIR/10_ui_init_context.txt"

{
    echo "=== ROOT CMAKE ==="
    cat CMakeLists.txt

    echo
    echo "=== MAIN CMAKE ==="
    cat main/CMakeLists.txt

    echo
    echo "=== EXPORT CMAKE ==="
    cat main/export_path/CMakeLists.txt

    echo
    echo "=== SIMULATOR CMAKE ==="
    cat simulator/CMakeLists.txt
} > "$OUTPUT_DIR/11_cmake_files.txt"

find main \
    -type f \
    \( \
        -name "nvs_storage.h" \
        -o -name "vehicle_profiles.h" \
        -o -name "elm327_ble_client.h" \
        -o -iname "*espnow*.h" \
    \) \
    | sort \
    > "$OUTPUT_DIR/12_required_platform_headers.txt"

sha256sum "$OUTPUT_DIR"/*.txt \
    > "$OUTPUT_DIR/SHA256SUMS.txt"

tar -czf \
    "$ROOT_DIR/docs/offline_ui_audit.tar.gz" \
    -C "$ROOT_DIR/docs" \
    offline_ui_audit

echo
echo "Audit generated:"
echo "  $OUTPUT_DIR"
echo
echo "Archive generated:"
echo "  $ROOT_DIR/docs/offline_ui_audit.tar.gz"

