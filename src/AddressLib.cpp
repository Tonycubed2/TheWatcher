#include "PCH.h"
#include "AddressLib.h"

namespace AddressLib
{
	namespace
	{
		struct Entry
		{
			std::uint64_t offset;
			std::uint64_t id;
		};

		std::vector<Entry> g_entries;  // sorted by offset
		std::vector<Entry> g_byId;     // same entries sorted by id
		bool               g_loaded = false;
		bool               g_labels = true;

		template <class T>
		bool ReadRaw(std::ifstream& a_file, T& a_out)
		{
			a_file.read(reinterpret_cast<char*>(&a_out), sizeof(T));
			return static_cast<bool>(a_file);
		}

		std::uint64_t ReadU8(std::ifstream& f)
		{
			std::uint8_t v = 0;
			ReadRaw(f, v);
			return v;
		}
		std::uint64_t ReadU16(std::ifstream& f)
		{
			std::uint16_t v = 0;
			ReadRaw(f, v);
			return v;
		}
		std::uint64_t ReadU32(std::ifstream& f)
		{
			std::uint32_t v = 0;
			ReadRaw(f, v);
			return v;
		}
		std::uint64_t ReadU64(std::ifstream& f)
		{
			std::uint64_t v = 0;
			ReadRaw(f, v);
			return v;
		}

		// Formats 1 and 2: delta-encoded (id, offset) pairs. The format field has already been read.
		bool ReadDeltaFormat(std::ifstream& file, const std::filesystem::path& path, std::vector<Entry>& entries)
		{
			std::int32_t ver[4]{};
			for (auto& v : ver) {
				ReadRaw(file, v);
			}
			std::int32_t nameLen = 0;
			ReadRaw(file, nameLen);
			if (nameLen < 0 || nameLen > 0x10000) {
				spdlog::warn("Address Library file {} has a bad header", path.string());
				return false;
			}
			file.seekg(nameLen, std::ios::cur);
			std::int32_t pointerSize = 0;
			std::int32_t count = 0;
			ReadRaw(file, pointerSize);
			ReadRaw(file, count);
			if (!file || pointerSize <= 0 || count <= 0) {
				spdlog::warn("Address Library file {} has a bad header", path.string());
				return false;
			}

			entries.reserve(static_cast<std::size_t>(count));
			std::uint64_t prevID = 0;
			std::uint64_t prevOffset = 0;
			for (std::int32_t i = 0; i < count; ++i) {
				const auto type = static_cast<std::uint8_t>(ReadU8(file));
				const auto lo = type & 0xF;
				const auto hi = type >> 4;

				std::uint64_t id = 0;
				switch (lo) {
				case 0: id = ReadU64(file); break;
				case 1: id = prevID + 1; break;
				case 2: id = prevID + ReadU8(file); break;
				case 3: id = prevID - ReadU8(file); break;
				case 4: id = prevID + ReadU16(file); break;
				case 5: id = prevID - ReadU16(file); break;
				case 6: id = ReadU16(file); break;
				case 7: id = ReadU32(file); break;
				default:
					spdlog::warn("Address Library file {} is corrupt (entry {})", path.string(), i);
					return false;
				}

				const std::uint64_t base = (hi & 8) ? prevOffset / static_cast<std::uint64_t>(pointerSize) : prevOffset;
				std::uint64_t offset = 0;
				switch (hi & 7) {
				case 0: offset = ReadU64(file); break;
				case 1: offset = base + 1; break;
				case 2: offset = base + ReadU8(file); break;
				case 3: offset = base - ReadU8(file); break;
				case 4: offset = base + ReadU16(file); break;
				case 5: offset = base - ReadU16(file); break;
				case 6: offset = ReadU16(file); break;
				case 7: offset = ReadU32(file); break;
				}
				if (hi & 8) {
					offset *= static_cast<std::uint64_t>(pointerSize);
				}

				if (!file) {
					spdlog::warn("Address Library file {} ended early (entry {} of {})", path.string(), i, count);
					return false;
				}
				entries.push_back({ offset, id });
				prevID = id;
				prevOffset = offset;
			}
			return true;
		}

		// Format 5 (AE 1.7.x): version[4], name[64], pointerSize, dataFormat, offsetCount, then one uint32
		// offset per ID (index = ID, 0 = no such ID). The format field has already been read.
		bool ReadDenseFormat(std::ifstream& file, const std::filesystem::path& path, std::vector<Entry>& entries)
		{
			std::uint32_t ver[4]{};
			for (auto& v : ver) {
				ReadRaw(file, v);
			}
			char name[64]{};
			file.read(name, sizeof(name));
			std::int32_t pointerSize = 0;
			std::int32_t dataFormat = 0;
			std::int32_t count = 0;
			ReadRaw(file, pointerSize);
			ReadRaw(file, dataFormat);
			ReadRaw(file, count);
			if (!file || count <= 0 || count > 0x4000000) {
				spdlog::warn("Address Library file {} has a bad header", path.string());
				return false;
			}

			std::vector<std::uint32_t> offsets(static_cast<std::size_t>(count));
			file.read(reinterpret_cast<char*>(offsets.data()), static_cast<std::streamsize>(offsets.size() * sizeof(std::uint32_t)));
			if (!file) {
				spdlog::warn("Address Library file {} is truncated ({} IDs expected)", path.string(), count);
				return false;
			}

			entries.reserve(offsets.size());
			for (std::size_t id = 0; id < offsets.size(); ++id) {
				if (offsets[id] != 0) {
					entries.push_back({ offsets[id], id });
				}
			}
			return true;
		}
	}

	bool Load()
	{
		if (g_loaded) {
			return true;
		}

		// AE ships versionlib-<ver>.bin (format 2 on 1.6.x, format 5 on 1.7.x); SE ships version-<ver>.bin (format 1).
		const auto version = REL::Module::get().version().string("-"sv);
		const std::filesystem::path aePath = std::format("Data/SKSE/Plugins/versionlib-{}.bin", version);
		const std::filesystem::path sePath = std::format("Data/SKSE/Plugins/version-{}.bin", version);

		std::filesystem::path path = aePath;
		bool                  isAEFile = true;
		std::ifstream         file(aePath, std::ios::binary);
		if (!file) {
			file.clear();
			file.open(sePath, std::ios::binary);
			path = sePath;
			isAEFile = false;
		}
		if (!file) {
			spdlog::warn("Address Library file not found ({} or {}); stack frames will not show IDs",
				aePath.string(), sePath.string());
			return false;
		}

		std::int32_t format = 0;
		ReadRaw(file, format);
		const bool formatOk = isAEFile ? (format == 2 || format == 5) : (format == 1);
		if (!formatOk) {
			spdlog::warn("Address Library file {} has format {} (expected {}); stack frames will not show IDs",
				path.string(), format, isAEFile ? "2 or 5" : "1");
			return false;
		}

		std::vector<Entry> entries;
		const bool         ok = format == 5 ? ReadDenseFormat(file, path, entries) : ReadDeltaFormat(file, path, entries);
		if (!ok) {
			return false;
		}

		g_byId = entries;
		std::sort(g_byId.begin(), g_byId.end(), [](const Entry& a, const Entry& b) { return a.id < b.id; });
		std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.offset < b.offset; });
		g_entries = std::move(entries);
		g_loaded = true;
		spdlog::info("Address Library loaded: {} IDs from {} (format {})", g_entries.size(), path.string(), format);
		return true;
	}

	void SetLabelsEnabled(bool a_enabled)
	{
		g_labels = a_enabled;
	}

	bool Lookup(std::uintptr_t a_offset, std::uint64_t& a_id, std::uint64_t& a_delta)
	{
		if (!g_labels || !g_loaded || g_entries.empty()) {
			return false;
		}
		auto it = std::upper_bound(g_entries.begin(), g_entries.end(), static_cast<std::uint64_t>(a_offset),
			[](std::uint64_t v, const Entry& e) { return v < e.offset; });
		if (it == g_entries.begin()) {
			return false;
		}
		--it;
		a_id = it->id;
		a_delta = a_offset - it->offset;
		return true;
	}

	std::string Annotate(std::uintptr_t a_offset)
	{
		std::uint64_t id = 0;
		std::uint64_t delta = 0;
		if (!Lookup(a_offset, id, delta)) {
			return {};
		}
		return std::format(" -> {}+0x{:X}", id, delta);
	}

	bool IdToOffset(std::uint64_t a_id, std::uintptr_t& a_offset)
	{
		if (!g_loaded || g_byId.empty()) {
			return false;
		}
		auto it = std::lower_bound(g_byId.begin(), g_byId.end(), a_id,
			[](const Entry& e, std::uint64_t v) { return e.id < v; });
		if (it == g_byId.end() || it->id != a_id || it->offset == 0) {
			return false;
		}
		a_offset = static_cast<std::uintptr_t>(it->offset);
		return true;
	}
}
