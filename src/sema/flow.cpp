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

#include "sema/flow.hpp"

#include <format>
#include <algorithm>
#include <initializer_list>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "support/flat_map.hpp"
#include "support/small_vec.hpp"

namespace phs {
namespace {
using namespace ast;

// One bit per tracked local or parameter of the current callable.
class Bits {
public:
    Bits() = default;
    Bits(usize size, bool value) : words_((size + 63) / 64, value ? ~u64{0} : u64{0}) {}
    bool test(usize i) const { return (words_[i / 64] >> (i % 64)) & 1; }
    void set(usize i, bool value) {
        const u64 mask = u64{1} << (i % 64);
        words_[i / 64] = value ? words_[i / 64] | mask : words_[i / 64] & ~mask;
    }
    void intersect(const Bits& other) {
        for (usize w = 0; w < words_.size(); ++w) words_[w] &= other.words_[w];
    }
    void unite(const Bits& other) {
        for (usize w = 0; w < words_.size(); ++w) words_[w] |= other.words_[w];
    }

private:
    std::vector<u64> words_;
};

// An unreachable state assigns everything and nothing, so dead code reports
// no initialization errors and merges as the identity.
struct State {
    Bits assigned;  // definitely assigned on every path reaching this point
    Bits maybe;     // assigned on at least one path
    Bits moved;     // moved on at least one path (one bit per movable binding)
    Bits moved_all; // moved on every path
    bool live = true;
};

void merge(State& into, const State& from) {
    if (!from.live) return;
    if (!into.live) { into = from; return; }
    into.assigned.intersect(from.assigned);
    into.maybe.unite(from.maybe);
    into.moved.unite(from.moved);
    into.moved_all.intersect(from.moved_all);
}

const Expr* unparen(const Expr* expr) {
    while (const auto* paren = as<ParenExpr>(expr)) expr = paren->inner;
    return expr;
}

bool control(const Expr& expr) {
    return expr.kind == ExprKind::If || expr.kind == ExprKind::Match || expr.kind == ExprKind::Block ||
           expr.kind == ExprKind::Unsafe;
}

bool conditional(const Expr& expr) {
    const auto* binary = as<BinaryExpr>(&expr);
    return binary && (binary->op == BinOp::And || binary->op == BinOp::Or || binary->op == BinOp::Coalesce);
}

// A call through `?.`, whose arguments run only when the receiver holds a value.
bool chained(const CallExpr& call) { return unparen(call.callee)->kind == ExprKind::OptMember; }

// Direct expression operands in evaluation order (left to right). Statements
// inside blocks and the branches of control expressions are not included.
using ExprList = SmallVec<const Expr*, 8>;

void operands(const Expr& expr, ExprList& out) {
    switch (expr.kind) {
        case ExprKind::Paren: out.push_back(cast<ParenExpr>(&expr)->inner); break;
        case ExprKind::Array: for (const Expr* e : cast<ArrayExpr>(&expr)->elems) out.push_back(e); break;
        case ExprKind::Tuple: for (const Expr* e : cast<TupleExpr>(&expr)->elems) out.push_back(e); break;
        case ExprKind::Interp: for (const Expr* e : cast<InterpExpr>(&expr)->exprs) out.push_back(e); break;
        case ExprKind::Unary: out.push_back(cast<UnaryExpr>(&expr)->operand); break;
        case ExprKind::Binary:
            out.push_back(cast<BinaryExpr>(&expr)->lhs);
            out.push_back(cast<BinaryExpr>(&expr)->rhs);
            break;
        case ExprKind::Cast: out.push_back(cast<CastExpr>(&expr)->operand); break;
        case ExprKind::Call:
            out.push_back(cast<CallExpr>(&expr)->callee);
            for (const auto& arg : cast<CallExpr>(&expr)->args) out.push_back(arg.value);
            break;
        case ExprKind::Member: out.push_back(cast<MemberExpr>(&expr)->base); break;
        case ExprKind::OptMember: out.push_back(cast<OptMemberExpr>(&expr)->base); break;
        case ExprKind::Index:
            out.push_back(cast<IndexExpr>(&expr)->base);
            out.push_back(cast<IndexExpr>(&expr)->index);
            break;
        case ExprKind::Unwrap: out.push_back(cast<UnwrapExpr>(&expr)->operand); break;
        case ExprKind::TupleField: out.push_back(cast<TupleFieldExpr>(&expr)->base); break;
        case ExprKind::GenericInst: out.push_back(cast<GenericInstExpr>(&expr)->base); break;
        case ExprKind::If: {
            const auto& branch = *cast<IfExpr>(&expr);
            out.push_back(branch.cond.expr);
            out.push_back(branch.then_block);
            out.push_back(branch.else_branch);
            break;
        }
        case ExprKind::Match:
            out.push_back(cast<MatchExpr>(&expr)->scrutinee);
            for (const auto& arm : cast<MatchExpr>(&expr)->arms) {
                out.push_back(arm.guard);
                out.push_back(arm.body);
            }
            break;
        case ExprKind::Unsafe: out.push_back(cast<UnsafeExpr>(&expr)->block); break;
        default: break;
    }
}

// Visits every statement and expression nested anywhere inside root, without
// the C++ stack. Closure bodies are separate callables, entered only on request.
template <typename S, typename E>
void each_node(const Expr* root, bool closures, S&& visit, E&& visit_expr) {
    SmallVec<std::variant<const Expr*, const Stmt*>, 16> work{root};
    ExprList exprs;
    while (!work.empty()) {
        const auto next = work.back();
        work.pop_back();
        exprs.clear();
        if (const auto* stmt_ptr = std::get_if<const Stmt*>(&next)) {
            const Stmt& stmt = **stmt_ptr;
            visit(stmt);
            switch (stmt.kind) {
                case StmtKind::Var: exprs.push_back(cast<VarStmt>(&stmt)->init); break;
                case StmtKind::Return: exprs.push_back(cast<ReturnStmt>(&stmt)->value); break;
                case StmtKind::Throw: exprs.push_back(cast<ThrowStmt>(&stmt)->value); break;
                case StmtKind::While:
                    exprs.push_back(cast<WhileStmt>(&stmt)->cond.expr);
                    exprs.push_back(cast<WhileStmt>(&stmt)->body);
                    break;
                case StmtKind::For:
                    exprs.push_back(cast<ForStmt>(&stmt)->iter);
                    exprs.push_back(cast<ForStmt>(&stmt)->body);
                    break;
                case StmtKind::Loop: exprs.push_back(cast<LoopStmt>(&stmt)->body); break;
                case StmtKind::Defer: exprs.push_back(cast<DeferStmt>(&stmt)->body); break;
                case StmtKind::Expr: exprs.push_back(cast<ExprStmt>(&stmt)->expr); break;
                case StmtKind::Assign:
                    exprs.push_back(cast<AssignStmt>(&stmt)->target);
                    exprs.push_back(cast<AssignStmt>(&stmt)->value);
                    break;
                case StmtKind::Break: case StmtKind::Continue: break;
            }
        } else if (const Expr* expr = std::get<const Expr*>(next)) {
            visit_expr(*expr);
            if (const auto* block = as<BlockExpr>(expr)) {
                for (const Stmt* stmt : block->stmts) work.push_back(stmt);
                continue;
            }
            if (const auto* closure = as<ClosureExpr>(expr); closure && closures) exprs.push_back(closure->body);
            operands(*expr, exprs);
        }
        for (const Expr* e : exprs)
            if (e) work.push_back(e);
    }
}

template <typename F>
void each_stmt(const Expr* root, F&& visit) {
    each_node(root, false, visit, [](const Expr&) {});
}

// Analyzes one callable at a time. Statement and control-expression recursion
// follows syntactic nesting, which the parser bounds (E0112); flat expression
// chains and `else if` chains are iterative.
class FlowChecker {
public:
    FlowChecker(const Resolution& resolution, const ScalarTypes& types, const Interner& names, DiagEngine& diag)
        : resolution_(resolution), types_(types), names_(names), diag_(diag), scratch_(diag.sources()) {
        for (usize id = 0; id < resolution.bindings().size(); ++id)
            if (const Item* item = resolution.bindings()[id].item) items_.emplace(item, BindingId{id});
    }

    FlowFacts facts() { return std::move(facts_); }

    void run(const File& file) {
        struct Work { const Item* item; const TypeDeclItem* owner; };
        std::vector<Work> work;
        for (const Item* item : file.items) work.push_back({item, nullptr});
        fields(work);
        while (!work.empty()) {
            const auto [item, owner] = work.back();
            work.pop_back();
            if (const auto* type = as_type_decl(item)) {
                // Construction runs the omitted defaults, the superclass arguments, the
                // instance field initializers in order and then the primary `init`
                // body, so a constructor parameter that one moves has possibly moved
                // for the rest.
                auto moved = defaults(type->ctor_params, type);
                const auto* parent = superclass(*type);
                for (const auto& base : type->supers)
                    for (usize a = 0; a < base.args.size(); ++a) {
                        // A superclass argument moves like a constructor call's argument.
                        const Param* param = parent ? super_parameter(*parent, base.args, a) : nullptr;
                        const bool consumes = param && param->conv != Convention::Inout &&
                                              (param->conv == Convention::Own || param->binding != Binding::None);
                        add(moved, callable(type->ctor_params, base.args[a].value, nullptr, moved, consumes));
                    }
                for (const Item* member : type->members) {
                    // Field initializers see the owner's primary-constructor parameters,
                    // and a closure in an instance field's initializer captures `self`.
                    const auto* var = as<VarItem>(member);
                    if (!var || !var->init || has_modifier(var->mods, Modifier::Static)) continue;
                    instance_fields_.insert(var);
                    capturing_receiver(type);
                    add(moved, callable(type->ctor_params, var->init, nullptr, moved, true));
                    receiver(nullptr, nullptr);
                }
                primary_moves_.try_emplace(type, std::move(moved));
                for (const Item* member : type->members) work.push_back({member, type});
            } else if (const auto* ext = as<ExternItem>(item)) {
                for (const Item* nested : ext->items) work.push_back({nested, owner});
            } else if (const auto* fun = as<FunItem>(item)) {
                const auto moved = defaults(fun->sig.params);
                if (fun->body) {
                    receiver(owner, fun);
                    callable(fun->sig.params, fun->body, fun, moved);
                    receiver(nullptr, nullptr);
                }
            } else if (const auto* var = as<VarItem>(item)) {
                if (var->init && !instance_fields_.contains(var)) {
                    if (!has_modifier(var->mods, Modifier::Static)) capturing_receiver(owner);
                    callable(owner ? owner->ctor_params : List<Param>{}, var->init, nullptr, {}, true);
                    receiver(nullptr, nullptr);
                }
            } else if (const auto* init = as<InitItem>(item)) {
                const auto moved = defaults(init->params);
                // Construction rules for `self` are separate, but closures still capture it.
                capturing_receiver(owner);
                if (init->body && (init->has_params || !owner)) callable(init->params, init->body, nullptr, moved);
                else if (init->body) callable(owner->ctor_params, init->body, nullptr, primary_moves_.at(owner));
                receiver(nullptr, nullptr);
            } else if (const auto* deinit = as<DeinitItem>(item)) {
                capturing_receiver(owner);
                if (deinit->body) callable({}, deinit->body, nullptr);
                receiver(nullptr, nullptr);
            }
        }
    }

private:
    struct Local {
        const VarStmt* decl;  // null for a `let` condition's binding
        usize tracked;  // bit index, or kUntracked for locals with an initializer
        Span declared;  // the name, which is one of decl->names when destructuring
        usize movable;  // bit index among the bindings that can be moved
    };
    static constexpr usize kUntracked = static_cast<usize>(-1);

    State live() const {
        return {Bits(tracked_, false), Bits(tracked_, false), Bits(movables_, false), Bits(movables_, false), true};
    }
    State dead() const {
        return {Bits(tracked_, true), Bits(tracked_, false), Bits(movables_, false), Bits(movables_, true), false};
    }

    // Diagnostics go nowhere while a loop body is evaluated only to learn
    // what an iteration moves. A cleanup is checked where it is registered,
    // and for moves again at each exit of its scope (3.30).
    enum class Filter : u8 { All, NoMoves, OnlyMoves };
    bool shown(DiagCode code) const {
        const bool moves = code == DiagCode::UseAfterMove;
        if (quiet_) return false;
        return moves ? filter_ != Filter::NoMoves : filter_ != Filter::OnlyMoves;
    }
    template <typename... Args>
    Diagnostic& error(DiagCode code, Span span, std::format_string<Args...> fmt, Args&&... args) {
        return (shown(code) ? diag_ : scratch_).error(code, span, fmt, std::forward<Args>(args)...);
    }

    // How a place may be modified (3.3). Unknown places, including `self`
    // during construction, are not checked.
    enum class Access : u8 { Unknown, Mutable, Immutable, ReadOnly, Temporary };
    enum class Storage : u8 { Unknown, Value, Reference };
    struct Place {
        Access access = Access::Unknown;
        std::string subject;  // what restricts the place, e.g. 'p' or field 'y'
        Span declared;
        std::string note;
    };
    struct Field {
        bool var = false;
        bool is_static = false;
        Span declared;
        std::string_view note;  // why a non-'var' field is immutable
    };

    // Registers every field before any body is checked, since members can be declared in any order.
    template <typename Work>
    void fields(const std::vector<Work>& roots) {
        std::vector<const Item*> work;
        for (const auto& root : roots) work.push_back(root.item);
        while (!work.empty()) {
            const Item* item = work.back();
            work.pop_back();
            if (const auto* type = as_type_decl(item)) {
                for (const auto& param : type->ctor_params)
                    if (type->kind == ItemKind::Struct || param.binding != Binding::None)
                        fields_.emplace(param.name.span.lo,
                                        Field{param.binding == Binding::Var, false, param.name.span,
                                              param.binding == Binding::Let ? "declared with 'let' here"
                                                                            : "declared without 'var' here"});
                for (const Item* member : type->members) {
                    if (const auto* var = as<VarItem>(member))
                        fields_.emplace(var->name.span.lo, Field{var->binding == Binding::Var,
                                                                 has_modifier(var->mods, Modifier::Static),
                                                                 var->name.span, "declared with 'let' here"});
                    work.push_back(member);
                }
            } else if (const auto* ext = as<ExternItem>(item)) {
                for (const Item* nested : ext->items) work.push_back(nested);
            }
        }
    }

    static Storage storage(ItemKind kind) {
        if (kind == ItemKind::Struct || kind == ItemKind::Enum) return Storage::Value;
        if (kind == ItemKind::Class || kind == ItemKind::Actor) return Storage::Reference;
        return Storage::Unknown;
    }
    Storage storage(ScalarType type) const {
        if (type.optional_depth) return Storage::Unknown;
        if (type.kind == ScalarType::Shared) return Storage::Reference;
        if (type.kind == ScalarType::Tuple) return Storage::Value;
        const auto* descriptor = type.kind == ScalarType::Nominal ? types_.descriptor(type) : nullptr;
        if (!descriptor || !descriptor->declaration.valid()) return Storage::Unknown;
        const Item* item = resolution_.binding(descriptor->declaration).item;
        return item ? storage(item->kind) : Storage::Unknown;
    }

    // `self` in an instance method is mutable only inside a `mut fun`.
    void receiver(const TypeDeclItem* owner, const FunItem* fun) {
        self_ = {};
        self_storage_ = Storage::Unknown;
        self_owner_ = nullptr;
        if (!owner || !fun || has_modifier(fun->mods, Modifier::Static)) return;
        self_storage_ = storage(owner->kind);
        self_owner_ = owner;
        if (has_modifier(fun->mods, Modifier::Mut)) self_ = {Access::Mutable, "'self'", {}, {}};
        else self_ = {Access::ReadOnly, "'self'", fun->name.span, "'self' is read-only in a method without 'mut'"};
    }

    // In an `init` or `deinit`, `self` stays unchecked for assignments, but a
    // closure that uses it captures it.
    void capturing_receiver(const TypeDeclItem* owner) {
        receiver(nullptr, nullptr);
        if (!owner) return;
        self_storage_ = storage(owner->kind);
        self_owner_ = owner;
    }

    Place field_place(const ResolvedBinding& binding, const Place& base, Storage storage) const {
        const auto it = fields_.find(binding.name.span.lo);
        if (it == fields_.end()) return {};
        const Field& field = it->second;
        const auto subject = "field '" + std::string(names_.str(binding.name.sym)) + "'";
        const Place fixed{Access::Immutable, subject, field.declared, std::string(field.note)};
        if (field.is_static) return field.var ? Place{Access::Mutable} : fixed;
        if (base.access == Access::Unknown) return {};
        // Nothing reached through a borrow becomes writable, whatever its type.
        if (base.access == Access::ReadOnly) return base;
        if (!field.var) return fixed;
        // Value types are immutable all the way down; a class object can be
        // changed through any reference that is not a borrow.
        if (storage == Storage::Value) return base;
        if (storage == Storage::Reference) return {Access::Mutable};
        return {};
    }

    // A bare field name inside a method. Inherited fields are found by the
    // type pass, so their lexical reference stays deferred.
    const ResolvedBinding* implicit_field(const NameExpr& name) const {
        const auto id = target(name);
        if (id.valid()) return resolution_.binding(id).kind == BindingKind::Field ? &resolution_.binding(id) : nullptr;
        const auto* record = types_.member(&name);
        if (!record || record->state != ResolutionState::Resolved || record->candidates.size() != 1) return nullptr;
        const auto& binding = resolution_.binding(record->candidates.front());
        return binding.kind == BindingKind::Field ? &binding : nullptr;
    }

    Place name_place(const NameExpr& name) const {
        const auto id = target(name);
        if (!id.valid()) {
            const auto* field = implicit_field(name);
            return field ? field_place(*field, self_, self_storage_) : Place{};
        }
        const auto& binding = resolution_.binding(id);
        const auto subject = "'" + std::string(names_.str(name.name.sym)) + "'";
        if (captured_.contains(id.value))
            return {Access::Immutable, "captured " + subject, binding.name.span, "a closure captures a read-only copy of it"};
        switch (binding.kind) {
            case BindingKind::Local: {
                // Like a borrowed parameter, a loop element cannot be changed through (3.33).
                if (loop_elements_.contains(binding.name.span.lo))
                    return {Access::ReadOnly, subject, binding.name.span, "a 'for' binding borrows each element"};
                const auto* found = local(id);
                if (!found) return {Access::Immutable, subject, binding.name.span, "bound here"};
                if (!found->decl) return {Access::Immutable, subject, found->declared, "bound by a 'let' condition here"};
                if (found->decl->binding == Binding::Var) return {Access::Mutable};
                return {Access::Immutable, subject, found->declared, "declared with 'let' here"};
            }
            case BindingKind::Pattern:
                // A payload binding borrows from the matched value (3.35).
                return {Access::ReadOnly, subject, binding.name.span, "a pattern binding borrows from the matched value"};
            case BindingKind::Parameter: {
                if (closure_params_.contains(binding.name.span.lo))
                    return {Access::ReadOnly, subject, binding.name.span, "closure parameters are read-only"};
                const auto it = params_.find(binding.name.span.lo);
                if (it == params_.end() || it->second->binding != Binding::None) return {};
                if (it->second->conv == Convention::Inout) return {Access::Mutable};
                return {it->second->conv == Convention::Own ? Access::Immutable : Access::ReadOnly, subject,
                        binding.name.span, "parameters are read-only unless declared 'inout'"};
            }
            case BindingKind::Variable: {
                const auto* var = as<VarItem>(binding.item);
                if (!var) return {};
                if (var->binding == Binding::Var) return {Access::Mutable};
                return {Access::Immutable, subject, binding.name.span, "declared with 'let' here"};
            }
            case BindingKind::Field: return field_place(binding, self_, self_storage_);
            default: return {};
        }
    }

    static const Expr* projected(const Expr& expr) {
        if (const auto* member = as<MemberExpr>(&expr)) return member->base;
        if (const auto* index = as<IndexExpr>(&expr)) return index->base;
        if (const auto* unwrap = as<UnwrapExpr>(&expr)) return unwrap->operand;
        if (const auto* element = as<TupleFieldExpr>(&expr)) return element->base;
        return nullptr;
    }

    // Member, index, tuple element and unwrap chains are walked iteratively from
    // their root. Elements and unwrapped optionals keep the access of their container.
    Place place(const Expr* expr) const {
        ExprList chain;
        expr = unparen(expr);
        while (const Expr* base = projected(*expr)) {
            chain.push_back(expr);
            expr = unparen(base);
        }
        // Any root other than a name or `self` is a value computed on the spot.
        Place result{Access::Temporary, "a temporary value"};
        if (expr->kind == ExprKind::SelfValue || expr->kind == ExprKind::Super) result = self_;
        else if (const auto* name = as<NameExpr>(expr)) result = name_place(*name);
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            const auto* member = as<MemberExpr>(*it);
            if (!member) continue;
            // A built-in array property is computed, never stored (3.33).
            if (types_.array_member(member)) return {Access::Temporary, "a temporary value"};
            const auto* record = types_.member(member);
            if (!record || record->state != ResolutionState::Resolved || record->candidates.size() != 1 ||
                resolution_.binding(record->candidates.front()).kind != BindingKind::Field)
                return {};
            // A type receiver names a static field, which ignores the base.
            if (record->type_receiver) result = {};
            result = field_place(resolution_.binding(record->candidates.front()), result,
                                 storage(types_.expression(member->base)));
        }
        return result;
    }

    static std::string_view describe(Access access) {
        return access == Access::ReadOnly ? "read-only" : access == Access::Temporary ? "temporary" : "immutable";
    }
    void restricted(Span span, std::string message, const Place& place) {
        auto& d = error(DiagCode::ImmutableAssignment, span, "{}", message);
        if (!place.note.empty()) d.notes.push_back({place.declared, place.note});
    }

    // An assignment to a field or to `self`; plain names go through write().
    void write_place(const Expr& target) {
        const Expr* inner = unparen(&target);
        const auto* member = as<MemberExpr>(inner);
        const auto* tuple = as<TupleFieldExpr>(inner);
        const bool element = inner->kind == ExprKind::Index, unwrapped = inner->kind == ExprKind::Unwrap;
        if (!member && !element && !unwrapped && !tuple && inner->kind != ExprKind::SelfValue) return;
        if (member && types_.array_member(member)) {
            restricted(target.span, std::format("cannot assign to '{}', which an array computes", names_.str(member->name.sym)), {});
            return;
        }
        const Place found = place(inner);
        if (found.access == Access::Unknown || found.access == Access::Mutable) return;
        if (element || unwrapped || tuple) {
            const auto what = tuple ? std::format("element {}", tuple->index)
                                    : std::string(element ? "an element" : "an unwrapped optional");
            if (found.access == Access::Temporary)
                restricted(target.span, std::format("cannot assign to {} of a temporary value", what), found);
            else
                restricted(target.span, std::format("cannot assign to {} because {} is {}", what, found.subject,
                                                    describe(found.access)), found);
            return;
        }
        if (!member) {
            restricted(target.span, std::format("cannot assign to {} 'self'", describe(found.access)), found);
            return;
        }
        const auto field = names_.str(member->name.sym);
        // place() resolved the member, so its record exists.
        const auto& binding = resolution_.binding(types_.member(member)->candidates.front());
        if (found.access == Access::Immutable && found.declared.lo == binding.name.span.lo)
            restricted(target.span, std::format("cannot assign to immutable field '{}'", field), found);
        else if (found.access == Access::Temporary)
            restricted(target.span, std::format("cannot assign to field '{}' of a temporary value", field), found);
        else
            restricted(target.span, std::format("cannot assign to field '{}' because {} is {}", field, found.subject,
                                                describe(found.access)), found);
    }

    // An `inout` argument must be a mutable place, whatever its type, since
    // the callee can replace the whole value.
    void inout_arguments(const CallExpr& call, const CheckedCall& checked, const FunItem& fun) {
        for (usize i = 0; i < checked.parameters.size() && i < call.args.size(); ++i) {
            const usize index = checked.parameters[i];
            if (index >= fun.sig.params.size() || fun.sig.params[index].conv != Convention::Inout) continue;
            const Place found = place(call.args[i].value);
            if (found.access == Access::Unknown || found.access == Access::Mutable) continue;
            const auto name = names_.str(fun.sig.params[index].name.sym);
            if (found.access == Access::Temporary)
                restricted(call.args[i].value->span,
                           std::format("cannot pass a temporary value to 'inout' parameter '{}'", name), found);
            else
                restricted(call.args[i].value->span,
                           std::format("the argument for 'inout' parameter '{}' must be mutable, but {} is {}", name,
                                       found.subject, describe(found.access)), found);
        }
    }

    // A place an argument names, as its root (a binding, `self` or a static
    // field) and the projections applied to it. Two places overlap when one
    // path is a prefix of the other; any two indexes may be the same element.
    struct Path {
        u64 root = 0;
        SmallVec<u64, 4> steps;  // field binding << 2, element << 2 | 1, index 2, unwrap 3
    };
    static constexpr u64 kSelfRoot = 1;
    static u64 root_key(BindingId id) { return (u64{id.value} + 1) << 1; }
    std::optional<Path> path(const Expr* expr) const {
        ExprList chain;
        expr = unparen(expr);
        while (const Expr* base = projected(*expr)) {
            chain.push_back(expr);
            expr = unparen(base);
        }
        // A static field is its own storage, whatever names its type, so the
        // qualifier before it need not be a place.
        const auto field = [&](const Expr* projection) -> const CheckedMember* {
            const auto* member = as<MemberExpr>(projection);
            const auto* record = member ? types_.member(member) : nullptr;
            if (!record || record->state != ResolutionState::Resolved || record->candidates.size() != 1 ||
                resolution_.binding(record->candidates.front()).kind != BindingKind::Field)
                return nullptr;
            return record;
        };
        Path result;
        usize start = chain.size();
        for (usize i = 0; i < chain.size(); ++i)
            if (const auto* record = field(chain[i]); record && record->type_receiver) {
                result.root = root_key(record->candidates.front());
                start = i;
                break;
            }
        if (start < chain.size()) {
            // Only the projections after the static field remain.
        } else if (expr->kind == ExprKind::SelfValue || expr->kind == ExprKind::Super) {
            result.root = kSelfRoot;
        } else if (const auto* name = as<NameExpr>(expr)) {
            BindingId id = target(*name);
            if (!id.valid()) id = implicit_binding(*name);
            if (!id.valid()) return std::nullopt;
            const auto& binding = resolution_.binding(id);
            if (binding.kind == BindingKind::Field && !is_static(binding)) {
                result.root = kSelfRoot;
                result.steps.push_back(u64{id.value} << 2);
            } else if (binding.kind == BindingKind::Field || binding.kind == BindingKind::Variable ||
                       binding.kind == BindingKind::Parameter || binding.kind == BindingKind::Local ||
                       binding.kind == BindingKind::Pattern) {
                result.root = root_key(id);
            } else {
                return std::nullopt;
            }
        } else {
            return std::nullopt;
        }
        for (usize i = start; i-- > 0;) {
            const Expr* step = chain[i];
            if (step->kind == ExprKind::Member) {
                const auto* record = field(step);
                if (!record) return std::nullopt;
                result.steps.push_back(u64{record->candidates.front().value} << 2);
            } else if (const auto* element = as<TupleFieldExpr>(step)) {
                result.steps.push_back(u64{element->index} << 2 | 1);
            } else {
                result.steps.push_back(step->kind == ExprKind::Index ? 2 : 3);
            }
        }
        return result;
    }
    enum class Overlap : u8 { No, Maybe, Yes };
    static Overlap overlap(const Path& a, const Path& b) {
        if (a.root != b.root) return Overlap::No;
        auto result = Overlap::Yes;
        for (usize i = 0; i < a.steps.size() && i < b.steps.size(); ++i) {
            if (a.steps[i] != b.steps[i]) return Overlap::No;
            if (a.steps[i] == 2) result = Overlap::Maybe;
        }
        return result;
    }

    std::string_view text(Span span) const {
        const auto& file = diag_.sources().file(span.file);
        return std::string_view(file.text).substr(span.lo, span.hi - span.lo);
    }

    // While a `for` loop's body runs, the array it runs over, and every place
    // that holds it, cannot change or move (E0506, 3.33).
    void changes(const std::optional<Path>& found, Span span, std::string_view spelled, std::string_view verb) {
        if (!found) return;
        for (auto it = iterated_.rbegin(); it != iterated_.rend(); ++it) {
            const auto found_overlap = overlap(*found, it->path);
            if (found_overlap == Overlap::No) continue;
            if (shown(DiagCode::ExclusiveAccess) && !reported_exclusive_.insert(span.lo)) return;
            const auto maybe = found_overlap == Overlap::Maybe ? ", which it may overlap" : "";
            auto& d = it->matched ? error(DiagCode::ExclusiveAccess, span, "cannot {} '{}' while a 'match' arm borrows from '{}'{}",
                                          verb, spelled, text(it->span), maybe)
                                  : error(DiagCode::ExclusiveAccess, span, "cannot {} '{}' while a 'for' loop runs over '{}'{}",
                                          verb, spelled, text(it->span), maybe);
            d.notes.push_back({it->span, it->matched ? "the pattern bindings borrow from it here" : "the loop runs over it here"});
            return;
        }
    }
    // The place that each parameter's argument names, if any. An omitted
    // parameter's default runs in the callee, where an earlier parameter
    // stands for the place its argument names.
    struct ArgumentPlaces {
        std::vector<std::optional<Path>> paths;
        std::vector<bool> supplied;
    };
    ArgumentPlaces argument_places(const CallExpr& call, const CheckedCall& checked, const FunItem& fun) const {
        ArgumentPlaces result{std::vector<std::optional<Path>>(fun.sig.params.size()),
                              std::vector<bool>(fun.sig.params.size(), false)};
        auto& paths = result.paths;
        for (usize i = 0; i < checked.parameters.size() && i < call.args.size(); ++i) {
            const usize index = checked.parameters[i];
            if (index >= fun.sig.params.size()) continue;
            result.supplied[index] = true;
            paths[index] = path(call.args[i].value);
        }
        for (usize index = 0; index < fun.sig.params.size(); ++index) {
            const auto& param = fun.sig.params[index];
            if (result.supplied[index] || !param.default_value) continue;
            auto found = path(param.default_value);
            if (!found) continue;
            // `self` in a default is the callee's receiver, not the caller's.
            if (found->root == kSelfRoot) continue;
            const auto& binding = resolution_.binding(BindingId{static_cast<u32>((found->root >> 1) - 1)});
            const auto earlier = std::ranges::find_if(fun.sig.params.begin(), fun.sig.params.begin() + index,
                                                      [&](const Param& other) {
                                                          return binding.kind == BindingKind::Parameter &&
                                                                 other.name.span.lo == binding.name.span.lo;
                                                      });
            if (earlier != fun.sig.params.begin() + index) {
                const auto& base = paths[static_cast<usize>(earlier - fun.sig.params.begin())];
                if (!base) continue;
                Path joined = *base;
                for (const u64 step : found->steps) joined.steps.push_back(step);
                found = std::move(joined);
            }
            paths[index] = std::move(found);
        }
        return result;
    }

    // The `inout` arguments, also omitted ones with a default, and the `mut
    // fun` receiver of a call inside a `for` loop's body.
    void loop_arguments(const CallExpr& call, const CheckedCall& checked, const FunItem& fun) {
        if (iterated_.empty()) return;
        const auto places = argument_places(call, checked, fun);
        for (usize i = 0; i < checked.parameters.size() && i < call.args.size(); ++i) {
            const usize index = checked.parameters[i];
            if (index < fun.sig.params.size() && fun.sig.params[index].conv == Convention::Inout)
                changes(places.paths[index], call.args[i].value->span, text(call.args[i].value->span), "change");
        }
        for (usize index = 0; index < fun.sig.params.size(); ++index) {
            const auto& param = fun.sig.params[index];
            if (!places.supplied[index] && param.conv == Convention::Inout && places.paths[index])
                changes(places.paths[index], call.callee->span, text(param.default_value->span), "change");
        }
        if (checked.kind != CallKind::Method || !has_modifier(fun.mods, Modifier::Mut) || has_modifier(fun.mods, Modifier::Static))
            return;
        if (checked.receiver) {
            changes(path(checked.receiver), checked.receiver->span, text(checked.receiver->span), "change");
            return;
        }
        const Expr* callee = unparen(call.callee);
        if (const auto* generic = as<GenericInstExpr>(callee)) callee = unparen(generic->base);
        if (callee->kind == ExprKind::Name) changes(Path{kSelfRoot, {}}, callee->span, "self", "change");
    }

    // While a call runs, an `inout` argument or the receiver of a `mut fun`
    // has exclusive access to its place, so no other argument of the call,
    // nor the receiver, may name an overlapping place (E0506, 3.32).
    void exclusive_arguments(const CallExpr& call, const CheckedCall& checked, const FunItem& fun) {
        struct Use {
            Span span;
            Path path;
            std::string who;      // what needs exclusive access, when it does
            std::string spelled;  // the place as written, or 'self' for an implicit receiver
            bool receiver = false;
            std::string_view by_default;  // the omitted parameter whose default names the place
        };
        const bool mut_receiver = checked.kind == CallKind::Method && has_modifier(fun.mods, Modifier::Mut);
        if (!mut_receiver && std::ranges::none_of(fun.sig.params, [](const Param& param) {
                return param.conv == Convention::Inout;
            }))
            return;
        std::vector<Use> accesses;
        if (checked.kind == CallKind::Method && !has_modifier(fun.mods, Modifier::Static)) {
            const bool mut = has_modifier(fun.mods, Modifier::Mut);
            std::string who = mut ? std::format("'mut fun {}'", names_.str(fun.name.sym)) : std::string{};
            const Expr* callee = unparen(call.callee);
            if (const auto* generic = as<GenericInstExpr>(callee)) callee = unparen(generic->base);
            if (checked.receiver) {
                if (auto found = path(checked.receiver))
                    accesses.push_back({checked.receiver->span, std::move(*found), who,
                                        std::string(text(checked.receiver->span)), true});
            } else if (callee->kind == ExprKind::Name) {
                // Also in an `init` or `deinit`, where the receiver's access is not checked.
                accesses.push_back({callee->span, Path{kSelfRoot, {}}, who, "self", true});
            }
        }
        const auto who = [&](const Param& param) {
            return param.conv == Convention::Inout ? std::format("'inout' parameter '{}'", names_.str(param.name.sym))
                                                   : std::string{};
        };
        const auto places = argument_places(call, checked, fun);
        for (usize i = 0; i < checked.parameters.size() && i < call.args.size(); ++i) {
            const usize index = checked.parameters[i];
            if (index >= fun.sig.params.size() || !places.paths[index]) continue;
            accesses.push_back({call.args[i].value->span, *places.paths[index], who(fun.sig.params[index]),
                                std::string(text(call.args[i].value->span)), false});
        }
        for (usize index = 0; index < fun.sig.params.size(); ++index) {
            const auto& param = fun.sig.params[index];
            if (places.supplied[index] || !places.paths[index]) continue;
            accesses.push_back({call.callee->span, *places.paths[index], who(param), std::string(text(param.default_value->span)),
                                false, names_.str(param.name.sym)});
        }
        for (usize b = 1; b < accesses.size(); ++b)
            for (usize a = 0; a < b; ++a) {
                const auto& first = accesses[a];
                const auto& second = accesses[b];
                const auto found = overlap(first.path, second.path);
                if ((first.who.empty() && second.who.empty()) || found == Overlap::No) continue;
                if (shown(DiagCode::ExclusiveAccess) && !reported_exclusive_.insert(second.span.lo)) break;
                const auto& exclusive = second.who.empty() ? first.who : second.who;
                // A default is reported at the call, with its own text in the note.
                auto& d = first.spelled == second.spelled
                              ? error(DiagCode::ExclusiveAccess, second.span,
                                      "'{}' is used twice in one call, but {} needs exclusive access to it", second.spelled,
                                      exclusive)
                              : error(DiagCode::ExclusiveAccess, second.span,
                                      "'{}' {} '{}', but {} needs exclusive access to it for the whole call", second.spelled,
                                      found == Overlap::Maybe ? "may overlap" : "overlaps", first.spelled, exclusive);
                for (const Use* use : {&first, &second})
                    if (!use->by_default.empty())
                        d.notes.push_back({call.callee->span, std::format("'{}' is the default of omitted parameter '{}'",
                                                                          use->spelled, use->by_default)});
                if (first.by_default.empty())
                    d.notes.push_back({first.span, first.receiver ? std::format("'{}' is the receiver here", first.spelled)
                                                                  : std::format("'{}' is also passed here", first.spelled)});
                break;
            }
    }

    // A `mut fun` needs a mutable receiver: a mutable place for value types,
    // and anything but a borrow for class instances.
    void mutating_call(const CallExpr& call) {
        const auto* checked = types_.call(&call);
        if (checked && checked->state == CallState::Resolved && checked->kind == CallKind::Array) {
            mutating_array_call(call, *checked);
            return;
        }
        if (!checked || checked->state != CallState::Resolved ||
            (checked->kind != CallKind::Method && checked->kind != CallKind::Function))
            return;
        const auto* fun = as<FunItem>(resolution_.binding(checked->target).item);
        if (!fun) return;
        inout_arguments(call, *checked, *fun);
        exclusive_arguments(call, *checked, *fun);
        loop_arguments(call, *checked, *fun);
        if (checked->kind != CallKind::Method || !has_modifier(fun->mods, Modifier::Mut) ||
            has_modifier(fun->mods, Modifier::Static))
            return;
        Place found;
        Storage kind = self_storage_;
        if (checked->receiver) {
            found = place(checked->receiver);
            kind = storage(types_.expression(checked->receiver));
        } else {
            const Expr* callee = unparen(call.callee);
            if (const auto* generic = as<GenericInstExpr>(callee)) callee = unparen(generic->base);
            if (callee->kind == ExprKind::Name) found = self_;
        }
        const bool rejected = found.access == Access::ReadOnly ||
                              (kind == Storage::Value && (found.access == Access::Immutable || found.access == Access::Temporary));
        if (!rejected) return;
        const auto name = names_.str(fun->name.sym);
        if (found.access == Access::Temporary)
            restricted(call.callee->span, std::format("cannot call 'mut fun {}' on a temporary value", name), found);
        else
            restricted(call.callee->span, std::format("cannot call 'mut fun {}' because {} is {}", name, found.subject,
                                                      describe(found.access)), found);
    }

    // `append` and `removeLast` change the array, which must be mutable (3.33).
    void mutating_array_call(const CallExpr& call, const CheckedCall& checked) {
        if (checked.array != ArrayMember::Append && checked.array != ArrayMember::RemoveLast) return;
        changes(path(checked.receiver), checked.receiver->span, text(checked.receiver->span), "change");
        // An array is a value, so the whole place it is stored in must be mutable.
        const Place found = place(checked.receiver);
        if (found.access != Access::ReadOnly && found.access != Access::Immutable && found.access != Access::Temporary) return;
        const auto name = checked.array == ArrayMember::Append ? "append" : "removeLast";
        if (found.access == Access::Temporary)
            restricted(call.callee->span, std::format("cannot call '{}' on a temporary array", name), found);
        else
            restricted(call.callee->span, std::format("cannot call '{}' because {} is {}", name, found.subject,
                                                      describe(found.access)), found);
    }

    // Defaults and initializers are separate roots with their own locals. Defaults keep the parameter
    // list so that writes to earlier parameters are still checked. An omitted argument's default runs
    // before the later defaults and the body, so the `own` parameters it may move are passed on as
    // possibly moved; the result has the move site of each, by parameter index.
    using ParamMoves = std::vector<std::optional<Span>>;
    // A default's value moves into an `own` parameter or a stored field of a primary constructor.
    ParamMoves defaults(List<Param> params, const TypeDeclItem* type = nullptr) {
        ParamMoves moved(params.size());
        for (const auto& param : params) {
            if (!param.default_value) continue;
            const bool stored = type && (type->kind == ItemKind::Struct || param.binding != Binding::None) &&
                                param.conv != Convention::Inout;
            add(moved, callable(params, param.default_value, nullptr, moved, param.conv == Convention::Own || stored));
        }
        return moved;
    }
    const TypeDeclItem* superclass(const TypeDeclItem& type) const {
        const auto it = items_.find(&type);
        const auto* hierarchy = it == items_.end() ? nullptr : types_.hierarchy(it->second);
        if (!hierarchy || !hierarchy->superclass.valid()) return nullptr;
        const Item* item = resolution_.binding(hierarchy->superclass).item;
        return item ? as_type_decl(item) : nullptr;
    }
    // The primary-constructor parameter that superclass argument a initializes:
    // positional arguments first, then labels.
    static const Param* super_parameter(const TypeDeclItem& parent, List<Arg> args, usize a) {
        if (args[a].label.sym.empty()) return a < parent.ctor_params.size() ? &parent.ctor_params[a] : nullptr;
        for (const auto& param : parent.ctor_params)
            if (param.name.sym == args[a].label.sym) return &param;
        return nullptr;
    }
    static void add(ParamMoves& into, const ParamMoves& from) {
        for (usize i = 0; i < into.size() && i < from.size(); ++i)
            if (!into[i]) into[i] = from[i];
    }

    // Returns where the body may move each `own` parameter, by index, on the paths that finish.
    // An initializer root's value moves into its variable, field or parameter.
    ParamMoves callable(List<Param> params, const Expr* body, const FunItem* fun, const ParamMoves& moved = {},
                        bool initializer = false) {
        invalid_captures_.clear();
        start(body);
        for (const auto& param : params) {
            params_.emplace(param.name.span.lo, &param);
            if (param.conv == Convention::Own) owned_.emplace(param.name.span.lo, movables_++);
        }
        move_sites_.assign(movables_, Span{});
        State state = live();
        for (usize i = 0; i < moved.size(); ++i)
            if (moved[i]) {
                const usize bit = owned_.find(params[i].name.span.lo)->second;
                state.moved.set(bit, true);
                move_sites_[bit] = *moved[i];
            }
        // An expression body's value moves out as the result (3.30).
        if (initializer || (fun && fun->expr_body && result(*fun) != ScalarType{ScalarType::Unit})) consume(body);
        eval(body, state);
        ParamMoves after(params.size());
        if (state.live)
            for (usize i = 0; i < params.size(); ++i)
                if (const auto it = owned_.find(params[i].name.span.lo); it != owned_.end() && state.moved.test(it->second))
                    after[i] = move_sites_[it->second];
        if (fun && !fun->expr_body && state.live && returns_value(*fun)) {
            const Span end{body->span.file, body->span.hi - 1, body->span.hi};
            auto& d = error(DiagCode::MissingReturn, end, "function '{}' must return '{}' on every path",
                                  names_.str(fun->name.sym), result_name(*fun));
            d.notes.push_back({fun->name.span, "the end of the body is reachable without a 'return'"});
        }
        // Each closure body is its own callable, checked after the body that
        // creates it. Its captures are read-only copies.
        while (!closures_.empty()) {
            const auto next = closures_.back();
            closures_.pop_back();
            start(next.closure->body);
            closure_ = next.closure;
            for (const auto& param : next.closure->params) closure_params_.emplace(param.name.span.lo, &param);
            for (const auto& capture : resolution_.captures(next.closure)) captured_.insert(capture.binding.value);
            self_ = next.self;
            self_storage_ = next.storage;
            self_owner_ = next.owner;
            move_sites_.assign(movables_, Span{});
            State closure_state = live();
            // The last expression of a closure body is its result.
            consume(next.closure->body);
            eval(next.closure->body, closure_state);
        }
        return after;
    }

    // Resets the per-callable tables and registers the locals of body.
    void start(const Expr* body) {
        params_.clear();
        closure_params_.clear();
        closure_ = nullptr;
        captured_.clear();
        locals_.clear();
        owned_.clear();
        consumed_.clear();
        iteration_moves_.clear();
        reported_moves_.clear();
        reported_borrows_.clear();
        reported_exclusive_.clear();
        defers_.clear();
        tracked_ = 0;
        movables_ = 0;
        const auto condition = [&](const Cond& cond) {
            if (cond.is_let) locals_.emplace(cond.name.span.lo, Local{nullptr, kUntracked, cond.name.span, movables_++});
        };
        each_node(body, false, [&](const Stmt& stmt) {
            if (const auto* var = as<VarStmt>(&stmt); var && var->names.empty())
                locals_.emplace(var->name.span.lo,
                                Local{var, var->init ? kUntracked : tracked_++, var->name.span, movables_++});
            else if (var)
                for (const Name& name : var->names)
                    locals_.emplace(name.span.lo, Local{var, kUntracked, name.span, movables_++});
            else if (const auto* loop = as<WhileStmt>(&stmt)) condition(loop->cond);
        }, [&](const Expr& expr) {
            if (const auto* branch = as<IfExpr>(&expr)) condition(branch->cond);
        });
    }

    ScalarType result(const FunItem& fun) const {
        const auto it = items_.find(&fun);
        const ScalarSignature* sig = it == items_.end() ? nullptr : types_.signature(it->second);
        return sig ? sig->result : ScalarType{};
    }
    bool returns_value(const FunItem& fun) const {
        if (!fun.sig.ret || fun.sig.ret->kind == TypeKind::Unit) return false;
        return result(fun) != ScalarType{ScalarType::Unit};
    }
    std::string result_name(const FunItem& fun) const {
        const auto type = result(fun);
        if (type.kind == ScalarType::Deferred || type.kind == ScalarType::Error) return "a value";
        return types_.type_name(type, resolution_, names_, 1024);
    }

    BindingId target(const NameExpr& name) const {
        const auto* ref = resolution_.reference(name.name.span);
        if (!ref || ref->state != ResolutionState::Resolved) return {};
        const auto ids = resolution_.candidates(ref->target);
        return ids.size() == 1 ? ids.front() : BindingId{};
    }
    const Local* local(BindingId id) const {
        if (!id.valid() || resolution_.binding(id).kind != BindingKind::Local) return nullptr;
        const auto it = locals_.find(resolution_.binding(id).name.span.lo);
        return it == locals_.end() ? nullptr : &it->second;
    }

    void read(const NameExpr& name, State& state) {
        const auto id = target(name);
        const auto* found = local(id);
        if (found && found->tracked != kUntracked && state.live && !state.assigned.test(found->tracked)) {
            auto& d = error(DiagCode::UninitializedUse, name.span, "'{}' is used before it is initialized",
                                     names_.str(name.name.sym));
            d.notes.push_back({found->declared, "declared here without an initializer"});
        }
        // A borrowed value is reported where a reachable use would move it.
        const bool moves = consumed_.contains(&name) && !(state.live && borrowed_move(name));
        if (moves && state.live && movable(id)) changes(Path{root_key(id), {}}, name.span, text(name.span), "move");
        use(id, name.span, moves, "used", state);
    }

    // The move bit of a local or `own` parameter of the current callable
    // whose type is known not to be Copy (3.30). Captures, borrowed
    // parameters and fields are not tracked.
    std::optional<usize> movable(BindingId id) const {
        if (!id.valid()) return std::nullopt;
        const auto& binding = resolution_.binding(id);
        usize bit = 0;
        if (const auto* found = local(id)) {
            bit = found->movable;
        } else if (const auto it = owned_.find(binding.name.span.lo);
                   binding.kind == BindingKind::Parameter && it != owned_.end()) {
            bit = it->second;
        } else {
            return std::nullopt;
        }
        if (copyable(types_.binding(id))) return std::nullopt;
        return bit;
    }

    // Reading a moved value is E0504; a consuming use moves it.
    void use(BindingId id, Span span, bool moves, std::string_view verb, State& state) {
        const auto bit = movable(id);
        if (!bit || !state.live) return;
        // A cleanup runs at every exit of its scope, but each use reports once.
        if (state.moved.test(*bit) && (!shown(DiagCode::UseAfterMove) || reported_moves_.insert(span.lo))) {
            const auto& binding = resolution_.binding(id);
            const auto name = names_.str(binding.name.sym);
            auto& d = state.moved_all.test(*bit)
                          ? error(DiagCode::UseAfterMove, span, "'{}' is {} after it was moved", name, verb)
                          : error(DiagCode::UseAfterMove, span,
                                           "'{}' is {} after it may have been moved", name, verb);
            d.notes.push_back({move_sites_[*bit], "moved here"});
            d.notes.push_back({binding.name.span, std::format("declared here with type '{}', which is not Copy",
                                                              type_name(types_.binding(id)))});
            // Report a moved value once, until it moves again. A quiet pass
            // keeps the move, since it only learns what reaches a back edge.
            if (!quiet_) unmove(*bit, state);
        }
        if (!moves) return;
        state.moved.set(*bit, true);
        state.moved_all.set(*bit, true);
        move_sites_[*bit] = span;
    }
    void unmove(usize bit, State& state) {
        state.moved.set(bit, false);
        state.moved_all.set(bit, false);
    }

    // Marks the names whose value an expression's result takes (3.30): the
    // expression itself, the elements of tuple and array literals, both sides
    // of `??`, the operand of `!`, `shared` and `as`, and the results of
    // blocks, `if` and `match`.
    void consume(const Expr* root) {
        SmallVec<const Expr*, 8> work{root};
        const auto tail = [&](const BlockExpr* block) {
            if (block && !block->stmts.empty())
                if (const auto* last = as<ExprStmt>(block->stmts[block->stmts.size() - 1])) work.push_back(last->expr);
        };
        while (!work.empty()) {
            const Expr* expr = unparen(work.back());
            work.pop_back();
            if (!expr) continue;
            switch (expr->kind) {
                case ExprKind::Name: case ExprKind::SelfValue: case ExprKind::Super: case ExprKind::Member:
                case ExprKind::OptMember: case ExprKind::TupleField: case ExprKind::Index:
                    consumed_.insert(expr);
                    break;
                case ExprKind::Tuple: for (const Expr* elem : cast<TupleExpr>(expr)->elems) work.push_back(elem); break;
                case ExprKind::Array: for (const Expr* elem : cast<ArrayExpr>(expr)->elems) work.push_back(elem); break;
                case ExprKind::Unwrap: work.push_back(cast<UnwrapExpr>(expr)->operand); break;
                case ExprKind::Unary:
                    if (const auto op = cast<UnaryExpr>(expr)->op; op == UnOp::Share || op == UnOp::Try || op == UnOp::TryOpt)
                        work.push_back(cast<UnaryExpr>(expr)->operand);
                    break;
                case ExprKind::Cast:
                    if (cast<CastExpr>(expr)->op != CastOp::Is) work.push_back(cast<CastExpr>(expr)->operand);
                    break;
                case ExprKind::Binary:
                    if (const auto* binary = cast<BinaryExpr>(expr); binary->op == BinOp::Coalesce) {
                        work.push_back(binary->lhs);
                        work.push_back(binary->rhs);
                    }
                    break;
                case ExprKind::Block: tail(cast<BlockExpr>(expr)); break;
                case ExprKind::Unsafe: tail(cast<UnsafeExpr>(expr)->block); break;
                case ExprKind::If:
                    tail(cast<IfExpr>(expr)->then_block);
                    work.push_back(cast<IfExpr>(expr)->else_branch);
                    break;
                case ExprKind::Match:
                    for (const auto& arm : cast<MatchExpr>(expr)->arms) work.push_back(arm.body);
                    break;
                default: break;
            }
        }
    }

    // What a binding that borrows its value is called: a loop element or a
    // pattern binding.
    std::optional<std::string> borrowed_binding(const ResolvedBinding& binding) const {
        if (binding.kind == BindingKind::Local && loop_elements_.contains(binding.name.span.lo)) return "loop element";
        if (binding.kind == BindingKind::Pattern) return "pattern binding";
        return std::nullopt;
    }
    // A consumed value that the callable only borrows cannot move (3.30): a
    // parameter other than `own`, `self`, a field, an element, or a capture
    // inside a closure body. Values of Copy types are copied instead.
    bool borrowed_move(const Expr& expr) {
        // Inside a closure body, `self` is a capture, which E0503 already checks.
        if (closure_ && self_rooted(expr)) return false;
        std::string subject;
        std::string fix = "use '.clone()' to move a copy";
        if (const auto* name = as<NameExpr>(&expr)) {
            const auto id = target(*name);
            const auto spelled = std::string(names_.str(name->name.sym));
            if (const auto* field = implicit_field(*name)) {
                subject = is_static(*field) ? "static field '" + spelled + "'" : "field '" + spelled + "' of 'self'";
            } else if (!id.valid()) {
                return false;
            } else if (captured_.contains(id.value)) {
                // A capture that is already an error is not reported again.
                if (invalid_captures_.contains(id.value)) return true;
                // A closure can run again, so it keeps what it captured (D3).
                subject = "captured '" + spelled + "'";
                fix = "a closure can be called more than once; use '" + spelled + ".clone()' to move a copy";
            } else {
                const auto& binding = resolution_.binding(id);
                if (binding.kind == BindingKind::Parameter && !owned_.contains(binding.name.span.lo)) {
                    subject = "borrowed parameter '" + spelled + "'";
                    const auto param = params_.find(binding.name.span.lo);
                    const bool inout = param != params_.end() && param->second->conv == Convention::Inout;
                    fix = closure_params_.contains(binding.name.span.lo) || inout
                              ? "use '" + spelled + ".clone()' to move a copy"
                              : "declare it 'own' to take ownership, or use '" + spelled + ".clone()' to move a copy";
                } else if (const auto borrowed = borrowed_binding(binding)) {
                    // `for x in a` borrows each element of a (3.33), and a
                    // pattern binding borrows part of the matched value (3.35).
                    subject = *borrowed + " '" + spelled + "'";
                    fix = "use '" + spelled + ".clone()' to move a copy";
                } else {
                    return false;
                }
            }
            if (generic_copy(types_.binding(id.valid() ? id : implicit_binding(*name)), expr)) return false;
        } else {
            if (expr.kind == ExprKind::SelfValue || expr.kind == ExprKind::Super) {
                // Only an instance method borrows its receiver, also viewed as its superclass.
                if (self_.access == Access::Unknown) return false;
                subject = expr.kind == ExprKind::Super ? "'super'" : "'self'";
            } else if (const auto* member = as<MemberExpr>(&expr)) {
                const auto* record = types_.member(member);
                if (!record || record->state != ResolutionState::Resolved || record->candidates.size() != 1 ||
                    resolution_.binding(record->candidates.front()).kind != BindingKind::Field)
                    return false;
                // Fields never move out on their own (D2), and no callable owns a static field.
                subject = std::string(record->type_receiver ? "static " : "") + "field '" +
                          std::string(names_.str(member->name.sym)) + "'";
            } else if (const auto* chained = as<OptMemberExpr>(&expr)) {
                // Nor through `?.`, which leaves the optional holding its value.
                const auto* record = types_.member(chained);
                if (!record || record->state != ResolutionState::Resolved || record->candidates.size() != 1 ||
                    resolution_.binding(record->candidates.front()).kind != BindingKind::Field)
                    return false;
                subject = "field '" + std::string(names_.str(chained->name.sym)) + "'";
            } else {
                subject = "an element";
            }
            if (generic_copy(types_.expression(&expr), expr)) return false;
        }
        borrow_error(expr.span, subject, fix);
        return true;
    }
    // Whether a type is Copy once generic parameters count as Copy; a move
    // that only that allows is recorded for code generation.
    bool generic_copy(ScalarType type, const Expr& expr) {
        if (!copyable(type, true)) return false;
        if (!copyable(type)) facts_.copies.insert(&expr);
        return true;
    }
    void borrow_error(Span span, std::string_view subject, const std::string& fix) {
        if (!shown(DiagCode::MoveOutOfBorrow) || reported_borrows_.insert(span.lo)) {
            auto& d = error(DiagCode::MoveOutOfBorrow, span, "cannot move out of {}", subject);
            d.notes.push_back({span, fix});
        }
    }
    bool self_rooted(const Expr& expr) const {
        const Expr* root = unparen(&expr);
        while (const Expr* base = projected(*root)) root = unparen(base);
        if (root->kind == ExprKind::SelfValue || root->kind == ExprKind::Super) return true;
        const auto* name = as<NameExpr>(root);
        if (!name) return false;
        const auto* field = implicit_field(*name);
        return field && !is_static(*field);
    }
    static bool is_static(const ResolvedBinding& binding) {
        return binding.item && has_modifier(binding.item->mods, Modifier::Static);
    }
    BindingId implicit_binding(const NameExpr& name) const {
        const auto* record = types_.member(&name);
        return record && record->candidates.size() == 1 ? record->candidates.front() : BindingId{};
    }

    // Arguments move into `own` parameters, into the stored fields of a
    // primary constructor and into the payload of an enum case.
    void consume_arguments(const CallExpr& call) {
        const auto* checked = types_.call(&call);
        if (checked && checked->state == CallState::Resolved && checked->kind == CallKind::EnumCase) {
            for (const auto& arg : call.args) consume(arg.value);
            return;
        }
        // `a.append(x)` moves x into the array.
        if (checked && checked->state == CallState::Resolved && checked->kind == CallKind::Array &&
            checked->array == ArrayMember::Append && !call.args.empty())
            consume(call.args[0].value);
        if (!checked || checked->state != CallState::Resolved || !checked->target.valid()) return;
        const Item* item = resolution_.binding(checked->target).item;
        if (!item) return;
        if (checked->kind == CallKind::NominalConstructor) {
            const auto* type = as_type_decl(item);
            if (!type) return;
            for (usize i = 0; i < checked->parameters.size() && i < call.args.size(); ++i) {
                const usize index = checked->parameters[i];
                if (index >= type->ctor_params.size()) continue;
                const auto& param = type->ctor_params[index];
                const bool stored = type->kind == ItemKind::Struct || param.binding != Binding::None;
                if (param.conv == Convention::Own || (stored && param.conv != Convention::Inout))
                    consume(call.args[i].value);
            }
        } else if (checked->kind == CallKind::Function || checked->kind == CallKind::Method) {
            const auto* fun = as<FunItem>(item);
            if (!fun) return;
            for (usize i = 0; i < checked->parameters.size() && i < call.args.size(); ++i) {
                const usize index = checked->parameters[i];
                if (index < fun->sig.params.size() && fun->sig.params[index].conv == Convention::Own)
                    consume(call.args[i].value);
            }
        }
    }

    // Creating a closure reads what it captures and copies it (3.29). Its body
    // is checked once the enclosing body is done.
    void create(const ClosureExpr& closure, State& state) {
        for (const auto& capture : resolution_.captures(&closure)) {
            const auto& binding = resolution_.binding(capture.binding);
            const auto name = names_.str(binding.name.sym);
            // A non-Copy local or `own` parameter moves into the closure.
            use(capture.binding, capture.use, true, "captured", state);
            // So does a capture of the enclosing closure, which keeps it (D3).
            if (closure_ && state.live && captured_.contains(capture.binding.value) &&
                !invalid_captures_.contains(capture.binding.value) && !copyable(types_.binding(capture.binding), true))
                borrow_error(capture.use, "captured '" + std::string(name) + "'",
                             "a closure can be called more than once; capture a copy made with '" + std::string(name) +
                                 ".clone()' instead");
            // A loop element is borrowed from its array, and a pattern binding
            // from the matched value, so neither can move into the closure (3.33, 3.35).
            const auto borrowed = borrowed_binding(binding);
            if (state.live && borrowed && !copyable(types_.binding(capture.binding)))
                facts_.copied_captures.insert({&closure, static_cast<u32>(capture.binding.value)});
            if (state.live && borrowed && !copyable(types_.binding(capture.binding), true)) {
                invalid_captures_.insert(capture.binding.value);
                borrow_error(capture.use, *borrowed + " '" + std::string(name) + "'",
                             "capture a copy made with '" + std::string(name) + ".clone()' instead");
            }
            if (const auto* found = local(capture.binding)) {
                if (found->tracked != kUntracked && state.live && !state.assigned.test(found->tracked)) {
                    auto& d = error(DiagCode::UninitializedUse, capture.use, "'{}' is captured before it is initialized", name);
                    d.notes.push_back({found->declared, "declared here without an initializer"});
                }
                continue;
            }
            if (binding.kind != BindingKind::Parameter) continue;
            // Parameters of an enclosing callable were checked where that callable's closures captured them.
            Convention convention = Convention::Default;
            // A promoted constructor parameter is still the parameter wherever it resolves
            // as one: in defaults, field initializers and the primary `init`.
            if (const auto it = params_.find(binding.name.span.lo); it != params_.end()) convention = it->second->conv; else if (!closure_params_.contains(binding.name.span.lo)) continue;
            if (convention == Convention::Inout || (convention != Convention::Own && !copyable(types_.binding(capture.binding))))
                invalid_captures_.insert(capture.binding.value);
            if (convention == Convention::Inout) {
                auto& d = error(DiagCode::InvalidCapture, capture.use,
                                      "a closure cannot capture 'inout' parameter '{}', which is only borrowed for the call", name);
                d.notes.push_back({binding.name.span, "declared 'inout' here"});
            } else if (convention != Convention::Own && !copyable(types_.binding(capture.binding))) {
                auto& d = error(DiagCode::InvalidCapture, capture.use,
                                      "a closure cannot capture borrowed parameter '{}' of type '{}', which is not Copy; declare it 'own' to move it into the closure",
                                      name, type_name(types_.binding(capture.binding)));
                d.notes.push_back({binding.name.span, "parameters are borrowed unless declared 'own'"});
            }
        }
        PendingClosure pending{&closure, self_, self_storage_, self_owner_};
        // Created again when the loop is checked, or already where its cleanup was registered.
        if (quiet_ || filter_ == Filter::OnlyMoves) return;
        if (self_storage_ != Storage::Unknown && self_owner_) {
            if (const auto use = uses_self(closure)) {
                // `self` is a parameter too: borrowed, or `inout` in a `mut fun`.
                const auto type = names_.str(self_owner_->name.sym);
                if (self_.access == Access::Mutable) {
                    error(DiagCode::InvalidCapture, *use,
                                "a closure cannot capture 'self' in a 'mut fun', where it is only borrowed for the call");
                } else if (!copyable(*self_owner_)) {
                    auto& d = error(DiagCode::InvalidCapture, *use,
                                          "a closure cannot capture 'self' of type '{}', which is borrowed and not Copy", type);
                    d.notes.push_back({self_owner_->name.span, "declared here"});
                }
                if (self_.access == Access::Mutable || !copyable(*self_owner_)) {
                    // Nested closures do not report it again.
                    pending.self = {};
                    pending.storage = Storage::Unknown;
                } else if (pending.self.access == Access::Unknown) {
                    // In an `init` or `deinit` too, the closure holds a read-only copy.
                    pending.self = {Access::Immutable, "the captured 'self'", {}, {}};
                }
            }
        }
        closures_.push_back(pending);
    }

    // Where a closure, or a closure inside it, uses `self`: explicitly, or
    // through a bare instance field or method name.
    // A direct call is decided by the overload it selected, not by any candidate.
    std::optional<Span> uses_self(const ClosureExpr& closure) const {
        std::optional<Span> found;
        SmallVec<const Expr*, 8> selected;  // callees already decided by their call
        each_node(closure.body, true, [](const Stmt&) {}, [&](const Expr& expr) {
            if (found) return;
            if (expr.kind == ExprKind::SelfValue || expr.kind == ExprKind::Super) {
                found = expr.span;
            } else if (const auto* call = as<CallExpr>(&expr); call && as<NameExpr>(call->callee)) {
                const auto* checked = types_.call(call);
                if (!checked || checked->state != CallState::Resolved || !checked->target.valid()) return;
                selected.push_back(call->callee);
                if (instance_member(checked->target)) found = call->callee->span;
            } else if (const auto* name = as<NameExpr>(&expr);
                       name && std::ranges::find(selected, &expr) == selected.end() && instance_member(*name)) {
                found = expr.span;
            }
        });
        return found;
    }
    bool instance_member(BindingId id) const {
        const auto& binding = resolution_.binding(id);
        if (binding.kind != BindingKind::Field && binding.kind != BindingKind::Function) return false;
        if (resolution_.scopes()[binding.scope.value].kind != ScopeKind::Members) return false;
        return !binding.item || !has_modifier(binding.item->mods, Modifier::Static);
    }
    bool instance_member(const NameExpr& name) const {
        const auto instance = [&](BindingId id) { return instance_member(id); };
        if (const auto id = target(name); id.valid()) return instance(id);
        const auto* record = types_.member(&name);
        return record && record->state == ResolutionState::Resolved && std::ranges::any_of(record->candidates, instance);
    }

    // Whether values of a type are copied implicitly (3.2). Ownership checks
    // are later work, so stage0 decides it structurally: scalars, function
    // values and raw pointers are Copy, as are tuples, optionals, structs and
    // payload-free enums made of Copy parts. Unknown types do not report.
    // A generic parameter is Copy when the argument it stands for is; outside
    // an instance, such as in a declaration's own `self`, it may not be.
    // With generic_copy, an unbound generic parameter or associated type counts
    // as Copy: what it stands for is unknown, so moving it out of a borrow is
    // not reported.
    bool copyable(ScalarType root, bool generic_copy = false) const {
        struct Part { ScalarType type; u32 env; };  // env: the instance whose arguments it sees
        std::vector<std::vector<std::pair<usize, Part>>> envs(1);
        std::vector<Part> work{{root, 0}};
        for (usize steps = 0; !work.empty(); ++steps) {
            if (steps > kCopyableSteps) return true;  // undecided; ownership checks decide later
            const auto [raw, env] = work.back();
            work.pop_back();
            const auto type = raw.scalar();
            switch (type.kind) {
                case ScalarType::String: case ScalarType::Array: case ScalarType::Shared: case ScalarType::Weak:
                    return false;
                case ScalarType::Projection:
                    if (generic_copy) break;
                    return false;
                case ScalarType::TypeParameter: {
                    const auto declaration = types_.descriptor(type)->declaration.value;
                    const auto& bound = envs[env];
                    const auto it = std::ranges::find(bound, declaration, &std::pair<usize, Part>::first);
                    if (it == bound.end()) {
                        if (generic_copy) break;
                        return false;
                    }
                    work.push_back(it->second);
                    break;
                }
                case ScalarType::Tuple:
                    for (const auto element : types_.descriptor(type)->parameters) work.push_back({element, env});
                    break;
                case ScalarType::Nominal: {
                    const auto* descriptor = types_.descriptor(type);
                    const Item* item = resolution_.binding(descriptor->declaration).item;
                    const auto* decl = item ? as_type_decl(item) : nullptr;
                    std::vector<ScalarType> fields;
                    if (!decl || !stored_fields(*decl, fields)) return false;
                    if (fields.empty()) break;
                    // The fields see this instance's arguments, as seen from where it is named.
                    std::vector<std::pair<usize, Part>> arguments;
                    const auto parameters = generic_parameters(*decl);
                    for (usize i = 0; i < parameters.size() && i < descriptor->arguments.size(); ++i)
                        arguments.push_back({parameters[i].value, {descriptor->arguments[i], env}});
                    envs.push_back(std::move(arguments));
                    for (const auto field : fields) work.push_back({field, static_cast<u32>(envs.size() - 1)});
                    break;
                }
                default: break;
            }
        }
        return true;
    }
    static constexpr usize kCopyableSteps = 4096;
    // A declaration's own `self`, whose generic fields stay symbolic.
    bool copyable(const TypeDeclItem& decl) const {
        std::vector<ScalarType> fields;
        if (!stored_fields(decl, fields)) return false;
        return std::ranges::all_of(fields, [&](ScalarType type) { return copyable(type); });
    }
    // The stored field types of a struct, or false when the declaration is
    // never Copy. Enums are Copy without payloads, and have no fields to check.
    bool stored_fields(const TypeDeclItem& decl, std::vector<ScalarType>& fields) const {
        if (decl.kind == ItemKind::Enum)
            return std::ranges::all_of(decl.members, [](const Item* member) {
                const auto* entry = as<EnumCaseItem>(member);
                return !entry || entry->fields.empty();
            });
        if (decl.kind != ItemKind::Struct) return false;
        const auto owner = items_.find(&decl);
        if (owner == items_.end()) return true;
        for (const BindingId id : resolution_.scopes()[resolution_.binding(owner->second).members.value].bindings) {
            const auto& binding = resolution_.binding(id);
            if (binding.kind != BindingKind::Field || (binding.item && has_modifier(binding.item->mods, Modifier::Static))) continue;
            fields.push_back(types_.binding(id));
        }
        return true;
    }
    // A type's generic parameters in declaration order, from the scope around its members.
    std::vector<BindingId> generic_parameters(const TypeDeclItem& decl) const {
        std::vector<BindingId> result;
        const auto owner = items_.find(&decl);
        if (owner == items_.end()) return result;
        const auto generics = resolution_.scopes()[resolution_.binding(owner->second).members.value].parent;
        for (const BindingId id : resolution_.scopes()[generics.value].bindings)
            if (resolution_.binding(id).kind == BindingKind::GenericParameter) result.push_back(id);
        return result;
    }
    std::string type_name(ScalarType type) const { return types_.type_name(type, resolution_, names_, 1024); }

    void immutable(const NameExpr& name, Span declared, std::string_view note) {
        auto& d = error(DiagCode::ImmutableAssignment, name.span, "cannot assign to immutable '{}'",
                              names_.str(name.name.sym));
        d.notes.push_back({declared, std::string(note)});
    }

    void write(const NameExpr& name, State& state) {
        const auto id = target(name);
        if (!id.valid()) {
            if (const auto* field = implicit_field(name)) write_field(name, *field);
            return;
        }
        const auto& binding = resolution_.binding(id);
        if (captured_.contains(id.value)) {
            auto& d = error(DiagCode::ImmutableAssignment, name.span,
                                  "cannot assign to captured '{}'; a closure captures a read-only copy of it",
                                  names_.str(name.name.sym));
            d.notes.push_back({binding.name.span, "declared outside the closure here"});
            return;
        }
        switch (binding.kind) {
            case BindingKind::Local: {
                const auto* found = local(id);
                if (!found) { immutable(name, binding.name.span, "bound here; only 'var' locals can be reassigned"); return; }
                // Assigning a moved local gives it a value again.
                unmove(found->movable, state);
                const bool mutable_local = found->decl && found->decl->binding == Binding::Var;
                if (found->tracked == kUntracked) {
                    if (!mutable_local) immutable(name, found->declared, found->decl ? "declared with 'let' here" : "bound by a 'let' condition here");
                    return;
                }
                if (!mutable_local && state.live && state.maybe.test(found->tracked)) {
                    auto& d = error(DiagCode::ImmutableAssignment, name.span,
                                          "'{}' may already be initialized; a 'let' binding is assigned only once",
                                          names_.str(name.name.sym));
                    d.notes.push_back({found->declared, "declared with 'let' here"});
                }
                state.assigned.set(found->tracked, true);
                state.maybe.set(found->tracked, true);
                return;
            }
            case BindingKind::Pattern:
                immutable(name, binding.name.span, "pattern bindings cannot be reassigned");
                return;
            case BindingKind::Parameter: {
                if (closure_params_.contains(binding.name.span.lo)) {
                    immutable(name, binding.name.span, "closure parameters are read-only");
                    return;
                }
                const auto it = params_.find(binding.name.span.lo);
                // Promoted constructor parameters name fields; receiver rules are later work.
                if (it == params_.end() || it->second->conv == Convention::Inout || it->second->binding != Binding::None) return;
                immutable(name, binding.name.span, "parameters are read-only unless declared 'inout'");
                return;
            }
            case BindingKind::Variable: {
                const auto* var = as<VarItem>(binding.item);
                if (var && var->binding != Binding::Var) immutable(name, binding.name.span, "declared with 'let' here");
                return;
            }
            case BindingKind::Field: write_field(name, binding); return;
            default: return;
        }
    }

    // An assignment to a bare field name, through the implicit `self`.
    void write_field(const NameExpr& name, const ResolvedBinding& binding) {
        const Place found = field_place(binding, self_, self_storage_);
        if (found.access == Access::Unknown || found.access == Access::Mutable) return;
        const auto field = names_.str(name.name.sym);
        if (found.declared.lo != binding.name.span.lo)
            restricted(name.span, std::format("cannot assign to field '{}' because 'self' is {}", field,
                                              describe(found.access)), found);
        else
            restricted(name.span, std::format("cannot assign to immutable field '{}'", field), found);
    }

    // A set of `let` locals that some statement inside a loop's condition or body assigns.
    Bits assigned_lets(std::initializer_list<const Expr*> parts) const {
        Bits result(tracked_, false);
        for (const Expr* part : parts) {
            if (!part) continue;
            each_stmt(part, [&](const Stmt& stmt) {
                const auto* assign = as<AssignStmt>(&stmt);
                const auto* name = assign ? as<NameExpr>(unparen(assign->target)) : nullptr;
                const auto* found = name ? local(target(*name)) : nullptr;
                if (found && found->tracked != kUntracked && found->decl->binding != Binding::Var) result.set(found->tracked, true);
            });
        }
        return result;
    }

    bool panics(const CallExpr& call) const {
        const auto* callee = as<NameExpr>(unparen(call.callee));
        if (!callee) return false;
        const auto id = target(*callee);
        return id.valid() && resolution_.binding(id).kind == BindingKind::BuiltinValue &&
               names_.str(resolution_.binding(id).name.sym) == "panic";
    }
    // Whether a call can throw: by the callee's signature or function type, or
    // for a constructor by its type's `init`s. An unknown callee may.
    bool throws(const CallExpr& call) const {
        const auto* checked = types_.call(&call);
        if (!checked || checked->state != CallState::Resolved) return !panics(call);
        switch (checked->kind) {
            case CallKind::NumericConversion:
            case CallKind::Builtin:
            case CallKind::Clone:
            case CallKind::Array:
            case CallKind::EnumCase: return false;
            case CallKind::IndirectFunction: {
                const auto* function = types_.descriptor(types_.expression(call.callee));
                return !function || function->throws;
            }
            default: break;
        }
        const Item* item = checked->target.valid() ? resolution_.binding(checked->target).item : nullptr;
        if (const auto* fun = as<FunItem>(item)) return fun->sig.throws;
        if (const auto* type = item ? as_type_decl(item) : nullptr) {
            for (const Item* member : type->members)
                if (const auto* init = as<InitItem>(member); init && init->throws) return true;
            return false;
        }
        return !panics(call);
    }
    bool diverges(const CallExpr& call) const {
        return types_.expression(&call) == ScalarType{ScalarType::Never} || panics(call);
    }

    void eval(const Expr* root, State& state) {
        if (!root) return;
        struct Frame { const Expr* expr; bool done; };
        SmallVec<Frame, 16> stack{{root, false}};
        ExprList children;
        while (!stack.empty()) {
            const auto [expr, done] = stack.back();
            stack.pop_back();
            if (!done) {
                if (control(*expr)) { control_flow(*expr, state); continue; }
                if (const auto* call = as<CallExpr>(expr)) consume_arguments(*call);
                else if (const auto* tuple = as<TupleExpr>(expr)) for (const Expr* elem : tuple->elems) consume(elem);
                else if (const auto* array = as<ArrayExpr>(expr)) for (const Expr* elem : array->elems) consume(elem);
                if (const auto* unary = as<UnaryExpr>(expr); unary && unary->op == UnOp::Try && caught_.empty()) {
                    // An error that `try` propagates leaves the callable from
                    // the call that throws it, through every pending cleanup.
                    caught_.push_back({dead(), 0});
                    eval(unary->operand, state);
                    caught_.pop_back();
                    continue;
                }
                if (const auto* unary = as<UnaryExpr>(expr); unary && unary->op == UnOp::TryOpt) {
                    // A caught error resumes after the `try?` expression.
                    caught_.push_back({dead(), defers_.size()});
                    eval(unary->operand, state);
                    merge(state, caught_.back().state);
                    caught_.pop_back();
                    continue;
                }
                stack.push_back({expr, true});
                if (conditional(*expr)) { stack.push_back({cast<BinaryExpr>(expr)->lhs, false}); continue; }
                if (const auto* call = as<CallExpr>(expr); call && chained(*call)) {
                    stack.push_back({call->callee, false});
                    continue;
                }
                children.clear();
                operands(*expr, children);
                for (auto it = children.rbegin(); it != children.rend(); ++it)
                    if (*it) stack.push_back({*it, false});
                continue;
            }
            // Through `?.`, the arguments and the call run on only some paths.
            const auto* skippable = as<CallExpr>(expr);
            if (skippable && !chained(*skippable)) skippable = nullptr;
            State skipped;
            if (skippable) {
                skipped = state;
                for (const auto& arg : skippable->args) eval(arg.value, state);
            }
            if (const auto* name = as<NameExpr>(expr)) read(*name, state);
            else if (state.live && consumed_.contains(expr)) borrowed_move(*expr);
            else if (const auto* closure = as<ClosureExpr>(expr)) create(*closure, state);
            else if (const auto* call = as<CallExpr>(expr)) {
                mutating_call(*call);
                // A throwing call inside `try` or `try?` may leave after its
                // operands have run; a panic aborts instead (3.7).
                if (!caught_.empty() && throws(*call)) catch_error(state);
                if (diverges(*call)) state = dead();
            } else if (conditional(*expr)) {
                // The right operand runs on only some paths.
                State rhs = state;
                eval(cast<BinaryExpr>(expr)->rhs, rhs);
                merge(state, rhs);
            }
            if (skippable) merge(state, skipped);
        }
    }

    static bool binds(const Pattern& root) {
        std::vector<const Pattern*> work{&root};
        while (!work.empty()) {
            const Pattern* pat = work.back();
            work.pop_back();
            if (pat->kind == PatKind::Binding || (pat->kind == PatKind::Is && !cast<IsPat>(pat)->binding.sym.empty())) return true;
            if (const auto* alt = as<AltPat>(pat)) for (const Pattern* p : alt->alts) work.push_back(p);
            else if (const auto* c = as<CasePat>(pat)) for (const Pattern* p : c->args) work.push_back(p);
        }
        return false;
    }
    void control_flow(const Expr& expr, State& state) {
        if (const auto* block = as<BlockExpr>(&expr)) {
            scopes_.push_back(defers_.size());
            for (const Stmt* stmt : block->stmts) statement(*stmt, state);
            cleanups(scopes_.back(), state);
            defers_.resize(scopes_.back());
            scopes_.pop_back();
        } else if (const auto* unsafe = as<UnsafeExpr>(&expr)) {
            control_flow(*unsafe->block, state);
        } else if (const auto* match = as<MatchExpr>(&expr)) {
            eval(match->scrutinee, state);
            if (match->arms.empty()) return;
            // Exhaustiveness is checked separately; some arm always runs. A
            // failed pattern or guard continues to the next arm with its effects.
            State out = dead();
            // Bindings borrow from the matched place, which the arm cannot change (3.35).
            auto subject = copyable(types_.expression(match->scrutinee)) ? std::nullopt : path(match->scrutinee);
            for (const auto& arm : match->arms) {
                const bool borrows = subject && binds(*arm.pattern);
                if (borrows) iterated_.push_back({*subject, unparen(match->scrutinee)->span, true});
                State path = state;
                eval(arm.guard, path);
                // An irrefutable pattern always runs its guard before the next arm.
                const bool irrefutable = arm.pattern->kind == PatKind::Wildcard || arm.pattern->kind == PatKind::Binding;
                if (arm.guard && irrefutable) state = path;
                else if (arm.guard) merge(state, path);
                eval(arm.body, path);
                if (borrows) iterated_.pop_back();
                merge(out, path);
                // Later arms are unreachable after an unguarded catch-all.
                if (irrefutable && !arm.guard) break;
            }
            state = std::move(out);
        } else {
            State out = dead();
            for (const auto* branch = cast<IfExpr>(&expr); branch;) {
                condition(branch->cond, state);
                State then = state;
                control_flow(*branch->then_block, then);
                merge(out, then);
                const Expr* otherwise = branch->else_branch;
                branch = as<IfExpr>(otherwise);
                if (!branch) {
                    if (otherwise) eval(otherwise, state);
                    merge(out, state);
                }
            }
            state = std::move(out);
        }
    }

    void loop(const Cond* cond, const BlockExpr* body, State& state, bool exits_normally) {
        // Assignment never removes initialization, so the loop head keeps the
        // entry's definite facts; any `let` assigned in the condition or body
        // may already be set when it runs again.
        state.maybe.unite(assigned_lets({cond ? cond->expr : nullptr, body}));
        // A value moved in one iteration is moved when the next one starts.
        // A quiet first pass finds those moves. Inside it, a nested loop's
        // head is its entry plus what its iterations move from a clean start,
        // which does not depend on the entry and is computed once per loop.
        if (quiet_) {
            state.moved.unite(iteration_moves(cond, body, state, exits_normally));
            State back = dead();
            state = iteration(cond, body, state, exits_normally, back);
            return;
        }
        State back = dead();
        {
            State probe = state;
            ++quiet_;
            iteration(cond, body, probe, exits_normally, back);
            --quiet_;
        }
        if (back.live) {
            state.moved.unite(back.moved);
            state.moved_all.intersect(back.moved_all);
        }
        state = iteration(cond, body, state, exits_normally, back);
    }

    // The bindings that may be moved when a loop returns to its head,
    // starting from a head where nothing has moved. A reinitialization
    // clears a move whatever came before it, so this set is the same for
    // every entry state.
    Bits iteration_moves(const Cond* cond, const BlockExpr* body, const State& entry, bool exits_normally) {
        if (const auto it = iteration_moves_.find(body); it != iteration_moves_.end()) return it->second;
        State clean = entry;
        clean.moved = Bits(movables_, false);
        clean.moved_all = Bits(movables_, false);
        State back = dead();
        iteration(cond, body, clean, exits_normally, back);
        return iteration_moves_.emplace(body, back.live ? back.moved : Bits(movables_, false)).first->second;
    }

    // Runs the cleanups registered since `from`, last first, as a scope exit
    // does. Only their moves reach the state after them, and one that
    // diverges ends the exit. While a cleanup runs, only the cleanups
    // registered before it are pending.
    void cleanups(usize from, State& state) {
        // Inside a cleanup's registration check, moves are not tracked.
        if (!state.live || defers_.size() <= from || filter_ == Filter::NoMoves) return;
        const auto filter = std::exchange(filter_, Filter::OnlyMoves);
        auto caught = std::exchange(caught_, {});
        const auto pending = defers_;
        for (usize i = pending.size(); i-- > from;) {
            defers_.resize(i);
            State cleanup = state;
            breaks_.push_back(dead());
            continues_.push_back(dead());
            control_flow(*pending[i]->body, cleanup);
            breaks_.pop_back();
            continues_.pop_back();
            if (!cleanup.live) { state = dead(); break; }
            state.moved = std::move(cleanup.moved);
            state.moved_all = std::move(cleanup.moved_all);
        }
        defers_ = pending;
        caught_ = std::move(caught);
        filter_ = filter;
    }

    // An error resumes after the innermost `try?`, once the cleanups of the
    // scopes it leaves inside that expression have run.
    void catch_error(const State& state) {
        State exit = state;
        cleanups(caught_.back().defers, exit);
        merge(caught_.back().state, exit);
    }

    // A `let` condition takes the optional's value, like a `let` initializer,
    // and binds it anew each time it runs.
    void condition(const Cond& cond, State& state) {
        if (cond.is_let) consume(cond.expr);
        eval(cond.expr, state);
        if (const auto it = locals_.find(cond.name.span.lo); cond.is_let && it != locals_.end()) unmove(it->second.movable, state);
    }

    // One pass over a loop from its head; back receives the states that
    // return to the head, and the result is the state after the loop.
    State iteration(const Cond* cond, const BlockExpr* body, State& state, bool exits_normally, State& back) {
        if (cond) condition(*cond, state);
        breaks_.push_back(dead());
        continues_.push_back(dead());
        loops_.push_back(defers_.size());
        State path = state;
        control_flow(*body, path);
        loops_.pop_back();
        back = std::move(continues_.back());
        continues_.pop_back();
        merge(back, path);
        State out = std::move(breaks_.back());
        breaks_.pop_back();
        if (exits_normally) merge(out, state);
        return out;
    }

    void statement(const Stmt& stmt, State& state) {
        switch (stmt.kind) {
            case StmtKind::Var: {
                const auto& var = *cast<VarStmt>(&stmt);
                consume(var.init);
                eval(var.init, state);
                if (const auto it = locals_.find(var.name.span.lo);
                    var.names.empty() && it != locals_.end() && it->second.tracked != kUntracked) {
                    state.assigned.set(it->second.tracked, false);
                    state.maybe.set(it->second.tracked, false);
                }
                // Each run of the declaration binds a new value.
                if (var.names.empty()) {
                    if (const auto it = locals_.find(var.name.span.lo); it != locals_.end()) unmove(it->second.movable, state);
                }
                for (const Name& name : var.names)
                    if (const auto it = locals_.find(name.span.lo); it != locals_.end()) unmove(it->second.movable, state);
                break;
            }
            case StmtKind::Return:
                consume(cast<ReturnStmt>(&stmt)->value);
                eval(cast<ReturnStmt>(&stmt)->value, state);
                cleanups(0, state);
                state = dead();
                break;
            case StmtKind::Throw:
                eval(cast<ThrowStmt>(&stmt)->value, state);
                // A caught error unwinds only to its `try?`.
                if (!caught_.empty()) catch_error(state);
                else cleanups(0, state);
                state = dead();
                break;
            case StmtKind::Break:
                // Leaving the loop body runs the cleanups registered inside it.
                if (!breaks_.empty()) {
                    if (!loops_.empty()) cleanups(loops_.back(), state);
                    merge(breaks_.back(), state);
                }
                state = dead();
                break;
            case StmtKind::Continue:
                if (!continues_.empty()) {
                    if (!loops_.empty()) cleanups(loops_.back(), state);
                    merge(continues_.back(), state);
                }
                state = dead();
                break;
            case StmtKind::While: {
                const auto& loop_stmt = *cast<WhileStmt>(&stmt);
                loop(&loop_stmt.cond, loop_stmt.body, state, true);
                break;
            }
            case StmtKind::For: {
                const auto& loop_stmt = *cast<ForStmt>(&stmt);
                loop_elements_.insert(loop_stmt.var.span.lo);
                eval(loop_stmt.iter, state);
                // An array the loop reads in place, rather than a temporary or a range.
                auto found = path(loop_stmt.iter);
                if (found) iterated_.push_back({std::move(*found), unparen(loop_stmt.iter)->span});
                loop(nullptr, loop_stmt.body, state, true);
                if (found) iterated_.pop_back();
                break;
            }
            case StmtKind::Loop: loop(nullptr, cast<LoopStmt>(&stmt)->body, state, false); break;
            case StmtKind::Defer: {
                // Cleanup reads see the state at registration and never initialize the scope.
                // Its errors are handled inside it, never by an enclosing `try?`.
                // Its moves happen at scope exit and are checked only there, so
                // this check, done once per cleanup, skips them; a quiet pass or
                // a cleanup's own exit run has nothing else to report.
                if (!quiet_ && filter_ != Filter::OnlyMoves) {
                    State cleanup = state;
                    breaks_.push_back(dead());
                    continues_.push_back(dead());
                    auto caught = std::exchange(caught_, {});
                    const auto filter = std::exchange(filter_, Filter::NoMoves);
                    control_flow(*cast<DeferStmt>(&stmt)->body, cleanup);
                    filter_ = filter;
                    caught_ = std::move(caught);
                    continues_.pop_back();
                    breaks_.pop_back();
                }
                defers_.push_back(cast<DeferStmt>(&stmt));
                // Its writes happen at scope exit, so a later write to the same
                // `let`, or another cleanup's, would assign it twice.
                state.maybe.unite(assigned_lets({cast<DeferStmt>(&stmt)->body}));
                break;
            }
            case StmtKind::Expr: eval(cast<ExprStmt>(&stmt)->expr, state); break;
            case StmtKind::Assign: {
                const auto& assign = *cast<AssignStmt>(&stmt);
                const auto* name = as<NameExpr>(unparen(assign.target));
                if (!name) eval(assign.target, state);
                else if (assign.op != AssignOp::Assign) read(*name, state);
                if (assign.op == AssignOp::Assign) consume(assign.value);
                eval(assign.value, state);
                if (state.live) changes(path(assign.target), assign.target->span, text(assign.target->span), "change");
                if (name) write(*name, state);
                else write_place(*assign.target);
                break;
            }
        }
    }

    const Resolution& resolution_;
    const ScalarTypes& types_;
    const Interner& names_;
    DiagEngine& diag_;
    FlatMap<const Item*, BindingId> items_;
    FlatMap<u32, const Param*> params_;  // name span.lo -> parameter of the current callable
    FlatMap<u32, Local> locals_;         // filled before each callable is evaluated
    FlatMap<u32, Field> fields_;
    Place self_;
    Storage self_storage_ = Storage::Unknown;
    const TypeDeclItem* self_owner_ = nullptr;
    struct PendingClosure {
        const ClosureExpr* closure;
        Place self;  // the captured `self`, if the body uses it
        Storage storage;
        const TypeDeclItem* owner;
    };
    std::vector<PendingClosure> closures_;          // created by the current callable, checked after it
    FlatMap<u32, const ClosureParam*> closure_params_;  // name span.lo -> parameter of the current closure
    FlatSet<usize> captured_;                        // bindings the current closure captures
    std::vector<State> breaks_;
    std::vector<State> continues_;
    struct Catch {
        State state;  // where the error exits of a `try?` resume
        usize defers; // defers_ size when the `try?` started
    };
    // Enclosing `try?` expressions, under the outermost `try` that propagates
    // errors out of the callable (with defers 0 and a discarded state).
    std::vector<Catch> caught_;
    usize tracked_ = 0;
    FlatMap<u32, usize> owned_;      // name span.lo -> move bit of an `own` parameter
    FlatSet<const VarItem*> instance_fields_;                       // checked with their type
    FlatMap<const TypeDeclItem*, ParamMoves> primary_moves_;        // into the primary `init` body
    FlatSet<const Expr*> consumed_;  // names whose value is moved where they are read
    FlatMap<const BlockExpr*, Bits> iteration_moves_;  // per loop body, while quiet
    std::vector<Span> move_sites_;   // where each movable binding last moved
    FlatSet<u32> reported_moves_;          // E0504 spans, which cleanups can reach more than once
    const ClosureExpr* closure_ = nullptr;  // the closure whose body is being checked
    FlatSet<usize> invalid_captures_;      // bindings a closure of this callable cannot capture (E0503)
    FlatSet<u32> reported_borrows_;        // E0505 spans, which loops and cleanups can reach more than once
    FlatSet<u32> loop_elements_;           // `for` bindings, which borrow array elements
    FlatSet<u32> reported_exclusive_;      // E0506 spans, likewise
    struct Iterated {
        Path path;
        Span span;
        bool matched = false;  // the value a `match` arm's bindings borrow from, not a loop's array
    };
    std::vector<Iterated> iterated_;       // the arrays that the enclosing `for` loops run over, innermost last
    FlowFacts facts_;
    std::vector<const DeferStmt*> defers_;  // cleanups of the enclosing scopes, innermost last
    std::vector<usize> scopes_;             // defers_ size where each enclosing block starts
    std::vector<usize> loops_;              // defers_ size where each enclosing loop body starts
    usize movables_ = 0;
    usize quiet_ = 0;
    Filter filter_ = Filter::All;
    DiagEngine scratch_;
};

}  // namespace

FlowFacts check_flow(const File& file, const Resolution& resolution, const ScalarTypes& types, const Interner& names,
                     DiagEngine& diag) {
    FlowChecker checker(resolution, types, names, diag);
    checker.run(file);
    return checker.facts();
}

}  // namespace phs
