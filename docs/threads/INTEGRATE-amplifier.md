# RaceAmplifier integration manifest

Paste-ready edits that wire `Source/JavaScriptCore/runtime/RaceAmplifier.{h,cpp}`
into the build and option machinery. **The Stub phase merges these** — the
amplifier workstream does not edit `OptionsList.h`, `Sources.txt`, or `VM.cpp`
itself, to keep the parallel workstreams' file sets disjoint.

The actual perturbation call sites (slow paths only) land later with the
workstream that owns each file; the intended call-site list is documented in
the header comment of `runtime/RaceAmplifier.h` and in
`docs/threads/AMPLIFIER.md`.

## 1. `Source/JavaScriptCore/runtime/OptionsList.h`

Insert the following three lines next to the existing fuzzer options, i.e.
immediately after this existing line (around line 498):

```cpp
    v(Bool, useRandomizingFuzzerAgent, false, Normal, nullptr) \
```

paste:

```cpp
    v(Unsigned, randomYieldPeriod, 0, Normal, "If non-zero, the RaceAmplifier injects a randomized sched_yield/short sleep on average once every this many visits per thread to instrumented slow-path sites. 0 disables (default; zero cost when off)."_s) \
    v(Unsigned, randomYieldSeed, 0, Normal, "Seed for the RaceAmplifier's per-thread PRNGs. 0 (default) picks a cryptographically random seed and dataLogs it so the run can be replayed with --randomYieldSeed=<seed>."_s) \
    v(Unsigned, randomYieldMaxMicroseconds, 100, Normal, "Upper bound, in microseconds, for the short sleeps the RaceAmplifier injects (about 1 in 4 perturbations sleep; the rest sched_yield)."_s) \
```

Notes:
- All three are `Unsigned`. There is no 64-bit option type in `OptionsList.h`;
  `RaceAmplifier.cpp` deliberately keeps the derived seed to 32 bits so the
  logged value round-trips through `--randomYieldSeed` exactly.
- Option name spelling must match `RaceAmplifier.cpp`, which reads
  `Options::randomYieldPeriod()`, `Options::randomYieldSeed()`, and
  `Options::randomYieldMaxMicroseconds()`.

## 2. `Source/JavaScriptCore/Sources.txt`

Insert (alphabetical within the `runtime/` block; `RaceAmplifier` sorts before
`RandomizingFuzzerAgent`), i.e. between these two existing lines:

```
runtime/ProxyRevoke.cpp
runtime/RandomizingFuzzerAgent.cpp
```

paste:

```
runtime/RaceAmplifier.cpp
```

## 3. `Source/JavaScriptCore/runtime/VM.cpp`

Arm the amplifier once Options are finalized. Add the include:

```cpp
#include "RaceAmplifier.h"
```

and in the `VM::VM(VMType, HeapType, ...)` constructor body, immediately after
the `vmCreationShouldCrash || g_jscConfig.vmCreationDisallowed` check (before
"// Set up lazy initializers."), paste:

```cpp
    // Arm the race amplifier (no-op unless --randomYieldPeriod is set).
    // Idempotent across VM constructions; see runtime/RaceAmplifier.h.
    RaceAmplifier::initialize();
```

`RaceAmplifier::initialize()` is `std::call_once`-guarded and reads only
finalized Options, so calling it from every VM construction is safe.
`RaceAmplifier::perturb()` is safe to call before `initialize()` (it does
nothing), so call-site patches and this manifest can land in either order.

## 4. Verification after the merge

```sh
# Disabled by default: no output change, no perturbation.
jsc --validateOptions=1 -e 'print(1)'

# Enabled: must dataLog one "[RaceAmplifier] enabled: period=... seed=..." line.
jsc --randomYieldPeriod=64 -e 'print(1)'

# Replay determinism: same seed twice must log the same seed line.
jsc --randomYieldPeriod=64 --randomYieldSeed=12345 -e 'print(1)'
```

Then run the harness end-to-end on any `JSTests/threads/` test:

```sh
Tools/threads/amplify.sh --runs 20 ./WebKitBuild/Debug/bin/jsc JSTests/threads/smoke.js
```
