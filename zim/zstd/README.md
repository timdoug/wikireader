# Zstandard decoder

`zstddeclib.c` began as the official decoder-only single-file amalgamation,
generated with upstream's `build/single_file_libs/create_single_file_decoder.sh`,
now from the `dev` branch at `49cf51799ea051457f57243527aa2058f999f005`
(2026-10-05). **It is no longer that file.** It carries about 1,100 added lines under 52 `__c33__` guards,
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

Upstream: <https://github.com/facebook/zstd/tree/dev>. The newest release is
still v1.5.7. The decoder changes on `dev` since then are AArch64, x86-64 and
RISC-V code, the BMI2 dispatch, and two fixes in dictionary code the reader
never reaches (the multiple-dictionary hash set and the custom calloc it
uses); article loads measure the same as with 1.5.7.

To move to a newer upstream, generate the amalgamation at the old and the new
revision and three-way merge this file onto the new one (`git merge-file`).
`ZSTD_decodeSequence` keeps the C33 version as its own branch beside
upstream's AArch64 and generic bodies.

License: BSD-3-Clause or GPL-2.0-only, at the user's option. This directory
includes the upstream BSD license in `LICENSE`.
