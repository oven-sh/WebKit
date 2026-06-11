/*
 * Copyright (C) 2011-2024 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "GCAssertions.h"
#include "HandleTypes.h"
#include "StructureID.h"
#include <type_traits>
#include <wtf/Atomics.h>
#include <wtf/ForbidHeapAllocation.h>
#include <wtf/RawPtrTraits.h>
#include <wtf/RawValueTraits.h>
#include <wtf/TZoneMalloc.h>

namespace JSC {

namespace DFG {
class DesiredWriteBarrier;
}

class JSCell;
class VM;
class JSGlobalObject;

template<class T>
using WriteBarrierTraitsSelect = typename std::conditional<std::is_same<T, Unknown>::value,
    RawValueTraits<T>, RawPtrTraits<T>
>::type;

template<class T, typename Traits = WriteBarrierTraitsSelect<T>> class WriteBarrierBase;
template<> class WriteBarrierBase<JSValue>;

JS_EXPORT_PRIVATE void slowValidateCell(JSCell*);
JS_EXPORT_PRIVATE void slowValidateCell(JSGlobalObject*);
    
#if ENABLE(GC_VALIDATION)
template<class T> inline void validateCell(T cell)
{
    ASSERT_GC_OBJECT_INHERITS(cell, std::remove_pointer<T>::type::info());
}

template<> inline void validateCell<JSCell*>(JSCell* cell)
{
    slowValidateCell(cell);
}

template<> inline void validateCell<JSGlobalObject*>(JSGlobalObject* globalObject)
{
    slowValidateCell(globalObject);
}
#else
template<class T> inline void validateCell(T)
{
}
#endif

enum WriteBarrierEarlyInitTag { WriteBarrierEarlyInit };

// We have a separate base class with no constructors for use in Unions.
template <typename T, typename Traits> class WriteBarrierBase {
    using StorageType = typename Traits::StorageType;
    static_assert(std::is_pointer_v<StorageType>, "Concurrent (relaxed-atomic) slot accesses below assume a plain pointer-sized word");

public:
    void set(VM&, const JSCell* owner, T* value);

    // This is meant to be used like operator=, but is called copyFrom instead, in
    // order to kindly inform the C++ compiler that its advice is not appreciated.
    void copyFrom(const WriteBarrierBase& other)
    {
        // FIXME add version with different Traits once needed.
        storeCell(other.cell());
    }

    void setMayBeNull(VM&, const JSCell* owner, T* value);

    // Should only be used by JSCell during early initialisation
    // when some basic types aren't yet completely instantiated
    void setEarlyValue(VM&, const JSCell* owner, T* value);
    
    T* get() const
    {
        // Copy m_cell to a local to avoid multiple-read issues. (See <http://webkit.org/b/110854>)
        StorageType cell = this->cell();
        if (cell)
            validateCell(reinterpret_cast<JSCell*>(static_cast<void*>(Traits::unwrap(cell))));
        return Traits::unwrap(cell);
    }

    T* operator*() const
    {
        StorageType cell = this->cell();
        ASSERT(cell);
        auto unwrapped = Traits::unwrap(cell);
        validateCell<T>(unwrapped);
        return Traits::unwrap(unwrapped);
    }

    T* operator->() const
    {
        StorageType cell = this->cell();
        ASSERT(cell);
        auto unwrapped = Traits::unwrap(cell);
        validateCell(unwrapped);
        return unwrapped;
    }

    void clear() { storeCell(nullptr); }

    // Slot cannot be used when pointers aren't stored as-is.
    template<typename BarrierT, typename BarrierTraits, std::enable_if_t<std::is_same<BarrierTraits, RawPtrTraits<BarrierT>>::value, void*> = nullptr>
    struct SlotHelper {
        static BarrierT** reinterpret(typename BarrierTraits::StorageType* cell) { return reinterpret_cast<T**>(cell); }
    };

    T** slot()
    {
        return SlotHelper<T, Traits>::reinterpret(&m_cell);
    }
    
    explicit operator bool() const { return !!cell(); }
    
    bool operator!() const { return !cell(); }

    void setWithoutWriteBarrier(T* value)
    {
#if ENABLE(WRITE_BARRIER_PROFILING)
        WriteBarrierCounters::usesWithoutBarrierFromCpp.count();
#endif
        storeCell(value);
    }

    T* unvalidatedGet() const { return Traits::unwrap(cell()); }

private:
    // JS cell slots are intentionally racy under shared-heap threading (object-model
    // ground truth, same ruling as WriteBarrierBase<Unknown> below): all reads and
    // writes of m_cell go through relaxed atomics — codegen-identical to plain
    // pointer accesses flag-off, but defined behavior for the blessed cross-thread
    // races flag-on. slot() remains the escape hatch for the GC visitors, which
    // have their own protocol.
    StorageType cell() const { return WTF::atomicLoad(const_cast<StorageType*>(&m_cell), std::memory_order_relaxed); }

    void storeCell(StorageType value) { WTF::atomicStore(&m_cell, value, std::memory_order_relaxed); }

    StorageType m_cell;
};

template <> class WriteBarrierBase<Unknown, RawValueTraits<Unknown>> {
public:
    void set(VM&, const JSCell* owner, JSValue);

    // JS value slots are intentionally racy under shared-heap threading (object-model
    // ground truth): all reads/writes of m_value must go through the concurrent
    // accessors (relaxed atomics on 64-bit — codegen-identical to plain accesses
    // flag-off), never plain C++ loads/stores.
    void setWithoutWriteBarrier(JSValue value)
    {
        updateEncodedJSValueConcurrent(m_value, JSValue::encode(value));
    }

    JSValue get() const
    {
        return JSValue::decodeConcurrent(&m_value);
    }
    void clear() { clearEncodedJSValueConcurrent(m_value); }
    void setUndefined() { updateEncodedJSValueConcurrent(m_value, JSValue::encode(jsUndefined())); }
    void setStartingValue(JSValue value) { updateEncodedJSValueConcurrent(m_value, JSValue::encode(value)); }
    bool isNumber() const { return get().isNumber(); }
    bool isInt32() const { return get().isInt32(); }
    inline bool isObject() const; // Defined in WriteBarrierInlines.h
    inline bool isNull() const; // Defined in WriteBarrierInlines.h
    inline bool isGetterSetter() const; // Defined in WriteBarrierInlines.h
    inline bool isCustomGetterSetter() const; // Defined in WriteBarrierInlines.h

    JSValue* slot() const
    { 
        return std::bit_cast<JSValue*>(&m_value);
    }
    
    int32_t* tagPointer() { return &std::bit_cast<EncodedValueDescriptor*>(&m_value)->asBits.tag; }
    int32_t* payloadPointer() { return &std::bit_cast<EncodedValueDescriptor*>(&m_value)->asBits.payload; }
    
    explicit operator bool() const { return !!get(); }
    bool operator!() const { return !get(); } 
    
private:
    EncodedJSValue m_value;
};

template <typename T, typename Traits = WriteBarrierTraitsSelect<T>>
class WriteBarrier : public WriteBarrierBase<T, Traits> {
    WTF_MAKE_TZONE_ALLOCATED_TEMPLATE(WriteBarrier);
public:
    WriteBarrier()
    {
        this->setWithoutWriteBarrier(nullptr);
    }

    WriteBarrier(VM& vm, const JSCell* owner, T* value)
    {
        this->set(vm, owner, value);
    }

    WriteBarrier(DFG::DesiredWriteBarrier&, T* value)
    {
        ASSERT(isCompilationThread());
        this->setWithoutWriteBarrier(value);
    }

    enum MayBeNullTag { MayBeNull };
    WriteBarrier(VM& vm, const JSCell* owner, T* value, MayBeNullTag)
    {
        this->setMayBeNull(vm, owner, value);
    }

    WriteBarrier(T* value, WriteBarrierEarlyInitTag)
    {
        this->setWithoutWriteBarrier(value);
    }
};

#define TZONE_TEMPLATE_PARAMS template <typename T, typename Traits>
#define TZONE_TYPE WriteBarrier<T, Traits>

WTF_MAKE_TZONE_ALLOCATED_TEMPLATE_IMPL_WITH_MULTIPLE_OR_SPECIALIZED_PARAMETERS();

#undef TZONE_TEMPLATE_PARAMS
#undef TZONE_TYPE

enum UndefinedWriteBarrierTagType { UndefinedWriteBarrierTag };
enum NullWriteBarrierTagType { NullWriteBarrierTag };
template <>
class WriteBarrier<Unknown, RawValueTraits<Unknown>> : public WriteBarrierBase<Unknown, RawValueTraits<Unknown>> {
    WTF_FORBID_HEAP_ALLOCATION_ALLOWING_PLACEMENT_NEW;
public:
    WriteBarrier()
    {
        this->setWithoutWriteBarrier(JSValue());
    }
    WriteBarrier(UndefinedWriteBarrierTagType)
    {
        this->setWithoutWriteBarrier(jsUndefined());
    }
    WriteBarrier(NullWriteBarrierTagType)
    {
        this->setWithoutWriteBarrier(jsNull());
    }

    WriteBarrier(VM& vm, const JSCell* owner, JSValue value)
    {
        this->set(vm, owner, value);
    }

    WriteBarrier(DFG::DesiredWriteBarrier&, JSValue value)
    {
        ASSERT(isCompilationThread());
        this->setWithoutWriteBarrier(value);
    }

    WriteBarrier(JSValue value, WriteBarrierEarlyInitTag)
    {
        this->setWithoutWriteBarrier(value);
    }
};

template <typename U, typename V, typename TraitsU, typename TraitsV>
inline bool operator==(const WriteBarrierBase<U, TraitsU>& lhs, const WriteBarrierBase<V, TraitsV>& rhs)
{
    return lhs.get() == rhs.get();
}

class WriteBarrierStructureID {
public:
    constexpr WriteBarrierStructureID() = default;

    WriteBarrierStructureID(VM& vm, const JSCell* owner, Structure* value)
    {
        set(vm, owner, value);
    }

    WriteBarrierStructureID(DFG::DesiredWriteBarrier&, Structure* value)
    {
        ASSERT(isCompilationThread());
        setWithoutWriteBarrier(value);
    }

    enum MayBeNullTag { MayBeNull };
    WriteBarrierStructureID(VM& vm, const JSCell* owner, Structure* value, MayBeNullTag)
    {
        setMayBeNull(vm, owner, value);
    }

    WriteBarrierStructureID(Structure* value, WriteBarrierEarlyInitTag)
    {
        setWithoutWriteBarrier(value);
    }

    void set(VM&, const JSCell* owner, Structure* value);

    void setMayBeNull(VM&, const JSCell* owner, Structure* value);

    // Should only be used by JSCell during early initialisation
    // when some basic types aren't yet completely instantiated
    void setEarlyValue(VM&, const JSCell* owner, Structure* value);

    Structure* get() const
    {
        // Copy m_structureID to a local to avoid multiple-read issues. (See <http://webkit.org/b/110854>)
        StructureID structureID = value();
        if (structureID) {
            Structure* structure = structureID.decode();
            validateCell(reinterpret_cast<JSCell*>(structure));
            return structure;
        }
        return nullptr;
    }

    Structure* operator*() const
    {
        StructureID structureID = value();
        ASSERT(structureID);
        Structure* structure = structureID.decode();
        validateCell(reinterpret_cast<JSCell*>(structure));
        return structure;
    }

    Structure* operator->() const
    {
        StructureID structureID = value();
        ASSERT(structureID);
        Structure* structure = structureID.decode();
        validateCell(reinterpret_cast<JSCell*>(structure));
        return structure;
    }

    void clear()
    {
        m_structureID = { };
    }

    explicit operator bool() const
    {
        return !!value();
    }

    bool operator!() const
    {
        return !value();
    }

    void setWithoutWriteBarrier(Structure* value)
    {
#if ENABLE(WRITE_BARRIER_PROFILING)
        WriteBarrierCounters::usesWithoutBarrierFromCpp.count();
#endif
        // TSAN family 7 (cell-header, §8.7 residual): this slot is read by
        // concurrent readers via StructureID::relaxedLoad; the writer must
        // pair with a relaxed store. TSAN-build-only atomic — non-TSAN builds
        // compile to the identical plain 32-bit store (flag-off codegen
        // unchanged).
        if (!value) {
            StructureID::relaxedStore(&m_structureID, StructureID());
            return;
        }
        StructureID::relaxedStore(&m_structureID, StructureID::encode(value));
    }

    Structure* unvalidatedGet() const
    {
        StructureID structureID = value();
        if (structureID)
            return structureID.decode();
        return nullptr;
    }

    // GIL-off (TSAN r15): relaxed atomic read pairing the relaxed writers
    // (setWithoutWriteBarrier / setEarlyValue / GC clear) — lock-free
    // cross-Thread readers (e.g. cachedPolyProtoStructure) race them by
    // design and revalidate. Identical plain 32-bit load outside TSAN.
    StructureID value() const { return StructureID::relaxedLoad(&m_structureID); }

private:
    StructureID m_structureID;
};

} // namespace JSC

namespace WTF {

template<typename T> struct VectorTraits<JSC::WriteBarrier<T>> : public SimpleClassVectorTraits {
    static_assert(std::is_trivially_destructible<JSC::WriteBarrier<T>>::value);
    static constexpr bool canCopyWithMemcpy = true;
};

template<> struct VectorTraits<JSC::WriteBarrier<JSC::Unknown>> : public SimpleClassVectorTraits {
    static_assert(std::is_trivially_destructible<JSC::WriteBarrier<JSC::Unknown>>::value);
#if USE(JSVALUE32_64)
    // We can memset only in JSVALUE64 since empty value is zero. On the other hand, JSVALUE32_64's empty value is not zero.
    static constexpr bool canInitializeWithMemset = false;
#endif
    static constexpr bool canCopyWithMemcpy = true;
};

} // namespace WTF
