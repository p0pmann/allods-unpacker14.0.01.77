// Header.h -- shared declarations for AllodsUnpacker14
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "Offsets.h"

// ---- engine calling conventions -------------------------------------------
// The engine is __thiscall. Emulate via __fastcall: arg1 -> ECX (this),
// arg2 -> EDX (ignored), remaining args on the stack. Passing a real argument
// in the EDX slot is a classic way to fault here.
// The parameter count must match the callee exactly: a __thiscall callee pops
// its own stack args, so passing one too many silently unbalances the CALLER's
// frame and corrupts its locals a few iterations later.
typedef void  (__fastcall* ThisCall0)(void*, void*);
typedef void  (__fastcall* ThisCall1)(void*, void*, void*);
typedef void  (__fastcall* ThisCallI1)(void*, void*, int);
typedef int   (__fastcall* ThisCallI2)(void*, void*, void*, int);
typedef void  (__fastcall* ThisCallRel)(void*, void*, int, int);
typedef void* (__cdecl*    MakeArchiveFn)(void*, int, void*);
typedef int   (__cdecl*    NormalizeFn)(unsigned*, void*);
typedef int   (__cdecl*    RefSerFn)(int, int, int, unsigned*);

// The engine's string is a {begin, end, cap} triple, NOT a NUL-terminated char*.
// `end` points at the last character, not the terminator.
struct GameStr {
    char* b;
    char* e;
    char* c;
    void set(char* p, size_t n) { b = p; e = p + n; c = e; }
};

namespace Log {
void init(const char* dir);
void write(const char* fmt, ...);
}

namespace Freeze {
bool isFrozen();
void install(int stepN);          // freeze the main thread at app-step N
void spoofTimers();               // QPC/GetTickCount must be frozen too or the
}                                 // protector's watchdog kills the process

namespace Fs {
bool setRoot(const char* dir);                    // output root, created if absent
bool write(const char* relPath, const void* data, UINT32 n);
bool fullPath(const char* relPath, char* out, UINT32 cch);
}

namespace Sink {
bool        init();
void        begin();
const char* data();               // the rendered document, not NUL-terminated
UINT32      size();
void*       object();             // the fake source object handed to the archive
}

namespace Fixups {
bool   load(const char* containerPath);
UINT32 targetFor(UINT32 slotBlobOffset);
UINT32 count();
}

namespace HrefMap {
bool        addCurrentContainer();                // accumulates across databases
const char* lookup(UINT32 objAddr);               // "path#xpointer(/FQN)"
const char* lookupFrom(UINT32 objAddr, const char* curPath);  // .txt goes relative
// Resolve a reference the loader left null, from the container's fixup stream.
const char* lookupExternal(UINT32 slotAddr, const char* curPath);
void        setContainerBase(UINT32 blobBase, bool isPack);
UINT32      count();
}

namespace SlotMap {
bool init();
int  slotFor(UINT32 vtable);      // serializer slot, clamped to VTABLE_MAX_SLOT
}

namespace EngineWriter {
// Serialize every resource of every database through the engine into outDir.
void        runAll(const char* outDir, UINT32 limit, const char* onlyMap);
const char* currentPath();        // resource being written, for relative hrefs
}

namespace MapLoader {
// Per-map content lives in Maps_*.bin containers the client only mounts on
// world entry. These mount one directly, swapping it in on the resource manager.
bool databaseDir(char* out, UINT32 cch);   // "<client>\data\Bin\"
bool load(const char* absPath);
}

namespace HrefWriter {
// The ref serializers implement ONLY the read path -- on write they open and
// close the scope and emit nothing. Hooking them and supplying the path from
// HrefMap makes the engine write the attribute itself.
void install();
UINT32 written();
UINT32 faults();
UINT32 unresolved();      // refs written as href="" because the target is unknown
UINT32 external();        // refs recovered from the container's fixup stream
}
