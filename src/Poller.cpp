#include "Poller.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <QTcpSocket>

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace ocd {

int pickWidth(quint64 addr, int size)
{
    for (int w : {4, 2, 1})
        if (size % w == 0 && addr % w == 0) return w;
    return 1;
}

quint64 extractBits(const QByteArray &raw, const BitSpec &bits)
{
    quint64 v = 0;
    for (int i = std::min<int>(raw.size(), 8) - 1; i >= 0; --i) v = (v << 8) | static_cast<quint8>(raw[i]);
    const quint64 mask = bits.width >= 64 ? ~0ull : (1ull << bits.width) - 1;
    v = (v >> bits.shift) & mask;
    if (bits.isSigned && bits.width < 64 && (v >> (bits.width - 1)))
        v |= ~mask;                                  // sign-extend
    return v;
}

QString buildWriteCmd(quint64 addr, const QByteArray &raw)
{
    const int size = raw.size();
    if (size < 1) throw std::runtime_error("nothing to write");
    const int w = pickWidth(addr, size);
    const char *cmd = w == 4 ? "mww" : w == 2 ? "mwh" : "mwb";
    QStringList parts;
    for (int i = 0; i < size; i += w) {
        quint64 word = 0;
        for (int k = w - 1; k >= 0; --k) word = (word << 8) | static_cast<quint8>(raw[i + k]);
        parts << QStringLiteral("%1 0x%2 0x%3").arg(cmd).arg(addr + static_cast<quint64>(i), 0, 16).arg(word, 0, 16);
    }
    return parts.join("; ");
}

Reading decode(const QByteArray &raw, char fmt, const std::optional<BitSpec> &bits)
{
    Reading r;
    if (bits) {
        const quint64 v = extractBits(raw, *bits);
        r.raw = v;
        r.v = bits->isSigned ? static_cast<double>(static_cast<qint64>(v)) : static_cast<double>(v);
        return r;
    }
    quint64 u = 0;
    for (int i = raw.size() - 1; i >= 0; --i) u = (u << 8) | static_cast<quint8>(raw[i]);
    switch (fmt) {
    case 'f': { float f; const quint32 b = static_cast<quint32>(u); std::memcpy(&f, &b, 4); r.v = f; r.raw = b; break; }
    case 'd': { double d; std::memcpy(&d, &u, 8); r.v = d; r.raw = u; break; }
    case 'b': r.raw = static_cast<quint64>(static_cast<qint64>(static_cast<qint8>(u))); r.v = static_cast<qint8>(u); break;
    case 'h': r.raw = static_cast<quint64>(static_cast<qint64>(static_cast<qint16>(u))); r.v = static_cast<qint16>(u); break;
    case 'i': r.raw = static_cast<quint64>(static_cast<qint64>(static_cast<qint32>(u))); r.v = static_cast<qint32>(u); break;
    case 'q': r.raw = u; r.v = static_cast<double>(static_cast<qint64>(u)); break;
    default: r.raw = u; r.v = static_cast<double>(u); break;
    }
    return r;
}

// "0x01 0x02 ..." -> little-endian bytes of `w`-byte words; empty optional on a malformed reply
static std::optional<QByteArray> wordsToBytes(const QString &text, int w, int count)
{
    const QStringList words = text.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    if (words.size() != count) return std::nullopt;
    QByteArray raw;
    for (const QString &x : words) {
        bool ok = false;
        const quint64 v = x.toULongLong(&ok, 0);
        if (!ok) return std::nullopt;
        for (int k = 0; k < w; ++k) raw.append(static_cast<char>((v >> (8 * k)) & 0xFF));
    }
    return raw;
}

}  // namespace ocd

Poller::Poller(QString host, quint16 port, std::function<std::vector<ReadSpec>()> getSpecs,
               std::function<double()> getPeriod)
    : host_(std::move(host)), port_(port), getSpecs_(std::move(getSpecs)), getPeriod_(std::move(getPeriod))
{
    qRegisterMetaType<Samples>();
    qRegisterMetaType<RegResults>();
}

void Poller::wake()
{
    QMutexLocker lk(&mu_);
    woken_ = true;
    cond_.wakeAll();
}

void Poller::sleepFor(double seconds)
{
    QMutexLocker lk(&mu_);
    if (!woken_) cond_.wait(&mu_, static_cast<unsigned long>(seconds * 1000.0) + 1);
    woken_ = false;
}

void Poller::requestWrite(const QString &name, quint64 addr, const QByteArray &raw)
{
    {
        QMutexLocker lk(&mu_);
        writes_.push({name, addr, raw, false, 0, 0, 0});
    }
    wake();
}

void Poller::requestRmw(const QString &name, quint64 addr, int size, quint64 mask, quint64 value)
{
    {
        QMutexLocker lk(&mu_);
        writes_.push({name, addr, {}, true, size, mask, value});
    }
    wake();
}

void Poller::requestRead(quint64 token, const RegSpecs &specs)
{
    {
        QMutexLocker lk(&mu_);
        reads_.push({token, specs});
    }
    wake();
}

void Poller::stop()
{
    stop_ = true;
    wake();
}

QString Poller::tcl(const QString &cmd)
{
    QByteArray msg = cmd.toUtf8();
    msg.append('\x1a');
    sock_->write(msg);
    if (!sock_->waitForBytesWritten(3000)) throw std::runtime_error("send timed out");
    QByteArray buf;
    QElapsedTimer waited;
    waited.start();
    for (;;) {
        if (stop_) throw std::runtime_error("stopped");
        sock_->waitForReadyRead(100);
        const QByteArray chunk = sock_->readAll();
        if (!chunk.isEmpty()) {
            buf += chunk;
            waited.restart();
        }
        if (buf.endsWith('\x1a')) break;
        if (sock_->state() != QAbstractSocket::ConnectedState && sock_->bytesAvailable() == 0)
            throw std::runtime_error("OpenOCD closed the connection");
        if (waited.elapsed() > 3000) throw std::runtime_error("timed out waiting for OpenOCD");
    }
    buf.chop(1);
    return QString::fromUtf8(buf).trimmed();
}

quint64 Poller::readInt(quint64 addr, int size)
{
    const int w = ocd::pickWidth(addr, size);
    const QString out = tcl(QStringLiteral("read_memory 0x%1 %2 %3").arg(addr, 0, 16).arg(w * 8).arg(size / w));
    auto raw = ocd::wordsToBytes(out, w, size / w);
    if (!raw) throw std::runtime_error(out.isEmpty() ? "empty reply" : out.left(150).toStdString());
    quint64 v = 0;
    for (int i = raw->size() - 1; i >= 0; --i) v = (v << 8) | static_cast<quint8>((*raw)[i]);
    return v;
}

RegResults Poller::readTolerant(const RegSpecs &specs)
{
    RegResults res;
    for (size_t i = 0; i < specs.size(); i += 100) {
        const size_t n = std::min<size_t>(100, specs.size() - i);
        QStringList parts;
        for (size_t k = 0; k < n; ++k) {
            const auto [addr, size] = specs[i + k];
            const int w = ocd::pickWidth(addr, size);
            parts << QStringLiteral("if {[catch {read_memory 0x%1 %2 %3} r]} {append o ERR \";\"} else {append o $r \";\"}")
                         .arg(addr, 0, 16).arg(w * 8).arg(size / w);
        }
        const QString out = tcl("set o \"\"; " + parts.join("; ") + "; set o");
        const QStringList chunks = out.split(';');
        for (size_t k = 0; k < n; ++k) {
            const auto [addr, size] = specs[i + k];
            const int w = ocd::pickWidth(addr, size);
            RegResult rr;
            if (static_cast<int>(k) < chunks.size()) {
                if (auto raw = ocd::wordsToBytes(chunks[static_cast<int>(k)], w, size / w)) {
                    rr.ok = true;
                    for (int b = std::min<int>(raw->size(), 8) - 1; b >= 0; --b) rr.value = (rr.value << 8) | static_cast<quint8>((*raw)[b]);
                }
            }
            res.insert(addr, rr);
        }
    }
    return res;
}

Samples Poller::readValues(const std::vector<ReadSpec> &specs)
{
    QStringList parts;
    for (const ReadSpec &s : specs) {
        const int w = ocd::pickWidth(s.addr, s.size);
        parts << QStringLiteral("append o [read_memory 0x%1 %2 %3] \";\"").arg(s.addr, 0, 16).arg(w * 8).arg(s.size / w);
    }
    const QString out = tcl("set o \"\"; " + parts.join("; ") + "; set o");
    const QStringList chunks = out.split(';');
    Samples vals;
    for (size_t i = 0; i < specs.size(); ++i) {
        const ReadSpec &s = specs[i];
        if (static_cast<int>(i) >= chunks.size()) break;
        const int w = ocd::pickWidth(s.addr, s.size);
        auto raw = ocd::wordsToBytes(chunks[static_cast<int>(i)], w, s.size / w);
        if (!raw) throw std::runtime_error(out.isEmpty() ? "empty reply" : out.left(200).toStdString());
        vals.push_back({s.id, ocd::decode(*raw, s.fmt, s.bits)});
    }
    return vals;
}

void Poller::doReads()
{
    for (;;) {
        ReadOp op;
        {
            QMutexLocker lk(&mu_);
            if (reads_.empty()) return;
            op = std::move(reads_.front());
            reads_.pop();
        }
        emit regsReady(op.token, readTolerant(op.specs));
    }
}

void Poller::doWrites()
{
    for (;;) {
        WriteOp op;
        {
            QMutexLocker lk(&mu_);
            if (writes_.empty()) return;
            op = std::move(writes_.front());
            writes_.pop();
        }
        QString out;
        try {
            QByteArray raw = op.raw;
            if (op.rmw) {
                const quint64 cur = readInt(op.addr, op.size);
                const quint64 all = op.size >= 8 ? ~0ull : (1ull << (8 * op.size)) - 1;
                const quint64 nv = ((cur & ~op.mask) | (op.value & op.mask)) & all;
                raw.clear();
                for (int i = 0; i < op.size; ++i) raw.append(static_cast<char>((nv >> (8 * i)) & 0xFF));
            }
            out = tcl(ocd::buildWriteCmd(op.addr, raw));
        } catch (const std::runtime_error &e) {
            if (stop_) return;
            emit status(QStringLiteral("Write %1 failed: %2").arg(op.name, e.what()));
            continue;
        }
        if (!out.isEmpty()) emit status(QStringLiteral("Write %1 failed: %2").arg(op.name, out.left(150)));
        else emit status(QStringLiteral("Wrote %1 @ 0x%2").arg(op.name, QString::number(op.addr, 16).rightJustified(8, '0').toUpper()));
    }
}

void Poller::run()
{
    QTcpSocket sock;
    sock_ = &sock;
    sock.connectToHost(host_, port_);
    if (!sock.waitForConnected(2000)) {
        emit status(QStringLiteral("Cannot connect to %1:%2: %3").arg(host_).arg(port_).arg(sock.errorString()));
        sock_ = nullptr;
        return;
    }
    sock.setSocketOption(QAbstractSocket::LowDelayOption, 1);
    emit status(QStringLiteral("Connected to OpenOCD at %1:%2").arg(host_).arg(port_));

    QElapsedTimer clock;
    clock.start();
    wall0_ = QDateTime::currentMSecsSinceEpoch() / 1000.0;
    try {
        while (!stop_) {
            const double tStart = clock.nsecsElapsed() * 1e-9;
            doWrites();
            doReads();
            const auto specs = getSpecs_();
            if (!specs.empty()) {
                Samples vals = readValues(specs);
                emit sample(clock.nsecsElapsed() * 1e-9, vals);
            }
            const double rest = getPeriod_() - (clock.nsecsElapsed() * 1e-9 - tStart);
            if (rest > 0) sleepFor(rest);
            else { QMutexLocker lk(&mu_); woken_ = false; }
        }
    } catch (const std::exception &e) {
        if (!stop_) emit status(QStringLiteral("Read error: %1").arg(e.what()));
    }
    sock.abort();
    sock_ = nullptr;
}
