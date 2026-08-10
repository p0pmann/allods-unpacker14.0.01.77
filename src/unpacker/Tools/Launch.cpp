// Tools/Launch.cpp -- let a clean client reach the pre-render extraction step.
#include "../Header.h"
#include "../Hooks/Hooks.h"

#include <cstring>

namespace {

struct Patch {
    DWORD rva;
    BYTE expected[6];
    BYTE replacement[6];
    UINT32 size;
};

Patch g_patches[] = {
    { 0x15BF2A, { 0x0F, 0x84, 0xBA, 0x01, 0x00, 0x00 },
                  { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 }, 6 },
    { 0x15BF32, { 0x0F, 0x84, 0xB2, 0x01, 0x00, 0x00 },
                  { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 }, 6 },
    { 0x8DA72E, { 0x75, 0x08 },
                  { 0xEB, 0x08 }, 2 },
    { 0x8DCA32, { 0x0F, 0x84, 0x81, 0x02, 0x00, 0x00 },
                  { 0xE9, 0x82, 0x02, 0x00, 0x00, 0x90 }, 6 },
};

DWORD (WINAPI* o_GetEnvironmentVariableW)(LPCWSTR, LPWSTR, DWORD) = nullptr;
volatile LONG g_patched = 0;

bool applyPatches()
{
    if (g_patched) return true;
    BYTE* base = (BYTE*)GetModuleHandleA(nullptr);

    for (UINT32 i = 0; i < sizeof(g_patches) / sizeof(g_patches[0]); ++i) {
        Patch& p = g_patches[i];
        BYTE* at = base + p.rva;
        bool expected = false, replaced = false;
        __try {
            expected = memcmp(at, p.expected, p.size) == 0;
            replaced = memcmp(at, p.replacement, p.size) == 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (!expected && !replaced) {
            Log::write("Launch: unexpected bytes at AOgame+0x%X", p.rva);
            return false;
        }
    }

    for (UINT32 i = 0; i < sizeof(g_patches) / sizeof(g_patches[0]); ++i) {
        Patch& p = g_patches[i];
        BYTE* at = base + p.rva;
        if (memcmp(at, p.replacement, p.size) == 0) continue;
        DWORD old = 0;
        if (!VirtualProtect(at, p.size, PAGE_EXECUTE_READWRITE, &old)) return false;
        memcpy(at, p.replacement, p.size);
        VirtualProtect(at, p.size, old, &old);
        FlushInstructionCache(GetCurrentProcess(), at, p.size);
    }

    InterlockedExchange(&g_patched, 1);
    Log::write("Launch: startup gates patched");
    return true;
}

DWORD WINAPI h_GetEnvironmentVariableW(LPCWSTR name, LPWSTR value, DWORD size)
{
    DWORD result = o_GetEnvironmentVariableW(name, value, size);
    if (!g_patched && name &&
        (lstrcmpiW(name, L"GC_TYPE_ID") == 0 || lstrcmpiW(name, L"SteamAppId") == 0)) {
        applyPatches();
    }
    return result;
}

} // namespace

namespace Launch {

bool install()
{
    SetEnvironmentVariableW(L"GC_TYPE_ID", L"359");
    SetEnvironmentVariableW(L"__GC_PROJECT_ID", L"0.359");
    SetEnvironmentVariableW(L"GC_PROJECT_ID", L"0.359");
    SetEnvironmentVariableW(L"SteamAppId", L"381640");

    bool ok = Hooks::createExport("kernel32.dll", "GetEnvironmentVariableW",
                                  &h_GetEnvironmentVariableW,
                                  (void**)&o_GetEnvironmentVariableW);
    Log::write("Launch: environment hook %s", ok ? "installed" : "FAILED");
    return ok;
}

} // namespace Launch
