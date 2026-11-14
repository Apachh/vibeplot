// Test firmware for the ELF parser: C++ namespaces, classes, bitfields, packed structs, enums,
// multi-dimensional arrays and a struct that is only forward-declared in this translation unit.
#include <stdint.h>

struct Opaque;                       // defined in fixture_b.cpp
extern Opaque *g_opaque_ptr;
extern const int g_shared;

enum Mode : uint8_t { MODE_OFF = 0, MODE_ON = 1, MODE_AUTO = 2 };
enum class Level : int16_t { Low = -1, Mid = 0, High = 5 };

struct Bits {
    uint32_t a : 3;
    uint32_t b : 5;
    int32_t c : 4;
    uint32_t : 4;                    // padding
    uint32_t d : 16;
};

struct __attribute__((packed)) Packed {
    uint8_t tag;
    uint32_t value;                  // unaligned
    uint16_t small;
    Mode mode;
};

struct Base {
    int32_t base_field;
    float ratio;
};

struct Derived : Base {
    uint16_t extra;
    Bits flags;
    union {
        uint32_t whole;
        struct { uint8_t lo, hi; uint16_t rest; };
    };
};

namespace app {
namespace drivers {
struct Uart {
    static uint32_t instances;
    uint32_t baud;
    Mode mode;
    static const uint32_t kMax = 4;
};
uint32_t Uart::instances = 3;
Uart console = {115200, MODE_AUTO};
}  // namespace drivers
int32_t counter = -42;
}  // namespace app

uint8_t matrix[3][4];
int16_t samples[8] = {1, -2, 3};
Derived derived = {{7, 1.5f}, 9, {1, 2, -3, 7}, {0x01020304}};
Packed packed = {1, 0xdeadbeef, 2, MODE_ON};
Level level = Level::High;
Mode mode_var = MODE_ON;
double dval = 3.25;
volatile uint64_t big = 0x1122334455667788ull;
const char *name_ptr = "hello";
bool flag = true;
char ch = 'x';

struct Node2 { int v; Node2 *next; };
Node2 chain[2] = {{1, &chain[1]}, {2, nullptr}};

static int file_static = 11;
int use_static() { static int fn_static = 5; return file_static + (++fn_static); }

int main() { return use_static() + g_shared + matrix[0][0] + (g_opaque_ptr != nullptr); }
extern "C" void _start() { main(); for (;;) {} }
