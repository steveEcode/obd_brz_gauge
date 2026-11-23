#!/usr/bin/env python3
"""Extract production confirmation/worker/progress paths, not copies of logic."""
from pathlib import Path
import sys
s = Path(sys.argv[1]).read_text()
a = s.index('/* Worker never calls LVGL.')
b = s.index('void ui_ScreenPageOTAMode_screen_init(void)', a)
Path(sys.argv[2]).write_text(s[a:b])
