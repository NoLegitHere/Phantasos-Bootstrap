# ABI and object model (draft)

The **Phantasos-native ABI** is unstable and may change between toolchain
versions; packages are always built with a single toolchain. Only
`extern "C"` and `@export("C")` declarations follow the stable **platform C
ABI**.

## A.1 Targets and primitive layout
| Target | Pointer / `Int` | C calling convention | Object format | Executable |
|---|---|---|---|---|
| `windows-x64` | 8 | Microsoft x64 | COFF | PE32+ |
| `linux-x86_64` | 8 | System V AMD64 | ELF64 | ELF64 (static, `ET_EXEC`/`ET_DYN` PIE later) |
| `linux-i386` | 4 | System V i386 (cdecl) | ELF32 | ELF32 |

- Integer and float types have their natural size and alignment.
- Exception: in `@repr("C")` types on i386, `Int64` and `Float64` are aligned
  to 4 bytes, matching the i386 C ABI. Phantasos-layout types always align
  them to 8.
- `Bool` is 1 byte (0 or 1), and `Char` is 4 bytes.

## A.2 Struct layout
- By default, fields are **reordered by decreasing alignment** (stable
  within equal alignment) to minimize padding.
- `@repr("C")` keeps declaration order with C padding rules and is required
  for FFI.
- Zero-sized structs take up 0 bytes inside other aggregates.
- Field offsets follow the reordered fields, each aligned to its own
  alignment. The struct's alignment is the largest field alignment, and its
  size is rounded up to a multiple of it. A `String` field is two words with
  8-byte alignment. For example, `struct Flags(on: Bool, level: Int8, mask:
  UInt32)` puts `mask` at 0, `on` at 4 and `level` at 5, and takes 8 bytes.
- A tuple is laid out like a struct whose fields are its elements, in
  order, so `(Bool, Int, Int8)` puts element 1 at 0, element 0 at 8 and
  element 2 at 9, and takes 16 bytes. It is passed like that struct too.
- A function value is two words, like a struct of a code pointer at 0 and
  an environment pointer at 8, and is passed like that struct. The code
  takes the environment as a hidden parameter (A.6). Stage0's values of named
  functions point at a thunk and have a null environment.

## A.3 Class objects
```
offset 0          vptr         -> vtable
offset P          strong: UInt32   (strong reference count; 1 when uniquely owned)
offset P+4        weak:   UInt32   (weak count + 1 while strong > 0)
offset align(P+8) fields of the root class, then fields of each subclass in order
```
(P = pointer size.) Superclass fields come before subclass fields, so a
pointer to a subclass object is also a valid pointer to its superclass.

- **vtable**:
  - `[0]` is the type descriptor.
  - `[1]` is `destroy(self)`, which runs the `deinit` chain, destroys fields,
    and frees memory (or only releases the strong portion if weak refs
    exist).
  - `[2..]` are virtual methods: superclass slots first, then new `open`
    methods in declaration order. An override reuses its parent's slot.
- **Type descriptor**: `{ size, align, depth, ancestors: *[*TypeDesc] (ancestors[depth] == self), name: *UInt8, itables: *[(*IfaceDesc, *ITable)], itable_count }`.
  This makes the `is` check O(1): `obj.desc.depth >= T.depth && obj.desc.ancestors[T.depth] == T`.

## A.4 Interfaces
- An **existential** value (a value whose static type is an interface) is a
  fat pointer `{ data: *Object, itable: *ITable }`.
- An itable has the same shape as the vtable method area for that
  interface's requirements. Default methods fill any slot the type does not
  implement itself.
- Generic code with interface bounds is monomorphized and needs no itables.

## A.5 Enums and optionals
- `enum`: the tag is the smallest unsigned integer that fits the number of
  cases, followed by a payload union aligned to the largest payload
  alignment.
- **Niche optimization**: `T?` for a class reference, `shared T`, `weak T`,
  or a non-null pointer is a single pointer, with `none == 0`.
- Any other `T?` is laid out like a struct of a `Bool` tag, set when it holds
  a value, and the value (A.2), and is passed like that struct. `none` is all
  zero bytes. So `Int?` puts the value at 0 and the tag at 8 and takes 16
  bytes, and `Int??` holds an `Int?` at 0 and its tag at 16.

## A.6 Calling convention (native)
Phantasos-native calls use the platform C convention, plus hidden
parameters in this order:

1. `ret*`: an indirect return slot, when the platform C ABI would use sret
   or the function throws.
2. `err*`: an error slot (only for throwing functions).
3. `self`: the receiver, or `env`: the environment of a call through a
   function value.
4. The explicit parameters in source order.

- **Throwing functions** return a status `Bool` in `al` (0 = success,
  1 = error). The value is written to `*ret` and the error to `*err`.
- `borrow` parameters of aggregates larger than 2 pointers are passed by
  address. `inout` is always passed by address.
- `own` parameters transfer ownership: the callee destroys them.
- Stage0 passes a `String` as a 16-byte aggregate of two words, a pointer to
  its UTF-8 bytes and their length, as System V does for such a struct: in two
  consecutive integer argument registers when both are free, otherwise both
  words on the stack (later scalars may still take the remaining register),
  and it returns one in `rax:rdx`. String literals live in `.rodata` (A.8).
- Stage0 passes other structs the same way by size. A struct of up to two
  words is passed in one or two integer registers, eightbyte by eightbyte, or
  on the stack whole, and returned in `rax` or `rax:rdx`. A larger struct is
  passed by address, as a `borrow` parameter; the callee copies it before use.
  A larger result is written through `ret*` in `rdi`, and the callee returns
  that address in `rax`. Stage0 has no floating-point fields yet, so every
  eightbyte is of the INTEGER class.
- Stage0 calls a function value by loading its code into `r11`, which no
  argument uses, and its environment as the first parameter after `ret*`,
  then `call r11`. A named function used as a value gets a thunk,
  `_P$5thunk` followed by its length-prefixed stage0 name (so `twice` gets
  `_P$5thunk5twice`), which takes the environment and the function's
  parameters, drops the environment and calls the function. A closure's
  code is its own function, which takes the environment as that first
  parameter. Its environment points to a copy of each captured value, laid
  out like a struct (A.2) in capture order, with the receiver last when the
  closure uses it; a closure that captures nothing has a null environment.
- Stage0 passes a method's receiver as its first parameter after `ret*`, like
  any other struct argument. A `mut fun` receiver is `inout`: it is passed by
  address whatever its size. The callee copies it in, and it writes the final
  value back through that address before it returns.

## A.7 Symbol mangling
```
symbol   = '_P' [ '$' ] path [ generics ] [ labels ]
path     = { len ident }            ; package, module components, type, member
generics = 'G' { type } 'E'
labels   = 'L' { len ident } 'E'    ; only for init overloads
type     = 'i1'|'i2'|'i4'|'i8'|'u1'|'u2'|'u4'|'u8'|'f4'|'f8'|'b'|'c'|'s'
         | 'N' path [ generics ]    ; nominal
         | 'O' type                 ; optional
         | 'A' type                 ; array
         | 'S' type | 'W' type      ; shared / weak
         | 'T' type type { type } 'E'  ; tuple, elements in order
```
- Example: `geometry.shapes.Circle.area` is mangled as
  `_P8geometry6shapes6Circle4area`.
- Tuple types are structural, so their code spells out every element:
  `(Int8, (Bool, String))` is `Ti1TbsEE`.
- The `_P$` prefix is reserved for privileged internal symbols, which are
  always emitted with hidden visibility.

## A.8 Program entry
- The runtime provides the process entry point: `_start` on Linux, and
  `__phantasos_entry` on Windows (set as `AddressOfEntryPoint`).
- The entry point initializes TLS, the allocator, and argv/env, then calls
  the package's mangled `main`, and finally calls `exit` with the result.
- No C runtime is linked.
- Stage0 emits a minimal Linux entry instead: `_start` calls `main` and
  passes its result (0 for a `Unit` main) to the `exit_group` system call,
  and `__phantasos_trap` writes a panic message with `write` and exits with
  status 101. The executable is `ET_EXEC` at `0x400000` with a read-only,
  executable `PT_LOAD` segment for code and a separate, page-aligned
  read-only, non-executable one for `.rodata` (string bytes such as the panic
  messages). Identical byte strings within an object share one copy.
- The stage0 runtime also provides the prelude `print` (spec 3.27) as
  `_P$print_string` (`rdi:rsi`), `_P$print_int`, `_P$print_uint` and
  `_P$print_bool` (`rdi`). Each writes its value and a newline to standard
  output through `_P$write`, which retries partial writes and `EINTR` and
  otherwise panics with `panic: cannot write to standard output`.
- When the program creates a closure that captures anything, the stage0
  runtime also provides `_P$alloc`, which takes a size in `rdi` and returns a
  zeroed, 16-byte aligned block in `rax`. It bumps a pointer through chunks of
  at least 1 MiB that it maps with `mmap`, never frees, and panics with
  `panic: out of memory` (trap code 4) when `mmap` fails. Its state lives in
  `.bss`, which gets a page-aligned, writable, non-executable `PT_LOAD`
  segment with no file bytes.
