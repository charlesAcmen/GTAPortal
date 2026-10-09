# GTAPortal

GTAPortal explores a real-time portal between the classic PC releases of GTA San Andreas and GTA V. The GTA SA ASI logs player/camera state, and the existing PortalDev.exe previews the dev game's window through Windows Graphics Capture. On 2026-10-09 the game and GPU preview ran together in the dev setup. The preview does not save real-time BMPs or explicitly read image pixels back to the CPU. GTA V integration and cross-process texture delivery to a game have not started.

## Clone on another device

Clone the main repository together with its pinned dependencies:

```powershell
git clone --recurse-submodules https://github.com/charlesAcmen/GTAPortal.git
Set-Location GTAPortal
git submodule status
```

If you already cloned without `--recurse-submodules`, run this from the repository root:

```powershell
git submodule update --init --recursive
```

The main repository records exact submodule commits. A regular `git pull` does not necessarily populate or update their working trees; run `git submodule update --init --recursive` after pulling changes to `.gitmodules` or the submodule pointers. Do not use `git submodule update --remote` for a reproducible build unless you intentionally plan to change the pinned versions.

| Path | Purpose | Pinned commit |
| --- | --- | --- |
| `third_party/plugin-sdk/plugin-sdk` | GTA SA headers, events, hook implementation, and SDK library build | `15f15b60bbf74c106e1b496ff92c98764abf4605` |
| `third_party/III.VC.SA.WindowedMode` | Optional dev-runtime window/borderless support for dual-monitor debugging; not linked into PortalSA | `f2f4e499a69019a47df35c77d354f8a1195db016` (`v2.2`) |
| `reference/gta-reversed` | Read-only game implementation reference; not linked into PortalSA | `01709e8eefdf0b4871f33a8eab19dfa0716d509f` |

The local GTA SA installation is **not** part of the repository or a submodule. Keep your own clean copy under `game/clean/GTA San Andreas` and use `game/dev/GTA San Andreas` for plugin testing. Only the dev copy should receive `PortalSA.asi`. The project currently targets the GTA SA 1.0 US HOODLUM x86 executable; the executable and game assets must be supplied locally.

After cloning on a new workstation, build GTAPortal and install the optional windowed-mode helper into that workstation's dev game:

```powershell
& '.\src\Build-Dev.ps1'
& '.\src\Install-DevWindowedMode.ps1'
```

The second command builds the pinned upstream source as x86 and copies only its ASI plus the tracked debug configuration into `game/dev/GTA San Andreas/scripts`. It never writes to `game/clean`.

## Repository layout

- `src/PortalSA/`: x86 ASI and camera probe. The earlier D3D9Ex bridge experiment remains disabled after it prevented the game from starting.
- `src/PortalDev/`: existing x64 development tool with Windows GPU window capture and preview.
- `src/Build-Dev.ps1`: reproducible build entry that selects a complete local MSVC toolchain.
- `src/Install-DevWindowedMode.ps1`: one-time per-workstation build/install helper for the optional windowed-mode dependency.
- `src/config/III.VC.SA.WindowedMode.ini`: reproducible dual-monitor debug defaults (1600x900 window, no pause on focus loss).
- `src/docs/commands.txt`: original project requirements.
- `src/docs/PROJECT_RESOURCES.md`: resource inventory and architectural analysis.
- `.gitmodules`: dependency URLs and paths.
- `.gitignore`: tracks project files and submodule pointers while excluding game files, local tools, and build output.

The GitHub repository is private. Sign in to GitHub on the other device with an account that has access before cloning it.
