/* SPDX-License-Identifier: GPL-3.0-or-later
 * Exercise the production history and question store with an in-memory card.
 * Only the file syscalls and wiki-ID lookup are replaced. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <grifo.h>
#include "history.h"
#include "wikilib.h"
#include "lcd_buf_draw.h"
#include "zim_sparrow.h"

extern HISTORY history_list[MAX_HISTORY];
extern int history_count, viewing_count;
int nCurrentWiki;
static unsigned char disk[sizeof(HISTORY) * (MAX_HISTORY + 1)];
static size_t disk_size, position;
static int fail_open, fail_read, short_write;

int get_wiki_id_from_idx(unsigned int index) { (void)index; return 1; }
void memrcpy(char *dest, char *src, int n) { memmove(dest, src, (size_t)n); }
file_error_t file_open(const char *name, file_access_t access)
{
    (void)name; (void)access; position = 0;
    return fail_open ? FILE_ERROR_DENIED : 1;
}
file_error_t file_create(const char *name, file_access_t access)
{ return file_open(name, access); }
file_error_t file_close(int handle) { (void)handle; return FILE_ERROR_OK; }
ssize_t file_read(int handle, void *buffer, size_t n)
{
    (void)handle;
    if (fail_read) return FILE_ERROR_RW_ERROR;
    if (n > disk_size - position) n = disk_size - position;
    memcpy(buffer, disk + position, n); position += n;
    return (ssize_t)n;
}
ssize_t file_write(int handle, void *buffer, size_t n)
{
    (void)handle;
    if (short_write) n /= 2;
    assert(position + n <= sizeof(disk));
    memcpy(disk + position, buffer, n); position += n; disk_size = position;
    return (ssize_t)n;
}

static uint32_t visit(const char *query)
{
    uint32_t id = zim_sparrow_save(query);
    assert(id);
    history_add(id, (const unsigned char *)"Rendered answer heading", 0);
    return id;
}

int main(void)
{
    const char *query = "who was us president when the berlin wall fell";
    uint32_t id;
    unsigned i;
    char text[80];
    history_list_init();
    id = visit(query);
    assert(!strcmp((char *)history_list[0].title, "ask who was us president when the berlin wall fell"));
    history_log_y_pos(137);
    history_add(42, (const unsigned char *)"Ordinary article", 0);
    history_set_y_pos(ARTICLE_WIKI_BITS(1) | id);
    assert(history_get_y_pos() == 137);
    history_add(id, (const unsigned char *)"Different answer", 1);
    assert(history_count == 2 && history_list[0].last_y_pos == 137);
    assert(zim_sparrow_save(query) == id);
    /* The question remains recoverable after all transient slots are evicted. */
    for (i = 0; i < 80; ++i) {
        snprintf(text, sizeof(text), "capital of fixture %u", i);
        assert(zim_sparrow_save(text));
    }
    assert(!strcmp(zim_sparrow_question(id), query));
    assert(history_list_save(HISTORY_SAVE_NORMAL) == 1);
    assert(!history_needs_save());
    viewing_count = 0;
    history_list_init();
    assert(history_count == 2 && history_list[0].last_y_pos == 137);
    assert(!strcmp(zim_sparrow_question(history_list[0].idx_article), query));
    assert(!strcmp((char *)history_list[1].title, "Ordinary article"));
    id = visit("capital of peru");
    assert(strcmp(zim_sparrow_question(id), query));
    assert(!strcmp(zim_sparrow_question(history_list[1].idx_article), query));

    history_clear();
    for (i = 0; i < MAX_HISTORY; ++i) {
        snprintf(text, sizeof(text), "population of fixture %u", i);
        visit(text);
    }
    history_list_save(HISTORY_SAVE_NORMAL);
    /* A full on-card list followed by extra data must not overrun the array. */
    memcpy(disk + disk_size, disk, sizeof(HISTORY)); disk_size += sizeof(HISTORY);
    viewing_count = 0;
    history_list_init();
    assert(history_count == MAX_HISTORY);
    for (i = 0; i < MAX_HISTORY; ++i) {
        snprintf(text, sizeof(text), "population of fixture %u", MAX_HISTORY - i - 1);
        assert(!strcmp(zim_sparrow_question(history_list[i].idx_article), text));
    }
    /* Invalid titles, invalid question records, truncated files, read errors. */
    memset(disk + 8, 'x', MAX_TITLE_ACTUAL);
    memset(disk + sizeof(HISTORY) + 8, 0, MAX_TITLE_ACTUAL);
    history_list_init();
    assert(history_count == MAX_HISTORY - 1); /* the extra valid record is read */
    disk_size = sizeof(HISTORY) - 1;
    history_list_init(); assert(history_count == 0);
    fail_read = 1; history_list_init(); assert(history_count == 0); fail_read = 0;

    visit(query);
    fail_open = 1;
    assert(history_list_save(HISTORY_SAVE_NORMAL) == -1 && history_needs_save());
    fail_open = 0; short_write = 1;
    assert(history_list_save(HISTORY_SAVE_NORMAL) == -1 && history_needs_save());
    short_write = 0;
    assert(history_list_save(HISTORY_SAVE_NORMAL) == 1 && !history_needs_save());
    history_clear(); history_list_save(HISTORY_SAVE_NORMAL);
    viewing_count = 0; history_list_init();
    assert(history_count == 0);
    puts("PASS: question history survives restart, cache eviction and full lists; scroll, deduplication, corruption, write retry and clear");
    return 0;
}
