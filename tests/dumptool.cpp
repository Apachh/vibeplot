// Developer tool: prints what the ELF / SVD parsers produce, in a stable text format.
//   dumptool elf firmware.elf     variables and their members (first levels)
//   dumptool svd device.svd       peripherals, registers and fields
#include "ElfModel.h"
#include "SvdRegs.h"

#include <QCoreApplication>
#include <QTextStream>

static QTextStream out(stdout);

static QString enumText(const EnumMap &m)
{
    if (m.isEmpty()) return "-";
    QStringList l;
    for (auto it = m.begin(); it != m.end(); ++it) l << QStringLiteral("%1=%2").arg(it.key()).arg(it.value());
    return l.join(",");
}

static void dumpNode(Node &n, int depth, int maxDepth)
{
    QString leaf = n.leaf ? QStringLiteral("%1%2").arg(n.leaf->size).arg(QChar(n.leaf->fmt)) : "-";
    QString bits = n.bits ? QStringLiteral("%1/%2/%3").arg(n.bits->shift).arg(n.bits->width).arg(n.bits->isSigned ? 1 : 0) : "-";
    QString rel = n.rel ? QString::number(*n.rel) : "-";
    out << QString(depth * 2, ' ') << n.label << "|" << n.path << "|" << n.addr << "|" << n.size << "|" << n.typeName << "|"
        << leaf << "|" << bits << "|" << rel << "|" << enumText(n.enumMap) << "\n";
    if (depth >= maxDepth || !n.expandable()) return;
    int shown = 0;
    for (const auto &k : n.children()) {
        dumpNode(*k, depth + 1, maxDepth);
        if (++shown >= 16) break;
    }
}

static void dumpPeripheral(const svd::Peripheral &p)
{
    out << "P|" << p.name << "|" << p.base << "|" << p.desc << "|" << p.regs.size() << "\n";
    for (const svd::Register &r : p.regs) {
        out << " R|" << r.name << "|" << r.addr << "|" << r.size << "|" << r.access << "|" << (r.sideEffect ? 1 : 0) << "|" << r.fields->size() << "|" << r.desc << "\n";
        for (const svd::Field &f : *r.fields)
            out << "  F|" << f.name << "|" << f.off << "|" << f.width << "|" << (f.sideEffect ? 1 : 0) << "|" << enumText(f.enumValues) << "\n";
    }
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 3) return 2;
    const QString mode = argv[1], path = argv[2];
    try {
        if (mode == "elf") {
            ElfModel m(path);
            out << "debug=" << (m.hasDebug ? 1 : 0) << " roots=" << m.roots.size() << "\n";
            for (const auto &r : m.roots) dumpNode(*r, 0, 3);
        } else {
            auto res = svd::loadSvd(path);
            out << "device=" << res.info.device << " cpu=" << res.info.cpu << " n=" << res.peripherals.size() << "\n";
            for (const auto &p : res.peripherals) dumpPeripheral(*p);
            for (const auto &p : svd::corePeripherals()) dumpPeripheral(*p);
        }
    } catch (const std::exception &e) {
        out << "ERROR " << e.what() << "\n";
        return 1;
    }
    return 0;
}
