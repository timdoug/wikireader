# WikiReader

Firmware and tools for the Epson S1C33 WikiReader handheld. Forked from
[stephen-mw/wikireader](https://github.com/stephen-mw/wikireader).

Most of the work here is a current toolchain, a full-system emulator, and a
reader for standard ZIM archives. The original dump-processing pipeline still
works and is unchanged.

## What's here

- `host-tools/toolchain-c33` - binutils 2.47 and GCC 16.2 targeting C33,
  prefix `c33-epson-elf-`. Builds the complete firmware. The original
  binutils 2.10.1 / GCC 3.3.2 build is kept as an ABI and assembler oracle.
- `emulator` - `wremu`, a full-system emulator. Boots the serial-FLASH chain
  and SD-card applications, including the DMA kernel. Runs headless or in an
  SDL2 window. Timing is modelled and calibrated against hardware; the limits
  are documented.
- `zim` - `zim.app`, a reader for ZIM 6 archives, the format Kiwix uses. Reads
  them through the kernel's FatFs service. Zstandard and WebP decoders are
  included, since the C33 has to decode both itself. Boot files sit on FAT32
  and the archive on a second exFAT partition, so archives over 4 GiB work.
- `samo-lib` - the grifo kernel, FatFs R0.16, mini-libc and drivers. SD reads
  use 32-bit DMA.
- `wiki` - the original reader application, with fixes.
- `doom` - `doom.app`. Monochrome, touch movement, front-button controls.
  Engine source is vendored and pinned.
- `sparrow` - an offline factual answer engine over Wikidata claims. It works,
  but has only been measured against a sample; see `sparrow/STATUS.md`.

Each directory has its own README with the detail.

## Building

Build the toolchain first. It downloads and builds binutils and GCC, and takes
about ten minutes:

```sh
host-tools/toolchain-c33/binutils/build.sh host-tools/toolchain-c33/work
host-tools/toolchain-c33/gcc/rebuild.sh
```

It installs under `host-tools/toolchain-c33/work/install/`. Then build the
components in dependency order, from the repository root:

```sh
TB="$PWD/host-tools/toolchain-c33/work/install/bin"
for d in samo-lib/mini-libc samo-lib/drivers samo-lib/fatfs samo-lib/grifo wiki; do
    make -C "$d" TOOLCHAIN_BIN="$TB" || break
done
```

Then the ZIM reader and Doom, which need grifo built first:

```sh
make -C zim TOOLCHAIN_BIN="$TB" SIMULATE=NO OPT=-O2
make -C doom -j4
```

Build in the component directories rather than using the root Makefile's
`grifo` and `wiki` targets. Those still depend on the legacy `gcc` target, so
they try to fetch and build binutils 2.10.1 and GCC 3.3.2 even when
`TOOLCHAIN_BIN` points at the current toolchain. The root targets for
`mini-libc`, `fatfs` and `drivers` are fine.

Extra flags go in `OPT`, which is appended after `-Werror`. Build output is
git-ignored.

## Emulator

```sh
make -C emulator
make -C emulator check
```

The decode tables are committed, so building the emulator does not need the
cross-compiler, and `check` is self-contained. SDL2 is only needed for the
window (`brew install sdl2`).

Running firmware needs two things the repository does not carry: a kernel you
have built, and a card image. `emulator/README.md` covers both, along with the
boot chain and the profiling caveats. Once you have them:

```sh
cd emulator
./wremu -c images/wrcard.img images/grifo.elf
```

`-R` resets and runs headless, `-n` caps cycles, `-s` traces syscalls, and
`-K`/`-T`/`-N` script keys, taps and buttons.

## SD cards

`zim/make-card-image` (macOS) writes a two-partition card: FAT32 for the boot
files, exFAT for the archive. `zim/README.md` has the layout and the launcher.

## Classic image build

The original pipeline renders a Wikimedia dump into the WikiReader's own
format. It runs in Docker:

```sh
docker pull stephenmw/wikireader
docker run --rm -v $(pwd)/build:/build -ti \
    docker.io/stephenmw/wikireader:latest autowiki 20200601
```

The result is in `build/<date>/image`; copy it to a FAT32 card. It needs about
16 GB of RAM and runs a little over 12 hours. The manual steps, the
dump-cleaning one-liner and multi-machine builds are in
[doc/image-build.text](doc/image-build.text).

This is separate from `zim.app`, which reads ZIM archives directly and needs
none of it.

## Notes

The Epson S1C33 manuals are not in this repository. They are available from
Epson and the Internet Archive; `host-tools/toolchain-c33/gcc/ABI.md` says
which ones and what they cover.

The older notes under `doc/` describe the legacy 3.3.2 path, including a
32-bit-host workaround the current toolchain does not need.
