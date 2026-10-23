## Version 13: startup capture update

Build and install TheWatcher.dll and TheWatcherDump.exe together from this source; the IPC protocol is now 13. Existing INI settings remain supported. The helper starts during plugin initialization and accepts the configured manual key before DataLoaded. External automatic capture remains off until game monitoring initializes and a fresh heartbeat arrives. Event sinks and the in-game watchdog still start at DataLoaded. See BUILD_VERSION_13.txt for build steps, validation steps and limitations.

# The Watcher — version 13

Reduces capture interference during legitimate loading and improves helper lifetime,
dump integrity and storage retention. Gameplay freeze detection remains 8 seconds.

## Defaults and loading

- `bCaptureLoadingStalls=0`: loading screens never automatically trigger captures by default.
  F12 remains available during loading. Load/menu/progress breadcrumbs are retained.
- `fLoadWarnSeconds=60`: a quiet load is logged as a slow load, not proof of a freeze.
  Zero gameplay frames during a loading screen is expected.
- `fLoadStallSeconds=300`: the no-event threshold used ONLY when loading capture is
  explicitly enabled. Game load events can be silent during legitimate phases.
- `bReduceWorkDuringLoading=1`: CPU/thread profiling, CSV stats, equip-event logging
  and optional slow-stack sampling pause while loading. Status writes slow to every
  10 seconds and omit detailed context/Papyrus reads. CPU baselines reset on resume;
  loading CPU is not attributed to gameplay. Actor context updates already skip loading.
- All-actor equip logging is now off by default; player equip events resume after loading.
- Dumps remain off in Capture, Aggressive and compiled defaults. F12 remains instant
  and window focus never suppresses capture. Snapshots still briefly suspend individual
  target threads to copy context/raw stack data; raw values are not unwound call stacks.

## Helper lifetime and optional dumps

The persistent monitor stays alive during the game session to monitor heartbeats/F12.
Capture workers return immediately after their work completes. Each helper has an
independent wait on the original Skyrim process handle and stops when Skyrim exits,
even if filesystem or capture work is stuck. Shared captures also check the target's
creation time to avoid acting on a reused PID. No other Skyrim/MO2 session or unrelated
helper is searched for or terminated. Restart Skyrim after installing the new binaries.

Optional dumps write to `.dmp.partial` first. Only a successful, flushed dump that passes
structural validation is renamed to `.dmp`. Checks cover the header, stream directory,
required thread/module/system streams, thread stack/context locations and memory payload
bounds. Failed validation removes the partial file and records the reason. Interrupted
partials are cleaned on the next session outside loading/capture. Structural validation
cannot guarantee successful stack unwinding in every debugger.

Snapshot-based PSS dumps are not implemented in this release. Enabling optional live
process dumps can still pause Skyrim while Windows writes them. Dumps remain disabled
by default; full memory still requires both level 3 and `bAllowFullMemoryDump=1`.

## External wait chains and resource attribution (introduced in version 12)

When `bWaitChains=1`, captures save `wait_chains.txt` from a separate process.
The capture worker saves its initial snapshot/small dump before starting this
process. The WCT child never suspends target threads. A job object binds its
lifetime to the capture worker; a five-second deadline stops only this child.
Logs distinguish setup failures, unavailable queries and incomplete timeout
results. The file is flushed before each query so earlier results survive a timeout.
The main thread is queried first. WCT cannot diagnose every wait type, and no
reported cycle does not prove that the game is free of deadlocks. This release
does not register COM-specific WCT callbacks, so COM resolution may be incomplete.

Optional `iMinidumpLevel=1` already provides a small dump for debugger stack
analysis. Raw stack snapshots remain candidates, not unwound call stacks; live
external stack unwinding has not been added. Dump sizes, pause duration and
successful debugger unwinding are not guaranteed.

Resource reports use confirmed entry-module locations, inferred stack origins,
and unknown origins. A thread's entry module is not proof of who consumed all
its CPU time. Thread-name hints are self-reported and are never used to assign
ownership. `iResourceSampleThreads=0` still disables resource stack sampling.
When enabled, the per-tick limit now bounds all resource stack capture attempts,
including new-thread attribution and failed attempts. Loading stays excluded.
No in-game overhead benchmark has been performed.

## Storage

`iMaxStorageMB=1024` in General sets a 1024 MiB (1 GiB) soft cap for the TheWatcher
subfolder. `0` disables the byte cap. Existing capture-count limits still apply.
Outside loading/captures, retention checks run every 60 seconds under the diagnostic
ownership gate. Oldest dumps are removed before old text evidence; symlinked files/folders
are skipped. Current monitor/CSV paths and recent non-dump files are protected, so the cap
can be exceeded by active files or during a write. A dump larger than the cap can be
removed on a subsequent cleanup. SKSE logs outside TheWatcher are not subject to this cap.

## Build and install

Extract into C:\dllmaker\TheWatcher, replacing source files. In the x64 Native Tools
Command Prompt for VS 2022:

    cd /d C:\dllmaker\TheWatcher
    cmake --preset build-release-msvc
    cmake --build --preset release-msvc --parallel 1

Install `build/release-msvc/TheWatcher.dll`, its matching `TheWatcher.pdb`,
`build/release-msvc/TheWatcherDump.exe`, and the included `TheWatcher.ini` together
in SKSE/Plugins. Both DLL and EXE must be version 13. The helper launches automatically.
Use the new INI to apply the updated defaults; old INI overrides may retain heavier settings.

## Validation and limits

Portable C++ regression checks cover eight-second gameplay detection, a six-minute load
without automatic captures, explicit loading opt-in, loading/gameplay transitions,
59-second capture overlap suppression and recovery/rearming. Separate tests cover zeroed
and truncated dump metadata/payloads, retention ordering, protected files, disabled caps,
partial cleanup and symlink exclusion. INI and source/build protocol checks are included.

A Windows integration test in `tests/external_waitchains_test.cpp` checks a
real mutex wait. That Windows integration test and in-game timing have not been validated. The maintainer has compiled the Windows binaries (see below). This repository contains source, not compiled binaries. The changes remove identified sources of interference;
they do not prove zero overhead or fix every underlying Skyrim freeze.


## Validation of version 13

The Windows DLL and helper were built by the maintainer. Inspection confirmed the DLL exports plugin version 13, both binaries include the version 13 startup changes, the helper identifies protocol 13, and the DLL matches its PDB. Portable capture-policy and hotkey regression checks passed. In-game startup/manual-capture validation is still pending; the source change improves capture coverage and does not establish the cause of an underlying Skyrim hang.
