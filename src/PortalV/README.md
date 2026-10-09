# PortalV

`PortalV.asi` is an x64 Script Hook V compatibility probe for GTA V Enhanced Story Mode. It publishes a latest-only camera snapshot through `Local\GTAPortal.VState.v1` and draws an animated native test overlay. `PortalDev.exe` reads the snapshot and can toggle the overlay. No image pixels or GPU textures are shared by this protocol.

The GTA V window frame producer lives in PortalDev and uses Windows Graphics Capture; it does not depend on this ASI. The two paths can be tested separately while the installed Script Hook V runtime lags the game patch.

Build with `src/Build-Dev.ps1`; the local `third_party/ScriptHookV_SDK_1.0.617.1a/` headers and library are required. The Script Hook V runtime and ASI loader must be supplied separately and placed in the game directory. PortalDev deploys only our ASI after validating the selected Enhanced executable and ensuring that the game is closed. On 2026-10-09 the local game launched the script main thread and showed the moving overlay even though the game EXE file version and runtime DLL file version differ.

Observed first-run evidence: `PortalV.log` says `PortalV script started`, Script Hook V's log shows the script thread launching, and the moving native overlay appeared in Story Mode. PortalDev's live camera snapshot still needs an explicit moving-camera acceptance check. If a future game update prevents loading, disable the plugin with PortalDev after closing the game and keep the loader's diagnostic text for investigation.
