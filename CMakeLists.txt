cmake_minimum_required(VERSION 3.21)

# DLL name: TheWatcher.dll
project(TheWatcher VERSION 13.0.0 LANGUAGES CXX)

if("${PROJECT_SOURCE_DIR}" STREQUAL "${PROJECT_BINARY_DIR}")
	message(FATAL_ERROR "in-source builds are not allowed")
endif()

# Build output stays in the build folder only (build\release-msvc or build\debug-msvc).
# Nothing is copied anywhere else; copy TheWatcher.dll, TheWatcher.pdb, TheWatcherDump.exe and TheWatcher.ini yourself.

# ---------------------------------------------------------------------------------------------
# CommonLibSSE-NG 6.7.0 source tree (same copy Precision Animal Excluder builds against).
# 6.7.0 reads the AE 1.7.x Address Library (format 5), models the 1.7.99 layout changes and sets
# the SKSE "Address Library V5" flag; without that flag SKSE 2.3.x refuses the DLL on 1.7.x.
# The old vcpkg commonlibsse-ng-fork (2025-03) has none of this.
# Override the location with the CommonLibSSEPath_NG environment variable.
# ---------------------------------------------------------------------------------------------
if(DEFINED ENV{CommonLibSSEPath_NG} AND IS_DIRECTORY "$ENV{CommonLibSSEPath_NG}")
	set(COMMONLIB_DIR "$ENV{CommonLibSSEPath_NG}")
else()
	set(COMMONLIB_DIR "C:/dllmaker/CommonLibSSE-NG")
endif()
message(STATUS "Using CommonLibSSE-NG from: ${COMMONLIB_DIR}")

set(ENABLE_SKYRIM_SE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_AE ON CACHE BOOL "" FORCE)
# VR needs the OpenVR SDK headers and import library in CommonLibSSE-NG\extern\openvr
# (headers\openvr.h and lib\win64\openvr_api.lib, OpenVR SDK 2.15.6). The Watcher calls no OpenVR
# function, so the DLL does not load openvr_api.dll on SE or AE.
set(ENABLE_SKYRIM_VR ON CACHE BOOL "" FORCE)
if(NOT EXISTS "${COMMONLIB_DIR}/extern/openvr/headers/openvr.h" OR NOT EXISTS "${COMMONLIB_DIR}/extern/openvr/lib/win64/openvr_api.lib")
	message(FATAL_ERROR "Skyrim VR support needs ${COMMONLIB_DIR}/extern/openvr/headers/openvr.h and lib/win64/openvr_api.lib (OpenVR SDK)")
endif()
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
# Patch-safety checks download a decoder with git at configure time; not needed here
set(SKSE_SUPPORT_PATCH_SAFETY OFF CACHE BOOL "" FORCE)

add_subdirectory("${COMMONLIB_DIR}" CommonLibSSE EXCLUDE_FROM_ALL)
# add_commonlibsse_plugin() helper (only included automatically for prebuilt CommonLib)
if(NOT COMMAND add_commonlibsse_plugin)
	include("${COMMONLIB_DIR}/cmake/CommonLibSSE.cmake")
endif()

include(cmake/headerlist.cmake)
include(cmake/sourcelist.cmake)
add_commonlibsse_plugin(${PROJECT_NAME} SOURCES ${headers} ${sources})

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)
target_include_directories(${PROJECT_NAME} PRIVATE src)
target_compile_options(${PROJECT_NAME} PRIVATE /utf-8)

# Release builds make a PDB too (/Z7 keeps debug info in the .obj files, so parallel builds never fight over one PDB)
target_compile_options(${PROJECT_NAME} PRIVATE $<$<CONFIG:Release>:/Z7>)
target_link_options(${PROJECT_NAME} PRIVATE $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)

# Stack walking / minidumps (dbghelp), process memory stats (psapi), per-process VRAM (dxgi), Documents folder lookup (shell32, ole32)
target_link_libraries(${PROJECT_NAME} PRIVATE dbghelp psapi dxgi shell32 ole32 advapi32)

# Helper program that writes the minidump from outside the frozen game. Ship it next to TheWatcher.dll.
add_executable(TheWatcherDump src/DumpHelper.cpp)
target_compile_features(TheWatcherDump PRIVATE cxx_std_20)
target_link_libraries(TheWatcherDump PRIVATE dbghelp user32 advapi32)
