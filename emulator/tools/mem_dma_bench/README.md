# C33 memory-copy benchmark

This Grifo application measures 120 cases, three samples per case.
It compares the existing libc copy, an eight-word CPU batch from SDRAM,
the same batch from A0 RAM, and software-triggered HSDMA0. The kernel, SD
transport and Wikipedia app are unchanged.

The cases cover aligned 64-byte through 512-KiB copies between different
rows of the same SDRAM bank and between different banks; 64-byte through
5-KiB copies to IVRAM; byte and halfword DMA; CPU/DMA fills; overlapping
backward copies; block transfers that restore the source after each
64-byte pattern; and 16-KiB DMA and A0-batch copies with their buffers
20 KiB to 7 MiB apart. A 12-MiB allocation provides owned buffers with explicit
4-MiB bank spacing. Only the unused IVRAM window is borrowed, at 0x81a00;
the visible framebuffer stays at 0x80000. Both the window and the borrowed
word at 0x84400 are saved and restored. The latter is outside this kernel's
SD descriptor table, in the stack region used by Zstd when it runs later.

Every sample initializes nonuniform data, poisons the destination, times
the complete call, then verifies every output word and surrounding guards.
Pattern generation, verification, rendering and filesystem I/O are outside
the timed interval. DMA setup and completion checks are included. Logs
contain all three raw 60-MHz tick measurements plus min/median/max, buffer
addresses, memory-controller settings, timer-call overhead and failure
registers. Results are not baseline-subtracted. Small-copy timings therefore
include timer and setup costs, as an actual caller would encounter them.

The app requires idle DMA channels, uses HSDMA0 with IRQ delivery masked,
and keeps storage outside timed runs. It uses unlimited DMA bus ownership,
with transfers capped at 512 KiB. The software timeout can run only when
the CPU gets the bus; the watchdog remains enabled for a controller stall.
It does not test simultaneous SD DMA, CPU/DMA overlap or bus-release limits.

On the device it is a launcher entry: copy `membench.app` and `membench.ico`
from `build/wr128/mem-dma` to the card and add `membench.ico : membench.app`
to `init.ini`. Each case has a closed log checkpoint in `membench.log`
before it starts. After success or a recoverable failure, it restores the
DMA configuration, frees its buffers and returns to the launcher at a tap;
with `off` as its argument, as the model runner gives it, it powers off.

## Model and tests

The local S1C33E07 manual is the source for the controller semantics:

- II.1.1: two bus phases per unit; accessible memory areas.
- II.1.3.2: unlimited DMA bus ownership blocks CPU access.
- II.1.3.3 and II.1.6.1: count encodings, address updates and block resets.
- II.1.5: software triggers, pending flags and channel priority.
- II.4.2.3: DMA reads use the SDRAM data queue, as CPU data reads do.

The model executes every DMA read and write through the existing SDRAM
timing model, including word/halfword bus widths, row activation, data-queue
hits, turnaround and refresh. It does not interpret "block" as a large FIFO.
The CPU stalls until an unlimited successive transfer or block completes.

The per-unit memory-DMA arbitration/internal overhead is not isolated yet.
`dma_mem_extra=0` adds only the bus phases to the existing memory timings;
2 and 4 provide sensitivity comparisons. These are assumptions, not error
bars. The prior `dma_extra=30` calibration describes the SPI path and remains
unchanged. Cross-bank SDRAM command/data overlap and IVRAM arbitration with
the live LCD remain approximate or unmodeled. The hardware comparison below
shows why one overhead constant cannot resolve every path's timing error.

From the repository root:

```sh
make -C emulator test-mem-dma test-dma test-sd-dma-driver test-sdramc
make -C emulator/tools/mem_dma_bench
python3 emulator/tools/mem_dma_bench/run.py
python3 emulator/tools/mem_dma_bench/run.py --no-build --extra 2
python3 emulator/tools/mem_dma_bench/run.py --no-build --extra 4
```

The runner uses the local C33 toolchain, the built launcher and the
currently built `samo-lib/grifo/grifo.elf` kernel (its hash is recorded in
each result).
It constructs a roughly 65-MB sparse FAT
fixture and boots it the way the device does: mask ROM, MBR, the FLASH boot
loader `samo-lib/mbr/make-flash.py` builds, the kernel, the launcher and the
benchmark. Neither a ZIM nor an attached card is read.
Builds, images, logs and JSON results go to `build/wr128/mem-dma`.

## Device results

`device-2026-10-02.txt` is the 32 MiB reader under today's Grifo (SDRAM
controller `0x1243`, refresh `0x01ff01c0`, as wremu runs it). All 120 cases
pass: 360 timed transfers, every output word and guard checked. Times below
are medians of three samples, including setup and timer calls:

| Copy | Bytes | Existing libc | CPU batch, SDRAM code | CPU batch, A0 code | DMA32 |
| --- | ---: | ---: | ---: | ---: | ---: |
| Same bank, different rows | 524,288 | 54.630 ms | 47.489 ms | **34.671 ms** | 37.515 ms |
| Different banks | 524,288 | 37.893 ms | 44.386 ms | 32.652 ms | **20.179 ms** |
| SDRAM to IVRAM | 5,120 | 360.1 us | 412.5 us | 284.6 us | **210.9 us** |
| Fill from fixed internal word | 524,288 | 11.164 ms | - | - | **8.969 ms** |

Different-bank DMA takes 46.7% less time than libc and 38.2% less than the
A0 batch. Same-bank A0 batching takes 36.5% less time than libc and 7.6% less
than DMA. Large fills take 19.7% less time with DMA. Copying the 5-KiB IVRAM
window saves 149 us versus libc, or 74 us versus the A0 batch; these are
small absolute savings, not a measured improvement to the reader UI.

**Banks are 4 MB, and a copy pays for sharing one.** The separation sweep
copies 16 KiB by DMA and by the A0 batch with the buffers 20 KiB to 7 MiB
apart. The DMA copy costs 1,209 us (17.7 cycles a word) at every separation
up to 3 MiB and 667 us (9.8) from 4 MiB on, where the destination moves into
the next bank; the A0 batch, which changes row twice per eight words rather
than twice per word, goes from 1,095 to 1,033 us. Two *read* streams cost
the device the same whatever their separation (ubench), so the controller
keeps a row each for reads and writes, and a bank can hold only one of them
open.

Among tested sizes, DMA first beats libc at 1 KiB in all three copy layouts.
It first beats the A0 batch at 4 KiB for different-bank and IVRAM copies;
the A0 batch wins every tested same-bank size. DMA loses the 4-KiB fill but
wins at 64 and 512 KiB. These bracket crossovers between sampled sizes, not
exact thresholds for a production dispatcher. Tests use aligned addresses
and one pair of buffers per layout.

Byte, halfword and word DMA all copied correctly. At 4 KiB, same-bank times
were 470.4, 366.0 and 329.9 us respectively; different-bank times were 335.2,
229.6 and 194.6 us. The backward overlapping DMA copy took 198.5 us versus
333.7 us for memmove. The 64-byte source-reset block test completed in
454.9 us, but has no equivalent CPU-pattern-fill control in this benchmark.
It verifies the mode, not an advantage over a tuned CPU implementation.

wremu with the same Grifo and launcher matches the 120 cases to 7.0% RMS
(log error; DMA 8.2%, CPU 6.3%) with reads and writes in row registers of
their own that conflict within a bank (`rw_ports`, `bank_conflict`) and no
controller overhead on a DMA row change beyond tRP and tRCD
(`dma_row_change_extra`). The large DMA copies come out 6.1% long within a
bank and 11.1% long between banks, the A0 batch 1.7% and 4.7% short, libc
2.9% and 10.5% short, the 5-KiB IVRAM DMA 8.8% short, the 512-KiB DMA fill
within 0.4% and the block-reset case 8.5% long. All percentages use
hardware time as the denominator. With one row register for all DMA, the
model had the DMA copies 35.6% and 152.1% long.

These results are what the reader's bulk-copy helper is built on: an A0 CPU
batch for ordinary copies and DMA for large ones and for fills. They do not
measure end-to-end page speed or CPU/DMA concurrency; unlimited DMA owns the
bus throughout each transfer.
