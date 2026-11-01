/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTGuardEmitter.h"

#if ENABLE(AOT)

#include "AOTBuiltinPristine.h"
#include "AOTCellFormat.h"
#include "AOTClosureTestEmitter.h"
#include "AOTCompiledStructure.h"
#include "AOTConstantEmitter.h"
#include "AOTEffectsOfProtocols.h"
#include "AOTDominators.h"
#include "AOTForInEmitter.h"
#include "AOTFunctionAnalyses.h"
#include "AOTFunctionData.h"
#include "AOTGuardAvailability.h"
#include "AOTPlacement.h"
#include "AOTShapeTestEmitter.h"
#include "AOTSlotAccessEmitter.h"
#include "AOTSlotEmitter.h"
#include "AOTSort.h"
#include "AOTStaticsEmitter.h"
#include "AOTTypeArena.h"
#include "AOTTypeTestEmitter.h"
#include "AOTVector.h"
#include "JSGlobalObject.h"
#include "MarkedBlock.h"
#include "Structure.h"
#include "Watchpoint.h"

namespace JSC { namespace AOT { namespace Typed {

static_assert(sizeof(OptionSet<CompiledStructure::Flag>) == sizeof(uint8_t));
static_assert(sizeof(WatchpointState) == sizeof(uint8_t));
static_assert(offsetOfFirstField <= MarkedBlock::atomSize);

Condition GuardEmitter::isPresentCell(Boxed value, const Type& type)
{
    // The empty value has no tag bits, so it passes isCell.
    if (type.primitives & Primitive::Absent) {
        Condition hasNoTagBits = isCell(value);
        return logicalAnd(hasNoTagBits, isNotEmpty(value));
    }
    return isCell(value);
}

CellPointer GuardEmitter::untestedCell(const OperationContext& context, ValueID id)
{
    switch (representationOf(id)) {
    case Representation::Cell:
        return in<CellPointer>(id);
    case Representation::JSValue:
        return unboxCell(in<Boxed>(id));
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::DoubleWithPayload:
    case Representation::ImpureDouble:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::Cell32:
    case Representation::LiteralString:
    case Representation::RawWord:
        break;
    }
    fail(context, InternalErrorKind::OpcodeWithoutCode);
    return emitter<ConstantEmitter>().zeroOf(Representation::Cell).as<CellPointer>();
}

CellPointer GuardEmitter::cellOrElse(const OperationContext& context, ValueID id, B3::FrequentedBlock isNotCell)
{
    const Type& type = typeOf(id);
    if (representationOf(id) == Representation::JSValue && (type.primitives & ~cellPrimitives)) {
        LBasicBlock isCellBlock = newBlock();
        m_out.branch(isPresentCell(in<Boxed>(id), type).value(), normalEdge(isCellBlock), isNotCell);
        m_out.appendTo(isCellBlock);
    }
    return untestedCell(context, id);
}

void GuardEmitter::generateShapeTest(const ValueContext& value)
{
    ValueID object = value->objectOperand();
    defineTestByBranching(value, [&](BranchTargets targets) {
        CellPointer cell = cellOrElse(value, object, targets.notTaken);
        emitter<ShapeTestEmitter>().branchOnShapeIn(cell, value->layouts(), typeOf(object), targets);
    });
}

// The test decides a branch and nothing else, and only reads rely on what it proves.
bool GuardEmitter::standsInFrontOfReadsAlone(const ValueContext& test)
{
    const UseLists& useLists = analyses().useLists();
    const Terminator& terminator = m_function.at(test.block()).terminator;
    if (test->accessSite() || !terminator.isConditionalBranch() || terminator.conditionOperand() != test.id() || useLists.useCount(test.id()) != 1)
        return false;
    Vector<ValueID, 4> guards;
    for (ValueID parameter : m_function.at(terminator.taken().block).parameters) {
        if (isGuardValue(m_function, parameter))
            guards.append(parameter);
    }
    while (!guards.isEmpty()) {
        for (const Use& use : useLists.usesOf(guards.takeLast())) {
            if (use.kind != Use::Kind::Operand)
                return false;
            switch (m_function.at(use.user).opcode) {
            case Opcode::GetSlot:
                break;
            case Opcode::AllGuards:
                guards.append(use.user);
                break;
            default:
                return false;
            }
        }
    }
    return true;
}

void GuardEmitter::generateIsSlotFilled(const ValueContext& value)
{
    CellPointer object = in<CellPointer>(value->objectOperand());
    const FieldAccess& access = m_placement.fieldAccess(value->fieldAccess());
    auto isFilled = [&] {
        SlotEmitter& slots = emitter<SlotEmitter>();
        MemoryLocation location = MemoryLocation::objectField(access.key, access.receivers);
        CellPointer base = emitter<SlotAccessEmitter>().baseOfSlot(object, access);
        if (value->holdsDeclaredType())
            return slots.holdsDeclaredType(base, access.slot, location, &access.extensionWord);
        // A read asks for a value in the slot. A field that is listed and has no storage goes the way of an empty slot, which ends at the same undefined.
        if (!value->countsFieldsWithoutStorage() && standsInFrontOfReadsAlone(value))
            return slots.holdsValue(base, access.slot, location, &access.extensionWord);
        return slots.isFilled(base, access.slot, location, &access.extensionWord);
    };

    switch (access.coverage) {
    case FieldAccess::Coverage::NoMember:
        defineTest(value, boolean(false));
        return;
    case FieldAccess::Coverage::SomeMembers:
        defineTestByBranching(value, [&](BranchTargets targets) {
            LBasicBlock hasField = newBlock();
            emitter<ShapeTestEmitter>().branchOnShapeIn(object, access.membersWithField, Type::forLayoutSet(access.receivers), { normalEdge(hasField), targets.notTaken });
            m_out.appendTo(hasField);
            branch(isFilled(), targets);
        });
        return;
    case FieldAccess::Coverage::EveryMemberHasSlot:
    case FieldAccess::Coverage::EveryMember:
        defineTest(value, isFilled());
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void GuardEmitter::defineTestOfCompiledObject(const ValueContext& value, NOESCAPE const Invocable<Condition()> auto& test)
{
    ShapeTestEmitter& shapes = emitter<ShapeTestEmitter>();
    const Type& type = typeOf(value->objectOperand());
    if (!shapes.mayBeCellWithBrand(type)) {
        CellPointer cell = in<CellPointer>(value->objectOperand());
        checkClaim(cell, [&] {
            return shapes.isCompiledObject(cell);
        });
        defineTest(value, test());
        return;
    }
    if (shapes.hasOnlyCellsWithBrands(type)) {
        defineTest(value, boolean(false));
        return;
    }
    defineTest(value, selectByBranch<Condition>(shapes.isCompiledObject(in<CellPointer>(value->objectOperand())), test, [&] {
        return boolean(false);
    }));
}

void GuardEmitter::generateHasNoExtraProperties(const ValueContext& value)
{
    ShapeTestEmitter& shapes = emitter<ShapeTestEmitter>();
    if (const Type& type = typeOf(value->objectOperand()); shapes.hasOnlyCellsWithBrands(type)) {
        CellPointer cell = in<CellPointer>(value->objectOperand());
        defineTest(value, shapes.hasOnlyCellsCreatedWithoutProperties(type) ? shapes.hasNoPropertyOfItsOwn(cell) : shapes.hasOnlyClassFieldNames(cell));
        return;
    }
    defineTestOfCompiledObject(value, [&] {
        return shapes.hasNoExtraProperties(in<CellPointer>(value->objectOperand()));
    });
}

// The bits of the flags byte of a compiled object that pass the test when they are clear. None: the test looks elsewhere.
uint8_t GuardEmitter::flagsTestedBy(const Value& test)
{
    switch (test.opcode) {
    case Opcode::HasBirthFieldsFilled:
        return TypeInfoPerCellBit;
    case Opcode::HasNoFieldOutsideSlot:
        return test.mayMeetUnfinishedObject() || generator().validates() ? 0 : TypeInfoPerCellBit;
    case Opcode::HasLayoutPrototype:
        return JSC::IsPristine;
    default:
        return 0;
    }
}

// Where a failed test goes, past blocks that only jump on.
static const Target& destinationOf(const Function& function, const Target& target)
{
    const Target* current = &target;
    for (unsigned steps = 0; steps < 4; ++steps) {
        const Block& block = function.at(current->block);
        if (!current->arguments.isEmpty() || !current->proofs.isEmpty() || !block.parameters.isEmpty() || !block.values.isEmpty() || block.terminator.kind != TerminatorKind::Jump)
            break;
        current = &block.terminator.targets[0];
    }
    return *current;
}

// A test asks for every bit that those tests of the same object ask for that stand above and below it in the dominator tree and fail to the same place.
void GuardEmitter::planTestsOfFlags()
{
    struct Test {
        ValueID id;
        ValueID object;
        BlockID block;
        const Target* whereItFails;
        uint8_t flags;
    };
    Vector<Test, 16> tests;
    HashSet<ValueID> mayBeUnfinished;
    for (BlockID block : analyses().cfg().reversePostOrder()) {
        for (ValueID id : m_function.at(block).values) {
            const Value& test = m_function.at(id);
            const Terminator& terminator = m_function.at(block).terminator;
            bool decidesBranch = terminator.isConditionalBranch() && terminator.conditionOperand() == id;
            uint8_t flags = decidesBranch ? flagsTestedBy(test) : 0;
            if (flags && emitter<ShapeTestEmitter>().mayBeCellWithBrand(typeOf(test.objectOperand())))
                continue;
            if (flags)
                tests.append(Test { id, analyses().identityOf(test.objectOperand()), block, &destinationOf(m_function, terminator.notTaken()), flags });
            else if (test.opcode == Opcode::HasNoFieldOutsideSlot)
                mayBeUnfinished.add(analyses().identityOf(test.objectOperand()));
        }
    }
    AOT::stableSortInLine(tests, { }, [](const Test& test) {
        return test.object.index();
    });

    Dominators& dominators = analyses().dominators();
    for (size_t first = 0; first < tests.size();) {
        size_t end = first;
        uint8_t all = 0;
        bool differ = false;
        for (; end < tests.size() && tests[end].object == tests[first].object; ++end) {
            differ |= tests[end].flags != tests[first].flags;
            all |= tests[end].flags;
        }
        if (differ && !mayBeUnfinished.contains(tests[first].object)) {
            for (size_t index = first; index < end; ++index) {
                uint8_t flags = tests[index].flags;
                for (size_t other = first; other < end && flags != all; ++other) {
                    if (!(*tests[index].whereItFails == *tests[other].whereItFails))
                        continue;
                    if (dominators.dominates(tests[index].block, tests[other].block) || dominators.dominates(tests[other].block, tests[index].block))
                        flags |= tests[other].flags;
                }
                if (flags != tests[index].flags)
                    m_flagsToTest.add(tests[index].id, flags);
            }
        }
        first = end;
    }
}

Condition GuardEmitter::testOfFlags(const ValueContext& value)
{
    if (!std::exchange(m_didPlanTestsOfFlags, true))
        planTestsOfFlags();
    auto planned = m_flagsToTest.find(value.id());
    Word32 flags = loadByte(in<CellPointer>(value->objectOperand()), m_heaps.JSCell_typeInfoFlags);
    return hasNoBits(flags, planned == m_flagsToTest.end() ? flagsTestedBy(value.value()) : planned->value);
}

Condition GuardEmitter::orderWordHasNoBits(const ValueContext& value, uint32_t bits)
{
    if (std::optional<StateOfNewObject> newObject = emitter<SlotAccessEmitter>().stateOfNewObjectBefore(value, value->objectOperand()))
        return boolean(!(newObject->orderWord & bits));
    Word32 orderWord = emitter<ShapeTestEmitter>().orderWordOf(in<CellPointer>(value->objectOperand()));
    return hasNoBits(orderWord, bits);
}

void GuardEmitter::generateHasNoFieldOutsideSlot(const ValueContext& value)
{
    defineTestOfCompiledObject(value, [&] {
        return flagsTestedBy(value.value()) ? testOfFlags(value) : orderWordHasNoBits(value, orderWordHasFieldOutsideSlot);
    });
}

void GuardEmitter::generateHasBirthFieldsFilled(const ValueContext& value)
{
    defineTestOfCompiledObject(value, [&] {
        return build().recordedOptions().isSabotaged(Sabotage::UnfinishedObjectsPassForFinished) ? boolean(true) : testOfFlags(value);
    });
}

void GuardEmitter::generateIsNotFrozen(const ValueContext& value)
{
    defineTestOfCompiledObject(value, [&] {
        return orderWordHasNoBits(value, orderWordIntegrityBits);
    });
}

void GuardEmitter::generateHasLayoutPrototype(const ValueContext& value)
{
    ShapeTestEmitter& shapes = emitter<ShapeTestEmitter>();
    if (const Type& type = typeOf(value->objectOperand()); shapes.hasOnlyCellsWithBrands(type)) {
        defineTest(value, shapes.hasPrototypeOfItsClass(in<CellPointer>(value->objectOperand()), type));
        return;
    }
    // The state of an object that was handed a prototype is known only behind a test that has passed.
    if (emitter<SlotAccessEmitter>().stateOfNewObjectBefore(value, value->objectOperand()))
        defineTest(value, boolean(true));
    else {
        defineTestOfCompiledObject(value, [&] {
            return testOfFlags(value);
        });
    }
}

// A class without a rank has no word to compare with. What is made below the prototype is linked as it is built.
void GuardEmitter::generateIsPrototypeOfClass(const ValueContext& value)
{
    std::optional<uint32_t> rank = m_placement.rankOfClass(declaredLayoutOf(m_program, value->layout()));
    if (!rank) {
        defineTest(value, boolean(false));
        return;
    }
    Pointer prototypes = loadImmutablePointer(AOT_INSTANCE_OFFSET(classes.classPrototypes));
    ptrdiff_t offset = static_cast<ptrdiff_t>(*rank) * sizeof(void*);
    defineTest(value, Condition(m_out.equal(in<CellPointer>(value->prototypeOperand()).value(), m_out.load64(m_out.address(m_heaps.entire(AbstractHeap::AllocatorState), prototypes.value(), offset)))));
}

void GuardEmitter::generateIsPristine(const ValueContext& value)
{
    ValueID subject = value->valueOperand();
    BuiltinClassID builtinClass = value->builtinClass();
    if (pristineTestOf(m_builtins, builtinClass) == PristineTest::ClosureAsCreated) {
        if (value->aspects().containsAny(aspectsWithoutTest)) {
            defineTest(value, boolean(false));
            return;
        }
        // Only closures have the tested Structures, so the test needs no test of the class in front of it.
        defineTestByBranching(value, [&](BranchTargets targets) {
            branch(isClosureAsCreated(cellOrElse(value, subject, targets.notTaken), builtinClass, value->aspects()), targets);
        });
        return;
    }

    Operand operand = get(subject);
    const TypeTestPlan& instanceTest = typeTestPlanFor(m_function.at(subject).type, operand.representation, types().intern(Type::builtinClass(builtinClass)));
    auto isPristine = [&] {
        return emitter<TypeTestEmitter>().isPristine(untestedCell(value, subject), builtinClass, value->aspects());
    };

    switch (instanceTest.verdict) {
    case TypeTestVerdict::NeverPasses:
        defineTest(value, boolean(false));
        return;
    case TypeTestVerdict::AlwaysPasses:
        defineTest(value, isPristine());
        return;
    case TypeTestVerdict::NeedsTest:
        defineTestByBranching(value, [&](BranchTargets targets) {
            LBasicBlock isInstance = newBlock();
            branchOnType(operand, instanceTest, { normalEdge(isInstance), targets.notTaken });
            m_out.appendTo(isInstance);
            branch(isPristine(), targets);
        });
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void GuardEmitter::generateIsNotThenable(const ValueContext& value)
{
    ValueID operand = value->valueOperand();
    const Type& type = typeOf(operand);
    if (!protocolMayFindMember(m_program, type, Protocol::Thenable)) {
        defineTest(value, boolean(true));
        return;
    }
    defineTestByBranching(value, [&](BranchTargets targets) {
        branchOnIsNotThenable(in<Boxed>(operand), type, targets);
    });
}

void GuardEmitter::generateHasKeyOrder(const ValueContext& value)
{
    std::optional<OrderWord> expected = value->orderWord();
    if (!expected) {
        defineTest(value, boolean(false));
        return;
    }
    defineTestOfCompiledObject(value, [&] {
        Word32 orderWord = emitter<ShapeTestEmitter>().orderWordOf(in<CellPointer>(value->objectOperand()));
        return equal(bitAnd(orderWord, orderAndCountMask), expected->value());
    });
}

void GuardEmitter::generateIsClosureOf(const ValueContext& value)
{
    FunctionID function = value->function();
    const Type& tested = typeOf(value->valueOperand());
    bool everyCellIsClosureOfFunction = tested.singleFunction() == function && !(tested.primitives & ~Primitive::UndefinedOrNull);
    defineTestByBranching(value, [&](BranchTargets targets) {
        CellPointer cell = cellOrElse(value, value->valueOperand(), targets.notTaken);
        if (everyCellIsClosureOfFunction) {
            m_out.jump(targets.taken.block());
            return;
        }
        emitter<ClosureTestEmitter>().branchOnIsClosureOf(cell, function, tested, targets, value.shape());
    });
}

void GuardEmitter::generateIsEnumeratorCurrent(const ValueContext& value)
{
    ForInEmitter& forIn = emitter<ForInEmitter>();
    CellPointer enumerator = in<CellPointer>(value->enumeratorOperand());
    ForInEmitter::EnumeratorFields fields = forIn.loadFields(enumerator);
    Integer nameIndex = forIn.nameIndexOf(fields, integerOf(value->positionOperand()));
    Condition isInStructurePart = forIn.isInStructurePart(fields, nameIndex);
    defineTestByBranching(value, [&](BranchTargets targets) {
        CellPointer object = cellOrElse(value, value->objectOperand(), targets.notTaken);
        branch(logicalAnd(isInStructurePart, forIn.hasCachedKeys(enumerator, fields, object)), targets);
    });
}

Condition GuardEmitter::holds(GuardBitID id)
{
    if (!id)
        return boolean(false);
    StaticsEmitter& statics = emitter<StaticsEmitter>();
    // A guard may begin to hold without a word to anybody: the first allocation of a constructor function's class defines the class, and no operation says that it writes MemberGuards.
    // A read from before says "does not hold", and the code goes the general way. What ENDS a guard says that it writes them: --aot-check=MovedLoads reports a guard that ended unseen.
    TypedPointer address = statics.guardByteOf(id);
    bool holdsWhileClear = m_placement.guardBit(id).holdsWhileClear;
    if (build().options().checksMovedLoads)
        m_records.guardTests.append(GuardTest { valueOfOrigin(m_out.origin()), static_cast<int32_t>(address.offset()), statics.guardMaskOf(id), holdsWhileClear });
    Word32 byte(m_out.load8ZeroExt32(address));
    return holdsWhileClear ? hasNoBits(byte, statics.guardMaskOf(id)) : hasBits(byte, statics.guardMaskOf(id));
}

Condition GuardEmitter::isClosureAsCreated(CellPointer cell, BuiltinClassID builtinClass, OptionSet<PristineAspects> aspects)
{
    // A new property, another prototype and a change of integrity each give a closure another Structure.
    RELEASE_ASSERT(builtinClass == BuiltinClasses::Function || builtinClass == BuiltinClasses::AsyncFunction);
    FunctionKindData kind { };
    kind.isAsync = builtinClass == BuiltinClasses::AsyncFunction;
    Word32 structureID = loadStructureID(cell);
    Condition isFullClosure = equal(structureID, loadImmutable32(AOT_INSTANCE_OFFSET(closures.closureHeaders) + kind.closureStructureOfKind() * sizeof(uint64_t)));
    Condition isSlimClosure = equal(structureID, loadImmutable32(AOT_INSTANCE_OFFSET(slimClosureHeaders) + kind.slimClosureKind() * sizeof(uint64_t)));
    Condition isAsCreated = logicalOr(isFullClosure, isSlimClosure);
    if (kind.isAsync)
        return isAsCreated;
    if (aspects.contains(PristineAspects::NamesKeptBelow)) {
        BuiltinClassID below = classBelowWithTestedStructure(m_builtins, builtinClass);
        RELEASE_ASSERT(below);
        isAsCreated = logicalOr(isAsCreated, isClosureAsCreated(cell, below, PristineAspects::Names));
    }
    // `Base.call(this)`: a constructor function has its `prototype` property, which hides no other name. A word that is read too early is zero, and the test fails.
    for (unsigned index = 0; index < ClosureWords::numberOfStructuresWithPrototypeProperty; ++index)
        isAsCreated = logicalOr(isAsCreated, equal(structureID, loadFilledOnce32(AOT_INSTANCE_OFFSET(closures.structuresWithPrototypeProperty) + index * sizeof(uint32_t))));
    return isAsCreated;
}

Condition GuardEmitter::isNotThenable(const OperationContext&, Boxed value, const Type& type)
{
    if (!protocolMayFindMember(m_program, type, Protocol::Thenable))
        return boolean(true);
    return asBoolean([&](BranchTargets targets) {
        branchOnIsNotThenable(value, type, targets);
    });
}

Condition GuardEmitter::hasInstanceRealm(Pointer structure)
{
    Pointer realm = loadField<Pointer>(structure, m_heaps.Structure_realm);
    return equal(realm, loadImmutablePointer(AOT_INSTANCE_OFFSET(globalObject)));
}

void GuardEmitter::branchOnIsNotThenable(Boxed value, const Type& type, BranchTargets targets)
{
    if (type.primitives & ~cellPrimitives) {
        LBasicBlock isCellBlock = newBlock();
        branch(isPresentCell(value, type), { normalEdge(isCellBlock), targets.taken });
        m_out.appendTo(isCellBlock);
    }
    CellPointer cell = unboxCell(value);
    if (type.primitives & cellPrimitives) {
        LBasicBlock isObjectBlock = newBlock();
        branch(isObjectCell(cell), { normalEdge(isObjectBlock), targets.taken });
        m_out.appendTo(isObjectBlock);
    }

    Pointer structure = loadStructure(cell);
    Word32 thenableState = bitAnd(loadField<Word32>(structure, m_heaps.Structure_bitField), Structure::s_definitelyNonThenableStateBits);
    Condition isCachedAsNonThenable = equal(thenableState, word32(static_cast<int32_t>(Structure::DefinitelyNonThenableState::NonThenable) << Structure::s_definitelyNonThenableStateShift));

    // The Structure of a compiled object gets this state only where it lists the extra properties of its objects, or they have none.
    LBasicBlock cachedStateApplies = newBlock();
    branch(logicalAnd(isCachedAsNonThenable, hasInstanceRealm(structure)), { normalEdge(cachedStateApplies), targets.notTaken });

    m_out.appendTo(cachedStateApplies);
    Pointer realm = loadImmutablePointer(AOT_INSTANCE_OFFSET(globalObject));
    branchOnIsWatched(m_out.address(realm.value(), m_heaps.InlineWatchpointSet_data, OBJECT_OFFSETOF(JSGlobalObject, m_promiseThenWatchpointSet)), targets);
}

void GuardEmitter::branchOnIsWatched(TypedPointer inlineWatchpointSet, BranchTargets targets)
{
    LBasicBlock isNotThinWatched = newBlock();
    LBasicBlock isInflated = newBlock();
    Pointer data(m_out.loadPtr(inlineWatchpointSet));
    branch(equal(data, pointerConstant(InlineWatchpointSet::encodeState(IsWatched))), { targets.taken, rareEdge(isNotThinWatched) });

    m_out.appendTo(isNotThinWatched);
    branch(hasNoBits(data, InlineWatchpointSet::IsThinFlag), { normalEdge(isInflated), targets.notTaken });

    m_out.appendTo(isInflated);
    branch(equal(loadByte(data, m_heaps.WatchpointSet_state), IsWatched), targets);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
