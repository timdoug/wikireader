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

Freestanding static PIE programs, with no libc and no relocations, run as
`/init` from a built-in initramfs. The kernel command line names one test
after `--`. Each boot runs that test, prints `TEST-DONE` and powers off.

| Test | Patch | Stock v7.4-rc3 | With the fixes |
|---|---|---|---|
| `bigmap` | 0045 | `WARNING: mm/page_alloc.c:5394`, `ENOMEM` | `ENOMEM`, no warning |
| `traced-map` | 0042 | `WARNING: mm/nommu.c:932`, mapping correct | mapping correct, no warning |
| `smallenv` | 0031 | initial sp 4.4 KB below the 8 KiB stack mapping | sp inside the stack mapping |
| `bigenv` | 0031 | hangs during the exec | sp inside the stack mapping |
| `devmem` | 0032 | two WARNINGs in `mm/util.c`, `EINVAL` | maps 0x80000000 and reads the Image header |
| `devmem-alias` | 0033 | `EINVAL`; with 0032 alone, `kernel BUG at mm/nommu.c:490` and a panic | maps the other process's page and reads the page's data |

In the `wr-linux` VM, with `gcc-riscv64-linux-gnu` and
`qemu-system-misc` installed and a v7.4-rc3 tree in `$tree`:

    linux/upstream/nommu-qemu/run.sh build $tree $out
    linux/upstream/nommu-qemu/run.sh test $out bigmap

`build` merges `nommu.config` over `nommu_virt_defconfig` and checks that
every line of the fragment survives `olddefconfig`. `test` writes the
whole console log to `$out/TEST.log` and prints the key lines.
