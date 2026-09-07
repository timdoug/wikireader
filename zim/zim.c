#include <grifo.h>

#include "wikilib.h"
#include "zim_bench.h"

int grifo_main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	debug_printf("starting ZIM reader\n");
	zim_bench_boot_begin();
	wikilib_run();
	return 1;
}
