// Unpack/HrefWriter.cpp -- make the ENGINE write resource hrefs.
//
// sub_57B4B0 / sub_5779D0 serialize a resource reference, but their entire body
// sits inside `if (archive->isReading)`. On write they open a scope, close it,
// and emit nothing -- which is why every <itemClass/> came out empty.
//
// The engine CAN write the attribute: archive->vtable[164] (sub_5AA160) branches
// on archive+0x68 and has a full write path; it is exactly what sub_5BB020 uses
// to emit the <Name href="..."/> that always worked. The only thing missing is
// the PATH, because the engine resolves an href by reverse-mapping the target
// object to a resource record and only ~64 records exist at the freeze.
//
// So we hook the two serializers, supply the path from HrefMap, and let the
// ENGINE write it. Output stays fully engine-authored.
#include "../Header.h"
#include "../Hooks/Hooks.h"

namespace {

RefSerFn g_origA = nullptr;
RefSerFn g_origB = nullptr;
volatile LONG g_written    = 0;
volatile LONG g_faults     = 0;
volatile LONG g_unresolved = 0;
volatile LONG g_unresolvedNonNull = 0;
volatile LONG g_external = 0;

int refWrite(int arc, int name, int flags, unsigned* objp, RefSerFn orig)
{
    // Every dereference below is on engine-owned memory. An unguarded fault here
    // kills the whole process -- that is what stopped an early full run at 15.6k
    // resources.
    unsigned char reading = 1;
    UINT32 vt = 0, tgt = 0;
    __try {
        if ((UINT32)arc < 0x10000) return 0;
        reading = *(unsigned char*)(arc + Off::ARC_IS_READ_BYTE);
        vt  = *(UINT32*)arc;
        tgt = objp ? *objp : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&g_faults);
        return 0;
    }

    if (reading) return orig(arc, name, flags, objp);   // read path unchanged
    if (!vt) return 0;

    // A reference we cannot resolve must STILL be written, as href="". Returning
    // early here deleted the whole element, so a null or foreign target made the
    // field vanish from the document entirely -- which is how <bumpTexture />,
    // <shadowSettings /> and the material textures went missing.
    const char* href = nullptr;
    __try {
        if (tgt)
            href = HrefMap::lookupFrom(tgt, EngineWriter::currentPath());
        if (!href && objp) {
            // A foreign target may be null, or the resource manager may have
            // rebound it to a runtime object outside both container blobs. In
            // either case the on-disk fixup stream is authoritative and is
            // keyed by the address of this very pointer field.
            href = HrefMap::lookupExternal((UINT32)(UINT_PTR)objp, EngineWriter::currentPath());
            if (href) InterlockedIncrement(&g_external);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&g_faults); href = nullptr; }
    if (!href) {
        InterlockedIncrement(&g_unresolved);
        href = "";
    }

    int ok = 0;
    __try {
        ok = ((ThisCallI2)(*(UINT32*)(vt + Off::ARC_BEGIN_SCOPE)))(
                 (void*)arc, nullptr, (void*)(INT_PTR)name, flags);
    } __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&g_faults); return 0; }
    if (!ok) return 0;

    // beginScope succeeded, so endScope MUST run whatever happens next.
    __try {
        // vtable[164]: __thiscall(archive, const char* attrName, GameStr* value)
        typedef void (__fastcall* AttrFn)(void*, void*, const char*, void*);
        static char pb[1024];
        int n = lstrlenA(href);
        if (n > 1000) n = 1000;
        memcpy(pb, href, n);
        pb[n] = 0;                       // the engine prepends the leading '/'
        GameStr gs; gs.set(pb, n);
        ((AttrFn)(*(UINT32*)(vt + Off::ARC_ATTR)))(
            (void*)arc, nullptr, (const char*)Off::STR_HREF /* "href" */, &gs);
        InterlockedIncrement(&g_written);
    } __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&g_faults); }

    __try {
        ((ThisCall0)(*(UINT32*)(vt + Off::ARC_END_SCOPE)))((void*)arc, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&g_faults); }
    return ok;
}

int __cdecl hookA(int a, int b, int c, unsigned* d) { return refWrite(a, b, c, d, g_origA); }
int __cdecl hookB(int a, int b, int c, unsigned* d) { return refWrite(a, b, c, d, g_origB); }

} // namespace

namespace HrefWriter {

void install()
{
    BYTE* base = (BYTE*)GetModuleHandleA(nullptr);
    void* ta = base + (Off::SER_REF  - Off::IMAGE_BASE);
    void* tb = base + (Off::SER_REF2 - Off::IMAGE_BASE);
    Log::write("HrefWriter: %s sub_57B4B0", Hooks::create(ta, &hookA, (void**)&g_origA) ? "OK" : "FAIL");
    Log::write("HrefWriter: %s sub_5779D0", Hooks::create(tb, &hookB, (void**)&g_origB) ? "OK" : "FAIL");
}

UINT32 written()    { return (UINT32)g_written;    }
UINT32 faults()     { return (UINT32)g_faults;     }
UINT32 unresolved() { return (UINT32)g_unresolved; }
UINT32 external()   { return (UINT32)g_external;   }

} // namespace HrefWriter
