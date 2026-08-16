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

// Load a stored ZIP entry directly from an installed .pak archive. No loose
// Maps_*.bin copy is needed.
bool loadSlice(const char* archivePath, UINT32 offset, UINT32 size);

// Return each candidate pack.bin blob offset for a blob-relative pointer slot.
// Start cursor at zero and call until zero is returned. Candidate validation is
// left to HrefMap because the decompressed blob may contain coincidental words
// that resemble fixup pairs.
UINT32 nextTarget(UINT32 slotBlobOffset, UINT32* cursor);

UINT32 count();
void   clear();

} // namespace Fixups
