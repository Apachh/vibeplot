#include "RegistersDialog.h"

#include "MainWindow.h"
#include "ValueParse.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {
constexpr int kPtrRole = Qt::UserRole;
constexpr int kKindRole = Qt::UserRole + 1;

QString hexN(quint64 v, int digits) { return QStringLiteral("0x") + QString::number(v, 16).toUpper().rightJustified(digits, '0'); }

template <class T> const T *ptrOf(const QTreeWidgetItem *it)
{
    return reinterpret_cast<const T *>(it->data(0, kPtrRole).value<quintptr>());
}
}  // namespace

RegistersDialog::RegistersDialog(MainWindow *main)
    : QDialog(main), main_(main), settings_("swdplot", "swd_liveplot")
{
    setWindowTitle("MCU registers");
    resize(1000, 700);

    btnLoad_ = new QPushButton(QString::fromUtf8("Load SVD…"));
    btnLoad_->setToolTip("Choose the CMSIS-SVD file of your MCU (the one Cortex-Debug uses as svdFile)");
    connect(btnLoad_, &QPushButton::clicked, this, &RegistersDialog::browseSvd);
    btnReload_ = new QPushButton("Reload SVD");
    connect(btnReload_, &QPushButton::clicked, this, [this] { if (!svdPath_.isEmpty()) openSvd(svdPath_, false); });
    lblSvd_ = new QLabel;
    lblSvd_->setWordWrap(true);
    editFilter_ = new QLineEdit;
    editFilter_->setPlaceholderText(QString::fromUtf8("Filter peripherals / registers…"));
    connect(editFilter_, &QLineEdit::textChanged, this, &RegistersDialog::applyFilter);
    btnRefresh_ = new QPushButton("Refresh now");
    connect(btnRefresh_, &QPushButton::clicked, this, [this] { refresh({}, true); });
    chkAuto_ = new QCheckBox("Auto-refresh");
    chkAuto_->setChecked(true);
    spinMs_ = new QSpinBox;
    spinMs_->setRange(100, 10000);
    spinMs_->setValue(500);
    spinMs_->setSuffix(" ms");
    spinMs_->setSingleStep(100);

    tree_ = new QTreeWidget;
    tree_->setHeaderLabels({"Name", "Address / bits", "Value", "Decoded", "Description"});
    const int widths[] = {230, 110, 130, 150};
    for (int c = 0; c < 4; ++c) tree_->setColumnWidth(c, widths[c]);
    tree_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tree_, &QTreeWidget::customContextMenuRequested, this, &RegistersDialog::onMenu);
    connect(tree_, &QTreeWidget::itemExpanded, this, &RegistersDialog::onExpand);
    connect(tree_, &QTreeWidget::itemDoubleClicked, this, &RegistersDialog::onDouble);
    lblStatus_ = new QLabel;
    lblStatus_->setWordWrap(true);

    auto *r1 = new QHBoxLayout;
    r1->addWidget(btnLoad_);
    r1->addWidget(btnReload_);
    r1->addWidget(lblSvd_, 1);
    auto *r2 = new QHBoxLayout;
    r2->addWidget(editFilter_, 1);
    r2->addWidget(btnRefresh_);
    r2->addWidget(chkAuto_);
    r2->addWidget(spinMs_);
    auto *lay = new QVBoxLayout(this);
    lay->addLayout(r1);
    lay->addLayout(r2);
    lay->addWidget(tree_, 1);
    lay->addWidget(lblStatus_);

    connect(&timer_, &QTimer::timeout, this, &RegistersDialog::tick);
    timer_.start(spinMs_->value());
    connect(spinMs_, &QSpinBox::valueChanged, &timer_, qOverload<int>(&QTimer::setInterval));
    setStatus("Expand a peripheral to read its registers. Changed values are highlighted. "
              "Double-click a value to write; right-click for more.");
}

// --------------------------------------------------------------------------
// loading / binding
// --------------------------------------------------------------------------
QString RegistersDialog::settingsKey(const QString &path)
{
    const QByteArray h = QCryptographicHash::hash(QDir::cleanPath(QFileInfo(path).absoluteFilePath()).toUtf8(), QCryptographicHash::Sha1);
    return QString::fromLatin1(h.toHex());
}

void RegistersDialog::showEvent(QShowEvent *ev)
{
    QDialog::showEvent(ev);
    if (!initialised_) {
        initialised_ = true;
        autoload();
    }
}

void RegistersDialog::onElfLoaded(const QString &path)
{
    if (path != elfFor_) {
        elfFor_ = path;
        if (initialised_) autoload();
    }
}

void RegistersDialog::autoload()
{
    elfFor_ = main_->elfPath();
    QString path;
    if (!elfFor_.isEmpty()) path = settings_.value("svd_for/" + settingsKey(elfFor_)).toString();
    const bool bound = !path.isEmpty();
    if (path.isEmpty()) path = settings_.value("svd/last").toString();
    if (!path.isEmpty() && QFileInfo::exists(path)) {
        if (openSvd(path, false, true)) {
            if (bound) lblSvd_->setText(lblSvd_->text() + "  (bound to this ELF)");
            return;
        }
    }
    setPeripherals({}, {});
}

void RegistersDialog::browseSvd()
{
    const QString start = svdPath_.isEmpty() ? QString() : QFileInfo(svdPath_).absolutePath();
    const QString path = QFileDialog::getOpenFileName(this, "Open SVD file", start, "SVD files (*.svd *.xml);;All files (*)");
    if (!path.isEmpty()) openSvd(path);
}

bool RegistersDialog::openSvd(const QString &path, bool remember, bool quiet)
{
    QApplication::setOverrideCursor(Qt::WaitCursor);
    svd::LoadResult res;
    try {
        res = svd::loadSvd(path);
    } catch (const std::exception &e) {
        QApplication::restoreOverrideCursor();
        if (!quiet) QMessageBox::critical(this, "SVD error", QStringLiteral("Cannot read %1:\n%2").arg(path, e.what()));
        setStatus(QStringLiteral("Cannot read SVD: %1").arg(e.what()));
        return false;
    }
    QApplication::restoreOverrideCursor();
    svdPath_ = path;
    setPeripherals(std::move(res.peripherals), res.info);
    if (remember) {
        settings_.setValue("svd/last", path);
        if (!main_->elfPath().isEmpty()) {
            settings_.setValue("svd_for/" + settingsKey(main_->elfPath()), path);
            lblSvd_->setText(lblSvd_->text() + "  (bound to this ELF)");
        }
    }
    return true;
}

void RegistersDialog::setPeripherals(std::vector<std::shared_ptr<svd::Peripheral>> periphs, const svd::DeviceInfo &info)
{
    std::vector<quint64> bases;
    for (const auto &p : periphs) bases.push_back(p->base);
    const size_t fromSvd = periphs.size();
    int core = 0;
    for (auto &c : svd::corePeripherals()) {
        if (std::find(bases.begin(), bases.end(), c->base) != bases.end()) continue;
        periphs.push_back(std::move(c));
        ++core;
    }
    tree_->clear();
    items_.clear();
    values_.clear();
    requests_.clear();
    pending_ = false;
    periphs_ = std::move(periphs);
    info_ = info;
    for (const auto &p : periphs_) tree_->addTopLevelItem(periphItem(p.get()));
    applyFilter(editFilter_->text());
    if (fromSvd) {
        const QString dev = info.device.isEmpty() ? QStringLiteral("SVD") : info.device;
        const QString cpu = info.cpu.isEmpty() ? QString() : ", " + info.cpu;
        lblSvd_->setText(QStringLiteral("%1%2: %3 peripherals + %4 Cortex-M core groups  -  %5").arg(dev, cpu).arg(fromSvd).arg(core).arg(svdPath_));
    } else {
        lblSvd_->setText("No SVD loaded: only the built-in Cortex-M core registers are shown. Press Load SVD…");
    }
}

// --------------------------------------------------------------------------
// tree items
// --------------------------------------------------------------------------
namespace {
void tag(QTreeWidgetItem *it, int kind, const void *ptr)
{
    it->setData(0, kPtrRole, QVariant::fromValue<quintptr>(reinterpret_cast<quintptr>(ptr)));
    it->setData(0, kKindRole, kind);
}
}  // namespace

QTreeWidgetItem *RegistersDialog::periphItem(const svd::Peripheral *p)
{
    auto *it = new QTreeWidgetItem({p->name, hexN(p->base, 8), "", "", p->desc});
    tag(it, KindPeripheral, p);
    if (!p->regs.empty()) it->addChild(new QTreeWidgetItem({QString::fromUtf8("…")}));
    return it;
}

QTreeWidgetItem *RegistersDialog::regItem(const svd::Register *r)
{
    auto *it = new QTreeWidgetItem({r->name, hexN(r->addr, 8), "", "", r->desc});
    tag(it, KindRegister, r);
    items_[r] = it;
    if (r->sideEffect) {
        it->setText(2, "(read on request)");
        it->setToolTip(2, "Reading this register has side effects (clears flags or pops data), so it is never read automatically. "
                          "Right-click → Read now.");
    } else if (!r->readable()) {
        it->setText(2, "(write-only)");
    }
    if (!r->fields->empty()) it->addChild(new QTreeWidgetItem({QString::fromUtf8("…")}));
    return it;
}

QTreeWidgetItem *RegistersDialog::fieldItem(const svd::Field *f)
{
    const int hi = f->off + f->width - 1;
    const QString bits = f->width > 1 ? QStringLiteral("[%1:%2]").arg(hi).arg(f->off) : QStringLiteral("[%1]").arg(f->off);
    auto *it = new QTreeWidgetItem({f->name, bits, "", "", f->desc});
    tag(it, KindField, f);
    return it;
}

bool RegistersDialog::hasPlaceholder(const QTreeWidgetItem *it)
{
    return it->childCount() == 1 && it->child(0)->data(0, kKindRole).toInt() == KindNone;
}

void RegistersDialog::onExpand(QTreeWidgetItem *it)
{
    const int kind = it->data(0, kKindRole).toInt();
    if (kind == KindNone) return;
    if (hasPlaceholder(it)) {
        delete it->takeChild(0);
        if (kind == KindPeripheral) {
            for (const svd::Register &r : ptrOf<svd::Peripheral>(it)->regs) it->addChild(regItem(&r));
            applyFilter(editFilter_->text());
        } else if (kind == KindRegister) {
            const auto *r = ptrOf<svd::Register>(it);
            for (const svd::Field &f : *r->fields) it->addChild(fieldItem(&f));
            auto v = values_.find(r);
            if (v != values_.end()) showValue(r, it, v->second, std::nullopt);
        }
    }
    if (kind == KindPeripheral) refresh({}, true);
}

void RegistersDialog::applyFilter(const QString &text)
{
    const QString t = text.trimmed().toLower();
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        QTreeWidgetItem *top = tree_->topLevelItem(i);
        const auto *p = ptrOf<svd::Peripheral>(top);
        const bool pHit = t.isEmpty() || p->name.toLower().contains(t) || p->desc.toLower().contains(t);
        bool rHit = pHit;
        for (const svd::Register &r : p->regs) rHit = rHit || r.name.toLower().contains(t);
        top->setHidden(!rHit);
        if (hasPlaceholder(top)) continue;
        for (int j = 0; j < top->childCount(); ++j) {
            QTreeWidgetItem *ch = top->child(j);
            ch->setHidden(!t.isEmpty() && !pHit && !ptrOf<svd::Register>(ch)->name.toLower().contains(t));
        }
    }
}

// --------------------------------------------------------------------------
// reading
// --------------------------------------------------------------------------
void RegistersDialog::setStatus(const QString &msg) { lblStatus_->setText(msg); }

std::vector<const svd::Register *> RegistersDialog::visibleRegs() const
{
    std::vector<const svd::Register *> regs;
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        QTreeWidgetItem *top = tree_->topLevelItem(i);
        if (top->isHidden() || !top->isExpanded() || hasPlaceholder(top)) continue;
        for (int j = 0; j < top->childCount(); ++j) {
            QTreeWidgetItem *ch = top->child(j);
            if (ch->data(0, kKindRole).toInt() != KindRegister) continue;
            const auto *r = ptrOf<svd::Register>(ch);
            if (r->readable() && !r->sideEffect && !ch->isHidden()) regs.push_back(r);
        }
    }
    return regs;
}

void RegistersDialog::refresh(std::vector<const svd::Register *> regs, bool force)
{
    Poller *poller = main_->poller();
    if (!poller) {
        setStatus("Not connected: press Connect in the main window first.");
        return;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (pending_ && !force && now - pendingSince_ < 3000) return;
    if (regs.empty()) regs = visibleRegs();
    if (regs.empty()) return;
    pending_ = true;
    pendingSince_ = now;
    RegSpecs specs;
    for (const svd::Register *r : regs) specs.push_back({r->addr, r->bytes()});
    const quint64 token = ++nextToken_;
    requests_[token] = std::move(regs);
    poller->requestRead(token, specs);
}

void RegistersDialog::tick()
{
    if (isVisible() && chkAuto_->isChecked() && main_->poller()) refresh();
}

void RegistersDialog::refreshSoon(const svd::Register *r)
{
    QTimer::singleShot(100, this, [this, r] { refresh({r}, true); });
}

void RegistersDialog::onRegs(quint64 token, const RegResults &res)
{
    auto req = requests_.find(token);
    if (req == requests_.end()) return;
    const std::vector<const svd::Register *> regs = std::move(req->second);
    requests_.erase(req);
    pending_ = false;
    int failed = 0;
    for (const svd::Register *r : regs) {
        auto itIt = items_.find(r);
        if (itIt == items_.end()) continue;
        QTreeWidgetItem *it = itIt->second;
        const RegResult rr = res.value(r->addr);
        if (!rr.ok) {
            ++failed;
            it->setText(2, "<read error>");
            color(it, 2, false);
            continue;
        }
        std::optional<quint64> prev;
        if (auto p = values_.find(r); p != values_.end()) prev = p->second;
        values_[r] = rr.value;
        showValue(r, it, rr.value, prev);
    }
    QString msg = QStringLiteral("Read %1 register(s)").arg(regs.size());
    if (failed) msg += QStringLiteral(", %1 could not be read (peripheral clock off, or reserved address)").arg(failed);
    setStatus(msg);
}

void RegistersDialog::color(QTreeWidgetItem *it, int col, bool changed)
{
    if (changed) it->setForeground(col, QBrush(QColor("#ffb74d")));
    else it->setData(col, Qt::ForegroundRole, QVariant());
}

void RegistersDialog::showValue(const svd::Register *r, QTreeWidgetItem *it, quint64 v, std::optional<quint64> prev)
{
    it->setText(2, hexN(v, r->size / 4));
    color(it, 2, prev && *prev != v);
    for (int i = 0; i < it->childCount(); ++i) {
        QTreeWidgetItem *ch = it->child(i);
        if (ch->data(0, kKindRole).toInt() != KindField) continue;
        const auto *f = ptrOf<svd::Field>(ch);
        const quint64 fv = f->extract(v);
        ch->setText(2, f->width <= 4 ? QString::number(fv) : QStringLiteral("0x") + QString::number(fv, 16).toUpper());
        ch->setText(3, f->enumValues.isEmpty() ? QString() : f->enumValues.value(static_cast<qint64>(fv), "?"));
        color(ch, 2, prev && f->extract(*prev) != fv);
    }
}

// --------------------------------------------------------------------------
// actions
// --------------------------------------------------------------------------
RegistersDialog::Owner RegistersDialog::ownerOf(QTreeWidgetItem *it) const
{
    switch (it->data(0, kKindRole).toInt()) {
    case KindPeripheral: return {ptrOf<svd::Peripheral>(it), nullptr};
    case KindRegister: return {ptrOf<svd::Peripheral>(it->parent()), ptrOf<svd::Register>(it)};
    default: return {ptrOf<svd::Peripheral>(it->parent()->parent()), ptrOf<svd::Register>(it->parent())};
    }
}

void RegistersDialog::onDouble(QTreeWidgetItem *it, int col)
{
    if (col != 2) return;
    const int kind = it->data(0, kKindRole).toInt();
    const Owner o = ownerOf(it);
    if (kind == KindRegister) writeRegister(o.periph, o.reg);
    else if (kind == KindField) writeField(o.periph, o.reg, ptrOf<svd::Field>(it));
}

void RegistersDialog::onMenu(const QPoint &pos)
{
    QTreeWidgetItem *it = tree_->itemAt(pos);
    if (!it) return;
    const int kind = it->data(0, kKindRole).toInt();
    if (kind == KindNone) return;
    const Owner o = ownerOf(it);
    QMenu menu(this);
    auto add = [&](const QString &text, std::function<void()> fn) { connect(menu.addAction(text), &QAction::triggered, this, std::move(fn)); };
    if (kind == KindPeripheral) {
        add("Expand and read registers", [it] { it->setExpanded(true); });
    } else if (kind == KindRegister) {
        add("Read now", [this, o] { refresh({o.reg}, true); });
        add("Plot register", [this, o] { plot(o.periph, o.reg, nullptr); });
        add(QString::fromUtf8("Write value…"), [this, o] { writeRegister(o.periph, o.reg); });
        add("Copy address", [o] { QApplication::clipboard()->setText(hexN(o.reg->addr, 8)); });
        add("Copy value", [it] { QApplication::clipboard()->setText(it->text(2)); });
    } else {
        const auto *f = ptrOf<svd::Field>(it);
        add("Plot field", [this, o, f] { plot(o.periph, o.reg, f); });
        add(QString::fromUtf8("Write field…"), [this, o, f] { writeField(o.periph, o.reg, f); });
        add("Copy value", [it] { QApplication::clipboard()->setText(it->text(2)); });
    }
    menu.exec(tree_->viewport()->mapToGlobal(pos));
}

void RegistersDialog::plot(const svd::Peripheral *p, const svd::Register *r, const svd::Field *f)
{
    if (r->sideEffect) {
        QMessageBox::information(this, "Cannot plot",
                                 QStringLiteral("%1.%2 has read side effects (reading changes the hardware), so it is not polled continuously.").arg(p->name, r->name));
        return;
    }
    if (!r->readable()) {
        QMessageBox::information(this, "Cannot plot", "This register is write-only.");
        return;
    }
    if (!f) main_->addRegisterSeries(p->name + "." + r->name, r->addr, r->bytes());
    else main_->addRegisterSeries(QStringLiteral("%1.%2.%3").arg(p->name, r->name, f->name), r->addr, r->bytes(),
                                  BitSpec{f->off, f->width, false}, f->enumValues);
}

bool RegistersDialog::confirmWrite(const QString &what)
{
    return QMessageBox::question(this, "Write to hardware", what + "\n\nThis changes the running hardware immediately. Continue?") == QMessageBox::Yes;
}

void RegistersDialog::writeRegister(const svd::Peripheral *p, const svd::Register *r)
{
    Poller *poller = main_->poller();
    if (!r->writable()) {
        QMessageBox::information(this, "Read-only", QStringLiteral("%1.%2 is read-only.").arg(p->name, r->name));
        return;
    }
    if (!poller) {
        QMessageBox::information(this, "Not connected", "Press Connect in the main window first.");
        return;
    }
    auto cur = values_.find(r);
    const QString dflt = cur != values_.end() ? hexN(cur->second, r->size / 4) : QStringLiteral("0x0");
    bool ok = false;
    const QString text = QInputDialog::getText(
        this, QStringLiteral("Write %1.%2").arg(p->name, r->name),
        QStringLiteral("New value for the %1-bit register at %2 (decimal or 0x hex):").arg(r->size).arg(hexN(r->addr, 8)),
        QLineEdit::Normal, dflt, &ok);
    if (!ok || text.trimmed().isEmpty()) return;
    const auto v = valparse::parseInteger(text);
    if (!v || !v->fitsUnsigned(r->size)) {
        QMessageBox::warning(this, "Invalid value",
                             QStringLiteral("Cannot write '%1': %2").arg(text, v ? QStringLiteral("must fit in %1 bits").arg(r->size) : QStringLiteral("not a number")));
        return;
    }
    if (!confirmWrite(QStringLiteral("Write 0x%1 to %2.%3 (%4)?").arg(QString::number(v->mag, 16).toUpper(), p->name, r->name, hexN(r->addr, 8)))) return;
    QByteArray raw;
    for (int i = 0; i < r->bytes(); ++i) raw.append(static_cast<char>((v->mag >> (8 * i)) & 0xFF));
    poller->requestWrite(p->name + "." + r->name, r->addr, raw);
    refreshSoon(r);
}

void RegistersDialog::writeField(const svd::Peripheral *p, const svd::Register *r, const svd::Field *f)
{
    Poller *poller = main_->poller();
    if (!r->writable()) {
        QMessageBox::information(this, "Read-only", QStringLiteral("%1.%2 is read-only.").arg(p->name, r->name));
        return;
    }
    if (!poller) {
        QMessageBox::information(this, "Not connected", "Press Connect in the main window first.");
        return;
    }
    const QString title = QStringLiteral("Write %1.%2.%3").arg(p->name, r->name, f->name);
    QString text;
    bool ok = false;
    if (!f->enumValues.isEmpty()) {
        QStringList items;
        for (auto it = f->enumValues.begin(); it != f->enumValues.end(); ++it) items << QStringLiteral("%1 (%2)").arg(it.value()).arg(it.key());
        text = QInputDialog::getItem(this, title, "Pick a value (or type a number):", items, 0, true, &ok);
    } else {
        auto cur = values_.find(r);
        text = QInputDialog::getText(this, title, QStringLiteral("New value for the %1-bit field (decimal or 0x hex):").arg(f->width),
                                     QLineEdit::Normal, cur != values_.end() ? QString::number(f->extract(cur->second)) : QStringLiteral("0"), &ok);
    }
    if (!ok || text.trimmed().isEmpty()) return;
    const auto v = valparse::parseWithEnum(text, &f->enumValues);
    if (!v || v->neg || v->mag > f->mask()) {
        QMessageBox::warning(this, "Invalid value",
                             QStringLiteral("Cannot write '%1': %2").arg(text, v ? QStringLiteral("must fit in %1 bit(s)").arg(f->width) : QStringLiteral("not a number")));
        return;
    }
    if (!confirmWrite(QStringLiteral("Set %1.%2.%3 to %4? The other bits of the register are kept.").arg(p->name, r->name, f->name).arg(v->mag))) return;
    poller->requestRmw(QStringLiteral("%1.%2.%3").arg(p->name, r->name, f->name), r->addr, r->bytes(), f->mask() << f->off, v->mag << f->off);
    refreshSoon(r);
}
