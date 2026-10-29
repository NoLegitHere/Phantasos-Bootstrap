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

#include <algorithm>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include "codegen/mir.hpp"

namespace phs::mir {
namespace {
using namespace ast;

Type scalar_type(ScalarType type) {
    PHS_ASSERT(type.optional_depth == 0);
    switch (type.kind) {
        case ScalarType::Int8: return Type::I8;
        case ScalarType::Int16: return Type::I16;
        case ScalarType::Int32: return Type::I32;
        case ScalarType::Int64: case ScalarType::Int: return Type::I64;
        case ScalarType::UInt8: return Type::U8;
        case ScalarType::UInt16: return Type::U16;
        case ScalarType::UInt32: return Type::U32;
        case ScalarType::UInt64: case ScalarType::UInt: return Type::U64;
        case ScalarType::Bool: return Type::Bool;
        case ScalarType::String: return Type::Str;
        case ScalarType::Unit: return Type::Unit;
        case ScalarType::Never: return Type::Never;
        default: PHS_ASSERT(false && "type outside the code generation subset"); unreachable();
    }
}

bool has_value(Type type) { return type != Type::Unit && type != Type::Never; }

const Expr* unparen(const Expr* expr) {
    while (const auto* paren = as<ParenExpr>(expr)) expr = paren->inner;
    return expr;
}

bool control(const Expr* expr) {
    expr = unparen(expr);
    return expr->kind == ExprKind::If || expr->kind == ExprKind::Block;
}

Op binary_op(BinOp op) {
    switch (op) {
        case BinOp::Add: return Op::Add;
        case BinOp::Sub: return Op::Sub;
        case BinOp::Mul: return Op::Mul;
        case BinOp::Div: return Op::Div;
        case BinOp::Rem: return Op::Rem;
        case BinOp::WrapAdd: return Op::WrapAdd;
        case BinOp::WrapSub: return Op::WrapSub;
        case BinOp::WrapMul: return Op::WrapMul;
        case BinOp::BitAnd: return Op::And;
        case BinOp::BitOr: return Op::Or;
        case BinOp::BitXor: return Op::Xor;
        case BinOp::Eq: return Op::Eq;
        case BinOp::Ne: return Op::Ne;
        case BinOp::Lt: return Op::Lt;
        case BinOp::Le: return Op::Le;
        case BinOp::Gt: return Op::Gt;
        case BinOp::Ge: return Op::Ge;
        default: PHS_ASSERT(false && "operator outside the code generation subset"); unreachable();
    }
}

Op assign_op(AssignOp op) {
    switch (op) {
        case AssignOp::Add: return Op::Add;
        case AssignOp::Sub: return Op::Sub;
        case AssignOp::Mul: return Op::Mul;
        case AssignOp::Div: return Op::Div;
        case AssignOp::Rem: return Op::Rem;
        case AssignOp::BitAnd: return Op::And;
        case AssignOp::BitOr: return Op::Or;
        case AssignOp::BitXor: return Op::Xor;
        default: PHS_ASSERT(false && "assignment outside the code generation subset"); unreachable();
    }
}

class Lowerer {
public:
    Lowerer(const Resolution& resolution, ScalarTypes& types, const Interner& names, const SourceManager& sources)
        : resolution_(resolution), types_(types), names_(names), sources_(sources) {
        for (usize id = 0; id < resolution.bindings().size(); ++id) {
            const auto& binding = resolution.bindings()[id];
            if (binding.item) items_.emplace(binding.item, BindingId{id});
            if (binding.kind == BindingKind::Local || binding.kind == BindingKind::Parameter ||
                binding.kind == BindingKind::GenericParameter)
                values_.emplace(binding.name.span.lo, BindingId{id});
        }
    }

    Program run(const File& file) {
        std::unordered_map<std::string, u32> seen;
        // Every struct in the file passed the gate; a struct's layout follows
        // those of the structs it holds. A generic struct has a layout per
        // instance, built when a type first needs it.
        for (const Item* item : file.items)
            if (item->kind == ItemKind::Struct && as_type_decl(item)->generics.empty())
                (void)structure(types_.declared(items_.at(item), {}));
        const auto add = [&](const FunItem& fun, std::string name) {
            // Overloads get a numeric suffix so that every name is unique.
            const u32 index = seen[name]++;
            if (index) name = std::format("{}.{}", name, index);
            // A generic function, or a method of a generic struct, is only a
            // template; each instance a call needs is added when the call is
            // lowered.
            if (generic(fun)) {
                templates_.emplace(&fun, std::move(name));
                return;
            }
            functions_.emplace(&fun, static_cast<u32>(program_.functions.size()));
            funs_.push_back({&fun, static_cast<u32>(program_.functions.size()), {}});
            auto& out = program_.functions.emplace_back();
            out.name = std::move(name);
            out.span = fun.name.span;
        };
        for (const Item* item : file.items) {
            if (item->kind == ItemKind::Struct || item->kind == ItemKind::Interface) {
                // Methods are named after their struct and follow it. An
                // interface's methods are templates whose instances are
                // the defaults a struct uses.
                const auto& type = *as_type_decl(item);
                for (const Item* member : type.members)
                    if (const auto* method = as<FunItem>(member)) {
                        owners_.emplace(method, &type);
                        add(*method, std::format("{}.{}", names_.str(type.name.sym), names_.str(method->name.sym)));
                    }
                continue;
            }
            const auto* fun = as<FunItem>(item);
            if (!fun) continue;
            if (names_.str(fun->name.sym) == "main") program_.main = static_cast<u32>(program_.functions.size());
            add(*fun, std::string(names_.str(fun->name.sym)));
        }
        // Closures become functions of their own, after the functions that
        // create them; a closure's body can add more, and any body can add
        // generic instances.
        usize next_fun = 0, next_closure = 0;
        while (next_fun < funs_.size() || next_closure < closures_.size()) {
            if (next_fun < funs_.size()) {
                const auto pending = funs_[next_fun++];
                env_ = pending.env;
                function(*pending.fun, pending.function);
            } else {
                const auto pending = closures_[next_closure++];
                env_ = pending.env;
                closure_function(pending);
            }
        }
        for (auto& fun : program_.functions) lift(fun);
        return std::move(program_);
    }

private:
    struct Loop {
        u32 head, exit;
    };
    // A function or generic instance whose body is yet to be lowered, with
    // the type arguments of its instance.
    struct PendingFunction {
        const FunItem* fun;
        u32 function;
        std::vector<ScalarTypes::TypeArgument> env;
    };
    // A closure whose function is yet to be lowered.
    struct PendingClosure {
        const ClosureExpr* closure;
        u32 function;
        std::vector<BindingId> captures;
        std::optional<Type> self;  // the receiver's type, when the closure uses it
        ScalarType self_type;      // the creating method's receiver type, if any
        std::vector<ScalarTypes::TypeArgument> env;  // that of the function creating it
    };
    // A part one step inside a place: a field or tuple element, or an array
    // element at an index evaluated along with the place.
    struct Step {
        u32 field = 0;
        bool element = false;
        Reg index;  // an element's U64 index
        Type type = Type::Unit;
        Span span;  // an element's indexing expression, where a bounds check panics
    };
    // A local or the receiver, and the parts below it from the root out.
    struct Place {
        Reg root;
        std::vector<Step> steps;
        bool diverged = false;  // an index ended the block
    };

    // ------------------------------------------------------------ building

    u32 block() {
        fun_->blocks.emplace_back();
        return static_cast<u32>(fun_->blocks.size() - 1);
    }
    Reg reg(Type type) {
        PHS_ASSERT(has_value(type));
        fun_->regs.push_back(type);
        return {static_cast<u32>(fun_->regs.size() - 1)};
    }
    void emit(Inst inst) { fun_->blocks[current_].insts.push_back(std::move(inst)); }
    void copy(Reg dst, Reg src) {
        if (dst.valid() && src.valid() && dst != src) emit({.op = Op::Copy, .dst = dst, .a = src});
    }
    Reg constant(Type type, u64 value) {
        const Reg dst = reg(type);
        emit({.op = Op::Const, .dst = dst, .imm = value});
        return dst;
    }
    // Ending a block starts an unreachable one; code after a jump lands there
    // and is pruned at the end.
    void end(Term term, Reg value = {}, u32 target = 0, u32 other = 0) {
        auto& b = fun_->blocks[current_];
        b.term = term;
        b.value = value;
        b.target = target;
        b.other = other;
        current_ = block();
    }

    // ------------------------------------------------------------ facts

    Type lower_type(ScalarType type) {
        if (type.optional_depth) return optional(lower_type(type.element()));
        if (type.kind == ScalarType::Tuple) return tuple(type);
        if (type.kind == ScalarType::Function) return function_type(type);
        if (type.kind == ScalarType::Array) return array(type);
        if (type.kind != ScalarType::Nominal) return scalar_type(type);
        return structure(type);
    }

    // A tuple type's layout, built once per type; its fields are the elements.
    Type tuple(ScalarType type) {
        PHS_ASSERT(type.optional_depth == 0);
        if (const auto it = tuples_.find(type.detail); it != tuples_.end()) return aggregate(it->second);
        Layout layout;
        layout.name = types_.type_name(type, resolution_, names_);
        layout.kind = Layout::Tuple;
        const auto elements = types_.descriptor(type)->parameters;  // a copy, like function_type's
        for (usize i = 0; i < elements.size(); ++i) {
            layout.fields.push_back(lower_type(elements[i]));
            layout.names.push_back(std::to_string(i));
        }
        place_fields(program_, layout);
        const auto index = static_cast<u32>(program_.layouts.size());
        program_.layouts.push_back(std::move(layout));
        tuples_.emplace(type.detail, index);
        return aggregate(index);
    }

    // An optional's layout, built once per value type: its tag and its value.
    Type optional(Type value) {
        if (const auto it = optionals_.find(static_cast<u32>(value)); it != optionals_.end()) return aggregate(it->second);
        Layout layout;
        layout.name = type_name(program_, value) + "?";
        if (layout.name.front() == '@') layout.name.erase(0, 1);
        layout.kind = Layout::Optional;
        layout.fields = {Type::Bool, value};
        layout.names = {"some", "value"};
        place_fields(program_, layout);
        const auto index = static_cast<u32>(program_.layouts.size());
        program_.layouts.push_back(std::move(layout));
        optionals_.emplace(static_cast<u32>(value), index);
        return aggregate(index);
    }
    bool is_optional(Type type) const {
        return is_aggregate(type) && program_.layouts[layout_index(type)].kind == Layout::Optional;
    }

    // An array type's layout, built once per type: its data, count and
    // capacity words, and the type of the elements that data points at.
    Type array(ScalarType type) {
        PHS_ASSERT(type.optional_depth == 0);
        if (const auto it = arrays_.find(type.detail); it != arrays_.end()) return aggregate(it->second);
        const ScalarType element = types_.descriptor(type)->element;  // copied, like function_type's descriptor
        Layout layout;
        layout.name = types_.type_name(type, resolution_, names_);
        layout.kind = Layout::Array;
        layout.fields = {Type::U64, Type::I64, Type::I64};
        layout.names = {"data", "count", "capacity"};
        layout.element = lower_type(element);
        place_fields(program_, layout);
        const auto index = static_cast<u32>(program_.layouts.size());
        program_.layouts.push_back(std::move(layout));
        arrays_.emplace(type.detail, index);
        return aggregate(index);
    }

    // A function type's layout, built once per type: the code and environment
    // words, and the signature of calls through it.
    Type function_type(ScalarType type) {
        PHS_ASSERT(type.optional_depth == 0);
        if (const auto it = function_types_.find(type.detail); it != function_types_.end()) return aggregate(it->second);
        // Copied, since building the layouts of its parts can intern types.
        const auto descriptor = *types_.descriptor(type);
        Layout layout;
        layout.name = types_.type_name(type, resolution_, names_);
        layout.kind = Layout::Function;
        layout.fields = {Type::U64, Type::U64};
        layout.names = {"code", "env"};
        for (const ScalarType param : descriptor.parameters) layout.params.push_back(lower_type(param));
        layout.result = lower_type(descriptor.result);
        place_fields(program_, layout);
        const auto index = static_cast<u32>(program_.layouts.size());
        program_.layouts.push_back(std::move(layout));
        function_types_.emplace(type.detail, index);
        return aggregate(index);
    }

    // A struct's layout, built once per instance; the gate rejects recursive
    // structs. A generic struct's fields take its instance's type arguments.
    Type structure(ScalarType type) {
        PHS_ASSERT(type.optional_depth == 0);
        if (const auto it = layouts_.find(type.detail); it != layouts_.end()) return aggregate(it->second);
        const auto descriptor = *types_.descriptor(type);  // a copy, like function_type's
        const auto& item = *as_type_decl(resolution_.binding(descriptor.declaration).item);
        std::vector<ScalarTypes::TypeArgument> arguments;
        for (usize i = 0; i < item.generics.size(); ++i)
            arguments.emplace_back(values_.at(item.generics[i].name.span.lo), descriptor.arguments[i]);
        Layout layout;
        layout.name = arguments.empty() ? std::string(names_.str(item.name.sym)) : types_.type_name(type, resolution_, names_);
        for (usize i = 0; i < item.ctor_params.size(); ++i) {
            const auto& param = item.ctor_params[i];
            layout.fields.push_back(lower_type(types_.substitute(types_.binding(values_.at(param.name.span.lo)), arguments)));
            layout.names.emplace_back(names_.str(param.name.sym));
            fields_.emplace(param.name.span.lo, static_cast<u32>(i));
        }
        place_fields(program_, layout);
        const auto index = static_cast<u32>(program_.layouts.size());
        program_.layouts.push_back(std::move(layout));
        layouts_.emplace(type.detail, index);
        return aggregate(index);
    }
    // The declaration index of the field a member expression reads.
    u32 field(const MemberExpr& member) { return field(&member, member.base); }
    u32 field(const Expr* member, const Expr* base) {
        (void)lower_type(scalar(base));
        const auto* record = types_.member(member);
        PHS_ASSERT(record && record->candidates.size() == 1);
        return fields_.at(resolution_.binding(record->candidates.front()).name.span.lo);
    }

    // A type with the current instance's type arguments in place of its type
    // parameters.
    ScalarType concrete(ScalarType type) { return env_.empty() ? type : types_.substitute(type, env_); }
    ScalarType scalar(const Expr* expr) { return concrete(types_.expression(expr)); }
    ScalarType binding_type(BindingId id) { return concrete(types_.binding(id)); }
    BindingId target(const NameExpr& name) const {
        const auto* ref = resolution_.reference(name.name.span);
        PHS_ASSERT(ref && ref->state == ResolutionState::Resolved);
        const auto ids = resolution_.candidates(ref->target);
        PHS_ASSERT(ids.size() == 1);
        return ids.front();
    }
    Reg local(const NameExpr& name) const { return locals_.at(target(name).value); }
    // The declaration index of the receiver's field that a bare name inside a method reads.
    std::optional<u32> implicit_field(const NameExpr& name) const {
        const auto& binding = resolution_.binding(target(name));
        if (binding.kind != BindingKind::Field) return {};
        return fields_.at(binding.name.span.lo);
    }
    static bool mutating(const FunItem& fun) { return has_modifier(fun.mods, Modifier::Mut); }
    // Whether a function has instances: it is generic, or a method of a
    // generic struct or an interface.
    bool generic(const FunItem& fun) const {
        const auto owner = owners_.find(&fun);
        return !fun.sig.generics.empty() ||
               (owner != owners_.end() && (!owner->second->generics.empty() || interface(*owner->second)));
    }
    static bool interface(const TypeDeclItem& type) { return type.kind == ItemKind::Interface; }
    // A struct's type as its own methods see it, such as `Box<T>`; an
    // interface's methods see `Self`.
    ScalarType declared(const TypeDeclItem& type) {
        if (interface(type)) return types_.parameter(items_.at(&type));
        std::vector<BindingId> parameters;
        for (const auto& param : type.generics) parameters.push_back(values_.at(param.name.span.lo));
        return types_.declared(items_.at(&type), parameters);
    }
    bool indirect(const CallExpr& call) const {
        const CheckedCall* checked = types_.call(&call);
        return checked && checked->kind == CallKind::IndirectFunction;
    }
    // A `mut fun` call, or `append` or `removeLast`: its receiver is a place.
    bool mutating_call(const CallExpr& call) const {
        const CheckedCall* checked = types_.call(&call);
        return checked && (checked->kind == CallKind::Array ||
                           (checked->kind == CallKind::Method &&
                            mutating(*cast<FunItem>(resolution_.binding(checked->target).item))));
    }
    // A used branch's type is the one its context expects, when the checker
    // knows it; otherwise that of its first tail with a value, or of the tail
    // that is the deepest optional, since the others lift to it.
    Type branch_type(const Expr* root) {
        if (const auto own = scalar(root); own.kind != ScalarType::Deferred && own.kind != ScalarType::Error)
            return lower_type(own);
        std::vector<const Expr*> work{root};
        std::optional<ScalarType> found;
        while (!work.empty()) {
            const Expr* expr = unparen(work.back());
            work.pop_back();
            if (const auto* block = as<BlockExpr>(expr)) {
                if (const auto* tail = block->stmts.empty() ? nullptr : as<ExprStmt>(block->stmts.back()))
                    work.push_back(tail->expr);
            } else if (const auto* branch = as<IfExpr>(expr)) {
                if (branch->else_branch) work.push_back(branch->else_branch);
                work.push_back(branch->then_block);
            } else if (const auto type = scalar(expr); has_value(lower_type(type))) {
                if (!found || type.optional_depth > found->optional_depth) found = type;
            }
        }
        return found ? lower_type(*found) : Type::Unit;
    }

    // ------------------------------------------------------------ items

    void function(const FunItem& item, u32 index) {
        fun_ = &program_.functions[index];
        locals_.clear();
        self_ = {};
        const ScalarSignature* sig = types_.signature(items_.at(&item));
        // A method's receiver is its hidden first parameter (abi.md A.6),
        // inout for a `mut fun`.
        self_type_ = {};
        if (const auto owner = owners_.find(&item); owner != owners_.end()) {
            self_type_ = concrete(declared(*owner->second));
            self_ = reg(lower_type(self_type_));
            if (mutating(item)) fun_->inout = {true};
        }
        for (const auto& param : item.sig.params)
            locals_[values_.at(param.name.span.lo).value] = reg(lower_type(binding_type(values_.at(param.name.span.lo))));
        fun_->params = static_cast<u32>(fun_->regs.size());
        fun_->result = lower_type(concrete(sig->result));
        current_ = block();
        if (item.expr_body) {
            const Reg result = any(item.body);
            end(Term::Ret, has_value(fun_->result) ? result : Reg{});
        } else {
            (void)branch(item.body, false);
            // Flow checks guarantee that a value-returning body cannot fall off its end.
            end(fun_->result == Type::Unit ? Term::Ret : Term::Unreachable);
        }
        prune();
    }

    // A closure's function takes its environment first, then its parameters.
    // Captured values are read from the environment on entry, so the body
    // sees them as locals, and the receiver as `self`.
    void closure_function(const PendingClosure& pending) {
        fun_ = &program_.functions[pending.function];
        locals_.clear();
        self_ = {};
        self_type_ = pending.self_type;
        const auto& closure = *pending.closure;
        const Reg env = reg(Type::U64);
        for (const auto& param : closure.params)
            locals_[values_.at(param.name.span.lo).value] = reg(lower_type(binding_type(values_.at(param.name.span.lo))));
        fun_->params = static_cast<u32>(fun_->regs.size());
        fun_->closure = true;
        fun_->result = lower_type(types_.descriptor(scalar(&closure))->result);
        current_ = block();
        if (fun_->environment) {
            const auto& layout = program_.layouts[*fun_->environment];
            for (u32 i = 0; i < layout.fields.size(); ++i) {
                const Reg value = reg(layout.fields[i]);
                emit({.op = Op::Env, .dst = value, .a = env, .imm = i});
                if (i < pending.captures.size()) locals_[pending.captures[i].value] = value;
                else self_ = value;
            }
        }
        if (has_value(fun_->result)) {
            const Reg result = any(closure.body);
            end(Term::Ret, result);
        } else {
            (void)branch(closure.body, false);
            end(fun_->result == Type::Unit ? Term::Ret : Term::Unreachable);
        }
        prune();
    }

    // Threads jumps through empty blocks, then keeps the blocks reachable from
    // the entry in depth-first order, with a branch's true side first.
    void prune() {
        auto& blocks = fun_->blocks;
        constexpr u32 none = std::numeric_limits<u32>::max();
        const auto thread = [&](u32 target) {
            for (usize steps = 0; steps < blocks.size(); ++steps) {
                const auto& b = blocks[target];
                if (!b.insts.empty() || b.term != Term::Br || b.target == target) break;
                target = b.target;
            }
            return target;
        };
        for (auto& b : blocks) {
            if (b.term == Term::Br || b.term == Term::CondBr) b.target = thread(b.target);
            if (b.term == Term::CondBr) b.other = thread(b.other);
        }
        std::vector<u32> index(blocks.size(), none), order;
        std::vector<u32> work{0};
        while (!work.empty()) {
            const u32 next = work.back();
            work.pop_back();
            if (index[next] != none) continue;
            index[next] = static_cast<u32>(order.size());
            order.push_back(next);
            const auto& b = blocks[next];
            if (b.term == Term::CondBr) work.push_back(b.other);
            if (b.term == Term::Br || b.term == Term::CondBr) work.push_back(b.target);
        }
        std::vector<Block> kept;
        for (const u32 old : order) kept.push_back(std::move(blocks[old]));
        for (auto& b : kept) {
            b.target = b.term == Term::Br || b.term == Term::CondBr ? index[b.target] : 0;
            b.other = b.term == Term::CondBr ? index[b.other] : 0;
        }
        blocks = std::move(kept);
    }

    // ------------------------------------------------------------ statements

    void statement(const Stmt& stmt) {
        switch (stmt.kind) {
            case StmtKind::Var: {
                const auto& var = *cast<VarStmt>(&stmt);
                const Reg init = var.init ? any(var.init) : Reg{};
                if (!var.names.empty()) {
                    // Destructuring: one local per element of the tuple.
                    for (usize i = 0; i < var.names.size(); ++i) {
                        const auto id = values_.at(var.names[i].span.lo);
                        const Reg dst = reg(lower_type(binding_type(id)));
                        locals_[id.value] = dst;
                        // A diverging initializer has already ended the block.
                        if (init.valid()) emit({.op = Op::Field, .dst = dst, .a = init, .imm = i});
                    }
                    return;
                }
                const auto id = values_.at(var.name.span.lo);
                const Reg dst = reg(lower_type(binding_type(id)));
                locals_[id.value] = dst;
                copy(dst, init);
                return;
            }
            case StmtKind::Assign: {
                const auto& assign = *cast<AssignStmt>(&stmt);
                const Place assigned = place(assign.target);
                if (assigned.diverged) return;
                if (!assigned.steps.empty()) return assign_field(assign, assigned);
                const Reg dst = assigned.root;
                if (assign.op == AssignOp::Assign) {
                    copy(dst, any(assign.value));
                    return;
                }
                // The target is read first; snapshot it if the value can assign it.
                const Reg target = writes(assign.value) ? snapshot(dst) : dst;
                const Reg src = any(assign.value);
                if (src.valid()) emit({.op = assign_op(assign.op), .dst = dst, .a = target, .b = src});
                return;
            }
            case StmtKind::Return: {
                const auto* ret = cast<ReturnStmt>(&stmt);
                const Reg result = ret->value ? any(ret->value) : Reg{};
                end(Term::Ret, has_value(fun_->result) ? result : Reg{});
                return;
            }
            case StmtKind::Break: end(Term::Br, {}, loops_.back().exit); return;
            case StmtKind::Continue: end(Term::Br, {}, loops_.back().head); return;
            case StmtKind::While: {
                const auto& loop = *cast<WhileStmt>(&stmt);
                const u32 head = block(), body = block(), exit = block();
                end(Term::Br, {}, head);
                current_ = head;
                Reg tested;
                const Reg cond = test(loop.cond, tested);
                end(Term::CondBr, cond, body, exit);
                current_ = body;
                bind(loop.cond, tested);
                loops_.push_back({head, exit});
                (void)branch(loop.body, false);
                loops_.pop_back();
                end(Term::Br, {}, head);
                current_ = exit;
                return;
            }
            case StmtKind::Loop: {
                const u32 body = block(), exit = block();
                end(Term::Br, {}, body);
                current_ = body;
                loops_.push_back({body, exit});
                (void)branch(cast<LoopStmt>(&stmt)->body, false);
                loops_.pop_back();
                end(Term::Br, {}, body);
                current_ = exit;
                return;
            }
            case StmtKind::For: for_loop(*cast<ForStmt>(&stmt)); return;
            case StmtKind::Expr: {
                const Expr* expr = cast<ExprStmt>(&stmt)->expr;
                if (control(expr)) (void)branch(expr, false);
                else (void)value(expr);
                return;
            }
            default: PHS_ASSERT(false && "statement outside the code generation subset");
        }
    }

    // `for x in a` reads the array once, then each element below the count
    // it had then, in order. `for i in lo..<hi` and `for i in lo..hi` read
    // both bounds once; the inclusive loop tests for its last value before
    // stepping, so a bound at the type's maximum does not overflow. Each
    // iteration binds a fresh copy, and `continue` goes to the step.
    void for_loop(const ForStmt& loop) {
        const auto id = values_.at(loop.var.span.lo);
        const Type type = lower_type(binding_type(id));
        const auto* range = as<BinaryExpr>(unparen(loop.iter));
        if (range && range->op != BinOp::Range && range->op != BinOp::RangeExcl) range = nullptr;
        Reg array, counter, limit;
        if (range) {
            // The lower bound is kept before the upper one runs, which can assign it.
            const Reg lo = any(range->lhs);
            // A diverging bound has already ended the block.
            if (!lo.valid()) return;
            counter = reg(type);
            copy(counter, lo);
            const Reg hi = any(range->rhs);
            if (!hi.valid()) return;
            limit = reg(type);
            copy(limit, hi);
        } else {
            const Reg iterated = any(loop.iter);
            if (!iterated.valid()) return;
            array = snapshot(iterated);
            counter = constant(Type::U64, 0);
            const Reg count = reg(Type::I64);
            emit({.op = Op::Field, .dst = count, .a = array, .imm = 1});
            limit = reg(Type::U64);
            emit({.op = Op::Widen, .dst = limit, .a = count});
        }
        const bool inclusive = range && range->op == BinOp::Range;
        const u32 head = block(), body = block(), step = block(), exit = block();
        end(Term::Br, {}, head);
        current_ = head;
        const Reg more = reg(Type::Bool);
        emit({.op = inclusive ? Op::Le : Op::Lt, .dst = more, .a = counter, .b = limit});
        end(Term::CondBr, more, body, exit);
        current_ = body;
        const Reg bound = reg(type);
        locals_[id.value] = bound;
        if (array.valid()) emit({.op = Op::Load, .dst = bound, .a = array, .b = counter});
        else copy(bound, counter);
        loops_.push_back({step, exit});
        (void)branch(loop.body, false);
        loops_.pop_back();
        end(Term::Br, {}, step);
        current_ = step;
        if (inclusive) {
            const Reg last = reg(Type::Bool);
            const u32 next = block();
            emit({.op = Op::Eq, .dst = last, .a = counter, .b = limit});
            end(Term::CondBr, last, exit, next);
            current_ = next;
        }
        // Below the limit, so the step cannot overflow.
        emit({.op = Op::WrapAdd, .dst = counter, .a = counter, .b = constant(fun_->regs[counter.id], 1)});
        end(Term::Br, {}, head);
        current_ = exit;
    }

    // `a.append(x)` makes room for one more element, then stores x after the
    // last one; `a.removeLast()` is `none` on an empty array and otherwise
    // the last element, which leaves the array. Either writes the array back
    // to its place, whose indices were evaluated before the arguments.
    Reg array_call(const CallExpr& call, const std::vector<Reg>& args) {
        const CheckedCall& checked = *types_.call(&call);
        Place receiver = std::move(receivers_.at(&call));
        if (receiver.diverged) return {};
        // A diverging argument has already ended the block.
        for (const Reg arg : args)
            if (!arg.valid()) return {};
        auto owners = read(receiver, receiver.steps.size());
        const Reg array = owners.back();
        const Reg count = reg(Type::I64);
        if (checked.array == ArrayMember::Append) {
            emit({.op = Op::Reserve, .dst = array, .a = array});
            emit({.op = Op::Field, .dst = count, .a = array, .imm = 1});
            const Reg grown = reg(Type::I64), at = reg(Type::U64);
            emit({.op = Op::Add, .dst = grown, .a = count, .b = constant(Type::I64, 1)});
            emit({.op = Op::With, .dst = array, .a = array, .b = grown, .imm = 1});
            emit({.op = Op::Truncate, .dst = at, .a = count});
            emit({.op = Op::Store, .a = array, .b = at, .args = {args.front()}});
            rebuild(receiver, owners);
            return {};
        }
        PHS_ASSERT(checked.array == ArrayMember::RemoveLast);
        const Reg result = reg(lower_type(scalar(&call))), empty = reg(Type::Bool);
        emit({.op = Op::Field, .dst = count, .a = array, .imm = 1});
        emit({.op = Op::Eq, .dst = empty, .a = count, .b = constant(Type::I64, 0)});
        const u32 then = block(), other = block(), join = block();
        end(Term::CondBr, empty, then, other);
        current_ = then;
        emit({.op = Op::None, .dst = result});
        end(Term::Br, {}, join);
        current_ = other;
        const Reg last = reg(Type::I64), at = reg(Type::U64);
        emit({.op = Op::Sub, .dst = last, .a = count, .b = constant(Type::I64, 1)});
        emit({.op = Op::Truncate, .dst = at, .a = last});
        const Reg value = reg(program_.layouts[layout_index(fun_->regs[array.id])].element);
        emit({.op = Op::Load, .dst = value, .a = array, .b = at});
        emit({.op = Op::With, .dst = array, .a = array, .b = last, .imm = 1});
        rebuild(receiver, owners);
        copy(result, value);
        end(Term::Br, {}, join);
        current_ = join;
        return result;
    }

    // A field or element of a local or of the receiver, possibly nested: the
    // new value is built into each enclosing aggregate in turn, from the
    // root's value after the assigned value was evaluated, and stored into
    // the innermost array element on the way. Compound assignment reads the
    // part first. Indices were evaluated with the place, before the value.
    void assign_field(const AssignStmt& assign, const Place& target) {
        Reg src;
        if (assign.op == AssignOp::Assign) src = any(assign.value);
        else {
            const Reg old = read(target, target.steps.size()).back();
            const Reg rhs = any(assign.value);
            if (rhs.valid()) {
                src = reg(fun_->regs[old.id]);
                emit({.op = assign_op(assign.op), .dst = src, .a = old, .b = rhs});
            }
        }
        // A diverging value has already ended the block.
        if (!src.valid()) return;
        auto owners = read(target, target.steps.size() - 1);
        owners.push_back(src);
        rebuild(target, owners);
    }

    // ------------------------------------------------------------ places

    // The place an assignment or a `mut fun` call targets. Its indices are
    // evaluated now, from the root out, and kept, so later code cannot change
    // them.
    Place place(const Expr* expr) {
        Place out;
        std::vector<const IndexExpr*> indices;
        expr = unparen(expr);
        while (true) {
            if (const auto* member = as<MemberExpr>(expr)) {
                out.steps.push_back({.field = field(*member), .type = lower_type(scalar(member))});
                expr = unparen(member->base);
            } else if (const auto* element = as<TupleFieldExpr>(expr)) {
                out.steps.push_back({.field = element->index, .type = lower_type(scalar(element))});
                expr = unparen(element->base);
            } else if (const auto* subscript = as<IndexExpr>(expr)) {
                out.steps.push_back({.element = true, .type = lower_type(scalar(subscript)), .span = subscript->span});
                indices.push_back(subscript);
                expr = unparen(subscript->base);
            } else break;
        }
        if (const auto* name = as<NameExpr>(expr); name && implicit_field(*name)) {
            out.steps.push_back({.field = implicit_field(*name).value(), .type = lower_type(scalar(name))});
            out.root = self_;
        } else out.root = name ? local(*name) : self_;
        std::reverse(out.steps.begin(), out.steps.end());
        auto next = indices.rbegin();
        for (auto& step : out.steps) {
            if (!step.element) continue;
            const Reg index = value((*next++)->index);
            // A diverging index has already ended the block.
            if (!index.valid()) {
                out.diverged = true;
                return out;
            }
            step.index = element_index(index);
        }
        return out;
    }
    // The root and the values of its first depth parts, each read from the previous.
    std::vector<Reg> read(const Place& place, usize depth) {
        std::vector<Reg> owners{place.root};
        for (usize i = 0; i < depth; ++i) {
            const Step& step = place.steps[i];
            if (step.element) {
                owners.push_back(load(owners.back(), step.index, step.span));
                continue;
            }
            const Reg inner = reg(step.type);
            emit({.op = Op::Field, .dst = inner, .a = owners.back(), .imm = step.field});
            owners.push_back(inner);
        }
        return owners;
    }
    // Writes owners.back() into the place's last part, rebuilding every
    // enclosing aggregate in place up to the root, or up to the innermost
    // array element, which is stored back into its array: the array's own
    // words do not change.
    void rebuild(const Place& place, const std::vector<Reg>& owners) {
        for (usize i = place.steps.size(); i-- > 0;) {
            const Step& step = place.steps[i];
            if (step.element) {
                bounds(owners[i], step.index, step.span);
                emit({.op = Op::Store, .a = owners[i], .b = step.index, .args = {owners[i + 1]}});
                return;
            }
            emit({.op = Op::With, .dst = owners[i], .a = owners[i], .b = owners[i + 1], .imm = step.field});
        }
    }

    // An index of any integer type as the U64 that Load and Store take. A
    // negative index becomes one too big to be in bounds.
    Reg element_index(Reg index) {
        const Type type = fun_->regs[index.id];
        if (is_signed(type)) {
            Reg wide = index;
            if (type != Type::I64) {
                wide = reg(Type::I64);
                emit({.op = Op::Widen, .dst = wide, .a = index});
            }
            const Reg bits = reg(Type::U64);
            emit({.op = Op::Truncate, .dst = bits, .a = wide});
            return bits;
        }
        const Reg wide = reg(Type::U64);
        emit({.op = type == Type::U64 ? Op::Copy : Op::Widen, .dst = wide, .a = index});
        return wide;
    }
    // Panics at span unless the U64 index is below the array's count.
    void bounds(Reg array, Reg index, Span span) {
        const Reg count = reg(Type::I64), limit = reg(Type::U64), in = reg(Type::Bool);
        emit({.op = Op::Field, .dst = count, .a = array, .imm = 1});
        emit({.op = Op::Widen, .dst = limit, .a = count});
        emit({.op = Op::Lt, .dst = in, .a = index, .b = limit});
        const u32 then = block(), other = block();
        end(Term::CondBr, in, then, other);
        current_ = other;
        panic("array index out of bounds", span);
        current_ = then;
    }
    // The element at a U64 index of an array, checked to be in bounds.
    Reg load(Reg array, Reg index, Span span) {
        bounds(array, index, span);
        const Reg dst = reg(program_.layouts[layout_index(fun_->regs[array.id])].element);
        emit({.op = Op::Load, .dst = dst, .a = array, .b = index});
        return dst;
    }
    // `a.count` or `a.isEmpty` of an array value.
    Reg array_property(Reg array, ArrayMember member) {
        const Reg count = reg(Type::I64);
        emit({.op = Op::Field, .dst = count, .a = array, .imm = 1});
        if (member == ArrayMember::Count) return count;
        PHS_ASSERT(member == ArrayMember::IsEmpty);
        const Reg empty = reg(Type::Bool);
        emit({.op = Op::Eq, .dst = empty, .a = count, .b = constant(Type::I64, 0)});
        return empty;
    }

    // ------------------------------------------------------------ expressions

    Reg any(const Expr* expr) { return control(expr) ? branch(expr, true) : value(expr); }

    Reg snapshot(Reg local) {
        const Reg dst = reg(fun_->regs[local.id]);
        emit({.op = Op::Copy, .dst = dst, .a = local});
        return dst;
    }

    // Only statements inside a block or `if`, and `mut fun` calls, can
    // assign a local or the receiver while an expression is evaluated.
    bool writes(const Expr* root) const {
        std::vector<const Expr*> work{root};
        while (!work.empty()) {
            const Expr* expr = work.back();
            work.pop_back();
            if (expr->kind == ExprKind::If || expr->kind == ExprKind::Block) return true;
            if (const auto* call = as<CallExpr>(expr); call && mutating_call(*call)) return true;
            if (const auto* paren = as<ParenExpr>(expr)) work.push_back(paren->inner);
            else if (const auto* unary = as<UnaryExpr>(expr)) work.push_back(unary->operand);
            else if (const auto* unwrap = as<UnwrapExpr>(expr)) work.push_back(unwrap->operand);
            else if (const auto* binary = as<BinaryExpr>(expr)) {
                work.push_back(binary->lhs);
                work.push_back(binary->rhs);
            } else if (const auto* call = as<CallExpr>(expr)) {
                for (const auto& arg : call->args) work.push_back(arg.value);
                if (const CheckedCall* checked = types_.call(call); checked && checked->receiver)
                    work.push_back(checked->receiver);
                if (indirect(*call)) work.push_back(call->callee);
            } else if (const auto* member = as<MemberExpr>(expr)) {
                work.push_back(member->base);
            } else if (const auto* chained = as<OptMemberExpr>(expr)) {
                work.push_back(chained->base);
            } else if (const auto* element = as<TupleFieldExpr>(expr)) {
                work.push_back(element->base);
            } else if (const auto* tuple = as<TupleExpr>(expr)) {
                for (const Expr* e : tuple->elems) work.push_back(e);
            } else if (const auto* array = as<ArrayExpr>(expr)) {
                for (const Expr* e : array->elems) work.push_back(e);
            } else if (const auto* subscript = as<IndexExpr>(expr)) {
                work.push_back(subscript->base);
                work.push_back(subscript->index);
            }
        }
        return false;
    }

    // Blocks and `if` chains. Recursion follows syntactic nesting; `else if`
    // chains are iterative. A nested branch whose value is the result takes
    // the result's type, as when an `if` without `else` is a block's tail.
    Reg branch(const Expr* root, bool used, std::optional<Type> want = {}) {
        root = unparen(root);
        if (const auto* block = as<BlockExpr>(root)) {
            const usize count = block->stmts.size();
            for (usize i = 0; i < count; ++i) {
                const auto* tail = used && i + 1 == count ? as<ExprStmt>(block->stmts[i]) : nullptr;
                if (tail) return control(tail->expr) ? branch(tail->expr, true, want ? want : branch_type(root)) : value(tail->expr);
                statement(*block->stmts[i]);
            }
            return {};
        }
        const auto type = !used ? Type::Unit : want ? *want : branch_type(root);
        const Reg result = has_value(type) ? reg(type) : Reg{};
        const u32 join = block();
        for (const auto* next = cast<IfExpr>(root); next;) {
            Reg tested;
            const Reg cond = test(next->cond, tested);
            const u32 then = block(), other = block();
            end(Term::CondBr, cond, then, other);
            current_ = then;
            bind(next->cond, tested);
            copy(result, branch(next->then_block, used, type));
            end(Term::Br, {}, join);
            current_ = other;
            const Expr* rest = next->else_branch;
            next = rest ? as<IfExpr>(rest) : nullptr;
            if (!next) {
                if (rest) copy(result, branch(rest, used, type));
                // Without `else`, a used `if` is `none` when no condition holds (3.24).
                else if (result.valid() && is_optional(type)) emit({.op = Op::None, .dst = result});
                end(Term::Br, {}, join);
            }
        }
        current_ = join;
        return result;
    }

    // A condition's Bool: for `let`, whether the optional it tests, kept in
    // tested, holds a value. An annotation can make the value lift first, so
    // `if let x: Int? = opt` binds opt itself even when it is `none`.
    Reg test(const Cond& cond, Reg& tested) {
        if (!cond.is_let) return any(cond.expr);
        tested = any(cond.expr);
        // A diverging value has already ended the block.
        if (!tested.valid()) return {};
        tested = fit(tested, optional(lower_type(binding_type(values_.at(cond.name.span.lo)))));
        const Reg some = reg(Type::Bool);
        emit({.op = Op::Field, .dst = some, .a = tested, .imm = 0});
        return some;
    }
    // Binds a `let` condition's name to the value inside the optional it tested.
    void bind(const Cond& cond, Reg tested) {
        if (!cond.is_let) return;
        const auto id = values_.at(cond.name.span.lo);
        const Reg dst = reg(lower_type(binding_type(id)));
        locals_[id.value] = dst;
        if (!tested.valid()) return;
        const Type value = program_.layouts[layout_index(fun_->regs[tested.id])].fields[1];
        if (value == fun_->regs[dst.id]) {
            emit({.op = Op::Field, .dst = dst, .a = tested, .imm = 1});
            return;
        }
        const Reg inner = reg(value);
        emit({.op = Op::Field, .dst = inner, .a = tested, .imm = 1});
        copy(dst, inner);
    }

    // Operators, names, literals and calls, with an explicit stack so that
    // long operator chains do not recurse.
    Reg value(const Expr* root) {
        struct Task {
            const Expr* expr;
            u32 stage = 0;
            Reg result;
            u32 join = 0;
        };
        std::vector<Task> tasks{{root}};
        std::vector<Reg> values;
        // A local read early in the expression keeps its value even if a later
        // operand assigns it.
        const bool snapshots = root->kind != ExprKind::Name && writes(root);
        while (!tasks.empty()) {
            Task& task = tasks.back();
            const Expr* expr = task.expr;
            const u32 stage = task.stage++;
            switch (expr->kind) {
                case ExprKind::Int: {
                    tasks.pop_back();
                    values.push_back(constant(lower_type(scalar(expr)), cast<IntExpr>(expr)->value));
                    break;
                }
                case ExprKind::Bool:
                    tasks.pop_back();
                    values.push_back(constant(Type::Bool, cast<BoolExpr>(expr)->value));
                    break;
                case ExprKind::String: {
                    tasks.pop_back();
                    const Reg dst = reg(Type::Str);
                    emit({.op = Op::Str, .dst = dst, .imm = string(names_.str(cast<StringExpr>(expr)->value))});
                    values.push_back(dst);
                    break;
                }
                case ExprKind::Unit:
                    tasks.pop_back();
                    values.push_back({});
                    break;
                case ExprKind::None: {
                    tasks.pop_back();
                    const Reg dst = reg(lower_type(scalar(expr)));
                    emit({.op = Op::None, .dst = dst});
                    values.push_back(dst);
                    break;
                }
                case ExprKind::Name: case ExprKind::GenericInst: {
                    tasks.pop_back();
                    if (const BindingId function = types_.function(expr); function.valid()) {
                        // A top-level function named as a value, an instance
                        // for a generic one.
                        const auto& item = *cast<FunItem>(resolution_.binding(function).item);
                        const Reg dst = reg(lower_type(scalar(expr)));
                        const u32 callee = generic(item) ? instance(item, {}, types_.function_arguments(expr))
                                                         : functions_.at(&item);
                        emit({.op = Op::FnRef, .dst = dst, .callee = callee});
                        values.push_back(dst);
                        break;
                    }
                    const auto& name = *cast<NameExpr>(expr);
                    if (const auto index = implicit_field(name)) {
                        // A bare field name inside a method reads the receiver's field.
                        const Reg dst = reg(lower_type(scalar(expr)));
                        emit({.op = Op::Field, .dst = dst, .a = self_, .imm = *index});
                        values.push_back(dst);
                        break;
                    }
                    const Reg local = this->local(name);
                    values.push_back(snapshots ? snapshot(local) : local);
                    break;
                }
                case ExprKind::SelfValue:
                    tasks.pop_back();
                    values.push_back(snapshots ? snapshot(self_) : self_);
                    break;
                case ExprKind::If: case ExprKind::Block:
                    tasks.pop_back();
                    values.push_back(branch(expr, true));
                    break;
                case ExprKind::Paren:
                    if (stage == 0) tasks.push_back({cast<ParenExpr>(expr)->inner});
                    else tasks.pop_back();
                    break;
                case ExprKind::Unwrap: {
                    if (stage == 0) {
                        tasks.push_back({cast<UnwrapExpr>(expr)->operand});
                        break;
                    }
                    tasks.pop_back();
                    const Reg operand = values.back();
                    values.pop_back();
                    values.push_back(operand.valid() ? unwrap(operand, expr->span) : Reg{});
                    break;
                }
                case ExprKind::Unary: {
                    const auto& unary = *cast<UnaryExpr>(expr);
                    const auto* literal = as<IntExpr>(unparen(unary.operand));
                    if (unary.op == UnOp::Neg && literal) {
                        // Fold a negated literal, which may be the signed minimum.
                        tasks.pop_back();
                        values.push_back(constant(lower_type(scalar(expr)), u64{0} - literal->value));
                    } else if (stage == 0) tasks.push_back({unary.operand});
                    else {
                        tasks.pop_back();
                        const Reg operand = values.back();
                        values.pop_back();
                        // A diverging operand has already ended the block.
                        if (!operand.valid()) {
                            values.push_back({});
                            break;
                        }
                        const Op op = unary.op == UnOp::Neg ? Op::Neg : unary.op == UnOp::Not ? Op::Not : Op::BitNot;
                        const Reg dst = reg(lower_type(scalar(expr)));
                        emit({.op = op, .dst = dst, .a = operand});
                        values.push_back(dst);
                    }
                    break;
                }
                case ExprKind::Binary: {
                    const auto& binary = *cast<BinaryExpr>(expr);
                    if (binary.op == BinOp::And || binary.op == BinOp::Or) logic(task, stage, binary, tasks, values);
                    else if (binary.op == BinOp::Coalesce) coalesce(task, stage, binary, tasks, values);
                    else if (stage == 0) tasks.push_back({binary.lhs});
                    else if (stage == 1) tasks.push_back({binary.rhs});
                    else {
                        tasks.pop_back();
                        const Reg rhs = values.back();
                        values.pop_back();
                        const Reg lhs = values.back();
                        values.pop_back();
                        values.push_back(arithmetic(binary, lhs, rhs));
                    }
                    break;
                }
                case ExprKind::Member: {
                    const auto& member = *cast<MemberExpr>(expr);
                    if (stage == 0) {
                        tasks.push_back({member.base});
                        break;
                    }
                    tasks.pop_back();
                    const Reg base = values.back();
                    values.pop_back();
                    // A diverging base has already ended the block.
                    if (!base.valid()) {
                        values.push_back({});
                        break;
                    }
                    if (const auto property = types_.array_member(expr)) {
                        values.push_back(array_property(base, *property));
                        break;
                    }
                    const u32 index = field(member);
                    const Reg dst = reg(lower_type(scalar(expr)));
                    emit({.op = Op::Field, .dst = dst, .a = base, .imm = index});
                    values.push_back(dst);
                    break;
                }
                case ExprKind::OptMember: {
                    const auto& chained = *cast<OptMemberExpr>(expr);
                    if (stage == 0) {
                        tasks.push_back({chained.base});
                        break;
                    }
                    tasks.pop_back();
                    const Reg base = values.back();
                    values.pop_back();
                    // A diverging base has already ended the block.
                    if (!base.valid()) {
                        values.push_back({});
                        break;
                    }
                    const auto property = types_.array_member(expr);
                    const u32 index = property ? 0 : field(&chained, chained.base);
                    const Type type = fun_->regs[base.id];
                    const Reg result = reg(lower_type(scalar(expr)));
                    const u32 join = chain(base, result);
                    const Type structure = program_.layouts[layout_index(type)].fields[1];
                    const Reg inner = reg(structure);
                    emit({.op = Op::Field, .dst = inner, .a = base, .imm = 1});
                    if (property) copy(result, array_property(inner, *property));
                    else {
                        const Reg value = reg(program_.layouts[layout_index(structure)].fields[index]);
                        emit({.op = Op::Field, .dst = value, .a = inner, .imm = index});
                        copy(result, value);
                    }
                    end(Term::Br, {}, join);
                    current_ = join;
                    values.push_back(result);
                    break;
                }
                case ExprKind::TupleField: {
                    const auto& element = *cast<TupleFieldExpr>(expr);
                    if (stage == 0) {
                        tasks.push_back({element.base});
                        break;
                    }
                    tasks.pop_back();
                    const Reg base = values.back();
                    values.pop_back();
                    // A diverging base has already ended the block.
                    if (!base.valid()) {
                        values.push_back({});
                        break;
                    }
                    (void)lower_type(scalar(element.base));
                    const Reg dst = reg(lower_type(scalar(expr)));
                    emit({.op = Op::Field, .dst = dst, .a = base, .imm = element.index});
                    values.push_back(dst);
                    break;
                }
                case ExprKind::Tuple: {
                    // Elements run left to right, then the tuple is built whole.
                    const auto& tuple = *cast<TupleExpr>(expr);
                    if (stage < tuple.elems.size()) {
                        tasks.push_back({tuple.elems[stage]});
                        break;
                    }
                    tasks.pop_back();
                    const auto count = static_cast<std::ptrdiff_t>(tuple.elems.size());
                    std::vector<Reg> elements(values.end() - count, values.end());
                    values.resize(values.size() - static_cast<usize>(count));
                    // A diverging element has already ended the block.
                    if (std::ranges::any_of(elements, [](Reg r) { return !r.valid(); })) {
                        values.push_back({});
                        break;
                    }
                    const Reg dst = reg(lower_type(scalar(expr)));
                    emit({.op = Op::Make, .dst = dst, .args = std::move(elements)});
                    values.push_back(dst);
                    break;
                }
                case ExprKind::Array: {
                    // Elements run left to right, then the array is allocated whole.
                    const auto& array = *cast<ArrayExpr>(expr);
                    if (stage < array.elems.size()) {
                        tasks.push_back({array.elems[stage]});
                        break;
                    }
                    tasks.pop_back();
                    const auto count = static_cast<std::ptrdiff_t>(array.elems.size());
                    std::vector<Reg> elements(values.end() - count, values.end());
                    values.resize(values.size() - static_cast<usize>(count));
                    // A diverging element has already ended the block.
                    if (std::ranges::any_of(elements, [](Reg r) { return !r.valid(); })) {
                        values.push_back({});
                        break;
                    }
                    const Reg dst = reg(lower_type(scalar(expr)));
                    emit({.op = Op::Array, .dst = dst, .args = std::move(elements)});
                    values.push_back(dst);
                    break;
                }
                case ExprKind::Index: {
                    // The array, then the index, then the bounds check.
                    const auto& subscript = *cast<IndexExpr>(expr);
                    if (stage < 2) {
                        tasks.push_back({stage == 0 ? subscript.base : subscript.index});
                        break;
                    }
                    tasks.pop_back();
                    const Reg index = values.back();
                    values.pop_back();
                    const Reg base = values.back();
                    values.pop_back();
                    // A diverging operand has already ended the block.
                    values.push_back(base.valid() && index.valid() ? load(base, element_index(index), expr->span) : Reg{});
                    break;
                }
                case ExprKind::Call: {
                    const auto& call = *cast<CallExpr>(expr);
                    // A method's receiver value, or the function value of an
                    // indirect call, runs before the arguments; a `mut fun`
                    // receiver is a place, read at the call.
                    const CheckedCall* checked = types_.call(&call);
                    const bool method = checked && ((checked->kind == CallKind::Method && !mutating_call(call)) ||
                                                    checked->kind == CallKind::Clone);
                    const usize first = method || indirect(call) ? 1 : 0;
                    // A `mut fun`, `append` or `removeLast` receiver's place,
                    // with its indices, comes before the arguments.
                    if (stage == 0 && mutating_call(call))
                        receivers_[&call] = checked->receiver ? place(checked->receiver) : Place{self_, {}};
                    if (stage < first) {
                        if (!method) tasks.push_back({call.callee});
                        else if (checked->receiver) tasks.push_back({checked->receiver});
                        else values.push_back(snapshots ? snapshot(self_) : self_);
                        break;
                    }
                    // Through `?.`, the arguments and the call run only when
                    // the receiver holds a value, which the call receives.
                    if (stage == first && chained(call) && values.back().valid()) {
                        const Reg receiver = values.back();
                        const Type result = lower_type(scalar(&call));
                        task.result = has_value(result) ? reg(result) : Reg{};
                        task.join = chain(receiver, task.result);
                        const Reg inner = reg(program_.layouts[layout_index(fun_->regs[receiver.id])].fields[1]);
                        emit({.op = Op::Field, .dst = inner, .a = receiver, .imm = 1});
                        values.back() = inner;
                    }
                    if (stage - first < call.args.size()) {
                        tasks.push_back({call.args[stage - first].value});
                        break;
                    }
                    const Reg result = task.result;
                    const u32 join = task.join;
                    tasks.pop_back();
                    const auto count = static_cast<std::ptrdiff_t>(call.args.size() + first);
                    std::vector<Reg> args(values.end() - count, values.end());
                    values.resize(values.size() - static_cast<usize>(count));
                    const Reg returned = this->call(call, std::move(args));
                    if (!join) {
                        values.push_back(returned);
                        break;
                    }
                    copy(result, returned);
                    end(Term::Br, {}, join);
                    current_ = join;
                    values.push_back(result);
                    break;
                }
                case ExprKind::Closure:
                    tasks.pop_back();
                    values.push_back(make_closure(*cast<ClosureExpr>(expr)));
                    break;
                default: PHS_ASSERT(false && "expression outside the code generation subset");
            }
        }
        PHS_ASSERT(values.size() == 1);
        return values.back();
    }

    // A closure value: its function, named after the function that creates it,
    // and a new environment holding a copy of each captured value.
    Reg make_closure(const ClosureExpr& closure) {
        PendingClosure pending{&closure, static_cast<u32>(program_.functions.size()), {}, {}, self_type_, env_};
        Layout layout;
        std::vector<Reg> captured;
        for (const auto& capture : resolution_.captures(&closure)) {
            pending.captures.push_back(capture.binding);
            layout.fields.push_back(lower_type(binding_type(capture.binding)));
            layout.names.emplace_back(names_.str(resolution_.binding(capture.binding).name.sym));
            captured.push_back(locals_.at(capture.binding.value));
        }
        if (self_.valid() && uses_self(closure)) {
            pending.self = fun_->regs[self_.id];
            layout.fields.push_back(*pending.self);
            layout.names.emplace_back("self");
            captured.push_back(self_);
        }
        const auto creator = static_cast<usize>(fun_ - program_.functions.data());
        auto& out = program_.functions.emplace_back();
        fun_ = &program_.functions[creator];
        out.name = std::format("{}$closure{}", fun_->name, closure_counts_[creator]++);
        out.span = closure.span;
        if (!layout.fields.empty()) {
            layout.name = out.name;
            layout.kind = Layout::Environment;
            place_fields(program_, layout);
            out.environment = static_cast<u32>(program_.layouts.size());
            program_.layouts.push_back(std::move(layout));
        }
        const Reg dst = reg(lower_type(scalar(&closure)));
        emit({.op = Op::Closure, .dst = dst, .callee = pending.function, .args = std::move(captured)});
        closures_.push_back(std::move(pending));
        return dst;
    }

    // Whether a closure, or one inside it, uses the receiver: explicitly, or
    // through a bare field name or a method call without a receiver.
    bool uses_self(const ClosureExpr& closure) const {
        std::vector<std::variant<const Expr*, const Stmt*>> work{closure.body};
        while (!work.empty()) {
            const auto next = work.back();
            work.pop_back();
            if (const auto* stmt_ptr = std::get_if<const Stmt*>(&next)) {
                const Stmt& stmt = **stmt_ptr;
                switch (stmt.kind) {
                    case StmtKind::Var: if (const Expr* init = cast<VarStmt>(&stmt)->init) work.push_back(init); break;
                    case StmtKind::Return: if (const Expr* value = cast<ReturnStmt>(&stmt)->value) work.push_back(value); break;
                    case StmtKind::While:
                        work.push_back(cast<WhileStmt>(&stmt)->cond.expr);
                        work.push_back(cast<WhileStmt>(&stmt)->body);
                        break;
                    case StmtKind::Loop: work.push_back(cast<LoopStmt>(&stmt)->body); break;
                    case StmtKind::For:
                        work.push_back(cast<ForStmt>(&stmt)->iter);
                        work.push_back(cast<ForStmt>(&stmt)->body);
                        break;
                    case StmtKind::Expr: work.push_back(cast<ExprStmt>(&stmt)->expr); break;
                    case StmtKind::Assign:
                        work.push_back(cast<AssignStmt>(&stmt)->target);
                        work.push_back(cast<AssignStmt>(&stmt)->value);
                        break;
                    default: break;
                }
                continue;
            }
            const Expr* expr = std::get<const Expr*>(next);
            if (expr->kind == ExprKind::SelfValue) return true;
            if (const auto* name = as<NameExpr>(expr)) {
                if (!types_.function(expr).valid() && implicit_field(*name)) return true;
            } else if (const auto* block = as<BlockExpr>(expr)) {
                for (const Stmt* stmt : block->stmts) work.push_back(stmt);
            } else if (const auto* inner = as<ClosureExpr>(expr)) {
                work.push_back(inner->body);
            } else if (const auto* paren = as<ParenExpr>(expr)) {
                work.push_back(paren->inner);
            } else if (const auto* unary = as<UnaryExpr>(expr)) {
                work.push_back(unary->operand);
            } else if (const auto* unwrap = as<UnwrapExpr>(expr)) {
                work.push_back(unwrap->operand);
            } else if (const auto* binary = as<BinaryExpr>(expr)) {
                work.push_back(binary->lhs);
                work.push_back(binary->rhs);
            } else if (const auto* call = as<CallExpr>(expr)) {
                const CheckedCall* checked = types_.call(call);
                if (checked && checked->kind == CallKind::Method && !checked->receiver) return true;
                for (const auto& arg : call->args) work.push_back(arg.value);
                if (checked && checked->receiver) work.push_back(checked->receiver);
                if (indirect(*call)) work.push_back(call->callee);
            } else if (const auto* member = as<MemberExpr>(expr)) {
                work.push_back(member->base);
            } else if (const auto* chained = as<OptMemberExpr>(expr)) {
                work.push_back(chained->base);
            } else if (const auto* element = as<TupleFieldExpr>(expr)) {
                work.push_back(element->base);
            } else if (const auto* tuple = as<TupleExpr>(expr)) {
                for (const Expr* e : tuple->elems) work.push_back(e);
            } else if (const auto* array = as<ArrayExpr>(expr)) {
                for (const Expr* e : array->elems) work.push_back(e);
            } else if (const auto* subscript = as<IndexExpr>(expr)) {
                work.push_back(subscript->base);
                work.push_back(subscript->index);
            } else if (const auto* branch = as<IfExpr>(expr)) {
                work.push_back(branch->cond.expr);
                work.push_back(branch->then_block);
                if (branch->else_branch) work.push_back(branch->else_branch);
            }
        }
        return false;
    }

    // Short-circuit `&&` and `||`: the right operand runs only when needed.
    template <typename Task>
    void logic(Task& task, u32 stage, const BinaryExpr& binary, std::vector<Task>& tasks, std::vector<Reg>& values) {
        if (stage == 0) {
            tasks.push_back({binary.lhs});
            return;
        }
        if (stage == 1) {
            const Reg lhs = values.back();
            values.pop_back();
            task.result = reg(Type::Bool);
            copy(task.result, lhs);
            const u32 rhs = block();
            task.join = block();
            if (binary.op == BinOp::And) end(Term::CondBr, lhs, rhs, task.join);
            else end(Term::CondBr, lhs, task.join, rhs);
            current_ = rhs;
            tasks.push_back({binary.rhs});
            return;
        }
        const Reg result = task.result;
        const u32 join = task.join;
        tasks.pop_back();
        copy(result, values.back());
        values.pop_back();
        end(Term::Br, {}, join);
        current_ = join;
        values.push_back(result);
    }

    bool chained(const CallExpr& call) const { return unparen(call.callee)->kind == ExprKind::OptMember; }
    // Starts `opt?.x`: when opt is `none`, result becomes `none` and control
    // goes to the returned join block; otherwise the current block continues.
    u32 chain(Reg optional, Reg result) {
        const Reg some = reg(Type::Bool);
        emit({.op = Op::Field, .dst = some, .a = optional, .imm = 0});
        const u32 then = block(), other = block(), join = block();
        end(Term::CondBr, some, then, other);
        current_ = other;
        if (result.valid()) emit({.op = Op::None, .dst = result});
        end(Term::Br, {}, join);
        current_ = then;
        return join;
    }

    // `opt!`: the value inside opt, or a panic at the expression when it is `none`.
    Reg unwrap(Reg optional, Span span) {
        const Reg some = reg(Type::Bool);
        emit({.op = Op::Field, .dst = some, .a = optional, .imm = 0});
        const u32 then = block(), other = block();
        end(Term::CondBr, some, then, other);
        current_ = other;
        panic("unwrapped 'none'", span);
        current_ = then;
        const Reg dst = reg(program_.layouts[layout_index(fun_->regs[optional.id])].fields[1]);
        emit({.op = Op::Field, .dst = dst, .a = optional, .imm = 1});
        return dst;
    }
    // Ends the current block with a panic that names what happened and where.
    void panic(std::string_view what, Span span) {
        const auto& file = sources_.file(span.file);
        const auto at = sources_.line_col(span.file, span.lo);
        fun_->blocks[current_].message = string(std::format("panic: {} at {}:{}:{}\n", what, file.path, at.line, at.col));
        end(Term::Panic);
    }
    // The index of bytes in the program's string pool, each distinct value once.
    u32 string(std::string_view bytes) {
        const auto [found, added] = strings_.try_emplace(std::string(bytes), program_.strings.size());
        if (added) program_.strings.emplace_back(bytes);
        return static_cast<u32>(found->second);
    }

    // `opt ?? fallback`: the fallback runs only when opt is `none`.
    template <typename Task>
    void coalesce(Task& task, u32 stage, const BinaryExpr& binary, std::vector<Task>& tasks, std::vector<Reg>& values) {
        if (stage == 0) {
            tasks.push_back({binary.lhs});
            return;
        }
        if (stage == 1) {
            const Reg lhs = values.back();
            values.pop_back();
            // A diverging operand has already ended the block.
            if (!lhs.valid()) {
                tasks.pop_back();
                values.push_back({});
                return;
            }
            const auto& layout = program_.layouts[layout_index(fun_->regs[lhs.id])];
            const Type value = layout.fields[1];
            const Reg some = reg(Type::Bool), inner = reg(value);
            emit({.op = Op::Field, .dst = some, .a = lhs, .imm = 0});
            const u32 then = block(), other = block();
            task.join = block();
            task.result = reg(lower_type(scalar(&binary)));
            end(Term::CondBr, some, then, other);
            current_ = then;
            emit({.op = Op::Field, .dst = inner, .a = lhs, .imm = 1});
            copy(task.result, inner);
            end(Term::Br, {}, task.join);
            current_ = other;
            tasks.push_back({binary.rhs});
            return;
        }
        const Reg result = task.result;
        const u32 join = task.join;
        tasks.pop_back();
        copy(result, values.back());
        values.pop_back();
        end(Term::Br, {}, join);
        current_ = join;
        values.push_back(result);
    }

    // `==` and `!=` with an optional compare both sides as the deeper
    // optional: tags first, then values, which `none` keeps zero.
    Reg optional_equality(const BinaryExpr& binary, Reg lhs, Reg rhs) {
        const ScalarType left = scalar(binary.lhs), right = scalar(binary.rhs);
        const Type type = lower_type(left.optional_depth >= right.optional_depth ? left : right);
        Reg equal = equality(fit(lhs, type), fit(rhs, type), type);
        if (binary.op == BinOp::Ne) {
            const Reg flipped = reg(Type::Bool);
            emit({.op = Op::Not, .dst = flipped, .a = equal});
            equal = flipped;
        }
        return equal;
    }
    Reg equality(Reg lhs, Reg rhs, Type type) {
        const Reg dst = reg(Type::Bool);
        if (!is_optional(type)) {
            emit({.op = Op::Eq, .dst = dst, .a = lhs, .b = rhs});
            return dst;
        }
        const Type value = program_.layouts[layout_index(type)].fields[1];
        const Reg a = reg(Type::Bool), b = reg(Type::Bool), x = reg(value), y = reg(value);
        emit({.op = Op::Field, .dst = a, .a = lhs, .imm = 0});
        emit({.op = Op::Field, .dst = b, .a = rhs, .imm = 0});
        const Reg tags = reg(Type::Bool);
        emit({.op = Op::Eq, .dst = tags, .a = a, .b = b});
        emit({.op = Op::Field, .dst = x, .a = lhs, .imm = 1});
        emit({.op = Op::Field, .dst = y, .a = rhs, .imm = 1});
        emit({.op = Op::And, .dst = dst, .a = tags, .b = equality(x, y, value)});
        return dst;
    }
    // A value lifted into the optional type `to`, if it is not of that type already.
    Reg fit(Reg value, Type to) {
        std::vector<Inst> out;
        const Reg lifted = lift(*fun_, out, value, to);
        for (auto& inst : out) emit(std::move(inst));
        return lifted;
    }

    Reg arithmetic(const BinaryExpr& binary, Reg lhs, Reg rhs) {
        const Type type = lower_type(scalar(&binary));
        if (!lhs.valid() || !rhs.valid()) {
            // Unit operands: `() == ()` is true. A Never operand already ended the block.
            if (lower_type(scalar(binary.lhs)) == Type::Unit && lower_type(scalar(binary.rhs)) == Type::Unit)
                return constant(Type::Bool, binary.op == BinOp::Eq);
            return {};
        }
        if ((binary.op == BinOp::Eq || binary.op == BinOp::Ne) &&
            (scalar(binary.lhs).optional_depth || scalar(binary.rhs).optional_depth))
            return optional_equality(binary, lhs, rhs);
        const Reg dst = reg(type);
        emit({.op = binary_op(binary.op), .dst = dst, .a = lhs, .b = rhs});
        return dst;
    }

    Reg call(const CallExpr& call, std::vector<Reg> args) {
        // Through `?.`, the method's own result, which the caller makes optional.
        const Type type = chained(call) ? lower_type(concrete(types_.call(&call)->result)) : lower_type(scalar(&call));
        if (const CheckedConversion* conversion = types_.conversion(&call)) {
            if (!args.front().valid()) return {};
            if (is_optional(type)) return checked_conversion(args.front(), type);
            if (type == lower_type(concrete(conversion->source))) return args.front();
            const Reg dst = reg(type);
            emit({.op = conversion->kind == ConversionKind::Truncating ? Op::Truncate : Op::Widen, .dst = dst,
                  .a = args.front()});
            return dst;
        }
        const CheckedCall& checked = *types_.call(&call);
        if (checked.kind == CallKind::Builtin) {
            // print: a diverging argument has already ended the block.
            if (args.front().valid()) emit({.op = Op::Print, .a = args.front()});
            return {};
        }
        if (checked.kind == CallKind::Array) return array_call(call, args);
        if (checked.kind == CallKind::Clone) {
            // Nothing owns heap memory yet, so a clone is the value itself (3.31).
            return args.front();
        }
        if (checked.kind == CallKind::NominalConstructor) {
            // A diverging argument has already ended the block.
            for (const Reg arg : args)
                if (!arg.valid()) return {};
            std::vector<Reg> fields(args.size());
            for (usize i = 0; i < args.size(); ++i) fields[checked.parameters[i]] = args[i];
            const Reg dst = reg(type);
            emit({.op = Op::Make, .dst = dst, .args = std::move(fields)});
            return dst;
        }
        if (checked.kind == CallKind::IndirectFunction) {
            // A diverging operand has already ended the block.
            for (const Reg arg : args)
                if (!arg.valid()) return {};
            const Reg dst = has_value(type) ? reg(type) : Reg{};
            emit({.op = Op::CallIndirect, .dst = dst, .a = args.front(), .args = {args.begin() + 1, args.end()}});
            if (type == Type::Never) end(Term::Unreachable);
            return dst;
        }
        const auto& target = *cast<FunItem>(resolution_.binding(checked.target).item);
        const bool method = checked.kind == CallKind::Method;
        // A method of an interface or a generic struct takes its receiver's
        // type; an implicit receiver is the current method's.
        ScalarType self;
        if (method && (interface(*owners_.at(&target)) || !owners_.at(&target)->generics.empty()))
            self = checked.receiver ? scalar(checked.receiver) : self_type_;
        // An interface method runs its witness for the receiver's struct: the
        // struct's own method, or the interface's default (static dispatch).
        const auto& callee = method && interface(*owners_.at(&target)) ? witness(target, self) : target;
        // Arguments run in source order and are passed in parameter order,
        // after a method's receiver. A `mut fun` requirement's receiver is a
        // place even when its witness is a plain `fun`, which takes its value.
        const bool at_place = method && mutating(target);
        const bool inout = method && mutating(callee);
        const usize first = method && !at_place ? 1 : 0;
        std::vector<Reg> ordered(callee.sig.params.size() + (method ? 1 : 0));
        if (first) ordered.front() = args.front();
        for (usize i = first; i < args.size(); ++i) ordered[checked.parameters[i - first] + (method ? 1 : 0)] = args[i];
        // A diverging operand has already ended the block.
        for (const Reg arg : args)
            if (!arg.valid()) return {};
        Place receiver;
        std::vector<Reg> owners;
        if (at_place) {
            // The receiver's place is read now, and written back after the
            // call when the callee mutates it.
            receiver = std::move(receivers_.at(&call));
            if (receiver.diverged) return {};
            owners = read(receiver, receiver.steps.size());
            ordered.front() = owners.back();
        }
        const Reg dst = has_value(type) ? reg(type) : Reg{};
        const u32 function = !generic(callee) ? functions_.at(&callee)
                           : !method || (!interface(*owners_.at(&callee)) && owners_.at(&callee)->generics.empty())
                               ? instance(callee, {}, checked.type_arguments)
                               : instance(callee, self, &callee == &target ? checked.type_arguments
                                                                           : std::span<const ScalarType>{});
        emit({.op = Op::Call, .dst = dst, .callee = function, .args = std::move(ordered)});
        if (type == Type::Never) end(Term::Unreachable);
        else if (inout) rebuild(receiver, owners);
        return dst;
    }

    // `T.checked(x)`: x as a T when it fits, else `none`. It fits when
    // truncating keeps its bits and, between types of different signedness,
    // when it is not negative as the signed one.
    Reg checked_conversion(Reg source, Type type) {
        const Type from = fun_->regs[source.id], to = program_.layouts[layout_index(type)].fields[1];
        const Reg narrowed = reg(to), fits = reg(Type::Bool);
        emit({.op = Op::Truncate, .dst = narrowed, .a = source});
        emit({.op = Op::Eq, .dst = fits, .a = narrowed, .b = source});
        Reg ok = fits;
        if (is_signed(from) != is_signed(to)) {
            const Reg signed_value = is_signed(from) ? source : narrowed;
            const Reg sign = reg(Type::Bool);
            emit({.op = Op::Ge, .dst = sign, .a = signed_value, .b = constant(fun_->regs[signed_value.id], 0)});
            ok = reg(Type::Bool);
            emit({.op = Op::And, .dst = ok, .a = fits, .b = sign});
        }
        const Reg result = reg(type);
        const u32 then = block(), other = block(), join = block();
        end(Term::CondBr, ok, then, other);
        current_ = then;
        copy(result, narrowed);
        end(Term::Br, {}, join);
        current_ = other;
        emit({.op = Op::None, .dst = result});
        end(Term::Br, {}, join);
        current_ = join;
        return result;
    }

    // The method that runs for interface method `requirement` on a struct
    // `receiver`: the struct's implementation, or the interface's default.
    // The gate has checked that there is one.
    const FunItem& witness(const FunItem& requirement, ScalarType receiver) const {
        const BindingId id = items_.at(&requirement);
        for (const auto& witness : types_.hierarchy(types_.descriptor(receiver)->declaration)->witnesses)
            if (witness.requirement == id) return *cast<FunItem>(resolution_.binding(witness.implementation).item);
        PHS_ASSERT(false && "a struct without a witness for an interface method");
        unreachable();
    }

    // The longest type argument an instance's name spells out in full.
    static constexpr usize kArgumentNameLimit = 256;
    // The function of a generic function's instance for a call's type
    // arguments, added the first time a call needs it. The gate has bounded
    // how many there are (E0602).
    // A method of a generic struct also takes the type arguments of `self`,
    // its receiver's type, first, and is named after that type.
    u32 instance(const FunItem& callee, ScalarType self, std::span<const ScalarType> type_arguments) {
        PendingFunction pending{&callee, static_cast<u32>(program_.functions.size()), {}};
        std::string name = templates_.at(&callee);
        if (self.kind == ScalarType::Nominal && interface(*owners_.at(&callee))) {
            // An interface's default for a struct, as in `Shape.twice<Square>`.
            pending.env.emplace_back(items_.at(owners_.at(&callee)), self);
            name += std::format("<{}>", types_.type_name(self, resolution_, names_, kArgumentNameLimit));
        } else if (self.kind == ScalarType::Nominal) {
            const auto& owner = *owners_.at(&callee);
            const auto owner_arguments = types_.descriptor(self)->arguments;
            for (usize i = 0; i < owner.generics.size(); ++i)
                pending.env.emplace_back(values_.at(owner.generics[i].name.span.lo), owner_arguments[i]);
            name = types_.type_name(self, resolution_, names_, kArgumentNameLimit) +
                   name.substr(names_.str(owner.name.sym).size());
        }
        for (usize i = 0; i < type_arguments.size(); ++i)
            pending.env.emplace_back(values_.at(callee.sig.generics[i].name.span.lo), concrete(type_arguments[i]));
        std::string key = std::to_string(reinterpret_cast<std::uintptr_t>(&callee));
        for (const auto& [parameter, argument] : pending.env)
            key += std::format(":{}.{}.{}", static_cast<int>(argument.kind), argument.optional_depth, argument.detail);
        if (const auto it = instances_.find(key); it != instances_.end()) return it->second;
        if (!type_arguments.empty()) {
            name += "<";
            for (usize i = 0; i < type_arguments.size(); ++i) {
                if (i) name += ", ";
                name += types_.type_name(pending.env[pending.env.size() - type_arguments.size() + i].second, resolution_,
                                         names_, kArgumentNameLimit);
            }
            name += ">";
        }
        // Long arguments are elided, so distinct instances can share a name;
        // later ones get the first numeric suffix no other instance has.
        // Every instance name holds a '<', so other functions never clash.
        std::string unique = name;
        for (u32 n = 1; !instance_names_.insert(unique).second; ++n) unique = std::format("{}.{}", name, n);
        name = std::move(unique);
        const auto caller = static_cast<usize>(fun_ - program_.functions.data());
        auto& out = program_.functions.emplace_back();
        fun_ = &program_.functions[caller];
        out.name = std::move(name);
        out.span = callee.name.span;
        instances_.emplace(std::move(key), pending.function);
        funs_.push_back(std::move(pending));
        return funs_.back().function;
    }

    // ------------------------------------------------------------ lifting

    // Wraps value in `some` until it has type `to`, appending the
    // instructions to out.
    Reg lift(Function& fun, std::vector<Inst>& out, Reg value, Type to) const {
        if (!value.valid() || fun.regs[value.id] == to) return value;
        PHS_ASSERT(is_optional(to));
        const Type inner = program_.layouts[layout_index(to)].fields[1];
        const Reg wrapped = lift(fun, out, value, inner);
        const auto added = [&](Type type) {
            fun.regs.push_back(type);
            return Reg{static_cast<u32>(fun.regs.size() - 1)};
        };
        const Reg some = added(Type::Bool), dst = added(to);
        out.push_back({.op = Op::Const, .dst = some, .imm = 1});
        out.push_back({.op = Op::Make, .dst = dst, .args = {some, wrapped}});
        return dst;
    }
    // Lifts every operand stored where its optional is expected: copies,
    // arguments, fields, elements and results.
    void lift(Function& fun) const {
        for (auto& block : fun.blocks) {
            std::vector<Inst> out;
            out.reserve(block.insts.size());
            for (auto& inst : block.insts) {
                switch (inst.op) {
                    case Op::Copy: inst.a = lift(fun, out, inst.a, fun.regs[inst.dst.id]); break;
                    case Op::Make: {
                        const auto& layout = program_.layouts[layout_index(fun.regs[inst.dst.id])];
                        for (usize i = 0; i < inst.args.size(); ++i)
                            inst.args[i] = lift(fun, out, inst.args[i], layout.fields[i]);
                        break;
                    }
                    case Op::With:
                        inst.b = lift(fun, out, inst.b, program_.layouts[layout_index(fun.regs[inst.a.id])].fields[inst.imm]);
                        break;
                    case Op::Array: {
                        const Type element = program_.layouts[layout_index(fun.regs[inst.dst.id])].element;
                        for (auto& arg : inst.args) arg = lift(fun, out, arg, element);
                        break;
                    }
                    case Op::Store:
                        inst.args.front() =
                            lift(fun, out, inst.args.front(), program_.layouts[layout_index(fun.regs[inst.a.id])].element);
                        break;
                    case Op::Call: {
                        const auto& callee = program_.functions[inst.callee];
                        for (usize i = 0; i < inst.args.size(); ++i)
                            inst.args[i] = lift(fun, out, inst.args[i], callee.regs[i]);
                        break;
                    }
                    case Op::CallIndirect: {
                        const auto& layout = program_.layouts[layout_index(fun.regs[inst.a.id])];
                        for (usize i = 0; i < inst.args.size(); ++i)
                            inst.args[i] = lift(fun, out, inst.args[i], layout.params[i]);
                        break;
                    }
                    default: break;
                }
                out.push_back(std::move(inst));
            }
            if (block.term == Term::Ret) block.value = lift(fun, out, block.value, fun.result);
            block.insts = std::move(out);
        }
    }

    const Resolution& resolution_;
    ScalarTypes& types_;
    const Interner& names_;
    const SourceManager& sources_;
    std::unordered_map<const Item*, BindingId> items_;
    std::unordered_map<u32, BindingId> values_;  // locals and parameters by declaration offset
    std::unordered_map<const FunItem*, u32> functions_;  // non-generic functions
    std::unordered_map<const FunItem*, std::string> templates_;  // functions with instances to their base names
    std::unordered_map<std::string, u32> instances_;  // generic function and type arguments to its instance
    std::unordered_set<std::string> instance_names_;
    std::vector<PendingFunction> funs_;
    std::vector<ScalarTypes::TypeArgument> env_;  // the type arguments of the instance being lowered
    std::unordered_map<std::string, u64> strings_;  // literal bytes to their index in program_.strings
    std::unordered_map<usize, u32> layouts_;  // struct type descriptor to its index in program_.layouts
    std::unordered_map<usize, u32> tuples_;   // tuple type descriptor to its index in program_.layouts
    std::unordered_map<usize, u32> function_types_;  // function type descriptor to its index in program_.layouts
    std::unordered_map<usize, u32> arrays_;   // array type descriptor to its index in program_.layouts
    std::unordered_map<u32, u32> optionals_;  // an optional's value type to its index in program_.layouts
    std::unordered_map<u32, u32> fields_;     // field declaration offset to its declaration index
    std::unordered_map<const FunItem*, const TypeDeclItem*> owners_;  // a method's struct
    Reg self_;  // the current method's receiver
    ScalarType self_type_;  // and its type
    Program program_;
    Function* fun_ = nullptr;
    u32 current_ = 0;
    std::unordered_map<usize, Reg> locals_;  // by binding ID
    std::vector<Loop> loops_;
    std::unordered_map<const CallExpr*, Place> receivers_;  // `mut fun` receivers placed before their arguments
    std::vector<PendingClosure> closures_;
    std::unordered_map<usize, u32> closure_counts_;  // closures created so far, by creating function
};

}  // namespace

Program lower(const File& file, const Resolution& resolution, ScalarTypes& types, const Interner& names,
              const SourceManager& sources) {
    return Lowerer(resolution, types, names, sources).run(file);
}

}  // namespace phs::mir
