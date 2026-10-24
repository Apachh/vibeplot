#include "ElfModel.h"

#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

class TestElf : public QObject {
    Q_OBJECT
    std::unique_ptr<ElfModel> model;

    Node *root(const QString &name) const
    {
        for (const auto &r : model->roots)
            if (r->label == name) return r.get();
        return nullptr;
    }
    static Node *child(Node &n, const QString &label)
    {
        for (const auto &k : n.children())
            if (k->label == label) return k.get();
        return nullptr;
    }

private slots:
    void initTestCase() { model = std::make_unique<ElfModel>(FIXTURE_ELF); }

    void loadsVariables()
    {
        QVERIFY(model->hasDebug);
        QVERIFY(model->roots.size() > 15);
        for (size_t i = 1; i < model->roots.size(); ++i)
            QVERIFY(model->roots[i - 1]->label.toLower() <= model->roots[i]->label.toLower());   // sorted
        Node *dval = root("dval");
        QVERIFY(dval);
        QVERIFY(dval->leaf);
        QCOMPARE(dval->leaf->size, 8);
        QCOMPARE(dval->leaf->fmt, 'd');
        QCOMPARE(dval->typeName, QString("double"));
        QCOMPARE(root("flag")->leaf->fmt, 'B');
        QCOMPARE(root("big")->typeName, QString("volatile uint64_t"));
        QCOMPARE(root("big")->leaf->fmt, 'Q');
        QCOMPARE(root("counter"), nullptr);                           // lives in namespace app
    }

    void namespacesAndStaticMembers()
    {
        QVERIFY(root("app::counter"));
        QCOMPARE(root("app::counter")->leaf->fmt, 'i');
        QVERIFY(root("app::drivers::Uart::instances"));               // defined through DW_AT_specification
        Node *console = root("app::drivers::console");
        QVERIFY(console);
        QVERIFY(console->expandable());
        QCOMPARE(console->children().size(), size_t(2));              // static members are not instance members
        QCOMPARE(child(*console, "baud")->addr, console->addr);
        QCOMPARE(child(*console, "mode")->addr, console->addr + 4);
        QVERIFY(root("fn_static"));                                   // function-local static
    }

    void enumerations()
    {
        Node *mode = root("mode_var");
        QCOMPARE(mode->enumMap.size(), 3);
        QCOMPARE(mode->enumMap.value(2), QString("MODE_AUTO"));
        Node *level = root("level");
        QCOMPARE(level->leaf->fmt, 'h');                              // signed 16 bit
        QCOMPARE(level->enumMap.value(-1), QString("Low"));
    }

    void arrays()
    {
        Node *m = root("matrix");
        QCOMPARE(m->typeName, QString("uint8_t[3][4]"));
        QCOMPARE(m->size, qint64(12));
        QVERIFY(!m->leaf);
        QCOMPARE(m->children().size(), size_t(3));
        Node *row = m->children()[1].get();
        QCOMPARE(row->label, QString("[1]"));
        QCOMPARE(row->addr, m->addr + 4);
        QCOMPARE(row->children().size(), size_t(4));
        Node *cell = row->children()[2].get();
        QCOMPARE(cell->path, QString("matrix[1][2]"));
        QCOMPARE(cell->addr, m->addr + 6);
        QCOMPARE(*cell->rel, quint64(6));
        QCOMPARE(root("samples")->children().size(), size_t(8));
    }

    void inheritanceAndAnonymousMembers()
    {
        Node *d = root("derived");
        QVERIFY(child(*d, "base_field"));                             // base class members are flattened in
        QCOMPARE(child(*d, "ratio")->leaf->fmt, 'f');
        QCOMPARE(*child(*d, "extra")->rel, quint64(8));
        QVERIFY(child(*d, "whole"));                                  // anonymous union
        QVERIFY(child(*d, "hi"));                                     // anonymous struct inside it
        QCOMPARE(child(*d, "hi")->addr, child(*d, "whole")->addr + 1);
    }

    void bitfields()
    {
        Node *flags = child(*root("derived"), "flags");
        QVERIFY(flags);
        Node *a = child(*flags, "a"), *b = child(*flags, "b"), *c = child(*flags, "c"), *d = child(*flags, "d");
        QVERIFY(a && b && c && d);
        QCOMPARE(a->bits->shift, 0);
        QCOMPARE(a->bits->width, 3);
        QVERIFY(!a->bits->isSigned);
        QCOMPARE(b->bits->shift, 3);
        QCOMPARE(b->bits->width, 5);
        QVERIFY(c->bits->isSigned);
        QCOMPARE(c->bits->width, 4);
        QCOMPARE(d->bits->width, 16);
        QCOMPARE(d->addr, flags->addr + 2);
        QCOMPARE(a->typeName, QString("uint32_t:3"));
        QVERIFY(!child(*flags, ""));                                  // unnamed padding bits are skipped
    }

    void packedStructs()
    {
        Node *p = root("packed");
        QCOMPARE(p->size, qint64(8));
        QCOMPARE(*child(*p, "tag")->rel, quint64(0));
        QCOMPARE(*child(*p, "value")->rel, quint64(1));               // unaligned
        QCOMPARE(*child(*p, "small")->rel, quint64(5));
        QCOMPARE(*child(*p, "mode")->rel, quint64(7));
        QCOMPARE(child(*p, "value")->addr, p->addr + 1);
    }

    void structsOfStructs()
    {
        Node *o = root("g_opaque_storage");                           // defined in another translation unit
        QVERIFY(o);
        QCOMPARE(child(*o, "levels")->children().size(), size_t(3));
        QCOMPARE(root("chain")->children()[1]->children().size(), size_t(2));
    }

    void findsNodesByPath()
    {
        Node *d = root("derived");
        Node *c = findNode(*d, "derived.flags.c");
        QVERIFY(c);
        QCOMPARE(c->label, QString("c"));
        Node *cell = findNode(*root("matrix"), "matrix[2][3]");
        QVERIFY(cell);
        QCOMPARE(cell->addr, root("matrix")->addr + 11);
        QCOMPARE(findNode(*d, "derived.nope"), nullptr);
    }

    void collectsLeavesWithLimit()
    {
        std::vector<Node *> leaves;
        collectLeaves(*root("derived"), leaves);
        QVERIFY(leaves.size() >= 10);
        for (Node *n : leaves) QVERIFY(n->leaf);
        std::vector<Node *> few;
        collectLeaves(*root("matrix"), few, 5);
        QCOMPARE(few.size(), size_t(5));
        std::vector<Node *> single;
        collectLeaves(*root("dval"), single);
        QCOMPARE(single.size(), size_t(1));
    }

    void fallsBackToSymbolTable()
    {
        QTemporaryDir dir;
        const QString stripped = dir.filePath("stripped.elf");
        QVERIFY(QFile::copy(FIXTURE_ELF, stripped));
        if (QProcess::execute("strip", {"-g", stripped}) != 0) QSKIP("strip is not available");
        ElfModel m(stripped);
        QVERIFY(!m.hasDebug);
        Node *dval = nullptr;
        for (const auto &r : m.roots)
            if (r->label == "dval") dval = r.get();
        QVERIFY(dval);
        QCOMPARE(dval->typeName, QString("raw64"));
        QCOMPARE(dval->leaf->size, 8);
        QCOMPARE(dval->leaf->fmt, 'Q');
        QVERIFY(!dval->expandable());
    }

    void rejectsBadFiles()
    {
        QVERIFY_EXCEPTION_THROWN(ElfModel("/nonexistent/file.elf"), std::runtime_error);
        QTemporaryDir dir;
        QFile f(dir.filePath("text.elf"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("this is not an ELF file");
        f.close();
        QVERIFY_EXCEPTION_THROWN(ElfModel(f.fileName()), std::runtime_error);
    }

    void survivesTheFileBeingOverwritten()
    {
        QTemporaryDir dir;
        const QString copy = dir.filePath("live.elf");
        QVERIFY(QFile::copy(FIXTURE_ELF, copy));
        ElfModel m(copy);
        QFile f(copy);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));  // a rebuild rewrites the file
        f.write("garbage");
        f.close();
        Node *derived = nullptr;
        for (const auto &r : m.roots)
            if (r->label == "derived") derived = r.get();
        QVERIFY(derived);
        QVERIFY(derived->children().size() > 5);                      // the model works from its in-memory copy
    }
};

QTEST_GUILESS_MAIN(TestElf)
#include "test_elf.moc"
