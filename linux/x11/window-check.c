// SPDX-License-Identifier: GPL-2.0-only
/* Query the actual X windows; both clients must be visible side by side. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

struct client { Window id; int x, y, width, height; };
static struct client terminal, eyes;

static void find(Display *dpy, Window root, Window window, unsigned int depth)
{
    Window parent, returned_root, *children = NULL, child;
    unsigned int count;
    XWindowAttributes attrs;
    XClassHint klass = {0};
    char *name = NULL;
    if (depth > 8 || !XGetWindowAttributes(dpy, window, &attrs))
        return;
    XGetClassHint(dpy, window, &klass);
    XFetchName(dpy, window, &name);
    if (attrs.map_state == IsViewable && klass.res_class) {
        struct client *client = NULL;
        if (strcmp(klass.res_class, "XTerm") == 0)
            client = &terminal;
        if (strcmp(klass.res_class, "XEyes") == 0)
            client = &eyes;
        if (client) {
            client->id = window;
            client->width = attrs.width;
            client->height = attrs.height;
            XTranslateCoordinates(dpy, window, root, 0, 0,
                                  &client->x, &client->y, &child);
            printf("%s %s: %dx%d+%d+%d\n", klass.res_class,
                   name ? name : "", client->width, client->height,
                   client->x, client->y);
        }
    }
    if (name) XFree(name);
    if (klass.res_name) XFree(klass.res_name);
    if (klass.res_class) XFree(klass.res_class);
    if (XQueryTree(dpy, window, &returned_root, &parent, &children, &count)) {
        for (unsigned int i = 0; i < count; ++i)
            find(dpy, root, children[i], depth + 1);
        if (children) XFree(children);
    }
}

static int shell_ready(void)
{
    long pid;
    char path[64], name[64];
    FILE *report = fopen("/mnt/sd/xshell.txt", "r");
    if (!report)
        return 0;
    int found = fscanf(report, "%ld %63s", &pid, path) == 2;
    fclose(report);
    if (!found)
        return 0;
    int slave = open(path, O_RDONLY | O_NOCTTY | O_NONBLOCK);
    if (slave < 0)
        return 0;
    struct termios tty;
    int raw = tcgetattr(slave, &tty) == 0 && !(tty.c_lflag & (ICANON | ECHO));
    close(slave);
    /* TIOCGPGRP on a slave requires the caller to share its session. This
     * checker runs on UART0, so inspect the shell's recorded tty group. */
    snprintf(path, sizeof(path), "/proc/%ld/stat", pid);
    FILE *stat = fopen(path, "r");
    char line[512], state;
    long parent, group, session, device, foreground = -1;
    int parsed = 0;
    if (stat && fgets(line, sizeof(line), stat)) {
        char *fields = strrchr(line, ')');
        if (fields)
            parsed = sscanf(fields + 1, " %c %ld %ld %ld %ld %ld", &state,
                            &parent, &group, &session, &device, &foreground);
    }
    if (stat) fclose(stat);
    int ready = parsed == 6 && foreground == pid && group == pid &&
        session == pid && device != 0 && raw;
    snprintf(path, sizeof(path), "/proc/%ld/comm", pid);
    FILE *comm = fopen(path, "r");
    int named = comm && fgets(name, sizeof(name), comm);
    printf("shell pid=%ld foreground=%ld raw=%d comm=%s", pid, foreground,
           raw, named ? name : "unavailable\n");
    ready = ready && named && strcmp(name, "sh\n") == 0;
    if (comm) fclose(comm);
    if (ready)
        puts("XTERM SHELL PASS: command completed; same shell waiting for input");
    return ready;
}

int main(int argc, char **argv)
{
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy)
        return 1;
    Window root = DefaultRootWindow(dpy);
    XFontStruct *font = XLoadQueryFont(dpy, "5x8");
    int small_font = font && font->min_bounds.width == 5 &&
        font->max_bounds.width == 5 && font->ascent + font->descent == 8;
    if (font) {
        printf("5x8 font: %dx%d\n", font->max_bounds.width, font->ascent + font->descent);
        XFreeFont(dpy, font);
    }
    find(dpy, root, root, 0);
    int passed = small_font && terminal.id && eyes.id && terminal.x >= 0 && terminal.y >= 0 &&
        terminal.x + terminal.width <= eyes.x && eyes.y >= 0 &&
        eyes.x + eyes.width <= 240 && terminal.y + terminal.height <= 208 &&
        eyes.y + eyes.height <= 208;
    if (passed) {
        XImage *text = XGetImage(dpy, terminal.id, 2, 2,
                                terminal.width - 4, terminal.height - 4,
                                AllPlanes, ZPixmap);
        unsigned int ink = 0;
        if (!text) {
            passed = 0;
        } else {
            unsigned long background = XGetPixel(text, text->width - 1,
                                                  text->height - 1);
            for (int y = 0; y < text->height; ++y)
                for (int x = 0; x < text->width; ++x)
                    ink += XGetPixel(text, x, y) != background;
            printf("xterm drawn pixels: %u\n", ink);
            passed = ink > 100;
            XDestroyImage(text);
        }
    }
    XCloseDisplay(dpy);
    if (argc == 2 && strcmp(argv[1], "--shell") == 0)
        passed = passed && shell_ready();
    puts(passed ? "X11 WINDOWS PASS" : "X11 WINDOWS FAIL");
    return passed ? 0 : 1;
}
