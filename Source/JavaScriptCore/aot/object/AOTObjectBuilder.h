/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLayoutData.h"
#include "AOTVector.h"
#include "Allocator.h"
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
#include <wtf/text/UniquedStringImpl.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

class JSObject;
class Structure;
class VM;

namespace AOT {

class ObjectBuilder;
struct State;

namespace Interpreter {
class ObjectLiteralPlans;
}

class ObjectBuilderPlan {
    WTF_MAKE_TZONE_ALLOCATED(ObjectBuilderPlan);
    WTF_MAKE_NONCOPYABLE(ObjectBuilderPlan);
public:
    // One for each name that the plan was made with, in that order.
    unsigned numberOfFields() const { return m_fields.size(); }
    bool hasFastPath() const { return !m_fastFields.isEmpty(); }
    // The plan remembers Structures by ID and keeps none of them alive, and the ID of a dead Structure can be given out again. Whoever holds a plan calls this at every collection.
    void clearAcceptedStructures() const { m_acceptedStructureIDs.fill(0); }
    const LayoutData& layout() const { return m_layout; }

    enum AcceptedValues : uint8_t {
        AcceptsInt32 = 1 << 0,
        AcceptsDouble = 1 << 1,
        AcceptsString = 1 << 2,
        AcceptsObject = 1 << 3,
        AcceptsBoolean = 1 << 4,
        AcceptsNull = 1 << 5,
        HoldsRawValueFromBirth = 1 << 6,
        StructureDecidesAcceptance = 1 << 7,
    };

    struct FastField {
        uint16_t offset;
        SlotRepresentation representation;
        uint8_t acceptedValues;
    };
    // One for each name, in the order of the names. Empty without a fast path.
    std::span<const FastField> fastFields() const LIFETIME_BOUND { return m_fastFields.span(); }

    static constexpr size_t maxFastPathWords = 32;
    // The first of those words holds the order and the shape.
    static constexpr size_t maxNumberOfWholeWordFastFields = maxFastPathWords - 1;

private:
    friend class ObjectBuilder;
    friend class Interpreter::ObjectLiteralPlans;

    struct PlannedField {
        bool isExtraProperty() const { return !field; }

        const FieldData* field { nullptr };
        bool acceptsEveryInt32 { false };
        bool acceptsEveryNumber { false };
        bool acceptsBooleans { false };
        unsigned extraPropertyIndex { 0 };
    };

    struct InitialWord {
        uint32_t offset;
        uint64_t word;
    };

    ObjectBuilderPlan(State&, const LayoutData&);

    void createInitialBody(uint32_t orderAndCount);
    void createFastPath();

    State& m_state;
    const LayoutData& m_layout;
    Vector<PlannedField> m_fields;
    Vector<RefPtr<UniquedStringImpl>> m_extraPropertyNames;
    Vector<uint16_t> m_keyOrder;
    Vector<uint8_t> m_initialBody;
    Vector<FastField> m_fastFields;
    Vector<InitialWord> m_initialWords;
    mutable Vector<uint32_t> m_acceptedStructureIDs;
    Allocator m_allocator;
    size_t m_allocationSize { 0 };
    bool m_namesEveryFieldOfBirth { false };
};

class ObjectBuilder {
    WTF_FORBID_HEAP_ALLOCATION;
    WTF_MAKE_NONCOPYABLE(ObjectBuilder);
public:
    // False for a layout about whose objects the compiler has proven something that only holds for the allocations it has seen.
    static bool canBuildWithoutCompilerKnowledge(const LayoutData&);
    // Every name is a field that lies in line, and the plan hasFastPath().
    static std::unique_ptr<ObjectBuilderPlan> tryCreatePlan(State&, const LayoutData&, std::span<UniquedStringImpl* const> names);
    // A name that is no field of the layout becomes an extra property, and a field may lie out of line. Where the plan has no fast path only an ObjectBuilder builds by it.
    static std::unique_ptr<ObjectBuilderPlan> tryCreatePlanWithExtraProperties(State&, const LayoutData&, std::span<UniquedStringImpl* const> names);
    // Of the records that parsers may make (ProgramTables::parsedRecordFields), the smallest that has every name and whose plan is accepted. The fields that it has beside them stay absent.
    static std::unique_ptr<ObjectBuilderPlan> tryCreatePlanOfSmallestRecord(State&, std::span<UniquedStringImpl* const> names, bool (*accepts)(const ObjectBuilderPlan&));

    // Needs hasFastPath(), the Structure of the plan's layout, and one value for each name given to tryCreatePlan(), in that order.
    // Takes any cell that is not a string for an object, and anything that is not a number, a boolean or a cell for null: the caller passes no symbol, BigInt or undefined.
    static JSObject* tryBuild(VM&, const ObjectBuilderPlan&, Structure*, const EncodedJSValue* values);

    // One object, field by field, for a caller that has numbers and booleans unboxed. Nothing is allocated before finish(), and nothing collects while the builder lives.
    ObjectBuilder(VM&, const ObjectBuilderPlan&);

    // False: the field does not take the value. The caller gives the object up.
    bool tryStore(unsigned index, JSValue);
    bool tryStoreInt32(unsigned index, int32_t);
    bool tryStoreDouble(unsigned index, double);
    bool tryStoreBoolean(unsigned index, bool);
    // Null unless every field of the plan is stored.
    JSObject* finish();

private:
    enum class NamesOfPlan : bool { AreFieldsInLine, MayBeExtraProperties };
    static std::unique_ptr<ObjectBuilderPlan> tryCreatePlan(State&, const LayoutData&, std::span<UniquedStringImpl* const> names, NamesOfPlan);
    static bool tryStoreSlow(const ObjectBuilderPlan&, uint8_t* cell, size_t index, JSValue);

    static constexpr unsigned inlineFieldCapacity = 64;
    static constexpr unsigned inlineExtraPropertyCapacity = 8;
    static constexpr unsigned numberOfHiddenWords = 2;
    static constexpr size_t inlineCellSize = WTF::roundUpToMultipleOf<16>(offsetOfFirstField + (inlineFieldCapacity + numberOfHiddenWords) * sizeof(EncodedJSValue));
    static constexpr size_t inlineExtensionSize = WTF::roundUpToMultipleOf<minExtensionSize>(sizeof(EncodedJSValue) + inlineFieldCapacity * sizeof(EncodedJSValue));

    static constexpr size_t numberOfWordsFor(size_t numberOfBytes) { return WTF::roundUpToMultipleOf<sizeof(uint64_t)>(numberOfBytes) / sizeof(uint64_t); }

    std::span<uint8_t> cellBytes() { return asMutableByteSpan(m_cellWords.mutableSpan()); }
    std::span<uint8_t> extensionBytes() { return asMutableByteSpan(m_extensionWords.mutableSpan()); }
    void* slotOf(const FieldData& field) { return (field.isOutOfLine() ? extensionBytes() : cellBytes()).subspan(field.slot.offset).data(); }
    void didStore(unsigned index) { m_stored.set(index); }

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
