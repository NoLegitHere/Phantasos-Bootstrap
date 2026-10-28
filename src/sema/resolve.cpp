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

#include "sema/resolve.hpp"

#include <algorithm>
#include <ranges>
#include <variant>

#include "support/flat_map.hpp"

namespace phs {
namespace {
using namespace ast;
struct Context {
    ScopeId scope;
    BindingId owner;
    bool receiver = false;
    bool implicit_receiver = false; // instance field initializers cannot use explicit self yet
    bool closure = false;           // inside a closure body, where outer locals are captured
};
struct TypeScopes {
    ScopeId generics, members, constructor;
    BindingId binding;
};
struct Declare {
    Name name;
    BindingKind kind;
};
struct ExposeParameter { BindingId binding; };
struct FinishExpr { const Expr* expr; };
struct Work {
    std::variant<const Item*, const Expr*, const Stmt*, const TypeExpr*, Declare, ExposeParameter, FinishExpr> node;
    Context context;
};
struct Target {
    ResolutionState state = ResolutionState::Deferred;
    BindingSetId set;
};
}  // namespace

class Resolver {
public:
    Resolver(Interner& names, DiagEngine& diag) : names_(names), diag_(diag) {}

    Resolution run(const File& file) {
        result_.file_ = file.file;
        const ScopeId prelude = scope({}, ScopeKind::Prelude);
        for (std::string_view name : {"Int8", "Int16", "Int32", "Int64", "UInt8", "UInt16", "UInt32",
                                     "UInt64", "Int", "UInt", "Float32", "Float64", "Bool", "Char",
                                     "String", "Unit", "Never", "Error", "Copy", "Sendable"})
            declare(prelude, {names_.intern(name), {}}, BindingKind::BuiltinType, NameSpace::Type);
        for (std::string_view name : {"panic", "print"})
            declare(prelude, {names_.intern(name), {}}, BindingKind::BuiltinValue, NameSpace::Value);
        const ScopeId root = scope(prelude, ScopeKind::File);
        for (const Import& import : file.imports) {
            const Name name = import.alias.sym.empty() ? import.path.parts.back() : import.alias;
            const auto id = declare(root, name, BindingKind::Import, NameSpace::Type);
            names_table_.try_emplace(name_key(root, name.sym, NameSpace::Value), binding_sets_[id.value]);
            diag_.error(DiagCode::ImportUnavailable, import.span,
                        "imports require package loading, which stage0 does not yet support");
        }
        collect(file.items, root);
        for (const Item* item : std::views::reverse(file.items)) push(item, {root});
        while (!work_.empty()) {
            const Work next = work_.back();
            work_.pop_back();
            std::visit([&](auto node) {
                if constexpr (std::is_pointer_v<decltype(node)>) visit(*node, next.context);
                else visit(node, next.context);
            }, next.node);
        }
        finish();
        return std::move(result_);
    }

private:
    // Packs scope bindings and overload sets into flat storage and sorts
    // references for binary search.
    void finish() {
        // A stable counting sort by group keeps each group in declaration order.
        const auto pack = [](const std::vector<Entry>& entries, usize groups, std::vector<BindingId>& items) {
            std::vector<usize> starts(groups + 1);
            for (const auto& entry : entries) ++starts[entry.group + 1];
            for (usize g = 0; g < groups; ++g) starts[g + 1] += starts[g];
            items.resize(entries.size());  // no reallocation later: the spans below point into it
            std::vector<usize> next(starts.begin(), starts.end() - 1);
            for (const auto& entry : entries) items[next[entry.group]++] = entry.binding;
            std::vector<std::span<const BindingId>> spans;
            spans.reserve(groups);
            for (usize g = 0; g < groups; ++g) spans.emplace_back(items.data() + starts[g], starts[g + 1] - starts[g]);
            return spans;
        };
        const auto scopes = pack(scope_entries_, result_.scopes_.size(), result_.scope_items_);
        for (usize i = 0; i < scopes.size(); ++i) result_.scopes_[i].bindings = scopes[i];
        result_.sets_ = pack(set_entries_, set_info_.size(), result_.set_items_);
        auto& refs = result_.references_;
        std::ranges::sort(refs, {}, [](const NameReference& r) { return r.span.lo; });
        for (usize i = 1; i < refs.size(); ++i) PHS_ASSERT(refs[i - 1].span.lo != refs[i].span.lo);
        refs.shrink_to_fit();
        // buckets[b] is the first reference at or after byte b << kReferenceBucketBits.
        auto& buckets = result_.reference_buckets_;
        const usize count = refs.empty() ? 0 : (refs.back().span.lo >> Resolution::kReferenceBucketBits) + 2;
        buckets.resize(count);
        for (usize b = 0, i = 0; b < count; ++b) {
            while (i < refs.size() && (refs[i].span.lo >> Resolution::kReferenceBucketBits) < b) ++i;
            buckets[b] = static_cast<u32>(i);
        }
        result_.bindings_.shrink_to_fit();
        result_.scopes_.shrink_to_fit();
    }

    ScopeId scope(ScopeId parent, ScopeKind kind) {
        const ScopeId id{result_.scopes_.size()};
        result_.scopes_.push_back({parent, kind, {}});
        PHS_ASSERT(id.value < (usize{1} << 31));  // fits name_key()
        scope_inherited_.push_back(false);
        return id;
    }

    BindingId declare(ScopeId into, Name name, BindingKind kind, NameSpace space, const Item* item = nullptr) {
        PHS_ASSERT(!name.sym.empty());
        const BindingId id{result_.bindings_.size()};
        result_.bindings_.push_back({name, kind, into, item, {}});
        scope_entries_.push_back({into.value, id});
        const auto [entry, inserted] = names_table_.try_emplace(name_key(into, name.sym, space), BindingSetId{set_info_.size()});
        const BindingSetId set = entry->second;  // entry is invalidated by later inserts
        if (inserted) set_info_.emplace_back();
        auto& group = set_info_[set.value];
        if (group.size && (kind != BindingKind::Function ||
                           result_.binding(group.first).kind != BindingKind::Function)) {
            auto& d = diag_.error(DiagCode::ConflictingDeclaration, name.span,
                                  "conflicting declaration of '{}'", names_.str(name.sym));
            d.notes.push_back({result_.binding(group.first).name.span, "first declared here"});
        }
        if (!group.size++) group.first = id;
        set_entries_.push_back({set.value, id});
        binding_sets_.push_back(set);
        return id;
    }

    BindingSetId direct(ScopeId in, Symbol name, NameSpace space, bool constructor = false) const {
        if (const auto it = names_table_.find(name_key(in, name, space)); it != names_table_.end()) return it->second;
        if (constructor) {
            if (const auto it = names_table_.find(name_key(in, name, NameSpace::Type)); it != names_table_.end()) return it->second;
        }
        return {};
    }

    Target lookup(ScopeId in, Symbol name, NameSpace space) const {
        while (in.valid()) {
            const auto found = direct(in, name, space, space == NameSpace::Value);
            if (found.valid()) return {ResolutionState::Resolved, found};
            if (scope_inherited_[in.value]) {
                // A type's own generic parameters precede inherited lookup.
                // A base member may shadow an outer declaration: do not commit
                // to the outer binding until inheritance has been checked.
                const auto generic = result_.scopes_[in.value].parent;
                const auto parameter = direct(generic, name, space, space == NameSpace::Value);
                if (parameter.valid()) return {ResolutionState::Resolved, parameter};
                return {};
            }
            in = result_.scopes_[in.value].parent;
        }
        return {ResolutionState::Unresolved, {}};
    }

    Target record(Span span, Context context, NameSpace space, Target target) {
        result_.references_.push_back({span, context.scope, space, target.state, target.set,
                                      context.receiver || context.implicit_receiver ? context.owner : BindingId{}});
        return target;
    }

    Target name_ref(Name name, Context context, NameSpace space) {
        const Target found = lookup(context.scope, name.sym, space);
        if (context.closure && space == NameSpace::Value && found.state == ResolutionState::Resolved) capture(found.set, context, name.span);
        if (found.state != ResolutionState::Unresolved) return record(name.span, context, space, found);
        const auto code = space == NameSpace::Type ? DiagCode::UnknownType : DiagCode::UnknownValue;
        diag_.error(code, name.span, "unknown {} '{}'", space == NameSpace::Type ? "type" : "name", names_.str(name.sym));
        return record(name.span, context, space, {ResolutionState::Unresolved, {}});
    }

    // A local, parameter or pattern binding named inside a closure but declared
    // outside it is captured by that closure and by every closure in between.
    void capture(BindingSetId set, Context context, Span use) {
        if (set_info_[set.value].size != 1) return;
        const BindingId id = set_info_[set.value].first;
        const auto& binding = result_.binding(id);
        if (binding.kind != BindingKind::Local && binding.kind != BindingKind::Parameter && binding.kind != BindingKind::Pattern) return;
        for (ScopeId s = context.scope; s.valid() && s != binding.scope; s = result_.scopes_[s.value].parent) {
            const auto closure = closure_scopes_.find(s.value);
            if (closure == closure_scopes_.end()) continue;
            if (captured_.insert(u64(s.value) << 32 | id.value)) result_.captures_[closure->second].push_back({id, use});
        }
    }

    Target qualified(Name name, Context context, NameSpace space, Target base) {
        if (base.state == ResolutionState::Unresolved)
            return record(name.span, context, space, {ResolutionState::Unresolved, {}});
        if (base.state == ResolutionState::Resolved && base.set.valid() && set_info_[base.set.value].size == 1) {
            const auto& binding = result_.binding(set_info_[base.set.value].first);
            if (binding.members.valid()) {
                const auto found = direct(binding.members, name.sym, space, space == NameSpace::Value);
                if (found.valid()) return record(name.span, context, space, {ResolutionState::Resolved, found});
                // Inherited members require the later type/inheritance pass.
                if (cast_type_decl(binding.item)->supers.empty()) {
                    diag_.error(space == NameSpace::Type ? DiagCode::UnknownType : DiagCode::UnknownValue,
                                name.span, "unknown {} '{}' in '{}'", space == NameSpace::Type ? "type" : "member",
                                names_.str(name.sym), names_.str(binding.name.sym));
                    return record(name.span, context, space, {ResolutionState::Unresolved, {}});
                }
            }
        }
        return record(name.span, context, space, {});
    }

    void path(List<Name> parts, Context context, NameSpace space) {
        if (parts.empty()) return;
        Target target = name_ref(parts.front(), context, space);
        for (Name part : parts.subspan(1)) target = qualified(part, context, space, target);
    }

    template <typename T>
    void push(const T* node, Context context) {
        if (node) work_.push_back({node, context});
    }

    void generics(List<GenericParam> params, ScopeId into) {
        for (const GenericParam& param : params)
            declare(into, param.name, BindingKind::GenericParameter, NameSpace::Type);
    }

    // Precollect every declaration list, including nested types, before any
    // reference is visited. Extern blocks are transparent declaration groups.
    void collect(List<Item*> items, ScopeId into) {
        struct Pending { const Item* item; ScopeId into; };
        std::vector<Pending> pending;
        for (const Item* item : std::views::reverse(items)) pending.push_back({item, into});
        while (!pending.empty()) {
            const auto [item, target] = pending.back();
            pending.pop_back();
            if (const auto* ext = as<ExternItem>(item)) {
                for (const Item* child : std::views::reverse(ext->items)) pending.push_back({child, target});
                continue;
            }
            if (const auto* type = as_type_decl(item)) {
                const auto id = declare(target, item->name, BindingKind::Type, NameSpace::Type, item);
                const auto generic = scope(target, ScopeKind::Generics);
                generics(type->generics, generic);
                const auto members = scope(generic, ScopeKind::Members);
                scope_inherited_[members.value] = !type->supers.empty();
                const auto ctor = scope(members, ScopeKind::Parameters);
                type_scopes_.emplace(type, TypeScopes{generic, members, ctor, id});
                result_.bindings_[id.value].members = members;
                for (const Param& p : type->ctor_params) {
                    constructor_params_.emplace(&p, declare(ctor, p.name, BindingKind::Parameter, NameSpace::Value));
                    if (type->kind == ItemKind::Struct || p.binding == Binding::Let || p.binding == Binding::Var)
                        declare(members, p.name, BindingKind::Field, NameSpace::Value);
                }
                for (const Item* child : std::views::reverse(type->members)) pending.push_back({child, members});
            } else if (!item->name.sym.empty()) {
                const auto kind = item->kind == ItemKind::Fun ? BindingKind::Function :
                                  item->kind == ItemKind::TypeAlias ? BindingKind::TypeAlias :
                                  item->kind == ItemKind::EnumCase ? BindingKind::EnumCase :
                                  result_.scopes_[target.value].kind == ScopeKind::Members ? BindingKind::Field :
                                  BindingKind::Variable;
                declare(target, item->name, kind,
                        kind == BindingKind::TypeAlias ? NameSpace::Type : NameSpace::Value, item);
            }
        }
    }

    void bounds(List<GenericParam> params, List<WhereClause> where, Context context) {
        for (const auto& clause : std::views::reverse(where)) {
            for (const TypeExpr* bound : std::views::reverse(clause.bounds)) push(bound, context);
            push(clause.type, context);
        }
        for (const auto& param : std::views::reverse(params))
            for (const TypeExpr* bound : std::views::reverse(param.bounds)) push(bound, context);
    }

    void arguments(List<Arg> args, Context context) {
        for (const Arg& arg : std::views::reverse(args)) push(arg.value, context);
    }

    void parameters(List<Param> params, Context context, bool constructor = false) {
        // Process defaults before inserting their parameter; all earlier
        // parameters are visible. Body work is scheduled below this work.
        context.receiver = false;
        for (const Param& p : std::views::reverse(params)) {
            if (constructor) work_.push_back({ExposeParameter{constructor_params_.at(&p)}, context});
            else work_.push_back({Declare{p.name, BindingKind::Parameter}, context});
            push(p.default_value, context);
            push(p.type, context);
        }
    }

    void visit(Declare decl, Context context) {
        declare(context.scope, decl.name, decl.kind, NameSpace::Value);
    }

    void visit(ExposeParameter expose, Context context) {
        const auto& binding = result_.binding(expose.binding);
        names_table_.try_emplace(name_key(context.scope, binding.name.sym, NameSpace::Value), binding_sets_[expose.binding.value]);
        scope_entries_.push_back({context.scope.value, expose.binding});
    }

    void visit(const Item& item, Context context) {
        const Context attribute_context = context;
        if (const auto* type = as_type_decl(&item)) {
            const auto s = type_scopes_.at(type);
            for (const Item* child : std::views::reverse(type->members)) push(child, {s.members, s.binding});
            for (const SuperEntry& base : std::views::reverse(type->supers)) {
                arguments(base.args, {s.constructor, s.binding});
                push(base.type, {s.generics, s.binding});
            }
            bounds(type->generics, type->where, {s.generics, s.binding});
            // Defaults must not see fields or later constructor parameters.
            parameters(type->ctor_params, {scope(s.generics, ScopeKind::Parameters), s.binding}, true);
        } else switch (item.kind) {
            case ItemKind::Fun: {
                const auto& fun = *cast<FunItem>(&item);
                Context generic{scope(context.scope, ScopeKind::Generics), context.owner};
                generics(fun.sig.generics, generic.scope);
                Context params{scope(generic.scope, ScopeKind::Parameters), context.owner,
                               context.owner.valid() && !has_modifier(item.mods, Modifier::Static)};
                push(fun.body, params);
                parameters(fun.sig.params, params);
                push(fun.sig.ret, generic);
                push(fun.sig.throws_type, generic);
                bounds(fun.sig.generics, fun.sig.where, generic);
                break;
            }
            case ItemKind::TypeAlias: {
                const auto& alias = *cast<TypeAliasItem>(&item);
                Context generic{scope(context.scope, ScopeKind::Generics), context.owner};
                generics(alias.generics, generic.scope);
                push(alias.type, generic);
                for (const TypeExpr* bound : std::views::reverse(alias.bounds)) push(bound, generic);
                bounds(alias.generics, {}, generic);
                break;
            }
            case ItemKind::Var: {
                const auto& var = *cast<VarItem>(&item);
                if (context.owner.valid() && !has_modifier(item.mods, Modifier::Static)) {
                    const auto* owner = cast_type_decl(result_.binding(context.owner).item);
                    context.scope = type_scopes_.at(owner).constructor;
                    context.implicit_receiver = true;
                }
                push(var.init, context);
                push(var.type, context);
                break;
            }
            case ItemKind::Init: {
                const auto& init = *cast<InitItem>(&item);
                Context body{scope(context.scope, ScopeKind::Parameters), context.owner, true};
                if (!init.has_params) {
                    const auto* owner = cast_type_decl(result_.binding(context.owner).item);
                    body.scope = type_scopes_.at(owner).constructor;
                }
                push(init.body, body);
                parameters(init.params, body);
                push(init.throws_type, context);
                break;
            }
            case ItemKind::Deinit:
                context.receiver = true;
                push(cast<DeinitItem>(&item)->body, context);
                break;
            case ItemKind::EnumCase:
                for (const auto& field : std::views::reverse(cast<EnumCaseItem>(&item)->fields)) push(field.type, context);
                break;
            case ItemKind::Extern:
                for (const Item* child : std::views::reverse(cast<ExternItem>(&item)->items)) push(child, context);
                break;
            default: unreachable();
        }
        // Attribute names and argument keys are metadata; their values are expressions.
        for (const Attribute& attr : std::views::reverse(item.attrs))
            for (const AttrArg& arg : std::views::reverse(attr.args)) push(arg.value, attribute_context);
    }

    Target self(Span span, Context context, bool value, bool super = false) {
        bool allowed = context.owner.valid() && (!value || context.receiver);
        if (allowed && super)
            allowed = !cast_type_decl(result_.binding(context.owner).item)->supers.empty();
        if (!allowed) {
            diag_.error(DiagCode::InvalidReceiver, span, "'{}' requires {}", super ? "super" : value ? "self" : "Self",
                        super ? "an instance body with a base type" : value ? "an instance body" : "an enclosing type");
            return record(span, context, value ? NameSpace::Value : NameSpace::Type, {ResolutionState::Unresolved, {}});
        }
        return record(span, context, value ? NameSpace::Value : NameSpace::Type,
                      super ? Target{} : Target{ResolutionState::Resolved, binding_sets_[context.owner.value]});
    }

    void visit(const TypeExpr& type, Context context) {
        if (type.kind == TypeKind::Path) path(type.path.parts, context, NameSpace::Type);
        if (type.kind == TypeKind::SelfType) self(type.span, context, false);
        push(type.throws_type, context);
        push(type.inner, context);
        for (const TypeExpr* arg : std::views::reverse(type.args)) push(arg, context);
    }

    Context condition(const Cond& cond, Context context) {
        Context body = context;
        if (cond.is_let) {
            body.scope = scope(context.scope, ScopeKind::Condition);
            declare(body.scope, cond.name, BindingKind::Local, NameSpace::Value);
        }
        return body;
    }

    void visit(const Stmt& stmt, Context context) {
        switch (stmt.kind) {
            case StmtKind::Var: {
                const auto& var = *cast<VarStmt>(&stmt);
                if (var.names.empty()) work_.push_back({Declare{var.name, BindingKind::Local}, context});
                for (const Name& name : std::views::reverse(var.names)) work_.push_back({Declare{name, BindingKind::Local}, context});
                push(var.init, context);
                push(var.type, context);
                break;
            }
            case StmtKind::Return: push(cast<ReturnStmt>(&stmt)->value, context); break;
            case StmtKind::Throw: push(cast<ThrowStmt>(&stmt)->value, context); break;
            case StmtKind::While: {
                const auto& loop = *cast<WhileStmt>(&stmt);
                push(loop.body, condition(loop.cond, context));
                push(loop.cond.expr, context);
                push(loop.cond.type, context);
                break;
            }
            case StmtKind::For: {
                const auto& loop = *cast<ForStmt>(&stmt);
                Context body = context;
                body.scope = scope(context.scope, ScopeKind::Loop);
                declare(body.scope, loop.var, BindingKind::Local, NameSpace::Value);
                push(loop.body, body);
                push(loop.iter, context);
                break;
            }
            case StmtKind::Loop: push(cast<LoopStmt>(&stmt)->body, context); break;
            case StmtKind::Defer: push(cast<DeferStmt>(&stmt)->body, context); break;
            case StmtKind::Expr: push(cast<ExprStmt>(&stmt)->expr, context); break;
            case StmtKind::Assign: {
                const auto& assign = *cast<AssignStmt>(&stmt);
                push(assign.value, context);
                push(assign.target, context);
                break;
            }
            case StmtKind::Break:
            case StmtKind::Continue: break;
        }
    }

    void pattern(const Pattern& root, Context context) {
        Context enclosing = context;
        enclosing.scope = result_.scopes_[context.scope.value].parent;
        std::vector<const Pattern*> pending{&root};
        while (!pending.empty()) {
            const Pattern* pat = pending.back();
            pending.pop_back();
            Name binding;
            switch (pat->kind) {
                case PatKind::Binding: binding = cast<BindingPat>(pat)->name; break;
                case PatKind::Is:
                    binding = cast<IsPat>(pat)->binding;
                    push(cast<IsPat>(pat)->type, enclosing);
                    break;
                case PatKind::Case: {
                    const auto& c = *cast<CasePat>(pat);
                    if (c.leading_dot) record(c.path.front().span, enclosing, NameSpace::Value, {});
                    else path(c.path, enclosing, NameSpace::Value);
                    for (const Pattern* arg : std::views::reverse(c.args)) pending.push_back(arg);
                    break;
                }
                case PatKind::Alt:
                    for (const Pattern* alt : std::views::reverse(cast<AltPat>(pat)->alts)) pending.push_back(alt);
                    break;
                case PatKind::Literal: push(cast<LiteralPat>(pat)->literal, context); break;
                case PatKind::Wildcard:
                case PatKind::Error: break;
            }
            // Validation already proved alternative binding sets agree. All
            // occurrences of that binding share the first declaration's ID.
            if (!binding.sym.empty() && !direct(context.scope, binding.sym, NameSpace::Value).valid())
                declare(context.scope, binding, BindingKind::Pattern, NameSpace::Value);
        }
    }

    void visit(FinishExpr finish, Context context) {
        const Expr* expr = finish.expr;
        if (const auto* member = as<MemberExpr>(expr)) {
            expression_targets_[expr] = qualified(member->name, context, NameSpace::Value, target(member->base));
        } else if (const auto* paren = as<ParenExpr>(expr)) expression_targets_[expr] = target(paren->inner);
        else if (const auto* generic = as<GenericInstExpr>(expr)) expression_targets_[expr] = target(generic->base);
        else unreachable();
    }

    Target target(const Expr* expr) const {
        if (const auto it = expression_targets_.find(expr); it != expression_targets_.end()) return it->second;
        return {};
    }

    void visit(const Expr& expr, Context context) {
        switch (expr.kind) {
            case ExprKind::Name:
                expression_targets_[&expr] = name_ref(cast<NameExpr>(&expr)->name, context, NameSpace::Value);
                break;
            case ExprKind::SelfType: expression_targets_[&expr] = self(expr.span, context, false); break;
            // Instance receivers retain their reference, but their members need
            // type checking (including inheritance and static/instance rules).
            case ExprKind::SelfValue: self(expr.span, context, true); break;
            case ExprKind::Super: self(expr.span, context, true, true); break;
            case ExprKind::ImplicitMember: record(cast<ImplicitMemberExpr>(&expr)->name.span, context, NameSpace::Value, {}); break;
            case ExprKind::Block:
                context.scope = scope(context.scope, ScopeKind::Block);
                for (const Stmt* stmt : std::views::reverse(cast<BlockExpr>(&expr)->stmts)) push(stmt, context);
                break;
            case ExprKind::If: {
                const auto& branch = *cast<IfExpr>(&expr);
                push(branch.else_branch, context);
                push(branch.then_block, condition(branch.cond, context));
                push(branch.cond.expr, context);
                push(branch.cond.type, context);
                break;
            }
            case ExprKind::Match: {
                const auto& match = *cast<MatchExpr>(&expr);
                for (const MatchArm& arm : std::views::reverse(match.arms)) {
                    Context local = context;
                    local.scope = scope(context.scope, ScopeKind::MatchArm);
                    push(arm.body, local);
                    push(arm.guard, local);
                    pattern(*arm.pattern, local);
                }
                push(match.scrutinee, context);
                break;
            }
            case ExprKind::Unary: push(cast<UnaryExpr>(&expr)->operand, context); break;
            case ExprKind::Binary: {
                const auto& binary = *cast<BinaryExpr>(&expr);
                push(binary.rhs, context);
                push(binary.lhs, context);
                break;
            }
            case ExprKind::Call: {
                const auto& call = *cast<CallExpr>(&expr);
                arguments(call.args, context);
                push(call.callee, context);
                break;
            }
            case ExprKind::Index: {
                const auto& index = *cast<IndexExpr>(&expr);
                push(index.index, context);
                push(index.base, context);
                break;
            }
            case ExprKind::Interp:
                for (const Expr* child : std::views::reverse(cast<InterpExpr>(&expr)->exprs)) push(child, context);
                break;
            case ExprKind::Array:
                for (const Expr* child : std::views::reverse(cast<ArrayExpr>(&expr)->elems)) push(child, context);
                break;
            case ExprKind::Tuple:
                for (const Expr* child : std::views::reverse(cast<TupleExpr>(&expr)->elems)) push(child, context);
                break;
            case ExprKind::Paren:
                work_.push_back({FinishExpr{&expr}, context});
                push(cast<ParenExpr>(&expr)->inner, context);
                break;
            case ExprKind::Cast:
                push(cast<CastExpr>(&expr)->type, context);
                push(cast<CastExpr>(&expr)->operand, context);
                break;
            case ExprKind::Member:
                work_.push_back({FinishExpr{&expr}, context});
                push(cast<MemberExpr>(&expr)->base, context);
                break;
            case ExprKind::OptMember:
                record(cast<OptMemberExpr>(&expr)->name.span, context, NameSpace::Value, {});
                push(cast<OptMemberExpr>(&expr)->base, context);
                break;
            case ExprKind::Unwrap: push(cast<UnwrapExpr>(&expr)->operand, context); break;
            case ExprKind::TupleField: push(cast<TupleFieldExpr>(&expr)->base, context); break;
            case ExprKind::GenericInst: {
                const auto& generic = *cast<GenericInstExpr>(&expr);
                work_.push_back({FinishExpr{&expr}, context});
                for (const TypeExpr* arg : std::views::reverse(generic.args)) push(arg, context);
                push(generic.base, context);
                break;
            }
            case ExprKind::Unsafe: push(cast<UnsafeExpr>(&expr)->block, context); break;
            case ExprKind::Closure: {
                const auto& closure = *cast<ClosureExpr>(&expr);
                Context inner = context;
                inner.scope = scope(context.scope, ScopeKind::Closure);
                inner.closure = true;
                closure_scopes_.emplace(inner.scope.value, &closure);
                push(closure.body, inner);
                for (const ClosureParam& p : std::views::reverse(closure.params)) {
                    work_.push_back({Declare{p.name, BindingKind::Parameter}, inner});
                    push(p.type, context);
                }
                break;
            }
            case ExprKind::Int:
            case ExprKind::Float:
            case ExprKind::Char:
            case ExprKind::String:
            case ExprKind::Bool:
            case ExprKind::None:
            case ExprKind::Unit:
            case ExprKind::Error: break;
        }
    }

    Interner& names_;
    DiagEngine& diag_;
    Resolution result_;
    // Every scope's names in one table, so a scope costs no allocation.
    static u64 name_key(ScopeId scope, Symbol name, NameSpace space) {
        return u64(scope.value) << 33 | u64(space == NameSpace::Value) << 32 | name.id;
    }
    FlatMap<u64, BindingSetId> names_table_;
    std::vector<u8> scope_inherited_;  // scope id -> members scope of a type with bases
    std::vector<BindingSetId> binding_sets_;
    // (scope or set, binding) in declaration order, packed into result_ by finish().
    struct Entry { usize group; BindingId binding; };
    std::vector<Entry> scope_entries_, set_entries_;
    struct SetInfo { BindingId first; u32 size = 0; };
    std::vector<SetInfo> set_info_;
    FlatMap<const TypeDeclItem*, TypeScopes> type_scopes_;
    FlatMap<const Param*, BindingId> constructor_params_;
    FlatMap<const Expr*, Target> expression_targets_;
    FlatMap<usize, const ClosureExpr*> closure_scopes_;  // a closure's own scope -> the closure
    FlatSet<u64> captured_;                               // (closure scope, binding) pairs already recorded
    std::vector<Work> work_;
};

const NameReference* Resolution::reference(Span span) const {
    if (span.file != file_) return nullptr;
    // Callers mostly ask in source order, so the bucket table and the
    // references it points to are read sequentially, unlike a binary search.
    const usize bucket = span.lo >> kReferenceBucketBits;
    if (bucket + 1 >= reference_buckets_.size()) return nullptr;
    for (u32 i = reference_buckets_[bucket]; i < reference_buckets_[bucket + 1]; ++i) {
        const auto& reference = references_[i];
        if (reference.span.lo < span.lo) continue;
        return reference.span.lo == span.lo && reference.span.hi == span.hi ? &reference : nullptr;
    }
    return nullptr;
}

Resolution resolve_file(const ast::File& file, Interner& names, DiagEngine& diag) {
    return Resolver(names, diag).run(file);
}

}  // namespace phs
