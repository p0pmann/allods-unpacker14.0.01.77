// AllodsV14.cpp -- entry point for the Allods Online 14.0.01.77 unpacker.
//
// Architecture follows AllodsOnlineUnpackerV8: a DLL side-loaded into AOgame.exe
// that hooks the engine and drives the game's OWN serializer, rather than
// parsing pack.bin externally. The client is the only thing that knows every
// field's type, element stride, enum name and nesting, so letting it serialize
// makes the output correct by construction.
//
// Flow:
//   1. DllMain -> init() on a worker thread (loaded before the CRT runs).
//   2. Freeze::install(1) stops the client at the first app step, with the
//      clock spoofed. At that point the ENTIRE resource corpus is already mapped
//      and pointer-fixed-up -- no login, server or world load is required.
//   3. On a WRITE_NOW sentinel, EngineWriter::runAll walks the pack's own heap
//      directory and serializes every resource through the engine into the
//      configured output directory.
//   4. HrefWriter makes the engine emit resource hrefs too (its ref serializers
//      only implement the read path).
//
// Everything the export needs is read out of the running client: the resource
// list and ids from the pack container, the concrete types from RTTI, the
// dotted class names from the class registry. There is no offline stage.
#include "Header.h"
#include "Hooks/Hooks.h"

namespace {

char g_binDir[MAX_PATH];
char g_iniPath[MAX_PATH];
char g_outputDir[MAX_PATH];
int  g_freezeStep = 1;

void loadPaths()
{
    GetModuleFileNameA(nullptr, g_binDir, MAX_PATH);
    char* last = g_binDir;
    for (char* p = g_binDir; *p; ++p) if (*p == '\\') last = p;
    *last = 0;
    wsprintfA(g_iniPath, "%s\\AllodsUnpacker14.ini", g_binDir);
}

int cfg(const char* key, int def)
{
    return GetPrivateProfileIntA("unpacker", key, def, g_iniPath);
}

DWORD WINAPI worker(LPVOID)
{
    char sentinel[MAX_PATH], onlyMap[64] = { 0 }, scope[512] = { 0 };
    wsprintfA(sentinel, "%s\\WRITE_NOW", g_binDir);
    // OnlyMap=<substring>: mount just the databases whose file name matches.
    GetPrivateProfileStringA("unpacker", "OnlyMap", "", onlyMap, sizeof(onlyMap), g_iniPath);
    GetPrivateProfileStringA("unpacker", "Scope", "", scope, sizeof(scope), g_iniPath);

    // FreezeStep=0 means something else already froze the client (the launch
    // patch payload does, and two hooks on the same app-step slot would fight).
    if (g_freezeStep > 0) while (!Freeze::isFrozen()) Sleep(50);
    Log::write("worker: waiting for %s", sentinel);
    while (GetFileAttributesA(sentinel) == INVALID_FILE_ATTRIBUTES) Sleep(100);
    DeleteFileA(sentinel);          // a stale sentinel must not start a second run
    Sleep(200);

    HrefWriter::install();
    EngineWriter::runAll(g_outputDir, (UINT32)cfg("Skip", 0),
                         (UINT32)cfg("Limit", 0), onlyMap, scope);
    return 0;
}

DWORD WINAPI init(LPVOID)
{
    loadPaths();
    Log::init(g_binDir);
    Log::write("======== AllodsUnpacker14 attached (pid=%lu) ========", GetCurrentProcessId());

    GetPrivateProfileStringA("unpacker", "OutputDir", "", g_outputDir,
                             sizeof(g_outputDir), g_iniPath);
    if (!g_outputDir[0]) {
        Log::write("config error: OutputDir is required (%s)", g_iniPath);
        return 0;
    }

    if (!Hooks::init()) { Log::write("MinHook init FAILED"); return 0; }
    if (!Launch::install()) return 0;

    g_freezeStep = cfg("FreezeStep", 1);
    Log::write("config: FreezeStep=%d Extract=%d OutputDir=%s (%s)",
               g_freezeStep, cfg("Extract", 1), g_outputDir, g_iniPath);
    if (g_freezeStep > 0) Freeze::install(g_freezeStep);
    if (cfg("Extract", 1)) CloseHandle(CreateThread(nullptr, 0, worker, nullptr, 0, nullptr));
    return 0;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        CloseHandle(CreateThread(nullptr, 0, init, nullptr, 0, nullptr));
    }
    return TRUE;
}
