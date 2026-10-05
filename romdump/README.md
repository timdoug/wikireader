# romdump

A launcher application that copies the WikiReader's boot FLASH and the
S1C33E07's boot ROM to the card, for emulators that want the real contents.

It only reads.  The FLASH (a Pm25LV512 on the SPI bus, shared with the card)
sees READ DATA, READ STATUS and JEDEC ID and nothing else, so nothing can
erase or program it.  Each run writes under the first number no file in
`dumps/` uses yet, creating every file in FatFs's create-new mode, so one
card can go round several devices and no run replaces another's dump.

## Install

```sh
make -C romdump
cp -X romdump/romdump.app romdump/romdump.ico /Volumes/WRBOOT/
printf 'romdump.ico : romdump.app\n' >> /Volumes/WRBOOT/init.ini
```

Tap the chip icon in the launcher.  A run takes a few seconds and ends at
"press any button", which returns to the launcher.

## What it writes

| File | Contents |
| --- | --- |
| `dumps/infoNNN.txt` | what the screen showed: board revision, FLASH status and JEDEC ID, CRC-32s, serial number |
| `dumps/flashNNN.rom` | the 64 KiB FLASH, read three times and compared |
| `dumps/fhighNNN.bin` | the next 64 KiB, only if it does not mirror the first |
| `dumps/freadNNN.rom` | a disagreeing re-read, only if the three reads differ |
| `dumps/bootNNN.rom` | 0x20000-0x2ffff, the 64 KiB specific ROM in area 1 |
| `dumps/gateNNN.bin` | 0xc00000, the 4-word gate ROM holding the boot vectors |
| `dumps/bthNNN.rom` | the boot ROM again, read with halfword loads |
| `dumps/btbNNN.rom` | the boot ROM again, read with byte loads |
| `dumps/btgNNN.rom` | the boot ROM again, each word loaded by the ROM's own code at 0x20456 |
| `dumps/btmNNN.rom` | 0x30000-0x3ffff, above the boot ROM |
| `dumps/btdmaNNN.bin` | 0x2c000-0x2ffff read by an HSDMA transfer |
| `dumps/btcroNNN.rom` | the boot ROM read with pMISC_COROM's test bit set |
| `dumps/btcdmNNN.bin` | 0x2c000-0x2ffff by HSDMA with that bit set |

Everything after `gateNNN.bin` probes 0x2c000-0x2ffff, which plain word loads
read as zeros ([BOOTROM.md](BOOTROM.md)).  The report also times a load from
the area against one from 0x20000, and reads it with the boot register's
BOOT_ENA set.  The two Misc bits are put back before the run ends.

The FLASH dump and the report are on the card before the boot ROM is read:
the manual reserves that area for Epson's firmware, and a device that faults
or stops there has already saved everything else.

In wremu and MAME the FLASH dump equals the `flash.rom` they were given.  MAME
runs this dump as the boot ROM, so its boot dumps equal it; wremu has no boot
ROM and they read as zeros there.
