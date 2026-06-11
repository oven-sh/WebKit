/*
 * Copyright (C) 2012-2023 Apple Inc. All rights reserved.
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

#include "LazyOperandValueProfile.h"
#include <utility>
#include <wtf/Atomics.h>

namespace JSC {

class ScriptExecutable;
class CodeBlock;

class LazyOperandValueProfileParser;

class CompressedLazyValueProfileHolder {
    WTF_MAKE_NONCOPYABLE(CompressedLazyValueProfileHolder);
public:
    CompressedLazyValueProfileHolder() = default;

    void computeUpdatedPredictions(const ConcurrentJSLocker&, CodeBlock*);

    LazyOperandValueProfile* addOperandValueProfile(const LazyOperandValueProfileKey&);
    JSValue* addSpeculationFailureValueProfile(BytecodeIndex);

    UncheckedKeyHashMap<BytecodeIndex, JSValue*> speculationFailureValueProfileBucketsMap();

private:
    friend class LazyOperandValueProfileParser;

    inline void initializeData();

    struct LazyValueProfileHolder {
        WTF_MAKE_STRUCT_TZONE_ALLOCATED(LazyValueProfileHolder);

        // THREADS §5.7.4/§5.7.7: `second` is the by-ref extra-value speculation-failure
        // profile slot. It is racily written by OSR-exit/JIT'd code (plain aligned 64-bit
        // stores, allowed to stay plain) and racily read+cleared by DFG compiler threads via
        // ValueProfileBase::computeUpdatedPredictionForExtraValue — on 64-bit
        // valueProfileLock() is NoLockingNecessaryTag, so that reader can run concurrently
        // with construction of later elements and with other compiler threads. Tolerance
        // contract: a racing sample may be lost; profiles only SELECT speculation and
        // emitted guards validate. So every C++ access to the slot — including its
        // INITIALIZATION here — must be a relaxed atomic (64-bit) or use the JSCJSValue.h
        // tag/payload concurrent protocol (32-bit); a plain constructor store (e.g.
        // JSValue's default constructor) would be the plain side of a race with a
        // concurrent relaxed reader. The anonymous union leaves the storage uninitialized
        // so the constructor body can perform the only — relaxed — initializing store.
        // Relaxed = plain moves on x86-64/arm64: flag-off codegen unchanged.
        // This is a drop-in replacement for std::pair<BytecodeIndex, JSValue> (members are
        // named first/second and it converts from that pair type).
        struct SpeculationFailureValueProfileBucket {
            // ConcurrentVector segments default-construct their backing array; those
            // slots are only ever read after appendConcurrently placement-news a real
            // element over them, so the default constructor deliberately leaves the
            // union storage uninitialized (no plain store to the racy slot).
            SpeculationFailureValueProfileBucket() { }

            SpeculationFailureValueProfileBucket(std::pair<BytecodeIndex, JSValue> pair)
                : first(pair.first)
            {
                static_assert(sizeof(JSValue) == sizeof(EncodedJSValue));
#if USE(JSVALUE64)
                WTF::atomicStore(std::bit_cast<EncodedJSValue*>(&second), JSValue::encode(pair.second), std::memory_order_relaxed);
#else
                updateEncodedJSValueConcurrent(*std::bit_cast<EncodedJSValue*>(&second), JSValue::encode(pair.second));
#endif
            }

            BytecodeIndex first; // Written once at append time on the owning mutator (addSpeculationFailureValueProfile asserts !isCompilationThread()); immutable afterwards.
            union {
                JSValue second; // THREADS: all C++ accesses, including construction, go through relaxed atomics / the concurrent tag-payload protocol (see above).
            };
        };

        ConcurrentVector<LazyOperandValueProfile, 8> operandValueProfiles;
        ConcurrentVector<SpeculationFailureValueProfileBucket, 8> speculationFailureValueProfileBuckets;
    };

    LazyValueProfileHolder* dataConcurrently() const
    {
        // Acquire pairs with initializeData()'s release publication so compiler
        // threads see the holder (and its ConcurrentVectors) fully constructed.
        return WTF::atomicLoad(std::bit_cast<LazyValueProfileHolder**>(const_cast<std::unique_ptr<LazyValueProfileHolder>*>(&m_data)), std::memory_order_acquire);
    }

    std::unique_ptr<LazyValueProfileHolder> m_data;
};

class LazyOperandValueProfileParser {
    WTF_MAKE_NONCOPYABLE(LazyOperandValueProfileParser);
public:
    LazyOperandValueProfileParser() = default;

    void initialize(CompressedLazyValueProfileHolder&);

    LazyOperandValueProfile* NODELETE getIfPresent(const LazyOperandValueProfileKey& key) const;

    SpeculatedType prediction(const ConcurrentJSLocker&, const LazyOperandValueProfileKey&) const;
private:
    UncheckedKeyHashMap<LazyOperandValueProfileKey, LazyOperandValueProfile*> m_map;
};

} // namespace JSC
