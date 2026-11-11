/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLayoutData.h"
#include "AOTVector.h"
#include "Allocator.h"
#include "JSCJSValue.h"
#include <memory>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/ForbidHeapAllocation.h>
#include <wtf/MathExtras.h>
#include <wtf/Noncopyable.h>
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
    bool hasFastPath() const { return !m_fastFields.isEmpty(); }
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
    std::span<const FastField> fastFields() const LIFETIME_BOUND { return m_fastFields.span(); }

    static constexpr size_t maxFastPathWords = 32;
    static constexpr size_t maxNumberOfWholeWordFastFields = maxFastPathWords - 1;

private:
    friend class ObjectBuilder;
    friend class Interpreter::ObjectLiteralPlans;

    struct PlannedField {
        const FieldData* field { nullptr };
        bool acceptsEveryInt32 { false };
        bool acceptsEveryNumber { false };
        bool acceptsBooleans { false };
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
    static bool canBuildWithoutCompilerKnowledge(const LayoutData&);
    static std::unique_ptr<ObjectBuilderPlan> tryCreatePlan(State&, const LayoutData&, std::span<UniquedStringImpl* const> names);
    static std::unique_ptr<ObjectBuilderPlan> tryCreatePlanOfSmallestRecord(State&, std::span<UniquedStringImpl* const> names, bool (*accepts)(const ObjectBuilderPlan&));

    static JSObject* tryBuild(VM&, const ObjectBuilderPlan&, Structure*, const EncodedJSValue* values);

private:
    static bool tryStoreSlow(const ObjectBuilderPlan&, uint8_t* cell, size_t index, JSValue);
};

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
