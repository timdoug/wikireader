/****************************************************************************
 * apps/system/bench/selftest_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/* Every language on the image, run on the device.
 *
 * All of them have been exercised under emulation and none of them on the
 * metal, which after a week in which three bugs turned out to be things no
 * emulator was ever going to model is not the same as knowing they work.
 * The scripts are written here rather than kept on the card so that this
 * needs nothing prepared: it writes them to /tmp, runs each interpreter on
 * one, and every script prints a line beginning "ST" with an answer in it
 * that is wrong if anything on the way was.
 *
 * The Forth is the awkward one. It cannot be given a script on its standard
 * input, because the first thing it does is discard whatever is waiting
 * there -- type-ahead, on a terminal, and the entire program when the
 * terminal is a file. What it does read at startup is forth.ini in the
 * working directory, which is how the metacompiler is driven too, so that
 * is what it gets.
 */

#include <nuttx/config.h>

#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "builtin/builtin.h"
#include "nshlib/nshlib.h"

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct selftest_s
{
  FAR const char *name;
  FAR const char *path;      /* where the script goes */
  FAR const char *text;      /* and what is in it */
  FAR char *const *argv;
  FAR const char *cwd;       /* run it from here, for a script it finds itself */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static FAR char *const g_lua[]    = { "lua", "/tmp/st.lua", NULL };
static FAR char *const g_python[] = { "micropython", "/tmp/st.py", NULL };
static FAR char *const g_bas[]    = { "bas", "/tmp/st.bas", NULL };
static FAR char *const g_tcc[]    = { "tcc", "-run", "/tmp/st.c", NULL };
static FAR char *const g_forth[]  = { "wrforth", NULL };

static const struct selftest_s g_tests[] =
{
  {
    "lua", "/tmp/st.lua",
    "print('ST lua', 6 * 7, math.floor(3.7), ('ab'):rep(2))\n",
    g_lua, NULL
  },
  {
    "micropython", "/tmp/st.py",
    "print('ST micropython', 6 * 7, 2 ** 40, sorted([3, 1, 2]))\n",
    g_python, NULL
  },
  {
    "bas", "/tmp/st.bas",
    "10 PRINT \"ST bas \";6*7\n20 END\n",
    g_bas, NULL
  },
  {
    "tcc", "/tmp/st.c",
    "#include <stdio.h>\n"
    "int main(void)\n"
    "{\n"
    "  printf(\"ST tcc %d\\n\", 6 * 7);\n"
    "  return 0;\n"
    "}\n",
    g_tcc, NULL
  },
  {
    "wrforth", "/tmp/forth.ini",
    ": st .\" ST wrforth \" 6 7 * . cr ;\nst\nbye\n",
    g_forth, "/tmp"
  },
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int selftest_write(FAR const struct selftest_s *test)
{
  FAR FILE *script = fopen(test->path, "w");

  if (script == NULL)
    {
      return -errno;
    }

  fputs(test->text, script);
  fclose(script);
  return OK;
}

static int selftest_run(FAR const struct selftest_s *test)
{
  struct nsh_param_s param;
  int status;
  int ret;
  pid_t pid;

  /* Output where this process's output goes -- the file, when bench is the
   * one that started it -- and the script on the input when that is the
   * only way in.
   */

  memset(&param, 0, sizeof(param));
  param.fd_in  = -1;
  param.fd_out = STDOUT_FILENO;
  param.fd_err = STDOUT_FILENO;

  /* A child inherits the working directory, which is the only way to say
   * where an interpreter that looks for its own script should look.
   */

  if (test->cwd != NULL && chdir(test->cwd) < 0)
    {
      printf("ST %s: cannot enter %s (%d)\n", test->name, test->cwd, errno);
      return -1;
    }

  pid = exec_builtin(test->argv[0], test->argv, &param);
  if (pid < 0)
    {
      printf("ST %s did not start: %d\n", test->name, pid);
      return -1;
    }

  ret = waitpid(pid, &status, 0) < 0 ? -1 : 0;
  if (test->cwd != NULL)
    {
      chdir("/");
    }

  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  int count = sizeof(g_tests) / sizeof(g_tests[0]);
  int failures = 0;
  int i;

  for (i = 0; i < count; i++)
    {
      int ret = selftest_write(&g_tests[i]);

      if (ret < 0)
        {
          printf("ST %s: cannot write %s (%d)\n", g_tests[i].name,
                 g_tests[i].path, -ret);
          failures++;
          continue;
        }

      if (selftest_run(&g_tests[i]) < 0)
        {
          failures++;
        }
    }

  printf("ST %d of %d interpreters ran\n", count - failures, count);
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
