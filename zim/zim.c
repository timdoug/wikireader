#include <grifo.h>

#include "wikilib.h"
#include "zim_startup.h"

int grifo_main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	zim_startup_init();
	debug_printf("starting ZIM reader\n");
	wikilib_run();
	return 1;
}
