#include "StreamSink.h"

#include <QFileInfo>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#define SWDPLOT_POSIX 1
#endif

namespace {
constexpr size_t kMaxQueued = 5000;

[[noreturn]] void fail(const QString &msg) { throw std::runtime_error(msg.toStdString()); }
QString errnoText() { return QString::fromLocal8Bit(std::strerror(errno)); }

#ifdef SWDPLOT_POSIX
speed_t baudConstant(int baud)
{
    switch (baud) {
    case 300: return B300;       case 600: return B600;       case 1200: return B1200;
    case 2400: return B2400;     case 4800: return B4800;     case 9600: return B9600;
    case 19200: return B19200;   case 38400: return B38400;   case 57600: return B57600;
    case 115200: return B115200; case 230400: return B230400;
#ifdef B460800
    case 460800: return B460800;
#endif
#ifdef B921600
    case 921600: return B921600;
#endif
#ifdef B1000000
    case 1000000: return B1000000;
#endif
#ifdef B2000000
    case 2000000: return B2000000;
#endif
#ifdef B3000000
    case 3000000: return B3000000;
#endif
#ifdef B4000000
    case 4000000: return B4000000;
#endif
    default: return B0;
    }
}
#endif
}  // namespace

StreamSink::StreamSink(Target target, QString path, int baud, bool append)
    : target_(target), path_(std::move(path)), baud_(baud), append_(append) {}

StreamSink::~StreamSink() { close(); }

void StreamSink::open()
{
#ifndef SWDPLOT_POSIX
    fail("Streaming is implemented for Linux/macOS only.");
#else
    if (target_ == File) {
        fd_ = ::open(path_.toLocal8Bit().constData(), O_WRONLY | O_CREAT | (append_ ? O_APPEND : O_TRUNC), 0644);
        if (fd_ < 0) fail(QStringLiteral("Cannot open %1: %2").arg(path_, errnoText()));
    } else if (target_ == VirtualCom) {
        const QFileInfo fi(path_);
        if (fi.exists() || fi.isSymLink()) {
            if (!fi.isSymLink()) fail(path_ + " exists and is not a symlink");
            ::unlink(path_.toLocal8Bit().constData());
        }
        fd_ = ::posix_openpt(O_RDWR | O_NOCTTY);
        if (fd_ < 0) fail("Cannot create a pty: " + errnoText());
        if (::grantpt(fd_) != 0 || ::unlockpt(fd_) != 0) fail("Cannot set up the pty: " + errnoText());
        const char *name = ::ptsname(fd_);
        if (!name) fail("Cannot get the pty name: " + errnoText());
        dev_ = QString::fromLocal8Bit(name);
        slave_ = ::open(name, O_RDWR | O_NOCTTY);
        if (slave_ < 0) fail("Cannot open the pty: " + errnoText());
        termios tio;
        if (::tcgetattr(slave_, &tio) == 0) {          // no echo / newline translation
            ::cfmakeraw(&tio);
            ::tcsetattr(slave_, TCSANOW, &tio);
        }
        ::fcntl(fd_, F_SETFL, ::fcntl(fd_, F_GETFL) | O_NONBLOCK);   // never block when nobody reads
        if (::symlink(name, path_.toLocal8Bit().constData()) != 0) fail("Cannot create " + path_ + ": " + errnoText());
        link_ = true;
    } else {
        fd_ = ::open(path_.toLocal8Bit().constData(), O_WRONLY | O_NOCTTY | O_NONBLOCK);   // a stalled port must not hang us
        if (fd_ < 0) fail(QStringLiteral("Cannot open %1: %2").arg(path_, errnoText()));
        if (::isatty(fd_)) {
            const speed_t sp = baudConstant(baud_);
            if (sp == B0) fail(QStringLiteral("Unsupported baud rate %1").arg(baud_));
            termios tio;
            if (::tcgetattr(fd_, &tio) == 0) {
                ::cfmakeraw(&tio);
                ::cfsetspeed(&tio, sp);
                tio.c_cflag |= CLOCAL | CREAD;
                ::tcsetattr(fd_, TCSANOW, &tio);
            }
        }
    }
#endif
}

void StreamSink::start() { thread_ = std::thread([this] { run(); }); }

void StreamSink::push(const QString &text)
{
    std::lock_guard<std::mutex> lk(mu_);
    if (queue_.size() >= kMaxQueued) {
        ++dropped_;
        return;
    }
    queue_.push_back(text.toUtf8());
    cv_.notify_one();
}

bool StreamSink::writeChunk(const QByteArray &data)
{
#ifdef SWDPLOT_POSIX
    const char *p = data.constData();
    size_t left = static_cast<size_t>(data.size());
    while (left) {
        const ssize_t n = ::write(fd_, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) {                      // reader not keeping up: drop the rest of this chunk
                ++dropped_;
                return true;
            }
            if (errno == EIO && target_ == VirtualCom) return true;
            std::lock_guard<std::mutex> lk(mu_);
            error_ = errnoText();
            return false;
        }
        p += n;
        left -= static_cast<size_t>(n);
    }
    written_ += data.size();
#else
    Q_UNUSED(data)
#endif
    return true;
}

void StreamSink::run()
{
    for (;;) {
        QByteArray data;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this] { return closing_ || !queue_.empty(); });
            if (queue_.empty()) return;                 // closing and drained
            data = std::move(queue_.front());
            queue_.pop_front();
        }
        if (!writeChunk(data)) return;
    }
}

QString StreamSink::describe() const
{
    const QString where = dev_.isEmpty() ? path_ : path_ + QString::fromUtf8(" → ") + dev_;
    return QString::fromUtf8("● ") + where +
           QStringLiteral(" — %1 bytes sent, %2 dropped").arg(written_.load()).arg(dropped_.load());
}

QString StreamSink::error() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return error_;
}

void StreamSink::close()
{
    {
        std::lock_guard<std::mutex> lk(mu_);
        closing_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
#ifdef SWDPLOT_POSIX
    if (fd_ >= 0) ::close(fd_);
    if (slave_ >= 0) ::close(slave_);
    fd_ = slave_ = -1;
    if (link_) {
        const QFileInfo fi(path_);
        if (fi.isSymLink() && fi.symLinkTarget() == dev_) ::unlink(path_.toLocal8Bit().constData());
        link_ = false;
    }
#endif
}
