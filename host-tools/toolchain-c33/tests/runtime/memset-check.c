/* Link with crt0.s, test.lds and the actual mini-libc memset object.
 * Compile with -fno-builtin so this exercises the library entry point. */
#include <stddef.h>
#include <string.h>

static unsigned char buffer[4160];

void _runtime_init(void) {}
int _runtime_fini(int status) { return status; }

static int check(size_t offset, size_t length, int value)
{
    size_t i;
    unsigned char *destination = buffer + 16 + offset;
    for (i = 0; i < sizeof(buffer); i++)
        buffer[i] = 0xa5;
    if (memset(destination, value, length) != destination)
        return 1;
    for (i = 0; i < sizeof(buffer); i++) {
        unsigned char expected = i >= 16 + offset &&
            i < 16 + offset + length ? (unsigned char)value : 0xa5;
        if (buffer[i] != expected)
            return 2;
    }
    return 0;
}

int main(void)
{
    static const int values[] = {0, 1, 127, 255, -1, 0x123, 0x100};
    static const size_t large[] = {511, 512, 513, 1023, 1024, 1025,
                                  4095, 4096, 4097};
    size_t offset, length, v;
    for (offset = 0; offset < 8; offset++) {
        for (v = 0; v < sizeof(values) / sizeof(values[0]); v++) {
            for (length = 0; length <= 96; length++)
                if (check(offset, length, values[v])) return 1;
            for (length = 0; length < sizeof(large) / sizeof(large[0]); length++)
                if (check(offset, large[length], values[v])) return 2;
        }
    }
    return 0;
}
