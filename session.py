"""Debug session without VS Code: start OpenOCD from the app and connect to it.

build_openocd_args / find_openocd / OpenOcdSession  no Qt, testable on their own
SessionDialog                                       the "Debug session…" window

OpenOCD itself must be installed (for example `sudo apt install openocd`); this module only launches
and supervises it. Only one program can use the debug probe at a time.
"""
import os
import queue
import re
import shlex
import shutil
import subprocess
import threading
import time

from pyqtgraph.Qt import QtCore, QtGui, QtWidgets

INTERFACES = ["interface/stlink.cfg", "interface/stlink-dap.cfg", "interface/cmsis-dap.cfg",
              "interface/jlink.cfg"]
TARGETS = ["target/stm32f0x.cfg", "target/stm32f1x.cfg", "target/stm32f2x.cfg", "target/stm32f3x.cfg",
           "target/stm32f4x.cfg", "target/stm32f7x.cfg", "target/stm32g0x.cfg", "target/stm32g4x.cfg",
           "target/stm32h5x.cfg", "target/stm32h7x.cfg", "target/stm32l0.cfg", "target/stm32l1.cfg",
           "target/stm32l4x.cfg", "target/stm32l5x.cfg", "target/stm32u5x.cfg", "target/stm32wbx.cfg",
           "target/stm32wlx.cfg", "target/nrf52.cfg", "target/rp2040.cfg"]
GDB_PORT = 3333


def find_openocd():
    """Path of the openocd executable, or '' if it cannot be found."""
    found = shutil.which("openocd")
    if found:
        return found
    for p in ("/usr/bin/openocd", "/usr/local/bin/openocd", "/opt/openocd/bin/openocd", "/snap/bin/openocd"):
        if os.path.isfile(p):
            return p
    return ""


def build_openocd_args(interface, target, tcl_port, gdb_open=False, extra="", scripts=""):
    """Command-line arguments (without the executable) for an OpenOCD debug session."""
    args = []
    if scripts.strip():
        args += ["-s", scripts.strip()]
    for cfg in (interface, target):
        for item in shlex.split(cfg):
            args += ["-f", item]
    args += ["-c", f"tcl_port {int(tcl_port)}", "-c", "telnet_port disabled",
             "-c", f"gdb_port {GDB_PORT}" if gdb_open else "gdb_port disabled"]
    args += shlex.split(extra)
    return args


class OpenOcdSession:
    """Runs OpenOCD as a child process and collects its output without blocking the caller."""
    READY = re.compile(r"Listening on port (\d+) for tcl connections")

    def __init__(self, exe, args):
        self.exe, self.args = exe, list(args)
        self.proc = None
        self.q = queue.Queue()
        self.tcl_port = None        # set when OpenOCD reports that the Tcl port is open
        self.eof = False            # all output has been read
        self.error = None

    def start(self):
        self.proc = subprocess.Popen([self.exe] + self.args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     stdin=subprocess.DEVNULL, text=True, bufsize=1, errors="replace")
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        try:
            for line in self.proc.stdout:
                self.q.put(line.rstrip("\r\n"))
        finally:
            self.q.put(None)

    def drain(self):
        """New output lines since the last call (and updates tcl_port / eof)."""
        lines = []
        while True:
            try:
                item = self.q.get_nowait()
            except queue.Empty:
                break
            if item is None:
                self.eof = True
                continue
            lines.append(item)
            m = self.READY.search(item)
            if m:
                self.tcl_port = int(m.group(1))
        return lines

    def running(self):
        return self.proc is not None and self.proc.poll() is None

    @property
    def returncode(self):
        return None if self.proc is None else self.proc.poll()

    def stop(self):
        if self.proc is not None and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()


class SessionDialog(QtWidgets.QDialog):
    """Start OpenOCD from the app, connect to it automatically, and control the target."""

    def __init__(self, main):
        super().__init__(main)
        W = QtWidgets
        self.main = main
        self.session = None
        self._auto_done = False
        self._started_at = 0.0
        st = main.settings
        self.setWindowTitle("Debug session")
        self.resize(760, 640)

        def val(key, default=""):
            v = st.value("session/" + key)
            return default if v is None else str(v)

        self.edit_exe = W.QLineEdit(val("exe", find_openocd()))
        self.edit_exe.setPlaceholderText("path to openocd (install it with: sudo apt install openocd)")
        btn_exe = W.QPushButton("Browse…")
        btn_exe.clicked.connect(self._browse_exe)
        self.cmb_if = W.QComboBox()
        self.cmb_if.setEditable(True)
        self.cmb_if.addItems(INTERFACES)
        self.cmb_if.setCurrentText(val("interface", INTERFACES[0]))
        self.cmb_target = W.QComboBox()
        self.cmb_target.setEditable(True)
        self.cmb_target.addItems(TARGETS)
        self.cmb_target.setCurrentText(val("target", "target/stm32f4x.cfg"))
        self.edit_scripts = W.QLineEdit(val("scripts"))
        self.edit_scripts.setPlaceholderText("optional: OpenOCD scripts folder (-s)")
        self.spin_port = W.QSpinBox()
        self.spin_port.setRange(1, 65535)
        self.spin_port.setValue(int(val("tcl_port", str(main.spin_port.value())) or 50001))
        self.chk_gdb = W.QCheckBox(f"Also open the GDB port ({GDB_PORT}) so a debugger can attach")
        self.chk_gdb.setChecked(val("gdb_open", "0") in ("1", "true", "True"))
        self.edit_extra = W.QLineEdit(val("extra"))
        self.edit_extra.setPlaceholderText('optional extra arguments, e.g.  -c "adapter speed 4000"')

        erow = W.QHBoxLayout()
        erow.addWidget(self.edit_exe, 1)
        erow.addWidget(btn_exe)
        form = W.QFormLayout()
        form.addRow("OpenOCD", erow)
        form.addRow("Debug probe (interface)", self.cmb_if)
        form.addRow("Target chip", self.cmb_target)
        form.addRow("Scripts folder", self.edit_scripts)
        form.addRow("Tcl port", self.spin_port)
        form.addRow("", self.chk_gdb)
        form.addRow("Extra arguments", self.edit_extra)

        self.btn_go = W.QPushButton("Start session")
        self.btn_go.clicked.connect(self.toggle)
        self.btn_halt = W.QPushButton("Halt")
        self.btn_resume = W.QPushButton("Resume")
        self.btn_reset = W.QPushButton("Reset (run)")
        self.btn_reset_halt = W.QPushButton("Reset (halt)")
        self.btn_flash = W.QPushButton("Flash ELF…")
        self.btn_halt.clicked.connect(lambda: self._cmd("Halt", "halt"))
        self.btn_resume.clicked.connect(lambda: self._cmd("Resume", "resume"))
        self.btn_reset.clicked.connect(lambda: self._cmd("Reset", "reset run"))
        self.btn_reset_halt.clicked.connect(lambda: self._cmd("Reset (halt)", "reset halt"))
        self.btn_flash.clicked.connect(self.flash)
        crow = W.QHBoxLayout()
        for b in (self.btn_halt, self.btn_resume, self.btn_reset, self.btn_reset_halt, self.btn_flash):
            crow.addWidget(b)
        crow.addStretch(1)

        self.log = W.QPlainTextEdit()
        self.log.setReadOnly(True)
        self.log.setMaximumBlockCount(3000)
        font = QtGui.QFont("monospace")
        font.setStyleHint(QtGui.QFont.StyleHint.Monospace)
        self.log.setFont(font)
        self.lbl = W.QLabel("")
        self.lbl.setWordWrap(True)
        hint = W.QLabel("Only one program can use the debug probe at a time: close the VS Code debug session "
                        "first, or tick the GDB option and attach VS Code to this session "
                        "(Cortex-Debug servertype \"external\", gdbTarget \"localhost:3333\").")
        hint.setWordWrap(True)
        hint.setStyleSheet("color: #9e9e9e;")

        lay = W.QVBoxLayout(self)
        lay.addLayout(form)
        lay.addWidget(self.btn_go)
        lay.addLayout(crow)
        lay.addWidget(self.lbl)
        lay.addWidget(self.log, 1)
        lay.addWidget(hint)

        self.timer = QtCore.QTimer(self)
        self.timer.timeout.connect(self._poll)
        self.timer.start(100)
        self._update()

    # ---- helpers
    def _browse_exe(self):
        path, _ = QtWidgets.QFileDialog.getOpenFileName(self, "Choose the openocd executable",
                                                        os.path.dirname(self.edit_exe.text()) or "")
        if path:
            self.edit_exe.setText(path)

    def _say(self, text):
        self.log.appendPlainText(text)

    def _save(self):
        st = self.main.settings
        for key, v in (("exe", self.edit_exe.text()), ("interface", self.cmb_if.currentText()),
                       ("target", self.cmb_target.currentText()), ("scripts", self.edit_scripts.text()),
                       ("tcl_port", str(self.spin_port.value())), ("extra", self.edit_extra.text()),
                       ("gdb_open", "1" if self.chk_gdb.isChecked() else "0")):
            st.setValue("session/" + key, v)
        st.sync()

    def _update(self):
        running = self.session is not None and self.session.running()
        self.btn_go.setText("Stop session" if running else "Start session")
        for w in (self.edit_exe, self.cmb_if, self.cmb_target, self.edit_scripts, self.spin_port,
                  self.chk_gdb, self.edit_extra):
            w.setEnabled(not running)
        connected = self.main.poller is not None
        for b in (self.btn_halt, self.btn_resume, self.btn_reset, self.btn_reset_halt, self.btn_flash):
            b.setEnabled(connected)

    # ---- start / stop
    def toggle(self):
        if self.session is not None and self.session.running():
            self.stop()
        else:
            self.start()

    def start(self):
        W = QtWidgets
        exe = self.edit_exe.text().strip() or "openocd"
        if shutil.which(exe) is None and not os.path.isfile(exe):
            W.QMessageBox.warning(self, "OpenOCD not found",
                                  "Cannot find the openocd program.\n\nInstall it (Debian/Ubuntu: "
                                  "sudo apt install openocd) or choose its location with Browse…")
            return
        if not self.cmb_target.currentText().strip():
            W.QMessageBox.warning(self, "Target chip", "Choose or type the target configuration file.")
            return
        try:
            args = build_openocd_args(self.cmb_if.currentText(), self.cmb_target.currentText(),
                                      self.spin_port.value(), self.chk_gdb.isChecked(),
                                      self.edit_extra.text(), self.edit_scripts.text())
        except ValueError as e:
            W.QMessageBox.warning(self, "Extra arguments", f"Cannot read the extra arguments: {e}")
            return
        self._save()
        if self.main.poller is not None:
            self.main.toggle_connect()                       # drop an old connection first
        self.log.clear()
        self._say("$ " + " ".join(shlex.quote(a) for a in [exe] + args))
        self.session = OpenOcdSession(exe, args)
        try:
            self.session.start()
        except OSError as e:
            self.session = None
            W.QMessageBox.warning(self, "Cannot start OpenOCD", str(e))
            return
        self._auto_done = False
        self._started_at = time.time()
        self.lbl.setText("Starting OpenOCD…")
        self._update()

    def stop(self):
        if self.main.poller is not None:
            self.main.toggle_connect()                       # close our Tcl connection first
        if self.session is not None:
            self.session.stop()
            for ln in self.session.drain():
                self._say(ln)
            self._say("[session stopped]")
        self.session = None
        self.lbl.setText("Session stopped.")
        self._update()

    def shutdown(self):
        """Called when the main window closes."""
        if self.session is not None:
            self.session.stop()
            self.session = None

    # ---- supervision (every 100 ms)
    def _poll(self):
        s = self.session
        if s is None:
            self._update()
            return
        for ln in s.drain():
            self._say(ln)
        if s.tcl_port and not self._auto_done:
            self._auto_done = True
            self.lbl.setText(f"OpenOCD is running - connected on Tcl port {s.tcl_port}. Add variables and watch them.")
            self.main.connect_to("localhost", s.tcl_port)
        if not s.running() and s.eof:
            code = s.returncode
            self.lbl.setText(f"OpenOCD stopped (exit code {code}). Check the log: wrong target or probe, "
                             "probe in use by another program, or missing USB permissions?")
            self.session = None
        elif not s.tcl_port and time.time() - self._started_at > 15:
            self.lbl.setText("Still waiting for OpenOCD to start… see the log below.")
        self._update()

    # ---- target control
    def _cmd(self, label, tcl, timeout=10.0):
        poller = self.main.poller
        if poller is None:
            QtWidgets.QMessageBox.information(self, "Not connected", "Start the session (or press Connect) first.")
            return
        self._say(f"> {label}")
        poller.request_command(label, tcl, timeout)

    def flash(self):
        W = QtWidgets
        elf = self.main.elf_path
        if not elf:
            W.QMessageBox.information(self, "Flash ELF", "Open the firmware ELF in the main window first.")
            return
        ans = W.QMessageBox.question(
            self, "Flash firmware",
            f"Program the target with\n{elf}\n\nThis erases and rewrites the flash, verifies it and resets the "
            "target. Continue?")
        if ans != W.QMessageBox.StandardButton.Yes:
            return
        self._cmd("Flash", "program {%s} verify reset" % elf, timeout=180.0)
