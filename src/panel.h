#pragma once

// IntelLite's MagelightUI panel: a hotkey-toggled view hosting
// Data/Magelight/IntelLite/views/panel/index.html with a JS<->C++ JSON bridge.
// The key setting lives in hotkey.{h,cpp}; the host owns the toggle, bound
// here through Magelight's BindHotkey.

#include <cstdint>
#include <string>

namespace intellite::panel
{
	// Returns the Magelight host version string, or "absent" when Magelight is
	// not present.
	std::string HostVersion();

	// Acquire the Magelight mod, create the view, register the JS listeners and
	// the onClose handler. A no-op (with a log line) when Magelight is absent.
	void Initialize();

	// Push version/host info to the page via
	// window.intelite.setStatus({ modVersion, skyrimnetApi, magelightHost, hotkey }).
	void PushStatus();

	// Game-thread, read-only scan of loaded actors within ~3000 units of the
	// player, pushed via window.intelite.setNearby([{ name, distance, formId }]).
	// MUST be called on the game thread.
	void PushNearby();

	// Push a JSON array of active tasks via
	// window.intelite.setTasks([{ id, type, agent, target, state, detail, canCancel }]).
	// Safe from any thread.
	void PushTasks(const std::string& json);

	// Push a JSON array of actor search results via
	// window.intelite.setSearchResults([{ name, formId }]). Safe from any thread.
	void PushSearchResults(const std::string& json);

	// Push a JSON array of executor search results via
	// window.intelite.setExecutorSearchResults([{ name, formId }]). Safe from any thread.
	void PushExecutorSearchResults(const std::string& json);

	// Entry point for a SkyrimNet-initiated NPC action. Called from the action
	// execute callback, which may run on any thread: the kind, the initiating
	// agent FormID and the raw params JSON are copied and posted to the game
	// thread, which resolves the target/location and then either opens the
	// consent prompt or - when the confirmation setting is off - creates the
	// task immediately with the resolved parameters (the same path as Agree).
	// Kind "cancelall" cancels every task instead and never opens a prompt.
	// Safe from any thread.
	void RequestAction(const std::string& kind, std::uint32_t agentFormID,
		const std::string& paramsJson);
}
