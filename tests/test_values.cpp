#include "Poller.h"
#include "ValueParse.h"

#include <QtTest>

using namespace valparse;

class TestValues : public QObject {
    Q_OBJECT
private slots:
    void parseInteger()
    {
        QCOMPARE(valparse::parseInteger("12")->mag, 12u);
        QCOMPARE(valparse::parseInteger(" 0x1F ")->mag, 31u);
        QCOMPARE(valparse::parseInteger("0b101")->mag, 5u);
        QCOMPARE(valparse::parseInteger("010")->mag, 10u);              // leading zeros are decimal
        QVERIFY(valparse::parseInteger("-7")->neg);
        QVERIFY(!valparse::parseInteger("-0")->neg);
        QVERIFY(!valparse::parseInteger("1.5"));
        QVERIFY(!valparse::parseInteger(""));
        QVERIFY(!valparse::parseInteger("abc"));
        QCOMPARE(valparse::parseInteger("0xFFFFFFFFFFFFFFFF")->mag, ~0ull);
    }

    void enumNames()
    {
        const EnumMap en = {{0, "OFF"}, {2, "AUTO"}, {-1, "ERR"}};
        QCOMPARE(parseWithEnum("AUTO", &en)->mag, 2u);
        QCOMPARE(parseWithEnum("AUTO (2)", &en)->mag, 2u);
        QVERIFY(parseWithEnum("ERR", &en)->neg);
        QCOMPARE(parseWithEnum("7", &en)->mag, 7u);
        QVERIFY(!parseWithEnum("NOPE", &en));
    }

    void packIntegers()
    {
        QByteArray raw;
        QString err;
        QVERIFY(packValue("0x1234", 'H', 2, nullptr, raw, err));
        QCOMPARE(raw, QByteArray("\x34\x12", 2));
        QVERIFY(packValue("-2", 'h', 2, nullptr, raw, err));
        QCOMPARE(raw, QByteArray("\xfe\xff", 2));
        QVERIFY(!packValue("256", 'B', 1, nullptr, raw, err));
        QVERIFY(!packValue("-1", 'B', 1, nullptr, raw, err));
        QVERIFY(!packValue("128", 'b', 1, nullptr, raw, err));
        QVERIFY(packValue("-128", 'b', 1, nullptr, raw, err));
        QVERIFY(packValue("18446744073709551615", 'Q', 8, nullptr, raw, err));
        QCOMPARE(raw, QByteArray(8, '\xff'));
        QVERIFY(!packValue("1.5", 'I', 4, nullptr, raw, err));
        QVERIFY(!err.isEmpty());
    }

    void packFloats()
    {
        QByteArray raw;
        QString err;
        QVERIFY(packValue("1.5", 'f', 4, nullptr, raw, err));
        float f;
        memcpy(&f, raw.constData(), 4);
        QCOMPARE(f, 1.5f);
        QVERIFY(packValue("-2.25", 'd', 8, nullptr, raw, err));
        double d;
        memcpy(&d, raw.constData(), 8);
        QCOMPARE(d, -2.25);
        QVERIFY(!packValue("1e40", 'f', 4, nullptr, raw, err));
        QVERIFY(!packValue("x", 'd', 8, nullptr, raw, err));
    }

    void packEnum()
    {
        const EnumMap en = {{1, "ON"}};
        QByteArray raw;
        QString err;
        QVERIFY(packValue("ON (1)", 'B', 1, &en, raw, err));
        QCOMPARE(raw, QByteArray("\x01", 1));
    }

    void pickWidth()
    {
        QCOMPARE(ocd::pickWidth(0x20000000, 4), 4);
        QCOMPARE(ocd::pickWidth(0x20000002, 4), 2);       // unaligned: never a 32-bit access
        QCOMPARE(ocd::pickWidth(0x20000001, 4), 1);
        QCOMPARE(ocd::pickWidth(0x20000000, 3), 1);
        QCOMPARE(ocd::pickWidth(0x20000000, 8), 4);
    }

    void bitfields()
    {
        const QByteArray raw("\xb4\x01", 2);                // 0x01b4
        QCOMPARE(ocd::extractBits(raw, {2, 4, false}), quint64(0xD));
        QCOMPARE(static_cast<qint64>(ocd::extractBits(raw, {2, 4, true})), qint64(-3));
        QCOMPARE(ocd::extractBits(raw, {0, 16, false}), quint64(0x1b4));
    }

    void writeCommands()
    {
        QCOMPARE(ocd::buildWriteCmd(0x20000000, QByteArray("\x78\x56\x34\x12", 4)), QString("mww 0x20000000 0x12345678"));
        QCOMPARE(ocd::buildWriteCmd(0x20000001, QByteArray("\x01\x02", 2)), QString("mwb 0x20000001 0x1; mwb 0x20000002 0x2"));
        QCOMPARE(ocd::buildWriteCmd(0x20000002, QByteArray("\x34\x12\x78\x56", 4)), QString("mwh 0x20000002 0x1234; mwh 0x20000004 0x5678"));
        QVERIFY_EXCEPTION_THROWN(ocd::buildWriteCmd(0, {}), std::runtime_error);
    }

    void decode()
    {
        QCOMPARE(ocd::decode(QByteArray("\xff", 1), 'b', std::nullopt).v, -1.0);
        QCOMPARE(ocd::decode(QByteArray("\xff", 1), 'B', std::nullopt).v, 255.0);
        float f = -1.5f;
        QCOMPARE(ocd::decode(QByteArray(reinterpret_cast<char *>(&f), 4), 'f', std::nullopt).v, -1.5);
        QCOMPARE(ocd::decode(QByteArray(8, '\xff'), 'Q', std::nullopt).raw, ~0ull);
        QCOMPARE(ocd::decode(QByteArray(8, '\xff'), 'q', std::nullopt).v, -1.0);
        const Reading r = ocd::decode(QByteArray("\x0e", 1), 'I', BitSpec{1, 3, true});    // bits 1..3 = 0b111
        QCOMPARE(r.v, -1.0);
    }
};

QTEST_GUILESS_MAIN(TestValues)
#include "test_values.moc"
