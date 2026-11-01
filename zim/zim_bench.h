/* Device benchmark: "zim.app bench LABEL [NEXT.app]" from the launcher.
 *
 * Loads every article listed in 0:/zimbench.lst cold, one after another,
 * renders its first screen and decodes the images on it, and appends one
 * line per load to 0:/zimbench.txt.  With NEXT it then chains to
 * "NEXT bench ..." so two builds run from one tap; the last one shows the
 * comparison and returns to the launcher on a tap or button.
 *
 * The hooks below cost nothing unless zim_bench.active is set. */
#ifndef WIKIREADER_ZIM_BENCH_H
#define WIKIREADER_ZIM_BENCH_H

#include <grifo.h>

typedef struct {
	int active;
	unsigned long retrieve_ticks;   /* __wrap_retrieve_article */
	file_io_stats_t io;             /* card reads during the retrieval */
	unsigned long image_create;     /* first image's decoder created */
	unsigned long image_destroy;    /* ...and destroyed */
	int image_started;
	int image_done;
	int images;                     /* decoders created during the load */
} ZIM_BENCH;

extern ZIM_BENCH zim_bench;

/* zim.c: record "bench LABEL [NEXT.app NEXTLABEL]". */
void zim_bench_args(int argc, char **argv);
/* wikilib_run(), once the reader is initialized: nonzero if a benchmark was
 * requested, in which case it runs it and never returns. */
int zim_bench_requested(void);
void zim_bench_start(void) __attribute__((noreturn));

/* search.c: the article whose title is exactly TITLE (ignoring case), found
 * through the same prefix search the keyboard uses; 0 if there is none. */
long zim_bench_find_title(const char *title);
/* search.c: forget every cached article and decoded cluster. */
void zim_bench_drop_caches(void);

#endif
