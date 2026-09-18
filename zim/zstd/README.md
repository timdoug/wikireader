# Zstandard decoder

`zstddeclib.c` began as the official decoder-only single-file amalgamation
from Zstandard 1.5.7, generated with upstream's
`build/single_file_libs/create_single_file_decoder.sh`. **It is no longer
that file.** It carries about 1,100 added lines under 52 `__c33__` guards,
and it is not a candidate for a drop-in re-generation: regenerating it
discards all of the following.

- The sequence loop is gone from here entirely; `zstd_c33_seq.s` has it.
- One Huffman decoder, the single-symbol one, its four streams decoded in
  turn rather than interleaved, running as an IVRAM overlay.
- FSE and Huffman tables built in a packed form the decoder loads as two
  words with one `ld.w` pair, and placed by SDRAM row behaviour.
- Decoder state in A0 RAM, the sequence loop on a DSTRAM stack, input and
  output buffers put in separate SDRAM banks.

Current ZIM archives store clusters in Zstandard format. On C33 this decoder
contributes about 71 KiB of text and negligible static data; working memory is
allocated only while an article cluster is decoded.

Upstream: <https://github.com/facebook/zstd/tree/v1.5.7>, which is still
upstream's newest release. The only entry above it on the `dev` branch is
v1.6.0 (Dec 2025), whose two lines both concern disabling legacy format
support; no decoder work has landed since 1.5.7. The decompression speed-ups
in zstd's history were x86-64 and ARM assembly and are already in 1.5.7.

License: BSD-3-Clause or GPL-2.0-only, at the user's option. This directory
includes the upstream BSD license in `LICENSE`.
