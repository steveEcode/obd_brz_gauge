#!/usr/bin/env bash
# One-click release: commit source → build → package release bins + latest.json → commit → push.
#
# Usage:  ./tools/release.sh "feat: your change message"
#
# Notes:
#   - `--no-verify` skips the global git hooks (PythonPep8 at GIS-lm-build/git-hooks).
#   - The source MUST be committed before building: gen_release.py derives
#     firmware.count / build_tag from the current git HEAD.
#   - Steps are skipped safely: if there is nothing to commit, the commit is skipped.
#   - ESP-IDF is auto-activated. `idf.py` here is a shell *alias* (not a real
#     binary), so the script sources the eim activation script and calls idf.py
#     through its python interpreter directly — no manual env activation needed.
set -euo pipefail

MSG="${1:-feat: update}"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

# ---- Activate ESP-IDF ------------------------------------------------------
if ! command -v idf.py >/dev/null 2>&1; then
    # eim-generated activation script (what the IDE/your normal shell uses).
    ACTIVATE="$(ls "$HOME"/.espressif/tools/activate_idf_*.sh 2>/dev/null | head -1)"
    if [ -n "$ACTIVATE" ]; then
        echo "activating ESP-IDF: $ACTIVATE"
        # This script can't be sourced directly here: it refuses to run unless it
        # thinks it is being sourced (keys off `$0` being a shell name), and it
        # references unbound vars (e.g. $ZSH_VERSION) that trip `set -u`.
        # Instead, source it in a fresh `bash` subshell (so its check passes) and
        # re-import the resulting environment (IDF_TOOLS_PATH, IDF_PATH, PATH…)
        # back into this shell. IDF_TOOLS_PATH is the bit that makes idf.py find
        # the eim-installed toolchain, which is why export.sh failed before.
        eval "$(bash -c '. "$1" >/dev/null 2>&1; export -p' bash "$ACTIVATE" \
            | grep -E '^declare -x (IDF_TOOLS_PATH|IDF_PATH|ESP_ROM_ELF_DIR|OPENOCD_SCRIPTS|IDF_PYTHON_ENV_PATH|ESP_IDF_VERSION|VIRTUAL_ENV|PATH)=')"
    else
        # Fallback: the classic install layout.
        IDF_EXPORT="$(ls -d "$HOME"/.espressif/*/esp-idf/export.sh 2>/dev/null | head -1)"
        if [ -n "$IDF_EXPORT" ]; then
            echo "activating ESP-IDF: $IDF_EXPORT"
            # shellcheck disable=SC1090
            set +u; . "$IDF_EXPORT" >/dev/null; set -u
        fi
    fi
fi

# Resolve how to invoke idf.py: prefer a real command; else python + full path.
if command -v idf.py >/dev/null 2>&1; then
    IDF_PY="idf.py"
    IDF_TOOL=""
else
    IDF_PY="${IDF_PYTHON_ENV_PATH:-$HOME/.espressif/tools/python/v5.5.3/venv}/bin/python"
    IDF_TOOL="${IDF_PATH:-$HOME/.espressif/v5.5.3/esp-idf}/tools/idf.py"
    if [ ! -x "$IDF_PY" ] || [ ! -f "$IDF_TOOL" ]; then
        echo "ERROR: idf.py not found — activate your ESP-IDF env first" >&2
        exit 1
    fi
fi

# ---- 1. Commit the source first (git tag/build count is read from HEAD) ----
git add -A
if ! git diff --cached --quiet; then
    git commit --no-verify -m "$MSG"
fi

# ---- 2. Build the firmware ----
if [ "$IDF_PY" = "idf.py" ]; then
    idf.py build
else
    "$IDF_PY" "$IDF_TOOL" build
fi

# ---- 3. Copy build/*.bin → firmware/release/ and write latest.json ----
python3 tools/gen_release.py

# ---- 4. Commit the release binaries + manifest, then push ----
git add firmware/release/
if ! git diff --cached --quiet; then
    git commit --no-verify -m "build: update release firmware binaries"
fi
git push --no-verify

echo "released: $(git rev-parse --short HEAD)  count=$(git rev-list --count HEAD)"
