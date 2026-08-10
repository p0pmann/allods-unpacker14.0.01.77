// Offsets.h -- every address / layout constant reversed for AOgame.exe 14.0.01.77
//
// The PE has relocations stripped, so it always loads at 0x400000 and these are
// stable absolute VAs at runtime. Everything here was verified against the live
// (protect.dll-decrypted) process, not the on-disk image -- the static IDB never
// decrypts most of .text/.rdata.
#pragma once
#include <windows.h>

namespace Off {

// ---------------------------------------------------------------- image
constexpr DWORD IMAGE_BASE = 0x400000;
constexpr DWORD CODE_LO    = 0x400000;   // valid code range, used to sanity-check
constexpr DWORD CODE_HI    = 0x2200000;  // any vtable slot before calling it

// ------------------------------------------------------- resource system
// The resource manager singleton. Stable .data global; the manager itself is
// fully constructed by the pre-render freeze.
constexpr DWORD G_RESMGR       = 0x235CB14;   // *(void**)  -> resource manager
constexpr DWORD RESMGR_VTABLE  = 0x1C9D18C;   // sanity check on the above
constexpr DWORD RESMGR_VFS     = 0x84;        // resmgr + 0x84  -> pack/VFS object
constexpr DWORD RESMGR_RECMAP  = 0x88;        // resmgr + 0x88  -> record map (addons only)

// VFS (pack) object. vtable[2] is literally "Search in pack.bin" (the 4.0
// PackBin tool's own comment); it returns a live, pointer-fixed-up resource.
constexpr DWORD VFS_LOOKUP_SLOT = 2;          // byte offset 8

// The decompressed container, as {begin,end,cap} vectors on the VFS. The heap
// holds the directory / type table / id maps; the blob holds the objects.
constexpr DWORD VFS_HEAP_BEGIN = 0x0C;
constexpr DWORD VFS_HEAP_END   = 0x14;
constexpr DWORD VFS_BLOB_BEGIN = 0x20;

// Mount a resource database: sub_619850(&pathStr, &altPathStr) -> bool. Builds
// a GameMain::InplaceLoader and installs it at resmgr+0x84, releasing the
// previous one by a single reference (the pack is held twice, so it survives).
// This is how the ~300 per-map Maps_*.bin containers are reached.
constexpr DWORD FN_LOAD_DB = 0x619850;

// Resource load / access
constexpr DWORD FN_LOAD_BY_KEY = 0x5D97B0;    // (resmgr, &internedKey) -> object
constexpr DWORD FN_GET_BY_PATH = 0x5D85E0;    // (resmgr, &out, &gstr)  -- resident only
constexpr DWORD FN_ALLOC_BY_KEY= 0x5D8C30;    // (resmgr, &key) -- needs a record
constexpr DWORD FN_NORMALIZE   = 0x5D58D0;    // (dstKey, &gstr) -- intern a raw path

// ------------------------------------------------------------- archive
// sub_5AACD0(source, mode, &pathStr) -> archive.  mode 1 = READ, 2 = WRITE.
// In write mode sub_5AC690 builds a TinyXML-style DOM -- so mode 2 emits XML.
constexpr DWORD FN_MAKE_ARCHIVE = 0x5AACD0;
constexpr DWORD FN_ARCHIVE_FLUSH= 0x5AB860;   // renders the DOM, calls sink->vtable[0](buf,len)
constexpr DWORD FN_RELEASE      = 0x5872D0;   // (obj, 1, 0xFFFFF)
constexpr DWORD ARCHIVE_VTABLE  = 0x1C9B624;

// Archive vtable byte offsets
constexpr DWORD ARC_BEGIN_SCOPE = 0x1C;  // [7]  (arc, name, flags) -> bool
constexpr DWORD ARC_END_SCOPE   = 0x20;  // [8]  (arc)
constexpr DWORD ARC_BOOL        = 0x28;  // [10] typed accessors used by the
constexpr DWORD ARC_FLOAT       = 0x2C;  // [11] sub_574xxx wrappers
constexpr DWORD ARC_INT         = 0x44;  // [17]
constexpr DWORD ARC_IS_READING  = 0x88;  // [34] returns byte at arc+0x68
constexpr DWORD ARC_ATTR        = 0xA4;  // [41] READS in read-mode, WRITES in write-mode
constexpr DWORD ARC_INIT        = 0x98;  // [38] (arc, 1)
constexpr DWORD ARC_IS_READ_BYTE= 0x68;  // arc + 0x68 : 1 = reading, 0 = writing

// The write-mode DOM is only built when the SOURCE object's flags at +0x20 have
// bit0 CLEAR and bit2 SET (sub_5AC690 gates on it). A zeroed buffer fails that
// silently and serializing then faults.
constexpr DWORD SINK_FLAGS_OFF  = 0x20;
constexpr DWORD SINK_FLAGS_VAL  = 0x4;
constexpr DWORD ARCHIVE_REFCNT  = 0x4;   // ++ before use, else FN_RELEASE is a no-op
                                         // and archives+DOMs leak (dies at ~190k)

// -------------------------------------------------- field serialization
// Per-field primitives. Validated by decompiling each once and cross-checking
// against known values (Texture.width=512 int, alphaTex bool, altitude 1.0f).
constexpr DWORD SER_BOOL   = 0x5744E0;
constexpr DWORD SER_INT    = 0x574420;
constexpr DWORD SER_FLOAT  = 0x574470;   // NOTE: float, not int -- easily swapped
constexpr DWORD SER_REF    = 0x57B4B0;   // resource ref  (READ path only!)
constexpr DWORD SER_REF2   = 0x5779D0;   // href variant handling '/' and '#'
constexpr DWORD SER_INLINE = 0x55F120;   // nested struct / TextFileRef
constexpr DWORD SER_HREF   = 0x57C390;   // base-class href

// Not serializers, despite showing up next to field-name pushes:
constexpr DWORD FN_STRLEN     = 0x59EC40;   // ~1 call per field, misleads scans
constexpr DWORD FN_LUA_PUSHSTR= 0x1B978D0;

// String literal "href", reused when writing the attribute.
constexpr DWORD STR_HREF = 0x1C99D80;

// ------------------------------------------------------------- vtables
// NDb vtables are EXACTLY 11 slots. Slot 11+ is the type-name string data that
// follows the vtable in .rdata -- calling one jumps into text and HANGS.
constexpr int VTABLE_MAX_SLOT = 10;
// Observed serializer slot distribution: 2 -> 573 types, 6 -> 494, 3 -> 186.
constexpr int SER_SLOT_DEFAULT = 6;

// --------------------------------------------------------------- misc
// Main-thread per-frame step, reached through the app vtable. Hooking it is the
// only reliable way to run code on the engine's own thread.
constexpr DWORD APP_VTABLE     = 0x1D35748;
constexpr DWORD APP_STEP_SLOT  = 0xC;
constexpr DWORD FN_APP_STEP    = 0xCDE940;

// The object blob is mapped 1:1, so objectAddr = blobBase + heapdirOffset --
// everything about extraction follows from that. The base itself is read from
// VFS+0x20 at runtime rather than hardcoded; this is only what it was measured
// to be for this build, kept as a sanity reference.
constexpr DWORD PACK_MAP_BASE_OBSERVED = 0xEC9CF000;

} // namespace Off
