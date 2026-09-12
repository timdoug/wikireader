/****************************************************************************
 * apps/interpreters/wrforth/wrforth.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/* Everything the WikiReader Forth knows about the world outside its own
 * dictionary arrives through the calls in this file.  On the device they
 * were the bare-metal drivers: a polled UART, FatFs, the touch controller.
 * Here they are POSIX, so the interpreter reads the card through the same
 * filesystem everything else uses and talks through whichever terminal the
 * task was started on -- the panel, or a serial line, or a pipe.
 *
 * The calling convention is the Forth's, not C's: a pair of cells in
 * %r4/%r5, the first a result and the second a status that the standard
 * calls "ior" and treats as zero for success.  Nothing reads the non-zero
 * values apart from printing them, so they are errno.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define WRFORTH_FILES  8
#define WRFORTH_DIRS   4
#define WRFORTH_NAME   64

/* The file access methods the Forth asks for by name and then hands back. */

#define WRFORTH_R_O    1
#define WRFORTH_W_O    2
#define WRFORTH_R_W    3

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* The 32-bit Forth cell, and the two-cell return the interpreter reads out
 * of %r4 and %r5.  Both must stay exactly this shape.
 */

typedef unsigned long forth_cell_t;

struct forth_return_s
{
  forth_cell_t result;
  forth_cell_t rc;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static FILE *g_files[WRFORTH_FILES];
static DIR  *g_dirs[WRFORTH_DIRS];

/* Written by the entry code before the interpreter starts. */

extern forth_cell_t initial_argument;

/* The interpreter's workspace, which on the metal is part of the image and
 * here is not: five pointers the assembly reads, and the two allocations
 * they point into.  The terminal buffer and the two stacks are one block,
 * laid out the way the image used to lay them out, so the data stack still
 * underflows into the return stack and not into the heap.
 */

extern uint8_t *forth_area_buffer;
extern forth_cell_t forth_area_buflen;
extern uint8_t *forth_area_stack;
extern uint8_t *forth_area_return;
extern uint8_t *forth_area_dict;

static uint8_t *g_workspace;
static uint8_t *g_dictionary;

/* Defined in the interpreter's entry code: leaves its stacks for the one
 * the task started on, then calls Forth_release below.
 */

extern void forth_leave(void);

/* The terminal settings to put back when BYE returns to the shell. */

static struct termios g_saved;
static bool g_restore;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: forth_name
 *
 * Description:
 *   A Forth string is an address and a length with no terminator.  Copy one
 *   into a C string, rejecting anything too long for the buffer rather than
 *   truncating it into the wrong file.
 *
 ****************************************************************************/

static bool forth_name(char *out, size_t size, const void *text,
                       forth_cell_t length)
{
  if (length >= size)
    {
      return false;
    }

  memcpy(out, text, length);
  out[length] = '\0';
  return true;
}

/****************************************************************************
 * Name: forth_file
 ****************************************************************************/

static FILE *forth_file(struct forth_return_s *r, forth_cell_t handle)
{
  if (handle < 1 || handle > WRFORTH_FILES || g_files[handle - 1] == NULL)
    {
      r->rc = EBADF;
      return NULL;
    }

  return g_files[handle - 1];
}

/****************************************************************************
 * Public Functions: the terminal
 ****************************************************************************/

/* Output is buffered, because the interpreter emits a character at a time
 * and each one would otherwise be a write.  Anything that waits for the
 * user flushes first, which covers both the prompt and a half-finished
 * line before a read.
 */

void Serial_PutChar(forth_cell_t c)
{
  putchar((int)(c & 0xff));
}

forth_cell_t Serial_PutReady(void)
{
  return 1;
}

forth_cell_t Serial_GetChar(void)
{
  int c;

  fflush(stdout);
  c = getchar();
  return c == EOF ? (forth_cell_t)-1 : (forth_cell_t)(c & 0xff);
}

forth_cell_t Serial_InputAvailable(void)
{
  struct pollfd fds;

  fflush(stdout);
  fds.fd = STDIN_FILENO;
  fds.events = POLLIN;
  fds.revents = 0;
  return poll(&fds, 1, 0) > 0 && (fds.revents & POLLIN) != 0;
}

void Serial_FlushInput(void)
{
  while (Serial_InputAvailable())
    {
      (void)getchar();
    }
}

/****************************************************************************
 * Public Functions: files
 ****************************************************************************/

void FileSystem_initialise(void)
{
}

void FileSystem_CloseAll(void)
{
  int i;

  for (i = 0; i < WRFORTH_FILES; i++)
    {
      if (g_files[i] != NULL)
        {
          fclose(g_files[i]);
          g_files[i] = NULL;
        }
    }

  for (i = 0; i < WRFORTH_DIRS; i++)
    {
      if (g_dirs[i] != NULL)
        {
          closedir(g_dirs[i]);
          g_dirs[i] = NULL;
        }
    }
}

forth_cell_t FileSystem_ReadOnly(void)
{
  return WRFORTH_R_O;
}

forth_cell_t FileSystem_ReadWrite(void)
{
  return WRFORTH_R_W;
}

forth_cell_t FileSystem_WriteOnly(void)
{
  return WRFORTH_W_O;
}

forth_cell_t FileSystem_bin(forth_cell_t fam)
{
  /* Nothing here translates line endings, so binary and text are the same
   * thing and the standard's BIN modifier has nothing to change.
   */

  return fam;
}

struct forth_return_s FileSystem_open(const void *filename,
                                      forth_cell_t length, forth_cell_t fam)
{
  struct forth_return_s r =
  {
    0, 0
  };

  char name[WRFORTH_NAME];
  int i;

  if (!forth_name(name, sizeof(name), filename, length))
    {
      r.rc = ENAMETOOLONG;
      return r;
    }

  for (i = 0; i < WRFORTH_FILES; i++)
    {
      if (g_files[i] == NULL)
        {
          const char *mode = fam == WRFORTH_R_O ? "rb" :
                             fam == WRFORTH_W_O ? "wb" : "r+b";

          g_files[i] = fopen(name, mode);
          if (g_files[i] == NULL)
            {
              r.rc = errno;
              return r;
            }

          r.result = i + 1;    /* handles start at one, not zero */
          return r;
        }
    }

  r.rc = EMFILE;
  return r;
}

struct forth_return_s FileSystem_create(const void *filename,
                                        forth_cell_t length,
                                        forth_cell_t fam)
{
  struct forth_return_s r =
  {
    0, 0
  };

  char name[WRFORTH_NAME];
  int i;

  if (!forth_name(name, sizeof(name), filename, length))
    {
      r.rc = ENAMETOOLONG;
      return r;
    }

  for (i = 0; i < WRFORTH_FILES; i++)
    {
      if (g_files[i] == NULL)
        {
          /* CREATE-FILE truncates an existing file, and leaves it readable
           * when the access method says so.
           */

          g_files[i] = fopen(name, fam == WRFORTH_R_O ? "rb" :
                                   fam == WRFORTH_W_O ? "wb" : "w+b");
          if (g_files[i] == NULL)
            {
              r.rc = errno;
              return r;
            }

          r.result = i + 1;
          return r;
        }
    }

  r.rc = EMFILE;
  return r;
}

struct forth_return_s FileSystem_close(forth_cell_t handle)
{
  struct forth_return_s r =
  {
    0, 0
  };

  if (forth_file(&r, handle) != NULL)
    {
      if (fclose(g_files[handle - 1]) != 0)
        {
          r.rc = errno;
        }

      g_files[handle - 1] = NULL;
    }

  return r;
}

struct forth_return_s FileSystem_read(forth_cell_t handle, void *buffer,
                                      forth_cell_t length)
{
  struct forth_return_s r =
  {
    0, 0
  };

  FILE *file = forth_file(&r, handle);

  if (file != NULL)
    {
      r.result = fread(buffer, 1, length, file);
      if (r.result != length && ferror(file))
        {
          r.rc = errno;
        }
    }

  return r;
}

struct forth_return_s FileSystem_write(forth_cell_t handle, void *buffer,
                                       forth_cell_t length)
{
  struct forth_return_s r =
  {
    0, 0
  };

  FILE *file = forth_file(&r, handle);

  if (file != NULL)
    {
      r.result = fwrite(buffer, 1, length, file);
      if (r.result != length)
        {
          r.rc = errno;
        }
    }

  return r;
}

struct forth_return_s FileSystem_sync(forth_cell_t handle)
{
  struct forth_return_s r =
  {
    0, 0
  };

  FILE *file = forth_file(&r, handle);

  if (file != NULL && fflush(file) != 0)
    {
      r.rc = errno;
    }

  return r;
}

struct forth_return_s FileSystem_lseek(forth_cell_t handle, forth_cell_t pos)
{
  struct forth_return_s r =
  {
    0, 0
  };

  FILE *file = forth_file(&r, handle);

  if (file != NULL && fseek(file, (long)pos, SEEK_SET) != 0)
    {
      r.rc = errno;
    }

  return r;
}

struct forth_return_s FileSystem_ltell(forth_cell_t handle)
{
  struct forth_return_s r =
  {
    0, 0
  };

  FILE *file = forth_file(&r, handle);

  if (file != NULL)
    {
      long pos = ftell(file);

      if (pos < 0)
        {
          r.rc = errno;
        }
      else
        {
          r.result = pos;
        }
    }

  return r;
}

struct forth_return_s FileSystem_lsize(forth_cell_t handle)
{
  struct forth_return_s r =
  {
    0, 0
  };

  FILE *file = forth_file(&r, handle);

  if (file != NULL)
    {
      struct stat info;

      if (fstat(fileno(file), &info) < 0)
        {
          r.rc = errno;
        }
      else
        {
          r.result = info.st_size;
        }
    }

  return r;
}

struct forth_return_s FileSystem_delete(const void *filename,
                                        forth_cell_t length)
{
  struct forth_return_s r =
  {
    0, 0
  };

  char name[WRFORTH_NAME];

  if (!forth_name(name, sizeof(name), filename, length))
    {
      r.rc = ENAMETOOLONG;
    }
  else if (unlink(name) < 0)
    {
      r.rc = errno;
    }

  return r;
}

struct forth_return_s FileSystem_rename(const void *oldname,
                                        forth_cell_t oldlength,
                                        const void *newname,
                                        forth_cell_t newlength)
{
  struct forth_return_s r =
  {
    0, 0
  };

  char from[WRFORTH_NAME];
  char to[WRFORTH_NAME];

  if (!forth_name(from, sizeof(from), oldname, oldlength) ||
      !forth_name(to, sizeof(to), newname, newlength))
    {
      r.rc = ENAMETOOLONG;
    }
  else if (rename(from, to) < 0)
    {
      r.rc = errno;
    }

  return r;
}

struct forth_return_s FileSystem_CreateDirectory(const void *dirname,
                                                 forth_cell_t length)
{
  struct forth_return_s r =
  {
    0, 0
  };

  char name[WRFORTH_NAME];

  if (!forth_name(name, sizeof(name), dirname, length))
    {
      r.rc = ENAMETOOLONG;
    }
  else if (mkdir(name, 0777) < 0)
    {
      r.rc = errno;
    }

  return r;
}

struct forth_return_s FileSystem_OpenDirectory(const void *dirname,
                                               forth_cell_t length)
{
  struct forth_return_s r =
  {
    0, 0
  };

  char name[WRFORTH_NAME];
  int i;

  if (!forth_name(name, sizeof(name), dirname, length))
    {
      r.rc = ENAMETOOLONG;
      return r;
    }

  for (i = 0; i < WRFORTH_DIRS; i++)
    {
      if (g_dirs[i] == NULL)
        {
          g_dirs[i] = opendir(name);
          if (g_dirs[i] == NULL)
            {
              r.rc = errno;
              return r;
            }

          r.result = i + 1;
          return r;
        }
    }

  r.rc = EMFILE;
  return r;
}

struct forth_return_s FileSystem_CloseDirectory(forth_cell_t handle)
{
  struct forth_return_s r =
  {
    0, 0
  };

  if (handle < 1 || handle > WRFORTH_DIRS || g_dirs[handle - 1] == NULL)
    {
      r.rc = EBADF;
      return r;
    }

  closedir(g_dirs[handle - 1]);
  g_dirs[handle - 1] = NULL;
  return r;
}

struct forth_return_s FileSystem_ReadDirectory(forth_cell_t handle,
                                               void *buffer,
                                               forth_cell_t length)
{
  struct forth_return_s r =
  {
    0, 0
  };

  struct dirent *entry;

  if (handle < 1 || handle > WRFORTH_DIRS || g_dirs[handle - 1] == NULL)
    {
      r.rc = EBADF;
      return r;
    }

  /* A zero count is how the interpreter is told the directory has ended. */

  entry = readdir(g_dirs[handle - 1]);
  if (entry != NULL)
    {
      size_t count = strlen(entry->d_name);

      if (count > length)
        {
          count = length;
        }

      memcpy(buffer, entry->d_name, count);
      r.result = count;
    }

  return r;
}

/* Raw sector access belongs to a Forth that owns the card.  This one shares
 * it with a mounted filesystem, and writing behind that filesystem's back
 * would corrupt it, so both refuse rather than pretend.
 */

struct forth_return_s FileSystem_AbsoluteRead(forth_cell_t sector,
                                              void *buffer,
                                              forth_cell_t count)
{
  struct forth_return_s r =
  {
    0, EPERM
  };

  return r;
}

struct forth_return_s FileSystem_AbsoluteWrite(forth_cell_t sector,
                                               const void *buffer,
                                               forth_cell_t count)
{
  struct forth_return_s r =
  {
    0, EPERM
  };

  return r;
}

/****************************************************************************
 * Public Functions: the rest of the machine
 ****************************************************************************/

/* The hardware words are answered rather than removed, so a program that
 * asks gets a defined answer instead of a link error.  Where the operating
 * system has the thing, it is used; where it would mean reaching past a
 * driver that owns the device, it reads as absent.
 */

forth_cell_t Tick_get(void)
{
  struct timespec now;

  clock_gettime(CLOCK_MONOTONIC, &now);
  return now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

void delay_us(forth_cell_t us)
{
  usleep(us);
}

forth_cell_t BoardRevision_get(void)
{
  return 0;
}

forth_cell_t Button_available(void)
{
  return 0;
}

forth_cell_t Button_get(void)
{
  return 0;
}

void Button_flush(void)
{
}

void Button_poll(void)
{
}

forth_cell_t CTP_PositionAvailable(void)
{
  return 0;
}

forth_cell_t CTP_GetPosition(void *position)
{
  return 0;
}

void CTP_flush(void)
{
}

forth_cell_t Analog_scan(void)
{
  return 0;
}

forth_cell_t Analog_BatteryMilliVolts(void)
{
  return 0;
}

forth_cell_t Analog_ContrastMilliVolts(void)
{
  return 0;
}

forth_cell_t Analog_TemperatureCelcius(void)
{
  return 0;
}

void Temperature_control(forth_cell_t enable)
{
}

forth_cell_t Contrast_get(void)
{
  return 0;
}

void Contrast_set(forth_cell_t value)
{
}

void suspend(void)
{
}

/* The serial FLASH shares the SPI bus with the card, which has a driver of
 * its own now; going behind it would mean moving the chip select while a
 * block transfer is in flight.
 */

forth_cell_t FLASH_read(forth_cell_t address, void *buffer,
                        forth_cell_t length)
{
  return 0;
}

forth_cell_t FLASH_write(forth_cell_t address, const void *buffer,
                         forth_cell_t length)
{
  return 0;
}

forth_cell_t FLASH_verify(forth_cell_t address, const void *buffer,
                          forth_cell_t length)
{
  return 0;
}

void FLASH_WriteEnable(forth_cell_t enable)
{
}

void FLASH_SectorErase(forth_cell_t address)
{
}

void FLASH_ChipErase(void)
{
}

void FLASH_SelectInternal(void)
{
}

void FLASH_SelectExternal(void)
{
}

void Debug_PutString(const char *string)
{
  fputs(string, stderr);
}

void xdebug(void)
{
}

/****************************************************************************
 * Public Functions: starting and stopping
 ****************************************************************************/

/****************************************************************************
 * Name: Forth_initialise
 *
 * Description:
 *   Called from the entry code once the Forth stacks are in place and
 *   before the interpreter starts.  The cold argument decides which script
 *   the interpreter looks for: zero means forth.ini, the way the device
 *   boots, and anything else means forth.tst.
 *
 ****************************************************************************/

void Forth_initialise(int argc, char **argv)
{
  size_t buffer = CONFIG_INTERPRETERS_WRFORTH_BUFFERSIZE;
  size_t stack  = CONFIG_INTERPRETERS_WRFORTH_STACKSIZE_FORTH;

  g_workspace = malloc(buffer + 2 * stack);
  g_dictionary = malloc(CONFIG_INTERPRETERS_WRFORTH_DICTSIZE);
  if (g_workspace == NULL || g_dictionary == NULL)
    {
      fprintf(stderr, "wrforth: no room for the interpreter's workspace\n");
      exit(EXIT_FAILURE);
    }

  forth_area_buffer = g_workspace;
  forth_area_buflen = buffer;

  /* Both stacks grow downwards from their top. */

  forth_area_stack  = g_workspace + buffer + stack;
  forth_area_return = g_workspace + buffer + 2 * stack;
  forth_area_dict   = g_dictionary;

  initial_argument = argc > 1 && strcmp(argv[1], "-t") == 0;
}

/****************************************************************************
 * Name: Forth_exit
 *
 * Description:
 *   BYE.  Nothing unwinds: the task's own stack was left behind when the
 *   interpreter took over the stack pointer, so there is nothing to return
 *   through.
 *
 ****************************************************************************/

void Forth_exit(void)
{
  FileSystem_CloseAll();
  fflush(NULL);
  if (g_restore)
    {
      tcsetattr(STDIN_FILENO, TCSANOW, &g_saved);
    }

  /* Nothing frees the workspace from here: the return stack is inside it,
   * and this function is standing on that.  forth_leave() steps off first.
   */

  forth_leave();
}

/****************************************************************************
 * Name: Forth_release
 *
 * Description:
 *   Reached from forth_leave(), on the task's own stack, with nothing of
 *   the interpreter's left in use.  A task that exits does not give its
 *   allocations back by itself here, so this does.
 *
 ****************************************************************************/

void Forth_release(void)
{
  free(g_workspace);
  free(g_dictionary);
  exit(EXIT_SUCCESS);
}

/****************************************************************************
 * Name: wrforth_raw
 *
 * Description:
 *   The interpreter has a line editor of its own -- it echoes what it
 *   accepts, erases over a backspace, and takes either carriage return or
 *   line feed as the end of a line.  It therefore wants the characters as
 *   typed: with the terminal's own echo and line discipline left on,
 *   everything appears twice, and on a slow console the echoing costs
 *   enough time to lose input.
 *
 ****************************************************************************/

static void wrforth_raw(void)
{
  struct termios raw;

  if (tcgetattr(STDIN_FILENO, &g_saved) < 0)
    {
      return;    /* not a terminal: a pipe or a file needs nothing done */
    }

  raw = g_saved;
  raw.c_lflag &= ~(ECHO | ECHONL | ICANON);
  raw.c_iflag &= ~(ICRNL | INLCR | IGNCR);
  raw.c_oflag &= ~OPOST;      /* the interpreter writes its own CR LF */
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;
  g_restore = tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0;
}

/****************************************************************************
 * Name: main
 ****************************************************************************/

extern void forth_entry(int argc, char **argv);

int main(int argc, char *argv[])
{
  wrforth_raw();
  forth_entry(argc, argv);
  return 0;
}
