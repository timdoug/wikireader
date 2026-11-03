# Kernel patches

Applied by `fetch.sh` in number order across the directories, since a later
patch may build on an earlier one. The directories say what each is:

- `arch/`: the C33 architecture and its SoC and board drivers, the device
  tree vendor prefix and the syscall table entry. The port itself.
- `fixes/`: corrections to shared kernel code -- no-MMU memory management,
  FDPIC loading and core dumps, `/dev/zero`, the ADC battery -- that stand
  on their own, independently of C33.
- `features/`: optional generic features: no-MMU memory locking, `msync`
  and `madvise` contracts, the 6x8 font without the fbdev console.
- `perf/`: performance choices for this board: lazy VT device
  registration and the FAT cluster-chain walk, each measured in
  `../README.md`.
