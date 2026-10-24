// Post-processing of recorded samples (moving average, median, EMA, derivative, ...).
#pragma once

#include <QString>
#include <QStringList>

#include <vector>

namespace proc {

const QStringList &modes();

std::vector<double> movingMean(const std::vector<double> &y, int n);
std::vector<double> ema(const std::vector<double> &y, double alpha);
std::vector<double> applyMode(const QString &mode, double param, const std::vector<double> &ts,
                              const std::vector<double> &y);

// raw -> filter/transform -> gain -> offset
std::vector<double> process(const QString &mode, double param, double gain, double offset,
                            const std::vector<double> &ts, const std::vector<double> &y);

}  // namespace proc
