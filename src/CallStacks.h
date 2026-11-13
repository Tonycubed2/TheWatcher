#pragma once
// Version 15: real call stacks, written by the external capture worker (TheWatcherDump.exe) as call_stacks.txt.
//
// thread_snapshot.txt only lists raw values from the top of each stack ("candidates"): stale return
// addresses and data mixed in, and the deeper callers missing. This file walks each stack the way a
// debugger does: every x64 module carries unwind tables (.pdata + UNWIND_INFO) that say exactly how
// each function moved the stack pointer and which registers it saved. Applying them frame by frame
// gives the true chain of callers. The tables are read from the running game's memory, so this also
// works for SkyrimSE.exe even though its code is encrypted on disk.
//
// SkyrimSE.exe frames are labeled with Address Library IDs (versionlib-*.bin / version-*.bin, formats
// 1, 2 and 5), so they match CrashLogger and CommonLib names.
//
// Each thread is suspended only long enough to copy its registers and stack (one ReadProcessMemory),
// then resumed; the walking and formatting happen afterwards on the copy.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <TlHelp32.h>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <set>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace CallStacks
{
    namespace fs = std::filesystem;

    struct Module
    {
        std::uintptr_t base = 0;
        std::uintptr_t size = 0;
        std::wstring   path;
        std::string    name;
    };

    inline std::string Hex(std::uint64_t v)
    {
        char s[32]{};
        std::snprintf(s, sizeof(s), "0x%llX", static_cast<unsigned long long>(v));
        return s;
    }

    inline std::string Narrow(const std::wstring& w)
    {
        if (w.empty()) return {};
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        std::string s(static_cast<std::size_t>(n > 0 ? n : 0), '\0');
        if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
        return s;
    }

    // ---------------------------------------------------------------- Address Library (formats 1, 2, 5)
    class AddressLibrary
    {
    public:
        bool Load(const fs::path& path)
        {
            std::ifstream f(path, std::ios::binary);
            if (!f) return false;
            std::int32_t format = 0;
            Read(f, format);
            std::vector<std::pair<std::uint64_t, std::uint64_t>> e;  // offset, id
            if (format == 5) {
                std::uint32_t ver[4]{};
                for (auto& v : ver) Read(f, v);
                char name[64]{};
                f.read(name, sizeof(name));
                std::int32_t ptr = 0, fmt = 0, count = 0;
                Read(f, ptr); Read(f, fmt); Read(f, count);
                if (!f || count <= 0 || count > 0x4000000) return false;
                std::vector<std::uint32_t> offs(static_cast<std::size_t>(count));
                f.read(reinterpret_cast<char*>(offs.data()), static_cast<std::streamsize>(offs.size() * 4));
                if (!f) return false;
                for (std::size_t id = 0; id < offs.size(); ++id)
                    if (offs[id]) e.emplace_back(offs[id], id);
            } else if (format == 1 || format == 2) {
                std::int32_t ver[4]{};
                for (auto& v : ver) Read(f, v);
                std::int32_t nameLen = 0;
                Read(f, nameLen);
                if (nameLen < 0 || nameLen > 0x10000) return false;
                f.seekg(nameLen, std::ios::cur);
                std::int32_t ptr = 0, count = 0;
                Read(f, ptr); Read(f, count);
                if (!f || ptr <= 0 || count <= 0) return false;
                std::uint64_t pid = 0, poff = 0;
                e.reserve(static_cast<std::size_t>(count));
                for (std::int32_t i = 0; i < count; ++i) {
                    std::uint8_t t = 0;
                    Read(f, t);
                    const auto lo = t & 0xF, hi = t >> 4;
                    std::uint64_t id = 0;
                    switch (lo) {
                    case 0: id = U64(f); break;
                    case 1: id = pid + 1; break;
                    case 2: id = pid + U8(f); break;
                    case 3: id = pid - U8(f); break;
                    case 4: id = pid + U16(f); break;
                    case 5: id = pid - U16(f); break;
                    case 6: id = U16(f); break;
                    case 7: id = U32(f); break;
                    default: return false;
                    }
                    const std::uint64_t b = (hi & 8) ? poff / static_cast<std::uint64_t>(ptr) : poff;
                    std::uint64_t o = 0;
                    switch (hi & 7) {
                    case 0: o = U64(f); break;
                    case 1: o = b + 1; break;
                    case 2: o = b + U8(f); break;
                    case 3: o = b - U8(f); break;
                    case 4: o = b + U16(f); break;
                    case 5: o = b - U16(f); break;
                    case 6: o = U16(f); break;
                    case 7: o = U32(f); break;
                    }
                    if (hi & 8) o *= static_cast<std::uint64_t>(ptr);
                    if (!f) return false;
                    e.emplace_back(o, id);
                    pid = id;
                    poff = o;
                }
            } else {
                return false;
            }
            std::sort(e.begin(), e.end());
            entries_ = std::move(e);
            return !entries_.empty();
        }

        // (16) offset of an ID from SkyrimSE.exe's base, 0 if the ID is not in the file
        std::uint64_t OffsetOf(std::uint64_t id) const
        {
            for (const auto& e : entries_)
                if (e.second == id) return e.first;
            return 0;
        }

        bool Loaded() const { return !entries_.empty(); }

        // " = ID 13299+0x4E" for an offset from SkyrimSE.exe's base
        std::string Label(std::uint64_t offset) const
        {
            if (entries_.empty()) return {};
            auto it = std::upper_bound(entries_.begin(), entries_.end(), std::make_pair(offset, ~0ull));
            if (it == entries_.begin()) return {};
            --it;
            return " = ID " + std::to_string(it->second) + "+" + Hex(offset - it->first);
        }

    private:
        template <class T>
        static void Read(std::ifstream& f, T& v) { f.read(reinterpret_cast<char*>(&v), sizeof(T)); }
        static std::uint64_t U8(std::ifstream& f) { std::uint8_t v = 0; Read(f, v); return v; }
        static std::uint64_t U16(std::ifstream& f) { std::uint16_t v = 0; Read(f, v); return v; }
        static std::uint64_t U32(std::ifstream& f) { std::uint32_t v = 0; Read(f, v); return v; }
        static std::uint64_t U64(std::ifstream& f) { std::uint64_t v = 0; Read(f, v); return v; }

        std::vector<std::pair<std::uint64_t, std::uint64_t>> entries_;
    };

    // "1-6-1170-0" from the game executable's version resource
    inline std::wstring GameVersion(const std::wstring& exe)
    {
        DWORD      handle = 0;
        const auto size = GetFileVersionInfoSizeW(exe.c_str(), &handle);
        if (!size) return {};
        std::vector<std::uint8_t> buf(size);
        if (!GetFileVersionInfoW(exe.c_str(), 0, size, buf.data())) return {};
        VS_FIXEDFILEINFO* info = nullptr;
        UINT              len = 0;
        if (!VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void**>(&info), &len) || !info) return {};
        return std::to_wstring(HIWORD(info->dwFileVersionMS)) + L"-" + std::to_wstring(LOWORD(info->dwFileVersionMS)) + L"-" +
               std::to_wstring(HIWORD(info->dwFileVersionLS)) + L"-" + std::to_wstring(LOWORD(info->dwFileVersionLS));
    }

    // ---------------------------------------------------------------- remote module unwind tables
    class Target
    {
    public:
        Target(HANDLE process, DWORD pid) : process_(process)
        {
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
            if (snap != INVALID_HANDLE_VALUE) {
                MODULEENTRY32W m{};
                m.dwSize = sizeof(m);
                if (Module32FirstW(snap, &m)) do {
                    Module mod{ reinterpret_cast<std::uintptr_t>(m.modBaseAddr), m.modBaseSize, m.szExePath, Narrow(m.szModule) };
                    mods_.push_back(std::move(mod));
                } while (Module32NextW(snap, &m));
                CloseHandle(snap);
            }
        }

        const Module* Find(std::uintptr_t a) const
        {
            for (const auto& m : mods_)
                if (a >= m.base && a < m.base + m.size) return &m;
            return nullptr;
        }

        const std::vector<Module>& Modules() const { return mods_; }

        bool Read(std::uintptr_t a, void* out, std::size_t n) const
        {
            SIZE_T got = 0;
            return ReadProcessMemory(process_, reinterpret_cast<LPCVOID>(a), out, n, &got) && got == n;
        }

        // The RUNTIME_FUNCTION covering rva in module m (copied), or false (leaf function / no table)
        bool Lookup(const Module& m, std::uint32_t rva, RUNTIME_FUNCTION& out)
        {
            auto& table = Table(m);
            std::size_t lo = 0, hi = table.size();
            while (lo < hi) {
                const std::size_t mid = (lo + hi) / 2;
                if (rva < table[mid].BeginAddress) hi = mid;
                else if (rva >= table[mid].EndAddress) lo = mid + 1;
                else { out = table[mid]; return true; }
            }
            return false;
        }

    private:
        const std::vector<RUNTIME_FUNCTION>& Table(const Module& m)
        {
            auto it = tables_.find(m.base);
            if (it != tables_.end()) return it->second;
            auto& t = tables_[m.base];
            IMAGE_DOS_HEADER dos{};
            IMAGE_NT_HEADERS64 nt{};
            if (Read(m.base, &dos, sizeof(dos)) && dos.e_magic == IMAGE_DOS_SIGNATURE &&
                Read(m.base + static_cast<std::uintptr_t>(dos.e_lfanew), &nt, sizeof(nt)) && nt.Signature == IMAGE_NT_SIGNATURE &&
                nt.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
                const auto& d = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
                if (d.VirtualAddress && d.Size && d.Size < 64u * 1024 * 1024) {
                    t.resize(d.Size / sizeof(RUNTIME_FUNCTION));
                    if (!Read(m.base + d.VirtualAddress, t.data(), t.size() * sizeof(RUNTIME_FUNCTION))) t.clear();
                }
            }
            return t;
        }

        HANDLE                                          process_;
        std::vector<Module>                             mods_;
        std::map<std::uintptr_t, std::vector<RUNTIME_FUNCTION>> tables_;
    };

    // ---------------------------------------------------------------- one thread's frozen state
    struct Frozen
    {
        DWORD                      tid = 0;
        bool                       ok = false;
        std::uint64_t              rip = 0;
        std::uint64_t              regs[16]{};  // rax rcx rdx rbx rsp rbp rsi rdi r8..r15
        std::uint64_t              stackBase = 0;  // address of stack[0] (= RSP at capture)
        std::vector<std::uint8_t>  stack;
        DWORD                      resumeError = 0;
    };

    inline Frozen Freeze(HANDLE process, DWORD tid, std::size_t maxStack)
    {
        Frozen f;
        f.tid = tid;
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, tid);
        if (!thread) return f;
        f.stack.resize(maxStack);
        if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
            CONTEXT c{};
            c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            if (GetThreadContext(thread, &c)) {
                f.ok = true;
                f.rip = c.Rip;
                const std::uint64_t r[16] = { c.Rax, c.Rcx, c.Rdx, c.Rbx, c.Rsp, c.Rbp, c.Rsi, c.Rdi, c.R8, c.R9, c.R10, c.R11, c.R12, c.R13, c.R14, c.R15 };
                std::memcpy(f.regs, r, sizeof(r));
                f.stackBase = c.Rsp;
                // Copy from RSP to the end of the committed stack (the stack's upper limit), at most maxStack bytes.
                MEMORY_BASIC_INFORMATION mbi{};
                std::size_t              n = 0;
                if (VirtualQueryEx(process, reinterpret_cast<LPCVOID>(c.Rsp), &mbi, sizeof(mbi)) == sizeof(mbi) && mbi.State == MEM_COMMIT) {
                    const auto end = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
                    n = (std::min<std::size_t>)(maxStack, static_cast<std::size_t>(end - c.Rsp));
                }
                SIZE_T got = 0;
                if (n) ReadProcessMemory(process, reinterpret_cast<LPCVOID>(c.Rsp), f.stack.data(), n, &got);
                f.stack.resize(got);
            }
            if (ResumeThread(thread) == static_cast<DWORD>(-1)) f.resumeError = GetLastError();
        }
        CloseHandle(thread);
        if (!f.ok) f.stack.clear();
        return f;
    }

    struct Frame
    {
        std::uint64_t rip = 0;
        bool          viaTable = false;  // unwound with the module's unwind table (exact) vs leaf/return-address pop
        std::uint64_t nv[8]{};           // (16) rbx rbp rsi rdi r12 r13 r14 r15 as they were in this frame
    };

    struct Walk
    {
        std::vector<Frame> frames;
        std::string        stop;  // why the walk ended
    };

    // Virtual unwind (the algorithm RtlVirtualUnwind implements) on the copied stack.
    inline Walk Unwind(Target& t, const Frozen& f, std::size_t maxFrames)
    {
        Walk w;
        std::uint64_t regs[16];
        std::memcpy(regs, f.regs, sizeof(regs));
        std::uint64_t rip = f.rip;
        auto stackRead = [&](std::uint64_t a, std::uint64_t& v) -> bool {
            if (a < f.stackBase || a + 8 > f.stackBase + f.stack.size()) return false;
            std::memcpy(&v, f.stack.data() + (a - f.stackBase), 8);
            return true;
        };
        bool viaTable = false;
        for (std::size_t n = 0; n < maxFrames; ++n) {
            {
                Frame fr{ rip, viaTable };
                const int idx[8] = { 3, 5, 6, 7, 12, 13, 14, 15 };
                for (int q = 0; q < 8; ++q) fr.nv[q] = regs[idx[q]];
                w.frames.push_back(fr);
            }
            const Module* m = t.Find(static_cast<std::uintptr_t>(rip));
            if (!m) { w.stop = "address is not in any loaded module (run-time code); cannot unwind further"; return w; }
            const auto       rva = static_cast<std::uint32_t>(rip - m->base);
            RUNTIME_FUNCTION rf{};
            const std::uint64_t rspBefore = regs[4];
            bool             machFrame = false;
            if (t.Lookup(*m, rva, rf)) {
                viaTable = true;
                std::uint32_t uiRva = rf.UnwindData;
                bool          primary = true;
                for (int chain = 0; chain < 32; ++chain) {
                    std::uint8_t hdr[4]{};
                    if (!t.Read(m->base + uiRva, hdr, 4)) { w.stop = "unwind info unreadable"; return w; }
                    const std::uint8_t flags = hdr[0] >> 3, count = hdr[2], frameReg = hdr[3] & 0xF, frameOff = hdr[3] >> 4;
                    std::vector<std::uint16_t> codes(count);
                    if (count && !t.Read(m->base + uiRva + 4, codes.data(), count * 2u)) { w.stop = "unwind codes unreadable"; return w; }
                    const std::uint32_t offInFunc = rva - rf.BeginAddress;
                    for (std::size_t k = 0; k < codes.size();) {
                        const std::uint8_t codeOff = codes[k] & 0xFF, op = (codes[k] >> 8) & 0xF, info = codes[k] >> 12;
                        std::size_t        slots = 1;
                        switch (op) {
                        case 1: slots = info == 0 ? 2 : 3; break;
                        case 4: case 8: slots = 2; break;
                        case 5: case 9: slots = 3; break;
                        case 7: slots = 3; break;
                        default: slots = 1; break;
                        }
                        const bool applies = !primary || codeOff <= offInFunc;
                        if (applies) {
                            std::uint64_t v = 0;
                            switch (op) {
                            case 0:  // UWOP_PUSH_NONVOL
                                if (!stackRead(regs[4], v)) { w.stop = "stack copy too short"; return w; }
                                regs[info] = v;
                                regs[4] += 8;
                                break;
                            case 1:  // UWOP_ALLOC_LARGE
                                if (k + 1 >= codes.size()) break;
                                regs[4] += info == 0 ? static_cast<std::uint64_t>(codes[k + 1]) * 8
                                                     : (k + 2 < codes.size() ? (static_cast<std::uint64_t>(codes[k + 1]) | (static_cast<std::uint64_t>(codes[k + 2]) << 16)) : 0);
                                break;
                            case 2:  // UWOP_ALLOC_SMALL
                                regs[4] += static_cast<std::uint64_t>(info) * 8 + 8;
                                break;
                            case 3:  // UWOP_SET_FPREG
                                regs[4] = regs[frameReg] - static_cast<std::uint64_t>(frameOff) * 16;
                                break;
                            case 4:  // UWOP_SAVE_NONVOL
                                if (k + 1 < codes.size() && stackRead(regs[4] + static_cast<std::uint64_t>(codes[k + 1]) * 8, v)) regs[info] = v;
                                break;
                            case 5:  // UWOP_SAVE_NONVOL_FAR
                                if (k + 2 < codes.size() && stackRead(regs[4] + (static_cast<std::uint64_t>(codes[k + 1]) | (static_cast<std::uint64_t>(codes[k + 2]) << 16)), v)) regs[info] = v;
                                break;
                            case 10: {  // UWOP_PUSH_MACHFRAME
                                const std::uint64_t base = regs[4] + (info ? 8 : 0);
                                std::uint64_t       newRip = 0, newRsp = 0;
                                if (!stackRead(base, newRip) || !stackRead(base + 24, newRsp)) { w.stop = "stack copy too short"; return w; }
                                rip = newRip;
                                regs[4] = newRsp;
                                machFrame = true;
                                break;
                            }
                            default: break;  // XMM saves and epilog markers do not move RSP
                            }
                        }
                        k += slots;
                    }
                    if (!(flags & 0x4)) break;  // UNW_FLAG_CHAININFO
                    // Chained: the parent RUNTIME_FUNCTION follows the (even-padded) code array; all its codes apply.
                    const std::uint32_t chainAt = uiRva + 4 + 2u * ((count + 1u) & ~1u);
                    RUNTIME_FUNCTION    parent{};
                    if (!t.Read(m->base + chainAt, &parent, sizeof(parent))) { w.stop = "chained unwind info unreadable"; return w; }
                    rf = parent;
                    uiRva = parent.UnwindData;
                    primary = false;
                }
            } else {
                viaTable = false;  // leaf function: the return address is on top of the stack
            }
            if (!machFrame) {
                std::uint64_t ret = 0;
                if (!stackRead(regs[4], ret)) { w.stop = "end of the copied stack"; return w; }
                regs[4] += 8;
                rip = ret;
            }
            if (rip == 0) { w.stop = "reached the start of the thread"; return w; }
            if (regs[4] <= rspBefore && !machFrame) { w.stop = "stack pointer did not move up; stopping to avoid a bad walk"; return w; }
        }
        w.stop = "frame limit reached";
        return w;
    }


    // ---------------------------------------------------------------- (16) lock waits and the objects they belong to

    inline std::string FormHex(std::uint32_t id)
    {
        char b[16]{};
        std::snprintf(b, sizeof(b), "%08X", id);
        return b;
    }

    // Class name from the object's MSVC RTTI ("Character"), or "" when p is not a C++ object with RTTI.
    inline std::string ClassName(const Target& t, std::uint64_t p)
    {
        std::uint64_t vtbl = 0, col = 0;
        if (!p || !t.Read(static_cast<std::uintptr_t>(p), &vtbl, 8) || !t.Find(static_cast<std::uintptr_t>(vtbl))) return {};
        if (!t.Read(static_cast<std::uintptr_t>(vtbl - 8), &col, 8)) return {};
        std::uint32_t c[6]{};  // signature, offset, cdOffset, type descriptor rva, class descriptor rva, self rva
        if (!t.Read(static_cast<std::uintptr_t>(col), c, sizeof(c)) || c[0] != 1) return {};
        const Module* m = t.Find(static_cast<std::uintptr_t>(col));
        if (!m || col - c[5] != m->base) return {};
        char name[129]{};
        if (!t.Read(m->base + c[3] + 0x10, name, 128)) return {};
        std::string n(name);
        if (n.rfind(".?AV", 0) == 0 || n.rfind(".?AU", 0) == 0) n = n.substr(4);
        if (const auto at = n.find("@@"); at != std::string::npos) n = n.substr(0, at);
        return n;
    }

    // "Character FF0012AB (base NPC 0001A6B2)", "TESObjectCELL 0000003C", or "" for memory that is not a C++ object
    inline std::string Describe(const Target& t, std::uint64_t p)
    {
        const auto cls = ClassName(t, p);
        if (cls.empty()) return {};
        std::string   s = cls;
        std::uint32_t formID = 0;
        std::uint8_t  type = 0;
        if (t.Read(static_cast<std::uintptr_t>(p + 0x14), &formID, 4) && t.Read(static_cast<std::uintptr_t>(p + 0x1A), &type, 1) && formID && type && type < 0x90) {
            s += " " + FormHex(formID);
            if (type == 0x3D || type == 0x3E) {  // reference / actor: the base object pointer at +0x40
                std::uint64_t base = 0;
                std::uint32_t baseID = 0;
                if (t.Read(static_cast<std::uintptr_t>(p + 0x40), &base, 8) && base && t.Read(static_cast<std::uintptr_t>(base + 0x14), &baseID, 4) && baseID)
                    s += " (base " + FormHex(baseID) + ")";
            }
        }
        return s;
    }

    struct LockWait
    {
        DWORD         waiter = 0;
        bool          wantsWrite = false;
        std::uint64_t lock = 0;
        std::uint32_t writer = 0;  // thread holding it for writing, 0 if none
        std::uint32_t word = 0;    // lock word: 0x80000000 = write, low bits = count
        std::vector<std::pair<DWORD, std::string>> regHolders;  // other threads whose saved registers carry the lock address
        std::string   object;      // the object the lock sits in, when one is found nearby
    };

    // Skyrim's BSReadWriteLock (CommonLib: { _writerThread, _lock }, kLockWrite 0x80000000). Threads blocked in
    // LockForRead / LockForWrite are matched to the lock they wait for (its address is in a saved register of the
    // wait frame or its caller), then to the thread that holds it.
    inline std::string LockReport(const Target& t, const AddressLibrary& al, const Module* exe, const std::wstring& ver,
        const std::vector<std::pair<DWORD, Walk>>& walks)
    {
        if (!exe || !al.Loaded()) return {};
        const bool se = ver.rfind(L"1-5-", 0) == 0;
        const std::uint64_t readOff = al.OffsetOf(se ? 66976 : 68233);
        const std::uint64_t writeOff = al.OffsetOf(se ? 66977 : 68234);
        if (!readOff || !writeOff) return {};
        std::set<DWORD> tids;
        for (const auto& w : walks) tids.insert(w.first);

        std::vector<LockWait> waits;
        for (const auto& [tid, w] : walks) {
            for (std::size_t k = 0; k < w.frames.size(); ++k) {
                const auto rip = w.frames[k].rip;
                if (rip < exe->base || rip >= exe->base + exe->size) continue;
                RUNTIME_FUNCTION rf{};
                if (!const_cast<Target&>(t).Lookup(*exe, static_cast<std::uint32_t>(rip - exe->base), rf)) continue;
                if (rf.BeginAddress != readOff && rf.BeginAddress != writeOff) continue;
                LockWait lw;
                lw.waiter = tid;
                lw.wantsWrite = rf.BeginAddress == writeOff;
                // candidates: saved registers of the wait frame and of its caller
                int best = 0;  // 4 = write-held by a live thread, 2 = read-held, +1 = in both frames
                for (std::size_t f = k; f < w.frames.size() && f <= k + 1; ++f) {
                    for (const auto v : w.frames[f].nv) {
                        std::uint32_t rw[2]{};
                        if (!v || !t.Read(static_cast<std::uintptr_t>(v), rw, 8)) continue;
                        const bool wHeld = (rw[1] & 0x80000000u) && tids.count(rw[0]);
                        const bool rHeld = !(rw[1] & 0x80000000u) && rw[0] == 0 && (rw[1] & 0x0FFFFFFF) > 0 && (rw[1] & 0x0FFFFFFF) < 0x10000;
                        // a value saved in both the wait frame and its caller is the lock argument itself
                        const bool both = k + 1 < w.frames.size() &&
                                          std::find(std::begin(w.frames[k].nv), std::end(w.frames[k].nv), v) != std::end(w.frames[k].nv) &&
                                          std::find(std::begin(w.frames[k + 1].nv), std::end(w.frames[k + 1].nv), v) != std::end(w.frames[k + 1].nv);
                        const int  score = (wHeld ? 4 : rHeld ? 2 : 0) + ((wHeld || rHeld) && both ? 1 : 0);
                        if (score > best) { best = score; lw.lock = v; lw.writer = wHeld ? rw[0] : 0; lw.word = rw[1]; }
                    }
                }
                if (lw.lock) waits.push_back(std::move(lw));
                break;
            }
        }
        if (waits.empty()) return {};

        for (auto& lw : waits) {
            // other threads that carry this lock's address in a saved register (it was taken or used in that frame)
            for (const auto& [tid, w] : walks) {
                if (tid == lw.waiter) continue;
                for (std::size_t k = 0; k < w.frames.size(); ++k) {
                    if (std::find(std::begin(w.frames[k].nv), std::end(w.frames[k].nv), lw.lock) != std::end(w.frames[k].nv)) {
                        lw.regHolders.emplace_back(tid, "frame #" + std::to_string(k));
                        break;
                    }
                }
            }
            // the object the lock is a member of: the nearest C++ object at most 0x400 bytes below it,
            // among the pointers saved in the waiting thread's and the holders' frames
            std::uint64_t bestP = 0;
            auto consider = [&](const Walk& w) {
                for (const auto& fr : w.frames)
                    for (const auto v : fr.nv)
                        if (v && v <= lw.lock && lw.lock - v <= 0x400 && v > bestP && !Describe(t, v).empty()) bestP = v;
            };
            for (const auto& [tid, w] : walks)
                if (tid == lw.waiter || tid == lw.writer ||
                    std::any_of(lw.regHolders.begin(), lw.regHolders.end(), [&](const auto& h) { return h.first == tid; }))
                    consider(w);
            if (bestP) lw.object = Describe(t, bestP) + " at +" + Hex(lw.lock - bestP);
        }

        std::string r = "LOCK WAITS (threads blocked on Skyrim read/write locks)\n";
        std::map<DWORD, std::set<DWORD>> edges;  // waiter -> threads it waits for
        for (const auto& lw : waits) {
            r += "  thread " + std::to_string(lw.waiter) + " waits to " + (lw.wantsWrite ? "WRITE" : "READ") + " lock " + Hex(lw.lock);
            if (!lw.object.empty()) r += " (in " + lw.object + ")";
            r += "\n";
            if (lw.writer) {
                r += "    held for WRITING by thread " + std::to_string(lw.writer) + "\n";
                edges[lw.waiter].insert(lw.writer);
            } else {
                r += "    held for READING by " + std::to_string(lw.word & 0x0FFFFFFF) + " reader(s)";
                if (!lw.regHolders.empty()) {
                    r += "; its address is in the saved registers of";
                    for (const auto& h : lw.regHolders) {
                        r += " thread " + std::to_string(h.first) + " (" + h.second + ")";
                        edges[lw.waiter].insert(h.first);
                    }
                    r += ": the likely holder(s)";
                }
                r += "\n";
            }
        }
        // cycles: a waits for b, b waits for ... a
        std::set<std::string> cycles;
        for (const auto& [start, _] : edges) {
            std::vector<DWORD> path{ start };
            std::function<void(DWORD)> dfs = [&](DWORD n) {
                if (path.size() > 8) return;
                for (const auto next : edges[n]) {
                    if (next == start) {
                        auto p = path;
                        std::rotate(p.begin(), std::min_element(p.begin(), p.end()), p.end());
                        std::string c;
                        for (const auto x : p) c += std::to_string(x) + " -> ";
                        cycles.insert(c + std::to_string(p.front()));
                    } else if (std::find(path.begin(), path.end(), next) == path.end()) {
                        path.push_back(next);
                        dfs(next);
                        path.pop_back();
                    }
                }
            };
            dfs(start);
        }
        for (const auto& c : cycles) r += "  DEADLOCK: each thread waits for the next one: " + c + "\n";
        if (cycles.empty()) r += "  no wait cycle found among these threads\n";
        return r + "\n";
    }

    inline std::string FrameText(const Target& t, const AddressLibrary& al, const Module* exe, std::uint64_t rip)
    {
        const Module* m = t.Find(static_cast<std::uintptr_t>(rip));
        if (!m) return Hex(rip) + " (no module)";
        std::string s = m->name + "+" + Hex(rip - m->base);
        if (m == exe) s += al.Label(rip - m->base);
        return s;
    }

    // Writes call_stacks.txt: the main thread first, then every other thread, threads with identical
    // stacks grouped together.
    inline void Write(DWORD pid, DWORD mainTid, const fs::path& file, const fs::path& log, void (*logFn)(const fs::path&, const std::string&))
    {
        HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
        if (!process) { logFn(log, "call stacks FAILED: cannot open the game process (" + std::to_string(GetLastError()) + ")"); return; }

        // Freeze each thread just long enough to copy registers and stack.
        std::vector<Frozen> threads;
        if (mainTid) threads.push_back(Freeze(process, mainTid, 512 * 1024));
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            THREADENTRY32 te{};
            te.dwSize = sizeof(te);
            if (Thread32First(snap, &te)) do {
                if (te.th32OwnerProcessID != pid || te.th32ThreadID == mainTid) continue;
                threads.push_back(Freeze(process, te.th32ThreadID, 128 * 1024));
            } while (Thread32Next(snap, &te));
            CloseHandle(snap);
        }

        Target         target(process, pid);
        const Module*  exe = nullptr;
        for (const auto& m : target.Modules()) {
            auto lower = m.name;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (lower == "skyrimse.exe" || lower == "skyrimvr.exe") { exe = &m; break; }
        }
        AddressLibrary al;
        std::string    alText = "Address Library: not loaded (SkyrimSE.exe frames show offsets only)";
        if (exe) {
            const auto ver = GameVersion(exe->path);
            const auto dir = fs::path(exe->path).parent_path();
            std::vector<fs::path> tries;
            for (const auto* prefix : { L"versionlib-", L"version-" }) {
                const std::wstring name = std::wstring(prefix) + ver + L".bin";
                tries.push_back(fs::path(L"Data") / L"SKSE" / L"Plugins" / name);  // through MO2's virtual Data folder
                tries.push_back(dir / L"Data" / L"SKSE" / L"Plugins" / name);
            }
            if (!ver.empty()) {
                for (const auto& p : tries) {
                    if (al.Load(p)) { alText = "Address Library: " + Narrow(p.wstring()); break; }
                }
            }
            if (ver.empty()) alText = "Address Library: game version unreadable; offsets only";
        }

        std::wstring gameVer = exe ? GameVersion(exe->path) : std::wstring{};
        std::ofstream out(file);
        out << "Version 16 call stacks (unwound with each module's unwind tables, read from the running game).\n"
            << "These are the real chains of callers, not raw stack values. Frame #0 is where the thread is now;\n"
            << "each line below it is the function that called the line above. SkyrimSE.exe frames carry the\n"
            << "Address Library ID (same IDs CrashLogger and CommonLib use).\n"
            << "When threads are stuck on Skyrim's read/write locks, LOCK WAITS names each lock, its holder, the object\n"
            << "it belongs to (form ID and base), and any deadlock (threads waiting for each other).\n"
            << alText << "\n\n";

        std::vector<std::pair<DWORD, Walk>> walks;
        std::string                         mainText;
        std::vector<std::string> otherKeys;
        std::map<std::string, std::vector<DWORD>> groups;
        for (std::size_t i = 0; i < threads.size(); ++i) {
            const auto& f = threads[i];
            std::string body;
            if (!f.ok) {
                body = "  context unavailable\n";
            } else {
                const auto w = Unwind(target, f, i == 0 && mainTid ? 128 : 48);
                walks.emplace_back(f.tid, w);
                for (std::size_t k = 0; k < w.frames.size(); ++k) {
                    body += "  #" + std::to_string(k) + "  " + FrameText(target, al, exe, w.frames[k].rip);
                    if (k && !w.frames[k].viaTable) body += "   (leaf/return-address step)";
                    body += '\n';
                }
                body += "  [" + w.stop + "]\n";
            }
            if (f.resumeError) body += "  WARNING: resuming this thread failed (" + std::to_string(f.resumeError) + ")\n";
            if (i == 0 && mainTid && f.tid == mainTid) {
                mainText = "MAIN THREAD " + std::to_string(f.tid) + "\n" + body + "\n";
            } else {
                if (!groups.count(body)) otherKeys.push_back(body);
                groups[body].push_back(f.tid);
            }
        }
        out << LockReport(target, al, exe, gameVer, walks) << mainText;
        out << "OTHER THREADS (" << threads.size() - (mainTid ? 1 : 0) << ", identical stacks grouped)\n\n";
        for (const auto& key : otherKeys) {
            const auto& tids = groups[key];
            out << (tids.size() > 1 ? std::to_string(tids.size()) + " threads: " : std::string("thread "));
            for (std::size_t k = 0; k < tids.size() && k < 12; ++k) out << (k ? ", " : "") << tids[k];
            if (tids.size() > 12) out << ", ...";
            out << '\n' << key << '\n';
        }
        out.flush();
        CloseHandle(process);
        logFn(log, "call stacks saved (" + std::to_string(threads.size()) + " threads)");
    }
}
