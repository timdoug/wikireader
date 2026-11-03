/* Allocator comparison: speed of common patterns, and memory the kernel
 * sees taken and given back (MemFree, so the allocator's own overhead and
 * what it returns are both counted). */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static long memfree_kb(void)
{
	char line[128];
	long v = -1;
	FILE *f = fopen("/proc/meminfo", "r");
	while (f && fgets(line, sizeof line, f))
		if (sscanf(line, "MemFree: %ld kB", &v) == 1)
			break;
	if (f) fclose(f);
	return v;
}

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static unsigned rng = 12345;
static unsigned rnd(void) { rng = rng * 1103515245 + 12345; return rng >> 8; }

#define LIVE 2048
static void *live[LIVE];

int main(void)
{
	double t;
	long base = memfree_kb(), m;
	int i, fails = 0;

	/* 1. Small-object churn: 100k ops over a live set of 2048, 8-256 B. */
	t = now();
	for (i = 0; i < 100000; i++) {
		unsigned k = rnd() % LIVE;
		free(live[k]);
		live[k] = malloc(8 + rnd() % 249);
		if (!live[k]) fails++;
		else memset(live[k], i, 8);
	}
	printf("B churn_ms %.1f\n", (now() - t) * 1000);
	printf("B churn_live_kb %ld\n", base - memfree_kb());
	for (i = 0; i < LIVE; i++) { free(live[i]); live[i] = 0; }
	printf("B churn_after_free_kb %ld\n", base - memfree_kb());

	/* 2. realloc growth, string-builder style, to 256 KB in 64 B steps. */
	t = now();
	{
		char *p = 0;
		size_t n;
		for (n = 64; n <= 256 * 1024; n += 64) {
			char *q = realloc(p, n);
			if (!q) { fails++; break; }
			p = q;
			p[n - 1] = 1;
		}
		free(p);
	}
	printf("B realloc_ms %.1f\n", (now() - t) * 1000);

	/* 3. Fragmentation: 4 MB of 64 B objects, every other one freed. */
	{
		enum { N = 4 * 1024 * 1024 / 64 };
		void **v = malloc(N * sizeof *v);
		m = memfree_kb();
		t = now();
		for (i = 0; i < N; i++)
			if (!(v[i] = malloc(64))) fails++;
		printf("B small4m_ms %.1f\n", (now() - t) * 1000);
		printf("B small4m_kb %ld\n", m - memfree_kb());
		for (i = 0; i < N; i += 2) { free(v[i]); v[i] = 0; }
		printf("B half_freed_kb %ld\n", m - memfree_kb());
		/* Can the holes serve new objects of another size? */
		for (i = 0; i < N; i += 2)
			if (!(v[i] = malloc(48))) fails++;
		printf("B refill48_kb %ld\n", m - memfree_kb());
		for (i = 0; i < N; i++) free(v[i]);
		free(v);
		printf("B all_freed_kb %ld\n", m - memfree_kb());
	}

	/* 4. Large blocks: 64 KB to 1 MB, allocated and freed. */
	t = now();
	for (i = 0; i < 200; i++) {
		size_t n = 65536u << (i % 5);
		char *p = malloc(n);
		if (!p) { fails++; continue; }
		p[0] = p[n - 1] = 1;
		free(p);
	}
	printf("B large_ms %.1f\n", (now() - t) * 1000);

	/* 5. Medium objects, 1-16 KB, a live set of 256. */
	t = now();
	for (i = 0; i < 20000; i++) {
		unsigned k = rnd() % 256;
		free(live[k]);
		live[k] = malloc(1024 + rnd() % 15361);
		if (!live[k]) fails++;
	}
	printf("B medium_ms %.1f\n", (now() - t) * 1000);
	printf("B medium_live_kb %ld\n", base - memfree_kb());
	for (i = 0; i < 256; i++) free(live[i]);
	printf("B end_kb %ld\n", base - memfree_kb());
	printf("B fails %d\n", fails);
	return 0;
}
