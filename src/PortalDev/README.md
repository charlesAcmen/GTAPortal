# PortalDev

`PortalDev.exe` is the existing GTA SA development tool. It builds and deploys `PortalSA.asi` to the dev game, shows its log, and previews the game's window with Windows Graphics Capture. It does not inject code: Ultimate ASI Loader loads the ASI when GTA SA starts.

The GPU checkbox controls capture in PortalDev and can be switched while the game runs. GTA SA keeps its native D3D9 device. The preview copies captured D3D11 textures to its swap chain on the GPU; this code does not read full images into CPU memory or save live BMP files. Displayed FPS counts frames received and presented by PortalDev. The new frame-age value measures compositor timestamp to the tool's Present call, and "skipped old frames" counts only frames explicitly discarded while draining the capture pool. These are not game render latency or total frames lost by Windows.

## Build and run

Close PortalDev before rebuilding. Double-click `src/Build-Dev.cmd` in File Explorer and wait for `BUILD AND TESTS SUCCEEDED`. From PowerShell at the repository root, the equivalent is:

```powershell
& '.\src\Build-Dev.ps1' -RunTests
```

Open `src/PortalDev/bin/x64/Release/PortalDev.exe`. Click **Deploy and open directory**, then start `gta-sa.EXE` in that dev game directory. PortalDev detects the game window automatically. The status should show **GPU 窗口捕获正常**, a growing frame count, capture FPS, and image dimensions; the preview should show moving game content. The log should include `PortalSA loaded` and `Render output hook installed`. State records appear after entering gameplay.

The dev game can also be started before PortalDev; window capture starts when PortalDev finds it. The ASI itself must already have been deployed before game startup. Deployment targets the selected SA development directory and never the repository's `game/clean` copy.

The **Choose SA directory** button opens a modal folder picker. The default remains `game/dev/GTA San Andreas` relative to the repository. A selected directory must contain the supported HOODLUM `gta-sa.EXE`; the repository's `game/clean` copy is rejected. **Choose GTA V directory** locates an existing GTA V Enhanced installation without copying game files. It checks `GTA5_Enhanced.exe`, x64 architecture, and Rockstar version metadata, then displays the version. Successful choices are saved under `HKCU\Software\GTAPortal\PortalDev` and restored on the next launch. A missing or invalid saved directory stays visible for correction.

For the GTA V compatibility probe, use a dedicated Enhanced Story Mode development installation with Script Hook V and its ASI loader. With the game closed, click **部署 V + FX**, then start Enhanced in Story Mode. A moving pink and blue test HUD appeared on a previous workstation on 2026-10-09; it is now off by default. PortalDev should show V connected and changing camera data while the game runs. **切换测试 HUD** enables the diagnostic shape temporarily, **打开 V 日志** opens `PortalV.log`, and **仅停用 V 插件** renames only our ASI after the game closes. This does not remove Script Hook V, its loader, ReShade, or shaders and cannot prepare the installation for Online. The tool checks the selected game executable, `ScriptHookV.dll`, and `dinput8.dll` before deployment. It displays file-version differences but does not block deployment based on them. The previously tested game file version was `1.0.1158.16` and the runtime DLL version ended in `1158.13`; the local Script Hook V log nevertheless reported successful initialization and launch of PortalV's main thread. That observation does not establish future-patch compatibility. The HUD is a native shape diagnostic and does not receive SA image textures.

The D3D9Ex shared-texture bridge tried earlier caused a game startup crash. Stage 1 (hook installation) and stage 2 (a proxy forwarding to the ordinary device) both reached gameplay successfully. The current **D3D9Ex stage 3** checkbox keeps capability queries on the legacy factory but changes `CreateDevice` to `CreateDeviceEx` on the next SA launch. The archived SA producer and shared-texture allocation remain disabled, isolating Ex-device compatibility. The working SA window preview does not use it.

## GTA V window producer

The **V GPU window producer** checkbox is independent of the `PortalV.asi` script. When the selected GTA V Enhanced process has a visible window, PortalDev captures that window with Windows Graphics Capture and previews that captured texture directly in the Dev window. The preview copies that captured texture straight to the Dev window; it does not consume a separate shared-frame channel. The old unused `Local\GTAPortal.VFrames.v1` diagnostic output has been removed. For the actual V→SA path, PortalDev converts the captured V frame entirely on the GPU into one of three textures owned by SA.

GTA SA's candidate consumer uses `Local\GTAPortal.VToSA.v1` and three GPU textures owned by the ASI. The slot states prevent PortalDev from overwriting a texture while SA is reading it. Keep both games and PortalDev on the same GPU adapter. This is still the visible GTA V window, not an offscreen portal camera.

For acceptance, build with `& '.\src\Build-Dev.ps1'`, deploy SA to `game/dev/GTA San Andreas`, enable the V GPU window producer, and start both games. In SA gameplay, press F9 and check that the GTA V picture remains fixed as the camera moves, is hidden by SA walls/characters/vehicles in front, and disappears about 500 ms after V capture stops. Check PortalDev's SA consumer status and `portal.log` if the picture is missing. If native sharing fails, enable D3D9Ex diagnostic and restart SA; first verify SA reaches gameplay. No code path or checkbox here prepares a modded GTA V installation for Online.

The Portal Camera calibration section edits the SA exit X/Y/Z/Yaw, captures the current SA player position as a starting exit centre, saves it to `PortalCalibration.txt` beside the dev game, and enables the V-to-SA camera mapping. The mapping uses GTA V's final rendered camera and the accepted V Grove Street plane. Disable the checkbox to restore the normal SA camera; loss of PortalDev commands also restores it automatically. The current implementation intentionally controls SA's main camera and keeps fixed-mode FOV 70, so it proves pose/parallax before a later offscreen-camera milestone.

To build all three Release targets and run the two synthetic GPU smoke tests without launching either game:

```powershell
& '.\src\Build-Dev.ps1' -RunTests
```

`SAFramePublisherSmoke` checks the SA→V named three-slot channel. `D3D11D3D12BridgeSmoke` checks the GPU copy and fence used by its GTA V consumer. Neither test replaces the still-required in-game V→SA rectangle and depth acceptance.
