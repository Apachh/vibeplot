#include "Processing.h"
#include "Series.h"

#include <QtTest>

#include <cmath>

using V = std::vector<double>;

class TestProcessing : public QObject {
    Q_OBJECT
    static void compare(const V &got, const V &want)
    {
        QCOMPARE(got.size(), want.size());
        for (size_t i = 0; i < got.size(); ++i) QVERIFY2(std::fabs(got[i] - want[i]) < 1e-9, qPrintable(QString("index %1: %2 != %3").arg(i).arg(got[i]).arg(want[i])));
    }
    static V times(size_t n)
    {
        V t(n);
        for (size_t i = 0; i < n; ++i) t[i] = static_cast<double>(i);
        return t;
    }

private slots:
    void movingMean() { compare(proc::movingMean({1, 2, 3, 4, 5}, 3), {1, 1.5, 2, 3, 4}); }

    void ema()
    {
        compare(proc::ema({0, 10, 10}, 0.5), {0, 5, 7.5});
        compare(proc::ema({}, 0.5), {});
    }

    void rms() { compare(proc::applyMode("Moving RMS (N)", 2, times(3), {3, 4, 0}), {3, std::sqrt(12.5), std::sqrt(8.0)}); }

    void medianOddAndEven()
    {
        compare(proc::applyMode("Median (N)", 3, times(4), {1, 100, 2, 3}), {1, 1, 2, 3});
        compare(proc::applyMode("Median (N)", 4, times(4), {1, 3, 5, 7}), {1, 1, 2, 4});   // numpy.median of an even window
    }

    void derivativeOnUnevenGrid()
    {
        const V t = {0, 1, 3, 4};
        V y;
        for (double x : t) y.push_back(2 * x + 1);
        compare(proc::applyMode("Derivative d/dt", 0, t, y), {2, 2, 2, 2});
        QCOMPARE(proc::applyMode("Derivative d/dt", 0, {0}, {5}).size(), size_t(1));
    }

    void integral() { compare(proc::applyMode(QString::fromUtf8("Integral ∫dt"), 0, times(3), {2, 2, 2}), {0, 2, 4}); }

    void simpleModes()
    {
        compare(proc::applyMode("Absolute value", 0, times(3), {-1, 2, -3}), {1, 2, 3});
        compare(proc::applyMode("Remove mean", 0, times(3), {1, 2, 6}), {-2, -1, 3});
        compare(proc::applyMode("None", 0, times(2), {4, 5}), {4, 5});
    }

    void gainAndOffset() { compare(proc::process("None", 0, 2.0, 1.0, times(3), {1, 2, 3}), {3, 5, 7}); }

    void seriesCachesUntilChanged()
    {
        Series s(0, "x", 0, 4, 'I', Qt::red);
        for (int i = 0; i < 5; ++i) s.append(i, i);
        s.mode = "Moving average (N)";
        s.param = 2;
        compare(s.processed(), {0, 0.5, 1.5, 2.5, 3.5});
        s.param = 1;
        compare(s.processed(), {0, 1, 2, 3, 4});          // setting changed
        s.append(5, 10);
        QCOMPARE(s.processed().size(), size_t(6));        // data changed
    }

    void seriesKeepsAtMostMaxPoints()
    {
        Series s(0, "x", 0, 4, 'I', Qt::red);
        for (int i = 0; i < kMaxPoints + 10; ++i) s.append(i, i);
        QCOMPARE(s.t.size(), size_t(kMaxPoints));
        QCOMPARE(s.t.front(), 10.0);
    }

    void enumText()
    {
        Series s(0, "x", 0, 1, 'b', Qt::red);
        s.enumValues = {{-1, "Low"}, {5, "High"}};
        QCOMPARE(s.fmtEnum(5), QString("High (5)"));
        QCOMPARE(s.fmtEnum(-1), QString("Low (-1)"));
        QCOMPARE(s.fmtEnum(3), QString("3"));
        Series u(0, "u", 0, 1, 'b', Qt::red);
        u.enumValues = {{255, "Low"}};                     // DWARF stored the signed enumerator unsigned
        QCOMPARE(u.fmtEnum(-1), QString("Low (-1)"));
    }
};

QTEST_GUILESS_MAIN(TestProcessing)
#include "test_processing.moc"
