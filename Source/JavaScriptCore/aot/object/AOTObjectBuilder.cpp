/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTObjectBuilder.h"

#if ENABLE(AOT)

#include "AOTCheckedSpan.h"
#include "AOTInstance.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTPrimitives.h"
#include "AOTProgramIdentifiers.h"
#include "AOTRuntimeReporter.h"
#include "AOTRuntimeType.h"
#include "AOTSlotAccess.h"
#include "AOTState.h"
#include "AOTVector.h"
#include "JSCInlines.h"
#include <algorithm>
#include <optional>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMallocInlines.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(ObjectBuilderPlan);

ObjectBuilderPlan::ObjectBuilderPlan(State& state, const LayoutData& layout)
    : m_state(state)
    , m_layout(layout)
{
}

static size_t numberOfBodyWords(const LayoutData& layout)
{
    return WTF::roundUpToMultipleOf<sizeof(uint64_t)>(layout.size - sizeof(JSCell)) / sizeof(uint64_t);
}

void ObjectBuilderPlan::createInitialBody(uint32_t orderAndCount)
{
    m_initialBody.fill(0, m_layout.size - sizeof(JSCell));
    auto slotAt = [&](uint32_t offsetInCell, size_t width) {
        return checkedSubspan(m_initialBody.mutableSpan(), offsetInCell - sizeof(JSCell), width).data();
    };
    for (const FieldData& field : m_layout.fields()) {
        if (!field.isOutOfLine())
            clearSlot(field.slot.representation(), slotAt(field.slot.offset, widthOf(field.slot.representation())));
    }
    *reinterpret_cast<uint32_t*>(slotAt(offsetOfOrderWord, sizeof(uint32_t))) = orderAndCount;
    *reinterpret_cast<uint32_t*>(slotAt(offsetOfShapeWord, sizeof(uint32_t))) = shapeWordOf(m_layout.shape);
}

void ObjectBuilderPlan::createFastPath()
{
    static_assert(sizeof(JSCell) + maxFastPathWords * sizeof(uint64_t) <= std::numeric_limits<decltype(FastField::offset)>::max());
    size_t numberOfWords = numberOfBodyWords(m_layout);
    ASSERT(numberOfWords <= maxFastPathWords);

    RuntimeLayoutTable& layouts = m_state.objects->layouts;
    BitVector namedFields(m_layout.numberOfFields);
    for (const PlannedField& planned : m_fields) {
        const FieldData& field = *planned.field;
        const RuntimeType& type = layouts.typeOf(field);
        OptionSet<PrimitiveSet> primitives = type.primitiveSet();
        uint8_t accepted = 0;
        if (planned.acceptsEveryInt32)
            accepted |= AcceptsInt32;
        if (planned.acceptsEveryNumber)
            accepted |= AcceptsDouble;
        if (planned.acceptsBooleans)
            accepted |= AcceptsBoolean;
        if (primitives.contains(PrimitiveSet::String))
            accepted |= AcceptsString;
        if (type.acceptsEveryObject())
            accepted |= AcceptsObject;
        if (layouts.slotAdmits(field, jsNull()))
            accepted |= AcceptsNull;
        if (field.holdsRawValueFromBirth)
            accepted |= HoldsRawValueFromBirth;
        bool structureDecidesAcceptance = std::ranges::all_of(type.cellTests(), [](const RuntimeCellTest& test) {
            return test.kind == CellTestKind::IsObject || test.kind == CellTestKind::IsCallable || test.kind == CellTestKind::CellTypeIs || test.kind == CellTestKind::ShapeIn || test.kind == CellTestKind::InheritsClassInfo;
        });
        if (structureDecidesAcceptance)
            accepted |= StructureDecidesAcceptance;
        m_fastFields.append({ static_cast<uint16_t>(field.slot.offset), field.slot.representation(), accepted });
        namedFields.set(m_layout.indexOf(field));
    }
    m_acceptedByConsecutiveWords = AcceptsInt32 | AcceptsDouble | AcceptsString | AcceptsObject | AcceptsBoolean;
    for (size_t index = 0; index < m_fastFields.size(); ++index) {
        const FastField& field = m_fastFields[index];
        if (field.representation != SlotRepresentation::JSValue || field.offset != m_fastFields[0].offset + index * sizeof(EncodedJSValue))
            m_acceptedByConsecutiveWords = 0;
        m_acceptedByConsecutiveWords &= field.acceptedValues;
    }
    m_acceptedStructureIDs.fill(0, m_fastFields.size());
    m_numberOfBodyWords = numberOfWords;
    m_namesEveryFieldOfBirth = true;
    for (const FieldData& field : m_layout.fields()) {
        if ((field.isFilledAtBirth || field.holdsRawValueFromBirth) && !namedFields.get(m_layout.indexOf(field)))
            m_namesEveryFieldOfBirth = false;
    }
    Vector<uint64_t, maxFastPathWords> initialWords;
    initialWords.fill(0, numberOfWords);
    memcpySpan(asMutableByteSpan(initialWords.mutableSpan()), m_initialBody.span());
    BitVector isOverwritten(numberOfWords);
    for (const FastField& field : m_fastFields) {
        if (widthOf(field.representation) == sizeof(uint64_t))
            isOverwritten.set((field.offset - sizeof(JSCell)) / sizeof(uint64_t));
    }
    for (size_t index = 0; index < numberOfWords; ++index) {
        if (!isOverwritten.get(index))
            m_initialWords.append({ static_cast<uint32_t>(sizeof(JSCell) + index * sizeof(uint64_t)), initialWords[index] });
    }
    m_allocator = m_state.instance.vm->cellSpace().allocatorFor(sizeof(JSCell) + numberOfWords * sizeof(uint64_t), AllocatorForMode::EnsureAllocator);
    m_allocationSize = m_allocator.cellSize();
}

static bool defersClosure(const FieldData& field)
{
    return traitsOf(field.slot.representation()).valueKind == SlotValueKind::DeferredClosure;
}

static bool mayStayAbsent(const FieldData& field)
{
    return (field.flags & fieldIsOptional) && field.canLeaveSlot() && !defersClosure(field);
}

bool ObjectBuilder::canBuildWithoutCompilerKnowledge(const LayoutData& layout)
{
    if (layout.isDerived || !layout.impossibleMutations().isEmpty())
        return false;
    return std::ranges::none_of(layout.fields(), [](const FieldData& field) {
        return field.flags & (fieldIsNeverStoredWithOtherType | fieldIsNeverDeleted | fieldIsNeverRedefined);
    });
}

std::unique_ptr<ObjectBuilderPlan> ObjectBuilder::tryCreatePlan(State& state, const LayoutData& layout, std::span<UniquedStringImpl* const> names)
{
    if (layout.kind() != LayoutKind::Record || !layout.shape || layout.size < offsetOfFirstField)
        return nullptr;
    if (names.empty() || numberOfBodyWords(layout) > ObjectBuilderPlan::maxFastPathWords)
        return nullptr;

    std::span<const FieldData> fields = layout.fields();
    if (static_cast<size_t>(std::ranges::count_if(fields, &FieldData::isRequired)) > names.size())
        return nullptr;

    VM& vm = *state.instance.vm;
    RuntimeLayoutTable& layouts = state.objects->layouts;
    BitVector namedFields(fields.size());
    Vector<uint16_t, 16> keyOrder;
    for (UniquedStringImpl* name : names) {
        const FieldData* field = layouts.field(vm, layout, name);
        if (!field || field->isPrivate() || field->attributes() || defersClosure(*field) || field->isOutOfLine())
            return nullptr;
        uint16_t index = layout.indexOf(*field);
        if (!namedFields.add(index))
            return nullptr;
        keyOrder.append(index);
    }
    for (size_t index = 0; index < fields.size(); ++index) {
        if (!namedFields.get(index) && !mayStayAbsent(fields[index]))
            return nullptr;
    }

    std::optional<uint32_t> orderAndCount = state.objects->keyOrders.orderStartingWith(layout, keyOrder.span());
    if (!orderAndCount)
        return nullptr;

    countEvent(vm, RuntimeCounterID::RecordPlansTried);
    std::unique_ptr<ObjectBuilderPlan> plan { new ObjectBuilderPlan(state, layout) };
    for (uint16_t index : keyOrder) {
        const FieldData* field = &fields[index];
        const RuntimeType& type = layouts.typeOf(*field);
        OptionSet<PrimitiveSet> primitives = type.primitiveSet();
        auto acceptsEveryIntegerIn = [&](Typed::Range integers) {
            auto range = type.integerRange();
            return primitives.contains(PrimitiveSet::Integer) && (!range || integers.isSubsetOf({ range->first, range->second }));
        };
        bool keepsMarkersAmongInt32s = field->slot.representation() == SlotRepresentation::OptionalInt32;
        bool acceptsEveryInt32 = acceptsEveryIntegerIn(keepsMarkersAmongInt32s ? Typed::Range { minInlineInt32, INT32_MAX } : Typed::Range::int32());
        bool acceptsEveryNumber = acceptsEveryIntegerIn(Typed::Range::full()) && primitives.containsAll({ PrimitiveSet::WholeNumber, PrimitiveSet::FractionalNumber });
        bool acceptsBooleans = primitives.containsAll({ PrimitiveSet::True, PrimitiveSet::False });
        plan->m_fields.append({ .field = field, .acceptsEveryInt32 = acceptsEveryInt32, .acceptsEveryNumber = acceptsEveryNumber, .acceptsBooleans = acceptsBooleans });
    }
    plan->m_orderAndCount = *orderAndCount;
    plan->createInitialBody(*orderAndCount);
    plan->createFastPath();
    return plan;
}

std::unique_ptr<ObjectBuilderPlan> ObjectBuilder::tryCreatePlanOfSmallestRecord(State& state, std::span<UniquedStringImpl* const> names, bool (*accepts)(const ObjectBuilderPlan&))
{
    VM& vm = *state.instance.vm;
    countEvent(vm, RuntimeCounterID::RecordPlanChoices);
    std::span<const Number24> shapesWithRarestName;
    for (UniquedStringImpl* name : names) {
        std::optional<uint32_t> number = identifiersOf(state.instance).numberOf(name);
        if (!number)
            return nullptr;
        std::span<const Number24> shapesWithName = state.tables.parsedRecordShapesWithName(*number);
        if (shapesWithName.empty())
            return nullptr;
        if (shapesWithRarestName.empty() || shapesWithName.size() < shapesWithRarestName.size())
            shapesWithRarestName = shapesWithName;
    }

    RuntimeLayoutTable& layouts = state.objects->layouts;
    size_t recordsAsked = 0;
    std::unique_ptr<ObjectBuilderPlan> chosen;
    for (Number24 shape : shapesWithRarestName) {
        ++recordsAsked;
        const LayoutData& layout = layouts.layoutForShape(shape);
        if (layout.numberOfFields < names.size() || !canBuildWithoutCompilerKnowledge(layout))
            continue;
        std::unique_ptr<ObjectBuilderPlan> plan = tryCreatePlan(state, layout, names);
        if (plan && accepts(*plan)) {
            chosen = WTF::move(plan);
            break;
        }
    }
    countEvent(vm, RuntimeCounterID::RecordPlanCandidates, recordsAsked);
    return chosen;
}

bool ObjectBuilder::tryStoreSlow(const ObjectBuilderPlan& plan, uint8_t* cell, size_t index, JSValue value)
{
    std::span<const ObjectBuilderPlan::FastField> fields = plan.m_fastFields.span();
    for (const ObjectBuilderPlan::FastField& unwritten : fields.subspan(index))
        clearSlot(unwritten.representation, cell + unwritten.offset);

    State& state = plan.m_state;
    const FieldData& field = *plan.m_fields[index].field;
    if (!state.objects->layouts.slotAdmits(field, value) || !tryWriteSlot(state.instance, field.slot.representation(), cell + fields[index].offset, value))
        return false;
    if (value.isObject() && fields[index].acceptedValues & ObjectBuilderPlan::StructureDecidesAcceptance)
        plan.m_acceptedStructureIDs[index] = value.asCell()->structureID().bits();
    return true;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
