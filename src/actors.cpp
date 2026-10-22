// IntelLite's fuzzy NPC resolver and its editable blacklist. See actors.h.

#include "actors.h"

#include "fuzzy.h"
#include "npc_index.h"
#include "skyrimnet_bridge.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace intellite::actors
{
	namespace
	{
		std::mutex g_blacklistMutex;
		std::map<std::string, std::string> g_blacklist;  // lowercase editorid -> original

		// Log a persistence failure only once (a broken path would otherwise
		// spam the log on every edit).
		std::atomic<bool> g_writeFailureLogged{ false };

		std::string Lowercase(const std::string& a_in)
		{
			std::string s = a_in;
			for (char& ch : s) {
				ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
			}
			return s;
		}

		// <exeDir>/Data/SKSE/Plugins/IntelLite/blacklist.json. Under MO2 a write
		// here is redirected into the overwrite folder.
		std::filesystem::path BlacklistPath()
		{
			return fuzzy::ExeDir()
				/ L"Data" / L"SKSE" / L"Plugins" / L"IntelLite" / L"blacklist.json";
		}

		std::string EditorIDOf(RE::Actor* a_actor)
		{
			return npc_index::BaseEditorID(a_actor);
		}

		bool IsEditorIDBlacklisted(const std::string& a_id)
		{
			if (a_id.empty()) return false;
			std::lock_guard lock(g_blacklistMutex);
			return g_blacklist.count(Lowercase(a_id)) != 0;
		}

		// Write the current set to disk. Caller holds g_blacklistMutex.
		void PersistLocked()
		{
			nlohmann::json arr = nlohmann::json::array();
			for (const auto& entry : g_blacklist) {
				arr.push_back(entry.second);
			}

			const std::filesystem::path path = BlacklistPath();
			std::error_code ec;
			std::filesystem::create_directories(path.parent_path(), ec);
			// A create_directories failure is not fatal on its own (the folder may
			// already exist); the stream below is the verdict.

			std::ofstream out(path, std::ios::binary | std::ios::trunc);
			if (!out) {
				if (!g_writeFailureLogged.exchange(true)) {
					logs::warn("actors: could not open blacklist for writing '{}'",
						fuzzy::PathToUtf8(path));
				}
				return;
			}
			out << arr.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
			if (!out) {
				if (!g_writeFailureLogged.exchange(true)) {
					logs::warn("actors: failed writing blacklist '{}'", fuzzy::PathToUtf8(path));
				}
			}
		}
	}

	void LoadBlacklist()
	{
		std::lock_guard lock(g_blacklistMutex);
		g_blacklist.clear();
		g_writeFailureLogged = false;

		const std::filesystem::path path = BlacklistPath();
		std::error_code ec;
		if (!std::filesystem::exists(path, ec)) {
			return;  // missing file = empty blacklist, no warning
		}

		std::ifstream in(path, std::ios::binary);
		if (!in) {
			logs::warn("actors: could not read blacklist '{}'", fuzzy::PathToUtf8(path));
			return;
		}

		nlohmann::json parsed;
		try {
			in >> parsed;
		} catch (...) {
			logs::warn("actors: blacklist '{}' is not valid JSON - starting empty",
				fuzzy::PathToUtf8(path));
			return;
		}
		if (!parsed.is_array()) {
			logs::warn("actors: blacklist '{}' is not a JSON array - starting empty",
				fuzzy::PathToUtf8(path));
			return;
		}

		for (const auto& entry : parsed) {
			if (!entry.is_string()) continue;
			const std::string original = entry.get<std::string>();
			if (original.empty()) continue;
			g_blacklist.emplace(Lowercase(original), original);
		}
		logs::info("actors: blacklist loaded - {} entr(ies)", g_blacklist.size());
	}

	bool IsBlacklisted(RE::Actor* a_actor)
	{
		return IsEditorIDBlacklisted(EditorIDOf(a_actor));
	}

	std::vector<std::string> BlacklistEntries()
	{
		std::lock_guard lock(g_blacklistMutex);
		std::vector<std::string> out;
		out.reserve(g_blacklist.size());
		for (const auto& entry : g_blacklist) {
			out.push_back(entry.second);
		}
		return out;
	}

	bool BlacklistAdd(const std::string& a_editorid)
	{
		if (a_editorid.empty()) return false;
		std::lock_guard lock(g_blacklistMutex);
		const std::string key = Lowercase(a_editorid);
		if (g_blacklist.count(key)) return false;
		g_blacklist.emplace(key, a_editorid);
		PersistLocked();
		return true;
	}

	bool BlacklistRemove(const std::string& a_editorid)
	{
		if (a_editorid.empty()) return false;
		std::lock_guard lock(g_blacklistMutex);
		if (!g_blacklist.erase(Lowercase(a_editorid))) return false;
		PersistLocked();
		return true;
	}

	namespace
	{
		// Merge the local world-wide NPC index's rows into a SkyrimNet search
		// reply, drop rows whose actor no longer resolves (stale FormIDs) and
		// rows for blacklisted actors, and dedup by resolved actor. GAME THREAD
		// ONLY. When nothing changed the SkyrimNet JSON is returned
		// byte-for-byte unchanged (the hot path).
		std::string MergeLocalRows(const std::string& a_query, int a_limit,
			std::string a_skyrimnetJson)
		{
			npc_index::RefreshLoaded();

			const std::string localJson = npc_index::SearchRowsJson(a_query, a_limit);

			nlohmann::json localParsed = nlohmann::json::array();
			try {
				auto parsed = nlohmann::json::parse(localJson);
				if (parsed.is_array()) {
					localParsed = std::move(parsed);
				}
			} catch (...) {
			}

			// Unparseable or non-array replies are treated as empty; the other
			// source's rows still flow through.
			nlohmann::json skyrimnetParsed = nlohmann::json::array();
			try {
				auto parsed = nlohmann::json::parse(a_skyrimnetJson);
				if (parsed.is_array()) {
					skyrimnetParsed = std::move(parsed);
				}
			} catch (...) {
			}

			// One pass over the SkyrimNet rows: resolve each row's actor to drop
			// rows whose actor no longer resolves (a stale row carries no
			// editorid and cannot be selected) and rows for blacklisted actors,
			// and to remember which actors are already present so a local row is
			// never appended twice.
			nlohmann::json out = nlohmann::json::array();
			std::unordered_set<const RE::Actor*> seen;
			std::size_t droppedCount = 0;
			for (const auto& entry : skyrimnetParsed) {
				if (a_limit > 0 && static_cast<int>(out.size()) >= a_limit) {
					++droppedCount;
					continue;
				}
				if (!entry.is_object()) {
					++droppedCount;
					continue;
				}
				std::uint32_t id = 0;
				if (auto it = entry.find("formId"); it != entry.end() && it->is_number()) {
					id = it->get<std::uint32_t>();
				}
				if (id == 0) {
					++droppedCount;
					continue;
				}

				auto* actor = RE::TESForm::LookupByID<RE::Actor>(id);
				if (!actor || actor->IsDeleted() || actor->IsPlayerRef() || actor->IsDead()) {
					++droppedCount;
					continue;
				}
				std::string editorid;
				if (auto it = entry.find("editorid"); it != entry.end() && it->is_string()) {
					editorid = it->get<std::string>();
				} else {
					editorid = EditorIDOf(actor);
				}
				if (IsEditorIDBlacklisted(editorid)) {
					++droppedCount;
					continue;
				}

				if (!seen.insert(actor).second) {
					++droppedCount;
					continue;
				}
				out.push_back(entry);
			}

			std::size_t addedCount = 0;
			for (const auto& entry : localParsed) {
				if (a_limit > 0 && static_cast<int>(out.size()) >= a_limit) break;
				if (!entry.is_object()) continue;
				std::uint32_t id = 0;
				if (auto it = entry.find("formId"); it != entry.end() && it->is_number()) {
					id = it->get<std::uint32_t>();
				}
				if (id == 0) continue;

				auto* actor = RE::TESForm::LookupByID<RE::Actor>(id);
				if (!actor || actor->IsDeleted() || actor->IsPlayerRef() || actor->IsDead()) continue;
				std::string editorid;
				if (auto it = entry.find("editorid"); it != entry.end() && it->is_string()) {
					editorid = it->get<std::string>();
				} else {
					editorid = EditorIDOf(actor);
				}
				if (IsEditorIDBlacklisted(editorid)) continue;
				if (!seen.insert(actor).second) continue;

				out.push_back(entry);
				++addedCount;
			}

			const std::size_t skyrimnetCount = skyrimnetParsed.size();
			logs::debug("actors: search '{}' - {} SkyrimNet row(s), {} world row(s) added, {} row(s) dropped",
				a_query, skyrimnetCount, addedCount, droppedCount);

			// Nothing changed: hand back the SkyrimNet JSON unchanged.
			if (addedCount == 0 && droppedCount == 0) return a_skyrimnetJson;

			return out.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
		}

		// Score an already-fetched SearchActors JSON against `a_query` and pick the
		// best live actor. GAME THREAD ONLY (it resolves actors and reads RE::).
		bool ScoreSearchJson(const std::string& a_query, const std::string& a_json,
			RE::Actor*& out, std::string& outName)
		{
			nlohmann::json parsed;
			try {
				parsed = nlohmann::json::parse(a_json);
			} catch (...) {
				return false;
			}
			if (!parsed.is_array()) return false;

			int         bestScore = -1;
			RE::Actor*  bestActor = nullptr;
			std::string bestName;

			for (const auto& entry : parsed) {
				if (!entry.is_object()) continue;

				std::uint32_t id = 0;
				if (auto it = entry.find("formId"); it != entry.end() && it->is_number()) {
					id = it->get<std::uint32_t>();
				}
				if (id == 0) continue;

				auto* actor = RE::TESForm::LookupByID<RE::Actor>(id);
				if (!actor || actor->IsDeleted()) continue;
				if (actor->IsPlayerRef()) continue;
				if (actor->IsDead()) continue;
				if (auto it = entry.find("editorid"); it != entry.end() && it->is_string()) {
					if (IsEditorIDBlacklisted(it->get<std::string>())) continue;
				} else if (IsBlacklisted(actor)) {
					continue;
				}

				const char* n = actor->GetDisplayFullName();
				std::string name = (n && *n) ? std::string(n) : std::string{};
				if (name.empty()) {
					if (auto it = entry.find("name"); it != entry.end() && it->is_string()) {
						name = it->get<std::string>();
					}
				}
				const int score = fuzzy::Score(a_query, name);
				if (score < fuzzy::kFloor) continue;

				// Strictly greater keeps the earlier search row on a tie.
				if (score > bestScore) {
					bestScore = score;
					bestActor = actor;
					bestName = name;
				}
			}

			if (!bestActor) return false;

			out = bestActor;
			outName = bestName;
			logs::info("actors: resolved '{}' -> '{}' (score {})", a_query, bestName, bestScore);
			return true;
		}
	}

	bool Resolve(const std::string& a_query, RE::Actor*& out, std::string& outName)
	{
		out = nullptr;
		outName.clear();
		if (a_query.empty()) return false;
		return ScoreSearchJson(a_query, SearchMerged(a_query, 12), out, outName);
	}

	void ResolveAsync(const std::string& a_query,
		std::function<void(RE::Actor*, std::string)> a_done)
	{
		if (!a_done) return;
		if (a_query.empty()) {
			a_done(nullptr, std::string());   // caller is the game thread (HandleAction)
			return;
		}
		SearchMergedAsync(a_query, 12,
			[query = a_query, done = std::move(a_done)](std::string json) mutable {
				RE::Actor* actor = nullptr;
				std::string name;
				(void)ScoreSearchJson(query, json, actor, name);
				done(actor, std::move(name));
			});
	}

	std::string SearchMerged(const std::string& a_query, int a_limit)
	{
		return MergeLocalRows(a_query, a_limit, skyrimnet::SearchActors(a_query, a_limit));
	}

	void SearchMergedAsync(const std::string& a_query, int a_limit,
		std::function<void(std::string)> a_done)
	{
		skyrimnet::SearchActorsAsync(a_query, a_limit,
			[query = a_query, limit = a_limit, done = std::move(a_done)](std::string json) mutable {
				// The bridge invokes this on the game thread, so the local
				// refresh + merge run there too.
				done(MergeLocalRows(query, limit, std::move(json)));
			});
	}
}
