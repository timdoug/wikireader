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

#define ZIM_BENCH_IMAGES 16

typedef struct {
	long y;                         /* top of the image in the article */
	int width, height;              /* as placed */
	unsigned long read_ticks;       /* its blob off the card */
	unsigned long decode_ticks;     /* decoder created .. destroyed */
	unsigned long created;
	unsigned long done;             /* decoded, in timer ticks */
} ZIM_BENCH_IMAGE;

/* Phase marks inside retrieve_article(), in timer ticks. */
enum {
	ZB_BLOB,        /* cluster decode / blob read starts */
	ZB_HTML,        /* HTML conversion starts */
	ZB_BUDGET,      /* image budget scan starts */
	ZB_WRAP,        /* wrapping starts */
	ZB_DONE,        /* wrapped */
	ZB_FINISH,      /* a preview's completion starts */
	ZB_FINISHED,    /* ...and is done */
	ZB_MARKS
};

typedef struct {
	int active;
	unsigned long start;            /* the tap */
	unsigned long mark[ZB_MARKS];
	unsigned long raw_size, text_size, article_size;
	ZIM_BENCH_IMAGE image[ZIM_BENCH_IMAGES];
	unsigned long retrieve_ticks;   /* __wrap_retrieve_article */
	file_io_stats_t io;             /* card reads during the retrieval */
	int images;                     /* decoders created, up to 16 */
	int preview;                    /* 1 previewed, 2 previewed but redisplayed */
} ZIM_BENCH;

extern ZIM_BENCH zim_bench;

#define ZIM_BENCH_MARK(phase) \
	do { if (zim_bench.active) zim_bench.mark[phase] = timer_get(); } while (0)

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
