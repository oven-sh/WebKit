## Post-wave hardening item (security review, 2026-06-12) — DONE (2026-06-12)
Landed: perLiteMode final farJump block in DFGThunks.cpp osrExitGenerationThunkGenerator wrapped in
`#if USE(JSVALUE64)`; `#else` arm fail-stops with `RELEASE_ASSERT(!perLiteMode)` and emits the
shared-VM farJump, mirroring the firstGPR convention. Preprocessor-only change: JSVALUE64 builds
take the identical code path (flag-off identity preserved by construction). Build verification
deferred to the post-wave builder pass per write-only discipline.

DFGThunks.cpp OSR-exit thunk: the perLiteMode final farJump block (~L240-248) is not under
`#if USE(JSVALUE64)` while the slot-initializing guard block (L167-202) is. Unreachable today
(gilOff is 64-bit-only; all Bun targets JSVALUE64) but on a hypothetical JSVALUE32_64 build with
perLiteMode it would jump through an uninitialized scratch slot. Fix after the wave gates: wrap the
perLiteMode jump block in `#if USE(JSVALUE64)`, add `RELEASE_ASSERT(!perLiteMode)` in the #else arm
(mirrors the existing convention at L131). One-line risk, do not hand-edit while wave owns the file.
