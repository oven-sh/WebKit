/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTEffectsDeclared.h"
#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTPlan.h"
#include "AOTSweep.h"
#include "AOTTypeArena.h"
#include "AOTWalkKnown.h"

namespace JSC { namespace AOT { namespace Typed {

static constexpr AbstractHeapSet heapsUnderStructureInHeirsOfBuiltins { AbstractHeap::NamedProperties, AbstractHeap::PrototypeLinks };

static constexpr AbstractHeapSet subclassElementHeaps = heapsUnderStructureInHeirsOfBuiltins | AbstractHeapSet { AbstractHeap::ArrayStorage, AbstractHeap::ShapeWords };

LayoutID findLayoutKnownByStructure(const Walker& walker, TypeID type)
{
    const Type& record = walker.types().recordOf(type);
    if (record.primitives || !holdsOneMember(record.layouts) || !record.isJavaScriptValue() || !record.hasOnlyLayouts() || record.traitsOfNamed.contains(ObjectTrait::MayHaveLayoutBelow))
        return { };
    LayoutID layout(positionOf(record.layouts));
    const Program& program = walker.program();
    const Layout& heir = program.at(layout);
    BuiltinClassID above = builtinClassOfInstances(heir);
    if (!above || !walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::Folding) || !walker.state().remembersPlaces())
        return { };
    bool isArray = above == BuiltinClasses::Array;
    bool isError = above == BuiltinClasses::Error;
    bool isView = isTypedView(program.builtins().typedArrayTypeOf(above));
    bool structureTellsEveryOwnName = above == BuiltinClasses::Map || above == BuiltinClasses::Set || above == BuiltinClasses::WeakMap || above == BuiltinClasses::WeakSet || above == BuiltinClasses::Promise
        || above == BuiltinClasses::Date || above == BuiltinClasses::DataView;
    if (!isKnownByStructureOfClass(heir) || (!structureTellsEveryOwnName && !isArray && !isError && !isView))
        return { };
    auto mayLieOutsideStructure = [&](PropertyKeyID key) {
        if (!key)
            return true;
        if (isArray)
            return key == program.keyOf(BuiltinNames::length) || program.isIndexLike(key);
        if (isView)
            return program.isIndexLike(key);
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

Reference buildLookAtStructureOfClass(Walker& walker, Reference value, LayoutID layout, Origin origin, bool asksElements)
{
    origin.isTailCall = false;
    Draft look(Opcode::HasStructureOfClass, origin);
    look.setValueOperand(value);
    look.setLayout(layout);
    look.setAsksElements(asksElements);
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
    const Operation& asked = walker.code().at(look.id());
    if (asked.opcode == Opcode::HasStructureOfClass && asked.asksElements())
        noteKnown(walker, KnownNote::ElementsOfSubclassAreSeen, name, { }, 0, LocationMasks(subclassElementHeaps), 1);
    return name;
}

Reference nameBehindLookAtStructureOfClass(const Walker& walker, Reference object, LayoutID layout)
{
    if (!object || !object.isValue())
        return { };
    std::optional<uint64_t> noted = findKnown(walker, KnownNote::StructureOfClassIsSeen, { }, object, layout.index());
    return noted ? Reference::fromBits(static_cast<uint32_t>(*noted)) : Reference();
}

LayoutID arraySubclassWithElements(const Walker& walker, TypeID type)
{
    TypeID present = type ? walker.types().withoutNullish(type) : TypeID();
    LayoutID layout = present ? layoutKnownByStructureOfClass(walker, present) : LayoutID();
    return layout && walker.program().at(layout).writtenBuiltinBase == BuiltinClasses::Array && isArrayPrototypeChainSane(walker) ? layout : LayoutID();
}

LayoutID viewSubclassWithLength(const Walker& walker, TypeID type)
{
    TypeID present = builtinClassBelowType(walker, type) ? walker.types().withoutNullish(type) : TypeID();
    LayoutID layout = present ? layoutKnownByStructureOfClass(walker, present) : LayoutID();
    BuiltinClassID above = layout ? walker.program().at(layout).writtenBuiltinBase : BuiltinClassID();
    PropertyKeyID key = walker.program().keyOf(BuiltinNames::length);
    if (!above || !isTypedView(walker.program().builtins().typedArrayTypeOf(above)) || walker.program().at(layout).findField(key) || walker.facts().hasMemberUnder(layout, ClassSide::Prototype, key))
        return { };
    return findMemberAboveHeirsOfBuiltinClass(walker, above, key) ? layout : LayoutID();
}

Draft draftSubclassElementsTest(Reference array, LayoutID subclass, Origin origin)
{
    origin.isTailCall = false;
    Draft test(Opcode::HasStructureOfClass, origin);
    test.setValueOperand(array);
    test.setLayout(subclass);
    test.setAsksElements(true);
    return test;
}

LayoutID subclassOfArrayName(const Walker& walker, Reference array)
{
    std::optional<uint64_t> noted = walker.program().someClassExtendsArray && array && array.isValue() ? findKnown(walker, KnownNote::SubclassOfArrayName, array) : std::nullopt;
    return noted ? LayoutID(static_cast<uint32_t>(*noted)) : LayoutID();
}

Reference arrayNameOfInstance(Walker& walker, Reference instance, Origin origin)
{
    if (!instance || !instance.isValue() || walker.isUnreachable())
        return { };
    TypeArena& types = walker.types();
    TypeID array = types.builtinClass(BuiltinClasses::Array);
    auto isNamedAsInstance = [&](ValueID name) {
        TypeID type = walker.code().at(name).type;
        return !types.overlap(type, array) || layoutKnownByStructureOfClass(walker, type);
    };
    ValueID named = instance.id();
    while (walker.code().at(named).opcode == Opcode::Narrowed && isNamedAsInstance(named))
        named = walker.code().at(named).valueOperand();
    if (walker.code().isConstant(named) || isNamedAsInstance(named))
        return { };
    Reference object = Reference::value(named);
    if (std::optional<uint64_t> noted = findKnown(walker, KnownNote::ArrayNameOfInstance, object))
        return Reference::fromBits(static_cast<uint32_t>(*noted));

    origin.isTailCall = false;
    bool isInvariant = walker.isInvariantInSpecializedVersion(object);
    Reference isArray;
    {
        Walker::SpecializedPreheaderScope preheaderScope(walker, isInvariant);
        Draft test(Opcode::TypeTest, origin);
        test.setValueOperand(object);
        test.setTestedType(array);
        isArray = walker.build(WTF::move(test));
    }
    if (!isArray || !isArray.isValue() || walker.isUnreachable())
        return { };
    bool isInPreheader = isInvariant && walker.tryBranchInPreheader(isArray, true);
    if (!isInPreheader && !walker.findPassedTest(isArray)) {
        BlockID failed = walker.addBlock(BlockFrequency::Rare);
        walker.continueIfCheckPasses(isArray, failed);
        BlockID passed = walker.currentBlock();
        walker.continueIn(failed);
        if (!walker.isUnreachable())
            walker.stopWhereWordOfLinkIsFalse(origin);
        walker.continueIn(passed);
        if (walker.isUnreachable())
            return { };
    }
    Walker::SpecializedPreheaderScope preheaderScope(walker, isInPreheader);
    Draft proof(Opcode::Narrowed, origin);
    proof.setValueOperand(object);
    proof.setTestOperand(isArray);
    proof.setOperandIndex(OperandIndex::TypeTest::value);
    proof.setIsOnTakenEdge(true);
    Reference name = walker.build(WTF::move(proof));
    if (name && name.isValue() && !walker.isUnreachable()) {
        noteKnown(walker, KnownNote::ArrayNameOfInstance, object, { }, 0, LocationMasks(), name.bits());
        if (LayoutID subclass = arraySubclassWithElements(walker, walker.typeOf(instance)))
            noteKnown(walker, KnownNote::SubclassOfArrayName, name, { }, 0, LocationMasks(), subclass.index());
    }
    return name;
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

bool isLengthOfTypedArraySubclass(const Walker& walker, const BodyValue& read, LayoutID subclass)
{
    if (read.opcode() != Opcode::PropGet || read.isFreeName() || read.key() != walker.program().keyOf(BuiltinNames::length))
        return false;
    BuiltinClassID above = walker.program().at(subclass).writtenBuiltinBase;
    return above && isTypedView(walker.program().builtins().typedArrayTypeOf(above));
}

void buildTypedArraySubclassLength(Walker& walker, const BodyValue& read, Reference view, LayoutID subclass)
{
    Origin origin = walker.originOf(read);
    Draft length(Opcode::GetTypedArrayLength, origin);
    walker.chooseConservativeFields(length);
    length.setViewOperand(view);
    length.setTypedArrayType(walker.program().builtins().typedArrayTypeOf(walker.program().at(subclass).writtenBuiltinBase));
    length.inputValueGiven = read.id();
    Reference built = walker.build(WTF::move(length));
    walker.bind(read.id(), built);
    if (built && built.isValue() && !walker.isUnreachable())
        noteKnown(walker, KnownNote::LengthOfList, view, { }, 0, LocationMasks(AbstractHeapSet { AbstractHeap::TypedArrayStorage }), built.bits());
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
