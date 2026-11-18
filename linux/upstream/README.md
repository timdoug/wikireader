# Kernel fixes offered upstream

Fixes from `../patches/fixes/` to shared kernel code, as mail-ready patch
series against v7.4-rc3, and the reproducers that show each bug on an
in-tree NOMMU target: QEMU's riscv64 `virt` machine with
`nommu_virt_defconfig`. These runs are of mainline on RISC-V, not of the
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
