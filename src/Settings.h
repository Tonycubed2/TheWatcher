#pragma once

struct Settings
{
	// [General]
	int   mode = 2;  // 0 off, 1 normal, 2 aggressive
	bool  statsLog = true;
	int   statsIntervalSec = 5;
	float hitchMs = 100.0f;
	int   keepStatsFiles = 20;
	int   keepStallCaptures = 10;
	int   maxStorageMB = 1024; // TheWatcher subfolder only; 0 disables byte retention

	// [Detection]
	float frameStallSec = 8.0f;
	float loadWarnSec = 60.0f;
	float loadStallSec = 300.0f;
	bool  captureLoadingStalls = false;
	bool  reduceWorkDuringLoading = true;
	float recaptureSec = 30.0f;
	int   maxCaptures = 20;
	bool  ignoreUnfocused = false; // legacy key is ignored in version 10
	bool  independentWatchdog = true;
	float independentFrameSec = 8.0f;

	// [Capture]
	bool mainThreadStack = true;
	bool allThreadStacks = false;
	int  minidumpLevel = 0;
	bool allowFullMemoryDump = false;
	bool outOfProcessDump = true;  // write the dump with TheWatcherDump.exe
	bool backupLogs = true;
	bool beep = true;
	bool flashWindow = true;
	bool alertOnWarning = true;
	bool addressLibIDs = true;   // label SkyrimSE.exe frames with Address Library IDs
	int  threadSamples = 1;      // how many times to sample every thread per capture (1 = no sampling)
	int  sampleIntervalMs = 100; // time between samples
	bool groupThreadStacks = true;   // (1.1) print identical thread stacks once, with a thread count
	bool skipDumpIfRecovered = true; // (1.1) no minidump / all-thread pass when the game was already running again
	bool waitChains = true;          // (version 6) Windows wait chain analysis: who is waiting for whom

	// [Sampling]  (1.1) main-thread stack history during hitches and slow loading gaps
	bool  slowSampling = false;
	float hitchSampleSec = 0.5f;     // start sampling once the current frame has taken this long
	float hitchReportSec = 1.0f;     // write a hitch report when the hitch lasted at least this long
	float loadGapSampleSec = 2.0f;   // during a loading screen: start sampling after this long with no load progress
	int   slowSampleIntervalMs = 100;
	int   maxSlowReports = 1000;       // per session

	// [Context]  (1.1) extra data in the stats CSV, events log and live status file
	bool actorBreakdown = true;
	bool papyrusStats = true;
	bool vramStats = true;
	bool statusFile = true;
	bool eventsLog = true;
	bool equipEvents = true;
	bool equipAllActors = false;
	int  keepEventFiles = 20;

	// [Hotkey]  (version 5) save logs and a report on demand
	bool        hotkeyEnabled = true;
	std::string hotkey = "F12";
	float       hotkeyHoldSec = 0.0f;  // hold the key this long to capture (0 = capture on press)
	bool        hotkeyMinidump = true;
	int         keepManualCaptures = 10;

	// [Resources]  (version 5) which mods are using CPU
	bool resourceProfiler = true;
	int  resourceIntervalMs = 500;
	int  resourceWindowSec = 60;
	int  resourceTop = 50;
	int  resourceSampleThreads = 0;
	bool resourceLog = true;

	[[nodiscard]] bool StatsEnabled() const { return mode == 2 || statsLog; }

	static Settings& Get();
	void Load();
	void LogValues() const;
};
