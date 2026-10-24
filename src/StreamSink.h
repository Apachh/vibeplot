// Destination of the live data stream: a file, a serial port or a self-created virtual COM port (pty).
// A background thread does the writing, so a slow or absent reader never blocks the GUI.
#pragma once

#include "StreamFormat.h"

#include <QString>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

class StreamSink {
public:
    enum Target { VirtualCom = 0, SerialPort = 1, File = 2 };

    StreamSink(Target target, QString path, int baud = 115200, bool append = false);
    ~StreamSink();
    StreamSink(const StreamSink &) = delete;
    StreamSink &operator=(const StreamSink &) = delete;

    // Throws std::runtime_error with a user-readable message.
    void open();
    void start();
    void push(const QString &text);
    void close();

    QString describe() const;
    QString error() const;
    QString device() const { return dev_; }
    qint64 written() const { return written_; }
    qint64 dropped() const { return dropped_; }

private:
    void run();
    bool writeChunk(const QByteArray &data);   // false: stop the writer thread

    Target target_;
    QString path_;
    int baud_;
    bool append_;
    QString dev_;                              // slave side of the virtual COM port

    int fd_ = -1;                              // file / serial descriptor, or pty master
    int slave_ = -1;                           // pty slave, kept open so writes never see EIO
    bool link_ = false;

    std::thread thread_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<QByteArray> queue_;
    bool closing_ = false;
    QString error_;
    std::atomic<qint64> written_{0};
    std::atomic<qint64> dropped_{0};
};

struct StreamConfig {
    StreamSink::Target target = StreamSink::VirtualCom;
    QString path;
    int baud = 115200;
    bool append = false;
    stream::Format format = stream::Csv;
    bool scaled = false;               // raw x gain + offset instead of raw memory values
    stream::TimeBase time = stream::SinceConnect;
    int every = 1;                     // send every n-th sample
};
