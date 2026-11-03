#name: C33 FDPIC dynamic executable relocates its TLS image in ld.so
#source: fdpic-tls-image.s
#ld: -pie
#readelf: -r
# ld.so copies the image for the first thread before the start code
# applies .rofixup, so the TLS word is a dynamic relocation; .data's is not.

Relocation section '.rela.dyn' at offset 0x[0-9a-f]+ contains 1 entry:
 Offset +Info +Type .*
[0-9a-f]+ +00000022 R_C33_RELATIVE +[0-9a-f]+
