/****************************************************************************
 * apps/interpreters/micropython/micropython_main.c
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

/* With no arguments this is the REPL; with a path it runs that file and
 * stops.  The garbage collector's heap comes from the operating system and
 * goes back to it, so an interpreter nobody starts costs nothing.
 */

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include "py/compile.h"
#include "py/builtin.h"
#include "py/lexer.h"
#include "py/gc.h"
#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/runtime.h"
#include "py/stackctrl.h"
#include "extmod/vfs.h"
#include "extmod/vfs_posix.h"
#include "shared/runtime/gchelper.h"
#include "shared/runtime/pyexec.h"

static char *g_heap;
static struct termios g_saved;
static bool g_restore;

/****************************************************************************
 * Name: micropython_raw
 *
 * Description:
 *   The interpreter's line editor echoes what it accepts and redraws the
 *   line itself, so it wants the characters as typed.  Leaving the
 *   terminal's own echo and line discipline on gets everything twice and
 *   hides the prompt behind a line the terminal has already drawn.
 *
 ****************************************************************************/

static void micropython_raw(void)
{
  struct termios raw;

  if (tcgetattr(STDIN_FILENO, &g_saved) < 0)
    {
      return;      /* a pipe or a file needs nothing done */
    }

  raw = g_saved;
  raw.c_lflag &= ~(ECHO | ECHONL | ICANON);
  raw.c_iflag &= ~(ICRNL | INLCR | IGNCR);
  raw.c_oflag &= ~OPOST;     /* the interpreter writes its own CR LF */
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;
  g_restore = tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0;
}

/* What help() prints when called with nothing to describe. */

const char micropython_help_text[] =
  "Welcome to MicroPython on the WikiReader.\n"
  "\n"
  "For a list of modules, type help('modules').\n"
  "For details on a module, object or method, pass it to help().\n"
  "Control-D on an empty line leaves the interpreter.\n";

void gc_collect(void)
{
  gc_collect_start();
  gc_helper_collect_regs_and_stack();
  gc_collect_end();
}

/****************************************************************************
 * Name: micropython_mount
 *
 * Description:
 *   Put NuttX's filesystem where MicroPython expects its own.  Everything
 *   the interpreter opens -- scripts, imports, sys.path -- goes through the
 *   mount table, so with the POSIX filesystem object mounted at the root a
 *   path means here what it means at the shell prompt.
 *
 ****************************************************************************/

static void micropython_mount(void)
{
  mp_obj_t args[2];

  args[0] = MP_OBJ_TYPE_GET_SLOT(&mp_type_vfs_posix, make_new)
              (&mp_type_vfs_posix, 0, 0, NULL);
  args[1] = MP_OBJ_NEW_QSTR(MP_QSTR__slash_);

  mp_vfs_mount(2, args, (mp_map_t *)&mp_const_empty_map);
  MP_STATE_VM(vfs_cur) = MP_STATE_VM(vfs_mount_table);
}

int main(int argc, char *argv[])
{
  size_t heap = CONFIG_INTERPRETERS_MICROPYTHON_HEAPSIZE;
  int status = 0;

  g_heap = malloc(heap);
  if (g_heap == NULL)
    {
      fprintf(stderr, "micropython: no room for a %zu byte heap\n", heap);
      return EXIT_FAILURE;
    }

  /* The collector walks the C stack looking for references, so it has to
   * be told where the top of it is before anything else happens.
   */

  mp_stack_ctrl_init();
  mp_stack_set_limit(CONFIG_INTERPRETERS_MICROPYTHON_STACKSIZE - 1024);

  gc_init(g_heap, g_heap + heap);
  mp_init();
  micropython_mount();

  if (argc > 1)
    {
      status = pyexec_file_if_exists(argv[1]) ? 0 : 1;
    }
  else
    {
      micropython_raw();
      pyexec_friendly_repl();
      if (g_restore)
        {
          tcsetattr(STDIN_FILENO, TCSANOW, &g_saved);
        }
    }

  mp_deinit();
  free(g_heap);
  return status;
}
