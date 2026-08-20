#!/usr/bin/env python3
"""
CarScanner Backup Parser
解析 CarScanner ELM OBD2 备份目录，导出所有车辆的 PID 信息为 CSV。

Usage:
    python3 parse_carscanner_backup.py <backup_directory>

Output:
    <backup_directory>/carscanner_pids.csv
    <backup_directory>/carscanner_summary.txt
"""

import sys
import os
import csv
import struct
from pathlib import Path

# Record marker used between PID records in .brc files
RECORD_MARKER = b'\x90\x61\xd5\x00'


def parse_brc_file(filepath: str) -> dict | None:
    """
    解析单个 .brc 录制文件，提取所有 PID 定义。

    .brc 文件结构:
        文件头:
            [0x00] 0x10                     - 长度前缀 (1 byte)
            [0x01] 'CARSCANNERRECORD'        - 魔数 (16 bytes)
            [0x11] version (4 bytes LE u32)
            [0x15] vin_length (1 byte)
            [0x16] vin (variable, ASCII)
            [var]  car_model_length (1 byte)
            [var]  car_model (variable, UTF-8)
            [var]  connection_length (1 byte)
            [var]  connection (variable)
            [var]  8 bytes unknown (timestamp?)
            [var]  --- PID 记录 ---

        每个 PID 记录:
            [90 61 d5 00]                    - 分隔符 (4 bytes)
            [PID number]                     - PID 编号 (4 bytes LE u32)
            [display_len]                    - 显示名长度 (1 byte)
            [display_name]                   - 显示名 (UTF-8)
            [internal_len]                   - 内部名长度 (1 byte)
            [internal_name]                  - 内部名 (UTF-8)
            [var]  (后续为数据值, 不解析)
    """
    with open(filepath, 'rb') as f:
        data = f.read()

    if len(data) < 30:
        return None

    # Verify magic
    magic = data[1:17]
    if magic != b'CARSCANNERRECORD':
        return None

    pos = 0x11

    # Version
    version = struct.unpack_from('<I', data, pos)[0]
    pos += 4

    # VIN
    vin_len = data[pos]
    pos += 1
    vin = data[pos:pos + vin_len].decode('utf-8', errors='replace')
    pos += vin_len

    # Car model
    model_len = data[pos]
    pos += 1
    car_model = data[pos:pos + model_len].decode('utf-8', errors='replace')
    pos += model_len

    # Connection
    conn_len = data[pos]
    pos += 1
    connection = data[pos:pos + conn_len].decode('utf-8', errors='replace')
    pos += conn_len

    # Skip 8 bytes unknown
    pos += 8

    # Parse all PID records by finding every RECORD_MARKER
    pids = []
    seen = set()
    scan_pos = pos

    while True:
        idx = data.find(RECORD_MARKER, scan_pos)
        if idx == -1:
            break

        rec_pos = idx + 4  # position after marker

        # Need at least 6 bytes for PID + length
        if rec_pos + 6 > len(data):
            scan_pos = idx + 1
            continue

        # Read PID number (4 bytes LE u32)
        pid_num = struct.unpack_from('<I', data, rec_pos)[0]
        rec_pos += 4

        if pid_num == 0 or pid_num > 0xFFFF:
            scan_pos = idx + 1
            continue

        # Read display name length (1 byte)
        display_len = data[rec_pos]
        rec_pos += 1

        if display_len == 0 or display_len > 300 or rec_pos + display_len > len(data):
            scan_pos = idx + 1
            continue

        # Read display name
        try:
            display_name = data[rec_pos:rec_pos + display_len].decode('utf-8', errors='replace')
        except UnicodeDecodeError:
            display_name = data[rec_pos:rec_pos + display_len].decode('ascii', errors='replace')
        rec_pos += display_len

        # Read internal name length
        if rec_pos >= len(data):
            scan_pos = idx + 1
            continue

        internal_len = data[rec_pos]
        rec_pos += 1

        if internal_len == 0 or internal_len > 500 or rec_pos + internal_len > len(data):
            scan_pos = idx + 1
            continue

        # Read internal name
        try:
            internal_name = data[rec_pos:rec_pos + internal_len].decode('utf-8', errors='replace')
        except UnicodeDecodeError:
            internal_name = data[rec_pos:rec_pos + internal_len].decode('ascii', errors='replace')

        if pid_num not in seen:
            seen.add(pid_num)
            pids.append({
                'pid_number': pid_num,
                'pid_hex': f'0x{pid_num:02X}',
                'display_name': display_name,
                'internal_name': internal_name,
            })

        scan_pos = idx + 4

    return {
        'filename': os.path.basename(filepath),
        'file_size': os.path.getsize(filepath),
        'vin': vin,
        'car_model': car_model,
        'connection': connection,
        'pid_count': len(pids),
        'pids': pids,
    }


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <backup_directory>")
        print(f"\nExample:")
        print(f"  python3 {sys.argv[0]} '/path/to/CarScanner/backup/'")
        sys.exit(1)

    # Join all arguments (handles paths with spaces when user forgets quotes)
    backup_dir = ' '.join(sys.argv[1:])
    if not os.path.isdir(backup_dir):
        print(f"Error: '{backup_dir}' is not a valid directory")
        sys.exit(1)

    # Find all .brc files (recursively)
    brc_files = sorted(Path(backup_dir).rglob('*.brc'))
    if not brc_files:
        print(f"No .brc files found in '{backup_dir}'")
        sys.exit(1)

    print(f"Found {len(brc_files)} .brc file(s)\n")

    all_records = []
    total_pids = 0
    for brc_path in brc_files:
        result = parse_brc_file(str(brc_path))
        if result:
            all_records.append(result)
            total_pids += result['pid_count']
            label = ' [empty]' if result['pid_count'] == 0 else f' → {result["pid_count"]} PIDs'
            print(f"  {result['filename']:30s} VIN={result['vin']:17s} Model={result['car_model']:30s}{label}")
        else:
            print(f"  {brc_path.name:30s} (skipped - invalid or empty)")

    # Filter to records with PIDs
    records_with_pids = [r for r in all_records if r['pid_count'] > 0]
    if not records_with_pids:
        print("\nNo PID data found in any .brc file.")
        sys.exit(1)

    # Collect all unique PID numbers
    all_pid_numbers = set()
    for rec in records_with_pids:
        for pid in rec['pids']:
            all_pid_numbers.add(pid['pid_number'])

    # Build per-VIN PID mapping
    vin_info = {}  # vin -> {'model': str, 'pids': {pid_num -> pid_info}}
    for rec in records_with_pids:
        vin = rec['vin']
        if vin not in vin_info:
            vin_info[vin] = {'model': rec['car_model'], 'pids': {}}
        for pid in rec['pids']:
            vin_info[vin]['pids'][pid['pid_number']] = pid

    # ---- Write CSV ----
    output_path = os.path.join(backup_dir, 'carscanner_pids.csv')
    vins = sorted(vin_info.keys())

    with open(output_path, 'w', newline='', encoding='utf-8-sig') as f:
        writer = csv.writer(f)

        # Header row
        header = ['PID Number', 'PID Hex']
        for vin in vins:
            model = vin_info[vin]['model']
            header.append(f'{vin} ({model})')
        writer.writerow(header)

        # Data rows: one row per unique PID
        for pid_num in sorted(all_pid_numbers):
            row = [pid_num, f'0x{pid_num:02X}']
            for vin in vins:
                pid_info = vin_info[vin]['pids'].get(pid_num)
                if pid_info:
                    row.append(pid_info['display_name'])
                else:
                    row.append('')
            writer.writerow(row)

    # ---- Write Summary ----
    summary_path = os.path.join(backup_dir, 'carscanner_summary.txt')
    with open(summary_path, 'w', encoding='utf-8') as f:
        f.write("CarScanner Backup PID Summary\n")
        f.write("=" * 60 + "\n\n")
        for vin in vins:
            info = vin_info[vin]
            f.write(f"VIN: {vin}\n")
            f.write(f"Model: {info['model']}\n")
            f.write(f"PID Count: {len(info['pids'])}\n")
            f.write("-" * 40 + "\n")
            for pn in sorted(info['pids'].keys()):
                pid = info['pids'][pn]
                f.write(f"  {pid['pid_hex']} ({pn:>4d}): {pid['display_name']}\n")
            f.write("\n")

        # Also write combined unique PID list
        f.write("=" * 60 + "\n")
        f.write(f"Combined Unique PIDs: {len(all_pid_numbers)}\n")
        f.write("=" * 60 + "\n\n")
        for pid_num in sorted(all_pid_numbers):
            # Collect which vehicles have this PID
            vehicles = []
            for vin in vins:
                if pid_num in vin_info[vin]['pids']:
                    vehicles.append(vin_info[vin]['pids'][pid_num]['display_name'])
            # Use the first available name
            name = vehicles[0] if vehicles else ''
            f.write(f"  {f'0x{pid_num:02X}':>6s} ({pid_num:>4d}): {name}\n")

    vehicle_labels = [f'{vin} ({vin_info[vin]["model"]})' for vin in vins]

    print(f"\n✅ Exported {len(all_pid_numbers)} unique PID(s) to:")
    print(f"   CSV:     {output_path}")
    print(f"   Summary: {summary_path}")
    print(f"\nVehicles: {', '.join(vehicle_labels)}")


if __name__ == '__main__':
    main()
