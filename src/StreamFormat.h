// Text formats of the live data stream (CSV / Teleplot / JSON lines).
#pragma once

#include "Types.h"

#include <QString>

#include <vector>

namespace stream {

enum Format { Csv = 0, Teleplot = 1, JsonLines = 2 };
enum TimeBase { SinceConnect = 0, UnixTime = 1, NoTime = 2 };

struct Item {
    QString name;
    QString text;      // number as text
    double number;     // number as double (JSON output)
};

// `ts` is only used when `hasTs`; `isUnix` selects the key / header name for the timestamp.
QString formatSample(Format kind, const std::vector<Item> &items, bool hasTs, double ts, bool isUnix, bool newHeader);

// Number as text: shortest round-trip form for floats, exact integers otherwise.
QString numText(char fmt, double v, quint64 raw, bool hasBits, bool scaled);

}  // namespace stream
