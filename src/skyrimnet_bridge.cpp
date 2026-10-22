// IntelLite <-> SkyrimNet bridge (single translation unit).
//
// PublicAPI.h defines its function-pointer globals at namespace scope with
// external linkage (they are NOT inline), so it may be included in exactly ONE
// translation unit. This is that unit — no other .cpp includes it.

#include "skyrimnet_bridge.h"

#include "panel.h"
#include "npc_index.h"
#include "tasks.h"
#include "vendor/skyrimnet/PublicAPI.h"
#include "vm_dispatch.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace intellite::skyrimnet
{
	namespace
	{
		std::atomic<bool> g_ready{ false };
		std::atomic<int>  g_apiVersion{ -1 };

		// The Commission category: one LLM-facing category over the four task
		// actions, so the main selector lists one line instead of four and
		// SkyrimNet's drill-down prompt picks the exact action.
		constexpr const char* kCommissionName  = "Commission";
		constexpr const char* kCommissionToken = "commission";

		// SkyrimNet 0.28 action type: an action filed under a custom category by
		// customCategory. Any other value is a top-level action.
		constexpr const char* kNestedActionType = "CPP_NESTED_ACTION";

		// OStim's "in a scene" marker faction. Optional dependency: when it cannot
		// be resolved the gate is simply inactive.
		constexpr const char* kOStimFactionEditorID = "OStimExcitementFaction";

		// True only once the Commission category AND all four of its members have
		// registered as nested actions. Otherwise the category node would be empty
		// or partial, so it is made permanently ineligible and the members fall back
		// to top-level registration.
		std::atomic<bool> g_commissionUsable{ false };

		// The category description is FINAL user text - do not reword, repunctuate
		// or respace it. These additions stay ASCII: the em dash is emitted as its
		// UTF-8 bytes (U+2014) so SkyrimNet receives UTF-8 rather than a byte
		// re-encoded through the compiler's execution charset.
		constexpr const char* kCommissionDescription =
			"(One-off assignments category for assassinating a named target, delivering a message to another NPC, "
			"fetching an NPC to {{ player.name }}, or meeting {{ player.name }} later. Only when {{ npc.name }} "
			"clearly agrees to carry out the action"
			"\xE2\x80\x94"
			"not while discussing, considering, or negotiating.)";

		std::atomic<bool> g_hotkeyFailureLogged{ false };

		void LogHotkeyFailureOnce()
		{
			bool expected = false;
			if (g_hotkeyFailureLogged.compare_exchange_strong(expected, true)) {
				logs::warn("SkyrimNet: SetCppHotkeysEnabled dispatch failed");
			}
		}

		std::atomic<bool> g_nativeEventUnavailableLogged{ false };

		void LogNativeEventUnavailableOnce()
		{
			bool expected = false;
			if (g_nativeEventUnavailableLogged.compare_exchange_strong(expected, true)) {
				logs::warn("SkyrimNet: PublicRegisterEvent unavailable - events are not registered");
			}
		}

		std::atomic<std::uint32_t> g_nativeEventRejectedCount{ 0 };
		constexpr std::uint32_t    kMaxRejectedEventLogs = 16;

		void LogNativeEventRejected(const char* a_reason, const char* a_type,
			const std::string& a_content, RE::Actor* a_originator, RE::Actor* a_target,
			std::size_t a_audienceCount)
		{
			const std::uint32_t n = ++g_nativeEventRejectedCount;
			if (n <= kMaxRejectedEventLogs) {
				logs::warn(
					"SkyrimNet: PublicRegisterEvent rejected ({}) [#{}/{}] - type='{}' orig={:08X}(3d={}) target={:08X}(3d={}) audience={} memReady={} content='{}'",
					a_reason ? a_reason : "id=0",
					n, kMaxRejectedEventLogs,
					a_type ? a_type : "(null)",
					a_originator ? a_originator->GetFormID() : 0u,
					a_originator && a_originator->Is3DLoaded() ? 1 : 0,
					a_target ? a_target->GetFormID() : 0u,
					a_target && a_target->Is3DLoaded() ? 1 : 0,
					a_audienceCount,
					IsMemoryReady() ? 1 : 0,
					a_content);
			}
		}

		// Register one event through SkyrimNet's native PublicRegisterEvent
		// (API v11+). A non-empty audience list restricts perception to exactly
		// those actors plus the originator and target; an empty one uses the
		// default audience (everyone near the originator, plus the player). A
		// null actor is passed as none (FormID 0). Game thread only.
		void RegisterNativeEvent(const char* a_type, const std::string& a_content,
			RE::Actor* a_originator, RE::Actor* a_target,
			const std::vector<RE::Actor*>& a_audience)
		{
			if (!g_ready.load(std::memory_order_relaxed) || !PublicRegisterEvent) {
				LogNativeEventUnavailableOnce();
				return;
			}

			std::vector<std::uint32_t> audienceIDs;
			audienceIDs.reserve(a_audience.size());
			for (auto* actor : a_audience) {
				if (actor) audienceIDs.push_back(actor->GetFormID());
			}
			// An explicit audience that resolves to no valid actor must not
			// silently fall back to the default (nearby) audience.
			if (!a_audience.empty() && audienceIDs.empty()) {
				LogNativeEventRejected("empty resolved audience", a_type, a_content,
					a_originator, a_target, 0);
				return;
			}

			// SkyrimNet resolves event participants through live (3D-loaded)
			// actors, and with audienceCount 0 ("everyone near the originator,
			// plus the player") RegisterEventImpl also rejects any originator
			// beyond 2* the interaction distance from the player.
			//   - When the originator is near the player, use it directly.
			//   - When the originator is unloaded or far from the player (e.g.
			//     picked from the panel at 2500 u) and the target is near the
			//     player (e.g. the player themselves), anchor the nearby
			//     audience on the target.
			//   - When neither participant is near the player (e.g. a delivery
			//     between two actors across town), switch to an explicit
			//     audience of the loaded participants so SkyrimNet stores the
			//     event for them without running the nearby-player distance
			//     filter; if neither is loaded, skip the call.
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto isNearPlayer = [player](RE::Actor* a_actor) -> bool {
				if (!a_actor || !a_actor->Is3DLoaded() || !player) return false;
				auto* wa = a_actor->GetWorldspace();
				auto* wp = player->GetWorldspace();
				if (wa != wp) return false;
				if (wa == nullptr && a_actor->GetParentCell() != player->GetParentCell()) return false;
				return a_actor->GetPosition().GetDistance(player->GetPosition()) <= 1200.0f;
			};

			RE::Actor* effectiveOrig = a_originator;
			if (audienceIDs.empty()) {
				if (isNearPlayer(a_originator)) {
					effectiveOrig = a_originator;
				} else if (isNearPlayer(a_target)) {
					effectiveOrig = a_target;
				} else {
					if (a_originator && a_originator->Is3DLoaded()) {
						audienceIDs.push_back(a_originator->GetFormID());
						effectiveOrig = a_originator;
					}
					if (a_target && a_target->Is3DLoaded() && a_target != a_originator) {
						audienceIDs.push_back(a_target->GetFormID());
						if (!effectiveOrig || !effectiveOrig->Is3DLoaded()) {
							effectiveOrig = a_target;
						}
					}
					if (audienceIDs.empty()) {
						return;
					}
				}
			}

			const int id = PublicRegisterEvent(a_type, a_content.c_str(),
				effectiveOrig ? effectiveOrig->GetFormID() : 0,
				a_target ? a_target->GetFormID() : 0,
				audienceIDs.empty() ? nullptr : audienceIDs.data(),
				static_cast<std::uint32_t>(audienceIDs.size()));
			if (id == 0) {
				LogNativeEventRejected("id=0", a_type, a_content,
					a_originator, a_target, audienceIDs.size());
			}
		}

		std::atomic<bool> g_setActorStateFailureLogged{ false };

		void LogSetActorStateFailureOnce()
		{
			bool expected = false;
			if (g_setActorStateFailureLogged.compare_exchange_strong(expected, true)) {
				logs::warn("SkyrimNet: StorageUtil.SetStringValue dispatch failed");
			}
		}

		std::atomic<bool> g_setActorFloatFailureLogged{ false };

		void LogSetActorFloatFailureOnce()
		{
			bool expected = false;
			if (g_setActorFloatFailureLogged.compare_exchange_strong(expected, true)) {
				logs::warn("SkyrimNet bridge: StorageUtil.SetFloatValue dispatch failed (logged once)");
			}
		}

		std::atomic<bool> g_addWorldKnowledgeUnavailableLogged{ false };

		void LogAddWorldKnowledgeUnavailableOnce()
		{
			bool expected = false;
			if (g_addWorldKnowledgeUnavailableLogged.compare_exchange_strong(expected, true)) {
				logs::warn("SkyrimNet: PublicAddWorldKnowledge unavailable - world knowledge entries are not added");
			}
		}

		std::atomic<bool> g_getWorldKnowledgeUnavailableLogged{ false };

		void LogGetWorldKnowledgeUnavailableOnce()
		{
			bool expected = false;
			if (g_getWorldKnowledgeUnavailableLogged.compare_exchange_strong(expected, true)) {
				logs::warn("SkyrimNet: PublicGetWorldKnowledge unavailable - rumor dedupe disabled");
			}
		}

		std::atomic<bool> g_appendActorStringListFailureLogged{ false };

		void LogAppendActorStringListFailureOnce()
		{
			bool expected = false;
			if (g_appendActorStringListFailureLogged.compare_exchange_strong(expected, true)) {
				logs::warn("SkyrimNet bridge: StorageUtil.StringListAdd dispatch failed (logged once)");
			}
		}

		std::atomic<bool> g_appendActorFloatListFailureLogged{ false };

		void LogAppendActorFloatListFailureOnce()
		{
			bool expected = false;
			if (g_appendActorFloatListFailureLogged.compare_exchange_strong(expected, true)) {
				logs::warn("SkyrimNet bridge: StorageUtil.FloatListAdd dispatch failed (logged once)");
			}
		}
	}

	void Initialize()
	{
		if (!FindFunctions()) {
			logs::warn("SkyrimNet: not present (FindFunctions returned false)");
			g_ready.store(false, std::memory_order_relaxed);
			g_apiVersion.store(-1, std::memory_order_relaxed);
			return;
		}

		g_apiVersion.store(PublicGetVersion(), std::memory_order_relaxed);
		g_ready.store(true, std::memory_order_relaxed);
		logs::info("SkyrimNet: API resolved, PublicGetVersion() = {}", g_apiVersion.load(std::memory_order_relaxed));
		logs::info("SkyrimNet: native event API {}", PublicRegisterEvent ? "available" : "UNAVAILABLE (API < v11)");
	}

	bool IsReady()
	{
		return g_ready.load(std::memory_order_relaxed);
	}

	int ApiVersion()
	{
		return g_ready.load(std::memory_order_relaxed) ? g_apiVersion.load(std::memory_order_relaxed) : -1;
	}

	namespace
	{
		// Register one action with IntelLite's fixed priority/trigger. A non-empty
		// a_customCategory files the action inside that native category (the 0.28
		// nested action type); empty keeps it a top-level "Social" action.
		// Returns false when SkyrimNet is not ready or registration failed.
		bool RegisterAction(const char* a_name, const char* a_description,
			const char* a_schema, const char* a_customCategory,
			std::function<bool(RE::Actor*)> a_eligible,
			std::function<bool(RE::Actor*, std::string)> a_execute)
		{
			if (!g_ready.load(std::memory_order_relaxed) || !PublicRegisterCPPAction) return false;
			const bool nested = a_customCategory && *a_customCategory;
			return PublicRegisterCPPAction(
				a_name, a_description,
				std::move(a_eligible), std::move(a_execute),
				"dialogue", nested ? kNestedActionType : "Social", 50,
				a_schema, nested ? a_customCategory : "", "", "");
		}

		// Eligibility reads only the Actor's FormID (a plain field read; no
		// game-state lookup) and then consults the task engine's guarded state,
		// so it is safe on SkyrimNet's calling thread.
		std::uint32_t FormIDOf(RE::Actor* a_actor)
		{
			return a_actor ? a_actor->GetFormID() : 0;
		}

		std::atomic<RE::TESFaction*> g_ostimFaction{ nullptr };

		// Resolve OStim's scene-marker faction once, on the game thread at
		// kDataLoaded. OStim is an optional dependency: when the faction cannot be
		// found the eligibility gate is simply inactive.
		RE::TESFaction* ResolveOStimFaction()
		{
			RE::TESFaction* faction = RE::TESForm::LookupByEditorID<RE::TESFaction>(kOStimFactionEditorID);
			if (!faction) {
				if (auto* dh = RE::TESDataHandler::GetSingleton()) {
					for (auto* candidate : dh->GetFormArray<RE::TESFaction>()) {
						if (candidate && npc_index::FormEditorID(candidate) == kOStimFactionEditorID) {
							faction = candidate;
							break;
						}
					}
				}
			}
			g_ostimFaction.store(faction, std::memory_order_relaxed);
			logs::info("SkyrimNet: OStim scene gate {} ({})", faction ? "active" : "inactive",
				faction ? "OStimExcitementFaction resolved" : "faction not found");
			return faction;
		}

		// Eligibility for the four commission actions and their category: the
		// speaker is not locked by a kill already under way, is not in combat,
		// and is not in an OStim scene; an active task does not block because a
		// new task replaces it. Runs on SkyrimNet workers; the two actor reads are
		// the same ones SkyrimNet's own is_in_combat / get_faction_rank decorators
		// perform there.
		bool CanTakeCommission(RE::Actor* a_actor)
		{
			const std::uint32_t id = FormIDOf(a_actor);
			if (id == 0) return false;
			if (tasks::HasCommittedKillTask(id)) return false;
			if (a_actor->IsInCombat()) return false;
			auto* faction = g_ostimFaction.load(std::memory_order_relaxed);
			if (faction && a_actor->GetFactionRank(faction, false) >= 0) return false;
			return true;
		}

		// A FRESH callable on every call: RegisterAction moves the callable into
		// SkyrimNet's registration, so one instance must never be shared.
		std::function<bool(RE::Actor*)> CommissionEligible()
		{
			return [](RE::Actor* a) -> bool {
				return CanTakeCommission(a);
			};
		}

		// Register the Commission category node: one LLM-facing entry that
		// SkyrimNet drills into for the four task actions. Eligible only once all
		// four members registered nested (g_commissionUsable), so a partial
		// category is never offered.
		bool RegisterCategory()
		{
			if (!g_ready.load(std::memory_order_relaxed) || !PublicRegisterCPPSubCategory) return false;
			return PublicRegisterCPPSubCategory(
				kCommissionName, kCommissionDescription,
				[](RE::Actor* a) -> bool {
					return g_commissionUsable.load(std::memory_order_relaxed) && CanTakeCommission(a);
				},
				"dialogue", 50, "{}", kCommissionToken, "", "");
		}

		// The execute callback copies the initiating FormID and the params JSON
		// and hands them to the panel, which posts the real work to the game
		// thread. It touches no game state itself.
		std::function<bool(RE::Actor*, std::string)> MakeExecute(const char* a_kind)
		{
			const std::string kind = a_kind;
			return [kind](RE::Actor* a_actor, std::string a_params) -> bool {
				const std::uint32_t agentID = FormIDOf(a_actor);
				if (agentID == 0) return false;
				panel::RequestAction(kind, agentID, a_params);
				return true;
			};
		}
	}

	int RegisterActions()
	{
		if (!g_ready.load(std::memory_order_relaxed) || !PublicRegisterCPPAction) {
			logs::warn("SkyrimNet: action registration skipped (API not ready)");
			return 0;
		}

		// The optional OStim scene faction is resolved once here, on the game
		// thread, before any eligibility callback can consult it.
		ResolveOStimFaction();

		int count = 0;

		const bool categoryOk = RegisterCategory();
		logs::info("SkyrimNet: category '{}' registration {}", kCommissionName, categoryOk);

		// The four task actions: nested in the Commission category when it and
		// the nested registration are available, otherwise top-level.
		int nested = 0;
		auto registerCommission = [&](const char* a_name, const char* a_description,
			const char* a_schema, const char* a_kind) -> bool {
			bool ok = false;
			if (categoryOk) {
				ok = RegisterAction(a_name, a_description, a_schema, kCommissionToken,
					CommissionEligible(), MakeExecute(a_kind));
				if (ok) {
					++nested;
				} else {
					logs::warn("SkyrimNet: {} nested registration failed - retrying top-level", a_name);
				}
			}
			if (!ok) {
				ok = RegisterAction(a_name, a_description, a_schema, "",
					CommissionEligible(), MakeExecute(a_kind));
			}
			if (ok) ++count;
			logs::info("SkyrimNet: {} registration {}", a_name, ok);
			return ok;
		};

		registerCommission(
			"FetchPerson",
			"Fetch a person to {{ player.name }}.",
			R"json({"target":"Name of the person to fetch"})json",
			"fetch");

		registerCommission(
			"DeliverMessage",
			"Deliver a message from one person to another.",
			R"json({"message":"The message to deliver","sender":"Sender's name (omit when it is from the actor themselves)","target":"Name of the message recipient"})json",
			"deliver");

		registerCommission(
			"MeetLater",
			"Arrange to meet {{ player.name }} at a place later.",
			R"json({"hours":"Hours from now (1-48)","location":"Any named location (e.g. Bannered Mare); omit to meet at the spot where the player is standing"})json",
			"meet");

		registerCommission(
			"Assassinate",
			"Accept a contract to kill a named person, for an optional fee.",
			R"json({"fee":"Gold the player pays the killer; 0 or omitted for no fee","target":"Name of the person to kill"})json",
			"assassinate");

		g_commissionUsable.store(categoryOk && nested == 4, std::memory_order_relaxed);

		{
			const bool ok = RegisterAction(
				"CancelAll",
				"Cancel the tasks you are currently running: fetching a person, delivering a message, meeting {{ player.name }}, or an assassination that has not yet reached its target. New tasks replace the current one automatically.",
				"",
				"",
				[](RE::Actor* a) -> bool {
					const std::uint32_t id = FormIDOf(a);
					return id != 0 && tasks::HasCancellableTask(id);
				},
				MakeExecute("cancelmine"));
			if (ok) ++count;
			logs::info("SkyrimNet: CancelAll registration {}", ok);
		}

		logs::info("SkyrimNet: registered {} IntelLite action(s) (category '{}' {})", count, kCommissionName,
			g_commissionUsable.load(std::memory_order_relaxed) ? "active" : "inactive");
		return count;
	}

	bool SetCppHotkeysEnabled(bool enabled)
	{
		// SkyrimNetApi.SetCppHotkeysEnabled(bool) -> int (0 = success). The VM
		// takes ownership of the argument block; a rejected dispatch (class or
		// function not found, no VM) runs nothing and returns false.
		auto* args = RE::MakeFunctionArguments(static_cast<bool>(enabled));
		if (!vm::DispatchStatic("SkyrimNetApi", "SetCppHotkeysEnabled", args)) {
			LogHotkeyFailureOnce();
			return false;
		}
		return true;
	}

	void RegisterPersistentEvent(const std::string& content, RE::Actor* originator, RE::Actor* target,
		const std::vector<RE::Actor*>& audience)
	{
		RegisterNativeEvent("persistent_generic", content, originator, target, audience);
	}

	void DirectNarration(const std::string& content, RE::Actor* originator, RE::Actor* target,
		const std::vector<RE::Actor*>& audience)
	{
		RegisterNativeEvent("direct_narration", content, originator, target, audience);
	}

	void SetActorState(RE::Actor* actor, const std::string& key, const std::string& value)
	{
		// StorageUtil.SetStringValue(Form ObjKey, string KeyName, string value)
		// -> string. The actor is passed as its Form base (the parameter's
		// declared type); an empty value clears the key. Args are prvalues so
		// the pack traits accept them.
		auto* args = RE::MakeFunctionArguments(
			static_cast<RE::TESForm*>(actor),
			std::string(key),
			std::string(value));
		if (!vm::DispatchStatic("StorageUtil", "SetStringValue", args)) {
			LogSetActorStateFailureOnce();
		}
	}

	void SetActorFloat(RE::Actor* actor, const std::string& key, float value)
	{
		// StorageUtil.SetFloatValue(Form ObjKey, string KeyName, float value)
		// -> float. Args are prvalues so the pack traits accept them.
		auto* args = RE::MakeFunctionArguments(
			static_cast<RE::TESForm*>(actor),
			std::string(key),
			static_cast<float>(value));
		if (!vm::DispatchStatic("StorageUtil", "SetFloatValue", args)) {
			LogSetActorFloatFailureOnce();
		}
	}

	int AddWorldKnowledge(const std::string& content, const std::string& conditionExpr,
		bool alwaysInject, float importance, const std::string& displayName)
	{
		if (!g_ready.load(std::memory_order_relaxed) || !PublicAddWorldKnowledge) {
			LogAddWorldKnowledgeUnavailableOnce();
			return 0;
		}
		if (!IsMemoryReady()) return 0;
		return PublicAddWorldKnowledge(content.c_str(), conditionExpr.c_str(),
			alwaysInject, importance, displayName.c_str());
	}

	// Find world knowledge entries whose display name is EXACTLY displayName,
	// over PublicGetWorldKnowledge (API v7+). A dedupe helper for callers that
	// must not create a duplicate entry. GAME THREAD ONLY.
	std::vector<int> FindWorldKnowledgeByDisplayName(const std::string& displayName)
	{
		if (!g_ready.load(std::memory_order_relaxed) || PublicGetWorldKnowledge == nullptr) {
			LogGetWorldKnowledgeUnavailableOnce();
			return {};
		}
		if (!IsMemoryReady()) return {};

		// The API's own default (maxCount <= 0) is only 100 entries; scan a
		// generous but bounded window so a match is not missed. A list that
		// fills the window warns, so a miss is never silently assumed absent.
		constexpr int kScanMax = 4096;
		const std::string raw = PublicGetWorldKnowledge(kScanMax);
		if (raw.empty()) return {};

		nlohmann::json parsed;
		try {
			parsed = nlohmann::json::parse(raw);
		} catch (...) {
			logs::warn("SkyrimNet: PublicGetWorldKnowledge returned unparseable JSON");
			return {};
		}
		if (!parsed.is_array()) return {};

		if (parsed.size() >= static_cast<std::size_t>(kScanMax)) {
			logs::warn("SkyrimNet: world knowledge list hit the {} scan cap - dedupe may have missed entries",
				kScanMax);
		}

		std::vector<int> ids;
		for (const auto& entry : parsed) {
			if (!entry.is_object()) continue;
			auto nameIt = entry.find("display_name");
			if (nameIt == entry.end() || !nameIt->is_string()) continue;
			if (nameIt->get<std::string>() != displayName) continue;
			auto idIt = entry.find("id");
			if (idIt == entry.end() || !idIt->is_number()) continue;
			ids.push_back(idIt->get<int>());
		}
		return ids;
	}

	void AppendActorStringList(RE::Actor* actor, const std::string& key, const std::string& value)
	{
		// StorageUtil.StringListAdd(Form ObjKey, string KeyName, string value,
		// bool allowDuplicate = true) -> int. The actor is passed as its Form
		// base; allowDuplicate is passed explicitly as true so repeated
		// messages are all kept. Args are prvalues so the pack traits accept
		// them.
		auto* args = RE::MakeFunctionArguments(
			static_cast<RE::TESForm*>(actor),
			std::string(key),
			std::string(value),
			true);
		if (!vm::DispatchStatic("StorageUtil", "StringListAdd", args)) {
			LogAppendActorStringListFailureOnce();
		}
	}

	void AppendActorFloatList(RE::Actor* actor, const std::string& key, float value)
	{
		// StorageUtil.FloatListAdd(Form ObjKey, string KeyName, float value,
		// bool allowDuplicate = true) -> int. Args are prvalues so the pack
		// traits accept them; allowDuplicate is passed explicitly as true.
		auto* args = RE::MakeFunctionArguments(
			static_cast<RE::TESForm*>(actor),
			std::string(key),
			static_cast<float>(value),
			true);
		if (!vm::DispatchStatic("StorageUtil", "FloatListAdd", args)) {
			LogAppendActorFloatListFailureOnce();
		}
	}

	bool IsMemoryReady()
	{
		return g_ready.load(std::memory_order_relaxed) && PublicIsMemorySystemReady != nullptr && PublicIsMemorySystemReady();
	}

	namespace
	{
		// Parse a raw PublicSearchActors JSON reply and enrich each row with the
		// per-row RE:: data (humanoid + dead filter + editorid). GAME THREAD ONLY. A null/
		// empty/ unparseable input yields "[]".
		std::string EnrichSearchJson(const std::string& a_raw)
		{
			if (a_raw.empty()) return "[]";

			nlohmann::json parsed;
			try {
				parsed = nlohmann::json::parse(a_raw);
			} catch (...) {
				logs::warn("SkyrimNet: SearchActors returned unparseable JSON");
				return "[]";
			}
			if (!parsed.is_array()) return "[]";

			// Best-effort row filter: an entry must resolve to a live, non-player
			// Actor whose race carries Skyrim's ActorTypeNPC keyword (if the
			// keyword cannot be resolved the humanoid part is skipped).
			RE::BGSKeyword* npcKw = npc_index::HumanoidKeyword();

			nlohmann::json out = nlohmann::json::array();
			for (const auto& entry : parsed) {
				if (!entry.is_object()) continue;

				std::uint64_t uuid = 0;
				if (auto it = entry.find("uuid"); it != entry.end() && it->is_number()) {
					uuid = it->get<std::uint64_t>();
				}

				std::uint32_t formId = 0;
				if (auto it = entry.find("formId"); it != entry.end() && it->is_number()) {
					formId = it->get<std::uint32_t>();
				}
				if (formId == 0 && uuid != 0 && PublicUUIDToFormID) {
					formId = PublicUUIDToFormID(uuid);
				}

				std::string name;
				if (auto it = entry.find("name"); it != entry.end() && it->is_string()) {
					name = it->get<std::string>();
				}
				if (name.empty() && uuid != 0 && PublicGetActorNameByUUID) {
					name = PublicGetActorNameByUUID(uuid);
				}

				if (formId == 0) continue;  // nothing to resolve against

				auto* actor = RE::TESForm::LookupByID<RE::Actor>(formId);
				if (!actor || actor->IsDeleted()) continue;
				if (actor->IsPlayerRef()) continue;
				if (npcKw) {
					auto* race = actor->GetRace();
					if (!race || !race->HasKeyword(npcKw)) continue;
				}
				// Dead actors are not valid targets (the resolver and the nearby
				// list already exclude them) - drop the row.
				if (actor->IsDead()) continue;
				std::string editorid = npc_index::BaseEditorID(actor);

				nlohmann::json row;
				row["name"]     = name.empty() ? "Unknown" : name;
				row["formId"]   = formId;
				row["editorid"] = editorid;
				out.push_back(std::move(row));
			}
			return out.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
		}

		struct AsyncSearchJob
		{
			std::string                      query;
			int                              limit = 0;
			std::function<void(std::string)> done;
		};

		std::mutex                  g_searchQueueMutex;
		std::vector<AsyncSearchJob> g_searchQueue;
		bool                        g_searchWorkerRunning = false;

		void RunSearchWorker()
		{
			for (;;) {
				AsyncSearchJob job;
				{
					std::lock_guard lock(g_searchQueueMutex);
					if (g_searchQueue.empty()) {
						g_searchWorkerRunning = false;
						return;
					}
					job = std::move(g_searchQueue.front());
					g_searchQueue.erase(g_searchQueue.begin());
				}

				std::string raw = "[]";
				if (g_ready.load(std::memory_order_relaxed) && PublicSearchActors != nullptr && IsMemoryReady()) {
					raw = PublicSearchActors(job.query.c_str(), job.limit);
					if (raw.empty()) raw = "[]";
				}
				auto* tasks = SKSE::GetTaskInterface();
				if (!tasks) {
					logs::warn("SkyrimNet: SearchActorsAsync dropped (no task interface)");
					continue;
				}
				tasks->AddTask([raw = std::move(raw), done = std::move(job.done)]() mutable {
					done(EnrichSearchJson(raw));  // game thread
				});
			}
		}
	}

	std::string SearchActors(const std::string& query, int limit)
	{
		if (!g_ready.load(std::memory_order_relaxed) || PublicSearchActors == nullptr || !IsMemoryReady()) {
			return "[]";
		}
		return EnrichSearchJson(PublicSearchActors(query.c_str(), limit));
	}

	void SearchActorsAsync(const std::string& a_query, int a_limit, std::function<void(std::string)> a_done)
	{
		if (!a_done) return;

		// Serialized on at most one worker thread: PublicSearchActors runs off the
		// game thread, and the RE:: enrichment + callback are posted to the game
		// thread via SKSE::GetTaskInterface().
		bool spawn = false;
		{
			std::lock_guard lock(g_searchQueueMutex);
			g_searchQueue.push_back(AsyncSearchJob{ a_query, a_limit, std::move(a_done) });
			if (!g_searchWorkerRunning) {
				g_searchWorkerRunning = true;
				spawn = true;
			}
		}
		if (spawn) {
			std::thread(&RunSearchWorker).detach();
		}
	}
}
