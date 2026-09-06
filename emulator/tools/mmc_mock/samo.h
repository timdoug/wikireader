#ifndef MMC_TEST_SAMO_H
#define MMC_TEST_SAMO_H
int check_card_power(void);
void enable_card_power(void);
void disable_card_power(void);
void mmc_test_select(int selected);
#define SDCARD_CS_LO() mmc_test_select(1)
#define SDCARD_CS_HI() mmc_test_select(0)
#endif
