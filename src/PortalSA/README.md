# PortalSA

`PortalSA.asi` targets GTA San Andreas 1.0 US HOODLUM (Win32/x86) in `game/dev/GTA San Andreas`. It logs player and camera state through plugin-sdk and observes `RwCameraShowRaster`. The earlier periodic BMP was a frame-access proof and has been removed. The active GPU preview is now captured by `PortalDev.exe` from the game window while GTA SA keeps its native D3D9 device.

## Build and deploy

Close GTA SA and PortalDev, then double-click `src/Build-Dev.cmd`. It builds the ASI and the existing x64 PortalDev tool. From PowerShell at the repository root:

```powershell
& '.\src\Build-Dev.ps1'
```

Start `src/PortalDev/bin/x64/Release/PortalDev.exe`, click **Deploy and open directory**, and start `gta-sa.EXE` there. The tool checks the dev EXE hash and copies only to the dev game. Ultimate ASI Loader loads the ASI at game startup. PortalDev can discover a running game for window capture, but it cannot inject an ASI into a game that started without one.

Expected log entries include `PortalSA loaded: GTA SA 1.0 US HOODLUM`, `PortalSA startup: native D3D9 mode`, and `Render output hook installed at RwCameraShowRaster`. In gameplay, `state tick_ms=` records appear approximately every 100 ms. Player and camera values should change when moving and turning. `render_view_row_major` and `render_projection_row_major` are logged when a recent world render provided them. No live BMP file is expected.

The old D3D9Ex bridge and shared-texture producer source is retained as a disabled experiment. Enabling that bridge made this game crash during startup; it is not part of the working GPU preview. The current shared mapping in PortalDev is control scaffolding, not a working frame producer from the ASI.

## Optional windowed development setup

After initializing the pinned submodules, install WindowedMode into the dev game only:

```powershell
git submodule update --init --recursive
& '.\src\Install-DevWindowedMode.ps1'
```

The tracked default is a 1600×900 window with `autoPause=0`. The installer also sets the current user's high-DPI compatibility entry for the dev `gta-sa.EXE` path; the clean game's setting does not apply to the dev path. The game's EXE is not modified. WindowedMode may save the last window size and position in the deployed INI.

The captured dimensions in PortalDev include the window area reported by Windows Graphics Capture. A 1600×900 game window is 900p, not 1080p. The preview FPS measures this workstation's current capture and display loop, not the future GTA V portal path.
