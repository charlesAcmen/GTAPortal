# PortalSA milestone 1

This is a Win32/x86 ASI for the GTA San Andreas 1.0 US HOODLUM dev copy. It reads player and camera state through the `third_party/plugin-sdk/plugin-sdk` submodule. It also saves one periodically updated BMP from the game's D3D9 backbuffer. There is no shared memory or GTA V code yet. The code has not been compiled or run in the game; the steps below are for your acceptance test.

## Build (run these commands yourself)

Open PowerShell in the repository root, with Visual Studio 2026 C++/Win32 tools installed. The SDK checkout's Premake script generates the SDK library project; `PortalSA.vcxproj` was already generated from that same script and adjusted to build a single `PortalSA.asi`. The following commands do not launch the game:

```powershell
$repo = (Get-Location).Path
git submodule update --init --recursive
$sdk = Join-Path $repo 'third_party\plugin-sdk\plugin-sdk'
$env:PLUGIN_SDK_DIR = $sdk
$msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe'

Push-Location $sdk
& '.\tools\premake\premake5.exe' '--file=tools\premake\premake5.lua' "--pluginsdkdir=$sdk" vs2026
& $msbuild '.\plugin_sa\Plugin_SA.vcxproj' '/p:Configuration=Release' '/p:Platform=Win32'
Pop-Location

& $msbuild (Join-Path $repo 'src\PortalSA\PortalSA.vcxproj') '/p:Configuration=Release GTA-SA' '/p:Platform=Win32'
```

Expected build artifacts: `third_party/plugin-sdk/plugin-sdk/output/lib/Plugin.lib` and `src/PortalSA/bin/GTA-SA/Release/PortalSA.asi`. The solution `PortalSA.slnx` has a `Release|GTA-SA` configuration mapped to Win32. If your Visual Studio installation is in another directory, point `$msbuild` at its `MSBuild.exe`.

The original SDK template would terminate a running game and copy the ASI during every build. Those post-build commands were removed from this project. Build and deploy are separate steps.

## Deploy and inspect (dev copy only)

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

`phase=loading`, `menu_or_pause`, `cutscene`, `gameplay`, or `no_player` describes the condition at sampling. `state_age_ms` on a `frame` record shows how old the last state sample is; an increasing age during pause/loading is expected when gameplay processing stops. A missing new frame during a particular loading transition can mean the game did not submit a new frame through this hook; the previous BMP remains intact.

If the log says `Render output hook failed`, or `frame ... result=failed hresult=...`, note the exact line. Capture supports 32-bit X8R8G8B8/A8R8G8B8 backbuffers and resolves multisampling before readback. A different display format, inaccessible device, or another mod's hook can still make capture fail. The synchronous readback may cause a brief hitch every two seconds; this proves access to the final picture, not a finished low-latency transport.

State values come from the plugin-sdk game's structs: `FindPlayerPed`, `CPlaceable::GetPosition/GetHeading`, `CPhysical::m_vecMoveSpeed`, `TheCamera`, and active `CCam`. Camera forward/up use `CMatrix::GetForward/GetUp`; in this SDK these getters map to the matrix's `up`/`at` fields, respectively. `m_vecMoveSpeed` is the game's per-tick movement vector, not a calibrated metres-per-second measurement. The projection/view arrays are the engine's D3D matrices observed after world rendering; they are omitted when no recent world render exists. A cutscene can have a different camera timeline or no player ped, and this is explicitly marked in the log.
