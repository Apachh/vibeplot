#!/usr/bin/env bash
# Launcher for swd_liveplot.py
# First run: creates a virtual environment and installs the dependencies.
# Usage: ./run_swdplot.sh [firmware.elf]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP="$SCRIPT_DIR/swd_liveplot.py"
VENV="${SWDPLOT_VENV:-$HOME/.venvs/swdplot}"
PY="$VENV/bin/python"
APT_HINT="On Debian/Ubuntu/Mint run:  sudo apt install python3 python3-venv python3-pip"

if [[ ! -f "$APP" ]]; then
    echo "Cannot find $APP (keep this script next to swd_liveplot.py)" >&2
    exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
    echo "python3 is not installed." >&2
    echo "$APT_HINT" >&2
    exit 1
fi

# A half-created environment (python present, pip missing) is rebuilt.
# Only directories that really are virtual environments are ever deleted.
if [[ -d "$VENV" ]] && ! "$PY" -m pip --version >/dev/null 2>&1; then
    if [[ -f "$VENV/pyvenv.cfg" ]]; then
        echo "Existing virtual environment is incomplete, recreating it ..."
        rm -rf "$VENV"
    else
        echo "$VENV exists but is not a working virtual environment." >&2
        echo "Remove it or set SWDPLOT_VENV to another path." >&2
        exit 1
    fi
fi

if [[ ! -x "$PY" ]]; then
    echo "Creating virtual environment in $VENV ..."
    mkdir -p "$(dirname "$VENV")"
    if ! python3 -m venv "$VENV" || ! "$PY" -m pip --version >/dev/null 2>&1; then
        [[ -f "$VENV/pyvenv.cfg" ]] && rm -rf "$VENV"
        echo "Could not create a virtual environment with pip." >&2
        echo "$APT_HINT" >&2
        exit 1
    fi
fi

# Install dependencies only when something is missing
if ! "$PY" -c "import PySide6, pyqtgraph, elftools, numpy, serial" 2>/dev/null; then
    echo "Installing dependencies (needs an internet connection) ..."
    if ! "$PY" -m pip install --upgrade pip \
        || ! "$PY" -m pip install pyside6 pyqtgraph pyelftools numpy pyserial; then
        echo "Installing the dependencies failed. Check your internet connection and run again." >&2
        exit 1
    fi
fi

rc=0
"$PY" "$APP" "$@" || rc=$?
if [[ $rc -ne 0 ]]; then
    echo >&2
    echo "The application exited with an error (code $rc)." >&2
    echo "If the message mentions the Qt platform plugin \"xcb\" (common on a fresh install):" >&2
    echo "    sudo apt install libxcb-cursor0" >&2
    echo "The error text names any other missing library; install it with apt the same way." >&2
fi
exit $rc
