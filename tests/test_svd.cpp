#include "SvdRegs.h"

#include <QTemporaryDir>
#include <QtTest>

static const char *kSvd = R"(<?xml version="1.0"?>
<device>
  <name>TESTDEV</name><description>  Test   device </description>
  <cpu><name>CM4</name></cpu>
  <size>32</size><access>read-write</access>
  <peripherals>
    <peripheral>
      <name>TIM1</name><description>Timer</description><baseAddress>0x40010000</baseAddress>
      <registers>
        <register>
          <name>CR1</name><addressOffset>0x0</addressOffset><description>control
            register</description>
          <fields>
            <field><name>DIR</name><lsb>4</lsb><msb>5</msb></field>
            <field><name>CEN</name><bitOffset>0</bitOffset><bitWidth>1</bitWidth>
              <enumeratedValues><name>E</name>
                <enumeratedValue><name>Off</name><value>0</value></enumeratedValue>
                <enumeratedValue><name>On</name><value>1</value></enumeratedValue>
                <enumeratedValue><name>Weird</name><value>#1x1</value></enumeratedValue>
              </enumeratedValues></field>
            <field><name>X</name><bitRange>[11:8]</bitRange></field>
            <field><name>BIT%s</name><dim>2</dim><dimIncrement>1</dimIncrement><bitOffset>16</bitOffset><bitWidth>1</bitWidth></field>
          </fields>
        </register>
        <register derivedFrom="CR1"><name>CR2</name><addressOffset>0x4</addressOffset></register>
        <register><name>CCR%s</name><dim>3</dim><dimIncrement>4</dimIncrement><dimIndex>1-3</dimIndex>
          <addressOffset>0x10</addressOffset><size>16</size><access>read-only</access></register>
        <cluster><name>CH%s</name><dim>2</dim><dimIncrement>0x20</dimIncrement><addressOffset>0x100</addressOffset>
          <register><name>CTL</name><addressOffset>0</addressOffset><access>write-only</access></register></cluster>
        <register><name>DR</name><addressOffset>0x30</addressOffset><readAction>clear</readAction></register>
      </registers>
    </peripheral>
    <peripheral derivedFrom="TIM1"><name>TIM2</name><baseAddress>0x40000000</baseAddress></peripheral>
    <peripheral><name>GPIO%s</name><dim>2</dim><dimIncrement>0x400</dimIncrement><dimIndex>A,B</dimIndex>
      <baseAddress>0x48000000</baseAddress>
      <registers><register><name>ODR</name><addressOffset>0x14</addressOffset></register></registers></peripheral>
  </peripherals>
</device>)";

class TestSvd : public QObject {
    Q_OBJECT
    QTemporaryDir dir;

    QString write(const char *text, const char *name = "dev.svd")
    {
        QFile f(dir.filePath(name));
        f.open(QIODevice::WriteOnly);
        f.write(text);
        return f.fileName();
    }
    static const svd::Peripheral *find(const svd::LoadResult &r, const QString &name)
    {
        for (const auto &p : r.peripherals)
            if (p->name == name) return p.get();
        return nullptr;
    }
    static const svd::Register *reg(const svd::Peripheral *p, const QString &name)
    {
        for (const auto &r : p->regs)
            if (r.name == name) return &r;
        return nullptr;
    }

private slots:
    void numbers()
    {
        qint64 v = 0;
        QVERIFY(svd::parseInt("0x10", v) && v == 16);
        QVERIFY(svd::parseInt("#101", v) && v == 5);
        QVERIFY(svd::parseInt(" 42 ", v) && v == 42);
        QVERIFY(svd::parseInt("007", v) && v == 7);
        QVERIFY(!svd::parseInt("#1x1", v));
        QVERIFY(!svd::parseInt("", v));
        QVERIFY(svd::parseInt("0xFFFFFFFF", v) && v == 0xFFFFFFFFll);
    }

    void deviceAndPeripherals()
    {
        const auto res = svd::loadSvd(write(kSvd));
        QCOMPARE(res.info.device, QString("TESTDEV"));
        QCOMPARE(res.info.cpu, QString("CM4"));
        QCOMPARE(res.info.description, QString("Test device"));
        QStringList names;
        for (const auto &p : res.peripherals) names << p->name;
        QCOMPARE(names, QStringList({"TIM1", "TIM2", "GPIOA", "GPIOB"}));
        QCOMPARE(find(res, "GPIOB")->base, quint64(0x48000400));
        QCOMPARE(reg(find(res, "GPIOB"), "ODR")->addr, quint64(0x48000414));
    }

    void registersAndInheritance()
    {
        const auto res = svd::loadSvd(write(kSvd));
        const svd::Peripheral *t = find(res, "TIM1");
        QCOMPARE(reg(t, "CR1")->desc, QString("control register"));
        QCOMPARE(reg(t, "CR1")->size, 32);
        QVERIFY(reg(t, "CR1")->writable());
        // dim array with a dimIndex range
        QCOMPARE(reg(t, "CCR1")->addr, quint64(0x40010010));
        QCOMPARE(reg(t, "CCR3")->addr, quint64(0x40010018));
        QCOMPARE(reg(t, "CCR2")->size, 16);
        QVERIFY(!reg(t, "CCR2")->writable());
        QCOMPARE(reg(t, "CCR2")->bytes(), 2);
        QVERIFY(!reg(t, "CCR%s"));
        // clusters
        QCOMPARE(reg(t, "CH0.CTL")->addr, quint64(0x40010100));
        QCOMPARE(reg(t, "CH1.CTL")->addr, quint64(0x40010120));
        QVERIFY(!reg(t, "CH0.CTL")->readable());
        // read side effects
        QVERIFY(reg(t, "DR")->sideEffect);
        QVERIFY(!reg(t, "CR1")->sideEffect);
    }

    void fields()
    {
        const auto res = svd::loadSvd(write(kSvd));
        const svd::Register *cr1 = reg(find(res, "TIM1"), "CR1");
        QCOMPARE(cr1->fields->size(), size_t(5));
        QCOMPARE((*cr1->fields)[0].name, QString("CEN"));            // sorted by bit offset
        const svd::Field &cen = (*cr1->fields)[0];
        QCOMPARE(cen.enumValues.value(1), QString("On"));
        QCOMPARE(cen.enumValues.size(), 2);                          // the "#1x1" pattern is skipped
        const svd::Field &dir = (*cr1->fields)[1];
        QCOMPARE(dir.name, QString("DIR"));
        QCOMPARE(dir.off, 4);
        QCOMPARE(dir.width, 2);
        QCOMPARE(dir.mask(), quint64(3));
        QCOMPARE(dir.extract(0x30), quint64(3));
        const svd::Field &x = (*cr1->fields)[2];
        QCOMPARE(x.off, 8);
        QCOMPARE(x.width, 4);
        QCOMPARE((*cr1->fields)[3].name, QString("BIT0"));            // field dim array
        QCOMPARE((*cr1->fields)[4].off, 17);
        // a register derived from another one shares its fields
        QCOMPARE(reg(find(res, "TIM1"), "CR2")->fields->size(), size_t(5));
    }

    void derivedPeripheralsMoveToTheirOwnBase()
    {
        const auto res = svd::loadSvd(write(kSvd));
        const svd::Peripheral *t2 = find(res, "TIM2");
        QCOMPARE(t2->desc, QString("Timer"));
        QCOMPARE(t2->regs.size(), find(res, "TIM1")->regs.size());
        QCOMPARE(reg(t2, "CR1")->addr, quint64(0x40000000));
        QCOMPARE(reg(t2, "CCR3")->addr, quint64(0x40000018));
        QCOMPARE(reg(find(res, "TIM1"), "CR1")->addr, quint64(0x40010000));   // the original is untouched
    }

    void rejectsBadFiles()
    {
        QVERIFY_EXCEPTION_THROWN(svd::loadSvd("/nonexistent.svd"), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(svd::loadSvd(write("<device><name>x</name></device>", "nop.svd")), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(svd::loadSvd(write("<device><peripherals>", "broken.svd")), std::runtime_error);
    }

    void corePeripherals()
    {
        const auto core = svd::corePeripherals();
        QCOMPARE(core.size(), size_t(5));
        const svd::Peripheral *scb = core[0].get();
        QCOMPARE(scb->name, QString("SCB"));
        const svd::Register *cfsr = reg(scb, "CFSR");
        QCOMPARE(cfsr->addr, quint64(0xE000ED28));
        QCOMPARE((*cfsr->fields)[0].name, QString("IACCVIOL"));
        const svd::Register *ctrl = reg(core[1].get(), "CTRL");
        QVERIFY(ctrl->sideEffect);                                    // reading SysTick CTRL clears COUNTFLAG
        QCOMPARE(core[2]->regs.size(), size_t(40));                   // NVIC banks
        QVERIFY(!reg(core[4].get(), "DHCSR")->writable());
        for (const auto &p : core) QVERIFY(p->builtin);
    }
};

QTEST_GUILESS_MAIN(TestSvd)
#include "test_svd.moc"
