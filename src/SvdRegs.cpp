#include "SvdRegs.h"

#include <QFile>
#include <QRegularExpression>
#include <QXmlStreamReader>

#include <algorithm>
#include <functional>
#include <stdexcept>
#include <unordered_map>

namespace svd {

// --------------------------------------------------------------------------
// Minimal element tree (the subset of ElementTree the parser needs)
// --------------------------------------------------------------------------
namespace {

struct Elem {
    QString tag, text;
    QMap<QString, QString> attrs;
    std::vector<Elem> kids;

    const Elem *find(const char *name) const
    {
        for (const Elem &k : kids)
            if (k.tag == name) return &k;
        return nullptr;
    }
    std::vector<const Elem *> findAll(const char *name) const
    {
        std::vector<const Elem *> r;
        for (const Elem &k : kids)
            if (k.tag == name) r.push_back(&k);
        return r;
    }
    // ElementTree.findtext: null QString when the child is missing, possibly empty otherwise
    QString findText(const char *name) const
    {
        const Elem *e = find(name);
        return e ? (e->text.isNull() ? QStringLiteral("") : e->text) : QString();
    }
    QString attr(const char *name) const { return attrs.value(name); }
};

Elem parseXml(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) throw std::runtime_error(("cannot open file: " + f.errorString()).toStdString());
    QXmlStreamReader xml(&f);
    std::vector<Elem *> stack;
    Elem root;
    bool haveRoot = false;
    while (!xml.atEnd()) {
        switch (xml.readNext()) {
        case QXmlStreamReader::StartElement: {
            Elem *e;
            if (stack.empty()) {
                root = Elem();
                haveRoot = true;
                e = &root;
            } else {
                stack.back()->kids.emplace_back();
                e = &stack.back()->kids.back();
            }
            e->tag = xml.name().toString();
            for (const auto &a : xml.attributes()) e->attrs.insert(a.name().toString(), a.value().toString());
            stack.push_back(e);
            break;
        }
        case QXmlStreamReader::Characters:
            if (!stack.empty()) stack.back()->text += xml.text().toString();
            break;
        case QXmlStreamReader::EndElement:
            if (!stack.empty()) stack.pop_back();
            break;
        default: break;
        }
    }
    if (xml.hasError()) throw std::runtime_error(xml.errorString().toStdString());
    if (!haveRoot) throw std::runtime_error("empty XML document");
    return root;
}

QString clean(const QString &text) { return text.simplified(); }

qint64 parseIntOrThrow(const QString &text)
{
    qint64 v;
    if (!parseInt(text, v)) throw std::runtime_error(("bad number '" + text + "'").toStdString());
    return v;
}

// number of a child element if present and non-empty
bool numChild(const Elem &e, const char *name, qint64 &out)
{
    const QString t = e.findText(name);
    if (t.isEmpty()) return false;
    out = parseIntOrThrow(t);
    return true;
}

struct Inherit {
    int size = 32;
    QString access;
};

}  // namespace

bool parseInt(const QString &text, qint64 &out)
{
    const QString t = text.trimmed();
    if (t.isEmpty()) return false;
    bool ok = false;
    if (t.startsWith('#')) {
        out = static_cast<qint64>(t.mid(1).toULongLong(&ok, 2));
    } else if (t.startsWith("0x") || t.startsWith("0X")) {
        out = static_cast<qint64>(t.mid(2).toULongLong(&ok, 16));
    } else if (t.startsWith('-')) {
        out = t.toLongLong(&ok, 10);
    } else {
        out = static_cast<qint64>(t.toULongLong(&ok, 10));
    }
    return ok;
}

namespace {

// [(name, increment)] of an element that may be a dim array
struct Dim {
    QString index;
    quint64 add;
};

std::vector<Dim> dimIndices(const Elem &el)
{
    std::vector<Dim> out;
    const QString dimText = el.findText("dim");
    if (dimText.isNull()) return out;
    const qint64 n = parseIntOrThrow(dimText);
    const QString incText = el.findText("dimIncrement");
    const qint64 inc = incText.isEmpty() ? 0 : parseIntOrThrow(incText);
    const QString raw = el.findText("dimIndex").trimmed();
    QStringList names;
    if (!raw.isEmpty()) {
        static const QRegularExpression num(R"(^(\d+)\s*-\s*(\d+)$)");
        static const QRegularExpression let(R"(^([A-Za-z])\s*-\s*([A-Za-z])$)");
        auto mn = num.match(raw);
        auto ml = let.match(raw);
        if (raw.contains(',')) {
            for (const QString &x : raw.split(',')) names << x.trimmed();
        } else if (mn.hasMatch()) {
            for (int i = mn.captured(1).toInt(); i <= mn.captured(2).toInt(); ++i) names << QString::number(i);
        } else if (ml.hasMatch()) {
            for (QChar c = ml.captured(1)[0]; c <= ml.captured(2)[0]; c = QChar(c.unicode() + 1)) names << QString(c);
        } else {
            names << raw;
        }
    } else {
        for (qint64 i = 0; i < n; ++i) names << QString::number(i);
    }
    for (qint64 i = 0; i < n; ++i)
        out.push_back({i < names.size() ? names[static_cast<int>(i)] : QString::number(i), static_cast<quint64>(i * inc)});
    return out;
}

std::vector<std::pair<QString, quint64>> expand(const Elem &el, const QString &name)
{
    auto d = dimIndices(el);
    if (el.find("dim") == nullptr) return {{name, 0}};
    std::vector<std::pair<QString, quint64>> out;
    for (const Dim &x : d) {
        QString n = name;
        out.push_back({n.replace("%s", x.index), x.add});
    }
    return out;
}

bool bitsOf(const Elem &el, int &off, int &width)
{
    const QString bo = el.findText("bitOffset");
    if (!bo.isNull()) {
        off = static_cast<int>(parseIntOrThrow(bo));
        const QString bw = el.findText("bitWidth");
        width = bw.isNull() ? 1 : static_cast<int>(parseIntOrThrow(bw));
        return true;
    }
    const QString lsb = el.findText("lsb"), msb = el.findText("msb");
    if (!lsb.isNull() && !msb.isNull()) {
        const int lo = static_cast<int>(parseIntOrThrow(lsb)), hi = static_cast<int>(parseIntOrThrow(msb));
        off = lo;
        width = hi - lo + 1;
        return true;
    }
    const QString br = el.findText("bitRange");
    if (!br.isEmpty()) {
        static const QRegularExpression re(R"(^\[\s*(\d+)\s*:\s*(\d+)\s*\]$)");
        auto m = re.match(br.trimmed());
        if (m.hasMatch()) {
            const int hi = m.captured(1).toInt(), lo = m.captured(2).toInt();
            off = lo;
            width = hi - lo + 1;
            return true;
        }
    }
    return false;
}

using EnumsByName = std::unordered_map<QString, EnumMap>;

EnumMap enumMapOf(const Elem &ev, EnumsByName &byName)
{
    const QString ref = ev.attr("derivedFrom");
    if (!ref.isEmpty()) {
        auto it = byName.find(ref.section('.', -1));
        if (it != byName.end()) return it->second;
    }
    EnumMap out;
    for (const Elem *e : ev.findAll("enumeratedValue")) {
        const QString nm = e->findText("name"), val = e->findText("value");
        if (nm.isEmpty() || val.isNull()) continue;
        qint64 v;
        if (!parseInt(val, v)) continue;            // don't-care patterns such as #1x1
        if (!out.contains(v)) out.insert(v, nm);
    }
    const QString nm = ev.findText("name");
    if (!nm.isEmpty()) byName[nm] = out;
    return out;
}

struct Ctx {
    EnumsByName &enums;
    std::unordered_map<QString, std::shared_ptr<const FieldList>> regFields;   // name -> fields, for derivedFrom
};

std::shared_ptr<const FieldList> parseFields(const Elem *fieldsEl, Ctx &ctx)
{
    auto out = std::make_shared<FieldList>();
    if (!fieldsEl) return out;
    for (const Elem *fel : fieldsEl->findAll("field")) {
        const QString name = fel->findText("name");
        int off, width;
        if (name.isEmpty() || !bitsOf(*fel, off, width)) continue;
        EnumMap en;
        for (const Elem *ev : fel->findAll("enumeratedValues")) {
            const EnumMap m = enumMapOf(*ev, ctx.enums);
            for (auto it = m.begin(); it != m.end(); ++it)
                if (!en.contains(it.key())) en.insert(it.key(), it.value());
        }
        const bool side = !clean(fel->findText("readAction")).isEmpty();
        const QString desc = clean(fel->findText("description"));
        for (const auto &[nm, add] : expand(*fel, name)) {
            Field f;
            f.name = nm;
            f.desc = desc;
            f.off = off + static_cast<int>(add);
            f.width = width;
            f.enumValues = en;
            f.sideEffect = side;
            out->push_back(std::move(f));
        }
    }
    std::stable_sort(out->begin(), out->end(), [](const Field &a, const Field &b) { return a.off < b.off; });
    return out;
}

void parseRegister(const Elem &el, quint64 base, quint64 rel, const Inherit &inh, const QString &prefix, Ctx &ctx,
                   std::vector<Register> &out)
{
    const QString name = el.findText("name");
    if (name.isEmpty()) return;
    const QString offText = el.findText("addressOffset");
    const quint64 off = offText.isEmpty() ? 0 : static_cast<quint64>(parseIntOrThrow(offText));
    qint64 sz;
    const int size = numChild(el, "size", sz) ? static_cast<int>(sz) : inh.size;
    const QString acc = clean(el.findText("access"));
    const QString access = acc.isEmpty() ? inh.access : acc;
    auto fields = parseFields(el.find("fields"), ctx);
    const QString ref = el.attr("derivedFrom");
    if (fields->empty() && !ref.isEmpty()) {
        auto it = ctx.regFields.find(ref.section('.', -1));
        if (it != ctx.regFields.end()) fields = it->second;
    }
    bool side = !clean(el.findText("readAction")).isEmpty();
    for (const Field &f : *fields) side = side || f.sideEffect;
    const QString desc = clean(el.findText("description"));
    for (const auto &[nm, add] : expand(el, name)) {
        Register r;
        r.name = prefix + nm;
        r.desc = desc;
        r.offset = rel + off + add;
        r.addr = base + rel + off + add;
        r.size = size;
        r.access = access;
        r.fields = fields;
        r.sideEffect = side;
        ctx.regFields.emplace(nm, fields);          // first one wins, like setdefault
        out.push_back(std::move(r));
    }
}

void parseChildren(const Elem &parent, quint64 base, quint64 rel, const Inherit &inh, const QString &prefix, Ctx &ctx,
                   std::vector<Register> &out)
{
    for (const Elem &el : parent.kids) {
        if (el.tag == "register") {
            parseRegister(el, base, rel, inh, prefix, ctx, out);
        } else if (el.tag == "cluster") {
            QString cname = el.findText("name");
            if (cname.isEmpty()) cname = "CLUSTER";
            const QString co = el.findText("addressOffset");
            const quint64 coff = co.isEmpty() ? 0 : static_cast<quint64>(parseIntOrThrow(co));
            Inherit inh2 = inh;
            qint64 sz;
            if (numChild(el, "size", sz)) inh2.size = static_cast<int>(sz);
            if (!clean(el.findText("access")).isEmpty()) inh2.access = clean(el.findText("access"));
            for (const auto &[nm, add] : expand(el, cname))
                parseChildren(el, base, rel + coff + add, inh2, prefix + nm + ".", ctx, out);
        }
    }
}

}  // namespace

LoadResult loadSvd(const QString &path)
{
    const Elem root = parseXml(path);
    Inherit dev;
    qint64 sz;
    dev.size = numChild(root, "size", sz) ? static_cast<int>(sz) : 32;
    dev.access = clean(root.findText("access"));
    if (dev.access.isEmpty()) dev.access = "read-write";

    LoadResult res;
    const Elem *cpu = root.find("cpu");
    res.info.device = clean(root.findText("name"));
    res.info.description = clean(root.findText("description"));
    res.info.cpu = cpu ? clean(cpu->findText("name")) : QString();

    const Elem *pels = root.find("peripherals");
    if (!pels) throw std::runtime_error("This file has no <peripherals> section - is it a CMSIS-SVD file?");

    struct Pending {
        const Elem *el;
        quint64 add;
    };
    std::unordered_map<QString, Pending> pending;
    std::vector<QString> order;
    for (const Elem *pel : pels->findAll("peripheral")) {
        const QString nm = pel->findText("name");
        if (nm.isEmpty()) continue;
        for (const auto &[full, add] : expand(*pel, nm)) {
            pending[full] = {pel, add};
            order.push_back(full);
        }
    }

    EnumsByName enums;
    std::unordered_map<QString, std::shared_ptr<Peripheral>> built;
    std::function<std::shared_ptr<Peripheral>(const QString &, int)> get = [&](const QString &name, int depth) {
        if (auto it = built.find(name); it != built.end()) return it->second;
        const Pending &pd = pending.at(name);
        const Elem &pel = *pd.el;
        const QString bt = pel.findText("baseAddress");
        const quint64 base = (bt.isEmpty() ? 0 : static_cast<quint64>(parseIntOrThrow(bt))) + pd.add;
        Inherit inh = dev;
        qint64 psz;
        if (numChild(pel, "size", psz)) inh.size = static_cast<int>(psz);
        if (!clean(pel.findText("access")).isEmpty()) inh.access = clean(pel.findText("access"));
        auto p = std::make_shared<Peripheral>();
        p->name = name;
        p->base = base;
        p->desc = clean(pel.findText("description"));
        std::vector<Register> regs;
        const QString ref = pel.attr("derivedFrom");
        if (!ref.isEmpty() && pending.count(ref) && depth < 20) {
            auto src = get(ref, depth + 1);
            if (p->desc.isEmpty()) p->desc = src->desc;
            for (const Register &r : src->regs) {
                Register c = r;
                c.addr = base + r.offset;
                regs.push_back(std::move(c));
            }
        }
        if (const Elem *rel = pel.find("registers")) {
            std::vector<Register> own;
            Ctx ctx{enums, {}};
            parseChildren(*rel, base, 0, inh, QString(), ctx, own);
            std::vector<Register> merged;
            for (Register &r : regs) {
                bool replaced = false;
                for (const Register &o : own) replaced = replaced || o.name == r.name;
                if (!replaced) merged.push_back(std::move(r));
            }
            for (Register &o : own) merged.push_back(std::move(o));
            regs = std::move(merged);
        }
        p->regs = std::move(regs);
        built[name] = p;
        return p;
    };
    for (const QString &n : order) res.peripherals.push_back(get(n, 0));
    return res;
}

// --------------------------------------------------------------------------
// Built-in Cortex-M core registers (not part of most SVD files)
// --------------------------------------------------------------------------
namespace {

Field mkField(const char *name, int lsb, int width = 1, const char *desc = "", EnumMap en = {})
{
    Field f;
    f.name = name;
    f.desc = desc;
    f.off = lsb;
    f.width = width;
    f.enumValues = std::move(en);
    return f;
}

FieldList bitset(std::initializer_list<std::pair<int, const char *>> names)
{
    FieldList l;
    for (const auto &[b, n] : names) l.push_back(mkField(n, b));
    return l;
}

Register mkReg(quint64 base, const char *name, quint64 off, const char *desc, FieldList fields = {},
               const char *access = "read-write", bool side = false)
{
    Register r;
    r.name = name;
    r.desc = desc;
    r.offset = off;
    r.addr = base + off;
    r.size = 32;
    r.access = access;
    r.fields = std::make_shared<const FieldList>(std::move(fields));
    r.sideEffect = side;
    return r;
}

std::shared_ptr<Peripheral> mkPeriph(const char *name, const char *desc, quint64 base, std::vector<Register> regs)
{
    auto p = std::make_shared<Peripheral>();
    p->name = name;
    p->desc = desc;
    p->base = base;
    p->regs = std::move(regs);
    p->builtin = true;
    return p;
}

}  // namespace

std::vector<std::shared_ptr<Peripheral>> corePeripherals()
{
    const quint64 scb = 0xE000ED00;
    const FieldList cfsr = bitset({{0, "IACCVIOL"}, {1, "DACCVIOL"}, {3, "MUNSTKERR"}, {4, "MSTKERR"}, {5, "MLSPERR"},
                                   {7, "MMARVALID"}, {8, "IBUSERR"}, {9, "PRECISERR"}, {10, "IMPRECISERR"},
                                   {11, "UNSTKERR"}, {12, "STKERR"}, {13, "LSPERR"}, {15, "BFARVALID"},
                                   {16, "UNDEFINSTR"}, {17, "INVSTATE"}, {18, "INVPC"}, {19, "NOCP"}, {20, "STKOF"},
                                   {24, "UNALIGNED"}, {25, "DIVBYZERO"}});
    std::vector<Register> scbRegs = {
        mkReg(scb, "CPUID", 0x00, "CPUID base",
              {mkField("REVISION", 0, 4), mkField("PARTNO", 4, 12), mkField("ARCHITECTURE", 16, 4),
               mkField("VARIANT", 20, 4), mkField("IMPLEMENTER", 24, 8)},
              "read-only"),
        mkReg(scb, "ICSR", 0x04, "Interrupt control and state",
              {mkField("VECTACTIVE", 0, 9, "Active exception number"), mkField("RETTOBASE", 11),
               mkField("VECTPENDING", 12, 9, "Highest pending exception"), mkField("ISRPENDING", 22),
               mkField("ISRPREEMPT", 23), mkField("PENDSTCLR", 25), mkField("PENDSTSET", 26),
               mkField("PENDSVCLR", 27), mkField("PENDSVSET", 28), mkField("NMIPENDSET", 31)}),
        mkReg(scb, "VTOR", 0x08, "Vector table offset"),
        mkReg(scb, "AIRCR", 0x0C, "Application interrupt and reset control",
              {mkField("VECTRESET", 0), mkField("VECTCLRACTIVE", 1), mkField("SYSRESETREQ", 2),
               mkField("PRIGROUP", 8, 3), mkField("ENDIANNESS", 15), mkField("VECTKEY", 16, 16)}),
        mkReg(scb, "SCR", 0x10, "System control", {mkField("SLEEPONEXIT", 1), mkField("SLEEPDEEP", 2), mkField("SEVONPEND", 4)}),
        mkReg(scb, "CCR", 0x14, "Configuration and control",
              {mkField("NONBASETHRDENA", 0), mkField("USERSETMPEND", 1), mkField("UNALIGN_TRP", 3),
               mkField("DIV_0_TRP", 4), mkField("BFHFNMIGN", 8), mkField("STKALIGN", 9)}),
        mkReg(scb, "SHPR1", 0x18, "System handler priority 1 (MemManage, BusFault, UsageFault)"),
        mkReg(scb, "SHPR2", 0x1C, "System handler priority 2 (SVCall)"),
        mkReg(scb, "SHPR3", 0x20, "System handler priority 3 (PendSV, SysTick)"),
        mkReg(scb, "SHCSR", 0x24, "System handler control and state",
              bitset({{0, "MEMFAULTACT"}, {1, "BUSFAULTACT"}, {3, "USGFAULTACT"}, {7, "SVCALLACT"},
                      {8, "MONITORACT"}, {10, "PENDSVACT"}, {11, "SYSTICKACT"}, {12, "USGFAULTPENDED"},
                      {13, "MEMFAULTPENDED"}, {14, "BUSFAULTPENDED"}, {15, "SVCALLPENDED"},
                      {16, "MEMFAULTENA"}, {17, "BUSFAULTENA"}, {18, "USGFAULTENA"}})),
        mkReg(scb, "CFSR", 0x28, "Configurable fault status (MemManage, BusFault, UsageFault)", cfsr),
        mkReg(scb, "HFSR", 0x2C, "HardFault status", {mkField("VECTTBL", 1), mkField("FORCED", 30), mkField("DEBUGEVT", 31)}),
        mkReg(scb, "DFSR", 0x30, "Debug fault status",
              bitset({{0, "HALTED"}, {1, "BKPT"}, {2, "DWTTRAP"}, {3, "VCATCH"}, {4, "EXTERNAL"}})),
        mkReg(scb, "MMFAR", 0x34, "MemManage fault address"),
        mkReg(scb, "BFAR", 0x38, "BusFault address"),
        mkReg(scb, "AFSR", 0x3C, "Auxiliary fault status"),
    };

    const quint64 stk = 0xE000E010;
    std::vector<Register> systick = {
        // reading CTRL clears COUNTFLAG, so it is only read on request
        mkReg(stk, "CTRL", 0x00, "SysTick control and status (reading clears COUNTFLAG)",
              {mkField("ENABLE", 0), mkField("TICKINT", 1), mkField("CLKSOURCE", 2, 1, "", {{0, "external"}, {1, "processor"}}),
               mkField("COUNTFLAG", 16)},
              "read-write", true),
        mkReg(stk, "LOAD", 0x04, "Reload value", {mkField("RELOAD", 0, 24)}),
        mkReg(stk, "VAL", 0x08, "Current value", {mkField("CURRENT", 0, 24)}),
        mkReg(stk, "CALIB", 0x0C, "Calibration", {mkField("TENMS", 0, 24), mkField("SKEW", 30), mkField("NOREF", 31)}, "read-only"),
    };

    const quint64 nvicBase = 0xE000E100;
    std::vector<Register> nvic;
    struct Bank {
        const char *prefix;
        quint64 start;
        const char *what;
    };
    for (const Bank &b : {Bank{"ISER", 0x000, "Interrupt set-enable"}, Bank{"ICER", 0x080, "Interrupt clear-enable"},
                          Bank{"ISPR", 0x100, "Interrupt set-pending"}, Bank{"ICPR", 0x180, "Interrupt clear-pending"},
                          Bank{"IABR", 0x200, "Interrupt active bit"}}) {
        for (int i = 0; i < 8; ++i) {
            Register r = mkReg(nvicBase, "", b.start + 4 * i, "");
            r.name = QStringLiteral("%1%2").arg(b.prefix).arg(i);
            r.desc = QStringLiteral("%1 (IRQ %2..%3)").arg(b.what).arg(32 * i).arg(32 * i + 31);
            nvic.push_back(std::move(r));
        }
    }

    const quint64 dwt = 0xE0001000;
    std::vector<Register> dwtRegs = {
        mkReg(dwt, "CTRL", 0x00, "DWT control", {mkField("CYCCNTENA", 0), mkField("NUMCOMP", 28, 4)}),
        mkReg(dwt, "CYCCNT", 0x04, "Cycle counter"),
    };

    const quint64 cd = 0xE000EDF0;
    std::vector<Register> coredbg = {
        // DHCSR's S_RESET_ST / S_RETIRE_ST clear on read, and writing needs a debug key
        mkReg(cd, "DHCSR", 0x00, "Debug halting control and status (read-only here; owned by the debugger)",
              bitset({{0, "C_DEBUGEN"}, {1, "C_HALT"}, {2, "C_STEP"}, {3, "C_MASKINTS"}, {16, "S_REGRDY"},
                      {17, "S_HALT"}, {18, "S_SLEEP"}, {19, "S_LOCKUP"}, {24, "S_RETIRE_ST"}, {25, "S_RESET_ST"}}),
              "read-only", true),
        mkReg(cd, "DEMCR", 0x0C, "Debug exception and monitor control",
              bitset({{0, "VC_CORERESET"}, {4, "VC_MMERR"}, {5, "VC_NOCPERR"}, {6, "VC_CHKERR"}, {7, "VC_STATERR"},
                      {8, "VC_BUSERR"}, {9, "VC_INTERR"}, {10, "VC_HARDERR"}, {16, "MON_EN"}, {17, "MON_PEND"},
                      {18, "MON_STEP"}, {19, "MON_REQ"}, {24, "TRCENA"}})),
    };

    return {
        mkPeriph("SCB", "System Control Block (Cortex-M core, built-in)", scb, std::move(scbRegs)),
        mkPeriph("SysTick", "SysTick timer (Cortex-M core, built-in)", stk, std::move(systick)),
        mkPeriph("NVIC", "Nested Vectored Interrupt Controller (Cortex-M core, built-in)", nvicBase, std::move(nvic)),
        mkPeriph("DWT", "Data Watchpoint and Trace (Cortex-M3/M4/M7/M33, built-in)", dwt, std::move(dwtRegs)),
        mkPeriph("CoreDebug", "Core debug registers (Cortex-M core, built-in)", cd, std::move(coredbg)),
    };
}

}  // namespace svd
