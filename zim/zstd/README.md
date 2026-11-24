# Zstandard decoder

`zstddeclib.c` began as the official decoder-only single-file amalgamation,
generated with upstream's `build/single_file_libs/create_single_file_decoder.sh`,
now from the v1.6.0 release (tag commit
`916e8e70b33317d469a41ee837e47317807815c8`, 2026-11-23). **It is no longer
that file.** It carries about 1,100 added lines under 49 `__c33__` guards,
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

Upstream: <https://github.com/facebook/zstd>. The literal scratch buffer can
be placed by the caller (`ZSTD_DCtx_setLiteralBuffer`), so each upstream
reference to `dctx->litExtraBuffer` is changed to `dctx->litExtraPtr` here,
including the one in v1.6.0's `ZSTD_zeroLiteralsBufferPadding`.

To move to a newer upstream, generate the amalgamation at the old and the new
revision and three-way merge this file onto the new one (`git merge-file`).
`ZSTD_decodeSequence` keeps the C33 version as its own branch beside
upstream's AArch64 and generic bodies.

License: BSD-3-Clause or GPL-2.0-only, at the user's option. This directory
includes the upstream BSD license in `LICENSE`.
