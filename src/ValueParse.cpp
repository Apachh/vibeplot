#include "ValueParse.h"

#include <cfloat>
#include <cmath>
#include <cstring>

namespace valparse {

std::optional<IntValue> parseInteger(const QString &text)
{
    QString t = text.trimmed();
    IntValue v;
    if (t.startsWith('-')) {
        v.neg = true;
        t = t.mid(1).trimmed();
    } else if (t.startsWith('+')) {
        t = t.mid(1).trimmed();
    }
    if (t.isEmpty()) return std::nullopt;
    int base = 10;
    if (t.startsWith("0x") || t.startsWith("0X")) { base = 16; t = t.mid(2); }
    else if (t.startsWith("0b") || t.startsWith("0B")) { base = 2; t = t.mid(2); }
    t.remove('_');
    bool ok = false;
    v.mag = t.toULongLong(&ok, base);
    if (!ok) return std::nullopt;
    if (v.mag == 0) v.neg = false;
    return v;
}

std::optional<IntValue> parseWithEnum(const QString &text, const EnumMap *en)
{
    if (en && !en->isEmpty()) {
        const QString base = text.trimmed().section(" (", 0, 0).trimmed();
        for (auto it = en->begin(); it != en->end(); ++it) {
            if (it.value() == base) {
                IntValue v;
                v.neg = it.key() < 0;
                v.mag = v.neg ? static_cast<quint64>(-it.key()) : static_cast<quint64>(it.key());
                return v;
            }
        }
    }
    return parseInteger(text);
}

bool packValue(const QString &text, char fmt, int size, const EnumMap *en, QByteArray &out, QString &err)
{
    out.clear();
    if (isFloatFmt(fmt)) {
        bool ok = false;
        const double d = text.trimmed().toDouble(&ok);
        if (!ok) {
            err = QStringLiteral("could not convert string to float: '%1'").arg(text.trimmed());
            return false;
        }
        if (fmt == 'f') {
            if (std::isfinite(d) && std::fabs(d) > FLT_MAX) {
                err = "float too large to pack with 'f' format";
                return false;
            }
            const float f = static_cast<float>(d);
            out = QByteArray(reinterpret_cast<const char *>(&f), 4);
        } else {
            out = QByteArray(reinterpret_cast<const char *>(&d), 8);
        }
        return true;
    }
    auto v = parseWithEnum(text, en);
    if (!v) {
        err = QStringLiteral("invalid literal for integer: '%1'").arg(text.trimmed());
        return false;
    }
    const int bits = size * 8;
    const bool sgn = isSignedFmt(fmt);
    if (sgn ? !v->fitsSigned(bits) : !v->fitsUnsigned(bits)) {
        err = QStringLiteral("value does not fit in %1 (%2-bit %3)")
                  .arg(typeNameOrQuestion(size, fmt)).arg(bits).arg(sgn ? "signed" : "unsigned");
        return false;
    }
    const quint64 raw = v->neg ? static_cast<quint64>(v->asInt64()) : v->mag;
    for (int i = 0; i < size; ++i) out.append(static_cast<char>((raw >> (8 * i)) & 0xFF));
    return true;
}

}  // namespace valparse
