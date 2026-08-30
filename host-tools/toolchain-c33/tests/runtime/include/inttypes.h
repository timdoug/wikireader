/* <inttypes.h> for the C33 DejaGnu board only.  */

#ifndef C33_TEST_INTTYPES_H
#define C33_TEST_INTTYPES_H

#include <stdint.h>

/* Exact-width and least-width types use char, short, long and long long.  */
#define __C33_PRI8   "hh"
#define __C33_PRI16  "h"
#define __C33_PRI32  "l"
#define __C33_PRI64  "ll"

/* Fast 8/16/32-bit types and pointer-sized integers are plain int.  */
#define __C33_PRIFAST  ""
#define __C33_PRIPTR   ""
#define __C33_PRIMAX   "ll"

#define PRId8       __C33_PRI8 "d"
#define PRIi8       __C33_PRI8 "i"
#define PRIo8       __C33_PRI8 "o"
#define PRIu8       __C33_PRI8 "u"
#define PRIx8       __C33_PRI8 "x"
#define PRIX8       __C33_PRI8 "X"
#define PRId16      __C33_PRI16 "d"
#define PRIi16      __C33_PRI16 "i"
#define PRIo16      __C33_PRI16 "o"
#define PRIu16      __C33_PRI16 "u"
#define PRIx16      __C33_PRI16 "x"
#define PRIX16      __C33_PRI16 "X"
#define PRId32      __C33_PRI32 "d"
#define PRIi32      __C33_PRI32 "i"
#define PRIo32      __C33_PRI32 "o"
#define PRIu32      __C33_PRI32 "u"
#define PRIx32      __C33_PRI32 "x"
#define PRIX32      __C33_PRI32 "X"
#define PRId64      __C33_PRI64 "d"
#define PRIi64      __C33_PRI64 "i"
#define PRIo64      __C33_PRI64 "o"
#define PRIu64      __C33_PRI64 "u"
#define PRIx64      __C33_PRI64 "x"
#define PRIX64      __C33_PRI64 "X"

#define PRIdLEAST8  PRId8
#define PRIiLEAST8  PRIi8
#define PRIoLEAST8  PRIo8
#define PRIuLEAST8  PRIu8
#define PRIxLEAST8  PRIx8
#define PRIXLEAST8  PRIX8
#define PRIdLEAST16 PRId16
#define PRIiLEAST16 PRIi16
#define PRIoLEAST16 PRIo16
#define PRIuLEAST16 PRIu16
#define PRIxLEAST16 PRIx16
#define PRIXLEAST16 PRIX16
#define PRIdLEAST32 PRId32
#define PRIiLEAST32 PRIi32
#define PRIoLEAST32 PRIo32
#define PRIuLEAST32 PRIu32
#define PRIxLEAST32 PRIx32
#define PRIXLEAST32 PRIX32
#define PRIdLEAST64 PRId64
#define PRIiLEAST64 PRIi64
#define PRIoLEAST64 PRIo64
#define PRIuLEAST64 PRIu64
#define PRIxLEAST64 PRIx64
#define PRIXLEAST64 PRIX64

#define PRIdFAST8   __C33_PRIFAST "d"
#define PRIiFAST8   __C33_PRIFAST "i"
#define PRIoFAST8   __C33_PRIFAST "o"
#define PRIuFAST8   __C33_PRIFAST "u"
#define PRIxFAST8   __C33_PRIFAST "x"
#define PRIXFAST8   __C33_PRIFAST "X"
#define PRIdFAST16  PRIdFAST8
#define PRIiFAST16  PRIiFAST8
#define PRIoFAST16  PRIoFAST8
#define PRIuFAST16  PRIuFAST8
#define PRIxFAST16  PRIxFAST8
#define PRIXFAST16  PRIXFAST8
#define PRIdFAST32  PRIdFAST8
#define PRIiFAST32  PRIiFAST8
#define PRIoFAST32  PRIoFAST8
#define PRIuFAST32  PRIuFAST8
#define PRIxFAST32  PRIxFAST8
#define PRIXFAST32  PRIXFAST8
#define PRIdFAST64  PRId64
#define PRIiFAST64  PRIi64
#define PRIoFAST64  PRIo64
#define PRIuFAST64  PRIu64
#define PRIxFAST64  PRIx64
#define PRIXFAST64  PRIX64

#define PRIdMAX     __C33_PRIMAX "d"
#define PRIiMAX     __C33_PRIMAX "i"
#define PRIoMAX     __C33_PRIMAX "o"
#define PRIuMAX     __C33_PRIMAX "u"
#define PRIxMAX     __C33_PRIMAX "x"
#define PRIXMAX     __C33_PRIMAX "X"
#define PRIdPTR     __C33_PRIPTR "d"
#define PRIiPTR     __C33_PRIPTR "i"
#define PRIoPTR     __C33_PRIPTR "o"
#define PRIuPTR     __C33_PRIPTR "u"
#define PRIxPTR     __C33_PRIPTR "x"
#define PRIXPTR     __C33_PRIPTR "X"

#define SCNd8       __C33_PRI8 "d"
#define SCNi8       __C33_PRI8 "i"
#define SCNo8       __C33_PRI8 "o"
#define SCNu8       __C33_PRI8 "u"
#define SCNx8       __C33_PRI8 "x"
#define SCNd16      __C33_PRI16 "d"
#define SCNi16      __C33_PRI16 "i"
#define SCNo16      __C33_PRI16 "o"
#define SCNu16      __C33_PRI16 "u"
#define SCNx16      __C33_PRI16 "x"
#define SCNd32      __C33_PRI32 "d"
#define SCNi32      __C33_PRI32 "i"
#define SCNo32      __C33_PRI32 "o"
#define SCNu32      __C33_PRI32 "u"
#define SCNx32      __C33_PRI32 "x"
#define SCNd64      __C33_PRI64 "d"
#define SCNi64      __C33_PRI64 "i"
#define SCNo64      __C33_PRI64 "o"
#define SCNu64      __C33_PRI64 "u"
#define SCNx64      __C33_PRI64 "x"

#define SCNdLEAST8  SCNd8
#define SCNiLEAST8  SCNi8
#define SCNoLEAST8  SCNo8
#define SCNuLEAST8  SCNu8
#define SCNxLEAST8  SCNx8
#define SCNdLEAST16 SCNd16
#define SCNiLEAST16 SCNi16
#define SCNoLEAST16 SCNo16
#define SCNuLEAST16 SCNu16
#define SCNxLEAST16 SCNx16
#define SCNdLEAST32 SCNd32
#define SCNiLEAST32 SCNi32
#define SCNoLEAST32 SCNo32
#define SCNuLEAST32 SCNu32
#define SCNxLEAST32 SCNx32
#define SCNdLEAST64 SCNd64
#define SCNiLEAST64 SCNi64
#define SCNoLEAST64 SCNo64
#define SCNuLEAST64 SCNu64
#define SCNxLEAST64 SCNx64

#define SCNdFAST8   __C33_PRIFAST "d"
#define SCNiFAST8   __C33_PRIFAST "i"
#define SCNoFAST8   __C33_PRIFAST "o"
#define SCNuFAST8   __C33_PRIFAST "u"
#define SCNxFAST8   __C33_PRIFAST "x"
#define SCNdFAST16  SCNdFAST8
#define SCNiFAST16  SCNiFAST8
#define SCNoFAST16  SCNoFAST8
#define SCNuFAST16  SCNuFAST8
#define SCNxFAST16  SCNxFAST8
#define SCNdFAST32  SCNdFAST8
#define SCNiFAST32  SCNiFAST8
#define SCNoFAST32  SCNoFAST8
#define SCNuFAST32  SCNuFAST8
#define SCNxFAST32  SCNxFAST8
#define SCNdFAST64  SCNd64
#define SCNiFAST64  SCNi64
#define SCNoFAST64  SCNo64
#define SCNuFAST64  SCNu64
#define SCNxFAST64  SCNx64

#define SCNdMAX     __C33_PRIMAX "d"
#define SCNiMAX     __C33_PRIMAX "i"
#define SCNoMAX     __C33_PRIMAX "o"
#define SCNuMAX     __C33_PRIMAX "u"
#define SCNxMAX     __C33_PRIMAX "x"
#define SCNdPTR     __C33_PRIPTR "d"
#define SCNiPTR     __C33_PRIPTR "i"
#define SCNoPTR     __C33_PRIPTR "o"
#define SCNuPTR     __C33_PRIPTR "u"
#define SCNxPTR     __C33_PRIPTR "x"

#endif
