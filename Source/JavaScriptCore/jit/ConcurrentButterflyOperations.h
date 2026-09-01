/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "JSCJSValue.h"
#include "OperationResult.h"

namespace JSC {

class JSObject;
class VM;

// ===========================================================================
// P5: per-thread butterfly TID tag (SPEC-jit R5/P5; THREAD.md regime-1
// flat-butterfly tagging).
//
// Value: uint64_t(currentButterflyTID()) << 48 - the pre-shifted tag the
// write/transition fast paths of every tier compare against the tagged
// butterfly word (SPEC-jit section 5.5 predicates (2)/(3)/transition; reads
// never compare the TID). Zero-initialization is correct ONLY for the main
// thread (TID 0), which is why initializeButterflyTIDTagForCurrentThread()
// is MANDATORY on thread attach (CS3/I19) and the clear on detach.
//
// Task 1 lands the storage and the init/clear/CS3 hook; Task 1b lands the
// per-platform JIT/LLInt load mechanics (SPEC-jit-annex App. R5: ELF
// initial-exec TLS offset baked at emission; Darwin pthread key cached in the
// M4a JSCConfig slot; Windows unsupported flag-on).
// ===========================================================================

#if OS(LINUX)
// ELF (glibc + musl): initial-exec model is REQUIRED so .so builds keep a
// thread-invariant TLS-base offset that the JIT tiers can bake as an
// immediate (App. R5).
extern "C" __attribute__((tls_model("initial-exec"))) thread_local uint64_t g_jscButterflyTIDTag;
#else
// Darwin: this thread_local serves C++ readers only; Mach-O TLV has no
// constant offset, so JIT/LLInt reads go through the pthread key landed in
// Task 1b (App. R5). Windows: unsupported flag-on (D8).
extern "C" thread_local uint64_t g_jscButterflyTIDTag;
#endif

ALWAYS_INLINE uint64_t butterflyTIDTagForCurrentThread() { return g_jscButterflyTIDTag; }

// Install the current thread's pre-shifted TID tag (and, first time through,
// register the CS3 VMLite hook so lazy VM-lite installs / multi-VM switches /
// detaches keep the tag coherent, I19). Idempotent; called on thread
// attach (api section 5.2 spawn path) and from CS3's setCurrent hook.
JS_EXPORT_PRIVATE void initializeButterflyTIDTagForCurrentThread();

// Reset the tag to 0 on thread detach.
JS_EXPORT_PRIVATE void clearButterflyTIDTagForCurrentThread();

// ===========================================================================
// Task 1b (SPEC-jit R5; annex App. R5): per-platform mechanics for the
// one-load JIT/LLInt read of g_jscButterflyTIDTag.
// ===========================================================================

#if OS(LINUX) && (CPU(X86_64) || CPU(ARM64))
// ELF initial-exec TLS: the thread-invariant offset of g_jscButterflyTIDTag
// from the thread pointer (%fs base on x86-64, TPIDR_EL0 on arm64). Computed
// and cached by the first initializeButterflyTIDTagForCurrentThread(); every
// subsequent thread's init recomputes and RELEASE_ASSERTs it unchanged
// (App. R5 constancy assert). Emitters bake it as an immediate via
// MacroAssembler{X86_64,ARM64}::loadFromELFTLS64(offset, dst); LLInt's
// loadButterflyTIDTag macro (Task 8) uses the equivalent link-time
// initial-exec relocations instead and does not consume this accessor.
// Valid only after P5 init has run at least once on this process.
JS_EXPORT_PRIVATE intptr_t butterflyTIDTagELFTLSOffset();
#endif

#if OS(DARWIN)
// Darwin: Mach-O TLV has no constant offset, so the JIT-visible copy of the
// tag lives in a pthread_key_create'd TSD slot (slots are uniform, so a
// dynamic key's slot sits at a constant offset from the thread base). The
// key is created at P5 init, cached here, and mirrored into the M4a
// JSCConfig slot (g_jscConfig.butterflyTIDTagTLSKey) for LLInt when that
// slot exists. JIT tiers emit
// loadFromTLS64(fastTLSOffsetForKey(butterflyTIDTagTLSKey()), dst), offset
// baked at emission. Valid only after P5 init has run at least once.
JS_EXPORT_PRIVATE uint32_t butterflyTIDTagTLSKey();
#endif

// I19 coherence check: RELEASE_ASSERTs that g_jscButterflyTIDTag (and, where
// a separate JIT-visible copy exists, that copy too) equals
// uint64_t(currentButterflyTID()) << 48. Wired at VM entry in debug builds
// via the INTEGRATE-jit.md hunk (runtime/** is not jit-ownable); also run by
// the 3-thread test path on every P5 init.
JS_EXPORT_PRIVATE void assertButterflyTIDTagCoherent();

#if ENABLE(JIT)

// ===========================================================================
// R3: JIT-operation shims over the object-model workstream's helpers
// (runtime/ConcurrentButterfly.h, SPEC-objectmodel section 9). Thin
// JSC_DEFINE_JIT_OPERATION wrappers; the JIT never implements butterfly
// regime semantics itself (SPEC-jit section 5.5).
//
// Task 8 disposition: the LLInt and Baseline/IC choke points
// (LowLevelInterpreter64.asm threadedButterfly*Predicate macros;
// CCallHelpers::loadButterflyForRead/ForWrite) route EVERY predicate failure
// to the op's EXISTING generic slow path, whose C++ object access goes
// through the OM's regime-aware paths once OM Tasks 4-8 land - so these
// dedicated shims stay unreferenced by Task 8 emission. They are the
// DFG/FTL tail-call form (Tasks 9/10), at which point the bodies forward to
// the OM helpers (segmentedOutOfLineSlot / ensureSharedWriteBit / locked AS
// ops). Until then they are unreachable asserts.
// THREADS-INTEGRATE(jit): forwarding bodies are completed against
// runtime/ConcurrentButterfly.h definitions (OM Tasks 4-8) when both
// workstreams' .cpps are in one tree (declaration-only today => referencing
// them now would be an undefined symbol); signatures below are the frozen
// call-side contract.
// ===========================================================================

// Segmented (regime-2) out-of-line property access: dependent load/store
// through the spine (SPEC-jit section 5.5 read/write predicate, top16 ==
// 0xFFFF case). `offset` is a PropertyOffset into out-of-line storage.
JSC_DECLARE_JIT_OPERATION(operationSegmentedButterflyLoad, EncodedJSValue, (VM*, JSObject*, int32_t offset));
JSC_DECLARE_JIT_OPERATION(operationSegmentedButterflyStore, void, (VM*, JSObject*, int32_t offset, EncodedJSValue));

// Segmented indexed-element access (array variants of the above).
JSC_DECLARE_JIT_OPERATION(operationSegmentedButterflyIndexedLoad, EncodedJSValue, (VM*, JSObject*, uint32_t index));
JSC_DECLARE_JIT_OPERATION(operationSegmentedButterflyIndexedStore, void, (VM*, JSObject*, uint32_t index, EncodedJSValue));

// First foreign write to a flat non-ArrayStorage butterfly: sets the SW bit
// via the OM's fire-then-DCAS protocol (forwards to
// ensureSharedWriteBit(VM&, ...)). The caller then performs its store
// (write predicate case (4), non-AS arm).
JSC_DECLARE_JIT_OPERATION(operationButterflyEnsureSharedWrite, void, (VM*, JSObject*));

// Locked ArrayStorage/SlowPut ops (AS-rule, SPEC-jit section 5.5 / OM I31):
// AS never segments; flag-on, any access reachable with SW=1 takes the cell
// lock. The store variant of case (4) fires F1, flips SW, and performs the
// write ITSELF under the cell lock / per-event-STW regime - the JIT must
// NEVER ensure-SW-then-store inline for AS shapes (I20).
JSC_DECLARE_JIT_OPERATION(operationSharedArrayStorageLoad, EncodedJSValue, (VM*, JSObject*, uint32_t index));
JSC_DECLARE_JIT_OPERATION(operationSharedArrayStorageStore, void, (VM*, JSObject*, uint32_t index, EncodedJSValue));

#endif // ENABLE(JIT)

} // namespace JSC
