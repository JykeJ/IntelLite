// IntelLite's MagelightUI panel.
//
// Threading contract (MagelightUI_API.h): JS listeners run INLINE ON THE RENDER
// THREAD mid-frame. Magelight calls are safe from any thread, but any RE:: access
// from a listener must be handed to the game thread with PostGameTask (host
// 0.30.1+) — never SKSE's AddTask/AddUITask, which can hang the game when called
// from inside a frame on Skyrim VR. This module touches no game state except
// inside the posted game tasks.

#include "panel.h"

#include "skyrimnet_bridge.h"
#include "tasks.h"
#include "places.h"
#include "actors.h"
#include "fuzzy.h"
#include "hotkey.h"
#include "npc_index.h"
#include "vendor/magelight/MagelightUI_API.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace intellite::panel
{
	namespace
	{
		constexpr const char*    kModId          = "IntelLite";
		constexpr const char*    kDisplayName    = "IntelLite";
		constexpr const char*    kModVersion     = "0.1.0";
		constexpr std::int32_t   kPanelW         = 1000;
		constexpr std::int32_t   kPanelH         = 700;
		constexpr std::int32_t   kPromptW        = 640;
		constexpr std::int32_t   kPromptH        = 420;
		constexpr float          kNearbyRadius   = 3000.0f;
		constexpr std::size_t    kNearbyMax      = 50;

		// Raw Magelight API pointers plus our mod registration. We use the raw
		// ABI rather than MagelightUI_Mod.h's wrapper so the mod-level event
		// sink (UIModeEntered/Exited/Refused, ViewLoadFailed) is ours: the
		// wrapper only demultiplexes per-view close/error.
		const MAGELIGHT_API::MagelightApi1* g_v1        = nullptr;
		const MAGELIGHT_API::MagelightApi4* g_v4        = nullptr;
		MAGELIGHT_API::ModId                g_modId     = 0;
		MAGELIGHT_API::ViewId               g_view      = 0;
		MAGELIGHT_API::ViewId               g_promptView = 0;

		// Last BindPanelHotkey failure (empty on success); pushed to the page as
		// status.hotkeyError.
		std::mutex  g_hotkeyErrorMutex;
		std::string g_hotkeyError;

		bool g_panelInUIMode  = false;
		bool g_promptInUIMode = false;

		std::string SafeDump(const nlohmann::json& a_json)
		{
			return a_json.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
		}

		// The set of views IntelLite owns: the panel and the consent prompt.
		// Both keep the SkyrimNet hotkey suppression in OnModEvent.
		bool IsOurView(MAGELIGHT_API::ViewId a_view)
		{
			return a_view != 0 && (a_view == g_view || a_view == g_promptView);
		}

		// Defined with the prompt state further down; called on every UI-mode
		// exit so an Escape (which the host turns into a UI-mode exit) clears
		// the pending request exactly as the Decline button does.
		void ClearPendingPrompt();

		// Defined with the nearby scan further down; declared here so the
		// UI-mode entry below can refresh the list when the panel opens.
		void RequestNearbyOnGameThread();

		// Defined with the JSON helpers further down; used by the panel action
		// path above it.
		bool JsonInt(const nlohmann::json& a_obj, const char* a_key, std::int64_t& a_out);

		// ModDesc.onEvent sink. Runs on the game thread (callbackThread =
		// GameThread), so the SkyrimNet VM dispatch below is safe. On UI-mode
		// entry we suppress SkyrimNet's native hotkeys so e.g. "G" cannot open
		// its text input over our panel; on every exit path we restore them and
		// hide the view.
		void OnModEvent(const MAGELIGHT_API::EventData* ev, void* /*user*/)
		{
			if (!ev) return;

			switch (ev->type) {
			case MAGELIGHT_API::Event::UIModeEntered:
				if (IsOurView(ev->view)) {
					if (g_view && ev->view == g_view) g_panelInUIMode = true;
					if (g_promptView && ev->view == g_promptView) g_promptInUIMode = true;
					skyrimnet::SetCppHotkeysEnabled(false);
					// The page persists across panel opens (panelReady fires
					// only once), so re-scan the nearby list on every open.
					if (g_view && ev->view == g_view) {
						RequestNearbyOnGameThread();
					}
				}
				break;
			case MAGELIGHT_API::Event::UIModeExited:
			case MAGELIGHT_API::Event::UIModeRefused:
				if (IsOurView(ev->view)) {
					if (g_view && ev->view == g_view) g_panelInUIMode = false;
					if (g_promptView && ev->view == g_promptView) g_promptInUIMode = false;
					if (!g_panelInUIMode && !g_promptInUIMode) {
						skyrimnet::SetCppHotkeysEnabled(true);
					}
					// Escape arrives here (a UI-mode exit), so treat any exit of
					// the prompt as a decline: drop the pending request.
					if (g_promptView && ev->view == g_promptView) {
						ClearPendingPrompt();
					}
					if (g_v4) g_v4->ShowView(ev->view, false);
				}
				break;
			case MAGELIGHT_API::Event::ViewLoadFailed:
				logs::error("IntelLite panel: view {} load failed - {}",
					ev->view, ev->detail ? ev->detail : "");
				break;
			default:
				break;
			}
		}

		std::string PagePath()
		{
			auto p = fuzzy::ExeDir()
			       / L"Data" / L"Magelight" / L"IntelLite" / L"views" / L"panel" / L"index.html";
			return fuzzy::PathToUtf8(p);
		}

		std::string PromptPagePath()
		{
			auto p = fuzzy::ExeDir()
			       / L"Data" / L"Magelight" / L"IntelLite" / L"views" / L"prompt" / L"index.html";
			return fuzzy::PathToUtf8(p);
		}

		// Best-effort centering. GetDisplaySize is 0,0 before the first frame, so
		// at kDataLoaded this is a no-op and panelReady (which fires after the view
		// is shown and the page has loaded) does the real centering.
		void CenterView()
		{
			if (!g_view || !g_v4) return;

			std::int32_t sw = 0;
			std::int32_t sh = 0;
			g_v4->GetDisplaySize(&sw, &sh);
			if (sw <= 0 || sh <= 0) return;

			const std::int32_t w = std::min<std::int32_t>(kPanelW, sw);
			const std::int32_t h = std::min<std::int32_t>(kPanelH, sh);
			g_v4->SetViewBounds(g_view, (sw - w) / 2, (sh - h) / 2, w, h);
		}

		// Post game-state work onto the game thread. From a JS listener the host
		// PostGameTask path is hang-safe (0.30.1+); on an older host - or when
		// Magelight is absent entirely (a SkyrimNet action still runs) - hand
		// the post to a thread of our own (never SKSE AddTask from inside a
		// frame).
		bool PostToGameThread(MAGELIGHT_API::GameTaskFn fn, void* user)
		{
			if (!fn) return false;

			if (g_v4 && g_v4->hostVersionNumber >= 3001) {
				if (g_v4->PostGameTask(fn, user) != MAGELIGHT_API::Result::Ok) {
					// fn never runs; the caller's heap user pointer would leak.
					logs::warn("IntelLite panel: PostGameTask refused");
					return false;
				}
				return true;
			}

			// On this fallback the detached thread cannot report back, so the
			// caller's payload ownership follows the return value: false means fn
			// never ran and the caller must free its payload; true hands it off.
			if (!SKSE::GetTaskInterface()) {
				return false;
			}
			std::thread([fn, user]() {
				if (auto* tasks = SKSE::GetTaskInterface()) {
					tasks->AddTask([fn, user]() { fn(user); });
				}
			}).detach();
			return true;
		}

		// GameTaskFn is a plain function pointer, so the posted work is a free
		// function (PushNearby is declared in panel.h).
		void OnGameTaskPushNearby(void* /*user*/)
		{
			PushNearby();
		}

		// Schedule the RE::-touching scan on the game thread. Called from JS
		// listeners, which run on the render thread.
		void RequestNearbyOnGameThread()
		{
			(void)PostToGameThread(&OnGameTaskPushNearby, nullptr);
		}

		// window.magelight.send('executeAction', <json>) —
		//   fetch:       {action:'fetch', agentFormId, targetFormId}
		//   deliver:     {action:'deliver', agentFormId, targetFormId, message}
		//   meet:        {action:'meet', agentFormId, locationName, hours}
		//   assassinate: {action:'assassinate', agentFormId, targetFormId, fee}
		struct ExecuteArgs
		{
			std::string   action;
			std::uint32_t agentFormId = 0;
			std::uint32_t targetFormId = 0;
			std::string   message;
			std::string   sender;
			std::string   locationName;
			int           hours = 0;
			std::uint32_t fee = 0;
		};

		constexpr std::size_t kMaxMessageLen = 200;
		constexpr std::size_t kMaxSenderLen  = 64;

		void OnGameTaskExecute(void* user)
		{
			std::unique_ptr<ExecuteArgs> args(static_cast<ExecuteArgs*>(user));
			if (!args) return;

			auto* agent = RE::TESForm::LookupByID<RE::Actor>(args->agentFormId);
			if (!agent) {
				logs::warn("IntelLite tasks: execute failed - executor lookup ({})",
					args->agentFormId);
				tasks::PushToPanelFresh();
				return;
			}

			bool ok = false;
			if (args->action == "deliver") {
				auto* recipient = RE::TESForm::LookupByID<RE::Actor>(args->targetFormId);
				if (!recipient) {
					logs::warn("IntelLite tasks: execute failed - recipient lookup ({})",
						args->targetFormId);
					tasks::PushToPanelFresh();
					return;
				}
				ok = tasks::AddDeliver(agent, recipient, args->message, args->sender);
			} else if (args->action == "meet") {
				ok = tasks::AddMeet(agent, args->locationName, args->hours);
			} else if (args->action == "assassinate") {
				auto* target = RE::TESForm::LookupByID<RE::Actor>(args->targetFormId);
				if (!target) {
					logs::warn("IntelLite tasks: execute failed - target lookup ({})",
						args->targetFormId);
					tasks::PushToPanelFresh();
					return;
				}
				// AddAssassinate performs the single fee payment (refusing with a
				// HUD when the player cannot afford it).
				ok = tasks::AddAssassinate(agent, target, args->fee);
			} else if (args->action == "fetch") {
				auto* target = RE::TESForm::LookupByID<RE::Actor>(args->targetFormId);
				if (!target) {
					logs::warn("IntelLite tasks: execute failed - target lookup ({})",
						args->targetFormId);
					tasks::PushToPanelFresh();
					return;
				}
				ok = tasks::AddFetch(agent, target);
			}

			logs::info("IntelLite tasks: execute {} (agent {}) ({})",
				args->action, args->agentFormId, ok ? "queued" : "refused");
			tasks::PushToPanelFresh();
		}

		void OnExecuteAction(MAGELIGHT_API::ViewId /*view*/, const char* arg, void* /*user*/)
		{
			if (!arg) return;

			nlohmann::json j;
			try {
				j = nlohmann::json::parse(arg);
			} catch (...) {
				logs::warn("IntelLite panel: executeAction bad JSON");
				return;
			}
			if (!j.is_object()) return;

			std::string action;
			if (auto it = j.find("action"); it != j.end() && it->is_string()) {
				action = it->get<std::string>();
			}
			if (action != "fetch" && action != "deliver" && action != "meet" &&
				action != "assassinate") {
				return;
			}

			auto readId = [&j](const char* key) -> std::uint32_t {
				if (auto it = j.find(key); it != j.end() && it->is_number_unsigned()) {
					return it->get<std::uint32_t>();
				}
				if (auto it = j.find(key); it != j.end() && it->is_number_integer()) {
					const auto v = it->get<std::int64_t>();
					return v > 0 ? static_cast<std::uint32_t>(v) : 0u;
				}
				return 0u;
			};
			auto readString = [&j](const char* key) -> std::string {
				if (auto it = j.find(key); it != j.end() && it->is_string()) {
					return it->get<std::string>();
				}
				return std::string();
			};

			auto* args = new ExecuteArgs{};
			args->action      = action;
			args->agentFormId = readId("agentFormId");

			if (action == "meet") {
				args->locationName = readString("locationName");
				std::int64_t hoursRaw = 0;
				if (JsonInt(j, "hours", hoursRaw) &&
					hoursRaw >= INT32_MIN && hoursRaw <= INT32_MAX) {
					args->hours = static_cast<int>(hoursRaw);
				}
				if (args->agentFormId == 0 || args->locationName.empty() ||
					args->hours < 1 || args->hours > 48) {
					delete args;
					return;
				}
			} else {
				args->targetFormId = readId("targetFormId");
				if (action == "deliver") {
					args->message = readString("message");
					if (args->message.empty() || args->message.size() > kMaxMessageLen) {
						delete args;
						return;
					}
					args->sender = fuzzy::ClampUtf8(readString("sender"), kMaxSenderLen);
				} else if (action == "assassinate") {
					// The fee is optional: a missing/zero value means no fee.
					std::int64_t feeRaw = 0;
					if (JsonInt(j, "fee", feeRaw)) {
						if (feeRaw > 0 && feeRaw <= INT32_MAX) args->fee = static_cast<std::uint32_t>(feeRaw);
					}
				}
				if (args->agentFormId == 0 || args->targetFormId == 0) {
					delete args;
					return;
				}
			}
			if (!PostToGameThread(&OnGameTaskExecute, args)) delete args;
		}

		// window.magelight.send('searchActors', <query or {role, query}>).
		struct SearchArgs
		{
			std::string role = "target";
			std::string query;
		};

		void OnGameTaskSearch(void* user)
		{
			std::unique_ptr<SearchArgs> args(static_cast<SearchArgs*>(user));
			if (!args) return;

			const std::string json = actors::SearchMerged(args->query, 10);
			if (args->role == "executor") {
				PushExecutorSearchResults(json);
			} else {
				PushSearchResults(json);
			}
		}

		void OnSearchActors(MAGELIGHT_API::ViewId /*view*/, const char* arg, void* /*user*/)
		{
			auto* args = new SearchArgs{};
			if (arg && *arg) {
				bool parsedObj = false;
				if (arg[0] == '{') {
					try {
						const auto j = nlohmann::json::parse(arg);
						if (j.is_object()) {
							parsedObj = true;
							if (auto it = j.find("role"); it != j.end() && it->is_string()) {
								args->role = it->get<std::string>();
							}
							if (auto it = j.find("query"); it != j.end() && it->is_string()) {
								args->query = it->get<std::string>();
							}
						}
					} catch (...) {
						parsedObj = false;
					}
				}
				if (!parsedObj) {
					args->query = arg;
				}
			}
			if (args->role != "executor") {
				args->role = "target";
			}
			if (!PostToGameThread(&OnGameTaskSearch, args)) delete args;
		}

		// window.magelight.send('requestTasks','') — pull the current task list.
		// Pure snapshot: no game thread needed.
		void OnRequestTasks(MAGELIGHT_API::ViewId /*view*/, const char* /*arg*/, void* /*user*/)
		{
			tasks::PushToPanel();
		}

		// Strict unsigned parse: the whole string must be digits (no trailing junk)
		// and the value must not exceed a_max; returns false on anything else.
		bool ParseU32Strict(const char* a_text, std::uint32_t& a_out, std::uint32_t a_max = 0xFFFFFFFFu)
		{
			if (!a_text || !*a_text) return false;
			std::uint64_t v = 0;
			for (const char* p = a_text; *p; ++p) {
				if (*p < '0' || *p > '9') return false;
				v = v * 10 + static_cast<unsigned>(*p - '0');
				if (v > a_max) return false;
			}
			a_out = static_cast<std::uint32_t>(v);
			return true;
		}

		// Read an integer parameter from either a JSON number or a numeric
		// string. SkyrimNet hands native action params to the callback as the
		// JSON text the LLM wrote ({"hours":"8"}), while the panel's JS sends
		// real numbers; both must work. A string must be digits-only after
		// trimming ASCII whitespace.
		bool JsonInt(const nlohmann::json& a_obj, const char* a_key, std::int64_t& a_out)
		{
			const auto it = a_obj.find(a_key);
			if (it == a_obj.end()) return false;
			if (it->is_number_unsigned()) {
				const auto v = it->get<std::uint64_t>();
				if (v > static_cast<std::uint64_t>(INT64_MAX)) return false;
				a_out = static_cast<std::int64_t>(v);
				return true;
			}
			if (it->is_number_integer()) {
				a_out = it->get<std::int64_t>();
				return true;
			}
			if (it->is_number_float()) {
				const double d = it->get<double>();
				if (!(d >= -9.2e18 && d <= 9.2e18)) return false;
				a_out = static_cast<std::int64_t>(d);
				return true;
			}
			if (it->is_string()) {
				std::string s = it->get<std::string>();
				const auto first = s.find_first_not_of(" \t\r\n");
				const auto last = s.find_last_not_of(" \t\r\n");
				if (first == std::string::npos) return false;
				s = s.substr(first, last - first + 1);
				std::uint32_t v = 0;
				if (!ParseU32Strict(s.c_str(), v, 1000000000u)) return false;
				a_out = static_cast<std::int64_t>(v);
				return true;
			}
			return false;
		}

		// window.magelight.send('cancelTask', '<id>') — queue a cancellation. The
		// next pump Tick() applies it on the game thread.
		void OnCancelTask(MAGELIGHT_API::ViewId /*view*/, const char* arg, void* /*user*/)
		{
			if (!arg || !*arg) return;
			std::uint32_t id = 0;
			if (!ParseU32Strict(arg, id)) return;
			if (id == 0) return;
			tasks::Cancel(id);
		}

		// window.magelight.send('panelReady','') — page has loaded: re-center,
		// push the status, and kick off a nearby scan.
		void OnPanelReady(MAGELIGHT_API::ViewId /*view*/, const char* /*arg*/, void* /*user*/)
		{
			CenterView();
			PushStatus();
			RequestNearbyOnGameThread();
		}

		// window.magelight.send('setConfirm','1'|'0') — the Settings tab's
		// confirmation checkbox. Updates the (thread-safe) setting and re-pushes
		// the status so the page reflects the stored value.
		void OnSetConfirm(MAGELIGHT_API::ViewId /*view*/, const char* arg, void* /*user*/)
		{
			const bool enabled = (arg && arg[0] == '1');
			tasks::SetConfirmPromptsEnabled(enabled);
			PushStatus();
		}

		// Bind the stored key to the panel view through the host: unbind every
		// previous binding of the view, then bind the DirectInput scancode with
		// the paused-UI-mode toggle. The host fires it outside engine menus, and
		// while the page has key focus only the view's own key fires (close).
		void BindPanelHotkey()
		{
			{
				std::lock_guard lock(g_hotkeyErrorMutex);
				g_hotkeyError.clear();
			}
			if (!g_v4 || !g_view || !g_v4->BindHotkey) return;
			g_v4->BindHotkey(g_view, 0, MAGELIGHT_API::kHotkeyActionUnbind);  // scancode 0 = every binding of the view
			const auto r = g_v4->BindHotkey(g_view, hotkey::Scancode(), MAGELIGHT_API::kHotkeyActionToggleUIModePaused);
			if (r == MAGELIGHT_API::Result::Ok) {
				logs::info("IntelLite hotkey: host-bound {} (toggle opens/closes the panel)", hotkey::KeyName());
			} else {
				{
					std::lock_guard lock(g_hotkeyErrorMutex);
					g_hotkeyError = "That key is unavailable - edit settings.json and restart.";
				}
				logs::warn("IntelLite hotkey: BindHotkey({}) failed (result {})",
					hotkey::KeyName(), static_cast<int>(r));
			}
		}

		// window.magelight.send('blacklistAdd', '<editorid>') (a search row's
		// Exclude button or the Settings tab's Add) and
		// ('blacklistRemove', '<editorid>') (a Settings row's Remove). The
		// blacklist module is mutex-guarded and touches no RE:: state, so it is
		// safe to call directly on the render thread; the status is then
		// re-pushed so the page reflects the stored set.
		void OnBlacklistAdd(MAGELIGHT_API::ViewId /*view*/, const char* arg, void* /*user*/)
		{
			if (arg && *arg) {
				actors::BlacklistAdd(arg);
			}
			PushStatus();
		}

		void OnBlacklistRemove(MAGELIGHT_API::ViewId /*view*/, const char* arg, void* /*user*/)
		{
			if (arg && *arg) {
				actors::BlacklistRemove(arg);
			}
			PushStatus();
		}

		// window.magelight.send('panelClose','') — the page's Close button.
		// Release UI mode only; onClose hides the view.
		void OnPanelClose(MAGELIGHT_API::ViewId /*view*/, const char* /*arg*/, void* /*user*/)
		{
			if (g_v4 && g_modId) {
				g_v4->ReleaseUIMode(g_modId);
			}
		}

		// ---- nearby scan (shared by the panel and the prompt) ---------------

		// Loaded humanoid actors within kNearbyRadius of the player, nearest
		// first, as a JSON array of {name, distance, formId}. Game thread only
		// (it touches RE::), so every caller runs it through PostToGameThread.
		// When `a_excludeBlacklisted` is true (the prompt Change screen's
		// candidate list), blacklisted actors are skipped; the panel's manual
		// nearby list passes false so the manual picker stays unaffected.
		nlohmann::json BuildNearbyJson(bool a_excludeBlacklisted = false)
		{
			nlohmann::json arr = nlohmann::json::array();

			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player) return arr;

			const auto playerPos = player->GetPosition();

			// Humanoid filter: Skyrim's ActorTypeNPC keyword is on every
			// humanoid race, so an actor whose race lacks it (a dog, a dragon,
			// ...) is not a candidate.
			RE::BGSKeyword* npcKw = npc_index::HumanoidKeyword();

			struct NearbyEntry
			{
				int            distance = 0;
				nlohmann::json row;
			};

			std::vector<NearbyEntry>       rows;
			std::unordered_set<RE::FormID> seen;
			rows.reserve(32);

			auto consider = [&](RE::Actor* actor) {
				if (!actor || actor == player || actor->IsPlayerRef()) return;
				if (actor->IsDeleted() || actor->IsDisabled()) return;
				if (actor->IsDead()) return;
				if (npcKw) {
					auto* race = actor->GetRace();
					if (!race || !race->HasKeyword(npcKw)) return;
				}
				if (!actor->Is3DLoaded()) return;
				if (a_excludeBlacklisted && actors::IsBlacklisted(actor)) return;
				if (!seen.insert(actor->GetFormID()).second) return;

				const float dist = playerPos.GetDistance(actor->GetPosition());
				if (dist > kNearbyRadius) return;

				const char* name = actor->GetDisplayFullName();
				const int distInt = static_cast<int>(dist + 0.5f);

				nlohmann::json row;
				row["name"]     = (name && *name) ? name : "Unknown";
				row["distance"] = distInt;
				row["formId"]   = static_cast<std::uint32_t>(actor->GetFormID());
				rows.push_back(NearbyEntry{ distInt, std::move(row) });
			};

			if (auto* lists = RE::ProcessLists::GetSingleton()) {
				for (const auto& h : lists->highActorHandles) {
					if (auto actor = h.get()) consider(actor.get());
				}
				for (const auto& h : lists->middleHighActorHandles) {
					if (auto actor = h.get()) consider(actor.get());
				}
				for (const auto& h : lists->middleLowActorHandles) {
					if (auto actor = h.get()) consider(actor.get());
				}
				for (const auto& h : lists->lowActorHandles) {
					if (auto actor = h.get()) consider(actor.get());
				}
			}

			std::sort(rows.begin(), rows.end(),
				[](const NearbyEntry& a, const NearbyEntry& b) {
					return a.distance < b.distance;
				});
			if (rows.size() > kNearbyMax) {
				rows.resize(kNearbyMax);
			}

			for (auto& entry : rows) {
				arr.push_back(std::move(entry.row));
			}
			return arr;
		}

		// ---- consent prompt -------------------------------------------------
		//
		// A SkyrimNet NPC action raises a Popup-layer card the player must
		// Agree / Decline / Change. The C++ side owns the pending request; the
		// game thread writes it, and the render-thread JS listeners snapshot it
		// under g_promptMutex. No task is created before the player agrees.

		struct PendingPrompt
		{
			std::uint64_t gen = 0;
			bool          active = false;
			bool          resolved = false;     // the fuzzy element (target / place) was found
			std::string   kind;                 // fetch | deliver | meet | assassinate
			std::uint32_t agentFormID = 0;
			std::uint32_t targetFormID = 0;
			std::string   agentName;
			std::string   targetName;
			std::string   rawName;              // the LLM's raw target query (actor kinds only)
			std::string   place;
			std::string   message;
			std::string   sender;
			std::string   locationName;
			int           hours = 0;
			std::uint32_t fee = 0;               // Assassinate: the gold fee
			std::string   text;
		};

		std::mutex    g_promptMutex;
		PendingPrompt g_prompt;
		std::uint64_t g_promptGen = 0;

		void ClearPendingPrompt()
		{
			std::lock_guard lock(g_promptMutex);
			g_prompt = PendingPrompt{};
		}

		std::string ActorName(RE::Actor* a)
		{
			const char* n = a ? a->GetDisplayFullName() : nullptr;
			return (n && *n) ? std::string(n) : std::string("Unknown");
		}

		// The <Place> shown for a target: its current location name, else its
		// parent cell's name, else "an unknown place". Game thread only.
		std::string PlaceName(RE::Actor* a)
		{
			if (!a) return "an unknown place";
			if (auto* loc = a->GetCurrentLocation()) {
				const char* n = loc->GetFullName();
				if (n && *n) return n;
			}
			if (auto* cell = a->GetParentCell()) {
				const char* n = cell->GetFullName();
				if (n && *n) return n;
			}
			return "an unknown place";
		}

		void RebuildPromptText(PendingPrompt& a_req)
		{
			const std::string& A = a_req.agentName;

			if (a_req.kind == "meet") {
				std::string wherePart;
				std::string clause;
				if (a_req.resolved) {
					wherePart = "at " + a_req.place;
				} else if (a_req.locationName.empty()) {
					wherePart = "somewhere";
					clause = " (no place given, agreeing does nothing)";
				} else {
					wherePart = "at " + a_req.locationName;
					clause = " (no place found, agreeing does nothing)";
				}
				a_req.text = "Allow " + A + " to meet you " + wherePart + clause +
					" in " + std::to_string(a_req.hours) + " hour(s)?";
			} else {
				// Actor kinds: the target element decides the name and clause.
				std::string name;
				std::string clause;
				if (a_req.resolved) {
					name = a_req.targetName;
					clause = " (currently in " + a_req.place + ")";
				} else if (a_req.targetName.empty()) {
					name = "someone";
					clause = " (no name given, agreeing does nothing)";
				} else {
					name = a_req.targetName;
					clause = " (no actor found, agreeing does nothing)";
				}

				if (a_req.kind == "deliver") {
					std::string msgClause;
					if (a_req.message.empty()) {
						msgClause = " (no message given, agreeing does nothing)";
					}
					if (!a_req.sender.empty() && a_req.sender != A) {
						a_req.text = "Allow " + A + " to deliver a message from " +
							a_req.sender + " to " + name + clause + msgClause + "?";
					} else {
						a_req.text = "Allow " + A + " to deliver a message to " + name +
							clause + msgClause + "?";
					}
				} else if (a_req.kind == "assassinate") {
					a_req.text = "Hire " + A + " to assassinate " + name + clause;
					// Always state the fee: the price, or that it is free.
					if (a_req.fee > 0) {
						a_req.text += " for " + std::to_string(a_req.fee) + " gold?";
					} else {
						a_req.text += " for free?";
					}
				} else {
					a_req.text = "Allow " + A + " to fetch " + name + clause + "?";
				}
			}
		}

		nlohmann::json PromptShowJson(const PendingPrompt& a_req)
		{
			nlohmann::json j;
			j["kind"]     = a_req.kind;
			j["text"]     = a_req.text;
			j["agent"]    = a_req.agentName;
			j["target"]   = a_req.targetName;
			j["place"]    = a_req.place;
			j["message"]  = a_req.message;
			j["sender"]   = a_req.sender;
			j["location"] = a_req.locationName;
			j["hours"]    = a_req.hours;
			j["fee"]      = a_req.fee;
			return j;
		}

		void PushPromptShow(const nlohmann::json& a_payload)
		{
			if (!g_promptView || !g_v4) return;
			const std::string js =
				"window.intelitePrompt && window.intelitePrompt.show(" + SafeDump(a_payload) + ")";
			g_v4->InvokeJS(g_promptView, js.c_str());
		}

		void PushPromptCandidates(const nlohmann::json& a_arr)
		{
			if (!g_promptView || !g_v4) return;
			const std::string js =
				"window.intelitePrompt && window.intelitePrompt.setCandidates(" + SafeDump(a_arr) + ")";
			g_v4->InvokeJS(g_promptView, js.c_str());
		}

		void PushPromptSearchResults(const std::string& a_json)
		{
			if (!g_promptView || !g_v4) return;
			const std::string js =
				"window.intelitePrompt && window.intelitePrompt.setSearchResults(" + a_json + ")";
			g_v4->InvokeJS(g_promptView, js.c_str());
		}

		void CenterPromptView()
		{
			if (!g_promptView || !g_v4) return;

			std::int32_t sw = 0;
			std::int32_t sh = 0;
			g_v4->GetDisplaySize(&sw, &sh);
			if (sw <= 0 || sh <= 0) return;

			const std::int32_t w = std::min<std::int32_t>(kPromptW, sw);
			const std::int32_t h = std::min<std::int32_t>(kPromptH, sh);
			g_v4->SetViewBounds(g_promptView, (sw - w) / 2, (sh - h) / 2, w, h);
		}

		// Show the prompt and take UI mode (no pause). Safe from any thread;
		// Magelight calls are thread-safe, and OnModEvent (game thread)
		// suppresses SkyrimNet's hotkeys on UIModeEntered.
		void ShowPromptView()
		{
			if (!g_promptView || !g_v4) return;
			CenterPromptView();
			g_v4->ShowView(g_promptView, true);
			g_v4->RequestUIMode(g_promptView, MAGELIGHT_API::kUIModeFlagNone);
		}

		// Hide the prompt and drop UI mode. Does not clear the pending request.
		void ClosePromptView()
		{
			if (!g_v4) return;
			if (g_modId) g_v4->ReleaseUIMode(g_modId);
			if (g_promptView) g_v4->ShowView(g_promptView, false);
		}

		// ---- prompt: game-thread handlers -----------------------------------

		// A SkyrimNet action execute callback copies these and posts them to
		// the game thread, where the target/location is resolved.
		struct ActionTask
		{
			std::string   kind;
			std::uint32_t agentFormID = 0;
			std::string   paramsJson;
		};

		void OpenPrompt(PendingPrompt a_req)
		{
			a_req.active = true;
			PendingPrompt snapshot;
			{
				std::lock_guard lock(g_promptMutex);
				a_req.gen = ++g_promptGen;
				g_prompt = a_req;
				snapshot = g_prompt;
			}
			ShowPromptView();
			PushPromptShow(PromptShowJson(snapshot));
		}

		// Defined after OnGameTaskAgree: the no-confirm shortcut reuses that
		// path so a task created without confirmation is created exactly as the
		// Agree button creates it (including the assassinate fee payment).
		void CreateTaskNow(const PendingPrompt& a_req);

		// Route a fully-resolved NPC action: raise the consent prompt when the
		// confirmation setting is on, or create the task immediately when it is
		// off.
		void FinishAction(PendingPrompt a_req)
		{
			if (tasks::ConfirmPromptsEnabled()) {
				OpenPrompt(std::move(a_req));
			} else {
				CreateTaskNow(a_req);
			}
		}

		void HandleAction(ActionTask a_task)
		{
			auto* agent = RE::TESForm::LookupByID<RE::Actor>(a_task.agentFormID);
			if (!agent) {
				logs::warn("IntelLite prompt: action from missing agent {}", a_task.agentFormID);
				return;
			}

			if (a_task.kind == "cancelall") {
				tasks::CancelAll();
				RE::SendHUDMessage::ShowHUDMessage("IntelLite: all tasks cancelled.");
				return;
			}

			if (a_task.kind == "cancelmine") {
				tasks::CancelForAgent(a_task.agentFormID);
				RE::SendHUDMessage::ShowHUDMessage(
					(ActorName(agent) + "'s tasks cancelled.").c_str());
				return;
			}

			nlohmann::json params;
			try {
				params = nlohmann::json::parse(a_task.paramsJson);
			} catch (...) {
				params = nlohmann::json::object();
			}
			if (!params.is_object()) params = nlohmann::json::object();

			PendingPrompt req;
			req.kind = a_task.kind;
			req.agentFormID = a_task.agentFormID;
			req.agentName = ActorName(agent);

			if (a_task.kind == "meet") {
				std::string location;
				if (auto it = params.find("location"); it != params.end() && it->is_string()) {
					location = it->get<std::string>();
				}
				int hours = 0;
				std::int64_t hoursRaw = 0;
				if (JsonInt(params, "hours", hoursRaw)) {
					hours = static_cast<int>(std::clamp<std::int64_t>(hoursRaw, 0, 48));
				} else if (auto it = params.find("hours"); it != params.end()) {
					logs::warn("IntelLite prompt: meet action hours {} is not an integer - using 1h",
						SafeDump(*it));
				}
				if (location.empty()) {
					logs::info("IntelLite prompt: meet action with no location - defaulting to Here");
					location = "Here";
				}
				if (hours < 1) hours = 1;
				if (hours > 48) hours = 48;
				req.locationName = location;
				req.hours = hours;

				// Resolve the meeting place the same way tasks::AddMeet does
				// (fuzzy named-location resolution) - but the result is prompt
				// DATA now, not an early return, so the player still sees the
				// consent card and can fix or decline it.
				if (location == "Here") {
					req.resolved = true;
					req.place = tasks::PlayerVenueLabel();
				} else {
					places::Place place;
					if (places::Resolve(location, place)) {
						req.resolved = true;
						req.place = place.name.empty() ? location : place.name;
					} else {
						req.resolved = false;
						req.place = location;
					}
				}
				RebuildPromptText(req);
				FinishAction(std::move(req));
				return;
			}

			// fetch / deliver / assassinate: resolve the target by name.
			std::string targetName;
			if (auto it = params.find("target"); it != params.end() && it->is_string()) {
				targetName = it->get<std::string>();
			}
			if (targetName.empty()) {
				logs::warn("IntelLite prompt: {} action with no target", a_task.kind);
			}

			// Parse the kind-specific extras up front: deliver message + sender, assassinate fee.
			if (a_task.kind == "deliver") {
				if (auto it = params.find("message"); it != params.end() && it->is_string()) {
					req.message = fuzzy::ClampUtf8(it->get<std::string>(), kMaxMessageLen);
				}
				if (req.message.empty()) {
					logs::warn("IntelLite prompt: deliver action with no message");
				}
				if (auto it = params.find("sender"); it != params.end() && it->is_string()) {
					req.sender = fuzzy::ClampUtf8(it->get<std::string>(), kMaxSenderLen);
				}
			} else if (a_task.kind == "assassinate") {
				// The LLM supplies the fee; it is optional (0 = no fee) and any
				// positive value is accepted. It arrives as a JSON string.
				std::int64_t feeRaw = 0;
				if (JsonInt(params, "fee", feeRaw)) {
					if (feeRaw > 0 && feeRaw <= INT32_MAX) req.fee = static_cast<std::uint32_t>(feeRaw);
				} else if (auto it = params.find("fee"); it != params.end()) {
					logs::warn("IntelLite prompt: assassinate fee {} is not an integer - using 0",
						SafeDump(*it));
				}
			}

			req.rawName = targetName;

			// No name given: there is nothing to resolve, so prompt with the
			// unresolved annotation instead of returning early.
			if (targetName.empty()) {
				req.resolved = false;
				req.targetFormID = 0;
				req.targetName.clear();
				req.place.clear();
				logs::info("IntelLite prompt: {} target '{}' did not resolve - prompting instead",
					a_task.kind, targetName);
				RebuildPromptText(req);
				FinishAction(std::move(req));
				return;
			}

			// fetch / deliver / assassinate: the SkyrimNet search runs off the
			// game thread; this continuation comes back on the game thread with
			// the resolved actor (or null). Resolution is prompt DATA now, so a
			// miss still raises the card (the player can fix it).
			const std::string wantedName = targetName;
			actors::ResolveAsync(targetName,
				[req = std::move(req), wantedName](RE::Actor* target, std::string resolvedName) mutable {
					if (target) {
						req.resolved = true;
						req.targetFormID = target->GetFormID();
						req.targetName = resolvedName.empty() ? ActorName(target) : resolvedName;
						req.place = PlaceName(target);
					} else {
						req.resolved = false;
						req.targetFormID = 0;
						req.targetName = wantedName;
						req.place.clear();
						logs::info("IntelLite prompt: {} target '{}' did not resolve - prompting instead",
							req.kind, wantedName);
					}

					RebuildPromptText(req);
					FinishAction(std::move(req));
				});
		}

		void OnGameTaskAction(void* user)
		{
			std::unique_ptr<ActionTask> task(static_cast<ActionTask*>(user));
			if (!task) return;
			HandleAction(std::move(*task));
		}

		struct AgreeTask
		{
			std::string   kind;
			std::uint32_t agentFormID = 0;
			std::uint32_t targetFormID = 0;
			bool          resolved = false;
			std::string   rawName;
			std::string   targetName;
			std::string   message;
			std::string   sender;
			std::string   locationName;
			int           hours = 0;
			std::uint32_t fee = 0;
		};

		struct ClearPromptTask
		{
			std::uint64_t gen = 0;
		};

		void OnGameTaskClearPrompt(void* user)
		{
			std::unique_ptr<ClearPromptTask> t(static_cast<ClearPromptTask*>(user));
			std::lock_guard lock(g_promptMutex);
			if (!t || g_prompt.gen == t->gen) {
				g_prompt = PendingPrompt{};
			}
		}

		// The consent card's Agree on an action that is not ready - the fuzzy
		// element (target / place) never resolved, or a deliver has no message:
		// emit the HUD message + persistent event from the agree path.
		// One event, one HUD line.
		void AnnounceNotReady(const AgreeTask& a_req, RE::Actor* a_agent)
		{
			const std::string A = ActorName(a_agent);

			if (a_req.kind == "meet") {
				if (a_req.locationName.empty()) {
					RE::SendHUDMessage::ShowHUDMessage((A + " didn't say where to meet.").c_str());
					skyrimnet::RegisterPersistentEvent(
						A + " tried to arrange a meeting but no place was given.", a_agent, nullptr);
				} else {
					RE::SendHUDMessage::ShowHUDMessage(
						(A + " cannot find a place called " + a_req.locationName + ".").c_str());
					skyrimnet::RegisterPersistentEvent(
						A + " could not arrange a meeting at " + a_req.locationName + ".",
						a_agent, nullptr);
				}
				return;
			}

			if (!a_req.resolved) {
				if (a_req.rawName.empty()) {
					if (a_req.kind == "fetch") {
						RE::SendHUDMessage::ShowHUDMessage((A + " didn't say who to fetch.").c_str());
						skyrimnet::RegisterPersistentEvent(
							A + " tried to fetch someone but no name was given.", a_agent, nullptr);
					} else if (a_req.kind == "deliver") {
						RE::SendHUDMessage::ShowHUDMessage(
							(A + " didn't say who the message is for.").c_str());
						skyrimnet::RegisterPersistentEvent(
							A + " tried to deliver a message but no recipient was given.",
							a_agent, nullptr);
					} else {
						RE::SendHUDMessage::ShowHUDMessage((A + " didn't name a target.").c_str());
						skyrimnet::RegisterPersistentEvent(
							A + " tried to arrange an assassination but no target was named.",
							a_agent, nullptr);
					}
				} else {
					RE::SendHUDMessage::ShowHUDMessage(
						(A + " couldn't find anyone called " + a_req.rawName + ".").c_str());
					skyrimnet::RegisterPersistentEvent(
						A + " tried to find " + a_req.rawName +
							" but could not find anyone by that name.",
						a_agent, nullptr);
				}
				return;
			}

			// Resolved target, but a deliver has no message to carry.
			RE::SendHUDMessage::ShowHUDMessage((A + " has no message to deliver.").c_str());
			skyrimnet::RegisterPersistentEvent(
				A + " tried to deliver a message to " + a_req.targetName +
					" but no message was given.",
				a_agent, nullptr);
		}

		void OnGameTaskAgree(void* user)
		{
			std::unique_ptr<AgreeTask> r(static_cast<AgreeTask*>(user));
			if (!r) return;

			auto* agent = RE::TESForm::LookupByID<RE::Actor>(r->agentFormID);
			const bool ready = r->resolved && !(r->kind == "deliver" && r->message.empty());
			if (agent && !ready) {
				AnnounceNotReady(*r, agent);
				logs::info("IntelLite prompt: agree {} (not ready - not queued)", r->kind);
			} else {
				bool ok = false;
				if (agent && r->kind == "meet") {
					ok = tasks::AddMeet(agent, r->locationName, r->hours);
				} else if (agent && r->kind == "deliver") {
					auto* target = RE::TESForm::LookupByID<RE::Actor>(r->targetFormID);
					if (target) ok = tasks::AddDeliver(agent, target, r->message, r->sender);
				} else if (agent && r->kind == "fetch") {
					auto* target = RE::TESForm::LookupByID<RE::Actor>(r->targetFormID);
					if (target) ok = tasks::AddFetch(agent, target);
				} else if (agent && r->kind == "assassinate") {
					auto* target = RE::TESForm::LookupByID<RE::Actor>(r->targetFormID);
					if (target) ok = tasks::AddAssassinate(agent, target, r->fee);
				} else if (agent) {
					logs::warn("IntelLite prompt: agree on unsupported kind {}", r->kind);
				}

				logs::info("IntelLite prompt: agree {} ({})", r->kind, ok ? "queued" : "refused");
			}

			{
				std::lock_guard lock(g_promptMutex);
				g_prompt = PendingPrompt{};
			}
		}

		// The no-confirm shortcut: build the same AgreeTask OnPromptAgree would
		// build from the pending prompt and run the agree path directly.
		void CreateTaskNow(const PendingPrompt& a_req)
		{
			auto* agree = new AgreeTask{};
			agree->kind         = a_req.kind;
			agree->agentFormID  = a_req.agentFormID;
			agree->targetFormID = a_req.targetFormID;
			agree->resolved     = a_req.resolved;
			agree->rawName      = a_req.rawName;
			agree->targetName   = a_req.targetName;
			agree->message      = a_req.message;
			agree->sender       = a_req.sender;
			agree->locationName = a_req.locationName;
			agree->hours        = a_req.hours;
			agree->fee          = a_req.fee;
			OnGameTaskAgree(agree);
		}

		// promptAgree / promptDecline: close the card, then act on the game
		// thread (task creation, or just clearing state).
		void OnPromptAgree(MAGELIGHT_API::ViewId /*view*/, const char* /*arg*/, void* /*user*/)
		{
			auto* r = new AgreeTask{};
			{
				std::lock_guard lock(g_promptMutex);
				r->kind         = g_prompt.kind;
				r->agentFormID  = g_prompt.agentFormID;
				r->targetFormID = g_prompt.targetFormID;
				r->resolved     = g_prompt.resolved;
				r->rawName      = g_prompt.rawName;
				r->targetName   = g_prompt.targetName;
				r->message      = g_prompt.message;
				r->sender       = g_prompt.sender;
				r->locationName = g_prompt.locationName;
				r->hours        = g_prompt.hours;
				r->fee          = g_prompt.fee;
			}
			ClosePromptView();
			if (!PostToGameThread(&OnGameTaskAgree, r)) delete r;
		}

		void OnPromptDecline(MAGELIGHT_API::ViewId /*view*/, const char* /*arg*/, void* /*user*/)
		{
			std::uint64_t gen = 0;
			{
				std::lock_guard lock(g_promptMutex);
				gen = g_prompt.gen;
			}
			ClosePromptView();
			auto* t = new ClearPromptTask{ gen };
			if (!PostToGameThread(&OnGameTaskClearPrompt, t)) delete t;
		}

		// promptChange: push the change-screen payload - the nearby actor
		// candidates (excluding blacklisted actors) and a reset search list.
		void OnGameTaskPromptChange(void* /*user*/)
		{
			{
				std::lock_guard lock(g_promptMutex);
				if (!g_prompt.active) return;
			}
			PushPromptCandidates(BuildNearbyJson(true));
			PushPromptSearchResults("[]");
		}

		void OnPromptChange(MAGELIGHT_API::ViewId /*view*/, const char* /*arg*/, void* /*user*/)
		{
			(void)PostToGameThread(&OnGameTaskPromptChange, nullptr);
		}

		// promptSearch: the search box on the change screen. The search runs off
		// the game thread; the result lands on the game thread and is pushed to
		// the prompt page. A monotonically increasing sequence drops a stale
		// result that arrives after a newer search.
		std::atomic<std::uint32_t> g_promptSearchSeq{ 0 };

		void OnPromptSearch(MAGELIGHT_API::ViewId /*view*/, const char* arg, void* /*user*/)
		{
			const std::string query = arg ? arg : "";
			const std::uint32_t seq = ++g_promptSearchSeq;
			actors::SearchMergedAsync(query, 10,
				[seq](std::string json) {
					if (seq != g_promptSearchSeq.load(std::memory_order_relaxed)) return;  // superseded
					PushPromptSearchResults(json);
				});
		}

		struct PickTarget
		{
			std::uint32_t formId = 0;
		};

		void OnGameTaskPickTarget(void* user)
		{
			std::unique_ptr<PickTarget> p(static_cast<PickTarget*>(user));
			if (!p) return;

			auto* actor = RE::TESForm::LookupByID<RE::Actor>(p->formId);
			if (!actor || actor->IsDeleted() || actor->IsDead() || actor->IsPlayerRef()) return;
			if (actors::IsBlacklisted(actor)) return;

			PendingPrompt snapshot;
			{
				std::lock_guard lock(g_promptMutex);
				if (!g_prompt.active) return;
				g_prompt.targetFormID = actor->GetFormID();
				g_prompt.targetName = ActorName(actor);
				g_prompt.place = PlaceName(actor);
				g_prompt.resolved = true;
				RebuildPromptText(g_prompt);
				snapshot = g_prompt;
			}
			PushPromptShow(PromptShowJson(snapshot));
		}

		void OnPromptPickTarget(MAGELIGHT_API::ViewId /*view*/, const char* arg, void* /*user*/)
		{
			if (!arg || !*arg) return;
			std::uint32_t id = 0;
			if (!ParseU32Strict(arg, id)) return;
			if (id == 0) return;
			auto* p = new PickTarget{ id };
			if (!PostToGameThread(&OnGameTaskPickTarget, p)) delete p;
		}

		void OnGameTaskSetLocation(void* user)
		{
			std::unique_ptr<std::string> loc(static_cast<std::string*>(user));
			if (!loc) return;

			PendingPrompt snapshot;
			{
				std::lock_guard lock(g_promptMutex);
				if (!g_prompt.active || g_prompt.kind != "meet") return;
				g_prompt.locationName = *loc;
				if (*loc == "Here") {
					g_prompt.resolved = true;
					g_prompt.place = tasks::PlayerVenueLabel();
				} else if (loc->empty()) {
					g_prompt.resolved = false;
					g_prompt.place.clear();
				} else {
					places::Place place;
					const bool ok = places::Resolve(*loc, place);
					g_prompt.resolved = ok;
					g_prompt.place = (ok && !place.name.empty()) ? place.name : *loc;
				}
				RebuildPromptText(g_prompt);
				snapshot = g_prompt;
			}
			PushPromptShow(PromptShowJson(snapshot));
		}

		void OnPromptSetLocation(MAGELIGHT_API::ViewId /*view*/, const char* arg, void* /*user*/)
		{
			auto* s = new std::string(arg ? arg : "");
			if (!PostToGameThread(&OnGameTaskSetLocation, s)) delete s;
		}

		struct SetHours
		{
			int hours = 0;
		};

		void OnGameTaskSetHours(void* user)
		{
			std::unique_ptr<SetHours> h(static_cast<SetHours*>(user));
			if (!h) return;

			PendingPrompt snapshot;
			{
				std::lock_guard lock(g_promptMutex);
				if (!g_prompt.active || g_prompt.kind != "meet") return;
				int hours = h->hours;
				if (hours < 1) hours = 1;
				if (hours > 48) hours = 48;
				g_prompt.hours = hours;
				RebuildPromptText(g_prompt);
				snapshot = g_prompt;
			}
			PushPromptShow(PromptShowJson(snapshot));
		}

		void OnPromptSetHours(MAGELIGHT_API::ViewId /*view*/, const char* arg, void* /*user*/)
		{
			if (!arg || !*arg) return;
			std::uint32_t v = 0;
			if (!ParseU32Strict(arg, v, 1000000u)) return;
			const int hours = static_cast<int>(v);
			auto* h = new SetHours{ hours };
			if (!PostToGameThread(&OnGameTaskSetHours, h)) delete h;
		}

		// promptSetFee: update an assassinate request's fee and rebuild the
		// prompt line. The fee is optional (0 = no fee); the prompt omits the
		// "for N gold" clause when it is zero.
		struct SetFee
		{
			std::uint32_t fee = 0;
		};

		void OnGameTaskSetFee(void* user)
		{
			std::unique_ptr<SetFee> f(static_cast<SetFee*>(user));
			if (!f) return;

			PendingPrompt snapshot;
			{
				std::lock_guard lock(g_promptMutex);
				if (!g_prompt.active || g_prompt.kind != "assassinate") return;
				g_prompt.fee = f->fee;
				RebuildPromptText(g_prompt);
				snapshot = g_prompt;
			}
			PushPromptShow(PromptShowJson(snapshot));
		}

		void OnPromptSetFee(MAGELIGHT_API::ViewId /*view*/, const char* arg, void* /*user*/)
		{
			if (!arg || !*arg) return;
			std::uint32_t fee = 0;
			if (!ParseU32Strict(arg, fee, static_cast<std::uint32_t>(INT32_MAX))) return;
			auto* f = new SetFee{ fee };
			if (!PostToGameThread(&OnGameTaskSetFee, f)) delete f;
		}

		// promptSetMessage: update a deliver request's message (the change
		// screen prefills it with the AI's message, and the agree path uses
		// whatever the player last set). Clamped to the panel's message cap.
		struct SetMessage
		{
			std::string message;
		};

		void OnGameTaskSetMessage(void* user)
		{
			std::unique_ptr<SetMessage> m(static_cast<SetMessage*>(user));
			if (!m) return;

			PendingPrompt snapshot;
			{
				std::lock_guard lock(g_promptMutex);
				if (!g_prompt.active || g_prompt.kind != "deliver") return;
				g_prompt.message = fuzzy::ClampUtf8(m->message, kMaxMessageLen);
				RebuildPromptText(g_prompt);
				snapshot = g_prompt;
			}
			PushPromptShow(PromptShowJson(snapshot));
		}

		void OnPromptSetMessage(MAGELIGHT_API::ViewId /*view*/, const char* arg, void* /*user*/)
		{
			auto* m = new SetMessage{ arg ? arg : "" };
			if (!PostToGameThread(&OnGameTaskSetMessage, m)) delete m;
		}

		// promptSetSender: update a deliver request's original-sender name (the
		// change screen may edit who the message is from). An over-long value is
		// truncated, matching the execute path's sender cap.
		struct SetSender
		{
			std::string sender;
		};

		void OnGameTaskSetSender(void* user)
		{
			std::unique_ptr<SetSender> s(static_cast<SetSender*>(user));
			if (!s) return;

			PendingPrompt snapshot;
			{
				std::lock_guard lock(g_promptMutex);
				if (!g_prompt.active || g_prompt.kind != "deliver") return;
				g_prompt.sender = fuzzy::ClampUtf8(s->sender, kMaxSenderLen);
				RebuildPromptText(g_prompt);
				snapshot = g_prompt;
			}
			PushPromptShow(PromptShowJson(snapshot));
		}

		void OnPromptSetSender(MAGELIGHT_API::ViewId /*view*/, const char* arg, void* /*user*/)
		{
			auto* s = new SetSender{ arg ? arg : "" };
			if (!PostToGameThread(&OnGameTaskSetSender, s)) delete s;
		}

	}

	std::string HostVersion()
	{
		return (g_v4 && g_v4->hostVersion) ? std::string(g_v4->hostVersion) : std::string("absent");
	}

	void Initialize()
	{
		// Acquire the raw API, then register this mod. Absent, older than v4
		// (0.10.0) or refused all degrade to "not present".
		g_v1 = MAGELIGHT_API::RequestApi();
		g_v4 = MAGELIGHT_API::RequestApi4();
		if (!g_v1 || !g_v4) {
			logs::info("Magelight: not present");
			return;
		}

		MAGELIGHT_API::ModDesc d{};
		d.size           = sizeof(d);
		d.modId          = kModId;
		d.displayName    = kDisplayName;
		d.modVersion     = 0;
		d.minHostVersion = MAGELIGHT_API::PackVersion(0, 16, 0);
		d.callbackThread = MAGELIGHT_API::CallbackThread::GameThread;
		d.onEvent        = &OnModEvent;
		d.onLog           = nullptr;
		d.user            = nullptr;
		d.sessionName     = nullptr;
		if (g_v4->RegisterMod(&d, &g_modId) != MAGELIGHT_API::Result::Ok) {
			logs::info("Magelight: not present");
			return;
		}

		// The Magelight probe line. Read the v4 struct so the
		// numbers are the host's real API (4) and host version, not the v1 view.
		logs::info("Magelight: requested v4, apiVersion = {}, hostVersion = {}",
			g_v4->apiVersion, g_v4->hostVersion ? g_v4->hostVersion : "(null)");

		const std::string page = PagePath();
		MAGELIGHT_API::ViewDesc vd{};
		vd.size         = sizeof(vd);
		vd.name         = "panel";
		vd.htmlPath     = page.c_str();
		vd.anchor       = MAGELIGHT_API::Anchor::TopLeft;
		vd.x            = 0;
		vd.y            = 0;
		vd.w            = kPanelW;
		vd.h            = kPanelH;
		vd.fullscreen   = false;
		vd.clickThrough = false;
		vd.startVisible = false;
		vd.layer        = MAGELIGHT_API::Layer::Panel;
		vd.uiScale      = 0.0f;
		vd.onDomReady   = nullptr;
		if (g_v4->CreateViewEx(g_modId, &vd, &g_view) != MAGELIGHT_API::Result::Ok) {
			g_view = 0;
			logs::error("IntelLite: Magelight view refused — {}",
				g_v4->GetLastErrorMessage(g_modId));
			return;
		}

		// Panel pages are rarely open: load lazily on the first show. Gate on the
		// packed host version and the authoritative capability (0.31.0 test builds
		// lack the field).
		if (g_v4->hostVersionNumber >= 3100 &&
			g_v4->QueryCapability("loadonshow") == 1) {
			g_v4->SetViewLoadOnShow(g_view, true);
		}

		g_v4->RegisterJSListenerEx(g_view, "panelReady", &OnPanelReady, nullptr);
		g_v4->RegisterJSListenerEx(g_view, "panelClose", &OnPanelClose, nullptr);
		g_v4->RegisterJSListenerEx(g_view, "requestTasks", &OnRequestTasks, nullptr);
		g_v4->RegisterJSListenerEx(g_view, "cancelTask", &OnCancelTask, nullptr);
		g_v4->RegisterJSListenerEx(g_view, "executeAction", &OnExecuteAction, nullptr);
		g_v4->RegisterJSListenerEx(g_view, "searchActors", &OnSearchActors, nullptr);
		g_v4->RegisterJSListenerEx(g_view, "setConfirm", &OnSetConfirm, nullptr);
		g_v4->RegisterJSListenerEx(g_view, "blacklistAdd", &OnBlacklistAdd, nullptr);
		g_v4->RegisterJSListenerEx(g_view, "blacklistRemove", &OnBlacklistRemove, nullptr);

		// The host owns the toggle: bind the stored key to the panel now that
		// the view and its JS listeners exist.
		BindPanelHotkey();

		// The consent prompt: a second, hidden Popup-layer view raised by a
		// SkyrimNet NPC action. Loaded on first show; heard by the same
		// OnModEvent sink, so UIModeEntered/Exited suppress and restore
		// SkyrimNet's hotkeys exactly as they do for the panel.
		const std::string promptPage = PromptPagePath();
		MAGELIGHT_API::ViewDesc pvd{};
		pvd.size         = sizeof(pvd);
		pvd.name         = "prompt";
		pvd.htmlPath     = promptPage.c_str();
		pvd.anchor       = MAGELIGHT_API::Anchor::TopLeft;
		pvd.x            = 0;
		pvd.y            = 0;
		pvd.w            = kPromptW;
		pvd.h            = kPromptH;
		pvd.fullscreen   = false;
		pvd.clickThrough = false;
		pvd.startVisible = false;
		pvd.layer        = MAGELIGHT_API::Layer::Popup;
		pvd.uiScale      = 0.0f;
		pvd.onDomReady   = nullptr;
		if (g_v4->CreateViewEx(g_modId, &pvd, &g_promptView) != MAGELIGHT_API::Result::Ok) {
			g_promptView = 0;
			logs::error("IntelLite: Magelight prompt view refused — {}",
				g_v4->GetLastErrorMessage(g_modId));
		} else {
			if (g_v4->hostVersionNumber >= 3100 &&
				g_v4->QueryCapability("loadonshow") == 1) {
				g_v4->SetViewLoadOnShow(g_promptView, true);
			}
			g_v4->RegisterJSListenerEx(g_promptView, "promptAgree", &OnPromptAgree, nullptr);
			g_v4->RegisterJSListenerEx(g_promptView, "promptDecline", &OnPromptDecline, nullptr);
			g_v4->RegisterJSListenerEx(g_promptView, "promptChange", &OnPromptChange, nullptr);
			g_v4->RegisterJSListenerEx(g_promptView, "promptSearch", &OnPromptSearch, nullptr);
			g_v4->RegisterJSListenerEx(g_promptView, "blacklistAdd", &OnBlacklistAdd, nullptr);
			g_v4->RegisterJSListenerEx(g_promptView, "promptPickTarget", &OnPromptPickTarget, nullptr);
			g_v4->RegisterJSListenerEx(g_promptView, "promptSetLocation", &OnPromptSetLocation, nullptr);
			g_v4->RegisterJSListenerEx(g_promptView, "promptSetHours", &OnPromptSetHours, nullptr);
			g_v4->RegisterJSListenerEx(g_promptView, "promptSetFee", &OnPromptSetFee, nullptr);
			g_v4->RegisterJSListenerEx(g_promptView, "promptSetMessage", &OnPromptSetMessage, nullptr);
			g_v4->RegisterJSListenerEx(g_promptView, "promptSetSender", &OnPromptSetSender, nullptr);
			CenterPromptView();  // near no-op at kDataLoaded; ShowPromptView re-centers
		}

		// Every way out of UI mode (Escape, the host's hotkey, the Close
		// button, a lost race) is handled by OnModEvent: it restores SkyrimNet's
		// hotkeys and hides the panel. The host owns the toggle - BindPanelHotkey
		// bound the key through Magelight, no input sink is involved. OnModEvent
		// runs on the callback thread chosen in ModDesc (GameThread), so ShowView
		// and the VM dispatch are safe.

		CenterView();  // near no-op at kDataLoaded; panelReady re-centers

		logs::info("IntelLite: panel ready - toggle with {}", hotkey::KeyName());
	}

	void PushStatus()
	{
		const auto* v4 = g_v4;
		if (!g_view || !v4) return;

		std::string hotkeyError;
		{
			std::lock_guard lock(g_hotkeyErrorMutex);
			hotkeyError = g_hotkeyError;
		}

		nlohmann::json status;
		status["modVersion"]    = kModVersion;
		status["skyrimnetApi"]  = skyrimnet::ApiVersion();
		status["magelightHost"] = (v4->hostVersion && *v4->hostVersion)
			? v4->hostVersion
			: "unknown";
		status["hotkey"]         = hotkey::KeyName();
		status["hotkeyError"]    = std::move(hotkeyError);
		status["confirm"]        = tasks::ConfirmPromptsEnabled() ? 1 : 0;
		status["blacklist"]     = actors::BlacklistEntries();

		const std::string js =
			"window.intelite && window.intelite.setStatus(" + SafeDump(status) + ")";
		v4->InvokeJS(g_view, js.c_str());
	}

	void PushNearby()
	{
		const auto* v4 = g_v4;
		if (!g_view || !v4) return;

		const std::string js =
			"window.intelite && window.intelite.setNearby(" + SafeDump(BuildNearbyJson()) + ")";
		v4->InvokeJS(g_view, js.c_str());
	}

	void PushTasks(const std::string& json)
	{
		const auto* v4 = g_v4;
		if (!g_view || !v4) return;

		const std::string js =
			"window.intelite && window.intelite.setTasks(" + json + ")";
		v4->InvokeJS(g_view, js.c_str());
	}

	void PushSearchResults(const std::string& json)
	{
		const auto* v4 = g_v4;
		if (!g_view || !v4) return;

		const std::string js =
			"window.intelite && window.intelite.setSearchResults(" + json + ")";
		v4->InvokeJS(g_view, js.c_str());
	}

	void PushExecutorSearchResults(const std::string& json)
	{
		const auto* v4 = g_v4;
		if (!g_view || !v4) return;

		const std::string js =
			"window.intelite && window.intelite.setExecutorSearchResults(" + json + ")";
		v4->InvokeJS(g_view, js.c_str());
	}

	void RequestAction(const std::string& kind, std::uint32_t agentFormID,
		const std::string& paramsJson)
	{
		// Called from a SkyrimNet action execute callback, which may run on any
		// thread: copy what we need, then do every RE:: / VM / view touch in the
		// posted game task (HandleAction).
		auto* task = new ActionTask{ kind, agentFormID, paramsJson };
		if (!PostToGameThread(&OnGameTaskAction, task)) delete task;
	}
}
