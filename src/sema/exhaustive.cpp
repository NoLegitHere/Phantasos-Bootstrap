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

#include "sema/exhaustive.hpp"

#include <algorithm>
#include <format>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace phs {
namespace {
using namespace ast;
using Type = ScalarType;

// A pattern as coverage sees it. Case is an enum case, by its binding,
// `true`/`false`, or `none` and the values of an optional (`some`, with one
// part); Literal is any other literal, by its value; Or holds alternatives.
struct Space {
    enum Kind : u8 { Any, Case, Literal, Or } kind = Any;
    u32 ctor = 0;
    std::vector<u32> parts;  // a case's payload patterns, or the alternatives
};
constexpr u32 kFalse = std::numeric_limits<u32>::max() - 1;
constexpr u32 kTrue = std::numeric_limits<u32>::max() - 2;
constexpr u32 kNone = std::numeric_limits<u32>::max() - 3;
constexpr u32 kSome = std::numeric_limits<u32>::max() - 4;
// A row of the pattern matrix: the patterns left to match, first column
// last so that it pops cheaply, and the arm they come from.
struct Row {
    std::vector<u32> columns;
    u32 arm = 0;
    bool guarded = false;
};
// The parts of a value no row matches, first column last.
using Witness = std::vector<std::string>;

// After Maranget's "Warnings for pattern matching": the values a match can
// see are split by the case each column starts with, until no columns are
// left. There the first unguarded row matches; it and the guarded rows
// before it are reached, and with no such row the values are not covered. A
// match is exhaustive when no values are left uncovered; an arm no value
// reaches never runs. Values are split once per case that the rows start
// with, so a wide match costs about as much as its arms.
class Coverage {
public:
    Coverage(const Resolution& resolution, const ScalarTypes& types, const Interner& names, DiagEngine& diag)
        : resolution_(resolution), types_(types), names_(names), diag_(diag) {}

    void check(const MatchExpr& match) {
        spaces_.assign(1, Space{});
        literals_.clear();
        supported_ = true;
        complex_ = false;
        work_ = 0;
        const Type type = types_.expression(match.scrutinee);
        if (type.kind == Type::Deferred || type.kind == Type::Error) return;
        std::vector<Row> rows;
        for (usize i = 0; i < match.arms.size(); ++i)
            rows.push_back({{space(*match.arms[i].pattern, type)}, static_cast<u32>(i), match.arms[i].guard != nullptr});
        if (!supported_) return;
        reached_.assign(match.arms.size(), false);
        const auto missing = walk(std::move(rows), {type}, 0);
        if (complex_) {
            diag_.error(DiagCode::MatchTooComplex, match.scrutinee->span,
                        "this 'match' is too complex to check for missing cases; split it into smaller ones");
            return;
        }
        for (usize i = 0; i < match.arms.size(); ++i)
            if (!reached_[i])
                diag_.warning(DiagCode::UnreachableArm, match.arms[i].pattern->span,
                              "this arm never runs: no value it matches gets past the arms before it");
        if (!missing) return;
        const std::string name = types_.type_name(type, resolution_, names_);
        if (missing->back() == "_")
            diag_.error(DiagCode::NonExhaustiveMatch, match.scrutinee->span,
                        "'match' does not cover every '{}'; add a '_' arm", name);
        else
            diag_.error(DiagCode::NonExhaustiveMatch, match.scrutinee->span,
                        "'match' does not cover every '{}': '{}' is not matched; add an arm for it or a '_' arm", name, missing->back());
    }

private:
    // Matches with more columns to walk, or more rows to copy, are rejected.
    static constexpr u32 kDepth = 512;
    static constexpr usize kWork = 2'000'000;
    static constexpr u32 kInhabited = 64;

    u32 add(Space space) {
        spaces_.push_back(std::move(space));
        return static_cast<u32>(spaces_.size() - 1);
    }
    // Patterns nest no deeper than the parser allows.
    u32 space(const Pattern& pat, Type type) {
        switch (pat.kind) {
            case PatKind::Wildcard: case PatKind::Binding: return 0;
            case PatKind::Literal: return literal(*cast<LiteralPat>(&pat)->literal, type);
            case PatKind::Is: return add({Space::Literal, literal_key("is" + std::to_string(pat.span.lo))});
            case PatKind::Case: {
                const auto& c = *cast<CasePat>(&pat);
                const BindingId id = types_.pattern_case(&c);
                if (!id.valid()) { supported_ = false; return 0; }
                const auto parts = fields(static_cast<u32>(id.value), type);
                if (parts.size() != c.args.size()) { supported_ = false; return 0; }
                Space result{Space::Case, static_cast<u32>(id.value)};
                for (usize i = 0; i < c.args.size(); ++i) result.parts.push_back(space(*c.args[i], parts[i]));
                return add(std::move(result));
            }
            case PatKind::Alt: {
                Space result{Space::Or};
                for (const Pattern* alt : cast<AltPat>(&pat)->alts) result.parts.push_back(space(*alt, type));
                return add(std::move(result));
            }
            case PatKind::Error: break;
        }
        supported_ = false;
        return 0;
    }
    u32 literal(const Expr& expr, Type type) {
        if (!type.optional_depth) return value(expr);
        if (expr.kind == ExprKind::None) return add({Space::Case, kNone});
        if (type.optional_depth > kDepth) {
            complex_ = true;
            return 0;
        }
        // Any literal but `none` matches a present value.
        u32 result = value(expr);
        for (u32 i = 0; i < type.optional_depth; ++i) result = add({Space::Case, kSome, {result}});
        return result;
    }
    u32 value(const Expr& expr) {
        if (const auto* value = as<BoolExpr>(&expr)) return add({Space::Case, value->value ? kTrue : kFalse});
        std::string key;
        const auto* negated = as<UnaryExpr>(&expr);
        const Expr& inner = negated ? *negated->operand : expr;
        const std::string sign = negated ? "-" : "";
        if (const auto* number = as<IntExpr>(&inner)) key = "i" + sign + std::to_string(number->value);
        else if (const auto* real = as<FloatExpr>(&inner)) key = std::format("f{}{}", sign, real->value);
        else if (const auto* character = as<CharExpr>(&inner)) key = "c" + std::to_string(character->value);
        else if (const auto* text = as<StringExpr>(&inner)) key = "s" + std::to_string(text->value.id);
        else if (inner.kind == ExprKind::None) key = "none";
        else key = "?" + std::to_string(expr.span.lo);
        return add({Space::Literal, literal_key(key)});
    }
    u32 literal_key(const std::string& key) {
        return literals_.try_emplace(key, static_cast<u32>(literals_.size())).first->second;
    }

    // The cases of an enum, `false` and `true`, or `none` and the rest of an
    // optional; empty for types whose values cannot be listed.
    struct Constructors { bool finite = false; std::vector<u32> ctors; };
    // Cases with a payload no value has, such as an empty enum, need no arm.
    Constructors constructors(Type type) {
        if (type.optional_depth) {
            if (inhabited(type.element())) return {true, {kNone, kSome}};
            return {true, {kNone}};
        }
        if (type.kind == Type::Bool) return {true, {kFalse, kTrue}};
        const BindingId id = enum_of(type);
        if (!id.valid()) return {};
        Constructors result{true, {}};
        for (const BindingId member : resolution_.scopes()[resolution_.binding(id).members.value].bindings)
            if (resolution_.binding(member).kind == BindingKind::EnumCase && inhabited(static_cast<u32>(member.value), type))
                result.ctors.push_back(static_cast<u32>(member.value));
        return result;
    }
    BindingId enum_of(Type type) const {
        if (type.optional_depth || type.kind != Type::Nominal) return {};
        const BindingId id = types_.descriptor(type)->declaration;
        const auto& binding = resolution_.binding(id);
        return binding.item && binding.item->kind == ItemKind::Enum && binding.members.valid() ? id : BindingId{};
    }
    bool inhabited(u32 ctor, Type type) {
        return std::ranges::all_of(fields(ctor, type), [&](Type field) { return inhabited(field); });
    }
    // Whether some value has `type`: not `Never`, an enum whose cases all
    // have such payloads, or a struct or tuple holding one. Generic
    // instances, deep types and types that hold themselves count as
    // inhabited, which can only ask for an arm too many.
    bool inhabited(Type type, u32 depth = 0) {
        if (type.optional_depth) return true;
        if (type.kind == Type::Never) return false;
        if (type.kind != Type::Nominal && type.kind != Type::Tuple) return true;
        if (depth > kInhabited) return true;
        if (const auto known = inhabited_.find(type.detail); known != inhabited_.end()) return known->second;
        inhabited_[type.detail] = true;  // while it is checked
        const auto* descriptor = types_.descriptor(type);
        const auto all = [&](const std::vector<Type>& parts) {
            return std::ranges::all_of(parts, [&](Type part) { return inhabited(part, depth + 1); });
        };
        bool result = true;
        if (type.kind == Type::Tuple) result = all(descriptor->parameters);
        else if (descriptor->arguments.empty()) {
            const auto& binding = resolution_.binding(descriptor->declaration);
            const bool is_enum = binding.item && binding.item->kind == ItemKind::Enum;
            const bool is_struct = binding.item && binding.item->kind == ItemKind::Struct;
            if ((is_enum || is_struct) && binding.members.valid()) {
                result = !is_enum;
                for (const BindingId member : resolution_.scopes()[binding.members.value].bindings) {
                    const auto& field = resolution_.binding(member);
                    if (is_enum && field.kind == BindingKind::EnumCase)
                        result = result || all(fields(static_cast<u32>(member.value), type));
                    else if (is_struct && field.kind == BindingKind::Field && !(field.item && has_modifier(field.item->mods, Modifier::Static)))
                        result = result && inhabited(types_.binding(member), depth + 1);
                }
            }
        }
        inhabited_[type.detail] = result;
        return result;
    }
    // The types of a case's parts, in a value of `type`.
    std::vector<Type> fields(u32 ctor, Type type) const {
        if (ctor == kSome) {
            --type.optional_depth;
            return {type};
        }
        if (ctor == kTrue || ctor == kFalse || ctor == kNone) return {};
        const auto* signature = types_.signature(BindingId{ctor});
        return signature ? signature->parameters : std::vector<Type>{};
    }
    static bool builtin(u32 ctor) { return ctor >= kSome; }
    std::string ctor_name(u32 ctor) const {
        if (ctor == kTrue) return "true";
        if (ctor == kFalse) return "false";
        if (ctor == kNone) return "none";
        return "." + std::string(names_.str(resolution_.binding(BindingId{ctor}).name.sym));
    }
    // The value a case pattern names, from its parts' witnesses. A present
    // optional reads as its value.
    void rebuild(Witness& witness, u32 ctor, usize arity) const {
        if (ctor == kSome) return;
        std::string text = ctor_name(ctor);
        if (!builtin(ctor) && cast<EnumCaseItem>(resolution_.binding(BindingId{ctor}).item)->has_fields) {
            text += "(";
            for (usize i = 0; i < arity; ++i) {
                text += (i ? ", " : "") + witness.back();
                witness.pop_back();
            }
            text += ")";
        }
        witness.push_back(std::move(text));
    }

    // Rows whose first pattern is an alternative stand for one row per alternative.
    std::vector<Row> expand(std::vector<Row> rows) const {
        if (std::ranges::none_of(rows, [&](const Row& row) { return spaces_[row.columns.back()].kind == Space::Or; })) return rows;
        std::vector<Row> out;
        for (Row& row : rows) {
            std::vector<u32> work{row.columns.back()};
            while (!work.empty()) {
                const u32 next = work.back();
                work.pop_back();
                if (spaces_[next].kind == Space::Or) {
                    work.insert(work.end(), spaces_[next].parts.rbegin(), spaces_[next].parts.rend());
                    continue;
                }
                out.push_back(row);
                out.back().columns.back() = next;
            }
        }
        return out;
    }

    // Marks the arms that the values of `types` reach through rows, and
    // returns a value no unguarded row matches, if any.
    std::optional<Witness> walk(std::vector<Row> rows, std::vector<Type> types, u32 depth) {
        work_ += rows.size();
        if (complex_ || depth > kDepth || work_ > kWork) {
            complex_ = true;
            return std::nullopt;
        }
        if (types.empty()) {
            for (const Row& row : rows) {
                reached_[row.arm] = true;
                if (!row.guarded) return std::nullopt;
            }
            return Witness{};
        }
        rows = expand(std::move(rows));
        const Type type = types.back();
        types.pop_back();

        // The rows starting with each case or literal, in order, and those
        // starting with `_`.
        std::vector<u32> heads;  // the first space seen for each start
        std::unordered_map<u64, std::vector<u32>> starts;
        std::vector<u32> wild;
        for (u32 i = 0; i < rows.size(); ++i) {
            const Space& head = spaces_[rows[i].columns.back()];
            if (head.kind == Space::Any) {
                wild.push_back(i);
                continue;
            }
            auto [it, fresh] = starts.try_emplace(u64{head.kind} << 32 | head.ctor);
            if (fresh) heads.push_back(rows[i].columns.back());
            it->second.push_back(i);
        }

        const auto listed = constructors(type);
        const auto has = [&](u32 ctor) { return starts.contains(u64{Space::Case} << 32 | ctor); };
        // A case no value has matches nothing, whatever its arms say.
        const std::unordered_set<u32> possible(listed.ctors.begin(), listed.ctors.end());
        std::optional<Witness> missing;
        for (const u32 id : heads) {
            if (complex_) return std::nullopt;
            const Space& head = spaces_[id];
            if (head.kind == Space::Case && listed.finite && !possible.contains(head.ctor)) continue;
            const auto parts = head.kind == Space::Case ? fields(head.ctor, type) : std::vector<Type>{};
            const auto& own = starts.at(u64{head.kind} << 32 | head.ctor);
            // Both lists are in row order; merge them to keep it.
            std::vector<Row> next;
            usize a = 0, b = 0;
            while (a < own.size() || b < wild.size()) {
                const bool take_own = b == wild.size() || (a < own.size() && own[a] < wild[b]);
                const Row& row = rows[take_own ? own[a++] : wild[b++]];
                next.push_back({{row.columns.begin(), row.columns.end() - 1}, row.arm, row.guarded});
                const auto& own_parts = spaces_[row.columns.back()].parts;
                if (take_own) next.back().columns.insert(next.back().columns.end(), own_parts.rbegin(), own_parts.rend());
                else next.back().columns.insert(next.back().columns.end(), parts.size(), 0);
            }
            auto next_types = types;
            next_types.insert(next_types.end(), parts.rbegin(), parts.rend());
            auto found = walk(std::move(next), std::move(next_types), depth + 1);
            // A literal's values are also missing from the rest below.
            if (found && !missing && head.kind == Space::Case) {
                rebuild(*found, head.ctor, parts.size());
                missing = std::move(found);
            }
        }
        if (listed.finite && std::ranges::all_of(listed.ctors, has)) return missing;

        // The values that start with no case or literal of the rows.
        std::vector<Row> rest;
        for (const u32 i : wild) rest.push_back({{rows[i].columns.begin(), rows[i].columns.end() - 1}, rows[i].arm, rows[i].guarded});
        auto found = walk(std::move(rest), std::move(types), depth + 1);
        if (!found || missing) return missing;
        // Name a missing case when some are matched, else any value.
        const auto absent = std::ranges::find_if(listed.ctors, [&](u32 c) { return !has(c); });
        if (listed.finite && !starts.empty() && absent != listed.ctors.end()) {
            if (*absent == kSome) found->push_back(any(type.element()));
            else {
                const usize arity = fields(*absent, type).size();
                found->insert(found->end(), arity, "_");
                rebuild(*found, *absent, arity);
            }
        } else found->push_back("_");
        return found;
    }
    // A present value of an optional's type, by its first case if it has some.
    std::string any(Type type) {
        const auto listed = constructors(type);
        if (!listed.finite || listed.ctors.empty() || listed.ctors.front() == kSome) return "_";
        Witness witness(fields(listed.ctors.front(), type).size(), "_");
        rebuild(witness, listed.ctors.front(), witness.size());
        return witness.back();
    }

    const Resolution& resolution_;
    const ScalarTypes& types_;
    const Interner& names_;
    DiagEngine& diag_;
    std::vector<Space> spaces_;  // spaces_[0] is `_`
    std::unordered_map<std::string, u32> literals_;
    bool supported_ = true;
    std::vector<bool> reached_;  // by arm
    std::unordered_map<usize, bool> inhabited_;  // by type
    bool complex_ = false;
    usize work_ = 0;
};
}  // namespace

void check_matches(const Resolution& resolution, const ScalarTypes& types, const Interner& names, DiagEngine& diag) {
    Coverage coverage(resolution, types, names, diag);
    for (const MatchExpr* match : types.matches()) coverage.check(*match);
}

}  // namespace phs
