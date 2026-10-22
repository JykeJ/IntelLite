// IntelLite's panel hotkey (see hotkey.h).
//
// No input sink lives here: the host owns the toggle. panel.cpp reads the
// stored DirectInput scancode (Scancode()) and binds it through Magelight's
// BindHotkey, which fires outside engine menus and, while the page has key
// focus, only for the view's own key (the close case). This module owns the
// setting: the atomic scancode, the name <-> scancode mapping and the
// settings.json round-trip. The binding is set ONLY by editing settings.json
// (written at first run, read at startup); there is no in-game capture. The
// default is Right Alt, and the panel displays the current key.

#include "hotkey.h"

#include "fuzzy.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

namespace intellite::hotkey
{
	namespace
	{
		// DirectInput scancodes (DIK_*), the id Magelight's BindHotkey takes.
		constexpr std::uint32_t kDefaultScancode = 0xB8;  // DIK_RALT (Right Alt)

		std::atomic<std::uint32_t> g_scancode{ kDefaultScancode };
		std::mutex                 g_settingsMutex;

		// <exeDir>/Data/SKSE/Plugins/IntelLite/settings.json. Under MO2 a write
		// here is redirected into the overwrite folder (same pattern as the
		// blacklist file).
		std::filesystem::path SettingsPath()
		{
			return fuzzy::ExeDir()
				/ L"Data" / L"SKSE" / L"Plugins" / L"IntelLite" / L"settings.json";
		}

		// Human-readable name for common DirectInput scancodes; "0xNN" otherwise.
		std::string ScancodeName(std::uint32_t a_code)
		{
			// Letters and digits first (they are dense and common).
			switch (a_code) {
			case 0x02: return "1"; case 0x03: return "2"; case 0x04: return "3";
			case 0x05: return "4"; case 0x06: return "5"; case 0x07: return "6";
			case 0x08: return "7"; case 0x09: return "8"; case 0x0A: return "9";
			case 0x0B: return "0";
			case 0x10: return "Q"; case 0x11: return "W"; case 0x12: return "E";
			case 0x13: return "R"; case 0x14: return "T"; case 0x15: return "Y";
			case 0x16: return "U"; case 0x17: return "I"; case 0x18: return "O";
			case 0x19: return "P";
			case 0x1E: return "A"; case 0x1F: return "S"; case 0x20: return "D";
			case 0x21: return "F"; case 0x22: return "G"; case 0x23: return "H";
			case 0x24: return "J"; case 0x25: return "K"; case 0x26: return "L";
			case 0x2C: return "Z"; case 0x2D: return "X"; case 0x2E: return "C";
			case 0x2F: return "V"; case 0x30: return "B"; case 0x31: return "N";
			case 0x32: return "M";
			case 0x3B: return "F1";  case 0x3C: return "F2";  case 0x3D: return "F3";
			case 0x3E: return "F4";  case 0x3F: return "F5";  case 0x40: return "F6";
			case 0x41: return "F7";  case 0x42: return "F8";  case 0x43: return "F9";
			case 0x44: return "F10"; case 0x57: return "F11"; case 0x58: return "F12";
			case 0x01: return "Escape";
			case 0x0E: return "Backspace";
			case 0x0F: return "Tab";
			case 0x1C: return "Enter";
			case 0x39: return "Space";
			case 0x3A: return "CapsLock";
			case 0x2A: return "Left Shift";
			case 0x36: return "Right Shift";
			case 0x1D: return "Left Ctrl";
			case 0x9D: return "Right Ctrl";
			case 0x38: return "Left Alt";
			case 0xB8: return "Right Alt";
			case 0x0C: return "Minus";
			case 0x0D: return "Equals";
			case 0x1A: return "[";
			case 0x1B: return "]";
			case 0x27: return ";";
			case 0x28: return "'";
			case 0x29: return "`";
			case 0x2B: return "\\";
			case 0x33: return ",";
			case 0x34: return ".";
			case 0x35: return "/";
			case 0xC7: return "Home";
			case 0xC8: return "Up";
			case 0xC9: return "Page Up";
			case 0xCB: return "Left";
			case 0xCD: return "Right";
			case 0xCF: return "End";
			case 0xD0: return "Down";
			case 0xD1: return "Page Down";
			case 0xD2: return "Insert";
			case 0xD3: return "Delete";
			case 0x52: return "Numpad 0"; case 0x4F: return "Numpad 1";
			case 0x50: return "Numpad 2"; case 0x51: return "Numpad 3";
			case 0x4B: return "Numpad 4"; case 0x4C: return "Numpad 5";
			case 0x4D: return "Numpad 6"; case 0x47: return "Numpad 7";
			case 0x48: return "Numpad 8"; case 0x49: return "Numpad 9";
			default: break;
			}

			char buf[8]{};
			std::snprintf(buf, sizeof(buf), "0x%02X", a_code);
			return buf;
		}

		// Case-insensitive inverse of ScancodeName: strips spaces/underscores so
		// "Left Shift", "LeftShift" and "left_shift" all match. Also accepts the
		// word spellings used in the file's _help line (LBracket, Semicolon,
		// Numpad0, ...) and 0xNN hex. Returns 0 for an unknown name.
		std::uint32_t ParseKeyName(const std::string& a_name)
		{
			std::string n;
			n.reserve(a_name.size());
			for (const char c : a_name) {
				if (c == ' ' || c == '_' || c == '\t') continue;
				n.push_back(static_cast<char>(
					std::tolower(static_cast<unsigned char>(c))));
			}
			if (n.empty()) return 0;

			// 0xNN hex only (a bare hex string is not accepted).
			if (n.size() > 2 && n[0] == '0' && n[1] == 'x') {
				char* end = nullptr;
				const unsigned long v = std::strtoul(n.c_str() + 2, &end, 16);
				if (end && *end == '\0' && v >= 1 && v <= 0xFF) {
					return static_cast<std::uint32_t>(v);
				}
				return 0;
			}

			// Function keys F1..F12 // no bare "F" key.
			if (n.size() >= 2 && n[0] == 'f') {
				bool digits = true;
				for (std::size_t i = 1; i < n.size(); ++i) {
					if (n[i] < '0' || n[i] > '9') { digits = false; break; }
				}
				const int num = digits ? std::atoi(n.c_str() + 1) : 0;
				if (num >= 1 && num <= 12) {
					static constexpr std::uint32_t kF[12] = {
						0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40,
						0x41, 0x42, 0x43, 0x44, 0x57, 0x58
					};
					return kF[num - 1];
				}
			}

			// Single letters A..Z.
			if (n.size() == 1 && n[0] >= 'a' && n[0] <= 'z') {
				switch (n[0]) {
				case 'q': return 0x10; case 'w': return 0x11; case 'e': return 0x12;
				case 'r': return 0x13; case 't': return 0x14; case 'y': return 0x15;
				case 'u': return 0x16; case 'i': return 0x17; case 'o': return 0x18;
				case 'p': return 0x19;
				case 'a': return 0x1E; case 's': return 0x1F; case 'd': return 0x20;
				case 'f': return 0x21; case 'g': return 0x22; case 'h': return 0x23;
				case 'j': return 0x24; case 'k': return 0x25; case 'l': return 0x26;
				case 'z': return 0x2C; case 'x': return 0x2D; case 'c': return 0x2E;
				case 'v': return 0x2F; case 'b': return 0x30; case 'n': return 0x31;
				case 'm': return 0x32;
				default: break;
				}
				return 0;
			}

			// Single digits 0..9.
			if (n.size() == 1 && n[0] >= '0' && n[0] <= '9') {
				switch (n[0]) {
				case '1': return 0x02; case '2': return 0x03; case '3': return 0x04;
				case '4': return 0x05; case '5': return 0x06; case '6': return 0x07;
				case '7': return 0x08; case '8': return 0x09; case '9': return 0x0A;
				case '0': return 0x0B;
				default: break;
				}
				return 0;
			}

			// Named keys (normalized) plus the _help line's word aliases.
			struct Entry { const char* name; std::uint32_t code; };
			static constexpr Entry kNames[] = {
				{ "escape", 0x01 }, { "backspace", 0x0E }, { "tab", 0x0F },
				{ "enter", 0x1C }, { "space", 0x39 }, { "capslock", 0x3A },
				{ "leftshift", 0x2A }, { "rightshift", 0x36 },
				{ "leftctrl", 0x1D }, { "rightctrl", 0x9D },
				{ "leftalt", 0x38 }, { "rightalt", 0xB8 },
				{ "minus", 0x0C }, { "equals", 0x0D },
				{ "[", 0x1A }, { "]", 0x1B }, { ";", 0x27 }, { "'", 0x28 },
				{ "`", 0x29 }, { "\\", 0x2B }, { ",", 0x33 }, { ".", 0x34 },
				{ "/", 0x35 },
				{ "lbracket", 0x1A }, { "rbracket", 0x1B }, { "semicolon", 0x27 },
				{ "quote", 0x28 }, { "backquote", 0x29 }, { "backslash", 0x2B },
				{ "comma", 0x33 }, { "period", 0x34 }, { "slash", 0x35 },
				{ "home", 0xC7 }, { "up", 0xC8 }, { "pageup", 0xC9 },
				{ "left", 0xCB }, { "right", 0xCD }, { "end", 0xCF },
				{ "down", 0xD0 }, { "pagedown", 0xD1 },
				{ "insert", 0xD2 }, { "delete", 0xD3 },
				{ "numpad0", 0x52 }, { "numpad1", 0x4F }, { "numpad2", 0x50 },
				{ "numpad3", 0x51 }, { "numpad4", 0x4B }, { "numpad5", 0x4C },
				{ "numpad6", 0x4D }, { "numpad7", 0x47 }, { "numpad8", 0x48 },
				{ "numpad9", 0x49 },
			};
			for (const auto& e : kNames) {
				if (n == e.name) return e.code;
			}
			return 0;
		}

		// Write the current binding to disk. Caller holds g_settingsMutex.
		void PersistLocked()
		{
			const std::uint32_t code = g_scancode.load(std::memory_order_relaxed);

			nlohmann::json j;
			j["hotkeyScancode"] = code;
			j["hotkey"]         = ScancodeName(code);
			j["_help"] = "Set the key by name (\"hotkey\") or DirectInput scancode "
				"(\"hotkeyScancode\", 1-255). Names: F1-F12, A-Z, 0-9, Space, Tab, "
				"Enter, Escape, Backspace, Insert, Delete, Home, End, PageUp, "
				"PageDown, Up, Down, Left, Right, LeftShift, RightShift, LeftCtrl, "
				"RightCtrl, LeftAlt, RightAlt, Minus, Equals, LBracket, RBracket, "
				"Semicolon, Quote, Backquote, Backslash, Comma, Period, Slash, "
				"Numpad0-Numpad9. The default is Right Alt. Examples: "
				"RightAlt=184, G=34, F10=68.";

			const std::filesystem::path path = SettingsPath();
			std::error_code ec;
			std::filesystem::create_directories(path.parent_path(), ec);

			std::ofstream out(path, std::ios::binary | std::ios::trunc);
			if (!out) {
				logs::warn("hotkey: could not open settings for writing '{}'", fuzzy::PathToUtf8(path));
				return;
			}
			out << j.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
			if (!out) {
				logs::warn("hotkey: failed writing settings '{}'", fuzzy::PathToUtf8(path));
			}
		}

		void LoadSettings()
		{
			const std::filesystem::path path = SettingsPath();
			std::error_code ec;
			if (!std::filesystem::exists(path, ec)) {
				// First run: materialize the defaults so the file is visible and
				// editable. The
				// global still holds its default here (Right Alt).
				std::lock_guard lock(g_settingsMutex);
				PersistLocked();
				logs::info("hotkey: wrote default settings '{}'", fuzzy::PathToUtf8(path));
				return;
			}

			std::ifstream in(path, std::ios::binary);
			if (!in) {
				logs::warn("hotkey: could not read settings '{}'", fuzzy::PathToUtf8(path));
				return;
			}

			nlohmann::json parsed;
			try {
				in >> parsed;
			} catch (...) {
				logs::warn("hotkey: settings '{}' is not valid JSON - using defaults", fuzzy::PathToUtf8(path));
				return;
			}
			if (!parsed.is_object()) {
				logs::warn("hotkey: settings '{}' is not a JSON object - using defaults", fuzzy::PathToUtf8(path));
				return;
			}

			// Name first (a string), then the numeric scancode: the numeric
			// field wins when both are present.
			if (auto it = parsed.find("hotkey");
				it != parsed.end() && it->is_string()) {
				const std::uint32_t code = ParseKeyName(it->get<std::string>());
				if (code != 0) {
					g_scancode.store(code, std::memory_order_relaxed);
				} else {
					logs::warn("hotkey: unknown key name '{}' - ignoring", it->get<std::string>());
				}
			}

			if (auto it = parsed.find("hotkeyScancode");
				it != parsed.end() && it->is_number_integer()) {
				const auto v = it->get<std::int64_t>();
				if (v > 0 && v <= 0xFF) {
					g_scancode.store(static_cast<std::uint32_t>(v), std::memory_order_relaxed);
				}
			}
		}
	}

	void Initialize()
	{
		LoadSettings();
		logs::info("IntelLite hotkey: settings loaded (key {})", KeyName());
	}

	std::string KeyName()
	{
		return ScancodeName(g_scancode.load(std::memory_order_relaxed));
	}

	std::uint32_t Scancode()
	{
		return g_scancode.load(std::memory_order_relaxed);
	}
}
