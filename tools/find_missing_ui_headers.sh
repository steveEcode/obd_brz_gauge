#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(
    cd "$(dirname "${BASH_SOURCE[0]}")/.."
    pwd
)"

LOG_FILE="$ROOT_DIR/docs/offline_ui_audit/15_ui_probe_build.log"
OUTPUT_FILE="$ROOT_DIR/docs/offline_ui_audit/17_missing_header_locations.txt"

if [ ! -f "$LOG_FILE" ]; then
    echo "Missing probe log:"
    echo "  $LOG_FILE"
    exit 1
fi

mapfile -t MISSING_HEADERS < <(
    sed -n \
      's/.*fatal error: \(.*\): No such file or directory.*/\1/p' \
      "$LOG_FILE" \
      | sort -u
)

{
    for missing_header in "${MISSING_HEADERS[@]}"; do
        echo
        echo "=================================================="
        echo "MISSING: $missing_header"
        echo "=================================================="

        base_name="$(basename "$missing_header")"

        find "$ROOT_DIR" \
            -type f \
            -name "$base_name" \
            -not -path "*/build/*" \
            -not -path "*/build-ui-probe/*" \
            | sort \
            || true
    done
} > "$OUTPUT_FILE"

echo "Generated:"
echo "  $OUTPUT_FILE"
