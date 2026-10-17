# JavaScriptCore Changelog

This changelog covers updates to JavaScriptCore in Bun's WebKit fork, from `9a2cc42ae1bf` to `7bc2f97e2835`.

## Performance

### RegExp SIMD Acceleration

Regular expressions got a major performance boost with a new SIMD-accelerated prefix search, inspired by V8's approach. When a regex has alternatives with known leading characters (e.g., `/aaaa|bbbb/`), JSC now uses SIMD instructions to scan 16 bytes at a time, rapidly rejecting non-matching positions before falling back to scalar matching. This is implemented for both ARM64 (using TBL2) and x86_64 (using PTEST), so all platforms benefit.

The x86_64 codegen also gained new constant materialization primitives (`move128ToVector`, `move64ToDouble`, `move32ToFloat`) using broadcast and shuffle instructions, which are necessary for the SIMD regex paths and future SIMD optimizations.

- [`579b96614b75`](https://github.com/nicowilliams/WebKit/commit/579b96614b75) — SIMD fast prefix search for RegExp (ARM64)
- [`b7ed3dae4a6a`](https://github.com/nicowilliams/WebKit/commit/b7ed3dae4a6a) — SIMD fast prefix search for RegExp (x86_64)
- [`aa596dded063`](https://github.com/nicowilliams/WebKit/commit/aa596dded063) — x86_64 constant materialization for SIMD masks

### RegExp JIT: Fixed-Count Parentheses

Non-capturing parenthesized subpatterns with fixed-count quantifiers like `(?:abc){3}` previously fell back to the slower Yarr interpreter. They are now JIT-compiled using a counter-based loop, yielding a **~3.9x speedup** on affected patterns. A follow-up patch also added JIT support for fixed-count subpatterns **with capture groups** (e.g., `/(a+){2}b/`), correctly saving and restoring capture state across iterations.

- [`ac63cc259d74`](https://github.com/nicowilliams/WebKit/commit/ac63cc259d74) — JIT support for non-capturing fixed-count parentheses (~3.9x faster)
- [`c8b66aa0832b`](https://github.com/nicowilliams/WebKit/commit/c8b66aa0832b) — JIT support for fixed-count subpatterns with captures

### `String#startsWith` Optimized in DFG/FTL

`String.prototype.startsWith` is now an intrinsic in the DFG and FTL JIT tiers, with constant folding support when both the string and search term are known at compile time.

| Benchmark | Speedup |
|---|---|
| `string-prototype-startswith` | **1.42x faster** |
| `string-prototype-startswith-constant-folding` | **5.76x faster** |
| `string-prototype-startswith-with-index` | **1.22x faster** |

- [`1f7d7d5a8c23`](https://github.com/nicowilliams/WebKit/commit/1f7d7d5a8c23)

### `Set#size` and `Map#size` Optimized in DFG/FTL and Inline Caches

The `.size` getter on `Set` and `Map` is now handled as an intrinsic in the DFG/FTL tiers and inline caches, eliminating the overhead of a generic getter call.

| Benchmark | Speedup |
|---|---|
| `set-size` | **2.24x faster** |
| `map-size` | **2.74x faster** |

- [`2e2c23521a24`](https://github.com/nicowilliams/WebKit/commit/2e2c23521a24)

### `String#trim` Optimized

`String.prototype.trim`, `trimStart`, and `trimEnd` now use direct pointer access via `span8()`/`span16()` instead of indirect `str[i]` character access, avoiding repeated bounds checking.

| Benchmark | Speedup |
|---|---|
| `string-trim` | **1.17x faster** |
| `string-trim-end` | **1.42x faster** |
| `string-trim-start` | **1.10x faster** |

- [`73a97d320d4b`](https://github.com/nicowilliams/WebKit/commit/73a97d320d4b)

### `Object.defineProperty` Handled in DFG/FTL

`Object.defineProperty` is now recognized as an intrinsic in the DFG and FTL JIT tiers. While this patch alone doesn't change benchmark numbers, it lays the groundwork for future optimizations that can specialize based on descriptor shape.

- [`b1703ed2b97e`](https://github.com/nicowilliams/WebKit/commit/b1703ed2b97e)

### `String.prototype.replace` Returns Ropes

When using `"string".replace("search", "replacement")` with string arguments, JSC now constructs a rope (lazy concatenation) instead of eagerly copying the entire result. This avoids unnecessary allocations for the common case where the result is only used briefly. This aligns with V8's behavior.

- [`69162bbdb602`](https://github.com/nicowilliams/WebKit/commit/69162bbdb602)

### B3 Switch Lowering Simplified

The B3 backend's `Switch` lowering was reworked to produce simpler CFG. Instead of an explicit bounds check + conditional branch before the jump table, the index is now clamped with a `min` and the fallthrough target is placed at the end of the jump table. This eliminates a branch and makes the resulting code easier for later optimization passes to analyze, particularly for large switch statements.

- [`9f768a57ee20`](https://github.com/nicowilliams/WebKit/commit/9f768a57ee20)

### Wasm Optimizations

- **`ref.cast` for final types**: When the target type of a `ref.cast` is a final type, JSC now uses a direct RTT pointer comparison instead of walking the RTT chain, which is faster. ([`17d713c71a50`](https://github.com/nicowilliams/WebKit/commit/17d713c71a50))
- **OMG VM access consolidation**: The OMG compiler now caches the `VM*` pointer in a single B3 value, avoiding redundant loads from the instance for write barriers and mutator fences. ([`6c59818feb76`](https://github.com/nicowilliams/WebKit/commit/6c59818feb76))

### Bytecode Cleanup: `op_end` Removed

The `op_end` opcode was removed and replaced with `op_ret` everywhere. `op_end` was semantically identical to `op_ret` but had worse codegen in the Baseline JIT. This simplifies the bytecode set.

- [`1883cff4b2c5`](https://github.com/nicowilliams/WebKit/commit/1883cff4b2c5)

### Memory Allocator: SequesteredArenaMalloc Enabled on macOS

The SequesteredArenaMalloc allocator, which provides per-thread arenas for JIT compiler allocations to improve isolation and reduce contention, is now enabled on macOS. A follow-up patch also removed the hard limit on the number of compiler thread handles, allowing an arbitrary number of compiler threads.

- [`9defcdb215ac`](https://github.com/nicowilliams/WebKit/commit/9defcdb215ac) — Enable on macOS
- [`af95d936795d`](https://github.com/nicowilliams/WebKit/commit/af95d936795d) — Support arbitrary compiler threads

## Bug Fixes

### DFG Constant Folding Crash Fix

Comparisons where both operands are the same value (e.g., `x <= x` where `x` is a Symbol) could cause the DFG to incorrectly treat the result as unreachable, generating a `brk` instruction that crashes at runtime. This is now fixed.

- [`9621a2879b14`](https://github.com/nicowilliams/WebKit/commit/9621a2879b14)

### `Function.prototype.toString()` Format Fixed

The string representation of native functions was changed from a multi-line format to a single-line format matching V8/Node.js:

```js
// Before:
"function Function() {\n    [native code]\n}"

// After:
"function Function() { [native code] }"
```

This fixes compatibility with packages that parse the output of `.toString()` on native functions.

- [`8ab210be7028`](https://github.com/nicowilliams/WebKit/commit/8ab210be7028) — Fixes [bun#26698](https://github.com/oven-sh/bun/issues/26698)

### `%TypedArray%.prototype.set` Spec Compliance

Fixed a spec compliance issue where `TypedArray.prototype.set` would return early during value conversion if the underlying `ArrayBuffer` was shrunk and then grown. The conversion loop now runs to completion per ECMA-262, correctly handling resizable `ArrayBuffer`s.

- [`841a8a56e8cd`](https://github.com/nicowilliams/WebKit/commit/841a8a56e8cd)

### `Error.captureStackTrace` Materialization Fix

When `Error.captureStackTrace` was called on an `ErrorInstance` before the error's properties were lazily materialized, the materialization would overwrite the `.stack` property set by `captureStackTrace`. A tracking bit now prevents this.

- [`0e62abf49f09`](https://github.com/nicowilliams/WebKit/commit/0e62abf49f09)

### RegExp Capture Group Reset Fix

A regression from the fixed-count parentheses optimization where nested capture groups were not being reset to `undefined` at the start of each iteration, as required by the ECMAScript spec. For example, `/(a)?b\1/{2}` now correctly resets capture group 1 between iterations.

- [`7c8968c2ec20`](https://github.com/nicowilliams/WebKit/commit/7c8968c2ec20)

### Exception Handling Fixes

Several missing exception checks were added:

- `String.prototype.replace` with regex and empty replacement string could miss an exception check. ([`ffbeef167f9a`](https://github.com/nicowilliams/WebKit/commit/ffbeef167f9a))
- The JS-to-Wasm entry wrapper could miss an exception when building the return frame. ([`9546f07aeb91`](https://github.com/nicowilliams/WebKit/commit/9546f07aeb91))
- Wasm GC IPInt slow paths could trigger GC, which reads `topCallFrame` for ShadowChicken — but IPInt's `topCallFrame` can be stale, causing crashes. Now nulled out in such slow paths. ([`d106624a1491`](https://github.com/nicowilliams/WebKit/commit/d106624a1491))
- Missing exception checks in `AbstractModuleRecord` and `JSModuleNamespaceObject`. ([`3712974e056a`](https://github.com/nicowilliams/WebKit/commit/3712974e056a))

## New Features

### `Math.sumPrecise` Enabled by Default

`Math.sumPrecise` has reached Stage 4 in TC39, so its feature flag has been removed. It is now always available. `Math.sumPrecise` computes the sum of an iterable of numbers with higher precision than naive sequential addition, avoiding catastrophic cancellation.

- [`fdd2af6e866e`](https://github.com/nicowilliams/WebKit/commit/fdd2af6e866e)

### WebAssembly Debugger (LLDB Integration)

A new WebAssembly debugger allows LLDB to debug Wasm code running in JSC. Key changes in this release:

- A dedicated `ENABLE_WEBASSEMBLY_DEBUGGER` macro was introduced (currently macOS ARM64 only).
- A preference/option was added to enable the debugger.
- A race condition in the debug server's accept thread was fixed, which previously caused LLDB connection timeouts.
- Idle VMs can now be interrupted for Stop-The-World debugging by dispatching to their RunLoop.
- Wasm binary hashes are now attached to internal function names for easier identification in debugger output.
- Added test coverage for Swift Wasm exception handling (`do-catch-throw`).

- [`f2e39ae2e148`](https://github.com/nicowilliams/WebKit/commit/f2e39ae2e148), [`4d96269236ae`](https://github.com/nicowilliams/WebKit/commit/4d96269236ae), [`69f3eff63c32`](https://github.com/nicowilliams/WebKit/commit/69f3eff63c32), [`6582b6b3fb43`](https://github.com/nicowilliams/WebKit/commit/6582b6b3fb43), [`b5980d315589`](https://github.com/nicowilliams/WebKit/commit/b5980d315589), [`91c756f67029`](https://github.com/nicowilliams/WebKit/commit/91c756f67029), [`a9e86615c78f`](https://github.com/nicowilliams/WebKit/commit/a9e86615c78f)

### LOL JIT Progress

The LOL (Lowered-Overhead Lightweight) JIT, a new JIT tier being developed between the interpreter and Baseline JIT, gained support for:

- All jump bytecodes (`jtrue`, `jfalse`, `jless`, `jgreater`, `jbelow`, etc.)
- Type-checking bytecodes (`is_undefined`, `is_boolean`, `is_number`, `is_big_int`, `is_string`, `is_object`, `is_callable`, etc.)

- [`da58be75db0b`](https://github.com/nicowilliams/WebKit/commit/da58be75db0b), [`afe79c1fa749`](https://github.com/nicowilliams/WebKit/commit/afe79c1fa749), [`04bcc9f854dd`](https://github.com/nicowilliams/WebKit/commit/04bcc9f854dd)

## Build & Toolchain

### LLVM Toolchain Upgraded to 21.1.8

The LLVM toolchain used for building was upgraded from 19.1.7 to 21.1.8.

- [`7bc2f97e2835`](https://github.com/nicowilliams/WebKit/commit/7bc2f97e2835)

### Other Build Fixes

- Fixed bmalloc MTE (Memory Tagging Extension) builds for open-source configurations. ([`590c4d847d46`](https://github.com/nicowilliams/WebKit/commit/590c4d847d46))
- Fixed `libpas` system-heap reporting for `LibpasMallocReportConfig=1`. ([`7de508db0171`](https://github.com/nicowilliams/WebKit/commit/7de508db0171))
- `[[nodiscard]]` added to `WTF::Vector` accessor functions. ([`4b535ee9b46f`](https://github.com/nicowilliams/WebKit/commit/4b535ee9b46f))
- `AvailableMemory` moved from bmalloc to WTF. ([`c021912ad55c`](https://github.com/nicowilliams/WebKit/commit/c021912ad55c))
