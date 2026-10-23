#include "PCH.h"
#include "Monitor.h"
#include "Events.h"
#include "Settings.h"
#include "Independent.h"
#include "AddressLib.h"
#include "Util.h"

namespace Monitor
{
	namespace
	{
		State        g_state;
		std::mutex   g_nameLock;  // guards g_ctx and the pending load kind
		Context      g_ctx;
		std::int64_t g_hitchUs = 100'000;

		std::vector<std::string> g_openMenus;  // guarded by g_nameLock
		std::string              g_pendingLoadKind;
		std::int64_t             g_pendingLoadKindNs = 0;

		// Main-thread-only bookkeeping
		std::int64_t  g_lastInfoNs = 0;
		int           g_menuClosedChecks = 0;
		std::uint64_t g_seenLoadGen = 0;
		std::uint64_t g_seenFocusGen = 0;
		std::uint32_t g_lastLocationID = 0;

		void UpdateMax(std::atomic<std::uint64_t>& a_value, std::uint64_t a_candidate)
		{
			auto cur = a_value.load(std::memory_order_relaxed);
			while (a_candidate > cur && !a_value.compare_exchange_weak(cur, a_candidate, std::memory_order_relaxed)) {
			}
		}

		std::string FileOf(const RE::TESForm* a_form)
		{
			if (!a_form) {
				return {};
			}
			const auto file = a_form->GetFile(0);
			return file ? std::string(file->GetFilename()) : std::string();
		}

		std::string JoinMenus()
		{
			std::string out;
			for (const auto& m : g_openMenus) {
				if (!out.empty()) {
					out += "|";
				}
				out += m;
			}
			return out;
		}

		// ---------- (12, load breadcrumbs) what the game loaded last ----------
		// Every object load/unload, reference attach/detach and "cell fully loaded" event is kept in a small ring,
		// already resolved to IDs, plugin names and a display name (no game data is touched when it is reported,
		// so reading it is safe while the game is frozen). During a loading screen the events are also counted per
		// form, so a load that loops (the same references loading over and over) shows up as "most repeated".
		struct Crumb
		{
			std::int64_t  ns = 0;
			char          kind = 0;  // 'O' loaded, 'U' unloaded, 'A' attached, 'D' detached, 'C' cell fully loaded
			std::uint32_t formID = 0;
			std::uint32_t baseID = 0;
			char          type[8]{};        // base form type, e.g. STAT, NPC_, CELL
			char          name[48]{};       // display name (cells: editor ID when they have no name)
			char          plugin[64]{};     // plugin that added the form ("" for runtime-created references)
			char          lastPlugin[64]{}; // last plugin that edits it, when different
			char          basePlugin[64]{}; // plugin of the base object, when the reference has none
		};

		constexpr std::size_t kCrumbMax = 64;
		constexpr std::size_t kRepeatMax = 20'000;  // distinct forms counted per loading screen

		std::mutex                                       g_crumbLock;  // held only to copy a finished Crumb in or out
		std::array<Crumb, kCrumbMax>                     g_crumbs;
		std::size_t                                      g_crumbNext = 0;
		std::size_t                                      g_crumbCount = 0;
		std::unordered_map<std::uint32_t, std::uint32_t> g_loadRepeats;  // formID -> events during this loading screen
		std::unordered_map<std::uint32_t, Crumb>         g_repeatInfo;   // formID -> last event seen for it (names for the list)

		void CopyStr(char* a_dst, std::size_t a_size, std::string_view a_src)
		{
			const auto n = std::min(a_size - 1, a_src.size());
			std::memcpy(a_dst, a_src.data(), n);
			a_dst[n] = '\0';
		}

		void FillForm(Crumb& a_c, const RE::TESForm* a_form)
		{
			if (!a_form) {
				return;
			}
			a_c.formID = a_form->GetFormID();
			const RE::TESForm* base = a_form;
			if (const auto ref = a_form->AsReference()) {
				if (const auto b = ref->GetBaseObject()) {
					base = b;
				}
			}
			a_c.baseID = base->GetFormID();
			CopyStr(a_c.type, sizeof(a_c.type), RE::FormTypeToString(base->GetFormType()));

			const char* name = a_form->GetName();
			if (!name || !*name) {
				name = base->GetName();
			}
			if ((!name || !*name) && a_form->GetFormType() == RE::FormType::Cell) {
				name = a_form->GetFormEditorID();
			}
			CopyStr(a_c.name, sizeof(a_c.name), name ? name : "");

			const auto first = a_form->GetFile(0);
			const auto last = a_form->GetFile(-1);
			if (first) {
				CopyStr(a_c.plugin, sizeof(a_c.plugin), first->GetFilename());
			}
			if (last && last != first) {
				CopyStr(a_c.lastPlugin, sizeof(a_c.lastPlugin), last->GetFilename());
			}
			if (!first && base != a_form) {
				if (const auto bf = base->GetFile(0)) {
					CopyStr(a_c.basePlugin, sizeof(a_c.basePlugin), bf->GetFilename());
				}
			}
		}

		void RecordCrumb(char a_kind, const RE::TESForm* a_form, std::uint32_t a_formID)
		{
			if (Settings::Get().recentLoadEvents <= 0) {
				return;
			}
			Crumb c;
			c.ns = NowNs();
			c.kind = a_kind;
			c.formID = a_formID;
			FillForm(c, a_form);  // game data is read here, outside the lock
			const bool loading = g_state.loading.load(std::memory_order_relaxed);

			std::scoped_lock lock(g_crumbLock);
			g_crumbs[g_crumbNext] = c;
			g_crumbNext = (g_crumbNext + 1) % kCrumbMax;
			if (g_crumbCount < kCrumbMax) {
				++g_crumbCount;
			}
			if (loading && c.formID != 0) {
				auto it = g_loadRepeats.find(c.formID);
				if (it != g_loadRepeats.end()) {
					++it->second;
					g_repeatInfo[c.formID] = c;
				} else if (g_loadRepeats.size() < kRepeatMax) {
					g_loadRepeats.emplace(c.formID, 1u);
					g_repeatInfo[c.formID] = c;
				}
			}
		}

		void ResetLoadRepeats()
		{
			std::scoped_lock lock(g_crumbLock);
			g_loadRepeats.clear();
			g_repeatInfo.clear();
		}

		// Copies the newest a_max crumbs (oldest first). Never waits more than ~50 ms for the lock.
		bool SnapshotCrumbs(std::size_t a_max, std::vector<Crumb>& a_out)
		{
			std::unique_lock lock(g_crumbLock, std::defer_lock);
			for (int i = 0; i < 50 && !lock.try_lock(); ++i) {
				std::this_thread::sleep_for(1ms);
			}
			if (!lock.owns_lock()) {
				return false;
			}
			const auto n = std::min(a_max, g_crumbCount);
			a_out.clear();
			a_out.reserve(n);
			for (std::size_t i = 0; i < n; ++i) {
				a_out.push_back(g_crumbs[(g_crumbNext + kCrumbMax - n + i) % kCrumbMax]);
			}
			return true;
		}

		const char* CrumbKind(char a_kind)
		{
			switch (a_kind) {
			case 'O': return "LOADED  ";
			case 'U': return "UNLOADED";
			case 'A': return "ATTACHED";
			case 'D': return "DETACHED";
			case 'C': return "CELL READY";
			default:  return "?";
			}
		}

		const char* CrumbEvent(char a_kind)
		{
			switch (a_kind) {
			case 'O': return "loaded";
			case 'U': return "unloaded";
			case 'A': return "attached";
			case 'D': return "detached";
			case 'C': return "cell_ready";
			default:  return "unknown";
			}
		}

		std::string CrumbWhere(const Crumb& a_c)
		{
			if (!a_c.type[0]) {
				return "form no longer exists";
			}
			std::string where = a_c.plugin[0] ? std::string(a_c.plugin) :
			                    a_c.basePlugin[0] ? std::format("created in game, base from {}", a_c.basePlugin) :
			                                        std::string("created in game");
			if (a_c.lastPlugin[0]) {
				where += std::format(", last edited by {}", a_c.lastPlugin);
			}
			return where;
		}

		std::string CrumbLine(const Crumb& a_c, std::int64_t a_now)
		{
			const std::string name = a_c.name[0] ? std::format(" '{}'", a_c.name) : std::string();
			std::string       what;
			if (a_c.kind == 'C') {
				what = std::format("cell {:08X}{}", a_c.formID, name);
			} else if (a_c.baseID != 0 && a_c.baseID != a_c.formID) {
				what = std::format("ref {:08X} -> {} {:08X}{}", a_c.formID, a_c.type, a_c.baseID, name);
			} else if (a_c.type[0]) {
				what = std::format("{} {:08X}{}", a_c.type, a_c.formID, name);
			} else {
				what = std::format("form {:08X}", a_c.formID);
			}
			return std::format("{:>7.1f}s  {}  {}  [{}]", -static_cast<double>(a_now - a_c.ns) / 1e9, CrumbKind(a_c.kind), what, CrumbWhere(a_c));
		}

		// ---------- loading screens ----------

		void OnLoadStart()
		{
			Independent::LoadStart();
			const auto now = NowNs();
			g_state.loadStartNs.store(now);
			g_state.lastProgressNs.store(now);
			g_state.loadEvents.store(0);
			g_state.loadMaxGapUs.store(0);
			g_state.loadFrames.store(0);
			ResetLoadRepeats();
			g_state.loadGen.fetch_add(1);
			g_state.loading.store(true);

			std::string kind = "transition";
			std::string cellName;
			{
				std::scoped_lock lock(g_nameLock);
				// A save load / new game message may arrive just before the loading screen opens
				if (!g_pendingLoadKind.empty() && now - g_pendingLoadKindNs < 15'000'000'000) {
					kind = g_pendingLoadKind;
				}
				g_pendingLoadKind.clear();
				g_ctx.loadKind = kind;
				cellName = g_ctx.cellName;
			}
			const auto n = g_state.loadCount.load() + 1;
			spdlog::info("Loading screen opened (load #{}, {}), cell before load {:08X} '{}'", n, kind, g_state.cellFormID.load(), cellName);
			Events::Write(std::format("LOAD START #{} ({}) from cell {:08X} '{}'", n, kind, g_state.cellFormID.load(), cellName));
		}

		void OnLoadEnd(std::string_view a_how)
		{
			if (!g_state.loading.exchange(false)) {
				return;
			}
			const auto   now = NowNs();
			Independent::LoadEnd();
			const double secs = static_cast<double>(now - g_state.loadStartNs.load()) / 1e9;
			const auto   n = g_state.loadCount.fetch_add(1) + 1;
			g_state.loadEndNs.store(now);
			// Count the quiet stretch between the last load event and the loading screen closing
			if (const auto last = g_state.lastProgressNs.load(); last > 0 && now > last) {
				UpdateMax(g_state.loadMaxGapUs, static_cast<std::uint64_t>((now - last) / 1000));
			}
			g_state.loadGen.fetch_add(1);
			g_state.intervalLoads.fetch_add(1);
			UpdateMax(g_state.intervalLongestLoadMs, static_cast<std::uint64_t>(secs * 1000.0));

			std::string kind;
			{
				std::scoped_lock lock(g_nameLock);
				kind = g_ctx.loadKind;
			}
			spdlog::info("Load #{} finished in {:.2f}s ({}, {}): {} load events, longest gap {:.0f} ms, {} frames during load",
				n, secs, kind, a_how, g_state.loadEvents.load(), g_state.loadMaxGapUs.load() / 1000.0, g_state.loadFrames.load());
			Events::Write(std::format("LOAD END   #{} ({}) {:.2f}s, {} load events, longest gap {:.0f} ms",
				n, kind, secs, g_state.loadEvents.load(), g_state.loadMaxGapUs.load() / 1000.0));
		}

		void OnLoadProgress()
		{
			g_state.intervalLoadEvents.fetch_add(1, std::memory_order_relaxed);
			if (!g_state.loading.load(std::memory_order_relaxed)) {
				return;
			}
			const auto now = NowNs();
			Independent::LoadProgress();
			const auto prev = g_state.lastProgressNs.exchange(now);
			if (prev > 0 && now > prev) {
				UpdateMax(g_state.loadMaxGapUs, static_cast<std::uint64_t>((now - prev) / 1000));
			}
			g_state.loadEvents.fetch_add(1, std::memory_order_relaxed);
		}

		// ---------- main thread, once a second ----------

		// Top N "plugin:count" entries, most first
		std::string TopPlugins(const std::unordered_map<std::string, std::uint32_t>& a_tally, std::size_t a_n)
		{
			std::vector<std::pair<std::string, std::uint32_t>> v(a_tally.begin(), a_tally.end());
			std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
			std::string out;
			for (std::size_t i = 0; i < v.size() && i < a_n; ++i) {
				if (!out.empty()) {
					out += " ";
				}
				out += std::format("{}:{}", v[i].first.empty() ? "(generated)" : v[i].first, v[i].second);
			}
			return out;
		}

		// Which plugin the actor "comes from": its base NPC's plugin, looking through runtime-generated bases
		// (leveled spawns get an FF-prefixed temporary base) to the original leveled base when needed.
		std::string ActorPlugin(RE::Actor* a_actor)
		{
			auto base = a_actor->GetActorBase();
			if (base && (base->GetFormID() >> 24) != 0xFF) {
				return FileOf(base);
			}
			if (const auto lvl = a_actor->extraList.GetByType<RE::ExtraLeveledCreature>(); lvl && lvl->originalBase) {
				return FileOf(lvl->originalBase);
			}
			return {};
		}

		void UpdateActors(RE::ProcessLists* a_lists)
		{
			std::unordered_map<std::string, std::uint32_t> all;
			std::unordered_map<std::string, std::uint32_t> spawned;
			std::uint32_t                                   ff = 0;
			for (auto& handle : a_lists->highActorHandles) {
				const auto actor = handle.get();
				if (!actor) {
					continue;
				}
				auto plugin = ActorPlugin(actor.get());
				if ((actor->GetFormID() >> 24) == 0xFF) {
					++ff;
					++spawned[plugin];
				}
				++all[std::move(plugin)];
			}
			g_state.spawnedActors.store(ff);
			auto top = TopPlugins(all, 4);
			auto topSpawned = TopPlugins(spawned, 4);
			std::scoped_lock lock(g_nameLock);
			g_ctx.actorPlugins = std::move(top);
			g_ctx.spawnPlugins = std::move(topSpawned);
		}

		void UpdateGameInfo()
		{
			const auto& cfg = Settings::Get();
			if (const auto player = RE::PlayerCharacter::GetSingleton()) {
				if (const auto cell = player->GetParentCell()) {
					const auto id = cell->GetFormID();
					const bool interior = cell->IsInteriorCell();
					const auto world = player->GetWorldspace();
					const auto pos = player->GetPosition();
					g_state.posX.store(pos.x);
					g_state.posY.store(pos.y);
					g_state.posZ.store(pos.z);

					std::int32_t gx = 0;
					std::int32_t gy = 0;
					if (!interior) {
						if (const auto ext = cell->GetCoordinates()) {
							gx = ext->cellX;
							gy = ext->cellY;
						}
					}

					const auto loc = player->GetCurrentLocation();
					const auto locID = loc ? loc->GetFormID() : 0;

					if (id != g_state.cellFormID.load() || locID != g_lastLocationID) {
						const char* name = cell->GetName();
						const char* worldEdid = world ? world->GetFormEditorID() : nullptr;
						const char* locName = loc ? loc->GetName() : nullptr;
						std::string line;
						{
							std::scoped_lock lock(g_nameLock);
							g_ctx.cellName = name ? name : "";
							g_ctx.worldspace = interior ? "" : (worldEdid ? worldEdid : "");
							g_ctx.location = locName ? locName : "";
							line = interior ?
							           std::format("CELL {:08X} '{}' (interior) location '{}'", id, g_ctx.cellName, g_ctx.location) :
							           std::format("CELL {:08X} '{}' {} ({},{}) location '{}'", id, g_ctx.cellName, g_ctx.worldspace, gx, gy, g_ctx.location);
						}
						Events::Write(line);
						g_lastLocationID = locID;
					}
					g_state.cellFormID.store(id);
					g_state.cellInterior.store(interior);
					g_state.worldFormID.store(world && !interior ? world->GetFormID() : 0);
					g_state.gridX.store(gx);
					g_state.gridY.store(gy);
				}
			}
			if (const auto lists = RE::ProcessLists::GetSingleton()) {
				g_state.highActors.store(static_cast<std::uint32_t>(lists->highActorHandles.size()));
				g_state.midHighActors.store(static_cast<std::uint32_t>(lists->middleHighActorHandles.size()));
				g_state.lowActors.store(static_cast<std::uint32_t>(lists->lowActorHandles.size()));
				if (cfg.actorBreakdown) {
					UpdateActors(lists);
				}
			}
		}

		// Safety net: if the "closed" event is ever missed, don't stay in loading mode forever
		void ReconcileLoading()
		{
			const auto ui = RE::UI::GetSingleton();
			if (!ui) {
				return;
			}
			if (ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME)) {
				g_menuClosedChecks = 0;
				return;
			}
			if (++g_menuClosedChecks >= 3) {
				g_menuClosedChecks = 0;
				OnLoadEnd("menu no longer open, close event missed");
			}
		}

		void OnFrame()
		{
			Independent::Frame();
			static bool threadRecorded = false;
			if (!threadRecorded) {
				threadRecorded = true;
				const auto tid = static_cast<std::uint32_t>(GetCurrentThreadId());
				if (g_state.mainThreadId.exchange(tid) != tid) {
					spdlog::info("Main thread id set from frame hook: {}", tid);
				}
			}

			const auto now = NowNs();
			const auto prev = g_state.lastFrameNs.exchange(now);
			const auto gen = g_state.loadGen.load(std::memory_order_relaxed);
			const bool loading = g_state.loading.load(std::memory_order_relaxed);
			const auto focusGen = g_state.focusGen.load(std::memory_order_relaxed);

			if (loading) {
				g_state.loadFrames.fetch_add(1, std::memory_order_relaxed);
			} else if (prev > 0 && gen == g_seenLoadGen && focusGen == g_seenFocusGen) {
				// Only time frames that did not straddle a loading screen or an alt-tab
				const auto dtUs = static_cast<std::uint64_t>((now - prev) / 1000);
				g_state.lastFrameDtUs.store(dtUs, std::memory_order_relaxed);
				g_state.intervalFrames.fetch_add(1, std::memory_order_relaxed);
				g_state.intervalFrameUsSum.fetch_add(dtUs, std::memory_order_relaxed);
				UpdateMax(g_state.intervalFrameUsMax, dtUs);
				if (static_cast<std::int64_t>(dtUs) >= g_hitchUs) {
					g_state.intervalHitches.fetch_add(1, std::memory_order_relaxed);
				}
			}
			g_seenLoadGen = gen;
			g_seenFocusGen = focusGen;

			if (now - g_lastInfoNs >= 1'000'000'000) {
				g_lastInfoNs = now;
				if (loading) {
					ReconcileLoading();
				} else {
					g_menuClosedChecks = 0;
					UpdateGameInfo();
				}
			}
		}

		// Chains the same call site hdtsmp64 hooks (SkyrimSE.exe 36544+0x160 on AE, visible in your crash logs)
		struct MainUpdateHook
		{
			static void thunk(RE::Main* a_main, float a_arg)
			{
				OnFrame();
				func(a_main, a_arg);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// ---------- event sinks ----------

		class MenuSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			static MenuSink* Get()
			{
				static MenuSink instance;
				return &instance;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (!a_event) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const std::string name(a_event->menuName.c_str() ? a_event->menuName.c_str() : "");
				{
					std::scoped_lock lock(g_nameLock);
					const auto it = std::find(g_openMenus.begin(), g_openMenus.end(), name);
					if (a_event->opening && it == g_openMenus.end()) {
						g_openMenus.push_back(name);
					} else if (!a_event->opening && it != g_openMenus.end()) {
						g_openMenus.erase(it);
					}
					g_ctx.openMenus = JoinMenus();
				}
				// Cursor and Fader toggle constantly and add nothing
				if (name != "Cursor Menu" && name != "Fader Menu") {
					Events::Write(std::format("MENU {} {}", a_event->opening ? "open " : "close", name));
				}

				if (a_event->menuName == RE::LoadingMenu::MENU_NAME) {
					if (a_event->opening) {
						OnLoadStart();
					} else {
						OnLoadEnd("loading menu closed");
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		class LoadSink final :
			public RE::BSTEventSink<RE::TESObjectLoadedEvent>,
			public RE::BSTEventSink<RE::TESCellAttachDetachEvent>,
			public RE::BSTEventSink<RE::TESCellFullyLoadedEvent>
		{
		public:
			static LoadSink* Get()
			{
				static LoadSink instance;
				return &instance;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESObjectLoadedEvent* a_event, RE::BSTEventSource<RE::TESObjectLoadedEvent>*) override
			{
				OnLoadProgress();
				if (a_event) {
					RecordCrumb(a_event->loaded ? 'O' : 'U', RE::TESForm::LookupByID(a_event->formID), a_event->formID);
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESCellAttachDetachEvent* a_event, RE::BSTEventSource<RE::TESCellAttachDetachEvent>*) override
			{
				OnLoadProgress();
				if (a_event && a_event->reference) {
					const auto ref = a_event->reference.get();
					RecordCrumb(a_event->attached ? 'A' : 'D', ref, ref->GetFormID());
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESCellFullyLoadedEvent* a_event, RE::BSTEventSource<RE::TESCellFullyLoadedEvent>*) override
			{
				OnLoadProgress();
				if (a_event && a_event->cell) {
					RecordCrumb('C', a_event->cell, a_event->cell->GetFormID());
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		// (1.1) Equip / unequip breadcrumbs (player only unless bEquipAllActors=1) and fast-travel tagging
		class GameSink final :
			public RE::BSTEventSink<RE::TESEquipEvent>,
			public RE::BSTEventSink<RE::TESFastTravelEndEvent>
		{
		public:
			static GameSink* Get()
			{
				static GameSink instance;
				return &instance;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESEquipEvent* a_event, RE::BSTEventSource<RE::TESEquipEvent>*) override
			{
				// Save loading generates many equip events. Keep load/menu breadcrumbs, skip noisy item logging.
				if (g_state.loading.load(std::memory_order_relaxed) && Settings::Get().reduceWorkDuringLoading)
					return RE::BSEventNotifyControl::kContinue;
				if (!a_event || !a_event->actor) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const auto actor = a_event->actor.get();
				const bool isPlayer = actor == RE::PlayerCharacter::GetSingleton();
				if (!isPlayer && !Settings::Get().equipAllActors) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const auto  item = RE::TESForm::LookupByID(a_event->baseObject);
				const char* itemName = item ? item->GetName() : nullptr;
				const char* who = isPlayer ? "player" : actor->GetName();
				Events::Write(std::format("{} {} [{:08X}] '{}' [{:08X} {}]", a_event->equipped ? "EQUIP  " : "UNEQUIP",
					who ? who : "", actor->GetFormID(), itemName ? itemName : "", a_event->baseObject, FileOf(item)));
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESFastTravelEndEvent*, RE::BSTEventSource<RE::TESFastTravelEndEvent>*) override
			{
				const auto now = NowNs();
				const bool loading = g_state.loading.load();
				const bool justLoaded = now - g_state.loadEndNs.load() < 10'000'000'000;
				{
					std::scoped_lock lock(g_nameLock);
					if (loading || justLoaded) {
						g_ctx.loadKind = "fast travel";
					}
				}
				const auto n = g_state.loadCount.load() + (loading ? 1 : 0);
				Events::Write(std::format("FAST TRAVEL ended{}", loading || justLoaded ? std::format(" (load #{} was a fast travel)", n) : ""));
				if (!loading && justLoaded) {
					spdlog::info("Load #{} was a fast travel", n);
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	State& Get() { return g_state; }

	std::int64_t NowNs()
	{
		return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	std::string CellName()
	{
		std::scoped_lock lock(g_nameLock);
		return g_ctx.cellName;
	}

	Context GetContext()
	{
		// Diagnostic readers must not wait for a lock held by a frozen game thread.
        thread_local Context cached;
        std::unique_lock lock(g_nameLock, std::try_to_lock);
        if (lock.owns_lock()) cached = g_ctx;
        return cached;
	}

	namespace
	{
		// SEH-guarded: nothing here needs unwinding, so __try is allowed
		bool ReadVM(RE::BSScript::Internal::VirtualMachine* a_vm, long long (&a_out)[4])
		{
			__try {
				a_out[0] = static_cast<long long>(a_vm->allRunningStacks.size());
				a_out[1] = static_cast<long long>(a_vm->waitingLatentReturns.size());
				a_out[2] = static_cast<long long>(a_vm->uiWaitingFunctionMessages);
				a_out[3] = a_vm->overstressed ? 1 : 0;
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}
	}

	PapyrusStats ReadPapyrus()
	{
		PapyrusStats out;
		const auto   vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
		if (!vm) {
			return out;
		}
		long long v[4]{};
		if (!ReadVM(vm, v)) {
			return out;
		}
		// A value outside these bounds means the read raced a rehash (or the layout is off): report it as unknown
		auto sane = [](long long a_v) { return a_v >= 0 && a_v < 10'000'000 ? a_v : -1; };
		out.runningStacks = sane(v[0]);
		out.latentWaits = sane(v[1]);
		out.queuedMessages = sane(v[2]);
		out.overstressed = static_cast<int>(v[3]);
		return out;
	}

	std::string RecentLoadsText(std::size_t a_max)
	{
		std::vector<Crumb> v;
		if (a_max == 0 || !SnapshotCrumbs(a_max, v)) {
			return a_max == 0 ? std::string() : std::string("  (busy; not read)\n");
		}
		if (v.empty()) {
			return "  (none recorded yet)\n";
		}
		const auto  now = NowNs();
		std::string out;
		for (const auto& c : v) {
			out += "  " + CrumbLine(c, now) + "\n";
		}
		return out;
	}

	std::string RecentLoadsJson(std::size_t a_max)
	{
		std::vector<Crumb> v;
		if (a_max == 0 || !SnapshotCrumbs(a_max, v) || v.empty()) {
			return "[]";
		}
		const auto  now = NowNs();
		std::string out = "[";
		for (std::size_t i = 0; i < v.size(); ++i) {
			const auto& c = v[i];
			out += std::format("{}{{\"seconds_ago\":{:.1f},\"event\":{},\"form\":\"{:08X}\",\"base\":\"{:08X}\",\"type\":{},\"name\":{},"
							   "\"plugin\":{},\"last_plugin\":{},\"base_plugin\":{}}}",
				i ? "," : "", static_cast<double>(now - c.ns) / 1e9, Util::Json(CrumbEvent(c.kind)),
				c.formID, c.baseID, Util::Json(c.type), Util::Json(c.name), Util::Json(c.plugin), Util::Json(c.lastPlugin), Util::Json(c.basePlugin));
		}
		out += "]";
		return out;
	}

	std::string LoadRepeatsText(std::size_t a_top, std::uint32_t a_minCount)
	{
		std::vector<std::pair<std::uint32_t, Crumb>> top;
		std::size_t distinct = 0;
		{
			std::unique_lock lock(g_crumbLock, std::defer_lock);
			for (int i = 0; i < 50 && !lock.try_lock(); ++i) {
				std::this_thread::sleep_for(1ms);
			}
			if (!lock.owns_lock()) {
				return "  (busy; not read)\n";
			}
			distinct = g_loadRepeats.size();
			for (const auto& [id, count] : g_loadRepeats) {
				if (count >= a_minCount) {
					const auto it = g_repeatInfo.find(id);
					top.emplace_back(count, it != g_repeatInfo.end() ? it->second : Crumb{});
				}
			}
		}
		if (top.empty()) {
			return std::format("  none: no form had {} or more load events ({} distinct forms seen)\n", a_minCount, distinct);
		}
		std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
		if (top.size() > a_top) {
			top.resize(a_top);
		}
		const auto  now = NowNs();
		std::string out;
		for (const auto& [count, c] : top) {
			out += std::format("  x{:<5} {}\n", count, CrumbLine(c, now));
		}
		return out;
	}

	std::string ContextLine()
	{
		const auto c = GetContext();
		const auto& s = g_state;
		std::string out;
		if (s.cellInterior.load()) {
			out = std::format("cell {:08X} '{}' (interior)", s.cellFormID.load(), c.cellName);
		} else {
			out = std::format("cell {:08X} '{}' {} ({},{}) pos ({:.0f}, {:.0f}, {:.0f})", s.cellFormID.load(), c.cellName, c.worldspace,
				s.gridX.load(), s.gridY.load(), s.posX.load(), s.posY.load(), s.posZ.load());
		}
		out += std::format(" | location '{}' | menus [{}] | last load kind '{}'", c.location, c.openMenus, c.loadKind);
		out += std::format(" | actors high {} (spawned {}) mid-high {} low {}", s.highActors.load(), s.spawnedActors.load(),
			s.midHighActors.load(), s.lowActors.load());
		if (!c.actorPlugins.empty()) {
			out += std::format(" | by plugin: {}", c.actorPlugins);
		}
		if (!c.spawnPlugins.empty()) {
			out += std::format(" | spawned by base plugin: {}", c.spawnPlugins);
		}
		if (Settings::Get().papyrusStats) {
			const auto p = ReadPapyrus();
			out += std::format(" | papyrus stacks {} latent {} queued {} overstressed {}", p.runningStacks, p.latentWaits,
				p.queuedMessages, p.overstressed);
		}
		return out;
	}

	void NoteGameMessage(std::uint32_t a_type)
	{
		const char* kind = nullptr;
		const char* what = nullptr;
		switch (a_type) {
		case SKSE::MessagingInterface::kPreLoadGame:  kind = "save load"; what = "SAVE LOAD requested"; break;
		case SKSE::MessagingInterface::kPostLoadGame: what = "SAVE LOAD done"; break;
		case SKSE::MessagingInterface::kNewGame:      kind = "new game"; what = "NEW GAME"; break;
		case SKSE::MessagingInterface::kSaveGame:     what = "GAME SAVED"; break;
		default: return;
		}
		if (kind) {
			std::scoped_lock lock(g_nameLock);
			if (g_state.loading.load()) {
				g_ctx.loadKind = kind;  // the loading screen opened first
			} else {
				g_pendingLoadKind = kind;
				g_pendingLoadKindNs = NowNs();
			}
		}
		Events::Write(what);
	}

	namespace
	{
		// Target of the rel32 CALL at a_site, or 0 if the byte there is not E8
		std::uintptr_t CallTarget(std::uintptr_t a_site)
		{
			if (*reinterpret_cast<const std::uint8_t*>(a_site) != 0xE8) {
				return 0;
			}
			const auto rel = *reinterpret_cast<const std::int32_t*>(a_site + 1);
			return a_site + 5 + static_cast<std::intptr_t>(rel);
		}

		std::size_t GameImageSize(std::uintptr_t a_base)
		{
			const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(a_base);
			const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(a_base + static_cast<std::uintptr_t>(dos->e_lfanew));
			return nt->OptionalHeader.SizeOfImage;
		}

		std::string ModuleNameAt(std::uintptr_t a_addr)
		{
			HMODULE mod = nullptr;
			if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCWSTR>(a_addr), &mod) && mod) {
				char path[MAX_PATH]{};
				if (GetModuleFileNameA(mod, path, MAX_PATH)) {
					return std::filesystem::path(path).filename().string();
				}
			}
			return "unknown (likely another plugin's trampoline)";
		}
	}

	// The frame heartbeat chains the call from Main loop (ID 35551 SE / 36544 AE) into Main::Update (35565 / 36564),
	// at +0x11F (SE) / +0x160 (AE 1.6.x and 1.7.104). The site is checked before anything is written:
	//   - CALL into Main::Update                      -> hook it
	//   - CALL into another module (hdtsmp64 and others hook the same site and load first) -> chain behind it
	//   - CALL to anything else inside SkyrimSE.exe   -> the function changed: search it for the CALL into Main::Update
	// On 1.7.x and later a site that cannot be confirmed is left alone (frame detection off, the game is untouched).
	// VR uses the SE IDs through its own Address Library (.csv), which AddressLib does not read: VR keeps the
	// same unverified install at 35551+0x11F it always had.
	bool InstallFrameHook(const Settings& a_settings)
	{
		g_hitchUs = static_cast<std::int64_t>(a_settings.hitchMs * 1000.0f);

		const bool          ae = REL::Module::IsAE();
		const auto          ver = REL::Module::get().version();
		const bool          strict = ae && ver.minor() >= 7;  // newer than anything this build was first written for
		const std::uint64_t parentID = ae ? 36544 : 35551;
		const std::uint64_t calleeID = ae ? 36564 : 35565;
		const std::uintptr_t knownOffset = ae ? 0x160 : 0x11F;

		const auto base = REL::Module::get().base();
		const auto imageSize = GameImageSize(base);

		std::uintptr_t parentOff = 0;
		if (!AddressLib::IdToOffset(parentID, parentOff)) {
			if (strict) {
				spdlog::error("Frame hook: Address Library has no ID {} for game {}; frame-stall detection disabled", parentID, ver.string());
				return false;
			}
			// Older runtimes: same behaviour as before (CommonLib's own lookup)
			parentOff = REL::ID(parentID).offset();
		}
		std::uintptr_t calleeOff = 0;
		const bool     haveCallee = AddressLib::IdToOffset(calleeID, calleeOff);
		const auto     parent = base + parentOff;
		const auto     callee = haveCallee ? base + calleeOff : 0;

		auto inGame = [&](std::uintptr_t a) { return a >= base && a < base + imageSize; };

		std::uintptr_t site = parent + knownOffset;
		const auto     target = CallTarget(site);
		std::string    how;

		if (target != 0 && haveCallee && target == callee) {
			how = "call into Main::Update confirmed";
		} else if (target != 0 && !inGame(target)) {
			how = std::format("chained behind {}", ModuleNameAt(target));
		} else {
			// Not where it used to be (or not confirmable): look for the CALL into Main::Update in the first 0x800 bytes
			std::uintptr_t found = 0;
			int            hits = 0;
			if (haveCallee) {
				for (std::uintptr_t o = 0; o < 0x800; ++o) {
					if (CallTarget(parent + o) == callee) {
						++hits;
						if (!found) {
							found = parent + o;
						}
					}
				}
			}
			if (hits == 1) {
				site = found;
				how = std::format("call into Main::Update found at +0x{:X} (expected +0x{:X})", site - parent, knownOffset);
			} else if (!strict && target != 0) {
				// SE / AE 1.6.x: keep the behaviour these versions always had, but say so
				how = haveCallee ? std::format("unconfirmed: call goes to SkyrimSE.exe+{:X}, not ID {}", target - base, calleeID) :
				                   std::format("unconfirmed: ID {} not in Address Library", calleeID);
			} else {
				spdlog::error("Frame hook: no confirmed call into Main::Update near ID {} (byte at +0x{:X} is {:02X}, {} matches); "
				              "frame-stall detection disabled, game code left untouched",
					parentID, knownOffset, *reinterpret_cast<const std::uint8_t*>(parent + knownOffset), hits);
				return false;
			}
		}

		SKSE::AllocTrampoline(14);
		MainUpdateHook::func = SKSE::GetTrampoline().write_call<5>(site, MainUpdateHook::thunk);
		spdlog::info("Frame hook installed at SkyrimSE.exe+{:X} (ID {}+0x{:X}, {} {}): {}", site - base, parentID, site - parent,
			ae ? "AE" : (REL::Module::IsVR() ? "VR" : "SE"), ver.string(), how);
		return true;
	}

	// (version 4) some event sources do not exist on every runtime. On Skyrim VR 1.4.15 the fast-travel-end source is
	// missing, CommonLib returns a null source, and registering on it crashed the game at startup. Every
	// script-event sink now checks its source first and is skipped (with a log line) if the runtime lacks it.
	template <class Event, class Sink>
	void AddScriptSink(RE::ScriptEventSourceHolder* a_holder, Sink* a_sink, std::string_view a_name)
	{
		if (auto* source = a_holder->GetEventSource<Event>()) {
			source->AddEventSink(static_cast<RE::BSTEventSink<Event>*>(a_sink));
		} else {
			spdlog::warn("The {} event is not available on this game version; that feature is skipped", a_name);
		}
	}

	void RegisterEventSinks()
	{
		if (const auto ui = RE::UI::GetSingleton()) {
			ui->AddEventSink<RE::MenuOpenCloseEvent>(MenuSink::Get());
		} else {
			spdlog::error("UI singleton missing; loading screens will not be tracked");
		}

		if (const auto holder = RE::ScriptEventSourceHolder::GetSingleton()) {
			AddScriptSink<RE::TESObjectLoadedEvent>(holder, LoadSink::Get(), "object loaded");
			AddScriptSink<RE::TESCellAttachDetachEvent>(holder, LoadSink::Get(), "cell attach/detach");
			AddScriptSink<RE::TESCellFullyLoadedEvent>(holder, LoadSink::Get(), "cell fully loaded");
			if (Settings::Get().eventsLog) {
				AddScriptSink<RE::TESFastTravelEndEvent>(holder, GameSink::Get(), "fast travel end");
				if (Settings::Get().equipEvents) {
					AddScriptSink<RE::TESEquipEvent>(holder, GameSink::Get(), "equip");
				}
			}
		} else {
			spdlog::error("ScriptEventSourceHolder missing; load progress will not be tracked");
		}
		spdlog::info("Event sinks registered");
	}
}
