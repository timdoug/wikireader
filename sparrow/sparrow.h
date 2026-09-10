/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SPARROW_H
#define SPARROW_H
#include <stdint.h>
#include <stddef.h>

#define SPARROW_QUERY_MAX 128
#define SPARROW_TEXT_MAX 256
#define SPARROW_MAX_ANSWERS 8
#define SPARROW_MAX_HOPS 3
#define SPARROW_EVIDENCE_HOPS 9
#define SPARROW_SET_MAX 256
#define SPARROW_BLOCK_MAX 65536
#define SPARROW_BUCKET_MAX 4096
#define SPARROW_MISSING 0xffffffffu
#define SPARROW_OFFICE 65534
#define SPARROW_READ_LIMIT 64
#define SPARROW_BYTE_LIMIT (250u * 1024u)

enum { SPARROW_OK, SPARROW_NO_ANSWER, SPARROW_AMBIGUOUS, SPARROW_UNSUPPORTED, SPARROW_IO_ERROR,
       SPARROW_BAD_FORMAT, SPARROW_LIMIT };
enum { SPARROW_ITEM = 1, SPARROW_TIME, SPARROW_QUANTITY, SPARROW_STRING };
enum { SPARROW_UNSAFE = 1, SPARROW_START = 2, SPARROW_END = 4, SPARROW_ASOF = 8,
       SPARROW_PARTIAL_TIME = 16, SPARROW_UNRESOLVED_OBJECT = 32 };
typedef int (*sparrow_read_fn)(void *, uint64_t, void *, size_t);
typedef struct {
    sparrow_read_fn read;
    void *opaque;
    uint64_t size, buckets_at, entities_at;
    uint32_t bucket_count, entity_count, reads, bytes;
    int error;
    char snapshot[32];
    unsigned char bucket[SPARROW_BUCKET_MAX];
    unsigned char block[SPARROW_BLOCK_MAX];
    uint32_t set[SPARROW_SET_MAX];
} SPARROW;
typedef struct {
    uint32_t id, qid;
    char label[SPARROW_TEXT_MAX], title[SPARROW_TEXT_MAX];
} SPARROW_ENTITY;
typedef struct {
    uint16_t property, flags;
    uint8_t kind, rank;
    uint32_t object, object_qid;
    int32_t date, start, end, asof;
    char value[SPARROW_TEXT_MAX], unit[SPARROW_TEXT_MAX], source[SPARROW_TEXT_MAX];
} SPARROW_CLAIM;
typedef struct {
    SPARROW_ENTITY subject;
    uint16_t property;
    SPARROW_CLAIM claims[SPARROW_MAX_ANSWERS];
    unsigned count;
} SPARROW_HOP;
typedef struct {
    int status;
    char answer[SPARROW_TEXT_MAX];
    SPARROW_HOP hops[SPARROW_EVIDENCE_HOPS];
    unsigned hop_count;
    SPARROW_CLAIM values[SPARROW_MAX_ANSWERS];
    unsigned value_count, evidence_limited;
    unsigned scalar_kind;
    char scalar[32], scalar_unit[16];
    uint32_t reads, bytes;
    const char *stage;
} SPARROW_RESULT;

enum { SPARROW_LOOKUP, SPARROW_AGE, SPARROW_TEMPORAL, SPARROW_COUNT,
       SPARROW_EXISTS, SPARROW_MEMBERSHIP, SPARROW_FAMILY, SPARROW_YEAR,
       SPARROW_SAME_PARENTS };
typedef struct {
    char entity[SPARROW_QUERY_MAX], office[SPARROW_QUERY_MAX];
    uint16_t properties[SPARROW_MAX_HOPS];
    unsigned count, mode;
} SPARROW_PLAN;

int sparrow_open(SPARROW *, sparrow_read_fn, void *, uint64_t);
int sparrow_normalize(const char *, char *, size_t);
int sparrow_resolve(SPARROW *, const char *, uint32_t *);
int sparrow_entity(SPARROW *, uint32_t, SPARROW_ENTITY *);
int sparrow_query(SPARROW *, const char *, SPARROW_RESULT *);
int sparrow_plan(const char *, SPARROW_PLAN *);
const char *sparrow_status(int);
const char *sparrow_property(uint16_t);
#endif
