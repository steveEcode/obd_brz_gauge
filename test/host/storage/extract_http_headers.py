#!/usr/bin/env python3
"""Compile actual OTA/API chunk header validation against a fake request."""
from pathlib import Path
import re
import sys
ota, api = (Path(p).read_text() for p in sys.argv[1:3])

def section(source, start, end):
    a = source.index(start)
    return source[a:source.index(end, a)]

parts = [
    section(ota, 'static esp_err_t send_err(', 'static esp_err_t send_204('),
    section(ota, 'static bool hdr_present(', '/* Prepare / erase bodies'),
    section(ota, 'static bool parse_chunk_headers(', 'static bool chunk_in_sequence('),
]
api_part = section(api, 'static bool hdr_u32(', '/* Whole request body')
api_part += section(api, '/* Chunk headers: X-OTA-Size', 'static bool chunk_continues(')
for name in ('hdr_u32', 'hdr_last'):
    api_part = re.sub(r'\b' + name + r'\b', 'api_' + name, api_part)
parts.append(api_part)
Path(sys.argv[3]).write_text('\n'.join(parts))
