/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTObjectBuilder.h"
#include "AOTPrimitives.h"
#include "AOTSlotAccess.h"
#include "AOTState.h"
#include "JSCInlines.h"
#include <wtf/UnalignedAccess.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

ALWAYS_INLINE JSObject* ObjectBuilder::tryBuild(VM& vm, const ObjectBuilderPlan& plan, Structure* structure, const EncodedJSValue* values, const uint8_t* kinds, uint8_t kindsOfAll)
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
    if (!(kindsOfAll & ~plan.m_acceptedByConsecutiveWords)) [[likely]] {
        auto* words = reinterpret_cast<EncodedJSValue*>(cell + fields->offset);
        size_t index = numberOfFields & 1;
        if (index)
            WTF::atomicStore<MemoryOrder::Relaxed>(&words[0], values[0]);
#pragma clang loop unroll(disable)
        for (; index < numberOfFields; index += 2) {
            WTF::atomicStore<MemoryOrder::Relaxed>(&words[index], values[index]);
            WTF::atomicStore<MemoryOrder::Relaxed>(&words[index + 1], values[index + 1]);
        }
        numberOfFields = 0;
    }
#pragma clang loop unroll(disable)
    for (size_t index = 0; index < numberOfFields; ++index) {
        uint32_t field = WTF::unalignedLoad<uint32_t>(fields + index);
        auto representation = static_cast<SlotRepresentation>(static_cast<uint8_t>(field >> 16));
        uint32_t acceptedValues = field >> 24;
        void* slot = cell + static_cast<uint16_t>(field);
        JSValue value = JSValue::decode(values[index]);
        uint32_t kind = kinds[index];
        if (kind == Plan::AcceptsNull) [[unlikely]]
            hasEveryFieldOfBirth &= !(acceptedValues & Plan::HoldsRawValueFromBirth);
        bool isCell = kind & (Plan::AcceptsString | Plan::AcceptsObject);
        if (acceptedValues & kind || (isCell && value.asCell()->structureID().bits() == acceptedStructureIDs[index])) [[likely]] {
            if (representation == SlotRepresentation::JSValue) {
                *static_cast<EncodedJSValue*>(slot) = values[index];
                continue;
            }
            if (isCell) {
                if (representation == SlotRepresentation::OptionalCell32 || representation == SlotRepresentation::Cell32) {
                    *static_cast<uint32_t*>(slot) = MarkedBlockCage::encode(plan.m_state.instance.cellCageBase, value.asCell());
                    continue;
                }
                if (representation == SlotRepresentation::Cell) {
                    *static_cast<EncodedJSValue*>(slot) = values[index];
                    continue;
                }
            } else if (kind == Plan::AcceptsInt32) {
                if (representation == SlotRepresentation::Int32 || (representation == SlotRepresentation::OptionalInt32 && value.asInt32() >= minInlineInt32)) {
                    *static_cast<int32_t*>(slot) = value.asInt32();
                    continue;
                }
                if (representation == SlotRepresentation::Double || representation == SlotRepresentation::OptionalDouble) {
                    *static_cast<double*>(slot) = value.asInt32();
                    continue;
                }
                if (representation == SlotRepresentation::Int64 || representation == SlotRepresentation::OptionalInt64) {
                    *static_cast<int64_t*>(slot) = value.asInt32();
                    continue;
                }
            } else if (kind == Plan::AcceptsDouble) {
                if (representation == SlotRepresentation::Double || representation == SlotRepresentation::OptionalDouble) {
                    *static_cast<double*>(slot) = value.asDouble();
                    continue;
                }
            } else if (kind == Plan::AcceptsBoolean) {
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
        cell[offsetOfByteWithMark] &= static_cast<uint8_t>(~byteWithMarkIsUnmarked);
    vm.mutatorFence();
    return object;
}

ALWAYS_INLINE JSObject* ObjectBuilder::copyWords(VM& vm, const ObjectBuilderPlan& plan, JSObject* source)
{
    uint8_t* cell = static_cast<uint8_t*>(plan.m_allocator.allocate(vm.heap, plan.m_allocationSize, nullptr, AllocationFailureMode::Assert));
    StructureID structureID = source->structureID();
    *reinterpret_cast<uint64_t*>(cell) = structureID.bits() | static_cast<uint64_t>(structureID.decode()->typeInfoBlob()) << 32;
    auto* words = reinterpret_cast<uint64_t*>(cell + sizeof(JSCell));
    const auto* sourceWords = reinterpret_cast<const uint64_t*>(reinterpret_cast<const uint8_t*>(source) + sizeof(JSCell));
    size_t numberOfWords = plan.m_numberOfBodyWords;
    size_t index = numberOfWords & 1;
    if (index)
        WTF::atomicStore<MemoryOrder::Relaxed>(&words[0], sourceWords[0]);
#pragma clang loop unroll(disable)
    for (; index < numberOfWords; index += 2) {
        WTF::atomicStore<MemoryOrder::Relaxed>(&words[index], sourceWords[index]);
        WTF::atomicStore<MemoryOrder::Relaxed>(&words[index + 1], sourceWords[index + 1]);
    }
    vm.mutatorFence();
    return reinterpret_cast<JSObject*>(cell);
}

template<typename CopyObject>
ALWAYS_INLINE JSObject* ObjectBuilder::tryCopy(VM& vm, const ObjectBuilderPlan& plan, JSObject* source, uint64_t fieldsWithObjects, const CopyObject& copyObject)
{
    using Plan = ObjectBuilderPlan;
    Instance& instance = plan.m_state.instance;
    const uint8_t* sourceCell = reinterpret_cast<const uint8_t*>(source);
    const Plan::FastField* fields = plan.m_fastFields.span().data();
    JSObject* copies[std::numeric_limits<uint64_t>::digits];
    uint64_t fieldsWithCopies = 0;
    size_t numberOfCopies = 0;
    for (; fieldsWithObjects; fieldsWithObjects &= fieldsWithObjects - 1) {
        size_t index = std::countr_zero(fieldsWithObjects);
        const Plan::FastField& field = fields[index];
        JSValue value = readSlot(field.representation, sourceCell + field.offset, instance);
        if (value && value.isObject()) [[likely]] {
            JSObject* copy = copyObject(asObject(value.asCell()));
            if (!copy) [[unlikely]]
                return nullptr;
            copies[numberOfCopies++] = copy;
            fieldsWithCopies |= 1ull << index;
        }
    }

    JSObject* result = copyWords(vm, plan, source);
    uint8_t* cell = reinterpret_cast<uint8_t*>(result);
    for (size_t copy = 0; fieldsWithCopies; fieldsWithCopies &= fieldsWithCopies - 1, ++copy) {
        const Plan::FastField& field = fields[std::countr_zero(fieldsWithCopies)];
        bool holdsCopy = tryWriteSlot(instance, field.representation, cell + field.offset, copies[copy]);
        RELEASE_ASSERT(holdsCopy);
    }
    vm.mutatorFence();
    return result;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
