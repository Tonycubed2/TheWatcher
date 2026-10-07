## Version 10

- Disabled automatic loading-screen captures by default; manual F12 capture remains available.
- Retained load-progress logging and labeled quiet loading phases as slow loads, not confirmed freezes.
- Increased default loading warning to 60 seconds and optional loading capture threshold to 300 seconds.
- Paused background CPU profiling, CSV stats, equip logging and optional slow-stack sampling during loads.
- Reduced loading status writes to every 10 seconds and skipped detailed context/Papyrus reads.
- Reset CPU/FPS sampling baselines across loading and disabled all-actor equip logging by default.
- Added independent target-lifetime waits so monitor/capture helpers stop when Skyrim exits.
- Bound shared capture workers to the original process creation time to guard against PID reuse.
- Added structural dump validation and temporary partial writes; invalid/incomplete dumps are discarded.
- Added a default 1024 MiB soft storage cap, deleting oldest dumps before old text logs.
- Kept dumps off, instant F12, focus-independent capture and 8-second gameplay freeze detection.
- Requires updating both TheWatcher.dll and TheWatcherDump.exe and installing the new INI.

## Version 9

- Prevented overlapping independent and in-game captures with shared nonblocking ownership.
- Limited automatic capture to one attempt per stall, with a cooldown and sustained recovery before rearming.
- Increased the default frame-freeze threshold to 8 seconds.
- Disabled dumps by default in the INI and compiled fallbacks.
- Protected old level-3 INIs from full dumps unless explicitly enabled with bAllowFullMemoryDump=1.
- Removed the in-process dump fallback when the external helper is unavailable.
- Disabled continuous thread suspension in the default diagnostic profile while retaining logs and CPU statistics.
- Added external thread/module snapshots without requiring a dump.
- Avoided duplicate thread sampling/dumps/three-beep alerts in supplemental reports.
- Kept instant F12 capture, automatic helper startup and capture regardless of window focus.
- Requires replacing both TheWatcher.dll and TheWatcherDump.exe and using the updated INI.
