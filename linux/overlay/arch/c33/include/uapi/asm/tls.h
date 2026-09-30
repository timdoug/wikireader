/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_ASM_C33_TLS_H
#define _UAPI_ASM_C33_TLS_H
/* UP NOMMU ABI: kernel publishes the current TP in this aligned A0 RAM word.
 * Reserved below Grifo's suspend scratch; no general register is reserved. */
#define C33_TLS_SLOT_ADDRESS 0x00001fbc
#endif
