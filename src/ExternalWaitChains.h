#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <wct.h>
#include <TlHelp32.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// Called ONLY by the disposable --waitchains process. It never calls
// SuspendThread, GetThreadContext, MiniDumpWriteDump or a Skyrim API.
namespace ExternalWaitChains {
inline const char* Type(WCT_OBJECT_TYPE type) {
    switch (type) {
    case WctCriticalSectionType: return "critical section";
    case WctSendMessageType: return "SendMessage";
    case WctMutexType: return "mutex";
    case WctAlpcType: return "ALPC";
    case WctComType: return "COM";
    case WctThreadWaitType: return "thread wait";
    case WctProcessWaitType: return "process wait";
    case WctThreadType: return "thread";
    case WctComActivationType: return "COM activation";
    case WctSocketIoType: return "socket I/O";
    case WctSmbIoType: return "SMB I/O";
    default: return "other/unknown object";
    }
}
inline const char* Status(WCT_OBJECT_STATUS status) {
    switch (status) {
    case WctStatusNoAccess: return "no access";
    case WctStatusRunning: return "running";
    case WctStatusBlocked: return "blocked";
    case WctStatusPidOnly: return "pid only";
    case WctStatusPidOnlyRpcss: return "pid only (rpcss)";
    case WctStatusOwned: return "owned";
    case WctStatusNotOwned: return "not owned";
    case WctStatusAbandoned: return "abandoned";
    case WctStatusUnknown: return "unknown";
    case WctStatusError: return "error";
    default: return "unknown status";
    }
}
inline int Collect(DWORD pid, DWORD mainTid, const std::filesystem::path& path) {
    std::ofstream out(path);
    if (!out) return 4;
    out << "External Windows wait-chain evidence. MAIN thread ID: " << mainTid << '\n'
        << "Point-in-time observations, not proof of which mod caused a freeze.\n"
        << "Unsupported waits include events, semaphores, SRW locks and spin locks.\n"
        << "COM-specific WCT callbacks are not registered: COM waits may appear, but\n"
        << "their server-side owners are not resolved.\n"
        << "Access errors, truncated chains or timeout mean incomplete evidence.\n"
        << "No reported cycle does not rule out a deadlock.\n";
    out.flush();
    HWCT session = OpenThreadWaitChainSession(0, nullptr);
    if (!session) {
        out << "FAILED opening WCT session: " << GetLastError() << '\n';
        return 5;
    }
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        out << "FAILED enumerating threads: " << GetLastError() << '\n';
        CloseThreadWaitChainSession(session);
        return 5;
    }
    std::vector<DWORD> tids;
    THREADENTRY32 entry{}; entry.dwSize = sizeof(entry);
    if (Thread32First(snapshot, &entry)) {
        do { if (entry.th32OwnerProcessID == pid) tids.push_back(entry.th32ThreadID); }
        while (Thread32Next(snapshot, &entry));
        if (GetLastError() != ERROR_NO_MORE_FILES)
            out << "WARNING thread enumeration incomplete: " << GetLastError() << '\n';
    } else out << "WARNING thread enumeration failed: " << GetLastError() << '\n';
    CloseHandle(snapshot);
    // Query MAIN first so its evidence survives a later timeout.
    auto mainIt = std::find(tids.begin(), tids.end(), mainTid);
    if (mainIt != tids.end()) std::iter_swap(tids.begin(), mainIt);
    else out << "WARNING MAIN thread not present in snapshot\n";
    unsigned queried = 0, failed = 0, cycles = 0;
    for (DWORD tid : tids) {
        out << "QUERY thread " << tid << (tid == mainTid ? " (MAIN)" : "") << '\n';
        out.flush();
        HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
        if (!thread || GetProcessIdOfThread(thread) != pid) {
            out << "UNAVAILABLE thread exited, inaccessible or no longer belongs to target\n";
            if (thread) CloseHandle(thread);
            ++failed; out.flush(); continue;
        }
        WAITCHAIN_NODE_INFO nodes[WCT_MAX_NODE_COUNT]{};
        DWORD count = WCT_MAX_NODE_COUNT;
        BOOL cycle = FALSE;
        BOOL ok = GetThreadWaitChain(session, 0, WCT_OUT_OF_PROC_FLAG | WCT_OUT_OF_PROC_CS_FLAG,
            tid, &count, nodes, &cycle);
        DWORD error = ok ? ERROR_SUCCESS : GetLastError();
        CloseHandle(thread);
        bool truncated = error == ERROR_MORE_DATA || error == ERROR_TOO_MANY_THREADS;
        if (!ok && !truncated) {
            out << "UNAVAILABLE WCT error " << error << '\n';
            ++failed; out.flush(); continue;
        }
        ++queried;
        if (truncated) { ++failed; out << "PARTIAL chain, WCT error " << error << '\n'; }
        if (cycle) { ++cycles; out << "CYCLE REPORTED BY WINDOWS\n"; }
        for (DWORD i = 0; i < std::min<DWORD>(count, WCT_MAX_NODE_COUNT); ++i) {
            const auto& node = nodes[i];
            out << "  node " << i << ": " << Type(node.ObjectType) << " [" << Status(node.ObjectStatus) << "]";
            if (node.ObjectType == WctThreadType) {
                out << " pid=" << node.ThreadObject.ProcessId << " tid=" << node.ThreadObject.ThreadId;
                if (node.ThreadObject.ProcessId == pid && node.ThreadObject.ThreadId == mainTid) out << " (MAIN)";
                out << " wait=" << node.ThreadObject.WaitTime;
            } else {
                out << " name=";
                for (unsigned j = 0; j < WCT_OBJNAME_LENGTH && node.LockObject.ObjectName[j]; ++j) {
                    wchar_t ch = node.LockObject.ObjectName[j];
                    out << (ch >= 32 && ch < 127 ? static_cast<char>(ch) : '?');
                }
            }
            out << '\n';
        }
        out.flush();
    }
    CloseThreadWaitChainSession(session);
    out << "FINISHED: queried=" << queried << " unavailable/partial=" << failed << " reported_cycles=" << cycles << '\n';
    out.flush();
    return out ? 0 : 4;
}
}
