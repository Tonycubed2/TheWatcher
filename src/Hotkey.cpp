#include "PCH.h"
#include "Hotkey.h"
#include "Settings.h"
#include "Util.h"

namespace Hotkey
{
	namespace
	{
		struct Combo
		{
			UINT vk = 0;
			bool shift = false;
			bool ctrl = false;
			bool alt = false;
		};

		Combo                      g_combo;
		std::uint64_t              g_holdMs = 0;  // 0 = fire on press; otherwise the key must be held this long
		std::uint64_t              g_lastTriggerMs = 0;
		std::uint64_t              g_downSinceMs = 0;  // when the key was first seen down (0 = up)
		bool                       g_firedThisHold = false;
		int                        g_debugLines = 0;

		std::string LowerTrim(std::string a_s)
		{
			const auto b = a_s.find_first_not_of(" \t\r\n");
			if (b == std::string::npos) {
				return {};
			}
			const auto e = a_s.find_last_not_of(" \t\r\n");
			a_s = a_s.substr(b, e - b + 1);
			std::transform(a_s.begin(), a_s.end(), a_s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return a_s;
		}

		// Key name -> Windows virtual-key code. 0 = not recognised.
		UINT KeyCode(const std::string& a_name)
		{
			static const std::unordered_map<std::string, UINT> kNames{
				{ "printscreen", VK_SNAPSHOT }, { "prtsc", VK_SNAPSHOT }, { "print", VK_SNAPSHOT }, { "prtscn", VK_SNAPSHOT },
				{ "pause", VK_PAUSE }, { "break", VK_PAUSE }, { "scrolllock", VK_SCROLL },
				{ "insert", VK_INSERT }, { "ins", VK_INSERT }, { "delete", VK_DELETE }, { "del", VK_DELETE },
				{ "home", VK_HOME }, { "end", VK_END }, { "pageup", VK_PRIOR }, { "pgup", VK_PRIOR },
				{ "pagedown", VK_NEXT }, { "pgdn", VK_NEXT }, { "backspace", VK_BACK }, { "tab", VK_TAB },
				{ "enter", VK_RETURN }, { "space", VK_SPACE }, { "up", VK_UP }, { "down", VK_DOWN },
				{ "left", VK_LEFT }, { "right", VK_RIGHT },
				{ "numpad*", VK_MULTIPLY }, { "numpad+", VK_ADD }, { "numpad-", VK_SUBTRACT }, { "numpad/", VK_DIVIDE },
				{ "numpad.", VK_DECIMAL },
			};
			if (const auto it = kNames.find(a_name); it != kNames.end()) {
				return it->second;
			}
			if (a_name.size() >= 2 && a_name[0] == 'f') {  // F1-F24
				try {
					const int n = std::stoi(a_name.substr(1));
					if (n >= 1 && n <= 24) {
						return static_cast<UINT>(VK_F1 + n - 1);
					}
				} catch (...) {
				}
			}
			if (a_name.rfind("numpad", 0) == 0 && a_name.size() == 7 && std::isdigit(static_cast<unsigned char>(a_name[6]))) {
				return static_cast<UINT>(VK_NUMPAD0 + (a_name[6] - '0'));
			}
			if (a_name.size() == 1 && std::isalnum(static_cast<unsigned char>(a_name[0]))) {  // A-Z, 0-9
				return static_cast<UINT>(std::toupper(static_cast<unsigned char>(a_name[0])));
			}
			if (a_name.rfind("0x", 0) == 0) {  // raw virtual-key code, e.g. 0x2C
				try {
					const auto v = std::stoul(a_name.substr(2), nullptr, 16);
					if (v > 0 && v < 256) {
						return static_cast<UINT>(v);
					}
				} catch (...) {
				}
			}
			return 0;
		}

		// "F12", "Shift+PrintScreen", "Ctrl+Alt+F12" ...
		bool Parse(const std::string& a_text, Combo& a_out)
		{
			Combo       c;
			std::size_t pos = 0;
			bool        any = false;
			while (pos <= a_text.size()) {
				// '+' separates parts, except a trailing "numpad+" key
				auto next = a_text.find('+', pos);
				if (next != std::string::npos && LowerTrim(a_text.substr(pos, next - pos)) == "numpad") {
					next = a_text.find('+', next + 1);
				}
				const auto part = LowerTrim(a_text.substr(pos, next == std::string::npos ? std::string::npos : next - pos));
				pos = (next == std::string::npos) ? a_text.size() + 1 : next + 1;
				if (part.empty()) {
					continue;
				}
				any = true;
				if (part == "shift") {
					c.shift = true;
				} else if (part == "ctrl" || part == "control") {
					c.ctrl = true;
				} else if (part == "alt") {
					c.alt = true;
				} else {
					if (c.vk != 0) {
						return false;  // two non-modifier keys
					}
					c.vk = KeyCode(part);
					if (c.vk == 0) {
						return false;
					}
				}
			}
			if (!any || c.vk == 0) {
				return false;
			}
			a_out = c;
			return true;
		}

		bool GameInFront() { return Util::GameWindowInFront(); }

		bool KeyDown(int a_vk) { return (GetAsyncKeyState(a_vk) & 0x8000) != 0; }

		// Only the modifiers named in sHotkey are required; extra modifiers held at the same time do not block it
		bool ModifiersOk()
		{
			return (!g_combo.shift || KeyDown(VK_SHIFT)) && (!g_combo.ctrl || KeyDown(VK_CONTROL)) && (!g_combo.alt || KeyDown(VK_MENU));
		}

		bool Fire(std::uint64_t a_now)
		{
			if (g_lastTriggerMs != 0 && a_now - g_lastTriggerMs < 3000) {
				return false;  // one capture per 3 seconds at most
			}
			g_lastTriggerMs = a_now;
			return true;
		}
	}

	void Start()
	{
		const auto& cfg = Settings::Get();
		if (!cfg.hotkeyEnabled) {
			spdlog::info("Manual capture hotkey is off (bHotkeyEnabled=0)");
			return;
		}
		if (!Parse(cfg.hotkey, g_combo)) {
			spdlog::error("sHotkey={} is not a key combination The Watcher understands; the hotkey is off. Example: F12", cfg.hotkey);
			g_combo = Combo{};
			return;
		}
		g_holdMs = static_cast<std::uint64_t>(std::max(0.0f, cfg.hotkeyHoldSec) * 1000.0f);
		if (g_holdMs > 0) {
			spdlog::info("Manual capture hotkey: hold {} for {:.1f} seconds (works while the game is frozen)", cfg.hotkey, cfg.hotkeyHoldSec);
		} else {
			spdlog::info("Manual capture hotkey: press {} (works while the game is frozen)", cfg.hotkey);
		}
	}

	void GetBinding(DWORD& key, DWORD& modifiers)
	{
		key = g_combo.vk;
		modifiers = (g_combo.shift ? 1u : 0u) | (g_combo.ctrl ? 2u : 0u) | (g_combo.alt ? 4u : 0u);
	}

	// Called from the watchdog thread every 50 ms. The key is read with GetAsyncKeyState, the same way many SKSE
	// plugins read keys; version 5's first build used a low-level keyboard hook, which never saw keys in game.
	bool ConsumePressed()
	{
		if (g_combo.vk == 0) {
			return false;
		}
		const auto now = GetTickCount64();
		const bool down = KeyDown(static_cast<int>(g_combo.vk));
		if (!down) {
			g_downSinceMs = 0;
			g_firedThisHold = false;
			return false;
		}

		if (g_downSinceMs == 0) {  // just went down
			g_downSinceMs = now;
			g_firedThisHold = false;
			if (g_debugLines++ < 6) {
				spdlog::info("Hotkey: {} seen down (modifiers ok {}, game in front {})", Settings::Get().hotkey, ModifiersOk(), GameInFront());
			}
		}
		if (g_firedThisHold || now - g_downSinceMs < g_holdMs) {
			return false;
		}
		if (!ModifiersOk() || !Fire(now)) {
			return false;
		}
		g_firedThisHold = true;  // once per valid press / hold; focus never suppresses it
		if (g_holdMs > 0) {
			spdlog::info("Hotkey held for {:.1f}s: starting manual capture", static_cast<double>(g_holdMs) / 1000.0);
		}
		return true;
	}
}
