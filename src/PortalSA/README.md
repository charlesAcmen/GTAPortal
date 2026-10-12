# PortalSA

`PortalSA.asi` targets GTA San Andreas 1.0 US HOODLUM (Win32/x86) in `game/dev/GTA San Andreas`. It logs player and camera state through plugin-sdk and observes `RwCameraShowRaster`. The earlier periodic BMP was a frame-access proof and has been removed. The active GPU preview is now captured by `PortalDev.exe` from the game window while GTA SA keeps its native D3D9 device.

For development, PortalSA waits until normal gameplay with CJ has remained available for two seconds, then automatically teleports CJ or his current vehicle once to `(2468.331787, -1661.175537, 13.321789)` with heading `-1.892188 rad` (`-108.414386°`). It loads exterior scene collision before moving and records completion in `portal.log`. This spawn is separate from the saved `PortalSAPlane.txt` picture rectangle and from PortalDev's camera-calibration exit.

## Build and deploy

Close GTA SA and PortalDev, then double-click `src/Build-Dev.cmd`. It builds the ASI and the existing x64 PortalDev tool. From PowerShell at the repository root:

```powershell
& '.\src\Build-Dev.ps1'
```

Start `src/PortalDev/bin/x64/Release/PortalDev.exe`, click **Deploy and open directory**, and start `gta-sa.EXE` there. The tool checks the dev EXE hash and copies only to the dev game. Ultimate ASI Loader loads the ASI at game startup. PortalDev can discover a running game for window capture, but it cannot inject an ASI into a game that started without one.

Expected log entries include `PortalSA loaded: GTA SA 1.0 US HOODLUM`, `PortalSA startup: native D3D9 mode`, and `Render output hook installed at RwCameraShowRaster`. In gameplay, `state tick_ms=` records appear approximately every 100 ms. Player and camera values should change when moving and turning. `render_view_row_major` and `render_projection_row_major` are logged when a recent world render provided them. No live BMP file is expected.

For portal-camera calibration, move CJ to the intended SA Grove Street development position and press **F8 while SA is in the foreground**. The plugin writes a high-precision `SA_CALIBRATION_MARKER` containing the player position, heading, camera position and camera forward vector. Normal state records continue as before, so the final gameplay position also remains recoverable from `portal.log`.

PortalDev can drive the SA main camera through `Local\\GTAPortal.PortalCamera.v1`. This is a fixed-camera prototype: it updates source, target and up every game tick, uses SA's fixed-mode FOV of 70 degrees, and automatically calls `RestoreWithJumpCut` when disabled or when commands are stale for more than 500 ms. It is not yet an offscreen second camera.

## GTA V picture in the SA world

The V consumer opens the three-slot latest-frame CPU mailbox described by `src/VToSAFrames.h`. PortalDev performs non-blocking D3D11 staging readback at the captured client resolution; SA copies only the newest completed BGRA frame into one of three local system-memory textures and queues `UpdateTexture` into a native D3D9 texture. The upload ring is protected by D3D9 event queries, so no texture is reused while the GPU still consumes it. No D3D9Ex device replacement or cross-API shared surface is required. The ASI draws the latest frame as a vertical world rectangle with its height derived from the current frame aspect ratio and depth testing after the SA scene render. With SA in the foreground, press F9 to place it about 3 m in front of CJ along his heading, centred 1.5 m above his position. Placement is saved beside the game EXE in `PortalSAPlane.txt`. If V frames stop for 500 ms, the rectangle is hidden. Build and CPU-mailbox smoke validation are automated, and the user confirmed the visible world rectangle in SA on 2026-10-12. Occlusion edge cases, latency and long-run stability still require targeted tests. The previous camera-relative placement overlapped CJ because the third-person camera was already about 3 m behind him.

The mailbox carries GTA V color pixels only. SA occludes the world rectangle with its own currently bound D3D9 scene depth surface; no GTA V depth frame is transferred or needed for this host-side test. The pane draw clears inherited stencil masks, clip planes and depth bias, keeps SA's active Z/W-buffer convention, and leaves depth writes disabled. The first draw after F9 logs the observed depth format and render states in `portal.log` so remaining in-game depth issues can be diagnosed.

Native D3D9 sharing is tried first. PortalDev shows the SA consumer stage and HRESULT if texture creation/opening fails. The old D3D9Ex bridge previously caused a startup crash. Stages 1 and 2 cleared the hook and ordinary-device proxy layers. The current **D3D9Ex stage 3** checkbox changes only device creation to `CreateDeviceEx` on the *next* SA launch; capability queries remain legacy-compatible and the archived producer stays disabled. Use it only in the dev copy. Reaching gameplay will clear Ex-device creation before stage 4 allocates shared textures.

## Optional windowed development setup

After initializing the pinned submodules, install WindowedMode into the dev game only:

```powershell
git submodule update --init --recursive
& '.\src\Install-DevWindowedMode.ps1'
```

The tracked default is a 1600×900 window with `autoPause=0`. The installer also sets the current user's high-DPI compatibility entry for the dev `gta-sa.EXE` path; the clean game's setting does not apply to the dev path. The game's EXE is not modified. WindowedMode may save the last window size and position in the deployed INI.

The captured dimensions in PortalDev include the window area reported by Windows Graphics Capture. A 1600×900 game window is 900p, not 1080p. The preview FPS measures this workstation's current capture and display loop, not the future GTA V portal path.
