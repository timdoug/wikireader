# Zstandard decoder

`zstddeclib.c` is the official decoder-only single-file amalgamation from
Zstandard 1.5.7. It was generated with upstream's
`build/single_file_libs/create_single_file_decoder.sh`; no WikiReader-specific
changes have been made to the generated source.

Current ZIM archives store clusters in Zstandard format. On C33 this decoder
contributes about 71 KiB of text and negligible static data; working memory is
allocated only while an article cluster is decoded.

Upstream: <https://github.com/facebook/zstd/tree/v1.5.7>

License: BSD-3-Clause or GPL-2.0-only, at the user's option. This directory
includes the upstream BSD license in `LICENSE`.
