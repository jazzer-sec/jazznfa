# jazznfa

A Windows NFA account tool built with the supplied GameSense-style Dear ImGui / DirectX 11 loader interface and the NFA Loader v0.5.5 services.

Small Verdana text, square dark panels, thin borders, green selections and a live status log.

**This repository contains source, source assets, third-party notices and build scripts only.** No executables, DLLs, compiled libraries, runtime bundles, account history or API keys are committed.

## Features

- Inspect a Steam login token locally, verify it with Steam, and sign in.
- Search account history, import token lines, query CS2 account status and restore remembered Steam logins.
- Fetch nfa.pub stock and prices, buy accounts and request replacements.
- Recover interrupted purchases using their original idempotency key.
- Edit and apply T / CT loadouts, change a Steam nickname and clear Workshop subscriptions.
- Save an API key encrypted with Windows DPAPI and select a Steam install folder.
- Build a single distributable `JazzNfa.exe` containing the required runtime.

Account-changing actions show a confirmation. Service work runs outside the render thread and reports its result in the status log.

## Build

Use Windows x64 with:

- PowerShell 7 (`pwsh`).
- .NET 10 SDK.
- CMake 3.20 or newer.
- Visual Studio with Desktop development with C++ and a Windows SDK.

From the repository root:

```powershell
pwsh -File .\Build.ps1
```

The GUI, Dear ImGui, FreeType and NFA service code are compiled from source. Standard Microsoft runtime packages are restored through NuGet. The worker is published self-contained, compressed with Windows MakeCAB and embedded in the native GUI executable.

The result is:

```text
gamesense-loader-ui-main/dist-single/JazzNfa.exe
```

Only that EXE needs to be shared. On first launch it unpacks its internal runtime into `%LOCALAPPDATA%\JazzNfa\runtime\<build-id>`; later launches reuse the cache. Friends need Windows x64 and Steam for sign-in, but no separate .NET installation. Each person supplies their own account tokens and API key.

The optional Steam Cloud sync feature from the original WinUI application requires Valve's official `steam_api64.dll`, which is deliberately excluded from this source-only repository. The native menu's core features build without it.

## Verify the standalone build

With Python 3.11 or newer installed:

```powershell
python .\gamesense-loader-ui-main\tests\smoke_test.py
```

The smoke test copies only the EXE to a new directory, removes .NET from the child search path and checks the bundled worker's startup, pipe handshake and loadout catalog twice. It does not sign into Steam, purchase anything or modify an external account.

## Layout

| Path | Contents |
| --- | --- |
| `gamesense-loader-ui-main/src/` | Native menu, DirectX host, worker connection and embedded-runtime extraction |
| `gamesense-loader-ui-main/external/` | Dear ImGui and FreeType source |
| `gamesense-loader-ui-main/Build.ps1` | Compile and package the single executable |
| `gamesense-loader-ui-main/tests/` | Standalone packaging smoke test |
| `nfatool-v0.5.5/NfaLoader/NativeBridge.cs` | Native-menu commands using the original NFA services |
| `nfatool-v0.5.5/NfaLoader/Services/` | Steam, nfa.pub, storage and account services |

The original WinUI UI remains in the NFA source tree. Avatar editing, history groups and cloud-sync configuration remain in that UI; the native menu provides the features listed above.

## Credits and notices

The native interface adapts the supplied `gamesense-loader-ui-main` source. NFA Loader v0.5.5 is copyright 2026 fakearchie and tuntun1337 and retains its [MIT license](nfatool-v0.5.5/LICENSE). Dear ImGui retains its [MIT license](gamesense-loader-ui-main/external/imgui/LICENSE.txt). FreeType retains its [FreeType license](gamesense-loader-ui-main/external/freetype/docs/FTL.TXT) and other notices in its source tree. No affiliation with GameSense or Valve is implied.
