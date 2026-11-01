#include <grifo.h>

#include "wikilib.h"
#include "zim_startup.h"
#include "zim_bench.h"

#include <string.h>

int grifo_main(int argc, char **argv)
{
	/* "bench LABEL [NEXT.app NEXTLABEL]": wikilib_run() hands over to the
	 * benchmark once the reader is initialized (zim_bench.h). */
	if (argc > 1 && !strcmp(argv[1], "bench")) {
		zim_bench_args(argc, argv);
	}
	zim_startup_init();
	debug_printf("starting ZIM reader\n");
	wikilib_run();
	return 1;
}
