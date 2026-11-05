# wremu timing accuracy — where this stands

Written 2026-11-05. `README.md` documents the emulator and every model
parameter; `tools/spi_dma_bench/README.md` documents spibench and what its
device runs found. This file is the part that is opinion rather than record:
where the model still disagrees with the device, and what I would measure
next to close it.

## How accuracy is judged

A model change is kept only if it scores at least as well on every
device-calibrated set, not just the one that motivated it:

| Set | Device data | Run |
|---|---|---|
| spibench: loops beside SPI DMA, the kernel's CRC loop, single instructions from A0 RAM | `tools/spi_dma_bench/device-2026-11-05.txt` | `python3 tools/spi_dma_bench/run.py --no-build --device ...` |
| membench: memory copies, CPU and DMA | `tools/mem_dma_bench/device-2026-10-02.txt` | `tools/mem_dma_bench/run.py` (score its `model-0.log` against the device file) |
| ubench: instruction and memory loops under NuttX | `nuttx/.../tools/ubench-device.txt` | `run_ubench.py --compare` |
| riscv interpreter: four placements of code and state | `riscv/rv*-device.txt` | `riscv/fit-model.py` |

Each takes a `WREMU_MODEL=name=value,...` override, so a candidate can be
scored before it becomes a default. The end-to-end check is the kernel's own
card read: `check` on the device and in wremu (`card-test.py`), whose
`read_timing` gives cycles a block by phase and, under `alone`, the CRC loop
with no transfer beside it.

At the 2026-11-05 defaults the sets score: spibench solo 2.60 cycles a pass
RMS (DMA loss 6.40), membench 0.0707, ubench 0.0751 and riscv 0.1687 RMS of
log(model/device). Against the device's `check`: CRC alone 7,906 cycles a
block against 7,992, check 8,884 against 9,506, poll 2,843 against 2,099,
total 13,679 against 13,546; boot to prompt 4.29 s against 4.26.

## Where the model still disagrees

1. **ext prefixes from A0 RAM.** The device runs a stream of `ext`-and pairs
   at one cycle a pair; wremu charges two. The manual says a prefix takes
   "zero or one cycle depending on the instruction queue status". Every rule
   tried for the zero case (`ext_local`, `fetch_ahead`: a queue gaining two
   halfwords a free cycle, with and without memory waits counted) makes the
   riscv interpreter's A0 variants about 9% fast, so prefixes keep their
   cycle. spibench's `crc` and `lookup4`, which reach their tables through
   absolute addresses, run about 4% slow in wremu because of it.
2. **A load used by the next instruction** costs the device about 0.8 cycles
   more than wremu charges (`i-lduse` against `i-ldh` plus `i-xor`).
3. **The kernel CRC loop two bytes on** (`unpack2`) is still 2-6% fast, and
   the check/poll split leans the same way; items 1 and 2 are the likely
   remainder.
4. **The CPU's losses to SPI DMA**, 6.4 cycles a pass RMS in spibench, worst
   for code running from SDRAM beside a stream (`moves-sdram`).
5. **div and mul in the riscv set**, the largest single errors there (the
   device up to 1.9x the model), probably short branchy loops.

Ruled out for the CRC loop, each by a device set it made worse or failed to
move: SDRAM write-to-read turnaround (`wr_rd_turn`), write recovery before a
precharge (`write_recovery`), row-change cost, a fetch wait for IVRAM in area
0 (`ivram_a0_fetch_wait`; that IVRAM times as A0 RAM does on the device),
register shifts (one cycle at any count) and post-increment addressing.

## What is next, in the order I would take it

1. **One scoring command.** A script that runs all four sets under a given
   `WREMU_MODEL` and prints one line each, so every change is judged on all
   of them at once; they are run by hand today.
2. **The ext rule, measured.** A spibench round from A0 RAM: `ext` before a
   load, a store and a branch; after a load and after a register operation;
   single and double prefixes; and a mix like the riscv interpreter's. That
   should say what lets the queue run ahead, and let the free case be modelled
   without the interpreter going fast.
3. **A load-use charge**, fitted to `i-lduse` once ext is settled, and scored
   on all four sets.
4. **The kernel's check after each change**, on the device and in wremu,
   comparing check, poll and alone.
5. **The older misses**, each with a small spibench or ubench case of its
   own: SDRAM code beside a stream, and riscv's div and mul.
