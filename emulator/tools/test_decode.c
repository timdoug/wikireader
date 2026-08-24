/*
 * Validate the generated decode tables from C.
 *
 * Reads "<hex16> <mnemonic>" lines (as produced from objdump) on stdin,
 * decodes each encoding through c33_forms.h, and reports any disagreement.
 * This exercises the exact tables the emulator will use.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../c33_forms.h"

/* Extract operand field n from an encoding. */
static int32_t field(uint16_t insn, const struct c33_field *f)
{
	int32_t v = (insn >> f->shift) & ((1u << f->width) - 1);
	if (f->is_signed && (v >> (f->width - 1)))
		v -= (int32_t)1 << f->width;
	return v;
}

int main(void)
{
	char line[256];
	unsigned long total = 0, ok = 0, bad = 0, invalid = 0;
	char first_bad[256] = "";

	while (fgets(line, sizeof line, stdin)) {
		unsigned insn;
		char want[64];

		if (sscanf(line, "%x %63s", &insn, want) != 2)
			continue;
		total++;

		const struct c33_form *form = &c33_forms[c33_form_of[insn & 0xffff]];
		const char *got = c33_op_name[form->op];

		if (form->op == OP_INVALID) {
			invalid++;
			continue;
		}
		if (strcmp(got, want) != 0) {
			if (!first_bad[0])
				snprintf(first_bad, sizeof first_bad,
					 "%04x: got %s want %s", insn, got, want);
			bad++;
			continue;
		}
		ok++;
	}

	printf("decoded        : %lu\n", total);
	printf("mnemonic match : %lu (%.2f%%)\n",
	       ok, total ? 100.0 * ok / total : 0.0);
	printf("invalid        : %lu\n", invalid);
	printf("mismatched     : %lu\n", bad);
	if (first_bad[0])
		printf("first mismatch : %s\n", first_bad);

	/* Spot-check operand extraction on known encodings. */
	struct { uint16_t insn; const char *desc; } probe[] = {
		{ 0xc23f, "ext 0x23f"      },
		{ 0x2300, "srl %r0,0x10"   },
		{ 0x2b14, "sra %r4,0x11"   },
		{ 0x88b7, "srl %r7,0xb"    },
		{ 0x4e1f, "ld.uh %r15,[%sp+0x21]" },
	};
	printf("\noperand extraction spot-check:\n");
	for (unsigned i = 0; i < sizeof probe / sizeof probe[0]; i++) {
		const struct c33_form *f =
			&c33_forms[c33_form_of[probe[i].insn]];
		printf("  %04x  %-8s %-14s ", probe[i].insn,
		       c33_op_name[f->op], f->shape);
		for (unsigned k = 0; k < f->nfields; k++)
			printf("%s%ld", k ? ", " : "",
			       (long)field(probe[i].insn, &f->f[k]));
		printf("   (expect %s)\n", probe[i].desc);
	}

	return (bad || total == 0) ? 1 : 0;
}
