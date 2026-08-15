cmake_minimum_required(VERSION 3.16)
project(UEHook VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# This project has no vcpkg dependencies. Disable the machine-wide vcpkg MSBuild
# integration for the generated projects so its applocal step (which shells out
# to pwsh.exe) doesn't run. Harmless either way, but keeps build output clean.
set(CMAKE_VS_GLOBALS "VcpkgEnabled=false")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# ---- reflection backend (static library) -------------------------------------
set(UEHOOK_SOURCES
    src/UEHook.cpp

    src/Engine/OffsetFinder.cpp

    src/Reflection/CoreUObject.cpp

    src/Unreal/NameArray.cpp
    src/Unreal/ObjectArray.cpp

    src/Core/UEGlobalsDetector.cpp
    src/Core/MemoryScanner.cpp
    src/Core/Hooking.cpp

    # MinHook
    src/ThirdParty/MinHook/MinHook.cpp
    src/ThirdParty/MinHook/Buffer.cpp
    src/ThirdParty/MinHook/Trampoline.cpp
    src/ThirdParty/MinHook/hde/hde64.cpp
)

add_library(UEHook STATIC ${UEHOOK_SOURCES})

# Consumers include "UEHook.h" (in include/) which pulls headers from src/.
target_include_directories(UEHook PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(UEHook PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

if(WIN32)
    target_link_libraries(UEHook PUBLIC kernel32 user32 psapi)
endif()

if(MSVC)
    target_compile_options(UEHook PRIVATE /MP /W3)
endif()

# ---- example mod DLL ---------------------------------------------------------
option(BUILD_EXAMPLES "Build example mod DLLs" ON)

if(BUILD_EXAMPLES)
    add_library(example_mod SHARED examples/example_mod.cpp)
    target_link_libraries(example_mod PRIVATE UEHook)
endif()
