// Loads a firmware ELF and exposes its global / static variables as a tree of Nodes.
#pragma once

#include "DwarfNode.h"

#include <QByteArray>

#include <memory>
#include <vector>

typedef struct Elf Elf;

class ElfModel {
public:
    // Throws std::runtime_error when the file cannot be read or is not an ELF.
    explicit ElfModel(const QString &path);
    ~ElfModel();
    ElfModel(const ElfModel &) = delete;
    ElfModel &operator=(const ElfModel &) = delete;

    std::vector<std::unique_ptr<Node>> roots;   // sorted by name, case-insensitive
    bool hasDebug = false;                      // false: only raw symbols are available

private:
    void loadDwarf();
    void loadSymtab();
    void walk(const dw::Die &die, Dwarf_Off parentOff, bool parentIsScope);

    struct Pending {
        QString name;
        quint64 addr;
        dw::Die type;
        dw::Die decl;
    };

    QByteArray data_;      // the whole file, so a rebuild can overwrite it while we run
    Elf *elf_ = nullptr;
    std::unique_ptr<DwarfCtx> ctx_;
    std::vector<Pending> pending_;
};

// Node below `root` whose path is `path` (e.g. "g.items[2].x"), or nullptr.
Node *findNode(Node &root, const QString &path);
