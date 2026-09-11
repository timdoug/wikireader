/****************************************************************************
 * boards/c33/s1c33e07/wikireader/src/wikireader_terminal.c
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

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <pty.h>
#include <semaphore.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <termios.h>
#include <unistd.h>
#include <nuttx/atomic.h>
#include <nuttx/input/touchscreen.h>
#include <nuttx/kthread.h>
#include <nuttx/nx/nx.h>
#include <nuttx/nx/nxfonts.h>
#include <nuttx/nx/nxterm.h>
#include <nuttx/video/fb.h>
#include "wikireader.h"

/****************************************************************************
 * Preprocessor Definitions
 ****************************************************************************/

#define WR_FONT_WIDTH  6
#define WR_FONT_HEIGHT 9
#define WR_KEY_COUNT   37
#define WR_SHIFT_KEY   20
#define WR_CTRL_KEY    30
#define WR_MODE_KEY    31

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct wr_keyimage_s
{
  char label[8];
  bool inverse;
  bool valid;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static NXHANDLE g_nx;
static NXWINDOW g_termwnd;
static NXWINDOW g_keywnd;
static NXTERM g_term;
static sem_t g_connected;
static sem_t g_synched;
static pthread_mutex_t g_keylock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t g_glyph[2][95][WR_FONT_HEIGHT];
static struct wr_keyimage_s g_keyimage[WR_KEY_COUNT];
static bool g_shift;
static bool g_control;
static bool g_symbols;
static bool g_listener_failed;
static int g_active = -1;
static atomic_t g_shellpid;
static const char *g_stage = "console";
static int g_master;
static int g_termfd;
static int g_touchfd;
static int g_serialin;
static int g_serialout;

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

void nsh_initialize(void);
int nsh_consolemain(int argc, char **argv);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int wr_writeall(int fd, const void *data, size_t len)
{
  const uint8_t *bytes = data;
  ssize_t n;

  while (len > 0)
    {
      n = write(fd, bytes, len);
      if (n < 0 && errno == EINTR)
        {
          continue;
        }

      if (n <= 0)
        {
          return -1;
        }

      bytes += n;
      len -= n;
    }

  return 0;
}

static void wr_input(const char *data, size_t len)
{
  pid_t foreground;
  int space;

  /* Keep the display/input worker responsive when a foreground application
   * stops reading.  A key is dropped if the bounded PTY input queue is full.
   * Control-C bypasses this check: the PTY delivers SIGINT before writing
   * to its pipe, so it can still interrupt a blocked foreground task.
   */

  if (len == 1 && data[0] == CONFIG_TTY_SIGINT_CHAR)
    {
      foreground = 0;
      if (ioctl(STDIN_FILENO, TIOCGPGRP, &foreground) < 0 &&
          errno == ENOTTY)
        {
          /* NSH detaches its controlling terminal after a foreground
           * application exits.  Restore the interactive shell as owner.
           */

          foreground = atomic_read(&g_shellpid);
          ioctl(STDIN_FILENO, TIOCSCTTY, foreground);
        }

      write(g_master, data, len);
      if (foreground == atomic_read(&g_shellpid))
        {
          /* NSH's own commands run in the shell task.  Its signal handler
           * interrupts sleeps; Ctrl-U and newline clear an unfinished line.
           */

          wr_input("\005\025\n", 3);
        }
    }
  else if (ioctl(g_master, FIONSPACE, &space) == 0 && space >= (int)len)
    {
      write(g_master, data, len);
    }
}

static void wr_keyrect(int key, struct nxgl_rect_s *rect)
{
  int row = key / 10;
  int col = key % 10;
  int width = 24;

  if (key >= 30)
    {
      static const uint8_t columns[7] =
      {
        0, 1, 2, 3, 6, 7, 8
      };

      col = columns[key - 30];
      width = key == 33 ? 72 : key == 36 ? 48 : 24;
    }

  rect->pt1.x = col * 24;
  rect->pt1.y = row * WR_KEY_HEIGHT;
  rect->pt2.x = rect->pt1.x + width - 1;
  rect->pt2.y = rect->pt1.y + WR_KEY_HEIGHT - 1;
}

static int wr_hitkey(int x, int y)
{
  int col;
  int row;

  if (x < 0 || x >= WR_WIDTH || y < WR_TERM_HEIGHT || y >= WR_HEIGHT)
    {
      return -1;
    }

  col = x / 24;
  row = (y - WR_TERM_HEIGHT) / WR_KEY_HEIGHT;
  if (row < 3)
    {
      return row * 10 + col;
    }

  if (col < 3)
    {
      return 30 + col;
    }

  return col < 6 ? 33 : col < 8 ? col + 28 : 36;
}

static char wr_keychar(int key)
{
  static const char *const letters[3] =
  {
    "qwertyuiop", "asdfghjkl", " zxcvbnm.?"
  };

  static const char *const symbols[2][3] =
  {
    {"1234567890", "-/:;()$&@", " \\\"'=+*_?!"},
    {"!@#$%^&*()", "[]{}<>|~`", " \\\"'=+_,.:"}
  };

  char ch;

  if (key == 19)
    {
      return '\b';
    }

  ch = g_symbols ? symbols[g_shift][key / 10][key % 10] :
                  letters[key / 10][key % 10];
  if (!g_symbols && g_shift)
    {
      if (ch >= 'a' && ch <= 'z')
        {
          ch -= 'a' - 'A';
        }
      else if (ch == '.')
        {
          ch = '>';
        }
    }

  return ch;
}

static void wr_label(int key, char *label)
{
  const char *text = NULL;

  switch (key)
    {
      case 19:
        text = "BS";
        break;
      case WR_SHIFT_KEY:
        text = "Sh";
        break;
      case WR_CTRL_KEY:
        text = "Ctl";
        break;
      case WR_MODE_KEY:
        text = g_symbols ? "ABC" : "123";
        break;
      case 32:
        text = "Tab";
        break;
      case 33:
        text = "Space";
        break;
      case 34:
        text = "<";
        break;
      case 35:
        text = ">";
        break;
      case 36:
        text = "Enter";
        break;
      default:
        label[0] = wr_keychar(key);
        label[1] = 0;
        return;
    }

  strlcpy(label, text, 8);
}

static void wr_paintkey(int key, bool force)
{
  struct wr_keyimage_s *image = &g_keyimage[key];
  struct nxgl_rect_s rect;
  struct nxgl_rect_s glyphrect;
  struct nxgl_point_s origin;
  const void *source[1];
  nxgl_mxpixel_t color[1];
  char label[8];
  bool inverse;
  int i;
  int x;
  int y;

  inverse = key == g_active || (key == WR_SHIFT_KEY && g_shift) ||
            (key == WR_CTRL_KEY && g_control) ||
            (key == WR_MODE_KEY && g_symbols);
  wr_label(key, label);
  if (!force && image->valid && image->inverse == inverse &&
      strcmp(image->label, label) == 0)
    {
      return;
    }

  /* Cache the appearance queued to NX, under g_keylock.  A normal tap only
   * changes that key's interior; borders need painting on initial display
   * or when NX reports that the window needs redrawing.
   */

  wr_keyrect(key, &rect);
  force |= !image->valid;
  image->valid = false;
  if (force)
    {
      color[0] = 1;
      if (nx_fill(g_keywnd, &rect, color) < 0)
        {
          return;
        }
    }

  rect.pt1.x++;
  rect.pt1.y++;
  rect.pt2.x--;
  rect.pt2.y--;
  color[0] = inverse;
  if (nx_fill(g_keywnd, &rect, color) < 0)
    {
      return;
    }

  x = (rect.pt1.x + rect.pt2.x + 1 - strlen(label) * WR_FONT_WIDTH) / 2;
  y = (rect.pt1.y + rect.pt2.y + 1 - WR_FONT_HEIGHT) / 2;
  for (i = 0; label[i] != 0; i++)
    {
      origin.x = x + i * WR_FONT_WIDTH;
      origin.y = y;
      glyphrect.pt1 = origin;
      glyphrect.pt2.x = origin.x + WR_FONT_WIDTH - 1;
      glyphrect.pt2.y = origin.y + WR_FONT_HEIGHT - 1;
      source[0] = g_glyph[inverse][(unsigned char)label[i] - 32];
      if (nx_bitmap(g_keywnd, &glyphrect, source, &origin, 1) < 0)
        {
          return;
        }
    }

  strlcpy(image->label, label, sizeof(image->label));
  image->inverse = inverse;
  image->valid = true;
}

static void wr_paintkeys(const struct nxgl_rect_s *damage)
{
  struct nxgl_rect_s rect;
  int key;

  for (key = 0; key < WR_KEY_COUNT; key++)
    {
      if (damage != NULL)
        {
          wr_keyrect(key, &rect);
          if (!nxgl_intersecting(&rect, damage))
            {
              continue;
            }
        }

      wr_paintkey(key, damage != NULL);
    }
}

static void wr_redraw(NXWINDOW hwnd, const struct nxgl_rect_s *rect,
                      bool more, void *arg)
{
  if (arg != NULL && g_keywnd != NULL)
    {
      pthread_mutex_lock(&g_keylock);
      wr_paintkeys(rect);
      pthread_mutex_unlock(&g_keylock);
    }
  else if (g_term != NULL)
    {
      struct nxtermioc_redraw_s redraw;

      redraw.handle = g_term;
      redraw.rect = *rect;
      redraw.more = more;
      nxterm_ioctl_tap(NXTERMIOC_NXTERM_REDRAW, (uintptr_t)&redraw);
    }
}

static void wr_position(NXWINDOW hwnd, const struct nxgl_size_s *size,
                        const struct nxgl_point_s *pos,
                        const struct nxgl_rect_s *bounds, void *arg)
{
}

static void wr_event(NXWINDOW hwnd, enum nx_event_e event,
                     void *arg1, void *arg2)
{
  if (event == NXEVENT_SYNCHED && arg2 != NULL)
    {
      sem_post(arg2);
    }
}

static const struct nx_callback_s g_callbacks =
{
  .redraw = wr_redraw,
  .position = wr_position,
  .event = wr_event
};

static void *wr_listener(void *arg)
{
  bool connected = false;

  for (; ; )
    {
      if (nx_eventhandler(g_nx) < 0)
        {
          g_listener_failed = true;
          sem_post(&g_connected);
          sem_post(&g_synched);
          return NULL;
        }

      if (!connected)
        {
          connected = true;
          sem_post(&g_connected);
        }
    }
}

static int wr_wait(sem_t *sem)
{
  struct timespec deadline;
  int ret;

  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += 5;
  do
    {
      ret = sem_timedwait(sem, &deadline);
    }
  while (ret < 0 && errno == EINTR);
  return g_listener_failed ? -1 : ret;
}

static int wr_server(int argc, char **argv)
{
  return nx_run(up_fbgetvplane(0, 0));
}

static void wr_keypress(int key)
{
  char output[3];
  size_t len = 1;

  if (key == WR_SHIFT_KEY)
    {
      g_shift = !g_shift;
    }
  else if (key == WR_CTRL_KEY)
    {
      g_control = !g_control;
    }
  else if (key == WR_MODE_KEY)
    {
      g_symbols = !g_symbols;
      g_shift = false;
    }
  else
    {
      if (key == 34 || key == 35)
        {
          memcpy(output, key == 34 ? "\033[D" : "\033[C", 3);
          len = 3;
        }
      else
        {
          output[0] = key == 32 ? '\t' : key == 33 ? ' ' :
                      key == 36 ? '\n' : wr_keychar(key);
          if (g_control && (output[0] == ' ' ||
                            (output[0] >= '@' && output[0] <= '_') ||
                            (output[0] >= 'a' && output[0] <= 'z')))
            {
              output[0] &= 0x1f;
            }
        }

      wr_input(output, len);
      g_shift = false;
      g_control = false;
    }
}

static void wr_touch(const struct touch_point_s *point)
{
  int hit = (point->flags & TOUCH_POS_VALID) != 0 ?
            wr_hitkey(point->x, point->y) : -1;
  int previous;

  pthread_mutex_lock(&g_keylock);
  previous = g_active;
  if ((point->flags & TOUCH_DOWN) != 0)
    {
      g_active = hit;
      if (previous >= 0)
        {
          wr_paintkey(previous, false);
        }

      if (g_active >= 0)
        {
          wr_paintkey(g_active, false);
        }
    }
  else if ((point->flags & TOUCH_UP) != 0)
    {
      g_active = -1;
      if (previous >= 0 && hit == previous)
        {
          wr_keypress(previous);
          wr_paintkeys(NULL);
        }
      else if (previous >= 0)
        {
          wr_paintkey(previous, false);
        }
    }
  else if ((point->flags & TOUCH_MOVE) != 0 && hit != g_active)
    {
      /* Sliding off a key cancels the tap; a held contact cannot emit
       * repeated characters or accidentally select an adjacent key.
       */

      g_active = -1;
      if (previous >= 0)
        {
          wr_paintkey(previous, false);
        }
    }

  pthread_mutex_unlock(&g_keylock);
}

static void *wr_bridge(void *arg)
{
  struct pollfd fds[3];
  struct touch_sample_s sample;
  char buffer[128];
  ssize_t n;
  int ret;

  fds[0].fd = g_master;
  fds[0].events = POLLIN;
  fds[1].fd = g_touchfd;
  fds[1].events = POLLIN;
  fds[2].fd = g_serialin;
  fds[2].events = POLLIN;
  for (; ; )
    {
      ret = poll(fds, 3, -1);
      if (ret < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          return NULL;
        }

      if ((fds[0].revents & POLLIN) != 0)
        {
          n = read(g_master, buffer, sizeof(buffer));
          if (n > 0)
            {
              if (wr_writeall(g_termfd, buffer, n) < 0 ||
                  wr_writeall(g_serialout, buffer, n) < 0)
                {
                  return NULL;
                }
            }
        }

      if ((fds[1].revents & POLLIN) != 0)
        {
          n = read(g_touchfd, &sample, sizeof(sample));
          if (n == sizeof(sample) && sample.npoints == 1)
            {
              wr_touch(&sample.point[0]);
            }
        }

      if ((fds[2].revents & POLLIN) != 0)
        {
          n = read(g_serialin, buffer, sizeof(buffer));
          if (n > 0)
            {
              ssize_t i;

              for (i = 0; i < n; i++)
                {
                  wr_input(&buffer[i], 1);
                }
            }
        }
    }
}

static int wr_initialize(void)
{
  struct nxterm_window_s term =
  {
    .wcolor = {0},
    .fcolor = {1},
    .wsize = {WR_WIDTH, WR_TERM_HEIGHT},
    .fontid = FONTID_X11_MISC_FIXED_6X9
  };

  struct nxgl_size_s size;
  struct nxgl_point_s pos;
  const struct nx_fontbitmap_s *bitmap;
  NXHANDLE font;
  pthread_t listener;
  pthread_t bridge;
  pthread_attr_t attr;
  struct sched_param priority;
  int slave;
  int nonblock = 1;
  struct termios tty;
  struct winsize winsz;
  int ch;
  int y;
  int ret;

  sem_init(&g_connected, 0, 0);
  sem_init(&g_synched, 0, 0);
  g_serialout = dup(STDOUT_FILENO);
  if (g_serialout < 0)
    {
      return -1;
    }

  font = nxf_getfonthandle(FONTID_X11_MISC_FIXED_6X9);
  for (ch = 32; ch <= 126; ch++)
    {
      bitmap = nxf_getbitmap(font, ch);
      if (bitmap != NULL)
        {
          nxf_convert_1bpp(g_glyph[0][ch - 32], WR_FONT_HEIGHT,
                           WR_FONT_WIDTH, 1, bitmap, 1);
        }

      for (y = 0; y < WR_FONT_HEIGHT; y++)
        {
          g_glyph[1][ch - 32][y] = ~g_glyph[0][ch - 32][y];
        }
    }

  g_stage = "NX server";
  ret = kthread_create("wr_nx", 180, 8192, wr_server, NULL);
  if (ret < 0)
    {
      return -1;
    }

  g_stage = "NX connection";
  g_nx = nx_connect();
  if (g_nx == NULL)
    {
      return -1;
    }

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 8192);
  priority.sched_priority = 120;
  pthread_attr_setschedparam(&attr, &priority);
  ret = pthread_create(&listener, &attr, wr_listener, NULL);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      return -1;
    }

  pthread_detach(listener);
  if (wr_wait(&g_connected) < 0)
    {
      return -1;
    }

  g_stage = "NX windows";
  g_termwnd = nx_openwindow(g_nx, 0, &g_callbacks, NULL);
  g_keywnd = nx_openwindow(g_nx, 0, &g_callbacks, (void *)1);
  if (g_termwnd == NULL || g_keywnd == NULL)
    {
      return -1;
    }

  pos.x = 0;
  pos.y = 0;
  size.w = WR_WIDTH;
  size.h = WR_TERM_HEIGHT;
  nx_setposition(g_termwnd, &pos);
  nx_setsize(g_termwnd, &size);
  pos.y = WR_TERM_HEIGHT;
  size.h = WR_HEIGHT - WR_TERM_HEIGHT;
  nx_setposition(g_keywnd, &pos);
  nx_setsize(g_keywnd, &size);
  if (nx_synch(g_keywnd, &g_synched) < 0 || wr_wait(&g_synched) < 0)
    {
      return -1;
    }

  g_stage = "register";
  g_term = nx_register(g_termwnd, &term, 0);
  if (g_term == NULL)
    {
      return -1;
    }

  g_stage = "open devices";
  g_termfd = open("/dev/nxterm0", O_WRONLY);
  g_touchfd = open("/dev/input0", O_RDONLY | O_NONBLOCK);
  g_serialin = open("/dev/ttyS0", O_RDONLY | O_NONBLOCK);
  if (g_termfd < 0 || g_touchfd < 0 || g_serialin < 0 ||
      openpty(&g_master, &slave, NULL, NULL, NULL) < 0)
    {
      return -1;
    }

  /* UART0 is the debug input/mirror for the same PTY session.  Raw input
   * avoids canonical reads losing partial lines with O_NONBLOCK, and lets
   * the PTY own echo, editing and foreground signal delivery.
   */

  g_stage = "UART mirror";
  if (tcgetattr(g_serialin, &tty) < 0)
    {
      close(slave);
      return -1;
    }

  tty.c_iflag = 0;
  tty.c_oflag &= ~OPOST;
  tty.c_lflag &= ~(ICANON | ECHO | ISIG);
  if (tcsetattr(g_serialin, TCSANOW, &tty) < 0)
    {
      close(slave);
      return -1;
    }

  /* PTY output processing supplies CR/LF.  Readline performs line
   * editing and relies on the PTY for character echo.  FIONBIO also
   * updates the PTY's underlying pipes.
   */

  if (ioctl(g_master, FIONBIO, &nonblock) < 0 ||
      tcgetattr(slave, &tty) < 0)
    {
      close(slave);
      return -1;
    }

  tty.c_lflag = (tty.c_lflag & ~ICANON) | ECHO;
  if (tcsetattr(slave, TCSANOW, &tty) < 0)
    {
      close(slave);
      return -1;
    }

  /* Publish the terminal window's size on the PTY.  A full-screen program
   * run from this shell asks its own standard input, not the framebuffer,
   * how many character cells it has to work with.  Failure is not fatal:
   * the program falls back to its configured default geometry.
   */

  g_stage = "window size";
  if (ioctl(g_termfd, TIOCGWINSZ, (unsigned long)(uintptr_t)&winsz) >= 0)
    {
      ioctl(slave, TIOCSWINSZ, (unsigned long)(uintptr_t)&winsz);
    }

  g_stage = "bridge";
  pthread_mutex_lock(&g_keylock);
  wr_paintkeys(NULL);
  pthread_mutex_unlock(&g_keylock);
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 8192);
  priority.sched_priority = 110;
  pthread_attr_setschedparam(&attr, &priority);
  ret = pthread_create(&bridge, &attr, wr_bridge, NULL);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      close(slave);
      return -1;
    }

  pthread_detach(bridge);
  g_stage = "stdio";
  fflush(stdout);
  fflush(stderr);
  if (dup2(slave, STDIN_FILENO) < 0 || dup2(slave, STDOUT_FILENO) < 0 ||
      dup2(slave, STDERR_FILENO) < 0)
    {
      close(slave);
      return -1;
    }

  close(slave);
  dprintf(g_serialout, "WIKIREADER_TERMINAL_READY\n");
  return 0;
}

static void wr_sigint(int signo)
{
  /* Interrupt NSH's blocking built-in commands without ending the UI. */
}

static int wr_shell(int argc, char **argv)
{
  struct sigaction action;

  memset(&action, 0, sizeof(action));
  action.sa_handler = wr_sigint;
  sigemptyset(&action.sa_mask);
  sigaction(SIGINT, &action, NULL);
  atomic_set(&g_shellpid, getpid());
  ioctl(STDIN_FILENO, TIOCSCTTY, 0);
  return nsh_consolemain(argc, argv);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int wikireader_main(int argc, char **argv)
{
  pid_t shell;
  int status;

  if (wr_initialize() < 0)
    {
      fprintf(stderr, "WikiReader terminal failed at %s: %d\n",
              g_stage, errno);
      return 1;
    }

  nsh_initialize();
  for (; ; )
    {
      shell = task_create("nsh", CONFIG_SYSTEM_NSH_PRIORITY, 8192,
                          wr_shell, NULL);
      if (shell < 0)
        {
          fprintf(stderr, "WikiReader shell creation failed: %d\n", errno);
          return 1;
        }

      /* Keep the UI alive when the user exits NSH or an application ends
       * the shell.  Child tasks inherit the PTY standard descriptors.
       */

      while (waitpid(shell, &status, 0) < 0 && errno == EINTR)
        {
        }
    }
}
