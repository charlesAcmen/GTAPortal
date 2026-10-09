# PortalDev

`PortalDev.exe` is the existing GTA SA development tool. It builds and deploys `PortalSA.asi` to the dev game, shows its log, and previews the game's window with Windows Graphics Capture. It does not inject code: Ultimate ASI Loader loads the ASI when GTA SA starts.

The GPU checkbox controls capture in PortalDev and can be switched while the game runs. GTA SA keeps its native D3D9 device. The preview copies captured D3D11 textures to its swap chain on the GPU; this code does not read full images into CPU memory or save live BMP files. Displayed FPS counts frames received and presented by PortalDev. The new frame-age value measures compositor timestamp to the tool's Present call, and "skipped old frames" counts only frames explicitly discarded while draining the capture pool. These are not game render latency or total frames lost by Windows.

## Build and run

Close PortalDev before rebuilding. Double-click `src/Build-Dev.cmd` in File Explorer and wait for `BUILD SUCCEEDED`. From PowerShell at the repository root, the equivalent is:

```powershell
& '.\src\Build-Dev.ps1'
```

Open `src/PortalDev/bin/x64/Release/PortalDev.exe`. Click **Deploy and open directory**, then start `gta-sa.EXE` in that dev game directory. PortalDev detects the game window automatically. The status should show **GPU 窗口捕获正常**, a growing frame count, capture FPS, and image dimensions; the preview should show moving game content. The log should include `PortalSA loaded` and `Render output hook installed`. State records appear after entering gameplay.

The dev game can also be started before PortalDev; window capture starts when PortalDev finds it. The ASI itself must already have been deployed before game startup. Deployment targets the selected SA development directory and never the repository's `game/clean` copy.

The **Choose SA directory** button opens a modal folder picker. The default remains `game/dev/GTA San Andreas` relative to the repository. A selected directory must contain the supported HOODLUM `gta-sa.EXE`; the repository's `game/clean` copy is rejected. **Choose GTA V directory** locates an existing GTA V Enhanced installation without copying game files. It checks `GTA5_Enhanced.exe`, x64 architecture, and Rockstar version metadata, then displays the version. Successful choices are saved under `HKCU\Software\GTAPortal\PortalDev` and restored on the next launch. A missing or invalid saved directory stays visible for correction.

For the GTA V compatibility probe, install Script Hook V and its ASI loader in the selected GTA V Enhanced directory. With the game closed, click **Deploy V ASI**, then start Enhanced in Story Mode. A moving pink and blue test overlay appeared on this workstation on 2026-10-09; PortalDev should also show V connected and changing camera data while the game is running. **Toggle V test image** controls the overlay, **Open V log** opens `PortalV.log`, and **Disable V ASI** renames only our plugin after the game closes. The tool checks the selected game executable, `ScriptHookV.dll`, and `dinput8.dll` before deployment. It displays file-version differences but does not block deployment based on them. The installed game's file version is `1.0.1158.16` and the installed runtime DLL version ends in `1158.13`; the local Script Hook V log nevertheless reports successful initialization and launch of PortalV's main thread. This observed compatibility does not establish future-patch compatibility. The overlay is a native shape test and does not receive SA image textures.

The D3D9Ex shared-texture bridge tried earlier caused a game startup crash and is no longer activated. Its experimental source remains in the tree for diagnosis, but the working SA preview does not use it. GTA V now has an external shared-texture producer; game-side consumption and sustained high-resolution throughput still need runtime verification.

## GTA V window producer

The **V GPU window producer** checkbox is independent of the `PortalV.asi` script. When the selected GTA V Enhanced process has a visible window, PortalDev captures that window with Windows Graphics Capture, previews it below the SA preview, and copies each latest frame into one of three named D3D11 textures. The frame control mapping is `Local\GTAPortal.VFrames.v1` and its layout is in `src/PortalVFrames.h`. Each texture uses a D3D11 NT shared handle and keyed mutex; a future D3D11 consumer can open the name in the slot and acquire key `1` after claiming `Ready`, then release key `0` and mark the slot `Free`. The producer reclaims the oldest `Ready` slot if all three are occupied, so the producer does not build an unbounded frame queue. It never overwrites a slot marked `Reading`.

The tool reports published, overwritten, and skipped frames. These count the producer's three-slot queue only; they do not measure frames lost inside the game or Windows compositor. The metadata includes the compositor timestamp, dimensions, format, adapter LUID, producer PID, and generation. On resize, the producer recreates its textures and advances the generation. This is a GPU frame producer and a tool preview, not yet GTA SA's game-side consumer or an offscreen GTA V camera. Keep the selected GTA V game on the same GPU adapter as the future consumer.

To check the GPU sharing path without launching GTA V, build and run the synthetic texture smoke test:

```powershell
& 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe' '.\src\tests\VFramePublisherSmoke.vcxproj' '/p:Configuration=Release' '/p:Platform=x64'
& '.\src\tests\bin\x64\Release\VFramePublisherSmoke.exe'
```
