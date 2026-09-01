# mc-jit-delete-reuse-stale-offset — execution diagnosis (2026-06-15)

## Result: INCONCLUSIVE for MC-JIT/S2(c); test masked by an unrelated runtime crash

### What happened

GIL-off ASAN Debug, default tiers: 5/5 runs crash with rc=134 (SIGABRT) on

```
ASSERTION FAILED: nextOffset == structure->transitionOffset()
Source/JavaScriptCore/runtime/Structure.cpp(717) : PropertyTable *JSC::Structure::materializePropertyTable(VM &, bool)
```

before the test's I21 cross-property-aliasing oracle (line 101-108) is ever
reached. Logs: `mc-jit-delete-reuse-stale-offset.CRASH.log`.

### Discriminator: NOT the MC-JIT mechanism

`--useJIT=0`: 5/5 identical crash, same assertion, same site. Log:
`mc-jit-delete-reuse-stale-offset.CRASH-nojit.log`.

The S2(c) mechanism under test is "hoisted DFG/FTL CheckStructure proof
survives a poll across the quarantine-epoch bump". With JIT disabled there is
no hoisted proof to survive anything; LLInt re-dispatches every opcode (map
S8). The crash therefore is **not** caused by stale-JIT-proof aliasing.

### What the crash IS

`materializePropertyTable` replays the structure transition chain into a fresh
PropertyTable. The assertion at :717 says a `PropertyAddition` link's recorded
`transitionOffset()` does not match what the freshly-built table computes as
the next free offset — i.e. the transition CHAIN is internally inconsistent.

The test's traffic that produces this chain, all on the same object:

- writer thread (LLInt slow path under `--useJIT=0`): `o.f = F_BASE+i` —
  after main's `delete o.f`, this is a slow-path put that **re-adds** `f`
  via a PropertyAddition transition;
- main thread: `delete o.f` (PropertyDeletion transition), then
  `o["g"+r] = ...` (PropertyAddition), then `o.f = F_BASE` (another
  PropertyAddition).

Two threads concurrently performing PropertyAddition transitions on the same
object (writer re-adding `f` vs main adding `g_r` / re-adding `f`) produced a
chain whose deleted-offset reuse / nextOffset bookkeeping diverged. This is a
**runtime locked-transition serialization** bug (OM §4.3 / SPEC-objectmodel
I15/I29/D1 territory — quarantine-slot reuse vs concurrent add), not MC-JIT.
It belongs to mechanism class MC-INIT or MC-TEAR (structure-transition
ordering), not "JIT proof outlives a structural change".

### MC-JIT/S2(c) governing-invariant status (source inspection)

The premise in the input verdict ("CheckTraps writes only InternalState") is
**stale**: the AUDIT-checktraps `checktraps-dejank-invalidation-point` fix has
landed. `dfg/DFGClobberize.h:809-905` (`case CheckTraps`) GIL-off now:

- emits an InvalidationPoint-shaped jump-replacement at the poll rejoin;
- `write(Watchpoint_fire)`, `write(NamedProperties)`, `write(Butterfly_publicLength)`;
- every conducted stop window bumps `s_conductorHeapFactRewriteEpoch`
  in-window (`bytecode/JSThreadsSafepoint.cpp:289,401,423,458,648,667` —
  BUMP-EDGE LAW), and a parked mutator whose epoch sample changed jettisons
  its on-stack optimizing code on resume (`VMTraps::handleTraps`).

S2(c)'s falsifier — quarantine-epoch promotion — happens at a collection stop,
which is a stop window that bumps the epoch. A compiled `fStorm` parked
through that stop OSR-exits at the poll rejoin before re-using its hoisted
`CheckStructure(S_old)` proof. So by source inspection S2(c) is closed by the
same mechanism that closes S2(a)/(b). **Cannot be empirically confirmed**
until the masking transition-chain assertion is fixed.

### Repro

```
WebKitBuild/Debug/bin/jsc --useJSThreads=1 --useThreadGIL=0 --useVMLite=1 \
  --useSharedAtomStringTable=1 --useSharedGCHeap=1 --useThreadGILOffUnsafe=1 \
  --useDollarVM=1 --useJIT=0 \
  JSTests/threads/cve/mc-jit-delete-reuse-stale-offset.js
```

5/5 SIGABRT at Structure.cpp:717. Reproduces with and without `--useJIT=0`,
with and without amplifier.
