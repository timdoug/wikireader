# GCC patches

Applied by `../rebuild.sh` and `../build.sh` in number order across the
directories:

- `fixes/`: corrections to GCC that stand on their own: nested ICE
  diagnostics, the analyzer's model of a direct global `errno`, uClibc-ng's
  direct pthreads, and restricting ARM's FDPIC EH encodings to ARM.
- `port/`: the C33 port's changes to shared code: FDPIC unwind-table
  discovery and link-time table search in libgcc, and the
  `APPLY_ARGS_SAVE_VALUE` macro behind `__builtin_apply` (`../ABI.md`).
