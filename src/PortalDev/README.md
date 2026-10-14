# PortalDev

`PortalDev.exe` builds and deploys the two portal plugins, reports their connection/transport state, and publishes the required image streams. It does not inject code: Ultimate ASI Loader loads the SA ASI and Script Hook V loads the V ASI.

PortalDev no longer displays either game's live picture and does not create preview swap chains. The required background transports remain active: GTA V window capture feeds V→SA, while SA main-window WGC feeds SA→V. The compact UI keeps only build, deployment, directory, log, teleport, and camera-calibration controls.

The SA status reports whether the mapped GTA V pose is currently controlling SA's main camera. If SA exits, PortalDev waits for the restarted game instead of showing an old connection.

## Build and run

Close PortalDev before rebuilding. Double-click `src/Build-Dev.cmd` in File Explorer and wait for `BUILD AND TESTS SUCCEEDED`. From PowerShell at the repository root, the equivalent is:

```powershell
& '.\src\Build-Dev.ps1' -RunTests
```

Open `src/PortalDev/bin/x64/Release/PortalDev.exe`. Click **部署 ASI**, then start `gta-sa.EXE` in the selected dev game directory. PortalDev detects the game automatically. The status should show a growing frame count and transport dimensions; use **打开日志** when detailed SA records are needed. The log should include `PortalSA loaded` and `Render output hook installed`.

The dev game can also be started before PortalDev; window capture starts when PortalDev finds it. The ASI itself must already have been deployed before game startup. Deployment targets the selected SA development directory and never the repository's `game/clean` copy.

The **Choose SA directory** button opens a modal folder picker. The default remains `game/dev/GTA San Andreas` relative to the repository. A selected directory must contain the supported HOODLUM `gta-sa.EXE`; the repository's `game/clean` copy is rejected. **Choose GTA V directory** locates an existing GTA V Enhanced installation without copying game files. It checks `GTA5_Enhanced.exe`, x64 architecture, and Rockstar version metadata, then displays the version. Successful choices are saved under `HKCU\Software\GTAPortal\PortalDev` and restored on the next launch. A missing or invalid saved directory stays visible for correction.

For the GTA V compatibility probe, use a dedicated Enhanced Story Mode development installation with Script Hook V and its ASI loader. With the game closed, click **部署 V + FX**, then start Enhanced in Story Mode. PortalDev should show V connected and changing camera data while the game runs; **打开 V 日志** opens `PortalV.log`. The tool checks the selected game executable, `ScriptHookV.dll`, and `dinput8.dll` before deployment. It displays file-version differences but does not block deployment based on them. Nothing in this tool prepares a modded installation for Online.

After deploying protocol v5 once, shader-only edits no longer require a GTA V restart. Build or edit `PortalSA.fx` while both games remain open, then click **热更新 FX**: PortalDev copies only the shader and PortalV asks ReShade to recompile it on the next frame. Native ASI changes still require closing the corresponding game because those DLLs own live Script Hook V, RenderWare, D3D hooks and callbacks; forcibly unloading them is intentionally not supported.

The D3D9Ex shared-texture bridge tried earlier caused a game startup crash. Stage 1 (hook installation) and stage 2 (a proxy forwarding to the ordinary device) both reached gameplay successfully. The current **D3D9Ex stage 3** checkbox keeps capability queries on the legacy factory but changes `CreateDevice` to `CreateDeviceEx` on the next SA launch. The archived SA producer and shared-texture allocation remain disabled, isolating Ex-device compatibility. The working SA window preview does not use it.

## GTA V window producer

When the selected GTA V Enhanced process has a visible window, PortalDev captures it with Windows Graphics Capture without displaying another copy in the Dev window. It queues the texture into a three-entry asynchronous GPU readback ring and publishes only completed frames to the V→SA latest-frame CPU mailbox.

GTA SA's consumer uses a producer-owned control mapping plus a dynamically sized three-slot CPU pixel mailbox. PortalDev rotates through three D3D11 staging textures and checks GPU event queries without blocking; completed frames replace only free or older ready mailbox slots. SA always takes the newest ready slot and uploads it through its own three-texture D3D9 ring. This avoids the unsupported ordinary-D3D9/D3D11 shared-surface path and does not require both APIs to share a resource handle. This is still the visible GTA V window, not an offscreen portal camera.

For acceptance, build, deploy both sides, and start both games. In SA gameplay, press F9 and check that the GTA V picture remains fixed as the camera moves, is hidden by SA walls/characters/vehicles in front, and disappears about 500 ms after V capture stops. Check PortalDev's V→SA status and `portal.log` if the picture is missing.

The Portal Camera calibration section edits the SA exit X/Y/Z/Yaw, captures the current SA player position as a starting exit centre, saves it to `PortalCalibration.txt`, and enables either direction of camera mapping. V-to-SA uses GTA V's final rendered camera look direction and FOV, never the player position, while keeping the SA camera position fixed at the live SA circle centre. SA-to-V reads SA's live main-camera look direction and horizontal FOV, never CJ's position, converts the FOV for GTA V, and drives a GTA V scripted camera above the V circle centre by a separate 1.2 m viewing offset. This offset does not modify or resave either calibrated portal. The two camera directions are mutually exclusive to prevent feedback. While SA's F9 following mode is active, its centre and yaw follow CJ without repeated file writes. The two visibility checkboxes independently show the SA picture in GTA V and the GTA V picture in SA; the defaults show only the GTA V-side portal to avoid recursive mirror images.

To build all three Release targets and run the five synthetic smoke tests without launching either game:

```powershell
& '.\src\Build-Dev.ps1' -RunTests
```

`SAFramePublisherSmoke` checks the SA→V named three-slot GPU channel. `D3D11D3D12BridgeSmoke` checks the GPU copy and fence used by its GTA V consumer. `VToSACpuBridgeSmoke` checks dynamic resolution, asynchronous D3D11 staging completion, latest-frame overwrite, and pixel integrity through the V→SA CPU mailbox. `SAOffscreenCpuBridgeSmoke` checks that PortalDev consumes the archived PortalSA-owned offscreen mailbox and uploads exact BGRA pixels to D3D11. `CameraProjectionSmoke` checks the fixed 960×540 FOV conversion at several narrow and wide source FOVs. These tests do not replace the still-required in-game circular clipping, occlusion, latency, and frame-pacing acceptance.
