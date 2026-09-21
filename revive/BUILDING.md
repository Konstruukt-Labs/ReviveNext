# Building the vendored Revive sources

`revive/` holds the source for `ReviveInjector.exe`, `LibRevive64.dll`
(the `Revive` project) and `LibReviveXR64.dll` (the `ReviveXR` project) -
originally from [LibreVR/Revive](https://github.com/LibreVR/Revive) (MIT,
see `LICENSE`), now maintained here since upstream has been inactive for
years. It still builds as its own Visual Studio solution
(`revive/Revive.sln`); ReviveNext's own CMake build invokes MSBuild on it
as part of `cmake --build` - see the root `CMakeLists.txt`.

Two dependencies can't be committed to this repo and need a one-time local
setup before `revive/Revive.sln` will build:

## Oculus SDK (`revive/Externals/LibOVR/`)

Meta's own license doesn't allow redistributing it.

1. Download the Oculus SDK for Windows (this was last built against
   1.64.0) from Meta's developer downloads page.
2. Unzip it and place the result at `revive/Externals/LibOVR/`, so
   `revive/Externals/LibOVR/Include/` etc. exist.

## OpenXR loader source (`revive/Externals/openxr/`)

1. Clone [KhronosGroup/OpenXR-SDK-Source](https://github.com/KhronosGroup/OpenXR-SDK-Source)
   (this was last built against 1.1.63).
2. Copy its `include/` and `src/loader/` into `revive/Externals/openxr/`,
   so `revive/Externals/openxr/include/openxr/` and
   `revive/Externals/openxr/src/loader/` exist.

Both paths are gitignored (see the root `.gitignore`) - once in place,
they persist across pulls same as any other local build dependency.
