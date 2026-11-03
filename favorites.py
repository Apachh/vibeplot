"""Favorite variables, organised in named groups, for quick access.

FavStore       plain data + JSON (no Qt), easy to test.
FavoritesPanel the side-panel widget. It does not know the main window: it is given two callbacks,
               resolve(path) -> (address, type name) or None   (is the variable in the loaded ELF?)
               add_paths(paths)                                (put variables on the plot)
Favorites are saved after every change, per ELF file (or shared by all ELF files).
"""
import hashlib
import json
import os

from pyqtgraph.Qt import QtCore, QtGui, QtWidgets

DEFAULT_GROUP = "Favorites"
MAX_NAME = 40


class FavStore:
    """Ordered groups of variable paths ('speed', 'motor.cur', 'table[2].g', 'ns::Class::var')."""

    def __init__(self, groups=None):
        self.groups = {name: list(paths) for name, paths in (groups or {}).items()}

    # ---- queries
    def paths(self):
        return {p for lst in self.groups.values() for p in lst}

    def group_names(self):
        names = list(self.groups)
        return names if DEFAULT_GROUP in self.groups else [DEFAULT_GROUP] + names

    # ---- changes (return True when something changed)
    def add(self, paths, group=DEFAULT_GROUP):
        lst = self.groups.setdefault(group, [])
        added = 0
        for p in paths:
            if p not in lst:
                lst.append(p)
                added += 1
        return added

    def remove(self, path, group=None):
        changed = False
        for g, lst in self.groups.items():
            if (group is None or g == group) and path in lst:
                lst.remove(path)
                changed = True
        return changed

    def move(self, path, src, dst):
        if src == dst:
            return False
        self.remove(path, src)
        self.add([path], dst)
        return True

    @staticmethod
    def valid_name(name):
        return bool(name) and len(name) <= MAX_NAME

    def new_group(self, name):
        if not self.valid_name(name) or name in self.groups:
            return False
        self.groups[name] = []
        return True

    def rename_group(self, old, new):
        if old not in self.groups or not self.valid_name(new) or new in self.groups:
            return False
        self.groups = {(new if k == old else k): v for k, v in self.groups.items()}
        return True

    def delete_group(self, name):
        return self.groups.pop(name, None) is not None

    # ---- persistence
    def to_json(self):
        return json.dumps({"groups": [[n, list(p)] for n, p in self.groups.items()]})

    @classmethod
    def from_json(cls, text):
        try:
            data = json.loads(text)
            groups = {}
            for name, paths in data["groups"]:
                if isinstance(name, str) and isinstance(paths, list):
                    groups[name] = [p for p in paths if isinstance(p, str)]
            return cls(groups)
        except (ValueError, KeyError, TypeError):
            return cls()


class FavoritesPanel(QtWidgets.QWidget):
    changed = QtCore.Signal()
    ROLE = QtCore.Qt.ItemDataRole.UserRole

    def __init__(self, resolve, add_paths, settings):
        super().__init__()
        W = QtWidgets
        self.resolve, self.add_paths, self.settings = resolve, add_paths, settings
        self.store = FavStore()
        self.elf_path = None
        self._collapsed = set()

        shared = str(settings.value("favorites/shared", "0")) in ("1", "true", "True")
        self.chk_shared = W.QCheckBox("Share between all ELF files")
        self.chk_shared.setToolTip("Off: each ELF file has its own favorites. On: one list for every project.")
        self.chk_shared.setChecked(shared)
        self.chk_shared.toggled.connect(self._on_shared)

        self.tree = W.QTreeWidget()
        self.tree.setHeaderLabels(["Group / variable", "Address", "Type"])
        self.tree.setColumnWidth(0, 200)
        self.tree.setColumnWidth(1, 90)
        self.tree.setEditTriggers(W.QAbstractItemView.EditTrigger.NoEditTriggers)
        self.tree.setSelectionMode(W.QAbstractItemView.SelectionMode.ExtendedSelection)
        self.tree.setContextMenuPolicy(QtCore.Qt.ContextMenuPolicy.CustomContextMenu)
        self.tree.customContextMenuRequested.connect(self._on_menu)
        self.tree.itemDoubleClicked.connect(lambda it, _c: self.plot_items([it]))
        self.tree.itemCollapsed.connect(lambda it: self._remember(it, True))
        self.tree.itemExpanded.connect(lambda it: self._remember(it, False))

        btn_plot = W.QPushButton("Add to plot ▶")
        btn_plot.setToolTip("Plot the selected variables, or every variable of the selected groups "
                            "(double-click does the same)")
        btn_plot.clicked.connect(lambda: self.plot_items(self.tree.selectedItems()))
        btn_new = W.QPushButton("New group…")
        btn_new.clicked.connect(self.new_group)
        btn_rm = W.QPushButton("Remove")
        btn_rm.setToolTip("Remove the selected variables from their group, or delete the selected groups")
        btn_rm.clicked.connect(self.remove_selected)
        self.lbl = W.QLabel("Use ★ Favorite in the Variables tab, or right-click a variable.")
        self.lbl.setWordWrap(True)

        row = W.QHBoxLayout()
        row.addWidget(btn_plot)
        row.addWidget(btn_new)
        row.addWidget(btn_rm)
        lay = W.QVBoxLayout(self)
        lay.setContentsMargins(0, 0, 0, 0)
        lay.addWidget(self.chk_shared)
        lay.addWidget(self.tree, 1)
        lay.addLayout(row)
        lay.addWidget(self.lbl)

    # ---- scope / persistence
    def _key(self):
        if self.chk_shared.isChecked():
            return "favorites/shared_store"
        if not self.elf_path:
            return None
        return "favorites/elf_" + hashlib.sha1(os.path.abspath(self.elf_path).encode()).hexdigest()

    def _load(self):
        key = self._key()
        text = self.settings.value(key) if key else None
        self.store = FavStore.from_json(str(text)) if text else FavStore()
        self.rebuild()
        self.changed.emit()

    def _save(self):
        key = self._key()
        if key:
            self.settings.setValue(key, self.store.to_json())
            self.settings.sync()

    def set_scope(self, elf_path):
        """A (new) ELF was loaded: switch to its favorites."""
        self.elf_path = elf_path
        self._load()

    def _on_shared(self, on):
        prev = self.store
        self.settings.setValue("favorites/shared", "1" if on else "0")
        self._load()
        if not self.store.groups and prev.groups:      # first time shared: start from the current list
            self.store = prev
            self._save()
            self.rebuild()
            self.changed.emit()

    def _commit(self):
        self._save()
        self.rebuild()
        self.changed.emit()

    # ---- public API used by the main window
    def groups(self):
        return self.store.group_names()

    def all_paths(self):
        return self.store.paths()

    def add(self, paths, group=None):
        if not self._key():
            self.lbl.setText("Open an ELF file first (or tick 'Share between all ELF files').")
            return
        group = group or DEFAULT_GROUP
        n = self.store.add(paths, group)
        self._commit()
        self.lbl.setText(f"{n} variable(s) added to “{group}”." if n else f"Already in “{group}”.")

    def add_to_new_group(self, paths):
        name = self._ask_name("New group", "Name of the new group:")
        if name and self.store.valid_name(name):
            if name in self.store.groups:
                self.add(paths, name)
            else:
                self.store.new_group(name)
                self.add(paths, name)

    def remove_paths(self, paths):
        for p in paths:
            self.store.remove(p)
        self._commit()

    def refresh(self):
        self.rebuild()

    # ---- tree
    def _remember(self, item, collapsed):
        d = item.data(0, self.ROLE)
        if d and d[0] == "group":
            (self._collapsed.add if collapsed else self._collapsed.discard)(d[1])

    def rebuild(self):
        W = QtWidgets
        grey = QtGui.QBrush(QtGui.QColor("#7a7a7a"))
        self.tree.clear()
        for name, paths in self.store.groups.items():
            g = W.QTreeWidgetItem([f"{name}  ({len(paths)})", "", ""])
            g.setData(0, self.ROLE, ("group", name))
            font = g.font(0)
            font.setBold(True)
            g.setFont(0, font)
            for p in paths:
                info = self.resolve(p)
                it = W.QTreeWidgetItem([p, f"0x{info[0]:08X}" if info else "", info[1] if info else ""])
                it.setData(0, self.ROLE, ("var", name, p))
                if info is None:
                    for c in range(3):
                        it.setForeground(c, grey)
                    it.setToolTip(0, "Not found in the loaded ELF (renamed, removed, or no ELF loaded)")
                g.addChild(it)
            self.tree.addTopLevelItem(g)
            g.setExpanded(name not in self._collapsed)

    def _ask_name(self, title, label, text=""):
        name, ok = QtWidgets.QInputDialog.getText(self, title, label, text=text)
        name = name.strip()
        if not ok or not name:
            return None
        if not self.store.valid_name(name):
            QtWidgets.QMessageBox.warning(self, title, f"Use 1 to {MAX_NAME} characters.")
            return None
        return name

    def _selection(self):
        out = []
        for it in self.tree.selectedItems():
            d = it.data(0, self.ROLE)
            if d:
                out.append(d)
        return out

    def _paths_of(self, sel):
        paths = []
        for d in sel:
            ps = self.store.groups.get(d[1], []) if d[0] == "group" else [d[2]]
            for p in ps:
                if p not in paths:
                    paths.append(p)
        return paths

    # ---- actions
    def plot_items(self, items):
        sel = [it.data(0, self.ROLE) for it in items if it.data(0, self.ROLE)]
        paths = self._paths_of(sel)
        if paths:
            self.add_paths(paths)

    def new_group(self):
        if not self._key():
            self.lbl.setText("Open an ELF file first (or tick 'Share between all ELF files').")
            return
        name = self._ask_name("New group", "Name of the new group:")
        if name and not self.store.new_group(name):
            QtWidgets.QMessageBox.information(self, "New group", f"A group named “{name}” already exists.")
        elif name:
            self._commit()

    def rename_group(self, old):
        new = self._ask_name("Rename group", "New name:", old)
        if new and new != old:
            if self.store.rename_group(old, new):
                self._collapsed.discard(old)
                self._commit()
            else:
                QtWidgets.QMessageBox.information(self, "Rename group", f"A group named “{new}” already exists.")

    def delete_group(self, name):
        n = len(self.store.groups.get(name, []))
        if n:
            ans = QtWidgets.QMessageBox.question(
                self, "Delete group", f"Delete “{name}” and its {n} variable(s) from the favorites?")
            if ans != QtWidgets.QMessageBox.StandardButton.Yes:
                return
        self.store.delete_group(name)
        self._commit()

    def move(self, pairs, dst):
        for src, path in pairs:
            self.store.move(path, src, dst)
        self._commit()

    def remove_selected(self):
        sel = self._selection()
        groups = [d[1] for d in sel if d[0] == "group"]
        for d in sel:
            if d[0] == "var" and d[1] not in groups:
                self.store.remove(d[2], d[1])
        for g in groups:
            self.store.delete_group(g)
        self._commit()

    def _on_menu(self, pos):
        it = self.tree.itemAt(pos)
        if it is None:
            return
        if not it.isSelected():
            self.tree.clearSelection()
            it.setSelected(True)
        sel = self._selection()
        menu = QtWidgets.QMenu(self)
        acts = {}

        def add(text, fn, parent=None):
            acts[(parent or menu).addAction(text)] = fn
        add("Add to plot", lambda: self.plot_items(self.tree.selectedItems()))
        vars_ = [(d[1], d[2]) for d in sel if d[0] == "var"]
        if vars_:
            sub = menu.addMenu("Move to group")
            for g in self.store.groups:
                add(g, lambda g=g: self.move(vars_, g), sub)
            sub.addSeparator()
            add("New group…", lambda: self._move_to_new(vars_), sub)
        if len(sel) == 1 and sel[0][0] == "group":
            add("Rename group…", lambda: self.rename_group(sel[0][1]))
        add("Remove" if vars_ else "Delete group", self.remove_selected)
        chosen = menu.exec(self.tree.viewport().mapToGlobal(pos))
        if chosen in acts:
            acts[chosen]()

    def _move_to_new(self, pairs):
        name = self._ask_name("New group", "Name of the new group:")
        if name:
            self.store.new_group(name)
            self.move(pairs, name)
