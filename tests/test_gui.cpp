// End-to-end test of the main window (offscreen): load an ELF, add variables, connect to a fake
// OpenOCD, see samples arrive, and exercise the tab / graph / cursor / registers features.
#include "FakeOpenOcd.h"
#include "GraphTab.h"
#include "MainWindow.h"
#include "PlotWidget.h"
#include "RegistersDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
#include <QtTest>

#include <cmath>
#include <cstring>

static const char *kSvd = R"(<device><name>T</name><size>32</size><access>read-write</access><peripherals>
<peripheral><name>TIM1</name><baseAddress>0x40010000</baseAddress><registers>
<register><name>CR1</name><addressOffset>0</addressOffset><fields>
<field><name>CEN</name><bitOffset>0</bitOffset><bitWidth>1</bitWidth></field>
<field><name>ARPE</name><bitOffset>7</bitOffset><bitWidth>1</bitWidth></field></fields></register>
<register><name>SR</name><addressOffset>4</addressOffset></register>
</registers></peripheral></peripherals></device>)";

class TestGui : public QObject {
    Q_OBJECT
    std::unique_ptr<MainWindow> w;
    FakeOpenOcd *server = nullptr;
    QTimer physics;
    int tick = 0;

    template <class T> T *find(const char *name) const
    {
        T *x = w->findChild<T *>(name);
        if (!x) qFatal("no widget named %s", name);
        return x;
    }
    QPushButton *button(const QString &text) const
    {
        for (QPushButton *b : w->findChildren<QPushButton *>())
            if (b->text() == text) return b;
        return nullptr;
    }
    QTreeWidgetItem *treeItem(const QString &name) const
    {
        QTreeWidget *tree = find<QTreeWidget>("treeVars");
        for (int i = 0; i < tree->topLevelItemCount(); ++i)
            if (tree->topLevelItem(i)->text(0) == name) return tree->topLevelItem(i);
        return nullptr;
    }
    Series *series(const QString &name) const
    {
        for (Series *s : w->allSeries())
            if (s->name == name) return s;
        return nullptr;
    }
    void addVariable(const QString &name)
    {
        QTreeWidgetItem *it = treeItem(name);
        QVERIFY2(it, qPrintable(name));
        find<QTreeWidget>("treeVars")->clearSelection();
        it->setSelected(true);
        QTest::mouseClick(find<QPushButton>("btnAddSelected"), Qt::LeftButton);
    }
    void put64(quint64 addr, quint64 v)
    {
        server->set32(addr, static_cast<quint32>(v));
        server->set32(addr + 4, static_cast<quint32>(v >> 32));
    }

private slots:
    void initTestCase()
    {
        server = new FakeOpenOcd(this);
        w = std::make_unique<MainWindow>();
        w->resize(1400, 820);
        w->show();
        QVERIFY(w->loadElf(FIXTURE_ELF));
        // the "hardware": a sine wave, a counter and a state machine, updated every 10 ms
        connect(&physics, &QTimer::timeout, this, [this] {
            ++tick;
            const double x = 3.0 * std::sin(tick * 0.08) + 5.0;
            quint64 bits;
            std::memcpy(&bits, &x, 8);
            put64(treeItem("dval")->text(1).toULongLong(nullptr, 16), bits);
            server->set32(treeItem("app::counter")->text(1).toULongLong(nullptr, 16), static_cast<quint32>(tick % 50 - 25));
            server->mem[treeItem("mode_var")->text(1).toULongLong(nullptr, 16)] = static_cast<quint8>((tick / 60) % 3);
            server->set32(0x40010000, 0x81);                                        // TIM1->CR1: CEN | ARPE
        });
        physics.start(10);
    }
    void cleanupTestCase()
    {
        physics.stop();
        w.reset();
    }

    void loadsTheElf()
    {
        QTreeWidget *tree = find<QTreeWidget>("treeVars");
        QVERIFY(tree->topLevelItemCount() > 15);
        QVERIFY(treeItem("derived"));
        QCOMPARE(treeItem("dval")->text(2), QString("8"));
        QCOMPARE(treeItem("dval")->text(3), QString("double"));
        QVERIFY(w->statusBar()->currentMessage().contains("variables loaded"));
    }

    void expandsStructsLazily()
    {
        QTreeWidgetItem *d = treeItem("derived");
        QCOMPARE(d->childCount(), 1);                                // placeholder
        d->setExpanded(true);
        QVERIFY(d->childCount() > 5);
        QTreeWidgetItem *flags = nullptr;
        for (int i = 0; i < d->childCount(); ++i)
            if (d->child(i)->text(0) == "flags") flags = d->child(i);
        QVERIFY(flags);
        flags->setExpanded(true);
        QCOMPARE(flags->child(0)->text(0), QString("a"));
        QCOMPARE(flags->child(0)->text(4), QString("+0xC  bits 0..2"));
        d->setExpanded(false);
    }

    void filtersVariables()
    {
        find<QLineEdit>("editFilter")->setText("MATRIX");
        QVERIFY(!treeItem("matrix")->isHidden());
        QVERIFY(treeItem("dval")->isHidden());
        find<QLineEdit>("editFilter")->clear();
        QVERIFY(!treeItem("dval")->isHidden());
    }

    void plotsLiveData()
    {
        addVariable("dval");
        addVariable("app::counter");
        addVariable("mode_var");
        QCOMPARE(w->currentTab()->series().size(), size_t(3));
        QVERIFY(series("mode_var")->enumActive());

        find<QLineEdit>("editHost")->setText("127.0.0.1");
        find<QSpinBox>("spinPort")->setValue(server->serverPort());
        QTest::mouseClick(find<QPushButton>("btnConnect"), Qt::LeftButton);
        QVERIFY(w->poller());
        QTRY_VERIFY_WITH_TIMEOUT(series("dval")->t.size() > 30, 5000);

        const Series *s = series("dval");
        QVERIFY(s->last.has_value());
        QVERIFY(s->last->v >= 2.0 && s->last->v <= 8.0);
        QCOMPARE(find<QPushButton>("btnConnect")->text(), QString("Disconnect"));
        // the value shows up in the table
        auto *table = w->currentTab()->findChild<QTableWidget *>();
        QTRY_VERIFY_WITH_TIMEOUT(table->item(0, 5)->text() != QString::fromUtf8("–"), 2000);
        QVERIFY(w->statusBar()->currentMessage().contains("Connected"));
        QTest::qWait(1500);                                           // let the curves grow for the screenshot
        QVERIFY(series("app::counter")->t.size() > 100);
    }

    void screenshot()
    {
        const QByteArray dir = qgetenv("SWDPLOT_SCREENSHOT_DIR");
        if (dir.isEmpty()) QSKIP("set SWDPLOT_SCREENSHOT_DIR to write screenshots");
        QVERIFY(w->grab().save(QString::fromLocal8Bit(dir) + "/main.png"));
    }

    void cursorsMeasureBetweenPoints()
    {
        QPushButton *cur = find<QPushButton>("btnCursors");
        cur->click();
        QVERIFY(cur->isChecked());
        QVERIFY(!find<QPushButton>("btnFollow")->isChecked());        // cursors stop following
        QTest::qWait(200);
        auto *tab = w->currentTab();
        auto *table = tab->findChild<QTableWidget *>();
        QTRY_VERIFY_WITH_TIMEOUT(!table->item(0, 6)->text().isEmpty() && table->item(0, 6)->text() != "", 2000);
        QVERIFY(table->item(0, 8)->text() != "");                     // delta column
        // enum variable reports "changed" / "same" instead of a number
        const QString e = table->item(2, 8)->text();
        QVERIFY(e == "same" || e == "changed");
        auto *pw = tab->findChild<PlotWidget *>();
        const auto [lo, hi] = pw->xRange();
        QVERIFY(pw->cursorValue(0) > lo && pw->cursorValue(1) < hi);
        cur->click();
        QCOMPARE(table->item(0, 6)->text(), QString());
        find<QPushButton>("btnFollow")->click();
        QVERIFY(find<QPushButton>("btnFollow")->isChecked());
    }

    void multipleGraphsAndTabs()
    {
        auto *tab = w->currentTab();
        QPushButton *addGraph = nullptr, *rmGraph = nullptr;
        for (QPushButton *b : tab->findChildren<QPushButton *>()) {
            if (b->text() == "Add graph") addGraph = b;
            if (b->text() == "Remove graph") rmGraph = b;
        }
        QVERIFY(addGraph && rmGraph);
        addGraph->click();
        QCOMPARE(tab->findChildren<PlotWidget *>().size(), 2);
        // move the counter to graph 2 through the table's Graph column
        auto *table = tab->findChild<QTableWidget *>();
        auto *combo = qobject_cast<QComboBox *>(table->cellWidget(1, 10));
        QVERIFY(combo);
        combo->setCurrentIndex(1);
        QTRY_VERIFY_WITH_TIMEOUT(series("app::counter")->plot != series("dval")->plot, 1000);
        QTest::qWait(300);
        // new tab with its own variable
        QTabWidget *tabs = find<QTabWidget>("tabs");
        QTest::mouseClick(tabs->cornerWidget(Qt::TopRightCorner), Qt::LeftButton);
        QCOMPARE(tabs->count(), 2);
        addVariable("flag");
        QCOMPARE(w->currentTab()->series().size(), size_t(1));
        QCOMPARE(w->allSeries().size(), size_t(4));
        QTRY_VERIFY_WITH_TIMEOUT(series("flag")->t.size() > 3, 3000);
        const QByteArray dir = qgetenv("SWDPLOT_SCREENSHOT_DIR");
        if (!dir.isEmpty()) {
            tabs->setCurrentIndex(0);
            QTest::qWait(200);
            w->grab().save(QString::fromLocal8Bit(dir) + "/two_graphs.png");
        }
        // removing the graph moves its variables back to graph 1
        tabs->setCurrentIndex(0);
        rmGraph->click();
        QCOMPARE(tab->findChildren<PlotWidget *>().size(), 1);
        QCOMPARE(series("app::counter")->plot, series("dval")->plot);
        // closing a tab drops its variables from the sampling list (an empty tab closes without asking)
        static_cast<GraphTab *>(tabs->widget(1))->removeAllSeries();
        tabs->tabCloseRequested(1);
        QCOMPARE(tabs->count(), 1);
        QCOMPARE(w->allSeries().size(), size_t(3));
    }

    void postProcessingAndExport()
    {
        auto *tab = w->currentTab();
        auto *table = tab->findChild<QTableWidget *>();
        table->selectRow(0);
        QComboBox *mode = nullptr;
        for (QComboBox *c : tab->findChildren<QComboBox *>())
            if (c->findText("Moving average (N)") >= 0) mode = c;
        QVERIFY(mode);
        mode->setCurrentText("Moving average (N)");
        QCOMPARE(series("dval")->mode, QString("Moving average (N)"));
        QVERIFY(series("dval")->param > 0);
        QTest::qWait(200);

        QTemporaryDir dir;
        const QString csv = dir.filePath("out.csv");
        QVERIFY(w->writeCsv(csv));
        QFile f(csv);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QList<QByteArray> lines = f.readAll().split('\n');
        QCOMPARE(lines[0].trimmed(), QByteArray("variable,time_s,raw,processed,tab"));
        QVERIFY(lines.size() > 100);
        QVERIFY(lines[1].startsWith("dval,"));
        QCOMPARE(lines[1].split(',').size(), 5);
    }

    void writesThroughTheConnection()
    {
        Series *s = series("app::counter");
        const quint64 addr = s->addr;
        physics.stop();
        QSignalSpy st(w->poller(), &Poller::status);
        w->poller()->requestWrite(s->name, addr, QByteArray("\x2a\x00\x00\x00", 4));
        QTRY_VERIFY_WITH_TIMEOUT(server->get32(addr) == 42, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(s->last && s->last->v == 42.0, 2000);
        physics.start(10);
    }

    void registersDialog()
    {
        QTemporaryDir dir;
        QFile f(dir.filePath("t.svd"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(kSvd);
        f.close();
        QTest::mouseClick(button(QString::fromUtf8("Registers\u2026")), Qt::LeftButton);
        RegistersDialog &dlg = *w->findChild<RegistersDialog *>();       // the dialog the main window owns
        QVERIFY(dlg.isVisible());
        QVERIFY(dlg.openSvd(f.fileName(), false));
        QTreeWidget *tree = dlg.findChild<QTreeWidget *>();
        QTreeWidgetItem *tim = nullptr;
        for (int i = 0; i < tree->topLevelItemCount(); ++i)
            if (tree->topLevelItem(i)->text(0) == "TIM1") tim = tree->topLevelItem(i);
        QVERIFY(tim);
        QCOMPARE(tree->topLevelItemCount(), 1 + 5);                   // TIM1 + the built-in core groups
        tim->setExpanded(true);
        QCOMPARE(tim->childCount(), 2);
        QTreeWidgetItem *cr1 = tim->child(0);
        QCOMPARE(cr1->text(0), QString("CR1"));
        QTRY_COMPARE_WITH_TIMEOUT(cr1->text(2), QString("0x00000081"), 3000);
        cr1->setExpanded(true);
        QCOMPARE(cr1->childCount(), 2);
        QTRY_COMPARE_WITH_TIMEOUT(cr1->child(0)->text(2), QString("1"), 3000);   // CEN
        QCOMPARE(cr1->child(1)->text(1), QString("[7]"));
        // unreadable registers are reported, not fatal
        server->failing.insert(0x40010004);
        tim->setExpanded(false);
        tim->setExpanded(true);
        QTRY_COMPARE_WITH_TIMEOUT(tim->child(1)->text(2), QString("<read error>"), 3000);
        // plotting a field adds a bitfield series on the current tab
        const size_t before = w->allSeries().size();
        w->addRegisterSeries("TIM1.CR1.ARPE", 0x40010000, 4, BitSpec{7, 1, false});
        QCOMPARE(w->allSeries().size(), before + 1);
        QTRY_VERIFY_WITH_TIMEOUT(series("TIM1.CR1.ARPE")->last && series("TIM1.CR1.ARPE")->last->v == 1.0, 3000);
        dlg.hide();
    }

    void survivesElfReload()
    {
        const quint64 oldAddr = series("dval")->addr;
        Series *reg = series("TIM1.CR1.ARPE");
        QVERIFY(w->loadElf(FIXTURE_ELF));
        // reloadElf is what the Reload button does; it re-resolves variables by name
        find<QPushButton>("btnReload")->click();
        QCOMPARE(series("dval")->addr, oldAddr);
        QCOMPARE(series("TIM1.CR1.ARPE"), reg);                       // registers are not looked up in the ELF
        QCOMPARE(series("TIM1.CR1.ARPE")->addr, quint64(0x40010000));
        QVERIFY(w->statusBar()->currentMessage().contains("ELF reloaded"));
    }

    void disconnectsCleanly()
    {
        QTest::mouseClick(find<QPushButton>("btnConnect"), Qt::LeftButton);
        QVERIFY(!w->poller());
        QCOMPARE(find<QPushButton>("btnConnect")->text(), QString("Connect"));
        QTest::mouseClick(find<QPushButton>("btnConnect"), Qt::LeftButton);    // and reconnect
        QVERIFY(w->poller());
        const size_t n = series("dval")->t.size();
        QTRY_VERIFY_WITH_TIMEOUT(series("dval")->t.size() > n, 3000);
        QTest::mouseClick(find<QPushButton>("btnConnect"), Qt::LeftButton);
        QVERIFY(!w->poller());
    }
};

QTEST_MAIN(TestGui)
#include "test_gui.moc"
