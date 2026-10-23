// Windows integration test, not a portable check.
// cl /std:c++20 /EHsc tests\external_waitchains_test.cpp /link advapi32.lib
#include "../src/ExternalWaitChains.h"
#include <cassert>
#include <iostream>
#include <iterator>

struct Fixture { HANDLE mutex; HANDLE ready; };
static DWORD WINAPI Waiter(void* arg) {
    auto& f = *static_cast<Fixture*>(arg);
    SetEvent(f.ready);
    DWORD result = WaitForSingleObject(f.mutex, 10000);
    if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) ReleaseMutex(f.mutex);
    return result == WAIT_OBJECT_0 ? 0 : 1;
}
int main() {
    Fixture fixture{CreateMutexW(nullptr, TRUE, nullptr), CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    assert(fixture.mutex && fixture.ready);
    DWORD tid = 0;
    HANDLE thread = CreateThread(nullptr, 0, Waiter, &fixture, 0, &tid);
    assert(thread);
    assert(WaitForSingleObject(fixture.ready, 5000) == WAIT_OBJECT_0);
    // Give the fixture time to enter its kernel mutex wait after signaling ready.
    Sleep(100);
    auto path = std::filesystem::temp_directory_path() /
        ("watcher_wct_test_" + std::to_string(GetCurrentProcessId()) + ".txt");
    int result = ExternalWaitChains::Collect(GetCurrentProcessId(), tid, path);
    ReleaseMutex(fixture.mutex);
    DWORD finished = WaitForSingleObject(thread, 5000);
    CloseHandle(thread); CloseHandle(fixture.ready); CloseHandle(fixture.mutex);
    std::ifstream input(path);
    std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    input.close();
    std::error_code ec; std::filesystem::remove(path, ec);
    assert(result == 0 && finished == WAIT_OBJECT_0);
    assert(text.find("FINISHED:") != std::string::npos);
    assert(text.find("mutex [") != std::string::npos);
    assert(text.find("CYCLE REPORTED BY WINDOWS") == std::string::npos);
    std::cout << "Windows wait-chain mutex integration test passed\n";
}
