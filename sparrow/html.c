/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "html.h"
#include <stdio.h>
#include <string.h>

typedef struct { char *p; size_t n, cap; int error; } OUT;
static void raw(OUT *o, const char *s)
{
    size_t n = strlen(s);
    if (o->error) return;
    if (n >= o->cap - o->n) { o->error = SPARROW_LIMIT; return; }
    memcpy(o->p + o->n, s, n); o->n += n; o->p[o->n] = 0;
}
static void escaped(OUT *o, const char *s)
{
    char ch[2] = {0, 0};
    while (*s) {
        switch (*s) {
        case '&': raw(o, "&amp;"); break;
        case '<': raw(o, "&lt;"); break;
        case '>': raw(o, "&gt;"); break;
        case '"': raw(o, "&quot;"); break;
        default: ch[0] = (unsigned char)*s < 32 ? ' ' : *s; raw(o, ch);
        }
        ++s;
    }
}
static void link(OUT *o, uint32_t id, const char *label)
{
    char tag[48];
    if (id != SPARROW_MISSING) {
        snprintf(tag, sizeof(tag), "<a href=\"@sparrow/%08lx\">", (unsigned long)id);
        raw(o, tag);
    }
    escaped(o, label);
    if (id != SPARROW_MISSING) raw(o, "</a>");
}
static void date(OUT *o, const char *prefix, int32_t d)
{
    char buf[64];
    if (!d) return;
    snprintf(buf, sizeof(buf), "%s%04ld", prefix, (long)(d / 10000));
    raw(o, buf);
    if (d / 100 % 100) {
        snprintf(buf, sizeof(buf), "-%02ld", (long)(d / 100 % 100)); raw(o, buf);
    }
    if (d % 100) {
        snprintf(buf, sizeof(buf), "-%02ld", (long)(d % 100)); raw(o, buf);
    }
}
int sparrow_html(const SPARROW *w, const SPARROW_RESULT *r, const char *query,
            char *buffer, size_t capacity, size_t *size)
{
    OUT o = {buffer, 0, capacity, 0};
    unsigned h, i;
    if (!buffer || !capacity || !size) return SPARROW_LIMIT;
    buffer[0] = 0;
    raw(&o, "<html><body><main><h1>"); escaped(&o, r->answer); raw(&o, "</h1><p>");
    escaped(&o, query); raw(&o, "</p>");
    if (r->evidence_limited)
        raw(&o, "<p>The total counts distinct recorded values. The first eight supporting claims are shown below.</p>");
    for (h = 0; h < r->hop_count; ++h) {
        const SPARROW_HOP *hop = &r->hops[h];
        raw(&o, "<p>"); link(&o, hop->subject.id, hop->subject.label);
        raw(&o, " &gt; "); escaped(&o, sparrow_property(hop->property)); raw(&o, "</p><ul>");
        for (i = 0; i < hop->count; ++i) {
            const SPARROW_CLAIM *c = &hop->claims[i];
            raw(&o, "<li>"); link(&o, c->object, c->value);
            if (*c->unit) { raw(&o, " "); escaped(&o, c->unit); }
            date(&o, "; from ", c->start); date(&o, "; through ", c->end);
            date(&o, "; as of ", c->asof);
            raw(&o, "</li>");
        }
        raw(&o, "</ul>");
    }
    raw(&o, "<p>Wikidata snapshot: "); escaped(&o, w->snapshot); raw(&o, "</p>");
    if (r->status) raw(&o, "<p>Try a specific fact, such as capital of Peru. Use ordinary search to find an article.</p>");
    else raw(&o, "<p>Recorded claims, not a guarantee of completeness. Tap an underlined name to read its article in the selected archive.</p>");
    for (h = 0; h < r->hop_count; ++h) {
        const SPARROW_HOP *hop = &r->hops[h];
        for (i = 0; i < hop->count; ++i) {
            raw(&o, "<p>Source: ");
            escaped(&o, hop->claims[i].source[0] ? hop->claims[i].source : "Wikidata (statement ID unavailable)");
            raw(&o, "</p>");
        }
    }
    raw(&o, "<p>Wikidata structured data: CC0.</p></main></body></html>");
    *size = o.n;
    return o.error;
}
