# GCC patches

Applied by `../rebuild.sh` and `../build.sh`, through
`../../tools/prepare-source.sh`, in number order across the directories, to
a tree made afresh from the tarball whenever they or the tree change.

- `fixes/`: corrections to GCC that stand on their own: nested ICE
  diagnostics, the analyzer's model of a direct global `errno`,
  restricting ARM's FDPIC EH encodings to ARM, and FDPIC's `dl_phdr_info`
  load map in libgcc's unwind-table discovery.
- `port/`: the C33 port's changes to shared code: C33 FDPIC beside FRV's
  and Blackfin's in libgcc's unwind-table discovery.

crtstuff's use of `PT_GNU_EH_FRAME` with musl is the target's own
choice, in `config/c33/linux.h`, beside its `CRT_GET_RFIB_DATA`.
