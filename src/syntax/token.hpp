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

#pragma once

#include <string_view>
#include <variant>

#include "support/interner.hpp"
#include "support/source.hpp"

namespace phs {

// Token and keyword lists. These are X-macros because C++23 cannot list an
// enum's enumerators; each list expands into the enum and its name tables.
// X(Name, spelling-used-in-diagnostics)
#define PHS_TOKENS(X)                \
    X(Eof, "end of file")            \
    X(Newline, "newline")            \
    X(Error, "invalid token")        \
    X(Ident, "identifier")           \
    X(IntLit, "integer literal")     \
    X(FloatLit, "float literal")     \
    X(CharLit, "character literal")  \
    X(StringLit, "string literal")   \
    X(StringHead, "string literal")  \
    X(StringMiddle, "string literal") \
    X(StringTail, "string literal")  \
    X(LParen, "'('")                 \
    X(RParen, "')'")                 \
    X(LBracket, "'['")               \
    X(RBracket, "']'")               \
    X(LBrace, "'{'")                 \
    X(RBrace, "'}'")                 \
    X(Comma, "','")                  \
    X(Semi, "';'")                   \
    X(Colon, "':'")                  \
    X(Dot, "'.'")                    \
    X(DotDot, "'..'")                \
    X(DotDotLt, "'..<'")             \
    X(Arrow, "'->'")                 \
    X(FatArrow, "'=>'")              \
    X(At, "'@'")                     \
    X(Underscore, "'_'")             \
    X(Question, "'?'")               \
    X(QuestionDot, "'?.'")           \
    X(QuestionQuestion, "'?\?'")     \
    X(Bang, "'!'")                   \
    X(Plus, "'+'")                   \
    X(Minus, "'-'")                  \
    X(Star, "'*'")                   \
    X(Slash, "'/'")                  \
    X(Percent, "'%'")                \
    X(AmpPlus, "'&+'")               \
    X(AmpMinus, "'&-'")              \
    X(AmpStar, "'&*'")               \
    X(Amp, "'&'")                    \
    X(Pipe, "'|'")                   \
    X(Caret, "'^'")                  \
    X(Tilde, "'~'")                  \
    X(Shl, "'<<'")                   \
    X(Shr, "'>>'")                   \
    X(EqEq, "'=='")                  \
    X(NotEq, "'!='")                 \
    X(Lt, "'<'")                     \
    X(LtEq, "'<='")                  \
    X(Gt, "'>'")                     \
    X(GtEq, "'>='")                  \
    X(AmpAmp, "'&&'")                \
    X(PipePipe, "'||'")              \
    X(Eq, "'='")                     \
    X(PlusEq, "'+='")                \
    X(MinusEq, "'-='")               \
    X(StarEq, "'*='")                \
    X(SlashEq, "'/='")               \
    X(PercentEq, "'%='")             \
    X(AmpEq, "'&='")                 \
    X(PipeEq, "'|='")                \
    X(CaretEq, "'^='")               \
    X(ShlEq, "'<<='")                \
    X(ShrEq, "'>>='")

// K(Name, keyword-text). Order defines the keyword symbol ids (1..N).
#define PHS_KEYWORDS(K)        \
    K(Abstract, "abstract")    \
    K(Actor, "actor")          \
    K(As, "as")                \
    K(Async, "async")          \
    K(Await, "await")          \
    K(Borrow, "borrow")        \
    K(Break, "break")          \
    K(Class, "class")          \
    K(Const, "const")          \
    K(Continue, "continue")    \
    K(Defer, "defer")          \
    K(Deinit, "deinit")        \
    K(Else, "else")            \
    K(Enum, "enum")            \
    K(Extern, "extern")        \
    K(False, "false")          \
    K(File, "file")            \
    K(Final, "final")          \
    K(For, "for")              \
    K(Fun, "fun")              \
    K(If, "if")                \
    K(Import, "import")        \
    K(In, "in")                \
    K(Init, "init")            \
    K(Inout, "inout")          \
    K(Interface, "interface")  \
    K(Internal, "internal")    \
    K(Is, "is")                \
    K(Let, "let")              \
    K(Loop, "loop")            \
    K(Match, "match")          \
    K(Mut, "mut")              \
    K(None, "none")            \
    K(Open, "open")            \
    K(Override, "override")    \
    K(Own, "own")              \
    K(Private, "private")      \
    K(Public, "public")        \
    K(Return, "return")        \
    K(Sealed, "sealed")        \
    K(SelfValue, "self")       \
    K(SelfType, "Self")        \
    K(Shared, "shared")        \
    K(Static, "static")        \
    K(Struct, "struct")        \
    K(Super, "super")          \
    K(Throw, "throw")          \
    K(Throws, "throws")        \
    K(True, "true")            \
    K(Try, "try")              \
    K(Type, "type")            \
    K(Unsafe, "unsafe")        \
    K(Var, "var")              \
    K(Weak, "weak")            \
    K(Where, "where")          \
    K(While, "while")          \
    K(Comptime, "comptime")    \
    K(Macro, "macro")          \
    K(Yield, "yield")          \
    K(Spawn, "spawn")

enum class Tok : u8 {
#define PHS_X(name, spelling) name,
    PHS_TOKENS(PHS_X)
#undef PHS_X
#define PHS_K(name, text) Kw##name,
    PHS_KEYWORDS(PHS_K)
#undef PHS_K
};

struct Token {
    Tok kind = Tok::Eof;
    bool space_before = false;  // whitespace, a comment or a line start precedes the token
    Span span;
    // Literal value or name, depending on `kind`. A wrong access aborts instead
    // of silently reading another member.
    //   Ident/keywords, String*: Symbol    IntLit: u64
    //   FloatLit: double                   CharLit: char32_t
    std::variant<std::monostate, u64, double, char32_t, Symbol> value;

    u64 int_value() const { return std::get<u64>(value); }
    double float_value() const { return std::get<double>(value); }
    u32 char_value() const { return std::get<char32_t>(value); }
    Symbol symbol() const { return std::get<Symbol>(value); }
};

std::string_view tok_name(Tok k);      // enum name, e.g. "LParen" (used in dumps)
std::string_view tok_spelling(Tok k);  // human spelling, e.g. "'('" (used in diagnostics)
bool tok_is_keyword(Tok k);

// Must be called on a fresh Interner before anything else is interned.
void seed_keywords(Interner& in);
// Returns Tok::Ident if the symbol is not a keyword.
Tok keyword_kind(Symbol s);

}  // namespace phs
