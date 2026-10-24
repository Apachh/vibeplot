// Variable tree built from DWARF debug info: a Node is a variable, struct member or array element.
// Children are created lazily when the node is first expanded.
#pragma once

#include "Types.h"

#include <elfutils/libdw.h>

#include <map>
#include <memory>
#include <unordered_map>

namespace dw {

// A DIE that may be absent (the Python version used None).
struct Die {
    Dwarf_Die d{};
    bool ok = false;
    explicit operator bool() const { return ok; }
};

}  // namespace dw

// Per-file DWARF state shared by every Node of one ElfModel.
struct DwarfCtx {
    Dwarf *dbg = nullptr;
    int addrSize = 4;
    // (tag, name) -> complete struct/union/class DIE, for forward declarations in other units
    std::map<std::pair<int, QString>, Dwarf_Off> typeIndex;
    // DIE offset -> offset of its enclosing namespace/class/struct (only recorded for those parents)
    std::unordered_map<Dwarf_Off, Dwarf_Off> scopeParent;
};

namespace dw {

constexpr int kPointerSize = 4;

Die dieAt(const DwarfCtx &ctx, Dwarf_Off off);
Dwarf_Off offsetOf(const Die &d);
unsigned tagOf(const Die &d);
bool hasAttr(const Die &d, unsigned at);
QString nameOf(const Die &d);                           // DW_AT_name or a null QString
std::optional<quint64> uintAttr(const Die &d, unsigned at);
Die refAttr(const Die &d, unsigned at);                 // follow a reference attribute
Die target(const Die &d);                               // DW_AT_type
Die strip(Die d);                                       // drop typedef/const/volatile/...
Die complete(const DwarfCtx &ctx, Die d);               // full definition of a forward-declared struct
std::vector<Die> children(const Die &d);

bool isStructTag(unsigned tag);
bool isScopeTag(unsigned tag);

std::vector<qint64> arrayDims(const Die &arrayType);
qint64 sizeOf(const DwarfCtx &ctx, const Die &die);
QString typeName(const Die &die);
std::optional<Leaf> leafFmt(const Die &strippedDie);
std::optional<quint64> varAddr(const DwarfCtx &ctx, const Die &variable);
EnumMap enumValues(const Die &enumType);

// Byte offset of a member / base class; nullopt when only known at run time (virtual bases).
std::optional<qint64> memberOffset(const Die &m, qint64 dflt = 0);

// Name, type DIE and declaration DIE of a variable (follows DW_AT_specification / abstract_origin).
struct VarInfo {
    QString name;
    Die type;
    Die decl;
};
VarInfo varInfo(const Die &variable);

// Prefix the name with its enclosing namespaces/classes (ns::Class::var).
QString qualified(const DwarfCtx &ctx, const QString &name, const Die &decl);

}  // namespace dw

class Node {
public:
    static constexpr int kArrayLimit = 512;

    struct Spec {
        QString label;
        QString path;
        quint64 addr = 0;
        dw::Die tdie;
        std::optional<std::vector<qint64>> dims;   // overrides the dimensions of an array type
        std::optional<Leaf> leaf;
        QString typeName;
        std::optional<BitSpec> bits;
    };

    Node(const DwarfCtx *ctx, Spec spec);

    QString label, path;
    quint64 addr = 0;
    std::optional<BitSpec> bits;     // bitfield; addr/leaf cover the bytes that hold it
    std::optional<quint64> rel;      // byte offset from the root variable (set for members)
    std::optional<Leaf> leaf;        // set when the node is a plottable scalar
    qint64 size = 0;
    QString typeName;
    EnumMap enumMap;

    bool expandable() const { return arr_.has_value() || isStruct_; }
    const std::vector<std::unique_ptr<Node>> &children();

private:
    struct ArrayInfo {
        std::vector<qint64> dims;
        dw::Die elem;
    };
    void memberNodes(const dw::Die &sdie, qint64 baseOff, std::vector<std::unique_ptr<Node>> &out);
    std::unique_ptr<Node> bitfieldNode(const dw::Die &m, const dw::Die &tdie, const QString &nm,
                                       qint64 baseOff, qint64 width);

    const DwarfCtx *ctx_;
    dw::Die tdie_;
    dw::Die d_;                      // stripped, completed type
    std::optional<ArrayInfo> arr_;
    bool isStruct_ = false;
    bool kidsBuilt_ = false;
    std::vector<std::unique_ptr<Node>> kids_;
};

// Plottable leaves below `node` (at most `limit`), in declaration order.
void collectLeaves(Node &node, std::vector<Node *> &out, size_t limit = 64);
