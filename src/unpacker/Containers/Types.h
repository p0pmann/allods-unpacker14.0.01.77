// Containers/Types.h -- what a resource's root element is called.
//
// Two independent facts are needed and both live in the running client:
//   * the CONCRETE C++ type of an object -- from the MSVC RTTI chain hanging off
//     its vtable, so no type table and no filename guessing;
//   * the DOTTED class name the .xdb dialect uses ("Geometry" ->
//     "client.Scene3D.Geometry") -- from the class registry the client itself
//     loaded, so no external 7.0 schema dump.
#pragma once
#include <windows.h>

namespace Types {

// Walk .rdata for NDb vtables and harvest the dotted names. Needs the client
// frozen (protect.dll decrypts .rdata lazily).
bool init();

UINT32      vtableCount();
UINT32      fqnCount();

// "Geometry" for a vtable that belongs to an NDb type, else nullptr.
const char* shortName(UINT32 vtable);

// Root element / xpointer target for a short type name. Server classes use the
// fully-qualified dotted name, client-only classes the bare simple name -- that
// distinction was 92% of all root mismatches against the 7.0 oracle.
const char* rootName(const char* shortName);

// Convenience: object address -> root element name, or nullptr.
const char* rootForObject(UINT32 objAddr);

} // namespace Types
