/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStaticsEmitter.h"

#if ENABLE(AOT)

#include "AOTBarrierEmitter.h"
#include "AOTMemoryLocation.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTSlotEmitter.h"
#include "JSArrayBufferView.h"

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

void StaticsEmitter::storeStaticField(VariableID id, Boxed stored, const Type& storedType, bool isUndoneAtRestart)
{
    emitter<SlotEmitter>().store(addressOfStaticField(id), SlotRepresentation::JSValue, stored);
    if (!isUndoneAtRestart)
        emitter<RuntimeCallEmitter>().countStoreForEffectsNet(CountInEffectsNet::StaticFieldsStored);
    if (storedType.hasCells())
        emitter<BarrierEmitter>().markCard(m_mentions.add(NumberSpace::CardOfVariable, id));
    refreshViewMirror(id, stored, storedType);
}

Operand StaticsEmitter::loadFieldOfEnvironment(VariableID id, LayoutID layout, uint32_t field)
{
    PlannedVariable variable = m_plan.variable(id);
    RELEASE_ASSERT(variable.hasWordInStatics() && variable.encoding == VariableEncoding::Plain);
    return emitter<SlotEmitter>().load(instanceWord(m_heaps.at(m_program, MemoryLocation::environmentField(layout, field)), m_mentions.add(NumberSpace::Variable, id)), variable.representation);
}

void StaticsEmitter::storeFieldOfEnvironment(VariableID id, LayoutID layout, uint32_t field, Operand stored, const Type& storedType, bool isUndoneAtRestart)
{
    PlannedVariable variable = m_plan.variable(id);
    RELEASE_ASSERT(variable.hasWordInStatics() && variable.encoding == VariableEncoding::Plain);
    emitter<SlotEmitter>().store(instanceWord(m_heaps.at(m_program, MemoryLocation::environmentField(layout, field)), m_mentions.add(NumberSpace::Variable, id)), variable.representation, stored);
    if (!isUndoneAtRestart)
        emitter<RuntimeCallEmitter>().countStoreForEffectsNet(CountInEffectsNet::VariablesOfScopesInStaticsStored);
    if (traitsOf(variable.representation).holdsCell && storedType.hasCells())
        emitter<BarrierEmitter>().markCard(m_mentions.add(NumberSpace::CardOfVariable, id));
    refreshViewMirror(id, stored, storedType);
}

Operand StaticsEmitter::loadVariable(VariableID id)
{
    return emitter<SlotEmitter>().load(addressOf(id), m_plan.variable(id).representation);
}

void StaticsEmitter::storeVariable(VariableID id, Operand stored, const Type& storedType, bool isUndoneAtRestart)
{
    PlannedVariable variable = m_plan.variable(id);
    emitter<SlotEmitter>().store(addressOf(id), variable.representation, stored);
    if (!isUndoneAtRestart)
        emitter<RuntimeCallEmitter>().countStoreForEffectsNet(CountInEffectsNet::VariablesOfModulesStored);
    if (traitsOf(variable.representation).holdsCell && storedType.hasCells())
        emitter<BarrierEmitter>().markCard(m_mentions.add(NumberSpace::CardOfVariable, id));
    refreshViewMirror(id, stored, storedType);
}

void StaticsEmitter::refreshViewMirror(VariableID id, Operand stored, const Type& storedType)
{
    if (!m_plan.variable(id).hasViewMirror)
        return;
    auto wordOfMirror = [&](int32_t offset) {
        return instanceWord(m_heaps.entire(AbstractHeap::TypedArrayStorage), m_mentions.add(NumberSpace::Variable, id.index(), offset));
    };
    const ClassPart* onlyClass = storedType.primitives || (!stored.is<CellPointer>() && !stored.is<Boxed>()) ? nullptr : singleBuiltinClassOf(types(), storedType);
    TypedArrayType typedArrayType = onlyClass ? m_program.builtins().typedArrayTypeOf(onlyClass->builtinClass) : NotTypedArray;
    bool mirrorsAlone = !m_plan.variable(id).firstViewOfBuffer;
    if (mirrorsAlone && isTypedView(typedArrayType)) {
        Machine::DeclaredAccessesSuspension mirrorsStore(m_out);
        CellPointer view(stored.value);
        Integer length = loadField<Integer>(view, m_heaps.JSArrayBufferView_length);
        int64_t greatest = (1ll << 31) >> logElementSize(typedArrayType);
        Integer mirrored = select(below(length, greatest), length, greatest);
        Condition hasFixedLength = hasNoBits(loadByte(view, m_heaps.JSArrayBufferView_mode), resizabilityAndAutoLengthMask);
        m_out.store64(loadField<Pointer>(view, m_heaps.JSArrayBufferView_vector).value(), wordOfMirror(viewMirrorVectorOffset));
        m_out.store64(select(hasFixedLength, mirrored, 0).value(), wordOfMirror(viewMirrorLengthOffset));
    } else if (mirrorsAlone && storedType.isUndefinedOrNull()) {
        Machine::DeclaredAccessesSuspension mirrorsStore(m_out);
        m_out.store64(m_out.constInt64(0), wordOfMirror(viewMirrorLengthOffset));
    } else
        callLeaf<Integer>(RuntimeOperation::RefreshViewMirror, { Integer(m_out.valueOf(addressOf(id))), Pointer(m_instance) });
}

Integer StaticsEmitter::loadMirroredLength(VariableID id, bool isOfBuffer)
{
    RELEASE_ASSERT(m_plan.variable(id).hasViewMirror);
    int32_t offset = isOfBuffer ? bufferLengthOffsetInViewMirror : viewMirrorLengthOffset;
    return zeroExtended(Word32(m_out.load32(instanceWord(m_heaps.entire(AbstractHeap::TypedArrayStorage), m_mentions.add(NumberSpace::Variable, id.index(), offset)))));
}

Pointer StaticsEmitter::loadMirroredVector(VariableID id)
{
    RELEASE_ASSERT(m_plan.variable(id).hasViewMirror);
    return Pointer(m_out.load64(instanceWord(m_heaps.entire(AbstractHeap::TypedArrayStorage), m_mentions.add(NumberSpace::Variable, id.index(), viewMirrorVectorOffset))));
}

CellPointer StaticsEmitter::loadMirroredView(VariableID id)
{
    RELEASE_ASSERT(m_plan.variable(id).hasViewMirror);
    return CellPointer(m_out.load64(instanceWord(m_heaps.entire(AbstractHeap::TypedArrayStorage), m_mentions.add(NumberSpace::Variable, id))));
}

ClassGuardKey StaticsEmitter::keyOfGuard(LayoutID classLayout, ClassSide side)
{
    RELEASE_ASSERT(classLayout && m_plan.hasClassRecord(classLayout));
    return MentionList::keyOfClassGuard(classLayout, side);
}

ClassGuardKey StaticsEmitter::keyOfGuard(LayoutID classLayout, ClassSide side, PropertyKeyID name, AskerOfClassGuardOfName asker)
{
    RELEASE_ASSERT(classLayout && name && m_plan.hasClassRecord(classLayout) && (asker == AskerOfClassGuardOfName::Reads || side == ClassSide::Prototype));
    if (!liesInThread(name))
        return MentionList::keyOfClassGuardOfName(classLayout, side, NumberSpace::Name, name.index(), asker);
    const PropertyKey& threadLocalKey = types().at(name);
    RELEASE_ASSERT(threadLocalKey.kind == PropertyKeyKind::Text);
    return MentionList::keyOfClassGuardOfName(classLayout, side, NumberSpace::NameOfText, threadLocalKey.text.index(), asker);
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

TypedPointer StaticsEmitter::rowOfCountsOfGuards(std::span<const Vector<ClassGuardKey, 4>> slots)
{
    return instanceWord(m_heaps.AOTInstance_classGuards, m_mentions.rowOfClassGuardCounts(slots));
}

TypedPointer StaticsEmitter::wordOfEnvironmentHandedBack(LayoutID layout)
{
    RELEASE_ASSERT(m_plan.layout(layout).cellsAreHandedBack);
    return instanceWord(m_heaps.collectorState, m_mentions.add(NumberSpace::EnvironmentHandedBack, layout));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
