/* Generated programs enter through the production Grifo launcher. */
#include <grifo.h>

extern int dt_main(void);

#ifdef DT_FILE_OUTPUT
static int output;
static unsigned used;
static bool failed;
static char buffer[4095]; /* whole nine-byte result lines */

static void flush_output(void)
{
    if (used && file_write(output, buffer, used) != (ssize_t)used)
        failed = true;
    used = 0;
}

void dt_emit(unsigned int value)
{
    static const char hex[] = "0123456789abcdef";
    int shift;
    if (failed) return;
    for (shift = 28; shift >= 0; shift -= 4)
        buffer[used++] = hex[(value >> shift) & 15];
    buffer[used++] = '\n';
    if (used == sizeof buffer) flush_output();
}
#endif

int grifo_main(int argc, char **argv)
{
    int result;
    debug_print("\nDT_BEGIN\n");
#ifdef DT_FILE_OUTPUT
    output = file_create("difftest.out", FILE_OPEN_WRITE);
    if (output < 0) { debug_print("DT_FAILED\n"); power_off(); }
#endif
    result = dt_main();
#ifdef DT_FILE_OUTPUT
    flush_output();
    if (file_close(output) != FILE_ERROR_OK || failed) result = 1;
#endif
    if (result != 0) {
        debug_print("DT_FAILED\n");
        power_off();
    }
    debug_print("DT_END\n");
    power_off();
}
