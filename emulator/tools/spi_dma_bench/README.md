# SPI DMA beside the CPU

`spibench.app` is a Grifo application that times SPI DMA streams while the
CPU runs controlled loops, to check wremu's `dma_async` model, its
`dma_extra` per-transfer cost and its `dma_cpu_penalty` bus hold against the
device. Memory-to-memory DMA is the [memory-copy
benchmark](../mem_dma_bench/README.md)'s; this one is the card's path.

Each case sends 1,024 characters with the card deselected, so only the SPI,
the DMA engines and the bus are measured. The SPI runs at the clock and
inter-character wait Grifo set. HSDMA3 takes every received character, on
receive full, into SDRAM or into the IVRAM window the display does not use.
The transmitter is fed all-ones from an IVRAM word either by HSDMA2 on
transmit empty, as the Linux driver and Grifo's word path do, or by IDMA on
receive full, as Grifo's byte path does. Transmit-empty pacing hides the
DMA's latency behind the character on the wire, so the 8-bit and IDMA
cases are the ones that show it.

The CPU meanwhile runs one loop, from A0 RAM unless noted, until HSDMA3
finishes, reading the completion flag once a pass:

| Loop | Each pass |
| --- | --- |
| `spin` | nothing but the flag |
| `load1`, `load4` | one or four sequential SDRAM loads |
| `store1`, `store4` | one or four sequential SDRAM stores |
| `copy4` | four loads then four stores, a word copy |
| `rowmiss` | one load a row (1 KiB + 4) further on, a row change each pass |
| `moves-a0` | twenty register moves |
| `moves-sdram` | the same from SDRAM, too long for the fetch queue |
| `lookup4` | four halfword loads from 2 KB of tables in A0 RAM |
| `crc` | the Linux card driver's CRC and unpack of one word: a stream load, `lookup4`'s loads and a swapped store |
| `unpack0`, `unpack2` | the kernel's `sd_unpack_crc()` itself, its machine code from `vmlinux` (`unpack_crc.inc`), one 512-byte block a pass, aligned and two bytes on; 400 passes alone |

The same six of them run again from IVRAM switched into area 0 (`-iv`), the
place the kernel runs its hot code, with the LCD controller's IRAM bit set,
their tables still in A0 RAM and the transmitter fed from DST RAM, beside
32- and 8-bit streams into SDRAM.

Each case runs three times with interrupts masked, and the same loop is
timed alone for 20,000 passes. A `RESULT` line gives the raw ticks, passes
and SPI status, and the medians as cycles a character (`unit_x100`) and
cycles a pass beside the DMA (`pass_x100`) and alone (`solo_pass_x100`),
all times 100. `ok=0` means a transfer did not finish within 200,000 passes,
overran its receiver or left a count behind. The `CONFIG` line records the
SPI, SDRAM controller and clock settings. The SPI control, DMA mode and
trigger selection, chip select, IDMA table base and the borrowed IVRAM and
DSTRAM bytes are restored afterwards; Grifo programs the channels afresh for
every transfer of its own.

## Device results

`device-2026-10-02.txt` is a 32 MiB reader running Grifo's launcher. All 72
cases finished, so 8- and 16-bit HSDMA transmission works too.

With HSDMA feeding the transmitter, every character takes its wire time and
8 MCLK more, to within 0.2 cycles: 136.1, 72.1 and 40.1 cycles at 32, 16
and 8 bits, whatever the CPU runs and wherever the data goes. The DMA never falls
behind; the 8 cycles are the SPI's own, two divided-clock periods where the
manual's SPI_WAIT + 1 gives one. Receive-paced IDMA takes 69 to 86 cycles a
character at 8 bits and 172 to 188 at 32, the most beside code running from
SDRAM.

The transfers take time from the CPU, which the device's DMA outranks:
beside an HSDMA stream into IVRAM the loops lose 1 to 11 cycles a
character, into SDRAM 2 to 20, the most for code running from SDRAM, and
beside IDMA 3 to 26. Alone, the two loops that do nothing on the bus but
read the flag, `spin` and `moves-a0`, take 3 cycles a pass less on the
device than in wremu (13 against 16 for `spin`), which points at the
interrupt controller's register being cheaper to read than the SDRAM
controller's that set `mmio_wait`.

`device-2026-11-04.txt` adds the loops after `moves-sdram`. Every loop
takes the same cycles from IVRAM switched into area 0 as from A0 RAM, alone
and beside a stream, as the manual has it. A store costs the CPU about three
cycles more than an SDRAM write's bus time hides, and the interrupt
controller's flag reads three fewer than other registers: wremu's
`write_issue` and `itc_wait`. The kernel's own unpack loop is the miss left:
the device takes 7,456 cycles a block aligned and 8,354 two bytes on, wremu
about 7,090 and 7,630, though `crc`, the same accesses compiled
differently, agrees to a cycle. Write-to-read turnaround, write recovery
before a precharge, row-change cost, a fetch wait for that IVRAM and
register-shift cost each either fail to close it or make another of the
device sets worse.

The instruction loops (`i-*`, run from IVRAM in area 0) give each of that
loop's instructions on its own: every register operation, shifts by any
amount, extensions and `swap` take one cycle on both; a halfword load from A0
RAM takes the device 2.12, a word load 2.50, a store 1.50, against wremu's
one each, which is `a0_read_half`, `a0_read_word_half` and `a0_write_half`.
With them the kernel's loop alone models at 7,906 cycles a block against the
device's 7,992. A run of `ext`-and pairs takes the device one cycle a pair
and wremu two, but freeing such prefixes makes the riscv interpreter's A0
variants 9% fast, so `ext` keeps its cycle; a load used by the next
instruction costs the device 0.8 more than wremu.

wremu's fit to these is in the [emulator README](../../README.md)
(`dma_bus_hold` and the parameters beside it).

## Running it

```sh
make -C emulator/tools/spi_dma_bench
python3 emulator/tools/spi_dma_bench/run.py
python3 emulator/tools/spi_dma_bench/run.py --no-build \
  --device emulator/tools/spi_dma_bench/device-2026-10-02.txt
```

`run.py` boots wremu through the launcher (MBR flash, Grifo, `init.app`,
`spibench.app off`), which powers off when done, and prints wremu's results
beside the device's when given its log. `WREMU_MODEL` passes through.

On the device, copy `build/wr128/spi-dma/spibench.app` and `spibench.ico`
to the card's root and add `spibench.ico : spibench.app` to `init.ini`. Tap
the framed square in the launcher; the run takes about ten seconds, writes
`spibench.log` to the card after every case, and returns to the launcher at
a tap.
