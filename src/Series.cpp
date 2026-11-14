#include "Series.h"

#include "Processing.h"

#include <cmath>

QString Series::fmtEnum(double v) const
{
    const qint64 iv = static_cast<qint64>(std::llround(v));
    auto it = enumValues.constFind(iv);
    if (it == enumValues.constEnd() && size < 8)    // signed enum stored with an unsigned DWARF form
        it = enumValues.constFind(iv & ((qint64(1) << (8 * size)) - 1));
    return it != enumValues.constEnd() ? QStringLiteral("%1 (%2)").arg(it.value()).arg(iv) : QString::number(iv);
}

void Series::append(double time, double value)
{
    t.push_back(time);
    y.push_back(value);
    if (t.size() > static_cast<size_t>(kMaxPoints)) {
        t.pop_front();
        y.pop_front();
    }
    ++rev_;
}

void Series::clearData()
{
    t.clear();
    y.clear();
    last.reset();
    ++rev_;
}

const std::vector<double> &Series::processed()
{
    if (cache_.rev != rev_ || cache_.mode != mode || cache_.param != param || cache_.gain != gain ||
        cache_.offset != offset) {
        cache_.data = proc::process(mode, param, gain, offset, times(), raw());
        cache_.rev = rev_;
        cache_.mode = mode;
        cache_.param = param;
        cache_.gain = gain;
        cache_.offset = offset;
    }
    return cache_.data;
}
