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

#include "syntax/lexer.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>

#include "support/utf8.hpp"

namespace phs {

namespace {

// Parses digits (underscores allowed) in the given base; nullopt if the value
// does not fit in 64 bits. Callers validate the digits beforehand.
std::optional<u64> parse_u64(std::string_view digits, int base) {
    const std::string clean = digits | std::views::filter([](char c) { return c != '_'; }) | std::ranges::to<std::string>();
    u64 value = 0;
    const auto [ptr, ec] = std::from_chars(clean.data(), clean.data() + clean.size(), value, base);
    if (ec != std::errc{} || ptr != clean.data() + clean.size()) return std::nullopt;
    return value;
}

// Parses a float literal; nullopt if it is not a finite Float64 or a non-zero
// literal underflows to zero. Out-of-range handling differs between C++
// libraries (inf, 0, or untouched output), so every case is checked here.
std::optional<double> parse_f64(std::string_view literal) {
    const std::string clean = literal | std::views::filter([](char c) { return c != '_'; }) | std::ranges::to<std::string>();
    double value = 0;
    const auto [ptr, ec] = std::from_chars(clean.data(), clean.data() + clean.size(), value);
    if (ec != std::errc{} || ptr != clean.data() + clean.size() || std::isinf(value)) return std::nullopt;
    const std::string_view mantissa = std::string_view(clean).substr(0, clean.find_first_of("eE"));
    if (value == 0.0 && std::ranges::any_of(mantissa, [](char d) { return d >= '1' && d <= '9'; }))
        return std::nullopt;
    return value;
}

constexpr std::string_view base_name(int base) {
    switch (base) {
        case 16: return "hexadecimal";
        case 8: return "octal";
        default: return "binary";
    }
}

class Lexer {
public:
    Lexer(const SourceManager& sm, FileId file, Interner& in, DiagEngine& diag)
        : src_(sm.file(file).text), file_(file), in_(in), diag_(diag) {}

    std::vector<Token> run() {
        validate_utf8();
        while (true) {
            skip_trivia();
            if (pos_ >= src_.size()) break;
            lex_token();
        }
        for (const Open& o : stack_ | std::views::reverse) {
            if (o.is_interp())
                diag_.error(DiagCode::UnterminatedString, o.span, "unterminated string interpolation");
            else
                diag_.error(DiagCode::UnclosedDelimiter, o.span, "unclosed delimiter '{}'", o.ch);
        }
        const auto end = static_cast<u32>(src_.size());
        out_.push_back({.kind = Tok::Eof, .space_before = true, .span = span(end, end)});
        return std::move(out_);
    }

private:
    static constexpr char kInterp = 'I';

    struct Open {
        char ch;  // '(' '[' '{' or kInterp for a string interpolation "\("
        Span span;
        bool is_interp() const { return ch == kInterp; }
        char opener() const { return is_interp() ? '(' : ch; }
    };

    Span span(u32 lo, u32 hi) const { return {file_, lo, hi}; }
    char peek(u32 off = 0) const { return pos_ + off < src_.size() ? src_[pos_ + off] : '\0'; }
    bool newline_significant() const { return stack_.empty() || stack_.back().ch == '{'; }
    std::string_view rest() const { return src_.substr(pos_); }

    // Advances over [pos_, end) while `pred` holds for the current character.
    template <typename Pred>
    void skip_while(Pred pred) {
        while (pos_ < src_.size() && pred(src_[pos_])) ++pos_;
    }

    // Source files must be UTF-8 (spec 1.1). Each run of invalid bytes is
    // reported once here; later stages treat such bytes as opaque.
    void validate_utf8() {
        std::string_view s = src_;
        while (!s.empty()) {
            if (const auto c = utf8_decode(s)) {
                s.remove_prefix(c->len);
                continue;
            }
            const auto start = static_cast<u32>(src_.size() - s.size());
            do s.remove_prefix(1);
            while (!s.empty() && static_cast<u8>(s.front()) >= 0x80 && !utf8_decode(s));
            diag_.error(DiagCode::InvalidUtf8, span(start, static_cast<u32>(src_.size() - s.size())), "invalid UTF-8 in source file");
        }
    }

    Token& push(Tok kind, u32 lo, decltype(Token::value) value = {}) {
        Token& t = out_.emplace_back(kind, space_, span(lo, pos_), value);
        space_ = false;
        return t;
    }

    // Newlines in comments obey the same delimiter and collapsing rules as
    // other trivia. The comment's contents never change the delimiter stack.
    void consume_newline() {
        if (newline_significant() && !out_.empty() && out_.back().kind != Tok::Newline)
            out_.push_back({.kind = Tok::Newline, .span = span(pos_, pos_ + 1)});
        ++pos_;
    }

    void skip_trivia() {
        while (pos_ < src_.size()) {
            const char c = src_[pos_];
            if (c == ' ' || c == '\t' || c == '\f' || c == '\v') {
                ++pos_;
            } else if (c == '\n') {
                consume_newline();
            } else if (rest().starts_with("//")) {
                skip_while([](char ch) { return ch != '\n'; });
            } else if (rest().starts_with("/*")) {
                skip_block_comment();
            } else {
                return;
            }
            space_ = true;
        }
    }

    void skip_block_comment() {
        const u32 start = pos_;
        pos_ += 2;
        u32 depth = 1;
        while (pos_ < src_.size() && depth > 0) {
            if (rest().starts_with("/*")) {
                ++depth;
                pos_ += 2;
            } else if (rest().starts_with("*/")) {
                --depth;
                pos_ += 2;
            } else if (src_[pos_] == '\n') {
                consume_newline();
            } else {
                ++pos_;
            }
        }
        if (depth > 0) diag_.error(DiagCode::UnterminatedComment, span(start, start + 2), "unterminated block comment");
    }

    void lex_token() {
        const u32 start = pos_;
        const char c = src_[pos_];
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') return lex_ident();
        if (std::isdigit(static_cast<unsigned char>(c))) return lex_number();
        if (c == '"') {
            ++pos_;
            return lex_string_segment(start, true);
        }
        if (c == '\'') return lex_char();
        if (static_cast<u8>(c) >= 0x80) {
            const auto ch = utf8_decode(rest());
            pos_ += ch ? ch->len : 1;
            // Invalid bytes were already reported by validate_utf8().
            if (ch) diag_.error(DiagCode::NonAsciiCharacter, span(start, pos_), "non-ASCII character outside of a literal or comment");
            space_ = true;
            return;
        }
        if (lex_punct(start)) return;
        ++pos_;
        diag_.error(DiagCode::UnexpectedCharacter, span(start, pos_), "unexpected character '{}'", c);
        space_ = true;
    }

    void lex_ident() {
        const u32 start = pos_;
        skip_while([](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
        const std::string_view text = src_.substr(start, pos_ - start);
        if (text == "_") {
            push(Tok::Underscore, start);
            return;
        }
        const Symbol sym = in_.intern(text);
        push(keyword_kind(sym), start, sym);
    }

    void lex_number() {
        const u32 start = pos_;
        // Right after a '.', even across a line break, digits are a tuple
        // index, so `t.0.1` is two accesses.
        usize last = out_.size();
        if (last && out_[last - 1].kind == Tok::Newline) --last;
        const bool index = last && out_[last - 1].kind == Tok::Dot;
        if (!index && src_[pos_] == '0' && (peek(1) == 'x' || peek(1) == 'o' || peek(1) == 'b')) return lex_prefixed_int(start);

        const auto scan_digits = [&] { skip_while([](char ch) { return std::isdigit(static_cast<unsigned char>(ch)) || ch == '_'; }); };
        scan_digits();
        bool is_float = false;
        if (!index && peek() == '.' && std::isdigit(static_cast<unsigned char>(peek(1)))) {
            is_float = true;
            ++pos_;
            scan_digits();
        }
        if (!index && (peek() == 'e' || peek() == 'E')) {
            const u32 save = pos_;
            ++pos_;
            if (peek() == '+' || peek() == '-') ++pos_;
            if (std::isdigit(static_cast<unsigned char>(peek()))) {
                is_float = true;
                scan_digits();
            } else {
                pos_ = save;
            }
        }
        const u32 end = pos_;
        if (pos_ < src_.size() && (std::isalnum(static_cast<unsigned char>(src_[pos_])) || src_[pos_] == '_')) {
            const u32 suffix = pos_;
            skip_while([](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
            diag_.error(DiagCode::InvalidNumber, span(suffix, pos_), "invalid suffix '{}' on number literal",
                        src_.substr(suffix, pos_ - suffix));
        }
        const std::string_view literal = src_.substr(start, end - start);
        if (index && (literal.contains('_') || (literal.size() > 1 && literal.front() == '0')))
            diag_.error(DiagCode::InvalidNumber, span(start, end), "tuple index '{}' must be written without underscores or leading zeros",
                        literal);

        if (is_float) {
            const auto value = parse_f64(literal);
            if (!value) diag_.error(DiagCode::NumberOutOfRange, span(start, end), "float literal is out of range for Float64");
            push(Tok::FloatLit, start, value.value_or(0.0));
            return;
        }
        const auto value = parse_u64(literal, 10);
        if (!value) diag_.error(DiagCode::NumberOutOfRange, span(start, end), "integer literal is too large for 64 bits");
        push(Tok::IntLit, start, value.value_or(0));
    }

    // 0x / 0o / 0b literals.
    void lex_prefixed_int(u32 start) {
        const int base = peek(1) == 'x' ? 16 : peek(1) == 'o' ? 8 : 2;
        pos_ += 2;
        const u32 digits_start = pos_;
        skip_while([](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
        const std::string_view digits = src_.substr(digits_start, pos_ - digits_start);

        if (digits.empty() || digits.front() == '_') {
            diag_.error(DiagCode::InvalidNumber, span(start, pos_), "missing digits after integer base prefix");
            push(Tok::IntLit, start, u64{0});
            return;
        }
        const auto bad = std::ranges::find_if_not(digits, [base](char d) {
            int value;
            return d == '_' ||
                std::from_chars(&d, &d + 1, value, base).ec == std::errc{};
        });
        if (bad != digits.end()) {
            const auto at = digits_start + static_cast<u32>(bad - digits.begin());
            diag_.error(DiagCode::InvalidNumber, span(at, at + 1), "invalid digit '{}' in {} literal", *bad, base_name(base));
            push(Tok::IntLit, start, u64{0});
            return;
        }
        const auto value = parse_u64(digits, base);
        if (!value) diag_.error(DiagCode::NumberOutOfRange, span(start, pos_), "integer literal is too large for 64 bits");
        push(Tok::IntLit, start, value.value_or(0));
    }

    // Parses an escape after the backslash at pos_-1. Returns false for an
    // interpolation start "\(" (which the caller handles).
    bool lex_escape(std::string& buf, bool in_char) {
        const u32 esc_start = pos_ - 1;
        const char e = peek();
        if (e == '(' && !in_char) return false;
        ++pos_;
        switch (e) {
            case 'n': buf += '\n'; return true;
            case 'r': buf += '\r'; return true;
            case 't': buf += '\t'; return true;
            case '0': buf += '\0'; return true;
            case '\\': buf += '\\'; return true;
            case '"': buf += '"'; return true;
            case '\'': buf += '\''; return true;
            case 'u': {
                if (peek() != '{') break;
                ++pos_;
                const u32 hex_start = pos_;
                skip_while([](char c) { return std::isxdigit(static_cast<unsigned char>(c)); });
                const std::string_view hex = src_.substr(hex_start, pos_ - hex_start);
                if (peek() != '}' || hex.empty() || hex.size() > 6) break;
                ++pos_;
                u32 cp = 0;
                std::from_chars(hex.data(), hex.data() + hex.size(), cp, 16);  // <= 6 hex digits always fit
                if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
                    diag_.error(DiagCode::InvalidEscape, span(esc_start, pos_), "invalid Unicode scalar value in escape");
                    return true;
                }
                utf8_encode(buf, cp);
                return true;
            }
            default:
                break;
        }
        diag_.error(DiagCode::InvalidEscape, span(esc_start, std::max(pos_, esc_start + 2)), "invalid escape sequence");
        return true;
    }

    // pos_ is just past the opening quote (first) or the closing ')' of an
    // interpolation; seg_start is where this token's span begins.
    void lex_string_segment(u32 seg_start, bool first) {
        const Tok end_kind = first ? Tok::StringLit : Tok::StringTail;
        std::string buf;
        while (true) {
            if (pos_ >= src_.size() || src_[pos_] == '\n') {
                diag_.error(DiagCode::UnterminatedString, span(seg_start, pos_), "unterminated string literal");
                push(end_kind, seg_start, in_.intern(buf));
                return;
            }
            const char c = src_[pos_];
            if (c == '"') {
                ++pos_;
                push(end_kind, seg_start, in_.intern(buf));
                return;
            }
            if (c == '\\') {
                ++pos_;
                if (!lex_escape(buf, false)) {
                    const u32 open_lo = pos_ - 1;
                    ++pos_;  // consume '('
                    push(first ? Tok::StringHead : Tok::StringMiddle, seg_start, in_.intern(buf));
                    stack_.push_back({kInterp, span(open_lo, pos_)});
                    return;
                }
                continue;
            }
            buf += c;
            ++pos_;
        }
    }

    void lex_char() {
        const u32 start = pos_;
        ++pos_;
        if (peek() == '\'') {
            ++pos_;
            diag_.error(DiagCode::InvalidCharacterLiteral, span(start, pos_), "empty character literal");
            push(Tok::CharLit, start, char32_t{0});
            return;
        }
        std::string buf;
        while (pos_ < src_.size() && src_[pos_] != '\'' && src_[pos_] != '\n') {
            if (src_[pos_] == '\\') {
                ++pos_;
                lex_escape(buf, true);
            } else {
                buf += src_[pos_++];
            }
        }
        if (peek() != '\'') {
            diag_.error(DiagCode::InvalidCharacterLiteral, span(start, pos_), "unterminated character literal");
            push(Tok::CharLit, start, char32_t{0});
            return;
        }
        ++pos_;
        // Exactly one scalar value is required. Invalid bytes were already
        // reported by validate_utf8() and just yield value 0.
        std::string_view s = buf;
        std::optional<u32> first;
        u32 count = 0;
        while (!s.empty()) {
            const auto c = utf8_decode(s);
            if (!c) {
                push(Tok::CharLit, start, char32_t{0});
                return;
            }
            if (!first) first = c->cp;
            ++count;
            s.remove_prefix(c->len);
        }
        if (count != 1) diag_.error(DiagCode::InvalidCharacterLiteral, span(start, pos_), "character literal must contain exactly one character");
        push(Tok::CharLit, start, static_cast<char32_t>(first.value_or(0)));
    }

    void open(char ch, Tok kind, u32 start) {
        ++pos_;
        push(kind, start);
        stack_.push_back({ch, span(start, pos_)});
    }

    void close(char opener, Tok kind, u32 start) {
        ++pos_;
        if (opener == '(' && !stack_.empty() && stack_.back().is_interp()) {
            stack_.pop_back();
            space_ = false;
            lex_string_segment(start, false);
            return;
        }
        if (!stack_.empty() && stack_.back().ch == opener) {
            stack_.pop_back();
        } else {
            Diagnostic& d = diag_.error(DiagCode::UnexpectedClosingDelimiter, span(start, pos_), "unexpected closing delimiter '{}'", src_[start]);
            if (!stack_.empty())
                d.notes.push_back(
                    {stack_.back().span, std::format("unclosed '{}' opened here", stack_.back().opener())});
            // Recover by closing everything up to the innermost matching opener.
            const auto match = std::ranges::find(stack_ | std::views::reverse, opener, &Open::opener);
            if (match.base() != stack_.begin()) stack_.erase(std::prev(match.base()), stack_.end());
        }
        push(kind, start);
    }

    bool lex_punct(u32 start) {
        const char c = src_[pos_];
        const char n = peek(1), n2 = peek(2);
        const auto emit = [&](Tok k, u32 len) {
            pos_ += len;
            push(k, start);
            return true;
        };
        switch (c) {
            case '(': open('(', Tok::LParen, start); return true;
            case '[': open('[', Tok::LBracket, start); return true;
            case '{': open('{', Tok::LBrace, start); return true;
            case ')': close('(', Tok::RParen, start); return true;
            case ']': close('[', Tok::RBracket, start); return true;
            case '}': close('{', Tok::RBrace, start); return true;
            case ',': return emit(Tok::Comma, 1);
            case ';': return emit(Tok::Semi, 1);
            case ':': return emit(Tok::Colon, 1);
            case '@': return emit(Tok::At, 1);
            case '~': return emit(Tok::Tilde, 1);
            case '^': return n == '=' ? emit(Tok::CaretEq, 2) : emit(Tok::Caret, 1);
            case '.':
                if (n == '.' && n2 == '<') return emit(Tok::DotDotLt, 3);
                if (n == '.') return emit(Tok::DotDot, 2);
                return emit(Tok::Dot, 1);
            case '-':
                if (n == '>') return emit(Tok::Arrow, 2);
                if (n == '=') return emit(Tok::MinusEq, 2);
                return emit(Tok::Minus, 1);
            case '=':
                if (n == '=') return emit(Tok::EqEq, 2);
                if (n == '>') return emit(Tok::FatArrow, 2);
                return emit(Tok::Eq, 1);
            case '?':
                if (n == '.') return emit(Tok::QuestionDot, 2);
                if (n == '?') return emit(Tok::QuestionQuestion, 2);
                return emit(Tok::Question, 1);
            case '!': return n == '=' ? emit(Tok::NotEq, 2) : emit(Tok::Bang, 1);
            case '+': return n == '=' ? emit(Tok::PlusEq, 2) : emit(Tok::Plus, 1);
            case '*': return n == '=' ? emit(Tok::StarEq, 2) : emit(Tok::Star, 1);
            case '/': return n == '=' ? emit(Tok::SlashEq, 2) : emit(Tok::Slash, 1);
            case '%': return n == '=' ? emit(Tok::PercentEq, 2) : emit(Tok::Percent, 1);
            case '&':
                if (n == '&') return emit(Tok::AmpAmp, 2);
                if (n == '+') return emit(Tok::AmpPlus, 2);
                if (n == '-') return emit(Tok::AmpMinus, 2);
                if (n == '*') return emit(Tok::AmpStar, 2);
                if (n == '=') return emit(Tok::AmpEq, 2);
                return emit(Tok::Amp, 1);
            case '|':
                if (n == '|') return emit(Tok::PipePipe, 2);
                if (n == '=') return emit(Tok::PipeEq, 2);
                return emit(Tok::Pipe, 1);
            case '<':
                if (n == '<' && n2 == '=') return emit(Tok::ShlEq, 3);
                if (n == '<') return emit(Tok::Shl, 2);
                if (n == '=') return emit(Tok::LtEq, 2);
                return emit(Tok::Lt, 1);
            case '>':
                if (n == '>' && n2 == '=') return emit(Tok::ShrEq, 3);
                if (n == '>') return emit(Tok::Shr, 2);
                if (n == '=') return emit(Tok::GtEq, 2);
                return emit(Tok::Gt, 1);
            default:
                return false;
        }
    }

    std::string_view src_;
    FileId file_;
    Interner& in_;
    DiagEngine& diag_;
    u32 pos_ = 0;
    bool space_ = true;
    std::vector<Token> out_;
    std::vector<Open> stack_;
};

}  // namespace

std::vector<Token> lex_file(const SourceManager& sm, FileId file, Interner& interner, DiagEngine& diag) {
    return Lexer(sm, file, interner, diag).run();
}

}  // namespace phs
