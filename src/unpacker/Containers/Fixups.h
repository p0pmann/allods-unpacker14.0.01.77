// Containers/Fixups.h -- recover the references the loader could not bind.
//
// A resource database records a reference to a resource in ANOTHER database as
// the target's blob offset in pack.bin. InplaceLoader only fixes up offsets
// inside its own blob, so those fields are left null and the engine writes an
// empty href. The records survive in the container's fixup stream, which is
// dropped after loading -- so the file is re-read and decompressed to get them.
//
// Encoding (validated over a whole container: 8436/8436 slots 4-aligned, 0
// external values carrying any other tag):
//     key & 7 == 4   ->  slot = (key - 4) / 2, value = pack.bin blob offset
#pragma once
#include <windows.h>

namespace Fixups {

// Load the external references of one container file. Replaces whatever was
// loaded before, so it follows the currently mounted database.
bool load(const char* containerPath);

// pack.bin blob offset for the reference stored at this blob-relative slot,
// or 0 if the slot carries no external reference.
UINT32 targetFor(UINT32 slotBlobOffset);

UINT32 count();
void   clear();

} // namespace Fixups
