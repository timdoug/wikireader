# Llama 2 on the WikiReader

A port of [llama2.c](https://github.com/karpathy/llama2.c) to the 60 MHz
S1C33E07. It generates TinyStories text on the device, with every weight
held as int8.

`stories260K` runs at **119 milliseconds a token**, and agrees with an
fp32 reference on the top-1 prediction at every teacher-forced position.
There is no floating point anywhere in the forward pass: not in the
matmuls, not in RMSNorm, softmax, SwiGLU or RoPE, and not in the weight
file. That is where almost all of the speed came from -- see [what costs
what](#what-costs-what).

## Use

```sh
make host                 # host build of the same model and tokenizer
make test                 # teacher-forced accuracy against an fp32 reference

make TOOLCHAIN_BIN=../host-tools/toolchain-c33/work/install/bin
python3 tools/convert.py stories260K.bin model.wrl
python3 make-card.py -f /tmp/card.img model.wrl tok512.bin --args "-v -n 32"
../emulator/wremu -R -e ../samo-lib/mbr/flash.rom -c /tmp/card.img -n 900000000
```

Checkpoints come from
[tinyllamas](https://huggingface.co/karpathy/tinyllamas); `tools/convert.py`
reads the fp32 `.bin` files directly and needs numpy but not PyTorch.

Application arguments: `-n` tokens, `-i` prompt, `-v` to echo the story to
the serial console, `-once` to power the machine off after one run instead
of idling (the profiler's buckets are cumulative, so an idle loop buries
what it is meant to measure).

The app halts when it is finished, in `event_wait`, rather than polling
`event_get` in a loop. Polling kicked the watchdog a million times a run
and never let the guest halt, so under the emulator's window the host
burned a core simulating a machine doing nothing. **Timings printed under
`-g` are wall clock, not guest cycles** -- `main.c` calls
`timer_use_wallclock` when there is a window, deliberately, so quote the
headless number.

`-once` has to power off rather than return. Returning from `grifo_main`
hands control back to `init`, which finds one entry in `init.ini` and
chains straight back into the app -- so the story generates, the app
"exits", and it all starts again, which looks a great deal like a watchdog
reset and is not one. Powering off also needs a delay first: the serial
line runs at its baud rate, about ten thousand cycles a character, and
there is no flush in the grifo API, so cutting the power immediately
truncates the last line printed.

## What the part gives you

Three properties of the silicon decide the whole design.

**There is no FPU.** A soft-float multiply is a call into `__mulsf3`, which
is 226 instructions in this toolchain's libgcc; a call and return pair
alone measured 21 cycles on the device (`UB callret` in the NuttX
microbenchmark). A float multiply-accumulate costs around 250 cycles. An
int8 one costs 18.

**There is no MAC instruction either.** The S1C33E07 is the *PE* core, and
`emulator/src/c33.c` rejects `MAC`, `MAC.W`, `DIV.W`, `LOOP`, `REPEAT` and
the saturating operations as undefined (Table I.5.3.5). The multiply is
`mlt.h`, five cycles into ALR, and reading the product back is a separate
instruction. Integer and float division are both software.

**Weights are read once per token and memory is the other wall.**
Sequential word loads from SDRAM measured 9.44 cycles each on the device --
about 25 MB/s -- and the card does about 0.8 MB/s. A model that does not
fit in RAM is bound by the card and by nothing else.

## What costs what

Measured with `wremu -F` on `stories260K`, attributed by
`tools/profile.py`, before and after the activation path was converted:

| share of cycles | int8 weights, fp32 activations | everything integer |
| --- | ---: | ---: |
| soft float | **80%** | **0%** |
| `matmul` -- every int8 weight | 8% | 59% |
| normalisation and exponent bookkeeping | -- | 15% |
| `llama_forward` itself (attention, SwiGLU, RoPE) | 1% | 14% |
| integer divide, `exp`, `isqrt` | -- | 8% |
| ms a token, 8 tokens | 1214 | **118** |
| ms a token, 32 tokens | 1817 | **131** |

The first port quantized the *weights* but kept llama2.c's fp32
*activations*: quantize, int8 matmul, dequantize back to float, RMSNorm and
SwiGLU and softmax in float, quantize again. int8 covered the O(weights)
work and float covered the O(dim) work around it, which on this part is
backwards -- 259,328 integer multiply-accumulates at 18 cycles, wrapped in
about 24,000 float operations at 250.

The largest single contributor was the three soft-float operations ending
every output row of every matmul, `xout[i] = (float)acc * (w->s[i] * xs)`:
3,512 rows a token at dim 64, so ten thousand float operations spent
scaling the results of a quarter million integer ones.

**It was not fixable piecemeal.** Scaling a matmul row cheaply needs its
consumer to take fixed point, which needs RMSNorm and SwiGLU to produce it,
which needs the attention path to carry it. The whole activation path had
to convert at once.

### How the fixed point works

Every activation vector is `int32` values carrying a **power-of-two
exponent and nothing else**: `value[i] = v[i] * 2^-e`. Exponents add, so
a matmul's output exponent is just `input_e + tensor_e - shift`, and no
scale ever has to be multiplied into the data.

That works because a non-power-of-two scale appears in exactly one place --
RMSNorm's `sqrt(n)/sqrt(sum x^2)` -- and is multiplied into the data there,
where a single 32-bit divide a call covers a whole vector. RMSNorm is also
the one operation that is *easier* in fixed point than in float: the input
exponent divides out, because normalising is what it means.

Two rules keep every product inside an `int32`:

- **Nothing widens.** gcc compiles `(int64_t)a * b` to a call to
  `__muldi3`, which was 5% of the old profile on its own. Both operands are
  cut to fifteen bits before every multiply instead.
- **Nothing divides that does not have to.** There is no hardware divide
  either. The softmax normalises with one reciprocal per head rather than
  one per score, and attention weights come out in Q7 -- 128 is one, so the
  weighted sum of values carries a shift and not a division.

The KV cache is int8 with one exponent per (layer, position), a quarter of
what fp32 took. Positions therefore have different exponents, so scores and
values are brought to the smallest exponent in the cache before being
compared or summed -- shifting down is exact, shifting up would overflow.

`fixed.c` supplies what libm would have: `exp` by Horner in Q12 (each step
rounds; five truncations compound into more error than the dropped
fifth-order term did), a bit-by-bit integer square root, and a `ilog2`,
since the PE core drops `SCAN0`/`SCAN1` along with `MAC`.

Accuracy is measured rather than assumed. `make test` feeds a fixed token
sequence to both this and an fp32 numpy reference and compares the top-1
prediction at every position: **45 of 45**. Free-running greedy generation
follows llama2.c's fp32 output for 42 tokens and then takes a different but
equally sensible turn, which is what one differing logit does under argmax
and says nothing about the arithmetic.

### The matmul

gcc compiles the inner loop to exactly the seven instructions the part
allows, in fourteen bytes:

```
ld.b %r4,[%r5]+ ; ld.b %r9,[%r7]+ ; mlt.h %r4,%r9
ld.w %r4,%alr   ; cmp %r0,%r5     ; jrne.d ; add %r6,%r4
```

At fourteen bytes and offset two in its line it stays inside the 27-byte
fetch window, so it never refetches itself -- the profile shows 4.7% of its
cycles waiting on fetch.

It still started at **36.5 cycles per multiply-accumulate, with exactly
2.00 SDRAM row activations per MAC**. The two `ld.b` alternate between the
weight stream and the quantized activation vector, which are in different
1 KiB rows, so every single load re-activated a row -- including the weight
stream, which is otherwise perfectly sequential.

Moving the activation vector into A0 internal RAM (`llama_alloc_fast`, via
the `.fastbss` section the standard application linker script already
places there) took row activations to **0.13 per MAC and the matmul to
17.2 cycles per MAC**, 2.1x. It is 236 bytes of internal RAM.

The remaining gap to the ~12 cycles the instruction timings allow is the
two byte loads: `ld.ub` walking a buffer measured 7.08 cycles each, against
9.44 for a word load that carries four times as much. Loading four weights
per `ld.w` and unpacking them with shifts is the next thing to try.

**`model.o` is built with `-falign-loops=16`, and that is load-bearing.**
Editing the screen code in `llama.c` and nothing else once moved this loop
across a fetch-line boundary: its fetch stall went from 4.2% of its cycles
to 71.2%, its cycles per instruction from 2.46 to 8.78, and a token from
130 ms to 330. Nothing about the loop changed; the linker put it somewhere
else. Aligning it makes the number reproducible instead of lucky, and it
was worth 130 -> 119 ms outright, because `reduce_to` was straddling a line
too.

## Format

`tools/convert.py` writes int8 weights with **one scale per output row**,
not upstream's fixed groups of 64 -- and the scale is an int16 mantissa
over a shared power-of-two exponent, not a float.

That is not a preference. `runq.c` walks groups with `for (j = 0; j <= n -
GS; j += GS)`, which assumes the row length is a multiple of the group
size. stories260K's `hidden_dim` is 172, so on the `w2` matmul that loop
covers 128 of 172 weights and silently drops the other 44. The model then
generates `Once upon upon upon upon` and nothing in the tool chain reports
a problem. A scale per row divides every shape exactly. On this checkpoint
it is also no less accurate, because `dim` is 64 and the two schemes then
agree exactly.

One exponent for a whole tensor costs precision on its smallest row: a
spread of 2^k leaves the smallest scale 15 - k bits. Every array in these
checkpoints spreads by at most 13x, which leaves eleven bits, but
`encode_scales` checks rather than assuming -- a row scale that quietly
rounds to zero deletes an output row with no symptom except worse text.

The RoPE cos/sin table is in the weight file too: `run.c` calls `powf`,
`cosf` and `sinf` for every token, and there is no libm here -- mini-libc
has no `math.h` at all. The values depend only on the checkpoint's
`seq_len` and `head_size`.

## What else would fit

Per-token multiply-accumulates are roughly the parameter count, and at the
measured 18.1 cycles each on a 60 MHz part:

| model | dim / layers | int8 weights | MACs/token | forward |
| --- | --- | ---: | ---: | ---: |
| stories260K | 64 / 5 | 0.28 MB | 0.26 M | **measured 119 ms** |
| stories15M | 288 / 6 | 16 MB | 15.2 M | 4.4 s |
| stories42M | 512 / 8 | 45 MB | 43.7 M | card-bound: 45 MB a token at 0.8 MB/s |
| stories110M | 768 / 12 | 116 MB | 110 M | card-bound, minutes a token |

The 32 MB board holds stories15M; the 16 MB revisions do not. Above that
the weights do not fit and the card sets the rate.

**The vocabulary is the lever worth pulling.** stories15M spends 9.2M of
its 15.2M MACs a token in the 32000-entry classifier. llama2.c documents
training a custom tokenizer, and notes that a 4096-entry vocabulary trained
on TinyStories gives the same sequence lengths as the 32000-entry one. At
vocab 4096 a dim-288 model drops to 7.2M MACs and 7.6 MB, and a dim-128 one
to 2.1M MACs and 1.9 MB, which at the measured 17.2 cycles a MAC is about
0.6 seconds a token -- roughly reading speed.

## Files

| | |
| --- | --- |
| `model.c` | the forward pass; `matmul`, `quantize`, `rmsnorm`, `softmax` |
| `tokenizer.c` | SentencePiece BPE, reading llama2.c's own `tokenizer.bin` |
| `runner.c` | the generation loop, greedy |
| `fixed.c` | `exp`, integer square root and `ilog2`, because there is no libm |
| `llama.c` | Grifo entry, screen, and the internal-RAM arena |
| `host.c` | the same model and tokenizer behind malloc and stdio |
| `tools/convert.py` | fp32 checkpoint to int8, and the RoPE table |
| `tools/profile.py` | attributes a `wremu -F` profile using the link map |
| `tools/reference.py` | an fp32 forward pass in numpy, for the accuracy test |
| `make-card.py` | a FAT32 emulator card, built in memory and written once (`-f` to replace one, never a device node) |
