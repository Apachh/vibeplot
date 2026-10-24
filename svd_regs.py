"""CMSIS-SVD parsing and built-in Cortex-M core register definitions.

No GUI code in here: swd_liveplot.py imports this module for its register viewer.
Supported SVD features: device/peripheral/register/field defaults (size, access), dim arrays
(%s names, dimIncrement, dimIndex lists and ranges), clusters, derivedFrom for peripherals,
registers, and enumeratedValues, fields given as bitOffset+bitWidth, lsb+msb or bitRange.
"""
import copy
import re
import xml.etree.ElementTree as ET


def parse_int(text):
    """SVD number: decimal, 0x hex or #binary."""
    t = text.strip()
    if t.startswith("#"):
        return int(t[1:], 2)
    try:
        return int(t, 0)
    except ValueError:
        return int(t, 10)       # decimals with leading zeros


def clean(text):
    return re.sub(r"\s+", " ", text).strip() if text else ""


class Field:
    __slots__ = ("name", "desc", "off", "width", "enum", "side_effect")

    def __init__(self, name, desc, off, width, enum=None, side_effect=False):
        self.name, self.desc, self.off, self.width = name, desc, off, width
        self.enum, self.side_effect = enum, side_effect

    @property
    def mask(self):
        return (1 << self.width) - 1

    def extract(self, regval):
        return (regval >> self.off) & self.mask


class Register:
    __slots__ = ("name", "desc", "offset", "addr", "size", "access", "fields", "side_effect")

    def __init__(self, name, desc, offset, addr, size, access, fields, side_effect=False):
        self.name, self.desc, self.offset, self.addr = name, desc, offset, addr
        self.size, self.access, self.fields, self.side_effect = size, access, fields, side_effect

    @property
    def readable(self):
        return self.access != "write-only"

    @property
    def writable(self):
        return self.access != "read-only"


class Peripheral:
    __slots__ = ("name", "desc", "base", "regs", "builtin")

    def __init__(self, name, desc, base, regs, builtin=False):
        self.name, self.desc, self.base, self.regs, self.builtin = name, desc, base, regs, builtin


# --------------------------------------------------------------------------
# SVD parsing
# --------------------------------------------------------------------------
def _dim_indices(el):
    dim = el.findtext("dim")
    if dim is None:
        return None
    n = parse_int(dim)
    inc = parse_int(el.findtext("dimIncrement") or "0")
    raw = (el.findtext("dimIndex") or "").strip()
    if raw:
        num = re.fullmatch(r"(\d+)\s*-\s*(\d+)", raw)
        let = re.fullmatch(r"([A-Za-z])\s*-\s*([A-Za-z])", raw)
        if "," in raw:
            names = [x.strip() for x in raw.split(",")]
        elif num:
            names = [str(i) for i in range(int(num[1]), int(num[2]) + 1)]
        elif let:
            names = [chr(c) for c in range(ord(let[1]), ord(let[2]) + 1)]
        else:
            names = [raw]
    else:
        names = [str(i) for i in range(n)]
    return [(names[i] if i < len(names) else str(i), i * inc) for i in range(n)]


def _expand(el, name):
    """[(name, address increment)] for an element that may be a dim array."""
    d = _dim_indices(el)
    if d is None:
        return [(name, 0)]
    return [(name.replace("%s", ix), off) for ix, off in d]


def _bits(el):
    bo = el.findtext("bitOffset")
    if bo is not None:
        bw = el.findtext("bitWidth")
        return parse_int(bo), parse_int(bw) if bw is not None else 1
    lsb, msb = el.findtext("lsb"), el.findtext("msb")
    if lsb is not None and msb is not None:
        lo, hi = parse_int(lsb), parse_int(msb)
        return lo, hi - lo + 1
    br = el.findtext("bitRange")
    if br:
        m = re.fullmatch(r"\[\s*(\d+)\s*:\s*(\d+)\s*\]", br.strip())
        if m:
            hi, lo = int(m[1]), int(m[2])
            return lo, hi - lo + 1
    return None


def _enum_map(ev_el, enums_by_name):
    ref = ev_el.get("derivedFrom")
    if ref:
        base = enums_by_name.get(ref.split(".")[-1])
        if base is not None:
            return base
    out = {}
    for e in ev_el.findall("enumeratedValue"):
        nm, val = e.findtext("name"), e.findtext("value")
        if not nm or val is None:
            continue
        try:
            out.setdefault(parse_int(val), nm)
        except ValueError:
            continue            # don't-care patterns such as #1x1
    nm = ev_el.findtext("name")
    if nm:
        enums_by_name[nm] = out
    return out


def _parse_fields(fields_el, ctx):
    out = []
    if fields_el is None:
        return out
    for fel in fields_el.findall("field"):
        name = fel.findtext("name")
        bits = _bits(fel)
        if not name or bits is None:
            continue
        enum = {}
        for ev in fel.findall("enumeratedValues"):
            for k, v in _enum_map(ev, ctx["enums"]).items():
                enum.setdefault(k, v)
        side = bool(clean(fel.findtext("readAction")))
        desc = clean(fel.findtext("description"))
        for nm, add in _expand(fel, name):
            out.append(Field(nm, desc, bits[0] + add, bits[1], enum or None, side))
    out.sort(key=lambda f: f.off)
    return out


def _parse_register(el, base, rel, inh, prefix, ctx):
    name = el.findtext("name")
    if not name:
        return []
    off = parse_int(el.findtext("addressOffset") or "0")
    size = parse_int(el.findtext("size")) if el.findtext("size") else inh["size"]
    access = clean(el.findtext("access")) or inh["access"]
    fields = _parse_fields(el.find("fields"), ctx)
    ref = el.get("derivedFrom")
    if not fields and ref:
        src = ctx["regs"].get(ref.split(".")[-1])
        if src is not None:
            fields = src.fields
    side = bool(clean(el.findtext("readAction"))) or any(f.side_effect for f in fields)
    desc = clean(el.findtext("description"))
    out = []
    for nm, add in _expand(el, name):
        r = Register(prefix + nm, desc, rel + off + add, base + rel + off + add, size, access, fields, side)
        ctx["regs"].setdefault(nm, r)
        out.append(r)
    return out


def _parse_children(parent, base, rel, inh, prefix, ctx, out):
    for el in parent:
        if el.tag == "register":
            out.extend(_parse_register(el, base, rel, inh, prefix, ctx))
        elif el.tag == "cluster":
            cname = el.findtext("name") or "CLUSTER"
            coff = parse_int(el.findtext("addressOffset") or "0")
            inh2 = dict(inh)
            if el.findtext("size"):
                inh2["size"] = parse_int(el.findtext("size"))
            if clean(el.findtext("access")):
                inh2["access"] = clean(el.findtext("access"))
            for nm, add in _expand(el, cname):
                _parse_children(el, base, rel + coff + add, inh2, prefix + nm + ".", ctx, out)


def load_svd(path):
    """Parse an SVD file. Returns (info dict, [Peripheral, ...])."""
    root = ET.parse(path).getroot()
    dev = {"size": parse_int(root.findtext("size") or "32"),
           "access": clean(root.findtext("access")) or "read-write"}
    cpu = root.find("cpu")
    info = {"device": clean(root.findtext("name")), "description": clean(root.findtext("description")),
            "cpu": clean(cpu.findtext("name")) if cpu is not None else ""}
    pels = root.find("peripherals")
    if pels is None:
        raise ValueError("This file has no <peripherals> section - is it a CMSIS-SVD file?")
    pending, order = {}, []
    for pel in pels.findall("peripheral"):
        nm = pel.findtext("name")
        if not nm:
            continue
        for full, add in _expand(pel, nm):
            pending[full] = (pel, add)
            order.append(full)
    enums = {}
    built = {}

    def get(name, depth=0):
        if name in built:
            return built[name]
        pel, add = pending[name]
        base = parse_int(pel.findtext("baseAddress") or "0") + add
        inh = dict(dev)
        if pel.findtext("size"):
            inh["size"] = parse_int(pel.findtext("size"))
        if clean(pel.findtext("access")):
            inh["access"] = clean(pel.findtext("access"))
        regs, desc = [], clean(pel.findtext("description"))
        ref = pel.get("derivedFrom")
        if ref and ref in pending and depth < 20:
            src = get(ref, depth + 1)
            desc = desc or src.desc
            for r in src.regs:
                c = copy.copy(r)
                c.addr = base + r.offset
                regs.append(c)
        rel = pel.find("registers")
        if rel is not None:
            own = []
            _parse_children(rel, base, 0, inh, "", {"enums": enums, "regs": {}}, own)
            names = {r.name for r in own}
            regs = [r for r in regs if r.name not in names] + own
        p = Peripheral(name, desc, base, regs)
        built[name] = p
        return p

    return info, [get(n) for n in order]


# --------------------------------------------------------------------------
# Built-in Cortex-M core registers (not part of most SVD files)
# --------------------------------------------------------------------------
def _f(name, lsb, width=1, desc="", enum=None):
    return Field(name, desc, lsb, width, enum)


def _bitset(names):
    return [_f(n, b) for b, n in names]


def core_peripherals():
    """System registers shared by Cortex-M cores (some are absent on M0/M0+, those read as 0)."""
    def reg(base, name, off, desc, fields=(), access="read-write", side=False):
        return Register(name, desc, off, base + off, 32, access, list(fields), side)

    scb = 0xE000ED00
    cfsr = (_bitset([(0, "IACCVIOL"), (1, "DACCVIOL"), (3, "MUNSTKERR"), (4, "MSTKERR"), (5, "MLSPERR"),
                     (7, "MMARVALID"), (8, "IBUSERR"), (9, "PRECISERR"), (10, "IMPRECISERR"),
                     (11, "UNSTKERR"), (12, "STKERR"), (13, "LSPERR"), (15, "BFARVALID"),
                     (16, "UNDEFINSTR"), (17, "INVSTATE"), (18, "INVPC"), (19, "NOCP"), (20, "STKOF"),
                     (24, "UNALIGNED"), (25, "DIVBYZERO")]))
    scb_regs = [
        reg(scb, "CPUID", 0x00, "CPUID base", [_f("REVISION", 0, 4), _f("PARTNO", 4, 12),
                                               _f("ARCHITECTURE", 16, 4), _f("VARIANT", 20, 4),
                                               _f("IMPLEMENTER", 24, 8)], "read-only"),
        reg(scb, "ICSR", 0x04, "Interrupt control and state",
            [_f("VECTACTIVE", 0, 9, "Active exception number"), _f("RETTOBASE", 11),
             _f("VECTPENDING", 12, 9, "Highest pending exception"), _f("ISRPENDING", 22),
             _f("ISRPREEMPT", 23), _f("PENDSTCLR", 25), _f("PENDSTSET", 26), _f("PENDSVCLR", 27),
             _f("PENDSVSET", 28), _f("NMIPENDSET", 31)]),
        reg(scb, "VTOR", 0x08, "Vector table offset"),
        reg(scb, "AIRCR", 0x0C, "Application interrupt and reset control",
            [_f("VECTRESET", 0), _f("VECTCLRACTIVE", 1), _f("SYSRESETREQ", 2), _f("PRIGROUP", 8, 3),
             _f("ENDIANNESS", 15), _f("VECTKEY", 16, 16)]),
        reg(scb, "SCR", 0x10, "System control",
            [_f("SLEEPONEXIT", 1), _f("SLEEPDEEP", 2), _f("SEVONPEND", 4)]),
        reg(scb, "CCR", 0x14, "Configuration and control",
            [_f("NONBASETHRDENA", 0), _f("USERSETMPEND", 1), _f("UNALIGN_TRP", 3), _f("DIV_0_TRP", 4),
             _f("BFHFNMIGN", 8), _f("STKALIGN", 9)]),
        reg(scb, "SHPR1", 0x18, "System handler priority 1 (MemManage, BusFault, UsageFault)"),
        reg(scb, "SHPR2", 0x1C, "System handler priority 2 (SVCall)"),
        reg(scb, "SHPR3", 0x20, "System handler priority 3 (PendSV, SysTick)"),
        reg(scb, "SHCSR", 0x24, "System handler control and state",
            _bitset([(0, "MEMFAULTACT"), (1, "BUSFAULTACT"), (3, "USGFAULTACT"), (7, "SVCALLACT"),
                     (8, "MONITORACT"), (10, "PENDSVACT"), (11, "SYSTICKACT"), (12, "USGFAULTPENDED"),
                     (13, "MEMFAULTPENDED"), (14, "BUSFAULTPENDED"), (15, "SVCALLPENDED"),
                     (16, "MEMFAULTENA"), (17, "BUSFAULTENA"), (18, "USGFAULTENA")])),
        reg(scb, "CFSR", 0x28, "Configurable fault status (MemManage, BusFault, UsageFault)", cfsr),
        reg(scb, "HFSR", 0x2C, "HardFault status",
            [_f("VECTTBL", 1), _f("FORCED", 30), _f("DEBUGEVT", 31)]),
        reg(scb, "DFSR", 0x30, "Debug fault status",
            _bitset([(0, "HALTED"), (1, "BKPT"), (2, "DWTTRAP"), (3, "VCATCH"), (4, "EXTERNAL")])),
        reg(scb, "MMFAR", 0x34, "MemManage fault address"),
        reg(scb, "BFAR", 0x38, "BusFault address"),
        reg(scb, "AFSR", 0x3C, "Auxiliary fault status"),
    ]
    stk = 0xE000E010
    systick = [
        # reading CTRL clears COUNTFLAG, so it is only read on request
        reg(stk, "CTRL", 0x00, "SysTick control and status (reading clears COUNTFLAG)",
            [_f("ENABLE", 0), _f("TICKINT", 1), _f("CLKSOURCE", 2, 1, "", {0: "external", 1: "processor"}),
             _f("COUNTFLAG", 16)], side=True),
        reg(stk, "LOAD", 0x04, "Reload value", [_f("RELOAD", 0, 24)]),
        reg(stk, "VAL", 0x08, "Current value", [_f("CURRENT", 0, 24)]),
        reg(stk, "CALIB", 0x0C, "Calibration", [_f("TENMS", 0, 24), _f("SKEW", 30), _f("NOREF", 31)],
            "read-only"),
    ]
    nvic_base = 0xE000E100
    nvic = []
    for prefix, start, what in (("ISER", 0x000, "Interrupt set-enable"), ("ICER", 0x080, "Interrupt clear-enable"),
                                ("ISPR", 0x100, "Interrupt set-pending"), ("ICPR", 0x180, "Interrupt clear-pending"),
                                ("IABR", 0x200, "Interrupt active bit")):
        for i in range(8):
            nvic.append(reg(nvic_base, f"{prefix}{i}", start + 4 * i, f"{what} (IRQ {32 * i}..{32 * i + 31})"))
    dwt = 0xE0001000
    dwt_regs = [
        reg(dwt, "CTRL", 0x00, "DWT control", [_f("CYCCNTENA", 0), _f("NUMCOMP", 28, 4)]),
        reg(dwt, "CYCCNT", 0x04, "Cycle counter"),
    ]
    cd = 0xE000EDF0
    coredbg = [
        # DHCSR's S_RESET_ST / S_RETIRE_ST clear on read, and writing needs a debug key
        reg(cd, "DHCSR", 0x00, "Debug halting control and status (read-only here; owned by the debugger)",
            _bitset([(0, "C_DEBUGEN"), (1, "C_HALT"), (2, "C_STEP"), (3, "C_MASKINTS"), (16, "S_REGRDY"),
                     (17, "S_HALT"), (18, "S_SLEEP"), (19, "S_LOCKUP"), (24, "S_RETIRE_ST"),
                     (25, "S_RESET_ST")]), "read-only", True),
        reg(cd, "DEMCR", 0x0C, "Debug exception and monitor control",
            _bitset([(0, "VC_CORERESET"), (4, "VC_MMERR"), (5, "VC_NOCPERR"), (6, "VC_CHKERR"),
                     (7, "VC_STATERR"), (8, "VC_BUSERR"), (9, "VC_INTERR"), (10, "VC_HARDERR"),
                     (16, "MON_EN"), (17, "MON_PEND"), (18, "MON_STEP"), (19, "MON_REQ"), (24, "TRCENA")])),
    ]
    return [
        Peripheral("SCB", "System Control Block (Cortex-M core, built-in)", scb, scb_regs, True),
        Peripheral("SysTick", "SysTick timer (Cortex-M core, built-in)", stk, systick, True),
        Peripheral("NVIC", "Nested Vectored Interrupt Controller (Cortex-M core, built-in)", nvic_base, nvic, True),
        Peripheral("DWT", "Data Watchpoint and Trace (Cortex-M3/M4/M7/M33, built-in)", dwt, dwt_regs, True),
        Peripheral("CoreDebug", "Core debug registers (Cortex-M core, built-in)", cd, coredbg, True),
    ]
