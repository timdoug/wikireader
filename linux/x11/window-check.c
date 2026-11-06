// SPDX-License-Identifier: GPL-2.0-only
/* Query the actual X windows; both clients must be visible side by side. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <locale.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

struct client { Window id; int x, y, width, height; };
static struct client terminal, eyes;

static int font_has(XFontStruct *font, unsigned int codepoint)
{
    unsigned int row = codepoint >> 8, column = codepoint & 255;
    if (!font || !font->per_char || row < font->min_byte1 || row > font->max_byte1 ||
        column < font->min_char_or_byte2 || column > font->max_char_or_byte2)
        return 0;
    unsigned int columns = font->max_char_or_byte2 - font->min_char_or_byte2 + 1;
    XCharStruct *glyph = font->per_char + (row - font->min_byte1) * columns +
        column - font->min_char_or_byte2;
    return glyph->width == 5 && glyph->rbearing > glyph->lbearing;
}

/* Check Xlib's UTF-8 font-set conversion against the actual core glyphs.
 * The locale must fall back from fixed's Latin-1 to the Unicode 5x8 font
 * for Greek, Cyrillic and the euro, while retaining the original geometry.
 */
static int unicode_fontset(Display *dpy)
{
    char **missing, *fallback, **names;
    int missing_count, count;
    XFontStruct **fonts, *latin = NULL, *unicode = NULL;
    XFontSet set = XCreateFontSet(dpy, "fixed,5x8", &missing, &missing_count, &fallback);
    if (!set)
        return 0;
    int passed = missing_count == 0;
    for (int i = 0; i < missing_count; i++)
        printf("fontset missing: %s\n", missing[i]);
    if (missing)
        XFreeStringList(missing);
    Window root = DefaultRootWindow(dpy);
    Pixmap actual = XCreatePixmap(dpy, root, 64, 20, DefaultDepth(dpy, DefaultScreen(dpy)));
    Pixmap expected = XCreatePixmap(dpy, root, 64, 20, DefaultDepth(dpy, DefaultScreen(dpy)));
    GC gc = XCreateGC(dpy, actual, 0, NULL);
    XSetForeground(dpy, gc, WhitePixel(dpy, DefaultScreen(dpy)));
    XFillRectangle(dpy, actual, gc, 0, 0, 64, 20);
    XFillRectangle(dpy, expected, gc, 0, 0, 64, 20);
    XSetForeground(dpy, gc, BlackPixel(dpy, DefaultScreen(dpy)));
    const char text[] = "\xc3\xa9\xce\xbb\xd0\x96\xe2\x82\xac";
    XRectangle ink, logical;
    int width = XmbTextExtents(set, text, sizeof(text) - 1, &ink, &logical);
    XmbDrawString(dpy, actual, set, gc, 0, 15, text, sizeof(text) - 1);
    count = XFontsOfFontSet(set, &fonts, &names);
    for (int i = 0; i < count; i++) {
        printf("fontset font: %s\n", names[i]);
        if (fonts[i]->max_byte1 == 0)
            latin = fonts[i];
        if (font_has(fonts[i], 0x3bb) && font_has(fonts[i], 0x416))
            unicode = fonts[i];
    }
    passed = passed && latin && unicode && latin->ascent + latin->descent == 13;
    if (latin && unicode) {
        const unsigned int codes[] = {0xe9, 0x3bb, 0x416, 0x20ac};
        int x = 0;
        for (unsigned int i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
            XFontStruct *font = i ? unicode : latin;
            XChar2b character = { .byte1 = codes[i] >> 8, .byte2 = codes[i] & 255 };
            XSetFont(dpy, gc, font->fid);
            XDrawString16(dpy, expected, gc, x, 15, &character, 1);
            x += XTextWidth16(font, &character, 1);
        }
        passed = passed && width == x;
    }
    XImage *a = XGetImage(dpy, actual, 0, 0, 64, 20, AllPlanes, ZPixmap);
    XImage *b = XGetImage(dpy, expected, 0, 0, 64, 20, AllPlanes, ZPixmap);
    passed = passed && a && b;
    if (a && b)
        for (int y = 0; y < 20; y++)
            for (int x = 0; x < 64; x++)
                passed = passed && XGetPixel(a, x, y) == XGetPixel(b, x, y);
    if (a) XDestroyImage(a);
    if (b) XDestroyImage(b);
    XFreeGC(dpy, gc);
    XFreePixmap(dpy, actual);
    XFreePixmap(dpy, expected);
    XFreeFontSet(dpy, set);
    printf("X11 UTF8 FONTSET %s: missing=%d fonts=%d width=%d\n",
           passed ? "PASS" : "FAIL", missing_count, count, width);
    return passed;
}

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

/* Print a hash of the eyes' window contents: it changes when the pupils
 * move. */
static int eyes_image(Display *dpy)
{
    Window root = DefaultRootWindow(dpy);
    find(dpy, root, root, 0);
    if (!eyes.id)
        return 0;
    XImage *image = XGetImage(dpy, eyes.id, 0, 0, eyes.width, eyes.height,
                              AllPlanes, ZPixmap);
    if (!image)
        return 0;
    unsigned int hash = 2166136261u;
    for (int y = 0; y < image->height; ++y)
        for (int x = 0; x < image->width; ++x)
            hash = (hash ^ (unsigned int)XGetPixel(image, x, y)) * 16777619u;
    XDestroyImage(image);
    printf("XEYES IMAGE %08x\n", hash);
    return 1;
}

int main(int argc, char **argv)
{
    if (!setlocale(LC_CTYPE, "C.UTF-8") || !XSupportsLocale())
        return 1;
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy)
        return 1;
    if (argc == 2 && !strcmp(argv[1], "--eyes")) {
        int found = eyes_image(dpy);
        XCloseDisplay(dpy);
        return !found;
    }
    if (argc == 2 && !strcmp(argv[1], "--fontset")) {
        int passed = unicode_fontset(dpy);
        XCloseDisplay(dpy);
        return !passed;
    }
    Window root = DefaultRootWindow(dpy);
    XFontStruct *font = XLoadQueryFont(dpy, "5x8");
    int small_font = font && font->min_bounds.width == 5 &&
        font->max_bounds.width == 5 && font->ascent + font->descent == 8;
    small_font = small_font && font_has(font, 0xe9) && font_has(font, 0x3bb) &&
        font_has(font, 0x416) && font_has(font, 0x20ac) && font_has(font, 0x2500);
    small_font = small_font && unicode_fontset(dpy);
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
