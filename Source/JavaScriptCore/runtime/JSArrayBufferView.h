/*
 * Copyright (C) 2013-2021 Apple Inc. All rights reserved.
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

#include "AuxiliaryBarrier.h"
#include "JSObject.h"
#include <wtf/Atomics.h>
#include <wtf/TaggedArrayStoragePtr.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

class JSArrayBufferView;
class LLIntOffsetsExtractor;

// This class serves two purposes:
//
// 1) It provides those parts of JSGenericTypedArrayView that don't depend
//    on template parameters.
//
// 2) It represents the DOM/WebCore-visible "JSArrayBufferView" type, which
//    C++ code uses when it wants to pass around a view of an array buffer
//    without concern for the actual type of the view.
//
// These two roles are quite different. (1) is just a matter of optimizing
// compile and link times by having as much code and data as possible not
// be subject to template specialization. (2) is *almost* a matter of
// semantics; indeed at the very least it is a matter of obeying a contract
// that we have with WebCore right now.
//
// One convenient thing that saves us from too much crazy is that
// ArrayBufferView is not instantiable.

// Typed array views have different modes depending on how big they are and
// whether the user has done anything that requires a separate backing
// buffer or the DOM-specified detaching capabilities.
enum TypedArrayMode : uint8_t {
    // Legend:
    // B: JSArrayBufferView::m_butterfly pointer
    // V: JSArrayBufferView::m_vector pointer
    // M: JSArrayBufferView::m_mode

    // Small and fast typed array. B is unused, V points to a vector
    // allocated in the primitive Gigacage, and M = FastTypedArray. V's
    // liveness is determined entirely by the view's liveness.
    FastTypedArray = 0b0001'0000,

    // A large typed array that still attempts not to waste too much
    // memory. B is unused, V points to a vector allocated using
    // Gigacage::tryMalloc(), and M = OversizeTypedArray. V's liveness is
    // determined entirely by the view's liveness, and the view will add a
    // finalizer to delete V.
    OversizeTypedArray = 0b0011'0000,

    // A typed array that was used in some crazy way. B's IndexingHeader
    // is hijacked to contain a reference to the native array buffer. The
    // native typed array view points back to the JS view. V points to a
    // vector allocated using who-knows-what, and M = WastefulTypedArray.
    // The view does not own the vector.
    WastefulTypedArray = 0b0101'1000,
    GrowableSharedWastefulTypedArray = 0b0101'1010,
    GrowableSharedAutoLengthWastefulTypedArray = 0b0101'1011,
    ResizableNonSharedWastefulTypedArray = 0b0101'1100,
    ResizableNonSharedAutoLengthWastefulTypedArray = 0b0101'1101,

    // A data view. B is unused, V points to a vector allocated using who-
    // knows-what, and M = DataViewMode. The view does not own the vector.
    // There is an extra field (in JSDataView) that points to the
    // ArrayBuffer.
    DataViewMode = 0b1000'1000,
    GrowableSharedDataViewMode = 0b1000'1010,
    GrowableSharedAutoLengthDataViewMode = 0b1000'1011,
    ResizableNonSharedDataViewMode = 0b1000'1100,
    ResizableNonSharedAutoLengthDataViewMode = 0b1000'1101,
};

constexpr uint8_t isAutoLengthMode          = 0b0000'0001;
constexpr uint8_t isGrowableSharedMode      = 0b0000'0010;
constexpr uint8_t isResizableNonSharedMode  = 0b0000'0100;
constexpr uint8_t isHavingArrayBufferMode   = 0b0000'1000;
constexpr uint8_t isTypedArrayMode          = 0b0001'0000;
constexpr uint8_t isWastefulTypedArrayMode  = 0b0100'0000;
constexpr uint8_t isDataViewMode            = 0b1000'0000;

constexpr uint8_t isResizableOrGrowableSharedMode = isResizableNonSharedMode | isGrowableSharedMode;
constexpr uint8_t resizabilityAndAutoLengthMask = isAutoLengthMode | isGrowableSharedMode | isResizableNonSharedMode;

inline bool hasArrayBuffer(TypedArrayMode mode)
{
    return static_cast<uint8_t>(mode) & isHavingArrayBufferMode;
}

inline bool isResizableOrGrowableShared(TypedArrayMode mode)
{
    return static_cast<uint8_t>(mode) & isResizableOrGrowableSharedMode;
}

inline bool isGrowableShared(TypedArrayMode mode)
{
    return static_cast<uint8_t>(mode) & isGrowableSharedMode;
}

inline bool isResizableNonShared(TypedArrayMode mode)
{
    return static_cast<uint8_t>(mode) & isResizableNonSharedMode;
}

inline bool isAutoLength(TypedArrayMode mode)
{
    return static_cast<uint8_t>(mode) & isAutoLengthMode;
}

inline bool isWastefulTypedArray(TypedArrayMode mode)
{
    return static_cast<uint8_t>(mode) & isWastefulTypedArrayMode;
}

inline bool canUseArrayBufferViewRawFieldsDirectly(TypedArrayMode mode)
{
    // Non-resizable, or growable-shared but not auto-length (since it is growing only).
    // !isResizableOrGrowableShared() || (!isAutoLength() && isGrowableShared());
    //
    // Lower three bits are the following.
    //     isAutoLengthMode          = 0b001;
    //     isGrowableSharedMode      = 0b010;
    //     isResizableNonSharedMode  = 0b100;
    //
    // And possible combinations are,
    //     (1) 0b000                     = non-growable, non-auto-length, non-resizable
    //     (2) 0b010                     = growable, non-auto-length, non-resizable
    //     (3) 0b011                     = growable, auto-length, non-resizable
    //     (4) 0b100                     = non-growable, non-auto-length, resizable
    //     (5) 0b101                     = non-growable, auto-length, resizable
    //
    // Since we would like to pass (1) and (2), we should extract lower three bits and check against isGrowableSharedMode.
    return (static_cast<uint8_t>(mode) & resizabilityAndAutoLengthMask) <= isGrowableSharedMode;
}

template<typename Getter> bool isArrayBufferViewOutOfBounds(JSArrayBufferView*, Getter&);

template<typename Getter> std::optional<size_t> integerIndexedObjectLength(JSArrayBufferView*, Getter&);
template<typename Getter> size_t integerIndexedObjectByteLength(JSArrayBufferView*, Getter&);
template<typename Getter> bool isIntegerIndexedObjectOutOfBounds(JSArrayBufferView*, Getter&);

extern JS_EXPORT_PRIVATE const ASCIILiteral typedArrayBufferHasBeenDetachedErrorMessage;

enum class CopyType {
    LeftToRight,
    Unobservable,
};

// TSAN-TRIAGE §3.24 (typedarray-sab; SPEC-ungil §A, SPEC-objectmodel §4.7
// analog): the view metadata words (m_length / m_byteOffset / m_mode) are read
// concurrently — by other mutators on shared views, by compiler threads, and
// by GC — while the owning thread constructs the view, detaches it, or runs
// slowDownAndWasteMemory(). The VALUE race is blessed: readers follow the
// read-*Raw-then-revalidate protocol (UG §A; IdempotentArrayBufferByteLength
// getters), visitChildren snapshots under cellLock, and slowDownAndWasteMemory
// publishes with an explicit storeStoreFence before the m_mode flip. But a
// plain C++ access racing another plain (or atomic) access is still UB/a TSAN
// report, so EVERY access to these fields — including the writes in
// JSArrayBufferView.cpp, which flow through this wrapper's converting
// constructor and operator= — is a RELAXED atomic. Relaxed loads/stores of
// these word-sized fields compile to the same plain MOV/STR on x86-64/arm64,
// so flag-off (useJSThreads=false) semantics and codegen are unchanged. Any
// ordering stronger than relaxed is supplied by the surrounding publication
// protocol (cellLock, storeStoreFence, safepoints), never by this wrapper.
template<typename T>
class RacyArrayBufferViewField {
public:
    ALWAYS_INLINE RacyArrayBufferViewField() { WTF::atomicStore(&m_value, T(), std::memory_order_relaxed); }
    ALWAYS_INLINE RacyArrayBufferViewField(T value) { WTF::atomicStore(&m_value, value, std::memory_order_relaxed); }

    ALWAYS_INLINE RacyArrayBufferViewField& operator=(T value)
    {
        WTF::atomicStore(&m_value, value, std::memory_order_relaxed);
        return *this;
    }

#if TSAN_ENABLED
    // Forced out of line under TSAN ONLY, so the relaxed probe symbolizes as
    // a RacyArrayBufferViewField frame and the narrow suppression anchor
    // (race:JSC::RacyArrayBufferViewField, Tools/tsan/suppressions.txt)
    // matches. Fully inlined, the probe's frame degrades to the enclosing
    // host function (seen in r11 as typedArrayViewProtoGetterFuncLength),
    // which forced a rule-3/4-violating function-level suppression over a
    // whole user-facing builtin. Production builds keep ALWAYS_INLINE below.
    NEVER_INLINE operator T() const { return WTF::atomicLoad(const_cast<T*>(&m_value), std::memory_order_relaxed); }
#else
    ALWAYS_INLINE operator T() const { return WTF::atomicLoad(const_cast<T*>(&m_value), std::memory_order_relaxed); }
#endif

private:
    T m_value;
};

static_assert(sizeof(RacyArrayBufferViewField<size_t>) == sizeof(size_t));
static_assert(sizeof(RacyArrayBufferViewField<TypedArrayMode>) == sizeof(TypedArrayMode));

// When WebCore uses a JSArrayBufferView, it expects to be able to get the native
// ArrayBuffer and little else. This requires slowing down and wasting memory,
// and then accessing things via the Butterfly. When JS uses a JSArrayBufferView
// it is always via the usual methods in the MethodTable, so this class's
// implementation of those has no need to even exist - we could at any time sink
// code into JSGenericTypedArrayView if it was convenient.

class JSArrayBufferView : public JSNonFinalObject {
public:
    using Base = JSNonFinalObject;

    template<typename, SubspaceAccess>
    static void subspaceFor(VM&)
    {
        RELEASE_ASSERT_NOT_REACHED();
    }

    static constexpr size_t fastSizeLimit = 1000;
    using VectorPtr = CagedBarrierPtr<Gigacage::Primitive, void>;

    static void* nullVectorPtr()
    {
        VectorPtr null { };
        return null.rawBits();
    }
    
    static size_t sizeOf(size_t length, unsigned elementSize)
    {
        Checked<size_t> result = length;
        result *= elementSize;
        result += sizeof(EncodedJSValue) - 1;
        return result.value() & ~(sizeof(EncodedJSValue) - 1);
    }

    static size_t allocationSize(Checked<size_t> inlineCapacity)
    {
        ASSERT_UNUSED(inlineCapacity, !inlineCapacity);
        return sizeof(JSArrayBufferView);
    }
        
protected:
    class ConstructionContext {
        WTF_MAKE_NONCOPYABLE(ConstructionContext);
        
    public:
        enum InitializationMode { ZeroFill, DontInitialize };
        
        JS_EXPORT_PRIVATE ConstructionContext(VM&, Structure*, size_t length, unsigned elementSize, InitializationMode = ZeroFill);
        
        // This is only for constructing fast typed arrays. It's used by the JIT's slow path.
        ConstructionContext(Structure*, size_t length, void* vector);
        
        JS_EXPORT_PRIVATE ConstructionContext(
            VM&, Structure*, RefPtr<ArrayBuffer>&&,
            size_t byteOffset, std::optional<size_t> length);
        
        enum DataViewTag { DataView };
        ConstructionContext(
            Structure*, RefPtr<ArrayBuffer>&&,
            size_t byteOffset, std::optional<size_t> length, DataViewTag);
        
        bool operator!() const { return !m_structure; }
        
        Structure* structure() const { return m_structure; }
        void* vector() const LIFETIME_BOUND { return m_vector.getMayBeNull(); }
        size_t length() const { return m_length; }
        size_t byteOffset() const { return m_byteOffset; }
        TypedArrayMode mode() const { return m_mode; }
        Butterfly* butterfly() const { return m_butterfly; }
        
    private:
        Structure* m_structure;
        using VectorType = CagedPtr<Gigacage::Primitive, void>;
        VectorType m_vector;
        size_t m_length;
        size_t m_byteOffset;
        TypedArrayMode m_mode;
        Butterfly* m_butterfly;
    };
    
    JS_EXPORT_PRIVATE JSArrayBufferView(VM&, ConstructionContext&);
    JS_EXPORT_PRIVATE void finishCreation(VM&);
    
public:
    TypedArrayMode mode() const { return m_mode; }
    bool hasArrayBuffer() const { return JSC::hasArrayBuffer(mode()); }
    
    inline bool isShared();
    JS_EXPORT_PRIVATE ArrayBuffer* unsharedBuffer();
    inline ArrayBuffer* possiblySharedBuffer();
    // Public (upstream: protected) for the threads-mode Atomics detach re-check, SPEC-ungil annex N6 arm 1.
    ArrayBuffer* existingBufferInButterfly();
    JSArrayBuffer* unsharedJSBuffer(JSGlobalObject* globalObject);
    JSArrayBuffer* possiblySharedJSBuffer(JSGlobalObject* globalObject);
    inline RefPtr<ArrayBufferView> unsharedImpl();
    JS_EXPORT_PRIVATE RefPtr<ArrayBufferView> possiblySharedImpl();
    bool isDetached() const { return hasArrayBuffer() && !hasVector(); }
    bool isResizableOrGrowableShared() const { return JSC::isResizableOrGrowableShared(m_mode); }
    bool isGrowableShared() const { return JSC::isGrowableShared(m_mode); };
    bool isResizableNonShared() const { return JSC::isResizableNonShared(m_mode); };
    bool isAutoLength() const { return JSC::isAutoLength(m_mode); }

    ALWAYS_INLINE bool canUseRawFieldsDirectly() const
    {
        // Non-resizable, or growable-shared but not auto-length (since it is growing only, m_length and m_byteOffset are always valid).
        return JSC::canUseArrayBufferViewRawFieldsDirectly(m_mode);
    }

    // TSAN-TRIAGE §3.24 (typedarray-sab): the m_vector caged word is written
    // with relaxed atomic stores (CagedBarrierPtr -> AuxiliaryBarrier
    // setWithoutBarrier/clear; refreshVector, detachFromArrayBuffer,
    // slowDownAndWasteMemory) while other threads read it lock-free. The
    // CagedBarrierPtr read accessors go through AuxiliaryBarrier::get(), a
    // plain load — the other half of the reported pair. All readers here load
    // the word with a single relaxed atomic load and re-wrap it; stale words
    // are legal (readers re-validate per UG §A; visitChildren snapshots under
    // cellLock). Relaxed loads are plain MOVs/LDRs: flag-off codegen unchanged.
    ALWAYS_INLINE CagedPtr<Gigacage::Primitive, void> vectorCagedConcurrently() const
    {
        static_assert(sizeof(VectorPtr) == sizeof(void*), "m_vector must be a single caged pointer word");
        void* bits = WTF::atomicLoad(reinterpret_cast<void**>(const_cast<VectorPtr*>(&m_vector)), std::memory_order_relaxed);
        return CagedPtr<Gigacage::Primitive, void>(bits);
    }

    bool hasVector() const { return !!vectorCagedConcurrently().getUnsafe(); }
    void* vector() const LIFETIME_BOUND { return vectorCagedConcurrently().getMayBeNull(); }
    void* vectorWithoutPACValidation() const LIFETIME_BOUND { return vectorCagedConcurrently().getUnsafe(); }

    std::span<const uint8_t> span() const LIFETIME_BOUND { return { static_cast<const uint8_t*>(vector()), byteLength() }; }
    
    size_t byteOffset() const
    {
        if (canUseRawFieldsDirectly()) [[likely]]
            return byteOffsetRaw();

        IdempotentArrayBufferByteLengthGetter<std::memory_order_seq_cst> getter;
        if (isArrayBufferViewOutOfBounds(const_cast<JSArrayBufferView*>(this), getter)) [[unlikely]]
            return 0;
        return byteOffsetRaw();
    }

    size_t byteOffsetRaw() const { return m_byteOffset; }

    size_t length() const
    {
        if (canUseRawFieldsDirectly()) [[likely]]
            return lengthRaw();

        IdempotentArrayBufferByteLengthGetter<std::memory_order_seq_cst> getter;
        return integerIndexedObjectLength(const_cast<JSArrayBufferView*>(this), getter).value_or(0);
    }

    size_t lengthRaw() const { return m_length; }

    size_t byteLength() const
    {
        // The absence of overflow is already checked in the constructor, so I only add the extra sanity check when asserts are enabled.
        // https://tc39.es/proposal-resizablearraybuffer/#sec-get-%typedarray%.prototype.bytelength
        if (canUseRawFieldsDirectly()) [[likely]]
            return byteLengthRaw();

        IdempotentArrayBufferByteLengthGetter<std::memory_order_seq_cst> getter;
        return integerIndexedObjectByteLength(const_cast<JSArrayBufferView*>(this), getter);
    }

    size_t byteLengthRaw() const
    {
#if ASSERT_ENABLED
        Checked<size_t> result = lengthRaw();
        result *= elementSize(type());
        return result.value();
#else
        return lengthRaw() << logElementSize(type());
#endif
    }

    bool isOutOfBounds() const
    {
        // https://tc39.es/proposal-resizablearraybuffer/#sec-isarraybufferviewoutofbounds
        if (isDetached()) [[unlikely]]
            return true;
        if (!isResizableNonShared()) [[likely]]
            return false;
        IdempotentArrayBufferByteLengthGetter<std::memory_order_seq_cst> getter;
        return isArrayBufferViewOutOfBounds(const_cast<JSArrayBufferView*>(this), getter);
    }

    DECLARE_EXPORT_INFO;

    DECLARE_VISIT_CHILDREN;
    
    static constexpr ptrdiff_t offsetOfVector() { return OBJECT_OFFSETOF(JSArrayBufferView, m_vector); }
    static constexpr ptrdiff_t offsetOfLength() { return OBJECT_OFFSETOF(JSArrayBufferView, m_length); }
    static constexpr ptrdiff_t offsetOfByteOffset() { return OBJECT_OFFSETOF(JSArrayBufferView, m_byteOffset); }
    static constexpr ptrdiff_t offsetOfMode() { return OBJECT_OFFSETOF(JSArrayBufferView, m_mode); }

    static inline RefPtr<ArrayBufferView> toWrapped(VM&, JSValue);
    static inline RefPtr<ArrayBufferView> toWrappedAllowResizable(VM&, JSValue);
    static inline RefPtr<ArrayBufferView> toWrappedAllowShared(VM&, JSValue);
    static inline RefPtr<ArrayBufferView> toWrappedAllowSharedAndResizable(VM&, JSValue);

    bool isIteratorProtocolFastAndNonObservable();

private:
    enum Requester { Mutator, ConcurrentThread };
    template<Requester> ArrayBuffer* possiblySharedBufferImpl();

    JS_EXPORT_PRIVATE ArrayBuffer* slowDownAndWasteMemory();
    static void finalize(JSCell*);
    void detachFromArrayBuffer();
    void refreshVector(void* newData);

protected:
    friend class LLIntOffsetsExtractor;
    friend class ArrayBuffer;

    VectorPtr m_vector;
    RacyArrayBufferViewField<size_t> m_length;
    RacyArrayBufferViewField<size_t> m_byteOffset { 0 };
    RacyArrayBufferViewField<TypedArrayMode> m_mode;
};

inline JSArrayBufferView* validateTypedArray(JSGlobalObject*, JSValue);

inline ArrayBuffer* JSArrayBufferView::existingBufferInButterfly()
{
    ASSERT(isWastefulTypedArray(m_mode));
    return butterfly()->indexingHeader()->arrayBuffer();
}

} // namespace JSC

namespace WTF {

JS_EXPORT_PRIVATE void printInternal(PrintStream&, JSC::TypedArrayMode);

} // namespace WTF

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
