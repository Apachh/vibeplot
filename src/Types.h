// Shared constants and small value types used across the application.
#pragma once

#include <QColor>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <optional>
#include <utility>
#include <vector>

constexpr int kMaxPoints = 50000;   // samples kept per variable

// {value -> name} of an enumeration; empty means "not an enum".
using EnumMap = QMap<qint64, QString>;

// Bitfield location inside the bytes that cover it.
struct BitSpec {
    int shift = 0;
    int width = 0;
    bool isSigned = false;
};

// Plottable scalar: size in bytes + format char (B b H h I i Q q f d), like Python's struct module.
struct Leaf {
    int size = 0;
    char fmt = 'I';
};

// One value read from the target. `v` is for plotting, `raw` keeps 64-bit integers exact.
struct Reading {
    double v = 0.0;
    quint64 raw = 0;
};

inline bool isFloatFmt(char f) { return f == 'f' || f == 'd'; }
inline bool isSignedFmt(char f) { return f == 'b' || f == 'h' || f == 'i' || f == 'q'; }

inline char unsignedFmt(int size) {
    switch (size) { case 1: return 'B'; case 2: return 'H'; case 4: return 'I'; case 8: return 'Q'; }
    return 0;
}
inline char signedFmt(int size) {
    switch (size) { case 1: return 'b'; case 2: return 'h'; case 4: return 'i'; case 8: return 'q'; }
    return 0;
}

// Variable types offered in the table (name -> size, fmt).
struct TypeInfo {
    const char *name;
    int size;
    char fmt;
};
inline const std::vector<TypeInfo> &typeTable() {
    static const std::vector<TypeInfo> t = {
        {"uint8", 1, 'B'},  {"int8", 1, 'b'},  {"uint16", 2, 'H'},  {"int16", 2, 'h'},
        {"uint32", 4, 'I'}, {"int32", 4, 'i'}, {"float32", 4, 'f'}, {"uint64", 8, 'Q'},
        {"int64", 8, 'q'},  {"float64", 8, 'd'},
    };
    return t;
}
inline QString typeNameFor(int size, char fmt) {
    for (const auto &t : typeTable())
        if (t.size == size && t.fmt == fmt) return t.name;
    return QStringLiteral("uint32");
}
inline const char *typeNameOrQuestion(int size, char fmt) {
    for (const auto &t : typeTable())
        if (t.size == size && t.fmt == fmt) return t.name;
    return "?";
}

inline const QStringList &plotColors() {
    static const QStringList c = {"#ff5555", "#55aaff", "#55dd77", "#ffaa33", "#cc77ff",
                                  "#ffee55", "#33dddd", "#ff77bb", "#aaaaaa", "#99cc33"};
    return c;
}
