#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace StoragePolicy {
namespace fs = std::filesystem;
struct Result {
    std::uint64_t before = 0, after = 0;
    std::size_t removed = 0;
    bool errors = false;
};
struct Entry { fs::path path; std::uint64_t size; fs::file_time_type time; bool dump; };
inline std::size_t RemoveIncomplete(const fs::path& root) {
    std::size_t removed = 0;
    std::error_code ec;
    if (fs::is_symlink(root, ec)) return 0;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (it->is_symlink(ec)) { it.disable_recursion_pending(); continue; }
        if (it->is_regular_file(ec) && it->path().filename().wstring().ends_with(L".dmp.partial")) {
            // Windows refuses removal while an active worker owns the exclusive writer handle.
            std::error_code removeError;
            if (fs::remove(it->path(), removeError)) ++removed;
        }
    }
    return removed;
}
// Only TheWatcher's own subfolder. Call outside loading and under the shared diagnostic gate.
// Active/recent non-dump logs are protected; a soft cap cannot truncate an open log safely.
inline Result Enforce(const fs::path& root, std::uint64_t limit,
                      const std::vector<fs::path>& protectedPaths = {}) {
    Result result;
    if (!limit) return result;
    std::error_code ec;
    std::vector<Entry> entries;
    if (fs::is_symlink(root, ec)) { result.errors = true; return result; }
    const auto recent = fs::file_time_type::clock::now() - std::chrono::minutes(2);
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        // Never follow or delete through links into unrelated folders.
        if (it->is_symlink(ec)) { if (it->is_directory(ec)) it.disable_recursion_pending(); continue; }
        if (!it->is_regular_file(ec)) continue;
        const auto bytes = it->file_size(ec);
        if (ec) break;
        const auto time = it->last_write_time(ec);
        if (ec) break;
        result.before += bytes;
        bool protect = false;
        const auto path = it->path().lexically_normal();
        for (const auto& p : protectedPaths) {
            if (p.empty()) continue;
            const auto rel = path.lexically_relative(p.lexically_normal());
            if (rel.empty() || (rel.begin() != rel.end() && *rel.begin() != "..")) { protect = true; break; }
        }
        const bool dump = path.extension() == L".dmp" || path.extension() == L".partial";
        if (!protect && (dump || time < recent)) entries.push_back({path, bytes, time, dump});
    }
    result.errors = static_cast<bool>(ec);
    result.after = result.before;
    if (result.errors) return result; // do not enforce against an incomplete size inventory
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.dump != b.dump) return a.dump > b.dump; // delete dumps before text evidence
        if (a.time != b.time) return a.time < b.time;
        return a.path < b.path;
    });
    for (const auto& e : entries) {
        if (result.after <= limit) break;
        ec.clear();
        if (fs::remove(e.path, ec)) { result.after -= e.size; ++result.removed; }
        if (ec) result.errors = true;
    }
    return result;
}
}
