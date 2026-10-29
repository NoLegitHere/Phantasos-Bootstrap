# Bootstrap roadmap

The roadmap follows the bootstrap strategy in
[ADR 0002](adr/0002-foundational-decisions.md). Checked entries describe
implemented behavior; an entire phase is complete only when its exit
criterion is met.

## P0: language and toolchain foundations

- [x] Draft language, package, concurrency and ABI specifications.
- [x] C++23 stage0 build on Windows x64 and Linux x86_64.
- [x] Source management, arena storage, interning and stable diagnostics.
- [x] License enforcement and CI.

Exit criterion: a documented Core language and a tested bootstrap toolchain
foundation. Complete.

## P1: stage0 native compiler

- [x] Lexer, parser with error recovery, syntax dumps and the frontend
  library, with a standalone [Core EBNF grammar](spec/phantasos.ebnf).
- [x] Structural validation and file-local name resolution.
- [x] Type checking: scalars, numeric conversions, aliases, composite and
  nominal types, members, inheritance and interface conformance, generic
  instantiation, argument inference, bounds and `where` clauses, and generic
  bodies checked once.
- [x] Flow checks: all-path returns, definite initialization, mutability of
  bindings, fields and `mut fun` receivers, and branch result types.
- [x] **Native scalar vertical slice (M1).** Compile a scalar Core subset
  (functions, locals, loops, calls, checked integer arithmetic) through a typed
  IR and naive x86_64 code to a static Linux x86_64 executable that returns its
  exit code. Constructs outside the subset receive a diagnostic.
  - [x] Code generation gate: `gate` reports constructs outside the scalar
    subset (`E0600`) and checks the `main` entry point (`E0601`).
  - [x] Typed MIR with strictly left-to-right lowering and a `mir` dump.
  - [x] Naive x86_64 code with overflow and division traps.
  - [x] Static ELF writer, stage0 `_start` and panic stub.
  - [x] `build` command and run tests in Linux CI.
- [x] **Output (M2).** `print` in the prelude for `String`, `Int`, `UInt`
  and `Bool` (spec 3.27), string literals in read-only data, and a runtime
  write, so stage0 programs can say hello.
  - [x] Prelude `print` and its call checking.
  - [x] Read-only data in objects and ELF executables.
  - [x] MIR and x86_64 for two-word `String` values; gate accepts them.
  - [x] Runtime write and integer formatting; gate accepts `print`.
  - [x] Run tests that compare standard output, with sample programs.
- [x] **Structs and tuples (M3).** Non-generic structs and unlabeled
  tuples as aggregate values on Linux x86_64: layout (ABI A.2), System V passing
  (A.6), field assignment and methods.
  - [x] Field and `mut fun` receiver mutability checks.
  - [x] Struct values: construction, field reads, locals, arguments and results.
  - [x] Field assignment.
  - [x] Methods, including `mut fun`.
  - [x] Tuples in the front end: types, literals, `.0` access and destructuring.
  - [x] Tuple code generation.
  - [x] Sample programs and run tests.
- [x] **Closures (M4).** Named functions as values with indirect calls,
  then brace closures (`{ x -> x * 2 }`) that capture by value, on Linux
  x86_64. A function value is two words, code and environment.
  - [x] Function values of named functions in the front end.
  - [x] Function values and indirect calls in MIR.
  - [x] x86_64 indirect calls and function value passing.
  - [x] Function value samples and run tests.
  - [x] Closure syntax.
  - [x] Closure checking: parameter inference and captures.
  - [x] Closure code generation with a stage0 heap.
  - [x] `own` and `borrow` parameters in code generation, so closures can
    capture non-Copy parameters.
  - [x] Closure samples and run tests.
- [x] **Ownership and moves (M5).** `check` enforces the move rules of
  spec 3.4 before destructors exist: no use after a move, no moving out of a
  borrow, and exclusive `inout` arguments.
  - [x] Use after move for locals and `own` parameters (E0504).
  - [x] A built-in `.clone()` for String and structurally clonable values.
  - [x] Moving out of borrowed parameters, `self`, fields and captures (E0505).
  - [x] Exclusive `inout` arguments (E0506).
  - [x] Move samples and run tests.
- [x] **Generics codegen (M6).** Monomorphized code for generic functions,
  structs and methods, with interfaces as bounds through static dispatch.
  - [x] Generic functions, with a limit on instances (E0602).
  - [x] Generic structs and their methods.
  - [x] Values of generic functions, and generic closures.
  - [x] Interfaces as bounds, through witnesses.
  - [x] Generics samples and run tests.
- [ ] **Arrays and optionals (M7).** `[T]` arrays and `T?` optionals in
  `check` and in code generation on Linux x86_64.
  - [x] Optionals in `check`: `if let`, `while let`, `??` and comparisons
    with `none`.
  - [x] Optional code generation, with a located panic for `!` on `none`.
  - [ ] Optional chaining (`?.`).
  - [ ] Arrays in `check`: built-in members and `for` over arrays and ranges.
  - [ ] Array values in code generation, with bounds checks.
  - [ ] `append` growth and `for` loops in code generation.
  - [ ] Moves, `.clone()` and generics for arrays and optionals.
  - [ ] Samples and run tests.
- [ ] Generic base substitution and built-in traits: generic base and
  interface graphs, derived `Copy`/`Sendable`/`Error`, associated defaults and
  the remaining generic method contracts.
- [ ] The rest of type checking: optionals, generic aliases, visibility,
  typed errors and the remaining numeric rules.
- [ ] Field and receiver initialization, borrows and destructors.
- [ ] Code generation for the rest of Core: arrays, optionals, classes,
  enums, `match`, errors and cleanup.
- [ ] COFF and relocatable ELF objects, and a PE/ELF linker. Stage0 writes
  static Linux executables directly today.
- [ ] Windows x64 executables, with their entry point and runtime.

Exit criterion: stage0 can compile, link and run Core programs natively on
both initial targets. In progress.

## P2: package and workspace support

- [ ] Parse the documented `Phantasos.toml` subset.
- [ ] Discover source modules and workspace members.
- [ ] Resolve local dependencies, report cycles and build in dependency order.
- [ ] Produce and consume `.phsi` library interfaces and object archives.
- [ ] Link libraries into binary packages.

Exit criterion: stage0 can build the multi-package compiler workspace from
manifests. See [the package specification](spec/04-packages-and-modules.md)
for the deliberately limited stage0 build-driver scope.

## Self-hosting and full-language work

### Readiness gates before the port

The normative [Core subset](spec/README.md) is the language of the self-hosted
compiler. The following gates turn the remaining deferrals into a concrete
bootstrap plan; successful parsing or today's partial `check` is insufficient.

1. **Close Core semantic gaps.** Finish generic instantiation/substitution,
   generic aliases/calls, inference and `where` constraints, then dependent
   conformance (associated types, `Self`, properties and required built-in
   contracts). Complete optionals, casts, block/branch result types,
   function/bound-method values, constructor selection/delegation, visibility,
   numeric policies, typed errors, unsafe contexts and privileged intrinsics.
   Track implementation and tests against every Core construct; no unresolved
   required type, call, member or witness may enter executable lowering.
   Unsupported constructs must produce diagnostics instead of silently passing
   this gate.
2. **Enforce Core safety and cleanup.** Add all-path returns, exhaustiveness,
   definite initialization, mutable-place rules, moves, borrows and calling
   conventions, plus destruction on every control-flow exit. Implement the
   required `Copy`/`Error` contracts and `shared`/`weak` ownership behavior.
3. **Execute native Core programs.** Finish typed IR and lowering, layouts,
   virtual/witness dispatch, typed-error returns and cleanup. Complete the
   [ABI](spec/abi.md), including collision-free overload/type mangling, before
   object emission, and implement the required `extern "C"` ABI. Supply the
   x86_64 backends, COFF/ELF writers, PE/ELF linkers and the minimal runtime/library
   surface the compiler needs (allocation, collections, strings, files, diagnostics
   and process entry). Run compiler-sized Core workloads on Windows x64 and Linux
   x86_64, using the own-toolchain policy in
   [ADR 0002](adr/0002-foundational-decisions.md).
4. **Finish P2 before porting.** Build a multi-package workspace from manifests,
   resolve dependencies, emit/read `.phsi` interfaces and link libraries. A
   file-local checker cannot build the compiler workspace.
5. **Prove the replacement before freezing stage0.** Build stage1 with stage0,
   compare accepted/rejected programs and executable behavior across both
   compilers, then require byte-identical stage2/stage3 binaries with controlled
   inputs and the same target/build configuration. Keep stage0 available until
   these checks pass on both initial targets.

Full-language features outside Core, including concurrency if the compiler
does not use it, can follow self-hosting. Any change to the documented Core
boundary must be explicit and precede relying on a smaller bootstrap subset.
The remaining targets, full C11 frontend, and full runtime/standard library also
remain later work; implement their bootstrap-required surface first. Incremental
build features listed below are not prerequisites for the port.

### Transition and later work

- [ ] Port the compiler to Phantasos and build stage1 with stage0.
- [ ] Produce byte-identical stage2 and stage3 binaries.
- [ ] Freeze the bootstrap compiler after reproducible self-hosting.
- [ ] Extend the self-hosted toolchain with the remaining targets, runtime,
  standard library, C frontend, and full-language features.

Incremental caching, lockfiles, git dependencies, parallel builds and
cross-compilation belong to the self-hosted build tool, not stage0.
