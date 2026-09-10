/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "zim_sparrow.h"
#include "zim_file.h"
#include "../sparrow/sparrow.h"
#include "../sparrow/html.h"
#include <stdbool.h>
#include "history.h"
#include <stdio.h>
#include <string.h>

static ZIM_FILE data;
static SPARROW box;
static SPARROW_RESULT result;
static unsigned sequence;
static struct { uint32_t id; char query[SPARROW_QUERY_MAX]; } questions[32];
static int attempted, ready;
extern HISTORY history_list[MAX_HISTORY];
extern int history_count;

/* History's existing title field holds the complete question, not the answer
 * heading. Runtime IDs are rebuilt on boot; only the text has durable meaning. */
static const char *history_question(const HISTORY *h)
{
    const char *title = (const char *)h->title;
    if (!ZIM_SPARROW_IS_PAGE(h->idx_article) ||
        !memchr(title, 0, sizeof(h->title)) || strncmp(title, "ask ", 4) ||
        !title[4] || strlen(title + 4) >= SPARROW_QUERY_MAX) return NULL;
    return title + 4;
}
static const char *remember(uint32_t id, const char *query)
{
    unsigned slot;
    char copy[SPARROW_QUERY_MAX];
    memcpy(copy, query, strlen(query) + 1);
    for (slot = 0; slot < 31 && questions[slot].id != id; ++slot) {}
    memmove(questions + 1, questions, slot * sizeof(questions[0]));
    questions[0].id = id;
    memcpy(questions[0].query, copy, strlen(copy) + 1);
    return questions[0].query;
}
void zim_sparrow_history_loaded(void)
{
    int i, kept = 0;
    sequence = 0;
    memset(questions, 0, sizeof(questions));
    for (i = 0; i < history_count; ++i) {
        HISTORY h = history_list[i];
        if (ZIM_SPARROW_IS_PAGE(h.idx_article)) {
            if (!history_question(&h)) continue;
            h.idx_article = ((uint32_t)h.idx_article & ~0x07ffffffu) |
                (ZIM_SPARROW_BASE + ++sequence);
        }
        history_list[kept++] = h;
    }
    memset(history_list + kept, 0, sizeof(HISTORY) * (MAX_HISTORY - kept));
    history_count = kept;
}

static int read_at(void *opaque, uint64_t off, void *p, size_t n)
{ return zim_file_read_at(opaque, off, p, n); }
static int open_box(void)
{
    if (attempted) return ready;
    attempted = 1;
    if (zim_file_open(&data, "1:/sparrow.dat") &&
        zim_file_open(&data, "0:/sparrow.dat")) return 0;
    ready = sparrow_open(&box, read_at, &data, data.size) == SPARROW_OK;
    if (!ready) zim_file_close(&data);
    return ready;
}
uint32_t zim_sparrow_save(const char *query)
{
    uint32_t id;
    int i;
    if (!query || !*query || strlen(query) >= SPARROW_QUERY_MAX) return 0;
    for (i = 0; i < history_count; ++i) {
        const char *saved = history_question(&history_list[i]);
        if (saved && !strcmp(saved, query))
            return (uint32_t)history_list[i].idx_article & 0x07ffffffu;
    }
    for (i = 0; i < 32; ++i)
        if (questions[i].id && !strcmp(questions[i].query, query)) return questions[i].id;
    if (sequence >= 4095) return 0;
    id = ZIM_SPARROW_BASE + ++sequence;
    remember(id, query);
    return id;
}
const char *zim_sparrow_question(uint32_t id)
{
    int i;
    id &= 0x07ffffffu;
    for (i = 0; i < history_count; ++i)
        if (((uint32_t)history_list[i].idx_article & 0x07ffffffu) == id) {
            const char *q = history_question(&history_list[i]);
            if (q) return remember(id, q);
        }
    for (i = 0; i < 32; ++i)
        if (questions[i].id == id) return remember(id, questions[i].query);
    return NULL;
}
int zim_sparrow_html(uint32_t id, unsigned char *buffer, size_t capacity, size_t *size)
{
    const char *query = zim_sparrow_question(id);
    if (!query || !open_box()) {
        int n = snprintf((char *)buffer, capacity, "<html><body><main><h1>%s</h1><p>%s</p></main></body></html>",
            !query ? "Answer expired" : "Question data unavailable",
            !query ? "Enter the question again with ask." :
            "Install sparrow.dat on the card and restart. Ordinary article search is available.");
        if (n < 0 || (size_t)n >= capacity) return -1;
        *size = (size_t)n;
        return 0;
    }
    sparrow_query(&box, query, &result);
    return sparrow_html(&box, &result, query, (char *)buffer, capacity, size) == SPARROW_OK ? 0 : -1;
}
int zim_sparrow_title(const unsigned char *path, size_t length, char *title, size_t capacity)
{
    uint32_t id = 0;
    unsigned i;
    SPARROW_ENTITY e;
    if (length != 17 || memcmp(path, "@sparrow/", 9) || !open_box()) return -1;
    for (i = 9; i < 17; ++i) {
        unsigned c = path[i];
        if (c >= '0' && c <= '9') c -= '0';
        else if (c >= 'a' && c <= 'f') c = c - 'a' + 10;
        else return -1;
        id = (id << 4) | c;
    }
    box.reads = box.bytes = 0; box.error = 0;
    if (sparrow_entity(&box, id, &e) || !e.title[0] || strlen(e.title) >= capacity) return -1;
    strcpy(title, e.title);
    return 0;
}
