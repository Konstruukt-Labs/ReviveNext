# ReviveNext

Play your Meta Horizon (Oculus Rift-platform) PC games in SteamVR — listed,
launched, and badged natively in SteamVR's own Library.

ReviveNext discovers the Rift-platform games installed on your machine and
registers each one as a SteamVR application, so they appear in SteamVR's
Library (Other Apps) with cover art and launch through SteamVR's normal
app flow. Launching a game runs it through Revive's proven
[LibOVR → OpenVR/OpenXR translation layer](revive/), which is built from
source here — no Oculus PC software needs to be running beyond what the
games' entitlement check talks to.

## How it works

- **Game discovery** — scans Meta Horizon's local install (registry +
  per-game manifests), independent of whether the Meta Horizon software
  itself is running.
- **SteamVR registration** — every game is registered as a
  `revive.app.<game>` application via `AddApplicationManifest`, with its
  matched art and Revive's action manifest attached. Launching from
  SteamVR's Library gives the full app-launch flow: loading screen while
  the game starts, the game running under its registered identity, and
  the dashboard afterwards.
- **Self-maintaining** — ReviveNext also registers *itself* as a hidden
  auto-launch application. Every time SteamVR starts, it briefly runs a
  headless copy of ReviveNext that rescans the installed games and
  refreshes the Library entries. Install or remove a game at any time;
  the next SteamVR session picks it up automatically. There is no
  resident background process. Each run is logged to
  `%LOCALAPPDATA%\ReviveNext\autoregister.log`.
- **Art matching** — a review window walks through every game and matches
  it against [SteamGridDB](https://www.steamgriddb.com) (a free API key,
  entered once, is all that's needed): the portrait cover and the
  widescreen Home art, both pickable in one search. Picked art is saved
  in ReviveNext's own storage (`Art/` next to the exe, keyed by game) and
  burned with the ReviveNext badge — 96 px on covers, 48 px on Home art,
  bottom-left with 20 px padding. Only newly picked art is stamped;
  re-picking refreshes it. Picking also stores the plain-English title.

## Using it

1. **One-time bootstrap:** start SteamVR, then run `ReviveNext.exe` once.
   The art review window opens — pick (or skip) art for each game, close
   it when done. This registers everything, including the auto-launch
   hook.
2. From then on, your games are in SteamVR's Library under **Other
   Apps**, with art, ready to launch. New games appear automatically at
   the next SteamVR start.

Command line:

| | |
|---|---|
| `ReviveNext.exe` | Interactive run: scan, review/match art, register |
| `ReviveNext.exe /autoregister` | Headless scan + register (what SteamVR auto-launch runs) |
| `ReviveNext.exe /unregister` | Remove every registered entry and the auto-launch hook |

Art matching needs a SteamGridDB API key (the window's *Settings…*
button asks for one); without it everything else still works.

## Building

Requires Visual Studio 2022 (v143 toolset, with the C++ and ATL
components — the vendored revive projects pin v143 and include
`atlbase.h`), CMake 3.21+, and NSIS (the configure step looks for
`makensis` for the installer target). Two dependencies that can't be
committed need a one-time local setup — see
[revive/BUILDING.md](revive/BUILDING.md) for details:

- Meta's Oculus SDK → `revive/Externals/LibOVR/`
- `detours` and `openxr-loader` from [vcpkg](https://vcpkg.io), resolved
  through its MSBuild integration (the projects use the
  `x64-windows-static` triplet)

Clone with submodules — a plain clone leaves `3rdparty/json` and the
`revive/Externals/` submodules (openvr, microprofile, Vulkan-Headers)
empty, and configure fails on the missing json sources before it can
say why:

```
git clone --recursive git@github.com:Konstruukt-Labs/ReviveNext.git
# or, in an existing clone:
git submodule update --init --recursive
```

The pinned openvr commit only needs its headers and libs; adding
`--filter=blob:none` fetches just what the checkouts materialize
(~13 MB instead of openvr's ~500 MB of history) — the same
blob-filtered fetch the CI workflow uses.

Then:

```
cmake -B build -S .
cmake --build build --config Release
```

Everything lands in `build/Release/`: `ReviveNext.exe`, the injector and
translation DLLs (built from `revive/` via its Visual Studio solution),
and the OpenVR loader. For the NSIS installer on top:

```
cmake --build build --config Release --target installer
```

produces `build/ReviveNext-Setup.exe` from that same layout. The
`--config Release` matters: the installer packages `build/Release/`,
and a config-less default Debug build leaves NSIS with no files.

## Project layout

| | |
|---|---|
| `src/` | ReviveNext itself — discovery, SteamVR registration, art review GUI |
| `revive/` | The vendored [Revive](https://github.com/LibreVR/Revive) runtime source (MIT): injector + LibOVR translation DLLs, maintained here |
| `3rdparty/` | nlohmann/json |
| `Art/` | Brand assets; the badge/lockup PNGs are compiled into the exe as resources |

### Maintenance carried in the vendored Revive source

Beyond upstream, `revive/` includes fixes made here:

- **Controller haptics work again.** SteamVR silently drops the legacy
  `TriggerHapticPulse` API when called by a scene application on several
  modern stacks (confirmed on Steam Link with a Quest), which left every
  game without rumble. Haptics now go through vibration actions — declared
  in Revive's action manifest and fired as short re-applied bursts from
  the haptics thread, the same approach the OpenXR backend already used.
- **Performance fixes:** per-node allocation removal and split GPU submits
  in ReviveXR's EndFrame, deferred swapchain image waits, a cached
  calibrated tracking origin (invalidated on recenter), O(1) haptics queue
  accounting, D3D flushes only where the D3D11on12 contract requires them,
  and the haptics thread no longer making idle OpenVR IPC calls — among
  the games' hottest per-frame paths.

## Known behavior

- **Library art doesn't render on the Steam Frame.** In Steam Frame
  sessions (Deckard over vrlink), the Library's Other Apps cards draw no
  artwork for manifest-registered apps: the session's library UI
  preloads its app list but never fetches vrmanifest images, and no
  Steam-side art (shortcuts, grid images) reaches those cards either —
  even artwork set through Steam's own UI, which renders on the desktop
  Library, is ignored there. The same SteamVR build shows ReviveNext's
  art on a Quest via Steam Link, and Steam games' store art renders on
  both. This is a gap in the Frame's library UI, not something
  ReviveNext registers differently.
- **Quitting a game ends the SteamVR session.** This is SteamVR's own
  lifecycle for applications launched from its Library as binary
  manifests — even Valve's own SteamVR Media Player behaves this way,
  and only SteamVR Home (special-cased inside SteamVR) survives it.
- SteamVR on a Quest via Steam Link occasionally fails its reconnect
  handshake right after a session ends, crash-looping until Steam is
  restarted or the headset is power-cycled. This is a SteamVR/vrlink
  issue, not specific to ReviveNext.

## Credits

- [LibreVR/Revive](https://github.com/LibreVR/Revive) — the runtime
  translation layer this project builds on (MIT).
- [SteamGridDB](https://www.steamgriddb.com) — art source.
