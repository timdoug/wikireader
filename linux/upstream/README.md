# Kernel fixes offered upstream

Fixes from `../patches/fixes/` to shared kernel code, as mail-ready patch
series against v7.4-rc3, and the reproducers that show each bug on
in-tree NOMMU targets: QEMU's riscv64 `virt` machine with
`nommu_virt_defconfig`, and for the FDPIC loader also QEMU's ColdFire
`mcf5208evb`. These runs are of mainline on RISC-V and m68k, not of the
WikiReader, so Grifo is not involved. The same patches are applied to the
WikiReader kernel, which is tested with `../boot-test.sh`.

| Series | Patch | Carried as | Bug |
|---|---|---|---|
| `kernel/mm-nommu/` | 1 | `fixes/0032` | `/dev/mem` mmap fails with two WARNs and `-EINVAL` since `/dev/mem` moved to `mmap_prepare` (ab04945f91bc) |
| | 2 | `fixes/0033` | a `/dev/mem` mapping at another region's start hits `BUG()` in `add_nommu_region()` (since 8feae13110d6) |
| | 3 | `fixes/0042` | a ptraced process's read-only private mapping of a ramfs file WARNs on the private-copy fallback (since b6b7a8faf05c) |
| | 4 | `fixes/0045` | a private mapping beyond `MAX_PAGE_ORDER` WARNs in the page allocator |
| `kernel/binfmt-elf-fdpic/` | 1 | `fixes/0031` | the FDPIC loader copies the argument image to the top of the NOMMU stack without checking that it fits, and writes below the allocation |

The To and Cc lines come from `scripts/get_maintainer.pl`, plus Greg
Ungerer for NOMMU and FDPIC. `0000-cover-letter.patch` is the cover
letter of the mm series. Before sending, check that the patches still apply to the
current mainline and mm-new, and that every commit ID in a `Fixes:` tag
exists there; `checkpatch.pl` cannot check the IDs in the VM's shallow
clone. Then send each directory with `git send-email`.

## How far the FDPIC stack fix is verified

`fixes/0031` is the largest change here, so its argument is recorded in
full. When the argument image is at most a quarter of the requested
stack, the allocation is unchanged, byte for byte; that covers ordinary
execs. A larger image raises the allocation to the image plus three
quarters of the requested size, so the allocation only grows, and
`bprm_stack_limits()` bounds the growth (128 KiB of strings plus the
pointer arrays). Each term of the estimate is an upper bound on what
`create_elf_fdpic_tables()` writes:

- the strings: exactly the span `transfer_args_to_stack()` copies;
- `ELF_PLATFORM` and `ELF_BASE_PLATFORM`;
- the executable's load map and, with an interpreter, the interpreter's;
- the auxiliary vector, which is built in `mm->saved_auxv`, so the size
  of that array is the ceiling;
- argc and the argv and envp arrays with their terminators;
- 64 bytes of alignment, against at most 51 used.

The sweep covers two ABIs: 64-bit little-endian with 4 KiB pages
(riscv64) and 32-bit big-endian with 8 KiB pages (ColdFire). Two points
are outside what the reproducers show:

- Real FDPIC binaries with an interpreter, and so the interpreter's load
  map term, run only on the WikiReader, in normal boots. Both QEMU
  machines load ordinary static PIE through `binfmt_elf_fdpic`. ARM
  Cortex-M FDPIC under `qemu-system-arm` would cover it, with an FDPIC
  toolchain and C library from Buildroot.
- The size is computed after `begin_new_exec()`, because the load maps
  are allocated there, so a refused exec kills the process instead of
  returning `E2BIG`. Only a corrupt `PT_GNU_STACK` size near the top of
  the address space reaches that path. A failure to allocate an
  over-large stack has always been reported at the same point.

## Other upstream candidates

From the review of all the carried kernel patches, these also fix shared
code, and are not yet in this directory:

- `fixes/0021`: System V shm on NOMMU. The shm file operations lack an
  `mmap_capabilities` hook that forwards the backing file's, so
  `shmat()` of a ramfs-backed segment fails. Sendable as it is.
- `fixes/0022`: a piped FDPIC core dump always prints the final size
  warning, because pipes leave `file->f_pos` at zero. Sendable as it is.
- `fixes/0023`: a private mmap of `/dev/zero` fails on NOMMU, because
  `kernel_read()` returns `-EINVAL` for a file with both `.read` and
  `.read_iter`. The
  patch removes `read_zero()`, which upstream added for speed
  (99f667352f6c); for upstream it has to be reworked, for example around
  `file_is_dev_zero()` in the NOMMU mmap path.
- `fixes/0029`: repeated exact mappings of one address replace the VMA in
  its Maple Tree slot and leak its file, region and callback references,
  a leak since the v6.1 Maple Tree conversion. The patch also depends on
  `fixes/0028` (mlock) and bundles sysctl and `total_vm` changes, so it
  has to be split first. `fixes/0046` builds on the same mapping rules.

## Reproducers (`nommu-qemu/`)

Freestanding static PIE programs, with no libc, run as `/init` from a
built-in initramfs, on two QEMU machines:

- `ARCH=riscv`: `virt` with `nommu_virt_defconfig` (64-bit, 4 KiB pages);
  the kernel command line names one test after `--`.
- `ARCH=m68k`: ColdFire `mcf5208evb` with `m5208evb_defconfig` (32-bit,
  big-endian, 8 KiB pages). This machine passes no command line, so the
  test is built into the kernel (`TEST=name` at build time, `stack` by
  default). The programs address data through a GOT, and `reloc.c`
  applies its `R_68K_RELATIVE` entries at startup.

Each boot runs its test, prints `TEST-DONE` and restarts or powers off.

| Test | Patch | Stock v7.4-rc3 | With the fixes |
|---|---|---|---|
| `bigmap` | 0045 | `WARNING: mm/page_alloc.c:5394`, `ENOMEM` | `ENOMEM`, no warning |
| `traced-map` | 0042 | `WARNING: mm/nommu.c:932`, mapping correct | mapping correct, no warning |
| `smallenv` | 0031 | riscv64: initial sp 4.4 KB below the 8 KiB stack mapping | sp inside the stack mapping |
| `bigenv` | 0031 | riscv64: hangs during the exec | sp inside the stack mapping |
| `sweep` | 0031 | riscv64: fails at an 8 KB environment, then a kernel oops and panic; ColdFire: a 32 KB environment into the 16 KiB minimum stack makes the kernel jump to 0x78787878 (the environment's `xxxx`) and panic | riscv64: 57 ok, 3 `E2BIG`; ColdFire: 60 ok |
| `stack` | 0031 | `smallenv`, `sweep` and `bigenv` in one boot | |
| `devmem` | 0032 | riscv64: two WARNINGs in `mm/util.c`, `EINVAL` | maps 0x80000000 and reads the Image header |
| `devmem-alias` | 0033 | riscv64: `EINVAL`; with 0032 alone, `kernel BUG at mm/nommu.c:490` and a panic | maps the other process's page and reads the page's data |

`sweep` execs `stack-victim`, linked with PT_GNU_STACK sizes of 8 KiB,
64 KiB and 0 (the 128 KiB default), with environments from 2 bytes to the
128 KiB argument limit and with 1000 to 60000 tiny argv or envp entries,
where the pointer arrays make up most of the image. Each exec passes if
the whole image is inside the stack mapping and the room below the stack
pointer is at least min(size - image, size - size / 4), with the size
raised to two pages and rounded to a page. `E2BIG` marks a layout over the
argument limit, refused before loading.

In the `wr-linux` VM, with `gcc-riscv64-linux-gnu`, `gcc-m68k-linux-gnu`
and `qemu-system-misc` installed and a v7.4-rc3 tree in `$tree`:

    linux/upstream/nommu-qemu/run.sh build $tree $out
    linux/upstream/nommu-qemu/run.sh test $out bigmap
    ARCH=m68k TEST=stack linux/upstream/nommu-qemu/run.sh build $tree $out
    ARCH=m68k linux/upstream/nommu-qemu/run.sh test $out stack

`build` merges `nommu.config` and `nommu-$ARCH.config` over the
defconfig, checks that every line of the fragment survives
`olddefconfig`, and installs the kernel's UAPI headers for the programs'
system call numbers. `test` writes the whole console log to
`$out/TEST.log` and prints the key lines.
