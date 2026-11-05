/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStaticsEmitter.h"

#if ENABLE(AOT)

#include "AOTBarrierEmitter.h"
#include "AOTMemoryLocation.h"
#include "AOTSlotEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

TypedPointer StaticsEmitter::addressOf(VariableID id)
{
    RELEASE_ASSERT(id && m_plan.variable(id).hasWordInStatics());
    return instanceWord(m_heaps.at(m_program, MemoryLocation::moduleVariable(id)), m_mentions.add(NumberSpace::Variable, id));
}

bool StaticsEmitter::holdsStaticField(VariableID id) const
{
    PlannedVariable variable = m_plan.variable(id);
    return id && variable.hasWordInStatics() && variable.encoding == VariableEncoding::Plain && variable.representation == SlotRepresentation::JSValue;
}

TypedPointer StaticsEmitter::addressOfStaticField(VariableID id)
{
    RELEASE_ASSERT(holdsStaticField(id));
    return instanceWord(m_heaps.entire(AbstractHeap::NamedProperties), m_mentions.add(NumberSpace::Variable, id));
}

void StaticsEmitter::storeStaticField(VariableID id, Boxed stored)
{
    emitter<SlotEmitter>().store(addressOfStaticField(id), SlotRepresentation::JSValue, stored);
    emitter<BarrierEmitter>().markCard(m_mentions.add(NumberSpace::CardOfVariable, id));
}

Operand StaticsEmitter::loadEncodedVariable(VariableID id)
{
    return emitter<SlotEmitter>().load(addressOf(id), m_plan.variable(id).representation);
}

Operand StaticsEmitter::loadVariable(VariableID id)
{
    PlannedVariable variable = m_plan.variable(id);
    SlotEmitter& slots = emitter<SlotEmitter>();
    Operand loaded = loadEncodedVariable(id);
    switch (variable.encoding) {
    case VariableEncoding::Plain:
    case VariableEncoding::FunctionAsZero:
        return loaded;
    case VariableEncoding::UndefinedAsZero:
        RELEASE_ASSERT(representsAbsentAsZero(variable.representation));
        return slots.absentAsUndefined(loaded, variable.representation);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void StaticsEmitter::storeVariable(VariableID id, Operand stored, const Type& storedType)
{
    PlannedVariable variable = m_plan.variable(id);
    switch (variable.encoding) {
    case VariableEncoding::Plain:
    case VariableEncoding::FunctionAsZero:
        break;
    case VariableEncoding::UndefinedAsZero: {
        if (!(storedType.primitives & Primitive::Undefined))
            break;
        Boxed boxed = stored.as<Boxed>();
        Condition isUndefined = equal(boxed, undefined());
        stored = select(isUndefined, boxedConstant(JSValue()), boxed);
        break;
    }
    }
    emitter<SlotEmitter>().store(addressOf(id), variable.representation, stored);
    if (traitsOf(variable.representation).holdsCell)
        emitter<BarrierEmitter>().markCard(m_mentions.add(NumberSpace::CardOfVariable, id));
}

ClassGuardKey StaticsEmitter::keyOfGuard(LayoutID classLayout, ClassSide side)
{
    RELEASE_ASSERT(classLayout && m_plan.hasClassRecord(classLayout));
    return MentionList::keyOfClassGuard(classLayout, side);
}

ClassGuardKey StaticsEmitter::keyOfGuard(LayoutID classLayout, ClassSide side, PropertyKeyID name, bool isAskedByStoresAlone)
{
    RELEASE_ASSERT(classLayout && name && m_plan.hasClassRecord(classLayout) && (!isAskedByStoresAlone || side == ClassSide::Prototype));
    if (!liesInThread(name))
        return MentionList::keyOfClassGuardOfName(classLayout, side, NumberSpace::Name, name.index(), isAskedByStoresAlone);
    const PropertyKey& threadLocalKey = types().at(name);
    RELEASE_ASSERT(threadLocalKey.kind == PropertyKeyKind::Text);
    return MentionList::keyOfClassGuardOfName(classLayout, side, NumberSpace::NameOfText, threadLocalKey.text.index(), isAskedByStoresAlone);
}

ClassGuardKey StaticsEmitter::keyOfGuardOfNames(LayoutID classLayout, ClassSide side, std::span<const PropertyKeyID> names)
{
    RELEASE_ASSERT(classLayout && names.size() > 1 && m_plan.hasClassRecord(classLayout));
    return MentionList::keyOfClassGuardOfNames(classLayout, side, names);
}

TypedPointer StaticsEmitter::guardByte(const ClassGuardKey& guard)
{
    return instanceWord(m_heaps.AOTInstance_classGuards, m_mentions.classGuard(guard));
}

TypedPointer StaticsEmitter::countOfGuards(std::span<const ClassGuardKey> guards)
{
    return instanceWord(m_heaps.AOTInstance_classGuards, m_mentions.classGuardCount(guards));
}

TypedPointer StaticsEmitter::wordOfEnvironmentHandedBack(LayoutID layout)
{
    RELEASE_ASSERT(m_plan.layout(layout).isEscapedOffset);
    return instanceWord(m_heaps.collectorState, m_mentions.add(NumberSpace::EnvironmentHandedBack, layout));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
