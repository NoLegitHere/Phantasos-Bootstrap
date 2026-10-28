// Copyright 2026 The Phantasos Authors
// SPDX-License-Identifier: Apache-2.0
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "syntax/token.hpp"

#include <array>
#include <utility>

namespace phs {

namespace {

constexpr auto kNames = std::to_array<std::string_view>({
#define PHS_X(name, spelling) #name,
    PHS_TOKENS(PHS_X)
#undef PHS_X
#define PHS_K(name, text) "Kw" #name,
    PHS_KEYWORDS(PHS_K)
#undef PHS_K
});

constexpr auto kSpellings = std::to_array<std::string_view>({
#define PHS_X(name, spelling) spelling,
    PHS_TOKENS(PHS_X)
#undef PHS_X
#define PHS_K(name, text) "'" text "'",
    PHS_KEYWORDS(PHS_K)
#undef PHS_K
});

constexpr auto kKeywordTexts = std::to_array<std::string_view>({
#define PHS_K(name, text) text,
    PHS_KEYWORDS(PHS_K)
#undef PHS_K
});

constexpr auto kFirstKeyword = std::to_underlying(Tok::KwAbstract);
static_assert(kSpellings.size() == kNames.size());
static_assert(kFirstKeyword + kKeywordTexts.size() == kNames.size());

}  // namespace

std::string_view tok_name(Tok k) { return kNames[std::to_underlying(k)]; }
std::string_view tok_spelling(Tok k) { return kSpellings[std::to_underlying(k)]; }
bool tok_is_keyword(Tok k) { return std::to_underlying(k) >= kFirstKeyword; }

// Keyword i is interned first, so its Symbol id is i + 1 and keyword lookup in
// the lexer is a range check instead of a second hash lookup.
void seed_keywords(Interner& in) {
    PHS_ASSERT(in.size() == 1);
    for (u32 i = 0; i < kKeywordTexts.size(); ++i) {
        const Symbol s = in.intern(kKeywordTexts[i]);
        PHS_ASSERT(s.id == i + 1);
    }
}

Tok keyword_kind(Symbol s) {
    if (s.id >= 1 && s.id <= kKeywordTexts.size()) return static_cast<Tok>(kFirstKeyword + s.id - 1);
    return Tok::Ident;
}

}  // namespace phs
