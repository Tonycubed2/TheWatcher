## Version 16 - lock waits and deadlocks, beeps back

- call_stacks.txt has a new LOCK WAITS section when threads are stuck on Skyrim's read/write locks (BSReadWriteLock):
  - which thread waits for which lock, and whether it wants to read or write;
  - who holds it: the writing thread from the lock itself, or for a read lock the threads that carry the lock in their saved registers;
  - the object the lock belongs to, read from the game's own type information, with its form ID and base form (for example a Character and its base NPC);
  - DEADLOCK lines when threads wait for each other in a circle.
- Found from a real freeze on waking from sleep: two of Skyrim's job threads locked the same actor in opposite order (one casting a spell and sending a Papyrus event, one updating the actor), and the main thread waited for both forever. Version 16 names those threads and the actor directly.
- The three beeps and flashing taskbar button on a capture are back. Since version 12 every capture comes from the external helper, and those captures skipped the alert.
- Works on SE 1.5.97 and AE 1.6 / 1.7 (Address Library IDs for LockForRead / LockForWrite). Skyrim VR: call stacks without the LOCK WAITS section.
- Helper protocol is unchanged (13). Install TheWatcher.dll and TheWatcherDump.exe together.

## Version 15 - real call stacks

- New file in every external capture folder: call_stacks.txt (manual F12 captures and automatic freeze captures).
- Every thread's stack is walked the way a debugger does it, using each module's own unwind tables (.pdata/UNWIND_INFO) read from the running game. The result is the true chain of callers, not the raw "candidate" values in thread_snapshot.txt.
- Works for SkyrimSE.exe even though its code is encrypted on disk: the unwind tables are read from the game's memory.
- SkyrimSE.exe frames are labeled with Address Library IDs (formats 1, 2 and 5: SE, AE 1.6, AE 1.7), the same IDs CrashLogger and CommonLib use. The game version comes from SkyrimSE.exe; the Address Library file is looked up in Data\SKSE\Plugins.
- The main thread is listed first (up to 128 frames); all other threads follow, with identical stacks grouped (for example 22 idle worker threads on one entry).
- Each thread is suspended only long enough to copy its registers and stack, then resumed; the walking happens on the copy.
- Fix (hook trace): the "PATCHED IN MEMORY" check compared 16 bytes even when the thread's function ended earlier, so a hook on the NEXT function in memory was reported as a patch on the thread's path. It now compares only bytes inside the thread's own function.
- TheWatcherDump.exe now also links version.lib. Helper protocol is unchanged (13).

## Version 14 - hook trace

- New file in every external capture folder: hook_trace.txt (manual F12 captures and automatic freeze captures).
- Main thread activity: CPU time used over half a second, so a capture says whether the frozen thread is running non-stop (an endless loop) or waiting (a lock, the GPU/driver, another thread). Five instruction-pointer samples, 100 ms apart.
- Names where the main thread is: module and offset, the exported function name when there is one, and whether it is the first instruction of a function (read from the module file's own unwind table).
- Compares the code there with the module file on disk. Code a mod rewrote at run time (a hook) is reported as PATCHED IN MEMORY; bytes the Windows loader relocates are skipped, so normal relocation is not reported.
- Follows the jumps a hook writes (jmp rel32/rel8, jmp [rip+x], mov rax/r10/r11 + jmp, push + ret) hop by hop. Run-time memory that belongs to no DLL (a hook library's jump page) is described, and its pointers into modules are counted, which shows the DLL that owns the hook.
- Reports a jump that leads back to an earlier hop as an endless loop, and a jump into unreadable memory as the crash case.
- Reason: a freeze on 2026-11-03 stopped the main thread on the first instruction of Windows' DXGI Present (dxgi.dll+0x19000), and a crash the day before happened there on "jmp 0x7FF7ED4D0000". The old capture could not say which mod placed that jump.
- Only reads memory. The main thread is suspended for microseconds per sample to read its registers and resumed before anything is written.
- TheWatcherDump.exe now links psapi.lib. Helper protocol is unchanged (13).

## Version 13 - startup capture fix

- Starts the independent helper and configures the manual capture key during plugin initialization, before Address Library loading and frame-hook installation.
- Makes external manual capture available before SKSE DataLoaded, including when startup stalls before the main menu.
- Keeps external automatic captures disabled until game-dependent monitoring initializes; discards startup clocks and waits for a fresh heartbeat.
- Leaves event sinks and the in-game watchdog at DataLoaded, when game APIs are available.
- Logs startup milestones, missing/failed SKSE message-listener registration, helper path resolution, output directory errors, and helper launch path.
- Ignores duplicate DataLoaded initialization.
- Uses helper protocol 13 to reject older helpers that lack the startup gate. Rebuild and install BOTH TheWatcher.dll and TheWatcherDump.exe from this source together.
- Keeps existing INI keys and settings. Startup hangs require a manual key press; automatic startup-stall detection is intentionally not enabled.
- Fixes a diagnostic coverage gap; does not claim to fix the underlying game startup hang.

## Version 12 (beta) - Skyrim 1.7.x support

- Supports Skyrim AE 1.7.x (checked against 1.7.104) in addition to SE 1.5.97 and AE 1.6.x.
- Built against CommonLibSSE-NG 6.7.0, which reads the 1.7 Address Library format and
  carries the SKSE "Address Library V5" flag SKSE 2.3.x requires on 1.7.x.
- Requires SKSE 2.3.x and Address Library for 1.7.x (versionlib-1-7-104-0.bin) on 1.7.104.
- Stack frame ID labels read the new Address Library format on 1.7.x.
- The frame hook checks its call site before patching: it confirms the call into
  Main::Update, chains behind another plugin's hook at the same site, or finds the
  moved call. On 1.7.x and newer, an unconfirmed site disables frame detection
  instead of patching game code.
- A missing Address Library ID no longer stops the game at startup on 1.7.x;
  frame detection is turned off with a log line.
- Skyrim VR 1.4.15 support kept (CommonLibSSE-NG needs the OpenVR SDK files in extern\openvr).
- New: load breadcrumbs (iRecentLoadEvents=40, [Context]). The last load events (object
  loaded/unloaded, reference attached/detached, cell fully loaded) are kept with form IDs,
  names and the plugins that add and last edit each form.
  - SLOW LOAD warnings in TheWatcher.log now list them.
  - New LONG LOAD warning: a loading screen open for fLoadWarnSeconds while load events
    keep arriving (a looping load, which the no-progress warning never caught) is logged
    with the recent events and the forms that loaded most often during it.
  - Every capture report has [RECENT LOAD EVENTS] and [MOST REPEATED DURING THE ... LOADING
    SCREEN] sections; summary.json has "recent_loads".
- In-game "logs and report saved" message calls the game function directly (CommonLibSSE-NG 6.7.0
  removed RE::DebugNotification); skipped with a log line if the game version lacks it.
- New: shader-compile pauses are not freezes (bIgnoreShaderCompiles=1, [Detection]). While the game is not
  finishing frames, the main thread's stack is checked twice a second; inside a shader compiler
  (sShaderCompileModules, default d3dcompiler) neither the in-game watchdog nor the independent helper
  captures, for up to fShaderCompileMaxSeconds (120). Covers ENB, Effects 11 and Community Shaders
  recompiling effects. TheWatcher.log and the events log note each pause; a capture that still happens
  during a compile is labelled in its report.
- Helper protocol still 12; the shared block gained one field at the end (shader heartbeat).
  Install the DLL and TheWatcherDump.exe from the same build.

## Version 12 (beta)

- External captures now collect Windows wait-chain evidence when bWaitChains=1
  (wait_chains.txt): which lock each thread waits for, who holds it, reported cycles.
- Wait-chain queries run in a disposable process with a five-second deadline;
  they never suspend Skyrim threads. Initial snapshot/small dump is saved first.
- Reports prioritize the main thread and distinguish access failures, partial
  chains and reported cycles. Absence of a cycle does not exclude deadlock.
- COM-specific WCT callbacks are not registered; COM owners are not resolved.
- Resource attribution distinguishes confirmed thread-entry module locations,
  inferred stack origins and unknown origins with self-reported thread-name hints.
- New-thread attribution and busy-thread sampling share the configured per-tick
  attempt limit; threads that miss it are retried on later ticks. Sampling
  remains disabled by default.
- A dump helper still running at its deadline is reported as TIMED OUT, not FAILED.
- summary.json reports the correct version.
- Only the frame hook publishes the main thread ID.
- Shared protocol is version 12: install DLL and helper EXE together.
  Version 11 and earlier helpers are rejected.
- Portable regression checks passed. Windows/MSVC and in-game verification pending;
  a Windows mutex/wait-chain integration test is included but was not run here.

## Version 11

- F12 pending-request queue, bPapyrusStats setting and distinct dump statuses.

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
