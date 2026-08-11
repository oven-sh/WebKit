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
regex-fuzz.js). Configs: jit, interp (--useRegExpJIT=0), gatesoff, nolb, nofactor, nodispatch,
nodfg, eager, asan (WebKitBuild/DebugASAN), base (a baseline jsc: $YARR_FUZZ_BASE_JSC), node.

`regex-fuzz.js` also runs standalone: `jsc --useDollarVM=1 regex-fuzz.js -- <seed> <count> [profile]`
or `node regex-fuzz.js <seed> <count> [profile]`; one JSON line per case, identical across engines
when they agree.

`run262.mjs` runs the RegExp-related test262 subset against a jsc; `bench.js` is the small
benchmark set quoted in the PR (isbot, keyword scans, lookbehind, Boyer-Moore, small test() loops).
