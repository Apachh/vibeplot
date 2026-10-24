// Background thread that talks to OpenOCD's Tcl port: samples the plotted variables, performs
// queued writes and answers register read requests.
#pragma once

#include "Types.h"

#include <QByteArray>
#include <QHash>
#include <QMutex>
#include <QThread>
#include <QVector>
#include <QWaitCondition>

#include <atomic>
#include <functional>
#include <queue>
#include <variant>

class QTcpSocket;

struct ReadSpec {
    int id;
    quint64 addr;
    int size;
    char fmt;
    std::optional<BitSpec> bits;
};

struct SampleValue {
    int id;
    Reading r;
};
using Samples = QVector<SampleValue>;

struct RegResult {
    bool ok = false;
    quint64 value = 0;
};
using RegResults = QHash<quint64, RegResult>;      // by address
using RegSpecs = std::vector<std::pair<quint64, int>>;   // (address, size in bytes)

Q_DECLARE_METATYPE(Samples)
Q_DECLARE_METATYPE(RegResults)

// Pure helpers of the OpenOCD protocol (exposed for the tests).
namespace ocd {
int pickWidth(quint64 addr, int size);                           // widest aligned access in bytes
quint64 extractBits(const QByteArray &raw, const BitSpec &bits);  // bitfield value as two's complement
QString buildWriteCmd(quint64 addr, const QByteArray &raw);      // mwb / mwh / mww commands
Reading decode(const QByteArray &raw, char fmt, const std::optional<BitSpec> &bits);
}  // namespace ocd

class Poller : public QThread {
    Q_OBJECT
public:
    Poller(QString host, quint16 port, std::function<std::vector<ReadSpec>()> getSpecs,
           std::function<double()> getPeriod);

    // Queue a memory write; it runs in the polling thread between reads.
    void requestWrite(const QString &name, quint64 addr, const QByteArray &raw);
    // Queue a read-modify-write of the `mask` bits (read and write happen back to back).
    void requestRmw(const QString &name, quint64 addr, int size, quint64 mask, quint64 value);
    // Queue a tolerant read of (address, size) pairs; the result arrives via regsReady.
    void requestRead(quint64 token, const RegSpecs &specs);
    void stop();

    double wall0() const { return wall0_.load(); }   // unix time of the first sample

signals:
    void sample(double t, Samples values);
    void status(QString message);
    void regsReady(quint64 token, RegResults results);

protected:
    void run() override;

private:
    struct WriteOp {
        QString name;
        quint64 addr;
        QByteArray raw;
        bool rmw;
        int size;
        quint64 mask, value;
    };
    struct ReadOp {
        quint64 token;
        RegSpecs specs;
    };

    QString tcl(const QString &cmd);
    quint64 readInt(quint64 addr, int size);
    RegResults readTolerant(const RegSpecs &specs);
    Samples readValues(const std::vector<ReadSpec> &specs);
    void doWrites();
    void doReads();
    void wake();
    void sleepFor(double seconds);

    QString host_;
    quint16 port_;
    std::function<std::vector<ReadSpec>()> getSpecs_;
    std::function<double()> getPeriod_;

    std::atomic<bool> stop_{false};
    std::atomic<double> wall0_{0.0};
    QTcpSocket *sock_ = nullptr;     // lives in the polling thread

    QMutex mu_;
    QWaitCondition cond_;
    bool woken_ = false;
    std::queue<WriteOp> writes_;
    std::queue<ReadOp> reads_;
};
