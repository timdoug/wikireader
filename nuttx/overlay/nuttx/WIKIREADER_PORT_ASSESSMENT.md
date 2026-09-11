# WikiReader NuttX port assessment

Assessed 2026-09-10 against NuttX `5866c4e3af` and the local
`../wikireader` checkout. The assessment phase was read-only.

Implementation follow-up: the C33 port now runs serial NSH, the configured
NuttX OS tests, and an LCD terminal with a touch keyboard in the emulator.
Direct ELF and full FLASH/card boot pass with an isolated, size-correct test
loader. Physical hardware remains untested. See the
[board README](boards/c33/s1c33e07/wikireader/README.md) for build commands,
validation and remaining work. Emulator UART changes were made with the
subsequent authorization to modify that part of `../wikireader`.

**A usable NSH terminal with an on-screen keyboard is feasible.** The main
investment is a new C33 CPU port. The hardware has ample RAM for this use,
and the existing toolchain, firmware sources, manuals, and emulator remove
much of the usual hardware discovery work.

The baseline had no C33/S1C33E07/WikiReader architecture or board. It required
new `arch/c33` support and a WikiReader board in addition to configuration.

| Area | Evidence and implication |
| --- | --- |
| CPU | Epson S1C33E07, C33 PE, 32-bit little-endian; firmware defines a 60 MHz PLL. Start with a single-core flat NuttX build. |
| RAM | Firmware maps SDRAM at `0x10000000`; board revisions use 16 or 32 MiB. NSH, NX, a keyboard, and task stacks should fit comfortably. The initial port caps its heap at the loader's configured memory size. |
| Display | 240×208, 1 bit per pixel. Firmware uses a 32-byte stride, so one framebuffer is 6,656 bytes in internal video RAM. |
| Touch | UART1 protocol, with down/move/up events and coordinates at twice LCD resolution. The existing driver divides coordinates by two. This is already decoded hardware, rather than an unknown touch protocol. |
| Console | UART0 has working firmware output and receive interrupt handling. Physical connection to the particular unit still needs checking. |
| Storage | microSD uses SPI. NuttX already supplies the MMC/SD SPI and FAT layers; C33 needs its SPI controller and board power/chip-select support. |
| Boot | The local file-loader loads `kernel.elf` from the card, checks the C33 ELF machine, loads sections, and calls the entry point. This is a promising route to boot NuttX without changing serial flash. |
| Toolchain | Installed GCC reports 16.2.0; GNU ld reports 2.47.20260726. The installed compiler is a freestanding C toolchain with PE libgcc. |

Hardware references: [S1C33E07 manual](../wikireader/id001557.pdf),
[C33 PE core manual](../wikireader/s1c33.pdf),
[V3 schematic](../wikireader/Circuits/SAMO_PM_V3_SCH_20100506.pdf),
[memory layout](../wikireader/samo-lib/grifo/lds/grifo.lds),
[board clocks and UART rates](../wikireader/samo-lib/include/samo.h),
[touch driver](../wikireader/samo-lib/grifo/src/CTP.c), and
[LCD implementation](../wikireader/samo-lib/grifo/src/LCD.c).

**CPU context switching is the largest technical risk.** NuttX needs startup,
architecture headers, interrupt masking and dispatch, task stack construction,
context save/restore, timer-driven scheduling, signal delivery, heap setup,
and diagnostic output. The relevant interfaces are documented in the local
[architecture API reference](Documentation/reference/os/arch.rst) and the
[published architecture reference](https://nuttx.apache.org/docs/latest/reference/os/arch.html).

The C33 interrupt path must preserve general registers, PC/PSR, stack state,
and arithmetic registers ALR/AHR. Its trap table requires 1 KiB alignment.
The core pushes PC/PSR for exceptions and returns using `reti`; a scheduler
must also support returning into a different task. C33 has instruction
extension prefixes and branch delay slots, so preemption needs testing around
those sequences and around multiply operations. A compiler-generated interrupt
handler that returns to its caller is only the starting point.

The modern compiler's ABI also preserves a 16-byte outgoing argument base,
despite the hardware's four-byte stack alignment. Both initial task frames and
C calls from interrupt entry assembly must respect that ABI. The compiler
supports absolute data addressing with `-medda32`; this is a useful initial
choice. Startup must still initialize `%r15` for any linked library code that
uses the default data pointer. See the [ABI guide](../wikireader/host-tools/toolchain-c33/gcc/ABI.md).

Compiler atomics are not a blocker for this single-core target. An actual
`__atomic_fetch_add` compilation emits an external `__atomic_fetch_add_4`
call. NuttX provides `CONFIG_LIBC_ATOMIC_IRQ`, including an implementation
using `up_irq_save()` / `up_irq_restore()`, so the architecture can select
that support instead of depending on an external libatomic. See
[atomic configuration](libs/libc/machine/Kconfig) and
[implementation](libs/libc/machine/arch_atomic.c).

**The first UI should be a small C application using NX and NXTerm.**
NuttX's [graphics configuration](graphics/Kconfig) supports
`CONFIG_NXTERM_BPP=1` and `CONFIG_NXTERM_NXKBDIN`. A C33 framebuffer driver can
expose the existing monochrome buffer; a touch driver can report standard
touch events. A keyboard application then maps touches to characters and
feeds the terminal's input path.

The [stock NXTerm example](https://nuttx.apache.org/docs/latest/applications/examples/nxterm/index.html)
leaves stdin on the original console. The WikiReader application must wire
keyboard input into NXTerm and redirect NSH's stdin as well as stdout/stderr.
Turning on NXTerm alone does not produce a touchscreen shell.

The original keyboard occupies 82 pixels at the bottom of the display,
leaving 126 pixels above it. A proposed 6×8 cell font would give roughly
40 columns and 15 text rows before margins; an 8×8 font would give roughly
30 columns. This is a design estimate, not a measured NXTerm layout.
Use a hide/show keyboard action, letters plus shift and symbols, space,
Enter, and Backspace. Shell-specific punctuation such as `/`, `-`, `_`, `.`,
quotes, and `=` matters more here than the reader's multilingual search UI.
History, Tab completion, and Ctrl-C should be added with the appropriate
shell and terminal input handling.

NXTerm currently provides basic scrolling text and backspace handling rather
than full VT100 emulation; start with NSH's basic line input. Rich line editing
or full-screen applications would add terminal work. The installed toolchain
has no C++ compiler, so NxWidgets/NxWM would introduce additional compiler
work. The proposed C UI avoids that dependency. Relevant local sources are
[NXTerm output](graphics/nxterm/nxterm_putc.c),
[keyboard input](graphics/nxterm/nxterm_kbdin.c), and the original
[keyboard layout](../wikireader/wiki/keyboard.c).

**Bring up NuttX as the card-loaded kernel.** Use the existing boot chain to
establish SDRAM and display state, then take control of vectors, clocks,
watchdog, and peripherals in NuttX. The first linker script should keep loaded
sections and initial vectors in SDRAM and preserve the loader's working RAM
until handoff. The local ELF loader processes section headers and clears
`SHT_NOBITS` sections, so copying Grifo's internal-RAM reservations blindly
could overwrite the active loader. Keep ELF section headers and validate the
actual factory loader with the final image layout.

The file-loader also powers the SD card down before entering the kernel;
NuttX must power it back on and initialize it for filesystem access. Grifo's
256 KiB kernel partition is a firmware layout choice, not a 256 KiB hardware
limit for NuttX. See [file-loader](../wikireader/samo-lib/mbr/file-loader.c)
and [ELF loader](../wikireader/samo-lib/drivers/src/elf32.c).

Start storage with polled SPI and NuttX's FAT support. Existing WikiReader DMA
code is valuable hardware reference, but DMA is unnecessary for initial shell
commands. The firmware documents a hardware failure to wake from HALT on DMA
completion; carry that observation into later optimization work. The current
ZIM application's exFAT service and Grifo application ABI would be separate
porting tasks. NSH and a keyboard do not require them.

**Suggested milestones and effort:** these are engineering estimates for one
experienced developer working full time, with access to the hardware. They
are cumulative ranges, not timings established by the probe.

| Milestone | Acceptance criterion | Rough elapsed effort |
| --- | --- | --- |
| Serial NSH | NuttX boots, starts tasks, preempts correctly, handles sleep/wakeup, and accepts commands over a working UART input path. | 1–3 weeks |
| Standalone handheld prototype | Display and touch drivers, NSH with soft keyboard, basic FAT card access. | 3–6 weeks total |
| Reliable daily-use firmware | Power-button behavior, idle/suspend/resume, storage recovery, signal/context stress tests, repeatable hardware boot. | 6–10+ weeks total |

CPU/compiler surprises could extend those ranges. Conversely, a deliberately
minimal demo could appear sooner. Long battery life deserves its own phase:
SDRAM retention, clock gating, touch wake, and watchdog behavior need silicon
testing. The current firmware's [suspend implementation](../wikireader/samo-lib/grifo/src/suspend.c)
and [emulator limitations](../wikireader/emulator/README.md) are useful references.

For an eventual upstream NuttX contribution, write the drivers under an
appropriate contribution license using the hardware specifications. The
existing Grifo drivers and keyboard files carry GPLv3-or-later notices; direct
copying would make upstream contribution a separate licensing consideration.

**Validation performed during this assessment:** the existing compiler and
emulator were executed without rebuilding or modifying anything in
`../wikireader`. All probe inputs and outputs are in
`/tmp/nuttx-wikireader-assessment/`.

- Compiled and linked a freestanding C11 PE ELF using GCC 16.2.0, binutils,
  and PE libgcc, with `-mc33pe -medda32 -O2`.
- Confirmed 32-bit pointers/int/long, little-endian ELF, and C33 machine flags.
- Executed runtime 64-bit division through the linked `__udivdi3`, variadic
  argument handling, and a software interrupt through a relocated trap table
  with a compiler-generated interrupt return. The program printed
  `C33 toolchain probe: PASS`.
- Confirmed the unresolved atomic helper and identified NuttX's existing
  interrupt-based implementation.
- Read the emulator's UART0 implementation: it captures output but always
  reports no received data. Serial interactive testing needs an input
  extension, a test harness, or physical UART access. The user subsequently
  authorized changes within `../wikireader/emulator`, preserving unrelated
  files and existing diffs, so that extension can be developed there. Its
  existing touch injection can exercise the proposed keyboard.

This was a feasibility assessment, not a NuttX build or hardware boot test.
The probe does not validate external-interrupt preemption, scheduler context
switching, LCD/touch operation under NuttX, or power management. The separate
NuttX applications tree, which contains NSH, will also be needed for the port.
