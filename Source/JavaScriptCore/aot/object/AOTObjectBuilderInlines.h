/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTObjectBuilder.h"
#include "AOTPrimitives.h"
#include "AOTState.h"
#include "JSCInlines.h"
#include <wtf/UnalignedAccess.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

ALWAYS_INLINE JSObject* ObjectBuilder::tryBuild(VM& vm, const ObjectBuilderPlan& plan, Structure* structure, const EncodedJSValue* values)
{
    using Plan = ObjectBuilderPlan;
    static_assert(sizeof(Plan::FastField) == sizeof(uint32_t) && !OBJECT_OFFSETOF(Plan::FastField, offset) && OBJECT_OFFSETOF(Plan::FastField, representation) == 2 && OBJECT_OFFSETOF(Plan::FastField, acceptedValues) == 3);
    ASSERT(plan.hasFastPath());

    uint8_t* cell = static_cast<uint8_t*>(plan.m_allocator.allocate(vm.heap, plan.m_allocationSize, nullptr, AllocationFailureMode::Assert));
    *reinterpret_cast<uint64_t*>(cell) = structure->id().bits() | static_cast<uint64_t>(structure->typeInfoBlob()) << 32;
    const Plan::InitialWord* initialWord = plan.m_initialWords.span().data();
    const Plan::InitialWord* initialWordsEnd = initialWord + plan.m_initialWords.size();
#pragma clang loop unroll(disable)
    for (; initialWord != initialWordsEnd; ++initialWord)
        *reinterpret_cast<uint64_t*>(cell + initialWord->offset) = initialWord->word;

    bool hasEveryFieldOfBirth = plan.m_namesEveryFieldOfBirth;
    const Plan::FastField* fields = plan.m_fastFields.span().data();
    const uint32_t* acceptedStructureIDs = plan.m_acceptedStructureIDs.span().data();
    size_t numberOfFields = plan.m_fastFields.size();
#pragma clang loop unroll(disable)
    for (size_t index = 0; index < numberOfFields; ++index) {
        uint32_t field = WTF::unalignedLoad<uint32_t>(fields + index);
        auto representation = static_cast<SlotRepresentation>(static_cast<uint8_t>(field >> 16));
        uint32_t acceptedValues = field >> 24;
        void* slot = cell + static_cast<uint16_t>(field);
        JSValue value = JSValue::decode(values[index]);
        uint32_t kind;
        if (value.isInt32())
            kind = Plan::AcceptsInt32;
        else if (value.isCell())
            kind = value.asCell()->isString() ? Plan::AcceptsString : Plan::AcceptsObject;
        else if (value.isDouble())
            kind = Plan::AcceptsDouble;
        else if (value.isBoolean())
            kind = Plan::AcceptsBoolean;
        else {
            kind = Plan::AcceptsNull;
            hasEveryFieldOfBirth &= !(acceptedValues & Plan::HoldsRawValueFromBirth);
        }
        bool isAccepted = acceptedValues & kind;
        if (!isAccepted && value.isCell())
            isAccepted = value.asCell()->structureID().bits() == acceptedStructureIDs[index];
        if (isAccepted) [[likely]] {
            if (representation == SlotRepresentation::JSValue || (representation == SlotRepresentation::Cell && value.isCell())) {
                *static_cast<EncodedJSValue*>(slot) = values[index];
                continue;
            }
            if (value.isCell() && (representation == SlotRepresentation::OptionalCell32 || representation == SlotRepresentation::Cell32)) {
                *static_cast<uint32_t*>(slot) = MarkedBlockCage::encode(plan.m_state.instance.cellCageBase, value.asCell());
                continue;
            }
            if (value.isNumber()) {
                if (representation == SlotRepresentation::Double || representation == SlotRepresentation::OptionalDouble) {
                    *static_cast<double*>(slot) = value.asNumber();
                    continue;
                }
                if (value.isInt32()) {
                    if (representation == SlotRepresentation::Int32 || (representation == SlotRepresentation::OptionalInt32 && value.asInt32() >= minInlineInt32)) {
                        *static_cast<int32_t*>(slot) = value.asInt32();
                        continue;
                    }
                    if (representation == SlotRepresentation::Int64 || representation == SlotRepresentation::OptionalInt64) {
                        *static_cast<int64_t*>(slot) = value.asInt32();
                        continue;
                    }
                }
            } else if (value.isBoolean()) {
                if (representation == SlotRepresentation::Boolean) {
                    *static_cast<uint8_t*>(slot) = value.asBoolean();
                    continue;
                }
                if (representation == SlotRepresentation::OptionalBoolean) {
                    *static_cast<uint8_t*>(slot) = static_cast<uint8_t>(values[index]);
                    continue;
                }
            }
        }
        if (!tryStoreSlow(plan, cell, index, value)) [[unlikely]]
            return nullptr;
    }
    auto* object = reinterpret_cast<JSObject*>(cell);
    if (!hasEveryFieldOfBirth) [[unlikely]]
        object->setPerCellBit(true);
    return object;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
