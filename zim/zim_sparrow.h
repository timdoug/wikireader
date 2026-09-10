#ifndef ZIM_SPARROW_H
#define ZIM_SPARROW_H
#include <stdint.h>
#include <stddef.h>
/* Reserved above the largest supported real archive in Ask mode. */
#define ZIM_SPARROW_BASE 0x07fff000u
#define ZIM_SPARROW_IS_PAGE(id) (((uint32_t)(id) & 0x07ffffffu) >= ZIM_SPARROW_BASE)
uint32_t zim_sparrow_save(const char *query);
void zim_sparrow_history_loaded(void);
const char *zim_sparrow_question(uint32_t id);
int zim_sparrow_html(uint32_t id, unsigned char *buffer, size_t capacity, size_t *size);
int zim_sparrow_title(const unsigned char *path, size_t length, char *title, size_t capacity);
#endif
