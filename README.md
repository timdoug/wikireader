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

It installs under `host-tools/toolchain-c33/work/install/`, which is what
everything else looks for by default. Then, from the repository root:

```sh
make wiki zim doom SIMULATE=NO
```

That builds all four applications; mini-libc, drivers, fatfs and grifo come in
as dependencies.

`SIMULATE=NO` turns off grifo's host simulator. A grifo application otherwise
builds twice: once as a C33 `.app`, and once as a Qt5 desktop program compiled
from the same sources with `-DGRIFO_SIMULATOR=1`. That second build is the only
reason a firmware build would need Qt5 installed. `zim` and `doom` set it in
their own makefiles already, so the flag is really for `wiki`. Drop it if you
want the simulator and have Qt5.

Clean targets are `<component>-clean`. Extra flags go in `OPT`, which is
appended after `-Werror` and already defaults to `-O2`. Build output is
git-ignored.

The original EPSON binutils 2.10.1 / GCC 3.3.2 toolchain is still buildable
with `make toolchain`. Nothing depends on it; it is kept as an ABI and
assembler oracle. Set `TOOLCHAIN_BIN` to `host-tools/toolchain-install/bin` to
build with it.

## Emulator

```sh
make -C emulator
make -C emulator check
```

The decode tables are committed, so building the emulator does not need the
cross-compiler. SDL2 is only needed for the window (`brew install sdl2`).
`check` runs on a bare checkout; the few tests that need a local card image or
objdump captures say so and skip.

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
