# Allods Online 14.0.01.77 unpacker

Minimal in-process resource unpacker for the 32-bit `AOgame.exe` from client
version **14.0.01.77**.

> **Disclaimer** — This is an unofficial, fan-made project. It is not affiliated
> with or endorsed by Allods Team, my.games. It ships no game assets;
> you need your own copy of the game to use it.
>
> "Allods Online" and related names, logos, and marks are trademarks of their
> respective owners (Allods Team / my.games). They are used here only to
> identify the game this project is compatible with. This project claims no
> ownership of them and is not sponsored, endorsed, or affiliated with the
> rights holders.

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
.\scripts\unpack.ps1 -ClientDir '<client-directory>'
```

The harness builds both DLLs, replaces `bin\pango.dll` temporarily, launches
`AOgame.exe`, waits for the unpacker freeze, creates the `WRITE_NOW` trigger,
and waits for completion. Extracted `.xdb` files are written under
`<client>\bin\data`. The launched client is stopped and the original carrier is
restored when the run finishes or fails.

For a quick smoke run:

```powershell
.\scripts\unpack.ps1 -ClientDir '<client-directory>' -Limit 100
```

Build without deploying from an x86 MSVC developer prompt:

```powershell
nmake
```

Outputs are placed in `build\`.

## Notes

- The original `pango.dll` is preserved as `pango_orig.dll`.
- MinHook is vendored under `src/unpacker/Hooks/minhook` under its BSD-2-Clause
  license.

## License

Project code is available under the [MIT License](LICENSE). Vendored MinHook
code remains under its included BSD-2-Clause license.
