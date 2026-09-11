/* Native compiler command for the WikiReader NSH terminal.
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#include <nuttx/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <setjmp.h>
#include "libtcc.h"
#include "tcc_files.h"
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include "runtime.h"
#include "softfloat.h"

extern int tcc_c33_setjmp(unsigned *);
extern void tcc_c33_longjmp(unsigned *, int);
struct runtime_object {
    struct runtime_object *next;
    void (*destroy)(void *);
    void *object;
};
static struct runtime_object *runtime_objects;
static int compiling_depth;
static sigset_t compiling_mask;

void tcc_runtime_compiling(int begin)
{
    sigset_t block;
    if (begin) {
        if (compiling_depth++ == 0) {
            sigemptyset(&block);
            sigaddset(&block, SIGINT);
            sigprocmask(SIG_BLOCK, &block, &compiling_mask);
        }
    } else if (--compiling_depth == 0) {
        sigprocmask(SIG_SETMASK, &compiling_mask, NULL);
    }
}
void *tcc_runtime_register(void (*destroy)(void *), void *object)
{
    struct runtime_object *p = malloc(sizeof(*p));
    if (!p) return NULL;
    p->destroy = destroy; p->object = object;
    p->next = runtime_objects; runtime_objects = p;
    return p;
}
void tcc_runtime_release(void *handle)
{
    struct runtime_object **p = &runtime_objects, *item = handle;
    while (*p && *p != item) p = &(*p)->next;
    if (!*p) return;
    *p = item->next;
    item->destroy(item->object);
    free(item);
}

/* TinyCC's frontend has process-global state in this flat NuttX build. */
static pthread_mutex_t compiler_lock = PTHREAD_MUTEX_INITIALIZER;
static jmp_buf program_jmp;
static int program_status;
struct invocation {
    TCCState *compiler;
};
static void cleanup(int status, void *arg)
{
    struct invocation *inv = arg;
    sigset_t block;
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigprocmask(SIG_BLOCK, &block, NULL);
    while (runtime_objects) tcc_runtime_release(runtime_objects);
    compiling_depth = 0;
    if (inv->compiler) tcc_delete(inv->compiler);
    pthread_mutex_unlock(&compiler_lock);
    free(inv);
}
static void interrupt_program(int signo)
{
    /* The frontend is never interrupted with half-updated global state.
     * During execution, exit runs cleanup and terminates this NSH task. */
    exit(128 + signo);
}
static void program_exit(int status)
{
    program_status = status;
    longjmp(program_jmp, 1);
}
static void program_abort(void) { program_exit(134); }

extern int __divsi3(int, int);
extern unsigned __udivsi3(unsigned, unsigned);
extern int __modsi3(int, int);
extern unsigned __umodsi3(unsigned, unsigned);
extern long long __divdi3(long long, long long);
extern unsigned long long __udivdi3(unsigned long long, unsigned long long);
extern long long __moddi3(long long, long long);
extern unsigned long long __umoddi3(unsigned long long, unsigned long long);
extern long long __ashldi3(long long, int);
extern long long __ashrdi3(long long, int);
extern unsigned long long __lshrdi3(unsigned long long, int);

const struct tcc_runtime_symbol *tcc_runtime_symbols(void)
{
#define EXPORT(name) { #name, (const void *)(uintptr_t)&name }
    static const struct tcc_runtime_symbol symbols[] = {
        EXPORT(printf),
        EXPORT(puts),
        EXPORT(putchar),
        EXPORT(getchar),
        EXPORT(sprintf),
        EXPORT(snprintf),
        EXPORT(fprintf),
        EXPORT(fflush),
        EXPORT(fopen),
        EXPORT(fclose),
        EXPORT(fread),
        EXPORT(fwrite),
        EXPORT(fgets),
        EXPORT(malloc),
        EXPORT(calloc),
        EXPORT(realloc),
        EXPORT(free),
        EXPORT(memcpy),
        EXPORT(memmove),
        EXPORT(memset),
        EXPORT(memcmp),
        EXPORT(strlen),
        EXPORT(strcmp),
        EXPORT(strncmp),
        EXPORT(strcpy),
        EXPORT(strncpy),
        EXPORT(strchr),
        EXPORT(atoi),
        EXPORT(strtol),
        EXPORT(strtoul),
        EXPORT(abs),
        EXPORT(__divsi3),
        EXPORT(__udivsi3),
        EXPORT(__modsi3),
        EXPORT(__umodsi3),
        EXPORT(__divdi3),
        EXPORT(__udivdi3),
        EXPORT(__moddi3),
        EXPORT(__umoddi3),
        EXPORT(__ashldi3),
        EXPORT(__ashrdi3),
        EXPORT(__lshrdi3),
        EXPORT(__addsf3),
        EXPORT(__subsf3),
        EXPORT(__mulsf3),
        EXPORT(__divsf3),
        EXPORT(__negsf2),
        EXPORT(__eqsf2),
        EXPORT(__nesf2),
        EXPORT(__ltsf2),
        EXPORT(__lesf2),
        EXPORT(__gtsf2),
        EXPORT(__gesf2),
        EXPORT(__unordsf2),
        EXPORT(__floatsisf),
        EXPORT(__floatunsisf),
        EXPORT(__fixsfsi),
        EXPORT(__fixunssfsi),
        EXPORT(__floatdisf),
        EXPORT(__floatundisf),
        EXPORT(__fixsfdi),
        EXPORT(__fixunssfdi),
        EXPORT(__adddf3),
        EXPORT(__subdf3),
        EXPORT(__muldf3),
        EXPORT(__divdf3),
        EXPORT(__negdf2),
        EXPORT(__eqdf2),
        EXPORT(__nedf2),
        EXPORT(__ltdf2),
        EXPORT(__ledf2),
        EXPORT(__gtdf2),
        EXPORT(__gedf2),
        EXPORT(__unorddf2),
        EXPORT(__floatsidf),
        EXPORT(__floatunsidf),
        EXPORT(__fixdfsi),
        EXPORT(__fixunsdfsi),
        EXPORT(__floatdidf),
        EXPORT(__floatundidf),
        EXPORT(__fixdfdi),
        EXPORT(__fixunsdfdi),
        EXPORT(__extendsfdf2),
        EXPORT(__truncdfsf2),
        EXPORT(open),
        EXPORT(close),
        EXPORT(read),
        EXPORT(write),
        EXPORT(lseek),
        EXPORT(unlink),
        EXPORT(getcwd),
        EXPORT(getenv),
        EXPORT(realpath),
        EXPORT(qsort),
        EXPORT(strtof),
        EXPORT(strtod),
        EXPORT(strtold),
        EXPORT(strtoll),
        EXPORT(strtoull),
        EXPORT(strrchr),
        EXPORT(strstr),
        EXPORT(strcat),
        EXPORT(strerror),
        EXPORT(__errno),
        EXPORT(lib_get_stream),
        EXPORT(fdopen),
        EXPORT(fputc),
        EXPORT(fputs),
        EXPORT(vsnprintf),
        EXPORT(vfprintf),
        EXPORT(perror),
        EXPORT(remove),
        EXPORT(time),
        EXPORT(localtime),
        EXPORT(gettimeofday),
        EXPORT(ldexp),
        EXPORT(ldexpl),
        EXPORT(tcc_c33_setjmp),
        EXPORT(tcc_c33_longjmp),
        EXPORT(tcc_runtime_symbols),
        EXPORT(tcc_runtime_register),
        EXPORT(tcc_runtime_release),
        EXPORT(tcc_runtime_compiling),
        { "exit", program_exit }, { "abort", program_abort }, { NULL, NULL }
    };
#undef EXPORT
    return symbols;
}
static void add_symbols(TCCState *s)
{
    const struct tcc_runtime_symbol *p;
    for (p = tcc_runtime_symbols(); p->name; ++p)
        tcc_add_symbol(s, p->name, p->address);
}
static int setup_files(void)
{
    unsigned i;
    if ((mkdir("/tmp/tcc", 0777) < 0 && errno != EEXIST) ||
        (mkdir("/tmp/tcc/include", 0777) < 0 && errno != EEXIST) ||
        (mkdir("/tmp/tcc/include/sys", 0777) < 0 && errno != EEXIST) ||
        (mkdir("/tmp/tcc/src", 0777) < 0 && errno != EEXIST)) {
        perror("tcc: /tmp (mount tmpfs first)");
        return -1;
    }
    for (i = 0; i < sizeof(tcc_files) / sizeof(tcc_files[0]); ++i) {
        int fd = open(tcc_files[i].path, O_WRONLY | O_CREAT | O_EXCL, 0666);
        const char *p = tcc_files[i].text;
        size_t left = strlen(p);
        if (fd < 0) {
            if (errno == EEXIST) continue;
            perror(tcc_files[i].path);
            return -1;
        }
        while (left) {
            ssize_t n = write(fd, p, left);
            if (n <= 0) {
                close(fd);
                unlink(tcc_files[i].path);
                return -1;
            }
            p += n; left -= n;
        }
        close(fd);
    }
    return 0;
}
static void compiler_error(void *unused, const char *message)
{
    fprintf(stderr, "%s\n", message);
}
int main(int argc, char **argv)
{
    TCCState *s = NULL;
    int i, run = 1, ret = 1, first = 0, string = 0;
    const char *source = NULL, *output = "/tmp/a.o";
    struct invocation *inv;
    struct sigaction action;
    sigset_t blocked, oldmask;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGINT);
    sigprocmask(SIG_BLOCK, &blocked, &oldmask);
    if (pthread_mutex_trylock(&compiler_lock)) {
        fprintf(stderr, "tcc: compiler is already running\n");
        sigprocmask(SIG_SETMASK, &oldmask, NULL);
        return 1;
    }
    inv = calloc(1, sizeof(*inv));
    if (!inv) {
        pthread_mutex_unlock(&compiler_lock);
        sigprocmask(SIG_SETMASK, &oldmask, NULL);
        return 1;
    }
    memset(&action, 0, sizeof(action));
    action.sa_handler = interrupt_program;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    if (on_exit(cleanup, inv) < 0) {
        cleanup(1, inv);
        return 1;
    }
    if (setup_files() < 0) goto done;
    if (argc < 2 || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
        puts("TinyCC C33 (experimental native compiler)\n"
             "  tcc -selfhost\n"
             "  tcc -run file.c [args...]\n"
             "  tcc -e 'int main(void) { return 0; }'\n"
             "  tcc -c file.c -o file.o\n"
             "  tcc -run file.o\n"
             "Headers: /tmp/tcc/include\n"
             "Examples: /tmp/hello.c, /tmp/fib.c\n"
             "Compiler source: /tmp/tcc/src/bootstrap.c\n"
             "Files in /tmp are lost at reboot.");
        ret = 0;
        goto done;
    }
    inv->compiler = s = tcc_new();
    if (!s) goto done;
    tcc_set_error_func(s, NULL, compiler_error);
    tcc_set_options(s, "-nostdlib -Werror=implicit-function-declaration");
    if (argc == 2 && !strcmp(argv[1], "-selfhost")) {
        source = "/tmp/tcc/src/bootstrap.c";
        output = "/tmp/tcc.o";
        run = 0;
        puts("Compiling TinyCC source to /tmp/tcc.o ...");
    } else for (i = 1; i < argc; ++i) {
        if (source && run) break; /* Everything after the input is a program argument. */
        if (!strcmp(argv[i], "-run")) { run = 1; continue; }
        if (!strcmp(argv[i], "-c")) { run = 0; continue; }
        if (!strcmp(argv[i], "-o") && i + 1 < argc) { output = argv[++i]; continue; }
        if (!strncmp(argv[i], "-I", 2) || !strncmp(argv[i], "-D", 2) || !strncmp(argv[i], "-U", 2)) {
            tcc_set_options(s, argv[i]); continue;
        }
        if (!strcmp(argv[i], "-e") && i + 1 < argc) {
            string = 1; source = argv[++i]; first = i; continue;
        }
        if (argv[i][0] == '-') { fprintf(stderr, "tcc: unknown option %s\n", argv[i]); goto done; }
        if (!source) { source = argv[i]; first = i; }
        else if (run) break;
        else { fprintf(stderr, "tcc: one input file is required\n"); goto done; }
    }
    if (!source) { fprintf(stderr, "tcc: missing input\n"); goto done; }
    if (tcc_set_output_type(s, run ? TCC_OUTPUT_MEMORY : TCC_OUTPUT_OBJ) < 0) goto done;
    if ((string ? tcc_compile_string(s, source) : tcc_add_file(s, source)) < 0) goto done;
    if (!run) {
        ret = tcc_output_file(s, output) < 0;
        if (!ret && !strcmp(argv[1], "-selfhost"))
            puts("Built /tmp/tcc.o. Try: tcc -run /tmp/tcc.o -run /tmp/fib.c");
        goto done;
    }
    add_symbols(s);
    if (tcc_relocate(s) < 0) goto done;
    {
        int (*entry)(int, char **) = tcc_get_symbol(s, "main");
        if (!entry) { fprintf(stderr, "tcc: main is missing\n"); goto done; }
        if (!setjmp(program_jmp)) {
            sigprocmask(SIG_SETMASK, &oldmask, NULL);
            program_status = entry(argc - first, argv + first);
        }
        sigprocmask(SIG_BLOCK, &blocked, NULL);
        ret = program_status;
    }
done:
    exit(ret); /* NuttX task return uses _exit; explicitly run on_exit. */
}
