#include "main-fn.h"
#include "../utilxx/utilxx.h"

int main(int argc, char *argv[])
{
    utilxxPrintHello();
    utilxxPrint("Hello from MPV! %s --", "wow");
    return mpv_main(argc, argv);
}
