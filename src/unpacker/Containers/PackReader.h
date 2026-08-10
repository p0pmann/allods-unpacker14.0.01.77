// Containers/PackReader.h -- read the pack's own structures straight out of the
// client's mapped image. The client has already decompressed pack.bin, so there
// is no zlib step and no file parsing: everything is a pointer walk.
#pragma once
#include <windows.h>

namespace Pack {

struct Res {
    UINT32 blobOff;      // heapdir offset; object = blobBase() + blobOff
    UINT32 pathOff;      // index into the path blob
    UINT32 resourceId;   // from id-map B, or 0 when the pack carries no id
};

// Locate the container currently installed on the resource manager and parse
// its header, heap directory and id-map B. Entries come out sorted by blobOff.
bool open();

// Forget it, so a later open() picks up whatever container was installed since.
// Buffers are kept and reused.
void close();

bool        isOpen();
UINT32      blobBase();                  // absolute address of blob offset 0
UINT32      count();
const Res*  at(UINT32 i);
const char* path(const Res& r);          // "Mechanics/Classes/Bard.xdb"

// Reverse lookup for hrefs: an object's address -> the resource path that names
// it, or nullptr when the address is not an allocation base.
const char* pathForAddr(UINT32 objAddr);

} // namespace Pack
