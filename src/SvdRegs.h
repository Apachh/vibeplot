// CMSIS-SVD parsing and built-in Cortex-M core register definitions. No GUI code.
//
// Supported SVD features: device/peripheral/register/field defaults (size, access), dim arrays
// (%s names, dimIncrement, dimIndex lists and ranges), clusters, derivedFrom for peripherals,
// registers and enumeratedValues, and fields given as bitOffset+bitWidth, lsb+msb or bitRange.
#pragma once

#include "Types.h"

#include <QMap>
#include <QString>

#include <memory>
#include <vector>

namespace svd {

struct Field {
    QString name, desc;
    int off = 0, width = 1;
    EnumMap enumValues;          // empty: none
    bool sideEffect = false;

    quint64 mask() const { return width >= 64 ? ~0ull : (1ull << width) - 1; }
    quint64 extract(quint64 regval) const { return (regval >> off) & mask(); }
};

using FieldList = std::vector<Field>;

struct Register {
    QString name, desc;
    quint64 offset = 0, addr = 0;
    int size = 32;                       // bits
    QString access;
    std::shared_ptr<const FieldList> fields = std::make_shared<FieldList>();
    bool sideEffect = false;

    bool readable() const { return access != "write-only"; }
    bool writable() const { return access != "read-only"; }
    int bytes() const { return size / 8; }
};

struct Peripheral {
    QString name, desc;
    quint64 base = 0;
    std::vector<Register> regs;
    bool builtin = false;
};

struct DeviceInfo {
    QString device, description, cpu;
};

struct LoadResult {
    DeviceInfo info;
    std::vector<std::shared_ptr<Peripheral>> peripherals;
};

// Throws std::runtime_error on unreadable or malformed files.
LoadResult loadSvd(const QString &path);

// System registers shared by Cortex-M cores (SCB, SysTick, NVIC, DWT, CoreDebug).
std::vector<std::shared_ptr<Peripheral>> corePeripherals();

// SVD number: decimal, 0x hex or #binary. Returns false when it is none of them (e.g. "#1x1").
bool parseInt(const QString &text, qint64 &out);

}  // namespace svd
