/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "sparrow.h"
#include <stdio.h>
#include <string.h>
#include "crc32.h"

static uint32_t crc32(const unsigned char *p, size_t n)
{
    uint32_t c = 0xffffffffu;
    while (n--) c = crc_table[(c ^ *p++) & 255] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

static uint16_t u16(const unsigned char *p)
{ return (uint16_t)(p[0] | (uint16_t)p[1] << 8); }
static uint32_t u32(const unsigned char *p)
{ return (uint32_t)u16(p) | (uint32_t)u16(p + 2) << 16; }
static uint64_t u64(const unsigned char *p)
{ return (uint64_t)u32(p) | (uint64_t)u32(p + 4) << 32; }

static int read_at(SPARROW *w, uint64_t off, void *p, size_t n)
{
    if (w->error) return w->error;
    if (off > w->size || n > w->size - off)
        return w->error = SPARROW_BAD_FORMAT;
    if (w->reads >= SPARROW_READ_LIMIT || n > SPARROW_BYTE_LIMIT - w->bytes)
        return w->error = SPARROW_LIMIT;
    ++w->reads;
    w->bytes += (uint32_t)n;
    if (w->read(w->opaque, off, p, n)) return w->error = SPARROW_IO_ERROR;
    return SPARROW_OK;
}

/* ASCII case folding only; keep UTF-8 bytes intact, identically to builder.
 * Do not strip stopwords, accents, negation, or possessive suffixes. */
int sparrow_normalize(const char *s, char *out, size_t cap)
{
    size_t n = 0;
    int space = 0;
    unsigned char c;
    if (!cap) return SPARROW_LIMIT;
    while ((c = (unsigned char)*s++)) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '_') {
            space = n != 0;
            continue;
        }
        if (space) {
            if (n + 1 >= cap) return SPARROW_LIMIT;
            out[n++] = ' ';
            space = 0;
        }
        if (n + 1 >= cap) return SPARROW_LIMIT;
        out[n++] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
    }
    out[n] = 0;
    return SPARROW_OK;
}

static uint32_t hash(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
    /* Mix the low bits used for the power-of-two directory. */
    h ^= h >> 16; h *= 0x7feb352du;
    h ^= h >> 15; h *= 0x846ca68bu;
    return h ^ (h >> 16);
}

int sparrow_open(SPARROW *w, sparrow_read_fn read, void *opaque, uint64_t size)
{
    unsigned char h[128];
    memset(w, 0, sizeof(*w));
    w->read = read; w->opaque = opaque; w->size = size;
    if (!read || read_at(w, 0, h, sizeof(h))) return SPARROW_BAD_FORMAT;
    if (memcmp(h, "SPRWQA1\0", 8) || u32(h + 8) != 1 ||
        u32(h + 12) != 128 || u64(h + 16) != size || crc32(h, 80) != u32(h + 80))
        return SPARROW_BAD_FORMAT;
    w->bucket_count = u32(h + 24); w->entity_count = u32(h + 28);
    w->buckets_at = u64(h + 32); w->entities_at = u64(h + 40);
    if (!w->bucket_count || (w->bucket_count & (w->bucket_count - 1)) ||
        w->buckets_at != 512 ||
        w->entities_at < w->buckets_at + (uint64_t)w->bucket_count * 16 ||
        w->entities_at > size ||
        (uint64_t)w->entity_count * 16 > size - w->entities_at ||
        !memchr(h + 48, 0, 32)) return SPARROW_BAD_FORMAT;
    memcpy(w->snapshot, h + 48, 32);
    return SPARROW_OK;
}

static int indexed(SPARROW *w, uint64_t table, uint32_t id,
                   unsigned char *buffer, size_t cap, size_t *len)
{
    unsigned char rec[16];
    uint64_t off, data_start = w->entities_at + (uint64_t)w->entity_count * 16;
    if (read_at(w, table + (uint64_t)id * 16, rec, 16)) return w->error;
    off = u64(rec); *len = u32(rec + 8);
    if (*len > cap || off < data_start || (off & 511))
        return w->error = SPARROW_BAD_FORMAT;
    if (read_at(w, off, buffer, *len)) return w->error;
    if (crc32(buffer, *len) != u32(rec + 12)) return w->error = SPARROW_BAD_FORMAT;
    return SPARROW_OK;
}

int sparrow_resolve(SPARROW *w, const char *name, uint32_t *id)
{
    char key[SPARROW_QUERY_MAX];
    size_t n, pos = 0, size;
    uint32_t found = SPARROW_MISSING;
    int matches = 0, rc;
    if (sparrow_normalize(name, key, sizeof(key))) return SPARROW_LIMIT;
    if (!*key) return SPARROW_NO_ANSWER;
    rc = indexed(w, w->buckets_at, hash(key) & (w->bucket_count - 1),
                 w->bucket, sizeof(w->bucket), &size);
    if (rc) return rc;
    n = strlen(key);
    while (pos < size) {
        uint32_t candidate;
        unsigned len;
        if (size - pos < 6) return SPARROW_BAD_FORMAT;
        candidate = u32(w->bucket + pos); len = u16(w->bucket + pos + 4);
        pos += 6;
        if (len > size - pos || (candidate != SPARROW_MISSING && candidate >= w->entity_count))
            return SPARROW_BAD_FORMAT;
        if (len == n && !memcmp(w->bucket + pos, key, n)) {
            if (candidate == SPARROW_MISSING) return SPARROW_AMBIGUOUS;
            if (matches && candidate != found) return SPARROW_AMBIGUOUS;
            found = candidate; matches = 1;
        }
        pos += len;
    }
    if (!matches) return SPARROW_NO_ANSWER;
    *id = found;
    return SPARROW_OK;
}

static int string_at(const unsigned char *p, size_t size, size_t *pos,
                     size_t len, char *out)
{
    if (len >= SPARROW_TEXT_MAX || *pos > size || len > size - *pos ||
        memchr(p + *pos, 0, len)) return SPARROW_BAD_FORMAT;
    memcpy(out, p + *pos, len); out[len] = 0; *pos += len;
    return SPARROW_OK;
}

static int entity_block(SPARROW *w, uint32_t id, SPARROW_ENTITY *e,
                        size_t *pos, size_t *size, unsigned *count)
{
    int rc;
    if (id >= w->entity_count) return SPARROW_BAD_FORMAT;
    rc = indexed(w, w->entities_at, id, w->block, sizeof(w->block), size);
    if (rc) return rc;
    if (*size < 12) return SPARROW_BAD_FORMAT;
    e->id = id; e->qid = u32(w->block); *count = u16(w->block + 8);
    *pos = 12;
    if (string_at(w->block, *size, pos, u16(w->block + 4), e->label) ||
        string_at(w->block, *size, pos, u16(w->block + 6), e->title))
        return SPARROW_BAD_FORMAT;
    return SPARROW_OK;
}

int sparrow_entity(SPARROW *w, uint32_t id, SPARROW_ENTITY *e)
{
    size_t pos, size;
    unsigned count;
    return entity_block(w, id, e, &pos, &size, &count);
}

static int claim_at(SPARROW *w, size_t *pos, size_t size, SPARROW_CLAIM *c)
{
    const unsigned char *p = w->block + *pos;
    unsigned vl, ul, sl;
    if (*pos > size || size - *pos < 36) return SPARROW_BAD_FORMAT;
    memset(c, 0, sizeof(*c));
    c->property = u16(p); c->kind = p[2]; c->rank = p[3];
    c->object = u32(p + 4); c->date = (int32_t)u32(p + 8);
    c->start = (int32_t)u32(p + 12); c->end = (int32_t)u32(p + 16);
    c->asof = (int32_t)u32(p + 20); c->flags = u16(p + 24);
    c->object_qid = u32(p + 32);
    vl = u16(p + 26); ul = u16(p + 28); sl = u16(p + 30);
    if (c->kind < SPARROW_ITEM || c->kind > SPARROW_STRING || c->rank > 1 ||
        (c->flags & ~63u) ||
        (c->object != SPARROW_MISSING && c->object >= w->entity_count))
        return SPARROW_BAD_FORMAT;
    *pos += 36;
    if (string_at(w->block, size, pos, vl, c->value) ||
        string_at(w->block, size, pos, ul, c->unit) ||
        string_at(w->block, size, pos, sl, c->source)) return SPARROW_BAD_FORMAT;
    return SPARROW_OK;
}

static int facts(SPARROW *w, uint32_t id, unsigned prop, int32_t date, SPARROW_HOP *hop)
{
    size_t pos, size;
    unsigned count, i, j, best = 0;
    int rc, unsafe = 0, overflow = 0;
    SPARROW_CLAIM c;
    memset(hop, 0, sizeof(*hop)); hop->property = (uint16_t)prop;
    rc = entity_block(w, id, &hop->subject, &pos, &size, &count);
    if (rc) return rc;
    for (i = 0; i < count; ++i) {
        if ((rc = claim_at(w, &pos, size, &c))) return rc;
        if (c.property != prop) continue;
        /* Historical office joins use all nondeprecated terms. Start/end
         * are inclusive: transition days with two holders are ambiguous. */
        if (date) {
            /* Exclude definitely disjoint imprecise intervals. A term that
             * might overlap still needs exact endpoints before we answer. */
            int32_t first = c.start, last = c.end;
            if (first && !(first % 10000)) first += 101;
            else if (first && !(first % 100)) first += 1;
            if (last && !(last % 10000)) last += 1231;
            else if (last && !(last % 100)) last += 31; /* conservative upper bound */
            if ((first && date < first) || (last && date > last)) continue;
            if (!c.start || !c.end || !(c.start % 100) || !(c.end % 100) ||
                (c.flags & SPARROW_UNSAFE)) {
                unsafe = 1; continue;
            }
            if (date < c.start || date > c.end) continue;
        } else {
            if (c.rank < best) continue;
            if (c.rank > best) {
                best = c.rank; hop->count = 0; unsafe = overflow = 0;
            }
        }
        if (c.flags & (SPARROW_UNSAFE | SPARROW_UNRESOLVED_OBJECT)) unsafe = 1;
        /* Repeated source statements do not create extra distinct answers.
         * Keep differently qualified or dated values separate. */
        for (j = 0; j < hop->count; ++j) {
            SPARROW_CLAIM *old = &hop->claims[j];
            if (old->kind == c.kind && old->object == c.object && old->date == c.date &&
                old->start == c.start && old->end == c.end && old->asof == c.asof &&
                old->flags == c.flags && !strcmp(old->value, c.value) && !strcmp(old->unit, c.unit)) break;
        }
        if (j < hop->count) continue;
        if (hop->count == SPARROW_MAX_ANSWERS) { overflow = 1; continue; }
        hop->claims[hop->count++] = c;
    }
    if (pos != size) return SPARROW_BAD_FORMAT;
    if (unsafe) return SPARROW_UNSUPPORTED;
    if (overflow) return SPARROW_LIMIT;
    if (!hop->count) return SPARROW_NO_ANSWER;
    return date && hop->count != 1 ? SPARROW_AMBIGUOUS : SPARROW_OK;
}

typedef struct { const char *before, *after; uint16_t first, second; } RULE;
static const RULE rules[] = {
    {"what is the capital of ", "", 36, 0}, {"capital of ", "", 36, 0},
    {"", " capital", 36, 0},
    {"where was ", " born", 19, 0}, {"", " birthplace", 19, 0},
    {"birthplace of ", "", 19, 0},
    {"where is the birthplace of ", "", 19, 0},
    {"give me the birth place of ", "", 19, 0},
    {"where did ", " die", 20, 0}, {"death place of ", "", 20, 0},
    {"when was ", " born", 569, 0}, {"", " birth date", 569, 0},
    {"when did ", " die", 570, 0}, {"", " death date", 570, 0},
    {"when was the death of ", "", 570, 0},
    {"what did ", " die from", 509, 0}, {"cause of death of ", "", 509, 0},
    {"how tall is ", "", 2048, 0}, {"height of ", "", 2048, 0},
    {"", " height", 2048, 0},
    {"how high is ", "", 2048, 0},
    {"who is the spouse of ", "", 26, 0}, {"spouse of ", "", 26, 0},
    {"", " spouse", 26, 0}, {"", " wife", 26, 0}, {"", " husband", 26, 0},
    {"who has ", " been married to", 26, 0},
    {"who was married to ", "", 26, 0},
    {"who is the father of ", "", 22, 0}, {"father of ", "", 22, 0},
    {"who is the mother of ", "", 25, 0}, {"mother of ", "", 25, 0},
    {"currency of ", "", 38, 0}, {"", " currency", 38, 0},
    {"what is the currency of ", "", 38, 0}, {"give me the currency of ", "", 38, 0},
    {"official language of ", "", 37, 0}, {"", " official language", 37, 0},
    {"what is the official language of ", "", 37, 0},
    {"what are the official languages of ", "", 37, 0},
    {"population of ", "", 1082, 0}, {"", " population", 1082, 0},
    {"what is the population of ", "", 1082, 0},
    {"what is the total population of ", "", 1082, 0},
    {"how many people live in ", "", 1082, 0},
    {"how many inhabitants does ", " have", 1082, 0},
    {"countries that border ", "", 47, 0}, {"", " borders", 47, 0},
    {"country of ", "", 17, 0}, {"", " country", 17, 0},
    {"in which country is ", "", 17, 0},
    {"in which country is ", " located", 17, 0},
    {"what country is ", " in", 17, 0},
    {"continent of ", "", 30, 0}, {"", " continent", 30, 0},
    {"when was ", " founded", 571, 0},
    {"when were ", " founded", 571, 0},
    {"when was ", " built", 571, 0},
    {"when did ", " dissolve", 576, 0},
    {"when was ", " published", 577, 0},
    {"who wrote ", "", 50, 0}, {"who wrote the book ", "", 50, 0},
    {"who is the author of ", "", 50, 0}, {"author of ", "", 50, 0},
    {"who directed ", "", 57, 0}, {"director of ", "", 57, 0},
    {"who composed ", "", 86, 0}, {"composer of ", "", 86, 0},
    {"who composed the music for ", "", 86, 0},
    {"who discovered ", "", 61, 0},
    {"who developed ", "", 178, 0}, {"who developed the video game ", "", 178, 0},
    {"who created ", "", 170, 0}, {"who created the comic ", "", 170, 0},
    {"who founded ", "", 112, 0}, {"who is the founder of ", "", 112, 0},
    {"who designed ", "", 287, 0},
    {"who is the architect of ", "", 84, 0},
    {"where did ", " study", 69, 0},
    {"who was the doctoral supervisor of ", "", 184, 0},
    {"what kind of music did ", " play", 136, 0},
    {"where did the architect of ", " study", 84, 69},
    {"which country does the creator of ", " come from", 170, 27},
    {"when did ", " happen", 585, 0},
    {"what language do they speak where ", " is", 17, 37},
    {"what currency do they use where ", " is", 17, 38},
    {"capital of the country ", " is in", 17, 36},
    {"what is the capital of the country ", " is in", 17, 36},
    {"who is the mayor of ", "", 6, 0}, {"who is the governor of ", "", 6, 0},
    {"head of government of ", "", 6, 0}, {"head of state of ", "", 35, 0},
    {"who is the owner of ", "", 127, 0}, {"who owns ", "", 127, 0},
    {"owner of ", "", 127, 0}, {"who is the editor of ", "", 98, 0},
    {"what is the birth name of ", "", 1477, 0}, {"birth name of ", "", 1477, 0},
    {"where is ", " buried", 119, 0}, {"where was ", " buried", 119, 0},
    {"burial place of ", "", 119, 0},
    {"where does ", " live", 551, 0}, {"residence of ", "", 551, 0},
    {"what is the alma mater of ", "", 69, 0},
    {"which university did ", " attend", 69, 0},
    {"which awards did ", " win", 166, 0}, {"awards of ", "", 166, 0},
    {"which instruments does ", " play", 1303, 0}, {"instruments played by ", "", 1303, 0},
    {"which programming languages influenced ", "", 737, 0},
    {"who was ", " inspired by", 737, 0}, {"influences on ", "", 737, 0},
    {"in which programming language is ", " written", 277, 0},
    {"programming language of ", "", 277, 0},
    {"who are the developers of ", "", 178, 0},
    {"who was the wife of ", "", 26, 0}, {"who was the husband of ", "", 26, 0},
    {"which river does ", " cross", 177, 0}, {"crossed by ", "", 177, 0},
    {"in which time zone is ", "", 421, 0}, {"what is the timezone in ", "", 421, 0},
    {"time zone of ", "", 421, 0}, {"timezone of ", "", 421, 0},
    {"what form of government is found in ", "", 122, 0},
    {"form of government of ", "", 122, 0},
    {"how many employees does ", " have", 1128, 0}, {"number of employees of ", "", 1128, 0},
    {"children of ", "", 40, 0}, {"list the children of ", "", 40, 0},
    {"how much is the elevation of ", "", 2044, 0}, {"elevation of ", "", 2044, 0},
    {"how large is the area of ", "", 2046, 0}, {"how big is the total area of ", "", 2046, 0},
    {"area of ", "", 2046, 0}, {"give me the runtime of ", "", 2047, 0},
    {"runtime of ", "", 2047, 0}, {"duration of ", "", 2047, 0},
    {"what is the net income of ", "", 2295, 0}, {"net income of ", "", 2295, 0},
    {"how much is the population of ", "", 1082, 0},
    {"how much is the total population of ", "", 1082, 0},
    {"when was the founding date of ", "", 571, 0},
    {"who composed the soundtrack for ", "", 86, 0}
};

static int match(const char *s, const char *before, const char *after, char *entity)
{
    size_t n = strlen(s), a = strlen(before), b = strlen(after);
    if (n <= a + b || strncmp(s, before, a) || strcmp(s + n - b, after)) return 0;
    memcpy(entity, s + a, n - a - b); entity[n - a - b] = 0;
    return 1;
}

static int pair(const char *text, const char *separator, char *left, char *right)
{
    const char *at = strstr(text, separator);
    size_t n = at ? (size_t)(at - text) : 0, skip = strlen(separator);
    if (!n || !at[skip] || strstr(at + skip, separator)) return 0;
    memcpy(left, text, n); left[n] = 0;
    strcpy(right, at + skip);
    return 1;
}

static int plan_query(const char *q, SPARROW_PLAN *plan)
{
    char name[SPARROW_QUERY_MAX];
    unsigned i, matches = 0;
    size_t specificity = 0;
    RULE rule = {0, 0, 0, 0};
    const char *when;
    static const struct { const char *before, *after; uint16_t prop; unsigned mode; } aggregates[] = {
        {"how many children does ", " have", 40, SPARROW_COUNT},
        {"how many children did ", " have", 40, SPARROW_COUNT},
        {"how many official languages does ", " have", 37, SPARROW_COUNT},
        {"how many borders does ", " have", 47, SPARROW_COUNT},
        {"how many awards did ", " win", 166, SPARROW_COUNT},
        {"did ", " have children", 40, SPARROW_EXISTS},
        {"does ", " have children", 40, SPARROW_EXISTS},
        {"was ", " married", 26, SPARROW_EXISTS},
        {"has ", " been married", 26, SPARROW_EXISTS},
        {"in which year was ", " born", 569, SPARROW_YEAR},
        {"in what year was ", " born", 569, SPARROW_YEAR},
        {"what year was ", " born", 569, SPARROW_YEAR},
        {"in which year did ", " die", 570, SPARROW_YEAR},
        {"in which year was ", " founded", 571, SPARROW_YEAR},
        {"grandchildren of ", "", 40, SPARROW_FAMILY},
        {"give me the grandchildren of ", "", 40, SPARROW_FAMILY},
        {"who are the grandchildren of ", "", 40, SPARROW_FAMILY},
        {"list the grandchildren of ", "", 40, SPARROW_FAMILY}
    };
    static const struct { const char *before, *middle; uint16_t prop; unsigned reverse; } memberships[] = {
        {"is ", " the father of ", 22, 1}, {"is ", " the mother of ", 25, 1},
        {"is ", " a child of ", 40, 1}, {"is ", " the parent of ", 40, 0},
        {"was ", " married to ", 26, 0}, {"did ", " study at ", 69, 0},
        {"was ", " born in ", 19, 0}
    };
    for (i = 0; i < sizeof(aggregates) / sizeof(aggregates[0]); ++i) {
        if (!match(q, aggregates[i].before, aggregates[i].after, name)) continue;
        strcpy(plan->entity, name); plan->properties[0] = aggregates[i].prop;
        plan->mode = aggregates[i].mode; plan->count = plan->mode == SPARROW_FAMILY ? 2 : 1;
        if (plan->count == 2) plan->properties[1] = 40;
        return SPARROW_OK;
    }
    if (match(q, "do ", " have the same parents", name)) {
        if (!pair(name, " and ", plan->entity, plan->office)) return SPARROW_UNSUPPORTED;
        plan->properties[0] = 22; plan->properties[1] = 25;
        plan->count = 2; plan->mode = SPARROW_SAME_PARENTS;
        return SPARROW_OK;
    }
    for (i = 0; i < sizeof(memberships) / sizeof(memberships[0]); ++i) {
        char left[SPARROW_QUERY_MAX], right[SPARROW_QUERY_MAX];
        if (!match(q, memberships[i].before, "", name) ||
            !pair(name, memberships[i].middle, left, right)) continue;
        strcpy(plan->entity, memberships[i].reverse ? right : left);
        strcpy(plan->office, memberships[i].reverse ? left : right);
        plan->mode = SPARROW_MEMBERSHIP; plan->count = 1;
        plan->properties[0] = memberships[i].prop;
        return SPARROW_OK;
    }
    if (!strncmp(q, "who was ", 8) && (when = strstr(q + 8, " when "))) {
        size_t n = (size_t)(when - q - 8);
        memcpy(plan->office, q + 8, n); plan->office[n] = 0;
        strcpy(plan->entity, when + 6);
        plan->properties[0] = 585; plan->properties[1] = SPARROW_OFFICE;
        plan->count = 2; plan->mode = SPARROW_TEMPORAL;
        return SPARROW_OK;
    }
    if (match(q, "how old was ", " when he died", name) ||
        match(q, "how old was ", " when she died", name) ||
        match(q, "how old was ", " at death", name)) {
        strcpy(plan->entity, name);
        plan->properties[0] = 569; plan->properties[1] = 570;
        plan->count = 2; plan->mode = SPARROW_AGE;
        return SPARROW_OK;
    }
    /* Match the complete query before touching storage. Multiple syntactic
     * interpretations abstain; a substring cannot swallow unknown words. */
    for (i = 0; i < sizeof(rules) / sizeof(rules[0]); ++i) {
        if (match(q, rules[i].before, rules[i].after, name)) {
            size_t score = strlen(rules[i].before) + strlen(rules[i].after);
            if (score < specificity) continue;
            if (score == specificity && matches) return SPARROW_AMBIGUOUS;
            specificity = score;
            matches = 1;
            rule = rules[i];
        }
    }
    if (!matches) return SPARROW_UNSUPPORTED;
    match(q, rule.before, rule.after, name);
    plan->count = rule.second ? 2 : 1;
    plan->properties[0] = rule.first; plan->properties[1] = rule.second;
    /* Compose explicit relation noun phrases, preserving their order and
     * refusing paths longer than the device's three-hop bound. */
    for (;;) {
        static const struct { const char *text; uint16_t prop; } relations[] = {
            {"capital of ", 36}, {"country of ", 17}, {"birthplace of ", 19},
            {"spouse of ", 26}, {"father of ", 22}, {"mother of ", 25},
            {"author of ", 50}, {"creator of ", 170}, {"architect of ", 84}
        };
        const char *s = !strncmp(name, "the ", 4) ? name + 4 : name;
        for (i = 0; i < sizeof(relations) / sizeof(relations[0]); ++i) {
            size_t n = strlen(relations[i].text);
            if (strncmp(s, relations[i].text, n) || !s[n]) continue;
            if (plan->count == SPARROW_MAX_HOPS) return SPARROW_LIMIT;
            memmove(plan->properties + 1, plan->properties, plan->count * sizeof(plan->properties[0]));
            plan->properties[0] = relations[i].prop; ++plan->count;
            memmove(name, s + n, strlen(s + n) + 1);
            break;
        }
        if (i == sizeof(relations) / sizeof(relations[0])) break;
    }
    strcpy(plan->entity, name);
    return SPARROW_OK;
}

int sparrow_plan(const char *query, SPARROW_PLAN *plan)
{
    char q[SPARROW_QUERY_MAX];
    size_t len;
    int rc;
    memset(plan, 0, sizeof(*plan));
    if ((rc = sparrow_normalize(query, q, sizeof(q)))) return rc;
    len = strlen(q);
    if (len && (q[len - 1] == '?' || q[len - 1] == '.' || q[len - 1] == '!')) q[--len] = 0;
    while (len && q[len - 1] == ' ') q[--len] = 0;
    return plan_query(q, plan);
}

static int resolve_entity(SPARROW *w, const char *name, uint32_t *id)
{
    int rc = sparrow_resolve(w, name, id);
    /* Keep an exact title, including its article, when it exists. Never
     * resolve ambiguity by deleting words or ranking candidates. */
    if (rc == SPARROW_NO_ANSWER && !strncmp(name, "the ", 4))
        rc = sparrow_resolve(w, name + 4, id);
    return rc;
}

/* Count a complete, bounded set of item values. Evidence keeps its first
 * eight distinct claims, while the count is computed from every claim. */
static int item_set(SPARROW *w, uint32_t id, unsigned prop, SPARROW_HOP *hop,
                    unsigned *total)
{
    size_t pos, start, size;
    unsigned count, i, j, best = 0;
    SPARROW_CLAIM c;
    int rc;
    memset(hop, 0, sizeof(*hop)); hop->property = (uint16_t)prop;
    if ((rc = entity_block(w, id, &hop->subject, &pos, &size, &count))) return rc;
    start = pos; *total = 0;
    for (i = 0; i < count; ++i) {
        if ((rc = claim_at(w, &pos, size, &c))) return rc;
        if (c.property == prop && c.rank > best) best = c.rank;
    }
    if (pos != size) return SPARROW_BAD_FORMAT;
    pos = start;
    for (i = 0; i < count; ++i) {
        if ((rc = claim_at(w, &pos, size, &c))) return rc;
        if (c.property != prop || c.rank != best) continue;
        if ((c.flags & SPARROW_UNSAFE) || c.kind != SPARROW_ITEM || !c.object_qid)
            return SPARROW_UNSUPPORTED;
        for (j = 0; j < *total && w->set[j] != c.object_qid; ++j) {}
        if (j < *total) continue;
        if (*total == SPARROW_SET_MAX) return SPARROW_LIMIT;
        w->set[(*total)++] = c.object_qid;
        if (hop->count < SPARROW_MAX_ANSWERS) hop->claims[hop->count++] = c;
    }
    return *total ? SPARROW_OK : SPARROW_NO_ANSWER;
}

static void boolean_result(SPARROW_RESULT *r, int yes, const char *answer)
{
    r->scalar_kind = SPARROW_STRING;
    strcpy(r->scalar, yes ? "true" : "false");
    snprintf(r->answer, sizeof(r->answer), "%s", answer);
}

static int aggregates(SPARROW *w, const SPARROW_PLAN *plan, uint32_t id, SPARROW_RESULT *r)
{
    uint32_t other = 0;
    SPARROW_ENTITY other_entity;
    unsigned n, i, j;
    int rc;
    if (plan->mode == SPARROW_MEMBERSHIP || plan->mode == SPARROW_SAME_PARENTS) {
        r->stage = "entity";
        if ((rc = resolve_entity(w, plan->office, &other))) return rc;
        if (plan->mode == SPARROW_MEMBERSHIP && (rc = sparrow_entity(w, other, &other_entity))) return rc;
    }
    r->stage = "claims";
    if (plan->mode == SPARROW_SAME_PARENTS) {
        int same = 1;
        for (i = 0; i < 2; ++i) {
            SPARROW_HOP *a = &r->hops[i * 2], *b = a + 1;
            if ((rc = facts(w, id, plan->properties[i], 0, a)) ||
                (rc = facts(w, other, plan->properties[i], 0, b))) return rc;
            r->hop_count += 2;
            if (a->count != 1 || b->count != 1 || a->claims[0].kind != SPARROW_ITEM ||
                b->claims[0].kind != SPARROW_ITEM || a->claims[0].flags || b->claims[0].flags)
                return SPARROW_UNSUPPORTED;
            if (a->claims[0].object != b->claims[0].object) same = 0;
        }
        boolean_result(r, same, same ? "Yes: same recorded parents" : "No: recorded parents differ");
    } else if (plan->mode == SPARROW_YEAR) {
        if ((rc = facts(w, id, plan->properties[0], 0, &r->hops[0]))) return rc;
        r->hop_count = 1;
        if (r->hops[0].count != 1 || r->hops[0].claims[0].kind != SPARROW_TIME ||
            !r->hops[0].claims[0].date) return SPARROW_UNSUPPORTED;
        r->scalar_kind = SPARROW_QUANTITY;
        snprintf(r->scalar, sizeof(r->scalar), "%ld", (long)(r->hops[0].claims[0].date / 10000));
        snprintf(r->answer, sizeof(r->answer), "%s", r->scalar);
    } else {
        if ((rc = item_set(w, id, plan->properties[0], &r->hops[0], &n))) return rc;
        r->hop_count = 1; r->evidence_limited = n > SPARROW_MAX_ANSWERS;
        if (plan->mode == SPARROW_COUNT) {
            r->scalar_kind = SPARROW_QUANTITY;
            snprintf(r->scalar, sizeof(r->scalar), "%u", n);
            snprintf(r->answer, sizeof(r->answer), "%u recorded %s", n, sparrow_property(plan->properties[0]));
        } else if (plan->mode == SPARROW_EXISTS) {
            boolean_result(r, 1, "Yes: recorded in this dataset");
            r->hops[0].count = 1; r->evidence_limited = 0;
        } else if (plan->mode == SPARROW_MEMBERSHIP) {
            for (i = 0; i < n && w->set[i] != other_entity.qid; ++i) {}
            if (i == n) return SPARROW_NO_ANSWER; /* absence is not a negative fact */
            if (i >= SPARROW_MAX_ANSWERS) {
                size_t pos, size;
                unsigned records;
                SPARROW_ENTITY entity;
                SPARROW_CLAIM c;
                if ((rc = entity_block(w, id, &entity, &pos, &size, &records))) return rc;
                for (j = 0; j < records; ++j) {
                    if ((rc = claim_at(w, &pos, size, &c))) return rc;
                    if (c.property == plan->properties[0] && c.object == other &&
                        c.rank == r->hops[0].claims[0].rank) {
                        r->hops[0].claims[0] = c; break;
                    }
                }
            } else r->hops[0].claims[0] = r->hops[0].claims[i];
            r->hops[0].count = 1; r->evidence_limited = 0;
            boolean_result(r, 1, "Yes: recorded in this dataset");
        } else if (plan->mode == SPARROW_FAMILY) {
            uint32_t children[SPARROW_MAX_ANSWERS];
            if (n > SPARROW_MAX_ANSWERS) return SPARROW_LIMIT;
            for (i = 0; i < n; ++i) {
                if (r->hops[0].claims[i].flags || r->hops[0].claims[i].object == SPARROW_MISSING)
                    return SPARROW_UNSUPPORTED;
                children[i] = r->hops[0].claims[i].object;
            }
            for (i = 0; i < n; ++i) {
                unsigned found;
                SPARROW_HOP *hop = &r->hops[r->hop_count];
                rc = item_set(w, children[i], 40, hop, &found);
                if (rc == SPARROW_NO_ANSWER) continue;
                if (rc) return rc;
                ++r->hop_count;
                if (found > SPARROW_MAX_ANSWERS) return SPARROW_LIMIT;
                for (j = 0; j < found; ++j) {
                    unsigned k;
                    if (hop->claims[j].flags & SPARROW_UNRESOLVED_OBJECT) return SPARROW_UNSUPPORTED;
                    for (k = 0; k < r->value_count && r->values[k].object_qid != w->set[j]; ++k) {}
                    if (k < r->value_count) continue;
                    if (r->value_count == SPARROW_MAX_ANSWERS) return SPARROW_LIMIT;
                    r->values[r->value_count++] = hop->claims[j];
                }
            }
            if (!r->value_count) return SPARROW_NO_ANSWER;
        }
    }
    r->stage = "answered";
    return SPARROW_OK;
}

static int execute(SPARROW *w, const SPARROW_PLAN *plan, SPARROW_RESULT *r)
{
    uint32_t id;
    unsigned i;
    int rc;
    r->stage = "entity";
    if ((rc = resolve_entity(w, plan->entity, &id))) return rc;
    if (plan->mode >= SPARROW_COUNT) return aggregates(w, plan, id, r);
    for (i = 0; i < plan->count; ++i) {
        int32_t date = 0;
        if (i && plan->mode == SPARROW_TEMPORAL) {
            SPARROW_HOP *event = &r->hops[0];
            if (event->count != 1 || !event->claims[0].date) return SPARROW_UNSUPPORTED;
            date = event->claims[0].date;
            r->stage = "entity";
            if ((rc = resolve_entity(w, plan->office, &id))) return rc;
        } else if (i && plan->mode != SPARROW_AGE) {
            SPARROW_HOP *bridge = &r->hops[i - 1];
            SPARROW_CLAIM *c = &bridge->claims[0];
            r->stage = "bridge";
            if (bridge->count != 1 || c->kind != SPARROW_ITEM ||
                c->object == SPARROW_MISSING || c->flags) return SPARROW_AMBIGUOUS;
            id = c->object;
        }
        r->stage = "claims";
        if ((rc = facts(w, id, plan->properties[i], date, &r->hops[i]))) return rc;
        r->hop_count = i + 1;
    }
    if (plan->mode == SPARROW_AGE) {
        int32_t birth, death, years;
        r->stage = "date arithmetic";
        if (r->hops[0].count != 1 || r->hops[1].count != 1) return SPARROW_AMBIGUOUS;
        birth = r->hops[0].claims[0].date; death = r->hops[1].claims[0].date;
        if (!birth || death < birth) return SPARROW_UNSUPPORTED;
        years = death / 10000 - birth / 10000 - (death % 10000 < birth % 10000);
        snprintf(r->answer, sizeof(r->answer), "%ld years", (long)years);
        r->scalar_kind = SPARROW_QUANTITY;
        snprintf(r->scalar, sizeof(r->scalar), "%ld", (long)years);
        strcpy(r->scalar_unit, "years");
    }
    r->stage = "answered";
    return SPARROW_OK;
}

int sparrow_query(SPARROW *w, const char *query, SPARROW_RESULT *r)
{
    SPARROW_PLAN plan;
    memset(r, 0, sizeof(*r));
    w->reads = w->bytes = 0; w->error = 0;
    r->stage = "wording";
    r->status = sparrow_plan(query, &plan);
    if (!r->status) r->status = execute(w, &plan, r);
    if (r->status) {
        r->hop_count = 0;
        r->value_count = r->scalar_kind = r->evidence_limited = 0;
        snprintf(r->answer, sizeof(r->answer), "%s", sparrow_status(r->status));
    } else if (!r->scalar_kind && !r->value_count && r->hop_count) {
        SPARROW_HOP *h = &r->hops[r->hop_count - 1];
        r->value_count = h->count;
        memcpy(r->values, h->claims, h->count * sizeof(h->claims[0]));
    }
    if (!r->status && !*r->answer) {
        if (r->value_count == 1)
            snprintf(r->answer, sizeof(r->answer), "%s", r->values[0].value);
        else snprintf(r->answer, sizeof(r->answer), "%u recorded values", r->value_count);
    }
    r->reads = w->reads; r->bytes = w->bytes;
    return r->status;
}

const char *sparrow_status(int s)
{
    static const char *const names[] = {"Answer", "No answer", "Ambiguous",
        "No answer for this question", "Data read error", "Invalid data", "Query limit reached"};
    return s >= 0 && s <= SPARROW_LIMIT ? names[s] : "Invalid data";
}

const char *sparrow_property(uint16_t p)
{
    switch (p) {
    case 6: return "head of government"; case 35: return "head of state";
    case 40: return "children"; case 98: return "editor";
    case 106: return "occupation"; case 119: return "burial place";
    case 122: return "form of government"; case 127: return "owner";
    case 159: return "headquarters"; case 166: return "awards";
    case 177: return "crosses"; case 277: return "programming language";
    case 421: return "time zone"; case 551: return "residence";
    case 737: return "influenced by"; case 1128: return "employees";
    case 1303: return "instruments"; case 1477: return "birth name";
    case 2044: return "elevation"; case 2046: return "area";
    case 2047: return "duration"; case 2142: return "box office";
    case 2295: return "net income";
    case 17: return "country"; case 19: return "birthplace";
    case 20: return "death place"; case 22: return "father";
    case 25: return "mother"; case 27: return "citizenship";
    case 26: return "spouse"; case 30: return "continent";
    case 36: return "capital"; case 37: return "official language";
    case 38: return "currency"; case 47: return "borders";
    case 50: return "author"; case 57: return "director";
    case 61: return "discoverer"; case 69: return "educated at";
    case 84: return "architect"; case 86: return "composer";
    case 112: return "founder"; case 136: return "genre";
    case 170: return "creator"; case 178: return "developer";
    case 184: return "doctoral supervisor"; case 287: return "designer";
    case 509: return "cause of death";
    case 569: return "born"; case 570: return "died";
    case 571: return "founded"; case 585: return "date";
    case 576: return "dissolved"; case 577: return "published";
    case 1082: return "population"; case 2048: return "height";
    case SPARROW_OFFICE: return "officeholder";
    default: return "property";
    }
}
