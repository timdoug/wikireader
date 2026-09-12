/* MicroPython configuration for the WikiReader.
 *
 * A 48 MHz chip with no floating-point unit and no MMU, so: single-precision
 * floats rather than double, the portable setjmp paths for both non-local
 * return and register spilling, and no native code emitter -- there is no
 * C33 backend for one, and the bytecode VM is the point here anyway.
 *
 * The garbage collector gets its heap from the operating system rather than
 * from a static array, so it costs nothing until somebody runs the thing.
 */

#include <alloca.h>
#include <stdint.h>

#define MICROPY_CONFIG_ROM_LEVEL       (MICROPY_CONFIG_ROM_LEVEL_EXTRA_FEATURES)

/* A flat build has one symbol namespace, and NuttX brings its own readline()
 * for the shell -- a different function with a different signature.  The
 * linker picked that one, so the REPL was calling the shell's line editor
 * with a vstr where it wanted a buffer.  Keep MicroPython's to itself.
 */

#define readline                       mp_readline
#define readline_init                  mp_readline_init
#define readline_init0                 mp_readline_init0
#define readline_process_char          mp_readline_process_char
#define readline_push_history          mp_readline_push_history

/* No architecture support for either, so take the portable route through
 * setjmp -- which this architecture only has because Toybox needed it.
 */

#define MICROPY_NLR_SETJMP             (1)
#define MICROPY_GCREGS_SETJMP          (1)
#define MICROPY_EMIT_X64               (0)
#define MICROPY_EMIT_THUMB             (0)
#define MICROPY_EMIT_ARM               (0)

#define MICROPY_ENABLE_GC              (1)
#define MICROPY_ENABLE_FINALISER       (1)
#define MICROPY_ENABLE_COMPILER        (1)
#define MICROPY_HELPER_REPL            (1)
#define MICROPY_REPL_AUTO_INDENT       (1)
#define MICROPY_ENABLE_EXTERNAL_IMPORT (1)
#define MICROPY_ENABLE_SOURCE_LINE     (1)
#define MICROPY_KBD_EXCEPTION          (1)

/* Software floating point makes a double twice the work for no gain at this
 * word size; single precision is what the hardware would have had.
 */

#define MICROPY_FLOAT_IMPL             (MICROPY_FLOAT_IMPL_FLOAT)

/* Without this an integer is 31 bits and 2**30 overflows, which is not the
 * Python anyone means.
 */

#define MICROPY_LONGINT_IMPL           (MICROPY_LONGINT_IMPL_MPZ)

/* Files and modules come off whatever NuttX has mounted.  MicroPython
 * normally owns its own filesystems, so the way to give it somebody else's
 * is its POSIX filesystem object, mounted at the root: open(), os.listdir()
 * and import all then go through read() and write() on a NuttX descriptor,
 * and a script on the card is reached by the path it has in the shell.
 */

#define MICROPY_VFS                    (1)
#define MICROPY_VFS_POSIX              (1)
#define MICROPY_READER_VFS             (1)
#define MICROPY_PY_SYS_PLATFORM        "nuttx"
#define MICROPY_PY_SYS_ARGV            (1)
#define MICROPY_PY_SYS_EXIT            (1)
#define MICROPY_PY_SYS_PATH            (1)
#define MICROPY_PY_SYS_STDFILES        (1)
#define MICROPY_PY_BUILTINS_INPUT      (1)
#define MICROPY_PY_BUILTINS_HELP       (1)
#define MICROPY_PY_BUILTINS_HELP_TEXT  micropython_help_text
#define MICROPY_PY_TIME                (1)
#define MICROPY_PY_TIME_INCLUDEFILE    "modtime_port.c"
#define MICROPY_EPOCH_IS_1970          (1)
#define MICROPY_PY_TIME_TIME_TIME_NS   (1)
#define MICROPY_PY_OS                  (1)

/* NuttX's libm has no gamma or error functions, and this is not the place
 * to write them.
 */

#define MICROPY_PY_MATH_SPECIAL_FUNCTIONS (0)

#define MICROPY_ALLOC_PATH_MAX         (256)
#define MICROPY_USE_INTERNAL_ERRNO     (0)

typedef long mp_off_t;

#define MICROPY_HW_BOARD_NAME          "WikiReader"
#define MICROPY_HW_MCU_NAME            "S1C33E07"

#define MP_STATE_PORT MP_STATE_VM
