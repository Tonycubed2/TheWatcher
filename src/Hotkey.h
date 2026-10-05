#pragma once

// (version 5) Manual capture hotkey.
// A low-level keyboard hook on its own thread watches for the key combination from TheWatcher.ini [Hotkey].
// It works even while the game is frozen (the hook thread is not the game's), and only reacts while the
// game window is in front. The key is passed on to Windows and the game as normal; it is never swallowed.
namespace Hotkey
{
	// Parse sHotkey from the INI and start the hook thread. Logs the result.
	void Start();

	// True once per press (cleared when read). Called from the watchdog thread.
	bool ConsumePressed();
}
