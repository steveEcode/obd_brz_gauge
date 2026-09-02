#!/usr/bin/env python3
"""
Fix NVS configuration to use STANDALONE mode (BLE only, no WiFi/ESP-NOW)
This prevents WiFi OOM crashes.
"""
import subprocess
import sys

PORT = '/dev/cu.usbmodem1101'
PARTITION_NAME = 'nvs'

# NVS key to modify: device_role
# Values: 0=STANDALONE, 1=MASTER, 2=SLAVE
NEW_ROLE = 0  # STANDALONE

print("Fixing NVS configuration...")
print(f"Setting device_role to {NEW_ROLE} (STANDALONE - BLE only, no WiFi)")

# Use parttool.py to set the value
cmd = [
    'python3',
    f'{sys.prefix}/../../../.espressif/v5.5.3/esp-idf/components/partition_table/parttool.py',
    '--port', PORT,
    '--partition-name', PARTITION_NAME,
    'write_partition',
    '--input', '-'
]

print("\nNote: This requires the parttool, which may not work directly.")
print("Instead, use idf.py to erase NVS and let it reinitialize with defaults:\n")
print("  idf.py erase-flash")
print("  idf.py build flash")
print("\nOr manually set via menuconfig before building.")
