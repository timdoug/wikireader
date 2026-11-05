/* Opt-in prototype, not wired into the default C33 timing path yet. */
#include <assert.h>
#include <string.h>

#include "frontend.h"

bool frontend_reset(struct frontend *f, const struct frontend_config *config,
                    frontend_peek peek, void *ctx, uint32_t pc, uint64_t now)
{
    if (!peek || config->halfwords < 2 || config->halfwords > 8 || pc & 1)
        return false;
    memset(f, 0, sizeof *f);
    f->config = *config;
    f->peek = peek;
    f->ctx = ctx;
    f->decode_pc = pc;
    f->fetch_pc = pc & ~3u;
    f->now = f->port_free = now;
    f->decode_started = UINT64_MAX;
    return true;
}

static void settle(struct frontend *f)
{
    if (f->executing && f->execute_end != UINT64_MAX && f->execute_end <= f->now) {
        f->executing = false;
        f->execute_data = false;
        if (f->redirect) {
            f->raw_count = f->semantic_count = f->base_slots = 0;
            f->prefixes = 0;
            f->decoding = false;
            f->fetch_pc = f->target & ~3u;
            f->decode_pc = f->target;
            f->decode_started = UINT64_MAX;
            f->redirect = false;
        }
    }
    if (f->decoding && f->decode_end <= f->now) {
        f->decoding = false;
        for (unsigned i = 0; i < f->decoding_count; i++) {
            struct frontend_word word = f->decoding_words[i];
            word.ready = f->decode_end;
            word.prefixes = word.kind == FRONT_EXT ? 0 : f->decoding_prefixes;
            assert(f->semantic_count < 16);
            f->semantic[f->semantic_count++] = word;
        }
        if (f->decoding_kind == FRONT_EXT)
            f->prefixes++;
        else {
            f->prefixes = f->following;
            f->base_slots++;
        }
    }
}

static void offer_fetch(struct frontend *f)
{
    uint64_t ready = f->port_free + (f->owner == 2 ? f->config.data_to_fetch : 0);
    if (f->fetch_blocked || ready > f->now || f->raw_count + 2 > f->config.halfwords)
        return;
    struct frontend_word words[2];
    for (unsigned i = 0; i < 2; i++) {
        words[i] = (struct frontend_word){.pc = f->fetch_pc + 2 * i,
                                          .ready = f->now + 4};
        /* Peek must be a side-effect-free mapped instruction source.
         * Rejected regions stop prefetch, never invoke MMIO reads. */
        if (!f->peek(f->ctx, words[i].pc, &words[i].bits, &words[i].kind)) {
            f->fetch_blocked = true;
            return;
        }
    }
    for (unsigned i = 0; i < 2; i++)
        if (words[i].pc >= f->decode_pc)
            f->raw[f->raw_count++] = words[i];
    f->fetch_pc += 4;
    f->port_free = f->now + 4;
    f->owner = 1;
}

static void decode(struct frontend *f)
{
    bool blocked = f->executing && f->execute_data;
    if (f->now % 4 || f->decoding || f->base_slots || !f->raw_count ||
        f->semantic_count > 13 ||
        (f->decode_started != UINT64_MAX && f->now < f->decode_started + 4) ||
        (f->config.prefix_gate && blocked && f->raw[0].kind == FRONT_EXT))
        return;
    struct frontend_word first = f->raw[0];
    if (first.ready > f->now + 4)
        return;
    unsigned consume = 1, prefixes = f->prefixes, following = 0;
    enum frontend_kind kind = first.kind;
    uint32_t pc = first.pc;
    if (kind == FRONT_EXT && first.ready <= f->now && f->raw_count > 1 &&
        f->raw[1].kind != FRONT_EXT && f->raw[1].ready <= f->now + 4) {
        consume = 2;
        kind = f->raw[1].kind;
        pc = f->raw[1].pc;
        prefixes++;
    }
    if (!(f->config.prefix_gate && blocked) && f->config.trailing_ext &&
        (kind == FRONT_ALU || kind == FRONT_IMM) && !prefixes && consume == 1 &&
        f->raw_count > 1 && f->raw[1].kind == FRONT_EXT && f->raw[1].ready <= f->now) {
        consume = 2;
        following = 1;
    }
    memcpy(f->decoding_words, f->raw, consume * sizeof f->raw[0]);
    memmove(f->raw, f->raw + consume, (f->raw_count - consume) * sizeof f->raw[0]);
    f->raw_count -= consume;
    f->decoding = true;
    f->decode_end = f->now + 4;
    f->decode_started = f->now;
    f->decoding_count = consume;
    f->decoding_prefixes = prefixes;
    f->following = following;
    f->decoding_kind = kind;
    f->decode_pc = pc + 2 + 2 * following;
}

static void refill_decode(struct frontend *f, bool data_wants_port)
{
    if (!data_wants_port) offer_fetch(f);
    decode(f);
    if (!data_wants_port) offer_fetch(f);
}

static uint64_t next_tick(const struct frontend *f, uint64_t until)
{
    /* Once both storage levels are full there is no fetch/decode event
     * until E completes or the caller's wait ends. Do not spend a host
     * iteration on every quarter-clock of an external memory wait. */
    if (!f->decoding && f->base_slots &&
        f->raw_count + 2 > f->config.halfwords && until > f->now) {
        if (f->executing && f->execute_end > f->now && f->execute_end < until)
            return f->execute_end;
        return until;
    }
    return f->now + 1;
}

bool frontend_next(struct frontend *f, uint64_t now, struct frontend_word *word)
{
    for (unsigned steps = 0; steps < 2000000; steps++) {
        settle(f);
        if (f->now >= now && f->semantic_count &&
            (f->semantic[0].kind == FRONT_EXT || (!f->executing && !(f->now % 4)))) {
            *word = f->semantic[0];
            memmove(f->semantic, f->semantic + 1, --f->semantic_count * sizeof *word);
            if (word->kind != FRONT_EXT) {
                assert(f->base_slots);
                f->base_slots--;
                f->executing = true;
                f->execute_data = false; /* set before advancing in data() */
                f->execute_end = UINT64_MAX;
            }
            return true;
        }
        refill_decode(f, false);
        if (f->fetch_blocked && !f->raw_count && !f->semantic_count &&
            !f->decoding && !f->executing)
            return false;
        uint64_t until = now;
        if (f->executing && f->execute_end != UINT64_MAX && f->execute_end > until)
            until = f->execute_end;
        f->now = next_tick(f, until);
    }
    return false;
}

uint64_t frontend_data(struct frontend *f, uint64_t requested,
                       unsigned service, bool shared)
{
    assert(f->executing && service);
    f->execute_data = shared;
    for (unsigned steps = 0; steps < 2000000; steps++, f->now++) {
        settle(f);
        uint64_t ready = f->port_free + (f->owner == 1 ? f->config.fetch_to_data : 0);
        if (f->now >= requested && (!shared || f->now >= ready)) {
            if (shared) {
                f->port_free = f->now + service;
                f->owner = 2;
            }
            return f->now;
        }
        refill_decode(f, shared && f->now >= requested);
    }
    assert(!"frontend data request did not finish");
    return UINT64_MAX;
}

void frontend_advance(struct frontend *f, uint64_t until)
{
    while (f->now < until) {
        settle(f);
        refill_decode(f, false);
        f->now = next_tick(f, until);
    }
    settle(f);
}

void frontend_complete(struct frontend *f, uint64_t end, bool redirect, uint32_t target)
{
    assert(f->executing && end >= f->now);
    f->execute_end = end;
    f->redirect = redirect;
    f->target = target;
}

void frontend_flush(struct frontend *f, uint32_t pc, uint64_t now)
{
    assert(now >= f->now && !(pc & 1));
    uint64_t port_free = f->port_free;
    unsigned owner = f->owner;
    struct frontend_config config = f->config;
    frontend_peek peek = f->peek;
    void *ctx = f->ctx;
    bool valid = frontend_reset(f, &config, peek, ctx, pc, now);
    assert(valid);
    f->port_free = port_free;
    f->owner = owner;
}
