// One plotted variable: what to read, how to post-process it and the recorded samples.
#pragma once

#include "Types.h"

#include <deque>
#include <vector>

class PlotWidget;

struct Series {
    Series(int id_, QString name_, quint64 addr_, int size_, char fmt_, QColor color_)
        : id(id_), name(std::move(name_)), addr(addr_), size(size_), fmt(fmt_), color(std::move(color_)) {}

    int id;
    QString name;
    quint64 addr;
    int size;                      // bytes
    char fmt;                      // see Leaf
    QColor color;

    bool active = true;
    QString mode = QStringLiteral("None");   // post-processing mode, see Processing.h
    double param = 10.0;           // N samples or EMA alpha, depending on mode
    double gain = 1.0;
    double offset = 0.0;           // added after filter and gain

    std::deque<double> t, y;
    std::optional<Reading> last;
    EnumMap enumValues;            // empty unless the variable is an enum
    std::optional<BitSpec> bits;   // set when the variable is a bitfield
    bool typeOverridden = false;   // user picked the type by hand: keep it on ELF reload
    bool fixed = false;            // registers / raw addresses: not looked up in the ELF

    PlotWidget *plot = nullptr;    // graph that shows this series
    int curve = -1;                // curve handle inside `plot`

    bool enumActive() const { return !enumValues.isEmpty() && !isFloatFmt(fmt); }
    QString fmtEnum(double v) const;

    void append(double time, double value);
    void clearData();

    // raw -> filter/transform -> gain -> offset, cached until the data or the settings change
    const std::vector<double> &processed();
    std::vector<double> times() const { return {t.begin(), t.end()}; }
    std::vector<double> raw() const { return {y.begin(), y.end()}; }

private:
    quint64 rev_ = 1;
    struct Cache {
        quint64 rev = 0;
        QString mode;
        double param = 0, gain = 0, offset = 0;
        std::vector<double> data;
    } cache_;
};
