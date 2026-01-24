cmake_minimum_required(VERSION 3.20)
project(MystralNativeRuntime VERSION 0.1.0 LANGUAGES C CXX)

# Windows: Use /MT (static CRT) for all builds
# Both Skia and Dawn (from library-builder) are built with /MT static CRT.
# The CI workflow passes -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded for all Windows builds.

# ============================================================================
# Options
# ============================================================================

# JS Engine (choose one)
option(MYSTRAL_USE_V8 "Use V8 JavaScript engine" OFF)
option(MYSTRAL_USE_JSC "Use JavaScriptCore engine" OFF)
option(MYSTRAL_USE_QUICKJS "Use QuickJS engine" ON)  # Default - simplest to start
option(MYSTRAL_USE_SWC "Enable SWC TypeScript transpiler" ON)

# V8 requires C++20, so use that if V8 is enabled, otherwise C++17
# Must be after option() definitions so MYSTRAL_USE_V8 is properly set
if(MYSTRAL_USE_V8)
    set(CMAKE_CXX_STANDARD 20)
    message(STATUS "V8 enabled - using C++20")
    # MSVC needs explicit flag for C++20 in CMAKE_CXX_FLAGS
    # (add_compile_options can be overridden by vcpkg toolchain)
    if(MSVC)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /std:c++20")
    endif()
else()
    set(CMAKE_CXX_STANDARD 17)
    message(STATUS "V8 disabled - using C++17")
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# WebGPU Implementation (choose one)
option(MYSTRAL_USE_DAWN "Use Dawn WebGPU implementation" OFF)
option(MYSTRAL_USE_WGPU "Use wgpu-native WebGPU implementation" ON)  # Default - has iOS support

# ============================================================================
# Platform Detection
# ============================================================================

if(APPLE)
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        set(MYSTRAL_PLATFORM "ios")
        set(MYSTRAL_USE_V8 OFF)
        set(MYSTRAL_USE_JSC ON)      # JSC is free on iOS
        set(MYSTRAL_USE_QUICKJS OFF)
        set(MYSTRAL_USE_DAWN OFF)
        set(MYSTRAL_USE_WGPU ON)     # Dawn doesn't support iOS
    else()
        set(MYSTRAL_PLATFORM "macos")
    endif()
elseif(WIN32)
    set(MYSTRAL_PLATFORM "windows")
elseif(ANDROID)
    set(MYSTRAL_PLATFORM "android")
    # Android uses QuickJS by default (simplest to integrate, no special runtime deps)
    set(MYSTRAL_USE_V8 OFF)
    set(MYSTRAL_USE_JSC OFF)
    set(MYSTRAL_USE_QUICKJS ON)
    # wgpu-native has Android support
    set(MYSTRAL_USE_DAWN OFF)
    set(MYSTRAL_USE_WGPU ON)
elseif(UNIX)
    set(MYSTRAL_PLATFORM "linux")
endif()

message(STATUS "Mystral Platform: ${MYSTRAL_PLATFORM}")
message(STATUS "JS Engine: V8=${MYSTRAL_USE_V8} JSC=${MYSTRAL_USE_JSC} QuickJS=${MYSTRAL_USE_QUICKJS}")
message(STATUS "WebGPU: Dawn=${MYSTRAL_USE_DAWN} wgpu=${MYSTRAL_USE_WGPU}")

# ============================================================================
# Third Party Dependencies
# ============================================================================

set(THIRD_PARTY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

# SDL3 - Build from source as static library for single-binary distribution
set(SDL3_DIR ${THIRD_PARTY_DIR}/sdl3)
set(SDL3_FOUND OFF)

# Find SDL3 source directory (SDL3-{version}/)
file(GLOB SDL3_SOURCE_DIRS ${SDL3_DIR}/SDL3-*)
if(SDL3_SOURCE_DIRS)
    list(GET SDL3_SOURCE_DIRS 0 SDL3_SOURCE_DIR)
    if(EXISTS ${SDL3_SOURCE_DIR}/CMakeLists.txt)
        message(STATUS "Building SDL3 from source: ${SDL3_SOURCE_DIR}")

        # Configure SDL3 build options - static library only
        set(SDL_SHARED OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC ON CACHE BOOL "" FORCE)
        set(SDL_TEST OFF CACHE BOOL "" FORCE)
        set(SDL_TESTS OFF CACHE BOOL "" FORCE)

        # Disable features we don't need to speed up build
        set(SDL_RENDER OFF CACHE BOOL "" FORCE)

        # Add SDL3 as a subdirectory
        add_subdirectory(${SDL3_SOURCE_DIR} ${CMAKE_BINARY_DIR}/sdl3-build EXCLUDE_FROM_ALL)

        # SDL3 creates SDL3::SDL3-static target
        if(TARGET SDL3::SDL3-static)
            set(SDL3_FOUND ON)
            set(SDL3_STATIC_TARGET SDL3::SDL3-static)
            message(STATUS "SDL3 static library target: SDL3::SDL3-static")
        elseif(TARGET SDL3-static)
            set(SDL3_FOUND ON)
            set(SDL3_STATIC_TARGET SDL3-static)
            message(STATUS "SDL3 static library target: SDL3-static")
        else()
            message(WARNING "SDL3 static target not found after add_subdirectory")
        endif()
    else()
        message(WARNING "SDL3 CMakeLists.txt not found in ${SDL3_SOURCE_DIR}")
    endif()
else()
    # Fallback: Check for pre-built SDL3 (Android AAR, etc.)
    if(ANDROID)
        set(SDL3_ANDROID_DIR ${THIRD_PARTY_DIR}/sdl3-android)
        if(EXISTS ${SDL3_ANDROID_DIR}/extracted)
            set(SDL3_EXTRACTED_DIR ${SDL3_ANDROID_DIR}/extracted)
            if(ANDROID_ABI STREQUAL "arm64-v8a")
                set(SDL3_ANDROID_ABI "arm64-v8a")
            elseif(ANDROID_ABI STREQUAL "armeabi-v7a")
                set(SDL3_ANDROID_ABI "armeabi-v7a")
            elseif(ANDROID_ABI STREQUAL "x86_64")
                set(SDL3_ANDROID_ABI "x86_64")
            elseif(ANDROID_ABI STREQUAL "x86")
                set(SDL3_ANDROID_ABI "x86")
            else()
                set(SDL3_ANDROID_ABI "arm64-v8a")
            endif()
            set(SDL3_PREFAB_HEADERS ${SDL3_EXTRACTED_DIR}/prefab/modules/SDL3-Headers)
            set(SDL3_PREFAB_LIB ${SDL3_EXTRACTED_DIR}/prefab/modules/SDL3-shared)
            if(EXISTS ${SDL3_PREFAB_LIB}/libs/android.${SDL3_ANDROID_ABI}/libSDL3.so)
                set(SDL3_LIBRARY ${SDL3_PREFAB_LIB}/libs/android.${SDL3_ANDROID_ABI}/libSDL3.so)
                set(SDL3_INCLUDE_DIR ${SDL3_PREFAB_HEADERS}/include)
                set(SDL3_FOUND ON)
                message(STATUS "Found SDL3 for Android: ${SDL3_LIBRARY}")
            endif()
        endif()
    endif()
endif()

if(NOT SDL3_FOUND)
    message(WARNING "SDL3 not found. Run 'node scripts/download-deps.mjs' first.")
endif()

# wgpu-native
if(MYSTRAL_USE_WGPU)
    # Platform-specific wgpu-native directories
    if(MYSTRAL_PLATFORM STREQUAL "ios")
        set(WGPU_DIR ${THIRD_PARTY_DIR}/wgpu-ios)
        # Determine target subdirectory based on CMAKE_OSX_SYSROOT
        if(CMAKE_OSX_SYSROOT MATCHES "simulator")
            if(CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64")
                set(WGPU_TARGET_DIR ${WGPU_DIR}/simulatorX64)
            else()
                set(WGPU_TARGET_DIR ${WGPU_DIR}/simulatorArm64)
            endif()
        else()
            set(WGPU_TARGET_DIR ${WGPU_DIR}/device)
        endif()
    elseif(MYSTRAL_PLATFORM STREQUAL "android")
        set(WGPU_DIR ${THIRD_PARTY_DIR}/wgpu-android)
        # Determine target subdirectory based on ANDROID_ABI
        if(ANDROID_ABI STREQUAL "arm64-v8a")
            set(WGPU_TARGET_DIR ${WGPU_DIR}/aarch64)
        elseif(ANDROID_ABI STREQUAL "x86_64")
            set(WGPU_TARGET_DIR ${WGPU_DIR}/x86_64)
        else()
            # Default to aarch64 (most common on modern Android)
            set(WGPU_TARGET_DIR ${WGPU_DIR}/aarch64)
        endif()
    else()
        set(WGPU_DIR ${THIRD_PARTY_DIR}/wgpu)
        set(WGPU_TARGET_DIR ${WGPU_DIR})
    endif()

    if(EXISTS ${WGPU_TARGET_DIR})
        # wgpu-native provides libwgpu_native.a (Unix) or wgpu_native.dll.lib (Windows)
        # Structure: wgpu/lib/libwgpu_native.a, wgpu/include/webgpu/webgpu.h
        # Prefer static library to avoid runtime linking issues
        if(WIN32)
            # Windows uses .dll.lib or .lib extension
            if(EXISTS ${WGPU_TARGET_DIR}/lib/wgpu_native.dll.lib)
                set(WGPU_LIBRARY ${WGPU_TARGET_DIR}/lib/wgpu_native.dll.lib)
            elseif(EXISTS ${WGPU_TARGET_DIR}/lib/wgpu_native.lib)
                set(WGPU_LIBRARY ${WGPU_TARGET_DIR}/lib/wgpu_native.lib)
            endif()
        else()
            # Unix uses .a for static
            if(EXISTS ${WGPU_TARGET_DIR}/lib/libwgpu_native.a)
                set(WGPU_LIBRARY ${WGPU_TARGET_DIR}/lib/libwgpu_native.a)
            else()
                find_library(WGPU_LIBRARY
                    NAMES wgpu_native
                    PATHS ${WGPU_TARGET_DIR}/lib
                    NO_DEFAULT_PATH
                )
            endif()
        endif()
        # Headers are in include/webgpu/ and include/wgpu/
        if(EXISTS ${WGPU_TARGET_DIR}/include)
            set(WGPU_INCLUDE_DIR ${WGPU_TARGET_DIR}/include)
        endif()

        if(WGPU_LIBRARY AND WGPU_INCLUDE_DIR)
            if(WIN32)
                add_library(wgpu::wgpu SHARED IMPORTED)
            else()
                add_library(wgpu::wgpu STATIC IMPORTED)
            endif()
            set_target_properties(wgpu::wgpu PROPERTIES
                IMPORTED_LOCATION ${WGPU_LIBRARY}
                INTERFACE_INCLUDE_DIRECTORIES ${WGPU_INCLUDE_DIR}
            )
            if(WIN32)
                # Windows needs the DLL at runtime
                set_target_properties(wgpu::wgpu PROPERTIES
                    IMPORTED_IMPLIB ${WGPU_LIBRARY}
                )
            endif()
            add_compile_definitions(MYSTRAL_WEBGPU_WGPU)
            set(MYSTRAL_WEBGPU_BACKEND_SET ON)
            message(STATUS "Found wgpu-native: ${WGPU_LIBRARY}")
            message(STATUS "wgpu-native includes: ${WGPU_INCLUDE_DIR}")
        else()
            message(WARNING "wgpu-native not found:")
            message(WARNING "  Library: ${WGPU_LIBRARY}")
            message(WARNING "  Include: ${WGPU_INCLUDE_DIR}")
        endif()
    else()
        if(MYSTRAL_PLATFORM STREQUAL "ios")
            message(WARNING "wgpu-native iOS not found. Run 'node scripts/download-deps.mjs --only wgpu-ios' first.")
        else()
            message(WARNING "wgpu-native not found. Run 'npm run deps:download:wgpu' first.")
        endif()
    endif()
endif()

# Dawn WebGPU Implementation
# Windows: Uses dawn_combined.lib from Skia build (library-builder), built with /MT static CRT
#          and full D3D11/D3D12 WebGPU implementation (not just proc stubs)
# Other platforms: Uses official Dawn releases from https://github.com/google/dawn/releases
if(MYSTRAL_USE_DAWN)
    set(DAWN_FOUND OFF)

    if(WIN32)
        # Windows: Look for dawn_combined.lib in the Skia build directory
        # The skia-win-static dependency from library-builder includes Dawn with full
        # D3D11/D3D12 backends, built with /MT static CRT to match Skia

        # Set up Skia paths early since Dawn depends on them
        # (normally defined later in the CMakeLists, but needed here for Dawn detection)
        set(SKIA_DIR ${THIRD_PARTY_DIR}/skia)
        set(SKIA_TARGET_DIR ${SKIA_DIR})
        set(SKIA_PLATFORM_DIR "win-gpu")
        set(SKIA_LIB_SUBDIR "x64")

        if(EXISTS ${SKIA_TARGET_DIR})
            # dawn_combined.lib is in the same directory as skia.lib
            set(DAWN_LIB_PATH ${SKIA_TARGET_DIR}/build/${SKIA_PLATFORM_DIR}/lib/Release/${SKIA_LIB_SUBDIR}/dawn_combined.lib)
            message(STATUS "Looking for Dawn combined library at: ${DAWN_LIB_PATH}")

            if(EXISTS ${DAWN_LIB_PATH})
                set(DAWN_LIBRARY ${DAWN_LIB_PATH})
                # Dawn headers in Skia build are split across two locations:
                # 1. build/include/third_party/externals/dawn/include/webgpu/webgpu.h (wrapper)
                # 2. build/include/dawn/webgpu.h (actual definitions)
                # The wrapper includes "dawn/webgpu.h", so we need both paths
                set(DAWN_INCLUDE_DIR
                    ${SKIA_TARGET_DIR}/build/include/third_party/externals/dawn/include
                    ${SKIA_TARGET_DIR}/build/include
                )
                set(DAWN_FOUND ON)
                message(STATUS "Found Dawn combined library from Skia build")
                message(STATUS "Dawn includes: ${DAWN_INCLUDE_DIR}")
            else()
                message(WARNING "dawn_combined.lib not found in Skia build at ${DAWN_LIB_PATH}")
                message(WARNING "Run 'node scripts/download-deps.mjs --only skia-win-static' first.")
            endif()
        else()
            message(WARNING "Skia directory not found. Dawn on Windows requires Skia build.")
            message(WARNING "Run 'node scripts/download-deps.mjs --only skia-win-static' first.")
        endif()
    else()
        # Non-Windows: Use official Dawn releases
        set(DAWN_DIR ${THIRD_PARTY_DIR}/dawn)
        if(EXISTS ${DAWN_DIR})
            # Find the platform-specific subdirectory (contains commit hash in name)
            file(GLOB DAWN_SUBDIRS ${DAWN_DIR}/Dawn-*-Release)
            if(DAWN_SUBDIRS)
                list(GET DAWN_SUBDIRS 0 DAWN_ACTUAL_DIR)
                message(STATUS "Found Dawn release directory: ${DAWN_ACTUAL_DIR}")

                # Debug: List what's in the lib directory (try both lib and lib64)
                file(GLOB DAWN_LIB_FILES ${DAWN_ACTUAL_DIR}/lib/*)
                file(GLOB DAWN_LIB64_FILES ${DAWN_ACTUAL_DIR}/lib64/*)
                message(STATUS "Dawn lib directory contents: ${DAWN_LIB_FILES}")
                message(STATUS "Dawn lib64 directory contents: ${DAWN_LIB64_FILES}")

                # Find library - try static first, then shared
                set(DAWN_LIB_NAMES "libwebgpu_dawn.a" "libdawn.a" "libwebgpu_dawn.so" "libdawn.so")
                # Linux uses lib64, macOS uses lib
                set(DAWN_LIB_DIRS "lib64" "lib")

                foreach(LIB_DIR ${DAWN_LIB_DIRS})
                    foreach(LIB_NAME ${DAWN_LIB_NAMES})
                        if(EXISTS ${DAWN_ACTUAL_DIR}/${LIB_DIR}/${LIB_NAME})
                            set(DAWN_LIBRARY ${DAWN_ACTUAL_DIR}/${LIB_DIR}/${LIB_NAME})
                            message(STATUS "Found Dawn library: ${LIB_DIR}/${LIB_NAME}")
                            break()
                        endif()
                    endforeach()
                    if(DAWN_LIBRARY)
                        break()
                    endif()
                endforeach()

                # Fallback: use find_library
                if(NOT DAWN_LIBRARY)
                    find_library(DAWN_LIBRARY
                        NAMES webgpu_dawn dawn
                        PATHS ${DAWN_ACTUAL_DIR}/lib ${DAWN_ACTUAL_DIR}/lib64
                        NO_DEFAULT_PATH
                    )
                endif()

                # Find headers
                if(EXISTS ${DAWN_ACTUAL_DIR}/include)
                    set(DAWN_INCLUDE_DIR ${DAWN_ACTUAL_DIR}/include)
                endif()

                if(DAWN_LIBRARY AND DAWN_INCLUDE_DIR)
                    set(DAWN_FOUND ON)
                endif()
            else()
                message(WARNING "Dawn release directory not found in ${DAWN_DIR}")
                message(WARNING "Run 'node scripts/download-deps.mjs --only dawn' first.")
            endif()
        endif()
    endif()

    if(DAWN_FOUND)
        add_library(dawn::webgpu STATIC IMPORTED)
        set_target_properties(dawn::webgpu PROPERTIES
            IMPORTED_LOCATION "${DAWN_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${DAWN_INCLUDE_DIR}"
        )
        # Only define MYSTRAL_WEBGPU_DAWN if wgpu-native wasn't already set up
        # (can't have both WebGPU backends active)
        if(NOT MYSTRAL_WEBGPU_BACKEND_SET)
            add_compile_definitions(MYSTRAL_WEBGPU_DAWN)
            set(MYSTRAL_WEBGPU_BACKEND_SET ON)
            message(STATUS "Using Dawn as WebGPU backend")
        else()
            message(STATUS "Dawn found but wgpu-native already configured as WebGPU backend")
        endif()
        message(STATUS "Found Dawn: ${DAWN_LIBRARY}")
        message(STATUS "Dawn includes: ${DAWN_INCLUDE_DIR}")
    else()
        message(WARNING "Dawn library or headers not found")
        if(WIN32)
            message(WARNING "  Run 'node scripts/download-deps.mjs --only skia-win-static'")
        else()
            message(WARNING "  Run 'node scripts/download-deps.mjs --only dawn'")
        endif()
    endif()
endif()

# QuickJS-NG (actively maintained fork with MSVC/Windows support)
if(MYSTRAL_USE_QUICKJS)
    set(QUICKJS_DIR ${THIRD_PARTY_DIR}/quickjs)
    if(EXISTS ${QUICKJS_DIR})
        # Try to find in subdirectory first (GitHub archives extract with version dir)
        file(GLOB QUICKJS_SUBDIRS ${QUICKJS_DIR}/quickjs-*)
        if(QUICKJS_SUBDIRS)
            list(GET QUICKJS_SUBDIRS 0 QUICKJS_ACTUAL_DIR)
        else()
            set(QUICKJS_ACTUAL_DIR ${QUICKJS_DIR})
        endif()

        # QuickJS is just source files - compile them
        # Note: quickjs-ng integrates libbf into quickjs.c, and adds dtoa.c for number conversion
        file(GLOB QUICKJS_SOURCES
            ${QUICKJS_ACTUAL_DIR}/quickjs.c
            ${QUICKJS_ACTUAL_DIR}/cutils.c
            ${QUICKJS_ACTUAL_DIR}/dtoa.c
            ${QUICKJS_ACTUAL_DIR}/libregexp.c
            ${QUICKJS_ACTUAL_DIR}/libunicode.c
        )
        if(QUICKJS_SOURCES)
            add_library(quickjs STATIC ${QUICKJS_SOURCES})
            # Use SYSTEM to prevent QuickJS's "version" file from shadowing C++ <version>
            target_include_directories(quickjs SYSTEM PUBLIC ${QUICKJS_ACTUAL_DIR})
            target_compile_definitions(quickjs PRIVATE
                CONFIG_VERSION="0.11.0"
                CONFIG_BIGNUM
            )
            # Suppress warnings in third-party code
            if(MSVC)
                target_compile_options(quickjs PRIVATE /W0)
                # Enable C11 for MSVC - required for C11 atomics used by QuickJS
                target_compile_options(quickjs PRIVATE /std:c11)
                # WIN32_LEAN_AND_MEAN prevents winsock.h from being included
                # via windows.h, avoiding conflicts with winsock2.h
                target_compile_definitions(quickjs PRIVATE WIN32_LEAN_AND_MEAN)
            else()
                target_compile_options(quickjs PRIVATE -w)
            endif()
            add_compile_definitions(MYSTRAL_JS_QUICKJS)
            message(STATUS "Found QuickJS-NG in ${QUICKJS_ACTUAL_DIR}")
        else()
            message(WARNING "QuickJS-NG sources not found in ${QUICKJS_ACTUAL_DIR}")
        endif()
    else()
        message(WARNING "QuickJS-NG not found. Run 'npm run deps:download' first.")
    endif()
endif()

# V8
# Prebuilts from kuoruan/libv8: https://github.com/kuoruan/libv8/releases
# See docs/V8_PREBUILTS.md for fork/update information
if(MYSTRAL_USE_V8)
    set(V8_DIR ${THIRD_PARTY_DIR}/v8)
    if(EXISTS ${V8_DIR})
        # kuoruan/libv8 structure: v8/include/, v8/lib/
        # Library is libv8_monolith.a (macOS/Linux) or v8_monolith.lib (Windows)
        if(WIN32)
            set(V8_LIB_NAME "v8_monolith.lib")
        else()
            set(V8_LIB_NAME "libv8_monolith.a")
        endif()

        # Find the library (kuoruan/libv8 puts it at root, not in lib/)
        if(EXISTS ${V8_DIR}/${V8_LIB_NAME})
            set(V8_LIBRARY ${V8_DIR}/${V8_LIB_NAME})
        elseif(EXISTS ${V8_DIR}/lib/${V8_LIB_NAME})
            set(V8_LIBRARY ${V8_DIR}/lib/${V8_LIB_NAME})
        else()
            find_library(V8_LIBRARY
                NAMES v8_monolith
                PATHS ${V8_DIR} ${V8_DIR}/lib
                NO_DEFAULT_PATH
            )
        endif()

        # Find headers
        # kuoruan/libv8 puts headers in include/ on Linux/macOS, but at root on Windows
        if(EXISTS ${V8_DIR}/include)
            set(V8_INCLUDE_DIR ${V8_DIR}/include)
        elseif(EXISTS ${V8_DIR}/v8.h)
            # Windows: headers are at root level
            set(V8_INCLUDE_DIR ${V8_DIR})
        endif()

        if(V8_LIBRARY AND V8_INCLUDE_DIR)
            add_library(v8::v8 STATIC IMPORTED)
            set_target_properties(v8::v8 PROPERTIES
                IMPORTED_LOCATION ${V8_LIBRARY}
                INTERFACE_INCLUDE_DIRECTORIES ${V8_INCLUDE_DIR}
            )
            add_compile_definitions(MYSTRAL_JS_V8)
            message(STATUS "Found V8: ${V8_LIBRARY}")
            message(STATUS "V8 includes: ${V8_INCLUDE_DIR}")

            # V8 requires additional system libraries
            if(APPLE)
                # V8 on macOS needs these frameworks
                find_library(COREFOUNDATION_FRAMEWORK CoreFoundation)
            elseif(UNIX)
                # V8 on Linux needs pthread and dl
                set(V8_SYSTEM_LIBS pthread dl)
            elseif(WIN32)
                # V8 on Windows needs these
                set(V8_SYSTEM_LIBS winmm dbghelp shlwapi)
            endif()
        else()
            message(WARNING "V8 not found:")
            message(WARNING "  Library: ${V8_LIBRARY}")
            message(WARNING "  Include: ${V8_INCLUDE_DIR}")
            message(WARNING "Run 'node scripts/download-deps.mjs --only v8' first.")
        endif()
    else()
        message(WARNING "V8 not found. Run 'node scripts/download-deps.mjs --only v8' first.")
    endif()
endif()

# JavaScriptCore (macOS/iOS)
if(MYSTRAL_USE_JSC)
    if(APPLE)
        find_library(JAVASCRIPTCORE_FRAMEWORK JavaScriptCore)
        if(JAVASCRIPTCORE_FRAMEWORK)
            add_compile_definitions(MYSTRAL_JS_JSC)
            message(STATUS "Found JavaScriptCore: ${JAVASCRIPTCORE_FRAMEWORK}")
        endif()
    else()
        message(WARNING "JavaScriptCore is only available on Apple platforms")
    endif()
endif()

# libwebp (for WebP image decoding - used by GLTF EXT_texture_webp extension)
# Note: webp prebuilts are only available for desktop platforms (macOS, Linux, Windows)
# iOS/Android cross-compilation would need webp built from source, which we skip for now
set(WEBP_DIR ${THIRD_PARTY_DIR}/webp)
if(EXISTS ${WEBP_DIR} AND NOT IOS AND NOT ANDROID)
    # libwebp prebuilt structure: webp/libwebp-{version}-{platform}/lib/libwebp.a
    file(GLOB WEBP_SUBDIRS ${WEBP_DIR}/libwebp-*)
    if(WEBP_SUBDIRS)
        list(GET WEBP_SUBDIRS 0 WEBP_ACTUAL_DIR)
        message(STATUS "Found libwebp directory: ${WEBP_ACTUAL_DIR}")

        # Find library
        if(WIN32)
            find_library(WEBP_LIBRARY
                NAMES webp libwebp
                PATHS ${WEBP_ACTUAL_DIR}/lib
                NO_DEFAULT_PATH
            )
        else()
            # Prefer static library
            if(EXISTS ${WEBP_ACTUAL_DIR}/lib/libwebp.a)
                set(WEBP_LIBRARY ${WEBP_ACTUAL_DIR}/lib/libwebp.a)
            else()
                find_library(WEBP_LIBRARY
                    NAMES webp
                    PATHS ${WEBP_ACTUAL_DIR}/lib
                    NO_DEFAULT_PATH
                )
            endif()
        endif()

        # Find headers
        if(EXISTS ${WEBP_ACTUAL_DIR}/include)
            set(WEBP_INCLUDE_DIR ${WEBP_ACTUAL_DIR}/include)
        endif()

        if(WEBP_LIBRARY AND WEBP_INCLUDE_DIR)
            add_library(webp::webp STATIC IMPORTED)
            set_target_properties(webp::webp PROPERTIES
                IMPORTED_LOCATION ${WEBP_LIBRARY}
                INTERFACE_INCLUDE_DIRECTORIES ${WEBP_INCLUDE_DIR}
            )
            add_compile_definitions(MYSTRAL_HAS_WEBP)
            message(STATUS "Found libwebp: ${WEBP_LIBRARY}")
            message(STATUS "libwebp includes: ${WEBP_INCLUDE_DIR}")
        else()
            message(WARNING "libwebp library or headers not found:")
            message(WARNING "  Library: ${WEBP_LIBRARY}")
            message(WARNING "  Include: ${WEBP_INCLUDE_DIR}")
        endif()
    else()
        message(WARNING "libwebp subdirectory not found in ${WEBP_DIR}")
    endif()
else()
    message(STATUS "libwebp not found - WebP texture support disabled. Run 'node scripts/download-deps.mjs --only webp' to enable.")
endif()

# Skia (for Canvas 2D implementation)
# All platforms now use olilarkin/skia-builder: https://github.com/olilarkin/skia-builder/releases
# Structure: skia/build/include/include/core/...
#            skia/build/{platform}-gpu/lib/Release/{arch}/libskia.a
if(MYSTRAL_PLATFORM STREQUAL "ios")
    set(SKIA_DIR ${THIRD_PARTY_DIR}/skia-ios)
    # iOS Skia structure: skia-ios/{device,simulator}/build/...
    if(CMAKE_OSX_SYSROOT MATCHES "simulator")
        set(SKIA_TARGET_DIR ${SKIA_DIR}/simulator)
        if(CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64")
            set(SKIA_LIB_SUBDIR "simulator-x86_64")
        else()
            set(SKIA_LIB_SUBDIR "simulator-arm64")
        endif()
        set(SKIA_PLATFORM_DIR "ios-gpu")
    else()
        set(SKIA_TARGET_DIR ${SKIA_DIR}/device)
        set(SKIA_LIB_SUBDIR "device-arm64")
        set(SKIA_PLATFORM_DIR "ios-gpu")
    endif()
else()
    set(SKIA_DIR ${THIRD_PARTY_DIR}/skia)
    set(SKIA_TARGET_DIR ${SKIA_DIR})

    # Determine platform directory and architecture
    if(WIN32)
        set(SKIA_PLATFORM_DIR "win-gpu")
        set(SKIA_LIB_SUBDIR "x64")
    elseif(APPLE)
        set(SKIA_PLATFORM_DIR "mac-gpu")
        if(CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64" OR CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
            set(SKIA_LIB_SUBDIR "arm64")
        else()
            set(SKIA_LIB_SUBDIR "x86_64")
        endif()
    else()
        set(SKIA_PLATFORM_DIR "linux-gpu")
        set(SKIA_LIB_SUBDIR "x64")
    endif()
endif()

if(EXISTS ${SKIA_TARGET_DIR})
    message(STATUS "Found Skia directory: ${SKIA_TARGET_DIR}")

    # olilarkin/skia-builder structure (used for all platforms now)
    # Headers: build/include/include/core/...
    # Library: build/{platform}-gpu/lib/Release/{arch}/libskia.a
    if(EXISTS ${SKIA_TARGET_DIR}/build/include/include)
        set(SKIA_INCLUDE_DIR ${SKIA_TARGET_DIR}/build/include)
    endif()

    # Find library
    if(WIN32)
        set(SKIA_LIB_PATH ${SKIA_TARGET_DIR}/build/${SKIA_PLATFORM_DIR}/lib/Release/${SKIA_LIB_SUBDIR}/skia.lib)
    else()
        set(SKIA_LIB_PATH ${SKIA_TARGET_DIR}/build/${SKIA_PLATFORM_DIR}/lib/Release/${SKIA_LIB_SUBDIR}/libskia.a)
    endif()

    if(EXISTS ${SKIA_LIB_PATH})
        set(SKIA_LIBRARY ${SKIA_LIB_PATH})
    endif()

    if(SKIA_LIBRARY AND SKIA_INCLUDE_DIR)
        add_library(skia::skia STATIC IMPORTED)
        set_target_properties(skia::skia PROPERTIES
            IMPORTED_LOCATION ${SKIA_LIBRARY}
            INTERFACE_INCLUDE_DIRECTORIES ${SKIA_INCLUDE_DIR}
        )
        add_compile_definitions(MYSTRAL_HAS_SKIA)
        message(STATUS "Found Skia: ${SKIA_LIBRARY}")
        message(STATUS "Skia includes: ${SKIA_INCLUDE_DIR}")
    else()
        message(WARNING "Skia library or headers not found:")
        message(WARNING "  Library: ${SKIA_LIB_PATH}")
        message(WARNING "  Include: ${SKIA_INCLUDE_DIR}")
    endif()
else()
    if(MYSTRAL_PLATFORM STREQUAL "ios")
        message(STATUS "Skia iOS not found - Canvas 2D support disabled. Run 'node scripts/download-deps.mjs --only skia-ios' to enable.")
    else()
        message(STATUS "Skia not found - Canvas 2D support disabled. Run 'node scripts/download-deps.mjs --only skia' to enable.")
    endif()
endif()

# SWC (TypeScript transpiler)
# Prebuilt layout: swc/include/swc.h and swc/lib/libswc.a (or swc.lib on Windows)
set(SWC_DIR ${THIRD_PARTY_DIR}/swc)
if(MYSTRAL_USE_SWC AND EXISTS ${SWC_DIR} AND NOT IOS AND NOT ANDROID)
    if(EXISTS ${SWC_DIR}/include)
        set(SWC_INCLUDE_DIR ${SWC_DIR}/include)
    endif()

    if(WIN32)
        set(SWC_LIB_PATH ${SWC_DIR}/lib/swc.lib)
    else()
        set(SWC_LIB_PATH ${SWC_DIR}/lib/libswc.a)
    endif()

    if(EXISTS ${SWC_LIB_PATH})
        set(SWC_LIBRARY ${SWC_LIB_PATH})
    else()
        find_library(SWC_LIBRARY
            NAMES swc libswc
            PATHS ${SWC_DIR}/lib ${SWC_DIR}
            NO_DEFAULT_PATH
        )
    endif()

    if(SWC_LIBRARY AND SWC_INCLUDE_DIR)
        add_library(swc::swc STATIC IMPORTED)
        set_target_properties(swc::swc PROPERTIES
            IMPORTED_LOCATION ${SWC_LIBRARY}
            INTERFACE_INCLUDE_DIRECTORIES ${SWC_INCLUDE_DIR}
        )
        add_compile_definitions(MYSTRAL_HAS_SWC)
        message(STATUS "Found SWC: ${SWC_LIBRARY}")
        message(STATUS "SWC includes: ${SWC_INCLUDE_DIR}")
    else()
        message(WARNING "SWC library or headers not found:")
        message(WARNING "  Library: ${SWC_LIBRARY}")
        message(WARNING "  Include: ${SWC_INCLUDE_DIR}")
    endif()
else()
    if(MYSTRAL_USE_SWC)
        message(STATUS "SWC not found - TypeScript transpiling disabled. Run 'node scripts/download-deps.mjs --only swc' to enable.")
    endif()
endif()

# libuv (async I/O library for non-blocking network, file, and timer operations)
# Prebuilt layout: libuv/include/uv.h, libuv/include/uv/*.h, libuv/libuv.a (or libuv.lib)
# Note: libuv is only used on desktop platforms; iOS/Android use platform-native async APIs
set(LIBUV_DIR ${THIRD_PARTY_DIR}/libuv)
if(EXISTS ${LIBUV_DIR} AND NOT IOS AND NOT ANDROID)
    # Headers are directly in include/
    if(EXISTS ${LIBUV_DIR}/include)
        set(LIBUV_INCLUDE_DIR ${LIBUV_DIR}/include)
    endif()

    # Library is at root level (from library-builder output)
    if(WIN32)
        set(LIBUV_LIB_PATH ${LIBUV_DIR}/libuv.lib)
    else()
        set(LIBUV_LIB_PATH ${LIBUV_DIR}/libuv.a)
    endif()

    if(EXISTS ${LIBUV_LIB_PATH})
        set(LIBUV_LIBRARY ${LIBUV_LIB_PATH})
    else()
        find_library(LIBUV_LIBRARY
            NAMES uv libuv
            PATHS ${LIBUV_DIR} ${LIBUV_DIR}/lib
            NO_DEFAULT_PATH
        )
    endif()

    if(LIBUV_LIBRARY AND LIBUV_INCLUDE_DIR)
        add_library(libuv::libuv STATIC IMPORTED)
        set_target_properties(libuv::libuv PROPERTIES
            IMPORTED_LOCATION ${LIBUV_LIBRARY}
            INTERFACE_INCLUDE_DIRECTORIES ${LIBUV_INCLUDE_DIR}
        )
        add_compile_definitions(MYSTRAL_HAS_LIBUV)
        message(STATUS "Found libuv: ${LIBUV_LIBRARY}")
        message(STATUS "libuv includes: ${LIBUV_INCLUDE_DIR}")

        # libuv requires additional system libraries on each platform
        if(WIN32)
            # Windows: network, system, and debug libraries
            # dbghelp is required for uv_kill stack trace functionality
            set(LIBUV_SYSTEM_LIBS iphlpapi psapi userenv ws2_32 dbghelp)
        elseif(UNIX AND NOT APPLE)
            # Linux: pthread and dl
            set(LIBUV_SYSTEM_LIBS pthread dl)
        endif()
    else()
        message(WARNING "libuv library or headers not found:")
        message(WARNING "  Library: ${LIBUV_LIB_PATH}")
        message(WARNING "  Include: ${LIBUV_INCLUDE_DIR}")
    endif()
else()
    message(STATUS "libuv not found - async I/O disabled. Run 'node scripts/download-deps.mjs --only libuv' to enable.")
endif()

# ============================================================================
# Main Library
# ============================================================================

# Android requires shared library for JNI/SDL loading
set(MYSTRAL_SOURCES
    src/runtime.cpp
    src/js/engine_factory.cpp
    src/js/module_resolver.cpp
    src/js/module_system.cpp
    src/js/ts_transpiler.cpp
    src/webgpu/bindings.cpp
    src/webgpu/context.cpp
    src/canvas/canvas.cpp
    src/canvas/canvas2d.cpp
    src/canvas/canvas2d_bindings.cpp
    src/input/input_shim.cpp
    src/platform/window.cpp
    src/platform/input.cpp
    src/utils/stb_impl.cpp
    src/utils/cgltf_impl.cpp
    src/http/http_client.cpp
    src/http/async_http_client.cpp
    src/fs/async_file.cpp
    src/fs/file_watcher.cpp
    src/gltf/gltf_loader.cpp
    src/audio/audio_context.cpp
    src/audio/audio_bindings.cpp
    src/vfs/embedded_bundle.cpp
    src/async/event_loop.cpp
)

if(ANDROID)
    add_library(mystral-runtime SHARED ${MYSTRAL_SOURCES})
else()
    add_library(mystral-runtime STATIC ${MYSTRAL_SOURCES})
endif()

target_include_directories(mystral-runtime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_include_directories(mystral-runtime PRIVATE
    ${THIRD_PARTY_DIR}/stb
    ${THIRD_PARTY_DIR}/cgltf
)

# Platform-specific sources
if(APPLE)
    target_sources(mystral-runtime PRIVATE
        src/platform/surface_metal.mm
    )
    if(MYSTRAL_USE_JSC)
        target_sources(mystral-runtime PRIVATE src/js/jsc_engine.mm)
        target_link_libraries(mystral-runtime PRIVATE ${JAVASCRIPTCORE_FRAMEWORK})
    endif()
elseif(WIN32)
    target_sources(mystral-runtime PRIVATE
        src/platform/surface_win32.cpp
    )
elseif(ANDROID)
    target_sources(mystral-runtime PRIVATE
        src/platform/surface_android.cpp
        src/platform/android_main.cpp
    )
elseif(UNIX)
    target_sources(mystral-runtime PRIVATE
        src/platform/surface_linux.cpp
    )
endif()

# JS engine linkage
if(MYSTRAL_USE_QUICKJS AND TARGET quickjs)
    target_sources(mystral-runtime PRIVATE src/js/quickjs_engine.cpp)
    target_link_libraries(mystral-runtime PRIVATE quickjs)
endif()

if(MYSTRAL_USE_V8 AND TARGET v8::v8)
    target_sources(mystral-runtime PRIVATE src/js/v8_engine.cpp)
    # V8 requires C++20 - force it at target level for MSVC
    # MSVC needs /Zc:__cplusplus to report the correct __cplusplus value
    # (V8 checks __cplusplus <= 201703L which fails without this flag)
    if(MSVC)
        target_compile_options(mystral-runtime PRIVATE /std:c++20 /Zc:__cplusplus)
    else()
        target_compile_features(mystral-runtime PRIVATE cxx_std_20)
    endif()
    # On macOS with Dawn, V8 is linked specially in the Dawn section with -force_load
    # to handle Abseil symbol conflicts. Skip normal linking here but keep include dirs.
    if(APPLE AND MYSTRAL_USE_DAWN)
        # Only add include directories, actual linking happens in Dawn section
        target_include_directories(mystral-runtime PRIVATE ${V8_INCLUDE_DIR})
    else()
        target_link_libraries(mystral-runtime PRIVATE v8::v8)
    endif()
    if(APPLE)
        target_link_libraries(mystral-runtime PRIVATE ${COREFOUNDATION_FRAMEWORK})
    elseif(V8_SYSTEM_LIBS)
        target_link_libraries(mystral-runtime PRIVATE ${V8_SYSTEM_LIBS})
    endif()
endif()

# SDL3 linkage - prefer static target for single-binary distribution
if(SDL3_STATIC_TARGET)
    target_link_libraries(mystral-runtime PRIVATE ${SDL3_STATIC_TARGET})
    message(STATUS "Linking SDL3 statically via ${SDL3_STATIC_TARGET}")
elseif(TARGET SDL3::SDL3-static)
    target_link_libraries(mystral-runtime PRIVATE SDL3::SDL3-static)
elseif(TARGET SDL3::SDL3)
    target_link_libraries(mystral-runtime PRIVATE SDL3::SDL3)
elseif(SDL3_LIBRARY)
    target_link_libraries(mystral-runtime PRIVATE ${SDL3_LIBRARY})
    target_include_directories(mystral-runtime PRIVATE ${SDL3_INCLUDE_DIR})
endif()

# WebGPU linkage
if(MYSTRAL_USE_WGPU AND TARGET wgpu::wgpu)
    # wgpu-native is a Rust library. If SWC (also Rust) is enabled, they share
    # Rust runtime symbols. On macOS, use -force_load on wgpu to ensure its symbols
    # are used first, then swc's duplicates will be ignored.
    if(APPLE AND TARGET swc::swc)
        get_target_property(WGPU_LOCATION wgpu::wgpu IMPORTED_LOCATION)
        get_target_property(WGPU_INCLUDES wgpu::wgpu INTERFACE_INCLUDE_DIRECTORIES)
        target_include_directories(mystral-runtime PRIVATE ${WGPU_INCLUDES})
        target_link_libraries(mystral-runtime PRIVATE
            "-Wl,-force_load,${WGPU_LOCATION}"
        )
        target_link_options(mystral-runtime PUBLIC "-Wl,-dead_strip")
    else()
        target_link_libraries(mystral-runtime PRIVATE wgpu::wgpu)
    endif()
endif()

if(MYSTRAL_USE_DAWN AND TARGET dawn::webgpu)
    # Dawn and V8 both include Abseil library, causing duplicate symbol errors.
    # On macOS, we need to handle the link order and flags carefully.
    if(APPLE AND MYSTRAL_USE_V8)
        # Use -force_load for V8 to ensure its Abseil symbols are loaded first,
        # then link Dawn normally. The linker will use the first definition.
        # We also suppress the deprecated -ld_classic warning and use -dead_strip
        # to remove unused duplicate symbols.
        get_target_property(V8_LOCATION v8::v8 IMPORTED_LOCATION)
        target_link_libraries(mystral-runtime PRIVATE
            "-Wl,-force_load,${V8_LOCATION}"
        )
        # Link Dawn after V8 - its duplicate symbols will be ignored
        target_link_libraries(mystral-runtime PRIVATE dawn::webgpu)
        target_link_options(mystral-runtime PUBLIC "-Wl,-dead_strip")
    else()
        target_link_libraries(mystral-runtime PRIVATE dawn::webgpu)
    endif()

    # Dawn requires additional dependencies
    if(APPLE)
        target_link_libraries(mystral-runtime PRIVATE
            "-framework IOKit"
            "-framework IOSurface"
        )
    elseif(WIN32)
        # Dawn D3D11/D3D12 SharedFence uses CompareObjectHandles from kernelbase.
        # OneCoreUap.lib is the umbrella library for Windows 10+ APIs that includes
        # all the necessary API sets. It's always available in the Windows SDK.
        target_link_libraries(mystral-runtime PUBLIC OneCoreUap.lib)
        # Dawn and V8 both include Abseil library, causing duplicate symbol errors.
        # Use /FORCE:MULTIPLE to ignore duplicate definitions on MSVC.
        if(MYSTRAL_USE_V8)
            target_link_options(mystral-runtime PUBLIC "/FORCE:MULTIPLE")
        endif()
    elseif(UNIX)
        # Dawn and V8 both include Abseil library, causing multiple definition errors.
        # Allow multiple definitions to resolve this conflict.
        if(MYSTRAL_USE_V8)
            target_link_options(mystral-runtime PUBLIC "-Wl,--allow-multiple-definition")
        endif()
    endif()
    # Note: Linux Dawn builds require Ubuntu 24.04+ (GCC 13+) for proper libstdc++ ABI
endif()

# Platform libraries
if(APPLE)
    target_link_libraries(mystral-runtime PRIVATE
        "-framework Metal"
        "-framework QuartzCore"
    )
    if(MYSTRAL_PLATFORM STREQUAL "ios")
        target_link_libraries(mystral-runtime PRIVATE
            "-framework UIKit"
            "-framework Foundation"
        )
    else()
        target_link_libraries(mystral-runtime PRIVATE
            "-framework Cocoa"
        )
    endif()
elseif(WIN32)
    target_link_libraries(mystral-runtime PRIVATE
        d3d12
        dxgi
        dxguid
    )
elseif(ANDROID)
    # Android NDK libraries
    target_link_libraries(mystral-runtime PRIVATE
        android        # ANativeWindow, AAssetManager
        log            # __android_log_print
        GLESv3         # OpenGL ES (required by wgpu-native)
        EGL            # EGL (required by wgpu-native)
    )
endif()

# HTTP client dependencies (libcurl + zlib)
# Note: These must be PUBLIC because mystral-runtime is a static library
# and the final executables need these dependencies to link correctly on Linux
# On iOS/Android, HTTP is handled by the OS (NSURLSession / HttpURLConnection)
if(MYSTRAL_PLATFORM STREQUAL "ios")
    # iOS uses Foundation/NSURLSession for HTTP
    add_compile_definitions(MYSTRAL_HTTP_FOUNDATION)
elseif(MYSTRAL_PLATFORM STREQUAL "android")
    # Android uses Java HttpURLConnection via JNI
    add_compile_definitions(MYSTRAL_HTTP_ANDROID)
else()
    # Desktop platforms use libcurl
    find_package(CURL REQUIRED)
    find_package(ZLIB REQUIRED)
    target_link_libraries(mystral-runtime PUBLIC CURL::libcurl ZLIB::ZLIB)
    message(STATUS "Found CURL: ${CURL_LIBRARIES}")
    message(STATUS "Found ZLIB: ${ZLIB_LIBRARIES}")
endif()

# libwebp linkage (optional - for WebP texture support)
if(TARGET webp::webp)
    target_link_libraries(mystral-runtime PRIVATE webp::webp)
endif()

# Skia linkage (optional - for Canvas 2D support)
if(TARGET skia::skia)
    target_link_libraries(mystral-runtime PRIVATE skia::skia)
endif()

# SWC linkage (TypeScript transpiler)
if(TARGET swc::swc)
    # Must be PUBLIC so dependents of the static runtime link SWC too.
    target_link_libraries(mystral-runtime PUBLIC swc::swc)
    # SWC (Rust) on Windows needs these system libraries
    if(WIN32)
        target_link_libraries(mystral-runtime PUBLIC userenv ntdll)
    endif()
endif()

# libuv linkage (async I/O - non-blocking HTTP, file I/O, timers)
if(TARGET libuv::libuv)
    target_link_libraries(mystral-runtime PUBLIC libuv::libuv)
    # libuv requires platform-specific system libraries
    if(LIBUV_SYSTEM_LIBS)
        target_link_libraries(mystral-runtime PUBLIC ${LIBUV_SYSTEM_LIBS})
    endif()
endif()

# ============================================================================
# CLI Executable (desktop only)
# ============================================================================

if(NOT MYSTRAL_PLATFORM STREQUAL "ios" AND NOT MYSTRAL_PLATFORM STREQUAL "android")
    add_executable(mystral src/cli/main.cpp)
    target_link_libraries(mystral PRIVATE mystral-runtime)

    # macOS: Set rpath for redistributable binary
    # The binary will look for SDL3.framework next to the executable
    if(APPLE)
        set_target_properties(mystral PROPERTIES
            MACOSX_RPATH ON
            # Use install rpath even in build output (for redistribution)
            BUILD_WITH_INSTALL_RPATH ON
            INSTALL_RPATH "@executable_path"
        )
    endif()

    # On Linux, the GNU linker needs help finding zlib (curl's transitive dependency).
    # Use --copy-dt-needed-entries to pull in dependencies from shared libraries.
    # Also allow multiple definitions because wgpu-native and swc both contain Rust runtime symbols.
    if(UNIX AND NOT APPLE)
        target_link_options(mystral PRIVATE
            "LINKER:--copy-dt-needed-entries"
            "LINKER:--allow-multiple-definition"
        )
    endif()

    # Install CLI to bin directory
    install(TARGETS mystral
        RUNTIME DESTINATION bin
    )

    # ============================================================================
    # Examples (desktop only)
    # ============================================================================

    # Embed Example - Shows how to embed the runtime in a C++ application
    # For most development, use the CLI instead: mystral run game.js
    add_executable(embed-example examples/embed-example/main.cpp)
    target_link_libraries(embed-example PRIVATE mystral-runtime)

    # macOS: Set rpath for redistributable binary
    if(APPLE)
        set_target_properties(embed-example PROPERTIES
            MACOSX_RPATH ON
            BUILD_WITH_INSTALL_RPATH ON
            INSTALL_RPATH "@executable_path"
        )
    endif()

    # On Linux, the GNU linker needs help finding zlib (curl's transitive dependency).
    # Also allow multiple definitions because wgpu-native and swc both contain Rust runtime symbols.
    if(UNIX AND NOT APPLE)
        target_link_options(embed-example PRIVATE
            "LINKER:--copy-dt-needed-entries"
            "LINKER:--allow-multiple-definition"
        )
    endif()
endif()

# ============================================================================
# Install
# ============================================================================

install(TARGETS mystral-runtime
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)

install(DIRECTORY include/mystral
    DESTINATION include
)
