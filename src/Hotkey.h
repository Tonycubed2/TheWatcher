#pragma once

// (version 5) Manual capture hotkey.
// The watchdog thread checks the key every 50 ms with GetAsyncKeyState, so it works even while the game is frozen
// (the watchdog is not the game's thread). It only reacts while the game window is in front, and never blocks the
// key: Windows, the game and other mods still see it.
namespace Hotkey
{
	// Parse sHotkey / fHotkeyHoldSeconds from the INI. Logs the result. Call before the watchdog starts.
	void Start();

	// True once per press (or once per hold, in hold mode). Called from the watchdog thread every loop.
	bool ConsumePressed();
}
