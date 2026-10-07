# The Watcher — version 10

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
in SKSE/Plugins. Both DLL and EXE must be version 10. The helper launches automatically.
Use the new INI to apply the updated defaults; old INI overrides may retain heavier settings.

## Validation and limits

Portable C++ regression checks cover eight-second gameplay detection, a six-minute load
without automatic captures, explicit loading opt-in, loading/gameplay transitions,
59-second capture overlap suppression and recovery/rearming. Separate tests cover zeroed
and truncated dump metadata/payloads, retention ordering, protected files, disabled caps,
partial cleanup and symlink exclusion. INI and source/build protocol checks are included.

Windows/MSVC compilation and in-game timing were not available here. This package is
source, not compiled binaries. The changes remove identified sources of interference;
they do not prove zero overhead or fix every underlying Skyrim freeze.
