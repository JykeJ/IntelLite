// Portions of this file are adapted from IntelEngine-NativePlugin
// (Copyright 2026 Galanx), licensed under the Apache License 2.0.
// Changed for IntelLite. See THIRD_PARTY_NOTICES.md and NOTICE.

// IntelLite's fuzzy named-location resolver. See places.h.
//
// No external database: the candidate index is gathered once, lazily, from
// TESDataHandler's BGSLocation form array (name via GetFullName) and its
// TESObjectCELL form array (interior cells with a name). Matching is the shared
// fuzzy ladder in fuzzy.h.

#include "places.h"

#include "fuzzy.h"

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace intellite::places
{
	namespace
	{
		struct Candidate
		{
			std::uint32_t formID = 0;
			bool          isLocation = false;
			std::string   name;   // display name
			std::string   norm;   // fuzzy::Normalize(name)
		};

		std::vector<Candidate> g_candidates;
		bool                   g_built = false;  // game thread only

		void EnsureIndex()
		{
			if (g_built) return;
			g_built = true;

			auto* dataHandler = RE::TESDataHandler::GetSingleton();
			if (!dataHandler) return;

			for (auto* loc : dataHandler->GetFormArray<RE::BGSLocation>()) {
				if (!loc) continue;
				const char* n = loc->GetFullName();
				if (!n || !*n) continue;
				Candidate c;
				c.formID = loc->GetFormID();
				c.isLocation = true;
				c.name = n;
				c.norm = fuzzy::Normalize(c.name);
				if (c.norm.empty()) continue;
				g_candidates.push_back(std::move(c));
			}

			std::set<std::uint32_t> cellIds;

			auto addCell = [&cellIds](RE::TESObjectCELL* a_cell) {
				if (!a_cell || !a_cell->IsInteriorCell()) return;
				const char* n = a_cell->GetFullName();
				if (!n || !*n) return;
				if (!cellIds.insert(a_cell->GetFormID()).second) return;
				Candidate c;
				c.formID = a_cell->GetFormID();
				c.isLocation = false;
				c.name = n;
				c.norm = fuzzy::Normalize(c.name);
				if (c.norm.empty()) return;
				g_candidates.push_back(std::move(c));
			};

			for (auto* cell : dataHandler->GetFormArray<RE::TESObjectCELL>()) {
				addCell(cell);
			}
			// GetFormArray<TESObjectCELL>() can come back empty on some engine
			// builds; `interiorCells` is the data handler's own
			// interior set, so fold it in (deduped) to guarantee named interiors
			// are indexed, as IntelEngine's LocationResolver does.
			for (auto* cell : dataHandler->interiorCells) {
				addCell(cell);
			}

			logs::info("places: index built - {} candidate(s)", g_candidates.size());
		}
		// Canonical reference position inside an interior cell for choosing its
		// main entrance door: prefers the cell's BGSLocation worldLocMarker when
		// that marker sits inside the cell (e.g. WhiterunDragonsreachCenterMarker
		// 0001F890), falling back to `a_ref`'s position when `a_ref` is inside
		// the cell.
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

		// Resolve a candidate's walk anchor. A BGSLocation resolves through its
		// worldLocMarker (the handle is copied before get(): the non-const get()
		// clears the stored handle, and the marker belongs to the game); when
		// that marker sits inside an interior cell, the cell's main exterior
		// door becomes `anchorFormID` and the marker becomes `entryFormID` so
		// the leg walks to the exterior door and enters the building. An
		// interior cell scans all of its teleport doors whose linked door has a
		// worldspace and picks the deterministic main door.
		bool ResolveAnchorFor(RE::TESForm* a_form, bool a_isLocation, Place& a_out)
		{
			if (!a_form) return false;

			auto findInCell = [](RE::TESObjectCELL* a_cell, RE::TESObjectREFR* a_ref,
				std::uint32_t& a_anchor, std::uint32_t& a_entry) -> bool {
				RE::NiPoint3 refPos{};
				const bool   haveRefPos = CellMainRefPos(a_cell, a_ref, refPos);

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
					const RE::ObjectRefHandle linked = teleport->teleportData->linkedDoor;
					auto linkedPtr = linked.get();
					auto* target = linkedPtr.get();
					if (!target || target->IsDisabled() || target->IsDeleted() || !target->GetWorldspace()) {
						return RE::BSContainer::ForEachResult::kContinue;
					}
					if (IsBetterCellDoor(haveRefPos, refPos, a_refr, target, bestInt, bestExt)) {
						bestInt = a_refr;
						bestExt = target;
					}
					return RE::BSContainer::ForEachResult::kContinue;
				});
				if (bestInt && bestExt) {
					a_anchor = bestExt->GetFormID();
					a_entry  = bestInt->GetFormID();
					return true;
				}
				return false;
			};

			auto findDoorsForCell = [&findInCell](RE::TESObjectCELL* a_cell, RE::TESObjectREFR* a_ref,
				std::uint32_t& a_anchor, std::uint32_t& a_entry) -> bool {
				if (findInCell(a_cell, a_ref, a_anchor, a_entry)) {
					return true;
				}
				// Two-hop interior fallback (e.g. a sub-cell / wing whose teleport
				// door leads to a main interior cell that exits to a worldspace).
				a_cell->ForEachReference([&](RE::TESObjectREFR* a_refr) {
					if (a_anchor) return RE::BSContainer::ForEachResult::kStop;
					if (!a_refr || a_refr->IsDisabled() || a_refr->IsDeleted()) {
						return RE::BSContainer::ForEachResult::kContinue;
					}
					auto* teleport = a_refr->extraList.GetByType<RE::ExtraTeleport>();
					if (!teleport || !teleport->teleportData) {
						return RE::BSContainer::ForEachResult::kContinue;
					}
					const RE::ObjectRefHandle linked = teleport->teleportData->linkedDoor;
					auto linkedPtr = linked.get();
					auto* target = linkedPtr.get();
					if (!target || target->IsDisabled() || target->IsDeleted() || target->GetWorldspace()) {
						return RE::BSContainer::ForEachResult::kContinue;
					}
					auto* midCell = target->GetParentCell();
					if (!midCell || midCell == a_cell || !midCell->IsInteriorCell()) {
						return RE::BSContainer::ForEachResult::kContinue;
					}
					std::uint32_t midAnchor = 0;
					std::uint32_t midEntry  = 0;
					if (findInCell(midCell, target, midAnchor, midEntry)) {
						a_anchor = midAnchor;
						a_entry  = a_refr->GetFormID();  // final interior-side door in the target cell
						return RE::BSContainer::ForEachResult::kStop;
					}
					return RE::BSContainer::ForEachResult::kContinue;
				});
				return a_anchor != 0;
			};

			if (a_isLocation) {
				auto* loc = a_form->As<RE::BGSLocation>();
				if (!loc) return false;
				const RE::ObjectRefHandle marker = loc->worldLocMarker;
				auto refPtr = marker.get();
				auto* ref = refPtr.get();
				if (!ref) return false;
				if (!ref->GetWorldspace()) {
					if (auto* markerCell = ref->GetParentCell(); markerCell && markerCell->IsInteriorCell()) {
						std::uint32_t anchor = 0;
						std::uint32_t entry  = 0;
						if (findDoorsForCell(markerCell, ref, anchor, entry)) {
							a_out.anchorFormID = anchor;
							a_out.entryFormID  = ref->GetFormID();
							return true;
						}
					}
				}
				a_out.anchorFormID = ref->GetFormID();
				a_out.entryFormID = 0;
				return true;
			}

			auto* cell = a_form->As<RE::TESObjectCELL>();
			if (!cell || !cell->IsInteriorCell()) return false;

			RE::TESObjectREFR* markerRef = nullptr;
			if (auto* loc = cell->GetLocation()) {
				const RE::ObjectRefHandle mh = loc->worldLocMarker;
				auto mp = mh.get();
				if (auto* m = mp.get(); m && m->GetParentCell() == cell) {
					markerRef = m;
				}
			}

			std::uint32_t anchor = 0;
			std::uint32_t entry  = 0;
			if (!findDoorsForCell(cell, markerRef, anchor, entry)) return false;
			a_out.anchorFormID = anchor;
			a_out.entryFormID  = markerRef ? markerRef->GetFormID() : entry;
			return true;
		}
	}

	bool Resolve(const std::string& query, Place& out)
	{
		out = Place{};
		EnsureIndex();

		const std::string norm = fuzzy::Normalize(query);
		if (norm.empty() || g_candidates.empty()) return false;

		struct Scored
		{
			std::size_t index;
			int         score;
		};

		std::vector<Scored> matches;
		for (std::size_t i = 0; i < g_candidates.size(); ++i) {
			const int score = fuzzy::ScoreNormalized(norm, g_candidates[i].norm);
			if (score >= fuzzy::kFloor) {
				matches.push_back({ i, score });
			}
		}
		if (matches.empty()) return false;

		// Best first; a tie prefers the Location.
		std::stable_sort(matches.begin(), matches.end(),
			[](const Scored& a, const Scored& b) {
				if (a.score != b.score) return a.score > b.score;
				const bool la = g_candidates[a.index].isLocation;
				const bool lb = g_candidates[b.index].isLocation;
				if (la != lb) return la;
				return a.index < b.index;
			});

		for (const auto& m : matches) {
			const Candidate& c = g_candidates[m.index];
			RE::TESForm* form = c.isLocation
				? static_cast<RE::TESForm*>(RE::TESForm::LookupByID<RE::BGSLocation>(c.formID))
				: static_cast<RE::TESForm*>(RE::TESForm::LookupByID<RE::TESObjectCELL>(c.formID));
			if (!form) continue;

			Place resolved;
			if (ResolveAnchorFor(form, c.isLocation, resolved)) {
				resolved.name = c.name;
				out = std::move(resolved);
				return true;
			}
		}
		return false;
	}
}
