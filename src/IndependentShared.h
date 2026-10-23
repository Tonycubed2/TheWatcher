#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

// Fixed x64 IPC layout. All mutable fields use Windows interlocked operations;
// no game mutex, allocator, logger or engine API is used by heartbeat writers.
inline constexpr DWORD kIndependentProtocol = 13; // requires the matching startup-fix helper
struct alignas(8) IndependentShared
{
    DWORD version = kIndependentProtocol;
    DWORD pid = 0;
    DWORD key = 0;
    DWORD modifiers = 0; // shift=1, ctrl=2, alt=4
    DWORD holdMs = 0;
    DWORD frameMs = 8000;
    DWORD loadMs = 300000;
    DWORD recaptureMs = 45000;
    DWORD maxCaptures = 3;
    DWORD dumpLevel = 0;
    DWORD manualDump = 1;
    DWORD keepManual = 10;
    DWORD keepEmergency = 10;
    DWORD captureLoading = 0;
    DWORD maxStorageMB = 1024;
    alignas(8) ULONGLONG targetCreationTime = 0;
    alignas(8) volatile LONG64 frameTick = 0;
    alignas(8) volatile LONG64 progressTick = 0;
    alignas(8) volatile LONG64 helperTick = 0;
    volatile LONG loading = 0;
    volatile LONG manualCompleted = 0;
    volatile LONG automaticCompleted = 0;
    volatile LONG captureOwner = 0; // 0 free, 1 plugin, 2 external worker, 3 brief profiler/history sampling
    alignas(8) volatile LONG64 captureEndTick = 0;
    DWORD waitChains = 1;
    volatile LONG mainThreadId = 0;
    // (12) Refreshed by the plugin while the main thread is compiling shaders (bIgnoreShaderCompiles=1).
    // The helper treats it like a frame heartbeat, so a deliberate shader-compile pause is not a freeze.
    // Protocol 13 requires matching binaries; older helpers are rejected.
    alignas(8) volatile LONG64 shaderTick = 0;
    volatile LONG gameReady = 0; // manual capture works during startup; automatic capture stays off
};
inline LONG64 ReadTick(volatile LONG64* p) { return InterlockedCompareExchange64(p, 0, 0); }
inline LONG ReadFlag(volatile LONG* p) { return InterlockedCompareExchange(p, 0, 0); }
