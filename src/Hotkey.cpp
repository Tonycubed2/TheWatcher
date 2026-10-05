#include "PCH.h"
#include "Hotkey.h"
#include "Settings.h"

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

		Combo                     g_combo;
		std::atomic<bool>         g_pressed{ false };
		std::atomic<std::uint64_t> g_lastTriggerMs{ 0 };
		bool                      g_downSeen = false;  // hook thread only

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

		// "Shift+PrintScreen", "Ctrl+Alt+F12", "F9" ...
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

		bool GameInFront()
		{
			const HWND fg = GetForegroundWindow();
			if (!fg) {
				return false;
			}
			DWORD pid = 0;
			GetWindowThreadProcessId(fg, &pid);
			return pid == GetCurrentProcessId();
		}

		bool ModifiersMatch()
		{
			const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
			const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
			const bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
			return shift == g_combo.shift && ctrl == g_combo.ctrl && alt == g_combo.alt;
		}

		void Trigger()
		{
			const auto now = GetTickCount64();
			if (now - g_lastTriggerMs.load() < 3000) {
				return;  // one capture per 3 seconds at most
			}
			g_lastTriggerMs.store(now);
			g_pressed.store(true);
		}

		LRESULT CALLBACK HookProc(int a_code, WPARAM a_wParam, LPARAM a_lParam)
		{
			if (a_code == HC_ACTION) {
				const auto* key = reinterpret_cast<const KBDLLHOOKSTRUCT*>(a_lParam);
				if (key && key->vkCode == g_combo.vk) {
					const bool down = (a_wParam == WM_KEYDOWN || a_wParam == WM_SYSKEYDOWN);
					if (down) {
						if (!g_downSeen) {
							g_downSeen = true;
							if (ModifiersMatch() && GameInFront()) {
								Trigger();
							}
						}
					} else {
						// Print Screen sometimes arrives as a key-up only; act on it then
						if (!g_downSeen && ModifiersMatch() && GameInFront()) {
							Trigger();
						}
						g_downSeen = false;
					}
				}
			}
			return CallNextHookEx(nullptr, a_code, a_wParam, a_lParam);
		}

		void HookThread()
		{
			const HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, HookProc, GetModuleHandleW(nullptr), 0);
			if (!hook) {
				spdlog::error("Manual capture hotkey could not be installed (error {}); the hotkey is off", GetLastError());
				return;
			}
			// A low-level hook needs a message loop on the thread that installed it
			MSG msg;
			while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
				TranslateMessage(&msg);
				DispatchMessageW(&msg);
			}
			UnhookWindowsHookEx(hook);
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
			spdlog::error("sHotkey={} is not a key combination The Watcher understands; the hotkey is off. Example: Shift+PrintScreen", cfg.hotkey);
			return;
		}
		std::thread(HookThread).detach();
		spdlog::info("Manual capture hotkey: {} (saves logs and a report on demand, works while the game is frozen)", cfg.hotkey);
	}

	bool ConsumePressed()
	{
		return g_pressed.exchange(false);
	}
}
