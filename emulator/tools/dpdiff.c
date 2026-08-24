/* Differential runner for the two linked S1C33 libgcc dp-bit variants. */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/c33.h"
#include "../src/mem.h"

#define STACK    (SDRAM_BASE + SDRAM_SIZE - 0x1000u)
#define SENTINEL 0x1003fff0u
#define DP       0x10040000u
#define LIMIT    200000u

struct machine { struct mem mem; struct c33 cpu; };

static bool load_machine(struct machine *m, const char *path)
{
	char err[256];
	if (!mem_init(&m->mem)) return false;
	m->mem.log = stderr;
	if (!elf_load(&m->mem, path, err, sizeof err)) {
		fprintf(stderr, "%s: %s\n", path, err); return false;
	}
	m->cpu.bus = (struct c33_bus){ mem_read, mem_write, &m->mem };
	m->mem.pc_src = &m->cpu.pc;
	return true;
}

static bool call(struct machine *m, uint32_t fn, uint32_t r6, uint32_t r7,
		 uint32_t r8, uint32_t r9, uint64_t *result, uint64_t *steps)
{
	c33_reset(&m->cpu, fn);
	m->cpu.sr[SR_SP] = STACK;
	m->cpu.r[15] = DP;
	m->cpu.r[6] = r6; m->cpu.r[7] = r7;
	m->cpu.r[8] = r8; m->cpu.r[9] = r9;
	mem_write(&m->mem, STACK, 4, SENTINEL);
	while (!m->cpu.halted && !m->cpu.fault && m->cpu.pc != SENTINEL &&
	       m->cpu.cycles < LIMIT)
		c33_step(&m->cpu);
	*steps += m->cpu.cycles;
	if (m->cpu.fault || m->cpu.pc != SENTINEL) {
		fprintf(stderr, "call failed fn=%08x pc=%08x cycles=%" PRIu64 " fault=%s\n",
			fn, m->cpu.pc, m->cpu.cycles,
			m->cpu.fault ? m->cpu.fault : "timeout");
		return false;
	}
	*result = ((uint64_t)m->cpu.r[5] << 32) | m->cpu.r[4];
	return true;
}

static void trace_call(struct machine *m, const char *tag, uint32_t fn,
		       uint64_t x, uint64_t y)
{
	char dis[128];
	c33_reset(&m->cpu, fn);
	m->cpu.sr[SR_SP] = STACK;
	m->cpu.r[15] = DP;
	m->cpu.r[6] = (uint32_t)x; m->cpu.r[7] = x >> 32;
	m->cpu.r[8] = (uint32_t)y; m->cpu.r[9] = y >> 32;
	mem_write(&m->mem, STACK, 4, SENTINEL);
	while (!m->cpu.halted && !m->cpu.fault && m->cpu.pc != SENTINEL &&
	       m->cpu.cycles < LIMIT) {
		c33_disasm(&m->cpu, m->cpu.pc, dis, sizeof dis);
		printf("%s %05" PRIu64 " pc=%08x psr=%02x "
		       "r4=%08x r5=%08x r6=%08x r7=%08x r8=%08x r9=%08x "
		       "r10=%08x r11=%08x r12=%08x r13=%08x  %s\n",
		       tag, m->cpu.cycles, m->cpu.pc, m->cpu.sr[SR_PSR],
		       m->cpu.r[4], m->cpu.r[5], m->cpu.r[6], m->cpu.r[7],
		       m->cpu.r[8], m->cpu.r[9], m->cpu.r[10], m->cpu.r[11],
		       m->cpu.r[12], m->cpu.r[13], dis);
		c33_step(&m->cpu);
	}
}

static size_t corpus(uint64_t *v, size_t cap)
{
	static const uint64_t fixed[] = {
		0x0000000000000000ULL, 0x8000000000000000ULL,
		0x0000000000000001ULL, 0x8000000000000001ULL,
		0x0000000000000002ULL, 0x8000000000000002ULL,
		0x0000000000000003ULL, 0x8000000000000003ULL,
		0x00000000ffffffffULL, 0x80000000ffffffffULL,
		0x0007ffffffffffffULL, 0x8007ffffffffffffULL,
		0x000ffffffffffffeULL, 0x000fffffffffffffULL,
		0x800ffffffffffffeULL, 0x800fffffffffffffULL,
		0x0010000000000000ULL, 0x8010000000000000ULL,
		0x0010000000000001ULL, 0x8010000000000001ULL,
		0x001fffffffffffffULL, 0x801fffffffffffffULL,
		0x3c90000000000000ULL, 0x3ca0000000000000ULL,
		0x3e70000000000000ULL, 0x3e80000000000000ULL,
		0x3fd0000000000000ULL, 0x3fdfffffffffffffULL,
		0x3fe0000000000000ULL, 0x3fe0000000000001ULL,
		0x3fefffffffffffffULL, 0x3ff0000000000000ULL,
		0x3ff0000000000001ULL, 0x3ff00000ffffffffULL,
		0x3ff0000100000000ULL, 0x3ff7ffffffffffffULL,
		0x3ff8000000000000ULL, 0x3fffffffffffffffULL,
		0x4000000000000000ULL, 0x4000000000000001ULL,
		0x400fffffffffffffULL, 0x4010000000000000ULL,
		0x41dfffffffc00000ULL, 0x41e0000000000000ULL,
		0x43dfffffffffffffULL, 0x43e0000000000000ULL,
		0x7ca0000000000000ULL, 0x7fd0000000000000ULL,
		0x7fe0000000000000ULL, 0x7feffffffffffffeULL,
		0x7fefffffffffffffULL, 0xffefffffffffffffULL,
		0x7ff0000000000000ULL, 0xfff0000000000000ULL,
		0x7ff0000000000001ULL, 0xfff0000000000001ULL,
		0x7ff7ffffffffffffULL, 0xfff7ffffffffffffULL,
		0x7ff8000000000000ULL, 0xfff8000000000000ULL,
		0x7ff8000000000001ULL, 0xfff8000000000001ULL,
		0x7fffffffffffffffULL, 0xffffffffffffffffULL
	};
	size_t n = sizeof fixed / sizeof fixed[0];
	if (n > cap) n = cap;
	memcpy(v, fixed, n * sizeof *v);
	return n;
}

static unsigned fpclass(uint64_t x)
{
	uint64_t e=(x>>52)&0x7ff, f=x&0xfffffffffffffULL;
	if (!e) return f ? 1 : 0;       /* zero, subnormal */
	if (e==0x7ff) return f ? 4 : 3; /* infinity, NaN */
	return 2;                       /* normal */
}

static uint64_t host_op(const char *name, uint64_t a, uint64_t b)
{
	double x,y,z; uint64_t out;
	memcpy(&x,&a,8); memcpy(&y,&b,8);
	if (!strcmp(name,"add")) z=x+y;
	else if (!strcmp(name,"sub")) z=x-y;
	else if (!strcmp(name,"mul")) z=x*y;
	else z=x/y;
	memcpy(&out,&z,8); return out;
}

struct funcs {
	uint32_t add, sub, mul, div, cmp, eq, ne, gt, ge, lt, le, unord;
	uint32_t floatsi, floatunsi, fixsi;
};

static int test_binary(struct machine *a, struct machine *b, const char *name,
		uint32_t fa, uint32_t fb, const uint64_t *v, size_t n,
		bool result32, uint64_t *calls, uint64_t *steps)
{
	uint64_t mismatches = 0;
	uint64_t matrix[5][5]={{0}}, vm_host=0, ref_host=0, finite=0, host_examples=0,
		 vm_host_examples=0;
	for (size_t i = 0; i < n; i++) for (size_t j = 0; j < n; j++) {
		uint64_t ra, rb;
		if (!call(a, fa, (uint32_t)v[i], v[i]>>32, (uint32_t)v[j], v[j]>>32, &ra, steps) ||
		    !call(b, fb, (uint32_t)v[i], v[i]>>32, (uint32_t)v[j], v[j]>>32, &rb, steps))
			return -1;
		(*calls)++;
		if (result32) { ra = (uint32_t)ra; rb = (uint32_t)rb; }
		if (!result32 && fpclass(v[i]) < 3 && fpclass(v[j]) < 3) {
			uint64_t h=host_op(name,v[i],v[j]); finite++;
			if (ra!=h) vm_host++;
			if (rb!=h) ref_host++;
			if ((ra != h || rb != h) && host_examples++ < 12)
				printf("HOSTDIFF %-7s a=%016" PRIx64 " b=%016" PRIx64
				       " vm=%016" PRIx64 " ref=%016" PRIx64
				       " host=%016" PRIx64 "\n", name, v[i], v[j], ra, rb, h);
			if (ra != h && vm_host_examples++ < 12)
				printf("VMHOST   %-7s a=%016" PRIx64 " b=%016" PRIx64
				       " vm=%016" PRIx64 " host=%016" PRIx64 "\n",
				       name, v[i], v[j], ra, h);
		}
		if (ra != rb) {
			matrix[fpclass(v[i])][fpclass(v[j])]++;
			if (mismatches < 12)
				printf("MISMATCH %-7s a=%016" PRIx64 " b=%016" PRIx64
				       " vm=%016" PRIx64 " ref=%016" PRIx64 "\n",
				       name, v[i], v[j], ra, rb);
			mismatches++;
		}
	}
	printf("%-8s %6zu cases, %" PRIu64 " mismatches\n", name, n*n, mismatches);
	if (!result32)
		printf("         finite-vs-host: VM=%" PRIu64 " REF=%" PRIu64 " of %" PRIu64 "\n",
		       vm_host,ref_host,finite);
	if (mismatches) {
		static const char *cn[]={"zero","subnormal","normal","infinity","nan"};
		for (unsigned i=0;i<5;i++) for(unsigned j=0;j<5;j++) if(matrix[i][j])
			printf("         %-9s / %-9s : %" PRIu64 "\n",cn[i],cn[j],matrix[i][j]);
	}
	return mismatches ? 1 : 0;
}

static void probe_div(struct machine *a, struct machine *b, uint32_t fa, uint32_t fb,
		      uint64_t x, uint64_t y, uint64_t *steps)
{
	uint64_t ra,rb,h=host_op("div",x,y);
	if (!call(a,fa,(uint32_t)x,x>>32,(uint32_t)y,y>>32,&ra,steps) ||
	    !call(b,fb,(uint32_t)x,x>>32,(uint32_t)y,y>>32,&rb,steps)) exit(2);
	printf("PROBE div a=%016" PRIx64 " b=%016" PRIx64
	       " vm=%016" PRIx64 " ref=%016" PRIx64 " host=%016" PRIx64 "\n",
	       x,y,ra,rb,h);
}

int main(int argc, char **argv)
{
	if (argc != 3) { fprintf(stderr, "usage: %s VM.elf REF.elf\n", argv[0]); return 2; }
	struct machine vm, ref;
	if (!load_machine(&vm, argv[1]) || !load_machine(&ref, argv[2])) return 2;
	/* VM default-layout offsets and ROOT wiki.app.old offsets. */
	const struct funcs fvm = {
		0x10054d28,0x10054d66,0x10054daa,0x100550e4,0x10055410,
		0x10055442,0x10055492,0x100554e2,0x10055532,0x10055582,
		0x100555d2,0x10055622,0x1005566a,0x100556e6,0x1005578e };
	const struct funcs fr = {
		0x10054d60,0x10054d9e,0x10054de2,0x10055144,0x1005546c,
		0x1005549e,0x100554ee,0x1005553e,0x1005558e,0x100555de,
		0x1005562e,0x1005567e,0x100556c6,0x1005575a,0x1005581a };
	const char *trace = getenv("DPDIFF_TRACE");
	if (trace) {
		if (!strcmp(trace, "vm")) trace_call(&vm, "VM", fvm.div,
			0x3ff0000000000000ULL, 0x3ff0000000000000ULL);
		else trace_call(&ref, "REF", fr.div,
			0x3ff0000000000000ULL, 0x3ff0000000000000ULL);
		return 0;
	}
	uint64_t v[128], calls = 0, steps = 0;
	size_t n = corpus(v, 128);
	int bad = 0;
	probe_div(&vm,&ref,fvm.div,fr.div,0x3ff0000000000000ULL,0x3ff0000000000000ULL,&steps);
	probe_div(&vm,&ref,fvm.div,fr.div,0x4000000000000000ULL,0x3ff0000000000000ULL,&steps);
	probe_div(&vm,&ref,fvm.div,fr.div,0x3ff0000000000000ULL,0x4000000000000000ULL,&steps);
	probe_div(&vm,&ref,fvm.div,fr.div,0x3ff8000000000000ULL,0x3ff0000000000000ULL,&steps);
	probe_div(&vm,&ref,fvm.div,fr.div,0x0010000000000000ULL,0x3ff0000000000000ULL,&steps);
	probe_div(&vm,&ref,fvm.div,fr.div,0x0000000000000001ULL,0x0000000000000001ULL,&steps);
	bad |= test_binary(&vm,&ref,"add",fvm.add,fr.add,v,n,false,&calls,&steps);
	bad |= test_binary(&vm,&ref,"sub",fvm.sub,fr.sub,v,n,false,&calls,&steps);
	bad |= test_binary(&vm,&ref,"mul",fvm.mul,fr.mul,v,n,false,&calls,&steps);
	bad |= test_binary(&vm,&ref,"div",fvm.div,fr.div,v,n,false,&calls,&steps);
	bad |= test_binary(&vm,&ref,"cmp",fvm.cmp,fr.cmp,v,n,true,&calls,&steps);
	bad |= test_binary(&vm,&ref,"eq",fvm.eq,fr.eq,v,n,true,&calls,&steps);
	bad |= test_binary(&vm,&ref,"ne",fvm.ne,fr.ne,v,n,true,&calls,&steps);
	bad |= test_binary(&vm,&ref,"gt",fvm.gt,fr.gt,v,n,true,&calls,&steps);
	bad |= test_binary(&vm,&ref,"ge",fvm.ge,fr.ge,v,n,true,&calls,&steps);
	bad |= test_binary(&vm,&ref,"lt",fvm.lt,fr.lt,v,n,true,&calls,&steps);
	bad |= test_binary(&vm,&ref,"le",fvm.le,fr.le,v,n,true,&calls,&steps);
	bad |= test_binary(&vm,&ref,"unord",fvm.unord,fr.unord,v,n,true,&calls,&steps);
	printf("total paired calls=%" PRIu64 " target instructions=%" PRIu64 "\n", calls, steps);
	mem_free(&vm.mem); mem_free(&ref.mem);
	return bad ? 1 : 0;
}
