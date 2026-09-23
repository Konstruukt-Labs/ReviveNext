# Building the vendored Revive sources

`revive/` holds the source for `ReviveInjector.exe`, `LibRevive64.dll`
(the `Revive` project) and `LibReviveXR64.dll` (the `ReviveXR` project) -
originally from [LibreVR/Revive](https://github.com/LibreVR/Revive) (MIT,
see `LICENSE`), now maintained here since upstream has been inactive for
years. It still builds as its own Visual Studio solution
(`revive/Revive.sln`); ReviveNext's own CMake build invokes MSBuild on it
as part of `cmake --build` - see the root `CMakeLists.txt`.

Dependencies that can't be committed to this repo need a one-time local
setup before `revive/Revive.sln` will build: `detours` and the OpenXR
loader (both through vcpkg), and Meta's Oculus SDK (manual download).

## Detours + OpenXR loader (vcpkg)

The solution resolves `detours` (Revive's own hooking code includes
`detours/detours.h`) and `openxr_loader.lib` (linked by ReviveXR)
through vcpkg's MSBuild integration. The projects set `VcpkgUseStatic`,
so the static-CRT triplet is the one consumed:

    vcpkg integrate install
    vcpkg install --triplet x64-windows-static detours openxr-loader

Ports installed for the default `x64-windows` triplet won't be found -
the static triplet is required.

## Oculus SDK (`revive/Externals/LibOVR/`)

Meta's own license doesn't allow redistributing it.

1. Download the Oculus SDK for Windows (this was last built against
   1.64.0) - the same public zip the CI workflow fetches, no Meta
   login needed:
   https://securecdn.oculus.com/binaries/download/?id=4377593722298679
2. Unzip it and place the `LibOVR/` folder at
   `revive/Externals/LibOVR/`, so `revive/Externals/LibOVR/Include/`
   and `revive/Externals/LibOVR/Shim/` exist.

## OpenXR loader from source (alternative to the vcpkg port)

The vcpkg route above is what CI uses. To provide the loader locally
instead:

1. Clone [KhronosGroup/OpenXR-SDK-Source](https://github.com/KhronosGroup/OpenXR-SDK-Source)
   (this was last built against 1.1.63).
2. Copy its `include/` and `src/loader/` into `revive/Externals/openxr/`,
   so `revive/Externals/openxr/include/openxr/` and
   `revive/Externals/openxr/src/loader/` exist.
3. Build the loader (CMake, Release) and copy the resulting
   `openxr_loader.lib` to `revive/Externals/openxr/src/loader/Release/` -
   the exact library path ReviveXR.vcxproj links against. Copying the
   sources alone isn't enough; without the lib there, the link fails.

All these paths are gitignored (see the root `.gitignore`) - once in
place, they persist across pulls same as any other local build
dependency.
