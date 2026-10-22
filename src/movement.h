// Portions of this file are adapted from IntelEngine-NativePlugin
// (Copyright 2026 Galanx), licensed under the Apache License 2.0.
// Changed for IntelLite. See THIRD_PARTY_NOTICES.md and NOTICE.

#pragma once

// IntelLite's real-walking movement backend (package-based, IntelEngine model).
//
// Drives a task actor along a cloned Travel package:
//   - point the actor's `IntelLite_TravelTarget` linked ref at the destination
//     (PO3_SKSEFunctions.SetLinkedRef via the Papyrus VM; TESObjectREFR exposes
//     no native SetLinkedRef/ClearLinkedRef, only ExtraDataList::SetLinkedRef);
//   - push `IntelLite_TravelPackage_Run` as a package override through
//     PapyrusUtil's ActorUtil.AddPackageOverride(actor, pkg, 100, 1);
//   - force an AI re-evaluation (Actor::EvaluatePackage(true, false)).
// StopTravel reverses all three: RemovePackageOverride, clear the keyworded
// linked ref, then EvaluatePackage(true, false).
//
// StartSandbox/StopSandbox are the same mechanism with the sandbox package
// (`IntelLite_SandboxNearPlayer`, priority 95 - above SkyrimNet's packages, below the travel package): StartSandbox anchors the actor's
// linked ref at a reference and pushes the 200-unit sandbox, so a fetched target
// lingers near the player; StopSandbox releases it.
//
// The records live in IntelLiteData.esp (keyword 000800, meet spots
// 000801-000804, travel package 000805, sandbox package 000806, sneak approach
// package 000807, target walk package 000808, swing package 000809). The
// package is a clone of IntelEngine's travel package: vanilla Travel template
// (016FAA) with location = the keyword-linked reference and PreferredSpeed Run;
// its UnlockDoorsAtPackageStart flag is unset so a locked building cannot be
// opened by the walk. Priority 100 and flags 1 match IntelEngine's
// PRIORITY_TRAVEL.
//
// Everything here is GAME THREAD ONLY: it touches RE:: and the Papyrus VM. The
// task engine falls back to simulated travel whenever this backend is
// unavailable or a call fails.

namespace RE
{
	class Actor;
	class TESObjectREFR;
}

namespace intellite::movement
{
	// True when IntelLiteData.esp resolves and both the travel keyword and the
	// travel package were found. Resolves once and logs the verdict exactly once
	// ("movement: IntelLiteData.esp loaded - package travel enabled" /
	//  "movement: IntelLiteData.esp missing - simulation only").
	bool IsAvailable();

	// Start `mover` walking to `destination`: set the linked ref, push the
	// travel package override, then force EvaluatePackage(true, false). `mover`
	// is an Actor (the package override is applied to an Actor); `destination`
	// is any reference. Game thread only. Returns true iff the forms resolved
	// and both dispatches were accepted; false means the caller should fall
	// back to simulation. Dispatch failures are logged once. `sneak` selects
	// the sneak approach package (000807: walk speed, AlwaysSneak +
	// WeaponDrawn) instead of the run package; when that record is missing the
	// run package is used. `walk` selects the slow walk package (000808) - the
	// furniture walk-away an assassination gives a target that is on furniture;
	// when that record is missing the run package is used.
	bool StartTravel(RE::Actor* mover, RE::TESObjectREFR* destination, bool sneak = false, bool walk = false);

	// The resolved travel package's FormID for the given mode (approach when
	// sneak, walk when walk, else run), or 0 when the record did not resolve.
	// Game-thread only (reads the cached ResolveForms result).
	RE::FormID TravelPackageFormID(bool sneak = false, bool walk = false);

	// Release a mover previously started with StartTravel: remove the package
	// override, clear the keyworded linked ref, then force
	// EvaluatePackage(true, false). Game thread only.
	void StopTravel(RE::Actor* mover);

	// Start `attacker` swinging at `victim`: point the keyworded linked ref at
	// the victim, push the swing package override (000809, priority 100,
	// flags 1), then force EvaluatePackage(true, false). The package fires one
	// cosmetic power attack and never kills, so an essential staged target is
	// never left in a broken killmove state. Game thread only. Returns true iff
	// the forms resolved and both dispatches were accepted; false means the
	// caller should skip the swing. Dispatch failures are logged once.
	bool StartSwing(RE::Actor* attacker, RE::TESObjectREFR* victim);

	// The resolved swing package's FormID, or 0 when the record did not resolve.
	// The task engine's swing watchdog compares this against the package an
	// actor is actually running to tell whether the swing package has engaged
	// yet. Game-thread only (reads the cached ResolveForms result).
	RE::FormID SwingPackageFormID();

	// Release an attacker previously started with StartSwing: remove the swing
	// package override, clear the keyworded linked ref, then force
	// EvaluatePackage(true, false). Game thread only.
	void StopSwing(RE::Actor* attacker);

	// Start `actor` sandboxing around `anchor`: set the linked ref at the
	// anchor, push the sandbox package override (priority 95, flags 1), then
	// force EvaluatePackage(true, false). Game thread only. Returns true iff the
	// forms resolved and both dispatches were accepted; false otherwise.
	bool StartSandbox(RE::Actor* actor, RE::TESObjectREFR* anchor);

	// Release an actor previously started with StartSandbox: remove the sandbox
	// package override, clear the keyworded linked ref, then force
	// EvaluatePackage(true, false). Game thread only.
	void StopSandbox(RE::Actor* actor);
}
