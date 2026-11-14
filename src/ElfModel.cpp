#include "ElfModel.h"

#include <QFile>

#include <dwarf.h>
#include <gelf.h>
#include <libelf.h>

#include <algorithm>
#include <set>
#include <stdexcept>

ElfModel::ElfModel(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) throw std::runtime_error(("Cannot open " + path + ": " + f.errorString()).toStdString());
    data_ = f.readAll();
    f.close();

    elf_version(EV_CURRENT);
    elf_ = elf_memory(data_.data(), static_cast<size_t>(data_.size()));
    if (!elf_ || elf_kind(elf_) != ELF_K_ELF) {
        if (elf_) elf_end(elf_);
        elf_ = nullptr;
        throw std::runtime_error("Not an ELF file: " + path.toStdString());
    }

    ctx_ = std::make_unique<DwarfCtx>();
    ctx_->addrSize = gelf_getclass(elf_) == ELFCLASS64 ? 8 : 4;
    ctx_->dbg = dwarf_begin_elf(elf_, DWARF_C_READ, nullptr);
    if (ctx_->dbg) loadDwarf();
    if (!roots.empty()) hasDebug = true;
    else loadSymtab();

    std::stable_sort(roots.begin(), roots.end(),
                     [](const auto &a, const auto &b) { return a->label.toLower() < b->label.toLower(); });
}

ElfModel::~ElfModel()
{
    roots.clear();                              // Nodes point into ctx_
    if (ctx_ && ctx_->dbg) dwarf_end(ctx_->dbg);
    if (elf_) elf_end(elf_);
}

void ElfModel::walk(const dw::Die &die, Dwarf_Off parentOff, bool parentIsScope)
{
    const unsigned tag = dw::tagOf(die);
    const Dwarf_Off off = dw::offsetOf(die);
    if (parentIsScope) ctx_->scopeParent[off] = parentOff;

    if (dw::isStructTag(tag)) {
        const QString nm = dw::nameOf(die);
        if (!nm.isEmpty() && !dw::hasAttr(die, DW_AT_declaration))
            ctx_->typeIndex.try_emplace({static_cast<int>(tag), nm}, off);
    } else if (tag == DW_TAG_variable) {
        if (auto addr = dw::varAddr(*ctx_, die)) {
            dw::VarInfo vi = dw::varInfo(die);
            if (!vi.name.isEmpty()) pending_.push_back({vi.name, *addr, vi.type, vi.decl});
        }
    }
    const bool scope = dw::isScopeTag(tag);
    for (const dw::Die &c : dw::children(die)) walk(c, off, scope);
}

void ElfModel::loadDwarf()
{
    Dwarf_Off off = 0, next = 0;
    size_t headerSize = 0;
    while (dwarf_nextcu(ctx_->dbg, off, &next, &headerSize, nullptr, nullptr, nullptr) == 0) {
        dw::Die cu = dw::dieAt(*ctx_, off + headerSize);
        if (cu.ok) walk(cu, 0, false);
        off = next;
    }
    std::set<std::pair<QString, quint64>> seen;
    for (const Pending &p : pending_) {
        const QString name = dw::qualified(*ctx_, p.name, p.decl);
        if (!seen.insert({name, p.addr}).second) continue;
        roots.push_back(std::make_unique<Node>(ctx_.get(), Node::Spec{.label = name, .path = name, .addr = p.addr, .tdie = p.type}));
    }
    pending_.clear();
}

void ElfModel::loadSymtab()
{
    for (Elf_Scn *scn = elf_nextscn(elf_, nullptr); scn; scn = elf_nextscn(elf_, scn)) {
        GElf_Shdr sh;
        if (!gelf_getshdr(scn, &sh) || sh.sh_type != SHT_SYMTAB || sh.sh_entsize == 0) continue;
        Elf_Data *data = elf_getdata(scn, nullptr);
        if (!data) continue;
        const size_t count = sh.sh_size / sh.sh_entsize;
        for (size_t i = 0; i < count; ++i) {
            GElf_Sym sym;
            if (!gelf_getsym(data, static_cast<int>(i), &sym)) continue;
            const int size = static_cast<int>(sym.st_size);
            if (GELF_ST_TYPE(sym.st_info) != STT_OBJECT || !unsignedFmt(size) || sym.st_value == 0) continue;
            const char *nm = elf_strptr(elf_, sh.sh_link, sym.st_name);
            if (!nm || !*nm) continue;
            const QString name = QString::fromUtf8(nm);
            roots.push_back(std::make_unique<Node>(
                ctx_.get(), Node::Spec{.label = name, .path = name, .addr = sym.st_value, .leaf = Leaf{size, unsignedFmt(size)},
                                       .typeName = QStringLiteral("raw%1").arg(size * 8)}));
        }
    }
}

Node *findNode(Node &root, const QString &path)
{
    Node *node = &root;
    while (node && node->path != path) {
        Node *next = nullptr;
        for (const auto &k : node->children()) {
            if (path == k->path || path.startsWith(k->path + ".") || path.startsWith(k->path + "[")) {
                next = k.get();
                break;
            }
        }
        node = next;
    }
    return node;
}
