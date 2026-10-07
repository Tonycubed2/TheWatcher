#include "PCH.h"
#include "Independent.h"
#include "IndependentShared.h"
#include "Hotkey.h"
#include "Settings.h"
#include "Monitor.h"
#include "Util.h"

namespace Independent
{
    namespace {
        std::atomic<IndependentShared*> g_shared{nullptr};
        HANDLE g_mapping = nullptr;
        HANDLE g_process = nullptr;
        LONG g_seen = 0;
        LONG g_seenAutomatic = 0;
        std::atomic<bool> g_localBusy{false}; // only consumed by the existing watchdog
    }
    void Frame() {
        if (auto p = g_shared.load(std::memory_order_acquire))
            InterlockedExchange64(&p->frameTick, static_cast<LONG64>(GetTickCount64()));
    }
    void LoadProgress() {
        if (auto p = g_shared.load(std::memory_order_acquire))
            InterlockedExchange64(&p->progressTick, static_cast<LONG64>(GetTickCount64()));
    }
    void LoadStart() {
        LoadProgress();
        if (auto p = g_shared.load(std::memory_order_acquire)) InterlockedExchange(&p->loading, 1);
    }
    void LoadEnd() {
        Frame();
        if (auto p = g_shared.load(std::memory_order_acquire)) InterlockedExchange(&p->loading, 0);
    }
    bool Active() {
        auto p = g_shared.load(std::memory_order_acquire);
        if (!p || !g_process) return false;
        if (WaitForSingleObject(g_process, 0) != WAIT_TIMEOUT) {
            static bool reported = false;
            if (!reported) {
                reported = true;
                DWORD code = 0; GetExitCodeProcess(g_process, &code);
                spdlog::error("Independent helper exited (code {}); falling back to in-game hotkey polling. Verify the version 10 EXE and independent logs", code);
            }
            return false;
        }
        // While the helper is alive it owns the hotkey, including startup.
        // A stale helper tick is diagnostic, not a reason to start a duplicate dump.
        return true;
    }
    bool PendingManual() {
        auto p = g_shared.load(std::memory_order_acquire);
        return p && ReadFlag(&p->manualCompleted) != g_seen;
    }
    bool PendingAutomatic() {
        auto p = g_shared.load(std::memory_order_acquire);
        return p && ReadFlag(&p->automaticCompleted) != g_seenAutomatic;
    }
    void Acknowledge(bool manual) { if (manual) ++g_seen; else ++g_seenAutomatic; }
    Guard::Guard(bool capture) : capture_(capture) {
        auto p = g_shared.load(std::memory_order_acquire);
        shared_ = p;
        if (p) held_ = InterlockedCompareExchange(&p->captureOwner, capture_ ? 1 : 3, 0) == 0;
        else { bool expected = false; held_ = g_localBusy.compare_exchange_strong(expected, true); }
    }
    Guard::~Guard() {
        if (!held_) return;
        if (auto p = static_cast<IndependentShared*>(shared_)) {
            if (capture_) InterlockedExchange64(&p->captureEndTick, static_cast<LONG64>(GetTickCount64()));
            InterlockedCompareExchange(&p->captureOwner, 0, capture_ ? 1 : 3);
        } else g_localBusy.store(false);
    }
    void Start() {
        const auto& cfg = Settings::Get();
        if (!cfg.independentWatchdog || g_shared.load()) return;
        HMODULE self = nullptr;
        wchar_t path[32768]{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&Start), &self) || !GetModuleFileNameW(self, path, 32768)) return;
        const auto exe = std::filesystem::path(path).parent_path() / L"TheWatcherDump.exe";
        const auto name = std::format(L"Local\\TheWatcher10_{}_{}", GetCurrentProcessId(), GetTickCount64());
        g_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(IndependentShared), name.c_str());
        if (!g_mapping) { spdlog::error("Independent mapping failed: {}", GetLastError()); return; }
        auto p = static_cast<IndependentShared*>(MapViewOfFile(g_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(IndependentShared)));
        if (!p) { CloseHandle(g_mapping); g_mapping = nullptr; spdlog::error("Independent mapping view failed"); return; }
        *p = IndependentShared{};
        p->pid = GetCurrentProcessId();
        Hotkey::GetBinding(p->key, p->modifiers);
        p->holdMs = static_cast<DWORD>(cfg.hotkeyHoldSec * 1000);
        p->frameMs = static_cast<DWORD>(cfg.independentFrameSec * 1000);
        p->loadMs = static_cast<DWORD>(cfg.loadStallSec * 1000);
        p->recaptureMs = static_cast<DWORD>(cfg.recaptureSec * 1000);
        p->maxCaptures = static_cast<DWORD>(cfg.maxCaptures);
        p->dumpLevel = static_cast<DWORD>(cfg.minidumpLevel);
        p->manualDump = cfg.hotkeyMinidump ? 1 : 0;
        p->keepManual = cfg.keepManualCaptures;
        p->keepEmergency = cfg.keepStallCaptures;
        p->captureLoading = cfg.captureLoadingStalls ? 1 : 0;
        p->maxStorageMB = static_cast<DWORD>(cfg.maxStorageMB);
        FILETIME created{}, exited{}, kernel{}, user{};
        if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
            p->targetCreationTime = (static_cast<ULONGLONG>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
        if (Monitor::Get().loading.load()) {
            p->loading = 1;
            p->progressTick = static_cast<LONG64>(GetTickCount64());
        }
        const auto out = Util::WatchdogDir();
        std::error_code ec;
        std::filesystem::create_directories(out, ec);
        std::wstring cmd = std::format(L"\"{}\" --watch {} \"{}\" \"{}\"", exe.wstring(), p->pid, name, out.wstring());
        STARTUPINFOW si{}; si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            const auto error = GetLastError();
            UnmapViewOfFile(p); CloseHandle(g_mapping); g_mapping = nullptr;
            spdlog::error("Independent helper could not start (error {}); install the VERSION 10 TheWatcherDump.exe beside the DLL", error);
            return;
        }
        CloseHandle(pi.hThread);
        g_process = pi.hProcess;
        // Mapping and process handle live for this game session. Avoid unloading races with heartbeat writers.
        g_shared.store(p, std::memory_order_release);
        spdlog::info("Independent helper started: pid {}, frame threshold {:.1f}s; hotkey and detection ignore window focus", pi.dwProcessId, cfg.independentFrameSec);
    }
}
