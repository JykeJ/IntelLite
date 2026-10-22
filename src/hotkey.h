#pragma once

// IntelLite's panel hotkey.
//
// The binding lives ONLY in <exeDir>/Data/SKSE/Plugins/IntelLite/settings.json
// (under MO2 a write there is redirected into the overwrite folder) as
// {"hotkeyScancode": <int>, "hotkey": "<name>"}; the default is Right Alt. The
// file is written at first run when it does not yet exist, so it is visible and
// editable, and it is read at startup: editing it is the only way to change the
// key. There is no in-game capture. The panel displays the current key.
//
// The host owns the toggle: panel.cpp binds the stored DirectInput scancode
// through Magelight's BindHotkey, which fires outside engine menus and, while
// the page has key focus, only for the view's own key (the close case). This
// module owns only the setting: no input sink is involved.

#include <cstdint>
#include <string>

namespace intellite::hotkey
{
	// Read the settings file. Call once at kDataLoaded, before
	// panel::PushStatus so the panel reflects the stored key.
	void Initialize();

	// The configured key as a human-readable name for common keys (F1-F12,
	// letters, digits, arrows, named keys), else "0xNN".
	std::string KeyName();

	// The configured key as a DirectInput scancode (reads the atomic).
	std::uint32_t Scancode();
}
