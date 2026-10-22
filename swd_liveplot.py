#!/usr/bin/env python3
"""
SWD Live Plot
=============
Load a firmware .elf, pick variables from its debug info, and plot them live
while you debug with Cortex-Debug (ST-Link / OpenOCD). No firmware changes.

How it works: Cortex-Debug starts OpenOCD, OpenOCD owns the ST-Link. This tool
connects to OpenOCD's Tcl port and reads the variables' RAM addresses over SWD
(background memory access, the CPU is not halted).

Install:   pip install pyside6 pyqtgraph pyelftools numpy
Run:       python swd_liveplot.py [firmware.elf]
"""
import csv
import queue
import socket
import struct
import sys
import threading
import time
from collections import deque

import numpy as np
from numpy.lib.stride_tricks import sliding_window_view
import pyqtgraph as pg
from pyqtgraph.Qt import QtCore, QtGui, QtWidgets
from elftools.elf.elffile import ELFFile
from elftools.elf.sections import SymbolTableSection

MAXPTS = 50000
COLORS = ["#ff5555", "#55aaff", "#55dd77", "#ffaa33", "#cc77ff",
          "#ffee55", "#33dddd", "#ff77bb", "#aaaaaa", "#99cc33"]

# name -> (size in bytes, struct format char)
TYPES = {
    "uint8": (1, "B"), "int8": (1, "b"),
    "uint16": (2, "H"), "int16": (2, "h"),
    "uint32": (4, "I"), "int32": (4, "i"), "float32": (4, "f"),
    "uint64": (8, "Q"), "int64": (8, "q"), "float64": (8, "d"),
}
FMT_TO_TYPE = {v: k for k, v in TYPES.items()}
UNSIGNED = {1: "B", 2: "H", 4: "I", 8: "Q"}
SIGNED = {1: "b", 2: "h", 4: "i", 8: "q"}
WIDTH = {1: "B", 2: "H", 4: "I"}


# --------------------------------------------------------------------------
# DWARF helpers
# --------------------------------------------------------------------------
QUALIFIERS = {"DW_TAG_typedef", "DW_TAG_const_type", "DW_TAG_volatile_type",
              "DW_TAG_restrict_type", "DW_TAG_atomic_type"}
STRUCT_TAGS = {"DW_TAG_structure_type", "DW_TAG_union_type", "DW_TAG_class_type"}


def attr(die, name, default=None):
    a = die.attributes.get(name)
    return default if a is None else a.value


def die_name(die):
    v = attr(die, "DW_AT_name")
    return v.decode(errors="replace") if isinstance(v, bytes) else v


def target(die):
    if die is None or "DW_AT_type" not in die.attributes:
        return None
    return die.get_DIE_from_attribute("DW_AT_type")


def strip(die):
    while die is not None and die.tag in QUALIFIERS:
        die = target(die)
    return die


def array_dims(d):
    dims = []
    for c in d.iter_children():
        if c.tag == "DW_TAG_subrange_type":
            cnt, ub = attr(c, "DW_AT_count"), attr(c, "DW_AT_upper_bound")
            if isinstance(cnt, int):
                dims.append(cnt)
            elif isinstance(ub, int) and ub >= 0:
                dims.append(ub + 1)
            else:
                dims.append(0)
    return dims or [0]


def sizeof(die):
    d = strip(die)
    if d is None:
        return 0
    bs = attr(d, "DW_AT_byte_size")
    if isinstance(bs, int):
        return bs
    if d.tag == "DW_TAG_pointer_type":
        return 4
    if d.tag == "DW_TAG_array_type":
        n = 1
        for c in array_dims(d):
            n *= c
        return n * sizeof(target(d))
    return 0


def type_name(die):
    if die is None:
        return "void"
    t, n = die.tag, die_name(die)
    if t in ("DW_TAG_base_type", "DW_TAG_typedef"):
        return n or "?"
    if t == "DW_TAG_const_type":
        return "const " + type_name(target(die))
    if t == "DW_TAG_volatile_type":
        return "volatile " + type_name(target(die))
    if t == "DW_TAG_pointer_type":
        return type_name(target(die)) + "*"
    if t == "DW_TAG_array_type":
        return type_name(target(die)) + "".join(f"[{c}]" for c in array_dims(die))
    if t == "DW_TAG_structure_type":
        return "struct " + (n or "{...}")
    if t == "DW_TAG_union_type":
        return "union " + (n or "{...}")
    if t == "DW_TAG_enumeration_type":
        return "enum " + (n or "{...}")
    return n or t.replace("DW_TAG_", "")


def leaf_fmt(d):
    """(size, struct fmt) for a stripped DIE that is a plottable scalar, else None."""
    if d.tag == "DW_TAG_pointer_type":
        return (4, "I")
    if d.tag == "DW_TAG_enumeration_type":
        size = attr(d, "DW_AT_byte_size", 4)
        under = strip(target(d))
        signed = under is not None and attr(under, "DW_AT_encoding") in (5, 6)
        tbl = SIGNED if signed else UNSIGNED
        return (size, tbl[size]) if size in tbl else None
    if d.tag == "DW_TAG_base_type":
        size, enc = attr(d, "DW_AT_byte_size"), attr(d, "DW_AT_encoding")
        if enc == 4:  # float
            return (size, {4: "f", 8: "d"}[size]) if size in (4, 8) else None
        tbl = SIGNED if enc in (5, 6) else UNSIGNED
        return (size, tbl[size]) if size in tbl else None
    return None


def var_addr(die):
    a = die.attributes.get("DW_AT_location")
    if a is None or not isinstance(a.value, (list, tuple)):
        return None
    v = a.value
    if len(v) >= 5 and v[0] == 0x03:  # DW_OP_addr
        addr = int.from_bytes(bytes(v[1:5]), "little")
        return addr or None
    return None


SCOPE_TAGS = {"DW_TAG_namespace", "DW_TAG_class_type",
              "DW_TAG_structure_type", "DW_TAG_union_type"}


def var_info(die):
    """(name, type DIE, declaration DIE). In C++ the defining DIE of a global or
    static member often has no name/type itself and points to its declaration
    through DW_AT_specification."""
    name, tdie, decl = die_name(die), target(die), die
    for key in ("DW_AT_specification", "DW_AT_abstract_origin"):
        if key in die.attributes:
            decl = die.get_DIE_from_attribute(key)
            if name is None:
                name = die_name(decl)
            if tdie is None:
                tdie = target(decl)
            break
    return name, tdie, decl


def qualified(name, decl):
    """Prefix the name with its enclosing namespaces/classes (ns::Class::var)."""
    parts = [name]
    p = decl.get_parent()
    while p is not None and p.tag in SCOPE_TAGS:
        parts.append(die_name(p) or "(anon)")
        p = p.get_parent()
    return "::".join(reversed(parts))


class Node:
    """A variable, struct member or array element. Children are built lazily."""
    ARRAY_LIMIT = 512

    def __init__(self, label, path, addr, tdie=None, dims=None, leaf=None, typename=None):
        self.label, self.path, self.addr = label, path, addr
        self.tdie = tdie
        self.d = strip(tdie)
        self.arr = None
        self.is_struct = False
        self.leaf = leaf
        self._kids = None
        if tdie is None and leaf is None:
            self.leaf = (4, "I")
        if self.d is not None:
            if self.d.tag == "DW_TAG_array_type":
                self.arr = (dims if dims is not None else array_dims(self.d), target(self.d))
            elif self.d.tag in STRUCT_TAGS:
                self.is_struct = True
            elif leaf is None:
                self.leaf = leaf_fmt(self.d)
        self.enum_map = None
        if self.d is not None and self.d.tag == "DW_TAG_enumeration_type":
            self.enum_map = enum_values(self.d) or None
        if self.arr:
            ds, et = self.arr
            n = 1
            for c in ds:
                n *= c
            self.size = n * sizeof(et)
            self.typename = typename or type_name(et) + "".join(f"[{c}]" for c in ds)
        else:
            self.size = (sizeof(tdie) if tdie is not None else 0) or (self.leaf[0] if self.leaf else 0)
            self.typename = typename or type_name(tdie)

    @property
    def expandable(self):
        return bool(self.arr) or self.is_struct

    def children(self):
        if self._kids is not None:
            return self._kids
        kids = []
        if self.arr:
            ds, et = self.arr
            sub = 1
            for c in ds[1:]:
                sub *= c
            stride = sizeof(et) * sub
            for i in range(min(ds[0], self.ARRAY_LIMIT)):
                a, p = self.addr + i * stride, f"{self.path}[{i}]"
                if len(ds) > 1:
                    kids.append(Node(f"[{i}]", p, a, self.tdie, dims=ds[1:]))
                else:
                    kids.append(Node(f"[{i}]", p, a, et))
        elif self.is_struct:
            kids = self._member_nodes(self.d, 0)
        self._kids = kids
        return kids

    def _member_nodes(self, sdie, base_off):
        out = []
        for m in sdie.iter_children():
            if m.tag == "DW_TAG_inheritance":  # C++ base class: flatten its members in
                bt = strip(target(m))
                off = attr(m, "DW_AT_data_member_location", 0)
                if bt is not None and bt.tag in STRUCT_TAGS and isinstance(off, int):
                    out += self._member_nodes(bt, base_off + off)
                continue
            if m.tag != "DW_TAG_member":
                continue
            # skip bitfields and static members (declarations without an offset)
            if any(k in m.attributes for k in ("DW_AT_bit_size", "DW_AT_declaration", "DW_AT_external")):
                continue
            off = attr(m, "DW_AT_data_member_location", 0)
            if not isinstance(off, int):
                continue
            nm = die_name(m) or "<anon>"
            out.append(Node(nm, f"{self.path}.{nm}", self.addr + base_off + off, target(m)))
        return out


def collect_leaves(node, out, limit=64):
    if len(out) >= limit:
        return
    if node.leaf:
        out.append(node)
        return
    for k in node.children():
        collect_leaves(k, out, limit)
        if len(out) >= limit:
            break


class ElfModel:
    def __init__(self, path):
        self.file = open(path, "rb")
        self.elf = ELFFile(self.file)
        self.roots = []
        self.has_debug = False
        if self.elf.has_dwarf_info():
            self._load_dwarf()
        if self.roots:
            self.has_debug = True
        else:
            self._load_symtab()
        self.roots.sort(key=lambda n: n.label.lower())

    def close(self):
        try:
            self.file.close()
        except Exception:
            pass

    def _load_dwarf(self):
        seen = set()
        for cu in self.elf.get_dwarf_info().iter_CUs():
            for die in cu.iter_DIEs():
                if die.tag != "DW_TAG_variable":
                    continue
                addr = var_addr(die)
                if addr is None:
                    continue
                name, tdie, decl = var_info(die)
                if not name:
                    continue
                name = qualified(name, decl)
                if (name, addr) in seen:
                    continue
                seen.add((name, addr))
                self.roots.append(Node(name, name, addr, tdie))

    def _load_symtab(self):
        for sec in self.elf.iter_sections():
            if not isinstance(sec, SymbolTableSection):
                continue
            for s in sec.iter_symbols():
                size = s["st_size"]
                if s["st_info"]["type"] == "STT_OBJECT" and size in UNSIGNED and s["st_value"]:
                    self.roots.append(Node(s.name, s.name, s["st_value"], None,
                                           leaf=(size, UNSIGNED[size]), typename=f"raw{size * 8}"))


# --------------------------------------------------------------------------
# OpenOCD Tcl polling thread
# --------------------------------------------------------------------------
class Poller(QtCore.QThread):
    sample = QtCore.Signal(float, object)
    status = QtCore.Signal(str)

    def __init__(self, host, port, get_specs, get_period):
        super().__init__()
        self.host, self.port = host, port
        self.get_specs, self.get_period = get_specs, get_period
        self._stop = False
        self.sock = None
        self.writes = queue.Queue()
        self.wake = threading.Event()

    def request_write(self, name, addr, raw):
        """Queue a memory write; it is executed by the polling thread between reads."""
        self.writes.put((name, addr, raw))
        self.wake.set()

    def _do_writes(self):
        while True:
            try:
                name, addr, raw = self.writes.get_nowait()
            except queue.Empty:
                return
            try:
                out = self.tcl(build_write_cmd(addr, raw))
            except ValueError as e:
                self.status.emit(f"Write {name} failed: {e}")
                continue
            if out:
                self.status.emit(f"Write {name} failed: {out[:150]}")
            else:
                self.status.emit(f"Wrote {name} @ 0x{addr:08X}")

    def stop(self):
        self._stop = True
        self.wake.set()
        try:
            if self.sock:
                self.sock.close()
        except Exception:
            pass

    def tcl(self, cmd):
        self.sock.sendall(cmd.encode() + b"\x1a")
        buf = bytearray()
        while True:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("OpenOCD closed the connection")
            buf += chunk
            if buf.endswith(b"\x1a"):
                break
        return buf[:-1].decode(errors="replace").strip()

    def read_values(self, specs):
        parts = []
        for _sid, addr, size, _fmt in specs:
            w = min(size, 4)
            parts.append(f'append o [read_memory 0x{addr:x} {w * 8} {size // w}] ";"')
        out = self.tcl('set o ""; ' + "; ".join(parts) + "; set o")
        chunks = out.split(";")
        vals = {}
        for (sid, addr, size, fmt), ch in zip(specs, chunks):
            w = min(size, 4)
            try:
                words = [int(x, 0) for x in ch.split()]
            except ValueError:
                raise RuntimeError(out[:200] or "empty reply")
            if len(words) != size // w:
                raise RuntimeError(f"bad reply at 0x{addr:X}: {out[:200]}")
            raw = b"".join(struct.pack("<" + WIDTH[w], x) for x in words)
            vals[sid] = struct.unpack("<" + fmt, raw)[0]
        return vals

    def run(self):
        try:
            self.sock = socket.create_connection((self.host, self.port), timeout=2)
            self.sock.settimeout(3)
        except OSError as e:
            self.status.emit(f"Cannot connect to {self.host}:{self.port}: {e}")
            return
        self.status.emit(f"Connected to OpenOCD at {self.host}:{self.port}")
        t0 = time.perf_counter()
        try:
            while not self._stop:
                t_start = time.perf_counter()
                self._do_writes()
                specs = self.get_specs()
                if specs:
                    vals = self.read_values(specs)
                    self.sample.emit(time.perf_counter() - t0, vals)
                rest = self.get_period() - (time.perf_counter() - t_start)
                if rest > 0:
                    self.wake.wait(rest)
                self.wake.clear()
        except Exception as e:
            if not self._stop:
                self.status.emit(f"Read error: {e}")
        finally:
            try:
                self.sock.close()
            except Exception:
                pass


# --------------------------------------------------------------------------
# GUI
# --------------------------------------------------------------------------
MODES = ["None", "Moving average (N)", "Moving RMS (N)", "Median (N)", "Low-pass EMA (α)",
         "Derivative d/dt", "Integral ∫dt", "Absolute value", "Remove mean"]


def moving_mean(y, N):
    """Trailing moving average; the first samples average what is available."""
    n = len(y)
    c = np.concatenate(([0.0], np.cumsum(y)))
    idx = np.arange(1, n + 1)
    lo = np.maximum(0, idx - N)
    return (c[idx] - c[lo]) / (idx - lo)


def ema(y, a):
    """Exponential moving average, exact and vectorized (block-wise)."""
    b = 1.0 - a
    n = len(y)
    if b <= 0.0:
        return y.copy()
    B = max(1, min(n, int(150 * np.log(10) / -np.log(b))))
    bk = b ** np.arange(B)
    out = np.empty(n)
    carry = y[0]
    for s in range(0, n, B):
        x = y[s:s + B]
        m = len(x)
        seg = a * bk[:m] * np.cumsum(x / bk[:m]) + b * bk[:m] * carry
        out[s:s + m] = seg
        carry = seg[-1]
    return out


def apply_mode(mode, p, ts, y):
    n = len(y)
    if n == 0 or mode == "None":
        return y
    if mode.startswith("Moving average"):
        return moving_mean(y, max(1, int(p)))
    if mode.startswith("Moving RMS"):
        return np.sqrt(moving_mean(y * y, max(1, int(p))))
    if mode.startswith("Median"):
        N = min(max(1, int(p)), 101)
        if N == 1:
            return y
        pad = np.concatenate((np.full(N - 1, y[0]), y))
        return np.median(sliding_window_view(pad, N), axis=1)
    if mode.startswith("Low-pass"):
        return ema(y, min(max(p, 0.001), 1.0))
    if mode.startswith("Derivative"):
        return np.gradient(y, ts) if n >= 2 else np.zeros(n)
    if mode.startswith("Integral"):
        out = np.zeros(n)
        if n >= 2:
            out[1:] = np.cumsum((y[1:] + y[:-1]) * 0.5 * np.diff(ts))
        return out
    if mode == "Absolute value":
        return np.abs(y)
    if mode == "Remove mean":
        return y - y.mean()
    return y


def enum_values(d):
    """{value: name} for a DW_TAG_enumeration_type DIE."""
    out = {}
    for c in d.iter_children():
        if c.tag == "DW_TAG_enumerator":
            nm, v = die_name(c), attr(c, "DW_AT_const_value")
            if nm and isinstance(v, int):
                out.setdefault(v, nm)
    return out


def parse_value(text, fmt, enum=None):
    """Text typed by the user -> number. Accepts enum names, 'NAME (3)', 12, 0x1F, 1.5."""
    t = text.strip()
    if enum:
        by_name = {nm: v for v, nm in enum.items()}
        base = t.split(" (")[0].strip()
        if base in by_name:
            return by_name[base]
    if fmt in ("f", "d"):
        return float(t)
    return int(t, 0)


def build_write_cmd(addr, raw):
    """OpenOCD Tcl command(s) that write the bytes in `raw` (little endian) at addr."""
    size = len(raw)
    if size not in (1, 2, 4, 8):
        raise ValueError(f"unsupported size {size}")
    w = min(size, 4)
    words = struct.unpack("<" + WIDTH[w] * (size // w), raw)
    cmd = {1: "mwb", 2: "mwh", 4: "mww"}[w]
    return "; ".join(f"{cmd} 0x{addr + i * w:x} 0x{word:x}" for i, word in enumerate(words))


class Series:
    def __init__(self, sid, name, addr, size, fmt, color):
        self.id, self.name, self.addr, self.size, self.fmt, self.color = sid, name, addr, size, fmt, color
        self.active = True
        self.mode = "None"      # post-processing mode (see MODES)
        self.param = 10.0       # N samples or EMA alpha, depending on mode
        self.gain = 1.0
        self.offset = 0.0       # added after filter and gain
        self.t = deque(maxlen=MAXPTS)
        self.y = deque(maxlen=MAXPTS)
        self.last = None
        self.curve = None
        self.enum = None        # {value: name} when the variable is an enum

    def enum_active(self):
        return bool(self.enum) and self.fmt not in ("f", "d")

    def fmt_enum(self, v):
        iv = int(round(v))
        name = self.enum.get(iv)
        if name is None:  # signed enum stored with an unsigned DWARF form
            name = self.enum.get(iv & ((1 << (8 * self.size)) - 1))
        return f"{name} ({iv})" if name else f"{iv}"


class MainWindow(QtWidgets.QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("SWD Live Plot")
        self.model = None
        self.series = []
        self.lock = threading.Lock()
        self.poller = None
        self.next_id = 0
        self.t_last = 0.0
        self.follow = True
        self._enum_ticks = None
        self.period = 0.01
        self._build_ui()
        self.timer = QtCore.QTimer(self)
        self.timer.timeout.connect(self.refresh)
        self.timer.start(50)

    # ---- UI construction
    def _build_ui(self):
        W = QtWidgets
        top = W.QHBoxLayout()
        self.btn_open = W.QPushButton("Open ELF…")
        self.btn_open.clicked.connect(self.open_elf)
        self.lbl_elf = W.QLabel("no file loaded")
        self.edit_host = W.QLineEdit("localhost")
        self.edit_host.setFixedWidth(100)
        self.spin_port = W.QSpinBox()
        self.spin_port.setRange(1, 65535)
        self.spin_port.setValue(6666)
        self.btn_conn = W.QPushButton("Connect")
        self.btn_conn.clicked.connect(self.toggle_connect)
        self.spin_period = W.QSpinBox()
        self.spin_period.setRange(1, 5000)
        self.spin_period.setValue(10)
        self.spin_period.setSuffix(" ms")
        self.spin_period.valueChanged.connect(lambda v: setattr(self, "period", v / 1000.0))
        self.spin_window = W.QDoubleSpinBox()
        self.spin_window.setRange(0.5, 3600)
        self.spin_window.setValue(10)
        self.spin_window.setSuffix(" s")
        self.btn_follow = W.QPushButton("Follow live")
        self.btn_follow.setCheckable(True)
        self.btn_follow.setChecked(True)
        self.btn_follow.setToolTip("Scroll the view with the newest data")
        self.btn_follow.toggled.connect(self.set_follow)
        self.btn_fit = W.QPushButton("Fit")
        self.btn_fit.setToolTip("Fit all data into the view (stops following)")
        self.btn_fit.clicked.connect(self.fit_view)
        self.btn_fit_y = QtWidgets.QPushButton("Fit Y")
        self.btn_fit_y.setToolTip("Fit the vertical range to the data visible in the current time range")
        self.btn_fit_y.clicked.connect(self.fit_y)
        self.btn_cursors = W.QPushButton("Cursors")
        self.btn_cursors.setCheckable(True)
        self.btn_cursors.setToolTip("Two measurement cursors (stops following so the view stays still)")
        self.btn_cursors.toggled.connect(self.set_cursors)
        self.btn_clear = W.QPushButton("Clear data")
        self.btn_clear.clicked.connect(self.clear_data)
        self.btn_csv = W.QPushButton("Export CSV…")
        self.btn_csv.clicked.connect(self.export_csv)
        top.addWidget(self.btn_open)
        top.addWidget(self.lbl_elf, 1)
        for lbl, w in (("OpenOCD Tcl host", self.edit_host), ("port", self.spin_port)):
            top.addWidget(W.QLabel(lbl))
            top.addWidget(w)
        top.addWidget(self.btn_conn)
        top.addWidget(W.QLabel("Period"))
        top.addWidget(self.spin_period)
        top.addWidget(W.QLabel("Window"))
        top.addWidget(self.spin_window)
        top.addWidget(self.btn_follow)
        top.addWidget(self.btn_fit)
        top.addWidget(self.btn_fit_y)
        top.addWidget(self.btn_cursors)
        top.addWidget(self.btn_clear)
        top.addWidget(self.btn_csv)

        # left: variable tree
        left = W.QWidget()
        ll = W.QVBoxLayout(left)
        ll.setContentsMargins(0, 0, 0, 0)
        self.edit_filter = W.QLineEdit()
        self.edit_filter.setPlaceholderText("Filter variables…")
        self.edit_filter.textChanged.connect(self.apply_filter)
        self.tree = W.QTreeWidget()
        self.tree.setHeaderLabels(["Variable", "Address", "Size", "Type"])
        self.tree.setColumnWidth(0, 220)
        self.tree.setColumnWidth(1, 90)
        self.tree.setColumnWidth(2, 45)
        self.tree.setEditTriggers(W.QAbstractItemView.EditTrigger.NoEditTriggers)
        self.tree.setSelectionMode(W.QAbstractItemView.SelectionMode.ExtendedSelection)
        self.tree.itemExpanded.connect(self.on_expand)
        self.tree.itemDoubleClicked.connect(lambda it, _c: self.add_items([it]))
        btn_add = W.QPushButton("Add selected to plot ▶")
        btn_add.clicked.connect(lambda: self.add_items(self.tree.selectedItems()))
        btn_addr = W.QPushButton("Add by address…")
        btn_addr.clicked.connect(self.add_by_address)
        row = W.QHBoxLayout()
        row.addWidget(btn_add)
        row.addWidget(btn_addr)
        ll.addWidget(self.edit_filter)
        ll.addWidget(self.tree, 1)
        ll.addLayout(row)

        # right: plot + series table
        self.plot = pg.PlotWidget()
        self.plot.showGrid(x=True, y=True, alpha=0.3)
        self.plot.setLabel("bottom", "time", units="s")
        self.plot.setClipToView(True)
        self.plot.setDownsampling(mode="peak", auto=True)
        self.plot.plotItem.vb.setAutoVisible(y=True)
        self.legend = self.plot.addLegend()
        self.table = W.QTableWidget(0, 10)
        self.table.setHorizontalHeaderLabels(
            ["Show", "Name", "Address", "Type", "Offset", "Value (dbl-click: write)", "@C1", "@C2",
             "Δ (C2−C1)", ""])
        self.table.setEditTriggers(W.QAbstractItemView.EditTrigger.NoEditTriggers)
        self.table.setSelectionBehavior(W.QAbstractItemView.SelectionBehavior.SelectRows)
        self.table.verticalHeader().setVisible(False)
        self.table.horizontalHeader().setSectionResizeMode(1, W.QHeaderView.ResizeMode.Stretch)
        for col, wd in ((0, 45), (2, 90), (3, 90), (4, 110), (5, 170), (6, 130), (7, 130), (8, 90), (9, 40)):
            self.table.setColumnWidth(col, wd)
        self.table.cellDoubleClicked.connect(lambda r, c: self.edit_value(r) if c == 5 else None)
        SC = getattr(QtGui, "QShortcut", None) or QtWidgets.QShortcut
        self.sc_del = SC(QtGui.QKeySequence("Delete"), self.table)
        self.sc_del.activated.connect(self.remove_selected)

        btn_rm_sel = W.QPushButton("Remove selected (Del)")
        btn_rm_sel.clicked.connect(self.remove_selected)
        btn_rm_all = W.QPushButton("Remove all variables")
        btn_rm_all.clicked.connect(self.remove_all_series)
        btn_write = W.QPushButton("Write value…")
        btn_write.setToolTip("Write a new value into the selected variable's memory (needs Connect)")
        btn_write.clicked.connect(lambda: self.edit_value())
        rowb = W.QHBoxLayout()
        rowb.addWidget(btn_write)
        rowb.addWidget(btn_rm_sel)
        rowb.addWidget(btn_rm_all)
        rowb.addStretch(1)
        tbl_panel = W.QWidget()
        tl = W.QVBoxLayout(tbl_panel)
        tl.setContentsMargins(0, 0, 0, 0)
        self._loading = False
        self.pp_box = W.QGroupBox("Post-processing of selected variable(s)  (click a row's name to select)")
        pl = W.QHBoxLayout(self.pp_box)
        self.lbl_sel = W.QLabel("")
        self.cmb_mode = W.QComboBox()
        self.cmb_mode.addItems(MODES)
        self.spin_param = W.QDoubleSpinBox()
        self.spin_gain = W.QDoubleSpinBox()
        self.spin_gain.setRange(-1e9, 1e9)
        self.spin_gain.setDecimals(6)
        self.spin_gain.setSingleStep(0.1)
        self.spin_gain.setValue(1.0)
        btn_pp_reset = W.QPushButton("Reset")
        btn_pp_reset.setToolTip("Mode None, gain 1, offset 0 for the selected variable(s)")
        pl.addWidget(self.lbl_sel, 1)
        for lbl, wdg in (("Mode", self.cmb_mode), ("N / α", self.spin_param), ("Gain ×", self.spin_gain)):
            pl.addWidget(W.QLabel(lbl))
            pl.addWidget(wdg)
        pl.addWidget(btn_pp_reset)
        self._configure_param(MODES[0])
        self.cmb_mode.currentIndexChanged.connect(self.on_mode_changed)
        self.spin_param.valueChanged.connect(self.on_pp_changed)
        self.spin_gain.valueChanged.connect(self.on_pp_changed)
        btn_pp_reset.clicked.connect(self.reset_pp)
        self.table.itemSelectionChanged.connect(self.on_selection)

        tl.addLayout(rowb)
        tl.addWidget(self.pp_box)
        tl.addWidget(self.table)
        self.on_selection()

        # measurement cursors
        self.cursor1 = pg.InfiniteLine(angle=90, movable=True, pen=pg.mkPen("#ffd54f", width=2),
                                       label="C1 {value:0.3f}s",
                                       labelOpts={"position": 0.95, "color": "#ffd54f"})
        self.cursor2 = pg.InfiniteLine(angle=90, movable=True, pen=pg.mkPen("#4fc3f7", width=2),
                                       label="C2 {value:0.3f}s",
                                       labelOpts={"position": 0.88, "color": "#4fc3f7"})
        for c in (self.cursor1, self.cursor2):
            c.setVisible(False)
            self.plot.addItem(c)

        right = W.QSplitter(QtCore.Qt.Orientation.Vertical)
        right.addWidget(self.plot)
        right.addWidget(tbl_panel)
        right.setSizes([520, 280])

        split = W.QSplitter(QtCore.Qt.Orientation.Horizontal)
        split.addWidget(left)
        split.addWidget(right)
        split.setSizes([420, 980])

        central = W.QWidget()
        cl = W.QVBoxLayout(central)
        cl.addLayout(top)
        cl.addWidget(split, 1)
        self.setCentralWidget(central)
        self.statusBar().showMessage("Open an ELF, add variables, start debugging in VS Code, then Connect.")

    # ---- ELF / tree
    def open_elf(self):
        path, _ = QtWidgets.QFileDialog.getOpenFileName(
            self, "Open firmware ELF", "", "ELF files (*.elf *.axf *.out);;All files (*)")
        if path:
            self.load_elf(path)

    def load_elf(self, path):
        QtWidgets.QApplication.setOverrideCursor(QtCore.Qt.CursorShape.WaitCursor)
        try:
            if self.model:
                self.model.close()
            self.model = ElfModel(path)
        except Exception as e:
            QtWidgets.QApplication.restoreOverrideCursor()
            QtWidgets.QMessageBox.critical(self, "ELF error", str(e))
            return
        QtWidgets.QApplication.restoreOverrideCursor()
        self.tree.clear()
        for n in self.model.roots:
            self.tree.addTopLevelItem(self._make_item(n))
        self.lbl_elf.setText(path)
        note = "" if self.model.has_debug else "  (no DWARF info: raw symbols only, build with -g for types/structs)"
        self.statusBar().showMessage(f"{len(self.model.roots)} variables loaded{note}")

    def _make_item(self, node):
        it = QtWidgets.QTreeWidgetItem([node.label, f"0x{node.addr:08X}", str(node.size), node.typename])
        it.setData(0, QtCore.Qt.ItemDataRole.UserRole, node)
        if node.expandable:
            it.addChild(QtWidgets.QTreeWidgetItem(["…"]))  # placeholder, replaced on expand
        return it

    def on_expand(self, it):
        node = it.data(0, QtCore.Qt.ItemDataRole.UserRole)
        if node is None or it.childCount() != 1:
            return
        if it.child(0).data(0, QtCore.Qt.ItemDataRole.UserRole) is not None:
            return
        it.takeChild(0)
        for k in node.children():
            it.addChild(self._make_item(k))

    def apply_filter(self, text):
        t = text.strip().lower()
        for i in range(self.tree.topLevelItemCount()):
            it = self.tree.topLevelItem(i)
            it.setHidden(bool(t) and t not in it.text(0).lower())

    # ---- series management
    def add_items(self, items):
        leaves = []
        for it in items:
            node = it.data(0, QtCore.Qt.ItemDataRole.UserRole)
            if node is not None:
                collect_leaves(node, leaves)
        if not leaves:
            self.statusBar().showMessage("Select a scalar variable (or a struct/array to add its members).")
            return
        for n in leaves:
            self.add_series(n.path, n.addr, n.leaf[0], n.leaf[1], n.enum_map)

    def add_by_address(self):
        text, ok = QtWidgets.QInputDialog.getText(self, "Add by address", "Hex address (e.g. 0x20000010):")
        if not ok or not text.strip():
            return
        try:
            addr = int(text.strip(), 0)
        except ValueError:
            QtWidgets.QMessageBox.warning(self, "Bad address", "Could not parse the address.")
            return
        self.add_series(f"mem_{addr:08X}", addr, 4, "I")

    def add_series(self, name, addr, size, fmt, enum=None):
        if any(s.name == name and s.addr == addr for s in self.series):
            return
        color = COLORS[self.next_id % len(COLORS)]
        s = Series(self.next_id, name, addr, size, fmt, color)
        s.enum = enum
        self.next_id += 1
        s.curve = self.plot.plot(pen=pg.mkPen(color, width=2), name=name)
        with self.lock:
            self.series.append(s)
        self._add_row(s)

    def _add_row(self, s):
        W = QtWidgets
        r = self.table.rowCount()
        self.table.insertRow(r)
        chk = W.QCheckBox()
        chk.setChecked(True)
        chk.toggled.connect(lambda on, s=s: self.set_active(s, on))
        self.table.setCellWidget(r, 0, chk)
        name = W.QTableWidgetItem(s.name)
        name.setForeground(QtGui.QBrush(QtGui.QColor(s.color)))
        self.table.setItem(r, 1, name)
        self.table.setItem(r, 2, W.QTableWidgetItem(f"0x{s.addr:08X}"))
        combo = W.QComboBox()
        combo.addItems(list(TYPES))
        combo.setCurrentText(FMT_TO_TYPE.get((s.size, s.fmt), "uint32"))
        combo.currentTextChanged.connect(lambda txt, s=s: self.set_type(s, txt))
        self.table.setCellWidget(r, 3, combo)
        off = W.QDoubleSpinBox()
        off.setRange(-1e9, 1e9)
        off.setDecimals(6)
        off.setStepType(W.QAbstractSpinBox.StepType.AdaptiveDecimalStepType)
        off.setValue(s.offset)
        off.valueChanged.connect(lambda v, s=s: setattr(s, "offset", v))
        self.table.setCellWidget(r, 4, off)
        self.table.setItem(r, 5, W.QTableWidgetItem("–"))
        for col in (6, 7, 8):
            self.table.setItem(r, col, W.QTableWidgetItem(""))
        btn = W.QPushButton("✕")
        btn.clicked.connect(lambda _checked=False, s=s: self.remove_series(s))
        self.table.setCellWidget(r, 9, btn)

    def set_active(self, s, on):
        with self.lock:
            s.active = on
        s.curve.setVisible(on)

    def set_type(self, s, tname):
        size, fmt = TYPES[tname]
        with self.lock:
            s.size, s.fmt = size, fmt
        s.t.clear()
        s.y.clear()

    def remove_series(self, s):
        if s not in self.series:
            return
        idx = self.series.index(s)
        with self.lock:
            self.series.pop(idx)
        self.table.removeRow(idx)
        self.plot.removeItem(s.curve)
        try:
            self.legend.removeItem(s.curve)
        except Exception:
            pass

    def get_specs(self):
        with self.lock:
            return [(s.id, s.addr, s.size, s.fmt) for s in self.series if s.active]

    # ---- connection
    def toggle_connect(self):
        if self.poller is not None:
            self.poller.stop()
            self.poller.wait(3000)
            self.poller = None
            self.btn_conn.setText("Connect")
            self.statusBar().showMessage("Disconnected")
            return
        self.poller = Poller(self.edit_host.text().strip() or "localhost", self.spin_port.value(),
                             self.get_specs, lambda: self.period)
        self.poller.sample.connect(self.on_sample)
        self.poller.status.connect(self.statusBar().showMessage)
        self.poller.finished.connect(self.on_poller_finished)
        self.poller.start()
        self.btn_conn.setText("Disconnect")

    def on_poller_finished(self):
        if self.poller is not None and not self.poller.isRunning():
            self.poller = None
            self.btn_conn.setText("Connect")

    def on_sample(self, t, values):
        self.t_last = t
        for s in self.series:
            v = values.get(s.id)
            if v is not None:
                s.t.append(t)
                s.y.append(v)
                s.last = v

    # ---- display
    @staticmethod
    def _at(ts, ys, x):
        """Linear interpolation of a series at time x, or None if x is outside the data."""
        if ts is None or len(ts) == 0 or x < ts[0] or x > ts[-1]:
            return None
        return float(np.interp(x, ts, ys))

    @staticmethod
    def _step_at(ts, ys, x):
        """Value of the latest sample at or before x (for enums), or None outside the data."""
        if ts is None or len(ts) == 0 or x < ts[0] or x > ts[-1]:
            return None
        return float(ys[int(np.searchsorted(ts, x, "right")) - 1])

    def _set_text(self, row, col, text):
        item = self.table.item(row, col)
        if item is not None:
            item.setText(text)

    def _update_enum_axis(self):
        """Label the Y axis with enum names when every visible variable is a plain enum."""
        act = [s for s in self.series if s.active]
        ok = bool(act) and all(s.enum_active() and s.mode == "None" and s.gain == 1.0 and s.offset == 0.0
                               for s in act)
        ticks = None
        if ok:
            merged = {}
            for s in act:
                merged.update(s.enum)
            if len(merged) <= 64:
                ticks = tuple(sorted(merged.items()))
        if ticks == self._enum_ticks:
            return
        self._enum_ticks = ticks
        self.plot.getAxis("left").setTicks(None if ticks is None else [[(float(v), nm) for v, nm in ticks]])

    def _set_cell(self, row, col, v):
        item = self.table.item(row, col)
        if item is not None:
            item.setText("–" if v is None else f"{v:.6g}")

    def refresh(self):
        cur = self.btn_cursors.isChecked()
        x1, x2 = self.cursor1.value(), self.cursor2.value()
        for i, s in enumerate(self.series):
            n = len(s.t)
            ts = ys = raw = None
            if n:
                ts = np.fromiter(s.t, float, n)
                raw = np.fromiter(s.y, float, n)
                ys = self.process(s, ts, raw)
                if s.active:
                    s.curve.setData(ts, ys)
            en = s.enum_active()
            item = self.table.item(i, 5)
            if item is not None and n:
                item.setText(s.fmt_enum(raw[-1]) if en else f"{ys[-1]:.6g}")
            if cur:
                if en:
                    a, b = self._step_at(ts, raw, x1), self._step_at(ts, raw, x2)
                    self._set_text(i, 6, "–" if a is None else s.fmt_enum(a))
                    self._set_text(i, 7, "–" if b is None else s.fmt_enum(b))
                    self._set_text(i, 8, "" if a is None or b is None else ("changed" if a != b else "same"))
                else:
                    v1, v2 = self._at(ts, ys, x1), self._at(ts, ys, x2)
                    self._set_cell(i, 6, v1)
                    self._set_cell(i, 7, v2)
                    self._set_cell(i, 8, v2 - v1 if v1 is not None and v2 is not None else None)
        self._update_enum_axis()
        if cur:
            dt = x2 - x1
            freq = f"   ({1.0 / abs(dt):.4g} Hz)" if dt else ""
            self.plot.setTitle(f"C1 = {x1:.4f} s    C2 = {x2:.4f} s    Δt = {dt:.4f} s{freq}", size="10pt")
        if self.follow and self.series:
            hi = self.t_last
            self.plot.setXRange(max(0.0, hi - self.spin_window.value()), max(hi, 0.1), padding=0)

    def set_follow(self, on):
        self.follow = on
        self.plot.plotItem.vb.setAutoVisible(y=on)
        if on:
            self.plot.enableAutoRange(axis="y")

    def fit_view(self):
        self.btn_follow.setChecked(False)
        self.follow = False
        self.plot.plotItem.vb.setAutoVisible(y=False)
        self.plot.enableAutoRange()
        self.plot.autoRange()

    def set_cursors(self, on):
        for c in (self.cursor1, self.cursor2):
            c.setVisible(on)
        if on:
            self.btn_follow.setChecked(False)
            (x0, x1), _ = self.plot.viewRange()
            self.cursor1.setValue(x0 + 0.35 * (x1 - x0))
            self.cursor2.setValue(x0 + 0.65 * (x1 - x0))
        else:
            self.plot.setTitle(None)
            for i in range(len(self.series)):
                for col in (6, 7, 8):
                    item = self.table.item(i, col)
                    if item is not None:
                        item.setText("")

    def remove_selected(self):
        rows = sorted({ix.row() for ix in self.table.selectedIndexes()}, reverse=True)
        for r in rows:
            if r < len(self.series):
                self.remove_series(self.series[r])

    def remove_all_series(self):
        for s in list(self.series):
            self.remove_series(s)

    # ---- writing values
    def edit_value(self, row=None):
        if row is None:
            sel = self.selected_series()
            if not sel:
                self.statusBar().showMessage("Select a variable (click its name) to write a value.")
                return
            s = sel[0]
        elif 0 <= row < len(self.series):
            s = self.series[row]
        else:
            return
        if self.poller is None:
            QtWidgets.QMessageBox.information(self, "Not connected", "Press Connect first.")
            return
        cur = s.last
        title = f"Write {s.name}"
        if s.enum_active():
            items = [f"{nm} ({v})" for v, nm in sorted(s.enum.items())]
            idx = 0
            if cur is not None:
                label = s.fmt_enum(cur)
                idx = items.index(label) if label in items else 0
            text, ok = QtWidgets.QInputDialog.getItem(
                self, title, "Pick an enum value (or type a number):", items, idx, True)
        else:
            default = "" if cur is None else (f"{cur:.9g}" if s.fmt in ("f", "d") else str(int(cur)))
            text, ok = QtWidgets.QInputDialog.getText(
                self, title, f"New value ({FMT_TO_TYPE.get((s.size, s.fmt), '?')}, "
                             "decimal or 0x hex; written raw, ignores gain/offset):",
                text=default)
        if not ok or not text.strip():
            return
        try:
            value = parse_value(text, s.fmt, s.enum if s.enum_active() else None)
            raw = struct.pack("<" + s.fmt, value)
        except (ValueError, struct.error, OverflowError) as e:
            QtWidgets.QMessageBox.warning(self, "Invalid value", f"Cannot write '{text}': {e}")
            return
        self.poller.request_write(s.name, s.addr, raw)

    # ---- vertical fit
    def fit_y(self):
        self.plot.plotItem.vb.setAutoVisible(y=True)
        self.plot.enableAutoRange(axis="y")

    # ---- post-processing
    @staticmethod
    def process(s, ts, ys):
        """raw -> filter/transform -> gain -> offset"""
        y = apply_mode(s.mode, s.param, ts, ys)
        if s.gain != 1.0 or s.offset != 0.0:
            y = y * s.gain + s.offset
        return y

    def selected_series(self):
        rows = sorted({ix.row() for ix in self.table.selectedIndexes()})
        return [self.series[r] for r in rows if r < len(self.series)]

    def _configure_param(self, mode, value=None):
        if mode.startswith("Low-pass"):
            lo, hi, dec, default, on = 0.001, 1.0, 3, 0.1, True
        elif mode.startswith("Median"):
            lo, hi, dec, default, on = 1, 101, 0, 9, True
        elif mode.startswith("Moving"):
            lo, hi, dec, default, on = 1, 10000, 0, 10, True
        else:
            lo, hi, dec, default, on = 0, 0, 0, 0, False
        self._loading = True
        self.spin_param.setDecimals(dec)
        self.spin_param.setRange(lo, hi)
        self.spin_param.setValue(default if value is None else value)
        self.spin_param.setEnabled(on)
        self._loading = False

    def on_selection(self):
        sel = self.selected_series()
        self.pp_box.setEnabled(bool(sel))
        if not sel:
            self.lbl_sel.setText("no variable selected")
            return
        s = sel[0]
        self.lbl_sel.setText(", ".join(x.name for x in sel[:3]) + (" …" if len(sel) > 3 else ""))
        self._loading = True
        self.cmb_mode.setCurrentText(s.mode)
        self.spin_gain.setValue(s.gain)
        self._loading = False
        self._configure_param(s.mode, s.param)

    def on_mode_changed(self, _i=0):
        if self._loading:
            return
        self._configure_param(self.cmb_mode.currentText())
        self.on_pp_changed()

    def on_pp_changed(self, *_):
        if self._loading:
            return
        mode, p, g = self.cmb_mode.currentText(), self.spin_param.value(), self.spin_gain.value()
        for s in self.selected_series():
            s.mode, s.param, s.gain = mode, p, g

    def reset_pp(self):
        for s in self.selected_series():
            s.mode, s.param, s.gain = "None", 10.0, 1.0
            sp = self.table.cellWidget(self.series.index(s), 4)
            if sp is not None:
                sp.setValue(0.0)
        self.on_selection()

    def clear_data(self):
        for s in self.series:
            s.t.clear()
            s.y.clear()
            s.last = None
            s.curve.setData([], [])

    def export_csv(self):
        path, _ = QtWidgets.QFileDialog.getSaveFileName(self, "Export CSV", "samples.csv", "CSV (*.csv)")
        if not path:
            return
        with open(path, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["variable", "time_s", "raw", "processed"])
            for s in self.series:
                n = len(s.t)
                if not n:
                    continue
                ts = np.fromiter(s.t, float, n)
                raw = np.fromiter(s.y, float, n)
                proc = self.process(s, ts, raw)
                for t, r, p in zip(ts, raw, proc):
                    w.writerow([s.name, f"{t:.6f}", r, p])
        self.statusBar().showMessage(f"Saved {path}")

    def closeEvent(self, ev):
        if self.poller is not None:
            self.poller.stop()
            self.poller.wait(3000)
        if self.model:
            self.model.close()
        super().closeEvent(ev)


def apply_dark_theme(app):
    app.setStyle("Fusion")
    P, C = QtGui.QPalette, QtGui.QColor
    pal = QtGui.QPalette()
    roles = {
        P.ColorRole.Window: C(37, 37, 38),
        P.ColorRole.WindowText: C(220, 220, 220),
        P.ColorRole.Base: C(30, 30, 30),
        P.ColorRole.AlternateBase: C(45, 45, 48),
        P.ColorRole.ToolTipBase: C(50, 50, 55),
        P.ColorRole.ToolTipText: C(220, 220, 220),
        P.ColorRole.Text: C(220, 220, 220),
        P.ColorRole.PlaceholderText: C(128, 128, 128),
        P.ColorRole.Button: C(51, 51, 55),
        P.ColorRole.ButtonText: C(220, 220, 220),
        P.ColorRole.BrightText: C(255, 80, 80),
        P.ColorRole.Link: C(86, 156, 214),
        P.ColorRole.Highlight: C(38, 79, 120),
        P.ColorRole.HighlightedText: C(255, 255, 255),
    }
    for role, col in roles.items():
        pal.setColor(role, col)
    for role in (P.ColorRole.Text, P.ColorRole.WindowText, P.ColorRole.ButtonText):
        pal.setColor(P.ColorGroup.Disabled, role, C(110, 110, 110))
    app.setPalette(pal)


def main():
    app = QtWidgets.QApplication(sys.argv)
    apply_dark_theme(app)
    pg.setConfigOptions(background="#1e1e1e", foreground="#d4d4d4")
    w = MainWindow()
    w.resize(1400, 820)
    w.show()
    if len(sys.argv) > 1:
        w.load_elf(sys.argv[1])
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
