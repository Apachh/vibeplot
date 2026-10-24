#include "GraphTab.h"

#include "ElfModel.h"
#include "MainWindow.h"
#include "Poller.h"
#include "Processing.h"
#include "StreamFormat.h"
#include "ValueParse.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QShortcut>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

QString g6(double v) { return QString::number(v, 'g', 6); }

// Linear interpolation of a series at time x, or nullopt if x is outside the data.
std::optional<double> interpAt(const std::vector<double> &ts, const std::vector<double> &ys, double x)
{
    if (ts.empty() || x < ts.front() || x > ts.back()) return std::nullopt;
    const size_t hi = std::lower_bound(ts.begin(), ts.end(), x) - ts.begin();
    if (hi == 0) return ys[0];
    const size_t lo = hi - 1;
    if (ts[hi] == ts[lo]) return ys[hi];
    return ys[lo] + (ys[hi] - ys[lo]) * (x - ts[lo]) / (ts[hi] - ts[lo]);
}

// Value of the latest sample at or before x (for enums), or nullopt outside the data.
std::optional<double> stepAt(const std::vector<double> &ts, const std::vector<double> &ys, double x)
{
    if (ts.empty() || x < ts.front() || x > ts.back()) return std::nullopt;
    const size_t i = std::upper_bound(ts.begin(), ts.end(), x) - ts.begin();
    return ys[i - 1];
}

}  // namespace

GraphTab::GraphTab(MainWindow *main) : main_(main)
{
    plotSplit_ = new QSplitter(Qt::Vertical);

    table_ = new QTableWidget(0, ColCount);
    table_->setHorizontalHeaderLabels({"Show", "Name", "Address", "Type", "Offset", "Value (dbl-click: write)", "@C1",
                                       "@C2", QString::fromUtf8("Δ (C2−C1)"), "", "Graph"});
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->verticalHeader()->setVisible(false);
    table_->horizontalHeader()->setSectionResizeMode(Name, QHeaderView::Stretch);
    const int widths[][2] = {{Show, 45}, {Address, 90}, {Type, 90}, {Offset, 110}, {Value, 170},
                             {AtC1, 130}, {AtC2, 130}, {Delta, 90}, {Remove, 40}, {Graph, 90}};
    for (const auto &w : widths) table_->setColumnWidth(w[0], w[1]);
    table_->horizontalHeader()->moveSection(Graph, Address);        // show "Graph" next to the name
    connect(table_, &QTableWidget::cellDoubleClicked, this, [this](int r, int c) {
        if (c == Value) editValue(r);
    });
    auto *del = new QShortcut(QKeySequence("Delete"), table_);
    del->setContext(Qt::WidgetWithChildrenShortcut);
    connect(del, &QShortcut::activated, this, &GraphTab::removeSelected);

    auto *btnRmSel = new QPushButton("Remove selected (Del)");
    connect(btnRmSel, &QPushButton::clicked, this, &GraphTab::removeSelected);
    auto *btnRmAll = new QPushButton("Remove all variables");
    connect(btnRmAll, &QPushButton::clicked, this, &GraphTab::removeAllSeries);
    auto *btnWrite = new QPushButton(QString::fromUtf8("Write value…"));
    btnWrite->setToolTip("Write a new value into the selected variable's memory (needs Connect)");
    connect(btnWrite, &QPushButton::clicked, this, [this] { editValue(); });
    auto *btnAddGraph = new QPushButton("Add graph");
    btnAddGraph->setToolTip("Add another graph below, sharing the time axis");
    connect(btnAddGraph, &QPushButton::clicked, this, &GraphTab::addPlot);
    auto *btnRmGraph = new QPushButton("Remove graph");
    btnRmGraph->setToolTip("Remove the graph chosen on the right; its variables move to Graph 1");
    connect(btnRmGraph, &QPushButton::clicked, this, &GraphTab::removePlot);
    cmbTarget_ = new QComboBox;
    cmbTarget_->setToolTip("Graph that receives newly added variables");

    auto *rowb = new QHBoxLayout;
    rowb->addWidget(btnAddGraph);
    rowb->addWidget(btnRmGraph);
    rowb->addWidget(new QLabel("New variables go to"));
    rowb->addWidget(cmbTarget_);
    rowb->addSpacing(12);
    rowb->addWidget(btnWrite);
    rowb->addWidget(btnRmSel);
    rowb->addWidget(btnRmAll);
    rowb->addStretch(1);

    auto *tblPanel = new QWidget;
    auto *tl = new QVBoxLayout(tblPanel);
    tl->setContentsMargins(0, 0, 0, 0);
    ppBox_ = new QGroupBox("Post-processing of selected variable(s)  (click a row's name to select)");
    auto *pl = new QHBoxLayout(ppBox_);
    lblSel_ = new QLabel;
    cmbMode_ = new QComboBox;
    cmbMode_->addItems(proc::modes());
    spinParam_ = new QDoubleSpinBox;
    spinGain_ = new QDoubleSpinBox;
    spinGain_->setRange(-1e9, 1e9);
    spinGain_->setDecimals(6);
    spinGain_->setSingleStep(0.1);
    spinGain_->setValue(1.0);
    auto *btnPpReset = new QPushButton("Reset");
    btnPpReset->setToolTip("Mode None, gain 1, offset 0 for the selected variable(s)");
    pl->addWidget(lblSel_, 1);
    pl->addWidget(new QLabel("Mode"));
    pl->addWidget(cmbMode_);
    pl->addWidget(new QLabel(QString::fromUtf8("N / α")));
    pl->addWidget(spinParam_);
    pl->addWidget(new QLabel(QString::fromUtf8("Gain ×")));
    pl->addWidget(spinGain_);
    pl->addWidget(btnPpReset);
    configureParam(proc::modes().first());
    connect(cmbMode_, &QComboBox::currentIndexChanged, this, [this] { onModeChanged(); });
    connect(spinParam_, &QDoubleSpinBox::valueChanged, this, [this] { onPpChanged(); });
    connect(spinGain_, &QDoubleSpinBox::valueChanged, this, [this] { onPpChanged(); });
    connect(btnPpReset, &QPushButton::clicked, this, &GraphTab::resetPp);
    connect(table_, &QTableWidget::itemSelectionChanged, this, &GraphTab::onSelection);

    tl->addLayout(rowb);
    tl->addWidget(ppBox_);
    tl->addWidget(table_);
    onSelection();

    addPlot();
    auto *split = new QSplitter(Qt::Vertical);
    split->addWidget(plotSplit_);
    split->addWidget(tblPanel);
    split->setSizes({520, 280});
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->addWidget(split);
}

// --------------------------------------------------------------------------
// graphs inside the tab
// --------------------------------------------------------------------------
void GraphTab::addPlot()
{
    auto *pw = new PlotWidget;
    pw->setAutoVisible(following_);
    pw->setCursorsVisible(cursorsOn_);
    if (!plots_.empty()) {                                  // same cursor positions and time range as the others
        for (int i = 0; i < 2; ++i) pw->setCursorValue(i, plots_[0]->cursorValue(i));
        auto [lo, hi] = plots_[0]->xRange();
        pw->setXRange(lo, hi, false);
    }
    connect(pw, &PlotWidget::cursorMoved, this, [this, pw](int i, double x) { syncCursor(pw, i, x); });
    connect(pw, &PlotWidget::xRangeChanged, this, [this, pw](double lo, double hi) { syncRange(pw, lo, hi); });
    plots_.push_back(pw);
    plotSplit_->addWidget(pw);
    plotsChanged();
    cmbTarget_->setCurrentIndex(static_cast<int>(plots_.size()) - 1);
}

void GraphTab::syncCursor(PlotWidget *src, int index, double x)
{
    for (PlotWidget *pw : plots_)
        if (pw != src) pw->setCursorValue(index, x);
}

void GraphTab::syncRange(PlotWidget *src, double lo, double hi)
{
    if (inRangeSync_) return;
    inRangeSync_ = true;
    for (PlotWidget *pw : plots_)
        if (pw != src) pw->setXRange(lo, hi, false);
    inRangeSync_ = false;
}

void GraphTab::plotsChanged()
{
    QStringList names;
    for (size_t i = 0; i < plots_.size(); ++i) names << QStringLiteral("Graph %1").arg(i + 1);
    const int cur = cmbTarget_->currentIndex();
    cmbTarget_->blockSignals(true);
    cmbTarget_->clear();
    cmbTarget_->addItems(names);
    cmbTarget_->setCurrentIndex(std::clamp(cur, 0, static_cast<int>(names.size()) - 1));
    cmbTarget_->blockSignals(false);
    for (size_t r = 0; r < series_.size(); ++r) table_->setCellWidget(static_cast<int>(r), Graph, makeGraphWidget(series_[r].get()));
}

void GraphTab::removePlot()
{
    if (plots_.size() <= 1) {
        main_->statusBar()->showMessage("Keep at least one graph in the tab.");
        return;
    }
    const int i = cmbTarget_->currentIndex();
    PlotWidget *pw = plots_[static_cast<size_t>(i)];
    for (auto &s : series_)
        if (s->plot == pw) moveSeries(s.get(), i ? 0 : 1);
    plots_.erase(plots_.begin() + i);
    pw->hide();
    pw->setParent(nullptr);
    pw->deleteLater();
    plotsChanged();
}

QWidget *GraphTab::makeGraphWidget(Series *s)
{
    auto *combo = new QComboBox;
    for (size_t i = 0; i < plots_.size(); ++i) combo->addItem(QStringLiteral("Graph %1").arg(i + 1));
    const auto it = std::find(plots_.begin(), plots_.end(), s->plot);
    combo->setCurrentIndex(it == plots_.end() ? 0 : static_cast<int>(it - plots_.begin()));
    connect(combo, &QComboBox::currentIndexChanged, this, [this, s](int i) { moveSeries(s, i); });
    return combo;
}

void GraphTab::moveSeries(Series *s, int i)
{
    const int row = rowOf(s);
    if (i < 0 || i >= static_cast<int>(plots_.size()) || plots_[static_cast<size_t>(i)] == s->plot) {
        if (row >= 0) table_->setCellWidget(row, Graph, makeGraphWidget(s));
        return;
    }
    detachCurve(s);
    s->plot = plots_[static_cast<size_t>(i)];
    s->curve = s->plot->addCurve(s->name, s->color);
    s->plot->setCurveVisible(s->curve, s->active);
    if (row >= 0) table_->setCellWidget(row, Graph, makeGraphWidget(s));
}

void GraphTab::detachCurve(Series *s)
{
    if (s->plot) s->plot->removeCurve(s->curve);
    s->curve = -1;
}

// --------------------------------------------------------------------------
// series management
// --------------------------------------------------------------------------
int GraphTab::rowOf(const Series *s) const
{
    for (size_t i = 0; i < series_.size(); ++i)
        if (series_[i].get() == s) return static_cast<int>(i);
    return -1;
}

Series *GraphTab::addSeries(const QString &name, quint64 addr, int size, char fmt, const EnumMap &en,
                            std::optional<BitSpec> bits, bool fixed)
{
    for (auto &s : series_)
        if (s->name == name && s->addr == addr) return nullptr;
    const int sid = main_->newId();
    auto s = std::make_unique<Series>(sid, name, addr, size, fmt, QColor(plotColors()[sid % plotColors().size()]));
    s->enumValues = en;
    s->bits = bits;
    s->fixed = fixed;
    s->plot = plots_[static_cast<size_t>(std::max(cmbTarget_->currentIndex(), 0))];
    s->curve = s->plot->addCurve(name, s->color);
    Series *raw = s.get();
    series_.push_back(std::move(s));
    addRow(raw);
    main_->specsChanged();
    return raw;
}

void GraphTab::addRow(Series *s)
{
    const int r = table_->rowCount();
    table_->insertRow(r);
    auto *chk = new QCheckBox;
    chk->setChecked(true);
    connect(chk, &QCheckBox::toggled, this, [this, s](bool on) { setActive(s, on); });
    table_->setCellWidget(r, Show, chk);
    auto *name = new QTableWidgetItem(s->name);
    name->setForeground(QBrush(s->color));
    table_->setItem(r, Name, name);
    table_->setItem(r, Address, new QTableWidgetItem(QStringLiteral("0x%1").arg(s->addr, 8, 16, QLatin1Char('0')).toUpper().replace("0X", "0x")));
    table_->setCellWidget(r, Type, makeTypeWidget(s));
    table_->setCellWidget(r, Offset, makeOffsetWidget(s));
    table_->setItem(r, Value, new QTableWidgetItem(QString::fromUtf8("–")));
    for (int col : {AtC1, AtC2, Delta}) table_->setItem(r, col, new QTableWidgetItem(""));
    auto *btn = new QPushButton(QString::fromUtf8("✕"));
    connect(btn, &QPushButton::clicked, this, [this, s] { removeSeries(s); });
    table_->setCellWidget(r, Remove, btn);
    table_->setCellWidget(r, Graph, makeGraphWidget(s));
}

QWidget *GraphTab::makeTypeWidget(Series *s)
{
    if (s->bits) {
        return new QLabel(QStringLiteral(" %1%2 bit(s) @ bit %3").arg(s->bits->isSigned ? "s" : "u").arg(s->bits->width).arg(s->bits->shift));
    }
    auto *combo = new QComboBox;
    for (const auto &t : typeTable()) combo->addItem(t.name);
    combo->setCurrentText(typeNameFor(s->size, s->fmt));
    connect(combo, &QComboBox::currentTextChanged, this, [this, s](const QString &txt) { setType(s, txt); });
    return combo;
}

// Offset editor that matches the variable's type: integer spin box for integer variables,
// decimal spin box (with the type's precision) for floats.
QWidget *GraphTab::makeOffsetWidget(Series *s)
{
    const auto step = QAbstractSpinBox::AdaptiveDecimalStepType;
    auto intBox = [&](qint64 lim) {
        auto *w = new QSpinBox;
        const int l = static_cast<int>(std::min<qint64>(lim, 2147483647));
        w->setRange(-l, l);
        w->setStepType(step);
        w->setValue(static_cast<int>(std::llround(s->offset)));
        s->offset = w->value();                                // keep the clamped / rounded value
        connect(w, &QSpinBox::valueChanged, this, [s](int v) { s->offset = v; });
        return w;
    };
    auto dblBox = [&](int decimals, double lim) {
        auto *w = new QDoubleSpinBox;
        w->setDecimals(decimals);
        w->setRange(-lim, lim);
        w->setStepType(step);
        w->setValue(decimals == 0 ? std::round(s->offset) : s->offset);
        s->offset = w->value();
        connect(w, &QDoubleSpinBox::valueChanged, this, [s](double v) { s->offset = v; });
        return w;
    };
    if (s->bits) return intBox((qint64(1) << std::min(s->bits->width, 62)) - 1);
    if (isFloatFmt(s->fmt)) return dblBox(s->fmt == 'f' ? 6 : 9, 1e15);
    if (s->size <= 4) return intBox((qint64(1) << (8 * s->size)) - 1);
    return dblBox(0, 9007199254740992.0);                      // 64-bit integers: exact range of a double
}

void GraphTab::setActive(Series *s, bool on)
{
    s->active = on;
    if (s->plot) s->plot->setCurveVisible(s->curve, on);
    main_->specsChanged();
}

void GraphTab::setType(Series *s, const QString &typeName)
{
    for (const auto &t : typeTable()) {
        if (typeName != t.name) continue;
        s->typeOverridden = true;
        s->size = t.size;
        s->fmt = t.fmt;
        break;
    }
    s->clearData();
    main_->specsChanged();
    const int row = rowOf(s);
    if (row >= 0) table_->setCellWidget(row, Offset, makeOffsetWidget(s));
}

void GraphTab::removeSeries(Series *s)
{
    const int idx = rowOf(s);
    if (idx < 0) return;
    detachCurve(s);
    table_->removeRow(idx);
    series_.erase(series_.begin() + idx);
    main_->specsChanged();
}

void GraphTab::removeSelected()
{
    std::vector<int> rows;
    for (const QModelIndex &ix : table_->selectionModel()->selectedIndexes())
        if (std::find(rows.begin(), rows.end(), ix.row()) == rows.end()) rows.push_back(ix.row());
    std::sort(rows.rbegin(), rows.rend());
    for (int r : rows)
        if (r < static_cast<int>(series_.size())) removeSeries(series_[static_cast<size_t>(r)].get());
}

void GraphTab::removeAllSeries()
{
    while (!series_.empty()) removeSeries(series_.back().get());
}

std::pair<int, int> GraphTab::remapSeries()
{
    int moved = 0, missing = 0;
    const ElfModel *model = main_->model();
    if (!model) return {0, 0};
    std::map<QString, std::vector<Node *>> byName;
    for (const auto &n : model->roots) byName[n->label].push_back(n.get());

    for (size_t row = 0; row < series_.size(); ++row) {
        Series *s = series_[row].get();
        if (s->fixed) continue;                                // register / raw address: nothing to look up
        int cut = s->name.size();
        for (QChar ch : {QChar('.'), QChar('[')}) {
            const int i = s->name.indexOf(ch);
            if (i != -1) cut = std::min(cut, i);
        }
        Node *best = nullptr;
        auto it = byName.find(s->name.left(cut));
        if (it != byName.end()) {
            for (Node *root : it->second) {                    // the same name can exist in several files
                Node *node = findNode(*root, s->name);
                if (node && node->leaf) {
                    auto dist = [&](Node *n) { return n->addr > s->addr ? n->addr - s->addr : s->addr - n->addr; };
                    if (!best || dist(node) < dist(best)) best = node;
                }
            }
        }
        if (!best) {
            ++missing;
            continue;
        }
        if (best->addr != s->addr) ++moved;
        s->addr = best->addr;
        s->enumValues = best->enumMap;
        if (!s->typeOverridden) {
            s->size = best->leaf->size;
            s->fmt = best->leaf->fmt;
            s->bits = best->bits;
        }
        setText(static_cast<int>(row), Address, QStringLiteral("0x%1").arg(s->addr, 8, 16, QLatin1Char('0')).toUpper().replace("0X", "0x"));
        table_->setCellWidget(static_cast<int>(row), Type, makeTypeWidget(s));
        table_->setCellWidget(static_cast<int>(row), Offset, makeOffsetWidget(s));
    }
    main_->specsChanged();
    return {moved, missing};
}

// --------------------------------------------------------------------------
// display
// --------------------------------------------------------------------------
void GraphTab::setText(int row, int col, const QString &text)
{
    if (QTableWidgetItem *item = table_->item(row, col)) item->setText(text);
}

void GraphTab::setCell(int row, int col, std::optional<double> v)
{
    setText(row, col, v ? g6(*v) : QString::fromUtf8("–"));
}

void GraphTab::updateEnumAxis()
{
    // Label the Y axis with enum names when every visible variable of the graph is a plain enum.
    for (PlotWidget *pw : plots_) {
        std::vector<const Series *> act;
        for (const auto &s : series_)
            if (s->active && s->plot == pw) act.push_back(s.get());
        bool ok = !act.empty();
        for (const Series *s : act) ok = ok && s->enumActive() && s->mode == "None" && s->gain == 1.0 && s->offset == 0.0;
        PlotWidget::Ticks ticks;
        if (ok) {
            EnumMap merged;
            for (const Series *s : act)
                for (auto it = s->enumValues.begin(); it != s->enumValues.end(); ++it) merged.insert(it.key(), it.value());
            if (merged.size() <= 64)
                for (auto it = merged.begin(); it != merged.end(); ++it) ticks.push_back({static_cast<double>(it.key()), it.value()});
        }
        if (ticks != pw->enumTicks()) pw->setEnumTicks(ticks);
    }
}

void GraphTab::refresh(bool cursors, bool follow, double window, double tLast)
{
    const double x1 = plots_[0]->cursorValue(0), x2 = plots_[0]->cursorValue(1);
    for (size_t i = 0; i < series_.size(); ++i) {
        Series *s = series_[i].get();
        const int row = static_cast<int>(i);
        const size_t n = s->t.size();
        std::vector<double> ts, raw;
        const std::vector<double> *ys = nullptr;
        if (n) {
            ts = s->times();
            raw = s->raw();
            ys = &s->processed();
            if (s->active) s->plot->setCurveData(s->curve, ts, *ys);
        }
        const bool en = s->enumActive();
        if (n) setText(row, Value, en ? s->fmtEnum(raw.back()) : g6(ys->back()));
        if (cursors) {
            if (en) {
                const auto a = stepAt(ts, raw, x1), b = stepAt(ts, raw, x2);
                setText(row, AtC1, a ? s->fmtEnum(*a) : QString::fromUtf8("–"));
                setText(row, AtC2, b ? s->fmtEnum(*b) : QString::fromUtf8("–"));
                setText(row, Delta, !a || !b ? QString() : (*a != *b ? "changed" : "same"));
            } else {
                const std::vector<double> empty;
                const auto v1 = interpAt(ts, ys ? *ys : empty, x1), v2 = interpAt(ts, ys ? *ys : empty, x2);
                setCell(row, AtC1, v1);
                setCell(row, AtC2, v2);
                setCell(row, Delta, v1 && v2 ? std::optional<double>(*v2 - *v1) : std::nullopt);
            }
        }
    }
    updateEnumAxis();
    if (cursors) {
        const double dt = x2 - x1;
        const QString freq = dt != 0 ? QStringLiteral("   (%1 Hz)").arg(1.0 / std::fabs(dt), 0, 'g', 4) : QString();
        const QString title = QStringLiteral("C1 = %1 s    C2 = %2 s    Δt = %3 s%4")
                                  .arg(x1, 0, 'f', 4).arg(x2, 0, 'f', 4).arg(dt, 0, 'f', 4).arg(freq);
        for (PlotWidget *pw : plots_) pw->setTitle(title);
    }
    if (follow && !series_.empty()) plots_[0]->setXRange(std::max(0.0, tLast - window), std::max(tLast, 0.1));
}

void GraphTab::setFollow(bool on)
{
    following_ = on;
    for (PlotWidget *pw : plots_) {
        pw->setAutoVisible(on);
        if (on) pw->enableAutoY();
    }
}

void GraphTab::fitX()
{
    // Show the whole recorded time range.
    double lo = INFINITY, hi = -INFINITY;
    for (const auto &s : series_) {
        if (!s->active || s->t.empty()) continue;
        lo = std::min(lo, s->t.front());
        hi = std::max(hi, s->t.back());
    }
    if (lo > hi) return;
    if (hi <= lo) hi = lo + 1.0;
    const double pad = 0.02 * (hi - lo);
    plots_[0]->setXRange(lo - pad, hi + pad);
}

void GraphTab::fitY()
{
    for (PlotWidget *pw : plots_) {
        pw->setAutoVisible(true);
        pw->enableAutoY();
    }
}

void GraphTab::setCursors(bool on)
{
    cursorsOn_ = on;
    for (PlotWidget *pw : plots_) pw->setCursorsVisible(on);
    if (on) {
        const auto [x0, x1] = plots_[0]->xRange();
        for (PlotWidget *pw : plots_) {
            pw->setCursorValue(0, x0 + 0.35 * (x1 - x0));
            pw->setCursorValue(1, x0 + 0.65 * (x1 - x0));
        }
    } else {
        for (PlotWidget *pw : plots_) pw->setTitle(QString());
        for (size_t i = 0; i < series_.size(); ++i)
            for (int col : {AtC1, AtC2, Delta}) setText(static_cast<int>(i), col, QString());
    }
}

void GraphTab::clearData()
{
    for (auto &s : series_) {
        s->clearData();
        s->plot->setCurveData(s->curve, {}, {});
    }
}

// --------------------------------------------------------------------------
// writing values
// --------------------------------------------------------------------------
void GraphTab::editValue(int row)
{
    auto status = [this](const QString &m) { main_->statusBar()->showMessage(m); };
    Series *s = nullptr;
    if (row < 0) {
        const auto sel = selectedSeries();
        if (sel.empty()) {
            status("Select a variable (click its name) to write a value.");
            return;
        }
        s = sel.front();
    } else if (row < static_cast<int>(series_.size())) {
        s = series_[static_cast<size_t>(row)].get();
    } else {
        return;
    }
    if (s->bits) {
        QMessageBox::information(this, "Bitfield",
                                 "Bitfields are read-only: writing one would need a read-modify-write that can race with the firmware.");
        return;
    }
    if (!main_->poller()) {
        QMessageBox::information(this, "Not connected", "Press Connect first.");
        return;
    }
    const std::optional<Reading> cur = s->last;
    const QString title = "Write " + s->name;
    QString text;
    bool ok = false;
    if (s->enumActive()) {
        QStringList items;
        for (auto it = s->enumValues.begin(); it != s->enumValues.end(); ++it)
            items << QStringLiteral("%1 (%2)").arg(it.value()).arg(it.key());
        int idx = 0;
        if (cur) idx = std::max(0, static_cast<int>(items.indexOf(s->fmtEnum(cur->v))));
        text = QInputDialog::getItem(this, title, "Pick an enum value (or type a number):", items, idx, true, &ok);
    } else {
        QString dflt;
        if (cur) dflt = isFloatFmt(s->fmt) ? QString::number(cur->v, 'g', 9) : stream::numText(s->fmt, cur->v, cur->raw, false, false);
        text = QInputDialog::getText(this, title,
                                     QStringLiteral("New value (%1, decimal or 0x hex; written raw, ignores gain/offset):")
                                         .arg(typeNameOrQuestion(s->size, s->fmt)),
                                     QLineEdit::Normal, dflt, &ok);
    }
    if (!ok || text.trimmed().isEmpty()) return;
    QByteArray raw;
    QString err;
    if (!valparse::packValue(text, s->fmt, s->size, s->enumActive() ? &s->enumValues : nullptr, raw, err)) {
        QMessageBox::warning(this, "Invalid value", QStringLiteral("Cannot write '%1': %2").arg(text, err));
        return;
    }
    main_->poller()->requestWrite(s->name, s->addr, raw);
}

std::vector<Series *> GraphTab::selectedSeries() const
{
    std::vector<int> rows;
    for (const QModelIndex &ix : table_->selectionModel()->selectedIndexes())
        if (std::find(rows.begin(), rows.end(), ix.row()) == rows.end()) rows.push_back(ix.row());
    std::sort(rows.begin(), rows.end());
    std::vector<Series *> out;
    for (int r : rows)
        if (r < static_cast<int>(series_.size())) out.push_back(series_[static_cast<size_t>(r)].get());
    return out;
}

// --------------------------------------------------------------------------
// post-processing panel
// --------------------------------------------------------------------------
void GraphTab::configureParam(const QString &mode, std::optional<double> value)
{
    double lo = 0, hi = 0, dflt = 0;
    int dec = 0;
    bool on = true;
    if (mode.startsWith("Low-pass")) { lo = 0.001; hi = 1.0; dec = 3; dflt = 0.1; }
    else if (mode.startsWith("Median")) { lo = 1; hi = 101; dflt = 9; }
    else if (mode.startsWith("Moving")) { lo = 1; hi = 10000; dflt = 10; }
    else on = false;
    loading_ = true;
    spinParam_->setDecimals(dec);
    spinParam_->setRange(lo, hi);
    spinParam_->setValue(value.value_or(dflt));
    spinParam_->setEnabled(on);
    loading_ = false;
}

void GraphTab::onSelection()
{
    const auto sel = selectedSeries();
    ppBox_->setEnabled(!sel.empty());
    if (sel.empty()) {
        lblSel_->setText("no variable selected");
        return;
    }
    Series *s = sel.front();
    QStringList names;
    for (size_t i = 0; i < std::min<size_t>(3, sel.size()); ++i) names << sel[i]->name;
    lblSel_->setText(names.join(", ") + (sel.size() > 3 ? QString::fromUtf8(" …") : QString()));
    loading_ = true;
    cmbMode_->setCurrentText(s->mode);
    spinGain_->setValue(s->gain);
    loading_ = false;
    configureParam(s->mode, s->param);
}

void GraphTab::onModeChanged()
{
    if (loading_) return;
    configureParam(cmbMode_->currentText());
    onPpChanged();
}

void GraphTab::onPpChanged()
{
    if (loading_) return;
    const QString mode = cmbMode_->currentText();
    const double p = spinParam_->value(), g = spinGain_->value();
    for (Series *s : selectedSeries()) {
        s->mode = mode;
        s->param = p;
        s->gain = g;
    }
}

void GraphTab::resetPp()
{
    for (Series *s : selectedSeries()) {
        s->mode = "None";
        s->param = 10.0;
        s->gain = 1.0;
        QWidget *w = table_->cellWidget(rowOf(s), Offset);
        if (auto *sb = qobject_cast<QSpinBox *>(w)) sb->setValue(0);
        else if (auto *db = qobject_cast<QDoubleSpinBox *>(w)) db->setValue(0);
    }
    onSelection();
}
