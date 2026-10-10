# PortalV: SA picture inside GTA V Enhanced

`PortalV.asi` has two jobs in Story Mode: the Script Hook V script publishes the GTA camera state and anchors a 16:9 picture plane; its ReShade add-on reads the latest GTA SA window frame from PortalDev and draws that frame on the plane with GTA depth occlusion. PortalDev captures SA through Windows Graphics Capture (WGC). Neither producer nor consumer reads a full frame back to the CPU. The older native SA D3D9Ex bridge is still disabled.

## Build and deploy

Build `src/PortalV/PortalV.vcxproj` and `src/PortalDev/PortalDev.vcxproj` as Release x64, or use `src/Build-Dev.ps1`. PortalDev's **部署 V + FX** copies `PortalV.asi` and `src/PortalV/shaders/PortalSA.fx` into the selected GTA V Enhanced installation. Use a dedicated Story Mode development installation; do not deploy to the installation used for GTA Online. The game must be closed before deployment. The app does not copy the game or overwrite a ReShade preset. **仅停用 V 插件** renames only `PortalV.asi`; it does not remove Script Hook V, the ASI loader, ReShade, or shader files and is not an Online safety switch.

The game needs Script Hook V, an ASI loader, and an add-on capable ReShade D3D12 runtime. The pinned ReShade source and official full add-on download are version 6.8.0 as checked on 2026-10-09: https://reshade.me/. `ReShade64.asi` plus `ReShade.fxh` in `reshade-shaders/Shaders` is the reference project's Enhanced setup; a working D3D12 ReShade installation is also suitable. PortalDev reports whether it detects a runtime, but the ReShade overlay/log is the actual loading check. In ReShade, enable the `PortalSA` technique from `PortalSA.fx`, and choose the GTA scene depth buffer if ReShade does not select it automatically. Do not replace an existing user's preset just to enable this technique.

## Place and test the picture

Run PortalDev, enable SA GPU window capture, then start SA and GTA V Story Mode. At the desired Grove Street location, look in the direction the picture should face and press **F9 once** in GTA V. This saves the plane centre about 3 metres ahead of the final rendered camera in `PortalVPlane.txt` beside the ASI. The plane is 2.4 m wide and 1.35 m high. Moving away should keep it fixed in the world. To reposition it, look from the new intended centre and press F9 again. The saved file contains four floats: X, Y, Z and yaw in degrees. The moving diagnostic HUD is off by default; PortalDev can enable it temporarily.

The effect linearly samples SA when the image is near native screen size; at smaller sizes it uses four bilinear taps to reduce aliasing. GTA depth hides the plane where GTA geometry is closer. If ReShade does not report an available scene depth buffer, the plane is hidden rather than drawn through GTA objects. ReShade exposes `HostReversedZ` and `DepthBias` for calibration. The effect draws after GTA's rendering, so GTA still renders pixels behind the plane. A later portal implementation would need a render integration point to avoid that work.

The first user-confirmed Grove Street test anchor is `X=96.491821, Y=-1932.880859, Z=21.770508, yaw=-133.509491°` in the local `PortalVPlane.txt`. F9 was confirmed in `PortalV.log` and the add-on received frames; if no picture appears, first enable the `PortalSA` technique in ReShade. F9 positions the plane; it does not toggle the effect.

## Evidence and remaining checks

The x64 ASI and PortalDev build. `VFramePublisherSmoke` verifies both SA and V three-slot channels; `D3D11D3D12BridgeSmoke` verifies a D3D11 GPU copy into a D3D12 shared resource using a GPU fence. That test reads one 16×16 result back **only for verification**; the actual producer/consumer path has no full-frame CPU readback. The pinned ReShade FX compiler generated DXBC for both shader stages using the reference project's `ReShade.fxh`. The complete game path and depth calibration still require in-game acceptance.

Check that ReShade shows `PortalSA.fx` without errors, SA appears on the plane, a GTA wall/pedestrian occludes it as expected, moving the GTA camera keeps the plane stationary, and the image disappears within about 500 ms if SA capture stops. Then record both games' frame times, VRAM use, PortalDev's produced/overwritten/skipped counters, and ReShade's `PortalV SA received/skipped` log while both games run. Test 1080p and 720p with fixed scenes before selecting the next performance target.
