/*
 * malloc, free, realloc and memalign at random over a fixed seed, every
 * block filled with its own pattern and checked before it is resized or
 * freed: overlapping blocks, lost bytes on a realloc in place, or a
 * misplaced free-area all show as a wrong byte.  Large blocks take the
 * unmapping path.  Prints MALLOC PASS and the time it took.
 */
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SLOTS	128
#define ROUNDS	3000

static struct {
	unsigned char *p;
	size_t size;
	unsigned char tag;
	unsigned int how;	/* 0 malloc, 1 realloc, else the memalign alignment */
} slot[SLOTS];

static uint32_t seed = 12345;
/* Debugging switches: leave out memalign, realloc or large blocks. */
static int no_memalign, no_realloc, no_large;

static uint32_t rnd(void)
{
	seed = seed * 1103515245 + 12345;
	return seed >> 8;
}

static size_t pick_size(void)
{
	uint32_t r = rnd() % 100;

	if (r < 70)
		return 1 + rnd() % 64;
	if (r < 99 || no_large)
		return 1 + rnd() % 2048;
	return 32768 + rnd() % 32768;
}

/* A block that overlaps another live one is the allocator's fault. */
static int overlaps(int i)
{
	int j;

	for (j = 0; j < SLOTS; j++)
		if (j != i && slot[j].p && slot[i].p < slot[j].p + slot[j].size &&
		    slot[j].p < slot[i].p + slot[i].size) {
			printf("MALLOC FAIL: block %d %p+%zu (how %u) overlaps block %d %p+%zu (how %u)\n",
			       i, slot[i].p, slot[i].size, slot[i].how,
			       j, slot[j].p, slot[j].size, slot[j].how);
			return 1;
		}
	return 0;
}

static int check(int i)
{
	size_t n;

	for (n = 0; n < slot[i].size; n++)
		if (slot[i].p[n] != (unsigned char)(slot[i].tag + n)) {
			printf("MALLOC FAIL: block %d %p+%zu (how %u, header %zu) wrong from byte %zu\n",
			       i, slot[i].p, slot[i].size, slot[i].how,
			       ((size_t *)slot[i].p)[-1], n);
			return 0;
		}
	return 1;
}

static void fill(int i, size_t from)
{
	size_t n;

	for (n = from; n < slot[i].size; n++)
		slot[i].p[n] = (unsigned char)(slot[i].tag + n);
}

/*
 * A block as small as memalign can make, freed between live ones: it
 * becomes a free-area of its own, and its descriptor must fit inside it,
 * not reach back into the block before.  The block before varies in size
 * so that some memalign lands aligned, and small blocks take up the
 * fragments memalign hands back around it.
 */
static int small_memalign(void)
{
	unsigned char *around[8];
	size_t align, k, n;

	for (k = 1; k <= 40; k++)
		for (align = 16; align <= 64; align *= 2) {
			unsigned char *before = malloc(k), *m;
			int ok = 1;

			memset(before, 0x5a, k);
			m = memalign(align, 1);
			for (n = 0; n < 8; n++)
				around[n] = malloc(24);
			free(m);
			for (n = 0; n < k; n++)
				ok &= before[n] == 0x5a;
			for (n = 0; n < 8; n++)
				free(around[n]);
			free(before);
			if (!ok) {
				printf("MALLOC FAIL: freeing memalign(%zu, 1) wrote into the block before\n",
				       align);
				return 0;
			}
		}
	return 1;
}

int main(int argc, char **argv)
{
	struct timespec t0, t1;
	int round, i;

	for (i = 1; i < argc; i++) {
		no_memalign |= !strcmp(argv[i], "nomemalign");
		no_realloc |= !strcmp(argv[i], "norealloc");
		no_large |= !strcmp(argv[i], "nolarge");
	}

	clock_gettime(CLOCK_MONOTONIC, &t0);
	if (!no_memalign && !small_memalign())
		return 1;
	for (round = 0; round < ROUNDS; round++) {
		i = rnd() % SLOTS;
		if (slot[i].p && !check(i)) {
			printf("MALLOC FAIL: at round %d\n", round);
			return 1;
		}
		switch (rnd() % 4) {
		case 0:
		case 1:
			free(slot[i].p);
			slot[i].size = pick_size();
			slot[i].tag = rnd();
			if (rnd() % 4 == 0 && !no_memalign) {
				size_t align = 8 << (rnd() % 7);

				slot[i].p = memalign(align, slot[i].size);
				slot[i].how = align;
				if (slot[i].p && (uintptr_t)slot[i].p % align) {
					printf("MALLOC FAIL: memalign(%zu) gave %p\n", align, slot[i].p);
					return 1;
				}
			} else {
				slot[i].p = malloc(slot[i].size);
				slot[i].how = 0;
			}
			if (!slot[i].p) {
				printf("MALLOC FAIL: no memory for %zu bytes\n", slot[i].size);
				return 1;
			}
			if (overlaps(i))
				return 1;
			fill(i, 0);
			break;
		case 2:
			if (slot[i].p && !no_realloc) {
				size_t old = slot[i].size, size = pick_size(), n;
				unsigned char *p = realloc(slot[i].p, size);

				if (!p) {
					printf("MALLOC FAIL: realloc to %zu bytes\n", size);
					return 1;
				}
				/* What the block held must have come along. */
				for (n = 0; n < old && n < size; n++)
					if (p[n] != (unsigned char)(slot[i].tag + n)) {
						printf("MALLOC FAIL: realloc lost byte %zu\n", n);
						return 1;
					}
				slot[i].p = p;
				slot[i].size = size;
				slot[i].how = 1;
				if (overlaps(i))
					return 1;
				fill(i, old < size ? old : size);
			}
			break;
		default:
			free(slot[i].p);
			slot[i].p = NULL;
			slot[i].size = 0;
			break;
		}
	}
	for (i = 0; i < SLOTS; i++) {
		if (slot[i].p && !check(i)) {
			printf("MALLOC FAIL: block %d corrupted at the end\n", i);
			return 1;
		}
		free(slot[i].p);
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	printf("MALLOC PASS: %d operations in %.2f s\n", ROUNDS,
	       t1.tv_sec - t0.tv_sec + (t1.tv_nsec - t0.tv_nsec) / 1e9);
	return 0;
}
