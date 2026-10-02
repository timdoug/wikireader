/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Macros to support TLS testing, C33 FDPIC version.

   Every C33 TLS relocation (tlsle, tlsie, tlsmod, tlsoff) is a data word,
   as in the compiler's own sequences.  Each macro emits its words once per
   object under a local label, and C reads them as hidden objects, so the
   compiler addresses them through its own data pointer.  */

#define TLS_C33_WORDS(label, words) \
  __asm__ __volatile__ (".ifndef " #label "\n\t" \
			".pushsection .data\n\t" \
			".balign 4\n" \
			#label ":\n\t" \
			words "\n\t" \
			".popsection\n\t" \
			".endif")

#define TLS_LE(x) \
  ({ extern const int __tls_le_##x __attribute__ ((visibility ("hidden"))); \
     TLS_C33_WORDS (__tls_le_##x, ".long tlsle(" #x ")"); \
     (int *) ((char *) __builtin_thread_pointer () + __tls_le_##x); })

#define TLS_IE(x) \
  ({ extern const int __tls_ie_##x __attribute__ ((visibility ("hidden"))); \
     TLS_C33_WORDS (__tls_ie_##x, ".long tlsie(" #x ")"); \
     (int *) ((char *) __builtin_thread_pointer () + __tls_ie_##x); })

#define TLS_LD(x) \
  ({ extern void *__tls_get_addr (void *); \
     extern int __tls_ldm[2] __attribute__ ((visibility ("hidden"))); \
     extern const int __tls_ldo_##x __attribute__ ((visibility ("hidden"))); \
     TLS_C33_WORDS (__tls_ldm, ".long tlsmod(0), 0"); \
     TLS_C33_WORDS (__tls_ldo_##x, ".long tlsoff(" #x ")"); \
     (int *) ((char *) __tls_get_addr (__tls_ldm) + __tls_ldo_##x); })

#define TLS_GD(x) \
  ({ extern void *__tls_get_addr (void *); \
     extern int __tls_gd_##x[2] __attribute__ ((visibility ("hidden"))); \
     TLS_C33_WORDS (__tls_gd_##x, ".long tlsmod(" #x "), tlsoff(" #x ")"); \
     (int *) __tls_get_addr (__tls_gd_##x); })
