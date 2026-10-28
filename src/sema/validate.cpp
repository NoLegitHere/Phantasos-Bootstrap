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

#include "sema/validate.hpp"

#include <algorithm>
#include <ranges>
#include <variant>
#include <vector>

#include "support/flat_map.hpp"

namespace phs {
namespace {
using namespace ast;
using Names = FlatMap<u32, Span>;

struct Context {
    const TypeDeclItem* owner = nullptr;
    bool callable = false;
    bool loop = false;
    bool can_throw = false;
    bool async = false;
    bool closure = false;  // innermost callable is a closure, which cannot throw or await
    bool deferred = false;
};

// Context travels with each work item: visiting one branch cannot leak loop,
// callable or type state into a sibling. Flat AST chains never use the C++ stack.
struct Work {
    std::variant<const Item*, const Expr*, const Stmt*> node;
    Context context;
};

class Validator {
public:
    Validator(const Interner& names, DiagEngine& diag) : names_(names), diag_(diag) {}

    void run(const File& file) {
        declarations(file.items);
        for (const Item* item : std::views::reverse(file.items)) push(item, {});
        while (!work_.empty()) {
            const Work next = work_.back();
            work_.pop_back();
            std::visit([&](auto node) { visit(*node, next.context); }, next.node);
        }
    }

private:
    template <typename T>
    void push(const T* node, Context context) {
        if (node) work_.push_back({node, context});
    }

    void bind(Names& scope, Name name) {
        if (name.sym.empty()) return;
        const auto [first, inserted] = scope.emplace(name.sym.id, name.span);
        if (!inserted) {
            auto& d = diag_.error(DiagCode::DuplicateBinding, name.span,
                                  "duplicate binding '{}'", names_.str(name.sym));
            d.notes.push_back({first->second, "first declared here"});
        }
    }

    template <typename T>
    void unique_names(List<T> entries) {
        Names scope;
        for (const auto& entry : entries) bind(scope, entry.name);
    }

    void declarations(List<Item*> items, List<Param> ctor_params = {}, bool promote_all = false) {
        // Type and value declaration checks are separate. Function overloads
        // and cross-namespace collisions require the later name/type pass.
        Names types, values;
        for (const Param& param : ctor_params) {
            if (!param.name.sym.empty() &&
                (promote_all || param.binding == Binding::Let || param.binding == Binding::Var))
                // Parameter-list duplicates are diagnosed separately. Keep the
                // earliest promoted declaration as the note target for members.
                values.try_emplace(param.name.sym.id, param.name.span);
        }
        for (const Item* item : items) {
            if (is_type_decl(item->kind) || item->kind == ItemKind::TypeAlias)
                bind(types, item->name);
            else if (item->kind == ItemKind::Var || item->kind == ItemKind::EnumCase)
                bind(values, item->name);
        }
    }

    void defaults(List<Param> parameters, Context context) {
        for (const Param& p : std::views::reverse(parameters)) push(p.default_value, context);
    }

    void params(List<Param> parameters, Context context) {
        unique_names(parameters);
        for (const Param& p : parameters) {
            if (p.binding == Binding::Let || p.binding == Binding::Var) {
                const Span keyword{p.span.file, p.span.lo, p.span.lo + 3};
                diag_.error(DiagCode::InvalidParameterBinding, keyword,
                            "'{}' parameter bindings are only allowed in primary constructors",
                            p.binding == Binding::Let ? "let" : "var");
            }
        }
        defaults(parameters, context);
    }

    void arguments(List<Arg> args, Context context) {
        for (const Arg& arg : std::views::reverse(args)) push(arg.value, context);
    }

    void visit(const Item& item, Context context) {
        if (const auto* type = as_type_decl(&item)) {
            unique_names(type->generics);
            unique_names(type->ctor_params);
            declarations(type->members, type->ctor_params, type->kind == ItemKind::Struct);
            const Context member{.owner = type};
            const Item* deinit = nullptr;
            const Item* primary_init = nullptr;
            for (const Item* child : type->members) {
                const bool primary = as<InitItem>(child) && !cast<InitItem>(child)->has_params;
                if (child->kind == ItemKind::Deinit || primary) {
                    const Item*& first = primary ? primary_init : deinit;
                    if (first) {
                        auto& d = diag_.error(DiagCode::InvalidDeclarationContext, child->span,
                                              "duplicate {} body", primary ? "primary init" : "deinit");
                        d.notes.push_back({first->span, "first declared here"});
                    } else {
                        first = child;
                    }
                }
            }
            for (const Item* child : std::views::reverse(type->members)) push(child, member);
            for (const SuperEntry& base : std::views::reverse(type->supers)) arguments(base.args, member);
            defaults(type->ctor_params, member);
        } else {
            switch (item.kind) {
                case ItemKind::Fun: {
                    const auto& f = *cast<FunItem>(&item);
                    unique_names(f.sig.generics);
                    push(f.body, {.owner = context.owner, .callable = true,
                                  .can_throw = f.sig.throws, .async = has_modifier(f.mods, Modifier::Async)});
                    params(f.sig.params, context);
                    break;
                }
                case ItemKind::Var: push(cast<VarItem>(&item)->init, context); break;
                case ItemKind::Extern: {
                    const auto items = cast<ExternItem>(&item)->items;
                    for (const Item* child : std::views::reverse(items)) push(child, context);
                    break;
                }
                case ItemKind::TypeAlias: {
                    const auto& alias = *cast<TypeAliasItem>(&item);
                    unique_names(alias.generics);
                    const bool associated = context.owner && context.owner->kind == ItemKind::Interface;
                    if (!associated && (!alias.type || !alias.bounds.empty()))
                        diag_.error(DiagCode::InvalidTypeAlias, alias.span,
                                    "a type alias outside an interface requires '= Type' and cannot declare bounds");
                    break;
                }
                case ItemKind::Init: {
                    const auto& init = *cast<InitItem>(&item);
                    if (!context.owner)
                        diag_.error(DiagCode::InvalidDeclarationContext, init.span,
                                    "'init' is only allowed as a type member");
                    push(init.body, {.owner = context.owner, .callable = true, .can_throw = init.throws});
                    params(init.params, context);
                    break;
                }
                case ItemKind::Deinit:
                    if (!context.owner)
                        diag_.error(DiagCode::InvalidDeclarationContext, item.span,
                                    "'deinit' is only allowed as a type member");
                    push(cast<DeinitItem>(&item)->body, {.owner = context.owner, .callable = true});
                    break;
                case ItemKind::EnumCase: {
                    Names labels;
                    for (const CaseField& field : cast<EnumCaseItem>(&item)->fields) bind(labels, field.label);
                    break;
                }
                default: unreachable();  // type declarations were handled above
            }
        }
        // Attribute values use the normal expression grammar. Attribute-specific
        // value restrictions belong to a later pass; flow rules apply already.
        for (const Attribute& attr : std::views::reverse(item.attrs))
            for (const AttrArg& arg : std::views::reverse(attr.args)) push(arg.value, context);
    }

    void visit(const Stmt& stmt, Context context) {
        switch (stmt.kind) {
            case StmtKind::Var: push(cast<VarStmt>(&stmt)->init, context); break;
            case StmtKind::Return:
                if (context.deferred)
                    diag_.error(DiagCode::InvalidControlFlow, stmt.span, "'return' cannot leave a defer block");
                else if (!context.callable)
                    diag_.error(DiagCode::InvalidControlFlow, stmt.span,
                                "'return' requires a function, init or deinit body");
                push(cast<ReturnStmt>(&stmt)->value, context);
                break;
            case StmtKind::Break:
            case StmtKind::Continue:
                if (!context.loop) {
                    const auto keyword = stmt.kind == StmtKind::Break ? "break" : "continue";
                    if (context.deferred)
                        diag_.error(DiagCode::InvalidControlFlow, stmt.span,
                                    "'{}' cannot leave a defer block; it requires a loop inside the block", keyword);
                    else if (context.closure)
                        diag_.error(DiagCode::InvalidControlFlow, stmt.span,
                                    "'{}' cannot leave a closure; it requires a loop inside the closure", keyword);
                    else
                        diag_.error(DiagCode::InvalidControlFlow, stmt.span,
                                    "'{}' requires an enclosing loop body", keyword);
                }
                break;
            case StmtKind::Throw:
                if (!context.can_throw) {
                    if (context.deferred)
                        diag_.error(DiagCode::ThrowOutsideThrows, stmt.span,
                                    "'throw' cannot escape a defer block; handle it with 'try?' inside the block");
                    else if (context.closure)
                        diag_.error(DiagCode::ThrowOutsideThrows, stmt.span,
                                    "'throw' cannot escape a closure, since closures cannot throw; handle it with 'try?' inside the closure");
                    else
                        diag_.error(DiagCode::ThrowOutsideThrows, stmt.span,
                                    "'throw' requires a throwing function/init or a 'try?' expression");
                }
                push(cast<ThrowStmt>(&stmt)->value, context);
                break;
            case StmtKind::While: {
                const auto& loop = *cast<WhileStmt>(&stmt);
                Context body = context;
                body.loop = true;
                push(loop.body, body);
                push(loop.cond.expr, context);
                break;
            }
            case StmtKind::For: {
                const auto& loop = *cast<ForStmt>(&stmt);
                Context body = context;
                body.loop = true;
                push(loop.body, body);
                push(loop.iter, context);
                break;
            }
            case StmtKind::Loop:
                context.loop = true;
                push(cast<LoopStmt>(&stmt)->body, context);
                break;
            case StmtKind::Defer:
                // Cleanup is a fresh, synchronous control-flow boundary. Loops
                // and try? inside it can establish their own local contexts.
                push(cast<DeferStmt>(&stmt)->body, {.owner = context.owner, .deferred = true});
                break;
            case StmtKind::Expr: push(cast<ExprStmt>(&stmt)->expr, context); break;
            case StmtKind::Assign: {
                const auto& assign = *cast<AssignStmt>(&stmt);
                push(assign.value, context);
                push(assign.target, context);
                break;
            }
        }
    }

    void pattern(const Pattern& root) {
        // Each alternative gets an independent binding set. Merge on exit,
        // checking that every branch exposes exactly the same names to the arm.
        struct Frame {
            const Pattern* node;
            usize scope;
            bool merge = false;
            usize first_child = 0;
        };
        std::vector<Names> scopes(1);
        std::vector<Frame> todo{{&root, 0}};
        while (!todo.empty()) {
            const Frame next = todo.back();
            todo.pop_back();
            if (next.merge) {
                const auto* alt = cast<AltPat>(next.node);
                const Names& first = scopes[next.first_child];
                Names combined;
                for (usize i = 0; i < alt->alts.size(); ++i) {
                    const Names& branch = scopes[next.first_child + i];
                    if (i != 0 && (branch.size() != first.size() ||
                        !std::ranges::all_of(first, [&](const auto& entry) { return branch.contains(entry.first); })))
                        diag_.error(DiagCode::PatternBindingMismatch, alt->alts[i]->span,
                                    "pattern alternatives must bind the same names");
                    for (const auto& [id, span] : branch) {
                        const auto [entry, inserted] = combined.try_emplace(id, span);
                        if (!inserted && span.lo < entry->second.lo) entry->second = span;
                    }
                }
                // Invalid alternatives recover with the union of their names,
                // retaining the earliest span. Parent duplicate checks therefore
                // do not depend on which alternative introduced a name first.
                std::vector<Name> bindings;
                for (const auto& [id, span] : combined) bindings.push_back({Symbol{id}, span});
                std::ranges::sort(bindings, {}, [](const Name& name) { return name.span.lo; });
                for (Name name : bindings) bind(scopes[next.scope], name);
                continue;
            }
            switch (next.node->kind) {
                case PatKind::Binding: bind(scopes[next.scope], cast<BindingPat>(next.node)->name); break;
                case PatKind::Is: bind(scopes[next.scope], cast<IsPat>(next.node)->binding); break;
                case PatKind::Case:
                    for (const Pattern* arg : std::views::reverse(cast<CasePat>(next.node)->args))
                        todo.push_back({arg, next.scope});
                    break;
                case PatKind::Alt: {
                    const auto alts = cast<AltPat>(next.node)->alts;
                    if (alts.empty()) break;
                    const usize begin = scopes.size();
                    scopes.resize(begin + alts.size());
                    todo.push_back({next.node, next.scope, true, begin});
                    for (usize i = alts.size(); i > 0; --i) todo.push_back({alts[i - 1], begin + i - 1});
                    break;
                }
                case PatKind::Wildcard:
                case PatKind::Literal:
                case PatKind::Error: break;
            }
        }
    }

    void visit(const Expr& expr, Context context) {
        switch (expr.kind) {
            case ExprKind::Block: {
                const auto stmts = cast<BlockExpr>(&expr)->stmts;
                Names locals;
                for (const Stmt* stmt : stmts)
                    if (const auto* var = as<VarStmt>(stmt)) {
                        bind(locals, var->name);
                        for (const Name& name : var->names) bind(locals, name);
                    }
                for (const Stmt* stmt : std::views::reverse(stmts)) push(stmt, context);
                break;
            }
            case ExprKind::If: {
                const auto& branch = *cast<IfExpr>(&expr);
                push(branch.else_branch, context);
                push(branch.then_block, context);
                push(branch.cond.expr, context);
                break;
            }
            case ExprKind::Match: {
                const auto& match = *cast<MatchExpr>(&expr);
                for (const MatchArm& arm : match.arms) pattern(*arm.pattern);
                for (const MatchArm& arm : std::views::reverse(match.arms)) {
                    push(arm.body, context);
                    push(arm.guard, context);
                }
                push(match.scrutinee, context);
                break;
            }
            case ExprKind::Unary: {
                const auto& unary = *cast<UnaryExpr>(&expr);
                if (unary.op == UnOp::Await && !context.async) {
                    if (context.deferred)
                        diag_.error(DiagCode::AwaitOutsideAsync, unary.span, "'await' is not allowed in a defer block");
                    else if (context.closure)
                        diag_.error(DiagCode::AwaitOutsideAsync, unary.span, "'await' is not allowed in a closure");
                    else
                        diag_.error(DiagCode::AwaitOutsideAsync, unary.span, "'await' requires an async function body");
                }
                if (unary.op == UnOp::TryOpt) context.can_throw = true;
                push(unary.operand, context);
                break;
            }
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
            case ExprKind::Paren: push(cast<ParenExpr>(&expr)->inner, context); break;
            case ExprKind::Cast: push(cast<CastExpr>(&expr)->operand, context); break;
            case ExprKind::Member: push(cast<MemberExpr>(&expr)->base, context); break;
            case ExprKind::OptMember: push(cast<OptMemberExpr>(&expr)->base, context); break;
            case ExprKind::Unwrap: push(cast<UnwrapExpr>(&expr)->operand, context); break;
            case ExprKind::TupleField: push(cast<TupleFieldExpr>(&expr)->base, context); break;
            case ExprKind::GenericInst: push(cast<GenericInstExpr>(&expr)->base, context); break;
            case ExprKind::Unsafe: push(cast<UnsafeExpr>(&expr)->block, context); break;
            case ExprKind::Closure: {
                // A closure body is its own callable: `return` leaves the closure,
                // and loops outside it are not targets for `break` or `continue`.
                const auto& closure = *cast<ClosureExpr>(&expr);
                unique_names(closure.params);
                push(closure.body, {.owner = context.owner, .callable = true, .closure = true});
                break;
            }
            case ExprKind::Int:
            case ExprKind::Float:
            case ExprKind::Char:
            case ExprKind::String:
            case ExprKind::Bool:
            case ExprKind::None:
            case ExprKind::Unit:
            case ExprKind::SelfValue:
            case ExprKind::Super:
            case ExprKind::SelfType:
            case ExprKind::Name:
            case ExprKind::ImplicitMember:
            case ExprKind::Error: break;
        }
    }

    const Interner& names_;
    DiagEngine& diag_;
    std::vector<Work> work_;
};
}  // namespace

void validate_file(const ast::File& file, const Interner& names, DiagEngine& diag) {
    Validator(names, diag).run(file);
}

}  // namespace phs
