// Containers/Fixups.cpp
//
// The whole container file is a single zlib stream. zlib1.dll ships with the
// client, so decompression costs nothing to implement.
//
// Layout of the decompressed image is the FILE form (not the in-memory form
// PackReader walks): sec2 = 0x18 + poolBytes, heap0 = sec2 + 0x2C,
// blob0 = heap0 + endptr. The fixup pair stream sits in the blob after the last
// allocation, which is why the loaded blob (objects only) does not contain it.
#include "Fixups.h"
#include "../Header.h"

#include <cstdlib>
#include <cstring>

namespace {

typedef int (__stdcall* UncompressFn)(BYTE*, ULONG*, const BYTE*, ULONG);

struct Ext { UINT32 slot; UINT32 target; };

Ext*   g_ext = nullptr;
UINT32 g_n   = 0;
const UINT32 EXT_CAP = 200000;

UncompressFn zlibUncompress()
{
    static UncompressFn fn = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        HMODULE m = GetModuleHandleA("zlib1.dll");
        if (!m) m = LoadLibraryA("zlib1.dll");
        if (m) fn = (UncompressFn)GetProcAddress(m, "uncompress");
        Log::write("Fixups: zlib1.dll %s", fn ? "ready" : "NOT AVAILABLE");
    }
    return fn;
}

int cmpExt(const void* a, const void* b)
{
    UINT32 x = ((const Ext*)a)->slot, y = ((const Ext*)b)->slot;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

// One reusable pair of buffers. Allocating and releasing tens of megabytes 304
// times over fragments a 32-bit address space badly.
BYTE*  g_src = nullptr;
BYTE*  g_dst = nullptr;
const UINT32 SRC_CAP = 16u << 20;
const UINT32 DST_CAP = 64u << 20;

bool buffers()
{
    if (!g_src) g_src = (BYTE*)VirtualAlloc(nullptr, SRC_CAP, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_dst) g_dst = (BYTE*)VirtualAlloc(nullptr, DST_CAP, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    return g_src && g_dst;
}

UINT32 readSlice(const char* path, UINT32 offset, UINT32 size)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD sz = GetFileSize(h, nullptr);
    if (size == 0) {
        if (offset > sz) { CloseHandle(h); return 0; }
        size = sz - offset;
    }
    if (size > SRC_CAP || offset > sz || size > sz - offset) { CloseHandle(h); return 0; }
    SetFilePointer(h, offset, nullptr, FILE_BEGIN);
    DWORD got = 0, rd = 0;
    while (got < size && ReadFile(h, g_src + got, size - got, &rd, nullptr) && rd) got += rd;
    CloseHandle(h);
    return got;
}

bool parse(UINT32 rawLen)
{
    UncompressFn unc = zlibUncompress();
    if (!unc || !rawLen) return false;
    ULONG len = DST_CAP;
    if (unc(g_dst, &len, g_src, rawLen) != 0) return false;

    UINT32 blob0 = 0, blobSize = 0;
    __try {
        UINT32* w = (UINT32*)g_dst;
        if (w[0] != 0 || w[1] != 8 || w[4] != 1 || w[6] != 8) return false;
        UINT32 sec2 = 0x18 + w[5];
        if (*(UINT32*)(g_dst + sec2) != 2) return false;
        blob0 = sec2 + 0x2C + *(UINT32*)(g_dst + sec2 + 4);
        if (blob0 >= len) return false;
        blobSize = len - blob0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }

    const UINT32* p = (const UINT32*)(g_dst + blob0);
    UINT32 words = blobSize / 4;
    // Pair streams can start on either 4-byte phase. Maps_ZoneContested6_inst,
    // for example, stores its music-project fixup at blob+0x12B6DC. Walking in
    // 8-byte steps from blob start silently skipped that entire phase.
    for (UINT32 i = 0; i + 1 < words && g_n < EXT_CAP; ++i) {
        UINT32 k = p[i], v = p[i + 1];
        if ((k & 7) != 4 || !v) continue;
        UINT32 slot = (k - 4) >> 1;
        if ((slot & 3) || slot >= blobSize) continue;
        g_ext[g_n].slot = slot;
        g_ext[g_n].target = v;
        ++g_n;
    }
    qsort(g_ext, g_n, sizeof(Ext), cmpExt);
    return g_n != 0;
}

} // namespace

namespace Fixups {

void clear() { g_n = 0; }
UINT32 count() { return g_n; }

bool load(const char* containerPath)
{
    g_n = 0;
    if (!buffers()) return false;
    if (!g_ext) {
        g_ext = (Ext*)VirtualAlloc(nullptr, sizeof(Ext) * EXT_CAP,
                                   MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!g_ext) return false;
    }
    return parse(readSlice(containerPath, 0, 0));
}

bool loadSlice(const char* archivePath, UINT32 offset, UINT32 size)
{
    g_n = 0;
    if (!buffers()) return false;
    if (!g_ext) {
        g_ext = (Ext*)VirtualAlloc(nullptr, sizeof(Ext) * EXT_CAP,
                                   MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!g_ext) return false;
    }
    return parse(readSlice(archivePath, offset, size));
}

UINT32 nextTarget(UINT32 slot, UINT32* cursor)
{
    if (!cursor) return 0;
    UINT32 at = *cursor;
    if (!at) {
        UINT32 lo = 0, hi = g_n;
        while (lo < hi) {
            UINT32 mid = (lo + hi) >> 1;
            if (g_ext[mid].slot < slot) lo = mid + 1; else hi = mid;
        }
        at = lo;
    }
    if (at >= g_n || g_ext[at].slot != slot) return 0;
    *cursor = at + 1;
    return g_ext[at].target;
}

} // namespace Fixups
