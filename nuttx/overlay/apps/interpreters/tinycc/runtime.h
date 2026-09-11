/* SPDX-License-Identifier: LGPL-2.0-or-later */
#ifndef TCC_NUTTX_RUNTIME_H
#define TCC_NUTTX_RUNTIME_H
struct tcc_runtime_symbol { const char *name; const void *address; };
const struct tcc_runtime_symbol *tcc_runtime_symbols(void);
void *tcc_runtime_register(void (*destroy)(void *), void *object);
void tcc_runtime_release(void *handle);
void tcc_runtime_compiling(int begin);
#endif
