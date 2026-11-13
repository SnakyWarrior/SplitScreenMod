# SplitScreenMod for LeviLauncher on Android (preloader-android SDK).
# Build: see android/README.md. The result is a .levipack (a zip) that LeviLauncher for Android imports.
cmake_minimum_required(VERSION 3.22)

project(SplitScreenModAndroid LANGUAGES CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(MOD_ID "SplitScreenMod" CACHE STRING "LeviLauncher mod package directory name")
set(MOD_NAME "SplitScreenMod" CACHE STRING "LeviLauncher mod display name")
set(MOD_AUTHOR "SnakyWarrior" CACHE STRING "LeviLauncher mod author")
set(MOD_VERSION "0.9.8" CACHE STRING "LeviLauncher mod version")
set(MOD_LIBRARY_NAME "SplitScreenMod" CACHE STRING "Native library name without lib prefix or .so suffix")
set(MOD_MINECRAFT_VERSIONS "[\"1.26.*\"]" CACHE STRING "Raw JSON array for manifest minecraft_versions")

if(NOT ANDROID)
    message(FATAL_ERROR "Configure with the Android NDK toolchain file (see android/README.md)")
endif()
if(NOT CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR "Only arm64-v8a is supported, got ${CMAKE_ANDROID_ARCH_ABI}")
endif()

include(FetchContent)
FetchContent_Declare(
    preloader_android
    GIT_REPOSITORY https://github.com/LiteLDev/preloader-android.git
    GIT_TAG 0.2.2
    SOURCE_SUBDIR cmake/preloader-sdk
)
FetchContent_Declare(
    fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG 11.2.0
)
set(FMT_DOC OFF CACHE BOOL "" FORCE)
set(FMT_TEST OFF CACHE BOOL "" FORCE)
set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(preloader_android fmt)
FetchContent_GetProperties(preloader_android)

# The SDK symbols are provided by the preloader that LeviLauncher has already loaded.
option(LEVI_LINK_PRELOADER "Build and link the preloader target instead of resolving SDK symbols at runtime" ON)
if(LEVI_LINK_PRELOADER AND NOT TARGET preloader)
    add_subdirectory("${preloader_android_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_preloader_android")
endif()

add_library(${MOD_LIBRARY_NAME} SHARED src/main.cpp src/SplitScreen.cpp src/Elf.cpp src/InputProbe.cpp src/DeviceManagerProbe.cpp src/ControllerSplit.cpp src/FormProbe.cpp src/Menus.cpp src/VTrace.cpp src/PostTrace.cpp src/Trampolines.S src/VTrampolines.S src/PostTrampolines.S)
target_include_directories(${MOD_LIBRARY_NAME} PRIVATE src "${preloader_android_SOURCE_DIR}/include")
target_compile_definitions(${MOD_LIBRARY_NAME} PRIVATE SPLITSCREEN_VERSION="${MOD_VERSION}")
target_compile_options(${MOD_LIBRARY_NAME} PRIVATE -Wall -Wextra -fvisibility=hidden -fvisibility-inlines-hidden)
target_link_options(${MOD_LIBRARY_NAME} PRIVATE "-Wl,-z,max-page-size=16384")

find_library(log-lib log REQUIRED)
target_link_libraries(${MOD_LIBRARY_NAME} PRIVATE ${log-lib} android dl fmt::fmt)
if(TARGET preloader)
    target_link_libraries(${MOD_LIBRARY_NAME} PRIVATE preloader)
else()
    target_link_options(${MOD_LIBRARY_NAME} PRIVATE "-Wl,--unresolved-symbols=ignore-all")
endif()

add_custom_command(TARGET ${MOD_LIBRARY_NAME} POST_BUILD
    COMMAND "${CMAKE_STRIP}" --strip-unneeded "$<TARGET_FILE:${MOD_LIBRARY_NAME}>"
    VERBATIM
)

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/manifest.json.in" "${CMAKE_CURRENT_BINARY_DIR}/manifest.json" @ONLY)

# SplitScreenMod-android-arm64.levipack: a zip with SplitScreenMod/manifest.json, SplitScreenMod/libSplitScreenMod.so,
# config/ and pack/ (the resource pack).
# The same zip is also written as .zip, which LeviLauncher's "Add Mod" accepts.
set(PACKAGE_ROOT "${CMAKE_CURRENT_BINARY_DIR}/package")
set(PACKAGE_MOD_DIR "${PACKAGE_ROOT}/${MOD_ID}")
set(PACKAGE_FILE "${CMAKE_CURRENT_BINARY_DIR}/${MOD_ID}-android-arm64.levipack")
add_custom_target(levi_package
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${PACKAGE_ROOT}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${PACKAGE_MOD_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:${MOD_LIBRARY_NAME}>" "${PACKAGE_MOD_DIR}/lib${MOD_LIBRARY_NAME}.so"
    COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_BINARY_DIR}/manifest.json" "${PACKAGE_MOD_DIR}/manifest.json"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/config" "${PACKAGE_MOD_DIR}/config"
    # the mod's resource pack, put into the game and turned on by the mod, with the Android changes from android/pack
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/../resource_packs/SplitScreenUI" "${PACKAGE_MOD_DIR}/pack"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/pack" "${PACKAGE_MOD_DIR}/pack"
    COMMAND "${CMAKE_COMMAND}" -E chdir "${PACKAGE_ROOT}" "${CMAKE_COMMAND}" -E tar cf "${PACKAGE_FILE}" --format=zip "${MOD_ID}"
    COMMAND "${CMAKE_COMMAND}" -E copy "${PACKAGE_FILE}" "${CMAKE_CURRENT_BINARY_DIR}/${MOD_ID}-android-arm64.zip"
    DEPENDS ${MOD_LIBRARY_NAME}
    VERBATIM
)
