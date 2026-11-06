# GDB patches

The C33 port's changes to GDB's own files, applied by `../build.sh` through
`../../tools/prepare-source.sh` after binutils' 0001-0003 (`config.sub`,
BFD and opcodes, `../../binutils/patches/`) and in number order.

- `port/`: the target in GDB's configuration and build; the no-MMU ptrace
  path for a C library other than uClibc; gdbserver for C33 Linux.
