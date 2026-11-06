# binutils patches

The C33 port's changes to binutils' own files, applied by `../build.sh`
through `../../tools/prepare-source.sh` in number order, after the port's
own files (`../files/`) are copied in.  GDB's build applies 0001-0003 to its
copies of `config.sub`, BFD and opcodes.

- `port/`: `config.sub`; the target in BFD, opcodes, gas and ld; ld's
  `elf.sc` hook for FDPIC's data segment; readelf's C33 relocations; and
  the ld testsuite's expectations of a C33 Linux (FDPIC) link.

The generated `Makefile.in`, `configure`, `bfd-in2.h` and `libbfd.h`
carry hunks matching their sources' changes, edited by hand.  A submission
regenerates them with automake 1.15.1, autoconf 2.69 and `make headers`,
the versions binutils 2.47 was generated with.
