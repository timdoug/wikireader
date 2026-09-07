#include <grifo.h>

#include "wikilib.h"

int grifo_main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	debug_printf("starting ZIM reader\n");
	wikilib_run();
	return 1;
}
