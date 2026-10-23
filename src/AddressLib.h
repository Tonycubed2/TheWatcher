#pragma once

// Reads the Address Library database (Data/SKSE/Plugins/versionlib-<game version>.bin) so stack frames in
// SkyrimSE.exe can be labeled "ID+offset", the same way CrashLogger labels them.
// Formats: 1 = SE 1.5.x (version-*.bin), 2 = AE 1.6.x, 5 = AE 1.7.x (dense offset table).
namespace AddressLib
{
	// Load once at startup (main thread). Returns false if the file is missing or unreadable.
	bool Load();

	// bAddressLibraryIDs: when false, Annotate/Lookup report nothing (the database is still loaded for IdToOffset)
	void SetLabelsEnabled(bool a_enabled);

	// For an offset from SkyrimSE.exe's base: " -> 12345+0x1A" (nearest ID at or below), or "" if unavailable.
	std::string Annotate(std::uintptr_t a_offset);

	// (1.1) Same lookup as numbers: nearest ID at or below the offset, and the distance from it
	bool Lookup(std::uintptr_t a_offset, std::uint64_t& a_id, std::uint64_t& a_delta);

	// (12, 1.7 support) Offset of an ID from SkyrimSE.exe's base, without CommonLib's fatal error when the ID
	// is missing. Returns false if the database is not loaded or does not contain the ID.
	bool IdToOffset(std::uint64_t a_id, std::uintptr_t& a_offset);
}
