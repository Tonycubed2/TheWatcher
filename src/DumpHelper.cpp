// Version 8: one-shot dump writer AND persistent independent watchdog.
// --watch <pid> <mapping> <directory>; --capture <pid> <folder> <level> <mapping>
// Legacy syntax remains: <pid> <output.dmp> <level 1-3>.
#include "IndependentShared.h"
#include "IndependentDecision.h"
#include "DumpValidation.h"
#include "StoragePolicy.h"
#include <DbgHelp.h>
#include <TlHelp32.h>
#include <algorithm>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
// Bind each helper to the original target HANDLE, not a later process reusing its PID.
// Never terminate a worker while Skyrim is alive: it may briefly own a suspended thread.
class TargetLifetime {
    HANDLE process_ = nullptr, stop_ = nullptr, thread_ = nullptr;
    static DWORD WINAPI WaitForTarget(void* self) {
        auto& target = *static_cast<TargetLifetime*>(self);
        HANDLE handles[] = {target.stop_, target.process_};
        if (WaitForMultipleObjects(2, handles, FALSE, INFINITE) == WAIT_OBJECT_0 + 1)
            TerminateProcess(GetCurrentProcess(), 11); // only this helper, and only AFTER Skyrim has exited
        return 0;
    }
public:
    explicit TargetLifetime(DWORD pid, ULONGLONG expectedCreation = 0) {
        process_ = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process_ || WaitForSingleObject(process_, 0) != WAIT_TIMEOUT) return;
        FILETIME created{}, exited{}, kernel{}, user{};
        if (!GetProcessTimes(process_, &created, &exited, &kernel, &user)) return;
        const auto creation = (static_cast<ULONGLONG>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
        if (expectedCreation && creation != expectedCreation) return;
        stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (stop_) thread_ = CreateThread(nullptr, 0, WaitForTarget, this, 0, nullptr);
    }
    explicit operator bool() const { return thread_ != nullptr; }
    ~TargetLifetime() {
        if (stop_) SetEvent(stop_);
        if (thread_) { WaitForSingleObject(thread_, INFINITE); CloseHandle(thread_); }
        if (stop_) CloseHandle(stop_);
        if (process_) CloseHandle(process_);
    }
    TargetLifetime(const TargetLifetime&) = delete;
    TargetLifetime& operator=(const TargetLifetime&) = delete;
};
static void Log(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::app);
    out << GetTickCount64() << " ms: " << text << '\n';
    out.flush();
}
static int Dump(DWORD pid, const fs::path& output, int level, const fs::path& log) {
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_DUP_HANDLE, FALSE, pid);
    if (!process) { Log(log, "OpenProcess failed: " + std::to_string(GetLastError())); return 3; }
    const auto partial = fs::path(output.wstring() + L".partial");
    HANDLE file = CreateFileW(partial.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        Log(log, "CreateFile failed: " + std::to_string(GetLastError())); CloseHandle(process); return 4;
    }
    DWORD type = MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules;
    if (level >= 2) type |= MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithHandleData | MiniDumpWithProcessThreadData;
    if (level >= 3) type |= MiniDumpWithFullMemory | MiniDumpWithFullMemoryInfo;
    Log(log, "dump started, level " + std::to_string(level));
    BOOL ok = MiniDumpWriteDump(process, pid, file, static_cast<MINIDUMP_TYPE>(type), nullptr, nullptr, nullptr);
    DWORD error = ok ? 0 : GetLastError();
    if (!FlushFileBuffers(file) && ok) { ok = FALSE; error = GetLastError(); }
    CloseHandle(file); CloseHandle(process);
    std::string validationError;
    if (ok && !DumpValidation::Validate(partial, validationError)) {
        ok = FALSE;
        Log(log, "dump validation FAILED: " + validationError);
    }
    if (ok && !MoveFileExW(partial.c_str(), output.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ok = FALSE; error = GetLastError();
    }
    if (!ok) {
        const bool removed = DeleteFileW(partial.c_str()) != FALSE;
        Log(log, "dump FAILED: " + std::to_string(error) + "; incomplete file " + (removed ? "removed" : "could not be removed"));
        return 5;
    }
    Log(log, "dump completed; header, streams and payload bounds validated");
    return 0;
}
// Minimal instruction pointers are still available when minidumps are disabled.
// Suspend only long enough to read a context; resume BEFORE formatting or logging.
static void ThreadPointers(DWORD pid, const fs::path& log) {
    std::ofstream out(log.parent_path() / L"thread_snapshot.txt");
    out << "Version 10 external thread snapshot. Stack values are candidates, NOT an unwound call stack.\n";
    HANDLE modules = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (modules != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W m{}; m.dwSize = sizeof(m);
        if (Module32FirstW(modules, &m)) do {
            out << "MODULE base=0x" << std::hex << reinterpret_cast<std::uintptr_t>(m.modBaseAddr)
                << " size=0x" << m.modBaseSize << std::dec << " " << fs::path(m.szExePath).string() << '\n';
        } while (Module32NextW(modules, &m));
        CloseHandle(modules);
    }
    HANDLE process = OpenProcess(PROCESS_VM_READ, FALSE, pid);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        Log(log, "thread snapshot failed");
        if (process) CloseHandle(process);
        return;
    }
    THREADENTRY32 te{}; te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) do {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
        if (!thread) { out << "THREAD " << te.th32ThreadID << " open failed\n"; continue; }
        CONTEXT context{}; context.ContextFlags = CONTEXT_CONTROL;
        std::uint64_t stack[64]{};
        SIZE_T bytes = 0;
        bool ok = false;
        DWORD resumeError = 0;
        if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
            ok = GetThreadContext(thread, &context) != FALSE;
            if (ok && process) ReadProcessMemory(process, reinterpret_cast<LPCVOID>(context.Rsp), stack, sizeof(stack), &bytes);
            if (ResumeThread(thread) == static_cast<DWORD>(-1)) resumeError = GetLastError();
        }
        CloseHandle(thread);
        // All formatting, allocations and disk writes happen AFTER resuming the target.
        out << "THREAD " << te.th32ThreadID;
        if (ok) {
            out << " RIP=0x" << std::hex << context.Rip << " RSP=0x" << context.Rsp << std::dec << '\n';
            out << "RAW STACK VALUES";
            for (SIZE_T i = 0; i < bytes / sizeof(stack[0]); ++i) out << " 0x" << std::hex << stack[i];
            out << std::dec << '\n';
        } else out << " context unavailable\n";
        if (resumeError) out << "WARNING resume failed: " << resumeError << '\n';
    } while (Thread32Next(snap, &te));
    out.flush();
    CloseHandle(snap);
    if (process) CloseHandle(process);
    Log(log, "external thread/module snapshot saved");
}
static bool Down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }
static void Prune(const fs::path& root, const std::wstring& prefix, DWORD keep) {
    std::error_code ec;
    std::vector<fs::path> paths;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_directory(ec) && it->path().filename().wstring().rfind(prefix, 0) == 0) paths.push_back(it->path());
    std::sort(paths.begin(), paths.end());
    // Folder names begin with a fixed-width wall-clock timestamp.
    while (paths.size() >= std::max<DWORD>(keep, 1)) {
        fs::remove_all(paths.front(), ec); paths.erase(paths.begin());
    }
}
static void PruneMonitorLogs(const fs::path& root) {
    std::error_code ec;
    std::vector<fs::path> logs;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        const auto name = it->path().filename().wstring();
        if (it->is_regular_file(ec) && name.rfind(L"independent_", 0) == 0 && it->path().extension() == L".log")
            logs.push_back(it->path());
    }
    std::sort(logs.begin(), logs.end());
    while (logs.size() >= 20) { fs::remove(logs.front(), ec); logs.erase(logs.begin()); }
}
static std::wstring Stamp() {
    SYSTEMTIME t{}; GetLocalTime(&t);
    wchar_t s[64]{};
    swprintf_s(s, L"%04u%02u%02u_%02u%02u%02u_%03u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    return s;
}
static int Watch(DWORD pid, const wchar_t* mappingName, const fs::path& root) {
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return 3;
    HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, mappingName);
    if (!mapping) { CloseHandle(process); return 6; }
    auto p = static_cast<IndependentShared*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(IndependentShared)));
    if (!p || p->version != 10 || p->pid != pid) {
        if (p) UnmapViewOfFile(p); CloseHandle(mapping); CloseHandle(process); return 7;
    }
    TargetLifetime lifetime(pid, p->targetCreationTime);
    if (!lifetime) { UnmapViewOfFile(p); CloseHandle(mapping); CloseHandle(process); return 11; }
    std::error_code ec;
    fs::create_directories(root, ec);
    PruneMonitorLogs(root);
    const auto log = root / (L"independent_" + Stamp() + L".log");
    Log(log, "version 10 independent watchdog ready; focus never suppresses capture; target pid " + std::to_string(pid));
    wchar_t exe[32768]{}; GetModuleFileNameW(nullptr, exe, 32768);
    HANDLE worker = nullptr;
    bool workerManual = false;
    DWORD autoCount = 0, sequence = 0;
    ULONGLONG lastManual = 0, workerStart = 0, lastRetention = 0;
    bool slowWorkerLogged = false;
    bool incompleteCleaned = false;
    IndependentDecision::Episode episode;
    IndependentDecision::KeyHold key;
    while (WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
        const auto now = GetTickCount64();
        InterlockedExchange64(&p->helperTick, static_cast<LONG64>(now));
        if (worker && WaitForSingleObject(worker, 0) == WAIT_OBJECT_0) {
            DWORD code = 0; GetExitCodeProcess(worker, &code);
            Log(log, "capture worker exited: " + std::to_string(code));
            CloseHandle(worker); worker = nullptr;
            // The worker normally releases its lease on exit. This also handles
            // a worker that failed before opening the mapping or crashed.
            if (ReadFlag(&p->captureOwner) == 2) {
                InterlockedExchange64(&p->captureEndTick, static_cast<LONG64>(now));
                InterlockedCompareExchange(&p->captureOwner, 0, 2);
            }
            if (workerManual) InterlockedIncrement(&p->manualCompleted);
            else InterlockedIncrement(&p->automaticCompleted);
        }
        if (worker && now - workerStart >= 180000 && !slowWorkerLogged) {
            slowWorkerLogged = true;
            Log(log, "capture worker has not finished after 180 seconds; monitor remains responsive, duplicate dump suppressed");
        }
        const bool modifiers = (!(p->modifiers & 1) || Down(VK_SHIFT)) &&
            (!(p->modifiers & 2) || Down(VK_CONTROL)) && (!(p->modifiers & 4) || Down(VK_MENU));
        const bool pressed = p->key && key.Poll(Down(static_cast<int>(p->key)), modifiers, now, p->holdMs);
        const bool manual = pressed && (!lastManual || now - lastManual >= 3000);
        if (manual) lastManual = now;
        const bool loading = ReadFlag(&p->loading) != 0;
        const auto heartbeat = static_cast<ULONGLONG>(ReadTick(loading ? &p->progressTick : &p->frameTick));
        const auto owner = ReadFlag(&p->captureOwner);
        const bool automatic = episode.ObserveState(now, heartbeat, loading ? p->loadMs : p->frameMs,
            worker != nullptr || owner == 1 || owner == 2,
            static_cast<ULONGLONG>(ReadTick(&p->captureEndTick)), loading, p->captureLoading != 0) && autoCount < p->maxCaptures;
        if (manual || automatic) {
            if (manual) Log(log, "manual capture requested (window focus ignored)");
            if (worker) {
                if (manual) Log(log, "manual capture not started: an independent capture is already running");
            } else if (InterlockedCompareExchange(&p->captureOwner, 2, 0) != 0) {
                if (manual) Log(log, "manual request busy: another diagnostic operation owns the capture gate");
            } else {
                // Consume this episode before starting any work. Transient progress
                // during a capture cannot cause an immediate second automatic capture.
                episode.Captured();
                try {
                const std::wstring prefix = manual ? L"independent_manual_" : L"independent_emergency_";
                Prune(root, prefix, manual ? p->keepManual : p->keepEmergency);
                const auto folder = root / (prefix + Stamp() + L"_" + std::to_wstring(++sequence));
                fs::create_directories(folder, ec);
                const auto stage = folder / L"capture.log";
                Log(stage, std::string("capture requested, reason: ") + (manual ? "manual key" : loading ? "loading progress stopped" : "frame heartbeat stopped") +
                    ", target pid " + std::to_string(pid) + ", heartbeat age ms " + (heartbeat ? std::to_string(now - heartbeat) : "unknown"));
                const DWORD level = manual && !p->manualDump ? 0 : p->dumpLevel;
                std::wstring command = L"\"" + std::wstring(exe) + L"\" --capture " + std::to_wstring(pid) + L" \"" + folder.wstring() + L"\" " + std::to_wstring(level) + L" \"" + mappingName + L"\"";
                STARTUPINFOW si{}; si.cb = sizeof(si); PROCESS_INFORMATION pi{};
                if (CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                    CloseHandle(pi.hThread); worker = pi.hProcess; workerManual = manual; workerStart = now; slowWorkerLogged = false;
                    Log(log, "capture worker started: " + folder.string());
                    if (!manual) ++autoCount;
                } else {
                    Log(stage, "capture worker launch FAILED: " + std::to_string(GetLastError()));
                    InterlockedExchange64(&p->captureEndTick, static_cast<LONG64>(now));
                    InterlockedCompareExchange(&p->captureOwner, 0, 2);
                    // One attempt per episode also bounds worker-launch failures.
                    if (!manual) ++autoCount;
                }
                } catch (...) {
                    if (!worker) {
                        InterlockedExchange64(&p->captureEndTick, static_cast<LONG64>(GetTickCount64()));
                        InterlockedCompareExchange(&p->captureOwner, 0, 2);
                    }
                    throw;
                }
            }
        }
        // Scan only outside loading and captures. Serialize against plugin reports/profiling.
        if (!loading && !worker && (!incompleteCleaned || (p->maxStorageMB && (!lastRetention || now - lastRetention >= 60000))) &&
            InterlockedCompareExchange(&p->captureOwner, 3, 0) == 0) {
            lastRetention = now;
            try {
                if (!incompleteCleaned) {
                    const auto removed = StoragePolicy::RemoveIncomplete(root);
                    if (removed) Log(log, "removed interrupted partial dumps: " + std::to_string(removed));
                    incompleteCleaned = true;
                }
                const auto result = StoragePolicy::Enforce(root, static_cast<std::uint64_t>(p->maxStorageMB) * 1024 * 1024, {log});
                if (result.removed || result.errors || result.after > static_cast<std::uint64_t>(p->maxStorageMB) * 1024 * 1024)
                    Log(log, "storage retention: removed " + std::to_string(result.removed) + ", remaining bytes " +
                        std::to_string(result.after) + "; active/recent logs protected; errors " + std::to_string(result.errors));
            } catch (...) { InterlockedCompareExchange(&p->captureOwner, 0, 3); throw; }
            InterlockedCompareExchange(&p->captureOwner, 0, 3);
        }
        Sleep(20);
    }
    Log(log, "target exited; independent monitor stopping");
    if (worker) {
        // The target has exited, so ending our own worker cannot leave a live game thread suspended.
        TerminateProcess(worker, 11);
        CloseHandle(worker);
    }
    UnmapViewOfFile(p); CloseHandle(mapping); CloseHandle(process);
    return 0;
}
// The worker releases the shared lease even if the monitor exits first.
struct WorkerLease {
    HANDLE mapping = nullptr;
    IndependentShared* p = nullptr;
    explicit WorkerLease(const wchar_t* name) {
        mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
        if (mapping) p = static_cast<IndependentShared*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(IndependentShared)));
    }
    ~WorkerLease() {
        if (p) {
            InterlockedExchange64(&p->captureEndTick, static_cast<LONG64>(GetTickCount64()));
            InterlockedCompareExchange(&p->captureOwner, 0, 2);
            UnmapViewOfFile(p);
        }
        if (mapping) CloseHandle(mapping);
    }
};
static int Main(int argc, wchar_t** argv) {
    if (argc == 5 && wcscmp(argv[1], L"--watch") == 0)
        return Watch(static_cast<DWORD>(wcstoul(argv[2], nullptr, 10)), argv[3], fs::path(argv[4]));
    if (argc == 6 && wcscmp(argv[1], L"--capture") == 0) {
        WorkerLease lease(argv[5]);
        if (!lease.p || lease.p->version != 10) return 6;
        DWORD pid = static_cast<DWORD>(wcstoul(argv[2], nullptr, 10));
        int level = _wtoi(argv[4]);
        if (!pid || level < 0 || level > 3) return 2;
        if (lease.p->pid != pid) return 6;
        TargetLifetime lifetime(pid, lease.p->targetCreationTime);
        if (!lifetime) return 11;
        const fs::path folder(argv[3]), log = folder / L"capture.log";
        Log(log, "capture worker started outside Skyrim");
        int result = 0;
        // Small dump FIRST, before any optional larger dump or detailed thread work.
        if (level) result = Dump(pid, folder / L"SkyrimSE.dmp", 1, log);
        if (level > 1 && result == 0) result = Dump(pid, folder / L"SkyrimSE_extended.dmp", level, log);
        if (!level || result != 0) ThreadPointers(pid, log);
        Log(log, "capture worker finished, result " + std::to_string(result));
        return result;
    }
    if (argc != 4) return 2;
    DWORD pid = static_cast<DWORD>(wcstoul(argv[1], nullptr, 10));
    int level = _wtoi(argv[3]);
    if (!pid || level < 1 || level > 3) return 2;
    TargetLifetime lifetime(pid);
    if (!lifetime) return 11;
    const fs::path output(argv[2]);
    return Dump(pid, output, level, fs::path(output.wstring() + L".log"));
}

int wmain(int argc, wchar_t** argv) {
    try { return Main(argc, argv); }
    catch (const std::exception& e) {
        // Leave evidence even if a filesystem conversion or allocation fails.
        try {
            fs::path folder;
            if (argc == 5 && wcscmp(argv[1], L"--watch") == 0) folder = argv[4];
            else if (argc == 6 && wcscmp(argv[1], L"--capture") == 0) folder = argv[3];
            else if (argc == 4) folder = fs::path(argv[2]).parent_path();
            if (!folder.empty()) Log(folder / L"independent_error.log", e.what());
        } catch (...) {}
        return 8;
    }
}
