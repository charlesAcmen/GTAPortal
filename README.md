# GTAPortal

GTAPortal explores a real-time portal between GTA San Andreas and GTA V Enhanced. PortalDev transports both directions and keeps three-slot latest-frame channels so slow consumers do not stall either game. PortalV is an x64 Script Hook V ASI and ReShade add-on: F9 anchors a persistent, live-calibrated world portal and the effect uses GTA V scene depth for visual occlusion. Both games draw an 8 m circular, depth-tested aperture with its bottom 0.6 m below the ground; the 16:9 source is centre-cropped to a square without stretching. For SA→V, PortalDev maps GTA V's final rendered pose and FOV onto SA's normal main camera and captures that stable window through WGC; the experimental offscreen second camera is disabled. Both visible directions were user-confirmed before the overlap milestone; the enlarged circular geometry, visibility switches, main-camera alignment, clipping, and Lock In still require combined in-game acceptance.

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
| `reference/universal-modder` | Legacy GTA V + Minecraft passthrough reference | `8370faa8e114baf33acdb23079aff552a7728c4b` |
| `reference/Gta-V-Enhanced-Minecraft-Steve-Passthrough` | Enhanced passthrough reference | `2363ee0169f3983108ff78b8dd3c464ebac96c33` |
| `reference/minecraft-in-gta-enhanced` | Enhanced 1.0.1158.16 integration reference | `981f14ab109d7a8f57b533d4c7ea09a9a36d4255` |
| `reference/SkyCraft` | Skyrim + Minecraft architecture comparison | `bfcaf178524b92c2cdeb88e4ce0f13ef9ded6f32` |
| `third_party/ScriptHookVDotNetEnhanced` | Optional GTA V Enhanced .NET scripting reference; not linked into either project | `d756d479337a677cce9853b0cce63471c1fc7b7c` (`v1.1.0.6`) |
| `third_party/ReShade` | Graphics add-on API source and examples; not installed into either game | `18deaa52de0c425a78b329e9cb3c497281cd00ec` (`v6.8.0`) |

Script Hook V is distributed through [the developer's download page](https://www.dev-c.com/gtav/scripthookv/), not an official public Git source repository. On 2026-10-09 a previous workstation's selected Enhanced executable had file version `1.0.1158.16`, while its Script Hook V DLL file version ended in `1158.13`. Despite that metadata difference, its Script Hook V log reported successful initialization, registered `PortalV.asi`, and launched its main thread. The user also observed PortalV's moving overlay in Story Mode. The user-supplied SDK belongs at `third_party/ScriptHookV_SDK_1.0.617.1a/` and is ignored by Git; cloning this repository does not populate it. No Script Hook V runtime binaries are vendored here. ReShade's nested source dependencies are populated by the recursive submodule command above.

For a new workstation, obtain the SDK from the Script Hook V developer and extract it as `third_party/ScriptHookV_SDK_1.0.617.1a/`. Obtain the matching runtime separately from the same official page. `ScriptHookV.dll` and its ASI loader are installed into the selected GTA V game directory by the user; PortalDev deploys only `PortalV.asi` and never redistributes the runtime.

The local GTA SA installation is **not** part of the repository or a submodule. Keep your own clean copy under `game/clean/GTA San Andreas` and use `game/dev/GTA San Andreas` for plugin testing. Only the dev copy should receive `PortalSA.asi`. The project currently targets the GTA SA 1.0 US HOODLUM x86 executable; the executable and game assets must be supplied locally.

After cloning on a new workstation, build GTAPortal and install the optional windowed-mode helper into that workstation's dev game:

```powershell
& '.\src\Build-Dev.ps1'
& '.\src\Install-DevWindowedMode.ps1'
```

To build and run the two hardware smoke tests as well:

```powershell
& '.\src\Build-Dev.ps1' -RunTests
```

The second command builds the pinned upstream source as x86 and copies only its ASI plus the tracked debug configuration into `game/dev/GTA San Andreas/scripts`. It never writes to `game/clean`.

## Repository layout

- `src/PortalSA/`: x86 ASI, camera probe, V→SA world aperture, and SA offscreen portal-camera renderer. The earlier D3D9Ex bridge is available only through an opt-in development diagnostic switch.
- `src/PortalV/`: x64 Enhanced ASI with the camera probe and candidate SA→V ReShade picture plane.
- `src/PortalDev/`: x64 development tool with WGC previews and both cross-game GPU bridges.
- `src/SAToVFrames.h`: latest-frame, three-slot SA-to-V GPU texture publishing protocol.
- `src/SAToVOffscreenFrames.h`: PortalSA-owned three-slot CPU mailbox for the offscreen SA portal camera before PortalDev republishes it to the GPU channel.
- `src/Build-Dev.ps1`: reproducible build entry that selects a complete local MSVC toolchain.
- `src/Install-DevWindowedMode.ps1`: one-time per-workstation build/install helper for the optional windowed-mode dependency.
- `src/config/III.VC.SA.WindowedMode.ini`: reproducible dual-monitor debug defaults (1600x900 window, no pause on focus loss).
- `src/docs/commands.txt`: original project requirements.
- `src/docs/PROJECT_RESOURCES.md`: resource inventory and architectural analysis.
- `src/docs/MILESTONE_2026-10-09.md`: verified current state, observations, and next measurements.
- `src/docs/MILESTONE_2026-10-10.md`: current dual-direction GPU bridge milestone, accepted behavior, failed D3D9Ex experiment, and remaining in-game checks.
- `src/docs/MILESTONE_V_TO_SA_2026-10-12.md`: user-confirmed GTA V picture in the SA world, the inherited depth/stencil-state fix, and remaining measurements.
- `src/docs/MILESTONE_OVERLAP_CALIBRATION_2026-10-12.md`: final-render-camera/FOV alignment, CJ-hidden synchronized rendering, circular clipping, independent visibility switches, and F9 live calibration with explicit Lock In persistence.
- `src/docs/MILESTONE_SA_TO_V_2026-10-09.md`: SA→V candidate implementation, tests, and game acceptance steps.
- `src/docs/CODE_AUDIT_2026-10-10.md`: current source inventory, GPU data flows, evidence boundaries, and cleanup priorities.
- `src/docs/CROSS_GAME_REFERENCES.md`: read-only mashup source index and a plain-language explanation of the current SA frame path.
- `.gitmodules`: dependency URLs and paths.
- `.gitignore`: tracks project files and submodule pointers while excluding game files, local tools, and build output.

The GitHub repository is private. Sign in to GitHub on the other device with an account that has access before cloning it.
