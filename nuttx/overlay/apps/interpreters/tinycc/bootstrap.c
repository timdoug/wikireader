/* The compiler built by TinyCC itself, with a small command-line driver.
 * All compiler stages below are compiled from source; runtime.h supplies
 * only OS/libc addresses and task cleanup, never compilation services.
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#include "libtcc.c"
#include "runtime.h"

static void destroy_compiler(void *s) { tcc_delete(s); }
int main(int argc, char **argv)
{
    TCCState *s;
    const struct tcc_runtime_symbol *p;
    const char *source = NULL, *output = "/tmp/a.o";
    int i, first = 0, run = 1, string = 0, ret = 1;
    void *handle;
    tcc_runtime_compiling(1);
    s = tcc_new();
    if (!s) { tcc_runtime_compiling(0); return 1; }
    handle = tcc_runtime_register(destroy_compiler, s);
    if (!handle) { tcc_delete(s); tcc_runtime_compiling(0); return 1; }
    tcc_set_options(s, "-nostdlib -Werror=implicit-function-declaration");
    if (argc < 2 || !strcmp(argv[1], "-h")) {
        puts("TinyCC C33: compiled by TinyCC on NuttX\n"
             "  -c source.c -o output.o\n  -run source.c|object.o [args...]\n"
             "  -e 'C source'");
        ret = 0;
        goto done;
    }
    for (i = 1; i < argc; ++i) {
        if (source && run) break;
        if (!strcmp(argv[i], "-run")) { run = 1; continue; }
        if (!strcmp(argv[i], "-c")) { run = 0; continue; }
        if (!strcmp(argv[i], "-o") && i + 1 < argc) { output = argv[++i]; continue; }
        if (!strncmp(argv[i], "-I", 2) || !strncmp(argv[i], "-D", 2) || !strncmp(argv[i], "-U", 2)) {
            tcc_set_options(s, argv[i]); continue;
        }
        if (!strcmp(argv[i], "-e") && i + 1 < argc) {
            string = 1; source = argv[++i]; first = i; continue;
        }
        if (argv[i][0] == '-' || source) { fprintf(stderr, "tcc: invalid input %s\n", argv[i]); goto done; }
        source = argv[i]; first = i;
    }
    if (!source) { fprintf(stderr, "tcc: missing input\n"); goto done; }
    if (tcc_set_output_type(s, run ? TCC_OUTPUT_MEMORY : TCC_OUTPUT_OBJ) < 0) goto done;
    if ((string ? tcc_compile_string(s, source) : tcc_add_file(s, source)) < 0) goto done;
    if (!run) { ret = tcc_output_file(s, output) < 0; goto done; }
    for (p = tcc_runtime_symbols(); p->name; ++p)
        tcc_add_symbol(s, p->name, p->address);
    if (tcc_relocate(s) < 0) goto done;
    {
        int (*entry)(int, char **) = tcc_get_symbol(s, "main");
        if (!entry) { fprintf(stderr, "tcc: main is missing\n"); goto done; }
        tcc_runtime_compiling(0);
        ret = entry(argc - first, argv + first);
        tcc_runtime_compiling(1);
    }
done:
    tcc_runtime_release(handle);
    tcc_runtime_compiling(0);
    return ret;
}
