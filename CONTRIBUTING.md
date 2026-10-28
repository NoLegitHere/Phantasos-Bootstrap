# Contributing to Phantasos

## Ground rules

1. **License.** All contributions are made under the Apache License 2.0.
2. **The license header is mandatory.** Every source file must begin with the
   project header, using that file's comment syntax:

   ```
   // Copyright <YEAR> The Phantasos Authors
   // SPDX-License-Identifier: Apache-2.0
   //
   // Licensed under the Apache License, Version 2.0 (the "License");
   // ...
   ```

   Run `python tools/license-check/license_check.py --fix` to insert any
   missing headers. CI fails if a header is missing.
3. **Write it from scratch.** Do not copy code or headers from LLVM, glibc,
   musl, the MSVC runtime, the Windows SDK, or the Linux kernel. Platform
   declarations in `platform/` must be written by us from public
   documentation (only facts such as syscall numbers, struct layouts, and
   function signatures).
4. **Developer Certificate of Origin.** Sign off every commit
   (`git commit -s`), which certifies the DCO (https://developercertificate.org).

## Stage0 C++ rules

- C++23, built with clang++ on Windows and Linux. The toolchain must pass the
  CMake feature probe; the README lists the versions CI uses.
- No exceptions (`-fno-exceptions`) and no RTTI (`-fno-rtti`). Report failures
  with `std::expected` or diagnostics.
- Use the typed IDs and enums (`FileId`, `BindingId`, `DiagCode`,
  `ast::Modifier`) rather than raw integers or strings. Diagnostic codes are
  stable once released.
- Downcast AST nodes with `ast::as<T>` (nullable) or `ast::cast<T>`
  (asserted); type declarations use `as_type_decl` and `cast_type_decl`.
- Arena objects must be trivially destructible.
- Traverse ASTs and type graphs with explicit work stacks, not recursion:
  flat expression chains can be far deeper than the parser's nesting limit.
- Passes do not modify the AST. Each phase (validation, resolution, typing,
  flow, the code generation gate, lowering) keeps its results in its own
  tables, and `FrontendSession` (`frontend/session.hpp`) owns them. A phase
  runs only if the file has no errors from the earlier ones.
- Speculative work such as overload probes must not emit diagnostics or
  change recorded facts.
- Keep CLI-only behavior in `main.cpp`; everything else belongs in the
  `phantasos_frontend` library that the CLI and tests link.
- Keep use of the C++ standard library small. Stage0 is frozen after
  self-hosting.
- When a change affects the language, update [`docs/spec`](docs/spec). When a
  milestone item is done, tick it in the [roadmap](docs/roadmap.md).

## Tests

- `ctest --test-dir build --output-on-failure` runs the C++ suites in
  `tests/unit/`, one per compiler area. CI runs them on both
  platforms and under AddressSanitizer and UndefinedBehaviorSanitizer.
- Golden tests live in `tests/golden/`. Each `*.phs` input has its expected
  output next to it:

  | Directory | Command | Expected output |
  |---|---|---|
  | `lexer/`, `parser/` | `lex`, `parse` | `*.tokens`, `*.ast`, `*.diag` |
  | `diagnostics/` | `check` | `*.diag`; always exits 1 |
  | `semantics/` | `check` | `*.diag` |
  | `codegen/` | `gate` | `*.diag` |
  | `mir/` | `mir` | `*.mir`, `*.diag` |
  | `asm/` | `asm` | `*.lst`, `*.diag` |
  | `run/` | `build`, then run | `*.exit`, `*.stdout`, `*.stderr`; Linux x86_64 only |

  A case is expected to fail when its `.diag` file has an error header, and
  to succeed otherwise. The `.diag` file is optional for `lexer/`, `parser/`,
  `mir/` and `asm/`, and `.stdout` and `.stderr` are optional for `run/`; a
  missing one means no output is expected.
- Run them with `python tests/run_golden.py --stage0 <exe>`. Add `--update
  [filter]` to regenerate expected output, then review the diff before
  committing. A new case needs empty expected files first.
- `python tests/test_frontend.py --stage0 <exe>` covers nesting limits, long
  inputs and parser state.
- `python tests/fuzz_frontend.py --stage0 <exe>` mutates the golden inputs and
  checks for crashes and hangs. Use `--command` to fuzz `check`, `gate`,
  `mir`, `asm` or `build` instead of `parse`.
- `python tests/test_license_check.py` and `python tests/test_golden_runner.py`
  test the license checker and the golden runner.
