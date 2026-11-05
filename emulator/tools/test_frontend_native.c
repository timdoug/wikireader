/* Drive the native scheduler with declarative operation categories. No guest
 * instructions or firmware are executed by this differential-test driver. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/frontend.h"

struct program { enum frontend_kind code[512]; unsigned count, base; };

static bool peek(void *ctx, uint32_t pc, uint16_t *bits, enum frontend_kind *kind)
{
    struct program *p = ctx;
    unsigned index = (pc - p->base) / 2;
    *kind = pc >= p->base && index < p->count ? p->code[index] : FRONT_ALU;
    *bits = (uint16_t)(pc ^ 0xa33au);
    return true;
}

static const char *names[] = {"ext", "alu", "imm", "ldh", "ldw", "st", "branch", "loop"};

struct bounded_source { uint16_t words[16]; unsigned rejected; bool memory; };

static bool bounded_peek(void *ctx, uint32_t pc, uint16_t *bits, enum frontend_kind *kind)
{
    struct bounded_source *s = ctx;
    if (pc >= sizeof s->words) { s->rejected++; return false; }
    *bits = s->words[pc / 2];
    *kind = pc == 0 && s->memory ? FRONT_HALF : FRONT_ALU;
    return true;
}

static void lifecycle(void)
{
    struct frontend f;
    struct frontend_config config = {8, 2, 2, true, true};
    struct bounded_source source = {.words = {0x1111, 0x2222, 0x3333, 0x4444}};
    struct frontend_word word;
    assert(frontend_reset(&f, &config, bounded_peek, &source, 0, 0));
    assert(frontend_next(&f, 0, &word) && word.pc == 0 && word.bits == 0x1111);
    source.words[1] = 0xabcd;
    frontend_complete(&f, f.now + 4, false, 0);
    assert(frontend_next(&f, 8, &word) && word.pc == 2 && word.bits == 0x2222);
    frontend_complete(&f, f.now + 4, false, 0);
    frontend_flush(&f, 2, f.now + 4);
    assert(frontend_next(&f, f.now, &word) && word.pc == 2 && word.bits == 0xabcd);
    frontend_complete(&f, f.now + 4, true, 6);
    assert(frontend_next(&f, f.now + 4, &word) && word.pc == 6 && word.bits == 0x4444);

    source.memory = true;
    assert(frontend_reset(&f, &config, bounded_peek, &source, 0, 0));
    assert(frontend_next(&f, 0, &word) && word.kind == FRONT_HALF);
    uint64_t start = frontend_data(&f, f.now, 12, true);
    frontend_complete(&f, start + 12, false, 0);
    frontend_flush(&f, 6, start + 2); /* an exception cancels speculative work */
    assert(f.port_free == start + 12);
    assert(frontend_next(&f, f.now, &word) && word.pc == 6);
    assert(f.now >= start + 12 + config.data_to_fetch + 4);
    assert(frontend_reset(&f, &config, bounded_peek, &source, 32, 100));
    assert(!frontend_next(&f, 100, &word) && source.rejected == 1);
    source.memory = false;
    frontend_flush(&f, 0, f.now);
    assert(frontend_next(&f, f.now, &word) && word.pc == 0);
    assert(frontend_reset(&f, &config, bounded_peek, &source, 0, 0));
    assert(!f.raw_count && !f.semantic_count && !f.executing && f.port_free == 0);

    /* More than one memory phase can belong to a single instruction:
     * completing a read does not independently retire the E stage. */
    source.memory = true;
    assert(frontend_reset(&f, &config, bounded_peek, &source, 0, 0));
    assert(frontend_next(&f, 0, &word));
    start = frontend_data(&f, f.now, 6, true);
    frontend_advance(&f, start + 6);
    assert(f.executing);
    uint64_t second = frontend_data(&f, f.now, 4, true);
    assert(second == start + 6);
    frontend_complete(&f, second + 8, false, 0);
    assert(frontend_next(&f, second + 8, &word) && word.pc == 2 && f.now == second + 8);

    source.memory = false;
    assert(frontend_reset(&f, &config, bounded_peek, &source, 0, 0));
    assert(frontend_next(&f, 0, &word));
    uint64_t end = f.now + 40000000;
    frontend_complete(&f, end, false, 0);
    assert(frontend_next(&f, end, &word) && word.pc == 2 && f.now == end);
    puts("frontend lifecycle: reset, redirect, code/mapping flush and in-flight port checks pass");
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--self-test")) { lifecycle(); return 0; }
    if (argc != 9 && argc != 11) return 2;
    struct frontend_config config = {.halfwords = (unsigned)atoi(argv[1]),
        .fetch_to_data = argc == 11 ? (unsigned)atoi(argv[9]) : 2,
        .data_to_fetch = argc == 11 ? (unsigned)atoi(argv[10]) : 2,
        .trailing_ext = atoi(argv[4]), .prefix_gate = atoi(argv[5])};
    unsigned offset = (unsigned)atoi(argv[2]), shared = (unsigned)atoi(argv[3]);
    unsigned unroll = (unsigned)atoi(argv[6]), passes = (unsigned)atoi(argv[7]);
    struct program program = {.base = offset};
    if (!unroll || passes < 4 || passes > 100 || strlen(argv[8]) * unroll + 2 > 512) return 2;
    for (unsigned i = 0; i < unroll; i++)
        for (const char *p = argv[8]; *p; p++) {
            const char *tag = strchr("eaihwsb", *p);
            if (!tag) return 2;
            program.code[program.count++] = (enum frontend_kind)(tag - "eaihwsb");
        }
    program.code[program.count++] = FRONT_ALU;
    program.code[program.count++] = FRONT_LOOP;
    struct frontend f;
    if (!frontend_reset(&f, &config, peek, &program, offset, 0)) return 2;
    uint64_t now = 0, ends[100];
    unsigned loops = 0, expected_pc = offset, prefixes = 0, words = 0;
    bool first = true;
    printf("{\"events\":[");
    while (loops < passes) {
        struct frontend_word word;
        assert(frontend_next(&f, now, &word));
        assert(word.pc == expected_pc && word.bits == (uint16_t)(word.pc ^ 0xa33au));
        words++;
        expected_pc += 2;
        now = f.now;
        if (word.kind == FRONT_EXT) { prefixes++; continue; }
        assert(prefixes == word.prefixes);
        prefixes = 0;
        printf("%s{\"quarter\":%llu,\"loop\":%u,\"kind\":\"%s\",\"address\":%u,\"prefixes\":%u}",
               first ? "" : ",", (unsigned long long)now, loops, names[word.kind], word.pc, word.prefixes);
        first = false;
        bool memory = word.kind == FRONT_HALF || word.kind == FRONT_WORD || word.kind == FRONT_STORE;
        uint64_t end;
        if (memory) {
            unsigned service = word.kind == FRONT_STORE ? shared ? 4 : 16 :
                               shared ? word.kind == FRONT_HALF ? 6 : 8 : 8;
            uint64_t at = frontend_data(&f, now + (word.prefixes ? 4 : 0), service, shared);
            end = at + service;
        } else end = now + (word.kind >= FRONT_BRANCH ? 12 : 4);
        bool redirect = word.kind >= FRONT_BRANCH;
        unsigned target = word.kind == FRONT_LOOP ? offset : expected_pc;
        frontend_complete(&f, end, redirect, target);
        now = end;
        if (word.kind == FRONT_LOOP) {
            ends[loops++] = end;
            expected_pc = offset;
        }
    }
    assert(words == program.count * passes);
    double cost = (double)(ends[passes - 1] - ends[passes / 2 - 1]) / (passes - passes / 2) / 4;
    printf("],\"cycles_per_pass\":%.9f,\"words\":%u}\n", cost, words);
    return 0;
}
