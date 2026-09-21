cmake_minimum_required(VERSION 3.21)

# The only place the project/target name is spelled out - change here to
# rename the whole thing later.
set(APP_NAME "ReviveNext")

project(${APP_NAME} LANGUAGES CXX)

set(JSON_BuildTests OFF CACHE INTERNAL "")
add_subdirectory(${CMAKE_SOURCE_DIR}/3rdparty/json)

add_executable(${APP_NAME}
    "src/Main.cpp"
    "src/GameDiscovery.cpp"
    "src/BinaryVdf.cpp"
    "src/SteamShortcuts.cpp"
    "src/IconConverter.cpp"
    "src/SteamGridDb.cpp"
    "src/GridArtSync.cpp"
    "src/ArtMatcherGui.cpp"
    "src/TitleOverrides.cpp"
)

target_compile_features(${APP_NAME} PUBLIC cxx_std_23)

target_link_libraries(${APP_NAME}
    PRIVATE
        nlohmann_json::nlohmann_json
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
# LibRevive64/LibReviveXR64/openvr_api64 it loads) unchanged - ReviveNext's
# own job stays scoped to discovery + Steam Library sync, not reimplementing
# the Oculus SDK -> OpenVR/OpenXR translation. These are prebuilt binaries
# vendored under runtime/ (see runtime/README.md for how to refresh them).
# The Input/ manifest files are what Main.cpp's EnsureActionManifestEnvVar
# points REVIVE_ACTION_MANIFEST at - vendored from Revive's own repo
# (Revive/Input/), since Revive's installer would otherwise be what puts
# these on disk.
add_custom_command(TARGET ${APP_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${APP_NAME}>/Input"
)
foreach(runtime_file
        ReviveInjector.exe LibRevive64.dll LibReviveXR64.dll openvr_api64.dll
        Input/action_manifest.json Input/gamepad_default.json Input/holographic_controller_default.json
        Input/knuckles_default.json Input/oculus_touch_default.json Input/vive_controller_default.json
        Input/vive_cosmos_default.json)
    add_custom_command(TARGET ${APP_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/runtime/${runtime_file}"
            "$<TARGET_FILE_DIR:${APP_NAME}>/${runtime_file}"
    )
endforeach()
