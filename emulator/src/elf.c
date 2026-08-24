/*
 * Minimal ELF32 little-endian loader for c33 images.
 *
 * grifo.elf, wiki.app and init.app are all statically linked ELF32 LSB;
 * we only need PT_LOAD segments and the entry point.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mem.h"

#define EM_C33_EXPECTED 0   /* the Epson port's e_machine is non-standard */

struct ehdr32 {
	uint8_t  ident[16];
	uint16_t type, machine;
	uint32_t version, entry, phoff, shoff, flags;
	uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct phdr32 {
	uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align;
};

#define PT_LOAD 1

uint32_t elf_load(struct mem *m, const char *path, char *err, size_t errlen)
{
	FILE *fp = fopen(path, "rb");
	if (!fp) {
		snprintf(err, errlen, "cannot open %s", path);
		return 0;
	}

	struct ehdr32 eh;
	if (fread(&eh, sizeof eh, 1, fp) != 1) {
		snprintf(err, errlen, "short read on ELF header");
		fclose(fp);
		return 0;
	}
	if (memcmp(eh.ident, "\177ELF", 4) != 0 || eh.ident[4] != 1 ||
	    eh.ident[5] != 1) {
		snprintf(err, errlen, "not an ELF32 little-endian image");
		fclose(fp);
		return 0;
	}

	unsigned loaded = 0;
	for (unsigned i = 0; i < eh.phnum; i++) {
		struct phdr32 ph;
		if (fseek(fp, eh.phoff + (long)i * eh.phentsize, SEEK_SET) != 0 ||
		    fread(&ph, sizeof ph, 1, fp) != 1) {
			snprintf(err, errlen, "bad program header %u", i);
			fclose(fp);
			return 0;
		}
		if (ph.type != PT_LOAD || ph.memsz == 0)
			continue;

		uint8_t *buf = calloc(1, ph.memsz);
		if (!buf) {
			snprintf(err, errlen, "out of memory for segment %u", i);
			fclose(fp);
			return 0;
		}
		if (ph.filesz) {
			if (fseek(fp, ph.offset, SEEK_SET) != 0 ||
			    fread(buf, 1, ph.filesz, fp) != ph.filesz) {
				snprintf(err, errlen, "short read on segment %u", i);
				free(buf);
				fclose(fp);
				return 0;
			}
		}
		for (uint32_t k = 0; k < ph.memsz; k++)
			mem_write(m, ph.vaddr + k, 1, buf[k]);

		fprintf(stderr, "  segment %u: vaddr 0x%08x  filesz %u  memsz %u\n",
			i, ph.vaddr, ph.filesz, ph.memsz);
		free(buf);
		loaded++;
	}

	fclose(fp);
	if (!loaded) {
		snprintf(err, errlen, "no PT_LOAD segments");
		return 0;
	}
	return eh.entry;
}
