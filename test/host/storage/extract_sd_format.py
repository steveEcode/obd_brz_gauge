#!/usr/bin/env python3
"""Test the production OTA format gate/service without linking Wi-Fi/HTTP."""
from pathlib import Path
import sys
s = Path(sys.argv[1]).read_text()
a = s.index('typedef struct {\n    TaskHandle_t worker;\n    bool accepted;\n} sd_format_gate_t;')
b = s.index('bool ota_wifi_server_is_active(void)', a)
Path(sys.argv[2]).write_text(s[a:b])
