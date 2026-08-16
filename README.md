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
  database is mapped, walks the live containers, calls the client's own XDB
  serializer, and expands the compiled localization database into sibling
  `.txt` resources.
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
.\scripts\unpack.ps1 -ClientDir '<client-directory>' -OutputDir '<output-directory>'
```

The harness builds both DLLs, replaces `bin\pango.dll` temporarily, launches
`AOgame.exe`, waits for the unpacker freeze, creates the `WRITE_NOW` trigger,
and waits for completion. Extracted `.xdb` and localized `.txt` files are
written to the mandatory absolute `-OutputDir`. The launched client is stopped
and the original carrier is restored when the run finishes or fails.

For reliable Direct3D 9 initialization in Remote Desktop sessions, the harness
temporarily changes `Personal\Global.cfg` from exclusive fullscreen to windowed
mode. The original configuration bytes are restored after success or failure.

The run discovers `Bin/pack.bin`, `Bin/pack.loc`, and every `Bin/Maps_*.bin`
database at runtime inside `data\Packs\*.pak`. Map databases are mounted through
the client VFS and all database bytes stay in their owning archives; nothing is
copied into `data\Bin`.

A full run automatically divides extraction by top-level resource directory and
starts a fresh 32-bit client for each scope. This avoids cumulative serializer
state exhausting the client during the 486,000-resource base database. Explicit
`-Scope` and `-Limit` runs use one process.

For a quick smoke run:

```powershell
.\scripts\unpack.ps1 -ClientDir '<client-directory>' -OutputDir '<output-directory>' -Limit 100
```

To unpack only one resource folder, pass `-Scope` at any depth. A trailing
slash is optional and either slash style is accepted:

```powershell
.\scripts\unpack.ps1 -ClientDir '<client-directory>' -OutputDir '<output-directory>' -Scope 'Mechanics\Classes'
.\scripts\unpack.ps1 -ClientDir '<client-directory>' -OutputDir '<output-directory>' -Scope 'Interface\'
```

The filter observes directory boundaries, so `Mechanics` matches everything
under `Mechanics\` without also matching similarly named sibling folders.

Build without deploying from an x86 MSVC developer prompt:

```powershell
nmake
```

Outputs are placed in `build\`.

When deploying manually, set the required absolute `OutputDir` value in
`AllodsUnpacker14.ini`; extraction will not start when it is empty.

## Notes

- The original `pango.dll` is preserved as `pango_orig.dll`.
- Every derived binary payload reference is resolved against both loose files
  and the installed `.pak` central directories. Fields whose exact `.bin` or
  `.hi.bin` payload is absent are omitted, so clean-client extraction produces
  complete links without inventing references for payload-less resources.
- Every `pack.loc` record is emitted at its declared `.txt` path as UTF-16LE
  with a BOM, matching the on-disk format consumed by the client.
- MinHook is vendored under `src/unpacker/Hooks/minhook` under its BSD-2-Clause
  license.

## License

Project code is available under the [MIT License](LICENSE). Vendored MinHook
code remains under its included BSD-2-Clause license.
