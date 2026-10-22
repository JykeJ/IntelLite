#pragma once

// IntelLite <-> SkyrimNet bridge.
//
// The ONLY translation unit that includes vendor/skyrimnet/PublicAPI.h is
// skyrimnet_bridge.cpp, because that header defines its function-pointer
// globals at namespace scope with external linkage (they are NOT inline).

#include <functional>
#include <string>
#include <vector>

namespace RE
{
	class Actor;
}

namespace intellite::skyrimnet
{
	// Resolve SkyrimNet's public API (FindFunctions) and log the probe lines.
	// Safe to call once; a no-op if already resolved.
	void Initialize();

	// True when SkyrimNet's public API resolved at Initialize().
	bool IsReady();

	// PublicGetVersion() as resolved at Initialize(), or -1 when not ready.
	int ApiVersion();

	// Register IntelLite's player-facing NPC actions with SkyrimNet: the five
	// actions FetchPerson, DeliverMessage, MeetLater, Assassinate and CancelAll.
	// The first four are filed inside the native Commission category
	// (PublicRegisterCPPSubCategory plus the 0.28 nested action type) and share
	// one eligibility callback that consults the task engine's thread-safe state
	// plus two actor reads (not in combat, not in an OStim scene). CancelAll
	// stays top-level and cancels only the speaking NPC's tasks. Each action
	// carries a JSON parameter schema and an execute callback that copies the
	// params and posts a game-thread task that resolves the target/location and
	// raises the consent prompt. Returns the number registered and logs each
	// result; 0 when SkyrimNet is not ready.
	int RegisterActions();

	// Enable or disable SkyrimNet's C++ hotkey manager through its Papyrus API
	// (SkyrimNetApi.SetCppHotkeysEnabled, Global Native). The panel calls this
	// with false while its UI mode is open and true on every exit path, so
	// SkyrimNet's native hotkeys (e.g. "G" opening text input) do not fire over
	// our panel. GAME THREAD ONLY: it dispatches through the Papyrus VM (the
	// same DispatchStaticCall path movement uses). Returns true when the
	// dispatch was accepted (not the called function's own result code); a
	// dispatch failure is logged once.
	bool SetCppHotkeysEnabled(bool enabled);

	// True when SkyrimNet's memory/database system is ready for queries
	// (a save is loaded). False before then or when the API is absent/older.
	bool IsMemoryReady();

	// Search SkyrimNet's known actors by name substring. Returns a JSON array of
	// {name, formId, editorid} (best match first); the per-row enrichment drops
	// rows that resolve to a non-humanoid or dead actor. Returns "[]" when the
	// memory system is not ready or PublicSearchActors is unavailable. The raw
	// search is thread-safe per SkyrimNet, but this call is GAME-THREAD ONLY
	// (the per-row enrichment reads RE::); use SearchActorsAsync() from other
	// threads.
	std::string SearchActors(const std::string& query, int limit);

	// The async variant of SearchActors(): the raw SkyrimNet search runs on a
	// worker thread and the per-row RE:: enrichment + the callback run on the
	// GAME THREAD, so a_done may touch game state. The result JSON is the same
	// shape SearchActors() returns ("[]" when not ready). Callable from any thread.
	void SearchActorsAsync(const std::string& query, int limit,
		std::function<void(std::string)> a_done);

	// Register a persistent event through SkyrimNet's native C++ API
	// (PublicRegisterEvent, event type "persistent_generic"; the C++
	// counterpart of Papyrus SkyrimNetApi.RegisterPersistentEvent). The event
	// informs actors for context without triggering dialogue reactions. The
	// audience list, when non-empty, restricts perception to exactly those
	// actors plus the originator and target; empty uses SkyrimNet's default
	// audience (everyone near the originator, plus the player). A null actor is
	// passed as none (FormID 0). GAME THREAD ONLY (our callers). A missing API
	// or a rejected registration is logged once.
	void RegisterPersistentEvent(const std::string& content, RE::Actor* originator, RE::Actor* target,
		const std::vector<RE::Actor*>& audience = {});

	// Dispatch a DIRECT NARRATION through SkyrimNet's native C++ API
	// (PublicRegisterEvent, event type "direct_narration"). Unlike a persistent
	// event, a narration makes nearby NPCs react. The audience list follows the
	// same rules as RegisterPersistentEvent. GAME THREAD ONLY (our callers). A
	// missing API or a rejected registration is logged once.
	void DirectNarration(const std::string& content, RE::Actor* originator, RE::Actor* target,
		const std::vector<RE::Actor*>& audience = {});

	// Store a per-actor string in PapyrusUtil's StorageUtil
	// (StorageUtil.SetStringValue(actor, key, value), Global Native); pass an
	// empty value to clear the key. The character-bio submodule reads it back
	// through the native papyrus_util decorator. GAME THREAD ONLY: it dispatches
	// through the Papyrus VM. A dispatch failure is logged once.
	void SetActorState(RE::Actor* actor, const std::string& key, const std::string& value);

	// Store a per-actor float in PapyrusUtil's StorageUtil
	// (StorageUtil.SetFloatValue(actor, key, value), Global Native). The
	// character-bio submodule reads it back through the native papyrus_util
	// decorator (the message time, in game days). GAME THREAD ONLY: it
	// dispatches through the Papyrus VM. A dispatch failure is logged once.
	void SetActorFloat(RE::Actor* actor, const std::string& key, float value);

	// Create a world knowledge entry through SkyrimNet's native C++ API
	// (PublicAddWorldKnowledge, API v7+): a shared fact retrieved for any NPC
	// whose Inja condition expression is true. Returns the new memory ID (>0)
	// on success; 0 when the API is unavailable, the memory system is not
	// ready, or the call failed. GAME THREAD ONLY (our callers).
	int AddWorldKnowledge(const std::string& content, const std::string& conditionExpr,
		bool alwaysInject, float importance, const std::string& displayName);

	// Find existing world knowledge entries whose display name is EXACTLY
	// displayName, over SkyrimNet's PublicGetWorldKnowledge (API v7+). Returns
	// the matching entry ids; empty when the API is unavailable, the memory
	// system is not ready, the reply is unparseable, or nothing matches. The
	// API-unavailable case is logged once. GAME THREAD ONLY (our callers).
	std::vector<int> FindWorldKnowledgeByDisplayName(const std::string& displayName);

	// Append one entry to a per-actor StorageUtil list (the recipient's
	// message history). Game thread only.
	void AppendActorStringList(RE::Actor* actor, const std::string& key, const std::string& value);
	void AppendActorFloatList(RE::Actor* actor, const std::string& key, float value);
}
