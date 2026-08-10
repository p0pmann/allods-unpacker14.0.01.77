// Containers/SlotMap.cpp -- vtable -> serializer slot.
//
// The serializer slot VARIES by type (2: 573 types, 6: 494, 3: 186). Hardcoding
// 6 capped the engine export at 39%; probing at runtime built up to 21 archives
// and DOMs per unknown type and stalled.
//
// The slot is identifiable from the code: the serializer is the vtable entry
// whose body pushes field-name string literals. Length-disassembling each
// candidate with HDE32 and counting those pushes picks it out, so the map is
// computed in-process and nothing has to be precomputed offline.
//
// Hard rule: NDb vtables are exactly 11 slots. Anything above VTABLE_MAX_SLOT is
// the type-name string data that follows the vtable in .rdata -- calling it
// jumps into text and hangs the process. Clamp, never trust.
#include "../Header.h"

#include "hde/hde32.h"

namespace {

const UINT32 SLOT_HASH = 16384;      // power of two, > 2x the vtable count
const UINT32 FN_HASH   = 32768;
const UINT32 CODE_WIN  = 0xC00;      // enough of a serializer to see its fields

struct Slot { UINT32 key; int slot; };
struct Fn   { UINT32 key; int score; };

Slot* g_slot = nullptr;
Fn*   g_fn   = nullptr;

bool isFieldName(UINT32 va)
{
    if (va < Off::CODE_LO || va >= Off::CODE_HI) return false;
    __try {
        const char* s = (const char*)va;
        int n = 0;
        bool lower = false;
        while (n < 30) {
            unsigned char c = (unsigned char)s[n];
            if (!c) break;
            if (c < 0x20 || c >= 0x7F) return false;
            if (c >= 'a' && c <= 'z') lower = true;
            else if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
            ++n;
        }
        if (n < 2 || n >= 30 || s[n]) return false;
        if (!lower) return false;
        char f = s[0];
        return (f >= 'a' && f <= 'z') || (f >= 'A' && f <= 'Z') || f == '_';
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// How many field-name literals this function pushes.
int score(UINT32 fn)
{
    UINT32 h = (fn * 2654435761u) & (FN_HASH - 1);
    for (UINT32 i = 0; i < FN_HASH; ++i) {
        Fn& e = g_fn[(h + i) & (FN_HASH - 1)];
        if (e.key == fn) return e.score;
        if (e.key) continue;
        int n = 0;
        __try {
            const BYTE* p = (const BYTE*)fn;
            for (UINT32 at = 0; at < CODE_WIN;) {
                hde32s hs;
                unsigned len = hde32_disasm(p + at, &hs);
                if (!len || (hs.flags & F_ERROR)) break;
                if (hs.opcode == 0x68 && (hs.flags & F_IMM32) && isFieldName(hs.imm.imm32)) ++n;
                if (hs.opcode == 0xC3 || hs.opcode == 0xC2) break;
                at += len;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { n = 0; }
        e.key = fn; e.score = n;
        return n;
    }
    return 0;
}

int compute(UINT32 vtable)
{
    int best = 0, bestSlot = Off::SER_SLOT_DEFAULT;
    for (int s = 0; s <= Off::VTABLE_MAX_SLOT; ++s) {
        UINT32 fn = 0;
        __try { fn = *(UINT32*)(vtable + 4 * s); } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
        if (fn < Off::CODE_LO || fn >= Off::CODE_HI) continue;
        int sc = score(fn);
        if (sc > best) { best = sc; bestSlot = s; }
    }
    return bestSlot;
}

} // namespace

namespace SlotMap {

bool init()
{
    if (g_slot) return true;
    g_slot = (Slot*)VirtualAlloc(nullptr, sizeof(Slot) * SLOT_HASH,
                                 MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_fn = (Fn*)VirtualAlloc(nullptr, sizeof(Fn) * FN_HASH,
                             MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_slot || !g_fn) { Log::write("SlotMap: out of memory"); return false; }
    return true;
}

int slotFor(UINT32 vtable)
{
    if (!g_slot || !vtable) return Off::SER_SLOT_DEFAULT;
    UINT32 h = (vtable * 2654435761u) & (SLOT_HASH - 1);
    for (UINT32 i = 0; i < SLOT_HASH; ++i) {
        Slot& e = g_slot[(h + i) & (SLOT_HASH - 1)];
        if (e.key == vtable) return e.slot;
        if (e.key) continue;
        int s = compute(vtable);
        if (s < 0 || s > Off::VTABLE_MAX_SLOT) s = Off::SER_SLOT_DEFAULT;
        e.key = vtable; e.slot = s;
        return s;
    }
    return Off::SER_SLOT_DEFAULT;
}

} // namespace SlotMap
