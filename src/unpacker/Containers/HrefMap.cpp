// Containers/HrefMap.cpp -- object address -> the href the engine should write.
//
// The engine resolves an href by reverse-mapping the target object back to a
// resource RECORD, and at the pre-render freeze only ~64 records exist -- which
// is why every <itemClass/> came out empty. The container's heap directory has
// the same information for all 486k resources, so we supply it.
//
// Form (from the 7.0 oracle): "<path>#xpointer(/<target root>)", except .txt
// targets, which are plain path refs and become a bare file name when they sit
// in the same directory as the referring resource.
//
// Two blocks, not one accumulating array. A map database's objects reference
// pack.bin resources and their own, never another map's -- and the pack stays
// mapped after the swap. So the pack is kept as an immutable BASE block and each
// mounted database replaces the small CURRENT block. That keeps lookup at two
// binary searches and avoids re-sorting half a million entries per map.
#include "../Header.h"
#include "PackReader.h"
#include "Types.h"

#include <cstdlib>
#include <cstring>

namespace {

struct Ent { UINT32 addr; UINT32 hrefOff; };

struct Block {
    Ent*   ent;
    UINT32 n;
    char*  blob;
    UINT32 used;
    UINT32 entCap;
    UINT32 blobCap;
};

Block g_base = {};
Block g_cur  = {};

int cmpEnt(const void* a, const void* b)
{
    UINT32 x = ((const Ent*)a)->addr, y = ((const Ent*)b)->addr;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

bool alloc(Block& b, UINT32 entCap, UINT32 blobCap)
{
    b.ent  = (Ent*)VirtualAlloc(nullptr, sizeof(Ent) * entCap, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    b.blob = (char*)VirtualAlloc(nullptr, blobCap, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    b.entCap = entCap;
    b.blobCap = blobCap;
    b.n = 0;
    b.used = 1;
    if (b.ent && b.blob) { b.blob[0] = 0; return true; }
    Log::write("HrefMap: out of memory");
    return false;
}

const char* find(const Block& b, UINT32 addr)
{
    UINT32 lo = 0, hi = b.n;
    while (lo < hi) {
        UINT32 mid = (lo + hi) >> 1;
        UINT32 k = b.ent[mid].addr;
        if (k == addr) return b.blob + b.ent[mid].hrefOff;
        if (k < addr) lo = mid + 1; else hi = mid;
    }
    return nullptr;
}

bool endsWithTxt(const char* p, size_t n)
{
    return n > 4 && p[n - 4] == '.' &&
           (p[n - 3] | 32) == 't' && (p[n - 2] | 32) == 'x' && (p[n - 1] | 32) == 't';
}

} // namespace

namespace HrefMap {

// Index the container PackReader currently has open. The first call fills the
// permanent base block (pack.bin); every later call replaces the current one.
bool addCurrentContainer()
{
    if (!Pack::isOpen()) return false;

    bool first = (g_base.ent == nullptr);
    if (first && !alloc(g_base, 700000, 128u * 1024 * 1024)) return false;
    if (!first && !g_cur.ent && !alloc(g_cur, 200000, 32u * 1024 * 1024)) return false;

    Block& b = first ? g_base : g_cur;
    b.n = 0;
    b.used = 1;

    UINT32 withPointer = 0, dropped = 0;
    for (UINT32 i = 0, n = Pack::count(); i < n; ++i) {
        const Pack::Res* r = Pack::at(i);
        const char* p = Pack::path(*r);
        size_t plen = strlen(p);
        UINT32 addr = Pack::blobBase() + r->blobOff;

        const char* root = endsWithTxt(p, plen) ? nullptr : Types::rootForObject(addr);
        size_t need = plen + (root ? strlen(root) + 13 : 1);
        if (b.n >= b.entCap || b.used + need >= b.blobCap) { ++dropped; continue; }

        char* d = b.blob + b.used;
        b.ent[b.n].addr    = addr;
        b.ent[b.n].hrefOff = b.used;
        memcpy(d, p, plen); d += plen;
        if (root) {
            memcpy(d, "#xpointer(/", 11); d += 11;
            size_t rl = strlen(root);
            memcpy(d, root, rl); d += rl;
            *d++ = ')';
            ++withPointer;
        }
        *d = 0;
        b.used = (UINT32)(d + 1 - b.blob);
        ++b.n;
    }
    qsort(b.ent, b.n, sizeof(Ent), cmpEnt);
    if (dropped) Log::write("HrefMap: WARNING %u entries dropped, block full", dropped);
    Log::write("HrefMap: %s block %u hrefs (%u with xpointer), %uK of text",
               first ? "base" : "map", b.n, withPointer, b.used / 1024);
    return true;
}

const char* lookup(UINT32 objAddr)
{
    const char* h = g_cur.n ? find(g_cur, objAddr) : nullptr;
    return h ? h : find(g_base, objAddr);
}

const char* lookupFrom(UINT32 objAddr, const char* curPath)
{
    const char* h = lookup(objAddr);
    if (!h || !curPath) return h;
    if (strchr(h, '#')) return h;                 // has an xpointer -> absolute
    const char* hs = strrchr(h, '/');
    const char* cs = strrchr(curPath, '/');
    size_t hd = hs ? (size_t)(hs - h) : 0;
    size_t cd = cs ? (size_t)(cs - curPath) : 0;
    if (hd == cd && (!hd || !strncmp(h, curPath, hd)))
        return hs ? hs + 1 : h;                   // same directory -> bare name
    return h;
}

// pack.bin's blob base has to outlive the swap: an external reference is a blob
// offset INTO PACK, and the base block is keyed by absolute address.
UINT32 g_packBase = 0;
UINT32 g_curBase  = 0;

void setContainerBase(UINT32 blobBase, bool isPack)
{
    g_curBase = blobBase;
    if (isPack) g_packBase = blobBase;
}

// A reference the loader could not bind. `slotAddr` is the address of the
// pointer field itself, which is what the ref serializer hands us.
const char* lookupExternal(UINT32 slotAddr, const char* curPath)
{
    if (!g_packBase || !g_curBase || slotAddr < g_curBase) return nullptr;
    UINT32 target = Fixups::targetFor(slotAddr - g_curBase);
    if (!target) return nullptr;
    return lookupFrom(g_packBase + target, curPath);
}

UINT32 count() { return g_base.n + g_cur.n; }

} // namespace HrefMap
