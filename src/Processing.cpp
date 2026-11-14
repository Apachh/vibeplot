#include "Processing.h"

#include <algorithm>
#include <cmath>

namespace proc {

const QStringList &modes()
{
    static const QStringList m = {"None", "Moving average (N)", "Moving RMS (N)", "Median (N)",
                                  QString::fromUtf8("Low-pass EMA (α)"), "Derivative d/dt",
                                  QString::fromUtf8("Integral ∫dt"), "Absolute value", "Remove mean"};
    return m;
}

// Trailing moving average; the first samples average what is available.
std::vector<double> movingMean(const std::vector<double> &y, int n)
{
    const size_t len = y.size();
    std::vector<double> c(len + 1, 0.0), out(len);
    for (size_t i = 0; i < len; ++i) c[i + 1] = c[i] + y[i];
    for (size_t i = 1; i <= len; ++i) {
        const size_t lo = i > static_cast<size_t>(n) ? i - n : 0;
        out[i - 1] = (c[i] - c[lo]) / static_cast<double>(i - lo);
    }
    return out;
}

// Exponential moving average starting from the first sample.
std::vector<double> ema(const std::vector<double> &y, double a)
{
    std::vector<double> out(y.size());
    if (y.empty()) return out;
    const double b = 1.0 - a;
    out[0] = y[0];
    for (size_t i = 1; i < y.size(); ++i) out[i] = a * y[i] + b * out[i - 1];
    return out;
}

static std::vector<double> movingMedian(const std::vector<double> &y, int n)
{
    const size_t len = y.size();
    std::vector<double> out(len), win(static_cast<size_t>(n));
    for (size_t i = 0; i < len; ++i) {
        for (int k = 0; k < n; ++k) {                // window of n samples ending at i, padded with y[0]
            const long idx = static_cast<long>(i) - (n - 1) + k;
            win[static_cast<size_t>(k)] = idx < 0 ? y[0] : y[static_cast<size_t>(idx)];
        }
        const size_t mid = static_cast<size_t>(n) / 2;
        std::nth_element(win.begin(), win.begin() + mid, win.end());
        double m = win[mid];
        if (n % 2 == 0) {
            const double lower = *std::max_element(win.begin(), win.begin() + mid);
            m = (m + lower) / 2.0;
        }
        out[i] = m;
    }
    return out;
}

// Same as numpy.gradient(y, ts): central differences on a non-uniform grid, one-sided at the ends.
static std::vector<double> gradient(const std::vector<double> &y, const std::vector<double> &t)
{
    const size_t n = y.size();
    std::vector<double> g(n, 0.0);
    if (n < 2) return g;
    g[0] = (y[1] - y[0]) / (t[1] - t[0]);
    g[n - 1] = (y[n - 1] - y[n - 2]) / (t[n - 1] - t[n - 2]);
    for (size_t i = 1; i + 1 < n; ++i) {
        const double hs = t[i] - t[i - 1], hd = t[i + 1] - t[i];
        g[i] = (hs * hs * y[i + 1] + (hd * hd - hs * hs) * y[i] - hd * hd * y[i - 1]) / (hs * hd * (hd + hs));
    }
    return g;
}

std::vector<double> applyMode(const QString &mode, double p, const std::vector<double> &ts, const std::vector<double> &y)
{
    const size_t n = y.size();
    if (n == 0 || mode == "None") return y;
    if (mode.startsWith("Moving average")) return movingMean(y, std::max(1, static_cast<int>(p)));
    if (mode.startsWith("Moving RMS")) {
        std::vector<double> sq(n);
        for (size_t i = 0; i < n; ++i) sq[i] = y[i] * y[i];
        auto m = movingMean(sq, std::max(1, static_cast<int>(p)));
        for (double &v : m) v = std::sqrt(v);
        return m;
    }
    if (mode.startsWith("Median")) {
        const int w = std::min(std::max(1, static_cast<int>(p)), 101);
        return w == 1 ? y : movingMedian(y, w);
    }
    if (mode.startsWith("Low-pass")) return ema(y, std::min(std::max(p, 0.001), 1.0));
    if (mode.startsWith("Derivative")) return gradient(y, ts);
    if (mode.startsWith("Integral")) {
        std::vector<double> out(n, 0.0);
        for (size_t i = 1; i < n; ++i) out[i] = out[i - 1] + (y[i] + y[i - 1]) * 0.5 * (ts[i] - ts[i - 1]);
        return out;
    }
    if (mode == "Absolute value") {
        std::vector<double> out(y);
        for (double &v : out) v = std::fabs(v);
        return out;
    }
    if (mode == "Remove mean") {
        double mean = 0;
        for (double v : y) mean += v;
        mean /= static_cast<double>(n);
        std::vector<double> out(y);
        for (double &v : out) v -= mean;
        return out;
    }
    return y;
}

std::vector<double> process(const QString &mode, double param, double gain, double offset,
                            const std::vector<double> &ts, const std::vector<double> &y)
{
    std::vector<double> out = applyMode(mode, param, ts, y);
    if (gain != 1.0 || offset != 0.0)
        for (double &v : out) v = v * gain + offset;
    return out;
}

}  // namespace proc
