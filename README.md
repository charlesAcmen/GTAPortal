# GTAPortal

GTAPortal explores a real-time portal between the classic PC releases of GTA San Andreas and GTA V. The current implementation is **milestone 1 on the GTA SA side only**: a Win32 ASI that logs player/camera state and periodically saves the final D3D9 frame. See [the PortalSA instructions](src/PortalSA/README.md) for build, deployment, and manual acceptance steps. It has not yet been compiled or tested in-game.

## Clone on another device

Clone the main repository together with its pinned dependencies:

```powershell
git clone --recurse-submodules https://github.com/charlesAcmen/GTAPortal.git
Set-Location GTAPortal
git submodule status
```

If you already cloned without `--recurse-submodules`, run this from the repository root:

```powershell
git submodule update --init --recursive
```

The main repository records exact submodule commits. A regular `git pull` does not necessarily populate or update their working trees; run `git submodule update --init --recursive` after pulling changes to `.gitmodules` or the submodule pointers. Do not use `git submodule update --remote` for a reproducible build unless you intentionally plan to change the pinned versions.

| Path | Purpose | Pinned commit |
| --- | --- | --- |
| `third_party/plugin-sdk/plugin-sdk` | GTA SA headers, events, hook implementation, and SDK library build | `15f15b60bbf74c106e1b496ff92c98764abf4605` |
| `reference/gta-reversed` | Read-only game implementation reference; not linked into PortalSA | `01709e8eefdf0b4871f33a8eab19dfa0716d509f` |

The local GTA SA installation is **not** part of the repository or a submodule. Keep your own clean copy under `game/clean/GTA San Andreas` and use `game/dev/GTA San Andreas` for plugin testing. Only the dev copy should receive `PortalSA.asi`. The project currently targets the GTA SA 1.0 US HOODLUM x86 executable; the executable and game assets must be supplied locally.

## Repository layout

- `src/PortalSA/`: milestone 1 source and Visual Studio project.
- `src/docs/commands.txt`: original project requirements.
- `src/docs/PROJECT_RESOURCES.md`: resource inventory and architectural analysis.
- `.gitmodules`: dependency URLs and paths.
- `.gitignore`: tracks project files and submodule pointers while excluding game files, local tools, and build output.

The GitHub repository is private. Sign in to GitHub on the other device with an account that has access before cloning it.
