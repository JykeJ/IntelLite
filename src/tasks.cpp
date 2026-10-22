// Portions of this file are adapted from IntelEngine-NativePlugin
// (Copyright 2026 Galanx), licensed under the Apache License 2.0.
// Changed for IntelLite. See THIRD_PARTY_NOTICES.md and NOTICE.

// IntelLite's persistent task engine.
//
// One pump thread wakes every kPumpIntervalMs and posts exactly one Tick() onto
// the game thread through SKSE::GetTaskInterface()->AddTask. It never requeues
// itself from inside the task (that drains the whole SKSE queue in one frame and
// hard-freezes the game); the pace comes from the pump's own sleep. Every RE::
// touch happens inside Tick()/AddFetch() on the game thread.
//
// State is a flat vector of POD+string records guarded by one mutex. Cancels
// arriving from other threads go into a queue that Tick() drains; ListJson() is
// a pure snapshot of already-stored strings, so it is safe to build a panel
// update from the render thread.
//
// Travel is ONE reusable leg mechanism (`Leg` + StartLeg/TickLeg/StopLeg) every
// action reuses: a leg either walks its actor along the package (`active`) or
// runs on the game-time simulation (`simulated`). A simulated leg whose mover
// becomes loaded upgrades to a live walk. The fetch flow's recovery policy is
// "blocked while visible": an executor whose live, on-screen walk times out is
// visibly stuck, so it turns around and reports; an off-screen walk or a
// simulated leg completes via the teleport fallback. Assassinate diverges: a
// staged scene plays when the pair is 3D-loaded, and an off-screen completion is
// the kill itself - no teleport.

#include "tasks.h"

#include "fuzzy.h"
#include "movement.h"
#include "npc_index.h"
#include "panel.h"
#include "places.h"
#include "skyrimnet_bridge.h"
#include "vm_dispatch.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace intellite::tasks
{
	namespace
	{
		// Co-save identity. One task record type, version 4 (versions 1-3
		// records are still read; the newer fields default), plus a separate
		// settings record (SETT v1) holding the confirmation preference.
		constexpr std::uint32_t kSerializationID = 'ILIT';
		constexpr std::uint32_t kRecordTasks     = 'TASK';
		constexpr std::uint32_t kTaskVersion     = 4;
		constexpr std::uint32_t kRecordSettings  = 'SETT';
		constexpr std::uint32_t kSettingsVersion = 1;

		// Travel simulation. Skyrim's game-time unit here is game DAYS
		// (Calendar::GetCurrentGameTime), so an hours estimate is divided by 24.
		// Distances and estimates are anchored (see ResolveAnchor): a destination
		// inside a building has no worldspace, so it is measured through the
		// exterior door its cell's teleport door leads to.
		constexpr float kMinTravelHours  = 0.05f;
		constexpr float kMaxTravelHours  = 7.0f;      // per-leg estimate cap (the longest real pair is ~5.8 h)
		constexpr float kCrossWorldHours = 0.5f;
		constexpr float kReferenceDistance  = 37200.0f; // anchored distance that estimates as one game hour
		constexpr float kDistanceExponent   = 1.15f;    // long journeys estimate slightly slower per unit
		constexpr float kTravelSpeedFactor  = 2.0f;     // agents travel at 2x the map-walking calibration (a runner / horse)
		constexpr float kDeadlineGraceHours = 1.0f;     // force-arrive this much past the estimate
		constexpr float kStationaryConfirmSeconds = 9.0f; // post-deadline stillness before staging
		constexpr int   kMapLocationHops    = 6;        // parent-location hops when resolving a marker
		constexpr float kArriveDistance  = 300.0f;      // every leg's arrival radius
		constexpr float kTeleportOffset  = 250.0f;      // units behind the anchor
		constexpr float kNavmeshSnap     = 50.0f;
		constexpr float kLingerReleaseDistance = 800.0f; // linger ends past this
		constexpr float kMeetArriveDistance = 400.0f;    // player-arrived-at-meet radius
		constexpr float kMeetWaitHours      = 6.0f;      // grace past meetGameTime
		constexpr float kDepartEstimateRefreshSeconds = 2.0f;  // pending-meet departure-estimate recompute window (plus a cell-change trigger)

		// Per-actor state keys stored through StorageUtil (read back by the
		// character-bio awareness submodule via the native papyrus_util
		// decorator). A Meet sets IntelLite_Meeting to the meeting place while
		// it is scheduled/active; an Assassinate sets IntelLite_Contract to the
		// target and fee for the life of the task. Each is cleared on
		// completion, failure, or cancellation.
		constexpr const char* kMeetingStateKey  = "IntelLite_Meeting";
		constexpr const char* kMeetingTimeKey   = "IntelLite_MeetingTime";
		constexpr const char* kContractStateKey = "IntelLite_Contract";

		// Meet "Here" spots: authored persistent XMarkers in IntelLiteData.esp
		// (0x801-0x804), leased one per Here-meet. The marker is moved to the
		// player's spot when the arrangement is made and becomes the meet's
		// travel destination.
		constexpr const char* kMeetDataPlugin = "IntelLiteData.esp";
		constexpr std::size_t kMeetSpotCount = 4;
		constexpr RE::FormID  kMeetSpotIDs[kMeetSpotCount] = { 0x801, 0x802, 0x803, 0x804 };

		constexpr const char* kSurvivorStateKey = "IntelLite_Survivor";
		constexpr const char* kSurvivorTimeKey  = "IntelLite_SurvivorTime";
		// kSurvivorStateKey is set on the VICTIM of a failed FAR-AWAY
		// assassination attempt (never 3D-loaded, never saw the attacker); the
		// bio submodule renders a survivor paragraph from it plus the float
		// game-day time. Never cleared; a repeated attempt overwrites the time.

		// Assassination. A 3D-loaded victim (Path B) is approached with the
		// sneak package and killed in a staged scene; an unloaded one (Path A)
		// is reached by the game-time simulation and killed off-screen.
		// kKillDamage is well past any actor's health.
		constexpr float kKillBeatSeconds = 3.0f;
		constexpr float kKillStandUpSeconds = 0.8f;
		constexpr float kKillConfirmSeconds = 2.0f;   // real window after the killmove beat for the animation's own kill to land
		constexpr float kKillDamage      = 1000.0f;
		constexpr float kKillArriveDistance = 150.0f;   // assassinate outbound leg: walk right up to the victim
		constexpr float kSwingArriveDistance = 70.0f;   // essential staged swing: the executor must start the swing within knife reach - the UseWeapon package will not close the gap itself
		constexpr float kFurnitureClearDistance = 100.0f;  // moved at least this far from the furniture = it really left
		constexpr float kKillTargetMoveDistance = 100.0f;  // reposition distance in front of the executor
		constexpr float kKillCleanupWaitSeconds = 6.0f;   // cap on waiting for the killmove to release the pair before the weapon cleanup
		constexpr float kKillWalkMinDistance = 80.0f;      // a furniture target must move this far from its spot for the kill
		constexpr float kKillWalkProgressDistance = 30.0f; // movement that refreshes the walk's no-progress clock
		constexpr float kKillWalkStallSeconds = 5.0f;      // no progress for this long -> the walk has failed
		constexpr float kKillWalkHardCapSeconds = 30.0f;   // absolute cap on the furniture walk
		constexpr float kFurnitureStandUpSeconds = 1.5f;   // the get-up animation's beat before the furniture drive finishes
		// The target walks toward the player while the player follows the
		// executor, so the pair closes at run pace + walk pace:
		// ~1.5 s * (350-400 + 100-150) = 675-825 u, plus
		// kKillArriveDistance (150) -> 825-975. 1200 leaves margin; below
		// it the arrival clear/settle is the backstop.
		constexpr float kLateFurnitureDriveMinLead = 1200.0f;
		constexpr float kKillVerdictGraceSeconds = 4.0f;  // fallback window for a deferred kill: after the killmove releases, an alive target gets this long to die
		constexpr float kKillSettleSeconds = 0.8f;       // stillness between the pair staging and the killmove dispatch (the engine needs a settled pair)
		constexpr float kKillEngageWaitSeconds = 1.2f;   // after a dispatch, how long the engine's killmove state may take to appear before the attempt is declared refused
		constexpr float kKillMaxPreDispatchDistance = 400.0f;  // re-stage once when the settle drifted the pair past this
		constexpr int   kKillMaxDispatches = 6;          // total killmove dispatches per scene (1 + up to 5 re-stage retries)
		constexpr float kSwingSeconds = 45.0f;           // hard cap for the one-package essential run: the swing package can take seconds to engage on an occupied actor, so this only bounds a run that never engages or attacks; the release normally fires when the attack completes
		constexpr float kSwingReissueSeconds = 2.5f;     // re-assert the swing package this often while it is not yet the running package
		constexpr float kSwingAttackHoldSeconds = 2.5f;  // release at most this long after the attack was first seen - bounds a stand-staring tail if the attack state lingers
		constexpr int   kSwingMaxReissues = 4;           // cap on swing re-issues: a package that never engages is not retried forever

		// Real-walking (IntelEngine-style package travel). The walk is the
		// primary mechanism: any reachable mover walks - a far destination leaves
		// the loaded area and the off-screen game-time deadline completes it. A
		// live walk's timeout is a
		// per-leg NO-PROGRESS budget: nothing beyond anchored/combat gates stops a
		// walk from starting, and recovery fires only after the mover has made no
		// progress for kWalkTimeoutSeconds. Anything the package cannot reach
		// keeps the simulated fallback.
		constexpr float kWalkTimeoutSeconds = 30.0f;    // real seconds with no progress before recovery
		constexpr float kProgressDistance   = 200.0f;   // movement that refreshes the stuck timer
		constexpr float kApproachDistance   = 7000.0f;  // visible return approach distance; stays inside the player's loaded 5x5 cell grid (~8192u guaranteed at uGridsToLoad=5, the same ceiling as the outbound stage's 7000 clamp)
		constexpr float kKeepStagedDistance = 4000.0f;  // deliver/fetch Recover fallback only: an agent already within this of a loaded anchor keeps its spot (deliberately NOT widened with the return stage - the Deliver path completes on the spot)
		constexpr float kOutboundApproachDistance = 3500.0f; // visible outbound approach / fallback distance when the destination is on screen
		constexpr float kApproachDistanceInterior = 700.0f;  // staging distance when the player/destination is inside a building
		constexpr float kDoorReachDistance  = 300.0f;   // "at the door" radius
		constexpr float kDoorResetDistance  = 450.0f;   // beyond this, reset the stall timer
		constexpr float kDoorWaitSeconds    = 6.0f;     // stall at a door before stepping through / turning back
		constexpr float kPairJoinDistance = 3000.0f;   // agent's return starts when the target is this close to the player
		constexpr float kAgentJoinOffset = 120.0f;     // agent's offset beside the target on the final approach
		constexpr float kPairStageDistance = 1500.0f;  // stage the pair together only when one is farther than this
		constexpr float kEntranceMinDistance = 500.0f;
		constexpr float kEntranceMaxDistance = 15000.0f; // sanity range for entrance-marker staging
		constexpr float kAgentRendezvousDistance = 800.0f;   // fetch: snap the agent to the target when it ended the outbound leg farther than this

		constexpr std::size_t kMaxStringLen = 1024;
		constexpr std::size_t kMaxTasks     = 256;
		constexpr std::size_t kMaxPending   = 1024;
		constexpr int         kPumpIntervalMs = 250;

		// One travel leg, shared by every action. `active` is a live package
		// walk; `simulated` is the game-time fallback (only one is ever set);
		// `offscreen` is display/telemetry set by TickLeg. The FormIDs are what
		// a leg resolves its mover and destination from, so a leg survives a
		// frame boundary without holding raw pointers.
		struct Leg
		{
			bool          active = false;
			bool          simulated = false;
			bool          offscreen = false;
			bool          approached = false;  // a return leg was already re-placed from a distance
			bool          doorStaged = false;   // this leg was already restaged at a building door
			int           retargets = 0;        // no-progress retargets at a moving actor (cap 3)
			float         evalAtReal = 0.0f;
			int           evalLeft = 0;
			std::uint32_t moverFormID = 0;
			std::uint32_t destFormID = 0;
			float         dueGameTime = 0.0f;

			// No-progress budget: the mover's position and the real time at the
			// last point it advanced more than kProgressDistance. A live walk
			// recovers only once this goes stale for kWalkTimeoutSeconds.
			float         progressX = 0.0f;
			float         progressY = 0.0f;
			float         progressZ = 0.0f;
			float         lastProgressReal = 0.0f;

			// A named-place destination inside a building is entered through its
			// door: the package walks to the exterior door (`walkRefFormID`) and
			// the leg's `destFormID` is the FINAL interior door. `entryFormID`
			// holds that interior door while the step-through is pending and is
			// cleared once the mover has been moved inside. Both are 0 on an
			// ordinary leg, where `destFormID` is itself the walk target.
			std::uint32_t walkRefFormID = 0;
			std::uint32_t entryFormID = 0;

			// Anchored (runtime-only) travel state. `hasAnchor` is set when both
			// the mover and the destination resolve an anchor (a destination
			// inside a building resolves through its exterior door). The stored
			// anchor is the DESTINATION's anchor position and anchorDistance is
			// the anchored mover->destination distance the walk gate and the
			// travel estimate use. destInterior flags a destination with no
			// worldspace, whose blocked-door stall AdvanceAgentTravel watches.
			bool          hasAnchor = false;
			float         anchorX = 0.0f;
			float         anchorY = 0.0f;
			float         anchorZ = 0.0f;
			float         anchorDistance = 0.0f;
			bool          destInterior = false;
			float         doorWaitStart = 0.0f;
			bool          deadlineFired = false;  // runtime-only: cut off by the estimate deadline, not a visible stall
			float         deadlineSeenReal = 0.0f;   // runtime-only: first real second past the off-screen deadline
			float         startGameTime = 0.0f;      // runtime-only: game time when the leg started
			std::uint32_t destCellFormID = 0;        // runtime-only: the destination's parent cell at plan time
			std::uint32_t destWorldspaceFormID = 0;  // runtime-only: the destination's worldspace at plan time
			bool          sneak = false;          // runtime-only: this leg's package walk uses the sneak approach package
			bool          strictArrival = false;  // runtime-only: arrival needs real proximity - the shared-interior shortcut is disabled (assassinate approach)
			bool          pendingStart = false;  // deferred: the mover was unloaded at StartLeg; the package applies on the first loaded tick
			float         pkgWatchdogReal = 0.0f; // runtime-only: next check that the expected travel package is the mover's running package
		};

		// RunningPackageOf (defined below) reads the package an actor is
		// actually running; declared here because functions above use it.
		struct RunningPkg
		{
			RE::FormID  form = 0;
			const char* id = "-";
		};
		RunningPkg RunningPackageOf(RE::Actor* a_actor);

		enum class LegStatus : std::uint8_t
		{
			Travelling,
			Arrived,
			Recover,
			Invalid
		};

		// Per-dispatch VM callback state for PlayIdleWithTarget (shared between
		// the owning Task and the deferred ResultStackCallback so concurrent
		// assassinations never overwrite each other's result).
		struct KillVmResult
		{
			std::atomic<bool>          have{ false };
			std::atomic<std::uint32_t> agent{ 0 };
			std::atomic<std::uint32_t> target{ 0 };
			std::atomic<bool>          result{ false };
		};

		struct Task
		{
			std::uint32_t id = 0;
			Type          type = Type::Fetch;
			State         state = State::Pending;

			// --- serialized fields (co-save layout: do not reorder or resize) ---
			std::uint32_t agentFormID = 0;
			std::uint32_t targetFormID = 0;
			std::string   agentName;
			std::string   targetName;
			float         dueGameTime = 0.0f;       // target leg's serialized deadline
			float         createdGameTime = 0.0f;
			std::uint32_t returnWorldspaceFormID = 0;
			float         returnX = 0.0f;
			float         returnY = 0.0f;
			float         returnZ = 0.0f;
			// v2 additions (appended after the v1 set; a v1 record defaults them).
			std::string   message;                  // Deliver: the message text
			std::uint32_t locationFormID = 0;       // Meet: destination (or player)
			float         meetGameTime = 0.0f;      // Meet: scheduled meet deadline
			float         departGameTime = 0.0f;    // Meet: when the agent sets off
			// v3 addition (appended after the v2 set; v1/v2 records default it).
			std::string   sender;                   // Deliver: the message's original author

			// --- runtime-only travel state (not serialized) ---
			std::uint32_t returnCellFormID = 0;
			// agentLeg: outbound agent -> target (fetch/deliver) or the meeting
			// place (meet). agentReturnLeg: the agent's own leg back to the
			// player (both the normal return and the unreachable-report walk).
			// targetLeg: the target's leg to the player (fetch only).
			Leg  agentLeg;
			Leg  agentReturnLeg;
			Leg  targetLeg;
			bool          targetArrived = false;
			bool          agentDone = false;
			bool          agentHeldOffscreen = false;  // runtime-only: Fetch executor held off-screen with SetDontMove while target is off-screen
			bool          outboundFailed = false;
			bool refusalFarAway = true;  // Assassinate: true when the refusal return was off-screen / never 3D-loaded

			// Meet pending: the throttled departure estimate (recomputed when its
			// inputs can have changed - a cell change or the throttle window).
			float         departEstimateHours = 0.0f;
			float         departEstimateRefreshReal = 0.0f;
			std::uint32_t estimateAgentCellFormID = 0;
			std::uint32_t estimateDestCellFormID = 0;
			bool          estimateFallbackLogged = false;

			// Meet only: the exterior door a named-place destination inside a
			// building is walked to before stepping inside (runtime-only; a
			// restored meet re-resolves through the destination's own anchor).
			std::uint32_t placeWalkRefFormID = 0;

			// --- Assassinate runtime-only state (not serialized; no co-save
			// bump). `killCommitted` marks the kill scene (the target is
			// 3D-loaded in the executor's cell), which is the not-cancellable
			// state; `killBeatEndReal` is the real-second deadline of that
			// beat; `sneakApproach` marks the Path B sneak package;
			// `killFurniture`/`killFurnitureFormID` carry the scene-start
			// furniture state. ---
			bool          killCommitted = false;
			bool          killPlayed = false;   // runtime-only: the staged animation was dispatched
			std::uint32_t spawnedWeaponFormID = 0;  // the dagger we spawned for the kill (serialized since TASK v4)
			bool          killAttempted = false;    // runtime-only: the kill verdict window has started
			float         killConfirmEndReal = 0.0f;  // runtime-only: the kill-verdict confirm window end
			bool          killPending = false;  // runtime-only: standing the target up before the animation
			bool          weaponDrawnByUs = false;  // runtime-only: we drew a weapon for the pair idle
			float         killBeatEndReal = 0.0f;
			bool          sneakApproach = false;      // runtime-only: the outbound leg uses the sneak approach package
			bool          killFurniture = false;      // runtime-only: the target was on furniture at scene start
			std::uint32_t killFurnitureFormID = 0;    // runtime-only: that furniture ref, for the left-it check
			bool          killCleanupWaiting = false;      // runtime-only: the cleanup wait has been logged
			float         killCleanupDeadlineReal = 0.0f;  // runtime-only: cap on the killmove-release wait
			bool          killWalkPending = false;        // runtime-only: the target is walking clear of its furniture
			float         killWalkHardCapReal = 0.0f;    // runtime-only: absolute cap on that walk
			float         killWalkStartX = 0.0f;          // runtime-only: the target's position when the walk started
			float         killWalkStartY = 0.0f;
			float         killWalkStartZ = 0.0f;
			float         killWalkProgressX = 0.0f;       // runtime-only: position at the last progress refresh
			float         killWalkProgressY = 0.0f;
			float         killWalkProgressZ = 0.0f;
			float         killWalkLastProgressReal = 0.0f; // runtime-only: when the target last advanced
			float         furnitureStandUpReal = 0.0f;   // runtime-only: the get-up beat's deadline (0 = no beat pending)
			bool          furnitureDrivePending = false; // runtime-only: mortal target was unloaded at contract start; drive it when it loads
			float         killVerdictGraceEndReal = 0.0f; // runtime-only: the post-killmove death grace
			bool          killEngaged = false;         // runtime-only: the engine's killmove state appeared after a dispatch
			bool          killReleased = false;        // runtime-only: an engaged killmove left the state (logged once)
			bool          killWaitingEngage = false;   // runtime-only: a dispatch is outstanding, waiting for engagement
			bool          killReformUsed = false;      // runtime-only: the pre-dispatch drift re-stage already ran
			int           killDispatchCount = 0;       // runtime-only: killmove dispatches issued for this scene
			float         killDispatchReal = 0.0f;     // runtime-only: when the last dispatch was issued
			float         killPrepareEndReal = 0.0f;   // runtime-only: the settle beat's end; the dispatch runs on the tick it expires
			float         killEngageDeadlineReal = 0.0f; // runtime-only: when a dispatch that has not engaged is declared refused
			float         killEngagedAtReal = 0.0f;    // runtime-only: when engagement was first observed
			bool          targetHeld = false;          // runtime-only: the target is under a SetDontMove hold
			float         killWalkStartReal = 0.0f;    // runtime-only: when the target's walk package was applied
			bool          killWalkPkgLogged = false;   // runtime-only: the one-shot walk-package check was logged
			bool          killWalkAfterAttempts = false; // runtime-only: the close-in attempts run first; on refusal the target walks (movement route)
			int           killEarlyAttemptsLeft = 0;     // runtime-only: extra close-in attempts before the movement route (1 = two attempts total)
			int           killWalkRetargets = 0;         // runtime-only: fetch-style walk re-issues on a stall (cap 2)
			int           killWalkEvalLeft = 0;          // runtime-only: fetch-style package re-evaluations left (0.3 s, then +1.2 s)
			float         killWalkEvalAtReal = 0.0f;     // runtime-only: next re-evaluation time
			bool          protectedCleared = false;       // runtime-only: we cleared the target's kProtected flag for this contract
			bool          protectedReCleared = false;     // runtime-only: the first engine re-set has been logged
			bool          targetBaseProtected = false;    // runtime-only: the target's base record carried Protected when the contract started
			bool          targetAliasProtected = false;   // runtime-only: the target's quest alias(es) carried Protected when the contract started
			bool          reapproached = false;           // runtime-only: a refused scene already got its one fresh re-approach
			bool          killPulseLogged = false;         // runtime-only: the anti-routine pulse has logged a caught re-attachment
			float         lingerReassertReal = 0.0f;  // runtime-only: next linger sandbox re-assert deadline
			bool          targetSandboxed = false;      // runtime-only: the target's linger sandbox is already applied
			float         lingerGraceUntilReal = 0.0f;  // runtime-only: no distance release before this real second (post-arrival streaming grace)
			bool          swingPending = false;         // runtime-only: an essential staged swing is playing (essential-target swing - runtime only, not serialized)
			float         swingEndReal = 0.0f;          // runtime-only: the swing beat's end (essential-target swing - runtime only, not serialized)
			bool          swingSawAttack = false;       // runtime-only: the swing attack has been observed (essential-target swing - runtime only, not serialized)
			float         swingAttackSeenReal = 0.0f;   // runtime-only: real second the swing attack was first observed (essential-target swing - runtime only, not serialized)
			float         swingCheckReal = 0.0f;        // runtime-only: the next swing diagnostic log's deadline (essential-target swing - runtime only, not serialized)
			int           swingReissues = 0;             // runtime-only: how many times the swing package has been re-issued (essential-target swing - runtime only, not serialized)
			float         swingReissueReal = 0.0f;       // runtime-only: the next swing re-issue deadline (essential-target swing - runtime only, not serialized)
			std::shared_ptr<KillVmResult> killVmResult;  // runtime-only: per-task deferred PlayIdleWithTarget result
		};

		float EstimateHours(RE::TESObjectREFR* a, RE::TESObjectREFR* b);

		// Forward declarations: the base-record and quest-alias Protected toggles
		// are defined just before RestoreProtectedFlag but are first used by
		// PlayKillMove.
		void SetBaseProtected(RE::Actor* a_target, bool a_protected);
		void SetAliasProtected(RE::Actor* a_target, bool a_protected);

		// Forward declaration: the panel/eligibility snapshot is rebuilt by Tick and
		// every game-thread mutation; the co-save callbacks refresh it too.
		void RefreshSnapshot();

		// g_tasks and every Task field are GAME-THREAD-ONLY: Tick(), the Add*
		// functions and the co-save callbacks (which SKSE invokes on the main
		// thread) are the only touchers. The cross-thread surface is the snapshot
		// below, the queued cancels, and the confirmation atomic.
		std::vector<Task>          g_tasks;
		std::vector<std::uint32_t> g_pendingHoldReleases;  // game thread only: saved assassinate targets whose SetDontMove/SetRestrained hold must be cleared after a load
		// Game thread only: kill daggers spawned by tasks that were DROPPED at load
		// (their runtime record is gone, so the dagger must be removed without it).
		struct PendingWeapon { std::uint32_t agent = 0; std::uint32_t weapon = 0; };
		std::vector<PendingWeapon> g_pendingWeaponReleases;
		std::uint32_t              g_nextID = 1;

		// The panel + eligibility snapshot: rebuilt by RefreshSnapshot() on the game
		// thread (every Tick, after every Add*, and after a co-save load/revert) and
		// read from any thread under g_snapshotMutex.
		std::mutex                 g_snapshotMutex;
		std::string                g_snapshotJson = "[]";
		std::vector<std::uint32_t> g_activeAgentSnapshot;  // agents with a non-finished, non-lingering task
		std::vector<std::uint32_t> g_cancellableAgentSnapshot;  // agents with a non-finished, cancellable task
		std::vector<std::uint32_t> g_committedAgentSnapshot;  // agents locked by a started kill scene
		bool                       g_anyActiveSnapshot = false;

		// Cancellations queued from other threads; drained by the next Tick().
		std::mutex                 g_cmdMutex;
		std::vector<std::uint32_t> g_pendingCancel;
		std::vector<std::uint32_t> g_pendingCancelAgent;
		bool                       g_cancelAll = false;

		std::atomic<bool>          g_confirmPrompts{ true };  // SETT v1

		std::atomic<bool>  g_stop{ false };
		std::atomic<bool>  g_pumpStarted{ false };
		std::atomic<float> g_nowCache{ 0.0f };  // last Now(), game thread only; read by RefreshSnapshot
		std::thread        g_pump;
		bool               g_firstTickLogged = false;  // game thread only
		bool               g_cleanupPersistedTravel = false;  // game thread only

		// ---- game-time helpers (game thread only) ---------------------------

		float Now()
		{
			if (auto* cal = RE::Calendar::GetSingleton()) {
				return cal->GetCurrentGameTime();  // game days
			}
			return 0.0f;
		}

		float HoursToDays(float a_hours)
		{
			return a_hours / 24.0f;
		}

		// Monotonic real seconds, used only for the walking timeout (game-time
		// does not advance while paused, so it is the wrong clock here). Game
		// thread only.
		float RealSeconds()
		{
			using clock = std::chrono::steady_clock;
			return std::chrono::duration<float>(clock::now().time_since_epoch()).count();
		}

		// Two references are in the same "space" when they share a worldspace,
		// or (both interior) share a parent cell. Anything else is treated as
		// far away, so an interior-to-exterior hop simulates cross-world travel.
		bool SameSpace(RE::TESObjectREFR* a, RE::TESObjectREFR* b)
		{
			if (!a || !b) return false;
			auto* wa = a->GetWorldspace();
			auto* wb = b->GetWorldspace();
			if (wa != wb) return false;
			if (wa == nullptr) return a->GetParentCell() == b->GetParentCell();
			return true;
		}

		// Canonical reference position inside an interior cell for choosing its
		// main entrance door: prefers the cell's BGSLocation worldLocMarker when
		// that marker sits inside the cell (Bethesda places it in the main hall
		// by the front entrance, e.g. WhiterunDragonsreachCenterMarker 0001F890),
		// falling back to `a_ref`'s position when `a_ref` is inside the cell.
		bool CellMainRefPos(RE::TESObjectCELL* a_cell, RE::TESObjectREFR* a_ref, RE::NiPoint3& a_out)
		{
			if (!a_cell) return false;
			if (auto* loc = a_cell->GetLocation()) {
				const RE::ObjectRefHandle mh = loc->worldLocMarker;
				auto mp = mh.get();
				if (auto* m = mp.get(); m && m->GetParentCell() == a_cell) {
					a_out = m->GetPosition();
					return true;
				}
			}
			if (a_ref && a_ref->GetParentCell() == a_cell) {
				a_out = a_ref->GetPosition();
				return true;
			}
			return false;
		}

		// Deterministic main-door comparison for an interior cell:
		//   1. Unlocked pair beats locked pair.
		//   2. Closest interior distance to the cell's main reference point
		//      (worldLocMarker or target ref).
		//   3. Lowest exterior Z (street level beats an elevated porch/balcony).
		//   4. Lowest interior door FormID.
		bool IsBetterCellDoor(
			bool a_haveRefPos, const RE::NiPoint3& a_refPos,
			RE::TESObjectREFR* a_intDoor, RE::TESObjectREFR* a_extDoor,
			RE::TESObjectREFR* a_bestIntDoor, RE::TESObjectREFR* a_bestExtDoor)
		{
			if (!a_bestIntDoor || !a_bestExtDoor) return true;
			const bool unlocked     = !a_intDoor->IsLocked() && !a_extDoor->IsLocked();
			const bool bestUnlocked = !a_bestIntDoor->IsLocked() && !a_bestExtDoor->IsLocked();
			if (unlocked != bestUnlocked) return unlocked;

			if (a_haveRefPos) {
				const float d     = a_intDoor->GetPosition().GetDistance(a_refPos);
				const float bestD = a_bestIntDoor->GetPosition().GetDistance(a_refPos);
				if (std::abs(d - bestD) > 1.0f) return d < bestD;
			}

			const float extZ     = a_extDoor->GetPosition().z;
			const float bestExtZ = a_bestExtDoor->GetPosition().z;
			if (std::abs(extZ - bestExtZ) > 1.0f) return extZ < bestExtZ;

			return a_intDoor->GetFormID() < a_bestIntDoor->GetFormID();
		}

		// Resolve a reference's travel anchor: the position a walk measures to.
		// An exterior (worldspace) reference is simply its own position. An
		// interior reference has no worldspace, so its anchor is the position of
		// the main exterior door it leads through: iterate the parent cell's
		// references for teleport doors (extraList -> ExtraTeleport) whose
		// linked door is in a worldspace, and pick the deterministic main door.
		// Returns false when no such door is found. Game thread only.
		bool ResolveAnchor(RE::TESObjectREFR* a_ref, RE::NiPoint3& a_out)
		{
			if (!a_ref) return false;

			if (a_ref->GetWorldspace()) {
				a_out = a_ref->GetPosition();
				return true;
			}

			auto* cell = a_ref->GetParentCell();
			if (!cell) return false;

			auto findInCell = [](RE::TESObjectCELL* a_cell, RE::TESObjectREFR* a_targetRef, RE::NiPoint3& a_pos) -> bool {
				RE::NiPoint3 refPos{};
				const bool   haveRefPos = CellMainRefPos(a_cell, a_targetRef, refPos);

				RE::TESObjectREFR* bestInt = nullptr;
				RE::TESObjectREFR* bestExt = nullptr;
				a_cell->ForEachReference([&](RE::TESObjectREFR* a_refr) {
					if (!a_refr || a_refr->IsDisabled() || a_refr->IsDeleted()) {
						return RE::BSContainer::ForEachResult::kContinue;
					}
					auto* teleport = a_refr->extraList.GetByType<RE::ExtraTeleport>();
					if (!teleport || !teleport->teleportData) {
						return RE::BSContainer::ForEachResult::kContinue;
					}
					// Copy the handle before resolving: the non-const get() clears
					// the stored handle, and this door's teleport data belongs to the
					// game.
					const RE::ObjectRefHandle linked = teleport->teleportData->linkedDoor;
					auto linkedRef = linked.get();
					auto* target = linkedRef.get();
					if (!target || target->IsDisabled() || target->IsDeleted() || !target->GetWorldspace()) {
						return RE::BSContainer::ForEachResult::kContinue;
					}
					if (IsBetterCellDoor(haveRefPos, refPos, a_refr, target, bestInt, bestExt)) {
						bestInt = a_refr;
						bestExt = target;
					}
					return RE::BSContainer::ForEachResult::kContinue;
				});
				if (bestExt) {
					a_pos = bestExt->GetPosition();
					return true;
				}
				return false;
			};

			if (findInCell(cell, a_ref, a_out)) return true;

			// Two-hop interior fallback (a sub-cell exiting through a main interior cell).
			bool found = false;
			cell->ForEachReference([&](RE::TESObjectREFR* a_refr) {
				if (found) return RE::BSContainer::ForEachResult::kStop;
				if (!a_refr || a_refr->IsDisabled() || a_refr->IsDeleted()) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				auto* teleport = a_refr->extraList.GetByType<RE::ExtraTeleport>();
				if (!teleport || !teleport->teleportData) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				const RE::ObjectRefHandle linked = teleport->teleportData->linkedDoor;
				auto linkedRef = linked.get();
				auto* target = linkedRef.get();
				if (!target || target->IsDisabled() || target->IsDeleted() || target->GetWorldspace()) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				auto* midCell = target->GetParentCell();
				if (!midCell || midCell == cell || !midCell->IsInteriorCell()) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				if (findInCell(midCell, target, a_out)) {
					found = true;
					return RE::BSContainer::ForEachResult::kStop;
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			return found;
		}

		std::string ClampString(std::string a_str, std::size_t a_max)
		{
			return fuzzy::ClampUtf8(std::move(a_str), a_max);
		}

		std::string DisplayName(RE::Actor* a)
		{
			const char* n = a ? a->GetDisplayFullName() : nullptr;
			return ClampString((n && *n) ? std::string(n) : std::string("Unknown"), kMaxNameChars);
		}

		std::string PlayerName()
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			const char* n = player ? player->GetDisplayFullName() : nullptr;
			return (n && *n) ? std::string(n) : std::string("the player");
		}

		// Set (or, with an empty value, clear) a per-actor StorageUtil string.
		// Game thread only (it dispatches through the Papyrus VM).
		void SetActorStateKey(RE::Actor* a_actor, const char* a_key, const std::string& a_value)
		{
			if (a_actor) {
				skyrimnet::SetActorState(a_actor, a_key, a_value);
			}
		}

		// Clear whichever per-actor state key a task owns (Meet or Assassinate).
		// Fetch/Deliver own none. Game thread only.
		void ClearTaskStateKey(const Task& a_task)
		{
			if (a_task.type != Type::Meet && a_task.type != Type::Assassinate) return;
			auto* agent = RE::TESForm::LookupByID<RE::Actor>(a_task.agentFormID);
			if (!agent) return;
			if (a_task.type == Type::Meet) {
				SetActorStateKey(agent, kMeetingStateKey, "");
				skyrimnet::SetActorFloat(agent, kMeetingTimeKey, 0.0f);
			} else {
				SetActorStateKey(agent, kContractStateKey, "");
			}
		}

		// The NPC base record's raw ACBS flags (Essential / Protected). These are
		// data reads and work for actors that have never been loaded, unlike the
		// runtime Actor::IsEssential()/IsProtected() reads. Mirrors the base-flag
		// access used by the protected clear above. Game thread only.
		bool BaseIsEssential(RE::Actor* a_actor)
		{
			if (!a_actor) return false;
			if (auto* npc = a_actor->GetActorBase();
				npc && npc->actorData.actorBaseFlags.all(RE::ACTOR_BASE_DATA::Flag::kEssential)) {
				return true;
			}
			if (auto* tpl = a_actor->GetTemplateBase();
				tpl && tpl->actorData.actorBaseFlags.all(RE::ACTOR_BASE_DATA::Flag::kEssential)) {
				return true;
			}
			return false;
		}

		bool BaseIsProtected(RE::Actor* a_actor)
		{
			if (!a_actor) return false;
			if (auto* npc = a_actor->GetActorBase();
				npc && npc->actorData.actorBaseFlags.all(RE::ACTOR_BASE_DATA::Flag::kProtected)) {
				return true;
			}
			if (auto* tpl = a_actor->GetTemplateBase();
				tpl && tpl->actorData.actorBaseFlags.all(RE::ACTOR_BASE_DATA::Flag::kProtected)) {
				return true;
			}
			return false;
		}

		// Quest-alias essentiality / protection: Actor::IsEssential() /
		// IsProtected() only inspect the runtime boolFlags, and the engine
		// continually re-asserts runtime kProtected from quest aliases (e.g.
		// DialogueCarriageSystem on Bjorlam, Stables on Skulvar) whenever AI or
		// packages evaluate. Inspecting ExtraAliasInstanceArray detects both
		// alias-driven essentiality and alias-driven protection. Game thread only.
		bool AliasIsEssential(RE::Actor* a_actor)
		{
			if (!a_actor) return false;
			auto* extraAliases = a_actor->extraList.GetByType<RE::ExtraAliasInstanceArray>();
			if (!extraAliases) return false;
			RE::BSReadLockGuard lock(extraAliases->lock);
			for (const auto* inst : extraAliases->aliases) {
				if (!inst || !inst->alias) continue;
				if (inst->quest && !inst->quest->IsEnabled()) continue;
				if (inst->alias->IsEssential()) return true;
			}
			return false;
		}

		bool AliasIsProtected(RE::Actor* a_actor)
		{
			if (!a_actor) return false;
			auto* extraAliases = a_actor->extraList.GetByType<RE::ExtraAliasInstanceArray>();
			if (!extraAliases) return false;
			RE::BSReadLockGuard lock(extraAliases->lock);
			for (const auto* inst : extraAliases->aliases) {
				if (!inst || !inst->alias) continue;
				if (inst->quest && !inst->quest->IsEnabled()) continue;
				if (inst->alias->IsProtected()) return true;
			}
			return false;
		}

		bool IsTargetEssential(RE::Actor* a_actor)
		{
			return a_actor && (a_actor->IsEssential() || BaseIsEssential(a_actor) || AliasIsEssential(a_actor));
		}

		bool IsTargetProtected(RE::Actor* a_actor)
		{
			return a_actor && (a_actor->IsProtected() || BaseIsProtected(a_actor) || AliasIsProtected(a_actor));
		}

		// ---- gold / fee (game thread only) ----------------------------------

		// The gold form: resolved once (with a direct Skyrim.esm fallback). Gold
		// is a MISC record; TESBoundObject is the base the inventory API takes.
		RE::TESBoundObject* ResolveGold()
		{
			static RE::TESBoundObject* gold = []() -> RE::TESBoundObject* {
				if (auto* g = RE::TESForm::LookupByEditorID<RE::TESBoundObject>("Gold001")) {
					return g;
				}
				if (auto* dataHandler = RE::TESDataHandler::GetSingleton()) {
					return dataHandler->LookupForm<RE::TESBoundObject>(0x0000000F, "Skyrim.esm");
				}
				return nullptr;
			}();
			return gold;
		}

		// Pay `a_fee` gold from the player and hand the same amount to the
		// executor (`a_agent`). Returns false (charging nothing) when the gold
		// form cannot be resolved or the player holds fewer than `a_fee`. A zero
		// fee is a no-op success (the contract is simply free), so nothing is
		// charged. Only the player pays, so a pointer that is not the player is
		// refused. Game thread only.
		bool PayFee(RE::Actor* a_player, RE::Actor* a_agent, std::uint32_t a_fee)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player || a_player != player) return false;
			if (a_fee == 0) return true;  // no fee: skip the charge

			auto* gold = ResolveGold();
			if (!gold) {
				logs::warn("IntelLite tasks: gold (Gold001) could not be resolved");
				return false;
			}
			if (static_cast<std::int64_t>(player->GetItemCount(gold)) <
				static_cast<std::int64_t>(a_fee)) {
				return false;
			}
			const std::int32_t count = static_cast<std::int32_t>(a_fee);
			player->RemoveItem(gold, count, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
			// The fee reaches the executor: same count into the agent's
			// inventory. AddObjectToContainer is the engine entry point the
			// Actor.AddItem native calls (this CommonLib has no AddItem).
			if (a_agent) {
				a_agent->AddObjectToContainer(gold, nullptr, count, nullptr);
			}
			return true;
		}

		// ---- the leg mechanism (game thread only) ---------------------------

		// Release a leg's live package walk, then reset it. Safe on an inactive
		// leg. Game thread only.
		void StopLeg(Leg& a_leg)
		{
			if (a_leg.active) {
				if (auto* mover = RE::TESForm::LookupByID<RE::Actor>(a_leg.moverFormID)) {
					movement::StopTravel(mover);
				}
			}
			a_leg = Leg{};
		}

		// Stamp the mover's current position and the real time as the leg's last
		// progress point (the no-progress budget's reference). Game thread only.
		void ResetProgress(Leg& a_leg, RE::TESObjectREFR* a_mover)
		{
			a_leg.lastProgressReal = RealSeconds();
			if (a_mover) {
				const auto p = a_mover->GetPosition();
				a_leg.progressX = p.x;
				a_leg.progressY = p.y;
				a_leg.progressZ = p.z;
			}
		}

		// Start one leg from `mover` toward `dest`. Both mover and destination
		// anchors are resolved (an interior reference resolves through its
		// exterior door), and a live package walk is attempted whenever the
		// movement backend is available, the mover is a non-combat Actor distinct
		// from the destination, and the leg is anchored - the anchored-distance
		// gate is deliberately gone, so a far destination walks off-screen and
		// completes on the game-time deadline instead of the executor standing in
		// place. The mover does NOT have to be 3D-loaded: the package is applied
		// anyway and the engine moves the actor when it can. Otherwise - or when a
		// walk cannot start - the leg runs on the game-time simulation. Either way
		// the anchored distance, both FormIDs and the deadline are stamped.
		//
		// `a_walkRef`, when given and distinct from `dest`, is a named place's
		// EXTERIOR door: the package walks to it (the anchor/distance estimate is
		// its), while `dest` stays the FINAL interior door the mover is stepped
		// through on arrival. `a_sneak` selects the sneak approach package for
		// the walk (the assassinate approach). Game thread only.
		void StartLeg(Leg& a_leg, RE::TESObjectREFR* a_mover, RE::TESObjectREFR* a_dest,
			RE::TESObjectREFR* a_walkRef = nullptr, bool a_sneak = false)
		{
			a_leg = Leg{};
			a_leg.doorStaged = false;  // a fresh leg never inherits a prior door stage (as `approached` resets)
			a_leg.sneak = a_sneak;
			a_leg.moverFormID = a_mover ? a_mover->GetFormID() : 0;
			a_leg.destFormID  = a_dest ? a_dest->GetFormID() : 0;
			ResetProgress(a_leg, a_mover);

			// A door-entry leg walks to the exterior door but finishes at the
			// interior one. If the mover is already inside the destination's space
			// (e.g. staged inside the building), walk straight to `a_dest`.
			const bool useWalkRef = a_walkRef && a_walkRef != a_dest &&
				(!a_mover || !SameSpace(a_mover, a_dest));
			RE::TESObjectREFR* walkRef = useWalkRef ? a_walkRef : a_dest;
			if (useWalkRef) {
				a_leg.walkRefFormID = a_walkRef->GetFormID();
				a_leg.entryFormID = a_dest ? a_dest->GetFormID() : 0;
			}

			// Anchor the leg: the destination's anchor is the position the walk
			// measures to (an interior destination through its exterior door,
			// unless both refs already share a space).
			RE::NiPoint3 moverAnchor{};
			RE::NiPoint3 destAnchor{};
			const bool sameWalkSpace = a_mover && walkRef && SameSpace(a_mover, walkRef);
			const bool moverHasAnchor = sameWalkSpace || ResolveAnchor(a_mover, moverAnchor);
			const bool destHasAnchor  = sameWalkSpace || ResolveAnchor(walkRef, destAnchor);
			if (sameWalkSpace) {
				moverAnchor = a_mover->GetPosition();
				destAnchor  = walkRef->GetPosition();
			}

			a_leg.hasAnchor = moverHasAnchor && destHasAnchor;
			if (a_leg.hasAnchor) {
				a_leg.anchorX = destAnchor.x;
				a_leg.anchorY = destAnchor.y;
				a_leg.anchorZ = destAnchor.z;
				a_leg.anchorDistance = moverAnchor.GetDistance(destAnchor);
			}
			a_leg.destInterior = a_dest && a_dest->GetWorldspace() == nullptr;
			a_leg.destCellFormID = (a_dest && a_dest->GetParentCell()) ? a_dest->GetParentCell()->GetFormID() : 0;
			a_leg.destWorldspaceFormID = (a_dest && a_dest->GetWorldspace()) ? a_dest->GetWorldspace()->GetFormID() : 0;

			const float hours = EstimateHours(a_mover, walkRef);
			const float now = Now();
			a_leg.startGameTime = now;
			a_leg.dueGameTime = now + HoursToDays(hours);

			auto* actor = a_mover ? a_mover->As<RE::Actor>() : nullptr;
			if (movement::IsAvailable() && actor && a_dest && a_mover != walkRef &&
				!actor->IsInCombat() && a_leg.hasAnchor) {
				if (!actor->Is3DLoaded()) {
					// The mover is not loaded yet. Applying the travel package
					// now is a no-op: an add while unloaded never ends the
					// previous travel procedure, whose stale goal resumes when
					// the mover loads. Defer instead - the travel package
					// applies on the first loaded tick (TickLeg) so the
					// procedure starts FRESH and resolves the CURRENT keyword
					// link.
					a_leg.active = true;
					a_leg.pendingStart = true;
				} else if (movement::StartTravel(actor, walkRef, a_sneak)) {
					a_leg.active = true;
					a_leg.evalAtReal = RealSeconds() + 0.3f;
					a_leg.evalLeft = 2;
					a_leg.pkgWatchdogReal = RealSeconds() + 1.5f;
				}
			}
			if (!a_leg.active) {
				a_leg.simulated = true;
			}
			logs::info("leg {:08X}->{:08X}: {} (due in {:.2f}h, anchor={}, dist={:.0f}u, combat={})",
				a_leg.moverFormID, a_leg.destFormID,
				a_leg.active ? (a_leg.pendingStart ? "walk (deferred)" : "walk") : "simulated",
				hours, a_leg.hasAnchor, a_leg.hasAnchor ? a_leg.anchorDistance : -1.0f,
				actor && actor->IsInCombat());
		}

		// The linked side of a load door within kDoorReachDistance of the mover, when
		// that side leads toward the destination: a stalled mover standing at a city
		// gate is usually blocked by the door. "Toward" is space-aware - a mover and
		// destination in different worldspaces (a city gate) can never be compared by
		// raw position, so the linked side must be in the destination's space.
		// Game thread only.
		RE::TESObjectREFR* FindDoorToward(RE::TESObjectREFR* a_mover, RE::TESObjectREFR* a_dest)
		{
			if (!a_mover || !a_dest) return nullptr;
			auto* cell = a_mover->GetParentCell();
			if (!cell) return nullptr;

			const bool sameSpace = SameSpace(a_mover, a_dest);
			auto* destWs = a_dest->GetWorldspace();
			auto* destCell = a_dest->GetParentCell();

			RE::NiPoint3 destPos{};
			// Raw positions are only comparable within one space; the anchor is
			// needed only for the same-space test.
			const bool haveAnchor = sameSpace && ResolveAnchor(a_dest, destPos);
			const auto mp = a_mover->GetPosition();
			const float moverDist = haveAnchor ? mp.GetDistance(destPos) : 0.0f;

			RE::TESObjectREFR* best = nullptr;
			float bestDoorDist = kDoorReachDistance;
			cell->ForEachReference([&](RE::TESObjectREFR* r) {
				if (!r) return RE::BSContainer::ForEachResult::kContinue;
				auto* tp = r->extraList.GetByType<RE::ExtraTeleport>();
				if (!tp || !tp->teleportData) return RE::BSContainer::ForEachResult::kContinue;
				const RE::ObjectRefHandle linked = tp->teleportData->linkedDoor;
				auto linkedPtr = linked.get();
				auto* linkedSide = linkedPtr.get();
				if (!linkedSide) return RE::BSContainer::ForEachResult::kContinue;
				// Never step through a LOCKED door: the travel package cannot
				// open one (its unlock flag is removed) and a MoveTo through it
				// would silently bypass the lock. A locked door falls through
				// to the normal stall/recover machinery instead.
				if (r->IsLocked() || linkedSide->IsLocked()) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				const float doorDist = mp.GetDistance(r->GetPosition());
				if (doorDist > bestDoorDist) return RE::BSContainer::ForEachResult::kContinue;

				bool toward = false;
				if (sameSpace) {
					// Same space: the linked side must also be in that space and
					// closer to the destination than the mover (stepping through
					// must advance the walk).
					toward = SameSpace(linkedSide, a_dest) && haveAnchor &&
						linkedSide->GetPosition().GetDistance(destPos) < moverDist;
				} else if (destWs) {
					// Different worldspaces: the linked side must be in the
					// destination's worldspace.
					toward = linkedSide->GetWorldspace() == destWs;
				} else {
					// Interior destination: the linked side must be interior and,
					// when the destination cell is known, the same cell.
					toward = linkedSide->GetWorldspace() == nullptr &&
						(!destCell || linkedSide->GetParentCell() == destCell);
				}
				if (!toward) return RE::BSContainer::ForEachResult::kContinue;
				best = linkedSide;
				bestDoorDist = doorDist;
				return RE::BSContainer::ForEachResult::kContinue;
			});
			return best;
		}

		// Re-issue a stalled leg's travel package toward its walk ref. A re-evaluation
		// often wakes a mover whose package was silently dropped; tried before any
		// staging/teleport, up to three times per leg. Game thread only.
		bool TryRetarget(Leg& a_leg, RE::Actor* a_mover, RE::TESObjectREFR* a_walkRef,
			RE::TESObjectREFR* a_door = nullptr)
		{
			if (!a_mover || !a_walkRef) return false;
			if (a_leg.retargets >= 3) return false;
			++a_leg.retargets;
			// A stalled mover standing at a load door (a city gate) is usually blocked
			// by the door: step it through to the linked side first.
			if (auto* door = a_door ? a_door : FindDoorToward(a_mover, a_walkRef)) {
				a_mover->MoveTo(door);
				(void)a_mover->MoveToNearestNavmesh(kNavmeshSnap);
				logs::info("IntelLite tasks: leg stepped through a door (attempt {})", a_leg.retargets);
			}
			if (!movement::StartTravel(a_mover, a_walkRef, a_leg.sneak)) return false;
			ResetProgress(a_leg, a_mover);
			a_leg.evalAtReal = RealSeconds() + 0.3f;
			a_leg.evalLeft = 2;
			a_leg.pkgWatchdogReal = RealSeconds() + 1.5f;
			logs::info("IntelLite tasks: leg retargeted (attempt {})", a_leg.retargets);
			return true;
		}

		// Advance one leg one step. A missing mover/destination, or a dead Actor
		// mover, is Invalid. Arrival (distance while both loaded and in the same
		// space, or sharing an interior parent cell) stops the leg and reports
		// Arrived. An on-screen walk steps through a load door toward the
		// destination after kDoorWaitSeconds and recovers from a stall after
		// kWalkTimeoutSeconds. An off-screen leg stages a flat
		// kStationaryConfirmSeconds after its distance-estimated game-time
		// deadline; a simulated leg recovers at the deadline; an on-screen walk
		// that outlives its estimate plus kDeadlineGraceHours is staged. All
		// report Recover. On Recover/Invalid the CALLER owns StopLeg. Game
		// thread only.
		LegStatus TickLeg(Leg& a_leg, float a_arriveDistance)
		{
			auto* mover = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_leg.moverFormID);
			auto* dest  = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_leg.destFormID);
			if (!mover || !dest) return LegStatus::Invalid;

			// The ref the package walks to: for a door-entry leg that is the
			// exterior door, otherwise the destination itself.
			auto* walkRef = a_leg.walkRefFormID
				? RE::TESForm::LookupByID<RE::TESObjectREFR>(a_leg.walkRefFormID)
				: dest;
			if (!walkRef) walkRef = dest;

			// A destination that moved to a different space mid-leg (walked into or
			// out of a building or city) invalidates the leg's anchor: re-anchor
			// against where it is now. Exterior grid-cell seam crossings inside the
			// same worldspace are ignored, elapsed travel time is preserved, the
			// deadline is clamped monotonically so a re-plan never pushes arrival
			// later, and deadlineSeenReal is preserved.
			{
				auto* destWs   = dest->GetWorldspace();
				auto* destCell = dest->GetParentCell();
				const std::uint32_t destWsID   = destWs ? destWs->GetFormID() : 0;
				const std::uint32_t destCellID = destCell ? destCell->GetFormID() : 0;
				const std::uint32_t oldWsID    = a_leg.destWorldspaceFormID;
				const std::uint32_t oldCellID  = a_leg.destCellFormID;
				const bool spaceChanged = (destWsID != oldWsID) ||
					(destWsID == 0 && destCellID != oldCellID);
				a_leg.destCellFormID = destCellID;
				if (spaceChanged) {
					a_leg.destWorldspaceFormID = destWsID;
					RE::NiPoint3 moverAnchor{};
					RE::NiPoint3 destAnchor{};
					const bool sameWalkSpace = SameSpace(mover, walkRef);
					const bool moverHasAnchor = sameWalkSpace || ResolveAnchor(mover, moverAnchor);
					const bool destHasAnchor  = sameWalkSpace || ResolveAnchor(walkRef, destAnchor);
					if (sameWalkSpace) {
						moverAnchor = mover->GetPosition();
						destAnchor  = walkRef->GetPosition();
					}
					a_leg.hasAnchor = moverHasAnchor && destHasAnchor;
					if (a_leg.hasAnchor) {
						a_leg.anchorX = destAnchor.x;
						a_leg.anchorY = destAnchor.y;
						a_leg.anchorZ = destAnchor.z;
						a_leg.anchorDistance = moverAnchor.GetDistance(destAnchor);
					}
					a_leg.destInterior = (destWs == nullptr);
					const float hours = EstimateHours(mover, walkRef);
					const float now = Now();
					const float baseTime = (a_leg.startGameTime > 0.0f) ? a_leg.startGameTime : now;
					const float candidateDue = baseTime + HoursToDays(hours);
					if (candidateDue < a_leg.dueGameTime) {
						a_leg.dueGameTime = candidateDue;
					}
					const float remHours = (std::max)(0.0f, (a_leg.dueGameTime - now) * 24.0f);
					logs::info("leg {:08X}: destination changed space (ws {:08X}->{:08X}, cell {:08X}->{:08X}) - re-anchored (eta {:.2f}h)",
						a_leg.moverFormID, oldWsID, destWsID, oldCellID, destCellID, remHours);
				}
			}

			auto* moverActor = mover->As<RE::Actor>();
			if (moverActor && moverActor->IsDead()) return LegStatus::Invalid;

			if (moverActor && a_leg.evalLeft > 0 && RealSeconds() >= a_leg.evalAtReal) {
				moverActor->EvaluatePackage(true, false);
				--a_leg.evalLeft;
				a_leg.evalAtReal = (a_leg.evalLeft > 0) ? RealSeconds() + 1.2f : 0.0f;
			}

			auto* moverCell = mover->GetParentCell();
			auto* destCell  = dest->GetParentCell();
			const bool sameInterior =
				moverCell && moverCell == destCell && moverCell->IsInteriorCell();
			const float directDist = mover->GetPosition().GetDistance(dest->GetPosition());
			const bool proxArrived =
				mover->Is3DLoaded() && dest->Is3DLoaded() && SameSpace(mover, dest) &&
				directDist < a_arriveDistance;
			const bool arrived = proxArrived || (!a_leg.strictArrival && sameInterior);
			if (arrived) {
				logs::info("leg {:08X}->{:08X}: arrived ({}, dist={:.0f}u, sameSpace={}, m3d={}, d3d={}, mCell={:08X}, dCell={:08X}, strict={})",
					a_leg.moverFormID, a_leg.destFormID,
					proxArrived ? "proximity" : "sameInterior",
					directDist, SameSpace(mover, dest),
					mover->Is3DLoaded(), dest->Is3DLoaded(),
					moverCell ? moverCell->GetFormID() : 0u,
					destCell ? destCell->GetFormID() : 0u,
					a_leg.strictArrival);
				StopLeg(a_leg);
				return LegStatus::Arrived;
			}

			// Door entry: the package walked the mover to the exterior door.
			// Once the mover is loaded and near it, step inside to the final
			// interior destination and complete the leg.
			if (a_leg.walkRefFormID && a_leg.walkRefFormID != a_leg.destFormID && a_leg.entryFormID) {
				const float walkDist = mover->GetPosition().GetDistance(walkRef->GetPosition());
				if (mover->Is3DLoaded() && walkDist < a_arriveDistance) {
					// Only an UNLOCKED door is stepped through; a locked one falls
					// through to the blocked-door watch. Stop the leg's package BEFORE
					// the move so the travel package cannot walk the mover back out to
					// the (exterior) linked ref.
					const bool locked = walkRef->IsLocked();
					if (!locked) {
						logs::info("leg {:08X}->{:08X}: arrived (doorEntry via {:08X}, dist={:.0f}u)",
							a_leg.moverFormID, a_leg.destFormID, a_leg.walkRefFormID, walkDist);
						StopLeg(a_leg);
						mover->MoveTo(dest);
						if (dest->extraList.GetByType<RE::ExtraTeleport>()) {
							if (auto* extTp = walkRef->extraList.GetByType<RE::ExtraTeleport>()) {
								if (extTp->teleportData) {
									if (moverActor) {
										moverActor->SetPosition(extTp->teleportData->position, true);
									} else {
										mover->SetPosition(extTp->teleportData->position);
									}
								}
							}
						}
						if (moverActor) {
							(void)moverActor->MoveToNearestNavmesh(kNavmeshSnap);
						}
						return LegStatus::Arrived;
					}
				}
			}

			a_leg.offscreen = moverActor && !moverActor->Is3DLoaded();

			// Progress bookkeeping: refresh the no-progress clock whenever the
			// mover advances more than kProgressDistance while actually running
			// our travel package (or while off-screen/simulated). Movement on a
			// vanilla routine package must not mask a dropped override.
			const RE::FormID expectedPkg = movement::TravelPackageFormID(a_leg.sneak, false);
			const bool onExpectedPkg = !moverActor || !moverActor->Is3DLoaded() ||
				!a_leg.active || a_leg.pendingStart || expectedPkg == 0 ||
				RunningPackageOf(moverActor).form == expectedPkg;
			if (onExpectedPkg) {
				const auto p = mover->GetPosition();
				const float dx = p.x - a_leg.progressX;
				const float dy = p.y - a_leg.progressY;
				const float dz = p.z - a_leg.progressZ;
				if (std::sqrt(dx * dx + dy * dy + dz * dz) > kProgressDistance) {
					a_leg.progressX = p.x;
					a_leg.progressY = p.y;
					a_leg.progressZ = p.z;
					a_leg.lastProgressReal = RealSeconds();
				}
			}

			// Deferred live start: apply the travel package only once the mover is
			// loaded, so the travel procedure starts FRESH and resolves the current
			// keyword link (a re-add while unloaded never ends the previous
			// procedure, whose stale goal would resume when the mover loads). The
			// off-screen deadline / recover logic and the arrival test above are
			// untouched for a pending leg - only the package application is
			// deferred here.
			if (a_leg.active && a_leg.pendingStart && moverActor && moverActor->Is3DLoaded() &&
				!moverActor->IsInCombat()) {
				if (movement::StartTravel(moverActor, walkRef, a_leg.sneak)) {
					a_leg.pendingStart = false;
					ResetProgress(a_leg, moverActor);
					a_leg.evalAtReal = RealSeconds() + 0.3f;
					a_leg.evalLeft = 2;
					a_leg.pkgWatchdogReal = RealSeconds() + 1.5f;
					logs::info("leg {:08X}: deferred live walk started",
						a_leg.moverFormID);
				}
				// On failure leave pendingStart set (it retries next tick;
				// LogStartFailureOnce already dedupes). A deferred leg is never
				// marked simulated.
			}

			// Running-package watchdog: if a 3D-loaded mover on an active walk is
			// not running our travel/approach package (e.g. its vanilla schedule
			// or PapyrusUtil's package-end hook displaced the override), re-assert
			// StartTravel promptly instead of letting the actor walk away on its
			// vanilla package.
			if (a_leg.active && !a_leg.pendingStart && moverActor && moverActor->Is3DLoaded() &&
				!moverActor->IsInCombat() && expectedPkg != 0 &&
				RunningPackageOf(moverActor).form != expectedPkg &&
				RealSeconds() >= a_leg.pkgWatchdogReal) {
				const RE::FormID curPkg = RunningPackageOf(moverActor).form;
				if (movement::StartTravel(moverActor, walkRef, a_leg.sneak)) {
					a_leg.pkgWatchdogReal = RealSeconds() + 2.0f;
					a_leg.evalAtReal = RealSeconds() + 0.3f;
					a_leg.evalLeft = 2;
					logs::info("leg {:08X}: travel package {:08X} not running (current {:08X}) - re-issued",
						a_leg.moverFormID, expectedPkg, curPkg);
				}
			}

			if (a_leg.simulated) {
				// A simulated leg whose mover has become loaded can upgrade to a
				// live walk (unchanged); otherwise the game-time estimate is the
				// whole simulation and it recovers at its deadline.
				if (movement::IsAvailable() && moverActor && mover != walkRef &&
					moverActor->Is3DLoaded() && !moverActor->IsInCombat() &&
					a_leg.hasAnchor) {
					if (movement::StartTravel(moverActor, walkRef, a_leg.sneak)) {
						a_leg.simulated = false;
						a_leg.active = true;
						ResetProgress(a_leg, mover);
						a_leg.evalAtReal = RealSeconds() + 0.3f;
						a_leg.evalLeft = 2;
						a_leg.pkgWatchdogReal = RealSeconds() + 1.5f;
					}
				}
				if (a_leg.simulated) {
					if (Now() >= a_leg.dueGameTime) {
						logs::info("leg {:08X}: recover - simulated deadline", a_leg.moverFormID);
						return LegStatus::Recover;
					}
					return LegStatus::Travelling;
				}
			}

			// Off-screen: the estimate IS the travel time. Past the game-time
			// deadline the leg stages after a flat kStationaryConfirmSeconds
			// real-time beat - no movement-based reset (an unloaded mover only
			// jumps via schedule hops, which would keep resetting a stillness
			// clock) and no 1-hour grace wait in this branch.
			if (a_leg.offscreen) {
				if (Now() >= a_leg.dueGameTime) {
					if (a_leg.deadlineSeenReal == 0.0f) {
						a_leg.deadlineSeenReal = RealSeconds();
					}
					if (RealSeconds() - a_leg.deadlineSeenReal >= kStationaryConfirmSeconds) {
						a_leg.deadlineFired = true;
						logs::info("leg {:08X}: recover - off-screen deadline ({:.0f}s past due)",
							a_leg.moverFormID, RealSeconds() - a_leg.deadlineSeenReal);
						return LegStatus::Recover;
					}
				}
				return LegStatus::Travelling;
			}

			// On-screen. A mover parked at a load door that leads toward the
			// destination steps through early (the city-gate case).
			if (a_leg.active && a_leg.retargets < 3 &&
				RealSeconds() - a_leg.lastProgressReal > kDoorWaitSeconds) {
				if (auto* earlyDoor = FindDoorToward(mover, walkRef)) {
					logs::info("leg {:08X}: stalled at a door toward the destination - stepping through early",
						a_leg.moverFormID);
					if (TryRetarget(a_leg, moverActor, walkRef, earlyDoor)) return LegStatus::Travelling;
				}
			}

			// An on-screen walk that outlives its estimate plus the grace is
			// staged rather than reported: the deadline is a simulation bound,
			// not evidence of a blocked route.
			if (Now() >= a_leg.dueGameTime + HoursToDays(kDeadlineGraceHours)) {
				a_leg.deadlineFired = true;
				logs::info("leg {:08X}: recover - deadline grace", a_leg.moverFormID);
				return LegStatus::Recover;
			}

			// A stalled on-screen walk recovers after kWalkTimeoutSeconds,
			// retrying the package (and a door step-through) up to three times.
			if (RealSeconds() - a_leg.lastProgressReal > kWalkTimeoutSeconds) {
				if (TryRetarget(a_leg, moverActor, walkRef)) return LegStatus::Travelling;
				logs::info("leg {:08X}: recover - no progress for {:.0f}s",
					a_leg.moverFormID, RealSeconds() - a_leg.lastProgressReal);
				return LegStatus::Recover;
			}
			return LegStatus::Travelling;
		}

		void ClearTargetHold(RE::Actor* a_target);
		bool DispatchInstance(RE::Actor* a_actor, const char* a_script,
			const char* a_fn, RE::BSScript::IFunctionArguments* a_args);

		void HoldAgentOffscreen(Task& a_task, RE::Actor* a_agent)
		{
			if (!a_agent || a_task.agentHeldOffscreen) return;
			a_task.agentHeldOffscreen = true;
			auto* dontMove = RE::MakeFunctionArguments(true);
			DispatchInstance(a_agent, "Actor", "SetDontMove", dontMove);
		}

		void ReleaseAgentOffscreenHold(Task& a_task, RE::Actor* a_agent)
		{
			if (!a_task.agentHeldOffscreen) return;
			a_task.agentHeldOffscreen = false;
			if (a_agent) {
				auto* dontMove = RE::MakeFunctionArguments(false);
				DispatchInstance(a_agent, "Actor", "SetDontMove", dontMove);
			}
		}

		// Release every leg a task owns (used on cancellation and failure). When
		// `a_stopAgentTravel` is false (the agent is immediately starting a new leg
		// in the same tick), reset the agent legs without dispatching
		// RemovePackageOverride on the agent so the removal cannot race with the
		// new AddPackageOverride. Game thread only.
		void StopAllLegs(Task& a_task, bool a_stopAgentTravel = true)
		{
			const bool agentStoppedByLeg = a_stopAgentTravel &&
				((a_task.agentLeg.active && a_task.agentLeg.moverFormID == a_task.agentFormID) ||
				 (a_task.agentReturnLeg.active && a_task.agentReturnLeg.moverFormID == a_task.agentFormID));
			if (a_stopAgentTravel) {
				StopLeg(a_task.agentLeg);
				StopLeg(a_task.agentReturnLeg);
			} else {
				a_task.agentLeg = Leg{};
				a_task.agentReturnLeg = Leg{};
			}
			StopLeg(a_task.targetLeg);
			if (auto* agent = RE::TESForm::LookupByID<RE::Actor>(a_task.agentFormID)) {
				ReleaseAgentOffscreenHold(a_task, agent);
				if (a_stopAgentTravel && !agentStoppedByLeg) {
					movement::StopTravel(agent);
				}
			}
		}

		// Release everything this mod can have placed on an actor: the travel
		// override (run/approach/walk) and the linger sandbox, then re-evaluate so
		// the actor's own AI resumes at once. Safe on an actor that never had one.
		// Game thread only.
		void ReleaseActor(RE::Actor* a_actor)
		{
			if (!a_actor) return;
			movement::StopTravel(a_actor);
			movement::StopSandbox(a_actor);
		}

		bool IsRefInLoadedArea(RE::TESObjectREFR* a_ref);

		// Teleport a mover to a point `offset` units BEHIND an anchor, using the
		// anchor's Z angle; snap to navmesh so the arrival lands on walkable
		// ground. Game thread only.
		void TeleportBehind(RE::Actor* a_mover, RE::TESObjectREFR* a_anchor, float a_offset)
		{
			if (!a_mover || !a_anchor) return;

			a_mover->MoveTo(a_anchor);
			// Cross-cell / cross-worldspace MoveTo onto an unloaded reference is
			// deferred by the engine; calling SetPosition in the same frame would
			// apply `a_anchor`'s coordinates in `a_mover`'s old space (e.g. near
			// (0,0,0) in Tamriel). Only offset and navmesh-snap when both refs
			// share the loaded active space.
			if (SameSpace(a_mover, a_anchor) && IsRefInLoadedArea(a_anchor)) {
				const auto  anchorPos = a_anchor->GetPosition();
				const float z = a_anchor->GetAngleZ();
				// Forward is (sin z, cos z); behind subtracts it.
				const RE::NiPoint3 pos{
					anchorPos.x - std::sin(z) * a_offset,
					anchorPos.y - std::cos(z) * a_offset,
					anchorPos.z
				};
				a_mover->SetPosition(pos, true);
				(void)a_mover->MoveToNearestNavmesh(kNavmeshSnap);
			}
			a_mover->EvaluatePackage(true, false);
		}

		// A worldspace belongs to the frame when it is the frame or a descendant
		// through the parent chain (city worldspaces are children of Tamriel with
		// shared coordinates). Game thread only.
		bool WorldspaceInFrame(RE::TESWorldSpace* a_ws, RE::TESWorldSpace* a_frame)
		{
			if (!a_ws || !a_frame) return false;
			for (auto* ws = a_ws; ws; ws = ws->parentWorld) {
				if (ws == a_frame) return true;
			}
			return false;
		}

		// The exterior-side linked door of an interior ref's cell - the way out of
		// the building. When a_requiredWs is given, only a door whose linked
		// (exterior) door is in that worldspace counts; nullptr accepts any
		// exterior door. When a_frameTolerant is true, a linked side in the frame
		// or any child worldspace of it counts (city cells exit into a child
		// world); existing callers pass no flag and keep exact matching. Returns
		// nullptr when the ref is not in an interior or the cell has no exterior
		// door. Game thread only.
		RE::TESObjectREFR* FindExitDoor(RE::TESObjectREFR* a_ref, RE::TESWorldSpace* a_requiredWs, bool a_frameTolerant = false)
		{
			if (!a_ref) return nullptr;
			auto* cell = a_ref->GetParentCell();
			if (!cell || !cell->IsInteriorCell()) return nullptr;

			auto findInCell = [a_requiredWs, a_frameTolerant](RE::TESObjectCELL* a_cell, RE::TESObjectREFR* a_targetRef) -> RE::TESObjectREFR* {
				RE::NiPoint3 refPos{};
				const bool   haveRefPos = CellMainRefPos(a_cell, a_targetRef, refPos);

				RE::TESObjectREFR* bestInt = nullptr;
				RE::TESObjectREFR* bestExt = nullptr;
				a_cell->ForEachReference([&](RE::TESObjectREFR* a_refr) {
					if (!a_refr || a_refr->IsDisabled() || a_refr->IsDeleted()) {
						return RE::BSContainer::ForEachResult::kContinue;
					}
					auto* teleport = a_refr->extraList.GetByType<RE::ExtraTeleport>();
					if (!teleport || !teleport->teleportData) {
						return RE::BSContainer::ForEachResult::kContinue;
					}
					// Copy the handle before resolving: the non-const get() clears
					// the stored handle, and this door's teleport data belongs to the
					// game (mirrors ResolveAnchor).
					const RE::ObjectRefHandle linked = teleport->teleportData->linkedDoor;
					auto linkedRef = linked.get();
					auto* dest = linkedRef.get();
					if (!dest || dest->IsDisabled() || dest->IsDeleted() || !dest->GetWorldspace()) {
						return RE::BSContainer::ForEachResult::kContinue;
					}
					if (a_requiredWs) {
						const bool matches = a_frameTolerant
							? WorldspaceInFrame(dest->GetWorldspace(), a_requiredWs)
							: dest->GetWorldspace() == a_requiredWs;
						if (!matches) return RE::BSContainer::ForEachResult::kContinue;
					}
					if (IsBetterCellDoor(haveRefPos, refPos, a_refr, dest, bestInt, bestExt)) {
						bestInt = a_refr;
						bestExt = dest;
					}
					return RE::BSContainer::ForEachResult::kContinue;
				});
				return bestExt;
			};

			if (auto* direct = findInCell(cell, a_ref)) {
				return direct;
			}

			// Two-hop interior fallback (a sub-cell exiting through a main interior cell).
			RE::TESObjectREFR* found = nullptr;
			cell->ForEachReference([&](RE::TESObjectREFR* a_refr) {
				if (found) return RE::BSContainer::ForEachResult::kStop;
				if (!a_refr || a_refr->IsDisabled() || a_refr->IsDeleted()) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				auto* teleport = a_refr->extraList.GetByType<RE::ExtraTeleport>();
				if (!teleport || !teleport->teleportData) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				const RE::ObjectRefHandle linked = teleport->teleportData->linkedDoor;
				auto linkedRef = linked.get();
				auto* dest = linkedRef.get();
				if (!dest || dest->IsDisabled() || dest->IsDeleted() || dest->GetWorldspace()) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				auto* midCell = dest->GetParentCell();
				if (!midCell || midCell == cell || !midCell->IsInteriorCell()) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				if (auto* twoHop = findInCell(midCell, dest)) {
					found = twoHop;
					return RE::BSContainer::ForEachResult::kStop;
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			return found;
		}

		// True when a reference sits in the player's currently active space and
		// is 3D-loaded or in an attached cell (covers invisible XMarkers/MapMarkers
		// whose Get3D() is null even when their cell is loaded around the player,
		// while excluding cached cells/actors in a space the player has left).
		// Game thread only.
		bool IsRefInLoadedArea(RE::TESObjectREFR* a_ref)
		{
			if (!a_ref) return false;
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player || !SameSpace(a_ref, player)) return false;
			if (a_ref->Is3DLoaded()) return true;
			auto* cell = a_ref->GetParentCell();
			return cell && cell->IsAttached();
		}

		// The interior-side entrance door and landing of an interior ref's cell -
		// the way into the building when a destination inside it is loaded.
		// Collects all unlocked exterior-linked doors in the cell and picks the
		// deterministic main door (closest to the cell's worldLocMarker / target
		// ref, tie-broken by lowest exterior Z so an elevated porch/balcony
		// never beats the street entrance), falling back to an unlocked
		// interior-to-interior door (sub-cells). Game thread only.
		bool FindInteriorEntryDoor(RE::TESObjectREFR* a_ref, RE::TESObjectREFR*& a_outDoor,
			RE::NiPoint3& a_outLanding)
		{
			a_outDoor = nullptr;
			if (!a_ref) return false;
			auto* cell = a_ref->GetParentCell();
			if (!cell || !cell->IsInteriorCell()) return false;

			RE::NiPoint3 refPos{};
			const bool   haveRefPos = CellMainRefPos(cell, a_ref, refPos);

			RE::TESObjectREFR* extLinkedDoor = nullptr;
			RE::TESObjectREFR* extOtherDoor  = nullptr;
			RE::NiPoint3       extLanding{};
			std::uint32_t      extCandidates = 0;
			RE::TESObjectREFR* intLinkedDoor = nullptr;
			RE::NiPoint3       intLanding{};

			cell->ForEachReference([&](RE::TESObjectREFR* a_refr) {
				if (!a_refr || a_refr->IsDisabled() || a_refr->IsDeleted()) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				auto* teleport = a_refr->extraList.GetByType<RE::ExtraTeleport>();
				if (!teleport || !teleport->teleportData) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				const RE::ObjectRefHandle linked = teleport->teleportData->linkedDoor;
				auto linkedRef = linked.get();
				auto* other = linkedRef.get();
				if (!other || other->IsDisabled() || other->IsDeleted()) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				if (a_refr->IsLocked() || other->IsLocked()) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				RE::NiPoint3 landing = a_refr->GetPosition();
				if (auto* otherTp = other->extraList.GetByType<RE::ExtraTeleport>()) {
					if (otherTp->teleportData) {
						landing = otherTp->teleportData->position;
					}
				}
				if (other->GetWorldspace()) {
					++extCandidates;
					if (IsBetterCellDoor(haveRefPos, refPos, a_refr, other, extLinkedDoor, extOtherDoor)) {
						extLinkedDoor = a_refr;
						extOtherDoor  = other;
						extLanding    = landing;
					}
					return RE::BSContainer::ForEachResult::kContinue;
				}
				if (!intLinkedDoor ||
					(haveRefPos && a_refr->GetPosition().GetDistance(refPos) < intLinkedDoor->GetPosition().GetDistance(refPos))) {
					intLinkedDoor = a_refr;
					intLanding = landing;
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});

			if (extLinkedDoor && extOtherDoor) {
				logs::info("door pick in {:08X}: picked {:08X}<->{:08X} (intDist={:.0f}u, extZ={:.0f}, {} candidate(s))",
					cell->GetFormID(), extLinkedDoor->GetFormID(), extOtherDoor->GetFormID(),
					haveRefPos ? extLinkedDoor->GetPosition().GetDistance(refPos) : -1.0f,
					extOtherDoor->GetPosition().z, extCandidates);
				a_outDoor = extLinkedDoor;
				a_outLanding = extLanding;
				return true;
			}
			if (intLinkedDoor) {
				a_outDoor = intLinkedDoor;
				a_outLanding = intLanding;
				return true;
			}
			return false;
		}

		// Distance -> estimated game hours. Map calibration (walking pace):
		// Whiterun<->Riverwood ~37.2k units = 1 h, Whiterun<->Riften ~177k = 6 h,
		// Riften<->Solitude ~312k = 12 h. The agents travel at
		// kTravelSpeedFactor times that pace (a runner / horse), so the estimates
		// halve: Riverwood ~0.5 h, Whiterun<->Riften ~3 h, Riften<->Solitude
		// ~5.8 h. Long journeys estimate slightly slower per unit. Game time, so
		// the duration is identical at any timescale. Game thread only.
		float HoursForDistance(float a_distance)
		{
			if (a_distance <= 0.0f) return kMinTravelHours;
			const float hours = std::pow(a_distance / kReferenceDistance, kDistanceExponent) / kTravelSpeedFactor;
			return std::clamp(hours, kMinTravelHours, kMaxTravelHours);
		}

		// The Tamriel worldspace, resolved once (Skyrim.esm 0x0000003C).
		RE::TESWorldSpace* TamrielWorldspace()
		{
			static RE::TESWorldSpace* ws = []() -> RE::TESWorldSpace* {
				if (auto* dataHandler = RE::TESDataHandler::GetSingleton()) {
					return dataHandler->LookupForm<RE::TESWorldSpace>(0x0000003C, "Skyrim.esm");
				}
				return nullptr;
			}();
			return ws;
		}

		// Resolve a ref into a worldspace frame: its own position when it is
		// already in that worldspace, else the first worldLocMarker within
		// kMapLocationHops parents of its current location, else (an interior
		// ref) the cell door that leads into the frame. Game thread only.
		bool MapFramePosition(RE::TESObjectREFR* a_ref, RE::TESWorldSpace* a_frame, RE::NiPoint3& a_out)
		{
			if (!a_ref || !a_frame) return false;
			if (WorldspaceInFrame(a_ref->GetWorldspace(), a_frame)) {
				a_out = a_ref->GetPosition();
				return true;
			}

			// An interior ref: the cell door into the frame is the precise way
			// out; the location marker chain is the fallback.
			if (!a_ref->GetWorldspace()) {
				if (auto* door = FindExitDoor(a_ref, a_frame, true)) {
					a_out = door->GetPosition();
					return true;
				}
			}

			auto* loc = a_ref->GetCurrentLocation();
			for (int hops = 0; loc && hops < kMapLocationHops; ++hops) {
				const RE::ObjectRefHandle markerHandle = loc->worldLocMarker;
				auto markerPtr = markerHandle.get();
				if (auto* marker = markerPtr.get()) {
					if (WorldspaceInFrame(marker->GetWorldspace(), a_frame)) {
						a_out = marker->GetPosition();
						return true;
					}
				}
				loc = loc->parentLoc;
			}
			return false;
		}

		// Estimated travel hours between two references, mirroring IntelEngine's
		// distance->deadline model: a same-worldspace pair uses the direct
		// distance; a cross-space pair (a city worldspace like WhiterunWorld, or
		// an interior cell) resolves both refs into Tamriel through their
		// locations' markers or door anchors. Falls back to kCrossWorldHours when
		// no common frame resolves. Game thread only.
		// The estimate plus whether the no-shared-frame fallback was used, so a
		// caller can log the fallback once instead of on every recomputation.
		struct Estimate
		{
			float hours = kCrossWorldHours;
			bool  fellBack = false;
		};

		Estimate EstimateHoursEx(RE::TESObjectREFR* a, RE::TESObjectREFR* b, bool a_logFallback,
			bool a_logCrossSpace = true)
		{
			if (!a || !b) return {};
			if (SameSpace(a, b)) {
				const float hours = HoursForDistance(a->GetPosition().GetDistance(b->GetPosition()));
				return { hours, false };
			}
			if (auto* tamriel = TamrielWorldspace()) {
				RE::NiPoint3 pa{};
				RE::NiPoint3 pb{};
				if (MapFramePosition(a, tamriel, pa) && MapFramePosition(b, tamriel, pb)) {
					const float hours = HoursForDistance(pa.GetDistance(pb));
					if (a_logCrossSpace) {
						logs::info("estimate: cross-space {:08X} -> {:08X} mapped via the Tamriel frame: {:.0f}u ({:.2f}h)",
							a->GetFormID(), b->GetFormID(), pa.GetDistance(pb), hours);
					}
					return { hours, false };
				}
			}
			if (a_logFallback) {
				auto wsName = [](RE::TESWorldSpace* ws) -> const char* {
					if (!ws) return "(interior)";
					const char* e = ws->GetFormEditorID();
					return (e && *e) ? e : "(unnamed)";
				};
				logs::info("estimate: no shared frame for {:08X} ({}) -> {:08X} ({}) - fallback {:.2f}h",
					a->GetFormID(), wsName(a->GetWorldspace()),
					b->GetFormID(), wsName(b->GetWorldspace()), kCrossWorldHours);
			}
			return { kCrossWorldHours, true };
		}

		float EstimateHours(RE::TESObjectREFR* a, RE::TESObjectREFR* b)
		{
			return EstimateHoursEx(a, b, true).hours;
		}

		// The location ref type that maps to a walled city's inside-side main
		// gate. Skyrim.esm 0x08AD62 is CWMainGateInterior; every one of the five
		// walled cities maps it on its location to the gate ref. Resolved once.
		RE::BGSLocationRefType* MainGateRefType()
		{
			static RE::BGSLocationRefType* refType = []() -> RE::BGSLocationRefType* {
				if (auto* dataHandler = RE::TESDataHandler::GetSingleton()) {
					return dataHandler->LookupForm<RE::BGSLocationRefType>(0x08AD62, "Skyrim.esm");
				}
				return nullptr;
			}();
			return refType;
		}

		// The inside-side gate ref of the walled city the player stands in.
		// Primary: the player's location chain (up to four parent-location hops,
		// mirroring the marker stage) maps CWMainGateInterior to the gate ref.
		// Fallback: the loaded-cell exit load door nearest the mover's origin -
		// a load door whose linked side leaves the player's worldspace. Returns
		// nullptr when neither finds one. Game thread only.
		RE::TESObjectREFR* FindCityGate(RE::TESObjectREFR* a_mover, RE::TESObjectREFR* a_player, RE::TESWorldSpace* a_playerWs)
		{
			// Primary: the vanilla CWMainGateInterior mapping on the player's
			// location chain; the inside-side gate lives in the player's
			// worldspace.
			if (auto* refType = MainGateRefType()) {
				auto* loc = a_player ? a_player->GetCurrentLocation() : nullptr;
				for (int hops = 0; loc && hops < 4; ++hops) {
					for (const auto& entry : loc->specialRefs) {
						if (entry.type != refType) continue;
						if (auto* gate = RE::TESForm::LookupByID<RE::TESObjectREFR>(entry.refData.refID)) {
							if (gate->GetWorldspace() == a_playerWs) return gate;
						}
					}
					loc = loc->parentLoc;
				}
			}

			// Fallback: scan the loaded cells for the exit load door nearest the
			// mover's origin. A candidate's linked door is in a different
			// worldspace than the player's, so it leaves the city (locked doors
			// are not skipped - a placement is not a door step-through).
			RE::NiPoint3 origin{};
			if (!MapFramePosition(a_mover, TamrielWorldspace(), origin)) {
				if (!a_player) return nullptr;
				origin = a_player->GetPosition();
			}

			RE::TESObjectREFR* best = nullptr;
			float              bestDist = 0.0f;
			if (auto* tes = RE::TES::GetSingleton()) {
				tes->ForEachCell([&](RE::TESObjectCELL* a_cell) {
					if (!a_cell) return;
					a_cell->ForEachReference([&](RE::TESObjectREFR* a_refr) {
						if (!a_refr) return RE::BSContainer::ForEachResult::kContinue;
						auto* teleport = a_refr->extraList.GetByType<RE::ExtraTeleport>();
						if (!teleport || !teleport->teleportData) {
							return RE::BSContainer::ForEachResult::kContinue;
						}

						// Copy the handle before resolving: the non-const get()
						// clears the stored handle, and this door's teleport data
						// belongs to the game (mirrors ResolveAnchor).
						const RE::ObjectRefHandle linked = teleport->teleportData->linkedDoor;
						auto linkedRef = linked.get();
						auto* dest = linkedRef.get();
						if (!dest || !dest->GetWorldspace() || dest->GetWorldspace() == a_playerWs) {
							return RE::BSContainer::ForEachResult::kContinue;
						}
						const float dist = origin.GetDistance(a_refr->GetPosition());
						if (!best || dist < bestDist) {
							best = a_refr;
							bestDist = dist;
						}
						return RE::BSContainer::ForEachResult::kContinue;
					});
				});
			}
			return best;
		}

		// The inside-side gate ref of the walled city the mover is in, resolved
		// from the mover's own location chain (up to four parent-location hops,
		// CWMainGateInterior via MainGateRefType) - the mirror of FindCityGate's
		// primary path for the "mover inside the city, player outside" return
		// case. Returns nullptr when none is found. Game thread only.
		RE::TESObjectREFR* FindMoverCityGate(RE::TESObjectREFR* a_mover)
		{
			if (auto* refType = MainGateRefType()) {
				auto* loc = a_mover ? a_mover->GetCurrentLocation() : nullptr;
				for (int hops = 0; loc && hops < 4; ++hops) {
					for (const auto& entry : loc->specialRefs) {
						if (entry.type != refType) continue;
						if (auto* gate = RE::TESForm::LookupByID<RE::TESObjectREFR>(entry.refData.refID)) {
							if (gate->GetWorldspace() != nullptr) return gate;
						}
					}
					loc = loc->parentLoc;
				}
			}
			return nullptr;
		}

		// On a return-leg recovery, prefer a visible approach over an instant
		// teleport: when the mover and the player share a worldspace, place the
		// mover kApproachDistance units from the player along the mover->player
		// direction (directly behind the player when that direction is
		// degenerate). When they do NOT share a worldspace - including an
		// interior, where GetWorldspace is null - the mover is placed directly
		// behind the player instead, so a cross-space return still walks the last
		// stretch in rather than teleporting on top of the player. It then snaps
		// to navmesh and restarts the leg. Returns true when the mover was placed
		// and the leg restarted (the caller must NOT teleport); false only for
		// null refs or a leg that was already approached once. Game thread only.
		bool ApproachFromDistance(Leg& a_leg, RE::Actor* a_mover, RE::Actor* a_player)
		{
			if (!a_mover || !a_player) return false;
			if (!movement::IsAvailable()) return false;
			if (a_leg.approached) return false;
			const bool strict = a_leg.strictArrival;

			auto* moverWs  = a_mover->GetWorldspace();
			auto* playerWs = a_player->GetWorldspace();

			auto* playerCell = a_player->GetParentCell();
			const bool playerInterior = playerCell && playerCell->IsInteriorCell();
			const float approach = playerInterior ? kApproachDistanceInterior : kApproachDistance;

			const auto moverPos  = a_mover->GetPosition();
			const auto playerPos = a_player->GetPosition();

			auto* tamriel = TamrielWorldspace();
			auto* moverParentCell = a_mover->GetParentCell();
			const bool moverInterior = moverParentCell && moverParentCell->IsInteriorCell();
			const bool sameSpace = (moverWs && playerWs && moverWs == playerWs) ||
				(moverParentCell && playerCell && moverParentCell == playerCell);

			// 1. Door-aware staging: when the player is inside a building, a
			// cross-space mover enters through that building's main entrance
			// door; when the player is outside and the mover is inside a local
			// building in the player's worldspace, the mover steps out of that
			// building's exterior door.
			if (!sameSpace && !a_leg.doorStaged) {
				if (playerInterior) {
					RE::TESObjectREFR* entryDoor = nullptr;
					RE::NiPoint3       landing{};
					if (FindInteriorEntryDoor(a_player, entryDoor, landing) && entryDoor) {
						logs::info("IntelLite tasks: {} enters through the door ({:08X}) to return",
							DisplayName(a_mover), entryDoor->GetFormID());
						a_mover->MoveTo(entryDoor);
						a_mover->SetPosition(landing, true);
						(void)a_mover->MoveToNearestNavmesh(kNavmeshSnap);
						a_mover->EvaluatePackage(true, false);
						StartLeg(a_leg, a_mover, a_player);
						a_leg.strictArrival = true;
						a_leg.doorStaged = true;
						a_leg.approached = true;
						return true;
					}
				} else if (auto* exitDoor = FindExitDoor(a_mover, playerWs);
					exitDoor && IsRefInLoadedArea(exitDoor) &&
					exitDoor->GetPosition().GetDistance(playerPos) <= kEntranceMaxDistance) {
					logs::info("IntelLite tasks: {} steps out of the door to return", DisplayName(a_mover));
					a_mover->MoveTo(exitDoor);
					(void)a_mover->MoveToNearestNavmesh(kNavmeshSnap);
					a_mover->EvaluatePackage(true, false);
					StartLeg(a_leg, a_mover, a_player);
					a_leg.strictArrival = true;
					a_leg.doorStaged = true;
					a_leg.approached = true;
					return true;
				}
			}

			// 2. City-gate staging: the player is on a walled city's street and
			// the mover is still outside it (not in a building that exits directly
			// into this city), so the plausible arrival is the city gate
			// (resolved via the location's CWMainGateInterior ref type, falling
			// back to the nearest exit load door) and the pair walks in visibly.
			const bool moverLocalBuilding = moverInterior && [a_mover, playerWs, &playerPos]() {
				auto* d = FindExitDoor(a_mover, playerWs);
				return d && IsRefInLoadedArea(d) &&
					d->GetPosition().GetDistance(playerPos) <= kEntranceMaxDistance;
			}();
			if (!playerInterior && playerWs && tamriel && playerWs != tamriel &&
				WorldspaceInFrame(playerWs, tamriel) && moverWs != playerWs && !moverLocalBuilding) {
				if (auto* gate = FindCityGate(a_mover, a_player, playerWs);
					gate && IsRefInLoadedArea(gate) &&
					gate->GetPosition().GetDistance(playerPos) <= kEntranceMaxDistance) {
					// The engine's own inside landing: the outside door's teleport
					// position is ~200u in front of the inside gate ref, on the
					// city street.
					RE::NiPoint3 landing = gate->GetPosition();
					bool         haveLanding = false;
					if (auto* gateTeleport = gate->extraList.GetByType<RE::ExtraTeleport>()) {
						if (gateTeleport->teleportData) {
							const RE::ObjectRefHandle outside = gateTeleport->teleportData->linkedDoor;
							auto outsidePtr = outside.get();
							if (auto* outsideRef = outsidePtr.get()) {
								if (auto* outsideTeleport = outsideRef->extraList.GetByType<RE::ExtraTeleport>()) {
									if (outsideTeleport->teleportData) {
										landing = outsideTeleport->teleportData->position;
										haveLanding = true;
									}
								}
							}
						}
					}
					if (!haveLanding) {
						const float dx = playerPos.x - landing.x;
						const float dy = playerPos.y - landing.y;
						const float len = std::sqrt(dx * dx + dy * dy);
						if (len > 1.0f) {
							landing.x += dx / len * 150.0f;
							landing.y += dy / len * 150.0f;
						}
					}
					a_mover->MoveTo(gate);
					a_mover->SetPosition(landing, true);
					(void)a_mover->MoveToNearestNavmesh(kNavmeshSnap);
					a_mover->EvaluatePackage(true, false);
					StartLeg(a_leg, a_mover, a_player);
					a_leg.strictArrival = strict;
					a_leg.approached = true;
					logs::info("IntelLite tasks: {} stages at the city gate ({:08X})", DisplayName(a_mover), gate->GetFormID());
					return true;
				}
				logs::info("IntelLite tasks: no usable city gate for the player's worldspace - falling through");
			}

			// 3. Mirror city-gate staging: the mover is inside a walled city (a
			// child worldspace of Tamriel, or an interior within it) and the
			// player is outside that city nearby, so she emerges through the
			// gate and walks the last stretch to the player instead of landing
			// behind them.
			if (!playerInterior && tamriel && a_mover) {
				if (auto* gate = FindMoverCityGate(a_mover)) {
					auto* gateWs = gate->GetWorldspace();
					if (gateWs != tamriel && WorldspaceInFrame(gateWs, tamriel) &&
						gateWs != playerWs && (!moverWs || moverWs == gateWs)) {
						// The landing is the engine's own crossing-OUT landing: the
						// gate ref's own teleportData->position. The outside door is
						// derived from the same teleport data, so there is NO second
						// landing source - unreadable teleport data falls through below.
						RE::TESObjectREFR* outsideDoor = nullptr;
						RE::NiPoint3      landing{};
						bool              haveLanding = false;
						if (auto* gateTeleport = gate->extraList.GetByType<RE::ExtraTeleport>()) {
							if (gateTeleport->teleportData) {
								// Copy the handle before resolving: the non-const
								// get() clears the stored handle (mirrors FindCityGate).
								const RE::ObjectRefHandle outside = gateTeleport->teleportData->linkedDoor;
								auto outsideRef = outside.get();
								outsideDoor = outsideRef.get();
								landing = gateTeleport->teleportData->position;
								haveLanding = true;
							}
						}
						if (haveLanding && outsideDoor && outsideDoor->GetWorldspace() == playerWs &&
							IsRefInLoadedArea(outsideDoor) &&
							landing.GetDistance(playerPos) <= kEntranceMaxDistance) {
							a_mover->MoveTo(outsideDoor);
							a_mover->SetPosition(landing, true);
							(void)a_mover->MoveToNearestNavmesh(kNavmeshSnap);
							a_mover->EvaluatePackage(true, false);
							StartLeg(a_leg, a_mover, a_player);
							a_leg.strictArrival = strict;
							a_leg.approached = true;
							logs::info("IntelLite tasks: {} comes out of the city gate ({:08X})", DisplayName(a_mover), gate->GetFormID());
							return true;
						}
					}
				}
				if (moverWs && moverWs != tamriel && WorldspaceInFrame(moverWs, tamriel) && moverWs != playerWs) {
					logs::info("IntelLite tasks: no usable city gate for the mover's city - falling through");
				}
			}

			// 4. Plausible exterior entrance: the player's current location's map
			// marker is the road entrance the player came in by - stage just inside it.
			if (!playerInterior && playerWs) {
				auto* loc = a_player->GetCurrentLocation();
				RE::TESObjectREFR* marker = nullptr;
				for (int hops = 0; loc && hops < 4; ++hops) {
					const RE::ObjectRefHandle markerHandle = loc->worldLocMarker;
					auto markerPtr = markerHandle.get();
					if (auto* m = markerPtr.get()) { marker = m; break; }
					loc = loc->parentLoc;
				}
				if (marker && marker->GetWorldspace() == playerWs && IsRefInLoadedArea(marker)) {
					const auto mp = marker->GetPosition();
					const float pd = mp.GetDistance(playerPos);
					if (pd >= kEntranceMinDistance && pd <= kEntranceMaxDistance) {
						const float dx = playerPos.x - mp.x;
						const float dy = playerPos.y - mp.y;
						const float len = std::sqrt(dx * dx + dy * dy);
						RE::NiPoint3 spot = mp;
						if (len > 1.0f) {
							spot.x = mp.x + dx / len * 150.0f;
							spot.y = mp.y + dy / len * 150.0f;
						}
						a_mover->MoveTo(marker);
						a_mover->SetPosition(spot, true);
						(void)a_mover->MoveToNearestNavmesh(kNavmeshSnap);
						a_mover->EvaluatePackage(true, false);
						StartLeg(a_leg, a_mover, a_player);
						a_leg.strictArrival = strict;
						a_leg.approached = true;
						logs::info("IntelLite tasks: {} stages at the entrance marker", DisplayName(a_mover));
						return true;
					}
					logs::info("IntelLite tasks: entrance marker out of range ({:.0f} u) - using the bearing stage", pd);
				} else {
					logs::info("IntelLite tasks: no usable entrance marker for the player's location - using the bearing stage");
				}
			}

			RE::NiPoint3 dest;
			if (sameSpace) {
				const RE::NiPoint3 toPlayer{
					playerPos.x - moverPos.x,
					playerPos.y - moverPos.y,
					playerPos.z - moverPos.z
				};
				const float len = std::sqrt(toPlayer.x * toPlayer.x +
					toPlayer.y * toPlayer.y + toPlayer.z * toPlayer.z);

				if (len > 1.0f) {
					// Keep the mover's current bearing from the player, pushed out to
					// approach so it runs the visible last stretch in.
					const float scale = approach / len;
					dest = RE::NiPoint3{
						playerPos.x - toPlayer.x * scale,
						playerPos.y - toPlayer.y * scale,
						playerPos.z
					};
				} else {
					// Degenerate direction: fall back to directly behind the player.
					const float z = a_player->GetAngleZ();
					dest = RE::NiPoint3{
						playerPos.x - std::sin(z) * approach,
						playerPos.y - std::cos(z) * approach,
						playerPos.z
					};
				}
			} else {
				// Different worldspace (or an interior with no resolved entry door):
				// prefer the Tamriel-frame bearing from the mover's origin, falling
				// back to directly behind the player.
				RE::NiPoint3 moverFrame{};
				RE::NiPoint3 playerFrame{};
				if (!playerInterior && tamriel &&
					MapFramePosition(a_mover, tamriel, moverFrame) &&
					MapFramePosition(a_player, tamriel, playerFrame)) {
					const float dx = playerFrame.x - moverFrame.x;
					const float dy = playerFrame.y - moverFrame.y;
					const float len = std::sqrt(dx * dx + dy * dy);
					if (len > 1.0f) {
						const float scale = approach / len;
						dest = RE::NiPoint3{
							playerPos.x - dx * scale,
							playerPos.y - dy * scale,
							playerPos.z
						};
					} else {
						const float z = a_player->GetAngleZ();
						dest = RE::NiPoint3{
							playerPos.x - std::sin(z) * approach,
							playerPos.y - std::cos(z) * approach,
							playerPos.z
						};
					}
				} else {
					const float z = a_player->GetAngleZ();
					dest = RE::NiPoint3{
						playerPos.x - std::sin(z) * approach,
						playerPos.y - std::cos(z) * approach,
						playerPos.z
					};
				}
			}

			a_mover->MoveTo(a_player);
			a_mover->SetPosition(dest, true);
			(void)a_mover->MoveToNearestNavmesh(kNavmeshSnap);
			a_mover->EvaluatePackage(true, false);

			// Restart the leg to the player and mark it approached, so a second
			// recovery teleports rather than looping.
			StartLeg(a_leg, a_mover, a_player);
			a_leg.strictArrival = strict;
			a_leg.approached = true;
			logs::info("IntelLite tasks: {} staged {:.0f}u out (bearing) to return",
				DisplayName(a_mover), a_mover->GetPosition().GetDistance(playerPos));
			return true;
		}

		// A mover's distance to the player, or a huge value when the two are not
		// in the same space (a cross-world/interior hop counts as far). Game
		// thread only.
		float DistanceToPlayer(RE::TESObjectREFR* a_mover, RE::Actor* a_player)
		{
			return (a_mover && a_player && SameSpace(a_mover, a_player))
				? a_mover->GetPosition().GetDistance(a_player->GetPosition())
				: 1e9f;
		}

		// Place a_follower right behind a_anchor (away from the player) and restart
		// its leg, so a staged fetch pair walks the final stretch in together from
		// the same point. The joined leg is marked approached so it cannot be
		// re-joined in a loop. Game thread only.
		void JoinAtStagingPoint(RE::Actor* a_follower, RE::Actor* a_anchor,
			RE::Actor* a_player, Leg& a_followerLeg)
		{
			if (!a_follower || !a_anchor || !a_player) return;
			const bool strict = a_followerLeg.strictArrival;
			ClearTargetHold(a_follower);
			if (!a_follower->Is3DLoaded()) {
				movement::StopTravel(a_follower);
			}
			a_follower->MoveTo(a_anchor);
			const auto ap = a_anchor->GetPosition();
			const auto pp = a_player->GetPosition();
			const float dx = ap.x - pp.x;
			const float dy = ap.y - pp.y;
			const float len = std::sqrt(dx * dx + dy * dy);
			RE::NiPoint3 spot = ap;
			if (len > 1.0f) {
				spot.x = ap.x + dx / len * kAgentJoinOffset;
				spot.y = ap.y + dy / len * kAgentJoinOffset;
			}
			a_follower->SetPosition(spot, true);
			(void)a_follower->MoveToNearestNavmesh(kNavmeshSnap);
			StartLeg(a_followerLeg, a_follower, a_player);
			a_followerLeg.strictArrival = strict;
			a_followerLeg.approached = true;
		}

		// "Return check" run when the agent's own travel completes: if it has
		// left the player's cell and is still in the worldspace of its saved
		// home position, put it back there. Returns true when the agent was
		// actually moved home. Game thread only.
		bool ReturnAgentIfNeeded(const Task& a_task)
		{
			auto* agent = RE::TESForm::LookupByID<RE::Actor>(a_task.agentFormID);
			if (!agent) return false;

			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player) return false;

			if (agent->GetParentCell() == player->GetParentCell()) return false;

			auto* agentCell = agent->GetParentCell();
			if (a_task.returnCellFormID != 0 && agentCell &&
				agentCell->GetFormID() == a_task.returnCellFormID) {
				agent->SetPosition(RE::NiPoint3{ a_task.returnX, a_task.returnY, a_task.returnZ }, true);
				(void)agent->MoveToNearestNavmesh(kNavmeshSnap);
				agent->EvaluatePackage(true, false);
				return true;
			}

			if (a_task.returnWorldspaceFormID == 0) return false;
			auto* homeWs = RE::TESForm::LookupByID<RE::TESWorldSpace>(a_task.returnWorldspaceFormID);
			if (!homeWs || agent->GetWorldspace() != homeWs) return false;

			const RE::NiPoint3 homePos{ a_task.returnX, a_task.returnY, a_task.returnZ };
			if (agent->GetPosition().GetDistance(homePos) > 8192.0f) return false;

			agent->SetPosition(homePos, true);
			(void)agent->MoveToNearestNavmesh(kNavmeshSnap);
			agent->EvaluatePackage(true, false);
			return true;
		}

		// Terminal-state logging + player-facing notification (game thread only).
		// The corner message is sent through the nearest API this checkout ships:
		// RE::DebugNotification is absent (see README), so SendHUDMessage is used.
		void AnnounceFetchDone(const Task& a_task)
		{
			if (a_task.type != Type::Fetch) return;
			const std::string msg = a_task.targetName + " has been brought to you.";
			RE::SendHUDMessage::ShowHUDMessage(msg.c_str());

			// The fetch is delivered: a direct narration (the target reacts) so
			// the encounter has a spoken reason behind it.
			auto* target = RE::TESForm::LookupByID<RE::Actor>(a_task.targetFormID);
			auto* player = RE::PlayerCharacter::GetSingleton();
			const std::string playerName = PlayerName();
			const std::string narration = a_task.targetName + " was told that " +
				playerName + " wanted to meet, agreed, and has just arrived to hear what " +
				playerName + " has to say.";
			skyrimnet::DirectNarration(narration, target, player);
		}

		// Emit the "called off" persistent event (and clear the per-actor state
		// key) when a task is cancelled. An Assassinate announces nothing on a
		// cancel - it only clears its contract key. Game thread only.
		void AnnounceCancel(const Task& a_task)
		{
			auto* agent = RE::TESForm::LookupByID<RE::Actor>(a_task.agentFormID);
			ClearTaskStateKey(a_task);

			// The cancelled task's target/recipient is never informed; the
			// player and any nearby bystanders are (default audience).
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (a_task.type == Type::Fetch) {
				skyrimnet::RegisterPersistentEvent(
					a_task.agentName + " has called off fetching " + a_task.targetName + ".",
					agent, player);
			} else if (a_task.type == Type::Deliver) {
				skyrimnet::RegisterPersistentEvent(
					a_task.agentName + " has called off delivering a message to " +
						a_task.targetName + ".",
					agent, player);
			} else if (a_task.type == Type::Meet) {
				skyrimnet::RegisterPersistentEvent(
					a_task.agentName + " has called off the meeting with " + PlayerName() + ".",
					agent, player);
			}
		}

		void AnnounceFetchFailed(const Task& a_task)
		{
			if (a_task.type != Type::Fetch) return;
			const std::string msg = "Fetch failed - " + a_task.targetName + " is unavailable.";
			RE::SendHUDMessage::ShowHUDMessage(msg.c_str());
		}

		// The near-destination unreachable report: the agent reached the target's
		// vicinity but could not get in. If the target is inside a building the
		// message says so; otherwise the target simply could not be reached. The
		// wording carries the action's own verb (fetch / deliver a message to;
		// a meet reports the meeting place). The failure is narrated to the
		// player (the fetch target / message recipient is never informed). Game
		// thread only.
		void AnnounceUnreachable(const Task& a_task)
		{
			auto* agent  = RE::TESForm::LookupByID<RE::Actor>(a_task.agentFormID);
			auto* target = RE::TESForm::LookupByID<RE::Actor>(a_task.targetFormID);

			// This is a terminal failure for the action, so drop its per-actor
			// state key (a Meet's, if any).
			ClearTaskStateKey(a_task);

			std::string hud;
			std::string event;

			if (a_task.type == Type::Meet) {
				hud = a_task.agentName + " couldn't reach the meeting place at " +
					a_task.targetName + " and is coming back.";
				event = a_task.agentName + " tried to meet at " + a_task.targetName +
					" but could not reach the meeting place.";
				RE::SendHUDMessage::ShowHUDMessage(hud.c_str());
				skyrimnet::RegisterPersistentEvent(event, agent, target);
				return;
			}

			auto* cell = target ? target->GetParentCell() : nullptr;
			const bool lockedBuilding = cell && cell->IsInteriorCell();
			const bool deliver = a_task.type == Type::Deliver;

			if (lockedBuilding) {
				hud = a_task.agentName + " couldn't reach " + a_task.targetName +
					" - they're inside a locked building.";
			} else {
				hud = a_task.agentName + " couldn't reach " + a_task.targetName +
					" and is coming back.";
			}
			RE::SendHUDMessage::ShowHUDMessage(hud.c_str());

			// The failure is narrated (the fetch target / message recipient is
			// never informed). The narration carries the whole fact.
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (deliver) {
				skyrimnet::DirectNarration(
					lockedBuilding
						? a_task.agentName + " returns and tells " + PlayerName() +
							" that the message could not be delivered - the door to " +
							a_task.targetName + " was locked."
						: a_task.agentName + " returns and tells " + PlayerName() +
							" that " + a_task.targetName + " could not be reached - the message was not delivered.",
					agent, player);
			} else {
				skyrimnet::DirectNarration(
					lockedBuilding
						? a_task.agentName + " tried to fetch " + a_task.targetName +
							" but could not reach them - the building was locked."
						: a_task.agentName + " tried to fetch " + a_task.targetName +
							" but could not reach them and returned.",
					agent, player);
			}
		}

		// The rare refusal when every Here-spot is leased.
		void AnnounceMeetNoSpot(RE::Actor* a_agent)
		{
			const std::string agentName = DisplayName(a_agent);
			RE::SendHUDMessage::ShowHUDMessage(
				(agentName + " can't arrange another meeting right now.").c_str());
			skyrimnet::RegisterPersistentEvent(
				agentName + " tried to arrange a meeting, but too many meetings are already arranged.",
				a_agent, RE::PlayerCharacter::GetSingleton());
		}

		// ---- assassination ------------------------------------------------

		// A stack callback that captures the VM call's result, so a rejected
		// PlayIdleWithTarget (the engine can refuse a paired idle while the
		// target runs a competing idle - a lean, a farm loop) is visible
		// instead of silently discarded. Writes into the per-task shared
		// KillVmResult so concurrent assassinations never collide.
		class ResultStackCallback : public RE::BSScript::IStackCallbackFunctor
		{
		public:
			bool                          haveResult = false;
			bool                          boolResult = false;
			RE::FormID                    agentFormID = 0;
			RE::FormID                    targetFormID = 0;
			std::shared_ptr<KillVmResult> sink;
			void operator()(RE::BSScript::Variable a_result) override
			{
				haveResult = true;
				boolResult = a_result.IsBool() ? a_result.GetBool() : false;
				if (sink) {
					sink->agent = agentFormID;
					sink->target = targetFormID;
					sink->result = boolResult;
					sink->have = true;
				}
				logs::info("IntelLite tasks: PlayIdleWithTarget returned {} (agent {:08X}, target {:08X})",
					boolResult ? "true" : "false", agentFormID, targetFormID);
			}
			void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}
		};

		// Dispatch an INSTANCE method call and report its boolean result.
		// Returns false when the dispatch itself failed; on success
		// `a_haveResult` says whether the VM delivered a result and
		// `a_result` holds it. Game thread only.
		bool DispatchInstanceBool(RE::Actor* a_actor, RE::TESObjectREFR* a_target,
			const char* a_script, const char* a_fn, RE::BSScript::IFunctionArguments* a_args,
			const std::shared_ptr<KillVmResult>& a_sink,
			bool& a_haveResult, bool& a_result)
		{
			a_haveResult = false;
			a_result = false;
			if (!a_actor) {
				delete a_args;
				return false;
			}
			auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
			if (!vm) {
				delete a_args;
				return false;
			}
			auto* policy = vm->GetObjectHandlePolicy1();
			if (!policy) {
				delete a_args;
				return false;
			}
			const auto handle = policy->GetHandleForObject(a_actor->GetFormType(), a_actor);
			if (handle == policy->EmptyHandle()) {
				delete a_args;
				return false;
			}
			RE::BSTSmartPointer<RE::BSScript::Object> script;
			if (!vm->FindBoundObject(handle, a_script, script) || !script) {
				delete a_args;
				return false;
			}
			RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback{
				new ResultStackCallback()
			};
			auto* resultCallback = static_cast<ResultStackCallback*>(callback.get());
			resultCallback->agentFormID = a_actor->GetFormID();
			resultCallback->targetFormID = a_target ? a_target->GetFormID() : 0;
			resultCallback->sink = a_sink;
			if (!vm->DispatchMethodCall1(script, RE::BSFixedString(a_fn), a_args, callback)) {
				return false;
			}
			a_haveResult = resultCallback->haveResult;
			a_result = resultCallback->boolResult;
			return true;
		}

		// Dispatch an INSTANCE method call on the Papyrus object bound to
		// `a_actor` (PairedAnimations' pattern, mirroring the AsyncDispatch
		// reference): obtain the actor's VM handle from the handle policy,
		// FindBoundObject the named script, then DispatchMethodCall1. Args are
		// always a fresh MakeFunctionArguments block (the VM takes ownership).
		// Game thread only.
		bool DispatchInstance(RE::Actor* a_actor, const char* a_script,
			const char* a_fn, RE::BSScript::IFunctionArguments* a_args)
		{
			if (!a_actor) {
				delete a_args;
				return false;
			}
			auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
			if (!vm) {
				delete a_args;
				return false;
			}

			auto* policy = vm->GetObjectHandlePolicy1();
			if (!policy) {
				delete a_args;
				return false;
			}

			const auto handle = policy->GetHandleForObject(a_actor->GetFormType(), a_actor);
			if (handle == policy->EmptyHandle()) {
				delete a_args;
				return false;
			}

			RE::BSTSmartPointer<RE::BSScript::Object> script;
			if (!vm->FindBoundObject(handle, a_script, script) || !script) {
				delete a_args;
				return false;
			}

			RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback{
				new vm::NoopStackCallback()
			};
			return vm->DispatchMethodCall1(
				script, RE::BSFixedString(a_fn), a_args, callback);
		}

		// Send a Debug.SendAnimationEvent (VM dispatch) at a reference. The VM
		// signature needs an ObjectReference - an Actor* argument silently
		// fails. Game thread only.
		bool SendAnimEvent(RE::TESObjectREFR* a_ref, const char* a_event)
		{
			if (!a_ref) return false;
			auto* args = RE::MakeFunctionArguments(
				static_cast<RE::TESObjectREFR*>(a_ref), std::string(a_event));
			if (!vm::DispatchStatic("Debug", "SendAnimationEvent", args)) {
				logs::warn("IntelLite tasks: Debug.SendAnimationEvent({}) dispatch failed for {:08X}",
					a_event, a_ref->GetFormID());
				return false;
			}
			return true;
		}

		// Play a paired killmove by dispatching the Actor Papyrus instance method
		// PlayIdleWithTarget(Idle, ObjectReference) on the executor - the
		// PairedAnimations approach. (Debug.SendAnimationEvent is wrong here: the
		// pa_* forms are IDLE forms, not animation events, so it produces
		// nothing.) The call's own result is captured: the engine can REFUSE
		// the idle (false) when the target runs a competing idle, and a refusal
		// must be visible (the scene's engagement poll decides the retry). Game thread only.
		bool PlayIdleWithTarget(RE::Actor* a_actor, RE::TESIdleForm* a_idle,
			RE::TESObjectREFR* a_target, const std::shared_ptr<KillVmResult>& a_sink)
		{
			if (!a_actor || !a_idle) return false;
			auto* args = RE::MakeFunctionArguments(
				static_cast<RE::TESIdleForm*>(a_idle),
				static_cast<RE::TESObjectREFR*>(a_target));
			bool haveResult = false;
			bool accepted = false;
			if (!DispatchInstanceBool(a_actor, a_target, "Actor", "PlayIdleWithTarget", args,
					a_sink, haveResult, accepted)) {
				logs::warn("IntelLite tasks: Actor.PlayIdleWithTarget dispatch failed for {:08X}",
					a_actor->GetFormID());
				return false;
			}
			if (!haveResult) {
				// The VM defers the result; nothing to read yet. The killmove
				// state itself (IsInKillMove) is the real confirmation.
				return true;
			}
			if (!accepted) {
				logs::warn("IntelLite tasks: PlayIdleWithTarget REJECTED (agent {:08X}, target {:08X})",
					a_actor->GetFormID(), a_target ? a_target->GetFormID() : 0);
				return false;
			}
			logs::info("IntelLite tasks: killmove accepted (agent {:08X}, target {:08X})",
				a_actor->GetFormID(), a_target ? a_target->GetFormID() : 0);
			return true;
		}

		// Turn a pair to face each other for the kill scene. When the agent is
		// behind the target (a backstab) the target is left facing away. Game
		// thread only.
		void FaceForKill(RE::Actor* a_agent, RE::Actor* a_target, bool a_agentBehind)
		{
			if (!a_agent || !a_target) return;
			const auto ap = a_agent->GetPosition();
			const auto tp = a_target->GetPosition();
			a_agent->SetHeading(static_cast<float>(std::atan2(tp.x - ap.x, tp.y - ap.y)));
			if (!a_agentBehind) {
				a_target->SetHeading(static_cast<float>(std::atan2(ap.x - tp.x, ap.y - tp.y)));
			}
		}

		// Release a SetDontMove + SetRestrained hold unconditionally (a hold
		// saved on an actor must be cleared even when the runtime flag did
		// not survive a reload). Game thread only.
		void ClearTargetHold(RE::Actor* a_target)
		{
			if (!a_target) return;
			auto* dontMove = RE::MakeFunctionArguments(false);
			DispatchInstance(a_target, "Actor", "SetDontMove", dontMove);
			auto* restrain = RE::MakeFunctionArguments(false);
			DispatchInstance(a_target, "Actor", "SetRestrained", restrain);
		}

		// Stop a target from moving AND acting for the kill window and keep it
		// stopped. SetDontMove alone does not hold a package-driven actor - its
		// own routine can walk it away - and an idle loop (a farm, a lean)
		// keeps re-asserting through it; SetRestrained is the engine's
		// own hold (the vanilla execution scenes' primitive) and the reference
		// paired-animation mod restrains its victim for exactly this window.
		// Both are released on engagement or at cleanup. Game thread only.
		void HoldTargetForKill(Task& a_task, RE::Actor* a_target)
		{
			if (!a_target) return;
			a_target->StopMoving(0.0f);
			auto* dontMove = RE::MakeFunctionArguments(true);
			if (!DispatchInstance(a_target, "Actor", "SetDontMove", dontMove)) {
				logs::warn("task {}: SetDontMove dispatch failed for {:08X}",
					a_task.id, a_target->GetFormID());
			}
			auto* restrain = RE::MakeFunctionArguments(true);
			if (!DispatchInstance(a_target, "Actor", "SetRestrained", restrain)) {
				logs::warn("task {}: SetRestrained dispatch failed for {:08X}",
					a_task.id, a_target->GetFormID());
			}
			if (!a_task.targetHeld) {
				a_task.targetHeld = true;
				logs::info("task {}: {} held in place for the killmove",
					a_task.id, a_task.targetName);
			}
		}

		void ReleaseTargetHold(Task& a_task, RE::Actor* a_target)
		{
			if (!a_task.targetHeld) return;
			a_task.targetHeld = false;
			ClearTargetHold(a_target);
			logs::info("task {}: target hold released", a_task.id);
		}

		// Keep a staged target out of its routine: the restraint hold blocks
		// actions but not package movement (a restrained actor can still walk
		// and resume its routine), so every scene tick re-clears a
		// furniture attachment, and - once the walk phase is over - stops the
		// current idle and the walk drift. Suspended around a dispatch so an
		// accepted killmove is never interrupted. Game thread only.
		void PulseTargetHold(Task& a_task, RE::Actor* a_target, bool a_stopMovement)
		{
			if (!a_target || a_target->IsDead()) return;
			if (auto* proc = a_target->GetActorRuntimeData().currentProcess) {
				if (a_stopMovement) {
					proc->StopCurrentIdle(a_target, true);
				}
				const RE::ObjectRefHandle handle = a_target->GetOccupiedFurniture();
				auto furniturePtr = handle.get();
				if (furniturePtr.get()) {
					proc->ClearFurniture(a_target);
					if (!a_task.killPulseLogged) {
						a_task.killPulseLogged = true;
						logs::info("task {}: {} re-took furniture - cleared it",
							a_task.id, a_task.targetName);
					}
				}
			}
			if (a_stopMovement) {
				a_target->StopMoving(0.0f);
			}
		}

		// Stage the pair for the paired killmove: force-stop the victim's
		// competing idle and face the executor at the victim. The executor
		// is NOT repositioned - the engine's killmove warps the pair
		// together anyway, so the dispatch goes out from wherever the
		// executor stands. No dispatch happens here - the dispatch runs
		// after the settle beat. Game thread only.
		void KillMovePrepare(Task& a_task, RE::Actor* a_agent, RE::Actor* a_target)
		{
			if (!a_agent || !a_target) return;

			// Keep the target where the executor found it for this attempt.
			HoldTargetForKill(a_task, a_target);

			// Force-stop any competing idle (a lean, a farm loop, a leftover
			// pose) and clear any furniture attachment the victim re-took in
			// the staging window: the engine refuses a paired idle while an
			// idle runs or the victim is back on a marker.
			if (auto* proc = a_target->GetActorRuntimeData().currentProcess) {
				proc->StopCurrentIdle(a_target, true);
				proc->ClearFurniture(a_target);
			}

			FaceForKill(a_agent, a_target, true);
		}

		// The package currently running on the actor, for the diagnostics:
		// whether OUR walk package is actually the one moving a target.
		// Game thread only.
		RunningPkg RunningPackageOf(RE::Actor* a_actor)
		{
			RunningPkg out;
			if (a_actor) {
				if (auto* proc = a_actor->GetActorRuntimeData().currentProcess) {
					if (auto* pkg = proc->GetRunningPackage()) {
						out.form = pkg->GetFormID();
						const char* e = pkg->GetFormEditorID();
						out.id = (e && *e) ? e : "-";
					}
				}
			}
			return out;
		}

		// One compact per-actor state string for the killmove diagnostics:
		// the flags the engine's paired-idle acceptance may depend on, the
		// actor's last played idle, and the engine's own killmove clocks
		// (MiddleHighProcessData). Game thread only.
		std::string ActorKillState(RE::Actor* a_actor)
		{
			if (!a_actor) return "-";
			auto* proc = a_actor->GetActorRuntimeData().currentProcess;
			auto* mh = proc ? proc->middleHigh : nullptr;

			RE::FormID  pkgForm = 0;
			const char* pkgId = "-";
			if (proc) {
				if (auto* pkg = proc->GetRunningPackage()) {
					pkgForm = pkg->GetFormID();
					const char* e = pkg->GetFormEditorID();
					pkgId = (e && *e) ? e : "-";
				}
			}

			RE::FormID furniture = 0;
			const RE::ObjectRefHandle handle = a_actor->GetOccupiedFurniture();
			auto furniturePtr = handle.get();
			if (auto* f = furniturePtr.get()) {
				furniture = f->GetFormID();
			}

			const char* idle = (mh && mh->lastIdlePlayed)
				? mh->lastIdlePlayed->GetFormEditorID() : nullptr;

			char buf[256]{};
			std::snprintf(buf, sizeof(buf),
				"sneak=%d move=%d cmbt=%d km=%d life=%d sit=%d furn=%08X pkg=%08X/%s idle=%s kmT=%.1f dkT=%.1f kq=%d dk=%d",
				a_actor->IsSneaking() ? 1 : 0,
				a_actor->IsMoving() ? 1 : 0,
				a_actor->IsInCombat() ? 1 : 0,
				a_actor->IsInKillMove() ? 1 : 0,
				static_cast<int>(a_actor->AsActorState()->GetLifeState()),
				a_actor->AsActorState()->IsSitting() ? 1 : 0,
				furniture,
				pkgForm, pkgId,
				(idle && *idle) ? idle : "-",
				mh ? mh->killMoveTimer : -1.0f,
				mh ? mh->deferredKillTimer : -1.0f,
				mh ? (mh->killQueued ? 1 : 0) : -1,
				mh ? (mh->inDeferredKill ? 1 : 0) : -1);
			return buf;
		}

		void LogKillState(const char* a_phase, const Task& a_task, RE::Actor* a_agent, RE::Actor* a_target)
		{
			const float dist = (a_agent && a_target)
				? a_agent->GetPosition().GetDistance(a_target->GetPosition())
				: -1.0f;
			logs::info("task {}: killmove {} - dist={:.0f} agent[{:08X}] {} target[{:08X}] {}",
				a_task.id, a_phase, dist,
				a_agent ? a_agent->GetFormID() : 0, ActorKillState(a_agent),
				a_target ? a_target->GetFormID() : 0, ActorKillState(a_target));
		}

		// Forward declaration for PlayKillMove.
		void BeginBountyGuard();

		// Dispatch the vanilla backstab paired idle (0F465A =
		// pa_1HMKillMoveBackStab, the Markarth scene's SneakKillMove) at the
		// target. The pair must already be staged (KillMovePrepare) and
		// settled; this only clears the protected flag, looks the idle up and
		// fires the VM call. Game thread only.
		bool PlayKillMove(Task& a_task, RE::Actor* a_agent, RE::Actor* a_target)
		{
			if (!a_agent || !a_target) return false;

			BeginBountyGuard();

			// The engine can re-set a protected flag while the contract runs
			// (from the base record or a quest alias) and a killmove on a
			// protected actor can be skipped or only produce a bleedout: clear
			// runtime, base, and alias Protected flags at the dispatch moment.
			if (!IsTargetEssential(a_target) && IsTargetProtected(a_target)) {
				a_target->GetActorRuntimeData().boolFlags.reset(RE::Actor::BOOL_FLAGS::kProtected);
				if (!a_task.protectedCleared) {
					a_task.protectedCleared = true;
					a_task.targetBaseProtected = BaseIsProtected(a_target);
					a_task.targetAliasProtected = AliasIsProtected(a_target);
				}
				SetBaseProtected(a_target, false);
				SetAliasProtected(a_target, false);
				logs::info("IntelLite tasks: kill target {:08X} was protected - flag cleared for the killmove",
					a_target->GetFormID());
			}

			auto* idle = RE::TESForm::LookupByEditorID<RE::TESIdleForm>("pa_1HMKillMoveBackStab");
			if (!idle) {
				logs::warn("IntelLite tasks: killmove idle pa_1HMKillMoveBackStab not found in this load order");
				return false;
			}
			a_task.killVmResult = std::make_shared<KillVmResult>();
			return PlayIdleWithTarget(a_agent, idle, a_target, a_task.killVmResult);
		}

		// A kill makes guards witness a murder and slaps the player with a crime
		// bounty. Snapshot every crime-tracking faction's crime gold when the kill
		// starts and clear any increase for a short window (the animation's own
		// kill event can land a moment later, so GuardBounty keeps checking after
		// the task completes). Game thread only.
		constexpr float kBountyGuardSeconds = 8.0f;
		struct BountyGuardEntry { RE::TESFaction* faction = nullptr; std::int32_t nv = 0; std::int32_t v = 0; };
		std::vector<BountyGuardEntry> g_bountySnapshot;
		float g_bountyGuardUntil = 0.0f;

		void BeginBountyGuard()
		{
			if (RealSeconds() < g_bountyGuardUntil && !g_bountySnapshot.empty()) {
				// Extend the guard window while keeping the pre-kill crime-gold baseline.
				g_bountyGuardUntil = RealSeconds() + kBountyGuardSeconds;
				return;
			}
			g_bountySnapshot.clear();
			g_bountyGuardUntil = RealSeconds() + kBountyGuardSeconds;
			auto* dh = RE::TESDataHandler::GetSingleton();
			if (!dh) return;
			for (auto* f : dh->GetFormArray<RE::TESFaction>()) {
				if (!f || !f->TracksCrimes()) continue;
				g_bountySnapshot.push_back({ f, f->GetCrimeGoldNonViolent(), f->GetCrimeGoldViolent() });
			}
			logs::info("IntelLite tasks: bounty guard armed ({} faction(s))", g_bountySnapshot.size());
		}

		void GuardBounty()
		{
			if (g_bountyGuardUntil <= 0.0f) return;
			if (RealSeconds() > g_bountyGuardUntil) {
				g_bountySnapshot.clear();
				g_bountyGuardUntil = 0.0f;
				return;
			}
			for (auto& e : g_bountySnapshot) {
				if (!e.faction) continue;
				const auto nv = e.faction->GetCrimeGoldNonViolent();
				const auto v  = e.faction->GetCrimeGoldViolent();
				if (nv > e.nv) {
					e.faction->ModCrimeGold(e.nv - nv, false);
					const std::string edid = npc_index::FormEditorID(e.faction);
					logs::info("IntelLite tasks: bounty guard cleared {} non-violent crime gold on {:08X} ({})",
						nv - e.nv, e.faction->GetFormID(), edid.empty() ? "(none)" : edid.c_str());
				}
				if (v > e.v) {
					e.faction->ModCrimeGold(e.v - v, true);
					const std::string edid = npc_index::FormEditorID(e.faction);
					logs::info("IntelLite tasks: bounty guard cleared {} violent crime gold on {:08X} ({})",
						v - e.v, e.faction->GetFormID(), edid.empty() ? "(none)" : edid.c_str());
				}
			}
		}

		// Guarantee the target is dead. Actor exposes no Kill(Actor*); the
		// engine entry point is KillImpl(attacker, damage, sendEvent,
		// ragdollInstant). Works on unloaded persistent references. A null
		// a_attacker is an intended no-culprit kill, used for a victim the
		// player cannot see; pass the agent when the victim is visible.
		// BeginBountyGuard/GuardBounty remain active in this build as the
		// crime-gold detector (and safety net for visible kills). The
		// protected flag was already cleared when the contract started; the
		// check here is a safety net. Game thread only.
		void KillTarget(RE::Actor* a_target, RE::Actor* a_attacker)
		{
			if (!a_target || a_target->IsDead()) return;
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* playerCrimeFaction = player ? player->GetCrimeFaction() : nullptr;
			auto* targetCrimeFaction = a_target->GetCrimeFaction();
			const std::string playerCrimeEdid = npc_index::FormEditorID(playerCrimeFaction);
			const std::string targetCrimeEdid = npc_index::FormEditorID(targetCrimeFaction);
			logs::info("IntelLite tasks: KillTarget target={:08X} attacker={:08X} playerCrimeFaction={:08X} ({}) targetCrimeFaction={:08X} ({})",
				a_target->GetFormID(),
				a_attacker ? a_attacker->GetFormID() : 0,
				playerCrimeFaction ? playerCrimeFaction->GetFormID() : 0,
				playerCrimeEdid.empty() ? "(none)" : playerCrimeEdid.c_str(),
				targetCrimeFaction ? targetCrimeFaction->GetFormID() : 0,
				targetCrimeEdid.empty() ? "(none)" : targetCrimeEdid.c_str());
			BeginBountyGuard();
			if (!IsTargetEssential(a_target) && IsTargetProtected(a_target)) {
				a_target->GetActorRuntimeData().boolFlags.reset(RE::Actor::BOOL_FLAGS::kProtected);
				SetBaseProtected(a_target, false);
				SetAliasProtected(a_target, false);
				logs::info("IntelLite tasks: kill target {:08X} was protected - flag cleared",
					a_target->GetFormID());
			}
			a_target->KillImpl(a_attacker, kKillDamage, true, false);
			GuardBounty();
		}

		// Both the NPC_ record (base/template) and quest aliases in
		// ExtraAliasInstanceArray (e.g. DialogueCarriageSystem on Bjorlam,
		// Stables on Skulvar) carry the Protected flag the engine keeps
		// re-asserting onto the runtime Actor boolFlags on every AI/package
		// update. A killmove hit on an actor with Protected set only produces a
		// bleedout (kEssentialDown, life=7). Clear base, template, and alias
		// Protected flags alongside the runtime flag for the contract, and
		// restore them if the contract ends without a kill or on save load/revert.
		// Game thread only.
		std::unordered_set<RE::FormID> g_protectedClearedBases;

		struct ClearedAliasProtected
		{
			RE::FormID    actorFormID = 0;
			RE::FormID    questFormID = 0;
			std::uint32_t aliasID = 0;
		};
		std::vector<ClearedAliasProtected> g_protectedClearedAliases;

		void SetBaseProtected(RE::Actor* a_target, bool a_protected)
		{
			if (!a_target) return;
			auto toggleNpc = [a_protected](RE::TESNPC* a_npc) {
				if (!a_npc) return;
				if (a_protected) {
					a_npc->actorData.actorBaseFlags.set(RE::ACTOR_BASE_DATA::Flag::kProtected);
					g_protectedClearedBases.erase(a_npc->GetFormID());
				} else {
					if (a_npc->actorData.actorBaseFlags.all(RE::ACTOR_BASE_DATA::Flag::kProtected)) {
						g_protectedClearedBases.insert(a_npc->GetFormID());
					}
					a_npc->actorData.actorBaseFlags.reset(RE::ACTOR_BASE_DATA::Flag::kProtected);
				}
			};
			toggleNpc(a_target->GetActorBase());
			if (auto* tpl = a_target->GetTemplateBase(); tpl && tpl != a_target->GetActorBase()) {
				toggleNpc(tpl);
			}
		}

		void SetAliasProtected(RE::Actor* a_target, bool a_protected)
		{
			if (!a_target) return;
			auto* extraAliases = a_target->extraList.GetByType<RE::ExtraAliasInstanceArray>();
			if (!extraAliases) return;
			const RE::FormID actorID = a_target->GetFormID();
			RE::BSReadLockGuard lock(extraAliases->lock);
			if (a_protected) {
				for (const auto* inst : extraAliases->aliases) {
					if (!inst || !inst->alias) continue;
					const RE::FormID qid = inst->quest ? inst->quest->GetFormID()
						: (inst->alias->owningQuest ? inst->alias->owningQuest->GetFormID() : 0);
					const std::uint32_t aid = inst->alias->aliasID;
					auto it = std::find_if(g_protectedClearedAliases.begin(), g_protectedClearedAliases.end(),
						[actorID, qid, aid](const ClearedAliasProtected& e) {
							return e.actorFormID == actorID && e.questFormID == qid && e.aliasID == aid;
						});
					if (it != g_protectedClearedAliases.end()) {
						const_cast<RE::BGSBaseAlias*>(inst->alias)->SetProtected(true);
						g_protectedClearedAliases.erase(it);
					}
				}
			} else {
				for (const auto* inst : extraAliases->aliases) {
					if (!inst || !inst->alias) continue;
					if (!inst->alias->IsProtected()) continue;
					const RE::FormID qid = inst->quest ? inst->quest->GetFormID()
						: (inst->alias->owningQuest ? inst->alias->owningQuest->GetFormID() : 0);
					const std::uint32_t aid = inst->alias->aliasID;
					const bool alreadyTracked = std::any_of(
						g_protectedClearedAliases.begin(), g_protectedClearedAliases.end(),
						[actorID, qid, aid](const ClearedAliasProtected& e) {
							return e.actorFormID == actorID && e.questFormID == qid && e.aliasID == aid;
						});
					if (!alreadyTracked) {
						g_protectedClearedAliases.push_back({ actorID, qid, aid });
					}
					const_cast<RE::BGSBaseAlias*>(inst->alias)->SetProtected(false);
				}
			}
		}

		// Restore a target's protected flag when a contract ends without a
		// kill: the early clear (done the moment the executor sets out) must
		// not outlive the contract, so a cancelled or failed assassination
		// does not leave the NPC permanently killable. Game thread only.
		void RestoreProtectedFlag(Task& a_task)
		{
			if (!a_task.protectedCleared) return;
			a_task.protectedCleared = false;
			auto* target = RE::TESForm::LookupByID<RE::Actor>(a_task.targetFormID);
			if (!target) return;
			if (a_task.targetBaseProtected) {
				SetBaseProtected(target, true);
			}
			if (a_task.targetAliasProtected) {
				SetAliasProtected(target, true);
			}
			if (!target->IsDead()) {
				target->GetActorRuntimeData().boolFlags.set(RE::Actor::BOOL_FLAGS::kProtected);
				logs::info("task {}: protected flag restored on {}", a_task.id, a_task.targetName);
			}
		}

		void RestoreAllProtectedFlags()
		{
			for (auto& t : g_tasks) {
				if (t.protectedCleared) {
					RestoreProtectedFlag(t);
				}
			}
			for (const auto baseId : g_protectedClearedBases) {
				if (auto* base = RE::TESForm::LookupByID<RE::TESNPC>(baseId)) {
					base->actorData.actorBaseFlags.set(RE::ACTOR_BASE_DATA::Flag::kProtected);
				}
			}
			g_protectedClearedBases.clear();
			for (const auto& entry : g_protectedClearedAliases) {
				auto* actor = RE::TESForm::LookupByID<RE::Actor>(entry.actorFormID);
				if (!actor) continue;
				auto* extraAliases = actor->extraList.GetByType<RE::ExtraAliasInstanceArray>();
				if (!extraAliases) continue;
				RE::BSReadLockGuard lock(extraAliases->lock);
				for (const auto* inst : extraAliases->aliases) {
					if (!inst || !inst->alias) continue;
					const RE::FormID qid = inst->quest ? inst->quest->GetFormID()
						: (inst->alias->owningQuest ? inst->alias->owningQuest->GetFormID() : 0);
					if (qid == entry.questFormID && inst->alias->aliasID == entry.aliasID) {
						const_cast<RE::BGSBaseAlias*>(inst->alias)->SetProtected(true);
					}
				}
			}
			g_protectedClearedAliases.clear();
		}

		// Make sure the executor carries a drawn iron dagger for the kill
		// scene: a Path B executor already got one at task creation, so only
		// the first call spawns. Falls back to a carried one-handed weapon
		// when the dagger form is missing. Game thread only.
		void EnsureKillDagger(Task& a_task, RE::Actor* a_agent)
		{
			if (!a_agent) return;
			if (a_task.spawnedWeaponFormID == 0) {
				auto* dagger = RE::TESForm::LookupByEditorID<RE::TESObjectWEAP>("IronDagger");
				if (dagger) {
					a_agent->AddObjectToContainer(dagger, nullptr, 1, nullptr);
					if (auto* equip = RE::ActorEquipManager::GetSingleton()) {
						equip->EquipObject(a_agent, dagger, nullptr, 1, nullptr, false, false, false, true);
					}
					a_task.spawnedWeaponFormID = dagger->GetFormID();
				} else {
					// Fallback: equip the first carried one-handed weapon.
					auto* equipped = a_agent->GetEquippedObject(false);
					if (!equipped || !equipped->IsWeapon()) {
						for (auto& [item, data] : a_agent->GetInventory()) {
							auto* weap = item ? item->As<RE::TESObjectWEAP>() : nullptr;
							if (weap && (weap->GetWeaponType() == RE::WEAPON_TYPE::kOneHandSword ||
								weap->GetWeaponType() == RE::WEAPON_TYPE::kOneHandDagger ||
								weap->GetWeaponType() == RE::WEAPON_TYPE::kOneHandAxe ||
								weap->GetWeaponType() == RE::WEAPON_TYPE::kOneHandMace)) {
								if (auto* equip = RE::ActorEquipManager::GetSingleton()) {
									equip->EquipObject(a_agent, weap, nullptr, 1, nullptr, false, false, false, true);
								}
								break;
							}
						}
					}
				}
			}
			a_agent->DrawWeaponMagicHands(true);
			a_task.weaponDrawnByUs = true;
		}

		// Undo what EnsureKillDagger did: sheathe, then UNEQUIP the spawned
		// dagger and remove it from the executor. The unequip is SYNCHRONOUS
		// (queueEquip=false, applyNow=true): a queued unequip processed after
		// the item was already removed races the engine's equip pipeline (a
		// crash through HDT-SMP's equip hook). Only what we added is undone.
		// Game thread only.
		void CleanupKillDagger(Task& a_task, RE::Actor* a_agent)
		{
			if (!a_agent || !a_task.weaponDrawnByUs) return;
			a_agent->DrawWeaponMagicHands(false);
			a_task.weaponDrawnByUs = false;
			if (a_task.spawnedWeaponFormID == 0) return;
			if (auto* w = RE::TESForm::LookupByID<RE::TESObjectWEAP>(a_task.spawnedWeaponFormID)) {
				if (auto* equip = RE::ActorEquipManager::GetSingleton()) {
					equip->UnequipObject(a_agent, w, nullptr, 1, nullptr, false, false, true, true);
				}
				a_agent->RemoveItem(w, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
			}
			a_task.spawnedWeaponFormID = 0;
			logs::info("task {}: kill dagger sheathed, unequipped and removed", a_task.id);
		}

		// Remove a dagger this mod spawned whose owning task no longer exists: a
		// dropped co-save task cannot run its own cleanup. Sheathe, unequip
		// synchronously, then remove - the same order as CleanupKillDagger. Game
		// thread only.
		void ReleasePendingWeapon(std::uint32_t a_agentFormID, std::uint32_t a_weaponFormID)
		{
			auto* agent  = RE::TESForm::LookupByID<RE::Actor>(a_agentFormID);
			auto* weapon = RE::TESForm::LookupByID<RE::TESObjectWEAP>(a_weaponFormID);
			if (!agent || !weapon) return;
			auto* changes = agent->GetInventoryChanges(true);
			if (changes && changes->GetItemCount(weapon) <= 0) return;  // already gone
			agent->DrawWeaponMagicHands(false);
			if (auto* equip = RE::ActorEquipManager::GetSingleton()) {
				equip->UnequipObject(agent, weapon, nullptr, 1, nullptr, false, false, true, true);
			}
			agent->RemoveItem(weapon, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
			logs::info("IntelLite tasks: pending kill dagger {:08X} removed from {:08X}",
				a_weaponFormID, a_agentFormID);
		}

		// A Path B executor was crouched by the approach package: after the
		// scene (or an off-screen release) it is stood back up when it is
		// still sneaking. Only for a task that ran the sneak approach. Game
		// thread only.
		void ReleaseSneak(Task& a_task, RE::Actor* a_agent)
		{
			if (!a_task.sneakApproach) return;
			a_task.sneakApproach = false;
			if (!a_agent) return;
			if (a_agent->IsSneaking()) {
				SendAnimEvent(a_agent, "sneakStop");
				logs::info("task {}: executor sneak released", a_task.id);
			}
		}

		// Drive a target that is using furniture (sitting, or leaning on a
		// lean marker - those are furniture records too) off it: the engine
		// blocks a package walk from furniture, so the state is cleared, the
		// pose reset and the actor snapped off the seat first; then it WALKS
		// (the slow walk package) toward the PLAYER - a destination that is
		// always loaded and on open ground. Called at contract start (mortal
		// only, before the executor sets out) so the executor intercepts a
		// moving target on clear ground, on a later travel tick for the
		// beat-completion block or the late retry when an unloaded mortal
		// target loads on approach, and again at scene start (mortal only).
		// With `a_standUpBeat` the get-up animation (idleChairExitStart) is
		// given a beat before the state is cleared and the walk starts - the
		// drive then completes on a later tick. A target that is not 3D-loaded
		// is never driven. Returns true when the drive handled the target.
		// Game thread only.
		bool DriveTargetOffFurniture(Task& a_task, RE::Actor* a_target, RE::Actor* a_player,
			bool a_standUpBeat)
		{
			if (!a_target || !a_player) return false;

			// Only a LOADED target is ever driven: the walk-toward-player
			// package is a visible-scene tool, and applying it to an unloaded
			// target leaves it running blind - the target would start walking
			// toward the player the moment it loads, which an off-screen kill
			// must never do.
			if (!a_target->Is3DLoaded()) {
				logs::info("task {}: {} is not loaded - the furniture drive is deferred",
					a_task.id, a_task.targetName);
				return false;
			}

			// The first call detects the furniture; a deferred get-up beat or
			// an already-started walk means the detection has happened.
			if (a_task.furnitureStandUpReal == 0.0f && !a_task.killWalkPending) {
				const RE::ObjectRefHandle handle = a_target->GetOccupiedFurniture();
				auto furniturePtr = handle.get();
				RE::TESObjectREFR* furniture = furniturePtr.get();
				const bool sitting = a_target->AsActorState()->IsSitting();
				if (furniture == nullptr && !sitting) return false;

				a_task.killFurniture = true;
				a_task.killFurnitureFormID = furniture ? furniture->GetFormID() : 0;
				const auto start = a_target->GetPosition();
				a_task.killWalkStartX = start.x;
				a_task.killWalkStartY = start.y;
				a_task.killWalkStartZ = start.z;

				if (a_standUpBeat) {
					// Let the get-up animation play before the state is
					// cleared and the walk starts.
					SendAnimEvent(a_target, "idleChairExitStart");
					a_task.furnitureStandUpReal = RealSeconds() + kFurnitureStandUpSeconds;
					logs::info("task {}: {} was on furniture - standing it up",
						a_task.id, a_task.targetName);
					return true;
				}
			}

			a_task.furnitureStandUpReal = 0.0f;
			// Force-stop the furniture idle first: the engine re-asserts it
			// otherwise (a leaning actor re-enters the lean).
			if (auto* proc = a_target->GetActorRuntimeData().currentProcess) {
				proc->StopCurrentIdle(a_target, true);
			}
			if (auto* proc = a_target->GetActorRuntimeData().currentProcess) {
				proc->ClearFurniture(a_target);
			}
			SendAnimEvent(a_target, "IdleForceDefaultState");
			(void)a_target->MoveToNearestNavmesh(kNavmeshSnap);

			if (movement::StartTravel(a_target, a_player, false, true)) {
				const auto pos = a_target->GetPosition();
				a_task.killWalkPending = true;
				a_task.killWalkHardCapReal = RealSeconds() + kKillWalkHardCapSeconds;
				a_task.killWalkLastProgressReal = RealSeconds();
				a_task.killWalkProgressX = pos.x;
				a_task.killWalkProgressY = pos.y;
				a_task.killWalkProgressZ = pos.z;
				a_task.killWalkRetargets = 0;
				a_task.killWalkEvalLeft = 2;
				a_task.killWalkEvalAtReal = RealSeconds() + 0.3f;
				a_task.killWalkStartReal = RealSeconds();
				a_task.killWalkPkgLogged = false;
				logs::info("task {}: {} was on furniture - walking it clear toward the player",
					a_task.id, a_task.targetName);
				return true;
			}

			// The movement backend is unavailable: fall back to the stand-up
			// beat and the settle fallback.
			logs::warn("IntelLite tasks: task {}: {} on furniture but the walk backend is unavailable",
				a_task.id, a_task.targetName);
			return false;
		}

		// Scene beat end: a furniture target that did not really leave its
		// furniture (still attached, or still at the seat) is cleared and
		// placed on open ground in front of the executor, so the paired idle
		// can play. Game thread only.
		void SettleFurnitureTarget(Task& a_task, RE::Actor* a_agent, RE::Actor* a_target)
		{
			if (!a_task.killFurniture || !a_agent || !a_target || a_target->IsDead()) return;

			const RE::ObjectRefHandle handle = a_target->GetOccupiedFurniture();
			auto furniturePtr = handle.get();
			RE::TESObjectREFR* furniture = furniturePtr.get();
			const bool stillAttached = (furniture != nullptr) || a_target->AsActorState()->IsSitting();

			bool haveRef = a_task.killFurnitureFormID != 0;
			float moved = 0.0f;
			if (auto* furn = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_task.killFurnitureFormID)) {
				moved = a_target->GetPosition().GetDistance(furn->GetPosition());
			} else {
				haveRef = false;
			}

			if (!stillAttached && haveRef && moved >= kFurnitureClearDistance) {
				logs::info("task {}: {} stood up from the furniture", a_task.id, a_task.targetName);
				return;
			}

			// Clear the engine-side furniture state, then place the target on
			// open ground just in front of the executor (the seat it was in
			// may sit inside geometry where no paired idle can play).
			if (auto* proc = a_target->GetActorRuntimeData().currentProcess) {
				proc->ClearFurniture(a_target);
			}
			const auto ap = a_agent->GetPosition();
			const float heading = a_agent->GetAngleZ();
			const RE::NiPoint3 spot{
				ap.x + std::sin(heading) * kKillTargetMoveDistance,
				ap.y + std::cos(heading) * kKillTargetMoveDistance,
				ap.z
			};
			a_target->SetPosition(spot, true);
			(void)a_target->MoveToNearestNavmesh(kNavmeshSnap);
			// Back to the blade: the target faces away from the executor, so
			// the killmove's behind-the-target placement lands between the pair.
			const auto tp = a_target->GetPosition();
			a_target->SetHeading(static_cast<float>(std::atan2(tp.x - ap.x, tp.y - ap.y)));
			SendAnimEvent(a_target, "IdleForceDefaultState");
			logs::info("task {}: {} did not leave the furniture - repositioned for the kill",
				a_task.id, a_task.targetName);
		}

		void AnnounceAssassinateSuccess(const Task& a_task)
		{
			RE::SendHUDMessage::ShowHUDMessage(
				(a_task.targetName + " has been assassinated.").c_str());
			// A persistent event naming the assassin: without it SkyrimNet's own
			// death event attributes the kill to the player. When both actors are
			// off-screen (Path A), include the player in the audience so SkyrimNet
			// stores the event instead of rejecting an all-unloaded pair.
			auto* agent  = RE::TESForm::LookupByID<RE::Actor>(a_task.agentFormID);
			auto* target = RE::TESForm::LookupByID<RE::Actor>(a_task.targetFormID);
			auto* player = RE::PlayerCharacter::GetSingleton();
			if ((!agent || !agent->Is3DLoaded()) && (!target || !target->Is3DLoaded()) && player) {
				skyrimnet::RegisterPersistentEvent(
					a_task.agentName + " assassinated " + a_task.targetName + ".",
					agent, target, { agent, target, player });
			} else {
				skyrimnet::RegisterPersistentEvent(
					a_task.agentName + " assassinated " + a_task.targetName + ".",
					agent, target);
			}
			// The contract is resolved: clear the per-actor state key so the
			// awareness submodule stops reporting it.
			ClearTaskStateKey(a_task);
			logs::info("task {}: {} assassinated {}",
				a_task.id, a_task.agentName, a_task.targetName);
		}

		// The staged failure (the killmove did not kill a non-essential target):
		// the target stopped the attempt; HUD + a SkyrimNet direct narration
		// (agent originator, player target; the staged target is nearby and
		// hears it); game thread only.
		void AnnounceAssassinateFailed(const Task& a_task)
		{
			auto* agent  = RE::TESForm::LookupByID<RE::Actor>(a_task.agentFormID);
			auto* player = RE::PlayerCharacter::GetSingleton();
			const std::string msg = a_task.agentName + " is back: " + a_task.targetName +
				" is still alive - the assassination failed.";
			RE::SendHUDMessage::ShowHUDMessage(msg.c_str());
			// A direct narration describing the attempt itself: the target
			// stopped it and, being nearby, hears the report.
			skyrimnet::DirectNarration(
				a_task.agentName + " tried to assassinate " + a_task.targetName +
					", but " + a_task.targetName + " managed to stop the attempt.",
				agent, player);
			ClearTaskStateKey(a_task);
			logs::info("task {}: assassination of {} failed - target survived",
				a_task.id, a_task.targetName);
		}

		// Build a world-knowledge condition expression from the target's public
		// factions: the hold crime faction (EditorID prefix "CrimeFaction", except
		// the "CrimeFactionNull" placeholder) and the town faction (EditorID
		// prefix "Town"), which is where a hold or city hears news. Returns ""
		// when the target has none. SkyrimNet's is_in_faction() takes EditorIDs.
		// Game thread only.
		std::string BuildPublicFactionCondition(const Task& a_task, RE::Actor* a_target)
		{
			if (!a_target) return {};
			auto* base = a_target->GetActorBase();
			if (!base) return {};
			std::string condition;
			std::uint32_t seen = 0;
			std::uint32_t matched = 0;
			std::uint32_t sampled = 0;
			std::string sample;
			for (const auto& fr : base->factions) {
				auto* faction = fr.faction;
				if (!faction) continue;
				++seen;
				// The editorid WITH the po3-Tweaks fallback: the game's own getter
				// returns an empty string for these factions in this environment
				// (the log shows the fallback resolving them).
				const std::string name = npc_index::FormEditorID(faction);
				if (sampled < 8) {
					std::string token = name.empty() ? std::string("<empty>") : ("<" + name + ">");
					if (token.size() > 32) token.resize(32);
					if (!sample.empty()) sample += ", ";
					sample += token;
					++sampled;
				}
				if (name.empty()) continue;
				const bool crime = name.rfind("CrimeFaction", 0) == 0 && name != "CrimeFactionNull";
				const bool town  = name.rfind("Town", 0) == 0;
				if (!crime && !town) continue;
				std::string escaped;
				escaped.reserve(name.size());
				for (char c : name) {
					if (c == '"' || c == '\\') continue;
					escaped.push_back(c);
				}
				if (escaped.empty()) continue;
				++matched;
				if (!condition.empty()) condition += " or ";
				condition += "is_in_faction(actorUUID, \"" + escaped + "\")";
			}
			if (condition.empty()) {
				logs::info("task {}: {} faction scan - {} seen, {} matched: {}",
					a_task.id, a_task.targetName, seen, matched, sample);
			}
			return condition;
		}

		// A far-away victim never saw the attacker: mark it as a survivor (the bio
		// submodule renders the awareness paragraph) and publish a rumor as world
		// knowledge scoped to the target's public factions. Game thread only.
		void RecordFarawaySurvivor(const Task& a_task)
		{
			auto* target = RE::TESForm::LookupByID<RE::Actor>(a_task.targetFormID);
			if (!target) {
				logs::warn("task {}: survivor state not written - {} did not resolve",
					a_task.id, a_task.targetName);
				return;
			}
			SetActorStateKey(target, kSurvivorStateKey, "1");
			skyrimnet::SetActorFloat(target, kSurvivorTimeKey, Now());
			logs::info("task {}: {} marked as an assassination survivor",
				a_task.id, a_task.targetName);
			const std::string condition = BuildPublicFactionCondition(a_task, target);
			if (condition.empty()) {
				logs::warn("task {}: no public factions for {} - rumor not published",
					a_task.id, a_task.targetName);
				return;
			}
			// SkyrimNet does not dedupe world knowledge, so the rumor is created
			// only once per victim: query the existing entries by EXACT display
			// name and skip the add when one is already present (the existing
			// entry is kept and no new copy is added). If the query is
			// unavailable the code falls through to the add.
			const std::string displayName = "Assassination attempt on " + a_task.targetName;
			const auto existing = skyrimnet::FindWorldKnowledgeByDisplayName(displayName);
			if (!existing.empty()) {
				logs::info("task {}: rumor for {} already published (world knowledge id {}) - not adding another",
					a_task.id, a_task.targetName, existing.front());
				return;
			}
			const int id = skyrimnet::AddWorldKnowledge(
				"Rumors tell that an unknown assassin tried to kill " + a_task.targetName + ", but failed.",
				condition, false, 0.8f, displayName);
			if (id > 0) {
				logs::info("task {}: rumor published to {}'s factions (world knowledge id {})",
					a_task.id, a_task.targetName, id);
			} else {
				logs::warn("task {}: world knowledge rumor for {} not added",
					a_task.id, a_task.targetName);
			}
		}

		// The assassination failed because the target is essential.
		// STAGED (a_farAway=false): the target stopped the attempt and is
		// nearby, so the narration IS its awareness - no state written.
		// FAR AWAY (a_farAway=true): the executor reports the failed attempt
		// to the player, and the target separately receives survivor awareness
		// + a world-knowledge rumor scoped to its public factions.
		// HUD + direct narration (agent originator, player target); game
		// thread only.
		void AnnounceAssassinateRefused(const Task& a_task, bool a_farAway)
		{
			auto* agent  = RE::TESForm::LookupByID<RE::Actor>(a_task.agentFormID);
			auto* player = RE::PlayerCharacter::GetSingleton();
			const std::string msg = a_task.agentName + " is back: " + a_task.targetName +
				" could not be killed.";
			RE::SendHUDMessage::ShowHUDMessage(msg.c_str());
			if (a_farAway) {
				skyrimnet::DirectNarration(
					a_task.agentName + " reports to " + PlayerName() + " that the assassination of " +
						a_task.targetName + " failed. " + a_task.agentName +
						" wasn't caught, but " + a_task.targetName + " still lives.",
					agent, player);
				RecordFarawaySurvivor(a_task);
			} else {
				skyrimnet::DirectNarration(
					a_task.agentName + " tried to assassinate " + a_task.targetName +
						", but " + a_task.targetName + " managed to stop the attempt.",
					agent, player);
			}
			ClearTaskStateKey(a_task);
			logs::info("task {}: assassination of {} failed - the target is essential",
				a_task.id, a_task.targetName);
		}

		// A visibly blocked outbound walk turns the executor back; it returns
		// and reports the failure as a narration to the player. Game thread
		// only.
		void AnnounceAssassinateUnreachable(const Task& a_task)
		{
			auto* agent  = RE::TESForm::LookupByID<RE::Actor>(a_task.agentFormID);
			auto* target = RE::TESForm::LookupByID<RE::Actor>(a_task.targetFormID);
			auto* cell   = target ? target->GetParentCell() : nullptr;
			const bool lockedBuilding = cell && cell->IsInteriorCell();

			std::string hud;
			if (lockedBuilding) {
				hud = a_task.agentName + " couldn't reach " + a_task.targetName +
					" - they're inside a locked building.";
			} else {
				hud = a_task.agentName + " couldn't reach " + a_task.targetName +
					" and is coming back.";
			}
			RE::SendHUDMessage::ShowHUDMessage(hud.c_str());

			// The executor has returned to report: a narration to the player
			// (the victim is never informed).
			auto* player = RE::PlayerCharacter::GetSingleton();
			skyrimnet::DirectNarration(
				lockedBuilding
					? a_task.agentName + " returns and reports to " + PlayerName() +
						" that " + a_task.targetName + " could not be reached - the building was locked."
					: a_task.agentName + " returns and reports to " + PlayerName() +
						" that " + a_task.targetName + " could not be reached.",
				agent, player);
			ClearTaskStateKey(a_task);
		}

		// Is this assassination committed (the kill scene has started)? A
		// committed one is not cancellable. Game thread only.
		bool AssassinationCommitted(const Task& a_task)
		{
			return a_task.type == Type::Assassinate && a_task.killCommitted;
		}

		// Resolve an assassination whose target was reached ON-SCREEN (the
		// outbound leg completed): a staged pair plays a scene (the same
		// killmove for a mortal and an essential target); an unstaged
		// ESSENTIAL target begins the failure return; otherwise a direct kill
		// runs. A "protected" target is killed normally. Game thread only.
		void ExecuteAssassination(Task& a_task, RE::Actor* agent, RE::Actor* target, RE::Actor* player)
		{
			if (!target) {
				CleanupKillDagger(a_task, agent);
				ReleaseSneak(a_task, agent);
				StopAllLegs(a_task);
				ClearTaskStateKey(a_task);
				logs::warn("IntelLite tasks: task {}: target unavailable", a_task.id);
				a_task.state = State::Failed;
				return;
			}
			a_task.furnitureDrivePending = false;

			// A staged pair plays a scene for a short beat: both mortal and
			// essential targets get the same paired killmove (the dagger makes
			// it safe). A mortal target is then guaranteed dead; an essential
			// one is left to the engine (expected bleedout) and reports the
			// failure narration.
			const bool staged = agent && target && agent->Is3DLoaded() && target->Is3DLoaded() &&
				SameSpace(agent, target) &&
				agent->GetPosition().GetDistance(target->GetPosition()) < 400.0f;
			if (!staged) {
				auto* agentCell  = agent  ? agent->GetParentCell()  : nullptr;
				auto* targetCell = target ? target->GetParentCell() : nullptr;
				logs::info("task {}: arrival not staged - agent3d={} target3d={} sameSpace={} dist={:.0f} agentCell={:08X} targetCell={:08X}",
					a_task.id,
					agent && agent->Is3DLoaded(), target && target->Is3DLoaded(),
					SameSpace(agent, target),
					(agent && target) ? agent->GetPosition().GetDistance(target->GetPosition()) : -1.0f,
					agentCell ? agentCell->GetFormID() : 0u,
					targetCell ? targetCell->GetFormID() : 0u);
			}
			if (staged) {
				BeginBountyGuard();
				// Stop the executor's residual movement so the kill scene does
				// not fight an in-flight travel package. Actor::StopMoving(float).
				agent->StopMoving(0.0f);
				logs::info("task {}: {} closes in on {}",
					a_task.id, a_task.agentName, a_task.targetName);
				// An essential target can never die: hand the swing package
				// over the MOMENT the executor arrives (no furniture drive, no
				// restraint hold, no stand-up/settle beats) and release when
				// the attack has actually been observed to complete. Mortal
				// targets keep the staged killmove tail below.
				if (IsTargetEssential(target)) {
					if (a_task.killWalkPending) {
						movement::StopTravel(target);
						a_task.killWalkPending = false;
					}
					a_task.furnitureStandUpReal = 0.0f;
					ReleaseTargetHold(a_task, target);
					const float dist = agent->GetPosition().GetDistance(target->GetPosition());
					// The dagger may have been sheathed during the run: re-ensure
					// it drawn before the package takes over (idempotent - it
					// reuses the dagger already spawned).
					EnsureKillDagger(a_task, agent);
					if (movement::StartSwing(agent, target)) {
						logs::info("task {}: {} is essential - swinging once (no killmove, {:.0f}u)", a_task.id, a_task.targetName, dist);
					} else {
						logs::warn("task {}: swing package unavailable - essential swing skipped for {}", a_task.id, a_task.targetName);
					}
					a_task.killCommitted = true;
					a_task.swingPending = true;
					a_task.swingSawAttack = false;
					a_task.swingAttackSeenReal = 0.0f;
					a_task.swingCheckReal = 0.0f;
					a_task.swingEndReal = RealSeconds() + kSwingSeconds;
					a_task.swingReissues = 0;
					a_task.swingReissueReal = RealSeconds() + kSwingReissueSeconds;
					a_task.state = State::Executing;
					return;
				}
				// The walk cases (a furniture-driven target or a stationary
				// one) get the executor's close-in attempts FIRST: the target
				// stands where it is while the pair is staged and the killmove
				// is fired up to twice; only when the engine refuses those
				// does the target get sent walking toward the player and the
				// kill retried on the move. A target already walking is NOT
				// redirected - it is stopped and held where it stands.
				if (!a_task.killWalkPending) {
					(void)DriveTargetOffFurniture(a_task, target, player, false);
				}
				const bool walkCase = a_task.killWalkPending || !target->IsMoving();
				if (walkCase) {
					// Drop any furniture-drive walk: the target stands where
					// the drive left it for the close-in attempts.
					movement::StopTravel(target);
					a_task.killWalkPending = false;
					a_task.killWalkAfterAttempts = true;
					a_task.killEarlyAttemptsLeft = 1;
				}
				// Hold the target from the moment the executor is upon it:
				// movement AND actions (the engine's restraint), so a farm
				// loop, a lean, or a walk back to its routine cannot re-assert
				// through the staging window that follows.
				HoldTargetForKill(a_task, target);
				SendAnimEvent(target, "IdleForceDefaultState");
				a_task.killPending = true;
				a_task.killCommitted = true;
				a_task.killAttempted = false;
				a_task.killBeatEndReal = RealSeconds() + kKillStandUpSeconds;
				a_task.state = State::Executing;
				return;
			}

			// Not staged (the pair shares an interior cell but is not
			// 3D-loaded): only an ESSENTIAL target is refused up front
			// (protected targets are killed normally).
			if (a_task.killWalkPending) {
				movement::StopTravel(target);
				a_task.killWalkPending = false;
			}
			a_task.furnitureStandUpReal = 0.0f;
			a_task.furnitureDrivePending = false; // symmetry; already cleared above
			if (IsTargetEssential(target)) {
				CleanupKillDagger(a_task, agent);
				ReleaseSneak(a_task, agent);
				logs::info("task {}: {} cannot kill {} - the target is essential",
					a_task.id, a_task.agentName, a_task.targetName);
				StartLeg(a_task.agentReturnLeg, agent, player);
				a_task.refusalFarAway = !target->Is3DLoaded();
				a_task.targetArrived = true;
				a_task.agentDone = false;
				a_task.state = State::TargetTravel;
				return;
			}

			// Far / unloaded and mortal: attempt the kill and verify it landed.
			CleanupKillDagger(a_task, agent);
			ReleaseSneak(a_task, agent);
			const bool victimLoaded = target->Is3DLoaded();
			logs::info("task {}: direct kill of {} (victim3d={}, attribution={})",
				a_task.id, a_task.targetName, victimLoaded,
				victimLoaded ? "attacker=agent" : "unattributed (nullptr)");
			KillTarget(target, victimLoaded ? agent : nullptr);
			if (!target->IsDead()) {
				logs::warn("task {}: direct kill of {} did not land (still alive; essential(runtime)={} essential(base)={} essential(alias)={} protected(runtime)={}) - treating as a refusal",
					a_task.id, a_task.targetName,
					target->IsEssential(), BaseIsEssential(target), AliasIsEssential(target), target->IsProtected());
				StartLeg(a_task.agentReturnLeg, agent, player);
				a_task.refusalFarAway = !target->Is3DLoaded();
				a_task.targetArrived = true;
				a_task.agentDone = false;
				a_task.state = State::TargetTravel;
				return;
			}
			StopAllLegs(a_task);
			AnnounceAssassinateSuccess(a_task);
			a_task.state = State::Done;
		}

		// The outbound leg completed on-screen (both 3D-loaded in the same
		// space within kKillArriveDistance). An already-dead target is
		// success; otherwise the target is resolved. Game thread only.
		void HandleAssassinateArrival(Task& a_task, RE::Actor* agent, RE::Actor* target, RE::Actor* player)
		{
			if (target && target->IsDead()) {
				CleanupKillDagger(a_task, agent);
				ReleaseSneak(a_task, agent);
				StopAllLegs(a_task);
				AnnounceAssassinateSuccess(a_task);
				a_task.state = State::Done;
				return;
			}
			ExecuteAssassination(a_task, agent, target, player);
		}

		// The outbound leg ended off-screen (simulated, or at its game-time
		// deadline): the simulation IS the trip - no teleport, no staged
		// scene. A mortal target is killed off-screen and the executor is
		// released back to its normal AI; an essential one turns the executor
		// around to report. Game thread only.
		void HandleAssassinateOffscreen(Task& a_task, RE::Actor* agent, RE::Actor* target, RE::Actor* player)
		{
			if (!target) {
				StopAllLegs(a_task);
				ClearTaskStateKey(a_task);
				logs::warn("IntelLite tasks: task {}: target unavailable", a_task.id);
				a_task.state = State::Failed;
				return;
			}
			if (a_task.killWalkPending) {
				movement::StopTravel(target);
				a_task.killWalkPending = false;
			}
			a_task.furnitureStandUpReal = 0.0f;
			a_task.furnitureDrivePending = false;
			if (target->IsDead()) {
				StopAllLegs(a_task);
				CleanupKillDagger(a_task, agent);
				ReleaseSneak(a_task, agent);
				AnnounceAssassinateSuccess(a_task);
				a_task.state = State::Done;
				return;
			}

			logs::info("task {}: {} reached {} off-screen (simulated)",
				a_task.id, a_task.agentName, a_task.targetName);
			logs::info("task {}: off-screen flags - {} essential(runtime)={} essential(base)={} essential(alias)={} protected(runtime)={} protected(base)={} 3d={}",
				a_task.id, a_task.targetName,
				target->IsEssential(), BaseIsEssential(target), AliasIsEssential(target),
				target->IsProtected(), BaseIsProtected(target),
				target->Is3DLoaded());
			CleanupKillDagger(a_task, agent);
			ReleaseSneak(a_task, agent);

			if (IsTargetEssential(target)) {
				logs::info("task {}: {} cannot kill {} - the target is essential",
					a_task.id, a_task.agentName, a_task.targetName);
				StartLeg(a_task.agentReturnLeg, agent, player);
				a_task.refusalFarAway = true;
				a_task.targetArrived = true;
				a_task.agentDone = false;
				a_task.state = State::TargetTravel;
				return;
			}

			// Mortal: attempt the kill; verify it actually landed. The engine
			// refuses to kill essential/protected actors, so a target that is
			// still alive afterwards is treated as a refusal (never a success).
			const bool victimLoaded = target->Is3DLoaded();
			logs::info("task {}: off-screen kill of {} (victim3d={}, attribution={})",
				a_task.id, a_task.targetName, victimLoaded,
				victimLoaded ? "attacker=agent" : "unattributed (nullptr)");
			KillTarget(target, victimLoaded ? agent : nullptr);
			if (!target->IsDead()) {
				logs::warn("task {}: off-screen kill of {} did not land (still alive; essential(runtime)={} essential(base)={} essential(alias)={} protected(runtime)={}) - treating as a refusal",
					a_task.id, a_task.targetName,
					target->IsEssential(), BaseIsEssential(target), AliasIsEssential(target), target->IsProtected());
				StartLeg(a_task.agentReturnLeg, agent, player);
				a_task.refusalFarAway = true;
				a_task.targetArrived = true;
				a_task.agentDone = false;
				a_task.state = State::TargetTravel;
				return;
			}
			StopAllLegs(a_task);
			AnnounceAssassinateSuccess(a_task);
			a_task.state = State::Done;
		}

		// Deliver a Deliver task's message: a SkyrimNet persistent event plus a
		// HUD line, fired when the agent reaches the recipient. Game thread
		// only.
		void DeliverMessage(const Task& a_task, RE::Actor* agent)
		{
			auto* recipient = RE::TESForm::LookupByID<RE::Actor>(a_task.targetFormID);
			if (a_task.sender != a_task.agentName) {
				skyrimnet::RegisterPersistentEvent(
					a_task.agentName + " delivers a message from " + a_task.sender + " to " +
						a_task.targetName + ": " + a_task.message,
					agent, recipient);
			} else {
				skyrimnet::RegisterPersistentEvent(
					a_task.agentName + " delivers a message to " + a_task.targetName + ": " + a_task.message,
					agent, recipient);
			}
			// Store the message on the recipient so the character-bio submodule can
			// surface it in the recipient's prompt ("## Messages for you"). The
			// sender is the message's original author; the courier key is set only
			// when someone other than the author carried it.
			if (recipient) {
				skyrimnet::SetActorState(recipient, "IntelLite_MessageText", a_task.message);
				skyrimnet::SetActorState(recipient, "IntelLite_MessageFrom", a_task.sender);
				skyrimnet::SetActorState(recipient, "IntelLite_MessageVia",
					(a_task.sender != a_task.agentName) ? a_task.agentName : std::string());
				skyrimnet::SetActorFloat(recipient, "IntelLite_MessageTime", Now());
				// Append to the recipient's message history lists: the character-bio
				// submodule renders every entry within 14 game days (newest first).
				// The single keys above stay updated as a fallback for pre-list saves.
				skyrimnet::AppendActorStringList(recipient, "IntelLite_MsgText", a_task.message);
				skyrimnet::AppendActorStringList(recipient, "IntelLite_MsgFrom", a_task.sender);
				skyrimnet::AppendActorStringList(recipient, "IntelLite_MsgVia",
					(a_task.sender != a_task.agentName) ? a_task.agentName : std::string());
				skyrimnet::AppendActorFloatList(recipient, "IntelLite_MsgTime", Now());
			}
			// Mirror the event above (and AddDeliver's start line): a distinct sender is named.
			if (a_task.sender != a_task.agentName) {
				RE::SendHUDMessage::ShowHUDMessage(
					(a_task.agentName + " delivers " + a_task.sender + "'s message to " +
						a_task.targetName + ".").c_str());
			} else {
				RE::SendHUDMessage::ShowHUDMessage(
					(a_task.agentName + " delivers a message to " + a_task.targetName + ".").c_str());
			}
			logs::info("task {}: {} delivered a message to {}",
				a_task.id, a_task.agentName, a_task.targetName);
		}

		void MarkDone(const Task& a_task)
		{
			// The player-facing notification already fired when the target
			// arrived; completing the task only logs.
			logs::info("task {}: done", a_task.id);
		}

		void MarkFailed(const Task& a_task, const char* a_reason)
		{
			logs::info("task {}: failed - {}", a_task.id, a_reason);
			ClearTaskStateKey(a_task);
			AnnounceFetchFailed(a_task);
		}

		// Fetch: the target has arrived at the player. Secure it immediately with
		// the linger sandbox - until now the only thing holding it was SkyrimNet's
		// brief dialogue package, which clears after a few seconds and lets the
		// target walk off (and unload). Announce the delivery, and when the agent
		// is still far away or in another space, pull the agent in with the target
		// so the pair is delivered together instead of leaving the task in the
		// return phase for minutes. Game thread only.
		void OnTargetArrivedFetch(Task& a_task, RE::Actor* a_agent, RE::Actor* a_target,
			RE::Actor* a_player, bool a_targetTraveled = true)
		{
			a_task.targetArrived = true;
			if (a_task.type != Type::Fetch) return;

			if (a_target && a_player && !a_task.targetSandboxed) {
				if (movement::StartSandbox(a_target, a_player)) {
					a_task.targetSandboxed = true;
					logs::info("task {}: target {} sandboxed on arrival", a_task.id, a_task.targetName);
				}
			}
			if (a_target && a_player) {
				AnnounceFetchDone(a_task);
			}

			// When the target arrives from actual travel and the executor is not
			// already walking right behind it in the same space, pull the
			// executor in behind the target so the pair always arrives together.
			if (a_targetTraveled && !a_task.agentDone && a_agent && a_player) {
				const float toPlayer = DistanceToPlayer(a_agent, a_player);
				const bool stalled =
					RealSeconds() - a_task.agentReturnLeg.lastProgressReal > 3.0f;
				if (!a_task.agentReturnLeg.active || !a_agent->Is3DLoaded() ||
					toPlayer > kPairStageDistance || (toPlayer > 800.0f && stalled)) {
					ReleaseAgentOffscreenHold(a_task, a_agent);
					StopLeg(a_task.agentReturnLeg);
					movement::StopTravel(a_agent);
					a_task.agentDone = true;
					// Anchor the pull on the TARGET (which just walked in through a
					// door), not the player, so the executor comes from the same
					// direction instead of appearing behind the player.
					TeleportBehind(a_agent, a_target ? a_target : a_player, 450.0f);
					logs::info("task {}: agent pulled in with the target", a_task.id);
				}
			}
		}

		// Enter the return phase: the agent escorts the target back to the
		// player. When the target is already in the player's loaded area, both
		// legs walk back together immediately; when the target is off-screen,
		// the target leg drives the off-screen deadline and the agent waits in
		// lockstep to join the target at the return staging point so they
		// always arrive together. Game thread only.
		void BeginReturn(Task& a_task)
		{
			a_task.state = State::TargetTravel;
			a_task.targetArrived = false;
			a_task.agentDone = false;

			auto* agent  = RE::TESForm::LookupByID<RE::Actor>(a_task.agentFormID);
			auto* target = RE::TESForm::LookupByID<RE::Actor>(a_task.targetFormID);
			auto* player = RE::PlayerCharacter::GetSingleton();

			if (a_task.type == Type::Fetch && agent && target) {
				skyrimnet::RegisterPersistentEvent(
					a_task.agentName + " told " + a_task.targetName + " that " +
						PlayerName() + " is looking for them.",
					agent, target);
			}

			// --- the target's return leg: already with the player means done;
			// otherwise walk it back, or fall back to the simulated recovery. ---
			if (!target || !player) {
				ReleaseAgentOffscreenHold(a_task, agent);
				StartLeg(a_task.agentReturnLeg, agent, player);
				a_task.agentReturnLeg.strictArrival = true;
				OnTargetArrivedFetch(a_task, agent, target, player, false);
				return;
			}

			const float toPlayer = SameSpace(target, player)
				? target->GetPosition().GetDistance(player->GetPosition())
				: 1e9f;
			if (toPlayer < kArriveDistance) {
				ReleaseAgentOffscreenHold(a_task, agent);
				StartLeg(a_task.agentReturnLeg, agent, player);
				a_task.agentReturnLeg.strictArrival = true;
				logs::info("task {}: target {} is already with you", a_task.id, a_task.targetName);
				OnTargetArrivedFetch(a_task, agent, target, player, false);
				return;
			}

			StartLeg(a_task.targetLeg, target, player);
			a_task.targetLeg.strictArrival = true;
			// Keep the serialized target-leg deadline in sync (the co-save
			// layout is unchanged; a resumed return reads this back).
			a_task.dueGameTime = a_task.targetLeg.dueGameTime;

			if (IsRefInLoadedArea(target)) {
				// Target is in the loaded area: both walk back together now.
				ReleaseAgentOffscreenHold(a_task, agent);
				StartLeg(a_task.agentReturnLeg, agent, player);
				a_task.agentReturnLeg.strictArrival = true;
			} else {
				// Target is off-screen: keep the executor's outbound travel
				// override (priority 100 toward the target) untouched so a
				// follower's vanilla Follow-Player package cannot take over and
				// walk it back to the player in High Process, and freeze an
				// unloaded executor in place with SetDontMove until the target
				// stages into the player's area (JoinAtStagingPoint).
				if (agent && !agent->Is3DLoaded()) {
					HoldAgentOffscreen(a_task, agent);
				}
				a_task.agentReturnLeg = Leg{};
				a_task.agentReturnLeg.simulated = true;
				a_task.agentReturnLeg.offscreen = true;
				a_task.agentReturnLeg.strictArrival = true;
				a_task.agentReturnLeg.moverFormID = agent ? agent->GetFormID() : 0;
				a_task.agentReturnLeg.destFormID = player->GetFormID();
				a_task.agentReturnLeg.startGameTime = a_task.targetLeg.startGameTime;
				a_task.agentReturnLeg.dueGameTime = a_task.targetLeg.dueGameTime;
				a_task.agentReturnLeg.destInterior = a_task.targetLeg.destInterior;
				a_task.agentReturnLeg.destCellFormID = a_task.targetLeg.destCellFormID;
				a_task.agentReturnLeg.destWorldspaceFormID = a_task.targetLeg.destWorldspaceFormID;
			}
		}

		// Complete a successful deliver: the message is already stored on the
		// recipient; the executor is released where it stands and resumes its own
		// AI (a follower follows the player again; anyone else returns to their
		// routine). There is no return leg and no "returns after delivering" event.
		void FinishDeliver(Task& a_task, RE::Actor* a_agent)
		{
			if (a_agent) {
				movement::StopTravel(a_agent);
			}
			logs::info("task {}: {} released after the delivery", a_task.id, a_task.agentName);
			a_task.state = State::Done;
			MarkDone(a_task);
		}

		// Advance a Meet task still in the scheduling phase: once its departure
		// time arrives the agent sets off for the meeting place. Returns true if
		// the state changed. Game thread only.
		bool AdvancePending(Task& a_task)
		{
			if (a_task.type != Type::Meet) return false;

			auto* agent = RE::TESForm::LookupByID<RE::Actor>(a_task.agentFormID);
			auto* dest  = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_task.locationFormID);
			if (!agent || !dest) {
				logs::info("task {}: meet failed - meeting place unavailable", a_task.id);
				ClearTaskStateKey(a_task);
				a_task.state = State::Failed;
				return true;
			}

			// A named place inside a building walks to its exterior door first.
			auto* walkRef = a_task.placeWalkRefFormID
				? RE::TESForm::LookupByID<RE::TESObjectREFR>(a_task.placeWalkRefFormID)
				: nullptr;
			if (!walkRef && !dest->GetWorldspace()) {
				walkRef = FindExitDoor(dest, nullptr);
				if (walkRef) {
					a_task.placeWalkRefFormID = walkRef->GetFormID();
				}
			}

			// Recompute the departure from the CURRENT positions, but only when its
			// inputs can have changed: a 2 s throttle plus an immediate recompute
			// when the agent or the destination changes cell (a player who moved
			// after agreeing is still respected). This keeps a long pending meet
			// from paying a cross-frame estimate - and a possible fallback log -
			// on every 250 ms tick.
			const std::uint32_t agentCell =
				agent->GetParentCell() ? agent->GetParentCell()->GetFormID() : 0;
			const std::uint32_t destCell =
				dest->GetParentCell() ? dest->GetParentCell()->GetFormID() : 0;
			const bool cellChanged =
				agentCell != a_task.estimateAgentCellFormID ||
				destCell != a_task.estimateDestCellFormID;
			if (RealSeconds() >= a_task.departEstimateRefreshReal || cellChanged) {
				const auto est = EstimateHoursEx(agent, dest, !a_task.estimateFallbackLogged, cellChanged);
				a_task.departEstimateHours = est.hours;
				if (est.fellBack) a_task.estimateFallbackLogged = true;
				a_task.estimateAgentCellFormID = agentCell;
				a_task.estimateDestCellFormID = destCell;
				a_task.departEstimateRefreshReal = RealSeconds() + kDepartEstimateRefreshSeconds;
			}
			const float now = Now();
			const float depart =
				a_task.meetGameTime - HoursToDays(a_task.departEstimateHours);
			a_task.departGameTime = (depart > now) ? depart : now;
			if (now < a_task.departGameTime) return false;

			StartLeg(a_task.agentLeg, agent, dest, walkRef);
			a_task.agentLeg.strictArrival = true;
			a_task.dueGameTime = a_task.agentLeg.dueGameTime;
			a_task.state = State::AgentTravel;
			logs::info("task {}: {} sets off to meet at {}",
				a_task.id, a_task.agentName, a_task.targetName);
			return true;
		}

		// The staged assassination scene: a stand-up beat (a furniture target
		// is walked clear instead), the dagger, the paired killmove (a mortal
		// target is expected to be killed by the animation; an essential one
		// is expected to bleed out and survive), a one-shot retry, a
		// confirmation window, a wait for the animation to release the pair,
		// then the outcome.
		// Returns true if the state changed. Game thread only.
		bool AdvanceAssassinateScene(Task& a_task)
		{
			auto* agent  = RE::TESForm::LookupByID<RE::Actor>(a_task.agentFormID);
			auto* target = RE::TESForm::LookupByID<RE::Actor>(a_task.targetFormID);
			auto* player = RE::PlayerCharacter::GetSingleton();

			// Keep a non-essential target's Protected flags cleared on every
			// scene tick (including while the killmove animation is in flight)
			// so the fatal hit inside the paired animation never turns into a
			// bleedout.
			if (target && !target->IsDead() && !IsTargetEssential(target) &&
				IsTargetProtected(target)) {
				target->GetActorRuntimeData().boolFlags.reset(RE::Actor::BOOL_FLAGS::kProtected);
				if (!a_task.protectedCleared) {
					a_task.protectedCleared = true;
					a_task.targetBaseProtected = BaseIsProtected(target);
					a_task.targetAliasProtected = AliasIsProtected(target);
				}
				SetBaseProtected(target, false);
				SetAliasProtected(target, false);
			}

			// The anti-routine pulse: while the scene stages, keep the target
			// out of its furniture/idle routine and out of a walk-away. It is
			// suspended while a dispatch is outstanding (killWaitingEngage)
			// and after an engagement, so a just-accepted killmove is never
			// touched. During the walk phase only the furniture check runs
			// (the walk itself is that phase's job). It is also suspended for
			// the whole one-package essential run: the swing package is already
			// inbound, so the target must not be StopMoving/StopCurrentIdle'd
			// every tick.
			if (target && a_task.killCommitted && !a_task.killWaitingEngage &&
				!a_task.killEngaged && !a_task.swingPending && !target->IsInKillMove()) {
				PulseTargetHold(a_task, target, !a_task.killWalkPending);
			}

			// Essential-target swing: a killmove on an essential target can
			// never land and leaves it in a broken state, so it is replaced by
			// one cosmetic swing (the swing package). The release fires when
			// the attack has actually been observed to complete - not on a
			// blind timer - and the hard cap only bounds a package that never
			// attacks. Then release everything and take the verdict exactly as
			// the end of the scene does.
			if (a_task.swingPending) {
				if (agent && target && SameSpace(agent, target) &&
					agent->GetPosition().GetDistance(target->GetPosition()) < 400.0f) {
					a_task.killCommitted = true;
				}
				if (agent && agent->IsAttacking()) {
					a_task.swingSawAttack = true;
					a_task.killCommitted = true;
					BeginBountyGuard();
					if (a_task.swingAttackSeenReal == 0.0f) a_task.swingAttackSeenReal = RealSeconds();
				}
				// The override can sit queued for seconds while the executor
				// finishes another package, so while the swing package is not
				// yet the running package re-assert it. Bounded by
				// kSwingMaxReissues so a package that never engages gives up.
				const bool engaged = agent && RunningPackageOf(agent).form == movement::SwingPackageFormID();
				if (!engaged && !a_task.swingSawAttack && agent &&
					a_task.swingReissues < kSwingMaxReissues && RealSeconds() >= a_task.swingReissueReal) {
					++a_task.swingReissues;
					a_task.swingReissueReal = RealSeconds() + kSwingReissueSeconds;
					BeginBountyGuard();
					if (movement::StartSwing(agent, target)) {
						logs::info("task {}: swing not engaged yet - re-issued (attempt {})", a_task.id, a_task.swingReissues);
					}
				}
				// Rate-limited (~1 s) diagnostic: exactly what the package is
				// doing during the swing window, so the next log explains a
				// missed attack (distance, running package, attacking/moving/
				// weapon-drawn state).
				if (agent && RealSeconds() >= a_task.swingCheckReal) {
					a_task.swingCheckReal = RealSeconds() + 1.0f;
					const float dist = target ? agent->GetPosition().GetDistance(target->GetPosition()) : -1.0f;
					logs::info("task {}: swing check - dist {:.0f}u pkg {:08X} attacking {} moving {} drawn {}",
						a_task.id, dist, RunningPackageOf(agent).form, agent->IsAttacking(), agent->IsMoving(),
						agent->AsActorState()->IsWeaponDrawn());
				}
				const bool attackDone = a_task.swingSawAttack && (!agent || !agent->IsAttacking() || (a_task.swingAttackSeenReal > 0.0f && RealSeconds() >= a_task.swingAttackSeenReal + kSwingAttackHoldSeconds));
				if (!attackDone && RealSeconds() < a_task.swingEndReal) return false;
				a_task.swingPending = false;
				movement::StopSwing(agent);
				StopAllLegs(a_task);
				CleanupKillDagger(a_task, agent);
				ReleaseSneak(a_task, agent);
				ReleaseTargetHold(a_task, target);
				if (a_task.swingSawAttack) {
					logs::info("task {}: essential swing attack finished - releasing {}", a_task.id, a_task.targetName);
				} else {
					logs::warn("task {}: no attack seen within the swing window - releasing {}", a_task.id, a_task.targetName);
					// If the executor never even reached the target within the
					// swing window (e.g. blocked by a locked door or obstacle),
					// route to the unreachable return rather than narrating that
					// the target stopped the attack.
					const bool nearTarget = agent && target && SameSpace(agent, target) &&
						agent->GetPosition().GetDistance(target->GetPosition()) <= 400.0f;
					if (!nearTarget) {
						a_task.killCommitted = false;
						a_task.outboundFailed = true;
						StartLeg(a_task.agentReturnLeg, agent, player);
						a_task.state = State::AgentTravel;
						logs::info("task {}: {} could not reach {} within the swing window - returning to report",
							a_task.id, a_task.agentName, a_task.targetName);
						return true;
					}
				}
				if (target && !target->IsDead()) {
					AnnounceAssassinateRefused(a_task, false);   // essential target - the staged target stopped the attempt
				} else {
					AnnounceAssassinateSuccess(a_task);
				}
				a_task.state = State::Done;
				return true;
			}

			// Phase 1a (furniture walk): the target is walking clear of its
			// furniture. Take the kill once it has really moved; a target
			// that never leaves falls back to the clear-and-reposition path.
			if (a_task.killWalkPending) {
				float moved = 0.0f;
				if (target) {
					const auto p = target->GetPosition();
					const float dx = p.x - a_task.killWalkStartX;
					const float dy = p.y - a_task.killWalkStartY;
					const float dz = p.z - a_task.killWalkStartZ;
					moved = std::sqrt(dx * dx + dy * dy + dz * dz);
					// Any real movement restarts the no-progress clock, so a
					// target that keeps walking is never cut off.
					const float px = p.x - a_task.killWalkProgressX;
					const float py = p.y - a_task.killWalkProgressY;
					const float pz = p.z - a_task.killWalkProgressZ;
					if (std::sqrt(px * px + py * py + pz * pz) > kKillWalkProgressDistance) {
						a_task.killWalkProgressX = p.x;
						a_task.killWalkProgressY = p.y;
						a_task.killWalkProgressZ = p.z;
						a_task.killWalkLastProgressReal = RealSeconds();
					}
				}
				// Fetch-style re-evaluation: StartLeg re-evaluates a walker
				// twice after applying the package (0.3 s, then +1.2 s);
				// mirror that so a routine-driven target actually takes the
				// walk override.
				if (target && a_task.killWalkEvalLeft > 0 && RealSeconds() >= a_task.killWalkEvalAtReal) {
					target->EvaluatePackage(true, false);
					--a_task.killWalkEvalLeft;
					a_task.killWalkEvalAtReal = (a_task.killWalkEvalLeft > 0) ? RealSeconds() + 1.2f : 0.0f;
				}
				if (target && !a_task.killWalkPkgLogged && a_task.killWalkStartReal > 0.0f &&
					RealSeconds() - a_task.killWalkStartReal >= 1.0f) {
					a_task.killWalkPkgLogged = true;
					const auto pkg = RunningPackageOf(target);
					const float toPlayer = SameSpace(target, player)
						? target->GetPosition().GetDistance(player->GetPosition()) : -1.0f;
					logs::info("task {}: {} walk check - pkg {:08X}/{} moved {:.0f}u dist {:.0f}u",
						a_task.id, a_task.targetName, pkg.form, pkg.id, moved, toPlayer);
				}
				const bool stalled =
					RealSeconds() - a_task.killWalkLastProgressReal >= kKillWalkStallSeconds;
				const bool capped = RealSeconds() >= a_task.killWalkHardCapReal;
				if (target && moved >= kKillWalkMinDistance) {
					logs::info("task {}: {} walked clear - taking the kill",
						a_task.id, a_task.targetName);
				} else if (!stalled && !capped) {
					return false;   // still walking - keep waiting
				} else {
					if (capped) {
						logs::info("task {}: {} walk hit the hard cap", a_task.id, a_task.targetName);
					} else {
						const float toPlayer = SameSpace(target, player)
							? target->GetPosition().GetDistance(player->GetPosition()) : -1.0f;
						logs::info("task {}: {} walk made no progress for {:.0f}s (dist {:.0f}u)",
							a_task.id, a_task.targetName,
							RealSeconds() - a_task.killWalkLastProgressReal, toPlayer);
					}
					bool attached = false;
					if (target) {
						const RE::ObjectRefHandle handle = target->GetOccupiedFurniture();
						auto furniturePtr = handle.get();
						attached = furniturePtr.get() != nullptr ||
							target->AsActorState()->IsSitting();
					}
					// Fetch-style re-issue: a stalled walker gets the package
					// re-applied (the leg's retarget) before the kill falls
					// back to where it stands.
					if (!attached && !capped && target && player &&
						a_task.killWalkRetargets < 2) {
						++a_task.killWalkRetargets;
						if (movement::StartTravel(target, player, false, true)) {
							a_task.killWalkLastProgressReal = RealSeconds();
							a_task.killWalkEvalLeft = 2;
							a_task.killWalkEvalAtReal = RealSeconds() + 0.3f;
							logs::info("task {}: {} walk re-issued (attempt {})",
								a_task.id, a_task.targetName, a_task.killWalkRetargets);
							return false;
						}
					}
					if (attached) {
						logs::info("task {}: {} did not leave the furniture - settling it",
							a_task.id, a_task.targetName);
						SettleFurnitureTarget(a_task, agent, target);
					} else {
						logs::info("task {}: {} stopped walking - taking the kill where it stands",
							a_task.id, a_task.targetName);
					}
				}
				// The furniture handling is done; route into the same stand-up
				// beat the normal path uses, so the target settles before the
				// killmove (dispatching mid-walk does not play).
				a_task.killWalkPending = false;
				a_task.killFurniture = false;
				a_task.killFurnitureFormID = 0;
				if (target) {
					movement::StopTravel(target);
					HoldTargetForKill(a_task, target);
				}
				SendAnimEvent(target, "IdleForceDefaultState");
				a_task.killPending = true;
				a_task.killBeatEndReal = RealSeconds() + kKillStandUpSeconds;
				return false;
			}

			// Phase 1 (stand-up beat): the target was just told to reset its
			// idle (or to leave its furniture). Give it that beat, settle a
			// furniture target onto open ground, then stage the pair (idle
			// reset, executor behind the victim) and start the settle beat.
			if (a_task.killPending) {
				if (RealSeconds() < a_task.killBeatEndReal) return false;
				a_task.killPending = false;
				SettleFurnitureTarget(a_task, agent, target);
				EnsureKillDagger(a_task, agent);
				if (agent && target && !target->IsDead()) {
					KillMovePrepare(a_task, agent, target);
					a_task.killPrepareEndReal = RealSeconds() + kKillSettleSeconds;
				} else {
					a_task.killPlayed = false;
					a_task.killWaitingEngage = false;
					a_task.killPrepareEndReal = 0.0f;
					a_task.killBeatEndReal = 0.0f;
					a_task.killAttempted = true;
					a_task.killConfirmEndReal = 0.0f;
				}
				return false;
			}

			// Phase 1b (dispatch): the pair has settled. Re-stage once when the
			// settle drifted them apart, otherwise snapshot the state and fire
			// the paired idle, then wait for the engine's killmove state.
			if (a_task.killPrepareEndReal != 0.0f) {
				if (RealSeconds() < a_task.killPrepareEndReal) return false;
				a_task.killPrepareEndReal = 0.0f;
				if (agent && target && !target->IsDead()) {
					const float dist = agent->GetPosition().GetDistance(target->GetPosition());
					// An essential target can never die, and the killmove
					// leaves it in a broken state: replace it with ONE cosmetic
					// swing (the swing package), then release + narrate on the
					// swing beat. The branch always sets swingPending, so the
					// tick routes into the early block and never into the
					// killmove pipeline.
					if (IsTargetEssential(target)) {
						ReleaseTargetHold(a_task, target);
						if (movement::StartSwing(agent, target)) {
							logs::info("task {}: {} is essential - swinging once (no killmove, {:.0f}u)",
								a_task.id, a_task.targetName, dist);
						} else {
							logs::warn("task {}: swing package unavailable - essential swing skipped for {}",
								a_task.id, a_task.targetName);
						}
						a_task.swingPending = true;
						a_task.swingSawAttack = false;
						a_task.swingAttackSeenReal = 0.0f;
						a_task.swingCheckReal = 0.0f;
						a_task.swingEndReal = RealSeconds() + kSwingSeconds;
						a_task.swingReissues = 0;
						a_task.swingReissueReal = RealSeconds() + kSwingReissueSeconds;
						return false;
					}
					if (dist > kKillMaxPreDispatchDistance && !a_task.killReformUsed) {
						a_task.killReformUsed = true;
						logs::info("task {}: the pair drifted {:.0f}u during the settle - re-staging",
							a_task.id, dist);
						KillMovePrepare(a_task, agent, target);
						a_task.killPrepareEndReal = RealSeconds() + kKillSettleSeconds;
						return false;
					}
					a_task.killVmResult.reset();
					LogKillState("dispatch", a_task, agent, target);
					a_task.killPlayed = PlayKillMove(a_task, agent, target);
				} else {
					a_task.killPlayed = false;
				}
				++a_task.killDispatchCount;
				a_task.killDispatchReal = RealSeconds();
				a_task.killEngaged = false;
				a_task.killReleased = false;
				a_task.killWaitingEngage = true;
				a_task.killEngageDeadlineReal = RealSeconds() + kKillEngageWaitSeconds;
				a_task.killBeatEndReal = RealSeconds() + kKillBeatSeconds;
				a_task.killAttempted = false;
				a_task.killConfirmEndReal = 0.0f;
				a_task.killVerdictGraceEndReal = 0.0f;
				a_task.killCleanupWaiting = false;
				a_task.killCleanupDeadlineReal = 0.0f;
				return false;
			}

			// Phase 1c (engagement): the engine sets its killmove state the
			// moment a paired killmove idle is accepted, so a dispatch that has
			// not engaged after kKillEngageWaitSeconds was refused (or silently
			// dropped). Re-stage and re-dispatch (up to kKillMaxDispatches
			// total) instead of waiting blind; log the release too, so the
			// verdicts can be read from the log.
			if (a_task.killWaitingEngage && agent && target) {
				const bool inKillMove = agent->IsInKillMove() || target->IsInKillMove();
				if (!a_task.killEngaged) {
					const auto& vmRes = a_task.killVmResult;
					const bool vmHave = vmRes && vmRes->have &&
						vmRes->agent == agent->GetFormID() &&
						vmRes->target == target->GetFormID();
					const bool vmRefused = vmHave && !vmRes->result;
					if (inKillMove) {
						BeginBountyGuard();
						a_task.killEngaged = true;
						a_task.killEngagedAtReal = RealSeconds();
						logs::info("task {}: killmove engaged after {:.2f}s (dispatch {})",
							a_task.id, RealSeconds() - a_task.killDispatchReal,
							a_task.killDispatchCount);
						LogKillState("engaged", a_task, agent, target);
						ReleaseTargetHold(a_task, target);
					} else if (target->IsDead()) {
						// The killmove is never going to start on a dead
						// target (the deferred kill never queued): stop
						// waiting and let the verdict run.
						a_task.killWaitingEngage = false;
					} else if (vmRefused ||
						RealSeconds() >= a_task.killEngageDeadlineReal) {
						logs::info("task {}: killmove did not engage (dispatch {} of {}, VM {}) - re-staging",
							a_task.id, a_task.killDispatchCount, kKillMaxDispatches,
							!vmHave ? "no result yet" :
								(vmRes->result ? "accepted" : "refused"));
						LogKillState("not-engaged", a_task, agent, target);
						a_task.killWaitingEngage = false;
						if (a_task.killWalkAfterAttempts) {
							if (a_task.killEarlyAttemptsLeft > 0) {
								// Close-in attempt two (the same preparation).
								--a_task.killEarlyAttemptsLeft;
								SendAnimEvent(target, "IdleForceDefaultState");
								KillMovePrepare(a_task, agent, target);
								a_task.killPrepareEndReal = RealSeconds() + kKillSettleSeconds;
								return false;
							}
							// Both close-in attempts were refused: send the
							// target walking toward the player (the movement
							// route) and take the kill once it has really moved.
							a_task.killWalkAfterAttempts = false;
							if (movement::StartTravel(target, player, false, true)) {
								const auto p = target->GetPosition();
								a_task.killWalkPending = true;
								a_task.killWalkHardCapReal = RealSeconds() + kKillWalkHardCapSeconds;
								a_task.killWalkLastProgressReal = RealSeconds();
								a_task.killWalkProgressX = p.x;
								a_task.killWalkProgressY = p.y;
								a_task.killWalkProgressZ = p.z;
								a_task.killWalkStartX = p.x;
								a_task.killWalkStartY = p.y;
								a_task.killWalkStartZ = p.z;
								a_task.killWalkRetargets = 0;
								a_task.killWalkEvalLeft = 2;
								a_task.killWalkEvalAtReal = RealSeconds() + 0.3f;
								a_task.killWalkStartReal = RealSeconds();
								a_task.killWalkPkgLogged = false;
								logs::info("task {}: {} walks clear for the kill (after the refused attempts)",
									a_task.id, a_task.targetName);
								return false;
							}
							logs::warn("IntelLite tasks: task {}: walk backend unavailable after refused attempts - continuing the attempts",
								a_task.id);
						}
						const float pairDist = agent->GetPosition().GetDistance(target->GetPosition());
						if (pairDist > kKillMaxPreDispatchDistance) {
							// The target has left the staging envelope: chasing
							// it with more dispatches only drags the pair
							// apart. End the
							// attempts; the verdict's re-approach handles it.
							logs::info("task {}: the pair drifted {:.0f}u apart - ending the attempts",
								a_task.id, pairDist);
						} else if (a_task.killDispatchCount < kKillMaxDispatches) {
							// Re-assert the victim's clean state for the next
							// attempt: a refusal usually means the engine did
							// not take the victim's animation state, and a
							// fresh reset widens the window.
							SendAnimEvent(target, "IdleForceDefaultState");
							KillMovePrepare(a_task, agent, target);
							a_task.killPrepareEndReal = RealSeconds() + kKillSettleSeconds;
							return false;
						}
					}
				} else if (!a_task.killReleased && !inKillMove) {
					a_task.killReleased = true;
					a_task.killWaitingEngage = false;
					// If an engaged killmove on a non-essential target still
					// left the target in bleedout (kEssentialDown / kBleedout),
					// finish the kill immediately as the animation releases.
					if (target && !target->IsDead() && !IsTargetEssential(target)) {
						const auto life = target->AsActorState()->GetLifeState();
						if (life == RE::ACTOR_LIFE_STATE::kEssentialDown ||
							life == RE::ACTOR_LIFE_STATE::kBleedout) {
							KillTarget(target, agent);
						}
					}
					logs::info("task {}: killmove released after {:.2f}s (target alive={})",
						a_task.id, RealSeconds() - a_task.killEngagedAtReal,
						target->IsDead() ? 0 : 1);
					LogKillState("released", a_task, agent, target);
				}
			}

			if (RealSeconds() < a_task.killBeatEndReal) return false;
			// The killmove animation is the ONLY thing that kills a visible
			// target: give its own kill a moment to land, then declare the
			// outcome (a target it did not kill survives, honestly).
			if (!a_task.killAttempted) {
				a_task.killAttempted = true;
				a_task.killConfirmEndReal = RealSeconds() + kKillConfirmSeconds;
				return false;
			}
			if (target && !target->IsDead() && RealSeconds() < a_task.killConfirmEndReal) return false;
			// The engine's own post-killmove handling (weapon restore) runs as
			// the paired animation releases the actors; cleaning the dagger up
			// mid-animation races it (a crash through HDT-SMP's equip hook).
			// Wait for the pair to leave the killmove, with a hard cap.
			if (agent && (agent->IsInKillMove() || (target && target->IsInKillMove()))) {
				if (!a_task.killCleanupWaiting) {
					a_task.killCleanupWaiting = true;
					a_task.killCleanupDeadlineReal = RealSeconds() + kKillCleanupWaitSeconds;
					logs::info("task {}: kill animation still running - delaying the weapon cleanup",
						a_task.id);
				}
				if (RealSeconds() < a_task.killCleanupDeadlineReal) return false;
			}
			// A killmove's own kill can land a beat after the animation
			// releases the pair (the engine defers it): an alive target gets
			// one short grace so a late kill is not narrated as "survived".
			if ((a_task.killCleanupWaiting || a_task.killEngaged) && target && !target->IsDead()) {
				if (a_task.killVerdictGraceEndReal == 0.0f) {
					a_task.killVerdictGraceEndReal = RealSeconds() + kKillVerdictGraceSeconds;
					logs::info("task {}: kill animation released - giving the kill a moment to land",
						a_task.id);
				}
				if (RealSeconds() < a_task.killVerdictGraceEndReal) return false;
				// The paired killmove animation engaged and ran to completion
				// on a non-essential target: ensure the fatal blow finishes
				// the target if a residual protected/bleedout state kept it alive.
				if (a_task.killEngaged && !IsTargetEssential(target)) {
					KillTarget(target, agent);
				}
			}
			// A refused scene gets one fresh encounter before the failure is
			// final: a new approach (a fresh drive + hold + scene) is what
			// converts a refusal. The dagger and the target hold are kept for
			// the retry.
			if (target && !target->IsDead() && !IsTargetEssential(target) && !a_task.reapproached) {
				a_task.reapproached = true;
				logs::info("task {}: kill window refused - re-approaching {} once",
					a_task.id, a_task.targetName);
				StopAllLegs(a_task, false);
				a_task.killCommitted = false;
				a_task.killPending = false;
				a_task.killPlayed = false;
				a_task.killAttempted = false;
				a_task.killWalkPending = false;
				a_task.killFurniture = false;
				a_task.killFurnitureFormID = 0;
				a_task.furnitureStandUpReal = 0.0f;
				a_task.furnitureDrivePending = false;
				a_task.killEngaged = false;
				a_task.killReleased = false;
				a_task.killWaitingEngage = false;
				a_task.killDispatchCount = 0;
				a_task.killDispatchReal = 0.0f;
				a_task.killEngagedAtReal = 0.0f;
				a_task.killReformUsed = false;
				a_task.killWalkAfterAttempts = false;
				a_task.killEarlyAttemptsLeft = 0;
				a_task.killWalkRetargets = 0;
				a_task.killWalkEvalLeft = 0;
				a_task.killWalkEvalAtReal = 0.0f;
				a_task.killPulseLogged = false;
				a_task.killPrepareEndReal = 0.0f;
				a_task.killBeatEndReal = 0.0f;
				a_task.killEngageDeadlineReal = 0.0f;
				a_task.killConfirmEndReal = 0.0f;
				a_task.killCleanupWaiting = false;
				a_task.killCleanupDeadlineReal = 0.0f;
				a_task.killVerdictGraceEndReal = 0.0f;
				a_task.swingPending = false;
				a_task.swingSawAttack = false;
				a_task.swingAttackSeenReal = 0.0f;
				a_task.swingCheckReal = 0.0f;
				a_task.swingEndReal = 0.0f;
				a_task.swingReissues = 0;
				a_task.swingReissueReal = 0.0f;
				a_task.killVmResult.reset();
				StartLeg(a_task.agentLeg, agent, target, nullptr, true);
				a_task.dueGameTime = a_task.agentLeg.dueGameTime;
				// A fresh re-approach is still the assassinate approach: it must
				// reach real proximity, not just the target's interior cell.
				a_task.agentLeg.strictArrival = true;
				a_task.state = State::AgentTravel;
				return true;
			}
			StopAllLegs(a_task);
			// Undo the scene's weapon: sheathe, unequip the spawned dagger,
			// then remove it (both the success and the failure paths).
			CleanupKillDagger(a_task, agent);
			ReleaseSneak(a_task, agent);
			ReleaseTargetHold(a_task, target);
			if (target && !target->IsDead()) {
				if (IsTargetEssential(target)) {
					AnnounceAssassinateRefused(a_task, false);   // essential target - the staged target stopped the attempt
				} else {
					AnnounceAssassinateFailed(a_task);    // "only managed to injure ... still lives"
				}
			} else {
				AnnounceAssassinateSuccess(a_task);
			}
			a_task.state = State::Done;
			return true;
		}

		// Advance a Meet task waiting at the meeting place. The meet completes
		// when the player comes within kMeetArriveDistance, or after the grace
		// period past meetGameTime. Returns true if the state changed. Game
		// thread only.
		bool AdvanceExecuting(Task& a_task)
		{
			if (a_task.type == Type::Assassinate) return AdvanceAssassinateScene(a_task);

			if (a_task.type != Type::Meet) return false;

			auto* agent  = RE::TESForm::LookupByID<RE::Actor>(a_task.agentFormID);
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!agent || !player) return false;

			const float dist = SameSpace(agent, player)
				? agent->GetPosition().GetDistance(player->GetPosition())
				: 1e9f;
			if (dist <= kMeetArriveDistance) {
				RE::SendHUDMessage::ShowHUDMessage(
					("You meet " + a_task.agentName + " as arranged.").c_str());
				// The agent spots the arriving player and reacts (narration).
				skyrimnet::DirectNarration(
					PlayerName() + " arrives at the meeting place and " + a_task.agentName +
						" spots them.",
					agent, player);
				ClearTaskStateKey(a_task);
				movement::StopSandbox(agent);
				a_task.state = State::Done;
				MarkDone(a_task);
				return true;
			}

			if (Now() >= a_task.meetGameTime + HoursToDays(kMeetWaitHours)) {
				RE::SendHUDMessage::ShowHUDMessage(
					(a_task.agentName + " waited for you, but you never came.").c_str());
				skyrimnet::RegisterPersistentEvent(
					a_task.agentName + " waited for the player, but they never came.", agent, player);
				ClearTaskStateKey(a_task);
				movement::StopSandbox(agent);
				a_task.state = State::Done;
				MarkDone(a_task);
				return true;
			}
			return false;
		}

		// Release any target-side furniture walk / hold / protected clear and
		// executor-side dagger / sneak when an assassination's outbound approach
		// aborts before reaching the target. Game thread only.
		void CleanupAssassinateOutboundAbort(Task& a_task, RE::Actor* a_agent, RE::Actor* a_target)
		{
			if (a_task.type != Type::Assassinate) return;
			a_task.furnitureStandUpReal = 0.0f;
			a_task.furnitureDrivePending = false;
			if (a_task.killWalkPending) {
				movement::StopTravel(a_target);
				a_task.killWalkPending = false;
			}
			ReleaseTargetHold(a_task, a_target);
			RestoreProtectedFlag(a_task);
			CleanupKillDagger(a_task, a_agent);
			ReleaseSneak(a_task, a_agent);
		}

		// Stage an outbound leg (Deliver, Fetch, or Meet) on its first
		// off-screen/deadline recovery when the executor is off-screen and the
		// destination is in the loaded area: prefer emerging from a local
		// building door, the mover's city gate, the destination's city gate,
		// the destination's interior entry door, or the location entrance
		// marker, and otherwise place the executor kOutboundApproachDistance
		// (3500 u) away along the incoming bearing so it runs the final
		// stretch in visibly instead of popping 250 u behind the target.
		// Sets approached=true so a second recovery falls through (no loop).
		// Game thread only.
		bool StageOutbound(Task& a_task, RE::Actor* a_agent, RE::TESObjectREFR* a_dest,
			bool a_wasApproached, bool a_wasDoorStaged)
		{
			if (!a_agent || !a_dest) return false;
			if (!movement::IsAvailable()) return false;
			if (a_agent->Is3DLoaded()) return false;

			auto* destCell = a_dest->GetParentCell();
			const bool destInterior = destCell && destCell->IsInteriorCell();

			auto* walkRef = (a_task.type == Type::Meet && a_task.placeWalkRefFormID)
				? RE::TESForm::LookupByID<RE::TESObjectREFR>(a_task.placeWalkRefFormID)
				: nullptr;
			if (!walkRef && destInterior) {
				walkRef = FindExitDoor(a_dest, nullptr);
			}
			RE::TESObjectREFR* walkTarget = (walkRef && walkRef != a_dest) ? walkRef : a_dest;

			const bool destLoaded = IsRefInLoadedArea(a_dest);
			const bool walkTargetLoaded = IsRefInLoadedArea(walkTarget);
			if (!destLoaded && !walkTargetLoaded) return false;

			const std::string purpose = (a_task.type == Type::Deliver)
				? std::string("to deliver the message")
				: (a_task.type == Type::Fetch)
					? ("to fetch " + a_task.targetName)
					: std::string("for the meeting");

			// 1. Loaded interior destination (the player is inside the same
			// building): enter through the cell's unlocked entrance door, else
			// approach from kApproachDistanceInterior inside the cell.
			if (destInterior && destLoaded) {
				if (!SameSpace(a_agent, a_dest) && !a_wasDoorStaged) {
					RE::TESObjectREFR* entryDoor = nullptr;
					RE::NiPoint3       landing{};
					if (FindInteriorEntryDoor(a_dest, entryDoor, landing) && entryDoor) {
						a_agent->MoveTo(entryDoor);
						a_agent->SetPosition(landing, true);
						(void)a_agent->MoveToNearestNavmesh(kNavmeshSnap);
						a_agent->EvaluatePackage(true, false);
						StartLeg(a_task.agentLeg, a_agent, a_dest);
						a_task.agentLeg.strictArrival = true;
						a_task.agentLeg.doorStaged = true;
						a_task.agentLeg.approached = true;
						a_task.dueGameTime = a_task.agentLeg.dueGameTime;
						logs::info("IntelLite tasks: {} enters through the door ({:08X}) {}",
							DisplayName(a_agent), entryDoor->GetFormID(), purpose);
						return true;
					}
				}
				if (a_wasApproached) return false;
				const auto destPos = a_dest->GetPosition();
				RE::NiPoint3 spot{};
				if (SameSpace(a_agent, a_dest)) {
					const auto moverPos = a_agent->GetPosition();
					const float dx = destPos.x - moverPos.x;
					const float dy = destPos.y - moverPos.y;
					const float len = std::sqrt(dx * dx + dy * dy);
					if (len > 1.0f) {
						const float scale = kApproachDistanceInterior / len;
						spot = { destPos.x - dx * scale, destPos.y - dy * scale, destPos.z };
					} else {
						const float z = a_dest->GetAngleZ();
						spot = {
							destPos.x - std::sin(z) * kApproachDistanceInterior,
							destPos.y - std::cos(z) * kApproachDistanceInterior,
							destPos.z
						};
					}
				} else {
					const float z = a_dest->GetAngleZ();
					spot = {
						destPos.x - std::sin(z) * kApproachDistanceInterior,
						destPos.y - std::cos(z) * kApproachDistanceInterior,
						destPos.z
					};
				}
				a_agent->MoveTo(a_dest);
				a_agent->SetPosition(spot, true);
				(void)a_agent->MoveToNearestNavmesh(kNavmeshSnap);
				a_agent->EvaluatePackage(true, false);
				StartLeg(a_task.agentLeg, a_agent, a_dest);
				a_task.agentLeg.strictArrival = true;
				a_task.agentLeg.doorStaged = true;
				a_task.agentLeg.approached = true;
				a_task.dueGameTime = a_task.agentLeg.dueGameTime;
				logs::info("IntelLite tasks: {} approaches {} from a distance ({:.0f}u)",
					DisplayName(a_agent), a_task.targetName,
					a_agent->GetPosition().GetDistance(destPos));
				return true;
			}

			if (a_wasApproached || !walkTargetLoaded) return false;

			auto* moverWs = a_agent->GetWorldspace();
			auto* destWs  = walkTarget->GetWorldspace();
			if (!destWs) return false;
			auto* tamriel = TamrielWorldspace();
			const auto destPos = walkTarget->GetPosition();

			// 2. Mover in an interior whose exterior exit door is in destWs
			// and loaded nearby (e.g. Uthgerd in the Bannered Mare -> Adrianne
			// in WhiterunWorld): step out of that building's exterior door.
			if (!SameSpace(a_agent, walkTarget)) {
				if (auto* exitDoor = FindExitDoor(a_agent, destWs);
					exitDoor && IsRefInLoadedArea(exitDoor) &&
					exitDoor->GetPosition().GetDistance(destPos) <= kEntranceMaxDistance) {
					a_agent->MoveTo(exitDoor);
					(void)a_agent->MoveToNearestNavmesh(kNavmeshSnap);
					a_agent->EvaluatePackage(true, false);
					StartLeg(a_task.agentLeg, a_agent, a_dest, walkRef);
					a_task.agentLeg.strictArrival = true;
					a_task.agentLeg.approached = true;
					a_task.dueGameTime = a_task.agentLeg.dueGameTime;
					logs::info("IntelLite tasks: {} steps out of the door {}",
						DisplayName(a_agent), purpose);
					return true;
				}
			}

			// 3. Mover inside a walled city (or an interior in one) and
			// destination outside that city in the gate's exterior worldspace:
			// place at the city gate's outside landing when loaded nearby.
			if (tamriel) {
				if (auto* gate = FindMoverCityGate(a_agent)) {
					auto* gateWs = gate->GetWorldspace();
					if (gateWs != tamriel && WorldspaceInFrame(gateWs, tamriel) &&
						gateWs != destWs && (!moverWs || moverWs == gateWs)) {
						RE::TESObjectREFR* outsideDoor = nullptr;
						RE::NiPoint3       landing{};
						bool               haveLanding = false;
						if (auto* gateTeleport = gate->extraList.GetByType<RE::ExtraTeleport>()) {
							if (gateTeleport->teleportData) {
								const RE::ObjectRefHandle outside = gateTeleport->teleportData->linkedDoor;
								auto outsideRef = outside.get();
								outsideDoor = outsideRef.get();
								landing = gateTeleport->teleportData->position;
								haveLanding = true;
							}
						}
						if (haveLanding && outsideDoor && outsideDoor->GetWorldspace() == destWs &&
							IsRefInLoadedArea(outsideDoor) &&
							landing.GetDistance(destPos) <= kEntranceMaxDistance) {
							a_agent->MoveTo(outsideDoor);
							a_agent->SetPosition(landing, true);
							(void)a_agent->MoveToNearestNavmesh(kNavmeshSnap);
							a_agent->EvaluatePackage(true, false);
							StartLeg(a_task.agentLeg, a_agent, a_dest, walkRef);
							a_task.agentLeg.strictArrival = true;
							a_task.agentLeg.approached = true;
							a_task.dueGameTime = a_task.agentLeg.dueGameTime;
							logs::info("IntelLite tasks: {} comes out of the city gate ({:08X}) {}",
								DisplayName(a_agent), gate->GetFormID(), purpose);
							return true;
						}
					}
				}
			}

			// 4. Destination on a walled city's street and mover outside that
			// city: place at the city gate's inside landing when loaded nearby.
			if (tamriel && destWs != tamriel && WorldspaceInFrame(destWs, tamriel) &&
				moverWs != destWs) {
				if (auto* gate = FindCityGate(a_agent, walkTarget, destWs);
					gate && IsRefInLoadedArea(gate) &&
					gate->GetPosition().GetDistance(destPos) <= kEntranceMaxDistance) {
					RE::NiPoint3 landing = gate->GetPosition();
					bool         haveLanding = false;
					if (auto* gateTeleport = gate->extraList.GetByType<RE::ExtraTeleport>()) {
						if (gateTeleport->teleportData) {
							const RE::ObjectRefHandle outside = gateTeleport->teleportData->linkedDoor;
							auto outsidePtr = outside.get();
							if (auto* outsideRef = outsidePtr.get()) {
								if (auto* outsideTeleport = outsideRef->extraList.GetByType<RE::ExtraTeleport>()) {
									if (outsideTeleport->teleportData) {
										landing = outsideTeleport->teleportData->position;
										haveLanding = true;
									}
								}
							}
						}
					}
					if (!haveLanding) {
						const float dx = destPos.x - landing.x;
						const float dy = destPos.y - landing.y;
						const float len = std::sqrt(dx * dx + dy * dy);
						if (len > 1.0f) {
							landing.x += dx / len * 150.0f;
							landing.y += dy / len * 150.0f;
						}
					}
					a_agent->MoveTo(gate);
					a_agent->SetPosition(landing, true);
					(void)a_agent->MoveToNearestNavmesh(kNavmeshSnap);
					a_agent->EvaluatePackage(true, false);
					StartLeg(a_task.agentLeg, a_agent, a_dest, walkRef);
					a_task.agentLeg.strictArrival = true;
					a_task.agentLeg.approached = true;
					a_task.dueGameTime = a_task.agentLeg.dueGameTime;
					logs::info("IntelLite tasks: {} stages at the city gate ({:08X}) {}",
						DisplayName(a_agent), gate->GetFormID(), purpose);
					return true;
				}
			}

			// 5. Plausible settlement entrance: the destination's location
			// worldLocMarker when in the same worldspace, loaded, and within range.
			{
				auto* loc = walkTarget->GetCurrentLocation();
				RE::TESObjectREFR* marker = nullptr;
				for (int hops = 0; loc && hops < 4; ++hops) {
					const RE::ObjectRefHandle markerHandle = loc->worldLocMarker;
					auto markerPtr = markerHandle.get();
					if (auto* m = markerPtr.get()) { marker = m; break; }
					loc = loc->parentLoc;
				}
				if (marker && marker->GetWorldspace() == destWs && IsRefInLoadedArea(marker)) {
					const auto mp = marker->GetPosition();
					const float pd = mp.GetDistance(destPos);
					if (pd >= kEntranceMinDistance && pd <= kEntranceMaxDistance) {
						const float dx = destPos.x - mp.x;
						const float dy = destPos.y - mp.y;
						const float len = std::sqrt(dx * dx + dy * dy);
						RE::NiPoint3 spot = mp;
						if (len > 1.0f) {
							spot.x = mp.x + dx / len * 150.0f;
							spot.y = mp.y + dy / len * 150.0f;
						}
						a_agent->MoveTo(marker);
						a_agent->SetPosition(spot, true);
						(void)a_agent->MoveToNearestNavmesh(kNavmeshSnap);
						a_agent->EvaluatePackage(true, false);
						StartLeg(a_task.agentLeg, a_agent, a_dest, walkRef);
						a_task.agentLeg.strictArrival = true;
						a_task.agentLeg.approached = true;
						a_task.dueGameTime = a_task.agentLeg.dueGameTime;
						logs::info("IntelLite tasks: {} stages at the entrance marker {}",
							DisplayName(a_agent), purpose);
						return true;
					}
				}
			}

			// 6. Bearing / distance fallback: place the executor
			// kOutboundApproachDistance (3500 u) away from the destination along
			// its incoming bearing (same-space or Tamriel-frame, else behind the
			// destination) and let it run the last stretch in visibly instead of
			// popping 250 u behind the target.
			RE::NiPoint3 toDest{};
			bool         haveDir = false;
			if (SameSpace(a_agent, walkTarget)) {
				const auto moverPos = a_agent->GetPosition();
				toDest = { destPos.x - moverPos.x, destPos.y - moverPos.y, 0.0f };
				haveDir = true;
			} else if (tamriel) {
				RE::NiPoint3 moverFrame{};
				RE::NiPoint3 destFrame{};
				if (MapFramePosition(a_agent, tamriel, moverFrame) &&
					MapFramePosition(walkTarget, tamriel, destFrame)) {
					toDest = { destFrame.x - moverFrame.x, destFrame.y - moverFrame.y, 0.0f };
					haveDir = true;
				}
			}

			RE::NiPoint3 spot{};
			const float len = haveDir ? std::sqrt(toDest.x * toDest.x + toDest.y * toDest.y) : 0.0f;
			if (len > 1.0f) {
				const float scale = kOutboundApproachDistance / len;
				spot = {
					destPos.x - toDest.x * scale,
					destPos.y - toDest.y * scale,
					destPos.z
				};
			} else {
				const float z = walkTarget->GetAngleZ();
				spot = {
					destPos.x - std::sin(z) * kOutboundApproachDistance,
					destPos.y - std::cos(z) * kOutboundApproachDistance,
					destPos.z
				};
			}

			// Keep the staging point inside the player's loaded exterior cell grid
			// (~8000u guaranteed for 5x5 uGrids) when the destination is itself far
			// from the player, so the staged executor always loads 3D and walks.
			if (auto* player = RE::PlayerCharacter::GetSingleton();
				player && SameSpace(walkTarget, player)) {
				const auto playerPos = player->GetPosition();
				if (spot.GetDistance(playerPos) > 7000.0f) {
					const float pdx = playerPos.x - destPos.x;
					const float pdy = playerPos.y - destPos.y;
					const float plen = std::sqrt(pdx * pdx + pdy * pdy);
					if (plen > 1.0f) {
						const float ux = pdx / plen;
						const float uy = pdy / plen;
						float nx = -uy;
						float ny = ux;
						if ((spot.x - destPos.x) * nx + (spot.y - destPos.y) * ny < 0.0f) {
							nx = -nx;
							ny = -ny;
						}
						const float dirX = 0.5f * ux + 0.8660254f * nx;
						const float dirY = 0.5f * uy + 0.8660254f * ny;
						spot.x = destPos.x + dirX * kOutboundApproachDistance;
						spot.y = destPos.y + dirY * kOutboundApproachDistance;
					}
				}
			}

			a_agent->MoveTo(walkTarget);
			a_agent->SetPosition(spot, true);
			(void)a_agent->MoveToNearestNavmesh(kNavmeshSnap);
			a_agent->EvaluatePackage(true, false);
			StartLeg(a_task.agentLeg, a_agent, a_dest, walkRef);
			a_task.agentLeg.strictArrival = true;
			a_task.agentLeg.approached = true;
			a_task.dueGameTime = a_task.agentLeg.dueGameTime;
			logs::info("IntelLite tasks: {} approaches {} from a distance ({:.0f}u)",
				DisplayName(a_agent), a_task.targetName,
				a_agent->GetPosition().GetDistance(destPos));
			return true;
		}

		// Advance one AgentTravel task (fetch, deliver, or meet). Returns true
		// if the state changed. Game thread only.
		bool AdvanceAgentTravel(Task& a_task)
		{
			auto* agent  = RE::TESForm::LookupByID<RE::Actor>(a_task.agentFormID);
			auto* target = RE::TESForm::LookupByID<RE::Actor>(a_task.targetFormID);
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player) return false;

			// Complete a deferred furniture drive: the get-up animation has
			// had its beat, so clear the state, snap off the seat and start
			// the walk toward the player.
			if (a_task.type == Type::Assassinate && !a_task.outboundFailed &&
				a_task.furnitureStandUpReal > 0.0f &&
				RealSeconds() >= a_task.furnitureStandUpReal &&
				target && !target->IsDead() && target->Is3DLoaded() &&
				SameSpace(target, player)) {
				DriveTargetOffFurniture(a_task, target, player, false);
			}
			if (a_task.type == Type::Assassinate && !a_task.outboundFailed &&
				a_task.furnitureDrivePending && a_task.agentLeg.active &&
				!a_task.agentLeg.pendingStart &&
				agent && agent->Is3DLoaded() && !agent->IsInCombat() &&
				target && !target->IsDead() && target->Is3DLoaded() &&
				!IsTargetEssential(target) &&
				!a_task.targetHeld && !HasActiveTask(a_task.targetFormID) &&
				SameSpace(agent, target) && SameSpace(target, player) &&
				a_task.furnitureStandUpReal == 0.0f && !a_task.killWalkPending) {
				const float driveDist = agent->GetPosition().GetDistance(target->GetPosition());
				if (driveDist > kLateFurnitureDriveMinLead &&
					DriveTargetOffFurniture(a_task, target, player, true)) {
					logs::info("task {}: deferred furniture drive started on {} ({:.0f}u out)",
						a_task.id, a_task.targetName, driveDist);
				}
			}

			// The reference the outbound leg walks to: a meet goes to its
			// location marker; fetch/deliver go to the target actor.
			RE::TESObjectREFR* dest = nullptr;
			if (a_task.type == Type::Meet) {
				dest = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_task.locationFormID);
			} else {
				dest = target;
			}

			// --- the executor was blocked on a visible walk: it is walking back
			// to report; the task fails when it arrives (or its recovery fires). ---
			if (a_task.outboundFailed) {
				const auto status = TickLeg(a_task.agentReturnLeg, kArriveDistance);
				if (status == LegStatus::Travelling) return false;
				StopAllLegs(a_task);
				if (a_task.type == Type::Assassinate) {
					logs::info("task {}: agent returned to report the assassination of {} unreachable",
						a_task.id, a_task.targetName);
					AnnounceAssassinateUnreachable(a_task);
				} else {
					logs::info("task {}: agent returned to report {} unreachable",
						a_task.id, a_task.targetName);
					AnnounceUnreachable(a_task);
				}
				a_task.state = State::Failed;
				return true;
			}

			// --- blocked-door watch: a destination inside a building is reached
			// through its door. If the executor is standing at that door (within
			// kDoorReachDistance of the destination's anchor) and is still not in
			// the destination's cell, it cannot get in - a locked door. Wait a
			// short budget (kDoorWaitSeconds) before turning back rather than
			// burning the full walk timeout. Moving away resets the wait. ---
			if (a_task.agentLeg.active && a_task.agentLeg.destInterior && agent && dest) {
				auto* destCell = dest->GetParentCell();
				const bool inDestCell = destCell && agent->GetParentCell() == destCell;
				const RE::NiPoint3 anchor{
					a_task.agentLeg.anchorX, a_task.agentLeg.anchorY, a_task.agentLeg.anchorZ
				};
				const float toAnchor = agent->GetPosition().GetDistance(anchor);
				if (!inDestCell && toAnchor <= kDoorReachDistance) {
					// The door to test: the leg's exterior walk ref when it has one
					// (a named-place destination), otherwise the destination's own
					// exterior door. Never the destination itself - an actor, or the
					// player, is never locked, so that watch would never fire.
					RE::TESObjectREFR* door = nullptr;
					if (a_task.agentLeg.walkRefFormID) {
						door = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_task.agentLeg.walkRefFormID);
					} else if (dest && !dest->GetWorldspace()) {
						door = FindExitDoor(dest, nullptr);
					}
					const bool locked = door && door->IsLocked();
					if (!locked) {
						a_task.agentLeg.doorWaitStart = 0.0f;
					} else if (a_task.agentLeg.doorWaitStart == 0.0f) {
						a_task.agentLeg.doorWaitStart = RealSeconds();
					} else if (RealSeconds() - a_task.agentLeg.doorWaitStart > kDoorWaitSeconds) {
						StopLeg(a_task.agentLeg);
						CleanupAssassinateOutboundAbort(a_task, agent, target);
						a_task.outboundFailed = true;
						StartLeg(a_task.agentReturnLeg, agent, player);
						logs::info("task {}: {} blocked at the door of {} - returning to report",
							a_task.id, a_task.agentName, a_task.targetName);
						return true;
					}
				} else if (toAnchor > kDoorResetDistance) {
					a_task.agentLeg.doorWaitStart = 0.0f;
				}
			}

			// --- outbound leg: agent -> destination ---
			// An essential assassinate victim is killed with the cosmetic swing
			// package, which does NOT close the gap itself: the executor must
			// walk into knife reach (kSwingArriveDistance) before the handoff,
			// so the package's own attack range covers the target. A mortal
			// victim keeps the wider killmove arrival, as does every other
			// action.
			const bool assassinateEssential =
				(a_task.type == Type::Assassinate) && IsTargetEssential(target);
			const float outboundArrive = (a_task.type == Type::Assassinate)
				? (assassinateEssential ? kSwingArriveDistance : kKillArriveDistance)
				: kArriveDistance;
			const auto status = TickLeg(a_task.agentLeg, outboundArrive);
			switch (status) {
			case LegStatus::Arrived:
				if (a_task.type == Type::Deliver) {
					logs::info("task {}: agent reached {} (walked)", a_task.id, a_task.targetName);
					DeliverMessage(a_task, agent);
					FinishDeliver(a_task, agent);
					return true;
				}
				if (a_task.type == Type::Meet) {
					logs::info("task {}: agent reached the meeting place at {}",
						a_task.id, a_task.targetName);
					if (dest) {
						movement::StartSandbox(agent, dest);
					}
					a_task.state = State::Executing;
					return true;
				}
				if (a_task.type == Type::Assassinate) {
					logs::info("task {}: agent reached {} (walked)", a_task.id, a_task.targetName);
					HandleAssassinateArrival(a_task, agent, target, player);
					return true;
				}
				logs::info("task {}: agent reached {} (walked)", a_task.id, a_task.targetName);
				BeginReturn(a_task);
				return true;
			case LegStatus::Invalid:
				StopAllLegs(a_task);
				if (a_task.type == Type::Assassinate) {
					CleanupAssassinateOutboundAbort(a_task, agent, target);
					logs::info("task {}: assassination of {} failed - the executor or target is unavailable",
						a_task.id, a_task.targetName);
					ClearTaskStateKey(a_task);
					a_task.state = State::Failed;
				} else {
					MarkFailed(a_task, "agent or target unavailable");
					a_task.state = State::Failed;
				}
				return true;
			case LegStatus::Recover: {
				// Capture the leg's walk/on-screen state before releasing it: a
				// live walk that was ON-SCREEN and stalled (not one cut off by
				// its estimate deadline) was visibly blocked (a locked door it
				// cannot open), so the executor turns around and reports instead
				// of teleporting in. An off-screen walk, a simulated leg, or a
				// deadline recover keeps the far path (teleport in).
				const bool wasWalk = a_task.agentLeg.active;
				const bool wasOffscreen = a_task.agentLeg.offscreen;
				const bool wasDeadline = a_task.agentLeg.deadlineFired;
				const bool wasApproached = a_task.agentLeg.approached;
				const bool wasDoorStaged = a_task.agentLeg.doorStaged;
				if (wasWalk && !wasOffscreen && !wasDeadline) {
					StopLeg(a_task.agentLeg);
					CleanupAssassinateOutboundAbort(a_task, agent, target);
					a_task.outboundFailed = true;
					StartLeg(a_task.agentReturnLeg, agent, player);
					logs::info("task {}: {} blocked on a visible walk to {} - returning to report",
						a_task.id, a_task.agentName, a_task.targetName);
				} else if (a_task.type == Type::Assassinate) {
					// The simulation is the trip: no teleport, no staged
					// scene - the executor reaches the target in game time
					// and the kill resolves off-screen (a mortal target
					// dies; an essential one turns the executor back).
					StopLeg(a_task.agentLeg);
					HandleAssassinateOffscreen(a_task, agent, target, player);
				} else if (StageOutbound(a_task, agent, dest ? dest : target, wasApproached, wasDoorStaged)) {
					return true;
				} else {
					auto* anchor = dest ? dest : static_cast<RE::TESObjectREFR*>(target);
					const bool fetchOffscreenTarget =
						(a_task.type == Type::Fetch) && (!anchor || !IsRefInLoadedArea(anchor));
					if (fetchOffscreenTarget) {
						// Do NOT call StopTravel(agent) here: removing the
						// priority-100 travel override lets a follower's vanilla
						// Follow-Player package take over in High Process and walk
						// the follower back to the player while the target is still
						// off-screen. Leave the override on until JoinAtStagingPoint.
						a_task.agentLeg = Leg{};
					} else {
						StopLeg(a_task.agentLeg);
					}
					if (a_task.type == Type::Deliver && anchor && IsRefInLoadedArea(anchor)) {
						// Never pop 250 u behind a visible recipient: if already staged in
						// the recipient's space within kKeepStagedDistance, keep that spot;
						// otherwise place at kOutboundApproachDistance (3500 u).
						if (!SameSpace(agent, anchor) ||
							(agent && agent->GetPosition().GetDistance(anchor->GetPosition()) > kKeepStagedDistance)) {
							TeleportBehind(agent, anchor, kOutboundApproachDistance);
						}
					} else if (a_task.type == Type::Fetch) {
						if (anchor && IsRefInLoadedArea(anchor)) {
							if (!SameSpace(agent, anchor) ||
								(agent && agent->GetPosition().GetDistance(anchor->GetPosition()) > kKeepStagedDistance)) {
								TeleportBehind(agent, anchor, kOutboundApproachDistance);
							}
						}
					} else {
						TeleportBehind(agent, anchor, kTeleportOffset);
					}
					if (a_task.type == Type::Deliver) {
						DeliverMessage(a_task, agent);
						FinishDeliver(a_task, agent);
					} else if (a_task.type == Type::Meet) {
						if (dest) {
							movement::StartSandbox(agent, dest);
						}
						a_task.state = State::Executing;
					} else {
						BeginReturn(a_task);
					}
				}
				return true;
			}
			case LegStatus::Travelling:
			default:
				return false;
			}
		}

		// Advance one TargetTravel task (the return phase). Returns true if the
		// state changed. The target side and the agent's own return side complete
		// independently; the task is done when both have arrived. Game thread
		// only.
		bool AdvanceTargetTravel(Task& a_task)
		{
			auto* target = RE::TESForm::LookupByID<RE::Actor>(a_task.targetFormID);
			auto* agent  = RE::TESForm::LookupByID<RE::Actor>(a_task.agentFormID);
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player) return false;

			// Assassinate failure return: the target was essential, so only the
			// agent's own leg back to the player matters; the task fails with
			// the survivor report when it arrives.
			if (a_task.type == Type::Assassinate) {
				if (!a_task.agentDone) {
					switch (TickLeg(a_task.agentReturnLeg, kArriveDistance)) {
					case LegStatus::Arrived:
						a_task.agentDone = true;
						break;
					case LegStatus::Invalid:
						StopAllLegs(a_task);
						ClearTaskStateKey(a_task);
						logs::info("task {}: assassination return aborted - executor unavailable", a_task.id);
						a_task.state = State::Failed;
						return true;
					case LegStatus::Recover:
						if (ApproachFromDistance(a_task.agentReturnLeg, agent, player)) {
							logs::info("task {}: agent approaching from a distance", a_task.id);
							break;
						}
						StopLeg(a_task.agentReturnLeg);
						a_task.agentDone = true;
						if (agent) {
							const auto          wasPos  = agent->GetPosition();
							auto*               wasWs   = agent->GetWorldspace();
							const float         wasDist = wasPos.GetDistance(player->GetPosition());
							const bool          was3d   = agent->Is3DLoaded();
							const RE::FormID    wasPkg  = RunningPackageOf(agent).form;
							TeleportBehind(agent, player, 450.0f);
							logs::info("task {}: agent teleported to you (was at ({:.0f},{:.0f},{:.0f}) ws={:08X}, {:.0f}u away, mover3d={}, pkg={:08X})",
								a_task.id,
								wasPos.x, wasPos.y, wasPos.z,
								wasWs ? wasWs->GetFormID() : 0,
								wasDist, was3d,
								wasPkg);
						} else {
							TeleportBehind(agent, player, 450.0f);
							logs::info("task {}: agent teleported to you", a_task.id);
						}
						break;
					case LegStatus::Travelling:
					default:
						break;
					}
				}
				if (a_task.agentDone) {
					StopAllLegs(a_task);
					AnnounceAssassinateRefused(a_task, a_task.refusalFarAway);
					a_task.state = State::Failed;
					return true;
				}
				return false;
			}

			// ---- target side: bring the target to the player ----
			if (!a_task.targetArrived) {
				switch (TickLeg(a_task.targetLeg, kArriveDistance)) {
				case LegStatus::Arrived:
					logs::info("task {}: target {} reached the player (walked)",
						a_task.id, a_task.targetName);
					OnTargetArrivedFetch(a_task, agent, target, player);
					break;
				case LegStatus::Recover:
					// Prefer a visible approach: place the target 7000 units out
					// and let it run the last stretch in. Only when it cannot be
					// placed (different worldspace, already approached) fall back
					// to the teleport.
					if (ApproachFromDistance(a_task.targetLeg, target, player)) {
						logs::info("task {}: target {} approaching from a distance",
							a_task.id, a_task.targetName);
						if (!a_task.agentDone && agent && target) {
							ReleaseAgentOffscreenHold(a_task, agent);
							const bool alreadyBesideTarget = SameSpace(agent, target) &&
								agent->GetPosition().GetDistance(target->GetPosition()) <= kArriveDistance;
							if (!alreadyBesideTarget) {
								JoinAtStagingPoint(agent, target, player, a_task.agentReturnLeg);
								logs::info("task {}: agent joins the target at the staging point", a_task.id);
							} else if (!a_task.agentReturnLeg.active) {
								StartLeg(a_task.agentReturnLeg, agent, player);
								a_task.agentReturnLeg.strictArrival = true;
								a_task.agentReturnLeg.approached = true;
							}
						}
						break;
					}
					StopLeg(a_task.targetLeg);
					TeleportBehind(target, player, kTeleportOffset);
					logs::info("task {}: target {} teleported to you",
						a_task.id, a_task.targetName);
					logs::info("task {}: target {} recovered to the player",
						a_task.id, a_task.targetName);
					OnTargetArrivedFetch(a_task, agent, target, player);
					break;
				case LegStatus::Invalid:
					StopAllLegs(a_task);
					MarkFailed(a_task, "target unavailable");
					a_task.state = State::Failed;
					return true;
				case LegStatus::Travelling:
				default:
					break;
				}
			}

			// ---- agent side: the agent's own leg back to the player ----
			if (!a_task.agentDone) {
				// While the target is still off-screen and has not been staged
				// into the player's area yet, hold the executor in lockstep with
				// the target's return leg so the executor can never arrive
				// before the target (even if the executor remained in Tamriel or
				// a follower framework pulled it through a load door).
				if (!a_task.targetArrived && !a_task.targetLeg.approached &&
					(!target || !IsRefInLoadedArea(target))) {
					if (agent && !agent->Is3DLoaded() && !a_task.agentHeldOffscreen) {
						HoldAgentOffscreen(a_task, agent);
					}
					a_task.agentReturnLeg.dueGameTime = a_task.targetLeg.dueGameTime;
					return false;
				}
				ReleaseAgentOffscreenHold(a_task, agent);
				// If the target entered the loaded area without ApproachFromDistance
				// (e.g. the player walked into the target's cell) while the agent
				// leg was still waiting in simulated lockstep, join the target now.
				if (!a_task.targetArrived && a_task.agentReturnLeg.simulated &&
					agent && target && IsRefInLoadedArea(target)) {
					if (!SameSpace(agent, target) ||
						agent->GetPosition().GetDistance(target->GetPosition()) > kArriveDistance) {
						JoinAtStagingPoint(agent, target, player, a_task.agentReturnLeg);
						logs::info("task {}: agent joins the target at the staging point", a_task.id);
					} else {
						StartLeg(a_task.agentReturnLeg, agent, player);
						a_task.agentReturnLeg.strictArrival = true;
					}
				}
				switch (TickLeg(a_task.agentReturnLeg, kArriveDistance)) {
				case LegStatus::Arrived:
					a_task.agentDone = true;
					logs::info("task {}: agent returned", a_task.id);
					break;
				case LegStatus::Invalid:
					StopLeg(a_task.agentReturnLeg);
					a_task.agentDone = true;
					logs::info("task {}: agent return leg invalid - executor unavailable", a_task.id);
					break;
				case LegStatus::Recover: {
					// Prefer a visible approach before the teleport fallback.
					if (ApproachFromDistance(a_task.agentReturnLeg, agent, player)) {
						logs::info("task {}: agent approaching from a distance", a_task.id);
						if (!a_task.targetArrived && target && !target->Is3DLoaded() &&
							DistanceToPlayer(target, player) > kPairStageDistance) {
							JoinAtStagingPoint(target, agent, player, a_task.targetLeg);
							logs::info("task {}: target joins the agent at the staging point", a_task.id);
						}
						break;
					}
					StopLeg(a_task.agentReturnLeg);
					a_task.agentDone = true;
					// The agent always rejoins the player: a return that could
					// not complete (timed out) is snapped back behind the player.
					TeleportBehind(agent, player, 450.0f);
					logs::info("task {}: agent teleported to you", a_task.id);
					break;
				}
				case LegStatus::Travelling:
				default:
					break;
				}
			}

			if (a_task.targetArrived && a_task.agentDone) {
				if (a_task.type == Type::Fetch) {
					// The fetch is delivered: the target lingers near the player
					// (IntelEngine-style sandbox) instead of completing instantly.
					// The sandbox usually already runs (applied on arrival); only
					// apply it here when it is not.
					if (target && !a_task.targetSandboxed) {
						if (movement::StartSandbox(target, player)) {
							a_task.targetSandboxed = true;
						}
					}
					a_task.lingerGraceUntilReal = RealSeconds() + 5.0f;
					a_task.state = State::Lingering;
					logs::info("task {}: target {} lingering near the player",
						a_task.id, a_task.targetName);
				} else {
					// Deliver: no linger - the task completes once the agent is
					// back with the player. Announce the completed round trip
					// (the recipient is not informed).
					skyrimnet::RegisterPersistentEvent(
						a_task.agentName + " returns after delivering the message to " +
							a_task.targetName + ".",
						agent, RE::PlayerCharacter::GetSingleton());
					a_task.state = State::Done;
					MarkDone(a_task);
				}
				return true;
			}
			return false;
		}

		// Advance one Lingering task: the target sandboxes near the player. The
		// linger releases (ReleaseActor -> Done) once the target is missing/dead,
		// no longer 3D-loaded, in another space, or farther than
		// kLingerReleaseDistance - there is no timer. Game thread only.
		bool AdvanceLingering(Task& a_task)
		{
			auto* target = RE::TESForm::LookupByID<RE::Actor>(a_task.targetFormID);
			auto* player = RE::PlayerCharacter::GetSingleton();

			if (!target || !player || target->IsDead()) {
				logs::info("task {}: linger ended - {}", a_task.id, !target ? "target gone" : (!player ? "player gone" : "target dead"));
				if (target) {
					ReleaseActor(target);
				}
				a_task.state = State::Done;
				MarkDone(a_task);
				return true;
			}

			const float dist = SameSpace(target, player)
				? target->GetPosition().GetDistance(player->GetPosition())
				: 1e9f;
			if (RealSeconds() >= a_task.lingerGraceUntilReal &&
				(!target->Is3DLoaded() || !SameSpace(target, player) ||
					dist > kLingerReleaseDistance)) {
				logs::info("task {}: linger ended - 3dLoaded={} sameSpace={} dist={:.0f}",
					a_task.id, target->Is3DLoaded() ? 1 : 0,
					SameSpace(target, player) ? 1 : 0, dist);
				ReleaseActor(target);
				logs::info("task {}: linger ended", a_task.id);
				a_task.state = State::Done;
				MarkDone(a_task);
				return true;
			}
			if (dist > 300.0f && RealSeconds() >= a_task.lingerReassertReal) {
				movement::StartSandbox(target, player);
				a_task.lingerReassertReal = RealSeconds() + 3.0f;
				logs::info("task {}: linger sandbox re-asserted (dist {:.0f})", a_task.id, dist);
			}
			return false;
		}

		const char* TypeName(Type a_type)
		{
			switch (a_type) {
			case Type::Fetch:       return "fetch";
			case Type::Deliver:     return "deliver";
			case Type::Meet:        return "meet";
			case Type::Assassinate: return "assassinate";
			default:                return "unknown";
			}
		}

		const char* StateName(State a_state)
		{
			switch (a_state) {
			case State::Pending:      return "pending";
			case State::AgentTravel:  return "traveling";
			case State::TargetTravel: return "returning";
			case State::Executing:    return "executing";
			case State::Done:         return "done";
			case State::Cancelled:    return "cancelled";
			case State::Failed:       return "failed";
			case State::Lingering:    return "lingering";
			default:                  return "unknown";
			}
		}

		const char* DetailText(State a_state)
		{
			switch (a_state) {
			case State::Pending:      return "Queued";
			case State::AgentTravel:  return "Executor is on the way to the target";
			case State::TargetTravel: return "Escorting the target to you";
			case State::Executing:    return "In progress";
			case State::Done:         return "Complete";
			case State::Cancelled:    return "Cancelled";
			case State::Failed:       return "Failed - actor unavailable";
			case State::Lingering:    return "Lingering near you";
			default:                  return "";
			}
		}

		bool IsFinished(State a_state)
		{
			return a_state == State::Done || a_state == State::Cancelled || a_state == State::Failed;
		}

		// Game thread. The shared cancellation body used by the panel/action
		// cancels and by the supersede path: refuses a committed assassination
		// (logs, returns false - it is left running); otherwise releases the
		// task's live legs and any sandbox it owns, walks the agent home when
		// a_sendHome is set (the cancel paths, not supersede - the agent there is
		// reassigned), announces the cancellation, and marks the task Cancelled.
		// Returns true when the task was cancelled.
		bool CancelTask(Task& a_task, bool a_sendHome, bool a_keepAgentTravel = false)
		{
			if (a_task.type == Type::Assassinate && AssassinationCommitted(a_task)) {
				logs::info("task {}: assassination in progress - not cancellable", a_task.id);
				return false;
			}
			auto* agent  = RE::TESForm::LookupByID<RE::Actor>(a_task.agentFormID);
			auto* target = RE::TESForm::LookupByID<RE::Actor>(a_task.targetFormID);

			const bool dismissOnly = (a_task.state == State::Lingering);
			if (a_task.state == State::AgentTravel || a_task.state == State::TargetTravel) {
				StopAllLegs(a_task, !a_keepAgentTravel);
				if (a_sendHome) {
					ReturnAgentIfNeeded(a_task);
				}
				// A fetch's target sandbox may already be applied (it starts at
				// target arrival, before the return completes): release it so a
				// cancel cannot strand the target in a sandbox.
				if (a_task.type == Type::Fetch) {
					ReleaseActor(target);
					a_task.targetSandboxed = false;
				}
			} else if (a_task.state == State::Lingering) {
				ReleaseActor(target);
				a_task.targetSandboxed = false;
				logs::info("task {}: linger dismissed - {} released to its own AI", a_task.id, a_task.targetName);
			} else if (a_task.state == State::Executing) {
				if (a_task.type == Type::Assassinate && a_task.swingPending) {
					if (!a_keepAgentTravel) {
						movement::StopSwing(agent);
					}
					a_task.swingPending = false;
				} else {
					movement::StopSandbox(agent);
				}
			}

			// Run contract-end actor cleanups immediately (not deferred to the
			// end of the next Tick), so a superseding task created right after
			// StoreTaskAndSupersede on the same agent/target cannot be clobbered
			// by the old task's deferred sweep.
			if (a_task.protectedCleared) {
				RestoreProtectedFlag(a_task);
			}
			a_task.furnitureStandUpReal = 0.0f;
			if (a_task.killWalkPending) {
				movement::StopTravel(target);
				a_task.killWalkPending = false;
			}
			if (a_task.targetSandboxed) {
				ReleaseActor(target);
				a_task.targetSandboxed = false;
			}
			if (a_task.targetHeld) {
				ReleaseTargetHold(a_task, target);
			}
			if (a_task.type == Type::Assassinate) {
				if (a_task.swingPending) {
					if (!a_keepAgentTravel) {
						movement::StopSwing(agent);
					}
					a_task.swingPending = false;
				}
				CleanupKillDagger(a_task, agent);
				if (a_keepAgentTravel) {
					a_task.sneakApproach = false;
				} else {
					ReleaseSneak(a_task, agent);
				}
			}

			if (!dismissOnly) {
				AnnounceCancel(a_task);
			}
			a_task.state = State::Cancelled;
			return true;
		}

		// Store a fully-populated task on the game thread, then cancel every other
		// non-finished task for the same agent: the agent is reassigned to the
		// new task, not sent home. Returns the new task id, or 0 when the task
		// cap was reached (nothing stored). Live travel is released on the game
		// thread because StopAllLegs dispatches through the VM.
		std::uint32_t StoreTaskAndSupersede(Task a_task)
		{
			const std::uint32_t agentFormID = a_task.agentFormID;
			const bool newTaskStartsAgentTravel = (a_task.state != State::Pending);
			auto* agentActor = RE::TESForm::LookupByID<RE::Actor>(agentFormID);
			const bool keepAgentTravel = newTaskStartsAgentTravel &&
				agentActor && agentActor->Is3DLoaded() && !agentActor->IsInCombat();

			std::uint32_t id = 0;
			if (g_tasks.size() >= kMaxTasks) {
				logs::warn("IntelLite tasks: task cap reached; task refused");
				return 0;
			}
			a_task.id = g_nextID++;
			id = a_task.id;
			g_tasks.push_back(std::move(a_task));

			std::vector<Task*> superseded;
			for (auto& task : g_tasks) {
				if (task.id != id && !IsFinished(task.state) &&
					task.agentFormID == agentFormID) {
					superseded.push_back(&task);
				}
			}
			for (auto* old : superseded) {
				// A lingering task's executor is already done with it: the linger must not
				// be cancelled (that would release the fetched target and emit a false
				// "called off" event) when the agent takes a new task.
				if (old->state == State::Lingering) {
					logs::info("task {}: lingering - left running while the agent takes a new task", old->id);
					continue;
				}
				if (CancelTask(*old, false, keepAgentTravel)) {
					logs::info("task {}: superseded by task {}", old->id, id);
				}
			}
			return id;
		}

		// ---- pump -----------------------------------------------------------

		void PumpLoop()
		{
			while (!g_stop.load(std::memory_order_relaxed)) {
				std::this_thread::sleep_for(std::chrono::milliseconds(kPumpIntervalMs));
				if (g_stop.load(std::memory_order_relaxed)) break;

				if (auto* taskInterface = SKSE::GetTaskInterface()) {
					taskInterface->AddTask([]() { Tick(); });
				}
			}
		}

		// ---- co-save helpers ------------------------------------------------

		template <class T>
		bool ReadPod(SKSE::SerializationInterface* a_intfc, T& a_out)
		{
			return a_intfc->ReadRecordData(a_out) == sizeof(T);
		}

		void WriteString(SKSE::SerializationInterface* a_intfc, const std::string& a_str)
		{
			const std::uint32_t len = static_cast<std::uint32_t>(a_str.size());
			a_intfc->WriteRecordData(len);
			if (len != 0) {
				a_intfc->WriteRecordData(a_str.data(), len);
			}
		}

		bool ReadString(SKSE::SerializationInterface* a_intfc, std::string& a_out)
		{
			std::uint32_t len = 0;
			if (!ReadPod(a_intfc, len)) return false;
			if (len > kMaxStringLen) {
				logs::warn("IntelLite tasks: co-save string length {} over cap", len);
				return false;
			}
			a_out.resize(len);
			if (len != 0 && a_intfc->ReadRecordData(a_out.data(), len) != len) {
				a_out.clear();
				return false;
			}
			return true;
		}

		// Game thread only (SKSE invokes the co-save callbacks on the main thread);
		// g_tasks is not guarded by a mutex.
		void SaveCallback(SKSE::SerializationInterface* a_intfc)
		{
			// The settings record (SETT v1) is written BEFORE the task record.
			// The load loop reads records in any order, but a malformed or
			// truncated TASK record aborts the rest of the record stream - so
			// writing SETT first guarantees the confirmation preference is
			// parsed before that abort and is never lost with the task list.
			// Kept apart from TASK so it survives even when the task list is
			// empty.
			if (!a_intfc->OpenRecord(kRecordSettings, kSettingsVersion)) {
				logs::error("IntelLite tasks: OpenRecord(SETT) failed");
				return;
			}
			const std::uint8_t confirm = g_confirmPrompts.load(std::memory_order_relaxed) ? 1u : 0u;
			a_intfc->WriteRecordData(confirm);

			if (!a_intfc->OpenRecord(kRecordTasks, kTaskVersion)) {
				logs::error("IntelLite tasks: OpenRecord failed");
				return;
			}

			const std::uint32_t count = static_cast<std::uint32_t>(g_tasks.size());
			a_intfc->WriteRecordData(count);
			for (const auto& t : g_tasks) {
				a_intfc->WriteRecordData(t.id);
				a_intfc->WriteRecordData(static_cast<std::uint8_t>(t.type));
				a_intfc->WriteRecordData(static_cast<std::uint8_t>(t.state));
				a_intfc->WriteRecordData(t.agentFormID);
				a_intfc->WriteRecordData(t.targetFormID);
				WriteString(a_intfc, t.agentName);
				WriteString(a_intfc, t.targetName);
				a_intfc->WriteRecordData(t.dueGameTime);
				a_intfc->WriteRecordData(t.createdGameTime);
				a_intfc->WriteRecordData(t.returnWorldspaceFormID);
				a_intfc->WriteRecordData(t.returnX);
				a_intfc->WriteRecordData(t.returnY);
				a_intfc->WriteRecordData(t.returnZ);
				// v2 fields, appended in a fixed order.
				WriteString(a_intfc, t.message);
				a_intfc->WriteRecordData(t.locationFormID);
				a_intfc->WriteRecordData(t.meetGameTime);
				a_intfc->WriteRecordData(t.departGameTime);
				// v3 field, appended in a fixed order.
				WriteString(a_intfc, t.sender);
				// v4 field, appended in a fixed order.
				a_intfc->WriteRecordData(t.spawnedWeaponFormID);
			}
		}

		void LoadCallback(SKSE::SerializationInterface* a_intfc)
		{
			std::vector<Task> loaded;
			bool confirmPrompts = true;  // SETT default when the record is absent

			RestoreAllProtectedFlags();
			g_bountySnapshot.clear();
			g_bountyGuardUntil = 0.0f;
			npc_index::ResetLoaded();
			g_pendingHoldReleases.clear();
			g_pendingWeaponReleases.clear();

			std::uint32_t type = 0;
			std::uint32_t version = 0;
			std::uint32_t length = 0;
			while (a_intfc->GetNextRecordInfo(type, version, length)) {
				// The settings record may sit before or after TASK; read it
				// wherever it appears.
				if (type == kRecordSettings) {
					if (version == kSettingsVersion) {
						std::uint8_t confirm = 1;
						if (ReadPod(a_intfc, confirm)) {
							confirmPrompts = (confirm != 0);
						}
					} else {
						logs::warn("IntelLite tasks: skipping SETT record v{}", version);
					}
					continue;
				}
				if (type != kRecordTasks) continue;
				// Accept every version up to the current one (the newer fields
				// default); skip anything newer/unknown.
				if (version < 1 || version > kTaskVersion) {
					logs::warn("IntelLite tasks: skipping TASK record v{}", version);
					continue;
				}

				std::uint32_t count = 0;
				if (!ReadPod(a_intfc, count)) break;
				if (count > kMaxTasks) {
					logs::warn("IntelLite tasks: co-save task count {} over cap", count);
					break;
				}

				bool truncated = false;
				for (std::uint32_t i = 0; i < count; ++i) {
					Task t;
					std::uint8_t typeByte = 0;
					std::uint8_t stateByte = 0;
					if (!ReadPod(a_intfc, t.id) ||
						!ReadPod(a_intfc, typeByte) ||
						!ReadPod(a_intfc, stateByte) ||
						!ReadPod(a_intfc, t.agentFormID) ||
						!ReadPod(a_intfc, t.targetFormID) ||
						!ReadString(a_intfc, t.agentName) ||
						!ReadString(a_intfc, t.targetName) ||
						!ReadPod(a_intfc, t.dueGameTime) ||
						!ReadPod(a_intfc, t.createdGameTime) ||
						!ReadPod(a_intfc, t.returnWorldspaceFormID) ||
						!ReadPod(a_intfc, t.returnX) ||
						!ReadPod(a_intfc, t.returnY) ||
						!ReadPod(a_intfc, t.returnZ)) {
						logs::warn("IntelLite tasks: truncated task record; aborting load");
						truncated = true;
						break;
					}

					// v2 appends the message and the meet scheduling fields; a
					// v1 record leaves them at their defaults.
					if (version >= 2) {
						if (!ReadString(a_intfc, t.message) ||
							!ReadPod(a_intfc, t.locationFormID) ||
							!ReadPod(a_intfc, t.meetGameTime) ||
							!ReadPod(a_intfc, t.departGameTime)) {
							logs::warn("IntelLite tasks: truncated task record; aborting load");
							truncated = true;
							break;
						}
					}

					// v3 appends the original sender; a v1/v2 record defaults it
					// to the deliverer.
					if (version >= 3) {
						if (!ReadString(a_intfc, t.sender)) {
							logs::warn("IntelLite tasks: truncated task record; aborting load");
							truncated = true;
							break;
						}
					}
					// v4 appends the spawned kill dagger (0 = none); v1-v3 records
					// default it. A non-zero value means we drew it, so the restore
					// marks it as ours for the scene cleanup.
					if (version >= 4) {
						if (!ReadPod(a_intfc, t.spawnedWeaponFormID)) {
							logs::warn("IntelLite tasks: truncated task record; aborting load");
							truncated = true;
							break;
						}
					}
					if (t.sender.empty()) t.sender = t.agentName;

					// A task dropped below never runs its own scene cleanup: stash
					// its spawned dagger so Tick() can remove it after the load.
					// Remap the two ids a dropped task may need to act on BEFORE the
					// drop checks: a dropped task's dagger may only be removed when
					// both its agent and the weapon resolve provably - never through
					// a raw id that could now point at something else.
					bool agentTrusted = false;
					if (t.agentFormID != 0) {
						RE::FormID remappedAgent = 0;
						if (a_intfc->ResolveFormID(t.agentFormID, remappedAgent)) {
							t.agentFormID = remappedAgent;
							agentTrusted = true;
						} else {
							t.agentFormID = 0;
						}
					}
					bool weaponTrusted = false;
					if (t.spawnedWeaponFormID != 0) {
						RE::FormID remappedWeapon = 0;
						if (a_intfc->ResolveFormID(t.spawnedWeaponFormID, remappedWeapon)) {
							t.spawnedWeaponFormID = remappedWeapon;
							weaponTrusted = true;
						} else {
							t.spawnedWeaponFormID = 0;
						}
					}
					t.weaponDrawnByUs = (t.spawnedWeaponFormID != 0);
					auto stashWeaponForRelease = [&t, agentTrusted, weaponTrusted]() {
						if (agentTrusted && weaponTrusted &&
							t.spawnedWeaponFormID != 0 && t.agentFormID != 0) {
							g_pendingWeaponReleases.push_back({ t.agentFormID, t.spawnedWeaponFormID });
						}
					};

					if (typeByte < static_cast<std::uint8_t>(Type::Fetch) ||
						typeByte > static_cast<std::uint8_t>(Type::Assassinate) ||
						stateByte > static_cast<std::uint8_t>(State::Lingering)) {
						logs::warn("IntelLite tasks: task record has unknown type/state ({}/{}) - record skipped",
							typeByte, stateByte);
						stashWeaponForRelease();
						continue;  // the stream is aligned here: every field of this task has been read
					}

					t.type  = static_cast<Type>(typeByte);
					t.state = static_cast<State>(stateByte);

					// Every FormID carries a load-order mod index: remap it or
					// drop the task. A failed resolve means the plugin is gone.
					bool dropped = false;
					auto resolve = [&a_intfc, &dropped](std::uint32_t& a_id) {
						if (a_id == 0) return;
						RE::FormID remapped = 0;
						if (!a_intfc->ResolveFormID(a_id, remapped)) {
							dropped = true;
						} else {
							a_id = remapped;
						}
					};
					resolve(t.targetFormID);
					resolve(t.returnWorldspaceFormID);
					resolve(t.locationFormID);
					if (dropped) {
						stashWeaponForRelease();
						continue;
					}

					if (t.type == Type::Assassinate && t.targetFormID != 0) {
						g_pendingHoldReleases.push_back(t.targetFormID);
					}
					if (t.type == Type::Fetch && t.agentFormID != 0) {
						g_pendingHoldReleases.push_back(t.agentFormID);
					}

					// The actors must still resolve and be alive.
					auto* agent  = RE::TESForm::LookupByID<RE::Actor>(t.agentFormID);
					auto* target = RE::TESForm::LookupByID<RE::Actor>(t.targetFormID);
					if (!agent || !target || agent->IsDead() || target->IsDead()) {
						stashWeaponForRelease();
						continue;
					}
					if (IsFinished(t.state)) {
						stashWeaponForRelease();
						continue;
					}

					// The runtime legs (and the real-time walk budget) do not
					// survive a load, so a resumed task continues on the
					// simulated path with fresh deadlines (preserving a valid
					// future dueGameTime when present). An assassination saved
					// mid-scene (State::Executing) resets to State::AgentTravel
					// because kill-scene state is runtime-only.
					if (t.type == Type::Assassinate && t.state == State::Executing) {
						t.state = State::AgentTravel;
					}

					const float now = Now();
					const float restoredStart = (t.createdGameTime > 0.0f && t.createdGameTime <= now)
						? t.createdGameTime
						: now;
					if (t.state == State::AgentTravel) {
						t.agentLeg = Leg{};
						t.agentLeg.simulated = true;
						t.agentLeg.strictArrival = true;
						t.agentLeg.moverFormID = t.agentFormID;
						t.agentLeg.startGameTime = restoredStart;
						if (t.type == Type::Meet) {
							// A meet's outbound leg goes to its location marker, not
							// to the player it stores as target.
							t.agentLeg.destFormID = t.locationFormID;
							auto* dest = RE::TESForm::LookupByID<RE::TESObjectREFR>(t.locationFormID);
							if (dest) {
								t.agentLeg.destInterior = (dest->GetWorldspace() == nullptr);
								t.agentLeg.destCellFormID = dest->GetParentCell() ? dest->GetParentCell()->GetFormID() : 0;
								t.agentLeg.destWorldspaceFormID = dest->GetWorldspace() ? dest->GetWorldspace()->GetFormID() : 0;
								if (t.agentLeg.destInterior && (!agent || !SameSpace(agent, dest))) {
									if (auto* exitDoor = FindExitDoor(dest, nullptr)) {
										t.placeWalkRefFormID = exitDoor->GetFormID();
										t.agentLeg.walkRefFormID = exitDoor->GetFormID();
										t.agentLeg.entryFormID = dest->GetFormID();
									}
								}
							}
							const float fallbackDue =
								now + HoursToDays(EstimateHours(agent, dest ? dest : static_cast<RE::TESObjectREFR*>(target)));
							t.agentLeg.dueGameTime = (t.dueGameTime > now) ? t.dueGameTime : fallbackDue;
						} else {
							t.agentLeg.destFormID = t.targetFormID;
							t.agentLeg.destInterior = (target->GetWorldspace() == nullptr);
							t.agentLeg.destCellFormID = target->GetParentCell() ? target->GetParentCell()->GetFormID() : 0;
							t.agentLeg.destWorldspaceFormID = target->GetWorldspace() ? target->GetWorldspace()->GetFormID() : 0;
							const float fallbackDue = now + HoursToDays(EstimateHours(agent, target));
							t.agentLeg.dueGameTime = (t.dueGameTime > now) ? t.dueGameTime : fallbackDue;
							// A resumed assassination keeps its approach style: a
							// loaded mortal victim is still approached sneaking.
							if (t.type == Type::Assassinate) {
								t.sneakApproach = target->Is3DLoaded() && !IsTargetEssential(target);
								t.agentLeg.sneak = t.sneakApproach;
							}
						}
						t.dueGameTime = t.agentLeg.dueGameTime;
					}

					if (t.state == State::TargetTravel) {
						// A deliver's legacy return phase or an assassinate refusal
						// return has no target to escort: its target side is
						// already complete by construction.
						if (t.type == Type::Deliver || t.type == Type::Assassinate) {
							t.targetArrived = true;
						}
						if (auto* player = RE::PlayerCharacter::GetSingleton()) {
							const std::uint32_t playerCellID =
								player->GetParentCell() ? player->GetParentCell()->GetFormID() : 0;
							const std::uint32_t playerWsID =
								player->GetWorldspace() ? player->GetWorldspace()->GetFormID() : 0;
							const float agentFallbackDue =
								now + HoursToDays(EstimateHours(agent, player));
							t.agentReturnLeg = Leg{};
							t.agentReturnLeg.simulated = true;
							t.agentReturnLeg.moverFormID = t.agentFormID;
							t.agentReturnLeg.destFormID = player->GetFormID();
							t.agentReturnLeg.startGameTime = restoredStart;
							t.agentReturnLeg.destInterior = (playerWsID == 0);
							t.agentReturnLeg.destCellFormID = playerCellID;
							t.agentReturnLeg.destWorldspaceFormID = playerWsID;
							t.agentReturnLeg.dueGameTime =
								(t.dueGameTime > now) ? t.dueGameTime : agentFallbackDue;

							if (t.type == Type::Fetch) {
								t.agentReturnLeg.strictArrival = true;
								const float targetFallbackDue =
									now + HoursToDays(EstimateHours(target, player));
								t.targetLeg = Leg{};
								t.targetLeg.simulated = true;
								t.targetLeg.strictArrival = true;
								t.targetLeg.moverFormID = t.targetFormID;
								t.targetLeg.destFormID = player->GetFormID();
								t.targetLeg.startGameTime = restoredStart;
								t.targetLeg.destInterior = (playerWsID == 0);
								t.targetLeg.destCellFormID = playerCellID;
								t.targetLeg.destWorldspaceFormID = playerWsID;
								t.targetLeg.dueGameTime =
									(t.dueGameTime > now) ? t.dueGameTime : targetFallbackDue;
								t.dueGameTime = t.targetLeg.dueGameTime;
							} else {
								t.dueGameTime = t.agentReturnLeg.dueGameTime;
							}
						}
					}

					loaded.push_back(std::move(t));
				}
				if (truncated) {
					loaded.clear();
					break;
				}
			}

			g_tasks = std::move(loaded);
			g_pendingCancel.clear();
			g_pendingCancelAgent.clear();
			g_cancelAll = false;
			g_nextID = 1;
			g_confirmPrompts.store(confirmPrompts, std::memory_order_relaxed);
			for (const auto& t : g_tasks) {
				const auto candidate = t.id + 1;
				if (candidate > g_nextID) {
					g_nextID = candidate;
				}
			}
			g_cleanupPersistedTravel = true;
			RefreshSnapshot();
			logs::info("IntelLite tasks: restored {} task(s) from co-save (confirm {})",
				g_tasks.size(), g_confirmPrompts ? 1 : 0);
		}

		void RevertCallback(SKSE::SerializationInterface* /*a_intfc*/)
		{
			RestoreAllProtectedFlags();
			g_bountySnapshot.clear();
			g_bountyGuardUntil = 0.0f;
			npc_index::ResetLoaded();
			g_tasks.clear();
			g_pendingCancel.clear();
			g_pendingCancelAgent.clear();
			g_cancelAll = false;
			g_pendingHoldReleases.clear();
			g_pendingWeaponReleases.clear();
			g_nextID = 1;
			g_confirmPrompts.store(true, std::memory_order_relaxed);  // Revert resets the setting to its default
			RefreshSnapshot();
			logs::info("IntelLite tasks: reverted (state cleared)");
		}

		// Game thread only: rebuild the panel JSON and the eligibility snapshot
		// from g_tasks (every Tick, so the ETAs stay fresh; after every Add*; and
		// after a co-save load/revert).
		void RefreshSnapshot()
		{
			nlohmann::json arr = nlohmann::json::array();
			std::vector<std::uint32_t> activeAgents;
			std::vector<std::uint32_t> cancellableAgents;
			std::vector<std::uint32_t> committedAgents;
			bool anyActive = false;
			for (const auto& t : g_tasks) {
				nlohmann::json row;
				row["id"] = t.id;
				row["type"] = TypeName(t.type);
				row["agent"] = t.agentName;
				row["target"] = t.targetName;
				row["state"] = StateName(t.state);

				// Detail string; travel states get a game-time ETA. Only the atomic
				// cache is read here, so this stays RE::-free. The live legs decide
				// the wording: an actor that is off-screen has already handed to
				// game time and shows the leg's remaining ETA.
				const float now = g_nowCache.load(std::memory_order_relaxed);
				auto etaSuffix = [now](const Leg& a_leg) {
					if (a_leg.deadlineSeenReal > 0.0f || a_leg.deadlineFired) {
						return std::string(" (arriving)");
					}
					float etaHours = (a_leg.dueGameTime - now) * 24.0f;
					if (etaHours <= 0.05f) return std::string(" (arriving)");
					if (etaHours < 0.1f) etaHours = 0.1f;  // floor at 0.1h
					char buf[32]{};
					std::snprintf(buf, sizeof(buf), " (~%.1fh)", etaHours);
					return std::string(buf);
				};

				std::string detail = DetailText(t.state);
				if (t.type == Type::Meet && t.state == State::Pending) {
					float hours = (t.meetGameTime - now) * 24.0f;
					if (hours < 0.0f) hours = 0.0f;
					char buf[64]{};
					std::snprintf(buf, sizeof(buf), "Scheduled to meet in ~%.1fh", hours);
					detail = buf;
				} else if (t.type == Type::Meet && t.state == State::Executing) {
					detail = "Waiting for you at the meeting place";
				} else if (t.type == Type::Assassinate && t.state == State::Executing) {
					detail = "Executing";
				} else if (t.type == Type::Assassinate && t.state == State::TargetTravel) {
					detail = "Returning - the target is essential";
				} else if (t.state == State::AgentTravel) {
					const Leg& leg = t.outboundFailed ? t.agentReturnLeg : t.agentLeg;
					if (leg.active) {
						if (leg.offscreen) {
							detail = "Executor is on the way" + etaSuffix(leg);
						} else if (t.outboundFailed) {
							detail = "Executor is returning";
						} else if (t.type == Type::Assassinate) {
							detail = "Approaching the target";
						} else {
							detail = "Executor is traveling to the target";
						}
					} else {
						detail += etaSuffix(leg);
					}
				} else if (t.state == State::TargetTravel) {
					if (!t.targetArrived) {
						if (t.targetLeg.active) {
							detail = t.targetLeg.offscreen
								? std::string("Target is on the way")
								: std::string("Target is traveling to you");
							if (t.targetLeg.offscreen) detail += etaSuffix(t.targetLeg);
						} else {
							detail += etaSuffix(t.targetLeg);
						}
					} else if (!t.agentDone) {
						if (t.agentReturnLeg.active) {
							detail = t.agentReturnLeg.offscreen
								? std::string("Executor is on the way")
								: std::string("Executor is returning");
							if (t.agentReturnLeg.offscreen) {
								detail += etaSuffix(t.agentReturnLeg);
							}
						} else {
							detail += etaSuffix(t.agentReturnLeg);
						}
					}
				}
				row["detail"] = std::move(detail);
				bool canCancel = !IsFinished(t.state);
				if (t.type == Type::Assassinate && t.killCommitted) canCancel = false;
				row["canCancel"] = canCancel;
				arr.push_back(std::move(row));

				if (!IsFinished(t.state)) {
					anyActive = true;
					if (t.state != State::Lingering) activeAgents.push_back(t.agentFormID);
					const bool committed = (t.type == Type::Assassinate && t.killCommitted);
					if (!committed) {
						cancellableAgents.push_back(t.agentFormID);
					} else {
						committedAgents.push_back(t.agentFormID);
					}
				}
			}
			std::string json = arr.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
			std::lock_guard lock(g_snapshotMutex);
			g_snapshotJson = std::move(json);
			g_activeAgentSnapshot = std::move(activeAgents);
			g_cancellableAgentSnapshot = std::move(cancellableAgents);
			g_committedAgentSnapshot = std::move(committedAgents);
			g_anyActiveSnapshot = anyActive;
		}
	}

	void Initialize()
	{
		bool expected = false;
		if (!g_pumpStarted.compare_exchange_strong(expected, true)) return;

		g_stop.store(false, std::memory_order_relaxed);
		g_pump = std::thread(&PumpLoop);
		g_pump.detach();
		logs::info("IntelLite tasks: pump started ({} ms interval)", kPumpIntervalMs);
	}

	void RegisterSerialization()
	{
		auto* ser = SKSE::GetSerializationInterface();
		if (!ser) {
			logs::error("IntelLite tasks: no serialization interface");
			return;
		}
		ser->SetUniqueID(kSerializationID);
		ser->SetSaveCallback(&SaveCallback);
		ser->SetLoadCallback(&LoadCallback);
		ser->SetRevertCallback(&RevertCallback);
		logs::info("IntelLite tasks: co-save registered (ILIT/TASK v{})", kTaskVersion);
	}

	// Stamp a task's return-home position and worldspace (shared by every
	// action, so a cancelled executor can be put back where it started).
	void CaptureHome(Task& a_task, RE::Actor* agent)
	{
		if (auto* ws = agent->GetWorldspace()) {
			a_task.returnWorldspaceFormID = ws->GetFormID();
		}
		if (auto* cell = agent->GetParentCell()) {
			a_task.returnCellFormID = cell->GetFormID();
		}
		const auto homePos = agent->GetPosition();
		a_task.returnX = homePos.x;
		a_task.returnY = homePos.y;
		a_task.returnZ = homePos.z;
	}

	// Look a stored task up by id and return its pointer (game thread only; the
	// pointer is valid until the next task-list mutation).
	Task* FindById(std::uint32_t a_id)
	{
		for (auto& task : g_tasks) {
			if (task.id == a_id) return &task;
		}
		return nullptr;
	}

	// Game thread only. True when the agent is the executor of a committed
	// assassination (the kill scene has started): such an agent takes no new task.
	bool HasCommittedAssassination(std::uint32_t a_agentFormID)
	{
		for (const auto& t : g_tasks) {
			if (t.type == Type::Assassinate && t.agentFormID == a_agentFormID &&
				t.killCommitted && !IsFinished(t.state)) {
				return true;
			}
		}
		return false;
	}

	bool AddFetch(RE::Actor* agent, RE::Actor* target)
	{
		if (!agent || !target || agent == target) return false;
		if (agent->IsDead() || target->IsDead()) return false;

		if (HasCommittedAssassination(agent->GetFormID())) {
			RE::SendHUDMessage::ShowHUDMessage(
				(DisplayName(agent) + " is in the middle of an assassination and cannot take a new task yet.").c_str());
			return false;
		}

		Task t;
		t.type = Type::Fetch;
		t.state = State::AgentTravel;
		t.agentFormID = agent->GetFormID();
		t.targetFormID = target->GetFormID();
		t.agentName = DisplayName(agent);
		t.targetName = DisplayName(target);
		const float travelHours = EstimateHours(agent, target);
		t.createdGameTime = Now();
		CaptureHome(t, agent);

		const std::string agentName = t.agentName;
		const std::string targetName = t.targetName;

		const std::uint32_t id = StoreTaskAndSupersede(std::move(t));
		if (id == 0) return false;

		if (Task* queued = FindById(id)) {
			StartLeg(queued->agentLeg, agent, target);
			queued->agentLeg.strictArrival = true;
			queued->dueGameTime = queued->agentLeg.dueGameTime;
			if (queued->agentLeg.active) {
				logs::info("task {}: {} walking to {}", id, agentName, targetName);
			} else {
				logs::info("task {}: {} on the way to {} (simulated)", id, agentName, targetName);
			}
		}

		RE::SendHUDMessage::ShowHUDMessage(
			(agentName + " has gone to fetch " + targetName + ".").c_str());
		skyrimnet::RegisterPersistentEvent(
			agentName + " has left to fetch " + targetName + ".", agent,
			RE::PlayerCharacter::GetSingleton());
		PushToPanelFresh();
		logs::info("IntelLite tasks: fetch queued (id {}) - {} -> {} (due in {:.1f}h)",
			id, agentName, targetName, travelHours);
		return true;
	}

	bool AddDeliver(RE::Actor* agent, RE::Actor* recipient, const std::string& message,
		const std::string& sender)
	{
		if (!agent || !recipient || agent == recipient) return false;
		if (agent->IsDead() || recipient->IsDead()) return false;

		if (HasCommittedAssassination(agent->GetFormID())) {
			RE::SendHUDMessage::ShowHUDMessage(
				(DisplayName(agent) + " is in the middle of an assassination and cannot take a new task yet.").c_str());
			return false;
		}

		if (message.empty()) return false;

		Task t;
		t.type = Type::Deliver;
		t.state = State::AgentTravel;
		t.agentFormID = agent->GetFormID();
		t.targetFormID = recipient->GetFormID();
		t.agentName = DisplayName(agent);
		t.targetName = DisplayName(recipient);
		t.message = ClampString(message, kMaxMessageChars);
		// The message's original author: the named sender, or the deliverer
		// when none is given.
		t.sender = sender.empty() ? DisplayName(agent) : ClampString(sender, kMaxSenderChars);
		const float travelHours = EstimateHours(agent, recipient);
		t.createdGameTime = Now();
		CaptureHome(t, agent);

		const std::string agentName = t.agentName;
		const std::string targetName = t.targetName;
		const std::string senderName = t.sender;

		const std::uint32_t id = StoreTaskAndSupersede(std::move(t));
		if (id == 0) return false;

		if (Task* queued = FindById(id)) {
			StartLeg(queued->agentLeg, agent, recipient);
			queued->agentLeg.strictArrival = true;
			queued->dueGameTime = queued->agentLeg.dueGameTime;
			if (queued->agentLeg.active) {
				logs::info("task {}: {} walking to {}", id, agentName, targetName);
			} else {
				logs::info("task {}: {} on the way to {} (simulated)", id, agentName, targetName);
			}
		}

		if (senderName != agentName) {
			RE::SendHUDMessage::ShowHUDMessage(
				(agentName + " sets off to deliver " + senderName + "'s message to " +
					targetName + ".").c_str());
			skyrimnet::RegisterPersistentEvent(
				agentName + " has left to deliver a message from " + senderName + " to " +
					targetName + ".",
				agent, RE::PlayerCharacter::GetSingleton());
		} else {
			RE::SendHUDMessage::ShowHUDMessage(
				(agentName + " sets off to deliver a message to " + targetName + ".").c_str());
			skyrimnet::RegisterPersistentEvent(
				agentName + " has left to deliver a message to " + targetName + ".",
				agent, RE::PlayerCharacter::GetSingleton());
		}
		PushToPanelFresh();
		logs::info("IntelLite tasks: deliver queued (id {}) - {} -> {} (due in {:.1f}h)",
			id, agentName, targetName, travelHours);
		return true;
	}

	namespace
	{
		// Resolve the i-th authored meet-spot marker, cached once for the
		// process like the movement record table. Game thread only.
		RE::TESObjectREFR* MeetSpotMarker(std::size_t a_index)
		{
			static const std::array<RE::TESObjectREFR*, kMeetSpotCount> spots = [] {
				std::array<RE::TESObjectREFR*, kMeetSpotCount> out{};
				if (auto* dataHandler = RE::TESDataHandler::GetSingleton()) {
					for (std::size_t i = 0; i < out.size(); ++i) {
						out[i] = dataHandler->LookupForm<RE::TESObjectREFR>(kMeetSpotIDs[i], kMeetDataPlugin);
					}
				}
				return out;
			}();
			return a_index < spots.size() ? spots[a_index] : nullptr;
		}

		// The first meet-spot marker no existing active task holds (ignoring a
		// replaceable task for `a_agentFormID`, which StoreTaskAndSupersede is
		// about to cancel), or nullptr when all four are leased (or the records
		// are missing). Game thread only: g_tasks is game-thread state.
		RE::TESObjectREFR* FindFreeMeetSpot(std::uint32_t a_agentFormID)
		{
			for (std::size_t i = 0; i < kMeetSpotCount; ++i) {
				auto* spot = MeetSpotMarker(i);
				if (!spot) continue;
				const auto spotID = spot->GetFormID();
				bool used = false;
				for (const auto& task : g_tasks) {
					if (IsFinished(task.state)) continue;
					if (task.agentFormID == a_agentFormID &&
						task.state != State::Lingering &&
						!AssassinationCommitted(task)) {
						continue;  // about to be superseded by this agent's new meet
					}
					if (task.locationFormID == spotID) {
						used = true;
						break;
					}
				}
				if (!used) return spot;
			}
			return nullptr;
		}
	}

	// The player's current venue as a display label: the interior cell's name
	// when indoors, else the current location's name, else "your location".
	// Game thread only.
	std::string PlayerVenueLabel()
	{
		if (auto* player = RE::PlayerCharacter::GetSingleton()) {
			if (auto* cell = player->GetParentCell(); cell && cell->IsInteriorCell()) {
				if (const char* n = cell->GetFullName(); n && *n) {
					return ClampString(n, kMaxNameChars);
				}
			}
			if (auto* loc = player->GetCurrentLocation()) {
				if (const char* n = loc->GetFullName(); n && *n) {
					return ClampString(n, kMaxNameChars);
				}
			}
		}
		return std::string("your location");
	}

	bool AddMeet(RE::Actor* agent, const std::string& locationName, int hoursFromNow)
	{
		if (!agent || agent->IsDead()) return false;

		if (HasCommittedAssassination(agent->GetFormID())) {
			RE::SendHUDMessage::ShowHUDMessage(
				(DisplayName(agent) + " is in the middle of an assassination and cannot take a new task yet.").c_str());
			return false;
		}

		if (locationName.empty()) return false;

		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) return false;

		// Resolve the destination: "Here" leases a meet-spot marker and moves
		// it to the player's current spot (a fixed destination); any other
		// name is a named place (a BGSLocation, or an interior cell), found
		// fuzzily. A place inside a building resolves to its interior door
		// (the final ref) and its exterior door (the walk ref).
		const bool here = (locationName == "Here");
		RE::TESObjectREFR* dest = nullptr;
		RE::TESObjectREFR* walkRef = nullptr;
		std::string display;
		if (here) {
			// "Here" = the spot the player stands on when the arrangement is
			// made. Lease a persistent marker and move it there, so the meet
			// travels to a fixed point instead of following the player.
			if (RE::TESObjectREFR* spot = FindFreeMeetSpot(agent->GetFormID())) {
				spot->MoveTo(player);
				spot->SetPosition(player->GetPosition());
				dest = spot;
				display = PlayerVenueLabel();
				logs::info("IntelLite tasks: meet spot marker {:08X} leased for {}",
					spot->GetFormID(), DisplayName(agent));
			} else if (!MeetSpotMarker(0)) {
				// No marker records resolved (IntelLiteData.esp missing) -
				// degrade to the player as the destination.
				logs::warn("IntelLite tasks: meet spot markers missing - meeting at the player instead");
				dest = player;
				display = PlayerVenueLabel();
			} else {
				AnnounceMeetNoSpot(agent);
				logs::warn("IntelLite tasks: meet refused for {} - all meeting places are in use", DisplayName(agent));
				return false;
			}
		} else {
			places::Place place;
			if (places::Resolve(locationName, place)) {
				const std::uint32_t finalID =
					place.entryFormID ? place.entryFormID : place.anchorFormID;
				dest = RE::TESForm::LookupByID<RE::TESObjectREFR>(finalID);
				if (place.entryFormID) {
					walkRef = RE::TESForm::LookupByID<RE::TESObjectREFR>(place.anchorFormID);
				}
				display = place.name.empty() ? locationName : place.name;
			}
		}

		if (!dest) {
			RE::SendHUDMessage::ShowHUDMessage(
				(DisplayName(agent) + " cannot find a place called " + locationName + ".").c_str());
			logs::warn("IntelLite tasks: meet refused - no place resolved for {}", locationName);
			return false;
		}

		if (hoursFromNow < 1) hoursFromNow = 1;
		if (hoursFromNow > 48) hoursFromNow = 48;

		const float now = Now();
		Task t;
		t.type = Type::Meet;
		t.state = State::Pending;
		t.agentFormID = agent->GetFormID();
		t.targetFormID = player->GetFormID();
		t.locationFormID = dest->GetFormID();
		t.placeWalkRefFormID = walkRef ? walkRef->GetFormID() : 0;
		t.agentName = DisplayName(agent);
		t.targetName = ClampString(display, kMaxNameChars);
		t.createdGameTime = now;
		t.meetGameTime = now + HoursToDays(static_cast<float>(hoursFromNow));
		t.departGameTime = t.meetGameTime - HoursToDays(EstimateHours(agent, dest));
		if (t.departGameTime < now) t.departGameTime = now;
		CaptureHome(t, agent);

		const std::string agentName = t.agentName;
		const std::string place = t.targetName;
		const float departGameTime = t.departGameTime;
		const float meetGameTime = t.meetGameTime;

		const std::uint32_t id = StoreTaskAndSupersede(std::move(t));
		if (id == 0) return false;

		const std::string hudPlace = place;
		RE::SendHUDMessage::ShowHUDMessage(
			(agentName + " agrees to meet you at " + hudPlace + ".").c_str());
		// A persistent event records the agreement, and the per-actor state key
		// lets the character-bio submodule remind the agent of it until the meet
		// completes, times out, or is cancelled.
		skyrimnet::RegisterPersistentEvent(
			agentName + " has agreed to meet " + PlayerName() + " at " + hudPlace +
				" in " + std::to_string(hoursFromNow) +
				(hoursFromNow == 1 ? " hour." : " hours."),
			agent, player);
		SetActorStateKey(agent, kMeetingStateKey, hudPlace);
		skyrimnet::SetActorFloat(agent, kMeetingTimeKey, meetGameTime);
		PushToPanelFresh();
		logs::info("IntelLite tasks: meet queued (id {}) - {} at {} in {}h (departs {:.2f}, meets {:.2f})",
			id, agentName, place, hoursFromNow, departGameTime, meetGameTime);
		return true;
	}

	bool AddAssassinate(RE::Actor* agent, RE::Actor* target, std::uint32_t fee)
	{
		if (!agent || !target || agent == target) return false;
		if (agent->IsDead() || target->IsDead()) return false;

		if (HasCommittedAssassination(agent->GetFormID())) {
			RE::SendHUDMessage::ShowHUDMessage(
				(DisplayName(agent) + " is in the middle of an assassination and cannot take a new task yet.").c_str());
			return false;
		}

		if (target->Is3DLoaded() && !agent->Is3DLoaded()) {
			RE::SendHUDMessage::ShowHUDMessage(
				(DisplayName(agent) + " is too far away to reach " + DisplayName(target) +
					" - a target in view needs an executor on the scene.").c_str());
			logs::info("IntelLite tasks: assassination refused - target {} is 3D-loaded but executor {} is off-screen",
				DisplayName(target), DisplayName(agent));
			return false;
		}

		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) return false;

		// Refuse up front when the task cap is full, so the fee is never charged
		// for a task that cannot be stored. Single game-thread writer, so this
		// check is reliable.
		if (g_tasks.size() >= kMaxTasks) {
			logs::warn("IntelLite tasks: task cap reached; assassination refused");
			return false;
		}

		if (!PayFee(player, agent, fee)) {
			RE::SendHUDMessage::ShowHUDMessage("You cannot afford the fee.");
			logs::warn("IntelLite tasks: assassination refused - cannot afford {} gold", fee);
			return false;
		}

		Task t;
		t.type = Type::Assassinate;
		t.state = State::AgentTravel;
		t.agentFormID = agent->GetFormID();
		t.targetFormID = target->GetFormID();
		t.agentName = DisplayName(agent);
		t.targetName = DisplayName(target);
		const float travelHours = EstimateHours(agent, target);
		t.createdGameTime = Now();
		CaptureHome(t, agent);

		const std::string agentName = t.agentName;
		const std::string targetName = t.targetName;

		const std::uint32_t id = StoreTaskAndSupersede(std::move(t));
		if (id == 0) return false;

		// The hire is recorded: a persistent event the executor and the player
		// know (the victim is excluded), plus a per-actor contract state key
		// (cleared on success, failure, or cancel).
		SetActorStateKey(agent, kContractStateKey,
			targetName + " (for " + std::to_string(fee) + " gold)");
		skyrimnet::RegisterPersistentEvent(
			agentName + " was hired to assassinate " + targetName + " and sets out now.",
			agent, nullptr, { agent, player });

		if (Task* queued = FindById(id)) {
			const bool essential = IsTargetEssential(target);
			// The contract treats only ESSENTIAL as unkillable: a PROTECTED
			// target's flag is cleared the moment the executor sets out - not
			// at the kill - so the engine's own killmove kill lands as a real
			// kill (no bleedout, no deferred death). It is restored if the
			// contract ends without a kill.
			if (!essential && IsTargetProtected(target)) {
				target->GetActorRuntimeData().boolFlags.reset(RE::Actor::BOOL_FLAGS::kProtected);
				queued->protectedCleared = true;
				queued->targetBaseProtected = BaseIsProtected(target);
				queued->targetAliasProtected = AliasIsProtected(target);
				SetBaseProtected(target, false);
				SetAliasProtected(target, false);
				logs::info("task {}: {} was protected - flag cleared for the contract",
					id, targetName);
			}
			const bool pathB = target->Is3DLoaded();
			// The one-package essential run: an essential victim that is
			// already 3D-loaded is attacked with the swing package applied at
			// contract creation. The package carries its own travel phase, so
			// it runs the executor to the target AND attacks, then the normal
			// attack-completion detection releases it. Its template keeps the
			// weapon drawn, so no separate drawn-run package is needed. This
			// path starts no travel leg at all.
			const bool essentialSwing = essential && pathB;
			// A furniture-bound target is driven off it NOW, before the
			// executor sets out: it gets up and walks toward the player, so
			// the executor intercepts a moving target on open ground and the
			// killmove plays like it does for any walker. The get-up animation
			// is given a beat; a later travel tick finishes the drive. The
			// one-package essential run is the exception: the executor is
			// already inbound under its own package, so the target must not
			// walk away.
			if (!essential) {
				(void)DriveTargetOffFurniture(*queued, target, player, true);
				// A target that is not 3D-loaded cannot take the walk package (it
				// would start walking blind when it loads). Defer the drive so
				// AdvanceAgentTravel runs it on the first live-approach tick where
				// the target is loaded. Essentials are excluded: they use the
				// cosmetic swing, not the killmove.
				queued->furnitureDrivePending = !pathB;
			}
			if (essentialSwing) {
				EnsureKillDagger(*queued, agent);
				BeginBountyGuard();
				if (movement::StartSwing(agent, target)) {
					logs::info("task {}: {} goes straight for {} (essential swing)", id, agentName, targetName);
				} else {
					logs::warn("task {}: swing package unavailable - essential swing skipped for {}", id, targetName);
				}
				queued->swingPending = true;
				queued->swingSawAttack = false;
				queued->swingAttackSeenReal = 0.0f;
				queued->swingCheckReal = 0.0f;
				queued->swingEndReal = RealSeconds() + kSwingSeconds;
				queued->swingReissues = 0;
				queued->swingReissueReal = RealSeconds() + kSwingReissueSeconds;
				queued->killCommitted = SameSpace(agent, target) &&
					agent->GetPosition().GetDistance(target->GetPosition()) < 400.0f;
				queued->state = State::Executing;
			} else {
				// Path B (the victim is 3D-loaded): the dagger comes first, then
				// the sneak approach package. Path A (unloaded): the shared
				// travel leg runs the executor out and the game-time simulation
				// completes the trip. An ESSENTIAL target that reaches here is
				// Path A only: it is never sneaked up on - it takes the plain
				// non-sneak run package, exactly like the fetch/deliver outbound
				// legs do, because the swing hand-off starts the moment the
				// executor arrives and must not fight a stealth approach.
				const bool sneak = pathB && !essential;
				queued->sneakApproach = sneak;
				if (pathB) {
					EnsureKillDagger(*queued, agent);
				}
				StartLeg(queued->agentLeg, agent, target, nullptr, sneak);
				queued->dueGameTime = queued->agentLeg.dueGameTime;
				// The assassinate approach must actually reach the target: the
				// shared-interior parent-cell shortcut would report Arrived from
				// across the cell and the staged gate (<400u) would then refuse.
				queued->agentLeg.strictArrival = true;
				if (queued->agentLeg.active) {
					if (sneak) {
						logs::info("task {}: {} stalks {} (sneak approach)", id, agentName, targetName);
					} else {
						logs::info("task {}: {} walking to {}", id, agentName, targetName);
					}
				} else {
					logs::info("task {}: {} on the way to {} (simulated)", id, agentName, targetName);
				}
			}
		}

		RE::SendHUDMessage::ShowHUDMessage(
			(agentName + " heads out after " + targetName + ".").c_str());
		PushToPanelFresh();
		logs::info("IntelLite tasks: assassinate queued (id {}) - {} -> {} for {} gold (due in {:.1f}h)",
			id, agentName, targetName, fee, travelHours);
		return true;
	}

	void Cancel(std::uint32_t id)
	{
		std::lock_guard lock(g_cmdMutex);
		if (g_pendingCancel.size() < kMaxPending) {
			g_pendingCancel.push_back(id);
		}
	}

	void CancelForAgent(std::uint32_t agentFormID)
	{
		std::lock_guard lock(g_cmdMutex);
		if (g_pendingCancelAgent.size() < kMaxPending) {
			g_pendingCancelAgent.push_back(agentFormID);
		}
	}

	void CancelAll()
	{
		std::lock_guard lock(g_cmdMutex);
		g_cancelAll = true;
	}

	bool HasActiveTask(std::uint32_t agentFormID)
	{
		std::lock_guard lock(g_snapshotMutex);
		for (const auto id : g_activeAgentSnapshot) {
			if (id == agentFormID) return true;
		}
		return false;
	}

	bool HasAnyActiveTask()
	{
		std::lock_guard lock(g_snapshotMutex);
		return g_anyActiveSnapshot;
	}

	bool HasCancellableTask(std::uint32_t agentFormID)
	{
		std::lock_guard lock(g_snapshotMutex);
		for (const auto id : g_cancellableAgentSnapshot) {
			if (id == agentFormID) return true;
		}
		return false;
	}

	bool HasCommittedKillTask(std::uint32_t agentFormID)
	{
		std::lock_guard lock(g_snapshotMutex);
		for (const auto id : g_committedAgentSnapshot) {
			if (id == agentFormID) return true;
		}
		return false;
	}

	bool ConfirmPromptsEnabled()
	{
		return g_confirmPrompts.load(std::memory_order_relaxed);
	}

	void SetConfirmPromptsEnabled(bool a_enabled)
	{
		g_confirmPrompts.store(a_enabled, std::memory_order_relaxed);
	}

	void Tick()
	{
		if (!g_firstTickLogged) {
			g_firstTickLogged = true;
			logs::info("IntelLite tasks: first tick on game thread");
		}
		g_nowCache.store(Now(), std::memory_order_relaxed);

		// Keep the bounty guard alive across ticks: the kill's own crime event can
		// land after the task completes, so the increase is cleared until the
		// guard window expires.
		GuardBounty();

		bool changed = false;

		// A load leaves the runtime travel state stale, but the actors may
		// still carry a package override / linked ref persisted in the save
		// from a walk that was live when it was written. Release both legs
		// once, on the first tick after the load, before the advance loop;
		// the fetch then resumes on the simulated path set up in
		// LoadCallback. Only the backend is stopped here - the restored Leg
		// state must survive. (Stopping an actor with no travel is a
		// harmless no-op.)
		if (g_cleanupPersistedTravel) {
			for (auto& t : g_tasks) {
				auto* agent = RE::TESForm::LookupByID<RE::Actor>(t.agentFormID);
				movement::StopTravel(agent);
				if (t.type == Type::Fetch) ClearTargetHold(agent);
				auto* target = RE::TESForm::LookupByID<RE::Actor>(t.targetFormID);
				movement::StopTravel(target);
				// A SetDontMove hold is saved on the actor and must not
				// survive a reload (the runtime flag that would release it
				// does not).
				if (t.type == Type::Assassinate) ClearTargetHold(target);
				// A persisted linger cannot resume its sandbox: release it
				// and finish the task (one-shot release after a load).
				if (t.state == State::Lingering) {
					ReleaseActor(target);
					t.state = State::Done;
				}
				// A fetch saved mid-return may already carry the arrival
				// sandbox (the runtime flag does not survive a save): release
				// it before the return re-resolves and re-applies it.
				if (t.type == Type::Fetch && t.state == State::TargetTravel) {
					ReleaseActor(target);
				}
				// A persisted meet waiting at the meeting place likewise
				// cannot resume its sandbox: release it, but let the meet
				// still complete on arrival or its grace timer.
				if (t.type == Type::Meet && t.state == State::Executing) {
					movement::StopSandbox(RE::TESForm::LookupByID<RE::Actor>(t.agentFormID));
				}
			}
			for (const auto id : g_pendingHoldReleases) {
				ClearTargetHold(RE::TESForm::LookupByID<RE::Actor>(id));
			}
			g_pendingHoldReleases.clear();
			for (const auto& pw : g_pendingWeaponReleases) {
				ReleasePendingWeapon(pw.agent, pw.weapon);
			}
			g_pendingWeaponReleases.clear();
			if (!g_tasks.empty()) changed = true;
			g_cleanupPersistedTravel = false;
			logs::info("IntelLite tasks: cleared persisted travel overrides after load");
		}

		// Apply cancellations queued from other threads. The queue is protected
		// by the command mutex; the application runs here on the game thread,
		// where g_tasks lives.
		bool cancelAll = false;
		std::vector<std::uint32_t> pending;
		std::vector<std::uint32_t> pendingAgents;
		{
			std::lock_guard lock(g_cmdMutex);
			cancelAll = g_cancelAll;
			g_cancelAll = false;
			pending.swap(g_pendingCancel);
			pendingAgents.swap(g_pendingCancelAgent);
		}
		if (cancelAll) {
			for (auto& t : g_tasks) {
				if (!IsFinished(t.state) && CancelTask(t, true)) {
					logs::info("task {}: cancelled", t.id);
				}
			}
		}
		if (!pending.empty()) {
			for (const auto id : pending) {
				for (auto& t : g_tasks) {
					if (t.id == id && !IsFinished(t.state) && CancelTask(t, true)) {
						logs::info("task {}: cancelled", t.id);
					}
				}
			}
			changed = true;
		}
		if (!pendingAgents.empty()) {
			for (const auto agentID : pendingAgents) {
				for (auto& t : g_tasks) {
					if (t.agentFormID == agentID && !IsFinished(t.state) && CancelTask(t, true)) {
						logs::info("task {}: cancelled", t.id);
					}
				}
				changed = true;
			}
		}
		if (cancelAll) changed = true;

		for (auto& t : g_tasks) {
			// Snapshot the display flags so a leg going off-screen (or
			// coming back) pushes the panel even when the task state itself
			// did not change.
			const bool offA = t.agentLeg.offscreen;
			const bool offB = t.agentReturnLeg.offscreen;
			const bool offC = t.targetLeg.offscreen;

			if (t.state == State::Pending) {
				if (AdvancePending(t)) changed = true;
			} else if (t.state == State::AgentTravel) {
				if (AdvanceAgentTravel(t)) changed = true;
			} else if (t.state == State::TargetTravel) {
				if (AdvanceTargetTravel(t)) changed = true;
			} else if (t.state == State::Executing) {
				if (AdvanceExecuting(t)) changed = true;
			} else if (t.state == State::Lingering) {
				if (AdvanceLingering(t)) changed = true;
			}

			if (offA != t.agentLeg.offscreen ||
				offB != t.agentReturnLeg.offscreen ||
				offC != t.targetLeg.offscreen) {
				changed = true;
			}
		}

		// The engine can re-assert a protected target's flag while the
		// contract runs (it is re-set mid-contract). Keep it cleared for the
		// life of the contract so the
		// killmove's own kill lands as a real kill.
		for (auto& t : g_tasks) {
			if (t.type != Type::Assassinate || !t.protectedCleared || IsFinished(t.state)) continue;
			auto* target = RE::TESForm::LookupByID<RE::Actor>(t.targetFormID);
			if (!target || target->IsDead() || !IsTargetProtected(target)) continue;
			target->GetActorRuntimeData().boolFlags.reset(RE::Actor::BOOL_FLAGS::kProtected);
			SetBaseProtected(target, false);
			SetAliasProtected(target, false);
			if (!t.protectedReCleared) {
				t.protectedReCleared = true;
				logs::info("task {}: {} was re-protected - flag cleared again",
					t.id, t.targetName);
			}
		}

		// A finished contract restores its target's protected flag (the
		// early clear must not outlive the contract), releases a target
		// still walking clear of its furniture back to its normal AI, and
		// releases a target held in the kill sandbox.
		for (auto& t : g_tasks) {
			if (!IsFinished(t.state)) continue;
			if (t.protectedCleared) {
				RestoreProtectedFlag(t);
			}
			if (t.killWalkPending) {
				movement::StopTravel(RE::TESForm::LookupByID<RE::Actor>(t.targetFormID));
				t.killWalkPending = false;
			}
			if (t.targetSandboxed) {
				ReleaseActor(RE::TESForm::LookupByID<RE::Actor>(t.targetFormID));
				t.targetSandboxed = false;
			}
			if (t.targetHeld) {
				ReleaseTargetHold(t, RE::TESForm::LookupByID<RE::Actor>(t.targetFormID));
			}
			// A contract that ended without reaching its scene cleanup (cancelled,
			// superseded) still owns the dagger it spawned: undo it here - both
			// helpers are idempotent, so a scene that already cleaned up no-ops.
			if (t.type == Type::Assassinate) {
				auto* daggerAgent = RE::TESForm::LookupByID<RE::Actor>(t.agentFormID);
				if (t.swingPending) {
					movement::StopSwing(daggerAgent);
					t.swingPending = false;
				}
				CleanupKillDagger(t, daggerAgent);
				ReleaseSneak(t, daggerAgent);
			}
		}

		const auto before = g_tasks.size();
		g_tasks.erase(
			std::remove_if(g_tasks.begin(), g_tasks.end(),
				[](const Task& t) { return IsFinished(t.state); }),
			g_tasks.end());
		if (g_tasks.size() != before) changed = true;

		// The snapshot is refreshed every tick (the ETAs and the eligibility
		// answers move even when no state changed); the panel is pushed only on a
		// real change.
		RefreshSnapshot();
		if (changed) PushToPanel();
	}

	std::string ListJson()
	{
		std::lock_guard lock(g_snapshotMutex);
		return g_snapshotJson;
	}

	void PushToPanel()
	{
		panel::PushTasks(ListJson());
	}

	void PushToPanelFresh()
	{
		RefreshSnapshot();
		panel::PushTasks(ListJson());
	}
}
