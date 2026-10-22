// Portions of this file are adapted from IntelEngine-NativePlugin
// (Copyright 2026 Galanx), licensed under the Apache License 2.0.
// Changed for IntelLite. See THIRD_PARTY_NOTICES.md and NOTICE.

// IntelLite's real-walking movement backend (package-based, IntelEngine model).
//
// Travel is driven entirely through
// the Papyrus VM: PO3_SKSEFunctions.SetLinkedRef points the actor's keyworded
// linked ref at the destination, and PapyrusUtil's ActorUtil.AddPackageOverride
// pushes the cloned Travel package. All RE::/VM access is game-thread only.
//
// A missing IntelLiteData.esp, a missing VM or a failed dispatch each degrade
// to "return false" (or a no-op for StopTravel) with at most one explanatory
// log line - the task engine then uses the time-simulated teleport fallback.
// Nothing here crashes on absence.

#include "movement.h"
#include "vm_dispatch.h"

#include <atomic>
#include <cstdint>

namespace intellite::movement
{
	namespace
	{
		constexpr const char* kDataPlugin = "IntelLiteData.esp";
		constexpr RE::FormID  kKeywordLocalID = 0x800;  // IntelLite_TravelTarget
		constexpr RE::FormID  kPackageLocalID = 0x805;  // IntelLite_TravelPackage_Run
		constexpr RE::FormID  kSandboxLocalID = 0x806;  // IntelLite_SandboxNearPlayer
		constexpr RE::FormID  kApproachLocalID = 0x807; // IntelLite_AssassinApproachPackage
		constexpr RE::FormID  kWalkPackageLocalID = 0x808; // IntelLite_TargetWalkPackage
		constexpr RE::FormID  kSwingPackageLocalID = 0x809; // IntelLite_SwingPackage

		constexpr const char* kActorUtilClass         = "ActorUtil";
		constexpr const char* kAddPackageOverrideFn   = "AddPackageOverride";
		constexpr const char* kRemovePackageOverrideFn= "RemovePackageOverride";

		// IntelEngine's PRIORITY_TRAVEL is 100 and its AddPackageOverride flags
		// literal is 1; both are repeated here verbatim. The sandbox override
		// sits just below the travel package at priority 95: comfortably above
		// SkyrimNet's own packages (TalkToPlayer registers at 1, FollowPlayer at
		// 10 in its shipped scripts), so a narration cannot displace the linger
		// sandbox, and still below our travel package so travel always wins when
		// both are present.
		constexpr std::int32_t kTravelPriority  = 100;
		constexpr std::int32_t kSandboxPriority = 95;
		constexpr std::int32_t kTravelFlags     = 1;

		struct Forms
		{
			RE::BGSKeyword* keyword = nullptr;
			RE::TESPackage* package = nullptr;
			RE::TESPackage* sandbox = nullptr;
			RE::TESPackage* approach = nullptr;
			RE::TESPackage* walk = nullptr;
			RE::TESPackage* swing = nullptr;
		};

		// Resolved once for the process; the log line fires exactly once.
		const Forms& ResolveForms()
		{
			static const Forms forms = [] {
				Forms f;
				if (auto* dataHandler = RE::TESDataHandler::GetSingleton()) {
					f.keyword = dataHandler->LookupForm<RE::BGSKeyword>(kKeywordLocalID, kDataPlugin);
					f.package = dataHandler->LookupForm<RE::TESPackage>(kPackageLocalID, kDataPlugin);
					f.sandbox = dataHandler->LookupForm<RE::TESPackage>(kSandboxLocalID, kDataPlugin);
					f.approach = dataHandler->LookupForm<RE::TESPackage>(kApproachLocalID, kDataPlugin);
					f.walk = dataHandler->LookupForm<RE::TESPackage>(kWalkPackageLocalID, kDataPlugin);
					f.swing = dataHandler->LookupForm<RE::TESPackage>(kSwingPackageLocalID, kDataPlugin);
				}
				if (f.keyword && f.package) {
					// Match the run package's interrupt-blocking flags (0xFEFF) on the
					// sneak approach and target walk packages so corpses, hellos, or
					// world interactions cannot interrupt an active approach or walk.
					const auto interruptFlags = f.package->packData.foBehaviorFlags;
					if (f.approach) {
						f.approach->packData.foBehaviorFlags = interruptFlags;
					}
					if (f.walk) {
						f.walk->packData.foBehaviorFlags = interruptFlags;
					}
					logs::info("movement: IntelLiteData.esp loaded - package travel enabled");
					if (!f.sandbox) {
						logs::warn("movement: IntelLiteData.esp sandbox package missing - linger disabled");
					}
					if (!f.approach) {
						logs::warn("movement: approach package (0x807) missing - sneak approach falls back to the run package");
					}
					if (!f.walk) {
						logs::warn("movement: target walk package (0x808) missing - the furniture walk falls back to the run package");
					}
					if (!f.swing) {
						logs::warn("movement: swing package (0x809) missing - the essential swing is skipped");
					}
				} else {
					logs::info("movement: IntelLiteData.esp missing - simulation only");
				}
				return f;
			}();
			return forms;
		}

		std::atomic<bool> g_startFailureLogged{ false };

		void LogStartFailureOnce()
		{
			bool expected = false;
			if (g_startFailureLogged.compare_exchange_strong(expected, true)) {
				logs::warn("movement: start dispatch failed - using simulated travel");
			}
		}
	}

	bool IsAvailable()
	{
		const auto& forms = ResolveForms();
		return forms.keyword != nullptr && forms.package != nullptr;
	}

	RE::FormID TravelPackageFormID(bool sneak, bool walk)
	{
		const auto& forms = ResolveForms();
		if (!forms.package) {
			return 0;
		}
		if (sneak && forms.approach) {
			return forms.approach->GetFormID();
		}
		if (walk && forms.walk) {
			return forms.walk->GetFormID();
		}
		return forms.package->GetFormID();
	}

	bool StartTravel(RE::Actor* mover, RE::TESObjectREFR* destination, bool sneak, bool walk)
	{
		if (!mover || !destination || mover == destination) {
			return false;
		}

		const auto& forms = ResolveForms();
		if (!forms.keyword || !forms.package) {
			return false;
		}

		// A sneak approach uses the dedicated walk/sneak package and the
		// furniture walk-away its own slow walk package; both degrade to the
		// run package when the record is missing (an older IntelLiteData.esp).
		RE::TESPackage* package = forms.package;
		if (sneak && forms.approach) {
			package = forms.approach;
		} else if (walk && forms.walk) {
			package = forms.walk;
		}

		// Point the keyworded linked ref at the destination synchronously so
		// the Travel package never evaluates against a null or stale linked ref
		// (which would make the procedure finish immediately and cause
		// PapyrusUtil's package-end hook to erase the override).
		mover->extraList.SetLinkedRef(destination, forms.keyword);

		// Remove any OTHER travel/swing package overrides without touching the
		// package we are adding.
		auto removeOther = [mover, package](RE::TESPackage* a_other) {
			if (!a_other || a_other == package) return;
			auto* args = RE::MakeFunctionArguments(
				static_cast<RE::Actor*>(mover),
				static_cast<RE::TESPackage*>(a_other));
			(void)vm::DispatchStatic(kActorUtilClass, kRemovePackageOverrideFn, args);
		};
		removeOther(forms.package);
		removeOther(forms.approach);
		removeOther(forms.walk);
		removeOther(forms.swing);

		// Push the package as an override (priority 100, flags 1), and force an
		// AI-resetting EvaluatePackage once the VM finishes executing
		// AddPackageOverride.
		auto* pkgArgs = RE::MakeFunctionArguments(
			static_cast<RE::Actor*>(mover),
			static_cast<RE::TESPackage*>(package),
			static_cast<std::int32_t>(kTravelPriority),
			static_cast<std::int32_t>(kTravelFlags));
		if (!vm::DispatchStaticWithEval(kActorUtilClass, kAddPackageOverrideFn, pkgArgs,
				mover->GetFormID(), true)) {
			mover->extraList.SetLinkedRef(nullptr, forms.keyword);
			LogStartFailureOnce();
			return false;
		}

		mover->EvaluatePackage(true, true);
		return true;
	}

	void StopTravel(RE::Actor* mover)
	{
		if (!mover) {
			return;
		}

		const auto& forms = ResolveForms();
		if (!forms.keyword || !forms.package) {
			return;
		}

		// Clear the keyworded linked ref synchronously and remove the override
		// for every package a travel leg can carry, evaluating once the VM
		// finishes removing the overrides.
		mover->extraList.SetLinkedRef(nullptr, forms.keyword);

		auto removeOverride = [mover](RE::TESPackage* a_package, bool a_evalAfter) {
			if (!a_package) return;
			auto* pkgArgs = RE::MakeFunctionArguments(
				static_cast<RE::Actor*>(mover),
				static_cast<RE::TESPackage*>(a_package));
			const bool ok = a_evalAfter
				? vm::DispatchStaticWithEval(kActorUtilClass, kRemovePackageOverrideFn, pkgArgs,
					mover->GetFormID(), false)
				: vm::DispatchStatic(kActorUtilClass, kRemovePackageOverrideFn, pkgArgs);
			if (!ok) {
				logs::warn("movement: RemovePackageOverride dispatch failed for {:08X}", mover->GetFormID());
			}
		};
		RE::TESPackage* lastPkg = forms.swing ? forms.swing
			: (forms.walk ? forms.walk : (forms.approach ? forms.approach : forms.package));
		removeOverride(forms.package, forms.package == lastPkg);
		removeOverride(forms.approach, forms.approach == lastPkg);
		removeOverride(forms.walk, forms.walk == lastPkg);
		removeOverride(forms.swing, forms.swing == lastPkg);

		mover->EvaluatePackage(true, false);
	}

	bool StartSwing(RE::Actor* attacker, RE::TESObjectREFR* victim)
	{
		if (!attacker || !victim || attacker == victim) {
			return false;
		}

		const auto& forms = ResolveForms();
		if (!forms.keyword || !forms.swing) {
			return false;
		}

		// Point the keyworded linked ref at the victim synchronously.
		attacker->extraList.SetLinkedRef(victim, forms.keyword);

		auto removeOther = [attacker](RE::TESPackage* a_other) {
			if (!a_other) return;
			auto* args = RE::MakeFunctionArguments(
				static_cast<RE::Actor*>(attacker),
				static_cast<RE::TESPackage*>(a_other));
			(void)vm::DispatchStatic(kActorUtilClass, kRemovePackageOverrideFn, args);
		};
		removeOther(forms.package);
		removeOther(forms.approach);
		removeOther(forms.walk);

		// Push the swing package as an override (priority 100, flags 1).
		auto* pkgArgs = RE::MakeFunctionArguments(
			static_cast<RE::Actor*>(attacker),
			static_cast<RE::TESPackage*>(forms.swing),
			static_cast<std::int32_t>(kTravelPriority),
			static_cast<std::int32_t>(kTravelFlags));
		if (!vm::DispatchStaticWithEval(kActorUtilClass, kAddPackageOverrideFn, pkgArgs,
				attacker->GetFormID(), true)) {
			attacker->extraList.SetLinkedRef(nullptr, forms.keyword);
			LogStartFailureOnce();
			return false;
		}

		attacker->EvaluatePackage(true, true);
		logs::info("movement: StartSwing {:08X} -> {:08X}", attacker->GetFormID(), victim->GetFormID());
		return true;
	}

	RE::FormID SwingPackageFormID()
	{
		const auto& forms = ResolveForms();
		return forms.swing ? forms.swing->GetFormID() : 0;
	}

	void StopSwing(RE::Actor* attacker)
	{
		if (!attacker) {
			return;
		}

		const auto& forms = ResolveForms();
		if (!forms.keyword || !forms.swing) {
			return;
		}

		// Clear the keyworded linked ref synchronously and remove the swing
		// package override, evaluating once the VM finishes the removal.
		attacker->extraList.SetLinkedRef(nullptr, forms.keyword);

		auto* pkgArgs = RE::MakeFunctionArguments(
			static_cast<RE::Actor*>(attacker),
			static_cast<RE::TESPackage*>(forms.swing));
		if (!vm::DispatchStaticWithEval(kActorUtilClass, kRemovePackageOverrideFn, pkgArgs,
				attacker->GetFormID(), false)) {
			logs::warn("movement: RemovePackageOverride(swing) dispatch failed for {:08X}", attacker->GetFormID());
		}

		attacker->EvaluatePackage(true, false);
		logs::info("movement: StopSwing {:08X}", attacker->GetFormID());
	}

	bool StartSandbox(RE::Actor* actor, RE::TESObjectREFR* anchor)
	{
		if (!actor || !anchor) {
			return false;
		}

		const auto& forms = ResolveForms();
		if (!forms.keyword || !forms.sandbox) {
			return false;
		}

		// Point the keyworded linked ref at the sandbox anchor synchronously.
		actor->extraList.SetLinkedRef(anchor, forms.keyword);

		// Push the sandbox package as an override (priority 95, flags 1).
		auto* pkgArgs = RE::MakeFunctionArguments(
			static_cast<RE::Actor*>(actor),
			static_cast<RE::TESPackage*>(forms.sandbox),
			static_cast<std::int32_t>(kSandboxPriority),
			static_cast<std::int32_t>(kTravelFlags));
		if (!vm::DispatchStaticWithEval(kActorUtilClass, kAddPackageOverrideFn, pkgArgs,
				actor->GetFormID(), true)) {
			actor->extraList.SetLinkedRef(nullptr, forms.keyword);
			LogStartFailureOnce();
			return false;
		}

		actor->EvaluatePackage(true, true);
		return true;
	}

	void StopSandbox(RE::Actor* actor)
	{
		if (!actor) {
			return;
		}

		const auto& forms = ResolveForms();
		if (!forms.keyword || !forms.sandbox) {
			return;
		}

		// Clear the keyworded linked ref synchronously and remove the sandbox override.
		actor->extraList.SetLinkedRef(nullptr, forms.keyword);

		auto* pkgArgs = RE::MakeFunctionArguments(
			static_cast<RE::Actor*>(actor),
			static_cast<RE::TESPackage*>(forms.sandbox));
		if (!vm::DispatchStaticWithEval(kActorUtilClass, kRemovePackageOverrideFn, pkgArgs,
				actor->GetFormID(), false)) {
			logs::warn("movement: RemovePackageOverride(sandbox) dispatch failed for {:08X}", actor->GetFormID());
		}

		actor->EvaluatePackage(true, false);
	}
}
