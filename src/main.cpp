#include "PCH.h"
#include "AddressLib.h"
#include "Capture.h"
#include "Events.h"
#include "Hotkey.h"
#include "Independent.h"
#include "Monitor.h"
#include "Settings.h"
#include "Util.h"
#include "Watchdog.h"

// Printed in the build output, so you can see whether CommonLib was built with SE (1.5.97) support.
// The authoritative check is the DLL's exports: SKSEPlugin_Query (SE) and SKSEPlugin_Version (AE).
#if defined(ENABLE_SKYRIM_SE)
#	pragma message("The Watcher: ENABLE_SKYRIM_SE is defined (SE 1.5.97 support compiled in)")
#else
#	pragma message("The Watcher: ENABLE_SKYRIM_SE is NOT defined - check the DLL exports for SKSEPlugin_Query")
#endif
#if defined(ENABLE_SKYRIM_AE)
#	pragma message("The Watcher: ENABLE_SKYRIM_AE is defined (AE 1.6.x and 1.7.x support compiled in)")
#endif
#if defined(ENABLE_SKYRIM_VR)
#	pragma message("The Watcher: ENABLE_SKYRIM_VR is defined (VR 1.4.15 support compiled in)")
#else
#	pragma message("The Watcher: ENABLE_SKYRIM_VR is NOT defined - Skyrim VR will not load this DLL")
#endif

namespace
{
	void SetupLog()
	{
		const auto dir = Util::LogDir();
		if (!dir) {
			return;
		}
		const auto path = *dir / "TheWatcher.log";
		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.string(), true);
		auto log = std::make_shared<spdlog::logger>("TheWatcher", std::move(sink));
		log->set_level(spdlog::level::info);
		log->flush_on(spdlog::level::info);  // flush every line: a hang must not lose buffered log text
		log->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%t] [%l] %v");
		spdlog::set_default_logger(std::move(log));
	}

	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		if (!a_msg) {
			return;
		}
		if (a_msg->type == SKSE::MessagingInterface::kDataLoaded) {
			static bool initialized = false;
			if (initialized) {
				spdlog::warn("Startup: duplicate DataLoaded ignored");
				return;
			}
			initialized = true;
			spdlog::info("Startup: DataLoaded received; initializing game-dependent monitoring");
			if (Settings::Get().eventsLog) {
				Events::Open();
				Events::Write(std::format("SESSION START  The Watcher version 13, game {}, pid {}", REL::Module::get().version().string(), GetCurrentProcessId()));
			}
			spdlog::info("Startup: registering event sinks");
			Monitor::RegisterEventSinks();
			spdlog::info("Startup: starting in-game watchdog");
			Watchdog::Start();
			Independent::GameReady();
			spdlog::info("Startup: game monitoring initialized; external automatic detection awaits a new frame or load heartbeat");
		} else {
			spdlog::info("Startup: SKSE message {} received", a_msg->type);
			Monitor::NoteGameMessage(a_msg->type);  // (1.1) save load / new game / save: tags loading screens + events log
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);
	SetupLog();

	const auto ver = REL::Module::get().version();
	const char* runtime = REL::Module::IsVR() ? "VR (1.4.x)" :
	                      REL::Module::IsAE() ? (ver.minor() >= 7 ? "AE (1.7.x)" : "AE (1.6.x)") :
	                      (REL::Module::IsSE() ? "SE (1.5.x)" : "unknown");
	spdlog::info("The Watcher version 13 startup fix, game {} - runtime {} (built with CommonLibSSE-NG 6.7.0)", ver.string(), runtime);

	// Plugins load on the game's main thread; the frame hook confirms this on its first call anyway
	Monitor::Get().mainThreadId.store(GetCurrentThreadId());
	Capture::Init();

	auto& cfg = Settings::Get();
	cfg.Load();
	cfg.LogValues();

	if (cfg.mode == 0) {
		spdlog::info("iMode=0: watchdog is off");
		return true;
	}

	// Only Win32/settings operations here; game singletons are not ready yet.
	spdlog::info("Startup: configuring manual capture before DataLoaded");
	Hotkey::Start();
	spdlog::info("Startup: launching independent helper in manual-only startup mode");
	Independent::Start();
	spdlog::info("Startup: registering SKSE message listener");
	if (const auto messaging = SKSE::GetMessagingInterface()) {
		if (messaging->RegisterListener(OnMessage)) {
			spdlog::info("Startup: SKSE message listener registered");
		} else {
			spdlog::error("Startup: SKSE listener registration FAILED; game monitoring cannot start; external manual capture remains available if the helper is running");
		}
	} else {
		spdlog::error("Startup: SKSE messaging interface missing; game monitoring cannot start; external manual capture remains available if the helper is running");
	}
	spdlog::info("Startup: loading Address Library");
	AddressLib::Load();
	AddressLib::SetLabelsEnabled(cfg.addressLibIDs);
	spdlog::info("Startup: installing frame hook");
	if (!Monitor::InstallFrameHook(cfg)) {
		spdlog::error("Continuing without frame heartbeat; only loading-screen stalls will be detected");
	}

	spdlog::info("Startup: plugin initialization complete; waiting for DataLoaded (external manual capture is independent of this event)");
	return true;
}
