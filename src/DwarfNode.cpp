#include "DwarfNode.h"

#include <dwarf.h>

#include <algorithm>

namespace dw {

Die dieAt(const DwarfCtx &ctx, Dwarf_Off off)
{
    Die r;
    r.ok = dwarf_offdie(ctx.dbg, off, &r.d) != nullptr;
    return r;
}

Dwarf_Off offsetOf(const Die &d) { return dwarf_dieoffset(const_cast<Dwarf_Die *>(&d.d)); }

unsigned tagOf(const Die &d) { return d.ok ? static_cast<unsigned>(dwarf_tag(const_cast<Dwarf_Die *>(&d.d))) : 0; }

bool hasAttr(const Die &d, unsigned at)
{
    return d.ok && dwarf_hasattr(const_cast<Dwarf_Die *>(&d.d), at) != 0;
}

QString nameOf(const Die &d)
{
    if (!d.ok) return {};
    Dwarf_Attribute a;
    if (!dwarf_attr(const_cast<Dwarf_Die *>(&d.d), DW_AT_name, &a)) return {};
    const char *s = dwarf_formstring(&a);
    return s ? QString::fromUtf8(s) : QString();
}

std::optional<quint64> uintAttr(const Die &d, unsigned at)
{
    if (!d.ok) return std::nullopt;
    Dwarf_Attribute a;
    if (!dwarf_attr(const_cast<Dwarf_Die *>(&d.d), at, &a)) return std::nullopt;
    Dwarf_Word w;
    if (dwarf_formudata(&a, &w) == 0) return w;
    Dwarf_Sword sw;
    if (dwarf_formsdata(&a, &sw) == 0) return static_cast<quint64>(sw);
    return std::nullopt;
}

Die refAttr(const Die &d, unsigned at)
{
    Die r;
    if (!d.ok) return r;
    Dwarf_Attribute a;
    if (!dwarf_attr(const_cast<Dwarf_Die *>(&d.d), at, &a)) return r;
    r.ok = dwarf_formref_die(&a, &r.d) != nullptr;
    return r;
}

Die target(const Die &d) { return refAttr(d, DW_AT_type); }

static bool isQualifier(unsigned t)
{
    return t == DW_TAG_typedef || t == DW_TAG_const_type || t == DW_TAG_volatile_type ||
           t == DW_TAG_restrict_type || t == DW_TAG_atomic_type;
}

bool isStructTag(unsigned t)
{
    return t == DW_TAG_structure_type || t == DW_TAG_union_type || t == DW_TAG_class_type;
}

bool isScopeTag(unsigned t) { return t == DW_TAG_namespace || isStructTag(t); }

Die strip(Die d)
{
    while (d.ok && isQualifier(tagOf(d))) d = target(d);
    return d;
}

Die complete(const DwarfCtx &ctx, Die d)
{
    if (d.ok && isStructTag(tagOf(d)) && hasAttr(d, DW_AT_declaration)) {
        auto it = ctx.typeIndex.find({static_cast<int>(tagOf(d)), nameOf(d)});
        if (it != ctx.typeIndex.end()) {
            Die full = dieAt(ctx, it->second);
            if (full.ok) return full;
        }
    }
    return d;
}

std::vector<Die> children(const Die &d)
{
    std::vector<Die> out;
    if (!d.ok) return out;
    Die c;
    if (dwarf_child(const_cast<Dwarf_Die *>(&d.d), &c.d) != 0) return out;
    c.ok = true;
    for (;;) {
        out.push_back(c);
        Die n;
        if (dwarf_siblingof(&c.d, &n.d) != 0) break;
        n.ok = true;
        c = n;
    }
    return out;
}

std::vector<qint64> arrayDims(const Die &arrayType)
{
    std::vector<qint64> dims;
    for (const Die &c : children(arrayType)) {
        if (tagOf(c) != DW_TAG_subrange_type) continue;
        auto cnt = uintAttr(c, DW_AT_count);
        auto ub = uintAttr(c, DW_AT_upper_bound);
        if (cnt) {
            dims.push_back(static_cast<qint64>(*cnt));
        } else if (ub && *ub < 0xFFFFFFFFull) {      // 0xFFFFFFFF is how old compilers write [0] / []
            dims.push_back(static_cast<qint64>(*ub) + 1);
        } else {
            dims.push_back(0);
        }
    }
    if (dims.empty()) dims.push_back(0);
    return dims;
}

qint64 sizeOf(const DwarfCtx &ctx, const Die &die)
{
    Die d = complete(ctx, strip(die));
    if (!d.ok) return 0;
    if (auto bs = uintAttr(d, DW_AT_byte_size)) return static_cast<qint64>(*bs);
    if (tagOf(d) == DW_TAG_pointer_type) return kPointerSize;
    if (tagOf(d) == DW_TAG_array_type) {
        qint64 n = 1;
        for (qint64 c : arrayDims(d)) n *= c;
        return n * sizeOf(ctx, target(d));
    }
    return 0;
}

QString typeName(const Die &die)
{
    if (!die.ok) return QStringLiteral("void");
    const unsigned t = tagOf(die);
    const QString n = nameOf(die);
    switch (t) {
    case DW_TAG_base_type:
    case DW_TAG_typedef:
        return n.isEmpty() ? QStringLiteral("?") : n;
    case DW_TAG_const_type: return "const " + typeName(target(die));
    case DW_TAG_volatile_type: return "volatile " + typeName(target(die));
    case DW_TAG_pointer_type: return typeName(target(die)) + "*";
    case DW_TAG_array_type: {
        QString s = typeName(target(die));
        for (qint64 c : arrayDims(die)) s += QStringLiteral("[%1]").arg(c);
        return s;
    }
    case DW_TAG_structure_type: return "struct " + (n.isEmpty() ? QStringLiteral("{...}") : n);
    case DW_TAG_union_type: return "union " + (n.isEmpty() ? QStringLiteral("{...}") : n);
    case DW_TAG_enumeration_type: return "enum " + (n.isEmpty() ? QStringLiteral("{...}") : n);
    default: break;
    }
    if (!n.isEmpty()) return n;
    switch (t) {
    case DW_TAG_class_type: return "class_type";
    case DW_TAG_reference_type: return "reference_type";
    case DW_TAG_rvalue_reference_type: return "rvalue_reference_type";
    case DW_TAG_restrict_type: return "restrict_type";
    case DW_TAG_atomic_type: return "atomic_type";
    case DW_TAG_subroutine_type: return "subroutine_type";
    case DW_TAG_ptr_to_member_type: return "ptr_to_member_type";
    case DW_TAG_unspecified_type: return "unspecified_type";
    default: return QStringLiteral("tag_0x%1").arg(t, 0, 16);
    }
}

static bool isSignedEncoding(quint64 enc) { return enc == DW_ATE_signed || enc == DW_ATE_signed_char; }

std::optional<Leaf> leafFmt(const Die &d)
{
    if (!d.ok) return std::nullopt;
    auto lookup = [](bool isSigned, int size) -> std::optional<Leaf> {
        char f = isSigned ? signedFmt(size) : unsignedFmt(size);
        if (!f) return std::nullopt;
        return Leaf{size, f};
    };
    switch (tagOf(d)) {
    case DW_TAG_pointer_type: return Leaf{4, 'I'};
    case DW_TAG_enumeration_type: {
        int size = static_cast<int>(uintAttr(d, DW_AT_byte_size).value_or(4));
        Die under = strip(target(d));
        bool sgn = under.ok && isSignedEncoding(uintAttr(under, DW_AT_encoding).value_or(0));
        return lookup(sgn, size);
    }
    case DW_TAG_base_type: {
        auto sz = uintAttr(d, DW_AT_byte_size);
        auto enc = uintAttr(d, DW_AT_encoding);
        if (!sz || !enc) return std::nullopt;
        int size = static_cast<int>(*sz);
        if (*enc == DW_ATE_float) {
            if (size == 4) return Leaf{4, 'f'};
            if (size == 8) return Leaf{8, 'd'};
            return std::nullopt;
        }
        return lookup(isSignedEncoding(*enc), size);
    }
    default: return std::nullopt;
    }
}

std::optional<quint64> varAddr(const DwarfCtx &ctx, const Die &variable)
{
    Dwarf_Attribute a;
    if (!variable.ok || !dwarf_attr(const_cast<Dwarf_Die *>(&variable.d), DW_AT_location, &a))
        return std::nullopt;
    if (!dwarf_hasform(&a, DW_FORM_exprloc) && !dwarf_hasform(&a, DW_FORM_block1) &&
        !dwarf_hasform(&a, DW_FORM_block2) && !dwarf_hasform(&a, DW_FORM_block4) &&
        !dwarf_hasform(&a, DW_FORM_block))
        return std::nullopt;                       // a location list: not a fixed address
    Dwarf_Block blk;
    if (dwarf_formblock(&a, &blk) != 0) return std::nullopt;
    if (blk.length < 1 + static_cast<size_t>(ctx.addrSize) || blk.data[0] != DW_OP_addr) return std::nullopt;
    quint64 addr = 0;
    for (int i = ctx.addrSize - 1; i >= 0; --i) addr = (addr << 8) | blk.data[1 + i];
    if (addr == 0) return std::nullopt;
    return addr;
}

EnumMap enumValues(const Die &enumType)
{
    EnumMap out;
    for (const Die &c : children(enumType)) {
        if (tagOf(c) != DW_TAG_enumerator) continue;
        const QString nm = nameOf(c);
        auto v = uintAttr(c, DW_AT_const_value);
        if (!nm.isEmpty() && v && !out.contains(static_cast<qint64>(*v))) out.insert(static_cast<qint64>(*v), nm);
    }
    return out;
}

std::optional<qint64> memberOffset(const Die &m, qint64 dflt)
{
    Dwarf_Attribute a;
    if (!m.ok || !dwarf_attr(const_cast<Dwarf_Die *>(&m.d), DW_AT_data_member_location, &a)) return dflt;
    if (dwarf_hasform(&a, DW_FORM_exprloc) || dwarf_hasform(&a, DW_FORM_block1) ||
        dwarf_hasform(&a, DW_FORM_block2) || dwarf_hasform(&a, DW_FORM_block4) ||
        dwarf_hasform(&a, DW_FORM_block)) {
        Dwarf_Op *ops = nullptr;
        size_t n = 0;
        if (dwarf_getlocation(&a, &ops, &n) == 0 && n >= 1 && ops[0].atom == DW_OP_plus_uconst)
            return static_cast<qint64>(ops[0].number);
        return std::nullopt;
    }
    Dwarf_Word w;
    if (dwarf_formudata(&a, &w) == 0) return static_cast<qint64>(w);
    return std::nullopt;
}

VarInfo varInfo(const Die &variable)
{
    VarInfo r;
    r.name = nameOf(variable);
    r.type = target(variable);
    r.decl = variable;
    for (unsigned key : {static_cast<unsigned>(DW_AT_specification), static_cast<unsigned>(DW_AT_abstract_origin)}) {
        if (!hasAttr(variable, key)) continue;
        r.decl = refAttr(variable, key);
        if (r.decl.ok) {
            if (r.name.isEmpty()) r.name = nameOf(r.decl);
            if (!r.type.ok) r.type = target(r.decl);
        } else {
            r.decl = variable;
        }
        break;
    }
    return r;
}

QString qualified(const DwarfCtx &ctx, const QString &name, const Die &decl)
{
    QStringList parts{name};
    auto it = ctx.scopeParent.find(offsetOf(decl));
    while (it != ctx.scopeParent.end()) {
        Die p = dieAt(ctx, it->second);
        const QString n = nameOf(p);
        parts << (n.isEmpty() ? QStringLiteral("(anon)") : n);
        it = ctx.scopeParent.find(it->second);
    }
    std::reverse(parts.begin(), parts.end());
    return parts.join("::");
}

}  // namespace dw

// --------------------------------------------------------------------------
// Node
// --------------------------------------------------------------------------
using namespace dw;

Node::Node(const DwarfCtx *ctx, Spec spec)
    : label(std::move(spec.label)), path(std::move(spec.path)), addr(spec.addr), bits(spec.bits),
      leaf(spec.leaf), ctx_(ctx), tdie_(spec.tdie)
{
    d_ = complete(*ctx_, strip(tdie_));
    if (!tdie_.ok && !leaf) leaf = Leaf{4, 'I'};
    if (d_.ok) {
        const unsigned t = tagOf(d_);
        if (t == DW_TAG_array_type) {
            arr_ = ArrayInfo{spec.dims ? *spec.dims : arrayDims(d_), target(d_)};
        } else if (isStructTag(t)) {
            isStruct_ = true;
        } else if (!spec.leaf) {
            leaf = leafFmt(d_);
        }
        if (t == DW_TAG_enumeration_type) enumMap = enumValues(d_);
    }
    if (arr_) {
        qint64 n = 1;
        for (qint64 c : arr_->dims) n *= c;
        size = n * sizeOf(*ctx_, arr_->elem);
        if (!spec.typeName.isEmpty()) {
            typeName = spec.typeName;
        } else {
            typeName = dw::typeName(arr_->elem);
            for (qint64 c : arr_->dims) typeName += QStringLiteral("[%1]").arg(c);
        }
    } else {
        size = tdie_.ok ? sizeOf(*ctx_, tdie_) : 0;
        if (size == 0 && leaf) size = leaf->size;
        if (bits && leaf) size = leaf->size;
        typeName = spec.typeName.isEmpty() ? dw::typeName(tdie_) : spec.typeName;
    }
}

const std::vector<std::unique_ptr<Node>> &Node::children()
{
    if (kidsBuilt_) return kids_;
    std::vector<std::unique_ptr<Node>> kids;
    if (arr_) {
        const auto &ds = arr_->dims;
        qint64 sub = 1;
        for (size_t i = 1; i < ds.size(); ++i) sub *= ds[i];
        const qint64 stride = sizeOf(*ctx_, arr_->elem) * sub;
        const qint64 count = std::min<qint64>(ds[0], kArrayLimit);
        for (qint64 i = 0; i < count; ++i) {
            const quint64 a = addr + static_cast<quint64>(i * stride);
            const QString p = QStringLiteral("%1[%2]").arg(path).arg(i);
            const QString lbl = QStringLiteral("[%1]").arg(i);
            if (ds.size() > 1) {
                kids.push_back(std::make_unique<Node>(
                    ctx_, Spec{.label = lbl, .path = p, .addr = a, .tdie = tdie_,
                               .dims = std::vector<qint64>(ds.begin() + 1, ds.end())}));
            } else {
                kids.push_back(std::make_unique<Node>(ctx_, Spec{.label = lbl, .path = p, .addr = a, .tdie = arr_->elem}));
            }
        }
    } else if (isStruct_) {
        memberNodes(d_, 0, kids);
    }
    for (auto &k : kids) k->rel = rel.value_or(0) + (k->addr - addr);
    kids_ = std::move(kids);
    kidsBuilt_ = true;
    return kids_;
}

void Node::memberNodes(const Die &sdie, qint64 baseOff, std::vector<std::unique_ptr<Node>> &out)
{
    for (const Die &m : dw::children(sdie)) {
        const unsigned mt = tagOf(m);
        if (mt == DW_TAG_inheritance) {                 // C++ base class: flatten its members in
            Die bt = complete(*ctx_, strip(target(m)));
            auto off = memberOffset(m);
            if (bt.ok && isStructTag(tagOf(bt)) && off) memberNodes(bt, baseOff + *off, out);
            continue;
        }
        if (mt != DW_TAG_member) continue;
        if (hasAttr(m, DW_AT_declaration) || hasAttr(m, DW_AT_external)) continue;   // static member
        Die tdie = target(m);
        const QString nm = nameOf(m);
        if (auto width = uintAttr(m, DW_AT_bit_size)) {
            if (auto bn = bitfieldNode(m, tdie, nm, baseOff, static_cast<qint64>(*width))) out.push_back(std::move(bn));
            continue;
        }
        auto off = memberOffset(m);
        if (!off) continue;
        Die t = complete(*ctx_, strip(tdie));
        if (nm.isEmpty() && t.ok && isStructTag(tagOf(t))) {       // anonymous struct/union: flatten
            memberNodes(t, baseOff + *off, out);
            continue;
        }
        const QString n = nm.isEmpty() ? QStringLiteral("<anon>") : nm;
        out.push_back(std::make_unique<Node>(
            ctx_, Spec{.label = n, .path = path + "." + n, .addr = addr + baseOff + *off, .tdie = tdie}));
    }
}

std::unique_ptr<Node> Node::bitfieldNode(const Die &m, const Die &tdie, const QString &nm, qint64 baseOff, qint64 width)
{
    if (nm.isEmpty() || width <= 0) return nullptr;            // unnamed padding bits
    qint64 unit = static_cast<qint64>(uintAttr(m, DW_AT_byte_size).value_or(0));
    if (unit == 0) unit = sizeOf(*ctx_, tdie);
    if (unit == 0) unit = 4;
    qint64 absBit;
    if (auto dbo = uintAttr(m, DW_AT_data_bit_offset)) {      // DWARF 4/5: from the start of the struct
        absBit = baseOff * 8 + static_cast<qint64>(*dbo);
    } else {                                                   // older DWARF: counted from the storage unit's MSB
        auto loc = memberOffset(m);
        if (!loc) return nullptr;
        absBit = (baseOff + *loc) * 8 + (unit * 8 - static_cast<qint64>(uintAttr(m, DW_AT_bit_offset).value_or(0)) - width);
    }
    const int shift = static_cast<int>(absBit % 8);
    const int nbytes = static_cast<int>((shift + width + 7) / 8);
    if (nbytes > 8) return nullptr;
    Die t = strip(tdie);
    bool sgn = false;
    if (t.ok && tagOf(t) == DW_TAG_base_type) {
        sgn = isSignedEncoding(uintAttr(t, DW_AT_encoding).value_or(0));
    } else if (t.ok && tagOf(t) == DW_TAG_enumeration_type) {
        auto lf = leafFmt(t);
        sgn = lf && isSignedFmt(lf->fmt);
    }
    return std::make_unique<Node>(
        ctx_, Spec{.label = nm, .path = path + "." + nm, .addr = addr + static_cast<quint64>(absBit / 8), .tdie = tdie,
                   .leaf = Leaf{nbytes, 'I'}, .typeName = QStringLiteral("%1:%2").arg(dw::typeName(tdie)).arg(width),
                   .bits = BitSpec{shift, static_cast<int>(width), sgn}});
}

void collectLeaves(Node &node, std::vector<Node *> &out, size_t limit)
{
    if (out.size() >= limit) return;
    if (node.leaf) {
        out.push_back(&node);
        return;
    }
    for (const auto &k : node.children()) {
        collectLeaves(*k, out, limit);
        if (out.size() >= limit) break;
    }
}
