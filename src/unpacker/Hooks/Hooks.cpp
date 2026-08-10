#include "Hooks.h"
#include "MinHook.h"

namespace Hooks {

bool init() { return MH_Initialize() == MH_OK; }

bool create(void* target, void* detour, void** original)
{
    if (!target) return false;
    return MH_CreateHook(target, detour, original) == MH_OK &&
           MH_EnableHook(target) == MH_OK;
}

bool createExport(const char* mod, const char* fn, void* detour, void** original)
{
    HMODULE m = GetModuleHandleA(mod);
    if (!m) m = LoadLibraryA(mod);
    if (!m) return false;
    return create((void*)GetProcAddress(m, fn), detour, original);
}

} // namespace Hooks
