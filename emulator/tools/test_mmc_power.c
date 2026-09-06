/* Exercise the real driver's power ioctl with a card that becomes busy
 * before power-off and has no usable bus at all once its supply is off. */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "mmc_mock/regs.h"

static uint32_t registers[7];
static bool powered, selected;
static unsigned bus_accesses, selections, received;
static unsigned busy_bytes;

uint32_t *mmc_test_register(unsigned reg)
{
	if (reg <= MOCK_STAT) {
		assert(powered && "SPI access with the card supply off");
		++bus_accesses;
	}
	if (reg == MOCK_RXD) {
		++received;
		registers[reg] = busy_bytes ? (--busy_bytes, 0) : 0xff;
	}
	return &registers[reg];
}

int check_card_power(void) { return powered; }
void enable_card_power(void) { powered = true; }
void disable_card_power(void)
{
	/* An enabled card must finish its busy phase and be deselected before
	 * losing power. Skipping all I/O unconditionally would fail this. */
	assert(!selected && busy_bytes == 0);
	powered = false;
}
void mmc_test_select(int value)
{
	assert(powered && "Selecting a card whose supply is off");
	selected = value != 0;
	++selections;
}
void delay_us(unsigned long usec) { (void)usec; }

#ifndef MMC_SOURCE
#define MMC_SOURCE "../../samo-lib/drivers/src/mmc.c"
#endif
#include MMC_SOURCE

static void power_down(void)
{
	BYTE off = 0;
	assert(mmc_disk_ioctl(0, CTRL_POWER, &off) == RES_OK);
	assert(!powered && (Stat & STA_NOINIT));
}

int main(void)
{
	registers[MOCK_STAT] = RDFF;
	/* Already off at boot: neither select nor access the isolated bus. */
	Stat = STA_NODISK;
	power_down();
	assert(Stat == (STA_NODISK | STA_NOINIT));
	assert(bus_accesses == 0 && selections == 0);

	/* First power-down waits for a pending write to finish. */
	powered = true;
	Stat = 0;
	busy_bytes = 4;
	power_down();
	assert(received >= 5 && selections == 2);
	unsigned previous_accesses = bus_accesses;
	unsigned previous_selections = selections;

	/* Input-only wakes can suspend again without a file-triggered restart. */
	for (unsigned i = 0; i < 100; ++i)
		power_down();
	assert(bus_accesses == previous_accesses);
	assert(selections == previous_selections);

	/* Hardware supply state, not STA_NOINIT, determines whether to drain. */
	powered = true;
	busy_bytes = 3;
	power_down();
	assert(bus_accesses > previous_accesses && selections == previous_selections + 2);
	puts("MMC power: active-card drain and repeated unpowered shutdowns pass");
	return 0;
}
