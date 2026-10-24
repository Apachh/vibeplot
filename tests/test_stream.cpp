#include "StreamFormat.h"
#include "StreamSink.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include <fcntl.h>
#include <unistd.h>

using namespace stream;

class TestStream : public QObject {
    Q_OBJECT
    const std::vector<Item> items{{"a", "1", 1}, {"ns::b,c", "2.5", 2.5}};

private slots:
    void csv()
    {
        QCOMPARE(formatSample(Csv, items, true, 1.5, false, true), QString("time_s,a,\"ns::b,c\"\n1.500000,1,2.5\n"));
        QCOMPARE(formatSample(Csv, items, true, 1.5, false, false), QString("1.500000,1,2.5\n"));
        QCOMPARE(formatSample(Csv, items, false, 0, false, true), QString("a,\"ns::b,c\"\n1,2.5\n"));
        QVERIFY(formatSample(Csv, items, true, 1700000000.25, true, true).startsWith("unix_time_s,"));
    }

    void teleplot()
    {
        QCOMPARE(formatSample(Teleplot, items, true, 1.5, false, false), QString(">a:1500:1\n>ns.b,c:1500:2.5\n"));
        QCOMPARE(formatSample(Teleplot, items, false, 0, false, false), QString(">a:1\n>ns.b,c:2.5\n"));
    }

    void jsonLines()
    {
        const QString line = formatSample(JsonLines, items, true, 1.5, false, false);
        QVERIFY(line.endsWith('\n'));
        const auto doc = QJsonDocument::fromJson(line.toUtf8());
        QVERIFY(doc.isObject());
        QCOMPARE(doc.object()["t"].toDouble(), 1.5);
        QCOMPARE(doc.object()["values"].toObject()["ns::b,c"].toDouble(), 2.5);
        // unix timestamps use a different key; NaN becomes null so the line stays valid JSON
        const std::vector<Item> nan{{"x", "nan", std::nan("")}};
        const auto d2 = QJsonDocument::fromJson(formatSample(JsonLines, nan, true, 2, true, false).toUtf8());
        QVERIFY(d2.object().contains("unix"));
        QVERIFY(d2.object()["values"].toObject()["x"].isNull());
    }

    void numberText()
    {
        QCOMPARE(numText('f', 0.1f, 0, false, false), QString("0.100000001"));   // %.9g: enough digits for a float32
        QCOMPARE(numText('d', 0.1, 0, false, false), QString("0.10000000000000001"));
        QCOMPARE(numText('Q', 0, ~0ull, false, false), QString("18446744073709551615"));    // exact, not a double
        QCOMPARE(numText('q', 0, static_cast<quint64>(-5), false, false), QString("-5"));
        QCOMPARE(numText('I', 0, static_cast<quint64>(-3), true, false), QString("-3"));      // signed bitfield
        QCOMPARE(numText('I', 7.5, 7, false, true), QString("7.5"));
    }

    void fileSink()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("out.txt");
        {
            StreamSink sink(StreamSink::File, path);
            sink.open();
            sink.start();
            sink.push("hello\n");
            sink.push("world\n");
            sink.close();
            QCOMPARE(sink.written(), qint64(12));
        }
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("hello\nworld\n"));
        f.close();
        {   // append mode keeps the old content
            StreamSink sink(StreamSink::File, path, 115200, true);
            sink.open();
            sink.start();
            sink.push("more\n");
        }
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("hello\nworld\nmore\n"));
    }

    void fileSinkReportsBadPath()
    {
        StreamSink sink(StreamSink::File, "/nonexistent-dir/x.txt");
        QVERIFY_EXCEPTION_THROWN(sink.open(), std::runtime_error);
    }

    void virtualComPort()
    {
        QTemporaryDir dir;
        const QString link = dir.filePath("vcom");
        StreamSink sink(StreamSink::VirtualCom, link);
        sink.open();
        sink.start();
        QVERIFY(QFileInfo(link).isSymLink());
        QVERIFY(!sink.device().isEmpty());
        const int fd = ::open(link.toLocal8Bit().constData(), O_RDONLY | O_NOCTTY | O_NONBLOCK);
        QVERIFY(fd >= 0);
        sink.push("ping 1\n");
        QByteArray got;
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            char buf[64];
            const ssize_t n = ::read(fd, buf, sizeof buf);
            if (n > 0) got.append(buf, static_cast<int>(n));
            return got.contains("ping 1\n");
        }()), 3000);
        ::close(fd);
        sink.close();
        QVERIFY(!QFileInfo(link).isSymLink());              // the link is removed again
    }

    void virtualComPortNobodyReads()
    {
        QTemporaryDir dir;
        StreamSink sink(StreamSink::VirtualCom, dir.filePath("vcom"));
        sink.open();
        sink.start();
        const QString chunk(4000, 'x');
        for (int i = 0; i < 300; ++i) sink.push(chunk);      // far more than the pty buffers: must not block
        sink.close();
        QVERIFY(sink.dropped() > 0);
    }

    void refusesToReplaceRegularFile()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("keep");
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.close();
        StreamSink sink(StreamSink::VirtualCom, path);
        QVERIFY_EXCEPTION_THROWN(sink.open(), std::runtime_error);
        QVERIFY(QFile::exists(path));
    }
};

QTEST_GUILESS_MAIN(TestStream)
#include "test_stream.moc"
