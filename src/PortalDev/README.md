# PortalDev

`PortalDev.exe` is the external development dashboard for the GTA SA side of GTAPortal. It does not inject code by itself. GTA San Andreas loads `PortalSA.asi` through the already installed Ultimate ASI Loader when the game starts.

Milestone 1 was accepted in the real game on 2026-10-08: injection, state logging, periodic BMP replacement, and the dual-monitor windowed workflow all succeeded.

The utility currently provides:

- exact MD5 validation of the dev `gta-sa.EXE`;
- one-click build entry through `src/Build-Dev.ps1`;
- guarded ASI deployment only to `game/dev/GTA San Andreas`;
- one-click ASI disable/rename for recovery after a failed hook experiment;
- game launch with the correct working directory;
- live status for ASI load, render hook, state records, and frame records;
- a continuously refreshed tail of `portal.log`;
- shortcuts to the log, captured BMP, and dev game directory.

Build from the repository root:

```powershell
& '.\src\Build-Dev.ps1'
```

Run:

```powershell
& '.\src\PortalDev\bin\x64\Release\PortalDev.exe'
```

For a new workstation, initialize submodules and install the optional windowed-mode helper before dual-monitor testing:

```powershell
git submodule update --init --recursive
& '.\src\Install-DevWindowedMode.ps1'
```

The tool is deliberately external to the game. Later milestones can add shared-memory state, frame age, camera request IDs, and performance counters here without putting UI and input hooks into GTA SA's render thread. An in-game overlay can still be added later when portal placement or direct visual debugging makes it useful.
