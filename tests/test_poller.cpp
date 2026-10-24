// Runs the Poller against a fake OpenOCD Tcl server that keeps a small memory image.
#include "FakeOpenOcd.h"
#include "Poller.h"

#include <QSignalSpy>
#include <QtTest>

class TestPoller : public QObject {
    Q_OBJECT
    FakeOpenOcd *server = nullptr;
    std::vector<ReadSpec> specs;

    std::unique_ptr<Poller> start()
    {
        auto p = std::make_unique<Poller>("127.0.0.1", server->serverPort(), [this] { return specs; }, [] { return 0.005; });
        p->start();
        return p;
    }
    static void finish(Poller &p)
    {
        p.stop();
        QVERIFY(p.wait(3000));
    }

private slots:
    void init()
    {
        server = new FakeOpenOcd(this);
        specs.clear();
    }
    void cleanup()
    {
        delete server;
        server = nullptr;
    }

    void deliversSamples()
    {
        server->set32(0x20000000, 42);
        server->set32(0x20000010, 0xFFFFFFFE);                       // int32 -2
        specs = {{7, 0x20000000, 4, 'I', std::nullopt}, {8, 0x20000010, 4, 'i', std::nullopt}};
        auto p = start();
        QSignalSpy spy(p.get(), &Poller::sample);
        QVERIFY(spy.wait(3000));
        QVERIFY(spy.count() >= 1);
        const Samples vals = spy.first().at(1).value<Samples>();
        QCOMPARE(vals.size(), 2);
        QCOMPARE(vals[0].id, 7);
        QCOMPARE(vals[0].r.v, 42.0);
        QCOMPARE(vals[1].r.v, -2.0);
        QTRY_VERIFY(spy.count() >= 3);                                // keeps sampling
        QVERIFY(spy.last().at(0).toDouble() > spy.first().at(0).toDouble());
        QVERIFY(p->wall0() > 1e9);
        finish(*p);
    }

    void unalignedAndBitfieldReads()
    {
        for (int i = 0; i < 4; ++i) server->mem[0x20000001 + i] = static_cast<quint8>(i + 1);   // 01 02 03 04
        server->mem[0x20000020] = 0b10110100;
        specs = {{1, 0x20000001, 4, 'I', std::nullopt}, {2, 0x20000020, 1, 'I', BitSpec{2, 4, false}}};
        auto p = start();
        QSignalSpy spy(p.get(), &Poller::sample);
        QVERIFY(spy.wait(3000));
        const Samples vals = spy.first().at(1).value<Samples>();
        QCOMPARE(vals[0].r.v, double(0x04030201));
        QCOMPARE(vals[1].r.v, double(0b1101));
        // the unaligned variable must have been read with byte accesses
        QVERIFY(std::any_of(server->commands.begin(), server->commands.end(), [](const QString &c) { return c.contains("read_memory 0x20000001 8 4"); }));
        finish(*p);
    }

    void writesAndReadModifyWrite()
    {
        server->set32(0x20000020, 0xAABBCCDD);
        auto p = start();
        QSignalSpy st(p.get(), &Poller::status);
        QVERIFY(st.wait(3000));                                       // "Connected ..."
        p->requestWrite("var", 0x20000010, QByteArray("\x78\x56\x34\x12", 4));
        p->requestRmw("fld", 0x20000020, 4, 0xF0, 0x50);
        QTRY_VERIFY_WITH_TIMEOUT(server->get32(0x20000010) == 0x12345678 && server->get32(0x20000020) == 0xAABBCC5D, 3000);
        QTRY_VERIFY(std::any_of(st.begin(), st.end(), [](const QVariantList &a) { return a[0].toString() == "Wrote fld @ 0x20000020"; }));
        finish(*p);
    }

    void tolerantRegisterReads()
    {
        server->set32(0x40000000, 0xCAFE);
        server->failing.insert(0x40000100);
        auto p = start();
        QSignalSpy spy(p.get(), &Poller::regsReady);
        p->requestRead(99, {{0x40000000, 4}, {0x40000100, 4}});
        QVERIFY(spy.wait(3000));
        QCOMPARE(spy.first().at(0).toULongLong(), 99u);
        const RegResults res = spy.first().at(1).value<RegResults>();
        QVERIFY(res[0x40000000].ok);
        QCOMPARE(res[0x40000000].value, quint64(0xCAFE));
        QVERIFY(!res[0x40000100].ok);
        finish(*p);
    }

    void reportsConnectionFailure()
    {
        const quint16 port = server->serverPort();
        server->close();
        Poller p("127.0.0.1", port, [] { return std::vector<ReadSpec>(); }, [] { return 0.01; });
        QSignalSpy spy(&p, &Poller::status);
        p.start();
        QVERIFY(spy.wait(5000));
        QVERIFY(spy.first().at(0).toString().startsWith("Cannot connect"));
        QVERIFY(p.wait(3000));
    }

    void reportsReadErrors()
    {
        server->failing.insert(0x20000000);
        specs = {{1, 0x20000000, 4, 'I', std::nullopt}};
        auto p = start();
        QSignalSpy st(p.get(), &Poller::status);
        QTRY_VERIFY_WITH_TIMEOUT(std::any_of(st.begin(), st.end(), [](const QVariantList &a) { return a[0].toString().startsWith("Read error"); }), 3000);
        QVERIFY(p->wait(3000));                                       // the thread ends by itself
    }

    void stopsPromptlyWhileIdle()
    {
        auto p = start();
        QSignalSpy st(p.get(), &Poller::status);
        QVERIFY(st.wait(3000));
        QElapsedTimer t;
        t.start();
        finish(*p);
        QVERIFY(t.elapsed() < 1000);
    }
};

QTEST_MAIN(TestPoller)
#include "test_poller.moc"
