# SWD Live Plot

A desktop tool that plots variables of a running Cortex-M target **live, while you debug**, without changing the firmware. Load your firmware `.elf`, pick variables from its debug info, and watch them on a graph. It reads their memory over SWD through the OpenOCD instance that Cortex-Debug already started in VS Code.

It was built for the setup *VS Code + Cortex-Debug + ST-Link (SWD) on a Cortex-M4*, where there is no UART, no SWO pin and no CubeMonitor available, but it should work with any target and probe that OpenOCD supports.

```
variables in RAM ──SWD──> ST-Link ──> OpenOCD (started by Cortex-Debug)
                                          │  Tcl port
                                          ▼
                                   SWD Live Plot  ──> graph, cursors, post-processing
                                          │
                                          └──> virtual COM port / serial port / file
```

> **Status:** early version. The ELF/DWARF parsing, filters, value writing and streaming logic were tested in isolation, but the GUI has not been verified on real hardware yet. Please try it on a harmless variable first and report anything that misbehaves.

## Features

- Load an `.elf` and browse its global and `static` variables, including C++ ones (namespaces, class static members, base classes).
- Structs, unions, arrays, C++ base classes and bitfields expand in the tree, including **packed structs** (unaligned members are read safely, byte by byte). The tree shows each member's address, byte offset and bit range. Double-click a struct to add all of its members.
- **Graph tabs**: each tab has its own plot, variable table and cursors. **Add selected** puts variables on the current tab; use **＋** for a new tab, double-click a tab to rename it, ✕ to close it. All tabs share one connection and one sampling loop. Inside a tab, **Add graph** stacks more graphs that share the time axis and cursors; the **Graph** column of the variable table (or **New variables go to**) chooses which graph shows each variable, and **Remove graph** moves its variables to Graph 1.
- Live graph with a rolling time window, **Follow live**, **Fit** (both axes) and **Fit Y** (vertical only).
- Two measurement cursors with values at each cursor, Δ between them, Δt and frequency.
- Post-processing per variable: moving average, moving RMS, median, low-pass (EMA), derivative, integral, absolute value, remove mean, plus gain and offset.
- Offset editor that matches the variable's type (whole numbers for integers, decimals for floats).
- **Write values** back into the target's memory (double-click the value).
- Enums are shown by name in the table, at the cursors and on the Y axis.
- **Stream** the plotted data live to a virtual COM port, a real serial port or a file (CSV, Teleplot or JSON lines), for comparison with another program.
- Export recorded data to CSV (raw and processed).
- Notices when the ELF is rebuilt and reloads it, keeping your plotted variables (matched by name) and moving them to their new addresses.
- Dark theme.

## Requirements

- Python 3.9 or newer (recommended). On a fresh Debian/Ubuntu/Mint install: `sudo apt install python3 python3-venv python3-pip libxcb-cursor0` (the last package is needed by Qt's window plugin).
- A target debugged with **Cortex-Debug using the OpenOCD server** (OpenOCD **0.10 or newer**, because the tool uses its `read_memory` Tcl command).
- A firmware ELF built **with debug info** (`-g`). Without it only raw symbols are listed.
- Python packages: `pyside6`, `pyqtgraph`, `pyelftools`, `numpy`, and `pyserial` (needed only for real serial/COM ports).

## Install and run

The easiest way is the launcher. It creates a virtual environment on first run, installs the dependencies and starts the app:

```bash
./run_swdplot.sh [firmware.elf]
```

Keep `run_swdplot.sh` next to `swd_liveplot.py`. If it is not executable: `chmod +x run_swdplot.sh`. Set `SWDPLOT_VENV=/some/path` to use a different virtual environment location (default `~/.venvs/swdplot`).

Manual install:

```bash
python -m venv ~/.venvs/swdplot
source ~/.venvs/swdplot/bin/activate
pip install pyside6 pyqtgraph pyelftools numpy pyserial
python swd_liveplot.py firmware.elf
```

A plain `pip install` outside a virtual environment fails on many Linux distributions with `externally-managed-environment`. Use a virtual environment as above.

## Quick start

1. In VS Code press **F5** so Cortex-Debug starts OpenOCD, and let the target run.
2. Find OpenOCD's **Tcl port** (see below).
3. Start the app and open your ELF (**Open ELF…** or pass it on the command line).
4. Enter the Tcl port in the top bar and press **Connect**.
5. Filter the variable list and double-click the variables you want. They appear on the graph.
6. After rebuilding the firmware the app reloads the ELF by itself (see **Reload ELF**). Remember to flash the new firmware as well, because addresses in the running target only change after you flash it.

### Finding the Tcl port

Cortex-Debug chooses the OpenOCD ports for each debug session. Look at the OpenOCD command line or its output, for example:

```
/usr/bin/openocd -c "gdb_port 50000" -c "tcl_port 50001" -c "telnet_port 50002" ...
```

Use the **Tcl** port (`50001` here), not the GDB or telnet port. The app's default is 50001 (the Tcl port in the example above). The port is chosen by Cortex-Debug and can change between sessions, so adjust it if Connect fails; the OpenOCD output also contains a line like `Listening on port 50001 for tcl connections`.

The ST-Link can only be owned by one OpenOCD at a time, so this tool must connect to Cortex-Debug's OpenOCD rather than start its own.

## Using the app

### Top bar

| Control | What it does |
|---|---|
| Open ELF… | Load a firmware ELF and list its variables |
| Reload ELF, Auto | Re-read the ELF after a rebuild and move the plotted variables to their new addresses. With **Auto** ticked (default) this happens by itself when the file changes on disk; otherwise the button turns into `Reload ELF ●` and the file name turns orange |
| Host / port, Connect | Connect to (or disconnect from) OpenOCD's Tcl port |
| Period | Target time between reads, in ms (the real rate is limited by the probe) |
| Window | Seconds of history shown while following |
| Follow live | Scroll the view with the newest data |
| Fit X | Show the whole recorded time range (stops following) |
| Fit Y | Fit the vertical range to the data in the visible time range (use with Fit X to fit both axes) |
| Cursors | Show two measurement cursors (stops following so the view stays still) |
| Clear data | Delete the recorded samples, keep the variables |
| Export CSV… | Save the recorded data of all tabs (`variable, time_s, raw, processed, tab`) |
| Stream… | Open the live streaming settings |

### Variable tree (left)

- Type in the filter box to search. Qualified C++ names (`ns::Class::var`) can be searched too.
- Double-click a variable to plot it. Double-click a struct or an array to add its members (up to 64).
- **Add by address…** plots any memory address as a raw 32-bit value. Change its type in the table.

### Variable table (bottom right)

| Column | Meaning |
|---|---|
| Show | Plot and read this variable (unchecked variables are not read, which saves SWD bandwidth) |
| Name, Address | The variable and its RAM address |
| Type | Data type used to interpret the memory. You can change it, for example when there is no debug info |
| Offset | Offset added to the plotted signal. The editor matches the type (integer or decimal) |
| Value | Latest value. Double-click it to **write** a new value. Enums show their name |
| @C1, @C2, Δ | Values at the cursors and their difference (when cursors are on) |
| ✕ | Remove the variable |

Select rows by clicking a variable's name. **Remove selected** (or the Delete key) and **Remove all variables** clear the plot quickly.

### Post-processing

Select one or more variables, then pick a mode in the panel above the table. Order of operations: **filter → gain → offset**.

| Mode | Parameter |
|---|---|
| Moving average / Moving RMS | N samples |
| Median | N samples (up to 101) |
| Low-pass EMA | α from 0.001 to 1 (smaller means smoother) |
| Derivative d/dt, Integral ∫dt | none (uses the real sample times) |
| Absolute value, Remove mean | none |

Integral and remove-mean work on the samples kept in memory (the last 50,000 per variable). The Value column, the cursor readouts and the CSV export use the processed signal.

### Cursors

Turn on **Cursors** and drag the yellow (C1) and blue (C2) lines. The plot title shows `C1`, `C2`, `Δt` and `1/Δt` in Hz, and the table shows each variable's value at both cursors (interpolated between samples; enum variables use the last sample before the cursor).

### Writing values

Double-click a variable's **Value** cell, or select it and press **Write value…**. Enter a decimal or hex number (`1234`, `0x1F`, `-5`, `1.5`), or pick an enum name. The value is checked against the variable's type and written to memory raw, ignoring gain and offset. It is sent through the same OpenOCD connection as the reads, using OpenOCD's `mww`, `mwh` and `mwb` commands.

Be careful: this writes straight into the running target. If your firmware updates the variable every cycle, it will overwrite your value again. **Bitfields are read-only**, because writing one would need a read-modify-write that can race with the firmware.

### Enums

Variables of an enum type (including C++ `enum class`, and enum members of structs and arrays) display as `NAME (value)`. If all visible variables are plain enums, the Y axis is labeled with the names.

### Streaming to another program

**Stream…** sends the plotted values live while the app is connected.

| Output | Notes |
|---|---|
| Virtual COM port (created by the app) | Linux/macOS. Creates a pty and a symlink, by default `/tmp/swdplot_com`. Open that path in the other program as a serial port (any baud rate). No `socat` needed |
| Existing serial port / COM port | For `/dev/ttyUSB0`, `COM10`, or one end of a com0com pair on Windows. Needs `pyserial` and a baud rate |
| File | Append or overwrite |

Formats:

```text
CSV        time_s,speed,current
           1.234567,120,1.5

Teleplot   >speed:1234:120          (timestamp in ms; without a timestamp: >speed:120)

JSON       {"t":1.234567,"values":{"speed":120,"current":1.5}}
```

Other options: raw memory values or raw × gain + offset, timestamp as seconds since Connect / Unix time / none, and "send every N samples". Post-processing filters are **not** applied to the stream (use Export CSV for the filtered signal). If you add or remove variables while streaming, CSV writes a new header row.

The writing runs in a background thread. If nothing reads the port, or the reader is too slow, data is dropped instead of freezing the plot; the dialog shows bytes sent and chunks dropped.

Quick check on Linux: start streaming to the virtual COM port and run `cat /tmp/swdplot_com`.

## Limitations

- **Only variables with a fixed address** can be plotted: globals and `static` variables. Stack variables, heap objects and members reached through a pointer are not found. For those, read the address in the debugger and use **Add by address…**.
- **Optimized builds** can remove or move variables into registers. Debug with `-Og` or `-O0` for best results.
- **Sample rate** is limited by the probe and OpenOCD, typically a few hundred reads per second in total, shared by all variables. Timing is approximate, so this is not suitable for fast ISR-level signals.
- **Plotting does not halt the CPU**, but the graph freezes at a breakpoint because the values stop changing.
- Compiler/DWARF notes: GCC (`arm-none-eabi-gcc`) output works. With clang and DWARF 5, addresses may be stored in a form the tool does not read yet; add `-gdwarf-4` if variables are missing.
- Little-endian targets only, 32-bit addresses.
- C++ **virtual base classes** (offset only known at run time) and members reached through pointers or references are not shown. Static data members are listed as separate variables.
- If the window does not open on Wayland or X11, try setting `QT_QPA_PLATFORM=wayland` or `xcb` before starting.

## Troubleshooting

| Problem | Check |
|---|---|
| "Cannot connect" | Wrong port (use the Tcl port, default 50001), OpenOCD not running, or the debug session ended |
| Values wrong right after a rebuild | The ELF was reloaded but the target still runs the old firmware; flash and restart the debug session |
| Read error with an OpenOCD message | OpenOCD older than 0.10, or the address is not readable memory |
| Struct or class fields missing | Rebuild with `-g` (and a recent GCC); bitfields, base classes and anonymous unions are supported, virtual bases are not |
| Variable missing from the list | Built without `-g`, optimized away, local variable, or clang DWARF 5 (see above) |
| Values look wrong | Change the variable's **Type** in the table (size and signedness) |
| Streaming to a COM port shows nothing | Reader must open the same path; on Windows use a com0com pair and the "Existing serial port" output |

## Files

| File | Purpose |
|---|---|
| `swd_liveplot.py` | The application |
| `run_swdplot.sh` | Launcher that sets up a virtual environment and starts the app |
| `README.md` | This file |
