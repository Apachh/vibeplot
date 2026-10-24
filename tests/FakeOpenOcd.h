// A fake OpenOCD Tcl server that keeps a small memory image: lets the tests run without hardware.
#pragma once

#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>

#include <map>
#include <optional>
#include <set>

class FakeOpenOcd : public QTcpServer {
public:
    explicit FakeOpenOcd(QObject *parent = nullptr) : QTcpServer(parent)
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            QTcpSocket *s = nextPendingConnection();
            connect(s, &QTcpSocket::readyRead, this, [this, s] { onData(s); });
        });
        listen(QHostAddress::LocalHost, 0);
    }

    std::map<quint64, quint8> mem;
    std::set<quint64> failing;                  // addresses that make read_memory fail
    QStringList commands;

    void set32(quint64 addr, quint32 v)
    {
        for (int i = 0; i < 4; ++i) mem[addr + i] = (v >> (8 * i)) & 0xFF;
    }
    quint32 get32(quint64 addr) const
    {
        quint32 v = 0;
        for (int i = 3; i >= 0; --i) {
            auto it = mem.find(addr + i);
            v = (v << 8) | (it == mem.end() ? 0 : it->second);
        }
        return v;
    }

private:
    QByteArray bufs;

    // "0x12 0x34" words of `width` bits starting at addr, or an empty optional if the access fails
    std::optional<QString> readWords(quint64 addr, int width, int count) const
    {
        if (failing.count(addr)) return std::nullopt;
        QStringList words;
        const int bytes = width / 8;
        for (int i = 0; i < count; ++i) {
            quint64 v = 0;
            for (int b = bytes - 1; b >= 0; --b) {
                auto it = mem.find(addr + static_cast<quint64>(i * bytes + b));
                v = (v << 8) | (it == mem.end() ? 0 : it->second);
            }
            words << QStringLiteral("0x%1").arg(v, bytes * 2, 16, QLatin1Char('0'));
        }
        return words.join(' ');
    }

    QString handle(const QString &cmd)
    {
        static const QRegularExpression readRe(R"(read_memory 0x([0-9a-f]+) (\d+) (\d+))");
        static const QRegularExpression writeRe(R"(\b(mww|mwh|mwb) 0x([0-9a-f]+) 0x([0-9a-f]+))");
        QString out;
        if (cmd.startsWith("set o")) {
            const bool tolerant = cmd.contains("catch");
            for (auto m = readRe.globalMatch(cmd); m.hasNext();) {
                const auto x = m.next();
                const auto words = readWords(x.captured(1).toULongLong(nullptr, 16), x.captured(2).toInt(), x.captured(3).toInt());
                if (words) out += *words + ";";
                else if (tolerant) out += "ERR;";
                else return "read failed";
            }
            return out;
        }
        if (cmd.startsWith("read_memory")) {
            const auto x = readRe.match(cmd);
            return readWords(x.captured(1).toULongLong(nullptr, 16), x.captured(2).toInt(), x.captured(3).toInt()).value_or(QString());
        }
        for (auto m = writeRe.globalMatch(cmd); m.hasNext();) {
            const auto x = m.next();
            const quint64 addr = x.captured(2).toULongLong(nullptr, 16), val = x.captured(3).toULongLong(nullptr, 16);
            const int n = x.captured(1) == "mww" ? 4 : x.captured(1) == "mwh" ? 2 : 1;
            for (int i = 0; i < n; ++i) mem[addr + static_cast<quint64>(i)] = (val >> (8 * i)) & 0xFF;
        }
        return {};
    }

    void onData(QTcpSocket *s)
    {
        bufs += s->readAll();
        int end;
        while ((end = bufs.indexOf('\x1a')) >= 0) {
            const QString cmd = QString::fromUtf8(bufs.left(end));
            bufs.remove(0, end + 1);
            commands << cmd;
            s->write(handle(cmd).toUtf8() + '\x1a');
        }
    }
};
