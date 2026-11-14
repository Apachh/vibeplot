#include "StreamFormat.h"

#include <QJsonArray>
#include <QJsonDocument>

#include <cmath>

namespace stream {

static QString g(double v, int prec) { return QString::number(v, 'g', prec); }

QString numText(char fmt, double v, quint64 raw, bool hasBits, bool scaled)
{
    if (scaled) return g(v, 12);
    if (fmt == 'f') return g(v, 9);
    if (fmt == 'd') return g(v, 17);
    // integers: bitfields are already extracted (and sign-extended) into `raw`
    if (hasBits || isSignedFmt(fmt)) return QString::number(static_cast<qint64>(raw));
    return QString::number(raw);
}

static QString tpName(QString n)
{
    return n.replace("::", ".").replace(":", "_").replace(" ", "_");
}

static QString jsonString(const QString &s)
{
    const QByteArray arr = QJsonDocument(QJsonArray{s}).toJson(QJsonDocument::Compact);   // ["..."]
    return QString::fromUtf8(arr.mid(1, arr.size() - 2));
}

// seconds with at most 6 decimals and no trailing zeros
static QString trimmedFixed(double v)
{
    QString s = QString::number(v, 'f', 6);
    while (s.endsWith('0')) s.chop(1);
    if (s.endsWith('.')) s.chop(1);
    return s;
}

static QString csvField(const QString &n)
{
    if (n.contains(',') || n.contains('"') || n.contains('\n')) return '"' + QString(n).replace("\"", "\"\"") + '"';
    return n;
}

QString formatSample(Format kind, const std::vector<Item> &items, bool hasTs, double ts, bool isUnix, bool newHeader)
{
    if (kind == Csv) {
        QString out;
        if (newHeader) {
            QStringList head;
            if (hasTs) head << (isUnix ? "unix_time_s" : "time_s");
            for (const Item &i : items) head << csvField(i.name);
            out = head.join(',') + '\n';
        }
        QStringList row;
        if (hasTs) row << QString::number(ts, 'f', 6);
        for (const Item &i : items) row << i.text;
        return out + row.join(',') + '\n';
    }
    if (kind == Teleplot) {
        QString out;
        for (const Item &i : items) {
            const QString nm = tpName(i.name);
            if (hasTs) out += QStringLiteral(">%1:%2:%3\n").arg(nm).arg(static_cast<qint64>(ts * 1000)).arg(i.text);
            else out += QStringLiteral(">%1:%2\n").arg(nm, i.text);
        }
        return out;
    }
    // JSON lines: {"t":1.234,"values":{"a":1,"b":2.5}} with the keys in column order
    QString out = "{";
    if (hasTs) out += QStringLiteral("\"%1\":%2,").arg(isUnix ? "unix" : "t", trimmedFixed(ts));
    out += "\"values\":{";
    for (size_t k = 0; k < items.size(); ++k) {
        const Item &i = items[k];
        if (k) out += ',';
        out += jsonString(i.name) + ':' + (std::isfinite(i.number) ? i.text : QStringLiteral("null"));
    }
    out += "}}\n";
    return out;
}

}  // namespace stream
