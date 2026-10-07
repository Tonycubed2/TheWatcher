#pragma once
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace DumpValidation {
// Structural validation, not a promise that every debugger can unwind every stack.
// Read only metadata, never map/read multi-GB memory payloads into the helper's heap.
inline bool Validate(const std::filesystem::path& path, std::string& error) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    auto fail = [&](const char* reason) { error = reason; return false; };
    if (ec || size < 32) return fail("missing/truncated header");
    std::ifstream in(path, std::ios::binary);
    if (!in) return fail("cannot reopen completed dump");
    auto range = [&](std::uint64_t offset, std::uint64_t bytes) {
        return offset <= size && bytes <= size - offset;
    };
    auto read = [&](std::uint64_t offset, void* out, std::size_t bytes) {
        if (!range(offset, bytes)) return false;
        in.clear(); in.seekg(static_cast<std::streamoff>(offset));
        in.read(static_cast<char*>(out), static_cast<std::streamsize>(bytes));
        return static_cast<bool>(in);
    };
    auto u32 = [&](std::uint64_t offset, std::uint32_t& value) { return read(offset, &value, 4); };
    auto u64 = [&](std::uint64_t offset, std::uint64_t& value) { return read(offset, &value, 8); };
    std::uint32_t signature{}, version{}, count{}, directory{};
    if (!u32(0, signature) || !u32(4, version) || !u32(8, count) || !u32(12, directory))
        return fail("unreadable header");
    if (signature != 0x504d444d || (version & 0xffff) != 0xa793)
        return fail("invalid minidump signature/version");
    if (!count || count > 4096 || directory < 32 || !range(directory, 12ULL * count))
        return fail("invalid stream directory");
    bool threads = false, modules = false, system = false;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t type{}, bytes{}, rva{};
        const auto offset = directory + 12ULL * i;
        if (!u32(offset, type) || !u32(offset + 4, bytes) || !u32(offset + 8, rva))
            return fail("unreadable stream entry");
        if (bytes && (rva < 32 || !range(rva, bytes))) return fail("stream outside file");
        if (type == 3 || type == 4 || type == 5) {
            std::uint32_t entries{};
            const std::uint64_t stride = type == 3 ? 48 : type == 4 ? 108 : 16;
            if (bytes < 4 || !u32(rva, entries) || 4 + stride * entries > bytes)
                return fail("truncated thread/module/memory list");
            if ((type == 3 || type == 4) && !entries) return fail("empty required list");
            if (type == 3) {
                threads = true;
                for (std::uint32_t j = 0; j < entries; ++j) {
                    for (const auto location : {32, 40}) {
                        std::uint32_t n{}, at{};
                        const auto base = rva + 4ULL + 48ULL * j + location;
                        if (!u32(base, n) || !u32(base + 4, at) || (n && !range(at, n)))
                            return fail("thread stack/context outside file");
                    }
                }
            } else if (type == 4) modules = true;
            else for (std::uint32_t j = 0; j < entries; ++j) {
                std::uint32_t n{}, at{};
                const auto base = rva + 4ULL + 16ULL * j + 8;
                if (!u32(base, n) || !u32(base + 4, at) || (n && !range(at, n)))
                    return fail("memory range outside file");
            }
        } else if (type == 7) {
            if (bytes < 56) return fail("truncated system info");
            system = true;
        } else if (type == 9) {
            std::uint64_t entries{}, base{};
            if (bytes < 16 || !u64(rva, entries) || !u64(rva + 8ULL, base) || entries > (bytes - 16ULL) / 16)
                return fail("truncated full-memory directory");
            for (std::uint64_t j = 0; j < entries; ++j) {
                std::uint64_t n{};
                if (!u64(rva + 24ULL + 16ULL * j, n) || !range(base, n))
                    return fail("full-memory payload outside file");
                base += n; // bounded by range(), so no integer overflow
            }
        }
    }
    if (!threads || !modules || !system) return fail("required streams missing (possibly zeroed directory)");
    error.clear(); return true;
}
}
