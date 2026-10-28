# 3. Types and semantics

## 3.1 Built-in types [Core]
| Type | Description |
|---|---|
| `Int8 Int16 Int32 Int64`, `UInt8 ... UInt64` | Fixed-width integers. Arithmetic **traps on overflow**; the `&+ &- &*` forms wrap. |
| `Int`, `UInt` | Pointer-sized integers (64-bit on x86_64, 32-bit on i386) |
| `Float32`, `Float64` | IEEE-754 floating point |
| `Bool` | `true` / `false` |
| `Char` | One Unicode scalar value (32-bit) |
| `String` | Owned, immutable UTF-8 text |
| `[T]` | Owned, growable array (`std.Array<T>`). Indexing is bounds-checked. |
| `T?` | Optional value: either `none` or a `T` |
| `()` | `Unit` |
| `Never` | The type of expressions that never return (`panic`, `throw`, infinite `loop`) |

There are **no implicit numeric conversions**. Widening is written
`Int64(x)`, and narrowing uses `Int8.checked(x)` or `Int8.truncating(x)`.

## 3.2 Type kinds
- **`struct`**: a value type with no identity. Fields are immutable unless
  declared `var`.
  - Copying is implicit when every field is `Copy` (automatically derived).
    Otherwise the struct is move-only.
  - `struct S: ~Copy` opts out of copying explicitly.
  - Plain parameters in a struct's primary constructor become `let` fields.
- **`class`**: a reference type with identity, heap-allocated, with dynamic
  dispatch through a vtable.
  - **Final by default.** `open class` allows subclassing, and `open fun`
    allows overriding. An override must say `override`.
  - `abstract class` cannot be instantiated and may declare methods without
    a body.
  - `sealed class` and `sealed interface` can only be subclassed or
    implemented within the same package, so `match` over them can be
    checked for exhaustiveness.
  - Only primary-constructor parameters marked `let` or `var` become fields.
- **`enum`**: a sum type. Each case can carry a payload, e.g.
  `BadDigit(at: Int)`.
- **`interface`**: a set of requirements. It can declare default method
  bodies and associated types (`type Item`). A class or struct can implement
  any number of interfaces.
- **Tuple**: `(A, B, ...)`, an unlabeled value type of two or more elements,
  with structural identity (3.28).
- **`actor`**: a reference type whose mutable state is isolated. See ch. 5.
  Primary-constructor parameters marked `let` or `var` become fields, as in
  classes; plain parameters do not.

## 3.3 Mutability [Core]
- `let` creates an immutable binding and `var` a mutable one. Fields follow
  the same rule.
- A method can only mutate `self` if it is declared `mut fun`. Which
  receivers may call it depends on the kind of type:
  - **Value types** (`struct`, `enum`): the receiver must be a mutable place,
    i.e. a `var` binding, a `var` field of a mutable place, an `inout`
    parameter, or `self` inside another `mut fun`. A `let`-bound value can
    never be mutated.
  - **Class instances**: the binding's `let`/`var` only controls whether the
    binding can be reassigned. A `mut` method may be called through any
    reference that is not read-only: an owning binding (`let` or `var`), a
    `shared` handle, an `inout` or `own` parameter, or `self` inside another
    `mut fun`. It may **not** be called through a `borrow` parameter
    (`borrow` is the default when a parameter has no convention) or through
    `self` inside a non-`mut` method, because a borrow grants read-only
    access (3.4).
- Immutability is **deep for value types**. For class references it is
  shallow: a `let` binding holding a class reference cannot be reassigned,
  but the object can still be mutated through its `mut` methods (by the rule
  above) if its fields are `var`.
- A type is **deeply immutable** when no state reachable from it can ever
  change. The definition is recursive:
  - primitive types, `String`, `()`, and `Never`;
  - `T?` and `[T]` when `T` is deeply immutable;
  - value types whose fields all have deeply immutable types;
  - `shared T` when `T` is deeply immutable;
  - classes that are **not `open`**, whose stored fields, including
    inherited ones, are all `let` and all have deeply immutable types.

  `var` fields, `weak T`, raw pointers, and `open` classes are never deeply
  immutable; an `open` class is excluded because a subclass could add mutable
  state. Recursive types are checked coinductively, so a class may refer to
  itself through a `let` field, for example `let next: shared Node?`.
  Deeply immutable types are automatically `Sendable` (ch. 5).

## 3.4 Ownership, borrowing, and destruction
- **Ownership.**
  - Every value has exactly one owner.
  - Assigning a non-`Copy` value, or passing it to an `own` parameter,
    **moves** it. Using a moved-from binding is a compile-time error
    (3.30). `x.clone()` makes an independent copy instead (3.31).
  - Class instances created with `ClassName(...)` are uniquely owned.
- **Borrowing.**
  - Parameters are `borrow` by default: read-only access for the duration of
    the call. `inout` gives exclusive read/write access for the duration of
    the call.
  - Borrows are **second-class**. They cannot be stored in fields, returned,
    or captured by closures, so no lifetime annotations are needed.
  - Closures capture by **copy** (3.29). A closure can capture a borrowed
    parameter only when its type is `Copy`, since it keeps a copy rather
    than the borrow, and it can never capture an `inout` parameter. An `own`
    parameter or a local is moved into the closure when its type is not
    `Copy`.
  - While an `inout` borrow is active, its source cannot be accessed any
    other way (the law of exclusivity, 3.32).
- **Sharing.**
  - `shared T` is a reference-counted handle; `shared expr` converts an owned
    value into a shared one.
  - `weak T` is a non-owning reference that can become `none`.
  - The compiler uses non-atomic reference counting unless a value is used
    across tasks or actors (see `Sendable`).
- **Destruction.**
  - A value is destroyed when its owner goes out of scope without having been
    moved, or when the last `shared` handle is released.
  - Destruction runs the type's `deinit`, then destroys the fields in
    reverse declaration order, then runs the superclass destructor chain.
  - A `deinit` cannot throw and cannot let `self` escape.

## 3.5 Construction (two-phase initialization)
1. **Phase 1.** The initializer assigns every stored field declared by its
   own class. Reading `self`, calling methods, or letting `self` escape is a
   compile-time error during this phase.
2. **Delegation.** The initializer calls `super(...)`, either implicitly
   through the superclass entry in the type head or explicitly. That
   initializes the superclass fields.
3. **Phase 2.** `self` is fully formed, and methods, including virtual ones,
   may be called.

Definite-initialization analysis enforces this. A virtual call can never
reach an object whose subclass fields are uninitialized.

A failing initializer is declared `init(...) throws(E)`. If it throws, only
the fields that were already initialized are destroyed.

## 3.6 Optionals and absence of null [Core]
- Phantasos has no null. `T?` holds either `none` or a `T`.
- `none` only fits an optional type. Where the expected type is a concrete
  non-optional type, such as `let x: Int = none`, a `none` value (or a branch
  tail that is `none`) is a type mismatch (E0400). An argument that is `none`
  rules out every overload whose parameter there is non-optional (E0411 when
  none is left).
- The ways to use an optional are `if let x = opt`, `opt?.member`,
  `opt ?? fallback`, `match`, and `opt!`. `opt!` panics with a source
  location if the value is `none`.
- `if let x = opt` and `while let x = opt` run their body when `opt` holds a
  value, with `x` bound to that value, one optional level down. An
  annotation names that inner type: `if let x: Int = opt` needs `opt: Int?`.
  The binding is immutable. A `let` condition whose value is not an optional,
  or is `none` itself, is `E0404`.
- `opt ?? fallback` is the value inside `opt`, or `fallback` when `opt` is
  `none`. The fallback has that inner type, or `opt`'s own type, in which
  case the result is still optional: `a ?? b ?? 0` tries `a`, then `b`. A
  `none` fallback takes the inner type when that is optional, and `opt`'s
  type otherwise. A left operand that is not an optional, or is `none`
  itself, is `E0404`.
- `==` and `!=` compare an optional with `none`, with a value of its inner
  type (which lifts), or with another optional of the same type. Stage0
  checks this when the inner type is a built-in scalar or `String`; for
  tuples, structs and other composites the comparison stays deferred, and
  neither operand takes the other's type.
- `none` takes the optional type it fills: the expected type, or the other
  operand's type in a comparison.
- **Smart casts.** After `if x is Circle`, the name `x` has type `Circle`
  inside the branch, as long as `x` is an immutable binding.

## 3.7 Errors [Core]
- `fun f(): T throws(E)` means `f` either returns a `T` or throws an `E`. A
  bare `throws` means `throws(any Error)`. A function with no `throws` clause
  cannot throw (it is `throws(Never)`).
- Every call to a throwing function must be marked with `try`, which
  propagates the error, or `try?`, which turns it into an optional. The
  compiler checks that each propagated error type converts to the caller's
  error type.
- Handling an error at the call site (`expr catch e { ... }`) is part of the
  full language; its grammar is still to be specified.
- Lowering: errors are returned as a tagged result. There is **no
  unwinding** and no exception tables.
- **Panics** (failed bounds checks, overflow, `!` on `none`, explicit
  `panic()`) are not catchable. They print a message and a backtrace, then
  abort the process.

## 3.8 Visibility [Core]
| Modifier | Visible to |
|---|---|
| `private` | the enclosing type (and its extensions in the same file) |
| `file` | the same source file |
| `internal` *(default)* | the same package |
| `public` | importers of the package |

A declaration is never more visible than its enclosing type. A public
signature cannot mention less-visible types.

## 3.9 Privileged internals
- The module `phantasos.internal` provides compiler intrinsics, raw
  syscalls, runtime hooks, and unchecked operations.
- Importing it is a **hard error** unless the importing package is
  privileged. The privileged packages are `std`, `rt`, and `libc-compat`:
  they ship in the toolchain directory, and their content hash must match
  the toolchain manifest.
- Internal items are mangled with the reserved `_P$` prefix and emitted with
  hidden visibility.
- Re-exporting internal items, or exposing them through public generic
  signatures, is rejected.

## 3.10 Unsafe code
- Raw pointers (`*T`, `*mut T`), `extern "C"` calls, and unchecked
  intrinsics are only allowed inside `unsafe { }` or in an `unsafe fun`.
  Calling an `unsafe fun` requires an `unsafe` block.
- Outside `unsafe`, a program never has undefined behavior.

## 3.11 Generics
- Type parameters are bounded by interfaces: `<T: Comparable & Hashable>`,
  or with a `where` clause.
- Generic code is checked once, against its bounds (not after
  instantiation), and then **monomorphized**.
- The full language may share generic code between class-typed
  instantiations.

## 3.12 Binding and control-flow constraints [Core]

- Parameter prefixes `let` and `var` are allowed only in primary
  constructors. Ordinary functions, methods, extern functions and secondary
  `init(...)` declarations cannot use them; stage0 reports `E0207`.
- Names must be unique within each parameter list, generic parameter list,
  or block's local bindings. A nested scope may shadow an outer binding;
  the function body is a nested scope of its parameters. Enum payload
  labels must be unique within a case. Named non-function value declarations
  cannot repeat in one declaration list, nor can type declarations or aliases.
  These checks do not determine which function overloads are valid.
  Promoted primary-constructor fields share the member value namespace:
  `let`/`var` parameters in classes and actors, and all parameters in structs,
  must not collide with explicit member value declarations.
- A name may be bound only once within a pattern. All alternatives of an
  alternative pattern must bind the same set of names, so the arm's guard
  and body can refer to those names regardless of the chosen alternative.
  For example, `.A(x) | .B(x)` is valid, while `.A(x) | .B(y)` is not.
- `init` and `deinit` must be type members. A type has at most one primary
  `init` body (the form without a parameter list) and one `deinit` body.
  Secondary constructor overloads are checked separately.
- `return` requires an enclosing function, initializer or destructor body.
  `break` and `continue` require an enclosing loop **body** in that callable;
  a loop's own condition or iterable expression is outside its body.
- An explicit `throw` must be handled by an enclosing `try?` expression or
  propagated from a function/initializer with a `throws` clause. A `try?`
  expression handles errors only from its operand, not later statements.
  Error-type compatibility and throwing-call checks still follow section 3.7.
- `await` requires an `async` function body. Defaults, attributes, field
  initializers and superclass arguments do not inherit the function body's
  async or throwing context. See chapter 5 for further concurrency rules.
- `defer` schedules synchronous cleanup on leaving the enclosing scope, in
  reverse registration order. Its body cannot `return`, jump to an enclosing
  loop outside the cleanup, propagate an error, or suspend with `await`.
  Loops nested inside a `defer` may use `break`/`continue`, and `try?` inside
  it may handle local errors. Each nested `defer` starts a fresh cleanup
  boundary. Throwing-call validation remains part of later effect analysis.

Stage0 validates these structural constraints and the type-alias forms in
chapter 2 before running the name resolver below. Initial typing and hierarchy
checks follow in 3.14–3.23, flow checks in 3.24, the stage0 code generation
subset in 3.25 and evaluation order in 3.26; remaining typing, visibility,
ownership and effect analysis remain on the [roadmap](../roadmap.md).

## 3.13 Lexical name resolution [Core]

- Types and values have separate namespaces. Type declarations, aliases and
  generic parameters bind type names; functions, variables, fields, enum
  cases, parameters and patterns bind value names. In an expression, lookup
  checks the value namespace first, then the type namespace at each scope
  (for constructor calls and type-qualified members), before visiting the
  parent scope. Type expressions only search the type namespace.
- File and type-member declarations are visible throughout their declaration
  list, including before their definition. `extern` blocks add functions to
  the surrounding scope. Functions sharing a name form an overload set;
  mixing functions with other values in the same scope is an error. Type
  and value declarations may share a spelling. Choosing an overload and
  validating its signature belong to type checking.
- All generic parameters are visible in their bounds, `where` clauses,
  signatures and bodies. A nested declaration may shadow outer generic
  parameters. Type aliases have their own generic scope; associated types
  are visible to other members of their interface.
- Parameters are visible in the body. A default expression sees earlier
  parameters, plus the enclosing declaration scope, but not its own or later
  parameters. Primary-constructor defaults see type generics and the outer
  scope, not member fields. Primary-constructor parameters are visible to
  superclass arguments, instance field initializers and the primary `init`
  body; only promoted fields are visible to other members. Secondary `init`
  bodies have their own parameter scope.
- A local binding becomes visible after its initializer. Thus `let x = x`
  uses an outer `x`, or reports an unknown name if none exists. A block adds
  a nested scope, so locals can shadow parameters and outer locals. A
  `defer` body resolves names where it is declared; later locals are not
  visible inside it.
- `if let` and `while let` bindings are visible only in their body, not in
  the condition or the `else` branch. A `for` binding is visible in its body,
  not its iterable. Each match arm has its own scope: its pattern bindings
  are visible in the guard and body. Matching alternatives share bindings;
  case names and pattern types are looked up outside the arm's bindings.
- A closure's parameters are visible in its body, which is a nested scope
  like a block's. A local, parameter or pattern binding declared outside the
  closure and named inside it is a **capture** of that closure, and of every
  closure between the name and its declaration (3.29). File-level names,
  fields and functions are not captures.
- `Self` denotes the enclosing type. `self` requires an instance function,
  initializer or destructor body; `super` additionally requires a declared
  base type. Static bodies and parameter defaults do not have a receiver.
  Static/instance access and initial inheritance checks run in the type pass
  (3.19–3.20). Initialization safety remains later work.

Stage0 retains immutable scope, declaration, overload-set and reference
records separately from the AST. References are explicitly resolved,
unresolved (with a diagnostic), or deferred. It resolves directly declared
nested types and type-qualified values. Receiver-dependent members, implicit
`.Case` names, alias-qualified names and dependent generic members such as
`T.Element` are deferred until type information is available. Inherited
lookup is also deferred, including an unqualified name that a base could
provide before an outer scope is searched. A type's own generic parameters
take precedence over that inherited lookup. Lexical lookup alone cannot
select inherited members or check visibility. Non-generic alias expansion and
cycles are checked by the type pass (3.17); initialization cycles remain pending.

The bootstrap prelude supplies type names `Int8/16/32/64`, `UInt8/16/32/64`,
`Int`, `UInt`, `Float32/64`, `Bool`, `Char`, `String`, `Unit`, `Never`, `Error`,
`Copy` and `Sendable`, plus the value names `panic` and `print`. Except for
`print`, whose calls are checked as in 3.27, these are name bindings, not
implemented runtime definitions or checked signatures. User declarations may
shadow them. Other library names must be declared explicitly.

The current API resolves one file. Package discovery, shared module scopes
and imported declarations belong to P2. `check` reports `E0304` for imports;
`parse` still accepts their syntax. Attribute names, argument keys and call
argument labels are metadata, while attribute argument values undergo the
same expression lookup as other expressions.

## 3.14 Scalar typing and bootstrap coverage [Core]

Integer literals default to `Int`, and floating-point literals to `Float64`.
An explicit scalar annotation or a concrete operator operand constrains numeric
literals, including parenthesized expressions and trees of numeric operators.
An integer literal takes an integer type; a floating-point literal takes a
floating-point type. A literal's magnitude must fit its chosen type; a directly
negated integer literal (possibly parenthesized) may use the signed minimum's
magnitude. `Float32` literals must fit its finite range. Stage0 checks range,
not exact floating-point representability. Arithmetic overflow remains a
runtime trap; this pass does not fold constant expressions.

Unannotated variables infer their initializer's type, then default any remaining
numeric literals. That type is fixed before uses in other expressions: a binding
inferred as `Int` cannot be assigned to `Int64` implicitly, even on x86_64.
`Int`/`UInt` remain distinct from `Int64`/`UInt64`; stage0's initial Windows x64
and Linux x86_64 targets use 64-bit ranges for them. User types that shadow
built-in names retain their own identity.

Discarded numeric expressions also default and undergo range checking. These
include expression statements before a block's final statement, and all
expression statements in ordinary function, initializer, destructor, loop and
cleanup bodies. If a block's result is used, its final expression takes the
expected type when one is known (3.24) and otherwise retains its deferred
contextual type. Discarding a call does not default its arguments.

Scalar arithmetic (`+ - * /`) requires operands of the same numeric type.
Remainder, wrapping arithmetic, shifts and bitwise operators require the same
integer type. Unary `-` requires signed numeric operands, and `~` requires an
integer. `!`, `&&`, `||`, ordinary `if`/`while` conditions and match guards
require `Bool`. Scalar equality accepts matching built-in types; ordering
accepts matching numeric types or `Char`. There are no implicit numeric
conversions between typed operands. Compound assignment follows the same
numeric/integer domains and requires the target's type.
Boolean, numeric, integer and ordering requirements are checked independently
for each available operand, even when the other operand's type is deferred.
Compound assignment checks both the target and source domains before type
compatibility. Matching operand types and inferring results can still remain
deferred.

Scalar variable annotations, parameter defaults, assignments and explicit return
values must match their expected type. A promoted primary-constructor field
retains its parameter's declared type in a separate field binding.
A block-bodied function without a return annotation returns `Unit`;
an expression-bodied function infers its result from the body, defaulting
numeric literals at the function boundary.
An explicit bare `return` produces `Unit`, with the same optional lifting as
`return ()`. Initializers and destructors also
return `Unit`. Section 3.24 checks that every path of a block body returns.

After successful validation and name resolution, stage0 checks these scalar
rules and retains expression/binding types. `E0400` reports mismatches,
`E0401` invalid operator domains, and `E0402` out-of-range literals. A type error
does not alter the syntax tree, and dependent expressions avoid cascading
diagnostics. Parsing alone performs none of these checks.

The type API explicitly returns `Deferred` for unsupported calls, unselected overloaded function values,
generic types and aliases, inherited/dependent members (3.19), optional
bindings, casts, effect operators and block/branch results. It traverses their
children to check supported scalar subexpressions. Forward variable references
can infer scalar types. Numeric literals in deferred contextual positions keep
deferred types instead of defaulting prematurely (for example, an unchecked
call argument could require `UInt64` rather than `Int`). Cyclic inference
without an established type remains
deferred. Generic calls, function-value inference, remaining numeric policies, initialization cycles,
ownership and effects still need later passes; all-path returns, local
initialization and binding mutability follow 3.24.
Successful `check` therefore continues to mean only that implemented checks
found no errors, not that the program is valid or executable.

## 3.15 Direct scalar function calls [Core]

Stage0 retains scalar parameter and return types for function declarations,
including extern functions. An omitted return annotation means `Unit` for a block body or declaration
without a body. Expression bodies infer their scalar result, including forward
calls and references. Recursive inference cycles without an established type
remain deferred; an explicit return annotation can break such a cycle.
Calls select targets from argument facts before adding return dependencies.
Only the selected target's return type participates, so eliminated overloads
cannot create artificial inference cycles.

Direct calls to file-level functions check supplied arguments against the
declared parameters. Positional arguments form a prefix in declaration order;
named arguments follow them and may be reordered. Labels name parameters.
A parameter may be supplied only once. Every omitted parameter must have a
default; defaults are checked once in their declaration's scope. A selected
call retains a function binding ID and the parameter index of each supplied
argument. Default insertion/evaluation is left to lowering.

Overload selection considers argument shape and types, without using the
expected result type. Typed scalar arguments require exact types, with no
implicit numeric conversions; `Never` can stand in for an expected type.
Numeric literal trees take a matching integer/float parameter type when their
literals fit its range. Among fully known candidates, prefer the fewest literal
types differing from the defaults `Int` and `Float64`. Remaining ties are
ambiguous; declaration order and return types never break a tie. Speculative
probes do not emit diagnostics or change literal types. Only the selected
candidate contextualizes its arguments. If a unique otherwise matching
signature has a range failure, report the literal's precise range/operator
diagnostic.

A potentially matching dependent generic/constrained signature, unsupported parameter type or pending
argument type keeps selection deferred, even if another scalar candidate fits.
Independently provable shape/type mismatches still eliminate candidates. Calls
through concrete explicit generic instantiations follow 3.21. Direct nominal methods and
type-qualified calls follow 3.19.
Known function-typed calls and primary constructors follow 3.18. Built-in numeric conversions follow section 3.16. The pass checks
their supported scalar children. Constraints, effects, conventions, visibility, duplicate
signature validity require later checks; all-path returns follow 3.24.

`E0410` reports calling a known scalar value, `E0411` no matching signature,
and `E0412` an ambiguous call. Candidate notes point to declarations. Return
types, call states and selected targets are cached with other scalar facts;
failed calls suppress dependent type diagnostics. These checks preserve the AST
and use iterative dependency traversal, including long forward call chains.

## 3.16 Explicit numeric conversions and optional facts [Core]

The numeric conversion APIs require exactly one positional argument. The callee
must resolve to a built-in numeric type; declarations that shadow a built-in
name keep their own meaning. Aliases expanded to plain built-in numeric types
can name the same intrinsics. Conversion type checking does not execute conversions or fold their
values; execution and lowering remain future compiler work.

- `T(value)` is an identity or lossless widening conversion. Integers of the
  same signedness can widen to equal or greater width; unsigned integers can
  widen to signed integers only at strictly greater width. Signed integers
  cannot widen to unsigned integers. `Int`/`UInt` use the initial x86_64 ranges
  but retain distinct identities from `Int64`/`UInt64`.
- Floating identity and `Float32` to `Float64` widening are supported.
- A numeric literal tree in a widening constructor takes the destination's
  matching integer/float type and must fit that range. This is literal typing,
  not narrowing an already typed value.
- `T.checked(value)` for integers returns `T?`: the converted value on success,
  or `none` when the source value is outside `T`'s range.
- `T.truncating(value)` for integers returns `T`: reduce the value modulo
  `2^N`, then interpret the bits using the destination's signedness, where `N`
  is its width. Checked and truncating operations accept any integer source
  width/signedness. Their numeric literal inputs default to `Int`; an explicit
  source constructor such as `UInt64(...)` can supply a wider literal value.

Optional wrappers on built-in types retain their full identity in type facts,
including signatures and inferred checked results. A value of `T` can be lifted
to `T?` (and deeper wrappers) in an expected optional context; this does not
permit numeric conversions between different underlying built-ins. Calls prefer
fewer optional lifts after numeric literal preferences. Checked results cannot
be used as ordinary numeric values: unwrap a known optional with `!` before
numeric use. Unwrapping retains its element type; runtime failure handling is
defined in section 3.6, which also covers `none`, `let` conditions, `??`
and optional equality. Optional chaining (`?.`) remains a later check.

Integer/floating interchange and floating narrowing remain explicitly deferred
until their rounding and failure policies are specified. Pending source types
also keep conversion results deferred, while independently invalid shape or
known non-numeric/optional operands are diagnosed. `E0420` reports conversion
argument shape, `E0421` invalid operands, and `E0422` a typed narrowing request in
a widening constructor. Existing literal range/operator diagnostics still apply.

Conversion records retain the mode, source/destination types and checked state.
Their call targets identify built-in types rather than functions; they introduce
no function-return dependency. All facts remain cached and the AST is unchanged.

## 3.17 Transparent alias expansion [Core]

A non-generic `type Name = Target` is transparent: it does not create a new
type identity. Stage0 expands chains in their declaration scopes using retained
lexical bindings. Forward declarations and aliases nested in nominal types are
supported. A use-site binding with the same spelling cannot capture names in
an alias's definition.

Expansion retains the terminal built-in or non-generic nominal declaration ID.
Built-in facts and optional depths pass through aliases unchanged; aliases to
the same type are identical for scalar annotations, calls and overloads.
Nominal targets also retain optional wrappers. Their canonical declaration IDs
are used for initial nominal checking (3.18). Aliases naming plain built-in numeric types support the
conversion APIs in 3.16 and use the canonical built-in as the intrinsic target.
Value bindings/functions still take precedence over type names at value uses.

Known direct type members can be looked up through a nominal alias, including
another alias. This lookup respects separate type/value namespaces and does not
rewrite the lexical resolver's deferred references. Missing direct members of
a non-inheriting nominal owner report `E0301`; known inherited paths are checked
in 3.20, while dependent lookups remain deferred. Annotation diagnostics are cached by syntax node, so generated
parameter/field pairs do not duplicate them.

Transparent aliases must be acyclic. `E0430` reports an expansion cycle with
notes on its path, including cycles through optional, array, pointer, function
and generic-argument syntax, or through a known alias-qualified member. Only
non-generic transparent definitions participate in this initial cycle pass.
Alias-qualified dependency discovery examines nested paths in all of these
forms, even when the composite expansion result remains deferred.
Dependent aliases retain an error fact without additional cycle diagnostics.
Alias definition errors gate value checking for that file while retaining alias
records; `parse` remains syntax-only. Traversal uses explicit work stacks.

Concrete generic aliases/instantiation follow 3.21. Captured generic types and associated requirements
and their overrideable defaults remain deferred. Fully known composite targets
normalize into canonical type descriptors (3.18).
These forms are not unconditional transparent expansion edges in this pass.
Unsupported forms still retain their declarations for later type checking.

## 3.18 Initial composite and nominal type checking [Core]

Type facts use a result-local canonical descriptor table for arrays, pointers,
`shared`/`weak` wrappers, function signatures and non-generic nominal instances.
Descriptors refer to other type facts by ID, not recursive owning objects.
Aliases reuse the same identities. Nominal identity is the canonical declaration
ID, including its enclosing scope; equal spellings in different scopes do not
create equal types. Optional wrappers preserve descriptor identity. Generic
instances follow 3.21; types capturing an enclosing generic owner remain deferred.

Fully known composite annotations, parameters and return types participate in
exact matching and optional lifting. Arrays are invariant; pointer mutability
is part of identity. Function descriptors include parameter/result/error types
and the throws flag. Nominal inheritance/conformance follows 3.20. Function
variance/effect coercions and ownership-wrapper conversions remain deferred;
they are not assumed to succeed or treated as proven mismatches. Unrelated
concrete nominal types and scalar-to-concrete-nominal mismatches are rejected.
Function compatibility compares parameters contravariantly and results
covariantly before ruling out a possible conversion.

Array literals take an expected `[T]` context when available, including nested
and parenthesized literals. Otherwise a concrete element can anchor inference.
Concrete anchors propagate through nested literals and take priority over
numeric defaults from siblings. Numeric literals default to `Int`/`Float64`.
Element types must match without implicit numeric conversion. `Never` elements
do not anchor inference. Empty and `Never`-only nested literals can take context
from a sibling; they do not
suppress errors in that sibling. Without an anchor or expected type, the array
remains deferred. Unresolved concrete element types also remain deferred.
Overload probes inspect elements without changing them,
and only the selected signature contextualizes its literal arguments.
Array equality remains deferred, but independently inferred literal operands
still undergo element validation.

Indexing a known non-optional array requires an integer index (or `Never`) and
produces its element type. Invalid array/index combinations report `E0440`.
This does not perform constant bounds evaluation; runtime bounds checks and
lowering are pending. String, pointer and nominal indexing remain later work.

Non-generic classes, structs and actors check primary constructor arguments,
labels and defaults using the same type rules as function calls. The result is
the nominal instance type. Abstract classes and interfaces cannot be constructed
directly (`E0441`). Secondary constructor overloads, enum cases, constraints,
initialization ordering, ownership and construction effects need later checks.
Independently provable constructor shape/type/range errors are still diagnosed.

Known non-optional function-typed values check positional argument count and
types and retain the declared result type; such signatures have no argument
labels or defaults. Direct named calls still select from argument facts before
traversing only the chosen return dependency.

A top-level function's name used other than as a direct callee is a function
value. Its type is `fun(P1, ..., Pn) -> R`, from the declared parameter types
and the declared or inferred result, with the function's `throws` clause;
labels and defaults are dropped, so calls through the value are positional.
When the name is overloaded, the expected type picks the overload whose
function type is exactly that type, as in `let f: fun(Int8) -> Int8 = neg` or
an argument for a function-typed parameter. An overload whose known parameter
types already differ is ruled out even while its result is still being
inferred. `E0400` reports an expected function type that no overload has, and a
single function whose type differs from the expected one. Function values drop
labels, so overloads that differ only in labels can share a function type; the
expected type cannot choose between them, and `E0412` reports the ambiguity.
Without an expected type, an overloaded name stays deferred.

A generic function without bounds, `where` clause or `throws` is a value too.
Its type arguments come from explicit arguments, as in `identity<Int>`, or
from the expected function type, which must match its parameter and result
types with the type parameters replaced, as in
`let f: fun(Int) -> Int = identity` or `apply(identity, 1)` for
`apply<T>(f: fun(T) -> T, x: T)`. Among generic overloads, the expected
type picks the one it matches, and explicit arguments must agree with it,
as in `let f: fun(Int) -> Int = pick<Int>`. The wrong number of explicit arguments is
`E0470`; a generic function whose type cannot match the expected one is
`E0400`. Without either, its name stays deferred. Methods, functions with a
`where` clause or bounds and functions with an `inout` or `own` parameter do
not have plain function values yet; their names stay deferred too.

Call records distinguish direct functions, numeric intrinsics, nominal primary
constructors and typed indirect functions. The latter two finish without adding
function-return dependencies. Indirect calls may identify a parameter/variable
binding, or have no static binding ID for a computed callee. Type normalization,
formatting, literal materialization and matching use explicit work stacks,
including across long alias chains. Matching visits each directed descriptor
pair once per comparison. Completed type/descriptor facts are cached.
Type-name formatting also bounds expansion work and output: public names use
a default 32 KiB limit, and diagnostic names use 1 KiB per type. Incomplete
names end in `...`; smaller explicit API limits shorten that marker as needed.
This prevents shared descriptor graphs from producing exponential diagnostics.
Descriptor pointers are stable after checking, but not across mutable interning.

Receiver-dependent field/method lookup follows 3.19–3.20. Generic conformance,
smart casts, effects, unsafe checking and remaining optional operations are pending.
Passing `check` continues to establish only the implemented checks.

## 3.19 Initial receiver-dependent members [Core]

Known non-optional, non-generic nominal receivers support directly declared
fields and methods. Promoted primary-constructor fields keep their separate
binding identities and declared types. Explicit field initializers can infer
their types. Field reads, chains and assignment type requirements use those
facts; initialization, mutable-place legality and ownership remain later checks.

Instance members require an instance receiver. Static fields/methods and nested
types require a type receiver, including a transparent alias or `Self`.
Unqualified instance fields/methods use the enclosing implicit receiver when
available; static bodies and parameter defaults have no implicit instance.
Instance field initializers can refer to fields without making explicit `self`
available. Unqualified static members are available in either kind of body.
Values take precedence over nested types with the same spelling.

Method calls use the direct-call argument, label, default, overload and literal
range rules. Only the selected method adds a return-inference dependency;
rejected overloads cannot create inference cycles. A field of function type
uses the positional indirect-call rules. Bound method values remain deferred;
concrete explicit generic method applications follow 3.21.

`E0450` reports a missing receiver member when lookup is complete. `E0451`
reports an incompatible static/instance receiver. Lexical type-qualified name
errors retain the resolver's diagnostics. Known inherited lookup and `super`
follow 3.20. Concrete generic owners without bases follow 3.21. Captured generic
types, generic inheritance, optional chaining, ownership-wrapper
and built-in members, visibility, effects and smart casts still require later
checking. Direct field facts of a derived owner can still be used.

`ScalarTypes::member` retains the owner, receiver kind and field/method/nested
type candidates for ordinary member expressions and implicit member names.
Lexical reference states and the AST remain unchanged. `CallKind::Method`
records retain selected targets and explicit instance receiver expressions;
implicit/static receivers have no explicit expression. Lookups, entity caching
and selected dependencies use explicit stacks, including long member/call chains.

## 3.20 Initial inheritance and conformance checking [Core]

Stage0 builds a canonical, file-local graph for non-generic nominal declarations.
Classes may have one class superclass and any number of interfaces; interfaces
extend interfaces, and structs, enums and actors may declare interface bases.
A superclass must be `open`, `abstract` or `sealed`, and cannot be `final`.
Sealed bases in this file are in the same package. Base arguments belong only
to a superclass. Optional/scalar bases, repeated canonical bases, invalid type
kinds and multiple class superclasses report `E0460`. Cycles report `E0461`
with declaration notes. Invalid/incomplete bases propagate to descendants.

Known inherited fields, nested types, aliases and method overloads participate
in lookup. More-derived declarations hide matching inherited declarations;
method input profiles include labels, parameter types and static/instance kind.
Different profiles remain overloads. Diamonds share declaration identities.
Class implementations take precedence over interface defaults, and a more
specific interface declaration hides all of its ancestor declarations. Unrelated
defaults remain ambiguous. Ambiguous non-method members report `E0452`.
Specificity is applied before default-body preference, including through cached
intermediate bases: a bodyless redeclaration still suppresses its ancestor's
default when an unrelated interface supplies the selected body. Base-list order
does not change this suppression.

The type pass retries lexical references deferred at an inheritance barrier.
It searches outer scopes only after complete inherited lookup finds nothing;
thus inherited names still shadow outer names. Type and value namespaces remain
separate. Inherited aliases and newly available base types normalize before
callable signatures; newly exposed alias cycles still report `E0430`. These
operations leave the AST and lexical reference states unchanged.
Qualified type paths cannot traverse an optional alias owner; such paths remain
deferred, including when inherited lookup would otherwise expose a nested type.

Known ancestry permits nominal superclass/interface upcasts and optional
lifting, while downcasts and unrelated known nominal assignments are rejected.
Arrays remain invariant and numeric conversions remain explicit. Overloads
prefer exact argument types; among equal literal/optional ranks, a signature
whose supplied parameter types are uniformly more specific wins. Incomparable
signatures remain ambiguous.
Structurally complete method lookup retains dependent overload candidates.
Calls discard candidates with incompatible argument counts/labels before
deferring selection for applicable generic or otherwise dependent signatures;
an inapplicable generic overload does not suppress checks on a known call.

A class override of an inherited input profile must say `override`, and its
base method must be open/abstract (interface methods are implicitly virtual).
Final methods cannot be overridden. Interface requirement implementations do
not require `override`, but an explicit modifier is checked. New overloads are
not overrides. Known result types may be covariant, throwing implementations
cannot replace non-throwing requirements, and implementations cannot strengthen
a read-only method into a `mut` method. Invalid overrides report `E0462`.

Concrete types must supply a compatible body for each supported interface or
abstract method requirement. A unique inherited default body can satisfy it;
missing, incompatible or ambiguous implementations report `E0463`. Abstract
types can retain unimplemented obligations. Calls still depend on only their
selected method return, including inherited methods and defaults.

`super.member` uses the immediate class superclass. Explicit superclass-head
arguments check primary constructor labels, arity, types and literal ranges.
Secondary constructor selection, implicit delegation, explicit `super(...)`
delegation calls and initialization ordering remain deferred; those calls retain
a known constructor target when available without claiming completed checking.

`ScalarTypes::hierarchy` retains direct bases, the superclass, structural graph
state, conformance state and requirement-to-implementation witnesses. A witness
has an implementation ID only when resolved. Deferred state is not successful
conformance. Generic/captured owners, `where` constraints, associated/property
requirements, interface `Self` substitution, non-default receiver/parameter
conventions and built-in `Copy`/`Sendable`/`Error` contracts need later checking.
Unknown effect/type relations remain deferred; visibility, full effect rules,
ownership, optional members and runtime dispatch/lowering are also pending.
Nominal ancestry facts describe declared relationships; passing `check` still
establishes only the implemented checks.

Graph traversal is iterative. Lookup caches share results along inheritance
chains and deduplicate diamonds; ancestry queries record visited declarations.
Base/type normalization advances only newly known/error facts, preserving
forward-declaration order and avoiding recursive owning type structures.

## 3.21 Initial generic instantiation and substitution [Core]

Stage0 normalizes explicit concrete type applications such as `Box<Int8>` and
transparent aliases such as `type Rows<T> = [[T]]`. Type arguments participate
in nominal identity and are invariant: `Box<Int>` and `Box<Bool>` are different
types even when their declarations are identical. Type names include arguments.
Substitution uses lexical generic binding IDs, not spellings; a method's own
`T` can shadow its owner's `T` without capturing the owner's argument.

Explicit function applications (`identity<Int8>(1)`) and method applications
substitute parameter and annotated return types before ordinary call probing.
Numeric and nested array literals receive context only from the selected
signature, preserving range errors and overload ranking. Explicit type-argument
count first filters overloads; no matching count reports `E0470`. Type/alias
applications with a wrong count or arguments on a non-generic declaration also
report `E0470`. Omitted type arguments use the initial inference rules in 3.22.
Transparent alias qualifiers resolve the terminal declaration before applying
arguments, so `Alias.Box<Int>` has the same identity and arity rules as
`Outer.Box<Int>` when `type Alias = Outer`. Optional/dependent qualifier barriers
still prevent traversal.

Primary constructors and direct fields/methods on concrete generic owners
without bases substitute the receiver's arguments. Member type aliases and
nominal `Self` types use the same environment. A method's explicit arguments
extend its receiver environment. Existing static/instance receiver rules apply.
Member aliases also substitute when used as numeric conversion or constructor
targets, including aliases with their own explicit type arguments.
Calls retain their selected declaration, explicit function type arguments and
substituted parameter types; member records retain the concrete receiver type.
Generic declaration signatures and shared AST/lexical references are unchanged.

Normalization and alias traversal use explicit stacks and cache results per
interned substitution environment. Generic alias cycles report `E0430`, including
cycles whose arguments grow at each expansion. Repeated applications share
canonical descriptors instead of expanding descriptor graphs as trees.

This stage establishes application identities and supported signature facts,
not complete generic program validity. Initial argument inference and concrete
bound/`where` satisfaction follow 3.22. Checking generic bodies once against
their bounds, captured nominal owners, generic base substitution/conformance, associated types and
dependent properties remain pending. Calls with applicable unchecked constraints,
omitted dependent defaults, or non-default conventions on a generic callee
remain deferred; a method's conventions do not defer its calls. Primary
constructors also defer non-default parameter conventions. An omitted
default requires established compatible expression facts in its declaration
environment, even when its parameter annotation is already concrete. Supplied
arguments bypass the default; known defaults work in either declaration order.
Generic expression-body return inference still uses the declaration's available facts;
bodies are not rechecked per application. Function/bound-method values, `none`
arguments for optional or generic parameters (3.6) and lowering/monomorphization need later stages. Unsupported
facts retain `Deferred`; passing `check` still proves only implemented rules.

## 3.22 Initial generic argument inference and constraints [Core]

Calls may omit all function/method type arguments, and nominal primary
constructors may omit their type arguments. Inference uses supplied argument
facts and parameter patterns, including unconstrained transparent aliases,
arrays, optionals, pointers, ownership wrappers, functions and nominal arguments.
Patterns use generic binding identities; shared declaration signatures stay
unchanged. Explicit applications continue to follow 3.21.

Concrete arguments anchor parameters before numeric-only literal arguments
default to `Int`/`Float64`. A concrete `Int8` sibling therefore contextualizes
`same(1, byte)` and `same(byte, 1)` identically. Repeated anchors must agree, allow
optional lifting, or be related by a supported nominal upcast. For related
nominal anchors, compare ancestry without optional wrappers and use
the maximum optional depth: both `same(child, parent)` and the reversed order
infer `Parent?` for a `Child?` and a `Parent`.
Previously unresolved nominal anchors are rechecked against the final widened
substitution. Supplying a base/interface later, as in `same(A(), B(), Parent())`,
can therefore resolve sibling anchors in any argument order, including their
optional depths, without searching for a common ancestor.
General common-ancestor and variance solving remains deferred. Inconsistent
scalar/composite anchors reject the candidate. `Never`, unknown facts and empty literals supply
no anchor. Expected return/binding types and omitted defaults do not infer
arguments. Missing arguments stay deferred; probes apply the final substitution
and only a selected candidate materializes literals.
Non-dependent positions contribute no inference evidence; ordinary probes
decide compatibility there. Protocol conversions without structural inference
evidence retain the probe's deferred result, just as in explicit applications.

Explicit type-argument count and ordinary argument shape filter candidates
before inference. Supported constraints then eliminate inapplicable candidates.
The usual literal/optional ranks and more-specific input types apply. When
supplied instantiated input types are identical, a function with no own generic
parameters takes precedence over an inferred generic function. Remaining ties
are ambiguous; declaration order and return types do not decide them. Constraint
subsumption and specialization ordering between generic profiles remain pending.
Speculative signature substitutions do not register constraint applications.
They use separate annotation caches; only the selected signature commits its
substituted parameter type uses. A later call selecting a previously rejected
signature still validates those uses. Unconditional declaration/type applications
continue to be checked independently. Known unrelated nominal declarations can
reject a candidate before any generic ancestry substitution would be needed.

Concrete interface bounds (`T: Read & Flag`) and `where` clauses check the
substituted source and bound types. A known source must declare the interface
relationship and provide a unique compatible supported implementation of each
required method, including inherited requirements/defaults. Interface and
abstract-class values can rely on their abstract method contracts. Available
implementation return facts participate through iterative dependencies; active
inference cycles stay deferred. Declared ancestry alone is insufficient.

`E0471` reports unsatisfied concrete constraints, including invalid concrete
type/alias applications outside calls. `ScalarTypes::constraints()` retains
application spans, declarations, arguments and resolved/deferred/error states.
Ordinary non-generic nominal annotations also register concrete `where` uses,
including qualified or wrapped annotations; construction is not required.
Validation retains the declaration path for discovered applications. Repeating
a declaration on that path stops further expansion of the recursive application
and retains it as deferred, even when its arguments grow. Its immediate bounds
can still report an independently known failure. Finite nested applications and
independent roots continue to be validated.
Structural type identities remain available independently of constraint validity.
Selected call records retain inferred arguments and an inference flag alongside
the substituted parameter types. Facts remain session-owned and ASTs unchanged.

Initial generic bodies, associated/property requirements and `Self` contracts
follow 3.23. Built-in `Copy`/`Sendable`/`Error` derivation and generic base/interface
substitution remain deferred. Constrained alias normalization and inference through such
aliases, naked generic-alias constructor inference and contextual return
inference also need later work. Concrete constraints
on alias applications are still checked even when their type facts are deferred.
Unresolved applicable constraints keep call selection deferred; type applications
retain their separate constraint state. This initial pass does not establish
ownership, effects, complete generic validity or executable lowering.

## 3.23 Initial generic body checking and dependent conformance [Core]

Stage0 checks supported generic function/method/type bodies once, including
unused declarations.
Parameters have symbolic types keyed by their declaration bindings. A body must
work under its declared interface/`where` bounds; a successful instantiation
cannot repair an invalid definition. Returning an arbitrary `T` as `Int`,
returning `true` as arbitrary `T`, or using an unbounded member reports the usual
type/member diagnostic at the definition. Applications do not rewrite or recheck
the body. Generic defaults are checked against their symbolic parameter types
once; a proven default can then be used by a concrete application.

Well-formed incoming nominal parameter types provide their own bounds as premises
for a callable's own formal parameters. For example, a parameter `Box<T>` where
`Box` requires `T: Read` lets that callable use `Read` on `T`. These premises do
not turn a concrete `Box<Bad>` into a valid type: selected signatures and actual
type applications still validate their substituted constraints. Conditions on
captured owner parameters remain deferred rather than strengthening other bodies.

Member lookup on a symbolic receiver uses its known bounds, including supported
inherited interface requirements. Method signatures substitute the receiver,
interface arguments and method arguments. `Self` in an interface contract denotes
that conforming receiver; class `Self` continues to denote its enclosing class.
Associated paths such as `T.Item` retain canonical symbolic projections. Explicit
conformer type aliases resolve those projections, while associated-type bounds
and supported projected `where` clauses provide further lookup premises.

Symbolic expression-body returns are inferred and substituted through their
descriptor graph at calls, including arrays/functions and associated projections.
`ScalarTypes::signature` exposes the symbolic declaration signature; selected
calls retain their application arguments/parameter types as before. The AST and
lexical references remain unchanged. `body(BindingId)` retains resolved/deferred/
error status for supported generic typing, including generic-owner methods and
interface schemas. It is not a proof of flow, initialization, ownership or effects.

Known non-generic interface contracts can be checked for generic nominal schemas
and their applications. Method contracts substitute `Self` and associated types;
conformance also checks explicit associated aliases (and their bounds), stored
properties, and interface `where` conditions. Writable requirements need writable
implementing fields; property types are invariant. Associated/property witnesses
retain a known type alongside their implementation binding. Default interface
methods can satisfy supported contracts. Abstract types may retain obligations.

Generic base/interface argument substitution, associated defaults, captured
nominal identities, generic-method witness equivalence, receiver-specific context
refinement, and inherited class implementations whose `Self` contract changes
remain pending. Built-in trait derivation and general function-value/variance
inference are also pending. Recursive or deep dependent queries stop at a bounded
depth (64 nested queries) and retain deferred states; independently known failures
still report diagnostics. Casts, inferred branch results without context and
ownership/effect rules remain separate roadmap work. Passing `check` continues
to establish only the implemented rules.

## 3.24 Branch results and flow checks [Core]

**Branch results.** When a block, `if`, `match` or `unsafe` expression is used
where a type is expected (an annotation, an argument, an explicit return or an
expression body with a declared result), each branch must produce that type.
A block produces its final expression statement; a block ending in `return`,
`throw`, `break`, `continue` or a `loop` without a reachable `break` out of it
produces no value on that path; any other block produces `Unit`. Literals in each branch take the expected type.
An `if` without `else` produces no value when its condition is false:

```phantasos
let v: Int? = if debug { 5 }   // Int?: 5, or none when debug is false
let w: Int = if debug { 5 }    // E0403: add an else or expect Int?
if debug { log("x") }          // statement: Unit
```

`else if` chains without a final `else` behave the same way. With an explicit
`else`, both branches lift to the optional as usual. A used branch result
without an expected type remains `Deferred`.

**Flow checks.** After scalar typing succeeds, stage0 checks each function,
initializer and destructor body, and separately each parameter default and
variable initializer. Each closure body is checked as its own callable after
the body that creates it (3.29). Analysis is structural and does not evaluate
conditions: both arms of an `if` and both operands of `&&`, `||` and `??` are
possible paths, `while` and `for` may run zero times, and `loop` exits only
through `break`. A call to `panic` or a call of (non-optional) type `Never`
ends its path. Inside `try?`, a `throw` or any call other than `panic` can
leave the operand early and resume after the `try?` expression. `match` is assumed exhaustive
(exhaustiveness is checked separately); a failed pattern or guard continues
to the next arm with the guard's effects, an arm with a wildcard or binding
pattern always runs its guard, and arms after an unguarded one are unreachable.

- **All-path return (`E0500`).** A block-bodied function whose declared result
  is not `Unit` must reach `return`, `throw` or a diverging call on every path;
  reaching the end of the body is an error. Expression bodies always produce
  their value.
- **Definite initialization (`E0501`).** A local declared without an
  initializer must be assigned on every path before it is read, including by a
  compound assignment. Unreachable code is not checked. A `defer` body is
  checked against the state where it is registered; its assignments to a
  `let` count as possible writes from then on, since they run at scope exit.
- **Immutable bindings (`E0502`).** Assignment and compound assignment may
  target a `var` local or global, or an `inout` parameter. Other parameters,
  `let` locals and globals, pattern and `for` bindings, and `if let` bindings
  are read-only, and so are closure parameters and a closure's captures
  (3.29). A `let` local declared without an initializer may be assigned
  once while it is definitely unassigned, so each path initializes it exactly
  once. Assignment anywhere in a loop's condition or body counts as possibly
  repeated on the next iteration.
- **Fields and receivers (`E0502`).** These follow 3.3. Assigning to a field,
  calling a `mut fun` or passing an `inout` argument needs a mutable place:
  - The field itself must be `var`. A `let` field, or a primary-constructor
    parameter without `var`, can't be assigned outside an initializer. A static
    field follows the same rule and ignores its receiver.
  - For a struct or enum, the receiver must be a `var` local or global, an
    `inout` parameter, `self` inside a `mut fun`, or a `var` field of such a
    place. A `let` binding, any other parameter, a pattern binding or a
    temporary value, such as a call result or an array literal, can't be
    modified. The rule applies to every field along a path like `a.b.c`.
  - Array elements (`a[i]`) and unwrapped optionals (`p!`) keep the access of
    their container, so assigning to an element of a `let` array, or to `p!`
    through a borrow, is also an error.
  - A class or actor instance can be modified through any reference except a
    borrow: a default or `borrow` parameter, or `self` outside a `mut fun`.
    A borrow stays read-only through every field along the path, `let` or
    `var`, whatever the type of each step.
  - Inside a method, a bare field name or method call, including an inherited
    field or an explicitly specialized generic method, uses `self`, and so does
    `super`. `self` is read-only unless the method is a `mut fun`. Assigning to
    `self` itself follows the same rule. In an `init` or `deinit`, fields of
    `self` can always be assigned, since construction rules are separate.
  - An argument for an `inout` parameter must be a mutable place of any type,
    since the callee can replace the whole value.

  Receivers whose storage is not known, such as type parameters and
  interface `self`, are only checked for borrows.

`init` ordering, moves, borrows and destruction remain pending, as do
stored-field initialization and global initialization order.

## 3.25 Stage0 code generation subset

This section describes an implementation limit of stage0, not a language rule.
Stage0's first native backend lowers only a subset of scalars, plain
structs, generic functions and structs, and interfaces used as bounds. The `gate` command
runs `check` first. If `check` succeeds, `gate` reports every construct outside
the subset, so that nothing half-checked reaches code generation:

- **Declarations.** Top-level functions with a body, plain structs with
  their methods, generic ones included, and plain interfaces. A function or method cannot be
  `async`, `unsafe` or throwing, or carry attributes. Its parameters have no defaults
  and are not `inout`; `borrow` and `own` parameters are passed like
  default ones, since stage0 values have no destructors yet. A function
  with an `own` parameter has no function value (3.18). A plain struct
  takes all its fields, at least one, from its primary constructor, without
  conventions or defaults.
  It has no attributes, its conformances name plain interfaces, and its body holds only instance
  methods, `fun` or `mut fun`: no static methods, body fields, `init` or
  `deinit`. It cannot hold itself through its fields, directly, through
  other structs or through their type arguments.
  A plain interface is not generic, extends no other interface and has no
  attributes or `where` clause. Its body holds only instance methods that are
  not generic, with or without a default body. Type parameter bounds may
  name only plain interfaces.
- **Types.** `Bool`, `Int8` to `Int64`, `UInt8` to `UInt64`, `Int`, `UInt`,
  `String`, `Unit` and `Never`, without optional wrappers. Parameters and
  locals need one of the value types, so they cannot be `Unit` or `Never`.
  `String` values come from literals and are only passed around whole: as
  locals, arguments, results and fields. A plain struct whose fields all have
  value types of the subset is a value type too, and so is an instance of a
  generic one whose type arguments are value types of the subset; a struct
  with a field of any other type is rejected along with the field. So is a non-optional
  tuple whose elements all have value types of the subset, and a
  non-optional, non-throwing function type whose parameters have value types
  of the subset and whose result is a type of the subset. A struct cannot
  take or return itself through a function type in its fields either.
  Inside a generic function or struct, its own type parameters are value
  types as well, but not optional ones.
- **Statements.** `let` and `var` locals, with or without an initializer;
  assignment and compound assignment to locals, to `self` in a `mut fun`,
  and to fields of either at any depth, tuple elements included; tuple
  destructuring with `let (a, b) = e` and `var (a, b) = e`; `if`, `while`, `loop`, `break`,
  `continue` and `return`.
- **Expressions.** Integer, Boolean and string literals, `()`, local and
  parameter names, parentheses, direct calls to top-level functions, generic
  ones with explicit or inferred type arguments that are value types of the
  subset (not `Unit` or `Never`), `print`
  (3.27), integer conversions, primary constructors of supported structs,
  reads of their fields, calls to their methods with an explicit or implicit
  receiver, `self` and bare field names inside methods, tuple literals and
  `.0` element reads, top-level functions named as values (3.14), generic
  ones included, with inferred or explicit type arguments, closures
  (3.29) and calls of function values, unary `-`, `!` and `~`, arithmetic including
  the wrapping forms, `&`, `|`, `^`, comparisons, `&&` and `||`. Used `if` and block
  results are allowed where their type is known (3.24).

Everything else reports `E0600`. That includes shifts, `for`, `defer`,
`throw`, `match`, `unsafe` blocks, operators on strings, string interpolation,
floating-point values, arrays, comparisons of tuples,
optionals, classes, enums, values of an interface type, generic
interfaces, interfaces that extend others, `where` clauses, members other
than struct fields and method calls, method values, globals, operators on function values,
and values of `where`-constrained functions. A closure's body is
checked like a function body. A `mut fun` call
needs a receiver rooted in a local, `self` or a field of `self`. A local whose
type is still deferred also reports `E0600` and needs an annotation. One
`E0600` is reported per construct, and uses of a rejected local are not
reported again.

A generic function's body is checked once, with values of its type
parameters opaque, and so are the methods of a generic struct. Code
generation then makes one instance of such a function or method for each
distinct list of type arguments that a call needs, starting from the calls
in non-generic functions and in non-generic methods of non-generic structs
and following the calls inside each instance with its arguments substituted.
A generic function used as a value needs an instance the same way, with the
type arguments the checker found for it (3.14).
A method of a generic struct takes its receiver's type arguments, followed
by its own when it is generic. A generic function or method that no such
chain reaches has no instance, and a generic struct has a layout for each of
its instance types that the program uses. Polymorphic recursion, where an instance calls
the same function with bigger type arguments, would need instances without
end, so stage0 reports `E0602` at the call or value where the instances nest more
than 64 levels deep or where the program would need more than 10,000 of
them. Calls whose type arguments are not inferred yet report `E0600`.

A call on a value whose type is a bounded type parameter, or on `self`
inside an interface's default body, is dispatched statically: once the
instance knows the receiver's struct, the call goes straight to the method
that implements the requirement for it. When that is the interface's default
body, the default gets one instance per conforming struct that uses it, with
`Self` standing for that struct. There are no vtables in stage0.

The program needs exactly one `fun main()` with no parameters or type
parameters. It returns `Int` (the exit status), nothing, or `Never`. A
missing, overloaded or otherwise invalid `main` reports `E0601`.

## 3.26 Evaluation order and arithmetic traps [Core]

Evaluation is strictly left to right. Operands of a binary operator, the
arguments of a call (in source order, even when labels pass them in another
parameter order) and the parts of an expression are evaluated before the
operation that uses them. Compound assignment `x op= e` reads its target
before it evaluates `e`. Assignment to a field of a local struct, such as
`p.a.x = e`, evaluates `e` and then stores into the struct that `p` holds at
that point; `p.a.x op= e` reads `p.a.x` first. A method call evaluates its
receiver before its arguments, except that a `mut fun` receiver is a place:
it is read when the call starts, after the arguments, and the method's
changes reach it when the call returns. A call of a function value evaluates
the function value before its arguments. `&&` and `||` evaluate their right operand only when
the left operand does not decide the result.

Integer `+`, `-`, `*` and unary `-` trap when the result does not fit the
type. `/` and `%` trap on a zero divisor and when the signed minimum is
divided by `-1`. Integer division truncates toward zero, and `%` takes the
sign of its left operand. The `&+`, `&-` and `&*` forms wrap.
`T.truncating(x)` keeps the low bits of `x`. A trap is a panic (3.7).

Stage0 lowers checked programs to a typed MIR that makes this order
explicit. `phantasos-stage0 mir <file>` prints it for a file that passes
`gate` (3.25). A function value in MIR is two words, the code to call and an
environment that the code receives as a hidden first argument; a top-level
function's value has a null environment. The x86_64 backend calls it through a
thunk (abi.md A.6). A closure becomes a MIR function of its own, named after
the function that creates it, a `$closure` suffix and its number there, so
the first closure in `main` is `main$closure0`. It takes its environment
first. Creating the closure allocates the environment and copies each
captured value into it, `self` included when the closure uses the receiver,
laid out like a struct; the closure's function reads them back on entry. A
closure that captures nothing has a null environment. Environments are never
freed in stage0. Each instance of a generic function (3.25) is a MIR function
of its own, named after the function, with its type arguments in angle
brackets, so `identity` called with an `Int` is `identity<Int>` and `pair`
called with `Int` and `(Bool, String)` is `pair<Int, (Bool, String)>`. An
overload's suffix comes before them, as in `pick.1<Int>`, and a closure in an
instance is named after the instance, as in `keep<Bool>$closure0`. A
generic function used as a value refers to the instance for its type
arguments, the one a call with them uses. A method
instance is named after its receiver's type, such as `Box<Int>.get`, with a
generic method's own type arguments after its name, as in
`Box<Int>.with<Bool>`. An interface's default body instantiated for a
struct is named after the interface and that struct, as in
`Shape.twice<Square>`. A type
argument longer than 256 bytes is cut short with `...`, and when that gives
two instances the same name, the later one gets a numeric suffix such as
`.1` after the closing bracket.

Stage0's first x86_64 backend checks every trapping operation inline. A
failed check calls `__phantasos_trap` with the trap code in the first
argument register: 1 for overflow and 2 for division by zero. The signed
minimum divided by `-1` is an overflow. `__phantasos_trap` does not return.
Functions use the System V AMD64 calling convention and get the symbol
`_P<length><name>` for their MIR name, so `fib` becomes `_P3fib` and an
overload named `sub.1` becomes `_P5sub.1`; the instance `identity<Int>`
becomes `_P13identity<Int>`. A method's MIR name is its struct's
name, a dot and its own, so `Point.sum` becomes `_P9Point.sum`. Struct values
use the layout of abi.md A.2 and are passed as A.6 describes. `phantasos-stage0 asm <file>` prints the code as a
listing. These names and codes are a stage0 detail, not a stable ABI.

On Linux x86_64, stage0 links a program into a static ELF executable with a
small runtime of its own and no C library (abi.md A.8). The process exits
with the value `main` returns, which the kernel truncates to its low 8 bits,
or with 0 when `main` returns nothing. A trap writes `panic: arithmetic
overflow` or `panic: division by zero` to standard error and exits with
status 101. When mapping memory for closure environments fails, the program
panics with `panic: out of memory`, trap code 4. Stage0 does not print a
backtrace yet, and it does not detect stack overflow, so recursion that runs
out of stack ends the process with a signal.

## 3.27 Output with `print` [Core]

`print` is a prelude function, so a program needs no import to use it. It
writes one value and a newline to standard output, and its result is `Unit`:

```phantasos
fun print(value: String)
fun print(value: Int)
fun print(value: UInt)
fun print(value: Bool)     // writes true or false
```

The argument is positional or labeled `value:`. An integer literal selects the
`Int` overload. Other integer types convert explicitly, for example
`print(Int(small))`, as they would for any other call (3.15). Any other argument count, label or type is `E0411`.
Integers print in decimal with a leading `-` when negative. A declaration
named `print` shadows the prelude function, as with other prelude names (3.13).

When a write to standard output fails, the program panics (3.7). Once `std`
exists (P2), the prelude `print` forwards to `std.io` with the same signature
and output.

Stage0 generates code for `print` on Linux x86_64: each call writes its bytes
with the `write` system call, retrying after a partial write or `EINTR`. Any
other failure, such as a full disk or a closed descriptor, panics with
`panic: cannot write to standard output` (exit status 101). Writing to a
closed pipe raises `SIGPIPE`, whose default action ends the process first.

## 3.28 Tuples [Core]

A tuple type `(A, B, ...)` has two or more unlabeled elements. Its identity is
structural: two tuple types are the same type exactly when they have the same
number of elements and the same element types in order, wherever they are
written, and a type alias names the same type. Tuples are invariant, so
`(Int8, Int)` and `(Int, Int)` are different types, as are `(Dog, Int)` and
`(Animal, Int)`. Optional wrappers apply to the whole tuple: `(Int, Int)?`.
Tuples are value types, like structs.

A tuple literal `(a, b, ...)` takes its type from context. When it meets an
expected tuple type of the same arity, through any parentheses, each element
is checked against its element type, so literal elements take that type and
other elements may lift into an optional element type:
`let t: (Int8, Int?) = (1, 2)`. Without an expected tuple type, literal
elements take their defaults, so `(1, 2.5)` is `(Int, Float64)`. An expected
tuple type of another arity is `E0400`; so is a tuple where some other type is
expected. A tuple whose element type is still deferred stays deferred. Only
literals are retyped this way: a tuple value must already have the expected
type, apart from optional lifting of the whole tuple. Overload selection and
generic inference look inside tuple types element by element (3.15, 3.22); the
concrete elements of a tuple literal anchor inference before its literal
elements, so `same((byte, 2))` and `same((2, byte))` both infer `Int8`.
Contextual typing of nested literals recurses once per level, which the parser's
nesting limit bounds.

`t.0`, `t.1` and so on read an element; the index is a decimal literal (1.5)
and chains such as `t.0.1` read nested elements. The receiver must be a
non-optional tuple (`E0451`), and an index past the last element is `E0450`.
A literal receiver takes its default type first, so `(1, 2).1` is an `Int`.

`let (a, b) = e` and `var (a, b) = e` bind one local per element of a tuple
`e`, which needs exactly as many elements as there are names (`E0400`
otherwise). An annotation applies to the whole tuple, as in
`let (a, b): (Int8, Bool) = (1, true)`. The names follow the binding keyword:
`let` names cannot be assigned, and `var` names can. Duplicate names are
`E0200`. Labeled tuples, tuple patterns in `match` and nested destructuring
are not part of Core yet. A tuple literal is not a place, so assigning to
several places at once, as in `(a, b) = (b, a)`, is `E0113`.

An element is a place exactly as mutable as the tuple holding it (3.3): in a
`var` tuple, `t.0 = 1` and `t.1.x = 2` assign in place, while through a `let`
tuple, a parameter or a temporary they are `E0502`. Elements of class type
still allow their fields to change through the reference.

Equality and ordering of tuples remain deferred, like other composite
comparisons; arithmetic on tuples is `E0401`. Stage0 generates code for tuples
of the subset (3.25); they use the struct layout and passing of abi.md A.2 and
A.6, with elements in place of fields.

## 3.29 Closures [Core]

A closure `{ x, y -> x + y }` (2.4) is a value of a function type. Its body
is a callable of its own: `return` leaves the closure with its result,
`break` and `continue` cannot leave it (`E0203`), and a closure cannot throw
(`E0204`) or `await` (`E0205`), since closure types are never `throws` or
`async`. Parameter names must be distinct (`E0200`).

**Types.** A closure takes its type from the function type it is expected to
have. The expected type comes from:

- the annotation of the `let` or `var` it initializes, or of the parameter
  whose default it is;
- the declared result of the function whose expression body or `return` it
  is;
- the target of a plain assignment (`f = { ... }`);
- the parameter it is passed to, as a call argument. The callee is a value
  of function type, a non-generic function or method, or the primary
  constructor of a non-generic type, named directly or through a type alias.
  When the callee is an overload set, the overloads that accept the call's
  shape must agree on that parameter's type, and none of them may be generic.
  A single generic function takes the type arguments that the call's other
  arguments infer, apart from closures with unannotated parameters; a type
  parameter that only the result of the expected function type names is
  then inferred from the closure's own result, as in `map(3, { x -> x > 1 })`
  for `map<T, U>(x: T, f: fun(T) -> U): U`;
- the result type of an enclosing closure, for a closure that is that
  closure's final expression or `return` value.

Parentheses around the closure do not matter, and an optional function type
gives the function type inside it. With an expected function type, each
parameter without an annotation takes the expected parameter type, an
annotated parameter must have exactly that type, and the body's value and
every `return` value must have the expected result type, so literals take it:

```phantasos
let wide: fun(Int64) -> Int64 = { x -> x + 1 }   // x: Int64, 1: Int64
fun makeAdder(n: Int): fun(Int) -> Int = { x -> x + n }
apply({ x -> x * 2 }, 3)                          // apply(f: fun(Int) -> Int, x: Int)
```

A closure whose parameters are all annotated needs no expected type. Its
result is then the type that its final expression and its `return` values
agree on, looking through the branches of an `if`, `match` or `unsafe` in
either place, with literals taking their defaults: `{ x: Int -> x * 2 }` is a
`fun(Int) -> Int`, and `{ -> 1.5 }` a `fun() -> Float64`. A body that never
produces a value has result `Never`.

Errors:

- A parameter without an annotation and without an expected function type
  is `E0480`. That includes a closure passed to a generic function whose
  other arguments do not settle the parameter's type, or to overloads that
  disagree on it, and one used in a tuple or array literal or another
  context not listed above.
- An expected type that is not a function type, or a function type with a
  different number of parameters, is `E0400`. So is an annotated parameter
  whose type differs from the expected one, a body or `return` value of the
  wrong type, and, without an expected type, results that disagree.
- A closure never throws, so against an expected function type that
  `throws` its typing stays deferred, like other effect coercions between
  function types (3.18).

**Captures.** A closure captures the outer locals, parameters and pattern
bindings it names (3.13), including those named only by a closure nested
inside it. Capturing copies the value when the closure is created, so later
changes to a `var` do not reach the closure, and the closure can be returned
and stored freely:

```phantasos
var count = 1
let read = { -> count }
count = 2
read()        // 1
```

Inside the closure, a capture is a read-only copy. Assigning to it, to its
fields or elements, or passing it to an `inout` parameter is `E0502`, as it
is for a `let` local (3.24). A captured class reference still allows the
object's `var` fields to change. Closure parameters are read-only too.
Creating a closure reads each capture, so a captured local must be
definitely initialized at that point (`E0501`).

Parameters are borrowed unless they are `own` or `inout` (3.4), and a closure
never holds a borrow:

- capturing an `inout` parameter is `E0503`;
- capturing a borrowed parameter, including a closure parameter, is `E0503`
  unless its type is `Copy`. Declaring the parameter `own` moves it into the
  closure instead;
- `self` is captured when the closure uses it, explicitly or through a bare
  instance field or method name, in a method, initializer, destructor or
  instance field initializer. A call uses it when the overload it selects is
  an instance method. It follows the same rules: in a `mut fun`
  it is `inout` and cannot be captured, and elsewhere its type must be
  `Copy` (`E0503`). The captured `self` is read-only.

Until `Copy` derivation is checked, stage0 decides it structurally: integer,
floating-point, `Bool`, `Char` and `Unit` values, function values and raw
pointers are `Copy`, and so are tuples, optionals and structs whose elements
or stored fields are all `Copy`, and enums whose cases carry no payload. A
generic instance's fields are checked with its arguments in place, so
`Box<Int>` can be `Copy` while `Box<String>` is not.
`String`, arrays, `shared` and `weak` handles, classes, actors, generic type
parameters and enums with payloads are not. Locals and `own` parameters of
other types are moved into the closure (3.30).

Closures cannot be generic, and are compared like other function values:
not at all yet. Stage0 compiles closures (3.25, 3.26): their environments
are allocated when they are created and never freed.

## 3.30 Moves [Core]

A value of a type that is not `Copy` moves when it is consumed, and its old
binding cannot be used again until it is given a new value (3.4). Stage0
decides `Copy` structurally (3.29), so values of unknown types never move.

An expression is consumed when it is:

- the initializer of a `let` or `var` (local, global or field), the value of
  a `let` condition (`if let x = opt`), or the value of a plain assignment;
- the value of a `return`, the expression body of a function with a
  result (declared or inferred), or the last expression of a closure body;
- an argument for an `own` parameter, or for a stored field of a primary
  constructor, including a superclass argument;
- an element of a tuple or array literal, or a value captured by a closure.

Consuming an `if`, `match` or `unsafe` block consumes the result of each
branch, and consuming `a ?? b`, `a!`, `shared a`, `a as T`, `try a` or
`try? a` consumes its operands. Every other read, such as an operand of an operator, a receiver or
an argument for a borrowed parameter, only borrows the value.

Moves are tracked for the callable's locals and its `own` parameters. Using
one after it moved, including moving it again, is `E0504`: "was moved" when
it moved on every path that reaches the use, and "may have been moved" when
it moved on only some of them. A value that moves in a loop body has moved
when the next iteration starts. A `defer` cleanup runs at each exit of its
scope (its end, `break`, `continue`, `return`, `throw`, and an error that
`try` propagates or `try?` catches, which leaves from the call that throws
it), so its moves happen there, and it cannot use a value that moved after
it was registered. Arguments run left to right, so `Box(a, a == "x")` reads
`a` after moving it. A default argument is consumed like the argument it
replaces, and one that moves an earlier `own` parameter may have moved it
for the later defaults and the body. Construction runs the defaults, the
superclass arguments, the instance field initializers in declaration order
and then the primary `init` body, so a constructor parameter that one of
them moves may have moved for the rest. Assigning a `var`, or running
a declaration again, gives the binding a new value. Each moved binding is
reported once until it moves again, and unreachable code reports nothing.

Only a callable's locals and `own` parameters can move. Consuming any other
non-`Copy` value is `E0505`, "cannot move out of", with a note suggesting
`own` or `.clone()` (3.31):

- a parameter that is not `own`: borrowed by default, `borrow`, `inout`, or
  a closure's parameter;
- `self` in an instance method, also viewed as its superclass through
  `super`, and a field of `self`, named bare or through `self`;
- a static field, named bare or through its type, which no callable owns;
- a field of any value, even an owned local: a struct moves whole or not at
  all, so `let n = p.name` is an error and `p.name.clone()` copies it;
- a tuple element (`t.1`) or an array element (`a[i]`);
- inside a closure body, a value it captured, since the closure can be
  called again; `{ -> s.clone() }` returns a copy instead. Creating a nested
  closure that captures it moves it too, so `{ -> { -> s.clone() } }` is an
  error, while cloning it into a local of the outer closure body first is
  not.

Unwrapping (`p!`) and the other forwarding forms above move their operand, so
`let s = p!` on a borrowed `p` is the same error. A capture that is already
`E0503`, such as `self` or a borrowed parameter, is not reported again inside
the closure body. A type made from generic parameters or associated types
counts as `Copy` here, because what it stands for is not known yet. Each
place is reported once, and only where a reachable use would move it. Nothing in stage0's generated code owns memory, so a
move copies the value's words and nothing is destroyed.

## 3.31 Cloning [Core]

`x.clone()`, with no arguments, returns an independent copy of `x` with the
same type. Stage0 provides it until a `Clone` trait exists, and decides
which types clone structurally, like `Copy` (3.29):

- `String`, and every `Copy` type, clones;
- a tuple or optional clones when its parts do;
- a non-generic struct clones when all of its stored fields do.

Arrays, classes, generic parameters, instances of generic structs and
reference types do not clone yet. Calling `clone()` on a value whose type has
none is `E0450`, with a note naming the part that does not clone. A type
that declares its own `clone` member uses it instead, as an ordinary method
call, as does a generic receiver whose bounds declare one; a type name is
not a receiver.

The receiver is only borrowed (3.30), so `take(a.clone())` leaves `a`
usable, and the result is a new value that moves like any other. A literal
receiver takes its default type first, so `7.clone()` is an `Int`. In
stage0's generated code a clone copies the value's words, because nothing
owns heap memory yet (3.30).

## 3.32 Exclusive access [Core]

An `inout` argument, and the receiver of a `mut fun`, has exclusive access
to its place for the whole call (3.4). No other argument of the same call,
and no receiver, may name an overlapping place, whatever its convention.
Doing so is `E0506`, reported at the later of the two:

```phantasos
swap(a, a)          // error: 'a' is used twice in one call
store(p, p.x)       // error: 'p.x' overlaps 'p'
p.shift(p.y)        // error: 'mut fun shift' needs 'p' exclusively
swap(p.x, p.y)      // fine: separate fields
```

A place is a local, a parameter, a global, `self` or a static field,
followed by fields, tuple elements, indexes and `!`. Two places overlap when
they have the same root and one path continues the other, so `p` overlaps
`p.x` but `p.x` and `p.y` do not. Any two indexes of the same array may be
the same element, so `swap(a[i], a[j])` is reported as a possible overlap.
Inside a method, `self`, `super` and bare field names all start at the
receiver, also in an `init` or `deinit`, and an implicit call to a `mut fun`
passes `self`. A static field is its own place, named bare or through any
type expression, such as `Outer.Inner.n` or `G<Int>.n`. The default of an
omitted parameter takes part in the call too, with an earlier parameter in
it standing for that parameter's argument, so `f(g)` is an error when `f`'s
omitted `inout` parameter defaults to `g`. An argument that is not a
place, such as `a + 1` or `p.x.clone()`, is a temporary and never overlaps;
copying a value into a local first is the usual fix. Stage0 compares places
by how they are written, so two class references to the same instance are
not reported.
