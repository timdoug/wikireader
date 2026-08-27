/*
 * <stdint.h> for the torture harness only.
 *
 * mini-libc has no stdint.h, and gcc's installed one is just a wrapper that
 * reaches for the system header unless __STDC_HOSTED__ is 0.  Setting that
 * with -ffreestanding would work, but -ffreestanding also implies
 * -fno-builtin, and a fair number of torture tests exist precisely to check
 * that a builtin folds -- 20021127-1 defines its own llabs() that calls
 * abort() and passes only if gcc never calls it.
 *
 * So point at gcc's self-contained copy directly and leave the builtins on.
 */

#ifndef C33_TEST_STDINT_H
#define C33_TEST_STDINT_H

#include <stdint-gcc.h>

#endif
