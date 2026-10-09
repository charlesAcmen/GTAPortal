# PortalDev

`PortalDev.exe` is the existing GTA SA development tool. It builds and deploys `PortalSA.asi` to the dev game, shows its log, and previews the game's window with Windows Graphics Capture. It does not inject code: Ultimate ASI Loader loads the ASI when GTA SA starts.

The GPU checkbox controls capture in PortalDev and can be switched while the game runs. GTA SA keeps its native D3D9 device. The preview copies captured D3D11 textures to its swap chain on the GPU; this code does not read full images into CPU memory or save live BMP files. The displayed FPS counts frames received and presented by PortalDev. It is not a measurement of end-to-end portal latency or a guarantee at 1080p.

## Build and run

Close PortalDev before rebuilding. Double-click `src/Build-Dev.cmd` in File Explorer and wait for `BUILD SUCCEEDED`. From PowerShell at the repository root, the equivalent is:

```powershell
& '.\src\Build-Dev.ps1'
```

Open `src/PortalDev/bin/x64/Release/PortalDev.exe`. Click **Deploy and open directory**, then start `gta-sa.EXE` in that dev game directory. PortalDev detects the game window automatically. The status should show **GPU 窗口捕获正常**, a growing frame count, capture FPS, and image dimensions; the preview should show moving game content. The log should include `PortalSA loaded` and `Render output hook installed`. State records appear after entering gameplay.

The dev game can also be started before PortalDev; window capture starts when PortalDev finds it. The ASI itself must already have been deployed before game startup. Only `game/dev/GTA San Andreas` is used for deployment; `game/clean` is left alone.

The D3D9Ex shared-texture bridge tried earlier caused a game startup crash and is no longer activated. Its experimental source remains in the tree for diagnosis, but the working preview does not use it. Cross-process GPU texture publication and sustained high-resolution throughput remain separate milestones.
