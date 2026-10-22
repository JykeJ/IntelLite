#pragma once

// IntelLite's persistent task engine.
//
// A task is a simulation the plugin owns across saves: an "agent" is sent to
// fetch a "target" and bring it to the player. Travel is simulated with a wall
// of game-time; the arrival is a teleport (real pathing is a later slice).
//
// Threading contract:
//   - Tick() is game-thread only and is the ONLY place RE:: state is touched.
//     g_tasks is game-thread only: Tick(), the Add* functions and the co-save
//     callbacks (SKSE invokes those on the main thread) are its only touchers.
//     The pump thread posts Tick() through SKSE::GetTaskInterface()->AddTask.
//   - AddFetch()/AddDeliver()/AddMeet()/AddAssassinate() and PushToPanelFresh()
//     are game-thread only (the panel posts them via PostGameTask).
//   - Cancel()/CancelAll()/CancelForAgent()/ListJson()/PushToPanel()/
//     HasActiveTask()/HasAnyActiveTask()/HasCancellableTask()/
//     HasCommittedKillTask()/ConfirmPromptsEnabled()/SetConfirmPromptsEnabled()
//     are safe from any thread: cancels are queued and applied by the next
//     Tick(); the list and the eligibility answers come from a snapshot that
//     the game thread rebuilds (the list is a pure snapshot of stored strings
//     - no RE:: access).
//   - The queue is never mutated from a render-thread listener directly: the
//     mutations are marshalled onto the game thread by the pump/PostGameTask.

#include <cstddef>
#include <cstdint>
#include <string>

namespace RE
{
	class Actor;
}

namespace intellite::tasks
{
	// Task types. Fetch/Deliver/Meet share the travel legs; Assassinate adds
	// its own approach (sneak package or game-time simulation) and kill scene.
	enum class Type : std::uint8_t
	{
		Fetch = 1,
		Deliver = 2,
		Meet = 3,
		Assassinate = 4
	};

	enum class State : std::uint8_t
	{
		Pending = 0,
		AgentTravel = 1,
		TargetTravel = 2,
		Executing = 3,
		Done = 4,
		Cancelled = 5,
		Failed = 6,
		Lingering = 7
	};

	// Stored-string caps. Every string written into the co-save must respect these;
	// the reader rejects an over-cap length as a tripwire (an over-cap write would
	// otherwise abort the whole TASK record on load).
	inline constexpr std::size_t kMaxMessageChars = 200;  // deliver message (matches the panel's UI cap)
	inline constexpr std::size_t kMaxSenderChars  = 64;
	inline constexpr std::size_t kMaxNameChars    = 128;  // agent/target display names

	// Start the pump thread (once). Game-thread safe; a no-op if already
	// started. Must be called after kDataLoaded.
	void Initialize();

	// Register the co-save callbacks. Call from SKSEPlugin_Load, right after
	// SKSE::Init. Unique ID 'ILIT', two records: 'TASK' version 4 (versions 1-3
	// are still readable) and 'SETT' version 1 (the confirmation preference).
	void RegisterSerialization();

	// Queue a Fetch task (game thread only). Returns false when refused
	// (null/dead/self actors or the task cap). Pushes the panel update.
	bool AddFetch(RE::Actor* agent, RE::Actor* target);

	// Queue a Deliver task (game thread only): the agent walks to `recipient`,
	// delivers `message` (a SkyrimNet persistent event + HUD), and is released
	// where it stands. `sender` names the message's original author when it is
	// from someone other than the delivering agent; empty means the deliverer is
	// the sender. Returns false when refused (null/dead/self actors, an empty
	// message, or the task cap). Pushes the panel update.
	bool AddDeliver(RE::Actor* agent, RE::Actor* recipient, const std::string& message,
		const std::string& sender);

	// Queue a Meet task (game thread only): the agent travels to `locationName`
	// ("Here" = the spot where the player stands when the arrangement is made,
	// captured into a leased persistent marker; otherwise any named location,
	// found fuzzily - a BGSLocation's world marker or an interior cell's door)
	// `hoursFromNow` game hours out (1-48), waits there, and completes when the
	// player arrives (or after a grace period). Returns false when refused
	// (null/dead agent, an empty location, an unresolvable place, or the task
	// cap). Pushes the panel update.
	bool AddMeet(RE::Actor* agent, const std::string& locationName, int hoursFromNow);

	// The player's current venue as a display label for meet strings: the
	// interior cell's name when indoors, else the current location's name,
	// else "your location". Game thread only.
	std::string PlayerVenueLabel();

	// Queue an Assassinate task (game thread only): an optional fee is paid from
	// the player (refused with a HUD when it cannot be afforded; nothing is
	// created and nothing is charged on refusal). A zero fee is allowed and
	// charges nothing. The agent then travels to `target`: a 3D-loaded target
	// is approached with the sneak package and killed in a staged scene; an
	// unloaded one is reached by the game-time simulation and killed
	// off-screen (an essential target is spared and the executor reports
	// back). Returns false when refused (null/dead/self actors, an
	// unaffordable fee, or the task cap). Pushes the panel update.
	bool AddAssassinate(RE::Actor* agent, RE::Actor* target, std::uint32_t fee);

	// Request cancellation. Safe from any thread: the task is marked and
	// removed by the next Tick(), which then pushes the panel update.
	void Cancel(std::uint32_t id);
	void CancelAll();

	// Thread-safe queries over guarded state only (no RE:: access). The
	// SkyrimNet action eligibility callbacks run off the game thread, so they
	// may use only these. HasActiveTask is true when a non-finished task is
	// assigned to the given agent; a merely-lingering fetch (the agent's work is
	// done) does not count. HasAnyActiveTask is true when any non-finished task
	// exists at all.
	bool HasActiveTask(std::uint32_t agentFormID);
	bool HasAnyActiveTask();

	// Request cancellation of every task assigned to one agent. Safe from any
	// thread: the agent ids are queued and applied by the next Tick(), which
	// cancels each of that agent's non-finished tasks through the same body the
	// per-task Cancel uses (a committed assassination is left running, a
	// lingering fetch is dismissed).
	void CancelForAgent(std::uint32_t agentFormID);

	// True when the given agent owns a non-finished task that the cancel body
	// would actually accept - any live task or a lingering fetch, but not a
	// committed assassination. Snapshot-only, safe from any thread.
	bool HasCancellableTask(std::uint32_t agentFormID);

	// True when the given agent is locked by a kill already under way - a
	// non-finished assassination whose kill scene has started. Such an agent
	// takes no new task. Snapshot-only, safe from any thread.
	bool HasCommittedKillTask(std::uint32_t agentFormID);

	// The confirmation preference (co-save SETT v1): when true (the default),
	// an NPC-initiated action raises the consent prompt; when false it is
	// created immediately with the resolved parameters. Safe from any thread.
	bool ConfirmPromptsEnabled();
	void SetConfirmPromptsEnabled(bool enabled);

	// Game-thread only; posted by the pump. Applies queued cancellations,
	// advances every task one step, drops finished rows, and pushes the panel
	// update on any change.
	void Tick();

	// Thread-safe snapshot: a JSON array of the currently active tasks, the
	// latest snapshot (refreshed by Tick and every game-thread mutation). Names
	// come from stored strings; no RE:: access.
	std::string ListJson();

	// Build ListJson() and hand it to the panel. Safe from any thread.
	void PushToPanel();

	// Rebuild the snapshot (panel + eligibility) and hand it to the panel.
	// Game-thread only: it reads g_tasks. The Add* functions and the game-thread
	// panel handlers use this; PushToPanel() pushes the last snapshot from any
	// thread.
	void PushToPanelFresh();
}
