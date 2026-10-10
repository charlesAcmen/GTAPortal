# Source audit (2026-10-10)

## Inventory

The `main` branch stores production source under the repository-root `src/` directory. After the diagnostic split, the audit counted 54 source, project, script, configuration, and documentation files with 7,179 lines in total. The executable implementation is concentrated in 17 `.cpp` files (3,938 lines), 17 headers, and one ReShade effect (86 lines).

| Area | Responsibility | Approximate implementation size |
| --- | --- | ---: |
| `PortalDev` | Window capture, preview, deployment UI, frame publication, status and path validation | 2,112 C++ source lines plus headers |
| `PortalSA` | SA state/camera probe, V→SA consumer, optional legacy D3D9Ex experiment | 1,023 C++ source lines plus headers |
| `PortalV` | V camera state, ReShade registration, SA→V compositor | 552 C++ source lines plus headers and 86 shader lines |
| Shared protocols | Four small cross-process control layouts | 212 header lines |
| Tests | Named-texture publisher and D3D11→D3D12 bridge smoke tests | 263 C++ source lines |

## Active data flows

### SA window to GTA V

```text
SA window
  -> Windows Graphics Capture returns a GPU texture to PortalDev
  -> PortalDev copies the newest frame into one of three named GPU textures
  -> a 752-byte shared control block publishes slot state, frame id, size and texture name
  -> PortalV opens the selected GPU texture and copies it into one of three GTA-V-side display textures
  -> a GPU fence orders that copy before ReShade samples the texture
  -> the shader projects the picture onto a world-fixed plane and compares it with GTA V scene depth
```

The full-resolution pixels do not pass through CPU memory. The CPU only updates the small control block. A keyed mutex protects each source texture, and a fence protects reuse of GTA V display textures.

Evidence: both producer channels and the D3D11→D3D12 bridge pass their synthetic hardware tests. GTA V previously registered the add-on and received SA frames. The visible plane and depth occlusion still need in-game acceptance with the ReShade technique enabled.

### GTA V window to SA

```text
GTA V window
  -> Windows Graphics Capture returns a BGRA GPU texture to PortalDev
  -> PortalDev makes a shader-readable GPU copy
  -> a fullscreen GPU draw scales/converts it into one of three 1280x720 textures owned by SA
  -> a 168-byte shared control block publishes slot state, frame id and completion time
  -> SA claims the newest completed slot and samples it while drawing a world rectangle
  -> an SA GPU completion query keeps the slot protected until drawing finishes
```

This path also avoids full-frame CPU readback. Unlike the SA→V source pool, the legacy D3D9/D3D11 shared textures do not use a keyed mutex; the slot state and GPU completion queries provide the ownership handshake. The code builds, but this complete path and its scene-depth behavior have not been accepted in-game.

## Other code around the frame paths

- `PortalSA/StateCapture` reads player position, camera basis, FOV, clip planes and the current render matrices.
- `PortalV/Main` reads GTA V camera state, stores the F9 plane anchor, and publishes a tiny seqlock-style state snapshot to PortalDev.
- `PortalDev/Main` validates game directories and versions, deploys or disables the ASIs, tails logs, discovers windows, shows previews and reports counters.
- `GamePaths` and `GameProcess` isolate filesystem/version checks and process/window discovery.
- `PortalProtocol.h` and the old SA-internal producer/consumer now live behind `PortalSA/source/legacy/diagnostics` and `PortalDev/source/legacy/diagnostics`. They are preserved diagnostic scaffolding, not the normal SA→V path.

## Audit findings and priorities

1. **Finish acceptance before architectural rewrites.** Both directions still need fixed-scene in-game checks for placement, depth, stale-frame hiding, resize, minimize, restart and device reset.
2. **The unused VFrames output has been removed.** `Publication::V` now previews the capture and writes only the active `VToSA` path. This removes one full-frame GPU copy and three full-size textures without changing the Dev preview.
3. **Legacy diagnostics are isolated.** The old SA-internal producer, D3D9Ex creation bridge, and PortalDev legacy preview consumer have moved under clearly named `legacy/diagnostics` modules without changing the normal capture paths.
4. **`PortalDev/source/Main.cpp` still owns several UI responsibilities.** The first split reduced it from 1,028 to 844 lines by removing the legacy GPU consumer. A later UI-only split can separate deployment commands, status composition and window layout after current acceptance.
5. **The reverse bridge needs its own automated test.** Current tests cover named D3D11 textures and D3D11→D3D12, not D3D11 writing into SA-owned D3D9 textures or the SA draw/depth stage.
6. **Developer verification should be one command.** `Build-Dev.ps1 -RunTests` now builds all Release targets and runs both existing hardware smoke tests.

## Verification performed

- `PortalSA.asi`, `PortalV.asi`, and `PortalDev.exe`: Release build passed on 2026-10-10.
- `SAFramePublisherSmoke`: SA→V channel passed with `produced=12 overwritten=7`.
- `D3D11D3D12BridgeSmoke`: passed with expected color `FF2468AC`.
- Build warning in `VConsumer.cpp`: removed by using the bounds-checked CRT scan entry point.

These checks prove compilation and isolated GPU transport behavior. They do not prove that either portal is visibly correct inside both games.
