# Phantasos Bootstrap

Phantasos is a programming language that compiles to native code. This
repository holds its draft specification and the C++ bootstrap compiler,
which runs a growing subset of the language on Linux x86_64.

The design goals, from [ADR 0002](docs/adr/0002-foundational-decisions.md),
are:

- Object-oriented, immutable by default, with no `null`: single class
  inheritance, `final` by default, interfaces with default methods and sealed
  hierarchies.
- Memory safety without a garbage collector: single ownership with moves,
  borrows checked at compile time without lifetime annotations, opt-in
  `shared` (reference counted) and `weak` references, and deterministic
  `deinit`.
- Typed errors with `throws(E)`, returned as values rather than unwound.
- `async`/`await`, actors and `Sendable` checking.
- A toolchain of its own: x86 backends, object writers and linkers for PE
  and ELF, with no LLVM and no C library. Planned targets are Windows x64,
  Linux x86_64 and Linux i386.

Most of this is not implemented yet. The [roadmap](docs/roadmap.md) tracks
what is.

## Status

Bootstrap phase P1. The stage0 compiler in [`src`](src)
is written in C++23 and builds and passes its tests on Windows x64 and Linux
x86_64. It has:

- A lexer and parser for Phantasos-Core, with error recovery and stable
  diagnostic codes.
- `check`: name resolution, type checking (including generics, interfaces
  and inheritance), flow checks (returns, initialization, mutability) and
  ownership checks (use after move, moves out of borrows, exclusive `inout`).
  Parts of the type system are still incomplete, so a program that passes
  `check` is not yet guaranteed to be valid.
- Native code for a subset of Core: integers, `Bool`, `String` literals,
  structs, tuples, methods, function values, closures, generic functions
  and structs with interface bounds, arrays and optionals. `build` turns such a program into a
  static Linux x86_64 executable with a small runtime of its own.

Not done yet: Windows executables, classes, enums, error
handling, destructors, a standard library, and package/workspace builds.

The language is specified in [`docs/spec`](docs/spec), with a standalone
[Core EBNF grammar](docs/spec/phantasos.ebnf).

## What runs today

`build <file.phs> [-o <out>]` compiles a program to a Linux x86_64
executable. Its exit status is the value `main` returns. Integer overflow,
division by zero, an array index out of bounds and `!` on `none` stop the
program with a panic message and exit status 101.
`print` writes a `String`, `Int`, `UInt` or `Bool` followed by a newline.
Anything outside the supported subset gets an `E0600` diagnostic; spec
[3.25](docs/spec/03-types-and-semantics.md) lists the subset.

```phantasos
fun main() {
    print("Hello, world!")
}
```

Structs, methods and tuples
([`run/vectors.phs`](tests/golden/run/vectors.phs),
[`run/bank_account.phs`](tests/golden/run/bank_account.phs),
[`run/divmod.phs`](tests/golden/run/divmod.phs)):

```phantasos
struct Vec(x: Int, y: Int) {
    fun plus(other: Vec): Vec = Vec(x: x + other.x, y: y + other.y)
}

fun divmod(a: Int, b: Int): (Int, Int) = (a / b, a % b)

fun main(): Int {
    let v = Vec(x: 3, y: 4).plus(Vec(x: 1, y: 1))
    let (q, r) = divmod(v.x * v.y, 7)
    print(q)
    return r
}
```

Functions are values: they can be passed, returned, stored in fields and
called through a variable
([`run/higher_order.phs`](tests/golden/run/higher_order.phs),
[`run/calculator.phs`](tests/golden/run/calculator.phs),
[`run/sorting.phs`](tests/golden/run/sorting.phs)):

```phantasos
fun square(x: Int): Int = x * x
fun plus(a: Int, b: Int): Int = a + b

fun fold(from: Int, to: Int, initial: Int, f: fun(Int) -> Int, combine: fun(Int, Int) -> Int): Int {
    var total = initial
    var i = from
    while i < to {
        total = combine(total, f(i))
        i += 1
    }
    return total
}

fun main(): Int = fold(1, 5, 0, square, plus)  // 1 + 4 + 9 + 16 = 30
```

Brace closures capture copies of the values they use, and can outlive the
call that made them
([`run/adders.phs`](tests/golden/run/adders.phs),
[`run/scores.phs`](tests/golden/run/scores.phs),
[`run/rules.phs`](tests/golden/run/rules.phs)):

```phantasos
fun makeAdder(n: Int): fun(Int) -> Int = { x -> x + n }

fun compose(f: fun(Int) -> Int, g: fun(Int) -> Int): fun(Int) -> Int = { x -> g(f(x)) }

fun main(): Int {
    let add5 = makeAdder(5)
    let double = { x: Int -> x * 2 }
    return compose(double, add5)(10)  // 25
}
```

Values that are not `Copy`, such as `String` and structs that hold one, move
when they are passed to an `own` parameter, stored or returned. `check`
reports a use after a move (`E0504`), a move out of something only borrowed
(`E0505`) and an `inout` argument or `mut fun` receiver that overlaps another
argument of the same call (`E0506`); `.clone()` makes a copy that can move
instead
([`run/inventory.phs`](tests/golden/run/inventory.phs),
[`run/greetings.phs`](tests/golden/run/greetings.phs),
[`run/clone.phs`](tests/golden/run/clone.phs)):

```phantasos
struct Item(name: String, var count: Int) {
    mut fun add(n: Int) { count += n }
}

fun stock(name: own String, count: Int): Item = Item(name, count)

fun main(): Int {
    let label = "bolts"
    var bolts = stock(label.clone(), 10)  // `stock(label, 10)` would move label
    print(label)
    let moved = bolts                     // bolts cannot be used after this
    var spare = moved.clone()
    spare.add(moved.count)
    return spare.count                    // 20
}
```

Generic functions and structs are compiled once per set of type arguments
used, and calls through an interface bound go directly to the struct's method
([`run/leaderboard.phs`](tests/golden/run/leaderboard.phs),
[`run/pipeline.phs`](tests/golden/run/pipeline.phs),
[`run/interfaces.phs`](tests/golden/run/interfaces.phs)):

```phantasos
interface Ranked {
    fun score(): Int
    fun beats(other: Self): Bool = score() > other.score()
}

struct Player(points: Int): Ranked {
    fun score(): Int = points
}

struct Pair<A, B>(first: A, second: B) {
    fun swap(): Pair<B, A> = Pair(first: second, second: first)
}

fun best<T: Ranked>(a: T, b: T): T {
    if b.beats(a) { return b }
    return a
}

fun twice<T>(f: fun(T) -> T): fun(T) -> T = { x -> f(f(x)) }

fun main(): Int {
    let winner = best(Player(points: 7), Player(points: 9))
    let p = Pair(first: true, second: winner.score()).swap()
    return twice({ x: Int -> x + 1 })(p.first)  // 11
}
```

Arrays grow with `append`, and `.clone()` copies one with its elements.
Optionals are read with `if let`, `??` and `?.` ([`run/tasks.phs`](tests/golden/run/tasks.phs),
[`run/stacks.phs`](tests/golden/run/stacks.phs)):

```phantasos
struct Stack<T>(var items: [T]) {
    mut fun push(x: own T) { items.append(x) }
    mut fun pop(): T? = items.removeLast()
}

fun main(): Int {
    var stack = Stack<[Int]>([])
    var scores = [1, 2]
    stack.push(scores.clone())   // a copy: later changes to scores don't reach it
    scores.append(3)
    var total = 0
    for s in scores { total += s }
    let saved = stack.pop()
    return total + (saved?.count ?? 0) + (stack.pop()?.count ?? 10)  // 6 + 2 + 10
}
```

## Repository layout

| Path | Purpose |
|---|---|
| `src/`, `cmake/` | The C++23 bootstrap compiler (`phantasos-stage0`) |
| `docs/spec/` | Language and ABI specification |
| `docs/adr/` | Architecture decision records |
| `docs/roadmap.md` | Bootstrap milestones |
| `tests/` | C++ unit tests (`unit/`), golden tests (`golden/`), fuzzers and stress tests |
| `tools/` | Developer tools such as `license-check` |

## Building

You need clang++ 18 or later with a C++23 standard library that passes the
CMake feature probe, CMake 3.28 or later, and Ninja. CI uses Clang 19 with
libstdc++ 14 on Linux and Clang with the MSVC STL on Windows.

On Ubuntu 24.04:

```sh
sudo apt-get install clang-19 g++-14 ninja-build
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++-19 -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS=--gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/14
cmake --build build
```

On Windows, or with a compatible toolchain already set up:

```sh
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

Run the tests:

```sh
ctest --test-dir build --output-on-failure                     # C++ tests
python tests/run_golden.py --stage0 build/phantasos-stage0     # golden tests
python tests/test_frontend.py --stage0 build/phantasos-stage0  # nesting and input limits
python tests/fuzz_frontend.py --stage0 build/phantasos-stage0  # fuzzing; add --command check|gate|mir|asm|build
python tools/license-check/license_check.py                    # license headers
```

[`tests/stress/profile_hierarchy.py`](tests/stress/profile_hierarchy.py)
times and profiles `check` on large generated class hierarchies; CI runs it
with memory and growth budgets. See [CONTRIBUTING.md](CONTRIBUTING.md) for
more on the tests.

## Trying it

```sh
build/phantasos-stage0 help                                    # list the commands
build/phantasos-stage0 parse tests/golden/parser/showcase.phs  # print the syntax tree
build/phantasos-stage0 check tests/golden/run/inventory.phs    # type, flow and ownership checks
build/phantasos-stage0 mir   tests/golden/mir/fib.phs          # print the MIR
build/phantasos-stage0 asm   tests/golden/asm/fib.phs          # print the x86_64 code
build/phantasos-stage0 build tests/golden/run/hello.phs -o hello
./hello                                                        # prints Hello, world!
```

Every program in [`tests/golden/run`](tests/golden/run) builds the same way;
its expected output and exit status sit next to it.

## License

Apache License 2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE). Every source
file must start with the project license header, and CI enforces this with
`tools/license-check`.
