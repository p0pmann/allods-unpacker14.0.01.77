/* pango.dll carrier: forward the real exports to pango_orig.dll and load the
 * unpacker before AOgame.exe reaches its entry point. */
#include <windows.h>
#include "pango_exports.h"   /* 397 /EXPORT forwarders -> pango_orig.dll */

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hInst);
        LoadLibraryA("AllodsUnpacker14.dll");
    }
    return TRUE;
}
