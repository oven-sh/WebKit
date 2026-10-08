# Yarr differential fuzzer

Grammar-based RegExp generator plus a driver that runs every generated (pattern, flags, subject)
case through several `jsc` configurations and node, and reports any difference in observable
results (exec/test/match/matchAll/replace/split/search, sticky and lastIndex sweeps, 8-bit and
forced 16-bit subjects, plus engine-independent metamorphic checks).

    # 200 seeds x 250 patterns of the "unicode" profile, JIT vs interpreter vs all-new-gates-off vs node
    node drive.mjs --seeds 1-200 --count 250 --profile unicode --par 8 --configs jit,interp,gatesoff,node --out out

    # summarize node differences into known classes (V8 bugs, intentional divergences) and residuals
    node triage.mjs out node

Profiles: mixed, lookbehind, alt, unicode, bm, deep, fold, small, strings, wide (see PROFILES in
regex-fuzz.js). Configs: jit, interp (--useRegExpJIT=0), linear (--useRegExpLinearEngine=1),
linear-interp (the same with --useRegExpJIT=0), linear-verify (the same with
--verifyRegExpLinearEngine=1: the bytecode interpreter checks every match of the matcher, and a
difference is a crash), gatesoff, nolb, nofactor, nodispatch, nodfg, eager, asan
(WebKitBuild/DebugASAN), base (a baseline jsc: $YARR_FUZZ_BASE_JSC), node.

A backtracking engine that reaches its match limit answers "no match", and the matcher of the
`linear` configurations has no such limit. So a `jit~linear` difference where the `jit` side took
seconds is that limit, and not a bug. `linear-triage.js` tells the two apart:
`jsc --useDollarVM=1 --useRegExpLinearEngine=1 linear-triage.js -- out/mismatch-mixed.jsonl` runs
the matcher and the bytecode interpreter on every prefix of each subject, and on every search of a
global loop over it. It reports where they agree, where the interpreter gives up, and where they
differ (a bug).

`regex-fuzz.js` also runs standalone: `jsc --useDollarVM=1 regex-fuzz.js -- <seed> <count> [profile]`
or `node regex-fuzz.js <seed> <count> [profile]`; one JSON line per case, identical across engines
when they agree.

`run262.mjs` runs the RegExp-related test262 subset against a jsc; `bench.js` is the small
benchmark set quoted in the PR (isbot, keyword scans, lookbehind, Boyer-Moore, small test() loops).

Two more oracles, for angles a plain engine differential cannot cover:

- `mutate.mjs --in <corpus.json|dir> --out cases --count N` takes real-world patterns (a JSON corpus of
  {source, flags}) and applies AST-located mutations (wrap in lookarounds/groups, toggle greediness,
  add quantifiers/backreferences/alternatives, flip flags, /v string classes and set operations, ...),
  emitting `{p, f, s:[subjects]}` lines that `regex-fuzz.js` replays with `profile file:<path>` (so
  `drive.mjs --profile file:cases/all.jsonl ...` runs them through every configuration).
- `refmatch.mjs` is a spec-literal (ECMA-262 22.2.2, continuation-passing) reference matcher for the
  subset without /i, /v and property escapes; `refgen.mjs` attaches its verdict (and V8's) to
  generated cases, `refrun.js` (run under jsc) reports where the engine disagrees with the reference,
  and `refcheck.mjs` validates the reference against the host engine. This catches "JIT and
  interpreter agree but both are wrong".

`mutate.mjs`, `refmatch.mjs`, `refgen.mjs` and `refcheck.mjs` need `@eslint-community/regexpp` (and
`mutate.mjs` also `randexp`) installed next to them.
