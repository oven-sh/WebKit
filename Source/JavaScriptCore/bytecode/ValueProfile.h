/*
 * Copyright (C) 2011-2023 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 * 3.  Neither the name of Apple Inc. ("Apple") nor the names of
 *     its contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "ConcurrentJSLock.h"
#include "SpeculatedType.h"
#include "Structure.h"
#include "VirtualRegister.h"
#include <span>
#include <wtf/Atomics.h>
#include <wtf/PrintStream.h>
#include <wtf/StringPrintStream.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

class UnlinkedValueProfile;

template<unsigned numberOfBucketsArgument, unsigned numberOfSpecFailBucketsArgument>
struct ValueProfileBase {
    friend class UnlinkedValueProfile;

    static constexpr unsigned numberOfBuckets = numberOfBucketsArgument;
    static constexpr unsigned numberOfSpecFailBuckets = numberOfSpecFailBucketsArgument;
    static constexpr unsigned totalNumberOfBuckets = numberOfBuckets + numberOfSpecFailBuckets;
    
    // THREADS §5.7.4/§5.7.7: profiles can be constructed in storage a concurrent reader is
    // still probing (lazily appended profile vectors observed by compiler threads, allocator
    // reuse). All ctor initialization of the racy words (buckets, prediction) goes through
    // the same relaxed-atomic helpers as every other C++ access so the ctor is never the
    // plain side of a report against an atomic reader. Relaxed = plain moves on
    // x86-64/arm64: flag-off codegen unchanged.
    ValueProfileBase()
    {
        clearBuckets();
        storePredictionConcurrently(SpecNone);
    }

    EncodedJSValue* specFailBucket(unsigned i)
    {
        ASSERT(numberOfBuckets + i < totalNumberOfBuckets);
        return m_buckets + numberOfBuckets + i;
    }

    // THREADS §5.7.4 (SPEC-jit Task 12): profile buckets are racily written by JIT'd/LLInt
    // fast paths (plain aligned 64-bit stores, allowed to stay plain) on any of N mutators,
    // racily written by C++ slow paths, and racily read by compiler threads (G8: not
    // lock-guarded on 64-bit, NoLockingNecessaryTag). Tolerance contract (I12): every
    // access is one aligned 64-bit load/store — word-atomic, never torn — and profiles
    // only ever SELECT speculation; emitted guards validate. C++ accesses go through these
    // relaxed-atomic helpers so the race is explicit and TSAN-clean. On 32-bit the
    // JSCJSValue.h tag/payload protocol is kept.
#if USE(JSVALUE64)
    EncodedJSValue loadBucketConcurrently(unsigned i) const
    {
        ASSERT(i < totalNumberOfBuckets);
        return WTF::atomicLoad(const_cast<EncodedJSValue*>(&m_buckets[i]), std::memory_order_relaxed);
    }

    void storeBucketConcurrently(unsigned i, EncodedJSValue value)
    {
        ASSERT(i < totalNumberOfBuckets);
        WTF::atomicStore(&m_buckets[i], value, std::memory_order_relaxed);
    }
#else
    // 32-bit: keep JSCJSValue.h's tag/payload protocol (plain when !ENABLE(CONCURRENT_JS)).
    EncodedJSValue loadBucketConcurrently(unsigned i) const
    {
        ASSERT(i < totalNumberOfBuckets);
        return JSValue::encode(JSValue::decodeConcurrent(&m_buckets[i]));
    }

    void storeBucketConcurrently(unsigned i, EncodedJSValue value)
    {
        ASSERT(i < totalNumberOfBuckets);
        updateEncodedJSValueConcurrent(m_buckets[i], value);
    }
#endif

    void clearBuckets()
    {
        for (unsigned i = 0; i < totalNumberOfBuckets; ++i)
            storeBucketConcurrently(i, JSValue::encode(JSValue()));
    }

    // THREADS §5.7.4/§5.7.7: the prediction word is racily merged by mutators (slow-path
    // profile updates, UnlinkedValueProfile::update) and racily read by compiler threads.
    // Merges are tolerate-don't-synchronize (a racing merge may be lost; profiles only
    // SELECT speculation and emitted guards validate), but plain mixed-thread accesses
    // are C++ UB, so all C++ accesses go through these relaxed-atomic helpers. Relaxed
    // load/store compile to plain moves on x86-64/arm64: flag-off codegen unchanged.
    SpeculatedType predictionConcurrently() const
    {
        return WTF::atomicLoad(const_cast<SpeculatedType*>(&m_prediction), std::memory_order_relaxed);
    }

    void storePredictionConcurrently(SpeculatedType prediction)
    {
        WTF::atomicStore(&m_prediction, prediction, std::memory_order_relaxed);
    }

    const ClassInfo* classInfo(unsigned bucket) const
    {
        JSValue value = JSValue::decode(loadBucketConcurrently(bucket));
        if (!!value) {
            if (!value.isCell())
                return nullptr;
            return value.asCell()->classInfo();
        }
        return nullptr;
    }

    unsigned numberOfSamples() const
    {
        unsigned result = 0;
        for (unsigned i = 0; i < totalNumberOfBuckets; ++i) {
            if (!!JSValue::decode(loadBucketConcurrently(i)))
                result++;
        }
        return result;
    }
    
    unsigned totalNumberOfSamples() const
    {
        return numberOfSamples() + isSampledBefore();
    }

    bool isSampledBefore() const { return predictionConcurrently() != SpecNone; }
    
    CString briefDescription(const ConcurrentJSLocker& locker)
    {
        SpeculatedType prediction = computeUpdatedPrediction(locker);
        
        StringPrintStream out;
        out.print("predicting ", SpeculationDump(prediction));
        return out.toCString();
    }
    
    SUPPRESS_TSAN void dump(PrintStream& out)
    {
        out.print("sampled before = ", isSampledBefore(), " live samples = ", numberOfSamples(), " prediction = ", SpeculationDump(predictionConcurrently()));
        bool first = true;
        for (unsigned i = 0; i < totalNumberOfBuckets; ++i) {
            JSValue value = JSValue::decode(loadBucketConcurrently(i));
            if (!!value) {
                if (first) {
                    out.printf(": ");
                    first = false;
                } else
                    out.printf(", ");
                out.print(value);
            }
        }
    }
    
    SpeculatedType computeUpdatedPrediction(const ConcurrentJSLocker&)
    {
        SpeculatedType merged = SpecNone;
        for (unsigned i = 0; i < totalNumberOfBuckets; ++i) {
            JSValue value = JSValue::decode(loadBucketConcurrently(i));
            if (!value)
                continue;

            mergeSpeculation(merged, speculationFromValue(value));

            storeBucketConcurrently(i, JSValue::encode(JSValue()));
        }

        mergeSpeculationConcurrently(m_prediction, merged);

        return predictionConcurrently();
    }

    // THREADS §5.7.4/§5.7.7: `value` aliases a CompressedLazyValueProfileHolder
    // speculation-failure bucket slot. That slot is racily written by OSR-exit/JIT'd code
    // (plain aligned 64-bit stores, allowed to stay plain) and racily read+cleared here by
    // DFG compiler threads — on 64-bit valueProfileLock() is NoLockingNecessaryTag, so two
    // compiler threads can race on the same slot. Tolerance contract: a racing sample may
    // be lost; profiles only SELECT speculation and emitted guards validate. All C++
    // accesses to the slot go through relaxed atomics (64-bit) or the JSCJSValue.h
    // tag/payload concurrent protocol (32-bit) so the race is explicit and TSAN-clean.
    // Relaxed = plain moves on x86-64/arm64: flag-off codegen unchanged.
    void computeUpdatedPredictionForExtraValue(const ConcurrentJSLocker&, JSValue& value)
    {
#if USE(JSVALUE64)
        static_assert(sizeof(JSValue) == sizeof(EncodedJSValue));
        EncodedJSValue* slot = std::bit_cast<EncodedJSValue*>(&value);
        JSValue observed = JSValue::decode(WTF::atomicLoad(slot, std::memory_order_relaxed));
        if (observed)
            mergeSpeculationConcurrently(m_prediction, speculationFromValue(observed));
        WTF::atomicStore(slot, JSValue::encode(JSValue()), std::memory_order_relaxed);
#else
        static_assert(sizeof(JSValue) == sizeof(EncodedJSValue));
        EncodedJSValue* slot = std::bit_cast<EncodedJSValue*>(&value);
        JSValue observed = JSValue::decodeConcurrent(slot);
        if (observed)
            mergeSpeculationConcurrently(m_prediction, speculationFromValue(observed));
        updateEncodedJSValueConcurrent(*slot, JSValue::encode(JSValue()));
#endif
    }

    EncodedJSValue m_buckets[totalNumberOfBuckets];

    // THREADS §5.7.4/§5.7.7: initialized in the constructor via storePredictionConcurrently
    // (relaxed store), never via a plain default-member-init, so every C++ access — including
    // construction — is atomic.
    SpeculatedType m_prediction;
};

#if USE(JSVALUE64)
// THREADS §5.7.4: word-atomicity of bucket accesses relies on naturally aligned 8-byte words.
static_assert(sizeof(EncodedJSValue) == 8);
static_assert(alignof(ValueProfileBase<1, 0>) >= alignof(EncodedJSValue));
static_assert(alignof(ValueProfileBase<0, 1>) >= alignof(EncodedJSValue));
#endif

struct MinimalValueProfile : public ValueProfileBase<0, 1> {
    MinimalValueProfile(): ValueProfileBase<0, 1>() { }
};

struct ValueProfile : public ValueProfileBase<1, 0> {
    ValueProfile() : ValueProfileBase<1, 0>() { }
    static constexpr ptrdiff_t offsetOfFirstBucket() { return OBJECT_OFFSETOF(ValueProfile, m_buckets[0]); }
};

struct ArgumentValueProfile : public ValueProfileBase<1, 1> {
    ArgumentValueProfile() : ValueProfileBase<1, 1>() { }
    static constexpr ptrdiff_t offsetOfFirstBucket() { return OBJECT_OFFSETOF(ValueProfile, m_buckets[0]); }
};

struct ValueProfileAndVirtualRegister : public ValueProfile {
    VirtualRegister m_operand;
};

static_assert(sizeof(ValueProfileAndVirtualRegister) >= sizeof(unsigned));
class alignas(ValueProfileAndVirtualRegister) ValueProfileAndVirtualRegisterBuffer final {
    WTF_MAKE_NONCOPYABLE(ValueProfileAndVirtualRegisterBuffer);
public:

    static ValueProfileAndVirtualRegisterBuffer* create(unsigned size)
    {
        void* buffer = VMMalloc::malloc(sizeof(ValueProfileAndVirtualRegisterBuffer) + size * sizeof(ValueProfileAndVirtualRegister));
        return new (buffer) ValueProfileAndVirtualRegisterBuffer(size);
    }

    static void destroy(ValueProfileAndVirtualRegisterBuffer* buffer)
    {
        buffer->~ValueProfileAndVirtualRegisterBuffer();
        VMMalloc::free(buffer);
    }

    template <typename Function>
    void forEach(Function function)
    {
        unsigned size = this->size();
        for (unsigned i = 0; i < size; ++i)
            function(data()[i]);
    }

    // THREADS §5.7.4/§5.7.7: this buffer lives in op_catch metadata and is probed by
    // compiler threads (ByteCodeParser/OSR entry) while mutators allocate/destroy
    // buffers through the shared allocator. Like the ValueProfileBase ctor above, all
    // C++ accesses to m_size — including the constructor's initialization into
    // just-malloc'd (possibly allocator-reused) memory — go through relaxed atomics so
    // construction is never the plain side of a report against a concurrent reader.
    // Relaxed = plain moves on x86-64/arm64: flag-off codegen unchanged.
    unsigned size() const { return WTF::atomicLoad(const_cast<unsigned*>(&m_size), std::memory_order_relaxed); }
    ValueProfileAndVirtualRegister* data() const LIFETIME_BOUND
    {
        return std::bit_cast<ValueProfileAndVirtualRegister*>(this + 1);
    }

    std::span<ValueProfileAndVirtualRegister> span() LIFETIME_BOUND { return { data(), size() }; }

private:

    ValueProfileAndVirtualRegisterBuffer(unsigned size)
    {
        // THREADS §5.7.4/§5.7.7: initialize m_size via a relaxed store (see size() above).
        WTF::atomicStore(&m_size, size, std::memory_order_relaxed);
        // FIXME: ValueProfile has more stuff than we need. We could optimize these value profiles
        // to be more space efficient.
        // https://bugs.webkit.org/show_bug.cgi?id=175413
        for (unsigned i = 0; i < size; ++i)
            new (&data()[i]) ValueProfileAndVirtualRegister();
    }

    ~ValueProfileAndVirtualRegisterBuffer()
    {
        unsigned size = this->size();
        for (unsigned i = 0; i < size; ++i)
            data()[i].~ValueProfileAndVirtualRegister();
    }

    unsigned m_size;
};

class UnlinkedValueProfile {
public:
    // THREADS §5.7.4/§5.7.7: like ValueProfileBase, initialize the racy prediction word
    // through a relaxed store so construction is never the plain side of a race with a
    // concurrent relaxed reader. Relaxed = plain move on x86-64/arm64: flag-off codegen
    // unchanged.
    UnlinkedValueProfile()
    {
        WTF::atomicStore(&m_prediction, SpecNone, std::memory_order_relaxed);
    }

    // THREADS §5.7.4/§5.7.7: racy bidirectional merge between the linked profile's
    // prediction and this unlinked prediction (shared across all CodeBlocks linked from
    // the same UnlinkedCodeBlock, so it races with itself across threads too). Lost
    // merges are tolerated; every access must be a relaxed atomic. Relaxed = plain
    // moves on x86-64/arm64, so flag-off codegen is unchanged.
    void update(ValueProfile& profile)
    {
        SpeculatedType newType = profile.predictionConcurrently() | WTF::atomicLoad(&m_prediction, std::memory_order_relaxed);
        profile.storePredictionConcurrently(newType);
        WTF::atomicStore(&m_prediction, newType, std::memory_order_relaxed);
    }

    void update(ArgumentValueProfile& profile)
    {
        SpeculatedType newType = profile.predictionConcurrently() | WTF::atomicLoad(&m_prediction, std::memory_order_relaxed);
        profile.storePredictionConcurrently(newType);
        WTF::atomicStore(&m_prediction, newType, std::memory_order_relaxed);
    }

private:
    // THREADS §5.7.4/§5.7.7: initialized in the constructor via relaxed store; all other
    // accesses are relaxed atomics (see update()).
    SpeculatedType m_prediction;
};

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
