#pragma once

// IntelLite's world-wide NPC name index - the off-screen half of actor search.
//
// SkyrimNet's PublicSearchActors only knows actors the save has registered
// (encountered), so a named NPC the player has never met - e.g. Lucan Valerius
// in a brand-new game - is invisible to it. This index reaches those unique
// named NPC bases directly from the data handler's NPC_ form array and resolves
// each one's placed reference by walking every worldspace's persistent cell and
// every interior cell (persistent references are instantiated before the main
// menu, so never-loaded cells are still reached), plus every named humanoid
// actor currently loaded (ProcessLists).
//
// RefreshLoaded() re-merges the loaded set; it is cheap (one ProcessLists pass)
// and is called on each merged search.
//
// GAME THREAD ONLY. The index stores reference FormIDs (never Actor*), so stale
// entries are harmless: a row whose reference no longer resolves is skipped.

#include <cstdint>
#include <string>

namespace RE
{
	class Actor;
	class BGSKeyword;
	class TESForm;
}

namespace intellite::npc_index
{
	// Build the world-wide index. Game thread, once at kDataLoaded. Clears and
	// rebuilds if called again.
	void Build();

	// Reset the index back to the persistent world-scan baseline (dropping
	// session-specific loaded overlays from a prior save). Game thread.
	void ResetLoaded();

	// Merge the currently loaded named actors into the index. Cheap (one
	// ProcessLists pass). Game thread.
	void RefreshLoaded();

	// Search rows in the SAME JSON shape SkyrimNet's search returns:
	// [{"name": "...", "formId": <uint32 ref FormID>, "editorid": "..."}, ...],
	// best score first, at most `limit` rows; rows whose reference no longer
	// resolves to a live Actor are skipped. "[]" for an empty query or
	// limit <= 0. Game thread.
	std::string SearchRowsJson(const std::string& query, int limit);

	// Skyrim's ActorTypeNPC keyword (0x13794 in Skyrim.esm), resolved once on
	// the game thread; nullptr when unavailable.
	RE::BGSKeyword* HumanoidKeyword();

	// A form's editorid (original case), or "" when unavailable. Prefers the
	// game's own getter and falls back to po3_Tweaks.dll!GetFormEditorID when
	// loaded. Game thread only.
	std::string FormEditorID(RE::TESForm* form);

	// The actor base's editorid (original case), or "" when unavailable.
	// Prefers the game's own getter (which returns real values when a lookup
	// patch such as Native EditorID Fix's native mode is active) and falls back
	// to po3-Tweaks' exported GetFormEditorID, resolved only when po3_Tweaks.dll
	// is loaded (a soft, optional dependency). Used for search rows and the
	// blacklist (add + match) so both sides always agree. Game thread only.
	std::string BaseEditorID(RE::Actor* actor);
}
