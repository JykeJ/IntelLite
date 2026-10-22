#pragma once

// IntelLite's fuzzy named-location resolver (the Meet destination picker).
//
// A "place" is a named BGSLocation (resolved through its worldLocMarker) or a
// named interior cell (resolved through the exterior door its teleport door
// leads to). The query is matched fuzzily so "The Bannered Mare" and
// "Bannered Mare" - or a small typo - reach the same place.
//
// GAME THREAD ONLY: it touches RE::.

#include <cstdint>
#include <string>

namespace intellite::places
{
	// A resolved destination. `anchorFormID` is the ref a walk measures to (a
	// BGSLocation's worldLocMarker, or an interior cell's EXTERIOR door);
	// `entryFormID` is the interior door to step through afterwards (0 when
	// there is no door entry, i.e. a plain exterior destination). `name` is the
	// canonical display name.
	struct Place
	{
		std::uint32_t anchorFormID = 0;
		std::uint32_t entryFormID = 0;
		std::string   name;
	};

	// Resolve `query` to a place, fuzzily (exact, contains, token-set Jaccard,
	// then small Levenshtein distance). The candidate index is built once,
	// lazily, over every named BGSLocation and named interior cell. On a match
	// the anchor(s) are resolved; a candidate with no anchor falls through to
	// the next-best. Returns false when nothing scores at or above the floor or
	// no candidate yields an anchor. Game thread only.
	bool Resolve(const std::string& query, Place& out);
}
