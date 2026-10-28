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

#include <concepts>
#include <span>
#include <type_traits>
#include <utility>

#include "support/interner.hpp"
#include "support/source.hpp"

// AST for Phantasos-Core. Every node lives in an Arena and is trivially
// destructible; lists are arena-backed spans. Nodes are tagged with a kind and
// downcast with as<T>() (no RTTI).

namespace phs::ast {

template <typename T>
using List = std::span<T>;

struct Expr;
struct Stmt;
struct Pattern;
struct Item;
struct BlockExpr;

struct Name {
    Symbol sym;
    Span span;
};

struct Path {
    List<Name> parts;
    Span span;
};

// ---------------------------------------------------------------- types

enum class TypeKind : u8 { Path, Optional, Shared, Weak, Pointer, Array, Unit, SelfType, Fun, Tuple, Error };

struct TypeExpr {
    TypeKind kind = TypeKind::Error;
    u32 id = 0;  // nonzero, unique within its file; keys side tables (NodeMap)
    Span span;
    Path path;                     // Path
    List<TypeExpr*> args;          // Path: generic args; Fun: parameter types; Tuple: element types
    TypeExpr* inner = nullptr;     // Optional/Shared/Weak/Pointer/Array element; Fun: return type
    TypeExpr* throws_type = nullptr;  // Fun
    bool is_mut = false;           // Pointer
    bool throws = false;           // Fun
};

// ---------------------------------------------------------------- expressions

enum class ExprKind : u8 {
    Int, Float, Char, String, Interp, Bool, None, Unit, SelfValue, Super, SelfType,
    Name, ImplicitMember, Paren, Array, Tuple, Unary, Binary, Cast, Call, Member, OptMember,
    TupleField, Index, Unwrap, GenericInst, If, Match, Block, Unsafe, Closure, Error
};

enum class UnOp : u8 { Neg, Not, BitNot, Try, TryOpt, Await, Share };

enum class BinOp : u8 {
    Coalesce, Or, And,
    Eq, Ne, Lt, Le, Gt, Ge,
    Range, RangeExcl,
    Add, Sub, WrapAdd, WrapSub, BitOr, BitXor,
    Mul, Div, Rem, WrapMul, BitAnd,
    Shl, Shr
};

enum class CastOp : u8 { Is, As, AsOpt };

struct Expr {
    ExprKind kind;
    u32 id = 0;  // nonzero, unique within its file; keys side tables (NodeMap)
    Span span;
};

template <ExprKind K>
struct ExprOf : Expr {
    static constexpr ExprKind kKind = K;
    ExprOf() : Expr{K, 0, {}} {}
};

struct Arg {
    Name label;  // label.sym empty if positional
    Expr* value;
};

struct Cond {
    bool is_let = false;
    Name name;                 // is_let
    TypeExpr* type = nullptr;  // is_let, optional
    Expr* expr = nullptr;
    Span span;
};

struct IntExpr : ExprOf<ExprKind::Int> { u64 value = 0; };
struct FloatExpr : ExprOf<ExprKind::Float> { double value = 0; };
struct CharExpr : ExprOf<ExprKind::Char> { u32 value = 0; };
struct StringExpr : ExprOf<ExprKind::String> { Symbol value; };
struct InterpExpr : ExprOf<ExprKind::Interp> {
    List<Symbol> texts;  // texts.size() == exprs.size() + 1
    List<Expr*> exprs;
};
struct BoolExpr : ExprOf<ExprKind::Bool> { bool value = false; };
struct NoneExpr : ExprOf<ExprKind::None> {};
struct UnitExpr : ExprOf<ExprKind::Unit> {};
struct SelfValueExpr : ExprOf<ExprKind::SelfValue> {};
struct SuperExpr : ExprOf<ExprKind::Super> {};
struct SelfTypeExpr : ExprOf<ExprKind::SelfType> {};
struct NameExpr : ExprOf<ExprKind::Name> { Name name; };
struct ImplicitMemberExpr : ExprOf<ExprKind::ImplicitMember> { Name name; };
struct ParenExpr : ExprOf<ExprKind::Paren> { Expr* inner = nullptr; };
struct ArrayExpr : ExprOf<ExprKind::Array> { List<Expr*> elems; };
struct TupleExpr : ExprOf<ExprKind::Tuple> { List<Expr*> elems; };  // two or more elements
struct UnaryExpr : ExprOf<ExprKind::Unary> {
    UnOp op = UnOp::Neg;
    Expr* operand = nullptr;
};
struct BinaryExpr : ExprOf<ExprKind::Binary> {
    BinOp op = BinOp::Add;
    Expr* lhs = nullptr;
    Expr* rhs = nullptr;
};
struct CastExpr : ExprOf<ExprKind::Cast> {
    CastOp op = CastOp::Is;
    Expr* operand = nullptr;
    TypeExpr* type = nullptr;
};
struct CallExpr : ExprOf<ExprKind::Call> {
    Expr* callee = nullptr;
    List<Arg> args;
};
struct MemberExpr : ExprOf<ExprKind::Member> {
    Expr* base = nullptr;
    Name name;
};
struct OptMemberExpr : ExprOf<ExprKind::OptMember> {
    Expr* base = nullptr;
    Name name;
};
// `t.0`: a tuple element, by its decimal index.
struct TupleFieldExpr : ExprOf<ExprKind::TupleField> {
    Expr* base = nullptr;
    u32 index = 0;
    Span index_span;
};
struct IndexExpr : ExprOf<ExprKind::Index> {
    Expr* base = nullptr;
    Expr* index = nullptr;
};
struct UnwrapExpr : ExprOf<ExprKind::Unwrap> { Expr* operand = nullptr; };
struct GenericInstExpr : ExprOf<ExprKind::GenericInst> {
    Expr* base = nullptr;
    List<TypeExpr*> args;
};
struct BlockExpr : ExprOf<ExprKind::Block> { List<Stmt*> stmts; };
struct IfExpr : ExprOf<ExprKind::If> {
    Cond cond;
    BlockExpr* then_block = nullptr;
    Expr* else_branch = nullptr;  // BlockExpr or IfExpr, or null
};
struct MatchArm {
    Pattern* pattern;
    Expr* guard;  // may be null
    Expr* body;   // BlockExpr or any expression
    Span span;
};
struct MatchExpr : ExprOf<ExprKind::Match> {
    Expr* scrutinee = nullptr;
    List<MatchArm> arms;
};
struct UnsafeExpr : ExprOf<ExprKind::Unsafe> { BlockExpr* block = nullptr; };
// `{ a, b: Int -> body }`: parameters with optional types, then statements
// whose last expression is the result. `{ -> body }` takes no parameters.
struct ClosureParam {
    Name name;
    TypeExpr* type;  // may be null
    Span span;
};
struct ClosureExpr : ExprOf<ExprKind::Closure> {
    List<ClosureParam> params;
    BlockExpr* body = nullptr;  // spans from '->' to the closing brace
};
struct ErrorExpr : ExprOf<ExprKind::Error> {};

// ---------------------------------------------------------------- patterns

enum class PatKind : u8 { Wildcard, Literal, Binding, Case, Is, Alt, Error };

struct Pattern {
    PatKind kind;
    Span span;
};

template <PatKind K>
struct PatOf : Pattern {
    static constexpr PatKind kKind = K;
    PatOf() : Pattern{K, {}} {}
};

struct WildcardPat : PatOf<PatKind::Wildcard> {};
struct LiteralPat : PatOf<PatKind::Literal> { Expr* literal = nullptr; };  // may be Unary(Neg, Int/Float)
struct BindingPat : PatOf<PatKind::Binding> { Name name; };
struct CasePat : PatOf<PatKind::Case> {
    bool leading_dot = false;
    List<Name> path;  // last element is the case name
    bool has_args = false;
    List<Pattern*> args;
};
struct IsPat : PatOf<PatKind::Is> {
    TypeExpr* type = nullptr;
    Name binding;  // empty if absent
};
struct AltPat : PatOf<PatKind::Alt> { List<Pattern*> alts; };
struct ErrorPat : PatOf<PatKind::Error> {};

// ---------------------------------------------------------------- statements

enum class StmtKind : u8 { Var, Return, Break, Continue, Throw, While, For, Loop, Defer, Expr, Assign };

enum class Binding : u8 { None, Let, Var, Const };

enum class AssignOp : u8 { Assign, Add, Sub, Mul, Div, Rem, BitAnd, BitOr, BitXor, Shl, Shr };

struct Stmt {
    StmtKind kind;
    Span span;
};

template <StmtKind K>
struct StmtOf : Stmt {
    static constexpr StmtKind kKind = K;
    StmtOf() : Stmt{K, {}} {}
};

struct VarStmt : StmtOf<StmtKind::Var> {
    Binding binding = Binding::Let;
    Name name;          // empty when destructuring
    List<Name> names;   // `let (a, b) = e`: one binding per tuple element
    TypeExpr* type = nullptr;
    Expr* init = nullptr;
};
struct ReturnStmt : StmtOf<StmtKind::Return> { Expr* value = nullptr; };
struct BreakStmt : StmtOf<StmtKind::Break> {};
struct ContinueStmt : StmtOf<StmtKind::Continue> {};
struct ThrowStmt : StmtOf<StmtKind::Throw> { Expr* value = nullptr; };
struct WhileStmt : StmtOf<StmtKind::While> {
    Cond cond;
    BlockExpr* body = nullptr;
};
struct ForStmt : StmtOf<StmtKind::For> {
    Name var;
    Expr* iter = nullptr;
    BlockExpr* body = nullptr;
};
struct LoopStmt : StmtOf<StmtKind::Loop> { BlockExpr* body = nullptr; };
struct DeferStmt : StmtOf<StmtKind::Defer> { BlockExpr* body = nullptr; };
struct ExprStmt : StmtOf<StmtKind::Expr> { Expr* expr = nullptr; };
struct AssignStmt : StmtOf<StmtKind::Assign> {
    AssignOp op = AssignOp::Assign;
    Expr* target = nullptr;
    Expr* value = nullptr;
};

// ---------------------------------------------------------------- items

enum class ItemKind : u8 {
    Fun, Class, Struct, Actor, Interface, Enum, EnumCase, Var, Extern, TypeAlias, Init, Deinit
};

constexpr bool is_type_decl(ItemKind k) {
    return k == ItemKind::Class || k == ItemKind::Struct || k == ItemKind::Actor || k == ItemKind::Interface ||
           k == ItemKind::Enum;
}

enum class Visibility : u8 { Default, Public, Internal, File, Private };

enum class Modifier : u16 {
    None = 0,
    Open = 1 << 0,
    Final = 1 << 1,
    Abstract = 1 << 2,
    Sealed = 1 << 3,
    Override = 1 << 4,
    Static = 1 << 5,
    Mut = 1 << 6,
    Async = 1 << 7,
    Unsafe = 1 << 8,
};

constexpr Modifier operator|(Modifier a, Modifier b) {
    return static_cast<Modifier>(std::to_underlying(a) | std::to_underlying(b));
}

constexpr Modifier& operator|=(Modifier& a, Modifier b) { return a = a | b; }

constexpr bool has_modifier(Modifier mods, Modifier flag) {
    return flag != Modifier::None && (std::to_underlying(mods) & std::to_underlying(flag)) == std::to_underlying(flag);
}

enum class Convention : u8 { Default, Borrow, Inout, Own };

struct AttrArg {
    Name key;  // empty if positional
    Expr* value;
};

struct Attribute {
    Name name;
    List<AttrArg> args;
    bool has_parens;
    Span span;
};

struct GenericParam {
    Name name;
    List<TypeExpr*> bounds;
};

struct WhereClause {
    TypeExpr* type;
    List<TypeExpr*> bounds;
};

struct Param {
    Binding binding;  // None, Let or Var (Let/Var only in primary constructors)
    Name name;
    Convention conv;
    TypeExpr* type;
    Expr* default_value;  // may be null
    Span span;
};

struct SuperEntry {
    TypeExpr* type;
    bool has_args;
    List<Arg> args;
};

struct Item {
    ItemKind kind;
    Span span;
    List<Attribute> attrs;
    Visibility vis = Visibility::Default;
    Modifier mods = Modifier::None;
    Name name;  // empty for Extern/Init/Deinit
};

template <ItemKind K>
struct ItemOf : Item {
    static constexpr ItemKind kKind = K;
    ItemOf() : Item{K, {}, {}, Visibility::Default, Modifier::None, {}} {}
};

struct Signature {
    List<GenericParam> generics;
    List<Param> params;
    TypeExpr* ret = nullptr;
    bool throws = false;
    TypeExpr* throws_type = nullptr;  // null for untyped `throws`
    List<WhereClause> where;
};

struct FunItem : ItemOf<ItemKind::Fun> {
    Signature sig;
    Expr* body = nullptr;  // BlockExpr, or an expression for `= expr`; null if absent
    bool expr_body = false;
};

// Shared by class, struct, actor, interface and enum declarations.
struct TypeDeclItem : Item {
    explicit TypeDeclItem(ItemKind k) : Item{k} { PHS_ASSERT(is_type_decl(k)); }

    List<GenericParam> generics;
    bool has_ctor = false;
    List<Param> ctor_params;
    List<SuperEntry> supers;
    List<WhereClause> where;
    bool has_body = false;
    List<Item*> members;  // for enums this includes EnumCase items in order
};

struct CaseField {
    Name label;  // empty if unlabeled
    TypeExpr* type;
};

struct EnumCaseItem : ItemOf<ItemKind::EnumCase> {
    bool has_fields = false;
    List<CaseField> fields;
};

struct VarItem : ItemOf<ItemKind::Var> {
    Binding binding = Binding::Let;
    TypeExpr* type = nullptr;
    Expr* init = nullptr;
};

struct ExternItem : ItemOf<ItemKind::Extern> {
    Symbol abi;
    List<Item*> items;
};

struct TypeAliasItem : ItemOf<ItemKind::TypeAlias> {
    List<GenericParam> generics;
    List<TypeExpr*> bounds;  // associated-type bounds inside interfaces
    TypeExpr* type = nullptr;  // null for an abstract associated type
};

struct InitItem : ItemOf<ItemKind::Init> {
    bool has_params = false;  // false: body of the primary constructor
    List<Param> params;
    bool throws = false;
    TypeExpr* throws_type = nullptr;
    BlockExpr* body = nullptr;
};

struct DeinitItem : ItemOf<ItemKind::Deinit> { BlockExpr* body = nullptr; };

struct Import {
    Path path;
    Name alias;  // empty if absent
    Span span;
};

struct File {
    FileId file;
    List<Import> imports;
    List<Item*> items;
    u32 expr_ids = 0;  // Expr ids run from 1 to this
    u32 type_ids = 0;  // TypeExpr ids run from 1 to this
};

// ---------------------------------------------------------------- casting

// Only tagged nodes in the matching AST family can be downcast. Constness
// comes from the source pointer; callers cannot accidentally cast it away.
template <typename B>
concept NodeBase = !std::is_volatile_v<B> &&
    (std::same_as<std::remove_const_t<B>, Expr> || std::same_as<std::remove_const_t<B>, Stmt> ||
     std::same_as<std::remove_const_t<B>, Pattern> || std::same_as<std::remove_const_t<B>, Item>);

template <typename T, typename B>
concept TaggedNodeFor = NodeBase<B> && std::same_as<T, std::remove_cv_t<T>> &&
    std::derived_from<T, std::remove_const_t<B>> && requires(B* node) {
        { node->kind == T::kKind } -> std::same_as<bool>;
    };

template <typename T, typename B>
using NodePtr = std::conditional_t<std::is_const_v<B>, const T*, T*>;

template <typename T, typename B>
    requires TaggedNodeFor<T, B>
[[nodiscard]] NodePtr<T, B> as(B* node) {
    return node && node->kind == T::kKind ? static_cast<NodePtr<T, B>>(node) : nullptr;
}

template <typename T, typename B>
    requires TaggedNodeFor<T, B>
[[nodiscard]] NodePtr<T, B> cast(B* node) {
    PHS_ASSERT(node && node->kind == T::kKind);
    return static_cast<NodePtr<T, B>>(node);
}

template <typename B>
    requires (std::same_as<B, Item> || std::same_as<B, const Item>)
[[nodiscard]] NodePtr<TypeDeclItem, B> as_type_decl(B* item) {
    return item && is_type_decl(item->kind) ? static_cast<NodePtr<TypeDeclItem, B>>(item) : nullptr;
}

template <typename B>
    requires (std::same_as<B, Item> || std::same_as<B, const Item>)
[[nodiscard]] NodePtr<TypeDeclItem, B> cast_type_decl(B* item) {
    PHS_ASSERT(item && is_type_decl(item->kind));
    return static_cast<NodePtr<TypeDeclItem, B>>(item);
}

}  // namespace phs::ast
