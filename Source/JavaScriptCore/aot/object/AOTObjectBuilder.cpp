/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTObjectBuilder.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTPrimitives.h"
#include "AOTProgramStrings.h"
#include "AOTRuntimeType.h"
#include "AOTSlotAccess.h"
#include "AOTState.h"
#include "JSCInlines.h"
#include "PureNaN.h"
#include <algorithm>
#include <optional>
#include <wtf/HashSet.h>
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

void ObjectBuilderPlan::createInitialBody(uint32_t orderAndCount)
{
    m_initialBody.fill(0, m_layout.size - sizeof(JSCell));
    auto slotAt = [&](uint32_t offsetInCell) {
        return m_initialBody.mutableSpan().subspan(offsetInCell - sizeof(JSCell)).data();
    };
    for (const FieldData& field : m_layout.fields()) {
        if (!field.isOutOfLine())
            clearSlot(field.slot.representation(), slotAt(field.slot.offset));
    }
    *reinterpret_cast<uint32_t*>(slotAt(offsetOfOrderWord)) = orderAndCount;
    *reinterpret_cast<uint32_t*>(slotAt(offsetOfShapeWord)) = shapeWordOf(m_layout.shape);
}

static bool canBeExtraProperty(VM& vm, const LayoutData& layout, UniquedStringImpl* name)
{
    // The slot belongs to the layout, while compiled code leaves out its test for extra properties where this shape cannot gain any.
    if (!layout.extraPropertiesSlot() || layout.impossibleMutations().contains(ObjectMutation::AddProperty))
        return false;
    return !name->isSymbol() && name != vm.propertyNames->underscoreProto.impl();
}

std::unique_ptr<ObjectBuilderPlan> ObjectBuilder::tryCreatePlan(State& state, const LayoutData& layout, std::span<UniquedStringImpl* const> names)
{
    if (layout.kind() != LayoutKind::Record || !layout.shape || layout.size < offsetOfFirstField)
        return nullptr;

    VM& vm = *state.instance.vm;
    RuntimeLayoutTable& layouts = state.objects->layouts;
    std::span<const FieldData> fields = layout.fields();
    std::unique_ptr<ObjectBuilderPlan> plan { new ObjectBuilderPlan(state, layout) };
    BitVector namedFields(fields.size());
    UncheckedKeyHashSet<UniquedStringImpl*> extraPropertyNames;
    uint32_t numberOfExtraKeys = 0;
    uint32_t numberOfPositionedExtraKeys = 0;
    for (UniquedStringImpl* name : names) {
        const FieldData* field = layouts.field(vm, layout, name);
        if (!field) {
            bool canCountAnotherKey = fields.size() + extraPropertyNames.size() < maxKeyOrderEntry;
            if (!canBeExtraProperty(vm, layout, name) || !canCountAnotherKey || !extraPropertyNames.add(name).isNewEntry)
                return nullptr;
            plan->m_fields.append({ .extraPropertyIndex = static_cast<unsigned>(plan->m_extraPropertyNames.size()) });
            plan->m_extraPropertyNames.append(name);
            numberOfExtraKeys += !parseIndex(*name);
            continue;
        }
        if (field->isPrivate() || traitsOf(field->slot.representation()).valueKind == SlotValueKind::DeferredClosure)
            return nullptr;
        uint16_t index = layout.indexOf(*field);
        if (!namedFields.add(index))
            return nullptr;
        if (numberOfExtraKeys > numberOfPositionedExtraKeys) {
            numberOfPositionedExtraKeys = numberOfExtraKeys;
            plan->m_keyOrder.append(static_cast<uint16_t>(fields.size() + numberOfExtraKeys - 1));
        }
        plan->m_keyOrder.append(index);
        if (field->isOutOfLine())
            plan->m_initialGranuleBits |= granuleBitOf(field->slot.offset);

        const RuntimeType& type = layouts.typeOf(*field);
        OptionSet<PrimitiveSet> primitives = type.primitiveSet();
        auto acceptsEveryIntegerIn = [&](Typed::Range integers) {
            auto range = type.integerRange();
            return primitives.contains(PrimitiveSet::Integer) && (!range || integers.isSubsetOf({ range->first, range->second }));
        };
        bool acceptsEveryInt32 = acceptsEveryIntegerIn(Typed::Range::int32());
        bool acceptsEveryNumber = acceptsEveryIntegerIn(Typed::Range::full()) && primitives.containsAll({ PrimitiveSet::WholeNumber, PrimitiveSet::FractionalNumber });
        bool acceptsBooleans = primitives.containsAll({ PrimitiveSet::True, PrimitiveSet::False });
        plan->m_fields.append({ .field = field, .acceptsEveryInt32 = acceptsEveryInt32, .acceptsEveryNumber = acceptsEveryNumber, .acceptsBooleans = acceptsBooleans });
    }
    for (size_t index = 0; index < fields.size(); ++index) {
        bool mayStayAbsent = (fields[index].flags & fieldIsOptional) && fields[index].canLeaveSlot();
        if (!namedFields.get(index) && !mayStayAbsent)
            return nullptr;
    }

    std::optional<uint32_t> orderAndCount = unorderedOrderWord;
    if (!layout.keysAreNeverEnumerated && plan->m_extraPropertyNames.isEmpty())
        orderAndCount = state.objects->keyOrders.orderStartingWith(layout, plan->m_keyOrder.span());
    if (!orderAndCount)
        return nullptr;
    plan->createInitialBody(*orderAndCount);
    return plan;
}

ObjectBuilder::ObjectBuilder(VM& vm, const ObjectBuilderPlan& plan)
    : m_vm(vm)
    , m_plan(plan)
    , m_deferGC(vm)
    , m_stored(plan.numberOfFields())
    , m_granuleBits(plan.m_initialGranuleBits)
    , m_cellWords(numberOfWordsFor(plan.m_layout.size))
    , m_extensionWords(FillWith { }, numberOfWordsFor(plan.m_layout.extensionSize()), 0)
    , m_extraPropertyValues(FillWith { }, plan.m_extraPropertyNames.size(), JSValue())
{
    ASSERT(&vm == plan.m_state.instance.vm);
    memcpySpan(cellBytes().subspan(sizeof(JSCell)), plan.m_initialBody.span());
}

void ObjectBuilder::storeDoubleInExtension(const FieldData& field, double number)
{
    uint32_t overflowOffset = m_plan.m_layout.overflowOffsetOf(field);
    *static_cast<int32_t*>(slotOf(field)) = doubleInExtensionInt32;
    *reinterpret_cast<double*>(extensionBytes().subspan(overflowOffset).data()) = number;
    m_granuleBits |= granuleBitOf(overflowOffset);
}

bool ObjectBuilder::tryStore(unsigned index, JSValue value)
{
    RELEASE_ASSERT(index < m_plan.numberOfFields());
    const ObjectBuilderPlan::PlannedField& planned = m_plan.m_fields[index];
    if (!value)
        return false;
    if (planned.isExtraProperty()) {
        m_extraPropertyValues[planned.extraPropertyIndex] = value;
        didStore(index);
        return true;
    }

    const FieldData& field = *planned.field;
    State& state = m_plan.m_state;
    if (!state.objects->layouts.fieldAccepts(field, value))
        return false;

    if (!tryWriteSlot(*state.strings, field.slot.representation(), slotOf(field), value)) {
        if (field.slot.representation() != SlotRepresentation::Int32OrDouble || !value.isNumber())
            return false;
        storeDoubleInExtension(field, value.asNumber());
    }
    didStore(index);
    return true;
}

bool ObjectBuilder::tryStoreInt32(unsigned index, int32_t number)
{
    const ObjectBuilderPlan::PlannedField& planned = m_plan.m_fields[index];
    if (!planned.acceptsEveryInt32 || !tryWriteInt32(planned.field->slot.representation(), slotOf(*planned.field), number))
        return tryStore(index, jsNumber(number));
    didStore(index);
    return true;
}

bool ObjectBuilder::tryStoreDouble(unsigned index, double number)
{
    const ObjectBuilderPlan::PlannedField& planned = m_plan.m_fields[index];
    if (!planned.acceptsEveryNumber || !tryWriteDouble(planned.field->slot.representation(), slotOf(*planned.field), number))
        return tryStore(index, jsNumber(purifyNaN(number)));
    didStore(index);
    return true;
}

bool ObjectBuilder::tryStoreBoolean(unsigned index, bool value)
{
    const ObjectBuilderPlan::PlannedField& planned = m_plan.m_fields[index];
    if (!planned.acceptsBooleans || !tryWriteBoolean(planned.field->slot.representation(), slotOf(*planned.field), value))
        return tryStore(index, jsBoolean(value));
    didStore(index);
    return true;
}

JSObject* ObjectBuilder::finish()
{
    if (m_stored.bitCount() != m_plan.numberOfFields())
        return nullptr;

    CompiledStructure* structure = m_plan.m_state.objects->structures.objectStructure(m_vm, m_plan.m_layout.shape);
    Object* object = Object::createWithBody(m_vm, structure, cellBytes().subspan(sizeof(JSCell), m_plan.m_initialBody.size()));
    if (m_granuleBits) {
        object->ensureExtensionGranules(m_vm, m_granuleBits);
        uint64_t* granule = object->extension()->granules();
        for (uint64_t remaining = m_granuleBits; remaining; remaining &= remaining - 1)
            *granule++ = m_extensionWords[offsetOfFirstExtensionSlot / sizeof(uint64_t) + std::countr_zero(remaining)];
    }
    if (m_extraPropertyValues.isEmpty())
        return object;

    Vector<Object::ExtraProperty, inlineExtraPropertyCapacity> extraProperties(m_extraPropertyValues.size(), [&](size_t index) {
        return Object::ExtraProperty { m_plan.m_extraPropertyNames[index].get(), m_extraPropertyValues[index] };
    });
    JSObject* extra = Object::createExtraProperties(structure->realm(), extraProperties.span());
    std::optional<Object::InitialKeyOrder> order = object->initialKeyOrder(m_vm, m_plan.m_keyOrder.span(), extra);
    if (!order)
        return nullptr;
    object->setInitialKeyOrder(m_vm, *order, extra);
    return object;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
