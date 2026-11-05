#ifndef C33_FRONTEND_H
#define C33_FRONTEND_H

#include <stdbool.h>
#include <stdint.h>

/* Experimental local-RAM fetch/decode scheduler. It schedules instruction
 * words; the C33 executor remains responsible for all architectural effects.
 * Time is in quarter-MCLK units, with F/E starts on whole MCLK edges.
 */
enum frontend_kind {
    FRONT_EXT, FRONT_ALU, FRONT_IMM, FRONT_HALF, FRONT_WORD, FRONT_STORE,
    FRONT_BRANCH, FRONT_LOOP, FRONT_OTHER
};

struct frontend_word {
    uint32_t pc;
    uint16_t bits;
    enum frontend_kind kind;
    uint64_t ready;
    unsigned prefixes;
};

typedef bool (*frontend_peek)(void *ctx, uint32_t pc,
                              uint16_t *bits, enum frontend_kind *kind);

struct frontend_config {
    unsigned halfwords;        /* raw FIFO capacity, 2..8 */
    unsigned fetch_to_data, data_to_fetch;
    bool trailing_ext, prefix_gate;
};

struct frontend {
    struct frontend_config config;
    frontend_peek peek;
    void *ctx;
    uint64_t now, port_free, decode_end, execute_end, decode_started;
    uint32_t fetch_pc, decode_pc, target;
    unsigned owner, raw_count, semantic_count, prefixes, base_slots;
    bool decoding, executing, execute_data, redirect, fetch_blocked;
    struct frontend_word raw[8], semantic[16], decoding_words[2];
    unsigned decoding_count, decoding_prefixes, following;
    enum frontend_kind decoding_kind;
};

bool frontend_reset(struct frontend *f, const struct frontend_config *config,
                    frontend_peek peek, void *ctx, uint32_t pc, uint64_t now);
/* Flush speculative state on a redirect, mapping/code change or exception;
 * unlike a cold reset, an already reserved RAM phase keeps its occupancy. */
void frontend_flush(struct frontend *f, uint32_t pc, uint64_t now);
/* Return the next word at or after @now. EXT has no E-stage dispatch;
 * its semantic event is still delivered in order to the architectural CPU.
 * Complete each non-EXT event before asking for another word. */
bool frontend_next(struct frontend *f, uint64_t now, struct frontend_word *word);
/* Reserve one data phase, returning its start; call after E dispatch and
 * before complete(). External data uses a separate port. */
uint64_t frontend_data(struct frontend *f, uint64_t requested,
                       unsigned service, bool shared);
/* Advance prefetch/decode through an execution/data wait without dispatching
 * another instruction. Multiple data phases may belong to the same E stage. */
void frontend_advance(struct frontend *f, uint64_t until);
/* Resolve E duration/control flow after the executor knows them. */
void frontend_complete(struct frontend *f, uint64_t end, bool redirect, uint32_t target);

#endif
