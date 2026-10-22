// Portions of this file are adapted from IntelEngine-NativePlugin
// (Copyright 2026 Galanx), licensed under the Apache License 2.0.
// Changed for IntelLite. See THIRD_PARTY_NOTICES.md and NOTICE.

// IntelLite's shared fuzzy name scorer. See fuzzy.h.
//
// The exact scoring ladder borrowed from IntelEngine's LocationResolver (exact,
// contains, token-set overlap, Levenshtein), with a strict floor. Shared by
// the location resolver and the NPC resolver.

#include "fuzzy.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <string>
#include <vector>

namespace intellite::fuzzy
{
	namespace
	{
		std::set<std::string> TokenSet(const std::string& a_norm)
		{
			std::set<std::string> tokens;
			std::size_t i = 0;
			while (i < a_norm.size()) {
				while (i < a_norm.size() && a_norm[i] == ' ') ++i;
				std::size_t j = i;
				while (j < a_norm.size() && a_norm[j] != ' ') ++j;
				if (j > i) tokens.insert(a_norm.substr(i, j - i));
				i = j;
			}
			return tokens;
		}

		double Jaccard(const std::set<std::string>& a, const std::set<std::string>& b)
		{
			if (a.empty() || b.empty()) return 0.0;
			std::size_t inter = 0;
			for (const auto& x : a) {
				if (b.count(x)) ++inter;
			}
			const std::size_t uni = a.size() + b.size() - inter;
			return uni ? static_cast<double>(inter) / static_cast<double>(uni) : 0.0;
		}

		int Levenshtein(const std::string& a, const std::string& b)
		{
			const std::size_t n = a.size();
			const std::size_t m = b.size();
			std::vector<int> prev(m + 1);
			std::vector<int> cur(m + 1);
			for (std::size_t j = 0; j <= m; ++j) prev[j] = static_cast<int>(j);
			for (std::size_t i = 1; i <= n; ++i) {
				cur[0] = static_cast<int>(i);
				for (std::size_t j = 1; j <= m; ++j) {
					const int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
					cur[j] = (std::min)({ prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost });
				}
				std::swap(prev, cur);
			}
			return prev[m];
		}
	}

	std::string Normalize(const std::string& a_in)
	{
		std::string s = a_in;
		for (char& ch : s) {
			ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
		}
		if (s.rfind("the ", 0) == 0) {
			s.erase(0, 4);
		}

		std::string spaced;
		spaced.reserve(s.size());
		for (char ch : s) {
			const auto c = static_cast<unsigned char>(ch);
			spaced.push_back(std::isalnum(c) ? static_cast<char>(c) : ' ');
		}

		std::string out;
		out.reserve(spaced.size());
		bool pendingSpace = false;
		for (char ch : spaced) {
			if (ch == ' ') {
				pendingSpace = true;
				continue;
			}
			if (pendingSpace && !out.empty()) out.push_back(' ');
			pendingSpace = false;
			out.push_back(ch);
		}
		return out;
	}

	int ScoreNormalized(const std::string& a_normQuery, const std::string& a_normCandidate)
	{
		if (a_normQuery.empty() || a_normCandidate.empty()) return 0;
		if (a_normQuery == a_normCandidate) return 100;
		if (a_normQuery.find(a_normCandidate) != std::string::npos ||
			a_normCandidate.find(a_normQuery) != std::string::npos) {
			return 80;
		}
		if (Jaccard(TokenSet(a_normQuery), TokenSet(a_normCandidate)) >= 0.6) return 70;
		if (a_normQuery.size() >= 4 && a_normCandidate.size() >= 4) {
			const int dist = Levenshtein(a_normQuery, a_normCandidate);
			if (dist <= 2) return 60 - 10 * dist;
		}
		return 0;
	}

	int Score(const std::string& a_query, const std::string& a_candidate)
	{
		return ScoreNormalized(Normalize(a_query), Normalize(a_candidate));
	}

	std::string ClampUtf8(std::string a_str, std::size_t a_max)
	{
		if (a_str.size() <= a_max) return a_str;
		std::size_t cut = a_max;
		// Walk back past UTF-8 continuation bytes (10xxxxxx = 0x80..0xBF) so a
		// multi-byte codepoint is never sliced in half.
		std::size_t walked = 0;
		while (cut > 0 && walked < 4 &&
			(static_cast<unsigned char>(a_str[cut]) & 0xC0u) == 0x80u) {
			--cut;
			++walked;
		}
		a_str.resize(cut);
		return a_str;
	}

	std::filesystem::path ExeDir()
	{
		std::wstring buf(MAX_PATH, L'\0');
		for (;;) {
			const DWORD len = GetModuleFileNameW(
				nullptr, buf.data(), static_cast<DWORD>(buf.size()));
			if (len == 0) {
				return {};
			}
			if (len < buf.size() - 1) {
				buf.resize(len);
				return std::filesystem::path(buf).parent_path();
			}
			if (buf.size() >= 32768) {
				buf.resize(len);
				return std::filesystem::path(buf).parent_path();
			}
			buf.resize(buf.size() * 2, L'\0');
		}
	}

	std::string PathToUtf8(const std::filesystem::path& a_path)
	{
		try {
			const auto u8 = a_path.u8string();
			return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
		} catch (...) {
			return {};
		}
	}
}
