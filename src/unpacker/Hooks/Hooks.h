// Hooks/Hooks.h -- thin wrapper over MinHook so call sites stay readable.
#pragma once
#include <windows.h>

namespace Hooks {
bool init();
bool create(void* target, void* detour, void** original);
bool createExport(const char* mod, const char* fn, void* detour, void** original);
}
