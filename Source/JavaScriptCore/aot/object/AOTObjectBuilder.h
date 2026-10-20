/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLayoutData.h"
#include "DeferGC.h"
#include "JSCJSValue.h"
#include <memory>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/ForbidHeapAllocation.h>
#include <wtf/MathExtras.h>
#include <wtf/Noncopyable.h>
#include <wtf/RefPtr.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Vector.h>
#include <wtf/text/UniquedStringImpl.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

class JSObject;
class VM;

namespace AOT {

class ObjectBuilder;
struct State;

class ObjectBuilderPlan {
    WTF_MAKE_TZONE_ALLOCATED(ObjectBuilderPlan);
    WTF_MAKE_NONCOPYABLE(ObjectBuilderPlan);
public:
    unsigned numberOfFields() const { return m_fields.size(); }

private:
    friend class ObjectBuilder;

    struct PlannedField {
        bool isExtraProperty() const { return !field; }

        const FieldData* field { nullptr };
        bool acceptsEveryInt32 { false };
        bool acceptsEveryNumber { false };
        bool acceptsBooleans { false };
        unsigned extraPropertyIndex { 0 };
    };

    ObjectBuilderPlan(State&, const LayoutData&);

    void createInitialBody(uint32_t orderAndCount);

    State& m_state;
    const LayoutData& m_layout;
    Vector<PlannedField> m_fields;
    Vector<RefPtr<UniquedStringImpl>> m_extraPropertyNames;
    Vector<uint16_t> m_keyOrder;
    Vector<uint8_t> m_initialBody;
    uint64_t m_initialGranuleBits { 0 };
};

class ObjectBuilder {
    WTF_FORBID_HEAP_ALLOCATION;
    WTF_MAKE_NONCOPYABLE(ObjectBuilder);
public:
    static std::unique_ptr<ObjectBuilderPlan> tryCreatePlan(State&, const LayoutData&, std::span<UniquedStringImpl* const> names);

    ObjectBuilder(VM&, const ObjectBuilderPlan&);

    bool tryStore(unsigned index, JSValue);
    bool tryStoreInt32(unsigned index, int32_t);
    bool tryStoreDouble(unsigned index, double);
    bool tryStoreBoolean(unsigned index, bool);
    JSObject* finish();

private:
    static constexpr unsigned inlineFieldCapacity = 64;
    static constexpr unsigned inlineExtraPropertyCapacity = 8;
    static constexpr unsigned numberOfHiddenWords = 2;
    static constexpr size_t inlineCellSize = WTF::roundUpToMultipleOf<16>(offsetOfFirstField + (inlineFieldCapacity + numberOfHiddenWords) * sizeof(EncodedJSValue));
    static constexpr size_t inlineExtensionSize = WTF::roundUpToMultipleOf<minExtensionSize>(sizeof(EncodedJSValue) + inlineFieldCapacity * sizeof(EncodedJSValue));

    static constexpr size_t numberOfWordsFor(size_t numberOfBytes) { return WTF::roundUpToMultipleOf<sizeof(uint64_t)>(numberOfBytes) / sizeof(uint64_t); }

    std::span<uint8_t> cellBytes() { return asMutableByteSpan(m_cellWords.mutableSpan()); }
    // Every granule of the layout at its offset. finish() copies the ones that are used.
    std::span<uint8_t> extensionBytes() { return asMutableByteSpan(m_extensionWords.mutableSpan()); }
    void* slotOf(const FieldData& field) { return (field.isOutOfLine() ? extensionBytes() : cellBytes()).subspan(field.slot.offset).data(); }
    void didStore(unsigned index) { m_stored.set(index); }
    void storeDoubleInExtension(const FieldData&, double);

    VM& m_vm;
    const ObjectBuilderPlan& m_plan;
    DeferGC m_deferGC;
    BitVector m_stored;
    uint64_t m_granuleBits;
    Vector<uint64_t, inlineCellSize / sizeof(uint64_t)> m_cellWords;
    Vector<uint64_t, inlineExtensionSize / sizeof(uint64_t)> m_extensionWords;
    Vector<JSValue, inlineExtraPropertyCapacity> m_extraPropertyValues;
};

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
