// Containers/Types.cpp
//
// vtable -> "Geometry":
//   MSVC puts a Complete Object Locator pointer at vtable[-1]; COL+12 is the
//   TypeDescriptor and TD+8 is the mangled name (".?AUGeometry@NDb@@"). Scanning
//   .rdata on that shape recovers every NDb vtable with no IDA pass. Measured
//   against the pack's own TAG pairs this agrees on 474,516 resources with zero
//   disagreements, so it is treated as authoritative.
//
// "Geometry" -> "client.Scene3D.Geometry":
//   the dotted names are NOT in the pack's type table (that holds only C++
//   names, all with "::"). They are live strings in the class registry the
//   client loads at startup, so they are harvested by scanning committed memory
//   for dotted identifiers whose last component is a known NDb type.
#include "Types.h"
#include "../Header.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace {

struct VtEnt  { UINT32 vt; UINT32 nameOff; };
struct FqnEnt { UINT32 nameOff; UINT32 fqnOff; UINT32 rank; };  // short -> dotted

VtEnt*  g_vt    = nullptr;
UINT32  g_vtN   = 0;
FqnEnt* g_fqn   = nullptr;
UINT32  g_fqnN  = 0;

char*   g_str     = nullptr;
UINT32  g_strUsed = 0;
const UINT32 STR_CAP = 4u * 1024 * 1024;
const UINT32 VT_CAP  = 8192;
const UINT32 FQN_CAP = 8192;

UINT32 intern(const char* s, UINT32 n)
{
    if (g_strUsed + n + 1 >= STR_CAP) return 0;
    UINT32 at = g_strUsed;
    memcpy(g_str + at, s, n);
    g_str[at + n] = 0;
    g_strUsed += n + 1;
    return at;
}

int cmpVt(const void* a, const void* b)
{
    UINT32 x = ((const VtEnt*)a)->vt, y = ((const VtEnt*)b)->vt;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

int cmpFqn(const void* a, const void* b)
{
    return strcmp(g_str + ((const FqnEnt*)a)->nameOff, g_str + ((const FqnEnt*)b)->nameOff);
}

const VtEnt* lookupVt(UINT32 vt)
{
    UINT32 lo = 0, hi = g_vtN;
    while (lo < hi) {
        UINT32 mid = (lo + hi) >> 1;
        if (g_vt[mid].vt == vt) return &g_vt[mid];
        if (g_vt[mid].vt < vt) lo = mid + 1; else hi = mid;
    }
    return nullptr;
}

// ---- section bounds ------------------------------------------------------
// AOgame has THREE .rdata sections and two .data ones. Keeping only the last of
// each finds no vtables at all -- the NDb ones live in the first .rdata.
struct Sec { UINT32 lo, hi; bool rdata; };
Sec g_sec[16];
int g_secN = 0;

bool inConstData(UINT32 a)
{
    for (int i = 0; i < g_secN; ++i)
        if (a >= g_sec[i].lo && a < g_sec[i].hi) return true;
    return false;
}

void findSections()
{
    BYTE* base = (BYTE*)GetModuleHandleA(nullptr);
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(base + dos->e_lfanew);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections && g_secN < 16; ++i) {
        const char* nm = (const char*)sec[i].Name;
        bool rdata = !strncmp(nm, ".rdata", 6);
        if (!rdata && strncmp(nm, ".data", 5)) continue;
        g_sec[g_secN].lo    = (UINT32)(base + sec[i].VirtualAddress);
        g_sec[g_secN].hi    = g_sec[g_secN].lo + sec[i].Misc.VirtualSize;
        g_sec[g_secN].rdata = rdata;
        ++g_secN;
    }
}

// ".?AUGeometry@NDb@@" -> "Geometry". Anything else -> 0.
UINT32 ndbNameFrom(const char* mangled)
{
    if (mangled[0] != '.' || mangled[1] != '?' || mangled[2] != 'A') return 0;
    if (mangled[3] != 'U' && mangled[3] != 'V') return 0;
    const char* s = mangled + 4;
    UINT32 n = 0;
    while (n < 96 && (isalnum((unsigned char)s[n]) || s[n] == '_')) ++n;
    if (!n || strncmp(s + n, "@NDb@@", 6)) return 0;
    return intern(s, n);
}

void scanVtables()
{
    for (int s = 0; s < g_secN; ++s) {
        if (!g_sec[s].rdata) continue;
        for (UINT32 a = g_sec[s].lo + 4; a + 4 <= g_sec[s].hi; a += 4) {
            __try {
                UINT32 col = *(UINT32*)(a - 4);
                if (!inConstData(col)) continue;
                UINT32 td = *(UINT32*)(col + 12);
                if (!inConstData(td)) continue;
                UINT32 off = ndbNameFrom((const char*)(td + 8));
                if (off && g_vtN < VT_CAP) { g_vt[g_vtN].vt = a; g_vt[g_vtN].nameOff = off; ++g_vtN; }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }
    qsort(g_vt, g_vtN, sizeof(VtEnt), cmpVt);
}

// ---- dotted class names --------------------------------------------------
// A candidate is a NUL-terminated identifier like "gameMechanics.world.avatar.
// CharacterClass": lowercase namespace root, dot-separated parts, and a final
// component that is a type we actually saw a vtable for.
bool dottedCandidate(const char* s, UINT32 n, const char** lastPart, UINT32* dotCount)
{
    if (n < 5 || n > 120) return false;
    if (!islower((unsigned char)s[0])) return false;
    UINT32 dots = 0;
    const char* last = s;
    for (UINT32 i = 0; i < n; ++i) {
        char c = s[i];
        if (c == '.') {
            // A namespace root is never one or two letters; requiring three
            // rejects tails of truncated buffers ("n.Cue" beating the real
            // gameMechanics.world.mob.interaction.Cue).
            if (!dots && i < 3) return false;
            if (i + 1 >= n || s[i + 1] == '.') return false;
            ++dots;
            last = s + i + 1;
            continue;
        }
        if (!isalnum((unsigned char)c) && c != '_') return false;
    }
    if (!dots || !isupper((unsigned char)*last)) return false;
    *lastPart = last;
    *dotCount = dots;
    return true;
}

// Sorted, de-duplicated short names -- the memory scan tests millions of
// strings against this, so it has to be a binary search, not a walk.
UINT32* g_name  = nullptr;
UINT32  g_nameN = 0;

int cmpName(const void* a, const void* b)
{
    return strcmp(g_str + *(const UINT32*)a, g_str + *(const UINT32*)b);
}

void buildNameIndex()
{
    g_name = (UINT32*)VirtualAlloc(nullptr, sizeof(UINT32) * VT_CAP,
                                   MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_name) return;
    for (UINT32 i = 0; i < g_vtN; ++i) g_name[g_nameN++] = g_vt[i].nameOff;
    qsort(g_name, g_nameN, sizeof(UINT32), cmpName);
    UINT32 w = 0;
    for (UINT32 i = 0; i < g_nameN; ++i)
        if (!w || strcmp(g_str + g_name[w - 1], g_str + g_name[i])) g_name[w++] = g_name[i];
    g_nameN = w;
}

bool haveShortName(const char* s, UINT32 n)
{
    UINT32 lo = 0, hi = g_nameN;
    while (lo < hi) {
        UINT32 mid = (lo + hi) >> 1;
        const char* nm = g_str + g_name[mid];
        int c = strncmp(nm, s, n);
        if (!c) c = (nm[n] == 0) ? 0 : 1;
        if (!c) return true;
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return false;
}

// Keep the best dotted name per short type. A server class beats a client one;
// among equals the DEEPER namespace wins (a shallow candidate is usually the
// tail of some other buffer), and depth ties go to the shorter string.
UINT32 rank(const char* fqn, UINT32 dots)
{
    bool isClient = !strncmp(fqn, "client.", 7);
    return (isClient ? 0u : 0x10000u) + dots;
}

void offerFqn(const char* fqn, UINT32 n, const char* last, UINT32 dots)
{
    UINT32 shortLen = (UINT32)(fqn + n - last);
    UINT32 r = rank(fqn, dots);
    for (UINT32 i = 0; i < g_fqnN; ++i) {
        const char* have = g_str + g_fqn[i].nameOff;
        if (strncmp(have, last, shortLen) || have[shortLen]) continue;
        const char* old = g_str + g_fqn[i].fqnOff;
        if (r < g_fqn[i].rank) return;
        if (r == g_fqn[i].rank && strlen(old) <= n) return;
        UINT32 off = intern(fqn, n);
        if (off) { g_fqn[i].fqnOff = off; g_fqn[i].rank = r; }
        return;
    }
    if (g_fqnN >= FQN_CAP) return;
    UINT32 so = intern(last, shortLen);
    UINT32 fo = intern(fqn, n);
    if (!so || !fo) return;
    g_fqn[g_fqnN].nameOff = so;
    g_fqn[g_fqnN].fqnOff  = fo;
    g_fqn[g_fqnN].rank    = r;
    ++g_fqnN;
}

void scanRegion(const char* p, SIZE_T len)
{
    SIZE_T start = 0;
    for (SIZE_T i = 0; i < len; ++i) {
        if (p[i]) continue;
        SIZE_T n = i - start;
        if (n >= 5 && n <= 120) {
            const char* last = nullptr;
            UINT32 dots = 0;
            if (dottedCandidate(p + start, (UINT32)n, &last, &dots) &&
                haveShortName(last, (UINT32)(p + start + n - last)))
                offerFqn(p + start, (UINT32)n, last, dots);
        }
        start = i + 1;
    }
}

void scanMemoryForFqns()
{
    MEMORY_BASIC_INFORMATION mbi;
    UINT32 addr = 0x10000;
    // 32-bit but LARGEADDRESSAWARE: the pack maps above 2 GB, and the class
    // registry lives inside it. Stopping at 0x7FFF0000 finds nothing.
    while (addr < 0xFFFF0000u && VirtualQuery((void*)addr, &mbi, sizeof(mbi))) {
        UINT32 base = (UINT32)(UINT_PTR)mbi.BaseAddress;
        SIZE_T size = mbi.RegionSize;
        DWORD  prot = mbi.Protect & 0xFF;
        bool readable = mbi.State == MEM_COMMIT &&
                        !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                        (prot == PAGE_READONLY || prot == PAGE_READWRITE ||
                         prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_READ ||
                         prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY);
        if (readable) {
            __try { scanRegion((const char*)base, size); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        if (base + size <= addr) break;
        addr = base + (UINT32)size;
    }
    qsort(g_fqn, g_fqnN, sizeof(FqnEnt), cmpFqn);
}

} // namespace

namespace Types {

bool init()
{
    if (g_vtN) return true;
    g_str = (char*)VirtualAlloc(nullptr, STR_CAP, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_vt  = (VtEnt*)VirtualAlloc(nullptr, sizeof(VtEnt) * VT_CAP, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_fqn = (FqnEnt*)VirtualAlloc(nullptr, sizeof(FqnEnt) * FQN_CAP, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_str || !g_vt || !g_fqn) { Log::write("Types: out of memory"); return false; }
    g_strUsed = 1;                       // offset 0 means "none"
    g_str[0] = 0;

    findSections();
    if (!g_secN) { Log::write("Types: no .rdata/.data sections"); return false; }
    scanVtables();
    if (!g_vtN) { Log::write("Types: no NDb vtables -- .rdata still encrypted?"); return false; }
    buildNameIndex();
    scanMemoryForFqns();
    Log::write("Types: %u NDb vtables, %u dotted class names", g_vtN, g_fqnN);
    return true;
}

UINT32 vtableCount() { return g_vtN; }
UINT32 fqnCount()    { return g_fqnN; }

const char* shortName(UINT32 vtable)
{
    const VtEnt* e = lookupVt(vtable);
    return e ? g_str + e->nameOff : nullptr;
}

const char* rootName(const char* shortName)
{
    if (!shortName || !*shortName) return nullptr;
    UINT32 lo = 0, hi = g_fqnN;
    while (lo < hi) {
        UINT32 mid = (lo + hi) >> 1;
        int c = strcmp(g_str + g_fqn[mid].nameOff, shortName);
        if (!c) {
            const char* f = g_str + g_fqn[mid].fqnOff;
            // client-only classes are written with their bare simple name
            return strncmp(f, "client.", 7) ? f : shortName;
        }
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return shortName;
}

const char* rootForObject(UINT32 objAddr)
{
    UINT32 vt = 0;
    __try { vt = *(UINT32*)objAddr; } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    const char* s = shortName(vt);
    return s ? rootName(s) : nullptr;
}

} // namespace Types
