// SPDX-License-Identifier: GPL-2.0-only
/*
 * WikiReader SD card block driver.
 *
 * The card is wired to the S1C33E07 SPI controller.  This deliberately starts
 * with polling and single-sector commands: the boot card is a recovery and
 * diagnostics path, so a small, bounded implementation is more useful than a
 * fast one whose DMA and interrupt paths have not yet met real hardware.
 */
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/delay.h>
#include <linux/highmem.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mutex.h>

#define WR_REG_BASE             0x00300000UL

#define WR_P3_DATA              (WR_REG_BASE + 0x386)
#define WR_P3_DIR               (WR_REG_BASE + 0x387)
#define WR_P5_DATA              (WR_REG_BASE + 0x38a)
#define WR_P5_DIR               (WR_REG_BASE + 0x38b)
#define WR_P5_FUNC03            (WR_REG_BASE + 0x3aa)
#define WR_P6_FUNC47            (WR_REG_BASE + 0x3ad)

#define WR_CMU_GATE1            (WR_REG_BASE + 0x1b04)
#define WR_CMU_PROTECT          (WR_REG_BASE + 0x1b24)
#define WR_CMU_GATE1_DMA        BIT(1)
#define WR_CMU_GATE1_SPI        BIT(6)

#define WR_SPI_RXD              (WR_REG_BASE + 0x1700)
#define WR_SPI_TXD              (WR_REG_BASE + 0x1704)
#define WR_SPI_CTL1             (WR_REG_BASE + 0x1708)
#define WR_SPI_CTL2             (WR_REG_BASE + 0x170c)
#define WR_SPI_WAIT             (WR_REG_BASE + 0x1710)
#define WR_SPI_STAT             (WR_REG_BASE + 0x1714)
#define WR_SPI_INT              (WR_REG_BASE + 0x1718)

#define WR_SPI_BPT_8            (7U << 10)
#define WR_SPI_DIV_4            (0U << 4)
#define WR_SPI_DIV_128          (5U << 4)
#define WR_SPI_MASTER           BIT(1)
#define WR_SPI_ENABLE           BIT(0)
#define WR_SPI_BUSY             BIT(6)
#define WR_SPI_RX_FULL          BIT(2)

#define WR_HS2_ENABLE           (WR_REG_BASE + 0x114c)
#define WR_HS3_ENABLE           (WR_REG_BASE + 0x115c)
#define WR_IDMA_ENABLE          (WR_REG_BASE + 0x1105)
#define WR_IDMAREQ_SPI          (WR_REG_BASE + 0x29b)
#define WR_IDMAEN_SPI           (WR_REG_BASE + 0x29c)
#define WR_IDMA_SPI_BIT         BIT(4)

#define WR_SD_CS                BIT(0)
#define WR_EEPROM_CS            BIT(2)
#define WR_CS_OUTPUTS           (WR_SD_CS | BIT(1) | WR_EEPROM_CS)
#define WR_SD_VCCEN             BIT(2)
#define WR_SD_BUFEN             BIT(3)
#define WR_SD_POWER_BITS        (WR_SD_VCCEN | WR_SD_BUFEN)

#define WR_SD_SECTOR_SIZE       512
#define WR_SD_COMMAND_POLLS     32
#define WR_SD_TOKEN_POLLS       100000
#define WR_SD_READY_POLLS       100000
#define WR_SD_INIT_TRIES        1000
#define WR_SPI_POLLS            1000000

#define SD_CMD0                 0
#define SD_CMD1                 1
#define SD_CMD8                 8
#define SD_CMD9                 9
#define SD_CMD16                16
#define SD_CMD17                17
#define SD_CMD24                24
#define SD_CMD55                55
#define SD_CMD58                58
#define SD_ACMD41               41

#define SD_R1_IDLE              BIT(0)
#define SD_R1_ILLEGAL           BIT(2)
#define SD_TOKEN_DATA           0xfe

struct wr_sd_device {
	struct gendisk *disk;
	struct mutex lock; /* Serialize commands on the shared SPI card. */
	sector_t sectors;
	int major;
	bool high_capacity;
};

static struct wr_sd_device wr_sd;

static inline u8 wr_read8(unsigned long address)
{
	return readb((void __iomem *)address);
}

static inline u32 wr_read32(unsigned long address)
{
	return readl((void __iomem *)address);
}

static inline void wr_write8(u8 value, unsigned long address)
{
	writeb(value, (void __iomem *)address);
}

static inline void wr_write16(u16 value, unsigned long address)
{
	writew(value, (void __iomem *)address);
}

static inline void wr_write32(u32 value, unsigned long address)
{
	writel(value, (void __iomem *)address);
}

static void wr_modify8(unsigned long address, u8 clear, u8 set)
{
	wr_write8((wr_read8(address) & ~clear) | set, address);
}

static int wr_spi_wait(u32 flag, bool wanted)
{
	unsigned int count;

	for (count = 0; count < WR_SPI_POLLS; count++) {
		if (!!(wr_read32(WR_SPI_STAT) & flag) == wanted)
			return 0;
		cpu_relax();
	}

	return -ETIMEDOUT;
}

static int wr_spi_transfer(u8 out, u8 *in)
{
	if (wr_spi_wait(WR_SPI_BUSY, false))
		return -ETIMEDOUT;
	wr_write32(out, WR_SPI_TXD);
	if (wr_spi_wait(WR_SPI_RX_FULL, true))
		return -ETIMEDOUT;
	*in = wr_read32(WR_SPI_RXD);
	return 0;
}

static int wr_spi_byte(u8 out)
{
	u8 in;

	if (wr_spi_transfer(out, &in))
		return -ETIMEDOUT;
	return in;
}

static void wr_sd_select(bool selected)
{
	wr_modify8(WR_P5_DATA, selected ? WR_SD_CS : 0,
		   selected ? 0 : WR_SD_CS);
}

static void wr_sd_release(void)
{
	wr_sd_select(false);
	wr_spi_byte(0xff);
}

/* Leave chip select asserted so the caller can consume an R3/R7 or data. */
static int wr_sd_command(u8 command, u32 argument, u8 crc)
{
	unsigned int count;
	int response;

	wr_sd_select(true);
	if (wr_spi_byte(0xff) < 0)
		return -ETIMEDOUT;
	if (wr_spi_byte(0x40 | command) < 0 ||
	    wr_spi_byte(argument >> 24) < 0 ||
	    wr_spi_byte(argument >> 16) < 0 ||
	    wr_spi_byte(argument >> 8) < 0 ||
	    wr_spi_byte(argument) < 0 || wr_spi_byte(crc) < 0)
		return -ETIMEDOUT;

	for (count = 0; count < WR_SD_COMMAND_POLLS; count++) {
		response = wr_spi_byte(0xff);
		if (response < 0 || !(response & 0x80))
			return response;
	}

	return -ETIMEDOUT;
}

static int wr_sd_wait_token(u8 wanted)
{
	unsigned int count;
	int value;

	for (count = 0; count < WR_SD_TOKEN_POLLS; count++) {
		value = wr_spi_byte(0xff);
		if (value < 0)
			return value;
		if (value == wanted)
			return 0;
		if (value != 0xff)
			return -EIO;
	}

	return -ETIMEDOUT;
}

static int wr_sd_read_register(u8 command, u8 value[16])
{
	unsigned int i;
	int response;
	int ret;

	response = wr_sd_command(command, 0, 0xff);
	if (response) {
		ret = response < 0 ? response : -EIO;
		goto out;
	}
	ret = wr_sd_wait_token(SD_TOKEN_DATA);
	if (ret)
		goto out;
	for (i = 0; i < 16; i++) {
		response = wr_spi_byte(0xff);
		if (response < 0) {
			ret = response;
			goto out;
		}
		value[i] = response;
	}
	wr_spi_byte(0xff);
	wr_spi_byte(0xff);
out:
	wr_sd_release();
	return ret;
}

static sector_t wr_sd_csd_sectors(const u8 csd[16])
{
	u32 csize;
	u32 blocks;
	u32 block_len;
	u32 multiplier;

	if ((csd[0] >> 6) == 1) {
		csize = ((u32)(csd[7] & 0x3f) << 16) |
			((u32)csd[8] << 8) | csd[9];
		return (sector_t)(csize + 1) * 1024;
	}

	/* CSD v1: capacity = (C_SIZE + 1) * 2^(C_SIZE_MULT+2) *
	 * 2^READ_BL_LEN bytes.  Express that in 512-byte sectors.
	 */
	csize = ((u32)(csd[6] & 3) << 10) | ((u32)csd[7] << 2) |
		(csd[8] >> 6);
	multiplier = ((csd[9] & 3) << 1) | (csd[10] >> 7);
	block_len = csd[5] & 0x0f;
	blocks = (csize + 1) << (multiplier + 2);
	if (block_len >= 9)
		return (sector_t)blocks << (block_len - 9);
	return (sector_t)blocks >> (9 - block_len);
}

static void wr_sd_hardware_init(void)
{
	u8 ignored;
	unsigned int i;

	/* Preserve P53: it is SDRAM address line SDA10 on this board. */
	wr_modify8(WR_P6_FUNC47, 0xfc, 0x54);
	wr_modify8(WR_P5_FUNC03, 0x3f, 0x01);
	wr_modify8(WR_P5_DATA, 0, WR_CS_OUTPUTS);
	wr_modify8(WR_P5_DIR, 0, WR_CS_OUTPUTS);

	wr_modify8(WR_P3_DIR, 0, WR_SD_POWER_BITS);
	wr_modify8(WR_P3_DATA, WR_SD_POWER_BITS, WR_SD_VCCEN);
	fsleep(10);
	wr_modify8(WR_P3_DATA, WR_SD_POWER_BITS, 0);
	mdelay(1);
	wr_modify8(WR_P3_DATA, WR_SD_POWER_BITS, WR_SD_BUFEN);

	wr_write32(0x96, WR_CMU_PROTECT);
	wr_write32(wr_read32(WR_CMU_GATE1) | WR_CMU_GATE1_SPI |
		   WR_CMU_GATE1_DMA, WR_CMU_GATE1);
	wr_write32(0, WR_CMU_PROTECT);

	/* Disarm the loader's SPI DMA channels before touching the controller. */
	wr_write16(0, WR_HS2_ENABLE);
	wr_write16(0, WR_HS3_ENABLE);
	wr_modify8(WR_IDMAEN_SPI, WR_IDMA_SPI_BIT, 0);
	wr_modify8(WR_IDMAREQ_SPI, WR_IDMA_SPI_BIT, 0);
	wr_write8(0, WR_IDMA_ENABLE);

	wr_write32(0, WR_SPI_CTL2);
	wr_write32(0, WR_SPI_WAIT);
	wr_write32(0, WR_SPI_INT);
	wr_write32(WR_SPI_BPT_8 | WR_SPI_DIV_128 | WR_SPI_MASTER |
		   WR_SPI_ENABLE, WR_SPI_CTL1);
	wr_read32(WR_SPI_RXD);

	wr_sd_select(false);
	for (i = 0; i < 10; i++)
		wr_spi_transfer(0xff, &ignored);
}

static int wr_sd_card_init(struct wr_sd_device *card)
{
	u8 csd[16];
	u8 r7[4];
	u8 ocr[4];
	unsigned int count;
	int response;
	int i;
	bool version2 = false;

	wr_sd_hardware_init();

	for (count = 0; count < 20; count++) {
		response = wr_sd_command(SD_CMD0, 0, 0x95);
		wr_sd_release();
		if (response == SD_R1_IDLE)
			break;
	}
	if (response != SD_R1_IDLE)
		return response < 0 ? response : -ENODEV;

	response = wr_sd_command(SD_CMD8, 0x1aa, 0x87);
	if (response == SD_R1_IDLE) {
		for (i = 0; i < 4; i++) {
			response = wr_spi_byte(0xff);
			if (response < 0) {
				wr_sd_release();
				return response;
			}
			r7[i] = response;
		}
		version2 = r7[2] == 1 && r7[3] == 0xaa;
	} else if (response < 0 || !(response & SD_R1_ILLEGAL)) {
		wr_sd_release();
		return response < 0 ? response : -EIO;
	}
	wr_sd_release();

	for (count = 0; count < WR_SD_INIT_TRIES; count++) {
		response = wr_sd_command(SD_CMD55, 0, 0xff);
		wr_sd_release();
		if (response < 0)
			return response;
		response = wr_sd_command(SD_ACMD41,
					 version2 ? BIT(30) : 0, 0xff);
		wr_sd_release();
		if (response == 0)
			break;
		if (response < 0 || (response & ~SD_R1_IDLE))
			break;
		fsleep(1000);
	}

	/* MMC and a few old SDSC cards need CMD1 instead of ACMD41. */
	if (response != 0) {
		for (count = 0; count < WR_SD_INIT_TRIES; count++) {
			response = wr_sd_command(SD_CMD1, 0, 0xff);
			wr_sd_release();
			if (response == 0)
				break;
			if (response < 0 || (response & ~SD_R1_IDLE))
				return response < 0 ? response : -EIO;
			fsleep(1000);
		}
	}
	if (response)
		return response < 0 ? response : -ETIMEDOUT;

	card->high_capacity = false;
	if (version2) {
		response = wr_sd_command(SD_CMD58, 0, 0xff);
		if (response) {
			wr_sd_release();
			return response < 0 ? response : -EIO;
		}
		for (i = 0; i < 4; i++) {
			response = wr_spi_byte(0xff);
			if (response < 0) {
				wr_sd_release();
				return response;
			}
			ocr[i] = response;
		}
		wr_sd_release();
		card->high_capacity = !!(ocr[0] & BIT(6));
	}

	if (!card->high_capacity) {
		response = wr_sd_command(SD_CMD16, WR_SD_SECTOR_SIZE, 0xff);
		wr_sd_release();
		if (response)
			return response < 0 ? response : -EIO;
	}

	if (wr_sd_read_register(SD_CMD9, csd))
		return -EIO;
	card->sectors = wr_sd_csd_sectors(csd);
	if (!card->sectors)
		return -EIO;

	/* MCLK/4 = 12 MHz, safely below the 25 MHz SPI-mode limit. */
	wr_write32(WR_SPI_BPT_8 | WR_SPI_DIV_4 | WR_SPI_MASTER |
		   WR_SPI_ENABLE, WR_SPI_CTL1);
	return 0;
}

static u32 wr_sd_argument(struct wr_sd_device *card, sector_t sector)
{
	return card->high_capacity ? (u32)sector : (u32)sector * 512;
}

static int wr_sd_read_sector(struct wr_sd_device *card, sector_t sector,
			     u8 *buffer)
{
	unsigned int i;
	int response;
	int ret;

	response = wr_sd_command(SD_CMD17, wr_sd_argument(card, sector), 0xff);
	if (response) {
		ret = response < 0 ? response : -EIO;
		goto out;
	}
	ret = wr_sd_wait_token(SD_TOKEN_DATA);
	if (ret)
		goto out;
	for (i = 0; i < WR_SD_SECTOR_SIZE; i++) {
		response = wr_spi_byte(0xff);
		if (response < 0) {
			ret = response;
			goto out;
		}
		buffer[i] = response;
	}
	wr_spi_byte(0xff);
	wr_spi_byte(0xff);
out:
	wr_sd_release();
	return ret;
}

static int wr_sd_write_sector(struct wr_sd_device *card, sector_t sector,
			      const u8 *buffer)
{
	unsigned int i;
	int response;
	int ret = 0;

	response = wr_sd_command(SD_CMD24, wr_sd_argument(card, sector), 0xff);
	if (response) {
		ret = response < 0 ? response : -EIO;
		goto out;
	}
	if (wr_spi_byte(0xff) < 0 || wr_spi_byte(SD_TOKEN_DATA) < 0) {
		ret = -ETIMEDOUT;
		goto out;
	}
	for (i = 0; i < WR_SD_SECTOR_SIZE; i++) {
		if (wr_spi_byte(buffer[i]) < 0) {
			ret = -ETIMEDOUT;
			goto out;
		}
	}
	if (wr_spi_byte(0xff) < 0 || wr_spi_byte(0xff) < 0) {
		ret = -ETIMEDOUT;
		goto out;
	}
	response = wr_spi_byte(0xff);
	if (response < 0 || (response & 0x1f) != 5) {
		ret = response < 0 ? response : -EIO;
		goto out;
	}
	for (i = 0; i < WR_SD_READY_POLLS; i++) {
		response = wr_spi_byte(0xff);
		if (response < 0) {
			ret = response;
			goto out;
		}
		if (response == 0xff)
			goto out;
	}
	ret = -ETIMEDOUT;
out:
	wr_sd_release();
	return ret;
}

static void wr_sd_submit_bio(struct bio *bio)
{
	struct wr_sd_device *card = bio->bi_bdev->bd_disk->private_data;
	struct bvec_iter iter;
	struct bio_vec bvec;
	sector_t sector = bio->bi_iter.bi_sector;
	int ret = 0;

	if (bio_op(bio) == REQ_OP_FLUSH) {
		bio_endio(bio);
		return;
	}
	if (bio_op(bio) != REQ_OP_READ && bio_op(bio) != REQ_OP_WRITE) {
		bio_io_error(bio);
		return;
	}

	mutex_lock(&card->lock);
	bio_for_each_segment(bvec, bio, iter) {
		u8 *data = bvec_kmap_local(&bvec);
		unsigned int offset;

		if (bvec.bv_len % WR_SD_SECTOR_SIZE ||
		    sector + bvec.bv_len / WR_SD_SECTOR_SIZE > card->sectors) {
			ret = -EIO;
		} else {
			for (offset = 0; offset < bvec.bv_len;
			     offset += WR_SD_SECTOR_SIZE, sector++) {
				if (bio_op(bio) == REQ_OP_READ)
					ret = wr_sd_read_sector(card, sector, data + offset);
				else
					ret = wr_sd_write_sector(card, sector, data + offset);
				if (ret)
					break;
			}
		}
		kunmap_local(data);
		if (ret)
			break;
	}
	mutex_unlock(&card->lock);

	if (ret) {
		pr_err_ratelimited("wrsd: I/O failed at sector %llu: %d\n",
				   (unsigned long long)sector, ret);
		bio_io_error(bio);
	} else {
		bio_endio(bio);
	}
}

static const struct block_device_operations wr_sd_fops = {
	.owner = THIS_MODULE,
	.submit_bio = wr_sd_submit_bio,
};

static int __init wr_sd_init(void)
{
	struct queue_limits limits = {
		.logical_block_size = WR_SD_SECTOR_SIZE,
		.physical_block_size = WR_SD_SECTOR_SIZE,
		.max_hw_sectors = 128,
		.features = BLK_FEAT_SYNCHRONOUS,
	};
	struct gendisk *disk;
	int ret;

	mutex_init(&wr_sd.lock);
	ret = wr_sd_card_init(&wr_sd);
	if (ret) {
		pr_err("wrsd: card initialization failed: %d\n", ret);
		return ret;
	}

	wr_sd.major = register_blkdev(0, "wrsd");
	if (wr_sd.major < 0)
		return wr_sd.major;

	disk = blk_alloc_disk(&limits, NUMA_NO_NODE);
	if (IS_ERR(disk)) {
		ret = PTR_ERR(disk);
		goto unregister;
	}
	wr_sd.disk = disk;
	disk->major = wr_sd.major;
	disk->first_minor = 0;
	disk->minors = 8;
	disk->fops = &wr_sd_fops;
	disk->private_data = &wr_sd;
	strscpy(disk->disk_name, "wrsd", DISK_NAME_LEN);
	set_capacity(disk, wr_sd.sectors);

	ret = add_disk(disk);
	if (ret)
		goto put_disk;

	pr_info("wrsd: %llu sectors, %s addressing\n",
		(unsigned long long)wr_sd.sectors,
		wr_sd.high_capacity ? "block" : "byte");
	return 0;

put_disk:
	put_disk(disk);
unregister:
	unregister_blkdev(wr_sd.major, "wrsd");
	return ret;
}
device_initcall(wr_sd_init);
