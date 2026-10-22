#pragma once

// IntelLite's fuzzy NPC-name resolver and its editable blacklist.
//
// The resolver feeds a raw name (from the LLM or a UI field) through SkyrimNet's
// actor search, then scores each candidate's display name with the shared fuzzy
// ladder (fuzzy.h), so "Elisif the Fair" reaches "Jarl Elisif the Fair". The
// blacklist is a set of NPC BASE editorids the resolver must never pick; it is
// editable from the panel and persisted as JSON.
//
// GAME THREAD ONLY for Resolve() and IsBlacklisted() (they touch RE::). The
// blacklist editing calls are mutex-guarded and safe from any thread.

#include <functional>
#include <string>
#include <vector>

namespace RE
{
	class Actor;
}

namespace intellite::actors
{
	// Resolve `query` to a live, non-player, non-blacklisted Actor. Searches the
	// merged set (SkyrimNet's known actors plus the local world-wide index, up
	// to 12 SkyrimNet rows), scores the query against each actor's display name,
	// and picks the highest score at or above fuzzy::kFloor (ties keep the
	// earlier search row). On success sets `out` and `outName` and logs the
	// chosen name + score. Returns false when nothing scores or resolves. Game
	// thread only.
	bool Resolve(const std::string& query, RE::Actor*& out, std::string& outName);

	// Async variant of Resolve(): the SkyrimNet search runs on a worker thread and
	// the scoring + RE:: lookups run on the game thread. a_done(actor, name) is
	// invoked on the GAME THREAD with the same verdict Resolve() would give
	// (actor == nullptr when nothing resolves). Searches the same merged set as
	// Resolve(). Callable from any thread.
	void ResolveAsync(const std::string& query,
		std::function<void(RE::Actor* actor, std::string name)> a_done);

	// The merged search used by the panel and the resolver: SkyrimNet's
	// registered-actor search first, then rows only the local world-wide NPC
	// index (npc_index.h) can see, deduped by resolved actor. When the local
	// index adds nothing, the SkyrimNet JSON is returned unchanged.
	// GAME THREAD ONLY.
	std::string SearchMerged(const std::string& query, int limit);

	// Async variant: the SkyrimNet search runs on a worker thread; the local
	// refresh + merge run on the game thread. a_done(json) is invoked on the
	// GAME THREAD with the merged rows. Callable from any thread.
	void SearchMergedAsync(const std::string& query, int limit,
		std::function<void(std::string)> a_done);

	// Read the persisted blacklist from
	// <exeDir>/Data/SKSE/Plugins/IntelLite/blacklist.json. A missing file is an
	// empty blacklist (no warning). Call once at kDataLoaded.
	void LoadBlacklist();

	// True when the actor's base editorid is blacklisted (case-insensitive).
	// An actor with no editorid is never blacklisted. Game thread only.
	bool IsBlacklisted(RE::Actor* actor);

	// The blacklist entries (original case), for the panel.
	std::vector<std::string> BlacklistEntries();

	// Add / remove an editorid (case-insensitive). Returns true when the set
	// changed. Persists on every change. Safe from any thread.
	bool BlacklistAdd(const std::string& editorid);
	bool BlacklistRemove(const std::string& editorid);
}
