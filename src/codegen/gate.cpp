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

#include "codegen/gate.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace phs {
namespace {
using namespace ast;

std::string_view expr_name(ExprKind kind) {
    switch (kind) {
        case ExprKind::Float: return "floating-point values";
        case ExprKind::Char: return "character values";
        case ExprKind::String: return "string literals";
        case ExprKind::Interp: return "string interpolation";
        case ExprKind::None: return "'none'";
        case ExprKind::SelfValue: return "'self'";
        case ExprKind::Super: return "'super'";
        case ExprKind::SelfType: return "'Self'";
        case ExprKind::ImplicitMember: return "implicit member expressions";
        case ExprKind::Array: return "array literals";
        case ExprKind::Tuple: return "tuples";
        case ExprKind::TupleField: return "tuple elements";
        case ExprKind::Cast: return "casts";
        case ExprKind::Member: return "member access";
        case ExprKind::OptMember: return "optional member access";
        case ExprKind::Index: return "indexing";
        case ExprKind::Unwrap: return "optional unwrapping";
        case ExprKind::GenericInst: return "explicit generic arguments";
        case ExprKind::Match: return "'match'";
        case ExprKind::Unsafe: return "'unsafe' blocks";
        case ExprKind::Closure: return "closures";
        default: return "this expression";
    }
}

std::string_view unary_name(UnOp op) {
    switch (op) {
        case UnOp::Try: return "'try'";
        case UnOp::TryOpt: return "'try?'";
        case UnOp::Await: return "'await'";
        case UnOp::Share: return "sharing";
        default: return "this operator";
    }
}

std::string_view binary_name(BinOp op) {
    switch (op) {
        case BinOp::Coalesce: return "'?\?'";
        case BinOp::Range: case BinOp::RangeExcl: return "ranges";
        case BinOp::Shl: case BinOp::Shr: return "shifts";
        default: return "this operator";
    }
}

bool supported_binary(BinOp op) {
    return op != BinOp::Range && op != BinOp::RangeExcl && op != BinOp::Shl &&
           op != BinOp::Shr;
}

bool supported_unary(UnOp op) { return op == UnOp::Neg || op == UnOp::Not || op == UnOp::BitNot; }

const Expr* unparen(const Expr* expr) {
    while (const auto* paren = as<ParenExpr>(expr)) expr = paren->inner;
    return expr;
}

bool supported_scalar(ScalarType type) {
    if (type.optional_depth != 0) return false;
    switch (type.kind) {
        case ScalarType::Int8: case ScalarType::Int16: case ScalarType::Int32: case ScalarType::Int64:
        case ScalarType::UInt8: case ScalarType::UInt16: case ScalarType::UInt32: case ScalarType::UInt64:
        case ScalarType::Int: case ScalarType::UInt: case ScalarType::Bool: case ScalarType::String:
        case ScalarType::Unit: case ScalarType::Never:
            return true;
        default: return false;
    }
}

// Iterative over expressions and statements. Control expressions in discarded
// positions have no value, so only their parts are checked.
class Gate {
public:
    Gate(const Resolution& resolution, ScalarTypes& types, const Interner& names, DiagEngine& diag)
        : resolution_(resolution), types_(types), names_(names), diag_(diag) {
        for (usize id = 0; id < resolution.bindings().size(); ++id) {
            const auto& binding = resolution.bindings()[id];
            if (binding.item) items_.emplace(binding.item, BindingId{id});
            if (binding.kind == BindingKind::Local || binding.kind == BindingKind::Parameter ||
                binding.kind == BindingKind::GenericParameter)
                values_.emplace(binding.name.span.lo, BindingId{id});
        }
    }

    bool run(const File& file) {
        for (const auto& import : file.imports) unsupported(import.span, "imports");
        interfaces(file);
        structs(file);
        // Methods of supported structs and interfaces; an interface's are
        // checked once, with `Self` opaque, like a generic function.
        std::vector<const FunItem*> methods;
        for (const Item* item : file.items)
            if ((item->kind == ItemKind::Struct && structs_.contains(items_.at(item).value)) ||
                (item->kind == ItemKind::Interface && interfaces_.contains(items_.at(item).value)))
                for (const Item* member : as_type_decl(item)->members)
                    if (const auto* method = as<FunItem>(member)) {
                        methods.push_back(method);
                        owners_.emplace(method, as_type_decl(item));
                    }
        functions_.insert(methods.begin(), methods.end());
        std::vector<const FunItem*> mains;
        for (const Item* item : file.items) {
            if (item->kind == ItemKind::Struct || item->kind == ItemKind::Interface) continue;
            const auto* fun = as<FunItem>(item);
            if (!fun) {
                unsupported(item->kind == ItemKind::Extern ? item->span : item->name.span, item_name(item->kind));
                continue;
            }
            functions_.insert(fun);
            if (names_.str(fun->name.sym) == "main") mains.push_back(fun);
        }
        for (const Item* item : file.items)
            if (const auto* fun = as<FunItem>(item)) function(*fun);
        for (const auto* method : methods) function(*method);
        entry(file, mains);
        if (ok_) instances(file, methods);
        return ok_;
    }

private:
    struct Work {
        std::variant<const Expr*, const Stmt*> node;
        bool used;
    };

    static std::string_view item_name(ItemKind kind) {
        switch (kind) {
            case ItemKind::Class: return "classes";
            case ItemKind::Struct: return "structs";
            case ItemKind::Actor: return "actors";
            case ItemKind::Interface: return "interfaces";
            case ItemKind::Enum: return "enums";
            case ItemKind::Var: return "global variables";
            case ItemKind::Extern: return "extern blocks";
            case ItemKind::TypeAlias: return "type aliases";
            default: return "this declaration";
        }
    }

    // ------------------------------------------------------------ interfaces

    // An interface is supported when it is a non-generic list of instance
    // methods, with or without default bodies. Its methods see `Self` as an
    // opaque type, and calls to them on a struct go to its witnesses (3.25).
    // Values of an interface type are not supported.
    void interfaces(const File& file) {
        for (const Item* item : file.items) {
            if (item->kind != ItemKind::Interface) continue;
            const auto& type = *as_type_decl(item);
            const bool before = ok_;
            ok_ = true;
            if (!type.attrs.empty()) unsupported(type.attrs.front().span, "attributes");
            if (!type.generics.empty()) unsupported(type.generics.front().name.span, "generic interfaces");
            if (!type.supers.empty()) unsupported(type.supers.front().type->span, "interfaces that extend interfaces");
            if (!type.where.empty()) unsupported(type.where.front().type->span, "'where' clauses");
            for (const Item* member : type.members) {
                if (const auto* method = as<FunItem>(member)) {
                    if (has_modifier(method->mods, Modifier::Static)) unsupported(method->name.span, "static methods");
                    else if (!method->sig.generics.empty())
                        unsupported(method->sig.generics.front().name.span, "generic methods in interfaces");
                    continue;
                }
                unsupported(member->name.sym.empty() ? member->span : member->name.span, member_name(member->kind));
            }
            if (ok_) interfaces_.insert(items_.at(item).value);
            ok_ = ok_ && before;
        }
    }

    // The supported interface a type expression names, if any.
    BindingId interface(const TypeExpr& type) const {
        if (type.kind != TypeKind::Path || type.path.parts.size() != 1 || !type.args.empty()) return {};
        const auto* ref = resolution_.reference(type.path.parts.front().span);
        if (!ref || ref->state != ResolutionState::Resolved) return {};
        const auto ids = resolution_.candidates(ref->target);
        return ids.size() == 1 && interfaces_.contains(ids.front().value) ? ids.front() : BindingId{};
    }

    // Type parameters may be bounded by supported interfaces only.
    void bounds(std::span<const GenericParam> generics, std::span<const WhereClause> where) {
        for (const auto& param : generics)
            for (const TypeExpr* bound : param.bounds)
                if (!interface(*bound).valid()) unsupported(bound->span, "bounds other than supported interfaces");
        if (!where.empty()) unsupported(where.front().type->span, "'where' clauses");
    }

    // A struct may implement supported interfaces whose requirements the
    // checker has matched, each to a witness.
    void conformances(const TypeDeclItem& type) {
        if (type.supers.empty()) return;
        const CheckedHierarchy* hierarchy = types_.hierarchy(items_.at(&type));
        for (const auto& super : type.supers)
            if (!interface(*super.type).valid()) {
                unsupported(super.type->span, "structs that implement anything but supported interfaces");
                return;
            }
        const bool witnessed = hierarchy && hierarchy->conformance == ExpansionState::Resolved &&
                               std::ranges::all_of(hierarchy->witnesses, [](const ConformanceWitness& witness) {
                                   return witness.state == ExpansionState::Resolved && witness.implementation.valid();
                               });
        if (!witnessed) unsupported(type.supers.front().type->span, "conformances that are not checked yet");
    }

    // ------------------------------------------------------------ structs

    // A struct is supported when it is a plain value type whose fields all
    // come from its primary constructor and have supported types; a generic
    // struct's fields may hold its type parameters.
    // Structs that contain themselves, directly or not, have no finite layout.
    void structs(const File& file) {
        std::vector<const TypeDeclItem*> candidates;
        for (const Item* item : file.items) {
            const auto* type = item->kind == ItemKind::Struct ? as_type_decl(item) : nullptr;
            if (type && plain(*type)) candidates.push_back(type);
        }
        for (const auto* type : candidates) structs_.insert(items_.at(type).value);
        recursive(candidates);
        // A field of a rejected struct's type rejects its owner too.
        for (bool changed = true; changed;) {
            changed = false;
            for (const auto* type : candidates) {
                if (!structs_.contains(items_.at(type).value)) continue;
                generics_.clear();
                for (const auto& param : type->generics) generics_.insert(values_.at(param.name.span.lo).value);
                for (const auto& param : type->ctor_params) {
                    const auto field = types_.binding(values_.at(param.name.span.lo));
                    if (stored(param.type->span, field, "fields")) continue;
                    structs_.erase(items_.at(type).value);
                    composites_.clear();
                    changed = true;
                    break;
                }
            }
        }
        generics_.clear();
    }

    bool plain(const TypeDeclItem& type) {
        const bool before = ok_;
        ok_ = true;
        if (!type.attrs.empty()) unsupported(type.attrs.front().span, "attributes");
        conformances(type);
        bounds(type.generics, type.where);
        if (type.ctor_params.empty()) unsupported(type.name.span, "structs without fields");
        for (const auto& param : type.ctor_params) {
            if (param.conv != Convention::Default) unsupported(param.name.span, "conventions on struct fields");
            if (param.default_value) unsupported(param.default_value->span, "parameter defaults");
        }
        for (const Item* member : type.members) {
            // Instance methods; their signatures and bodies are checked like functions.
            if (const auto* method = as<FunItem>(member)) {
                if (has_modifier(method->mods, Modifier::Static)) unsupported(method->name.span, "static methods");
                continue;
            }
            unsupported(member->name.sym.empty() ? member->span : member->name.span, member_name(member->kind));
        }
        const bool result = ok_;
        ok_ = ok_ && before;
        return result;
    }

    static std::string_view member_name(ItemKind kind) {
        switch (kind) {
            case ItemKind::Var: return "fields declared in a struct body";
            case ItemKind::Init: return "initializers";
            case ItemKind::Deinit: return "deinitializers";
            default: return item_name(kind);
        }
    }

    // Rejects every struct on a cycle of fields, depth first and without recursion.
    void recursive(const std::vector<const TypeDeclItem*>& candidates) {
        std::unordered_map<usize, const TypeDeclItem*> by_id;
        for (const auto* type : candidates) by_id.emplace(items_.at(type).value, type);
        const auto children = [&](const TypeDeclItem& type) {
            std::vector<usize> out;
            for (const auto& param : type.ctor_params) {
                // A field holds the structs inside its tuples too, and,
                // conservatively, those its function types take or return
                // and those a generic struct's type arguments name.
                // Composite types can share parts, so each is walked once.
                std::vector<ScalarType> held{types_.binding(values_.at(param.name.span.lo))};
                std::unordered_set<usize> seen;
                while (!held.empty()) {
                    // An optional holds its value inline.
                    const auto field = held.back().scalar();
                    held.pop_back();
                    if ((field.kind == ScalarType::Tuple || field.kind == ScalarType::Function ||
                         field.kind == ScalarType::Nominal) &&
                        !seen.insert(field.detail).second)
                        continue;
                    const auto* descriptor = types_.descriptor(field);
                    if (field.kind == ScalarType::Tuple || field.kind == ScalarType::Function)
                        held.insert(held.end(), descriptor->parameters.begin(), descriptor->parameters.end());
                    if (field.kind == ScalarType::Function) held.push_back(descriptor->result);
                    else if (field.kind == ScalarType::Nominal) {
                        held.insert(held.end(), descriptor->arguments.begin(), descriptor->arguments.end());
                        if (by_id.contains(descriptor->declaration.value)) out.push_back(descriptor->declaration.value);
                    }
                }
            }
            return out;
        };
        enum class Mark : u8 { New, Active, Done };
        std::unordered_map<usize, Mark> marks;
        std::unordered_set<usize> cyclic;
        for (const auto* root : candidates) {
            const usize start = items_.at(root).value;
            if (marks[start] != Mark::New) continue;
            struct Frame { usize id; std::vector<usize> next; usize at = 0; };
            std::vector<Frame> stack{{start, children(*root)}};
            marks[start] = Mark::Active;
            while (!stack.empty()) {
                auto& frame = stack.back();
                if (frame.at == frame.next.size()) {
                    marks[frame.id] = Mark::Done;
                    stack.pop_back();
                    continue;
                }
                const usize next = frame.next[frame.at++];
                if (marks[next] == Mark::Active) {
                    // Everything from next up to the top of the stack is on the cycle.
                    for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
                        cyclic.insert(it->id);
                        if (it->id == next) break;
                    }
                } else if (marks[next] == Mark::New) {
                    marks[next] = Mark::Active;
                    stack.push_back({next, children(*by_id.at(next))});
                }
            }
        }
        for (const auto* type : candidates) {
            if (!cyclic.contains(items_.at(type).value)) continue;
            unsupported(type->name.span, "recursive structs");
            structs_.erase(items_.at(type).value);
            composites_.clear();
        }
    }

    bool supported_type(ScalarType type) const {
        // An optional holds a value, so its value type cannot be 'Unit' or 'Never'.
        if (type.optional_depth)
            return type.kind != ScalarType::Unit && type.kind != ScalarType::Never && supported_type(type.scalar());
        if (supported_scalar(type)) return true;
        if ((type.kind == ScalarType::Tuple || type.kind == ScalarType::Function) && !type.optional_depth) {
            // Composite types can share parts, so each answer is kept.
            if (const auto it = composites_.find(type.detail); it != composites_.end()) return it->second;
            const auto* descriptor = types_.descriptor(type);
            // Elements and parameters hold values, so they cannot be 'Unit' or
            // 'Never'; a function's result may be either.
            const bool supported =
                (type.kind == ScalarType::Tuple || (!descriptor->throws && supported_type(descriptor->result))) &&
                std::ranges::all_of(descriptor->parameters, [&](ScalarType part) {
                    return part.kind != ScalarType::Unit && part.kind != ScalarType::Never && supported_type(part);
                });
            composites_.emplace(type.detail, supported);
            return supported;
        }
        if (type.kind == ScalarType::TypeParameter && !type.optional_depth)
            return generics_.contains(types_.descriptor(type)->declaration.value);
        if (type.kind != ScalarType::Nominal || type.optional_depth) return false;
        const auto* descriptor = types_.descriptor(type);
        if (!descriptor || !structs_.contains(descriptor->declaration.value)) return false;
        if (descriptor->arguments.empty()) return true;
        // A generic struct's instance holds values of its arguments, which
        // can share parts like composite types.
        if (const auto it = composites_.find(type.detail); it != composites_.end()) return it->second;
        const bool supported = std::ranges::all_of(descriptor->arguments, [&](ScalarType argument) {
            return argument.kind != ScalarType::Unit && argument.kind != ScalarType::Never && supported_type(argument);
        });
        composites_.emplace(type.detail, supported);
        return supported;
    }

    void unsupported(Span span, std::string_view what) {
        ok_ = false;
        diag_.error(DiagCode::UnsupportedCodegen, span, "stage0 code generation does not support {} yet", what);
    }

    void unsupported_type(Span span, ScalarType type) {
        if (type.kind == ScalarType::Deferred) {
            ok_ = false;
            diag_.error(DiagCode::UnsupportedCodegen, span,
                        "stage0 code generation needs a known type here; this type is not inferred yet");
            return;
        }
        ok_ = false;
        diag_.error(DiagCode::UnsupportedCodegen, span, "stage0 code generation does not support values of type '{}' yet",
                    types_.type_name(type, resolution_, names_, 256));
    }

    bool type(Span span, ScalarType type) {
        if (supported_type(type)) return true;
        unsupported_type(span, type);
        return false;
    }

    // Parameters and locals hold a value, so 'Unit' and 'Never' are not stored.
    bool stored(Span span, ScalarType value, std::string_view what) {
        if (!type(span, value)) return false;
        if (value.kind != ScalarType::Unit && value.kind != ScalarType::Never) return true;
        ok_ = false;
        diag_.error(DiagCode::UnsupportedCodegen, span, "stage0 code generation does not support {} of type '{}' yet", what,
                    value.kind == ScalarType::Unit ? "Unit" : "Never");
        return false;
    }

    BindingId target(const NameExpr& name) const {
        const auto* ref = resolution_.reference(name.name.span);
        if (!ref || ref->state != ResolutionState::Resolved) return {};
        const auto ids = resolution_.candidates(ref->target);
        return ids.size() == 1 ? ids.front() : BindingId{};
    }

    void function(const FunItem& fun) {
        const bool before = ok_;
        ok_ = true;
        current_ = &fun;
        // A generic function's body is checked once, with values of its type
        // parameters, and those of a generic struct's methods, opaque; its
        // instances are checked by instances().
        generics_.clear();
        const auto owner = owners_.find(&fun);
        const bool requirement = owner != owners_.end() && owner->second->kind == ItemKind::Interface;
        if (requirement) generics_.insert(items_.at(owner->second).value);  // `Self`
        else if (owner != owners_.end())
            for (const auto& param : owner->second->generics) generics_.insert(values_.at(param.name.span.lo).value);
        for (const auto& param : fun.sig.generics) generics_.insert(values_.at(param.name.span.lo).value);
        if (!fun.attrs.empty()) unsupported(fun.attrs.front().span, "attributes");
        if (has_modifier(fun.mods, Modifier::Async)) unsupported(fun.name.span, "'async' functions");
        if (has_modifier(fun.mods, Modifier::Unsafe)) unsupported(fun.name.span, "'unsafe' functions");
        if (fun.sig.throws) unsupported(fun.name.span, "throwing functions");
        bounds(fun.sig.generics, fun.sig.where);
        // An interface method without a body is a requirement that its
        // conforming structs implement.
        if (!fun.body && !requirement) unsupported(fun.name.span, "functions without a body");
        if (ok_) {
            for (const auto& param : fun.sig.params) {
                if (param.conv == Convention::Inout) unsupported(param.name.span, "'inout' parameters");
                if (param.default_value) unsupported(param.default_value->span, "parameter defaults");
                const auto it = values_.find(param.name.span.lo);
                if (it != values_.end()) stored(param.name.span, types_.binding(it->second), "parameters");
            }
            const auto id = items_.find(&fun);
            const ScalarSignature* sig = id == items_.end() ? nullptr : types_.signature(id->second);
            const Span result_span = fun.sig.ret ? fun.sig.ret->span : fun.name.span;
            if (!sig) unsupported_type(result_span, ScalarType{});
            else type(result_span, sig->result);
        }
        // Report a body only once its signature is supported, to avoid repeating
        // the same unsupported type. A block body's tail is not the result.
        if (ok_ && fun.body) body(fun.body, fun.expr_body);
        ok_ = ok_ && before;
        generics_.clear();
    }

    void body(const Expr* root, bool used) {
        std::vector<Work> work{{root, used}};
        while (!work.empty()) {
            const auto [node, value] = work.back();
            work.pop_back();
            if (const auto* stmt = std::get_if<const Stmt*>(&node)) statement(**stmt, work);
            else expression(*std::get<const Expr*>(node), value, work);
        }
    }

    // A `let` condition stores the value inside the optional it tests.
    void condition(const Cond& cond, std::vector<Work>& work) {
        if (cond.is_let) {
            const auto it = values_.find(cond.name.span.lo);
            if (it == values_.end() || !stored(cond.name.span, types_.binding(it->second), "locals")) {
                if (it != values_.end()) rejected_.insert(it->second.value);
                return;
            }
        }
        work.push_back({cond.expr, true});
    }

    void statement(const Stmt& stmt, std::vector<Work>& work) {
        switch (stmt.kind) {
            case StmtKind::Var: {
                const auto& var = *cast<VarStmt>(&stmt);
                if (!var.names.empty()) {
                    // Destructuring: every name needs a stored type of its own.
                    bool supported = true;
                    for (const Name& name : var.names) {
                        const auto bound = values_.find(name.span.lo);
                        const auto local = bound == values_.end() ? ScalarType{} : types_.binding(bound->second);
                        if (local.kind == ScalarType::Deferred) {
                            ok_ = false;
                            diag_.error(DiagCode::UnsupportedCodegen, name.span,
                                        "stage0 code generation needs the type of '{}'; add a type annotation",
                                        names_.str(name.sym));
                            supported = false;
                        } else supported &= stored(name.span, local, "locals");
                    }
                    if (supported) {
                        work.push_back({var.init, true});
                        return;
                    }
                    for (const Name& name : var.names)
                        if (const auto bound = values_.find(name.span.lo); bound != values_.end()) rejected_.insert(bound->second.value);
                    return;
                }
                const auto it = values_.find(var.name.span.lo);
                const auto local = it == values_.end() ? ScalarType{ScalarType::Unit} : types_.binding(it->second);
                if (var.binding == Binding::Const) unsupported(var.name.span, "'const' locals");
                else if (local.kind == ScalarType::Deferred) {
                    ok_ = false;
                    diag_.error(DiagCode::UnsupportedCodegen, var.name.span,
                                "stage0 code generation needs the type of '{}'; add a type annotation",
                                names_.str(var.name.sym));
                } else if (stored(var.name.span, local, "locals")) {
                    if (var.init) work.push_back({var.init, true});
                    return;
                }
                // Uses of a rejected local are not reported again.
                if (it != values_.end()) rejected_.insert(it->second.value);
                return;
            }
            case StmtKind::Return:
                if (const Expr* value = cast<ReturnStmt>(&stmt)->value) work.push_back({value, true});
                return;
            case StmtKind::Break: case StmtKind::Continue: return;
            case StmtKind::Throw: unsupported(stmt.span, "'throw'"); return;
            case StmtKind::For: unsupported(stmt.span, "'for' loops"); return;
            case StmtKind::Defer: unsupported(stmt.span, "'defer'"); return;
            case StmtKind::While: {
                const auto& loop = *cast<WhileStmt>(&stmt);
                work.push_back({loop.body, false});
                condition(loop.cond, work);
                return;
            }
            case StmtKind::Loop: work.push_back({cast<LoopStmt>(&stmt)->body, false}); return;
            case StmtKind::Expr: work.push_back({cast<ExprStmt>(&stmt)->expr, false}); return;
            case StmtKind::Assign: {
                const auto& assign = *cast<AssignStmt>(&stmt);
                if (assign.op == AssignOp::Shl || assign.op == AssignOp::Shr) {
                    unsupported(stmt.span, "shifts");
                    return;
                }
                // A local, or a field of a local struct at any depth; the
                // target's member reads check the fields themselves.
                if (!place(assign.target)) {
                    unsupported(assign.target->span, "assignment to anything but a local variable or its fields");
                    return;
                }
                work.push_back({assign.value, true});
                work.push_back({assign.target, true});
                return;
            }
        }
    }

    void expression(const Expr& expr, bool used, std::vector<Work>& work) {
        if (!supported_kind(expr)) return;
        if (const auto* binary = as<BinaryExpr>(&expr); binary && binary->op != BinOp::Coalesce) {
            const auto lhs = types_.expression(binary->lhs).kind, rhs = types_.expression(binary->rhs).kind;
            if (lhs == ScalarType::Tuple || rhs == ScalarType::Tuple) {
                unsupported(expr.span, "comparisons of tuples");
                return;
            }
            if (lhs == ScalarType::Function || rhs == ScalarType::Function) {
                unsupported(expr.span, "operators on function values");
                return;
            }
        }
        if (const auto* name = as<NameExpr>(&expr)) {
            if (const auto id = target(*name); id.valid() && rejected_.contains(id.value)) return;
        }
        // A control expression's value, even in parentheses, is that of its tails, which are checked
        // in their own right; its own type is only recorded when discarded.
        const Expr* inner = &expr;
        while (const auto* paren = as<ParenExpr>(inner)) inner = paren->inner;
        const bool control = inner->kind == ExprKind::If || inner->kind == ExprKind::Block;
        const auto actual = types_.expression(&expr);
        // A type deferred only because of an already rejected local is not reported again.
        if (!control && actual.kind == ScalarType::Deferred && mentions_rejected(expr)) return;
        if (!control && !type(expr.span, actual)) return;
        switch (expr.kind) {
            case ExprKind::Int: case ExprKind::Bool: case ExprKind::String: case ExprKind::Unit: case ExprKind::SelfValue:
            case ExprKind::None:
                return;
            case ExprKind::Name: name(*cast<NameExpr>(&expr)); return;
            case ExprKind::GenericInst:
                // `identity<Int>` as a value; calls check their callee themselves.
                if (!function_value(expr)) unsupported(expr.span, expr_name(expr.kind));
                return;
            case ExprKind::Paren: work.push_back({cast<ParenExpr>(&expr)->inner, used}); return;
            case ExprKind::Unary: work.push_back({cast<UnaryExpr>(&expr)->operand, true}); return;
            case ExprKind::Unwrap: work.push_back({cast<UnwrapExpr>(&expr)->operand, true}); return;
            case ExprKind::Binary: {
                const auto& binary = *cast<BinaryExpr>(&expr);
                // Strings are only passed around whole for now; `??` only moves them.
                if (binary.op != BinOp::Coalesce && (types_.expression(binary.lhs).kind == ScalarType::String ||
                                                     types_.expression(binary.rhs).kind == ScalarType::String)) {
                    unsupported(expr.span, "operators on strings");
                    return;
                }
                work.push_back({binary.rhs, true});
                work.push_back({binary.lhs, true});
                return;
            }
            case ExprKind::Call: call(*cast<CallExpr>(&expr), work); return;
            case ExprKind::Member: {
                // Field reads of supported structs.
                const auto& member = *cast<MemberExpr>(&expr);
                const auto* record = types_.member(&member);
                const bool field = record && record->state == ResolutionState::Resolved && !record->type_receiver &&
                                   record->candidates.size() == 1 &&
                                   resolution_.binding(record->candidates.front()).kind == BindingKind::Field;
                if (!field) {
                    unsupported(expr.span, "member access other than struct fields");
                    return;
                }
                if (!type(member.base->span, types_.expression(member.base))) return;
                work.push_back({member.base, true});
                return;
            }
            case ExprKind::Tuple: {
                const auto& tuple = *cast<TupleExpr>(&expr);
                for (usize i = tuple.elems.size(); i-- > 0;) work.push_back({tuple.elems[i], true});
                return;
            }
            case ExprKind::TupleField: {
                const auto& element = *cast<TupleFieldExpr>(&expr);
                if (!type(element.base->span, types_.expression(element.base))) return;
                work.push_back({element.base, true});
                return;
            }
            case ExprKind::Block: {
                const auto& block = *cast<BlockExpr>(&expr);
                for (usize i = block.stmts.size(); i-- > 0;) {
                    const auto* tail = i + 1 == block.stmts.size() ? as<ExprStmt>(block.stmts[i]) : nullptr;
                    if (tail && used) work.push_back({tail->expr, true});
                    else work.push_back({block.stmts[i], false});
                }
                return;
            }
            case ExprKind::If: {
                const auto& branch = *cast<IfExpr>(&expr);
                if (branch.else_branch) work.push_back({branch.else_branch, used});
                work.push_back({branch.then_block, used});
                condition(branch.cond, work);
                return;
            }
            case ExprKind::Closure: {
                // Its supported function type covers its parameters. The body's
                // tail is its result unless that is 'Unit' or 'Never'.
                const auto result = types_.descriptor(actual)->result.kind;
                work.push_back({cast<ClosureExpr>(&expr)->body, result != ScalarType::Unit && result != ScalarType::Never});
                return;
            }
            default: unsupported(expr.span, expr_name(expr.kind)); return;
        }
    }

    bool supported_kind(const Expr& expr) {
        if (const auto* unary = as<UnaryExpr>(&expr); unary && !supported_unary(unary->op)) {
            unsupported(expr.span, unary_name(unary->op));
            return false;
        }
        if (const auto* binary = as<BinaryExpr>(&expr); binary && !supported_binary(binary->op)) {
            unsupported(expr.span, binary_name(binary->op));
            return false;
        }
        switch (expr.kind) {
            case ExprKind::Int: case ExprKind::Bool: case ExprKind::String: case ExprKind::Unit: case ExprKind::Name:
            case ExprKind::Paren: case ExprKind::Unary: case ExprKind::Binary: case ExprKind::Call: case ExprKind::Block:
            case ExprKind::If: case ExprKind::Member: case ExprKind::SelfValue: case ExprKind::Tuple:
            case ExprKind::TupleField: case ExprKind::Closure: case ExprKind::GenericInst: case ExprKind::None:
            case ExprKind::Unwrap:
                return true;
            default:
                unsupported(expr.span, expr_name(expr.kind));
                return false;
        }
    }

    bool mentions_rejected(const Expr& root) const {
        if (rejected_.empty()) return false;
        std::vector<const Expr*> work{&root};
        while (!work.empty()) {
            const Expr* expr = work.back();
            work.pop_back();
            if (const auto* name = as<NameExpr>(expr)) {
                if (const auto id = target(*name); id.valid() && rejected_.contains(id.value)) return true;
            } else if (const auto* paren = as<ParenExpr>(expr)) work.push_back(paren->inner);
            else if (const auto* unary = as<UnaryExpr>(expr)) work.push_back(unary->operand);
            else if (const auto* unwrap = as<UnwrapExpr>(expr)) work.push_back(unwrap->operand);
            else if (const auto* binary = as<BinaryExpr>(expr)) {
                work.push_back(binary->lhs);
                work.push_back(binary->rhs);
            } else if (const auto* call = as<CallExpr>(expr)) {
                for (const auto& arg : call->args) work.push_back(arg.value);
                work.push_back(call->callee);
            } else if (const auto* member = as<MemberExpr>(expr)) {
                work.push_back(member->base);
            } else if (const auto* element = as<TupleFieldExpr>(expr)) {
                work.push_back(element->base);
            } else if (const auto* tuple = as<TupleExpr>(expr)) {
                for (const Expr* e : tuple->elems) work.push_back(e);
            }
        }
        return false;
    }

    // A local, the receiver or one of its fields, and fields or tuple elements below them.
    bool place(const Expr* expr) const {
        expr = unparen(expr);
        while (true) {
            if (const auto* member = as<MemberExpr>(expr)) expr = unparen(member->base);
            else if (const auto* element = as<TupleFieldExpr>(expr)) expr = unparen(element->base);
            else break;
        }
        if (expr->kind == ExprKind::SelfValue) return true;
        const auto* name = as<NameExpr>(expr);
        const auto id = name ? target(*name) : BindingId{};
        return id.valid() &&
               (resolution_.binding(id).kind == BindingKind::Local || resolution_.binding(id).kind == BindingKind::Field);
    }

    void name(const NameExpr& name) {
        const auto id = target(name);
        const auto kind = id.valid() ? resolution_.binding(id).kind : BindingKind::Import;
        // A bare field name inside a method reads the receiver's field.
        if (kind == BindingKind::Local || kind == BindingKind::Parameter || kind == BindingKind::Field) return;
        if (function_value(name)) return;
        if (kind == BindingKind::Function) unsupported(name.span, "values of functions stage0 cannot compile");
        else unsupported(name.span, "this name");
    }

    // A top-level function named as a value, a generic one with type
    // arguments the checker inferred or that were written out. Reports
    // unstorable type arguments itself and returns true then.
    bool function_value(const Expr& expr) {
        const auto function = types_.function(&expr);
        const auto* fun = function.valid() ? as<FunItem>(resolution_.binding(function).item) : nullptr;
        if (!fun || !functions_.contains(fun) || owners_.contains(fun)) return false;
        const auto arguments = types_.function_arguments(&expr);
        if (arguments.size() != fun->sig.generics.size()) return false;
        for (const ScalarType argument : arguments)
            if (!stored(expr.span, argument, "type arguments")) return true;
        if (!fun->sig.generics.empty()) generic_calls_[current_].push_back(&expr);
        return true;
    }

    void call(const CallExpr& call, std::vector<Work>& work) {
        if (const CheckedConversion* conversion = types_.conversion(&call)) {
            // Conversions between integer types: widening, checked or truncating.
            if (conversion->state != CallState::Resolved || !type(call.span, conversion->source) || call.args.size() != 1) {
                if (conversion->state != CallState::Resolved) unsupported_type(call.span, ScalarType{});
                return;
            }
            work.push_back({call.args.front().value, true});
            return;
        }
        const CheckedCall* checked = types_.call(&call);
        if (checked && checked->kind == CallKind::Builtin) {
            // print: its argument is a String, Int, UInt or Bool (3.27).
            if (checked->state != CallState::Resolved) unsupported_type(call.span, ScalarType{});
            else work.push_back({call.args.front().value, true});
            return;
        }
        if (checked && checked->kind == CallKind::Clone) {
            // x.clone() copies a value of a supported type (3.31).
            if (checked->state != CallState::Resolved || !supported_type(types_.expression(&call)))
                unsupported_type(call.span, types_.expression(&call));
            else work.push_back({checked->receiver, true});
            return;
        }
        if (checked && checked->kind == CallKind::NominalConstructor) {
            // Primary constructors of supported structs.
            if (checked->state != CallState::Resolved || !supported_type(types_.expression(&call))) {
                unsupported(call.callee->span, "constructors other than those of supported structs");
                return;
            }
            for (usize i = call.args.size(); i-- > 0;) work.push_back({call.args[i].value, true});
            return;
        }
        if (checked && checked->kind == CallKind::Method) {
            // Instance methods of supported structs, on an explicit or implicit receiver.
            const bool resolved = checked->state == CallState::Resolved && checked->target.valid();
            const auto* fun = resolved ? as<FunItem>(resolution_.binding(checked->target).item) : nullptr;
            if (!fun || !functions_.contains(fun) || checked->type_arguments.size() != fun->sig.generics.size() ||
                (checked->receiver && !supported_type(types_.expression(checked->receiver)))) {
                unsupported(call.callee->span, "calls to methods other than those of supported structs");
                return;
            }
            for (const ScalarType argument : checked->type_arguments)
                if (!stored(call.callee->span, argument, "type arguments")) return;
            const auto* owner = owners_.at(fun);
            if (!fun->sig.generics.empty() || !owner->generics.empty() || owner->kind == ItemKind::Interface)
                generic_calls_[current_].push_back(&call);
            // A `mut fun` changes its receiver in place.
            if (has_modifier(fun->mods, Modifier::Mut) && checked->receiver && !place(checked->receiver)) {
                unsupported(checked->receiver->span, "'mut fun' calls on anything but a local variable or its fields");
                return;
            }
            for (usize i = call.args.size(); i-- > 0;) work.push_back({call.args[i].value, true});
            if (checked->receiver) work.push_back({checked->receiver, true});
            return;
        }
        if (checked && checked->kind == CallKind::IndirectFunction) {
            // Calls of function values: the callee runs first, then the arguments.
            if (checked->state != CallState::Resolved) {
                unsupported_type(call.span, ScalarType{});
                return;
            }
            for (usize i = call.args.size(); i-- > 0;) work.push_back({call.args[i].value, true});
            work.push_back({call.callee, true});
            return;
        }
        // A top-level function by name, a generic one with explicit or
        // inferred type arguments.
        const Expr* named = unparen(call.callee);
        if (const auto* inst = as<GenericInstExpr>(named)) named = unparen(inst->base);
        const bool direct = checked && checked->state == CallState::Resolved && checked->kind == CallKind::Function &&
                            named->kind == ExprKind::Name && checked->target.valid();
        const auto* fun = direct ? as<FunItem>(resolution_.binding(checked->target).item) : nullptr;
        if (!fun || !functions_.contains(fun) || checked->type_arguments.size() != fun->sig.generics.size()) {
            unsupported(call.callee->span, "calls to anything but a top-level function");
            return;
        }
        // Instances store values of their type parameters, so these hold a value.
        for (const ScalarType argument : checked->type_arguments)
            if (!stored(call.callee->span, argument, "type arguments")) return;
        if (!fun->sig.generics.empty()) generic_calls_[current_].push_back(&call);
        for (usize i = call.args.size(); i-- > 0;) work.push_back({call.args[i].value, true});
    }

    // A struct's type as its own methods see it, such as `Box<T>`; an
    // interface's methods see `Self`.
    ScalarType declared(const TypeDeclItem& type) {
        if (type.kind == ItemKind::Interface) return types_.parameter(items_.at(&type));
        std::vector<BindingId> parameters;
        for (const auto& param : type.generics) parameters.push_back(values_.at(param.name.span.lo));
        return types_.declared(items_.at(&type), parameters);
    }

    // The instances a program needs, from the generic calls of non-generic
    // functions and methods through those of each instance, with each instance's type
    // arguments substituted into its calls. Polymorphic recursion would
    // never end, so nesting and the number of instances are bounded (E0602).
    void instances(const File& file, const std::vector<const FunItem*>& methods) {
        static constexpr usize kMaxDepth = 64, kMaxInstances = 10000;
        struct Instance {
            const FunItem* fun;
            std::vector<ScalarTypes::TypeArgument> arguments;
            usize depth;
        };
        std::vector<Instance> work;
        std::unordered_set<std::string> seen;
        const auto key = [](const FunItem* fun, const std::vector<ScalarType>& arguments) {
            std::string out = std::to_string(reinterpret_cast<std::uintptr_t>(fun));
            for (const ScalarType argument : arguments)
                out += std::format(":{}.{}.{}", static_cast<int>(argument.kind), argument.optional_depth, argument.detail);
            return out;
        };
        const auto calls = [&](const Instance& from) {
            const auto it = generic_calls_.find(from.fun);
            if (it == generic_calls_.end()) return true;
            for (const Expr* use : it->second) {
                const auto* call = as<CallExpr>(use);
                const CheckedCall* checked = call ? types_.call(call) : nullptr;
                const auto* target = as<FunItem>(resolution_.binding(checked ? checked->target : types_.function(use)).item);
                const Span span = call ? call->callee->span : use->span;
                // A method of an interface or a generic struct takes its
                // receiver's type; an implicit receiver is the caller's.
                const auto owned = [&](const FunItem* fun) {
                    const auto owner = owners_.find(fun);
                    return owner == owners_.end() ? nullptr : owner->second;
                };
                ScalarType receiver;
                if (const auto* owner = owned(target); owner && (owner->kind == ItemKind::Interface || !owner->generics.empty()))
                    receiver = types_.substitute(
                        checked->receiver ? types_.expression(checked->receiver) : declared(*owners_.at(from.fun)),
                        from.arguments);
                // An interface method runs its witness for the receiver's
                // struct: the struct's own method, or the interface's default.
                const FunItem* callee = target;
                if (const auto* owner = owned(target); owner && owner->kind == ItemKind::Interface) {
                    callee = witness(*target, receiver);
                    if (!callee) continue;
                }
                std::vector<BindingId> parameters;
                std::vector<ScalarType> arguments;
                if (const auto* owner = owned(callee); owner && owner->kind == ItemKind::Interface) {
                    parameters.push_back(items_.at(owner));
                    arguments.push_back(receiver);
                } else if (owner && !owner->generics.empty()) {
                    for (const auto& param : owner->generics) parameters.push_back(values_.at(param.name.span.lo));
                    const auto owner_arguments = types_.descriptor(receiver)->arguments;
                    arguments.insert(arguments.end(), owner_arguments.begin(), owner_arguments.end());
                }
                if (callee == target) {
                    for (const auto& param : callee->sig.generics) parameters.push_back(values_.at(param.name.span.lo));
                    for (const ScalarType argument : checked ? std::span<const ScalarType>(checked->type_arguments)
                                                             : types_.function_arguments(use))
                        arguments.push_back(types_.substitute(argument, from.arguments));
                }
                // A witness of a plain struct is a root of its own.
                if (parameters.empty()) continue;
                if (!seen.insert(key(callee, arguments)).second) continue;
                if (from.depth + 1 > kMaxDepth || seen.size() > kMaxInstances) {
                    ok_ = false;
                    auto& d = diag_.error(DiagCode::InstanceLimit, span,
                                          "this {} needs more generic instances than stage0 code generation allows",
                                          call ? "call" : "function value");
                    d.notes.push_back({span,
                                       from.depth + 1 > kMaxDepth
                                           ? std::format("instances nest more than {} levels deep here", kMaxDepth)
                                           : std::format("a program can have at most {} instances", kMaxInstances)});
                    return false;
                }
                Instance next{callee, {}, from.depth + 1};
                for (usize i = 0; i < arguments.size(); ++i) next.arguments.emplace_back(parameters[i], arguments[i]);
                work.push_back(std::move(next));
            }
            return true;
        };
        std::vector<const FunItem*> roots;
        for (const FunItem* method : methods)
            if (method->sig.generics.empty() && owners_.at(method)->generics.empty() &&
                owners_.at(method)->kind != ItemKind::Interface)
                roots.push_back(method);
        for (const Item* item : file.items)
            if (const auto* fun = as<FunItem>(item); fun && fun->sig.generics.empty()) roots.push_back(fun);
        for (const FunItem* fun : roots)
            if (!calls({fun, {}, 0})) return;
        while (!work.empty()) {
            const Instance next = std::move(work.back());
            work.pop_back();
            if (!calls(next)) return;
        }
    }

    // The method that runs for interface method `requirement` on a struct
    // `receiver`: the struct's implementation, or the interface's default.
    const FunItem* witness(const FunItem& requirement, ScalarType receiver) const {
        if (receiver.kind != ScalarType::Nominal) return nullptr;
        const CheckedHierarchy* hierarchy = types_.hierarchy(types_.descriptor(receiver)->declaration);
        if (!hierarchy) return nullptr;
        const BindingId id = items_.at(&requirement);
        for (const auto& witness : hierarchy->witnesses)
            if (witness.requirement == id && witness.implementation.valid())
                return as<FunItem>(resolution_.binding(witness.implementation).item);
        return nullptr;
    }

    void entry(const File& file, const std::vector<const FunItem*>& mains) {
        if (mains.empty()) {
            ok_ = false;
            diag_.error(DiagCode::InvalidEntryPoint, Span{file.file, 0, 0},
                        "a program needs an entry point 'fun main()'");
            return;
        }
        for (usize i = 1; i < mains.size(); ++i) {
            ok_ = false;
            auto& d = diag_.error(DiagCode::InvalidEntryPoint, mains[i]->name.span, "'main' cannot be overloaded");
            d.notes.push_back({mains.front()->name.span, "first 'main' declared here"});
        }
        const FunItem& main = *mains.front();
        if (!main.sig.params.empty()) {
            ok_ = false;
            diag_.error(DiagCode::InvalidEntryPoint, main.sig.params.front().name.span, "'main' takes no parameters");
        }
        if (!main.sig.generics.empty()) {
            ok_ = false;
            diag_.error(DiagCode::InvalidEntryPoint, main.sig.generics.front().name.span, "'main' cannot be generic");
        }
        const auto id = items_.find(&main);
        const ScalarSignature* sig = id == items_.end() ? nullptr : types_.signature(id->second);
        const auto result = sig ? sig->result : ScalarType{};
        const bool exits = result == ScalarType{ScalarType::Int} || result == ScalarType{ScalarType::Unit} ||
                           result == ScalarType{ScalarType::Never};
        if (!exits && supported_type(result)) {
            ok_ = false;
            diag_.error(DiagCode::InvalidEntryPoint, main.sig.ret ? main.sig.ret->span : main.name.span,
                        "'main' must return 'Int' or nothing, not '{}'", types_.type_name(result, resolution_, names_, 256));
        }
    }

    const Resolution& resolution_;
    ScalarTypes& types_;
    const Interner& names_;
    DiagEngine& diag_;
    std::unordered_map<const Item*, BindingId> items_;
    std::unordered_map<u32, BindingId> values_;  // locals, parameters and type parameters by declaration offset
    std::unordered_set<const FunItem*> functions_;
    std::unordered_map<const FunItem*, const TypeDeclItem*> owners_;  // methods of supported structs
    const FunItem* current_ = nullptr;
    std::unordered_set<usize> generics_;  // the current function's type parameters
    // Generic calls and generic function values in each function.
    std::unordered_map<const FunItem*, std::vector<const Expr*>> generic_calls_;
    // Whether each tuple, function and generic struct instance type is
    // supported, by descriptor; reset when a struct is rejected.
    mutable std::unordered_map<usize, bool> composites_;
    std::unordered_set<usize> rejected_;  // locals whose declaration was reported
    std::unordered_set<usize> structs_;   // binding IDs of supported structs
    std::unordered_set<usize> interfaces_;  // binding IDs of supported interfaces
    bool ok_ = true;
};

}  // namespace

bool check_codegen_subset(const File& file, const Resolution& resolution, ScalarTypes& types,
                          const Interner& names, DiagEngine& diag) {
    return Gate(resolution, types, names, diag).run(file);
}

}  // namespace phs
