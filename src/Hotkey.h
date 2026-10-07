#pragma once
#include <Windows.h>

// (version 5) Manual capture hotkey.
// The watchdog thread checks the key every 50 ms with GetAsyncKeyState, so it works even while the game is frozen
// (the watchdog is not the game's thread). Window focus does not restrict captures. It never blocks the
// key: Windows, the game and other mods still see it.
namespace Hotkey
{
	// Parse sHotkey / fHotkeyHoldSeconds from the INI. Logs the result. Call before the watchdog starts.
	void Start();
	// Read the parsed binding once at startup for the external watchdog.
	void GetBinding(DWORD& key, DWORD& modifiers);

	// True once per press (or once per hold, in hold mode). Called from the watchdog thread every loop.
	bool ConsumePressed();
}
