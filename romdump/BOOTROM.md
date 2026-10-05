# The S1C33E07 boot ROM

The S1C33E07's "specific ROM" is a 64 KB mask ROM at 0x20000-0x2ffff (area 1),
fixed when the chip is made.  It holds the boot code for the chip's five boot
modes and an MP3 decoder the Technical Manual describes only as "extended core
functions".  `romdump` read it from three WikiReaders, two V7 boards and a V5;
all three dumps are identical:

| | |
| --- | --- |
| Size | 0x10000 bytes, of which 0x0000-0xbfbf is used |
| CRC-32 | 5efd1ab8 |
| SHA-1 | 0d7fd66f92fdfaeeb2ebf46c820bff54ebacf006 |

Plain loads read 0x2c000-0x2ffff as zeros, but the decoder calls code there:
see [The unreadable part](#the-unreadable-part).  Addresses below are CPU
addresses; the dump starts at 0x20000.  Section numbers refer to the S1C33E07
Technical Manual (`id001557.pdf`).

How much of it Epson documents is worth saying plainly.  The boot code is
1114 bytes, 1.7% of the ROM, and Appendix D covers it completely: a section
for each boot mode, with flowcharts, pin states and data formats, and what
the code does agrees with it throughout.  The decoder is the other 78%, and its
documentation is three words in I.6.3 - "extended core functions" - followed
by "Make sure this area is not accessed from the user program or debugger".
Neither manual mentions MP3, MPEG, Layer III or audio at all.  Everything
below about the decoder was read out of the ROM or measured on a device.

## Layout

| Address | Contents |
| --- | --- |
| 0x20000 | chip ID, 4 bytes |
| 0x20004 | boot entry table, 4 jumps |
| 0x20014-0x2045a | boot code |
| 0x23000 | decoder entry table, 8 pointers |
| 0x23020-0x2714f | decoder core |
| 0x27150-0x27df3 | decoder interface: entry points, interrupt handler, DMA and I2S |
| 0x27df4-0x2bfbf | decoder tables |
| 0x2c000-0x2ffff | reads as zeros; holds decoder code |

## Chip ID

0x20000 holds `06 0e 07 21` (I.5.8): C33 PE little-endian core, E series,
model E07, version 0x21.  The WikiReader's MBR prints it as
"S1C33E07 V 0x21".

## Boot

At reset the C33 PE takes its vector from TTBR, initially 0xc00000, where a
four-word gate ROM holds the entry into this ROM for the boot mode the BOOT1,
BOOT0 and #CE10 pins select (I.6.1, D.1):

| Entry | Jumps to | Boot mode |
| --- | --- | --- |
| 0x20004 | 0x2005e | NAND flash, large or small page by #CE10 |
| 0x20008 | 0x20248 | PC RS-232C |
| 0x2000c | 0x20338 | NOR flash or external ROM |
| 0x20010 | 0x20384 | SPI EEPROM, or RS-232C when #CE10 is low |

Bit 1 of the Boot Register (pMISC_BOOT, 0x300018), BOOT_ENA, makes the reset
vector come from the gate ROM.  The boot code clears it (routine 0x2004c,
after writing 0x96 to the Misc Protect Register) just before it jumps to the
loaded code, so afterwards 0xc00000 is the external #CE10 area and reads as
whatever the bus holds: two runs on the same unit read different values.

The SPI and RS-232C sequences run with the stack at 0x84700 (DSTRAM) and
interrupts off; the SPI sequence starts by unlocking the clock management unit
(routine 0x20034: 0x96 to pCMU_PROTECT, clocks on).  Appendix D documents the
modes, and the code read here agrees with it.

### SPI EEPROM (D.4)

The WikiReader's mode.  0x20384 makes P57 an input and reads it: low means
#CE10 is held to ground, and it goes to the RS-232C sequence instead.  The pin
has an internal pull-up, so a board boots from its EEPROM unless something
pulls it down.  Then:

1. P52 becomes the EEPROM's chip select, and P65-P67 the SPI pins.
2. The SPI runs at 8-bit characters, MCLK/16, master (pSPI_CTL1 = 0x1c23).
3. READ STATUS (0x05) until the write-in-progress bit clears.
4. READ (0x03) with a four-byte address of zero, then one discarded byte.
5. 512 bytes into A0RAM from 0x0.
6. Routine 0x2004c, then a jump to 0x0.

An EEPROM with three address bytes is already sending data during the fourth,
so the 512 bytes the ROM keeps start at EEPROM offset 1 (D.4.3).  The
WikiReader's MBR is stored there.

### PC RS-232C (D.5)

With BOOT1 and BOOT0 high and #CE10 (P57) low, the ROM boots from serial
channel 0 (P00 SIN0, P01 SOUT0).  It times the low pulses of four 0x80 bytes
from the PC with a 16-bit timer to find the baud rate, sends the 4-byte chip
ID, receives 512 bytes into A0RAM from 0x0, sends them back for the PC to
check, and jumps to 0x0.  The baud rate is 9600 bps typically; Table D.5.1.2
gives the maximum for each OSC3 frequency.  On a WikiReader, #CE10 is a test
point: grounding it at power-on and sending an MBR over the serial console
starts a board whatever its EEPROM holds.

### NOR flash and NAND flash (D.2, D.3)

Not used by the WikiReader and not examined beyond the entry table.

## The MP3 decoder

A fixed-point MPEG-1 and MPEG-2 Layer III decoder that plays through the I2S
port.  The manual does not mention it; nothing here comes from documentation.

Nor is there any to find.  A search of Epson's published documents in October
2026 turned up nothing on it: no middleware manual, no application note, and
no S5U1C33-series library package.  What the record does show is why it is
here.  The S1C33E07 was released in May 2006 for electronic dictionaries and
label printers, and in December 2006 Epson announced the S1C33E08, the same
processor plus an MP3 hardware accelerator, advertised as a "built-in MP3
decoder and playback API" good for 48 kHz and 320 kbit/s at 88 mW.  So this
ROM holds the software decoder that came first, and the measurements below
say why a hardware one followed: it leaves 5% of a 60 MHz CPU at 44.1 kHz
stereo and cannot keep up at 48 kHz at all.

Evidence for what it is:

- MPEG scalefactor band tables (16-bit): MPEG-2 22.05 kHz long blocks at
  0x2a958, MPEG-1 44.1 kHz long at 0x2aa36 and short at 0x2aa64, 48 kHz long
  at 0x2aa80, 32 kHz long at 0x2aaca.
- The frame header parser (0x27934) reads the version bit (19) and the
  sample-rate index (bits 11:10) and sets 1152 or 576 samples a frame and a
  rate of 44100, 48000 or 32000 Hz (MPEG-1) or 22050, 24000 or 16000 Hz
  (MPEG-2).  The rates are also a table at 0x27df4.
- The two decoding entry points save and restore the MAC registers (AHR,
  ALR).
- The bitstream readers (0x23218 n bits, 0x231a8 one bit) read 16-bit words
  through mask tables at 0x27fa8 and 0x27fec and refill from the input
  callback.

The interface below is from the code.  `mp3rom` (in `../mp3rom`) ran the frame
decoder on a V7 board through the internal routines `play` uses: 0x274f6
resets the state, 0x27520 primes the input and the decoder, and 0x27822
decodes one frame into an output half.  What it measured is under
[Output](#output), [Modes](#modes) and [Speed](#speed).  It did not call
`open` or `play`, which make P04 an I2S pin: on a WikiReader P04 also carries
the touch controller's serial output, and driving it would fight that.

### Calling convention

The table at 0x23000 holds the entry points.  Arguments and results follow
the C33 ABI: arguments in r6 upward, result in r4.  Every entry except 0 and
1 disables interrupts, switches to a private stack at 0x2fe8 and restores the
caller's stack and interrupt state on return.

| Entry | Address | Function |
| --- | --- | --- |
| 0 | 0x27150 | open: `int open(const config *c)`; 0, or -10 if the interrupt vector would not take |
| 1 | 0x27294 | close: undo everything open did |
| 2 | 0x2730c | play from the start of the input; returns the status |
| 3 | 0x27352 | stop |
| 4 | 0x2736e | pause |
| 5 | 0x2738a | resume |
| 6 | 0x273a6 | play, trying up to eight times to find a frame that decodes; returns the status |
| 7 | 0x273ec | clear the output buffers |

0x27408, outside the table, returns the state pointer.

### Memory

The decoder needs the 12 KB IVRAM moved into area 0 at 0x2000-0x4fff
(pLCDC_IRAM, 0x301a64 = 1).  `open` does that if it is not done already and
`close` puts it back, so the LCD cannot use IVRAM while the decoder is open.
It keeps there:

| Address | Use |
| --- | --- |
| below 0x2fe8 | its stack |
| 0x2000-0x200c | scratch |
| 0x2fe8-0x4fe7 | the synthesis filter's sample buffers |
| 0x4fe8-0x4ff7 | a buffer pointer and a ring index for each channel, then the interpolation carry at 0x4ff4 |
| 0x4ff8 | pointer to its state, S = workspace + 0x2400 |
| 0x4ffc | pointer to its DMA descriptor, workspace + 0x244c |

With IVRAM filled with 0xa5 beforehand, decoding changed exactly
0x2fe8-0x4fff.  That run used Grifo's stack, not the private one.

The private stack does not fit in IVRAM.  The deepest call chain needs about
10.7 KB, counting the frames in the code (0x23696, which decodes a frame's
granules, takes 0x24b8 bytes alone) but not the routines in 0x2c000-0x2ffff.
From 0x2fe8 that reaches about 0x600, in A0RAM (8 KB at 0x0-0x1fff, I.6.2).
The decoder treats area 0 as one 20 KB RAM, 0x0-0x4fff, and its interrupt
handler runs on that stack, so while it plays nothing else can live in A0RAM
above about 0x600.  Under Grifo that is the suspend code (0x800-0x1fff) and
the top of the vector table (0x400-0x7ff, `samo-lib/grifo/lds/grifo.lds`).

The workspace, from the configuration, holds the output first: two 0x1200-byte
halves, each one MPEG-1 frame of 16-bit stereo samples.  The state follows at
+0x2400, the DMA descriptor at +0x244c and the frame decoder's state at
+0x246c.  Filled with 0xa5, the workspace was changed up to +0x3877, so it
needs 0x3878 bytes.

### Configuration

`open` takes a pointer to seven words:

| Offset | Contents |
| --- | --- |
| 0x00 | workspace address |
| 0x04 | input buffer address; reads land at +0x3c0, 0x400 bytes at a time |
| 0x08 | not read by `open` |
| 0x0c | the CPU clock in Hz, for the I2S dividers |
| 0x10 | the synthesis mode, clamped to 0-2: see [Modes](#modes) |
| 0x14 | `int read(void *buffer, int bytes)`: the input callback, returning the bytes read |
| 0x18 | `void status(int status)`, called from the interrupt handler, or 0 |

The input is MP3 data supplied by the caller through `read`.  The decoder
swaps the bytes of each halfword it reads (`swaph`), so its bit readers take
the stream as big-endian 16-bit units.

### State

Fields of S that the interface code sets or tests:

| Offset | Contents |
| --- | --- |
| 0x00 | status |
| 0x04 | frame decoder state |
| 0x08 | status callback |
| 0x0c | input callback |
| 0x10 | bytes the last read returned |
| 0x14 | read size, 0x400 |
| 0x1c | input buffer |
| 0x20 | workspace |
| 0x24 | 0xf80 |
| 0x28 | output buffer size, 0x2400 |
| 0x2c | 1 stopped, 2 playing, 3 paused |
| 0x34 | set when a read returned less than asked: end of input |
| 0x3c | CPU clock |
| 0x40 | the HSDMA 0 vector `open` replaced |
| 0x44 | mode |
| 0x48 | 0 if `open` moved IVRAM and `close` should move it back |

Status 0 is success and 10 is the end of the input.  Other values are the
frame decoder's error code (its state at +0x64).

### Playback

`play` resets the state, sets up the DMA, reads the first input, decodes two
frames to fill both output halves and starts the transfer.  From then on the
decoder runs in the interrupt handler, not in the caller:

- `open` installs the handler, 0x27416, as the vector for HSDMA channel 0
  (TTBR + 0x58, vector 22), keeping the old one for `close`.
- HSDMA channels 0 and 1 move the samples into the I2S FIFO (0x301c20 and
  0x301c22), triggered by the I2S port (pHSDMA_HTGR1 = 0x99).
- I2S is set up on P04-P07 (pP0_47_CFP = 0xaa), with its DMA mode
  (pI2S_HSDMAMD = 1), its control bits and clock dividers computed from the
  CPU clock and the stream's sample rate (0x27c00), and started through
  pI2S_START.
- The DMA interrupt is enabled at priority 7; `open` saves and `close`
  restores the interrupt controller's DMA and I2S settings and the port
  function.
- Each completion interrupt clears the DMA cause, points the DMA at the half
  just filled, reads input and decodes the next frame into the other half.
  When the input ends or a frame fails it stops and calls the status
  callback.
- The input ends when a read returns less than the read size (the refill at
  0x27874 sets S+0x34).  The handler tests that before it decodes, so the
  frames still in the 0xf80-byte input buffer are never played.  Decoding
  the same way, the device run got 76 of t44s's 79 frames and 74 of t22s's
  80.
- P11 is driven high for the length of the handler (bit 1 of 0x300382), which
  makes the decoder's load visible on a scope if the pin is a plain output.

`stop` stops the DMA and I2S and moves to state 1.  `pause` does the same from
state 2 to 3, and `resume` restarts the DMA from 3 to 2.

### Output

Each output half holds one frame as two blocks of 16-bit little-endian
samples: the left channel's 1152, then the right's.  A 576-sample MPEG-2
frame fills 0-575 and 1152-1727 (the rest of the half kept the test's zero
fill), and the DMA count (descriptor word 5) is the frame's sample count.  One
HSDMA channel feeds each side of the I2S FIFO.

The test streams were LAME-encoded: tones, to measure precision, and pink
noise at each sample rate, to find where the output stops.
`../mp3rom/README.md` has the recipe.  Compared with mpg123's decoding of
the same files:

- Samples are at half mpg123's scale.  In mode 0 they are mpg123's halved,
  to within one count on the tones (65.7 dB SNR) and 68-70 dB SNR below
  15 kHz on t44j.
- The output stops at about 15.8 kHz whatever the sample rate, so the limit
  is a frequency and not a count of subbands.  Pink noise at each rate, full
  to the encoder's own limit, came out:

  | Stream | Falls away at | Subbands that reach it | Frame |
  | --- | --- | --- | --- |
  | 44.1 kHz | 15.85 kHz | 23 of 32 | 24.8 ms |
  | 48 kHz | 15.75 kHz | 21 of 32 | 24.2 ms |
  | 32 kHz | not at all | 32 of 32 | 27.9 ms |
  | 22.05 kHz (MPEG-2) | not at all | 32 of 32 | 14.1 ms |

  At 44.1 kHz the output is down 6 dB at 15.8 kHz, 26 dB at 16.0 and gone by
  16.2, where mpg123 still decodes at full level.  32 kHz and 22.05 kHz reach
  15.8 kHz in fewer than 32 subbands, and nothing of them is lost.  A frame
  costs about 0.34 ms for each subband synthesised, which is why 32 kHz,
  every subband of which is under the limit, costs more a frame than 48 kHz.
- A mono stream puts its 1152 samples in the left block and does not write
  the right one, so playing it through I2S as it stands would leave one
  channel silent.  A frame costs 13.9 ms.
- MPEG-2.5 is rejected.  An 11.025 kHz stream (header `ff e3`) decoded no
  frames and returned status 0xffffffff after three reads, so the parser
  reads only the MPEG-1 version bit.
- LAME's Info header in the first frame is not recognised.  The decoder
  outputs that frame as silence, then the stream with the encoder's delay
  and padding, which a gapless decoder would trim.
- A run with the workspace and IVRAM filled with 0xa5 instead of zeros
  produced the same samples, so the decoder initialises everything it reads.

### Modes

The mode selects the synthesis routine (the dispatch is at 0x270e4):

| Mode | Routine | Samples computed | ms a frame, t44s |
| --- | --- | --- | --- |
| 0 | 0x2d140 | all | 24.8 |
| 1 | 0x2d1d0 | every second | 22.3 |
| 2 | 0x2d270 | every fourth | 20.6 |
| 3 | 0x2d320 | not reachable: `open` clamps the mode to 0-2 | |

Modes 1 and 2 compute one sample of each pair or group of four (granule
positions 1, 3, 5, ... and 3, 7, 11, ...) and fill in the rest by linear
interpolation from the previous computed sample.  Every interpolated sample
tested is the exact integer ramp between its neighbours: 214,800 of them in
stereo and 43,774 of 43,774 in mono, apart from those at the start of a
granule (below).  Going from mode 0 to mode 2 saves 4.2 ms a frame, 17%,
which puts the full synthesis at about 5.5 ms of the 24.8.

What they compute is not simply mode 0's samples at those positions.  On a
440 Hz tone they match it to within a few counts, but on pink noise they do
not: in mono the computed samples differ from mode 0's by 118 rms and 620 at
worst, against a signal of 816 rms, and no filter of mode 0's output over a
window of nine samples accounts for them (54 rms residual).  So the reduced
modes change the synthesis itself, not just how often it runs.  How is not
established.

The cost of that is all in the content.  Against mpg123 below 15 kHz:

| Stream | Mode 0 | Mode 1 | Mode 2 |
| --- | --- | --- | --- |
| 440 Hz and 1 kHz tones, stereo | 68 dB | 32 dB | 25 dB |
| pink noise, mono | 70 dB | 13 dB | |

Mode 1's error on noise is spread over the whole band, 33 dB down at
0-5 kHz and no better than the signal above 9 kHz, so these modes suit
narrow-band material and little else.

Both channels share one interpolation carry, the word at 0x4ff4.  Each
granule's first interpolated samples (one in mode 1, three in mode 2)
therefore start from the other channel's last sample:

- the left channel's from the right channel's last sample of the previous
  granule;
- the right channel's from the left channel's last sample of the same
  granule.

All 298 such samples in mode 1 and all 894 in mode 2 follow this, and it
clicks every 576 samples, with errors up to 1482 counts on a tone that peaks
at 972.  A mono stream is unaffected: all 149 of its granule starts
interpolate from its own previous sample, so this is a fault of the stereo
path alone.  The shared carry also shows the order: a granule's left channel
is synthesised before its right.

### Speed

`timer_get` timed each frame decode on a V7 board under Grifo: 60 MHz,
interrupts off, IVRAM in area 0, the workspace and the stream in SDRAM, and
the read callback copying from RAM.

| Stream | Mode | ms a frame | Audio a frame | CPU |
| --- | --- | --- | --- | --- |
| t44s, MPEG-1 44.1 kHz | 0 | 24.8 | 26.1 ms | 95% |
| t44s | 1 | 22.3 | 26.1 ms | 85% |
| t44s | 2 | 20.6 | 26.1 ms | 79% |
| t44j, joint stereo | 0 | 24.9 | 26.1 ms | 95% |
| t22s, MPEG-2 22.05 kHz | 0 | 14.0 | 26.1 ms | 54% |
| pink noise, MPEG-1 48 kHz | 0 | 24.2 | 24.0 ms | 101% |
| pink noise, MPEG-1 32 kHz | 0 | 27.9 | 36.0 ms | 77% |
| pink noise, 44.1 kHz mono | 0 | 13.9 | 26.1 ms | 53% |
| pink noise, 44.1 kHz mono | 1 | 12.5 | 26.1 ms | 48% |

Mode 0 at 44.1 kHz stereo leaves 5% of the CPU, before the cost of the DMA
interrupts themselves, and 48 kHz stereo does not fit at all: a frame takes
longer to decode than it lasts.  Mono and MPEG-2 have the room to spare.

### Not established

- The meaning of configuration word 2.
- The playback path, `open` through the I2S output, has not run on silicon.
- What the reduced modes do differently, beyond interpolating.
- What the seven routines in 0x2c000-0x2ffff besides the synthesis ones do.

## The unreadable part

The decoder calls eleven routines between 0x2c000 and 0x2d5c4: 0x2c000 from
0x23b32; 0x2c0c0, 0x2c484 and 0x2c6c0 each from two places near 0x263c8 and
0x264ca; 0x2d140, 0x2d1d0, 0x2d270 and 0x2d320 from 0x27116-0x27142; 0x2d410
and 0x2d500 from 0x23554 and 0x2356c; and 0x2d5c4 from three places.
0x2d140, 0x2d1d0, 0x2d270 and 0x2d320 are the synthesis routines for modes 0
to 3 ([Modes](#modes)), and modes 0-2 ran on silicon.
Several of the others are tail calls from short wrappers, so the code there
is real, but word loads from anywhere in 0x2bfc0-0x2ffff return zero on
all three units.  The readable data ends at 0x2bfbf on a table of fixed-point
coefficients.  The likeliest reading is that 0x2c000-0x2ffff is execute-only.
The boot code does not touch it, so booting from this dump is unaffected; the
decoder would not run from it.

Halfword loads, byte loads and a load made from inside the ROM (calling the
boot code's own `ld.w %r1,[%r10]; ret` at 0x20456) all read the area as zeros
too, so the protection does not depend on where the load comes from.  The ROM
repeats at 0x30000-0x3ffff, unreadable part included.

Four ways around the load were tried as well, and none of them reads it:

- **HSDMA.** A dual-address transfer with its source in area 1 runs to
  completion and reads nothing but zeros, from 0x20000 as much as from
  0x2c000, while the same channel moving RAM to RAM a moment earlier carried
  the right data.  So the engine cannot read area 1 at all, as II.1.9's
  chapter note says and its source-address register's note does not.
- **How long the load takes.** A word from 0x2c000 takes 809 ns, exactly as
  long as one from 0x20000 or 0x30000, against 1007 ns from SDRAM.  Something
  answers for the area in the time the rest of the ROM takes: it is mapped,
  and the zeros come from it rather than from an empty bus.
- **pMISC_COROM (0x30001A), the COROM Switch Register**, whose one bit the
  manual describes only as a test bit for factory use.  With it set the
  readable part reads the same as ever and the rest is still zeros, by load
  or by HSDMA.
- **pMISC_BOOT D1 (BOOT_ENA) set again.** The gate ROM does come back, and
  0xc00000 reads 0x00020010, the SPI boot entry; the hidden area is still
  zeros, so the lock does not follow the boot state.

What is left is hardware: the debug unit, whose trace pins the chip
multiplexes onto P15, P16 and P34-P36, or the mask itself under a
microscope.
