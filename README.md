# Allods Online 14.0.01.77 unpacker

Minimal in-process resource unpacker for the 32-bit `AOgame.exe` from client
version **14.0.01.77**.

The repository contains only three pieces:

- `src/carrier`: a `pango.dll` proxy that forwards all 397 exports to the stock
  `pango_orig.dll` and loads `AllodsUnpacker14.dll` during process startup.
- `src/unpacker`: the custom DLL. It freezes the client after its resource
  database is mapped, walks the live containers, and calls the client's own XDB
  serializer.
- `scripts/unpack.ps1`: builds, deploys, launches, triggers extraction, waits for
  completion, and restores the stock carrier.

No client binaries or extracted game data are included.

## Requirements

- Windows and an Allods Online 14.0.01.77 client.
- Visual Studio with the MSVC x86 C++ toolchain.
- PowerShell 5.1 or newer.

All engine addresses in `src/unpacker/Offsets.h` are specific to 14.0.01.77.

## Run

From PowerShell:

```powershell
.\scripts\unpack.ps1 -ClientDir 'E:\allods\clients\14.1'
```

The harness builds both DLLs, replaces `bin\pango.dll` temporarily, launches
`AOgame.exe`, waits for the unpacker freeze, creates the `WRITE_NOW` trigger,
and waits for completion. Extracted `.xdb` files are written under
`<client>\bin\data`. The launched client is stopped and the original carrier is
restored when the run finishes or fails.

For a quick smoke run:

```powershell
.\scripts\unpack.ps1 -ClientDir 'E:\allods\clients\14.1' -Limit 100
```

Build without deploying from an x86 MSVC developer prompt:

```powershell
nmake
```

Outputs are placed in `build\`.

## Notes

- The original `pango.dll` is preserved as `pango_orig.dll`; the harness will
  not overwrite an existing backup with a different file.
- MinHook is vendored under `src/unpacker/Mhook/minhook` under its BSD-2-Clause
  license.
- Full extraction includes `pack.bin` and the installed `Maps_*.bin` databases
  and can produce several gigabytes of files.
