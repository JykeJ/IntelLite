// Portions of this file are adapted from IntelEngine-NativePlugin
// (Copyright 2026 Galanx), licensed under the Apache License 2.0.
// Changed for IntelLite. See THIRD_PARTY_NOTICES.md and NOTICE.

// IntelLite's world-wide NPC name index. See npc_index.h.

#include "npc_index.h"

#include "fuzzy.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace intellite::npc_index
{
	namespace
	{
		std::mutex g_mutex;
		std::unordered_map<std::string, std::vector<RE::FormID>> g_baseByName;  // persistent baseline from Build()
		std::unordered_map<std::string, std::vector<RE::FormID>> g_byName;      // fuzzy::Normalize(name) -> reference FormIDs

		void AddRefUnique(std::vector<RE::FormID>& a_vec, RE::FormID a_ref)
		{
			if (a_ref == 0) return;
			if (std::find(a_vec.begin(), a_vec.end(), a_ref) == a_vec.end()) {
				a_vec.push_back(a_ref);
			}
		}

		// Whether an actor belongs in the index: alive, not the player, and a
		// humanoid. The humanoid check is skipped when the keyword is unavailable.
		bool IsIndexable(RE::Actor* a_actor)
		{
			if (!a_actor) return false;
			if (a_actor->IsDeleted()) return false;
			if (a_actor->IsPlayerRef()) return false;
			if (a_actor->IsDead()) return false;

			auto* kw = HumanoidKeyword();
			if (kw) {
				auto* race = a_actor->GetRace();
				if (!race || !race->HasKeyword(kw)) return false;
			}
			return true;
		}

		// Collect (normalized name, reference FormID) for every named humanoid
		// actor currently loaded (ProcessLists). Game thread; touches no lock.
		std::vector<std::pair<std::string, RE::FormID>> LoadedActorRows()
		{
			std::vector<std::pair<std::string, RE::FormID>> rows;
			auto* lists = RE::ProcessLists::GetSingleton();
			if (!lists) return rows;

			auto consider = [&rows](RE::Actor* a_actor) {
				if (!IsIndexable(a_actor)) return;
				const char* n = a_actor->GetDisplayFullName();
				if (!n || !*n) return;
				std::string key = fuzzy::Normalize(n);
				if (key.empty()) return;
				rows.emplace_back(std::move(key), a_actor->GetFormID());
			};

			for (const auto& h : lists->highActorHandles) {
				if (auto ptr = h.get()) consider(ptr.get());
			}
			for (const auto& h : lists->middleHighActorHandles) {
				if (auto ptr = h.get()) consider(ptr.get());
			}
			for (const auto& h : lists->middleLowActorHandles) {
				if (auto ptr = h.get()) consider(ptr.get());
			}
			for (const auto& h : lists->lowActorHandles) {
				if (auto ptr = h.get()) consider(ptr.get());
			}
			return rows;
		}

		// po3-Tweaks' exported editorid provider - the same one SkyrimNet uses.
		// Resolved lazily and cached once found; a null result means the DLL is
		// not installed (the fallback is simply unavailable). Game thread only.
		using Po3GetEditorID = const char* (*)(std::uint32_t);

		Po3GetEditorID Po3EditorIDProvider()
		{
			static Po3GetEditorID provider = nullptr;
			if (!provider) {
				if (HMODULE po3 = GetModuleHandleW(L"po3_Tweaks.dll")) {
					provider = reinterpret_cast<Po3GetEditorID>(
						GetProcAddress(po3, "GetFormEditorID"));
					if (provider) {
						logs::info("npc_index: editorid fallback resolved (po3_Tweaks.dll!GetFormEditorID)");
					}
				}
			}
			return provider;
		}
	}

	RE::BGSKeyword* HumanoidKeyword()
	{
		static RE::BGSKeyword* kw = nullptr;
		if (!kw) {
			if (auto* found = RE::TESForm::LookupByEditorID<RE::BGSKeyword>("ActorTypeNPC")) {
				kw = found;
			} else if (auto* dataHandler = RE::TESDataHandler::GetSingleton()) {
				kw = dataHandler->LookupForm<RE::BGSKeyword>(0x13794, "Skyrim.esm");
			}
		}
		return kw;
	}

	void Build()
	{
		auto* dh = RE::TESDataHandler::GetSingleton();
		if (!dh) {
			logs::warn("npc_index: no data handler - index not built");
			return;
		}

		// Phase 1: unique named NPC bases (base FormID -> normalized name).
		std::unordered_map<RE::FormID, std::string> bases;
		for (auto* npc : dh->GetFormArray<RE::TESNPC>()) {
			if (!npc) continue;
			const char* n = npc->GetFullName();
			if (!n || !*n) continue;
			if (!npc->IsUnique()) continue;
			bases.emplace(npc->GetFormID(), fuzzy::Normalize(n));
		}

		// Phases 1.5/1.6: resolve each base to placed references by scanning
		// persistent references. Persistent references are instantiated before
		// the main menu, so never-loaded cells are reached. The scan works on
		// locals only - no lock is taken here.
		std::unordered_map<RE::FormID, std::vector<RE::FormID>> refsByBase;
		auto scanCell = [&bases, &refsByBase](RE::TESObjectCELL* a_cell) {
			if (!a_cell) return;
			a_cell->ForEachReference(
				[&bases, &refsByBase](RE::TESObjectREFR* a_refr) -> RE::BSContainer::ForEachResult {
					if (!a_refr) return RE::BSContainer::ForEachResult::kContinue;

					auto* actor = a_refr->As<RE::Actor>();
					if (!IsIndexable(actor)) return RE::BSContainer::ForEachResult::kContinue;

					// A ref flagged disabled is not present in the world, so never
					// offer it as a target from the world scan. Only this scan
					// filters the flag: the loaded overlay indexes actors that are
					// in the world right now, whatever their initial flag was.
					if (actor->IsDisabled()) return RE::BSContainer::ForEachResult::kContinue;

					auto* base = actor->GetActorBase();
					if (!base) return RE::BSContainer::ForEachResult::kContinue;

					if (bases.find(base->GetFormID()) == bases.end()) {
						return RE::BSContainer::ForEachResult::kContinue;
					}
					AddRefUnique(refsByBase[base->GetFormID()], actor->GetFormID());
					return RE::BSContainer::ForEachResult::kContinue;
				});
		};

		for (auto* ws : dh->GetFormArray<RE::TESWorldSpace>()) {
			if (ws) scanCell(ws->persistentCell);
		}
		for (auto* cell : dh->interiorCells) {
			scanCell(cell);
		}

		// Combine: base name -> resolved references, then overlay the loaded set.
		std::unordered_map<std::string, std::vector<RE::FormID>> baseByName;
		for (const auto& [baseId, name] : bases) {
			if (name.empty()) continue;
			auto it = refsByBase.find(baseId);
			if (it == refsByBase.end()) continue;
			auto& bucket = baseByName[name];
			for (const auto ref : it->second) {
				AddRefUnique(bucket, ref);
			}
		}
		auto byName = baseByName;
		for (auto& [key, ref] : LoadedActorRows()) {
			AddRefUnique(byName[key], ref);
		}

		std::size_t finalCount = 0;
		{
			std::lock_guard lock(g_mutex);
			g_baseByName = std::move(baseByName);
			g_byName = std::move(byName);
			finalCount = g_byName.size();
		}

		logs::info("npc_index: built - {} unique named base(s), {} ref(s) resolved, {} name(s) indexed",
			bases.size(), refsByBase.size(), finalCount);
	}

	void ResetLoaded()
	{
		std::lock_guard lock(g_mutex);
		g_byName = g_baseByName;
	}

	void RefreshLoaded()
	{
		// Gather the loaded rows first (RE:: calls, no lock held), then merge
		// under the lock with no RE:: call inside it.
		auto rows = LoadedActorRows();
		std::lock_guard lock(g_mutex);
		for (auto& [key, ref] : rows) {
			AddRefUnique(g_byName[key], ref);
		}
	}

	std::string SearchRowsJson(const std::string& query, int limit)
	{
		if (query.empty() || limit <= 0) return "[]";

		const std::string normQuery = fuzzy::Normalize(query);

		struct Match
		{
			int         score = 0;
			std::string name;
			RE::FormID  ref = 0;
		};

		std::vector<Match> matches;
		{
			std::lock_guard lock(g_mutex);
			for (const auto& [name, refs] : g_byName) {
				const int score = fuzzy::ScoreNormalized(normQuery, name);
				if (score < fuzzy::kFloor) continue;
				for (const auto ref : refs) {
					matches.push_back(Match{ score, name, ref });
				}
			}
		}
		if (matches.empty()) return "[]";

		// Best score first; ties break by name ascending, then FormID ascending for determinism.
		std::sort(matches.begin(), matches.end(), [](const Match& a, const Match& b) {
			if (a.score != b.score) return a.score > b.score;
			if (a.name != b.name) return a.name < b.name;
			return a.ref < b.ref;
		});

		nlohmann::json out = nlohmann::json::array();
		for (const auto& m : matches) {
			if (static_cast<int>(out.size()) >= limit) break;
			auto* actor = RE::TESForm::LookupByID<RE::Actor>(m.ref);
			if (!actor || actor->IsDeleted()) continue;
			if (actor->IsPlayerRef()) continue;
			if (actor->IsDead()) continue;  // dead actors are not valid targets

			const char* n = actor->GetDisplayFullName();

			nlohmann::json row;
			row["name"] = (n && *n) ? n : m.name;

			const std::string editorid = BaseEditorID(actor);

			row["formId"] = m.ref;
			row["editorid"] = editorid;
			out.push_back(std::move(row));
		}

		return out.empty()
			? "[]"
			: out.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
	}

	std::string FormEditorID(RE::TESForm* a_form)
	{
		if (!a_form) return {};

		// The game's own getter first: real editorids whenever a lookup patch
		// (e.g. Native EditorID Fix's native mode) re-implements it.
		if (const char* id = a_form->GetFormEditorID(); id && *id) {
			return id;
		}

		// Fallback: po3-Tweaks carries editorids for forms the engine discards,
		// NPC bases and factions included.
		if (auto provider = Po3EditorIDProvider()) {
			if (const char* id = provider(a_form->GetFormID()); id && *id) {
				return id;
			}
		}
		return {};
	}

	std::string BaseEditorID(RE::Actor* a_actor)
	{
		if (!a_actor) return {};
		return FormEditorID(a_actor->GetActorBase());
	}
}
