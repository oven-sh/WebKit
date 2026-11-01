# Where this fork's tests differ from upstream's, and why

`JSTests/`, `LayoutTests/js` and the PerformanceTests collections are upstream's tests. This fork changes 129 of them and
three runner scripts. Every change is listed here by the reason for it, so that after an upstream sync a failing test can
be sorted into one of three piles:

1. **The fork behaves differently on purpose.** Update the expectation. Sections A1–A10 list every such behavior, how to
   recognize it in a failure, and the fork code that causes it. If a failure matches one of these and the fork code is
   still there, the test is the thing to change.
2. **The test cannot pass in a mode or build we run.** Skip it in that mode only, with the reason. Section B.
3. **A known gap or bug in the fork.** Skipped with `TODO(bun)`. Section C. Remove the skip when the gap is closed.

Anything that fits none of these is a regression from the sync: fix the engine, not the test. The exception is an
unchanged test whose result depends on the load of the machine: those have a section of their own after C.

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
- Files (11): `ChakraCore/test/` `Array/protoLookup`, `Error/ErrorCtorProps_v3`, `Function/prototype`,
  `Function/toString`, `Lib/toString` (163 functions), `es6/letconst_global_shadow_builtins`, `strict/05.arguments_sm`,
  `typedarray/set` (baselines); `LayoutTests/js/basic-strict-mode` (script and expected text);
  `stress/proxy-revocable-result-and-revoke-function.js` (the revoke function).

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
- Files: `stress/regexp-unicode-code-unit-read-for-bmp-terms.js`, two expectations;
  `stress/regexp-stringlist-eol-length-groups.js`, one.

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

### A10. The bundled ICU is 79.1 (CLDR 49)

- `icu/source.json`. The macOS lanes use the system's ICU, so each test keeps upstream's expectation for ICU older than 79
  (`$vm.icuVersion()`), or adds CLDR 49's text to the list where the test already accepts a list.
- `stress/intl-date-pattern-includes-literal-text.js`: CLDR 49 removed fr's `HH 'h'` pattern: `23 h` → `23`.
- `stress/intl-datetimeformat-resolved-options-am-pm.js`: th's short time pattern is now `HH:mm น.`, and ICU uses it for a
  2-digit hour but not for a numeric one. `resolvedOptions()` reports a 2-digit hour, so the recreated formatter prints
  `10:05 น.` where the original prints `10:05`.
- `stress/intl-extended-timezone-names.js`: CLDR 49 removed zh-Hant's short names for America/Los_Angeles: `PDT` → `GMT-7`,
  `PT` → `洛杉磯時間`.

---

## B. The test cannot pass in a mode or build we run: narrowed, not changed

| file(s) | directive | reason |
|---|---|---|
| 32 × `stress/ffi-*.js` | `--useExecutableAllocationFuzz=false` added to `requireOptions` | the `ftl-eager-no-cjit` mode fails executable allocations at random; `bun:ffi`'s thunks have no fallback tier and throw `RangeError: Out of memory`. Same opt-out as `wasm-loop-consistency.js` |
| `stress/sampling-profiler-bound-function-name.js` | `requireOptions("--useExecutableAllocationFuzz=false")` | with the DFG enabled it waits for the stack `foo < baz`, which exists only once the DFG has inlined the bound function. In the same mode, a process in which the compilations of `baz` and its caller fail sees `foo < bound foo < baz` for the 50 s the test allows itself |
| `stress/sampling-profiler-line-column.js` | `$skipModes` for `ftl-eager`; the lines it waits for are 13 to 18 (upstream: 11 to 16), because the two lines added at the top move `test()` down by two | it waits for a sample of `test()` with a line inside `test()`. A frame of FTL code is sampled with no line (4294967295), so a sample has to fall into the few milliseconds before `test()` is compiled by the FTL; a process that misses them waits its 50 s in vain. 35 of 600 runs on a busy machine with `main`'s own shell (87ee10da), each over in under 5 s or never |
| `stress/buffer-accessor-jit-byteoffset.js`, `-fractional-value.js`, `-large.js` | `$skipModes << :lockdown` | they check `numberOfDFGCompiles()`; lockdown runs with the JIT off |
| `stress/regress-174463162.js` | `$skipModes` for `dfg-eager`, `dfg-eager-no-cjit-validate`, `ftl-eager`, `ftl-eager-no-cjit`, `no-cjit-collect-continuously` | its `$vm` helper scribbles a live cell's header; a collection before the script ends marks a cell with no Structure. Only the `--collectContinuously=true` modes collect that early. Upstream's bots hit it too |
| `stress/regexp-legacy-static-properties-rematch-failure.js` | `$skipModes` for `regexp-flat-program`, `regexp-image-mode` | after a `test()` that matched, it expects the second match, which `RegExp.lastMatch` and the other legacy properties start for the captures, to run out of stack and throw a `RangeError` (upstream 191803bb23). A flat program and the code of an image make that match within `--maxRegExpStackSize=65536`: the properties have their values and `exec()` returns the 1601 entries |
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

## Not changed: the result depends on the load of the machine

Upstream's tests as upstream has them. A failure with the message given here is not a regression.

| file | message | what it depends on |
|---|---|---|
| `stress/new-largeish-contiguous-array-with-size.js` | `Error: heap too big before forced GC: <bytes>` | with `--forceRAMSize=10000000` it allocates 1,000 arrays of 50,000 elements in a loop and then bounds `gcHeapSize()` at 10,000,000 without asking for a collection. The bound holds if the concurrent collector gets processor time before the loop ends. Run alone it passes; the more processes share the processors, the more often it fails |

---

## TestWTF (`Tools/TestWebKitAPI/Tests/WTF`)

Upstream's too, and run by the `test` job.

| file | change | reason |
|---|---|---|
| `Threading.cpp` | `WTF_Thread.SchedulingAttributesFollowQOS` and `.HighPriorityThreadsRestoresClampWhenReenabled` are compiled only without `USE(BUN_JSC_ADDITIONS)` | on purpose, as in A: `Thread::updateSchedulingAttributes()` (`wtf/posix/ThreadingPOSIX.cpp`) does nothing on Linux, so a thread keeps the scheduling policy, nice level and utilization clamp of the process that embeds it. The tests expect upstream's mapping from QOS |
| `StackTraceTest.cpp` | `StackTraceTest.StackTraceWorks` and `.CaptureStackTraceLimitAndSkip` are `DISABLED_` on Linux | the build, as in B: `DEFAULT_CFLAGS` in `Dockerfile` has `-fno-unwind-tables -fno-asynchronous-unwind-tables`, and glibc's `backtrace()` then returns its own frame alone |
| `StringBuilder.cpp` | the `JSONEscape*` tests are the fork's own | they hold `appendEscapedJSONStringContent()` (`wtf/text/StringBuilderJSON.h`), which the fork vectorizes, to a scalar escaper, to its buffer, and to its check that the output has six characters of room for each one of the input |
| `PtrTag.cpp` | the file is the fork's own | `WTF::removeReturnAddressSignature()` (`wtf/PtrTag.h`) gives the return address of a function that signs the one it saves. The function is compiled with `pac-ret` on ARM64 Linux in every build. On a processor without pointer authentication nothing is signed and the test passes whatever the code does |

---

## D. Runner scripts

- `Tools/Scripts/run-javascriptcore-tests`: `--no-slow` and `--asan`, passed on to `run-jsc-stress-tests`; the port lookup
  also runs wherever `--jsc-only` is given (so the JSCOnly port can be tested on macOS and Windows, where `jsc` is
  `<root>/bin/jsc`).
- `Tools/Scripts/run-jsc-stress-tests`: `--asan` sets `$asan`, for `//@ skip if $asan`.
- `Tools/Scripts/webkitdirs.pm`: on Windows the machine's architecture is read from the registry (no `uname`; an emulated
  x64 perl on Windows-on-ARM reports `AMD64`); `ARM64` as cmake on Windows spells it is `arm64`.

---

## The fork's own tests

Not upstream's: a sync neither brings nor changes them. Most are named `stress/bun-*.js`. Regression tests for fixes meant to go upstream are named the way upstream names its tests.

| file | what it holds the engine to |
|---|---|
| `stress/bun-async-context-propagation.js` | the embedder's async-context slot is captured when a job is scheduled and current while it runs |
| `stress/bun-aot-builtins-table.js` | the ahead-of-time compiler's table of built-ins (`Source/JavaScriptCore/aot/builtins/*.builtins.ts`) agrees with the engine. **After a sync that removes, renames, moves or reimplements a built-in that the table knows this fails, and each line it prints names the entry to fix.** |
| `stress/sort-scratch-double-pivot.js` | double arithmetic in the comparator of an inlined sort stores no double into an Int32 array |
| `stress/array-sort-inline-int32-double-comparator.js` | an earlier, smaller test for the sort fix |
| `stress/bun-object-assign-calls-indexed-setters-on-the-chain.js` | `Object.assign()` stores with `[[Set]]` on its fast path too: an indexed setter on the target's prototype chain is called, and a read-only element there refuses the store |
| `stress/bun-arrays-become-prototypes-below-immutable-prototypes.js` | with `Object.prototype` and `Array.prototype` immutable and a setter under an index on a class prototype, an array or an object with elements that becomes a prototype keeps the storage it has; reads, stores and changes of length on it and through its heirs answer as the language says |
| `stress/function-prototype-call-not-called-as-a-method.js` | `f.call(...)` is compiled to a call of `f`, so only other ways reach `Function.prototype.call` itself: `call.bind(f)`, `Reflect.apply`, `call.call` and `call.apply`. Through each of them `this`, the arguments and `new.target` arrive as they should, and a callee that is not callable is a `TypeError`, in every tier |
| `stress/regexp-greedy-group-in-tail-position.js` | a greedy `*` or `+` group that nothing can fail after runs without the state of its earlier iterations: `exec()`, `match()` and `replace()` give the listed results |
| `stress/regexp-greedy-group-with-minimum-can-match-empty.js` | `YarrPattern` drops the minimum of a greedy group that has no maximum and whose body can match the empty string: the results are the same as with the minimum |
| `stress/regexp-long-source.js` | the limit of 1 MiB on the source of a pattern does not include the text of character classes made only of characters and ranges, up to 4 MiB in all. Everything else is "too large" where it was before |
| `stress/regexp-lookaround-releases-paren-contexts.js` | YarrJIT frees the state of the groups in a lookaround once the lookaround has matched: a search over a long input finds its match, where it used to run out of stack and report none |
| `stress/regexp-many-single-character-alternatives.js` | a long run of neighbouring one-character alternatives becomes several character classes: matches are right at the sizes around each split, and at 50,000 alternatives, where finishing one class was quadratic |
| `stress/regexp-repeated-group-saves-the-alternative-that-matched.js` | before each iteration of a repeated group YarrJIT saves only the frame slots of the alternative that matched the iteration before: patterns that backtrack into earlier iterations give the listed results |
| `stress/regexp-repeated-group-with-many-alternatives.js` | a repeated group of up to 16,000 capturing alternatives matches, and only the last iteration's capture is set. Compiling such a group was quadratic |
| `stress/regexp-tail-loop-with-many-alternatives.js` | the pass that lets a tail loop match a run of characters stops after a fixed amount of work: matches are the same whether or not it reaches the end of the loop, up to 12,000 alternatives and with classes of a million code points |
| `stress/regexp-bytecode-of-deeply-nested-fixed-count-groups.js` | the bytecode compiler of regular expressions takes a time in proportion to the depth of a nest of groups with a fixed count: no more than three times what it takes for lazy groups, at two depths |
| `stress/regexp-flat-program-counts-turns-of-alternatives-tried-once.js` | a pattern with a `^` in one alternative and none in another: the flat program that counts turns (`yarr/YarrFlatProgram*`) walks the alternatives that are tried once and their copies in the bytecode's order, and counts the same turns |
| `stress/regexp-flat-program-many-groups-and-surrogates.js` | in unicode mode the flat program skips positions where the first characters cannot match, counting a surrogate pair as one character, and after a failed attempt resets only the captures the attempt can have written: results match the bytecode interpreter |
| `stress/regexp-flat-program-runs-before-back-references.js` | a run in front of a back reference: the flat program knows what the reference begins with, keeps no undo frame for a capture that is set again before anything reads it, and hands a long search to a second program that goes from one possible end of the run to the next: results match the bytecode interpreter |
| `stress/regexp-image-mode-skips-starts-behind-a-failed-run.js` | the code a build makes for a pattern, and a flat program: once a run without an upper bound has failed at all its ends, the next attempt begins behind the starts that would only enter the same run, and with several alternatives the run remembers how far it failed: results match the sticky form asked at one place after the other |
| `stress/regexp-lazy-run-that-stops-at-its-first-end.js` | where a pattern is analyzed, a lazy run in front of one unit becomes a run that never goes back (`.*?\]` as `[^\]]*\]`) if nothing stands behind the unit, or a run that takes all the first takes: results match the same pattern with an upper bound on the run, which is left as written |
| `stress/regexp-reference-to-a-group-that-reads-nothing.js` | flat programs: a reference to a group that holds only a lookahead reads nothing, so what stands in front of the reference is not limited to what the lookahead accepts; a group still open at the end of the input has no first unit to read |
| `stress/regexp-one-set-or-run.js` | flat programs: a pattern that is one set, or a run of one set, answers as the same pattern on the machine does |
| `stress/regexp-flat-program-memoizes-unbounded-runs.js` | the flat program memoizes where the rest of an unbounded run has failed, keys entries only by the capture bounds a backreference can still read, and records a failed search under the bounds it started with: results and turn counts match the bytecode interpreter |
| `stress/regexp-flat-program-remembers-failed-searches-beside-references.js` | beside references the flat program's table of failed searches has an entry for each set of the referenced groups' bounds: results and turns are the bytecode's |
| `stress/regexp-flat-program-texts-behind-runs.js` | a flat program whose start scan is selective (`\/[^\/]*\.log`) looks once for the text that every match holds behind a run without a bound, when its attempts have come 128 units without a match and as many are left: results are those of a search written out by hand, and the bytecode's |
| `stress/regexp-flat-program-alternative-at-input-end.js` | a flat program of a pattern with an alternative that begins with `$` (`\/name\|$`) still searches for where a match can begin and for the text that the other alternatives hold, and tries the end of the subject last: `(?:R)\|$` finds what R finds, or the end |
| `stress/regexp-flat-program-start-pair.js` | a flat program of a pattern that begins with a text looks through a subject of 64 units or more for two units of the text, a vector at a time, before it asks its start table: it matches where `indexOf` finds the text |
| `stress/regexp-image-code-hands-long-searches-over.js` | with `--useRegExpImageMode=1`, in a `jsc` that has the ahead-of-time compiler: the code of a pattern stops after nine turns a unit and 65,537, and the match is made again by the flat programs, which give it up where the JIT does. In a `jsc` without the compiler the option changes nothing, and both runs hold the JIT to the same answers |
| `stress/switch-string-substring-of-base-widened-by-atomization.js` | a `switch` over strings that reads the characters of a substring in place, in its base, goes by the width that the base has now: atomizing the base may have put an equal 16-bit string in its place since the substring was made (DFG and FTL) |
| `stress/bun-weak-map-and-set-shrunk-before-eden-collection.js` | the buffer of a `WeakMap` or `WeakSet` counts in the heap's size as it is behind a collection (`WeakMapImpl::reconcileWeakReferencesAtGCEnd`; upstream counts it as the last visit found it), but an eden collection never leaves that size below what the collection before it left: a buffer that shrank stops counting at the next full collection |
| `stress/intl-timezone-posix-style.js` | `EST5EDT`, `CST6CDT`, `MST7MDT` and `PST8PDT` stay aliases of their `America/*` zones (tc39/ecma402#1103) on every ICU, including those where CLDR treats them as canonical zones (45 and earlier, 49 and later): accepted, kept as written, not in `supportedValuesOf`, same offsets and names as the primary zone, and resolved to it when they are the host's zone. `CET`, `EET`, `MET` and `WET` are aliases of `Europe/Brussels`, `Europe/Athens`, `Europe/Brussels` and `Europe/Lisbon` in the same way: tzdata before 2024b has them as zones, and the system ICU of a Mac can have such data |
| `stress/temporal-calendar-gregory-near-min-date.js` | the `gregory` calendar agrees with the ISO calendar in the first years of Temporal's range: reading fields, building from fields and formatting (`prolepticGregorianChangeDate`, `runtime/JSDateMath.h`) |
| `stress/inline-cache-stub-freed-during-call-to-custom-setter.js` | an inline cache stub that has called native code stays until the call returns, also when the callee signed the return address that it saved (`JITStubRoutineSet::mark`). The setter of `$vm`'s `customAccessor` is compiled with `pac-ret` on ARM64 Linux in every build. On a processor without pointer authentication nothing is signed and the test passes whatever the code does |
| `stress/bun-return-address-of-function-is-signed.js` | with `ENABLE(RETURN_ADDRESS_SIGNING)` a function signs the return address that it saves in LLInt, Baseline, the DFG and the FTL alike: `$vm.returnAddressOfCallerIsSigned()` signs the address again and compares. Elsewhere, and on a processor without pointer authentication (`$vm.functionsSignReturnAddress()`), the test ends at once |
| `stress/bun-return-address-with-wrong-signature-is-not-returned-to.js` | with `ENABLE(RETURN_ADDRESS_SIGNING)` a function does not return once a bit of the signature of its saved return address was flipped, in each of the four tiers: the run must crash. Where nothing is signed the test crashes by itself |
| the fr-CA block of `stress/intl-date-pattern-includes-literal-text.js` (the file is upstream's) | keeps the file's coverage on CLDR 49, where fr's pattern no longer has literal text: an `h` inside literal text is not mistaken for the hour field |
| the last block of `stress/regexp-test-sticky-first-char-filter.js` (the file is upstream's; nothing of upstream's text changes) | the first-character filter of a sticky `test()` comes from the first sets of the alternatives (`yarr/YarrFirstFollowSets.*`): behind a leading lookbehind, forward reference or lookahead, and in groups nested more than eight deep, a byte that passes the filter still reaches the matcher |

`jsc` cannot compile a program ahead of time by itself (the compiler's input comes from the front end, in Bun's repository), so
the programs that test ahead-of-time compilation are in Bun's `test/aot`.

---

## E. After a sync: a failing test, step by step

1. Does the difference match A1–A10 exactly (same text, same shape)? Check that the fork code named there is still in the
   tree, then update the expectation with a `// Bun:` comment and add the file to its list here.
2. Does it fail only in one mode or only with ASan, for a reason in the test and not the engine (it counts JIT compiles,
   depends on allocation never failing, needs more than 300 s)? Narrow it as in B.
3. Is it a new upstream test for a feature the sync did not bring over (compare `git log upstream/main -- Source/` for
   the commit that added the test)? Bring the feature over, or skip with `TODO(bun)` naming the upstream commit, as in C.
4. Is it listed under "Not changed: the result depends on the load of the machine", with the message given there? Run it
   alone with the flags of the failing mode. If it passes, change nothing.
5. Otherwise the sync broke something. Upstream's own results for the same test and mode:
   `https://results.webkit.org/api/results/javascriptcore-tests/<url-encoded test.mode>?limit=5000`.
