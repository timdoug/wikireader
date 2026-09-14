# C33 memory-copy benchmark

This one-shot Grifo application measures 94 cases, three samples per case.
It compares the existing libc copy, an eight-word CPU batch from SDRAM,
the same batch from A0 RAM, and software-triggered HSDMA0. The kernel, SD
transport and Wikipedia app are unchanged.

The cases cover aligned 64-byte through 512-KiB copies between different
rows of the same SDRAM bank and between different banks; 64-byte through
5-KiB copies to IVRAM; byte and halfword DMA; CPU/DMA fills; overlapping
backward copies; and block transfers that restore the source after each
64-byte pattern. A 12-MiB allocation provides owned buffers with explicit
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

Before the first transfer, the app consumes `membench.on` and restores the
normal `init.ini`. Each case has a closed log checkpoint before it starts.
After success or a recoverable failure, it restores the DMA configuration,
frees its buffers and chains `zim.app`. A subsequent boot runs Wikipedia.
The hardware installer verifies the previously tested kernel identity and
backs up the small boot files before changing the launch command.

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

The runner uses the local C33 toolchain and small captured boot files in
`build/wr128/hsdma-tx/hsdma-boot`, with the currently built
`samo-lib/grifo/grifo.elf` kernel (its hash is recorded in each result).
It constructs a roughly 65-MB sparse FAT
fixture and executes the real file-loader, current tested kernel, launcher
and benchmark. The harness supplies the loader's inherited stack; earlier
FLASH stages are not modeled. Neither a ZIM nor an attached card is read.
Builds, images, logs and JSON results go to `build/wr128/mem-dma`.

## Device results

All 94 cases pass on the 32 MiB reader: 282 timed transfers, every output
word and guard checked. Times below are medians of three samples, including
setup and timer calls:

| Copy | Bytes | Existing libc | CPU batch, SDRAM code | CPU batch, A0 code | DMA32 |
| --- | ---: | ---: | ---: | ---: | ---: |
| Same bank, different rows | 524,288 | 57.608 ms | 48.006 ms | **34.856 ms** | 37.834 ms |
| Different banks | 524,288 | 40.960 ms | 44.910 ms | 33.244 ms | **20.561 ms** |
| SDRAM to IVRAM | 5,120 | 360.083 us | 412.100 us | 286.900 us | **225.017 us** |
| Fill from fixed internal word | 524,288 | 11.328 ms | - | - | **9.130 ms** |

Different-bank DMA takes 49.8% less time than libc and 38.2% less than the
A0 batch. Same-bank A0 batching takes 39.5% less time than libc and 7.9% less
than DMA. Large fills take 19.4% less time with DMA. Copying the 5-KiB IVRAM
window saves 135 us versus libc, or 62 us versus the A0 batch; these are
small absolute savings, not a measured improvement to the reader UI.

**Treat the bank labels as "4 MB apart" and "close together", not as banks.**
Buffers here are spaced by an assumed 4 MiB bank stride. A later probe found
that two *read* streams cost the device the same whatever their separation,
which is why the reader no longer places anything by bank, and the timing
model charges no distance penalty. That probe does not cover a copy, and this
benchmark plainly measured distance mattering to one -- 57.6 ms against
41.0 for libc, 37.8 against 20.6 for DMA -- while barely touching the
batched A0 copy. Nothing here explains that; it is the open question in this
file.

Among tested sizes, DMA first beats libc at 1 KiB in all three copy layouts.
It first beats the A0 batch at 4 KiB for different-bank and IVRAM copies;
the A0 batch wins every tested same-bank size. DMA loses the 4-KiB fill but
wins at 64 and 512 KiB. These bracket crossovers between sampled sizes, not
exact thresholds for a production dispatcher. Tests use aligned addresses
and one pair of buffers per layout.

Byte, halfword and word DMA all copied correctly. At 4 KiB, same-bank times
were 485.8, 377.5 and 341.7 us respectively; different-bank times were 351.5,
243.7 and 206.7 us. The backward overlapping DMA copy took 210.2 us versus
333.9 us for memmove. The 64-byte source-reset block test completed in
480.7 us, but has no equivalent CPU-pattern-fill control in this benchmark.
It verifies the mode, not an advantage over a tuned CPU implementation.

The zero-extra-overhead model got the main winners right, but timing errors
depend on the path. It predicts 42.349/25.131 ms for the large same/different
bank DMA copies: 11.9%/22.2% longer than hardware. Conversely, its A0 batch
predictions are 5.6%/6.9% shorter than hardware. Its 5-KiB IVRAM DMA prediction
is 9.3% short. The 512-KiB DMA fill and block-reset case agree within 0.3%.
All percentages here use hardware time as the denominator.

For the large SDRAM copies, the model excess is about 2.1 MCLKs per word
in both bank layouts. That suggests investigating shared read/turnaround
timing or phase overlap; this experiment does not isolate the cause. Adding
positive `dma_mem_extra` makes those predictions worse, and spoils the close
fill result. No global timing calibration was changed to force a fit.

The logged SDRAM configuration is `0x1353` on hardware and `0x1352` in the
loader fixture (32 versus 16 MiB). The model uses the same 1-KiB row and
4-MiB bank spacing for both settings, and all benchmark buffers fit below
16 MiB at identical addresses. Refresh settings match after masking the
read-only SELDO status bit: hardware `0x01ff0120`, model `0x03ff0120`.
The model currently reports SELDO whenever self-refresh is enabled; that
readback is a known approximation, not a different refresh interval.

These results are what the reader's bulk-copy helper is built on: an A0 CPU
batch for ordinary copies and DMA for large ones and for fills. They do not
measure end-to-end page speed or CPU/DMA concurrency; unlimited DMA owns the
bus throughout each transfer.
