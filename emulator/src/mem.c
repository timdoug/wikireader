#include <stdlib.h>
#include <string.h>

#include "mem.h"
#include "model.h"

bool mem_init(struct mem *m)
{
	memset(m, 0, sizeof *m);
	m->a0ram  = calloc(1, A0RAM_SIZE);
	m->ivram  = calloc(1, IVRAM_SIZE);
	m->dstram = calloc(1, DSTRAM_SIZE);
	m->sdram  = calloc(1, SDRAM_SIZE);
	m->log = stderr;
	return m->a0ram && m->ivram && m->dstram && m->sdram;
}

void mem_clear_ram(struct mem *m)
{
	memset(m->a0ram,  0, A0RAM_SIZE);
	memset(m->ivram,  0, IVRAM_SIZE);
	memset(m->dstram, 0, DSTRAM_SIZE);
	memset(m->sdram,  0, SDRAM_SIZE);
}

void mem_free(struct mem *m)
{
	free(m->a0ram); free(m->ivram); free(m->dstram); free(m->sdram);
	memset(m, 0, sizeof *m);
}

void mem_add_mmio(struct mem *m, const char *name, uint32_t off, uint32_t len,
		  mmio_fn fn, void *ctx)
{
	if (m->ndev >= MAX_MMIO)
		return;
	m->dev[m->ndev++] = (struct mmio_dev){ name, off, len, fn, ctx };
}

void mem_set_timing(struct mem *m, mem_wait_fn fn, void *ctx)
{
	m->wait = fn;
	m->wait_ctx = ctx;
}

uint64_t mem_wait(void *ctx, enum mem_access access, uint32_t addr,
		  unsigned size, uint64_t now)
{
	struct mem *m = ctx;

	/* A peripheral register is not free, and until the device was asked
	 * this charged nothing at all: eight reads of the SDRAM controller's
	 * timing register cost the machine 82.50 cycles a pass against the
	 * 15.15 the same loop costs with adds in it, so one costs about nine.
	 * Every driver on this part is a loop over registers, which is why
	 * the card benchmark spends most of its time somewhere the profile
	 * could not account for.
	 */
	if (model.mmio_wait && addr - REG_BASE < REG_SIZE)
		return model.mmio_wait;

	return m->wait ? m->wait(m->wait_ctx, access, addr, size, now) : 0;
}

/* Resolve an address to a host pointer, or NULL if not plain RAM. */
/*
 * Host pointer for a mapped guest address, or NULL.
 *
 * The bounds tests are written as "offset within region" rather than
 * "a + size <= end", because the latter wraps: for a = 0xffffffff and
 * size = 4, a + size is 3, which is inside every region, and the function
 * would hand back a pointer 0xefffffff bytes past the SDRAM buffer. Real
 * firmware never generated an address like that, so this sat unnoticed
 * until the boot ROM path ran code against uninitialised hardware.
 */
void mem_set_sdram_size(struct mem *m, uint32_t bytes)
{
	if (bytes >= SDRAM_SIZE || (bytes & (bytes - 1)))
		bytes = 0;
	if (m->sdram_alias != bytes) {
		m->sdram_alias = bytes;
		m->sdram_epoch++;
	}
}

static uint8_t *ram_ptr(struct mem *m, uint32_t a, unsigned size)
{
	if (a >= SDRAM_BASE && a - SDRAM_BASE <= SDRAM_SIZE - size) {
		uint32_t off = a - SDRAM_BASE;
		if (m->sdram_alias) {
			off &= m->sdram_alias - 1;
			if (off > m->sdram_alias - size)
				return NULL;
		}
		return m->sdram + off;
	}
	if (a <= A0RAM_SIZE - size)
		return m->a0ram + a;
	if (a >= IVRAM_BASE && a - IVRAM_BASE <= IVRAM_SIZE - size)
		return m->ivram + (a - IVRAM_BASE);
	if (a >= DSTRAM_BASE && a - DSTRAM_BASE <= DSTRAM_SIZE - size)
		return m->dstram + (a - DSTRAM_BASE);
	return NULL;
}

static bool mmio(struct mem *m, uint32_t a, unsigned size, uint32_t *v,
		 bool write)
{
	if (a < REG_BASE || a >= REG_BASE + REG_SIZE)
		return false;
	uint32_t off = a - REG_BASE;
	for (unsigned i = 0; i < m->ndev; i++) {
		struct mmio_dev *d = &m->dev[i];
		if (off >= d->off && off < d->off + d->len)
			if (d->fn(d->ctx, off, size, v, write))
				return true;
	}
	if (m->trace_mmio)
		fprintf(m->log, "  mmio %s %s+0x%04x size %u%s\n",
			write ? "write" : "read ", "REG", off, size,
			write ? "" : " -> 0 (unclaimed)");
	if (!write)
		*v = 0;
	return true;   /* swallow unclaimed register accesses */
}

/*
 * Fixed identification bytes, S1C33E07 Technical Manual I.5.2:
 * C33 PE little-endian core, S1C33E series, model E07, version 0x21.
 */
static bool chip_id(uint32_t addr, unsigned size, uint32_t *value)
{
	static const uint8_t id[CHIP_ID_SIZE] = { 0x06, 0x0e, 0x07, 0x21 };

	if (addr < CHIP_ID_BASE || addr - CHIP_ID_BASE > CHIP_ID_SIZE - size)
		return false;
	*value = 0;
	for (unsigned k = 0; k < size; k++)
		*value |= (uint32_t)id[addr - CHIP_ID_BASE + k] << (8 * k);
	return true;
}

uint8_t *mem_region(struct mem *m, uint32_t addr, uint32_t *base, uint32_t *len)
{
	if (addr >= SDRAM_BASE && addr < SDRAM_BASE + SDRAM_SIZE) {
		if (m->sdram_alias) {
			/* Each alias window maps linearly onto the same cells. */
			*base = SDRAM_BASE + ((addr - SDRAM_BASE) & ~(m->sdram_alias - 1));
			*len = m->sdram_alias;
			return m->sdram;
		}
		*base = SDRAM_BASE; *len = SDRAM_SIZE; return m->sdram;
	}
	if (addr < A0RAM_SIZE) {
		*base = 0; *len = A0RAM_SIZE; return m->a0ram;
	}
	if (addr >= IVRAM_BASE && addr < IVRAM_BASE + IVRAM_SIZE) {
		*base = IVRAM_BASE; *len = IVRAM_SIZE; return m->ivram;
	}
	if (addr >= DSTRAM_BASE && addr < DSTRAM_BASE + DSTRAM_SIZE) {
		*base = DSTRAM_BASE; *len = DSTRAM_SIZE; return m->dstram;
	}
	return NULL;
}

uint32_t mem_read(void *ctx, uint32_t addr, unsigned size)
{
	struct mem *m = ctx;
	uint8_t *p = ram_ptr(m, addr, size);
	uint32_t v = 0;

	if (p) {
		memcpy(&v, p, size);        /* host is little-endian, as is c33 */
		return v;
	}
	if (size <= CHIP_ID_SIZE && chip_id(addr, size, &v))
		return v;
	if (mmio(m, addr, size, &v, false))
		return v;

	m->unmapped_reads++;
	if (m->unmapped_reads <= 8)
		fprintf(m->log, "  unmapped read  0x%08x size %u (pc=0x%08x)\n",
			addr, size, m->pc_src ? *m->pc_src : 0);
	return 0;
}

void mem_write(void *ctx, uint32_t addr, unsigned size, uint32_t val)
{
	struct mem *m = ctx;
	uint8_t *p = ram_ptr(m, addr, size);

	if (m->vwatch_on && size == 1 && (val & 0xff) == m->vwatch &&
	    m->vhits < 12) {
		m->vhits++;
		fprintf(m->log, "  VALWATCH: byte 0x%02x -> 0x%08x (pc=0x%08x)\n",
			val & 0xff, addr, m->pc_src ? *m->pc_src : 0);
	}
	if (m->watch_on && addr <= m->watch && addr + size > m->watch)
		fprintf(m->log, "  WATCH: store to 0x%08x size %u = 0x%08x"
			" (pc=0x%08x)\n", addr, size, val,
			m->pc_src ? *m->pc_src : 0);

	if (p) {
		memcpy(p, &val, size);
		return;
	}
	if (size <= CHIP_ID_SIZE && chip_id(addr, size, &val))
		return;                    /* identification area is read-only */
	if (mmio(m, addr, size, &val, true))
		return;

	m->unmapped_writes++;
	if (m->unmapped_writes <= 8)
		fprintf(m->log, "  unmapped write 0x%08x size %u = 0x%x"
			" (pc=0x%08x)\n", addr, size, val,
			m->pc_src ? *m->pc_src : 0);
}
