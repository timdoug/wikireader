/* Exercise the production reboot syscall after FLASH -> Grifo -> init. */
#include <grifo.h>

int grifo_main(int argc, char **argv)
{
    debug_print("WDT_RESET_APP\n");
    grifo_reboot();
}
