// Containers/PackReader.cpp
//
// pack.bin is one zlib stream on disk, but the client has already decompressed
// it into two allocations -- a HEAP and an object BLOB -- and fixed up every
// pointer, so the container is read in place with no file access at all.
//
// resmgr+0x84 is the VFS; its fields are {begin,end,cap} vectors:
//   +0x0C heap begin   +0x14 heap end   +0x20 blob begin (= object offset 0)
//
// The heap begins with what the on-disk container calls sec2+0x08, so the
// familiar header fields sit at fixed offsets from it and the heap proper
// (what the file format calls heap0) starts 0x24 bytes in:
//   +0x00 = 0x24 (constant)  +0x04 heapdir buckets  +0x08 P2  +0x0C typeCount
//   +0x10 P3 (id map A)      +0x14 buckets          +0x18 P4 (id map B)
//   +0x1C buckets            +0x20 build            +0x24 heap0
//
// Heap directory (heap0 + 0): `buckets` entries of {blockOff, count}; bucket j's
// block is at heap0 + blockOff + 8*j and holds `count` triples {a, recLen,
// blobOff} followed by the run's path records. Record i is at block + a + 12*i
// and is {u32 tag=1, u32 hash, char path[recLen-9], NUL}.
//
// Id map B (heap0 + P4): same bucket shape, one u32 of stream header first, and
// blocks of {blobOff, resourceId} pairs. That is where <Header> comes from.
#include "PackReader.h"
#include "../Header.h"

#include <cstdlib>

namespace {

bool    g_open     = false;
UINT32  g_fileBase = 0;
UINT32  g_blobBase = 0;

Pack::Res* g_res       = nullptr;
UINT32     g_resN      = 0;
char*      g_paths     = nullptr;
UINT32     g_pathsUsed = 0;

const UINT32 RES_CAP   = 700000;
const UINT32 PATH_CAP  = 64u * 1024 * 1024;

inline UINT32 rd(UINT32 a) { return *(UINT32*)a; }

void* alloc(size_t n)
{
    return VirtualAlloc(nullptr, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}

int cmpRes(const void* a, const void* b)
{
    UINT32 x = ((const Pack::Res*)a)->blobOff, y = ((const Pack::Res*)b)->blobOff;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

// Index of the entry with this exact blobOff, or -1.
int find(UINT32 blobOff)
{
    UINT32 lo = 0, hi = g_resN;
    while (lo < hi) {
        UINT32 mid = (lo + hi) >> 1;
        UINT32 k = g_res[mid].blobOff;
        if (k == blobOff) return (int)mid;
        if (k < blobOff) lo = mid + 1; else hi = mid;
    }
    return -1;
}

// ---- heap directory ------------------------------------------------------
void readHeapDir(UINT32 heap0, UINT32 heapEnd, UINT32 buckets)
{
    for (UINT32 entry = 0; entry < buckets; ++entry) {
        UINT32 blockOff = 0, cnt = 0;
        __try {
            blockOff = rd(heap0 + entry * 8);
            cnt      = rd(heap0 + entry * 8 + 4);
        } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
        if (!cnt) continue;                    // empty bucket, slot still counts
        UINT32 block = heap0 + blockOff + 8 * entry;
        if (block < heap0 || block >= heapEnd) continue;

        for (UINT32 i = 0; i < cnt && g_resN < RES_CAP; ++i) {
            __try {
                UINT32 a       = rd(block + i * 12);
                UINT32 recLen  = rd(block + i * 12 + 4);
                UINT32 blobOff = rd(block + i * 12 + 8);
                UINT32 rpos    = block + a + 12 * i;
                if (recLen < 10 || rd(rpos) != 1) continue;
                UINT32 plen = recLen - 9;
                if (plen > 500) continue;
                const char* src = (const char*)(rpos + 8);
                if (*(const unsigned char*)(rpos + 8 + plen) != 0) continue;
                if (g_pathsUsed + plen + 1 >= PATH_CAP) continue;

                char* dst = g_paths + g_pathsUsed;
                for (UINT32 k = 0; k < plen; ++k) {
                    char ch = src[k];
                    if ((unsigned char)ch < 0x20 || (unsigned char)ch >= 0x7F) { dst[0] = 0; break; }
                    dst[k] = (ch == '\\') ? '/' : ch;    // one path separator
                }
                if (!dst[0]) continue;
                dst[plen] = 0;

                Pack::Res& r = g_res[g_resN];
                r.blobOff    = blobOff;
                r.pathOff    = g_pathsUsed;
                r.resourceId = 0;
                g_pathsUsed += plen + 1;
                ++g_resN;
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }
}

// ---- id map B ------------------------------------------------------------
// Same bucket layout as the heap directory, one u32 of stream header ahead of
// the index, and 8-byte {blobOff, resourceId} pairs instead of path triples.
UINT32 readIdMap(UINT32 mapBase, UINT32 mapEnd, UINT32 buckets)
{
    UINT32 idx = mapBase + 4, got = 0;
    for (UINT32 entry = 0; entry < buckets; ++entry) {
        UINT32 blockOff = 0, cnt = 0;
        __try {
            blockOff = rd(idx + entry * 8);
            cnt      = rd(idx + entry * 8 + 4);
        } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
        if (!cnt) continue;
        UINT32 block = idx + blockOff + 8 * entry;
        if (block < idx || block >= mapEnd) continue;
        for (UINT32 i = 0; i < cnt; ++i) {
            __try {
                UINT32 key = rd(block + i * 8);
                UINT32 val = rd(block + i * 8 + 4);
                int at = find(key);
                if (at >= 0) { g_res[at].resourceId = val; ++got; }
            } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
        }
    }
    return got;
}

} // namespace

namespace Pack {

bool   isOpen()   { return g_open; }
UINT32 blobBase() { return g_blobBase; }
UINT32 count()    { return g_resN; }

const Res*  at(UINT32 i)       { return (i < g_resN) ? &g_res[i] : nullptr; }
const char* path(const Res& r) { return g_paths + r.pathOff; }

const char* pathForAddr(UINT32 objAddr)
{
    if (!g_open || objAddr < g_blobBase) return nullptr;
    int i = find(objAddr - g_blobBase);
    return (i >= 0) ? g_paths + g_res[i].pathOff : nullptr;
}

void close()
{
    g_open = false;
    g_resN = 0;
    g_pathsUsed = 0;
}

bool open()
{
    if (g_open) return true;

    BYTE* image = (BYTE*)GetModuleHandleA(nullptr);
    UINT32 vfs = 0, heapEnd = 0;
    __try {
        UINT32 resmgr = *(UINT32*)(image + (Off::G_RESMGR - Off::IMAGE_BASE));
        if (!resmgr) { Log::write("Pack: resource manager not constructed yet"); return false; }
        vfs = *(UINT32*)(resmgr + Off::RESMGR_VFS);
        if (!vfs) { Log::write("Pack: no VFS"); return false; }
        g_fileBase = *(UINT32*)(vfs + Off::VFS_HEAP_BEGIN);
        heapEnd    = *(UINT32*)(vfs + Off::VFS_HEAP_END);
        g_blobBase = *(UINT32*)(vfs + Off::VFS_BLOB_BEGIN);
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_fileBase = 0; }

    if (!g_fileBase || !g_blobBase || heapEnd <= g_fileBase) {
        Log::write("Pack: VFS vectors not populated (heap=%08X..%08X blob=%08X)",
                   g_fileBase, heapEnd, g_blobBase);
        return false;
    }

    UINT32 heap0 = 0, dirBuckets = 0, p4 = 0, p4Buckets = 0;
    __try {
        if (rd(g_fileBase) != 0x24) {
            Log::write("Pack: heap header marker is %08X, expected 24", rd(g_fileBase));
            return false;
        }
        dirBuckets = rd(g_fileBase + 0x04);
        p4         = rd(g_fileBase + 0x18);
        p4Buckets  = rd(g_fileBase + 0x1C);
        heap0      = g_fileBase + 0x24;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }

    if (!g_res)   g_res   = (Res*)alloc(sizeof(Res) * RES_CAP);
    if (!g_paths) g_paths = (char*)alloc(PATH_CAP);
    if (!g_res || !g_paths) { Log::write("Pack: out of memory"); return false; }
    g_resN = 0; g_pathsUsed = 0;

    readHeapDir(heap0, heapEnd, dirBuckets);
    if (!g_resN) { Log::write("Pack: heap directory yielded nothing"); return false; }
    qsort(g_res, g_resN, sizeof(Res), cmpRes);

    UINT32 ids = readIdMap(heap0 + p4, heapEnd, p4Buckets);

    Log::write("Pack: heap=%08X blob=%08X resources=%u ids=%u paths=%uK",
               g_fileBase, g_blobBase, g_resN, ids, g_pathsUsed / 1024);
    g_open = true;
    return true;
}

} // namespace Pack
