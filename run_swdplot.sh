#!/usr/bin/env bash
# Launcher for swd_liveplot.py
# First run: creates a virtual environment and installs the dependencies.
# Usage: ./run_swdplot.sh [firmware.elf]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP="$SCRIPT_DIR/swd_liveplot.py"
VENV="${SWDPLOT_VENV:-$HOME/.venvs/swdplot}"

if [[ ! -f "$APP" ]]; then
    echo "Cannot find $APP (keep this script next to swd_liveplot.py)" >&2
    exit 1
fi

if [[ ! -x "$VENV/bin/python" ]]; then
    echo "Creating virtual environment in $VENV ..."
    if ! python3 -m venv "$VENV"; then
        echo "Failed to create the venv. On Debian/Ubuntu run: sudo apt install python3-venv" >&2
        exit 1
    fi
fi

# Install dependencies only when something is missing
if ! "$VENV/bin/python" -c "import PySide6, pyqtgraph, elftools, numpy, serial" 2>/dev/null; then
    echo "Installing dependencies ..."
    "$VENV/bin/pip" install --upgrade pip
    "$VENV/bin/pip" install pyside6 pyqtgraph pyelftools numpy pyserial
fi

exec "$VENV/bin/python" "$APP" "$@"
