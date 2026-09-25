/* Exercise card waits through clocked SPI traffic, not firmware counters. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/mem.h"
#include "../src/model.h"
#include "../src/sdcard.h"

static struct mem memory;
static struct sdcard card;
static struct port port;
static uint64_t ticks;

static unsigned exchange(unsigned byte)
{
	mem_write(&memory, REG_BASE + 0x1704, 4, byte);
	unsigned polls = 0;
	while (!(mem_read(&memory, REG_BASE + 0x1714, 4) & 4)) {
		ticks += 4;
		sd_poll(&card);
		assert(++polls < 100);
	}
	return mem_read(&memory, REG_BASE + 0x1700, 4) & 255;
}

static void command(unsigned number, unsigned arg)
{
	exchange(0x40 | number);
	for (int shift = 24; shift >= 0; shift -= 8)
		exchange((arg >> shift) & 255);
	exchange(0x95);
}

static unsigned response(void)
{
	for (unsigned i = 0; i < 10000; i++) {
		unsigned byte = exchange(255);
		if (byte != 255)
			return byte;
	}
	assert(!"response timed out");
	return 255;
}

static void payload(void)
{
	for (unsigned i = 0; i < 512; i++)
		assert(exchange(255) == 0x5a);
	exchange(255);
	exchange(255);
}

int main(void)
{
	const char *path = "/tmp/test_sd_timing.img";
	FILE *image = fopen(path, "wb");
	assert(image);
	unsigned char data[512];
	memset(data, 0x5a, sizeof data);
	for (unsigned i = 0; i < 8; i++)
		assert(fwrite(data, 1, sizeof data, image) == sizeof data);
	assert(!fclose(image));
	assert(mem_init(&memory));
	port_reset(&port);
	port.reg[OFF_P3D] |= 1u << 3;   /* level buffer on */
	port.reg[OFF_P5D] &= ~(1u << CS_SDCARD_BIT);
	assert(sd_attach(&memory, &card, path, &port, NULL, false));
	sd_set_clock(&card, &ticks);
	mem_write(&memory, REG_BASE + 0x1708, 4, (7u << 10) | 3u);
	model.sd_init_latency = 60000;
	model.sd_read_latency = 12000;
	model.sd_read_gap = 3000;
	model.sd_write_latency = 6000;

	command(0, 0);
	assert(response() == 1);
	uint64_t start = ticks;
	unsigned polls = 0, result;
	do {
		command(55, 0);
		assert(response() == 1);
		command(41, 1u << 30);
		result = response();
		assert(result <= 1 && ++polls < 1000);
	} while (result);
	assert(polls > 1 && ticks - start >= model.sd_init_latency);
	assert(ticks - start < model.sd_init_latency + 1500);
	command(55, 0);
	assert(response() == 0);
	command(41, 1u << 30);
	assert(response() == 0); /* an initialized card remains ready */

	command(17, 1);
	start = ticks;
	assert(response() == 0);
	assert(response() == 0xfe);
	assert(ticks - start >= model.sd_read_latency);
	assert(ticks - start <= model.sd_read_latency + 32);
	payload();
	command(18, 1);
	assert(response() == 0 && response() == 0xfe);
	payload();
	start = ticks;
	assert(response() == 0xfe);
	assert(ticks - start >= model.sd_read_gap);
	assert(ticks - start <= model.sd_read_gap + 64);
	payload();
	assert(exchange(255) == 255); /* next block is pending */
	start = ticks;
	command(12, 0);
	assert(response() == 0 && ticks - start < model.sd_read_gap);
	command(8, 0x1aa);
	assert(response() == 1);
	assert(exchange(255) == 0 && exchange(255) == 0);
	assert(exchange(255) == 1 && exchange(255) == 0xaa);

	command(24, 5);
	assert(response() == 0);
	exchange(0xfe);
	for (unsigned i = 0; i < 512; i++) exchange(0x5a);
	exchange(255); exchange(255);
	start = ticks;
	assert(response() == 5);
	assert(exchange(255) == 0);
	port.reg[OFF_P5D] |= 1u << CS_SDCARD_BIT;
	assert(exchange(255) == 255);
	port.reg[OFF_P3D] |= 1u << 3;   /* level buffer on */
	port.reg[OFF_P5D] &= ~(1u << CS_SDCARD_BIT);
	assert(exchange(255) == 0); /* programming survives deselection */
	while (exchange(255) == 0) assert(ticks - start < 10000);
	assert(ticks - start >= model.sd_write_latency);
	assert(ticks - start <= model.sd_write_latency + 32);

	/* Reset cancels pending timing state instead of leaking it into boot. */
	command(17, 1);
	assert(response() == 0);
	sd_reset(&card);
	mem_write(&memory, REG_BASE + 0x1708, 4, (7u << 10) | 3u);
	command(0, 0);
	assert(response() == 1);
	command(55, 0);
	assert(response() == 1);
	command(41, 1u << 30);
	assert(response() == 1);
	sd_close(&card);
	remove(path);
	puts("SD timing: initialization, first token, stream gap, CMD12, programming and reset pass");
	return 0;
}
