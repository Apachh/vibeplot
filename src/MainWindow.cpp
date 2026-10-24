#include "MainWindow.h"

#include "GraphTab.h"
#include "RegistersDialog.h"
#include "StreamDialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <charconv>

namespace {

constexpr int kNodeRole = Qt::UserRole;

QString hex8(quint64 v) { return QStringLiteral("0x%1").arg(QString::number(v, 16).rightJustified(8, '0').toUpper()); }

Node *nodeOf(const QTreeWidgetItem *it)
{
    const QVariant v = it->data(0, kNodeRole);
    return v.isValid() ? reinterpret_cast<Node *>(v.value<quintptr>()) : nullptr;
}

std::optional<std::pair<qint64, qint64>> statSig(const QString &path)
{
    const QFileInfo fi(path);
    if (!fi.exists()) return std::nullopt;
    return std::make_pair(fi.lastModified().toMSecsSinceEpoch(), static_cast<qint64>(fi.size()));
}

QString shortest(double v)
{
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof buf, v);
    return QString::fromLatin1(buf, static_cast<int>(r.ptr - buf));
}

QString csvQuote(const QString &s)
{
    if (s.contains(',') || s.contains('"') || s.contains('\n') || s.contains('\r')) return '"' + QString(s).replace("\"", "\"\"") + '"';
    return s;
}

}  // namespace

MainWindow::MainWindow()
{
    setWindowTitle("SWD Live Plot");
    buildUi();
    connect(&timer_, &QTimer::timeout, this, &MainWindow::refresh);
    timer_.start(50);
}

MainWindow::~MainWindow() = default;

// --------------------------------------------------------------------------
// UI construction
// --------------------------------------------------------------------------
void MainWindow::buildUi()
{
    auto *top = new QHBoxLayout;
    auto *btnOpen = new QPushButton(QString::fromUtf8("Open ELF…"));
    connect(btnOpen, &QPushButton::clicked, this, &MainWindow::openElf);
    btnReload_ = new QPushButton("Reload ELF");
    btnReload_->setObjectName("btnReload");
    btnReload_->setToolTip("Re-read the ELF and move the plotted variables to their new addresses");
    connect(btnReload_, &QPushButton::clicked, this, [this] { reloadElf(); });
    chkAutoElf_ = new QCheckBox("Auto");
    chkAutoElf_->setChecked(true);
    chkAutoElf_->setToolTip("Reload automatically when the ELF file changes on disk (after a rebuild)");
    lblElf_ = new QLabel("no file loaded");
    editHost_ = new QLineEdit("localhost");
    editHost_->setFixedWidth(100);
    editHost_->setObjectName("editHost");
    spinPort_ = new QSpinBox;
    spinPort_->setRange(1, 65535);
    spinPort_->setValue(50001);
    spinPort_->setObjectName("spinPort");
    btnConn_ = new QPushButton("Connect");
    btnConn_->setObjectName("btnConnect");
    connect(btnConn_, &QPushButton::clicked, this, &MainWindow::toggleConnect);
    spinPeriod_ = new QSpinBox;
    spinPeriod_->setRange(1, 5000);
    spinPeriod_->setValue(10);
    spinPeriod_->setSuffix(" ms");
    connect(spinPeriod_, &QSpinBox::valueChanged, this, [this](int v) { period_ = v / 1000.0; });
    spinWindow_ = new QDoubleSpinBox;
    spinWindow_->setRange(0.5, 3600);
    spinWindow_->setValue(10);
    spinWindow_->setSuffix(" s");
    btnFollow_ = new QPushButton("Follow live");
    btnFollow_->setCheckable(true);
    btnFollow_->setObjectName("btnFollow");
    btnFollow_->setChecked(true);
    btnFollow_->setToolTip("Scroll the view with the newest data");
    connect(btnFollow_, &QPushButton::toggled, this, &MainWindow::setFollow);
    auto *btnFit = new QPushButton("Fit X");
    btnFit->setToolTip("Show the whole recorded time range (stops following). Use Fit Y for the vertical axis.");
    connect(btnFit, &QPushButton::clicked, this, &MainWindow::fitView);
    auto *btnFitY = new QPushButton("Fit Y");
    btnFitY->setToolTip("Fit the vertical range to the data visible in the current time range");
    connect(btnFitY, &QPushButton::clicked, this, [this] { currentTab()->fitY(); });
    btnCursors_ = new QPushButton("Cursors");
    btnCursors_->setCheckable(true);
    btnCursors_->setObjectName("btnCursors");
    btnCursors_->setToolTip("Two measurement cursors (stops following so the view stays still)");
    connect(btnCursors_, &QPushButton::toggled, this, &MainWindow::setCursors);
    auto *btnClear = new QPushButton("Clear data");
    connect(btnClear, &QPushButton::clicked, this, &MainWindow::clearData);
    auto *btnCsv = new QPushButton(QString::fromUtf8("Export CSV…"));
    connect(btnCsv, &QPushButton::clicked, this, &MainWindow::exportCsv);
    btnStream_ = new QPushButton(QString::fromUtf8("Stream…"));
    btnStream_->setToolTip("Send the plotted values live to a virtual COM port, serial port or file");
    connect(btnStream_, &QPushButton::clicked, this, &MainWindow::showStream);
    auto *btnRegs = new QPushButton(QString::fromUtf8("Registers…"));
    btnRegs->setToolTip("Browse the MCU's peripheral and Cortex-M core registers (SVD file)");
    connect(btnRegs, &QPushButton::clicked, this, &MainWindow::showRegisters);

    top->addWidget(btnOpen);
    top->addWidget(btnReload_);
    top->addWidget(chkAutoElf_);
    top->addWidget(lblElf_, 1);
    top->addWidget(new QLabel("OpenOCD Tcl host"));
    top->addWidget(editHost_);
    top->addWidget(new QLabel("port"));
    top->addWidget(spinPort_);
    top->addWidget(btnConn_);
    top->addWidget(new QLabel("Period"));
    top->addWidget(spinPeriod_);
    top->addWidget(new QLabel("Window"));
    top->addWidget(spinWindow_);
    for (QPushButton *b : {btnFollow_, btnFit, btnFitY, btnCursors_, btnClear, btnCsv, btnStream_, btnRegs}) top->addWidget(b);

    // left: variable tree
    auto *left = new QWidget;
    auto *ll = new QVBoxLayout(left);
    ll->setContentsMargins(0, 0, 0, 0);
    editFilter_ = new QLineEdit;
    editFilter_->setPlaceholderText(QString::fromUtf8("Filter variables…"));
    editFilter_->setObjectName("editFilter");
    connect(editFilter_, &QLineEdit::textChanged, this, &MainWindow::applyFilter);
    tree_ = new QTreeWidget;
    tree_->setHeaderLabels({"Variable", "Address", "Size", "Type", "Offset / bits"});
    tree_->setColumnWidth(0, 220);
    tree_->setColumnWidth(1, 90);
    tree_->setColumnWidth(2, 45);
    tree_->setObjectName("treeVars");
    tree_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    connect(tree_, &QTreeWidget::itemExpanded, this, &MainWindow::onExpand);
    connect(tree_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *it, int) { addItems({it}); });
    auto *btnAdd = new QPushButton(QString::fromUtf8("Add selected to current tab ▶"));
    btnAdd->setObjectName("btnAddSelected");
    connect(btnAdd, &QPushButton::clicked, this, [this] { addItems(tree_->selectedItems()); });
    auto *btnAddr = new QPushButton(QString::fromUtf8("Add by address…"));
    connect(btnAddr, &QPushButton::clicked, this, &MainWindow::addByAddress);
    auto *row = new QHBoxLayout;
    row->addWidget(btnAdd);
    row->addWidget(btnAddr);
    ll->addWidget(editFilter_);
    ll->addWidget(tree_, 1);
    ll->addLayout(row);

    // right: tabs, each with its own plots and variable table
    tabs_ = new QTabWidget;
    tabs_->setTabsClosable(true);
    tabs_->setMovable(true);
    tabs_->setObjectName("tabs");
    connect(tabs_, &QTabWidget::tabCloseRequested, this, &MainWindow::closeTab);
    connect(tabs_->tabBar(), &QTabBar::tabBarDoubleClicked, this, &MainWindow::renameTab);
    tabs_->tabBar()->setToolTip("Double-click a tab to rename it");
    auto *btnNewTab = new QToolButton;
    btnNewTab->setText(QString::fromUtf8("＋"));
    btnNewTab->setToolTip("New graph tab");
    connect(btnNewTab, &QToolButton::clicked, this, [this] { addTab(); });
    tabs_->setCornerWidget(btnNewTab, Qt::TopRightCorner);
    addTab();

    auto *split = new QSplitter(Qt::Horizontal);
    split->addWidget(left);
    split->addWidget(tabs_);
    split->setSizes({420, 980});

    auto *central = new QWidget;
    auto *cl = new QVBoxLayout(central);
    cl->addLayout(top);
    cl->addWidget(split, 1);
    setCentralWidget(central);
    statusBar()->showMessage("Open an ELF, add variables, start debugging in VS Code, then Connect.");
}

// --------------------------------------------------------------------------
// ELF / variable tree
// --------------------------------------------------------------------------
void MainWindow::openElf()
{
    const QString path = QFileDialog::getOpenFileName(this, "Open firmware ELF", {}, "ELF files (*.elf *.axf *.out);;All files (*)");
    if (!path.isEmpty()) loadElf(path);
}

bool MainWindow::loadElf(const QString &path, bool quiet)
{
    QApplication::setOverrideCursor(Qt::WaitCursor);
    std::unique_ptr<ElfModel> fresh;
    try {
        fresh = std::make_unique<ElfModel>(path);
    } catch (const std::exception &e) {
        QApplication::restoreOverrideCursor();
        if (!quiet) QMessageBox::critical(this, "ELF error", e.what());
        statusBar()->showMessage(QStringLiteral("Cannot read ELF: %1").arg(e.what()));
        return false;
    }
    tree_->clear();                       // the items point into the old model
    model_ = std::move(fresh);
    for (const auto &n : model_->roots) tree_->addTopLevelItem(makeItem(n.get()));
    QApplication::restoreOverrideCursor();
    applyFilter(editFilter_->text());
    elfPath_ = path;
    elfSig_ = statSig(path);
    elfPending_.reset();
    btnReload_->setText("Reload ELF");
    lblElf_->setStyleSheet("");
    lblElf_->setText(path);
    const QString note = model_->hasDebug ? QString() : QStringLiteral("  (no DWARF info: raw symbols only, build with -g for types/structs)");
    statusBar()->showMessage(QStringLiteral("%1 variables loaded%2").arg(model_->roots.size()).arg(note));
    if (regDlg_) regDlg_->onElfLoaded(path);
    return true;
}

void MainWindow::reloadElf(bool automatic)
{
    if (elfPath_.isEmpty()) {
        statusBar()->showMessage("No ELF loaded yet.");
        return;
    }
    if (!loadElf(elfPath_, automatic)) return;
    const int total = static_cast<int>(allSeries().size());
    if (!total) return;
    int moved = 0, missing = 0;
    for (GraphTab *tab : allTabs()) {
        const auto [m, x] = tab->remapSeries();
        moved += m;
        missing += x;
    }
    QString msg = QStringLiteral("ELF reloaded (%1 variables): %2 plotted variable(s) updated").arg(model_->roots.size()).arg(total - missing);
    if (moved) msg += QStringLiteral(", %1 moved to a new address").arg(moved);
    if (missing) msg += QStringLiteral(", %1 not found (kept old address)").arg(missing);
    statusBar()->showMessage(msg);
}

// Called about once a second: notice a rebuilt ELF once the linker has finished writing it.
void MainWindow::checkElf()
{
    if (elfPath_.isEmpty()) return;
    const auto sig = statSig(elfPath_);
    if (!sig || sig == elfSig_) {
        elfPending_.reset();
        return;
    }
    if (elfPending_ != sig) {             // still changing: wait for the next check
        elfPending_ = sig;
        return;
    }
    elfPending_.reset();
    elfSig_ = sig;
    if (chkAutoElf_->isChecked()) {
        reloadElf(true);
    } else {
        btnReload_->setText(QString::fromUtf8("Reload ELF ●"));
        lblElf_->setStyleSheet("color: #ffb74d;");
        statusBar()->showMessage("The ELF file changed on disk - press Reload ELF.");
    }
}

QTreeWidgetItem *MainWindow::makeItem(Node *node)
{
    const QString size = node->bits ? QStringLiteral("%1 bit").arg(node->bits->width) : QString::number(node->size);
    QString layout;
    if (node->rel) {
        layout = QStringLiteral("+0x") + QString::number(*node->rel, 16).toUpper();
        if (node->bits)
            layout += QStringLiteral("  bits %1..%2").arg(node->bits->shift).arg(node->bits->shift + node->bits->width - 1);
    }
    auto *it = new QTreeWidgetItem({node->label, hex8(node->addr), size, node->typeName, layout});
    it->setData(0, kNodeRole, QVariant::fromValue<quintptr>(reinterpret_cast<quintptr>(node)));
    if (node->expandable()) it->addChild(new QTreeWidgetItem({QString::fromUtf8("…")}));   // placeholder
    return it;
}

void MainWindow::onExpand(QTreeWidgetItem *it)
{
    Node *node = nodeOf(it);
    if (!node || it->childCount() != 1 || nodeOf(it->child(0))) return;
    delete it->takeChild(0);
    for (const auto &k : node->children()) it->addChild(makeItem(k.get()));
}

void MainWindow::applyFilter(const QString &text)
{
    const QString t = text.trimmed().toLower();
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        QTreeWidgetItem *it = tree_->topLevelItem(i);
        it->setHidden(!t.isEmpty() && !it->text(0).toLower().contains(t));
    }
}

void MainWindow::addItems(const QList<QTreeWidgetItem *> &items)
{
    std::vector<Node *> leaves;
    for (QTreeWidgetItem *it : items)
        if (Node *n = nodeOf(it)) collectLeaves(*n, leaves);
    if (leaves.empty()) {
        statusBar()->showMessage("Select a scalar variable (or a struct/array to add its members).");
        return;
    }
    for (Node *n : leaves) currentTab()->addSeries(n->path, n->addr, n->leaf->size, n->leaf->fmt, n->enumMap, n->bits);
}

void MainWindow::addByAddress()
{
    bool ok = false;
    const QString text = QInputDialog::getText(this, "Add by address", "Hex address (e.g. 0x20000010):", QLineEdit::Normal, {}, &ok);
    if (!ok || text.trimmed().isEmpty()) return;
    qint64 addr = 0;
    bool parsed = false;
    {
        QString t = text.trimmed();
        int base = 10;
        if (t.startsWith("0x") || t.startsWith("0X")) { base = 16; t = t.mid(2); }
        addr = static_cast<qint64>(t.toULongLong(&parsed, base));
    }
    if (!parsed) {
        QMessageBox::warning(this, "Bad address", "Could not parse the address.");
        return;
    }
    currentTab()->addSeries(QStringLiteral("mem_%1").arg(QString::number(addr, 16).rightJustified(8, '0').toUpper()),
                            static_cast<quint64>(addr), 4, 'I', {}, std::nullopt, true);
}

void MainWindow::addRegisterSeries(const QString &name, quint64 addr, int size, std::optional<BitSpec> bits, const EnumMap &en)
{
    if (!unsignedFmt(size)) {
        QMessageBox::information(this, "Unsupported", QStringLiteral("%1-bit registers cannot be plotted.").arg(size * 8));
        return;
    }
    currentTab()->addSeries(name, addr, size, unsignedFmt(size), en, bits, true);
    statusBar()->showMessage(QStringLiteral("Added %1 to the plot").arg(name));
}

// --------------------------------------------------------------------------
// connection / sampling
// --------------------------------------------------------------------------
void MainWindow::specsChanged()
{
    std::vector<ReadSpec> specs;
    std::unordered_map<int, Series *> byId;
    for (Series *s : allSeries()) {
        byId[s->id] = s;
        if (s->active) specs.push_back({s->id, s->addr, s->size, s->fmt, s->bits});
    }
    byId_ = std::move(byId);
    QMutexLocker lk(&specsMu_);
    specs_ = std::move(specs);
}

std::vector<ReadSpec> MainWindow::specsSnapshot()
{
    QMutexLocker lk(&specsMu_);
    return specs_;
}

void MainWindow::toggleConnect()
{
    if (poller_) {
        Poller *p = poller_;
        poller_.clear();
        p->disconnect(this);
        p->stop();
        p->wait(3000);
        btnConn_->setText("Connect");
        statusBar()->showMessage("Disconnected");
        return;
    }
    auto *p = new Poller(editHost_->text().trimmed().isEmpty() ? QStringLiteral("localhost") : editHost_->text().trimmed(),
                         static_cast<quint16>(spinPort_->value()), [this] { return specsSnapshot(); },
                         [this] { return period_.load(); });
    poller_ = p;
    connect(p, &Poller::sample, this, &MainWindow::onSample);
    connect(p, &Poller::regsReady, this, [this](quint64 token, const RegResults &res) {
        if (regDlg_) regDlg_->onRegs(token, res);
    });
    connect(p, &Poller::status, this, [this](const QString &m) { statusBar()->showMessage(m); });
    connect(p, &QThread::finished, p, &QObject::deleteLater);
    connect(p, &QThread::finished, this, [this, guard = QPointer<Poller>(p)] {
        if (guard && poller_ == guard) {
            poller_.clear();
            btnConn_->setText("Connect");
        }
    });
    p->start();
    btnConn_->setText("Disconnect");
}

void MainWindow::onSample(double t, const Samples &values)
{
    tLast_ = t;
    QHash<int, Reading> byId;
    for (const SampleValue &sv : values) {
        byId.insert(sv.id, sv.r);
        auto it = byId_.find(sv.id);
        if (it == byId_.end()) continue;
        Series *s = it->second;
        s->append(t, sv.r.v);
        s->last = sv.r;
    }
    streamSample(t, byId);
}

// --------------------------------------------------------------------------
// tabs
// --------------------------------------------------------------------------
std::vector<GraphTab *> MainWindow::allTabs() const
{
    std::vector<GraphTab *> out;
    for (int i = 0; i < tabs_->count(); ++i) out.push_back(static_cast<GraphTab *>(tabs_->widget(i)));
    return out;
}

std::vector<Series *> MainWindow::allSeries() const
{
    std::vector<Series *> out;
    for (GraphTab *tab : allTabs())
        for (const auto &s : tab->series()) out.push_back(s.get());
    return out;
}

GraphTab *MainWindow::currentTab() const { return static_cast<GraphTab *>(tabs_->currentWidget()); }

GraphTab *MainWindow::addTab(const QString &title)
{
    auto *tab = new GraphTab(this);
    ++tabNo_;
    const int i = tabs_->addTab(tab, title.isEmpty() ? QStringLiteral("Graph %1").arg(tabNo_) : title);
    tabs_->setCurrentIndex(i);
    tab->setFollow(follow_);
    if (btnCursors_ && btnCursors_->isChecked()) tab->setCursors(true);
    return tab;
}

void MainWindow::closeTab(int i)
{
    if (tabs_->count() <= 1) {
        statusBar()->showMessage("Keep at least one graph tab.");
        return;
    }
    auto *tab = static_cast<GraphTab *>(tabs_->widget(i));
    if (!tab->series().empty() &&
        QMessageBox::question(this, "Close tab",
                              QStringLiteral("Close '%1' and remove its %2 variable(s)?").arg(tabs_->tabText(i)).arg(tab->series().size())) !=
            QMessageBox::Yes)
        return;
    tab->removeAllSeries();
    tabs_->removeTab(i);
    tab->deleteLater();
    specsChanged();
}

void MainWindow::renameTab(int i)
{
    if (i < 0) return;
    bool ok = false;
    const QString text = QInputDialog::getText(this, "Rename tab", "Tab name:", QLineEdit::Normal, tabs_->tabText(i), &ok);
    if (ok && !text.trimmed().isEmpty()) tabs_->setTabText(i, text.trimmed());
}

// --------------------------------------------------------------------------
// display
// --------------------------------------------------------------------------
void MainWindow::refresh()
{
    if (++tick_ % 20 == 0) checkElf();
    const bool cur = btnCursors_->isChecked();
    const double window = spinWindow_->value();
    for (GraphTab *tab : allTabs()) tab->refresh(cur, follow_, window, tLast_);
    updateStreamUi();
}

void MainWindow::setFollow(bool on)
{
    follow_ = on;
    for (GraphTab *tab : allTabs()) tab->setFollow(on);
}

// Fit X: show the whole recorded time range of the current tab and stop following.
void MainWindow::fitView()
{
    btnFollow_->setChecked(false);
    follow_ = false;
    currentTab()->fitX();
}

void MainWindow::setCursors(bool on)
{
    if (on) btnFollow_->setChecked(false);
    for (GraphTab *tab : allTabs()) tab->setCursors(on);
}

void MainWindow::clearData()
{
    for (GraphTab *tab : allTabs()) tab->clearData();
}

// --------------------------------------------------------------------------
// registers / streaming / export
// --------------------------------------------------------------------------
void MainWindow::showRegisters()
{
    if (!regDlg_) regDlg_ = new RegistersDialog(this);
    regDlg_->show();
    regDlg_->raise();
    regDlg_->activateWindow();
}

void MainWindow::showStream()
{
    if (!streamDlg_) streamDlg_ = new StreamDialog(this);
    streamDlg_->updateStatus();
    streamDlg_->show();
    streamDlg_->raise();
    streamDlg_->activateWindow();
}

QString MainWindow::startStream(const StreamConfig &cfg)
{
    auto sink = std::make_unique<StreamSink>(cfg.target, cfg.path, cfg.baud, cfg.append);
    try {
        sink->open();
    } catch (const std::exception &e) {
        streamMsg_ = QStringLiteral("Cannot start: %1").arg(e.what());
        return e.what();
    }
    sink->start();
    sink_ = std::move(sink);
    streamCfg_ = cfg;
    streamColsValid_ = false;
    streamN_ = 0;
    streamMsg_.clear();
    statusBar()->showMessage(QStringLiteral("Streaming to %1").arg(cfg.path));
    return {};
}

void MainWindow::stopStream()
{
    if (!sink_) return;
    std::unique_ptr<StreamSink> sink = std::move(sink_);
    sink->close();
    streamMsg_ = QStringLiteral("Stopped (%1 bytes sent, %2 dropped)").arg(sink->written()).arg(sink->dropped());
}

void MainWindow::streamSample(double t, const QHash<int, Reading> &values)
{
    if (!sink_) return;
    const StreamConfig &cfg = streamCfg_;
    ++streamN_;
    if (cfg.every > 1 && (streamN_ - 1) % cfg.every) return;

    std::vector<const Series *> cols;
    std::vector<QString> seen;                                  // a variable plotted in several tabs is sent once
    for (const Series *s : allSeries()) {
        if (!values.contains(s->id) || std::find(seen.begin(), seen.end(), s->name) != seen.end()) continue;
        seen.push_back(s->name);
        cols.push_back(s);
    }
    if (cols.empty()) return;

    std::vector<stream::Item> items;
    for (const Series *s : cols) {
        const Reading &r = values[s->id];
        const double num = cfg.scaled ? r.v * s->gain + s->offset : r.v;
        items.push_back({s->name, stream::numText(s->fmt, num, r.raw, s->bits.has_value(), cfg.scaled), num});
    }
    const bool isUnix = cfg.time != stream::SinceConnect;
    bool hasTs = cfg.time != stream::NoTime;
    double ts = t;
    if (cfg.time == stream::UnixTime) {
        const double w0 = poller_ && poller_->wall0() > 0 ? poller_->wall0() : QDateTime::currentMSecsSinceEpoch() / 1000.0 - t;
        ts = w0 + t;
    }
    std::vector<int> ids;
    for (const Series *s : cols) ids.push_back(s->id);
    const bool newHeader = !streamColsValid_ || ids != streamCols_;
    streamCols_ = ids;
    streamColsValid_ = true;
    sink_->push(stream::formatSample(cfg.format, items, hasTs, ts, isUnix, newHeader));
}

void MainWindow::updateStreamUi()
{
    if (sink_ && !sink_->error().isEmpty()) {
        const QString err = sink_->error();
        stopStream();
        streamMsg_ = "Stopped: " + err;
        statusBar()->showMessage("Streaming stopped: " + err);
    }
    btnStream_->setText(sink_ ? QString::fromUtf8("Stream ●") : QString::fromUtf8("Stream…"));
    if (streamDlg_ && streamDlg_->isVisible()) streamDlg_->updateStatus();
}

void MainWindow::exportCsv()
{
    const QString path = QFileDialog::getSaveFileName(this, "Export CSV", "samples.csv", "CSV (*.csv)");
    if (!path.isEmpty() && writeCsv(path)) statusBar()->showMessage("Saved " + path);
}

bool MainWindow::writeCsv(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, "Export CSV", "Cannot write " + path + ": " + f.errorString());
        return false;
    }
    f.write("variable,time_s,raw,processed,tab\r\n");
    const auto tabs = allTabs();
    for (size_t i = 0; i < tabs.size(); ++i) {
        const QString tabName = csvQuote(tabs_->tabText(tabs_->indexOf(tabs[i])));
        for (const auto &s : tabs[i]->series()) {
            if (s->t.empty()) continue;
            const std::vector<double> &proc = s->processed();
            QByteArray block;
            for (size_t k = 0; k < s->t.size(); ++k) {
                block += (csvQuote(s->name) + ',' + QString::number(s->t[k], 'f', 6) + ',' + shortest(s->y[k]) + ',' + shortest(proc[k]) +
                          ',' + tabName + "\r\n").toUtf8();
            }
            f.write(block);
        }
    }
    return true;
}

void MainWindow::closeEvent(QCloseEvent *ev)
{
    stopStream();
    if (poller_) {
        Poller *p = poller_;
        poller_.clear();
        p->disconnect(this);
        p->stop();
        p->wait(3000);
    }
    QMainWindow::closeEvent(ev);
}
