# Bootstrap roadmap

The roadmap follows the bootstrap strategy in
[ADR 0002](adr/0002-foundational-decisions.md) and
[ADR 0003](adr/0003-bootstrap-subset.md). Checked entries describe
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
- [x] **Arrays and optionals (M7).** `[T]` arrays and `T?` optionals in
  `check` and in code generation on Linux x86_64.
  - [x] Optionals in `check`: `if let`, `while let`, `??` and comparisons
    with `none`.
  - [x] Optional code generation, with a located panic for `!` on `none`.
  - [x] Optional chaining (`?.`).
  - [x] Arrays in `check`: built-in members and `for` over arrays and ranges.
  - [x] Array values in code generation, with bounds checks.
  - [x] `append` growth and `for` loops in code generation.
  - [x] Moves, `.clone()` and generics for arrays and optionals.
  - [x] Samples and run tests.
- [x] Shifts (`<<`, `>>` and their compound assignments) in code generation.
- [ ] **Enums and `match` (M8).** Enum values with payloads, `match` with
  exhaustiveness checks, and generic enums such as `Result<T, E>`.
  - [x] Enum cases in `check`.
  - [x] `match` patterns in `check`, with payload bindings that borrow.
  - [x] Exhaustiveness checks, and enums that hold themselves.
  - [ ] Enum values in code generation.
  - [ ] `match` in code generation.
  - [ ] Generic enums and enum methods.
  - [ ] Samples and run tests.
- [ ] **Strings as data (M9).** Comparison, byte access, slicing and
  building strings from bytes, with `String` always valid UTF-8.
- [ ] **Files and process (M10).** Reading and writing files, arguments,
  exit codes and stderr.
- [ ] **Programs of several files (M11).** A directory of modules with
  `import` and visibility, compiled as one program.

Exit criterion: stage0 compiles the bootstrap subset
([ADR 0003](adr/0003-bootstrap-subset.md)) to Linux x86_64, and a part of
stage1 written in that subset builds and runs in CI. In progress.

## Stage1: the compiler in Phantasos

Stage1 is written in the bootstrap subset and checked against it with
stage0's `gate` on every build. Its design (compiler architecture, standard
library and tooling) is still being drafted.

- [ ] Port the compiler to Phantasos and build stage1 with stage0.
- [ ] The rest of type checking: generic base substitution, built-in
  traits, typed errors, borrows and destructors.
- [ ] Code generation for the rest of Core: classes, values of interface
  type, built-in traits, typed errors, destructors and cleanup.
- [ ] COFF and ELF objects, a PE/ELF linker, and Windows x64 executables.
- [ ] Packages and workspaces from `Phantasos.toml`, `.phsi` interfaces and
  libraries.
- [ ] Produce byte-identical stage2 and stage3 binaries.
- [ ] Freeze stage0 after reproducible self-hosting.
- [ ] The remaining targets, runtime, standard library, C frontend and
  full-language features.
