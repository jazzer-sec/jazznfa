# jazznfa

NFA account tool using the supplied GameSense-style Dear ImGui / DirectX 11 loader source: small Verdana text, square dark panels, thin borders, green selection marks and a service status log.

## Send to a friend

Send **`dist-single/JazzNfa.exe`**. That is the only file needed. It includes the NFA worker and its self-contained .NET 10 / Windows App SDK runtime. No installer, .NET installation, Visual Studio runtime installation or adjacent files are required.

On first launch it unpacks internal files into `%LOCALAPPDATA%\JazzNfa\runtime\<build-id>`. Later launches reuse that cache. Account history, preferences and the encrypted API key use the original `%APPDATA%\nfa.pub Loader` data folder. Your own settings, API key and accounts are **not** included in the executable. Friends enter their own API key.

Requires Windows x64 (Windows 10 version 1809 or newer) and Steam installed for Steam sign-in. Internet access is needed for Steam and nfa.pub operations.

## Menu

- **Sign in:** paste a token or `steamid----token` line; inspect its format and expiry locally, verify it with Steam, or restart Steam using the account.
- **Accounts:** search History, import token lines, query CS2 status, select a token, remove a local record, request a replacement by order ID, or restore a remembered Steam login.
- **Store:** fetch live CS2 product prices, availability and balance; confirm a purchase; recover an interrupted purchase using its original idempotency key.
- **Tools:** edit and apply T / CT weapon presets, change the token account's nickname, or clear CS2 and Wallpaper Engine Workshop subscriptions.
- **Settings:** save an encrypted nfa.pub API key and an optional Steam install folder.

Sign-in, purchases, replacements and account-changing tools show a confirmation with the operation's details. Operations run outside the render thread. Closing is disabled while a service request is running. The NFA service's existing purchase recovery, account history and DPAPI storage are retained. Network failures do not report success.

## Build from raw source

Requires the .NET 10 SDK, CMake 3.20+, and Visual Studio with the C++ desktop toolchain and a Windows SDK. Keep `../nfatool-v0.5.5` beside this directory.

```powershell
.\Build.ps1
```

The script compiles the NFA source, packages its published runtime into a compressed Windows cabinet, then compiles the GUI, Dear ImGui and FreeType directly from their source files. The cabinet and its file manifest become resources in `JazzNfa.exe`. No precompiled GUI executable, library, object or precompiled header from the supplied GUI is used.

Build intermediates stay in `build-native/`; the distributable is `dist-single/JazzNfa.exe`. Third-party notices are included in the embedded runtime under `licenses/`.

## Verification

```powershell
python tests/smoke_test.py
```

This copies just the final EXE into a new directory with spaces in its name, removes .NET from the child search path, and runs the embedded worker's pipe handshake twice. The internal `--self-test` switch performs no login, purchase, Workshop request, or account change.

## Source layout

- `src/gui.cpp`: the supplied loader drawing code, repurposed for the NFA menu.
- `src/nfa_client.cpp`: private anonymous pipes to the NFA service worker.
- `src/runtime_bundle.cpp`: Windows-native first-launch extraction and cache verification.
- `../nfatool-v0.5.5/NfaLoader/NativeBridge.cs`: service commands, using the original NFA services.
- `../nfatool-v0.5.5/NfaLoader/Program.cs`: the `--native-bridge` worker mode. The original WinUI entry point remains available.

The UI provides nickname editing; avatar editing, history groups and CS2 cloud-sync configuration remain in the original NFA application.

## Third party

- Dear ImGui 1.92.9b (MIT), compiled from `external/imgui`.
- FreeType 2.14.3 (FreeType License), compiled from `external/freetype`. Portions copyright The FreeType Project.
- NFA Loader: see `../nfatool-v0.5.5/LICENSE`.
