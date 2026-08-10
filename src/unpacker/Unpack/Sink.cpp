// Unpack/Sink.cpp -- the output object handed to the write archive.
//
// sub_5AC690 only builds the XML DOM when the SOURCE object's flags at +0x20
// have bit0 CLEAR and bit2 SET. A zeroed buffer fails that gate silently, the
// DOM is never created, and serializing then faults.
//
// On flush, sub_5AB860 renders the DOM and calls source->vtable[0](buf, len).
// We supply our own object whose slot 0 appends into a memory buffer -- the
// document is finished off (root name, <Header>) before it reaches disk, so it
// must not be streamed straight to a file.
#include "../Header.h"

namespace {

char*  g_buf   = nullptr;
UINT32 g_used  = 0;
const UINT32 BUF_CAP = 64u * 1024 * 1024;

UINT32 g_vtbl[16];
UINT32 g_obj[32];

void __fastcall sinkWrite(void* /*this*/, void* /*edx*/, const char* buf, int len)
{
    if (!g_buf || !buf || len <= 0) return;
    if (g_used + (UINT32)len >= BUF_CAP) return;      // never overrun; truncate
    memcpy(g_buf + g_used, buf, (size_t)len);
    g_used += (UINT32)len;
}

} // namespace

namespace Sink {

bool init()
{
    if (g_buf) return true;
    g_buf = (char*)VirtualAlloc(nullptr, BUF_CAP, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_buf) { Log::write("Sink: out of memory"); return false; }
    return true;
}

void begin()
{
    g_used = 0;
    ZeroMemory(g_vtbl, sizeof(g_vtbl));
    ZeroMemory(g_obj,  sizeof(g_obj));
    for (int i = 0; i < 16; ++i) g_vtbl[i] = (UINT32)(void*)&sinkWrite;
    g_obj[0] = (UINT32)(void*)g_vtbl;                       // vtable
    g_obj[Off::SINK_FLAGS_OFF / 4] = Off::SINK_FLAGS_VAL;   // +0x20 -> writable
}

const char* data() { return g_buf; }
UINT32      size() { return g_used; }
void*       object() { return g_obj; }

} // namespace Sink
