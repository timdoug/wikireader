/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "sparrow.h"
#include "html.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SPARROW box;
static SPARROW_RESULT result;
static int read_at(void *opaque, uint64_t off, void *p, size_t n)
{
    FILE *f = opaque;
    return fseeko(f, (off_t)off, SEEK_SET) || fread(p, 1, n, f) != n;
}

static void show_date(const char *prefix, int32_t date)
{
    if (!date) return;
    printf(" %s%04ld", prefix, (long)(date / 10000));
    if (date / 100 % 100) printf("-%02ld", (long)(date / 100 % 100));
    if (date % 100) printf("-%02ld", (long)(date % 100));
}

static void json_string(const char *s)
{
    putchar('"');
    while (*s) {
        unsigned c = (unsigned char)*s++;
        if (c == '"' || c == 92) { putchar(92); putchar((int)c); }
        else if (c < 32) printf("\\u%04x", c);
        else putchar((int)c);
    }
    putchar('"');
}

static void json_result(void)
{
    unsigned i;
    printf("{\"status\":%d,\"reads\":%lu,\"bytes\":%lu,\"answer\":", result.status,
           (unsigned long)result.reads, (unsigned long)result.bytes);
    json_string(result.answer);
    printf(",\"snapshot\":"); json_string(box.snapshot);
    printf(",\"stage\":"); json_string(result.stage ? result.stage : "unknown");
    printf(",\"values\":[");
    if (!result.status && result.scalar_kind) {
        printf("{\"kind\":%u,\"qid\":0,\"date\":0,\"value\":", result.scalar_kind);
        json_string(result.scalar); printf(",\"unit\":"); json_string(result.scalar_unit); putchar('}');
    } else if (!result.status) {
        for (i = 0; i < result.value_count; ++i) {
            SPARROW_CLAIM *c = &result.values[i];
            if (i) putchar(',');
            printf("{\"kind\":%u,\"qid\":%lu,\"date\":%ld,\"value\":", c->kind,
                   (unsigned long)c->object_qid, (long)c->date);
            json_string(c->value);
            printf(",\"unit\":"); json_string(c->unit); putchar('}');
        }
    }
    puts("]}");
}

int main(int argc, char **argv)
{
    FILE *f;
    off_t size;
    int rc, mode = 0;
    unsigned h, i;
    if (argc == 3 && !strcmp(argv[1], "--plan")) {
        SPARROW_PLAN plan;
        rc = sparrow_plan(argv[2], &plan);
        printf("{\"status\":%d,\"entity\":", rc); json_string(plan.entity);
        printf(",\"office\":"); json_string(plan.office);
        printf(",\"mode\":%u,\"properties\":[", plan.mode);
        for (i = 0; i < plan.count; ++i) printf("%s%u", i ? "," : "", plan.properties[i]);
        puts("]}"); return rc ? 1 : 0;
    }
    if (argc == 4 && (!strcmp(argv[3], "--json") || !strcmp(argv[3], "--html"))) {
        mode = !strcmp(argv[3], "--json") ? 1 : 2; --argc;
    }
    if (argc != 3) { fprintf(stderr, "usage: %s sparrow.dat 'question' [--json|--html]\n", argv[0]); return 2; }
    f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    if (fseeko(f, 0, SEEK_END) || (size = ftello(f)) < 0) { fclose(f); return 2; }
    rc = sparrow_open(&box, read_at, f, (uint64_t)size);
    if (rc) { fprintf(stderr, "%s\n", sparrow_status(rc)); fclose(f); return 2; }
    rc = sparrow_query(&box, argv[2], &result);
    if (mode == 1) { json_result(); fclose(f); return rc ? 1 : 0; }
    if (mode == 2) {
        static char html[32768];
        size_t n;
        int err = sparrow_html(&box, &result, argv[2], html, sizeof(html), &n);
        if (!err) fwrite(html, 1, n, stdout);
        fclose(f); return err ? 2 : rc ? 1 : 0;
    }
    printf("%s\n", result.answer);
    for (h = 0; h < result.hop_count; ++h) {
        SPARROW_HOP *hop = &result.hops[h];
        for (i = 0; i < hop->count; ++i) {
            SPARROW_CLAIM *c = &hop->claims[i];
            printf("%s -> %s -> %s%s%s", hop->subject.label,
                   sparrow_property(hop->property), c->value, *c->unit ? " " : "", c->unit);
            show_date("from ", c->start); show_date("through ", c->end); show_date("as of ", c->asof);
            printf(" [Wikidata %s]\n", *c->source ? c->source : "statement ID unavailable");
        }
    }
    printf("snapshot=%s reads=%lu bytes=%lu status=%d\n", box.snapshot,
           (unsigned long)result.reads, (unsigned long)result.bytes, rc);
    fclose(f);
    return rc ? 1 : 0;
}
