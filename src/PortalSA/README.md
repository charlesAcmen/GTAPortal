# PortalSA milestone 1 (complete)

This is a Win32/x86 ASI for the GTA San Andreas 1.0 US HOODLUM dev copy. It reads player and camera state through the `third_party/plugin-sdk/plugin-sdk` submodule. It also saves one periodically updated BMP from the game's D3D9 backbuffer. On 2026-10-08 the user verified the state log, repeated frame captures, and dual-monitor windowed workflow in the real game. There is no shared-memory transport or GTA V code yet.

## Build (run these commands yourself)

The repository now includes `src/Build-Dev.ps1`, which selects a complete installed MSVC toolchain, generates the matching plugin-sdk project, and builds both the ASI and the external development utility. From the repository root:

```powershell
& '.\src\Build-Dev.ps1'
```

On the current workstation the script selects the complete Visual Studio 2022 Build Tools v143 installation. Visual Studio 2026 is present but its C++ runtime-library component is incomplete. The lower-level commands remain below for reference; they do not launch the game:

```powershell
$repo = (Get-Location).Path
git submodule update --init --recursive
$sdk = Join-Path $repo 'third_party\plugin-sdk\plugin-sdk'
$env:PLUGIN_SDK_DIR = $sdk
$msbuild = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe'

Push-Location $sdk
& '.\tools\premake\premake5.exe' '--file=tools\premake\premake5.lua' "--pluginsdkdir=$sdk" vs2022
& $msbuild '.\plugin_sa\Plugin_SA.vcxproj' '/p:Configuration=Release' '/p:Platform=Win32'
Pop-Location

& $msbuild (Join-Path $repo 'src\PortalSA\PortalSA.vcxproj') '/p:Configuration=Release GTA-SA' '/p:Platform=Win32' '/p:PlatformToolset=v143'
```

Expected build artifacts: `third_party/plugin-sdk/plugin-sdk/output/lib/Plugin.lib`, `src/PortalSA/bin/GTA-SA/Release/PortalSA.asi`, and `src/PortalDev/bin/x64/Release/PortalDev.exe`. The ASI is always built as Win32/x86; the external utility is x64.

The original SDK template would terminate a running game and copy the ASI during every build. Those post-build commands were removed from this project. Build and deploy are separate steps.

## Optional dual-monitor windowed development setup

Classic GTA SA otherwise uses exclusive D3D9 fullscreen and minimizes when focus moves to PortalDev on another display. The pinned `third_party/III.VC.SA.WindowedMode` v2.2 submodule is a development-runtime dependency only: it is not linked into PortalSA and is installed only into the dev game's `scripts` directory.

After initializing all submodules, run once per workstation:

```powershell
git submodule update --init --recursive
& '.\src\Install-DevWindowedMode.ps1'
```

The tracked default is a centered 1600x900 framed window with `autoPause=0`. `Ctrl+Enter` switches framed/borderless window style and `Alt+Enter` switches between windowed and desktop fullscreen. WindowedMode may save the last window position and size into its deployed INI; the tracked template remains unchanged. Remove the two `III.VC.SA.WindowedMode.*` files from the dev `scripts` directory to roll it back. Do not install it into `game/clean`.

## Deploy and inspect (dev copy only)

### Recommended: PortalDev utility

Run `src/PortalDev/bin/x64/Release/PortalDev.exe`. It verifies the dev game's MD5, deploys only to `game/dev`, launches the game, refreshes injection status, tails `portal.log`, and opens the captured frame. Use **Deploy and launch** for the normal development loop. It is not a second injector: Ultimate ASI Loader still loads `PortalSA.asi` inside GTA SA. The utility intentionally stays outside the game so later shared-memory inspection can be added without destabilizing the render hook.

### Manual equivalent

With the game closed, copy the ASI to the dev game's root directory, beside the existing `.asi` files:

```powershell
$repo = (Get-Location).Path
$dev = Join-Path $repo 'game\dev\GTA San Andreas'
Copy-Item -LiteralPath (Join-Path $repo 'src\PortalSA\bin\GTA-SA\Release\PortalSA.asi') -Destination (Join-Path $dev 'PortalSA.asi') -Force
Start-Process -FilePath (Join-Path $dev 'gta-sa.EXE') -WorkingDirectory $dev
```

Observe the loading/front-end screens, load the existing save, walk/turn CJ, rotate the camera, drive a vehicle, open the pause menu, and resume. If a cutscene is available, observe that too. After at least a few seconds, inspect:

```powershell
Get-Content -LiteralPath (Join-Path $dev 'portal.log') -Tail 15
Get-Item -LiteralPath (Join-Path $dev 'portal-frame.bmp') | Select-Object Length,LastWriteTime
Copy-Item -LiteralPath (Join-Path $dev 'portal-frame.bmp') -Destination (Join-Path $env:TEMP 'portal-frame-review.bmp') -Force
Start-Process -FilePath (Join-Path $env:TEMP 'portal-frame-review.bmp')
```

Expected log: `PortalSA loaded`, `Render output hook installed`, `state` records about every 100 ms while the game runs, and `frame ... result=ok` about every 2 seconds while the game presents frames. Position and heading should change with CJ; `player_move_speed_per_tick` should change with motion, and `velocity_source=vehicle` while driving. Camera position, forward, up and FOV should respond to camera motion. The `render_view_row_major` and `render_projection_row_major` arrays appear only when a recent world render supplied them. `portal-frame.bmp` should show the actual current game picture, including the HUD or menu when visible; its timestamp should advance while frames are being presented. The saved image is atomically replaced, so a reviewer sees a complete previous frame during the next write.

The `.bmp` is an uncompressed Windows bitmap. PortalSA writes a 54-byte BMP/DIB header followed by 32-bit top-down BGRA pixels (`BI_RGB`). It is intentionally easy to inspect and lossless, but large and unsuitable as the final 60 FPS transport. The current build performs a synchronous GPU-to-CPU readback and atomically replaces the same file about every two seconds; the visible periodic update proves final-frame access only.

`phase=loading`, `menu_or_pause`, `cutscene`, `gameplay`, or `no_player` describes the condition at sampling. `state_age_ms` on a `frame` record shows how old the last state sample is; an increasing age during pause/loading is expected when gameplay processing stops. A missing new frame during a particular loading transition can mean the game did not submit a new frame through this hook; the previous BMP remains intact.

If the log says `Render output hook failed`, or `frame ... result=failed hresult=...`, note the exact line. Capture supports 32-bit X8R8G8B8/A8R8G8B8 backbuffers and resolves multisampling before readback. A different display format, inaccessible device, or another mod's hook can still make capture fail. The synchronous readback may cause a brief hitch every two seconds; this proves access to the final picture, not a finished low-latency transport.

State values come from the plugin-sdk game's structs: `FindPlayerPed`, `CPlaceable::GetPosition/GetHeading`, `CPhysical::m_vecMoveSpeed`, `TheCamera`, and active `CCam`. Camera forward/up use `CMatrix::GetForward/GetUp`; in this SDK these getters map to the matrix's `up`/`at` fields, respectively. `m_vecMoveSpeed` is the game's per-tick movement vector, not a calibrated metres-per-second measurement. The projection/view arrays are the engine's D3D matrices observed after world rendering; they are omitted when no recent world render exists. A cutscene can have a different camera timeline or no player ped, and this is explicitly marked in the log.

## Next milestone

Milestone 2 publishes a versioned, pointer-free camera/state snapshot through Win32 shared memory and extends PortalDev into the independent reader/latency inspector. Logging remains throttled, but the shared snapshot will update once per game/render frame and include a monotonically increasing sequence plus `QueryPerformanceCounter` timestamp. See `src/docs/PROJECT_RESOURCES.md` for the transport and camera-override plan.
