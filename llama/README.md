# Llama 2 on the WikiReader

A port of [llama2.c](https://github.com/karpathy/llama2.c) to the 60 MHz
S1C33E07. It generates TinyStories text on the device, with every weight
held as int8.

`stories260K` runs and produces the same story llama2.c's fp32 `run.c`
does, token for token. It is not fast: **1.21 seconds a token**, where the
integer arithmetic alone would allow about 70 milliseconds. The gap is
measured and is almost entirely one thing -- see [what costs
what](#what-costs-what).

## Use

```sh
make host                 # host build of the same model and tokenizer
make test                 # checks the output against llama2.c's fp32

make TOOLCHAIN_BIN=../host-tools/toolchain-c33/work/install/bin
python3 tools/convert.py stories260K.bin model.wrl
python3 make-card.py /tmp/card.img model.wrl tok512.bin --args "-v -n 32"
../emulator/wremu -R -e ../samo-lib/mbr/flash.rom -c /tmp/card.img -n 900000000
```

Checkpoints come from
[tinyllamas](https://huggingface.co/karpathy/tinyllamas); `tools/convert.py`
reads the fp32 `.bin` files directly and needs numpy but not PyTorch.

Application arguments: `-n` tokens, `-i` prompt, `-v` to echo the story to
the serial console, `-once` to stop after one run instead of waiting for a
keypress (the profiler's buckets are cumulative, so an idle loop buries
what it is meant to measure).

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

Measured with `wremu -F` over 19 tokens of `stories260K`, attributed by
`tools/profile.py`:

| | share of cycles |
| --- | ---: |
| soft float (`__mulsf3`, `__unpack_f`, `__pack_f`, `__addsf3`, ...) | **80%** |
| `matmul` -- every int8 weight in the model | 8% |
| everything else | 12% |

The model's actual arithmetic is 8% of the runtime. The other 80% is the
fp32 that surrounds it: RMSNorm, the attention dot products, SwiGLU, the
softmax, the RoPE rotation, and -- the largest single contributor -- the
three soft-float operations at the end of every output row of every
matmul, `xout[i] = (float)acc * (w->s[i] * xs)`. There are 3,512 such rows
per token at dim 64, which is 10,500 float operations spent scaling the
results of 259,328 integer ones.

**This is why the port is slow, and it is not fixable piecemeal.** Scaling
a matmul row cheaply requires its consumer to take fixed point, which
requires RMSNorm and SwiGLU to produce it, which requires the attention
path to carry it. The whole activation path has to become integer at once
or none of it can.

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
places there) took row activations to **0.157 per MAC and the matmul to
18.1 cycles per MAC**, 2.0x. It is 236 bytes of internal RAM.

The remaining gap to the ~12 cycles the instruction timings allow is the
two byte loads: `ld.ub` walking a buffer measured 7.08 cycles each, against
9.44 for a word load that carries four times as much. Loading four weights
per `ld.w` and unpacking them with shifts is the next thing to try.

## Format

`tools/convert.py` writes int8 weights with **one fp32 scale per output
row**, not upstream's fixed groups of 64.

That is not a preference. `runq.c` walks groups with `for (j = 0; j <= n -
GS; j += GS)`, which assumes the row length is a multiple of the group
size. stories260K's `hidden_dim` is 172, so on the `w2` matmul that loop
covers 128 of 172 weights and silently drops the other 44. The model then
generates `Once upon upon upon upon` and nothing in the tool chain reports
a problem. A scale per row divides every shape exactly, and costs one
float multiply per output element instead of one per group. On this
checkpoint it is also no less accurate, because `dim` is 64 and the two
schemes then agree exactly.

The RoPE cos/sin table is in the weight file too: `run.c` calls `powf`,
`cosf` and `sinf` for every token, and there is no libm here -- mini-libc
has no `math.h` at all. The values depend only on the checkpoint's
`seq_len` and `head_size`.

## What else would fit

Per-token multiply-accumulates are roughly the parameter count, and at the
measured 18.1 cycles each on a 60 MHz part:

| model | dim / layers | int8 weights | MACs/token | forward |
| --- | --- | ---: | ---: | ---: |
| stories260K | 64 / 5 | 0.28 MB | 0.26 M | measured 1.21 s (80% soft float) |
| stories15M | 288 / 6 | 16 MB | 15.2 M | 4.6 s, integer only |
| stories42M | 512 / 8 | 45 MB | 43.7 M | card-bound: 45 MB a token at 0.8 MB/s |
| stories110M | 768 / 12 | 116 MB | 110 M | card-bound, minutes a token |

The 32 MB board holds stories15M; the 16 MB revisions do not. Above that
the weights do not fit and the card sets the rate.

**The vocabulary is the lever worth pulling.** stories15M spends 9.2M of
its 15.2M MACs a token in the 32000-entry classifier. llama2.c documents
training a custom tokenizer, and notes that a 4096-entry vocabulary trained
on TinyStories gives the same sequence lengths as the 32000-entry one. At
vocab 4096 a dim-288 model drops to 7.2M MACs and 7.6 MB, and a dim-128 one
to 2.1M MACs and 1.9 MB -- which, if the float were gone, would read at
about the speed a person does.

## Files

| | |
| --- | --- |
| `model.c` | the forward pass; `matmul`, `quantize`, `rmsnorm`, `softmax` |
| `tokenizer.c` | SentencePiece BPE, reading llama2.c's own `tokenizer.bin` |
| `runner.c` | the generation loop, greedy |
| `fmath.c` | `expf` and `rsqrtf`, because there is no libm |
| `llama.c` | Grifo entry, screen, and the internal-RAM arena |
| `host.c` | the same model and tokenizer behind malloc and stdio |
| `tools/convert.py` | fp32 checkpoint to int8, and the RoPE table |
| `tools/profile.py` | attributes a `wremu -F` profile using the link map |
| `make-card.py` | a FAT32 emulator card, built in memory and written once |
