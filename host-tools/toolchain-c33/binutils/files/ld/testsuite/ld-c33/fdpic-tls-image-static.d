#name: C33 FDPIC static executable lists its TLS image in .rofixup
#source: fdpic-tls-image.s
#ld:
#readelf: -r -x .rofixup
# Both words and __dp.

There are no relocations in this file.

Hex dump of section '.rofixup':
  0x[0-9a-f]+ [0-9a-f]{8} [0-9a-f]{8} [0-9a-f]{8} +.*
