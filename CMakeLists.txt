cmake_minimum_required(VERSION 3.21)

# The only place the project/target name is spelled out - change here to
# rename the whole thing later.
set(APP_NAME "ReviveNext")

project(${APP_NAME} LANGUAGES CXX)

set(JSON_BuildTests OFF CACHE INTERNAL "")
add_subdirectory(${CMAKE_SOURCE_DIR}/3rdparty/json)

add_executable(${APP_NAME} WIN32
    "src/Main.cpp"
    "src/AppData.cpp"
    "src/GameDiscovery.cpp"
    "src/SteamVrAppRegistration.cpp"
    "src/IconConverter.cpp"
    "src/SteamGridDb.cpp"
    "src/GridArtSync.cpp"
    "src/ArtMatcherGui.cpp"
    "src/TitleOverrides.cpp"
    "src/ReviveNext.rc"
)

target_compile_features(${APP_NAME} PUBLIC cxx_std_23)

# OpenVR for SteamVrAppRegistration (registering the per-game
# revive.app.* entries SteamVR identifies launched games with). Uses
# the pristine loader shipped in the vendored submodule.
set(OPENVR_DIR "${CMAKE_SOURCE_DIR}/revive/Externals/openvr")
target_include_directories(${APP_NAME} PRIVATE "${OPENVR_DIR}/headers")

# openvr_api.dll lives under Resources/ (not beside the exe), so it is
# delay-loaded and explicitly preloaded from there at startup (see
# wWinMain) - the implicit-import loader only searches the exe's own
# directory.
target_link_options(${APP_NAME} PRIVATE "/DELAYLOAD:openvr_api.dll")
target_link_libraries(${APP_NAME}
    PRIVATE
        nlohmann_json::nlohmann_json
        delayimp
        "${OPENVR_DIR}/lib/win64/openvr_api.lib"
        windowscodecs
        ole32
        winhttp
        gdiplus
        shlwapi
        comctl32
)

if (MSVC)
    target_compile_options(${APP_NAME} PRIVATE /W4)
endif()

# Actual game launching reuses Revive's proven ReviveInjector.exe (and the
# LibRevive64/LibReviveXR64/openvr_api64 it loads) - ReviveNext's own job
# stays scoped to discovery + Steam Library sync, not reimplementing the
# Oculus SDK -> OpenVR/OpenXR translation. The source lives in revive/
# (originally LibreVR/Revive, now maintained here - see revive/BUILDING.md
# for its two local-only dependencies) and is still built via its own
# Visual Studio solution, proven and not worth hand-porting to CMake and
# risking subtly wrong compiler/linker settings for this VR-runtime-
# critical shim code - located via vswhere and invoked here so `cmake
# --build` alone still produces a complete ReviveNext install.
find_program(VSWHERE_EXE vswhere
    PATHS "C:/Program Files (x86)/Microsoft Visual Studio/Installer"
)
if (NOT VSWHERE_EXE)
    message(FATAL_ERROR "vswhere.exe not found - is Visual Studio installed?")
endif()

execute_process(
    COMMAND "${VSWHERE_EXE}" -latest -requires Microsoft.Component.MSBuild -find "MSBuild/**/Bin/MSBuild.exe"
    OUTPUT_VARIABLE MSBUILD_EXE
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if (NOT MSBUILD_EXE)
    message(FATAL_ERROR "MSBuild.exe not found via vswhere - is the \"MSBuild\" Visual Studio component installed?")
endif()

set(REVIVE_DIR "${CMAKE_SOURCE_DIR}/revive")
set(REVIVE_OUT_DIR "${REVIVE_DIR}/Release")

add_custom_target(revive_binaries
    # Built through Revive.sln, not the .vcxproj directly - $(SolutionDir)
    # (used throughout these projects' Externals include paths) only
    # resolves correctly to revive/ itself when MSBuild has an actual
    # solution context; building a lone .vcxproj instead makes it resolve
    # per-project to that project's own directory, breaking every
    # $(Externals) reference.
    COMMAND "${MSBUILD_EXE}" "${REVIVE_DIR}/Revive.sln" /t:ReviveInjector /p:Configuration=Release /p:Platform=x64 /m
    WORKING_DIRECTORY "${REVIVE_DIR}"
    COMMENT "Building ReviveInjector.exe/LibRevive64.dll/LibReviveXR64.dll/openvr_api64.dll from revive/ (see revive/BUILDING.md if this fails to find LibOVR/openxr)"
    VERBATIM
)
add_dependencies(${APP_NAME} revive_binaries)

add_custom_command(TARGET ${APP_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${APP_NAME}>/Resources/Input"
)
# Everything ReviveNext needs at runtime lives under Resources/ - only the
# exe itself (plus its compiled-in icon/badges/lockup) stays at the top
# level. The installer ships exactly this layout.
set(RESOURCES_DIR "$<TARGET_FILE_DIR:${APP_NAME}>/Resources")
# The openvr_api.dll ReviveNext.exe itself loads for registration -
# distinct from the injected openvr_api64.dll shim games get.
add_custom_command(TARGET ${APP_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${OPENVR_DIR}/bin/win64/openvr_api.dll"
        "${RESOURCES_DIR}"
)
foreach(revive_binary ReviveInjector.exe LibRevive64.dll LibReviveXR64.dll openvr_api64.dll)
    add_custom_command(TARGET ${APP_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${REVIVE_OUT_DIR}/${revive_binary}"
            "${RESOURCES_DIR}/${revive_binary}"
    )
endforeach()
foreach(input_file
        action_manifest.json gamepad_default.json holographic_controller_default.json
        knuckles_default.json oculus_touch_default.json vive_controller_default.json
        vive_cosmos_default.json)
    add_custom_command(TARGET ${APP_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${REVIVE_DIR}/Revive/Input/${input_file}"
            "${RESOURCES_DIR}/Input/${input_file}"
    )
endforeach()

# Installer (NSIS): cmake --build build --target installer produces
# build/ReviveNext-Setup.exe from the just-built Release layout.
find_program(MAKENSIS_EXE NAMES makensis PATHS "C:/Program Files (x86)/NSIS")
if(NOT MAKENSIS_EXE)
    message(FATAL_ERROR "makensis.exe not found - is NSIS installed?")
endif()
add_custom_target(installer
    COMMAND "${MAKENSIS_EXE}" -V2 -DROOT=${CMAKE_SOURCE_DIR} "${CMAKE_SOURCE_DIR}/installer/ReviveNext.nsi"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Building ReviveNext-Setup.exe with NSIS"
    VERBATIM
)
add_dependencies(installer ${APP_NAME})
