#pragma once
// Version 14: hook trace, written by the external capture worker (TheWatcherDump.exe) as hook_trace.txt.
//
// Why: on 2026-11-03 the main thread froze at the first instruction of Windows' DXGI Present
// (dxgi.dll+0x19000), and a day earlier the game crashed at the same address on "jmp 0x7FF7ED4D0000".
// Some mod had replaced the start of Present with a jump into memory it allocated, and nothing in
// the old capture said which mod. This trace answers that from outside the game:
//   1. Is the main thread running (spinning) or waiting? Thread CPU time over half a second, plus
//      five instruction-pointer samples.
//   2. Where is it? Module+offset, the exported name when the address is an export, and whether it
//      is the first instruction of a function (from the module's own unwind table on disk).
//   3. Was the code there changed in memory? The bytes are compared with the module file on disk
//      (bytes the loader relocates are skipped, so a normal relocation is not reported as a patch).
//   4. Where does it jump? jmp rel32/rel8, jmp [rip+x], mov rax/r11 + jmp, push+ret stubs are
//      followed hop by hop (up to 8). Memory that belongs to no module is described (who allocated
//      it cannot be read directly), and its pointers into modules are counted: a hook library keeps
//      the address of its detour function there, so the module those pointers lead to is the owner.
//   5. A jump that leads back to an earlier hop is reported as a loop (the thread spins forever);
//      an unreadable target is reported too (that is the crash case).
// Only reads memory. The main thread is suspended for a few microseconds per sample to read its
// registers, exactly like thread_snapshot.txt, and is always resumed before anything is formatted.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <Psapi.h>
#include <TlHelp32.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace HookTrace
{
    namespace fs = std::filesystem;

    struct Module
    {
        std::uintptr_t base = 0;
        std::uintptr_t size = 0;
        std::wstring   path;
    };

    inline std::string Hex(std::uint64_t v)
    {
        char s[32]{};
        std::snprintf(s, sizeof(s), "0x%llX", static_cast<unsigned long long>(v));
        return s;
    }

    inline std::string Utf8(const std::wstring& w)
    {
        if (w.empty()) return {};
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        std::string s(static_cast<std::size_t>(n > 0 ? n : 0), '\0');
        if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
        return s;
    }

    inline std::string FileName(const std::wstring& path)
    {
        const auto slash = path.find_last_of(L"\\/");
        return Utf8(slash == std::wstring::npos ? path : path.substr(slash + 1));
    }

    inline std::string Bytes(const std::uint8_t* b, std::size_t n)
    {
        std::string s;
        char        t[4]{};
        for (std::size_t i = 0; i < n; ++i) {
            std::snprintf(t, sizeof(t), "%02X", b[i]);
            if (i) s += ' ';
            s += t;
        }
        return s;
    }

    inline std::vector<Module> Modules(DWORD pid)
    {
        std::vector<Module> out;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snap == INVALID_HANDLE_VALUE) return out;
        MODULEENTRY32W m{};
        m.dwSize = sizeof(m);
        if (Module32FirstW(snap, &m)) do {
            out.push_back({ reinterpret_cast<std::uintptr_t>(m.modBaseAddr), m.modBaseSize, m.szExePath });
        } while (Module32NextW(snap, &m));
        CloseHandle(snap);
        return out;
    }

    inline const Module* Find(const std::vector<Module>& mods, std::uintptr_t a)
    {
        for (const auto& m : mods) {
            if (a >= m.base && a < m.base + m.size) return &m;
        }
        return nullptr;
    }

    inline std::string Where(const std::vector<Module>& mods, std::uintptr_t a)
    {
        if (const auto* m = Find(mods, a)) return FileName(m->path) + "+" + Hex(a - m->base);
        return Hex(a) + " (no module)";
    }

    inline bool Read(HANDLE process, std::uintptr_t a, void* buf, std::size_t n)
    {
        SIZE_T got = 0;
        return ReadProcessMemory(process, reinterpret_cast<LPCVOID>(a), buf, n, &got) && got == n;
    }

    // The module file on disk, used to tell original code from code changed in memory and to name addresses.
    class DiskImage
    {
    public:
        explicit DiskImage(const std::wstring& path)
        {
            std::ifstream f(fs::path(path), std::ios::binary);
            if (!f) return;
            data_.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
            if (data_.size() < 0x200 || data_[0] != 'M' || data_[1] != 'Z') { data_.clear(); return; }
            const auto lfanew = U32(0x3C);
            if (lfanew + sizeof(IMAGE_NT_HEADERS64) > data_.size() || U32(lfanew) != IMAGE_NT_SIGNATURE) { data_.clear(); return; }
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(data_.data() + lfanew);
            if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) { data_.clear(); return; }
            for (int i = 0; i < 16; ++i) dirs_[i] = nt->OptionalHeader.DataDirectory[i];
            const auto* sec = IMAGE_FIRST_SECTION(nt);
            const auto  count = nt->FileHeader.NumberOfSections;
            if (reinterpret_cast<const std::uint8_t*>(sec + count) > data_.data() + data_.size()) { data_.clear(); return; }
            sections_.assign(sec, sec + count);
            ok_ = true;
            LoadRelocations();
        }

        bool Ok() const { return ok_; }

        // Copies n bytes at an RVA from the file; false when the RVA is not backed by file data.
        bool At(std::uint32_t rva, std::uint8_t* out, std::size_t n) const
        {
            const auto off = Offset(rva);
            if (!off || *off + n > data_.size()) return false;
            std::copy_n(data_.data() + *off, n, out);
            return true;
        }

        bool Relocated(std::uint32_t rva) const
        {
            // a DIR64 relocation covers 8 bytes starting at its RVA
            auto it = relocs_.upper_bound(rva);
            if (it == relocs_.begin()) return false;
            --it;
            return rva - *it < 8;
        }

        std::string ExportAt(std::uint32_t rva) const
        {
            const auto& d = dirs_[IMAGE_DIRECTORY_ENTRY_EXPORT];
            const auto  off = Offset(d.VirtualAddress);
            if (!d.Size || !off || *off + sizeof(IMAGE_EXPORT_DIRECTORY) > data_.size()) return {};
            const auto* ed = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(data_.data() + *off);
            const auto  funcs = Offset(ed->AddressOfFunctions), names = Offset(ed->AddressOfNames), ords = Offset(ed->AddressOfNameOrdinals);
            if (!funcs || !names || !ords) return {};
            for (DWORD i = 0; i < ed->NumberOfNames; ++i) {
                if (*names + i * 4 + 4 > data_.size() || *ords + i * 2 + 2 > data_.size()) break;
                const auto ord = U16(*ords + i * 2);
                if (*funcs + ord * 4u + 4 > data_.size() || U32(*funcs + ord * 4u) != rva) continue;
                const auto nameOff = Offset(U32(*names + i * 4));
                if (!nameOff) continue;
                std::string s;
                for (std::size_t k = *nameOff; k < data_.size() && data_[k] && s.size() < 128; ++k) s += static_cast<char>(data_[k]);
                return s;
            }
            return {};
        }

        // Start RVA of the function containing rva, from the exception directory (0 if unknown)
        std::uint32_t FunctionStart(std::uint32_t rva) const { return FunctionRange(rva).first; }

        // End RVA (exclusive) of the function containing rva (0 if unknown)
        std::uint32_t FunctionEnd(std::uint32_t rva) const { return FunctionRange(rva).second; }

        std::pair<std::uint32_t, std::uint32_t> FunctionRange(std::uint32_t rva) const
        {
            const auto& d = dirs_[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
            const auto  off = Offset(d.VirtualAddress);
            if (!d.Size || !off) return { 0, 0 };
            const std::size_t n = d.Size / sizeof(RUNTIME_FUNCTION);
            std::size_t       lo = 0, hi = n;
            while (lo < hi) {
                const std::size_t mid = (lo + hi) / 2;
                const std::size_t at = *off + mid * sizeof(RUNTIME_FUNCTION);
                if (at + sizeof(RUNTIME_FUNCTION) > data_.size()) return { 0, 0 };
                const auto begin = U32(at), end = U32(at + 4);
                if (rva < begin) hi = mid;
                else if (rva >= end) lo = mid + 1;
                else return { begin, end };
            }
            return { 0, 0 };
        }

    private:
        std::uint32_t U32(std::size_t o) const { std::uint32_t v = 0; if (o + 4 <= data_.size()) std::memcpy(&v, data_.data() + o, 4); return v; }
        std::uint16_t U16(std::size_t o) const { std::uint16_t v = 0; if (o + 2 <= data_.size()) std::memcpy(&v, data_.data() + o, 2); return v; }

        std::optional<std::size_t> Offset(std::uint32_t rva) const
        {
            for (const auto& s : sections_) {
                const auto size = (std::max)(s.Misc.VirtualSize, s.SizeOfRawData);
                if (rva >= s.VirtualAddress && rva < s.VirtualAddress + size) {
                    const std::uint32_t delta = rva - s.VirtualAddress;
                    if (delta >= s.SizeOfRawData) return std::nullopt;  // zero-filled in memory, not in the file
                    return static_cast<std::size_t>(s.PointerToRawData) + delta;
                }
            }
            return std::nullopt;
        }

        void LoadRelocations()
        {
            const auto& d = dirs_[IMAGE_DIRECTORY_ENTRY_BASERELOC];
            const auto  off = Offset(d.VirtualAddress);
            if (!d.Size || !off) return;
            std::size_t       p = *off;
            const std::size_t end = (std::min)(data_.size(), *off + d.Size);
            while (p + 8 <= end) {
                const auto page = U32(p), blockSize = U32(p + 4);
                if (blockSize < 8 || p + blockSize > end) break;
                for (std::size_t e = p + 8; e + 2 <= p + blockSize; e += 2) {
                    const auto entry = U16(e);
                    if ((entry >> 12) == IMAGE_REL_BASED_DIR64) relocs_.insert(page + (entry & 0xFFF));
                }
                p += blockSize;
            }
        }

        std::vector<std::uint8_t>         data_;
        std::vector<IMAGE_SECTION_HEADER> sections_;
        IMAGE_DATA_DIRECTORY              dirs_[16]{};
        std::set<std::uint32_t>           relocs_;
        bool                              ok_ = false;
    };

    struct Jump
    {
        bool           found = false;
        std::uintptr_t target = 0;
        std::string    kind;
        bool           unreadablePointer = false;
    };

    // Recognizes the jump stubs hook libraries write (MinHook, Detours, PolyHook, hand-written patches)
    inline Jump DecodeJump(HANDLE process, std::uintptr_t a, const std::uint8_t* b)
    {
        Jump j;
        auto i32 = [&](int o) { std::int32_t v; std::memcpy(&v, b + o, 4); return v; };
        auto u64 = [&](int o) { std::uint64_t v; std::memcpy(&v, b + o, 8); return v; };
        auto u32 = [&](int o) { std::uint32_t v; std::memcpy(&v, b + o, 4); return v; };
        if (b[0] == 0xE9) {
            j = { true, a + 5 + static_cast<std::intptr_t>(i32(1)), "jmp rel32" };
        } else if (b[0] == 0xEB) {
            j = { true, a + 2 + static_cast<std::int8_t>(b[1]), "jmp rel8" };
        } else if (b[0] == 0xFF && b[1] == 0x25) {
            const std::uintptr_t slot = a + 6 + static_cast<std::intptr_t>(i32(2));
            std::uint64_t        t = 0;
            if (Read(process, slot, &t, 8)) j = { true, static_cast<std::uintptr_t>(t), "jmp [" + Hex(slot) + "]" };
            else j = { true, 0, "jmp [" + Hex(slot) + "] - pointer UNREADABLE", true };
        } else if (b[0] == 0x48 && b[1] == 0xB8 && b[10] == 0xFF && b[11] == 0xE0) {
            j = { true, static_cast<std::uintptr_t>(u64(2)), "mov rax, imm64; jmp rax" };
        } else if (b[0] == 0x48 && b[1] == 0xB8 && b[10] == 0x50 && b[11] == 0xC3) {
            j = { true, static_cast<std::uintptr_t>(u64(2)), "mov rax, imm64; push rax; ret" };
        } else if (b[0] == 0x49 && b[1] == 0xBB && b[10] == 0x41 && b[11] == 0xFF && b[12] == 0xE3) {
            j = { true, static_cast<std::uintptr_t>(u64(2)), "mov r11, imm64; jmp r11" };
        } else if (b[0] == 0x49 && b[1] == 0xBA && b[10] == 0x41 && b[11] == 0xFF && b[12] == 0xE2) {
            j = { true, static_cast<std::uintptr_t>(u64(2)), "mov r10, imm64; jmp r10" };
        } else if (b[0] == 0x68 && b[5] == 0xC7 && b[6] == 0x44 && b[7] == 0x24 && b[8] == 0x04 && b[13] == 0xC3) {
            j = { true, static_cast<std::uintptr_t>(u32(1)) | (static_cast<std::uintptr_t>(u32(9)) << 32), "push imm32; mov [rsp+4], imm32; ret" };
        }
        return j;
    }

    inline std::string RegionText(HANDLE process, std::uintptr_t a, MEMORY_BASIC_INFORMATION& mbi, bool& ok)
    {
        ok = VirtualQueryEx(process, reinterpret_cast<LPCVOID>(a), &mbi, sizeof(mbi)) == sizeof(mbi);
        if (!ok) return "VirtualQueryEx failed (" + std::to_string(GetLastError()) + ")";
        std::string type = mbi.Type == MEM_IMAGE ? "image (a loaded file)" : mbi.Type == MEM_MAPPED ? "mapped file/section" :
                           mbi.Type == MEM_PRIVATE ? "private (allocated at run time, e.g. VirtualAlloc)" : "unknown";
        std::string state = mbi.State == MEM_COMMIT ? "committed" : mbi.State == MEM_RESERVE ? "reserved only" : "FREE (not allocated)";
        std::string prot;
        switch (mbi.Protect & 0xFF) {
        case PAGE_EXECUTE: prot = "execute"; break;
        case PAGE_EXECUTE_READ: prot = "execute+read"; break;
        case PAGE_EXECUTE_READWRITE: prot = "execute+read+WRITE"; break;
        case PAGE_EXECUTE_WRITECOPY: prot = "execute+read+writecopy"; break;
        case PAGE_READONLY: prot = "read only (NOT executable)"; break;
        case PAGE_READWRITE: prot = "read+write (NOT executable)"; break;
        case PAGE_NOACCESS: prot = "NO ACCESS"; break;
        default: prot = Hex(mbi.Protect); break;
        }
        std::string s = "memory: " + type + ", " + state + ", " + prot + ", allocation base " + Hex(reinterpret_cast<std::uintptr_t>(mbi.AllocationBase)) +
                        ", region " + Hex(reinterpret_cast<std::uintptr_t>(mbi.BaseAddress)) + " size " + Hex(mbi.RegionSize);
        if (mbi.Type == MEM_IMAGE || mbi.Type == MEM_MAPPED) {
            wchar_t name[1024]{};
            if (GetMappedFileNameW(process, reinterpret_cast<LPVOID>(a), name, 1024)) s += ", file " + Utf8(name);
        }
        return s;
    }

    // Pointers inside a block of run-time memory that lead into modules, counted per module.
    // A hook library's relay/trampoline page holds the address of the hook function (in the owner
    // DLL) and the address to continue at in the original function.
    inline void PointerOwners(HANDLE process, const std::vector<Module>& mods, std::uintptr_t allocBase, std::ofstream& out)
    {
        std::map<std::string, std::pair<int, std::vector<std::string>>> hits;
        std::uintptr_t                                                  a = allocBase;
        std::size_t                                                     scanned = 0;
        while (scanned < 0x100000) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQueryEx(process, reinterpret_cast<LPCVOID>(a), &mbi, sizeof(mbi)) != sizeof(mbi)) break;
            if (reinterpret_cast<std::uintptr_t>(mbi.AllocationBase) != allocBase) break;
            const auto regionEnd = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
            const bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
            if (readable) {
                const std::size_t        n = (std::min<std::size_t>)(mbi.RegionSize, 0x100000 - scanned);
                std::vector<std::uint8_t> buf(n);
                SIZE_T                    got = 0;
                ReadProcessMemory(process, mbi.BaseAddress, buf.data(), n, &got);
                for (std::size_t o = 0; o + 8 <= got; o += 8) {
                    std::uint64_t v;
                    std::memcpy(&v, buf.data() + o, 8);
                    if (const auto* m = Find(mods, static_cast<std::uintptr_t>(v))) {
                        auto& h = hits[FileName(m->path)];
                        ++h.first;
                        if (h.second.size() < 4) h.second.push_back(FileName(m->path) + "+" + Hex(v - m->base) + " (stored at " + Hex(reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + o) + ")");
                    }
                }
                scanned += n;
            }
            a = regionEnd;
        }
        out << "    pointers in this allocation that lead into modules (the hook owner's code is usually among them):\n";
        if (hits.empty()) out << "      none found\n";
        std::vector<std::pair<std::string, std::pair<int, std::vector<std::string>>>> sorted(hits.begin(), hits.end());
        std::sort(sorted.begin(), sorted.end(), [](const auto& x, const auto& y) { return x.second.first > y.second.first; });
        for (std::size_t i = 0; i < sorted.size() && i < 8; ++i) {
            out << "      " << sorted[i].first << ": " << sorted[i].second.first << " pointer(s), e.g.";
            for (const auto& e : sorted[i].second.second) out << "\n        " << e;
            out << '\n';
        }
    }

    inline void Write(DWORD pid, DWORD mainTid, const fs::path& file, const fs::path& log, void (*logFn)(const fs::path&, const std::string&))
    {
        if (!mainTid) { logFn(log, "hook trace SKIPPED: main thread id not known yet"); return; }
        HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, mainTid);
        if (!process || !thread) {
            logFn(log, "hook trace FAILED: could not open the game process/main thread (" + std::to_string(GetLastError()) + ")");
            if (process) CloseHandle(process);
            if (thread) CloseHandle(thread);
            return;
        }
        auto cpu = [&]() -> std::uint64_t {
            FILETIME c{}, e{}, k{}, u{};
            if (!GetThreadTimes(thread, &c, &e, &k, &u)) return 0;
            return ((static_cast<std::uint64_t>(k.dwHighDateTime) << 32) | k.dwLowDateTime) +
                   ((static_cast<std::uint64_t>(u.dwHighDateTime) << 32) | u.dwLowDateTime);
        };
        // ---- sampling: suspend only to read registers; resume before anything else ----
        std::uintptr_t rips[5]{};
        bool           got[5]{};
        DWORD          resumeError = 0;
        const auto     cpu0 = cpu();
        const auto     t0 = GetTickCount64();
        for (int i = 0; i < 5; ++i) {
            if (i) Sleep(100);
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_CONTROL;
            if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
                got[i] = GetThreadContext(thread, &ctx) != FALSE;
                if (ResumeThread(thread) == static_cast<DWORD>(-1)) resumeError = GetLastError();
                if (got[i]) rips[i] = static_cast<std::uintptr_t>(ctx.Rip);
            }
        }
        const auto cpu1 = cpu();
        const auto wallMs = GetTickCount64() - t0;
        CloseHandle(thread);

        const auto     mods = Modules(pid);
        std::ofstream  out(file);
        out << "Version 14 hook trace of the main thread (thread " << mainTid << ").\n"
            << "Reads memory only. Follows the code the main thread is in, compares it with the module file on disk and\n"
            << "follows any jump a mod wrote there, to name who changed that code. Evidence, not proof of blame.\n\n";
        if (resumeError) out << "WARNING: resuming the main thread after a sample failed (" << resumeError << ")\n";

        const double cpuMs = static_cast<double>(cpu1 - cpu0) / 10000.0;
        out << "MAIN THREAD ACTIVITY over " << wallMs << " ms: CPU time used " << static_cast<long long>(cpuMs) << " ms -> ";
        if (cpuMs > 0.6 * static_cast<double>(wallMs)) out << "BUSY: the thread is running non-stop (a loop that never ends, or very long work)\n";
        else if (cpuMs < 0.05 * static_cast<double>(wallMs)) out << "NOT RUNNING: the thread is waiting (for a lock, the GPU/driver, another thread or an event)\n";
        else out << "PARTLY RUNNING: the thread runs and waits in turns\n";
        out << "instruction pointer samples (100 ms apart):";
        bool same = true;
        for (int i = 0; i < 5; ++i) {
            out << "\n  " << (got[i] ? Where(mods, rips[i]) : std::string("context unavailable"));
            if (i && rips[i] != rips[0]) same = false;
        }
        out << "\n" << (same ? "all samples at the same address\n\n" : "samples differ\n\n");

        if (!got[0]) {
            out << "No context for the main thread; nothing to trace.\n";
            CloseHandle(process);
            logFn(log, "hook trace saved (no main thread context)");
            return;
        }

        std::map<std::wstring, std::unique_ptr<DiskImage>> disk;
        auto image = [&](const Module& m) -> const DiskImage* {
            auto& d = disk[m.path];
            if (!d) d = std::make_unique<DiskImage>(m.path);
            return d->Ok() ? d.get() : nullptr;
        };

        out << "CODE PATH FROM THE MAIN THREAD'S INSTRUCTION POINTER\n";
        std::vector<std::uintptr_t> seen;
        std::set<std::uintptr_t>    scannedAllocs;
        std::uintptr_t              a = rips[0];
        bool                        anyPatch = false;
        for (int hop = 0; hop < 8; ++hop) {
            out << "\nHOP " << hop << ": " << Hex(a) << "  " << Where(mods, a) << '\n';
            MEMORY_BASIC_INFORMATION mbi{};
            bool                     qok = false;
            out << "    " << RegionText(process, a, mbi, qok) << '\n';
            std::uint8_t b[32]{};
            if (!Read(process, a, b, sizeof(b))) {
                out << "    code bytes UNREADABLE: jumping here makes the game crash (access violation)\n";
                break;
            }
            out << "    bytes in memory: " << Bytes(b, 16) << '\n';
            const Module* m = Find(mods, a);
            if (m) {
                const auto rva = static_cast<std::uint32_t>(a - m->base);
                if (const auto* d = image(*m)) {
                    const auto name = d->ExportAt(rva);
                    const auto start = d->FunctionStart(rva);
                    if (!name.empty()) out << "    exported function: " << name << '\n';
                    if (start) out << "    function containing it starts at " << FileName(m->path) << "+" << Hex(start)
                                   << (start == rva ? "  (this IS the first instruction of the function: where hooks are placed)" : "") << '\n';
                    // (14 fix) compare only bytes of the function this address is in: a hook on the NEXT
                    // function in memory is not a patch on the thread's path
                    const auto     fend = d->FunctionEnd(rva);
                    const std::uint32_t span = (fend > rva) ? (std::min<std::uint32_t>)(16, fend - rva) : 16;
                    std::uint8_t orig[16]{};
                    if (d->At(rva, orig, span)) {
                        int diff = 0, masked = 0;
                        for (std::uint32_t i = 0; i < span; ++i) {
                            if (d->Relocated(rva + i)) { ++masked; continue; }
                            if (orig[i] != b[i]) ++diff;
                        }
                        out << "    bytes on disk:   " << Bytes(orig, span) << (span < 16 ? "  (function ends here; following bytes belong to another function)" : "") << '\n';
                        if (diff) {
                            anyPatch = true;
                            out << "    PATCHED IN MEMORY: " << diff << " of " << (static_cast<int>(span) - masked) << " compared bytes differ from " << FileName(m->path)
                                << " on disk - something rewrote this code at run time (a hook)\n";
                        } else {
                            out << "    matches the file on disk (not patched)" << (masked ? " - relocated bytes skipped" : "") << '\n';
                        }
                    } else {
                        out << "    no file bytes at this address (data or zero-filled section)\n";
                    }
                } else {
                    out << "    module file could not be read from disk: " << Utf8(m->path) << '\n';
                }
            } else if (qok && mbi.Type == MEM_PRIVATE) {
                const auto ab = reinterpret_cast<std::uintptr_t>(mbi.AllocationBase);
                out << "    This is run-time memory, not part of any DLL: typical of a hook library's jump/trampoline page.\n";
                if (scannedAllocs.insert(ab).second) PointerOwners(process, mods, ab, out);
            }

            const auto j = DecodeJump(process, a, b);
            if (!j.found) {
                out << "    no jump here: the code runs on from this point (end of the path)\n";
                break;
            }
            out << "    " << j.kind << " -> " << (j.unreadablePointer ? std::string("?") : Hex(j.target) + "  " + Where(mods, j.target)) << '\n';
            if (j.unreadablePointer) {
                out << "    the jump goes through a pointer that cannot be read: executing it crashes\n";
                break;
            }
            seen.push_back(a);
            if (const auto it = std::find(seen.begin(), seen.end(), j.target); it != seen.end()) {
                out << "    LOOP: this jumps back to hop " << (it - seen.begin())
                    << ". The thread can never leave this chain - an endless loop between hooks (for example a hook\n"
                    << "    whose 'call the original' pointer leads back to the hooked entry, after hooks were added or removed out of order).\n";
                break;
            }
            a = j.target;
        }
        out << "\nSUMMARY: " << (anyPatch ? "code on the main thread's path was changed in memory; the hop after the patched bytes leads to the code that changed it."
                                          : "no patched code found on the main thread's path.")
            << '\n';
        out.flush();
        CloseHandle(process);
        logFn(log, std::string("hook trace saved") + (anyPatch ? " (patched code found on the main thread's path)" : ""));
    }
}
