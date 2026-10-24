// Turning text typed by the user into numbers / raw memory bytes.
#pragma once

#include "Types.h"

#include <QByteArray>

namespace valparse {

struct IntValue {
    bool neg = false;
    quint64 mag = 0;

    bool fitsUnsigned(int bits) const { return !neg && (bits >= 64 || mag < (1ull << bits)); }
    bool fitsSigned(int bits) const
    {
        const quint64 lim = 1ull << (bits - 1);
        return neg ? mag <= lim : mag < lim;
    }
    qint64 asInt64() const { return neg ? -static_cast<qint64>(mag) : static_cast<qint64>(mag); }
};

// Decimal, 0x hex, 0b binary, optional sign. Nothing else (no floats, no enum names).
std::optional<IntValue> parseInteger(const QString &text);

// Integer from text; also accepts an enum name or "NAME (3)" when `en` is given.
std::optional<IntValue> parseWithEnum(const QString &text, const EnumMap *en);

// Raw little-endian bytes for a variable of format `fmt`/`size`. On failure returns false and sets `err`.
bool packValue(const QString &text, char fmt, int size, const EnumMap *en, QByteArray &out, QString &err);

}  // namespace valparse
