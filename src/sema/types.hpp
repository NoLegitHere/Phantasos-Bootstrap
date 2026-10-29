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

#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "sema/resolve.hpp"
#include "support/flat_map.hpp"
#include "support/id_map.hpp"
#include "support/node_map.hpp"

namespace phs {

// Built-in identity or a canonical composite/nominal descriptor, plus optional
// wrappers. Descriptor IDs belong to one ScalarTypes result, never another file.
// Int/UInt use the initial x86_64 ranges and stay distinct from Int64/UInt64.
// Deferred means this pass cannot establish a type, not success.
struct ScalarType {
    enum Kind : u8 {
        Deferred, Error, Int8, Int16, Int32, Int64, UInt8, UInt16, UInt32, UInt64,
        Int, UInt, Float32, Float64, Bool, Char, String, Unit, Never,
        Nominal, Array, Pointer, Shared, Weak, Function, Tuple,
        TypeParameter, Projection // symbolic generic variables and associated type projections
    };
    Kind kind = Deferred;
    u32 optional_depth = 0;
    usize detail = std::numeric_limits<usize>::max(); // descriptor ID in this result's type table
    constexpr ScalarType(Kind value = Deferred, u32 depth = 0, usize descriptor = std::numeric_limits<usize>::max())
        : kind(value), optional_depth(depth), detail(descriptor) {}
    constexpr ScalarType scalar() const { auto result = *this; result.optional_depth = 0; return result; }
    constexpr ScalarType optional() const {
        return kind == Deferred || kind == Error ? ScalarType{kind} : ScalarType{kind, optional_depth + 1, detail};
    }
    constexpr ScalarType element() const { return optional_depth ? ScalarType{kind, optional_depth - 1, detail} : ScalarType{}; }
    friend bool operator==(ScalarType, ScalarType) = default;
};

std::string scalar_type_name(ScalarType type);

struct TypeDescriptor {
    ScalarType::Kind kind = ScalarType::Deferred;
    ScalarType element;
    std::vector<ScalarType> parameters; // function parameters, or tuple elements
    ScalarType result;
    ScalarType error;
    BindingId declaration;
    bool mutable_pointer = false;
    bool throws = false;
    std::vector<ScalarType> arguments; // nominal generic arguments, invariant and part of identity
    friend bool operator==(const TypeDescriptor&, const TypeDescriptor&) = default;
};
struct TypeDescriptorHash {
    usize operator()(const TypeDescriptor& descriptor) const;
};
class TypeTable {
public:
    static constexpr usize kNameLimit = 32 * 1024;
    ScalarType intern(TypeDescriptor descriptor);
    // During construction, interning may invalidate descriptor pointers. A
    // completed ScalarTypes result is immutable and exposes stable pointers.
    const TypeDescriptor* descriptor(ScalarType type) const {
        return type.detail == std::numeric_limits<usize>::max() ? nullptr : &descriptors_.at(type.detail);
    }
    // Bound both output and expansion work; an incomplete name ends in "...".
    std::string name(ScalarType type, const Resolution& resolution, const Interner& names, usize limit = kNameLimit) const;
private:
    struct Slot { u32 id = 0; u32 hash = 0; };  // index in descriptors_ + 1 (0 is empty), low hash bits
    void grow();
    std::vector<TypeDescriptor> descriptors_;
    std::vector<u64> hashes_;  // parallel to descriptors_
    std::vector<Slot> slots_;  // open addressing; size is zero or a power of two
};

enum class ExpansionState : u8 { Resolved, Deferred, Error };
struct ExpandedType {
    ExpansionState state = ExpansionState::Deferred;
    BindingId target; // terminal declaration identity, not the alias's identity
    ScalarType scalar; // historical name: includes normalized composite facts
    u32 optional_depth = 0; // retained even for nominal targets
    const ast::TypeExpr* syntax = nullptr; // eligible transparent RHS, for composite type normalization
};
struct TypeExpansion {
    ExpandedType type;
    ast::Name unknown_member;
    BindingId member_owner;
};

class AliasExpander;
// Owns expansion facts, with IDs belonging to the supplied Resolution. Keep
// that result, its AST and interner alive when querying type syntax/entities.
// Queries are pure; unknown_member lets the caller diagnose an unresolved
// direct nominal member without duplicating a declaration's existing error.
class AliasExpansion {
public:
    const ExpandedType* alias(BindingId id) const {
        const auto it = aliases_.find(id.value);
        return it == aliases_.end() ? nullptr : &it->second;
    }
    TypeExpansion type(const ast::TypeExpr* syntax, const Resolution& resolution) const;
    ExpandedType entity(const ast::Expr* syntax, const Resolution& resolution) const;

private:
    friend class AliasExpander;
    friend class ScalarChecker;
    struct Lookup {
        ExpandedType type;
        BindingId pending;
        ast::Name unknown;
        BindingId owner;
    };
    Lookup binding(BindingId id, const Resolution& resolution) const;
    Lookup path(std::span<const ast::Name> parts, NameSpace space, const Resolution& resolution) const;
    Lookup lookup(const ast::TypeExpr* syntax, const Resolution& resolution) const;
    std::unordered_map<usize, ExpandedType> aliases_;
    std::unordered_map<usize, ScalarType> builtins_;
    // Keyed by member_key(members scope, name): one table for every type.
    static u64 member_key(usize scope, u32 name) { return u64(scope) << 32 | name; }
    FlatMap<u64, BindingId> member_types_;  // type and type alias members
    FlatSet<u64> member_values_;            // names of the other members
    std::vector<u8> progress_;
    BindingId unit_;
};

// Non-generic transparent aliases; associated-type requirements stay deferred.
// Both dependency traversal and wrapper traversal are iterative. Requires a
// successful resolution, and leaves the AST/Resolution unchanged.
[[nodiscard]] AliasExpansion expand_aliases(const Resolution& resolution, const Interner& names, DiagEngine& diag);

struct ScalarSignature {
    std::vector<ScalarType> parameters; // function parameters, or tuple elements
    ScalarType result = ScalarType::Deferred;
    bool generic = false;
};

enum class CallState : u8 { Resolved, Deferred, Error };
// Builtin is a prelude function such as print, whose target is its BuiltinValue
// binding and whose parameter_types hold the selected overload. Clone is the
// built-in `x.clone()`, with no target and `x` as its receiver (3.31). Array
// is a built-in array method, with no target, the array as its receiver and
// the method in `array` (3.33). EnumCase builds a case of an enum from its
// payload, with the case binding as its target (3.34).
enum class CallKind : u8 { Function, NumericConversion, NominalConstructor, IndirectFunction, Method, Builtin, Clone, Array, EnumCase };
// The built-in members of `[T]` (3.33).
enum class ArrayMember : u8 { Count, IsEmpty, Append, RemoveLast };
struct CheckedMember {
    ResolutionState state = ResolutionState::Deferred;
    BindingId owner;
    bool type_receiver = false;
    std::vector<BindingId> candidates; // field, method overloads, or nested type
    ScalarType receiver_type; // concrete instance, including generic arguments when available
};
struct ConformanceWitness {
    BindingId requirement;
    BindingId implementation; // valid only for a Resolved witness
    ExpansionState state = ExpansionState::Deferred;
    ScalarType type; // associated type/field witness, when known
};

struct CheckedBody {
    // Supported generic typing only; initialization, effects, ownership and
    // full control-flow validity are separate. Unknown typing stays deferred.
    ExpansionState state = ExpansionState::Deferred;
};
struct CheckedHierarchy {
    ExpansionState state = ExpansionState::Deferred; // structural base graph
    ExpansionState conformance = ExpansionState::Deferred; // supported override/requirement checks
    BindingId superclass;
    bool dependent = false;
    std::vector<BindingId> bases;
    std::vector<ConformanceWitness> witnesses;
};
enum class ConversionKind : u8 { Widening, Checked, Truncating };
struct CheckedConversion {
    CallState state = CallState::Deferred;
    ConversionKind kind = ConversionKind::Widening;
    ScalarType source;
    ScalarType destination;
};
// Resolved means the argument-based target was selected. Return typing,
// effects, conventions and ownership can still require later checking.
struct CheckedCall {
    CallState state = CallState::Deferred;
    BindingId target;
    // Maps each supplied argument to a parameter; omitted defaults have no entry.
    std::vector<usize> parameters;
    CallKind kind = CallKind::Function;
    const ast::Expr* receiver = nullptr; // explicit instance for a Method call; implicit/static receivers are null
    std::vector<ScalarType> type_arguments; // explicit or inferred arguments of a selected application
    std::vector<ScalarType> parameter_types; // selected signature after receiver/function substitution
    bool inferred_type_arguments = false;
    ScalarType result;  // a call through `?.`: the method's own result, before it is made optional
    ArrayMember array = ArrayMember::Count;  // the method of an Array call
};

struct CheckedConstraint {
    Span span;
    BindingId declaration;
    std::vector<ScalarType> arguments;
    ExpansionState state = ExpansionState::Deferred;
};

class ScalarChecker;

// Owns type facts, without modifying the AST or Resolution. Expression keys
// refer to the input AST; keep it alive. Binding IDs belong to that Resolution.
// Missing/unsupported facts return Deferred. Error marks a diagnosed expression.
class ScalarTypes {
public:
    const CheckedBody* body(BindingId id) const {
        const auto it = bodies_.find(id.value);
        return it == bodies_.end() ? nullptr : &it->second;
    }
    std::span<const CheckedConstraint> constraints() const { return constraints_; }
    ScalarType expression(const ast::Expr* expr) const {
        const auto it = expressions_.find(expr);
        return it == expressions_.end() ? ScalarType::Deferred : it->second;
    }
    ScalarType binding(BindingId id) const { return bindings_.at(id.value); }
    const ScalarSignature* signature(BindingId id) const {
        const auto it = signatures_.find(id.value);
        return it == signatures_.end() ? nullptr : &it->second;
    }
    const CheckedCall* call(const ast::CallExpr* expr) const {
        const auto it = calls_.find(expr);
        return it == calls_.end() ? nullptr : &it->second;
    }
    // The top-level function that a name used as a value refers to, once its
    // type is known; overloads are picked by the expected function type.
    BindingId function(const ast::Expr* expr) const {
        const auto it = functions_.find(expr);
        return it == functions_.end() ? BindingId{} : it->second;
    }
    // The type arguments of a generic function named as a value.
    std::span<const ScalarType> function_arguments(const ast::Expr* expr) const {
        const auto it = function_arguments_.find(expr);
        return it == function_arguments_.end() ? std::span<const ScalarType>{} : it->second;
    }
    const CheckedConversion* conversion(const ast::CallExpr* expr) const {
        const auto it = conversions_.find(expr);
        return it == conversions_.end() ? nullptr : &it->second;
    }
    // Ordinary MemberExpr, implicit member NameExpr and `.case` ImplicitMemberExpr
    // records. Resolution is left unchanged; overloaded method values can still
    // have Deferred types.
    const CheckedMember* member(const ast::Expr* expr) const {
        const auto it = members_.find(expr);
        return it == members_.end() ? nullptr : &it->second;
    }
    // A built-in member of an array, such as `a.count` or the callee of `a.append(x)`.
    std::optional<ArrayMember> array_member(const ast::Expr* expr) const {
        const auto it = array_members_.find(expr);
        return it == array_members_.end() ? std::nullopt : std::optional{it->second};
    }
    // The enum case a case pattern matches, once its scrutinee is known (3.35).
    BindingId pattern_case(const ast::CasePat* pattern) const {
        const auto it = pattern_cases_.find(pattern);
        return it == pattern_cases_.end() ? BindingId{} : it->second;
    }
    // The `match` expressions whose patterns were typed against a known
    // scrutinee, in the order they were typed.
    std::span<const ast::MatchExpr* const> matches() const { return matches_; }
    const ExpandedType* alias(BindingId id) const { return aliases_.alias(id); }
    const CheckedHierarchy* hierarchy(BindingId id) const {
        const auto it = hierarchies_.find(id.value);
        return it == hierarchies_.end() ? nullptr : &it->second;
    }
    const TypeDescriptor* descriptor(ScalarType type) const { return types_.descriptor(type); }
    std::string type_name(ScalarType type, const Resolution& resolution, const Interner& names, usize limit = TypeTable::kNameLimit) const {
        return types_.name(type, resolution, names, limit);
    }
    // For generic instances in code generation: `type` with each type
    // parameter, named by its binding, replaced by its argument. New types
    // are interned, which can move descriptors, so a descriptor pointer read
    // before a call is not used after it.
    using TypeArgument = std::pair<BindingId, ScalarType>;
    ScalarType substitute(ScalarType type, std::span<const TypeArgument> arguments);
    // A nominal type as its own declaration sees it, such as `Box<T>` inside
    // `Box`: its type parameters, by binding, are its arguments.
    ScalarType declared(BindingId declaration, std::span<const BindingId> parameters);
    // A type parameter by its binding; an interface's binding names its `Self`.
    ScalarType parameter(BindingId declaration);

private:
    friend class ScalarChecker;
    NodeMap<const ast::Expr*, ScalarType> expressions_;
    std::vector<ScalarType> bindings_;
    std::unordered_map<usize, ScalarSignature> signatures_;
    NodeMap<const ast::CallExpr*, CheckedCall> calls_;
    std::unordered_map<const ast::CallExpr*, CheckedConversion> conversions_;
    std::unordered_map<const ast::Expr*, BindingId> functions_;
    std::unordered_map<const ast::Expr*, std::vector<ScalarType>> function_arguments_;
    NodeMap<const ast::Expr*, CheckedMember> members_;
    std::unordered_map<const ast::Expr*, ArrayMember> array_members_;
    std::unordered_map<const ast::CasePat*, BindingId> pattern_cases_;
    std::vector<const ast::MatchExpr*> matches_;
    IdMap<CheckedHierarchy> hierarchies_;
    AliasExpansion aliases_;
    TypeTable types_;
    std::vector<CheckedConstraint> constraints_;
    std::unordered_map<usize, CheckedBody> bodies_;
};

// Run only after successful validation and resolution. Checks scalar literals,
// bindings, operators, conditions, defaults, explicit returns and assignments.
// Also infers expression-body returns and checks direct file-level function
// calls, numeric conversions and optional wrappers on built-in facts.
// Includes fully known composites, non-generic nominal identities, arrays,
// primary constructors, nominal/inherited members and known function types.
// Includes concrete generic applications, transparent alias/signature substitution
// and direct members of instantiated owners without bases. Non-generic hierarchy
// graphs and witnesses are separate from dependent contracts. Argument inference
// and supported concrete interface/where constraints retain checked application
// facts. Generic bodies use symbolic declarations once; known associated/Self
// and property contracts retain witnesses. Remaining dependent contracts,
// effects, flow safety and ownership need later passes.
// Dependency evaluation and AST traversal use explicit stacks, including
// forward binding inference. Cyclic inference remains deferred for a later pass.
[[nodiscard]] ScalarTypes check_scalar_types(const ast::File& file, const Resolution& resolution,
                                             const Interner& names, DiagEngine& diag);

}  // namespace phs
