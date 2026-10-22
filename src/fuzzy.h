#pragma once

// IntelLite's shared fuzzy name scorer - the ladder originally written for the
// named-location resolver (places.cpp) and reused for NPC-name resolution.
//
// Pure string work: no RE:: access, safe from any thread.

#include <cstddef>
#include <filesystem>
#include <string>

namespace intellite::fuzzy
{
	// A match at or above this score is accepted. The lowest scoring rule
	// (Levenshtein distance 2 on two >=4-char names) scores 40.
	inline constexpr int kFloor = 40;

	// Truncate a UTF-8 string to at most `a_max` bytes without slicing a
	// multi-byte codepoint in half (walks back past any trailing UTF-8
	// continuation bytes + incomplete lead byte).
	std::string ClampUtf8(std::string a_str, std::size_t a_max);

	// The directory containing SkyrimSE.exe (no MAX_PATH truncation).
	std::filesystem::path ExeDir();

	// Convert a filesystem path to UTF-8 (never throws on non-ACP characters).
	std::string PathToUtf8(const std::filesystem::path& a_path);

	// Lowercase, strip a leading "the ", map punctuation to spaces, then
	// collapse whitespace (and trim).
	std::string Normalize(const std::string& in);

	// The scoring ladder. Both arguments are normalized internally, so `query`
	// and `candidate` are raw user/display strings:
	//   exact                      -> 100
	//   one contains the other     -> 80
	//   token-set Jaccard >= 0.6   -> 70
	//   Levenshtein <= 2 on two >=4-char names -> 60 - 10*distance (60/50/40)
	// Returns 0 below the floor (fuzzy::kFloor).
	int Score(const std::string& query, const std::string& candidate);

	// The ladder over ALREADY-Normalize()d strings (Score() is Normalize + this).
	// Use when the query and/or the candidate are pre-normalized (e.g. an index).
	int ScoreNormalized(const std::string& a_normQuery, const std::string& a_normCandidate);
}
