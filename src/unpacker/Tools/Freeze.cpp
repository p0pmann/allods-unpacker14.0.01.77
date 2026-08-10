// Tools/Freeze.cpp -- stop the client at the pre-render step, with the clock spoofed.
//
// The entire corpus is already mapped and pointer-fixed-up at the FIRST app step,
// long before any UI renders -- so this is where extraction happens. No login, no
// server, no world load is required.
//
// Two non-obvious requirements:
//  * run on the ENGINE's own thread: hook the app's per-frame step
//    (app vtable[+0xC]); a worker thread cannot safely drive the serializer.
//  * spoof QueryPerformanceCounter / GetTickCount to the values captured at the
//    freeze, or protect.dll's watchdog kills the frozen process.
#include "../Header.h"
#include "../Hooks/Hooks.h"

namespace {
volatile LONG g_frozen = 0;
LARGE_INTEGER g_qpc{};
DWORD         g_tick = 0;
int           g_target = 1;

BOOL (WINAPI* o_QPC)(LARGE_INTEGER*) = nullptr;
DWORD (WINAPI* o_GetTick)(void) = nullptr;
ULONGLONG (WINAPI* o_GetTick64)(void) = nullptr;
char (__fastcall* o_Step)(void*, void*, int) = nullptr;

BOOL WINAPI h_QPC(LARGE_INTEGER* p) { if (g_frozen && p) { *p = g_qpc; return TRUE; } return o_QPC(p); }
DWORD WINAPI h_GetTick() { return g_frozen ? g_tick : o_GetTick(); }
ULONGLONG WINAPI h_GetTick64() { return g_frozen ? (ULONGLONG)g_tick : o_GetTick64(); }

char __fastcall h_Step(void* thisp, void* edx, int a2)
{
    static int n = 0;
    if (!g_frozen && ++n >= g_target) {
        o_QPC(&g_qpc);
        g_tick = o_GetTick();
        InterlockedExchange(&g_frozen, 1);      // spoof only AFTER capturing
        Log::write("Freeze: main thread frozen at step %d (clock spoofed)", n);
        for (;;) Sleep(60000);                  // the extraction runs from here
    }
    return o_Step(thisp, edx, a2);
}
} // namespace

namespace Freeze {

bool isFrozen() { return g_frozen != 0; }

void spoofTimers()
{
    Hooks::createExport("kernel32.dll", "QueryPerformanceCounter", &h_QPC, (void**)&o_QPC);
    Hooks::createExport("kernel32.dll", "GetTickCount",   &h_GetTick,   (void**)&o_GetTick);
    Hooks::createExport("kernel32.dll", "GetTickCount64", &h_GetTick64, (void**)&o_GetTick64);
}

void install(int stepN)
{
    g_target = stepN;
    spoofTimers();
    // The app vtable lives in .rdata that protect.dll decrypts lazily -- poll
    // until the slot holds the real step function, or patching corrupts it.
    BYTE* base = (BYTE*)GetModuleHandleA(nullptr);
    UINT32* slot = (UINT32*)(base + (Off::APP_VTABLE - Off::IMAGE_BASE) + Off::APP_STEP_SLOT);
    UINT32 want = (UINT32)(base + (Off::FN_APP_STEP - Off::IMAGE_BASE));
    for (int i = 0; i < 120000; ++i) {
        __try {
            if (*slot == want) {
                o_Step = (char(__fastcall*)(void*, void*, int))(UINT_PTR)*slot;
                DWORD old;
                VirtualProtect(slot, 4, PAGE_READWRITE, &old);
                *slot = (UINT32)(UINT_PTR)&h_Step;
                VirtualProtect(slot, 4, old, &old);
                Log::write("Freeze: step hook installed");
                return;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        Sleep(1);
    }
    Log::write("Freeze: step slot never decrypted");
}

} // namespace Freeze
