/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTEffectsDeclared.h"
#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTPlan.h"
#include "AOTSweep.h"
#include "AOTTypeArena.h"
#include "AOTWalkKnown.h"

namespace JSC { namespace AOT { namespace Typed {

static constexpr AbstractHeapSet heapsUnderStructureInHeirsOfBuiltins { AbstractHeap::NamedProperties, AbstractHeap::PrototypeLinks };

LayoutID layoutKnownByStructureOfClass(const Walker& walker, TypeID type)
{
    if (!type || holdsOnlyPrimitives(type) || !walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::Folding) || !walker.state().remembersPlaces())
        return { };
    const Type& record = walker.types().recordOf(type);
    if (!record.isJavaScriptValue() || record.primitives || !record.hasOnlyLayouts() || record.traitsOfNamed.contains(ObjectTrait::MayHaveLayoutBelow) || !holdsOneMember(record.layouts))
        return { };
    LayoutID layout(positionOf(record.layouts));
    const Layout& heir = walker.program().at(layout);
    BuiltinClassID above = heir.writtenBuiltinBase;
    const Program& program = walker.program();
    bool isArray = above == BuiltinClasses::Array;
    bool isError = above == BuiltinClasses::Error;
    bool structureTellsEveryOwnName = above == BuiltinClasses::Map || above == BuiltinClasses::Set || above == BuiltinClasses::WeakMap || above == BuiltinClasses::WeakSet || above == BuiltinClasses::Promise;
    if (!isKnownByStructureOfClass(heir) || (!structureTellsEveryOwnName && !isArray && !isError))
        return { };
    auto mayLieOutsideStructure = [&](PropertyKeyID key) {
        if (!key)
            return true;
        if (isArray)
            return key == program.keyOf(BuiltinNames::length) || program.isIndexLike(key);
        return isError && program.builtins().mayDefineOnNewInstances(above, program.builtinNameOf(key));
    };
    bool someNameMayLieOutsideStructure = std::ranges::any_of(heir.fields, [&](const Field& field) {
        return mayLieOutsideStructure(field.key);
    }) || std::ranges::any_of(walker.facts().membersOf(layout), [&](const ClassMember& member) {
        return member.side == ClassSide::Prototype && mayLieOutsideStructure(member.key);
    });
    if (someNameMayLieOutsideStructure)
        return { };
    return walker.facts().isEvaluatedAtMostOnce(layout) && walker.plan().hasClassRecord(layout) ? layout : LayoutID();
}

Reference buildLookAtStructureOfClass(Walker& walker, Reference value, LayoutID layout, Origin origin)
{
    origin.isTailCall = false;
    Draft look(Opcode::HasStructureOfClass, origin);
    look.setValueOperand(value);
    look.setLayout(layout);
    Reference built = walker.build(WTF::move(look));
    if (built)
        walker.count(StatisticID::BuiltinSubclassesCheckedByStructure);
    return built;
}

Reference buildNameBehindLookAtStructureOfClass(Walker& walker, Reference value, Reference look, LayoutID layout, Origin origin)
{
    if (!look || !look.isValue() || walker.isUnreachable())
        return { };
    origin.isTailCall = false;
    Draft proof(Opcode::Narrowed, origin);
    proof.setValueOperand(value);
    proof.setTestOperand(look);
    proof.setOperandIndex(OperandIndex::HasStructureOfClass::value);
    proof.setIsOnTakenEdge(true);
    Reference name = walker.build(WTF::move(proof));
    if (!name || !name.isValue() || walker.isUnreachable())
        return name;
    ValueID root = underlyingCellOf(walker.code(), name.id());
    if (!walker.state().isFresh(walker.code().isConstant(root) ? Reference::constant(root) : Reference::value(root)))
        noteKnown(walker, KnownNote::StructureOfClassIsSeen, { }, name, layout.index(), LocationMasks(heapsUnderStructureInHeirsOfBuiltins), name.bits());
    return name;
}

Reference nameBehindLookAtStructureOfClass(const Walker& walker, Reference object, LayoutID layout)
{
    if (!object || !object.isValue())
        return { };
    std::optional<uint64_t> noted = findKnown(walker, KnownNote::StructureOfClassIsSeen, { }, object, layout.index());
    return noted ? Reference::fromBits(static_cast<uint32_t>(*noted)) : Reference();
}

void checkStructureOfClassThatIsRemembered(Walker& walker, Reference name, LayoutID layout, Origin origin)
{
    if (!walker.checksProofs() || walker.isUnreachable())
        return;
    origin.isTailCall = false;
    Draft look(Opcode::HasStructureOfClass, origin);
    look.setValueOperand(name);
    look.setLayout(layout);
    Reference holds = walker.buildTestOfCheck(WTF::move(look));
    if (holds && holds.isValue() && !walker.isUnreachable())
        walker.checkProof(holds, TypeArena::fromPrimitives(Primitive::True), TypeTable::boolean, ProofSource::RememberedFact, origin);
}

void nameFieldInEffectsOfAccessToHeir(const Program& program, Opcode opcode, PropertyKeyID key, Effects& effects)
{
    static_assert(declaredBoundOf(Opcode::PutPropertyOfHeir).writes.containsOnly(AbstractHeapSet { AbstractHeap::NamedProperties }) && !declaredBoundOf(Opcode::PutPropertyOfHeir).mayRunOtherCode());
    MemoryLocation field = MemoryLocation::objectField(key);
    if (opcode == Opcode::GetPropertyOfHeir) {
        effects.reads.add(program, field);
        return;
    }
    RELEASE_ASSERT(opcode == Opcode::PutPropertyOfHeir);
    effects.writes.remove(AbstractHeapSet { AbstractHeap::NamedProperties });
    effects.writes.add(program, field);
}

static LocationMasks heapsUnderValueOfFieldInHeirsOfBuiltins(const Program& program, LayoutID layout, uint32_t field)
{
    LocationMasks heaps(AbstractHeapSet { AbstractHeap::NamedProperties });
    heaps.add(program, MemoryLocation::objectField(fieldAt(program, FieldReference { layout, field }).key));
    return heaps;
}

Reference buildReadOfFieldOfHeir(Walker& walker, Reference name, LayoutID layout, uint32_t field, Origin origin)
{
    Draft read(Opcode::GetPropertyOfHeir, origin);
    read.setObjectOperand(name);
    read.setLayout(layout);
    read.setField(field);
    if (std::optional<uint64_t> noted = findKnown(walker, KnownNote::ValueOfFieldOfHeir, { }, name, field)) {
        Reference held = Reference::fromBits(static_cast<uint32_t>(*noted));
        if (walker.checksProofs())
            walker.checkThatMemoryAnswersAsBelieved(held, WTF::move(read));
        return held;
    }
    Reference built = walker.build(WTF::move(read));
    if (built && built.isValue() && !walker.isUnreachable())
        noteKnown(walker, KnownNote::ValueOfFieldOfHeir, { }, name, field, heapsUnderValueOfFieldInHeirsOfBuiltins(walker.program(), layout, field), built.bits());
    return built;
}

Reference buildStoreIntoFieldOfHeir(Walker& walker, Reference name, Reference stored, LayoutID layout, uint32_t field, Origin origin)
{
    origin.isTailCall = false;
    Reference valueBeforeStore = walker.canRecordUndoableStore() ? buildReadOfFieldOfHeir(walker, name, layout, field, origin) : Reference();
    if (walker.hasFailed() || walker.isUnreachable())
        return { };
    Draft store(Opcode::PutPropertyOfHeir, origin);
    store.setObjectOperand(name);
    store.setValueOperand(stored);
    store.setLayout(layout);
    store.setField(field);
    if (valueBeforeStore && walker.canRecordUndoableStore())
        walker.recordUndoableFieldStore(name, layout, field, valueBeforeStore);
    Reference built = walker.build(WTF::move(store));
    walker.takePendingUndoableStore();
    if (built && !walker.isUnreachable())
        noteKnown(walker, KnownNote::ValueOfFieldOfHeir, { }, name, field, heapsUnderValueOfFieldInHeirsOfBuiltins(walker.program(), layout, field), stored.bits());
    return built;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
