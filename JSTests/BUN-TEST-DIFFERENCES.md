# Where this fork's tests differ from upstream's, and why

`JSTests/`, `LayoutTests/js` and the PerformanceTests collections are upstream's tests. This fork changes 122 of them and
three runner scripts. Every change is listed here by the reason for it, so that after an upstream sync a failing test can
be sorted into one of three piles:

1. **The fork behaves differently on purpose.** Update the expectation. Sections A1–A10 list every such behavior, how to
   recognize it in a failure, and the fork code that causes it. If a failure matches one of these and the fork code is
   still there, the test is the thing to change.
2. **The test cannot pass in a mode or build we run.** Skip it in that mode only, with the reason. Section B.
3. **A known gap or bug in the fork.** Skipped with `TODO(bun)`. Section C. Remove the skip when the gap is closed.

Anything that fits none of these is a regression from the sync: fix the engine, not the test.

Rules for an edit to an upstream test:

- A comment starting `// Bun:` or `// TODO(bun):` on the changed line says what upstream expects and why this fork differs.
- Compare exact values. Do not loosen a check (a regex over the message, `includes`, stripping text) to make it pass.
- Skip the narrowest thing: one mode (`$skipModes`), one build kind (`skip if $asan`), not the file.
- The baselines (`*.baseline-jsc`, `*-expected.txt`) are diffed against the test's whole output. After a sync, a baseline
  line that differs for a reason not listed here differs for a real reason.

---

## A. The fork behaves differently on purpose: expectations updated

### A1. `ReferenceError` text: `x is not defined` (upstream: `Can't find variable: x`)

- Fork code: `runtime/ExceptionHelpers.cpp:59` (`createUndefinedVariableError`). When the message itself cannot be
  allocated: `Variable is not defined` (upstream `Can't find variable`), line 61.
- Recognize: the only difference is that text. Node and V8 print `x is not defined`.
- Files (47). Only that text changes in each:
  - `ChakraCore/test/`: `Basics/IdsWithEscapes`, `Basics/With`, `Bugs/blue_245702`, `Error/CallNonFunction_3`,
    `Error/ErrorCtorProps_v3`, `Error/NativeErrors_v4`, `Function/defernested`, `Function/funcExpr5`, `LetConst/r`,
    `Miscellaneous/HasOnlyWritableDataPropertiesCache` (60 lines), `Object/var`, `Operators/new`, `Regex/blue_102584_1`,
    `es5/exceptions3`, `fieldopts/fieldhoist6`, `6b`, `7`, `_negzero`, `_stripbailouts`, `_undefined_global`,
    `_unreachable`, `strict/10.eval_sm` (all `.baseline-jsc`)
  - `modules/aliasing.js`, `modules/module-is-strict-code.js`
  - `stress/array-push-intrinsic.js`, eight `stress/eval-func-decl-*.js`,
    `stress/global-object-read-modify-write-remove-at-get-strict.js`,
    `stress/out-of-memory-while-creating-undefined-variable-error.js` (the no-name variant), `stress/regress-151324.js`,
    `stress/regress-277219.js`, `stress/reserved-word-with-escape.js`, `stress/superclass-expression-strictness.js`
  - `LayoutTests/js/`: `arrowfunction-lexical-bind-arguments-top-level`, `basic-strict-mode`, `class-syntax-name`,
    `kde/lval-exceptions`, `let-syntax`, `number-constructor`, `object-literal-shorthand-construction`,
    `reparsing-semicolon-insertion` (all `-expected.txt`)

### A2. A native function's `toString()` is one line: `function f() { [native code] }`

- Upstream prints three lines (`function f() {\n    [native code]\n}`).
- Fork code: `runtime/JSFunction.cpp:251`, `runtime/FunctionExecutable.cpp:206` (commit 3ad19d9e49d5).
- Files (10): `ChakraCore/test/` `Array/protoLookup`, `Error/ErrorCtorProps_v3`, `Function/prototype`,
  `Function/toString`, `Lib/toString` (163 functions), `es6/letconst_global_shadow_builtins`, `strict/05.arguments_sm`,
  `typedarray/set` (baselines); `LayoutTests/js/basic-strict-mode` (script and expected text).

### A3. Module link errors are worded differently

- Fork code: `runtime/CyclicModuleRecord.cpp` under `USE(BUN_JSC_ADDITIONS)`, lines 153–303.

  | upstream | this fork |
  |---|---|
  | `Importing binding name 'B' is not found.` | `Export named 'B' not found in module '<url>'.` |
  | `Importing binding name 'B' cannot be resolved due to ambiguous multiple bindings.` | `Export named 'B' cannot be resolved due to ambiguous multiple bindings in module '<url>'.` |
  | `Importing binding name 'default' cannot be resolved by star export entries.` | `Missing 'default' export in module '<url>'.` |
  | `Indirectly exported binding name 'B' is not found.` | `export 'B' not found in './x.js'` |
  | `Indirectly exported binding name 'B' cannot be resolved due to ambiguous multiple bindings.` | `Cannot export 'B' multiple times in './x.js'` |
  | `Indirectly exported binding name 'default' cannot be resolved by star export entries.` | `export default cannot be used with export *` |

- Where the message has the module's URL the test builds it from `import.meta.url` / `callerSourceOrigin()`.
- Files (6): `modules/different-view.js`, `fallback-ambiguous.js`, `import-error.js`, `indirect-export-error.js`,
  `namespace-error.js`; `stress/re-execute-error-module.js`.

### A4. Import attributes: unknown keys and unknown types are the host's, not errors

- Upstream: a key other than `type` is `SyntaxError: Import attribute "k" is not supported`; an unknown `type` rejects.
- Fork code: `parser/NodesAnalyzeModule.cpp` `tryCreateAttributes` (the key check is compiled out, commit 24b83754f70c;
  an unknown type is `ScriptFetchParameters::Type::HostDefined`), `runtime/Completion.cpp` for `import()`'s
  `options.with`. Bun passes every attribute to its loaders (`with { type: "file" }`, `{ type: "sqlite", embed: "true" }`).
- Files (2): `modules/import-attributes-unsupported.js` (the unknown-key case expects no error; the two malformed-syntax
  cases are unchanged), `stress/invalid-import-assertion.js` (resolves instead of rejecting).

### A5. An array that contains itself joins as `""`, it does not overflow the stack

- Upstream removed the guard: `[a].join()` with `a` inside `a` throws `RangeError: Maximum call stack size exceeded`.
- Fork code: `StringRecursionChecker` in `runtime/ArrayPrototype.cpp:329` (`toString`) and `:470` (`join`), as V8 and
  SpiderMonkey. Only arrays: `Error` and `RegExp` that reach themselves overflow, as upstream.
- Files (6): `LayoutTests/js/` `array-string-recursion`, `array-tostring-and-join`, `toString-recursion` (each: the
  script under `script-tests/` and the `-expected.txt`). `array-string-recursion` is this fork's previous version of
  the test (upstream rewrote it to expect the `RangeError`).

### A6. `/u` regular expressions never start a match inside a surrogate pair

- Fork code: `RegExp::matchInlineAtCodePointBoundaries` (`runtime/RegExpInlines.h:115`, fork PR #299).
  - A sticky or global `/u` regexp whose `lastIndex` is inside a pair matches from the pair's start, as the spec's
    `RegExpBuiltinExec` and V8 do. Upstream: no match.
  - A match is not reported at an index between the two halves. Here the fork differs from V8 too.
- File: `stress/regexp-unicode-code-unit-read-for-bmp-terms.js`, two expectations.

### A7. A host time-zone change is seen when the VM is next entered

- `$vm.setHostTimeZone()` bumps `WTF::lastTimeZoneID()`; the VM drops its date and Intl caches on the next entry
  (`runtime/JSDateMath.cpp:519`), not in the middle of the current task. Bun calls `timeZoneDidChange()` itself.
- File: `stress/intl-datetimeformat-default-timezone-change.js`: each check runs in its own `setTimeout`, as the other
  `setHostTimeZone` tests already do. The values expected are unchanged.

### A8. A builtin's stack frame has an empty URL, not `[native code]`

- The lanes build with `ALLOW_LINE_AND_COLUMN_NUMBER_IN_BUILTINS`, so `Array.prototype.map` keeps its code block on the
  frame and the frame's URL is that block's: empty.
- File: `LayoutTests/js/stack-trace-expected.txt`: `map at [native code]` → `map at ` (13 lines).

### A9. A string literal loaded from the bytecode cache is not flagged as an atom

- Fork code: `DecoderStringTable::jsStringFor` (`runtime/CachedTypes.h:132`): loading a cache interns nothing.
- File: `stress/jsstring-definitely-atom-bit.js`: `$skipModes << "bytecode-cache"`. The other modes run it unchanged.

### A10. With parameter expressions, a `var arguments` in the body is a binding of its own

- Upstream keeps one `arguments` binding, in the parameter scope. A store to the body's `var arguments` changes what the
  parameter expressions and their closures read. So does a function named `arguments` in a block of the body, which
  Annex B.3.2.1 stores to the var environment when the block runs.
- Fork code: the `FunctionNode` constructor of `BytecodeGenerator` (`bytecompiler/BytecodeGenerator.cpp`). Its loop over
  `varDeclarations()` creates the var `arguments` like any other var; upstream skips it when
  `shouldCreateArgumentsVariableInParameterScope` is set. `initializeDefaultParameterValuesAndSetupFunctionScopeStack`
  then copies the parameter scope's value into the var, as it does for a var that has a parameter's name.
  `hoistSloppyModeFunctionIfNecessary` always finds the var; upstream falls back to the parameter scope. This is
  FunctionDeclarationInstantiation step 28, and what V8 does.
- Recognize: in `function foo(x = () => arguments) { var arguments = 25; }`, `x()` is the arguments object. Upstream: 25.
- Files (2), one expectation each: `stress/arrow-functions-as-default-parameter-values.js`,
  `stress/sloppy-mode-hoist-arguments-function-non-simple-parameter-list.js`.

---

## B. The test cannot pass in a mode or build we run: narrowed, not changed

| file(s) | directive | reason |
|---|---|---|
| 32 × `stress/ffi-*.js` | `--useExecutableAllocationFuzz=false` added to `requireOptions` | the `ftl-eager-no-cjit` mode fails executable allocations at random; `bun:ffi`'s thunks have no fallback tier and throw `RangeError: Out of memory`. Same opt-out as `wasm-loop-consistency.js` |
| `stress/buffer-accessor-jit-byteoffset.js`, `-fractional-value.js`, `-large.js` | `$skipModes << :lockdown` | they check `numberOfDFGCompiles()`; lockdown runs with the JIT off |
| `stress/regress-174463162.js` | `$skipModes` for `dfg-eager`, `dfg-eager-no-cjit-validate`, `ftl-eager`, `ftl-eager-no-cjit`, `no-cjit-collect-continuously` | its `$vm` helper scribbles a live cell's header; a collection before the script ends marks a cell with no Structure. Only the `--collectContinuously=true` modes collect that early. Upstream's bots hit it too |
| `wasm/regress/298930.js` | `skip if $asan` | ASan's fake stack moves `ConstExprInterpreter`'s `MarkedArgumentBuffer` off the stack the collector scans |
| `LayoutTests/.../wasm/core/js/simd/simd_f32x4_cmp.wast.js` | `skip if $asan` | `RunLoopGeneric`: the collector thread starts a timer while it is in `fired()` without the lock; `ASSERTION FAILED: !isScheduled()`. Only assertion builds see it, and of our lanes those are the asan ones |
| `wasm/stress/memory64-overflow.js` | `slow!` | past the 300 s hard timeout under ASan in `wasm-collect-continuously` |
| `stress/function-toString-native-one-line.js` | none; test fixed | its walker was a top-level `function`, so a property of `globalThis`, and its own source contains the `[native code]` it looks for. It is a `const` |

`$asan` and `slow!` are fork additions to the runner, see D.

---

## C. Known gaps: skipped with `TODO(bun)`

| file(s) | what is missing |
|---|---|
| `microbenchmarks/parse-line-comment.js`, `stress/class-subclassing-function.js` | `SourceCodeKey::operator==` does not compare source text (3186362fe1a8). Two sources of the same length, flags, name and host whose 24-bit `StringImpl::hash()` collide share a code cache entry, and the second runs the first's code. 3 collisions in 20,000 same-length sources measured |
| `microbenchmarks/regexp-buffer-boundary-anchor-start.js`, `-anchor-end.js`, `stress/regexp-boundary-assertions.js`, `stress/regexp-buffer-boundaries-anchoring.js` | upstream's RegExp buffer boundaries (`\A \z \Z`, 2f66f5ed23f9). Sync #455 brought the tests but not the `yarr/` changes: no feature, no `--useRegExpBufferBoundaries` |
| `stress/module-loader-promise-then-tampered.js` | `globalFuncImportModule()` wraps the loader's promise and resolves the wrapper through `resolve()`, which looks up `Promise.prototype.then`. The fast path that avoided it (8a5ce3999589) is unreachable since the loader rewrite (4a638109b905) |

---

## D. Runner scripts

- `Tools/Scripts/run-javascriptcore-tests`: `--no-slow` and `--asan`, passed on to `run-jsc-stress-tests`; the port lookup
  also runs wherever `--jsc-only` is given (so the JSCOnly port can be tested on macOS and Windows, where `jsc` is
  `<root>/bin/jsc`).
- `Tools/Scripts/run-jsc-stress-tests`: `--asan` sets `$asan`, for `//@ skip if $asan`.
- `Tools/Scripts/webkitdirs.pm`: on Windows the machine's architecture is read from the registry (no `uname`; an emulated
  x64 perl on Windows-on-ARM reports `AMD64`); `ARM64` as cmake on Windows spells it is `arm64`.

---

## E. After a sync: a failing test, step by step

1. Does the difference match A1–A10 exactly (same text, same shape)? Check that the fork code named there is still in the
   tree, then update the expectation with a `// Bun:` comment and add the file to its list here.
2. Does it fail only in one mode or only with ASan, for a reason in the test and not the engine (it counts JIT compiles,
   depends on allocation never failing, needs more than 300 s)? Narrow it as in B.
3. Is it a new upstream test for a feature the sync did not bring over (compare `git log upstream/main -- Source/` for
   the commit that added the test)? Bring the feature over, or skip with `TODO(bun)` naming the upstream commit, as in C.
4. Otherwise the sync broke something. Upstream's own results for the same test and mode:
   `https://results.webkit.org/api/results/javascriptcore-tests/<url-encoded test.mode>?limit=5000`.
