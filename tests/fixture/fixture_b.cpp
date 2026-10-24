#include <stdint.h>
struct Opaque {
    uint32_t id;
    uint8_t flags;
    int16_t levels[3];
};
Opaque g_opaque_storage = {77, 3, {1, 2, 3}};
Opaque *g_opaque_ptr = &g_opaque_storage;
extern const int g_shared = 123;
