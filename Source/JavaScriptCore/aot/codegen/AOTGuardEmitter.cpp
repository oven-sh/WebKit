/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTGuardEmitter.h"

#if ENABLE(AOT)

#include "AOTBuiltinMembers.h"
#include "AOTBuiltinPristine.h"
#include "AOTCellFormat.h"
#include "AOTClassGuards.h"
#include "AOTClosureTestEmitter.h"
#include "AOTCompileOptions.h"
#include "AOTCompiledStructure.h"
#include "AOTConstantEmitter.h"
#include "AOTFunctionData.h"
#include "AOTMemberLookup.h"
#include "AOTShapeTestEmitter.h"
#include "AOTSlotAccessEmitter.h"
#include "AOTSlotEmitter.h"
#include "AOTSort.h"
#include "AOTStaticsEmitter.h"
#include "AOTTypeArena.h"
#include "AOTTypeTestEmitter.h"
#include "JSGlobalObject.h"
#include "MarkedBlock.h"
#include "Structure.h"
#include "Watchpoint.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static_assert(sizeof(OptionSet<CompiledStructure::Flag>) == sizeof(uint8_t));
static_assert(sizeof(WatchpointState) == sizeof(uint8_t));
static_assert(offsetOfFirstField <= MarkedBlock::atomSize);
static_assert(!((orderWordIntegrityBits | orderWordHasExtraProperties | orderWordHasFieldOutsideSlot) & ~(0xffu << 24)));

static constexpr uint8_t maskOfClassGuardInItsByte = maskOfClassGuardBit(0);

struct GuardOfClassAskedByTest {
    bool isOfSameSideAs(const GuardOfClassAskedByTest& other) const { return classLayout == other.classLayout && side == other.side; }

    LayoutID classLayout;
    ClassSide side;
    PropertyKeyID name;
    bool isAskedByStoresAlone { false };

    friend bool operator==(const GuardOfClassAskedByTest&, const GuardOfClassAskedByTest&) = default;
    friend auto operator<=>(const GuardOfClassAskedByTest&, const GuardOfClassAskedByTest&) = default;
};

void GuardEmitter::failAndDefineFalse(const ValueContext& value, InternalErrorKind kind)
{
    fail(value, kind);
    defineTest(value, boolean(false));
}

Condition GuardEmitter::isPresentCell(Boxed value, const Type& type)
{
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
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::Cell32:
    case Representation::RawWord:
        break;
    }
    fail(context, InternalErrorKind::OpcodeWithoutCode);
    return emitter<ConstantEmitter>().zeroOf(Representation::Cell).as<CellPointer>();
}

CellPointer GuardEmitter::cellOrElse(const OperationContext& context, ValueID id, BlockEdge isNotCell)
{
    return cellOrElse(context, id, typeOf(id), isNotCell);
}

CellPointer GuardEmitter::cellOrElse(const OperationContext& context, ValueID id, const Type& type, BlockEdge isNotCell)
{
    if (representationOf(id) == Representation::JSValue && (type.primitives & ~cellPrimitives)) {
        LBasicBlock isCellBlock = newBlock();
        m_out.branch(isPresentCell(in<Boxed>(id), type).value(), normalEdge(isCellBlock), isNotCell);
        m_out.appendToContinuation(isCellBlock);
    }
    return untestedCell(context, id);
}

void GuardEmitter::generateShapeTest(const ValueContext& value)
{
    ValueID object = value->objectOperand();
    const Type& known = value->asksInFull() ? types().at(TypeTable::anythingOrAbsent) : typeOf(object);
    bool asksForInstance = !hasOneInstance() && !holdsOnlyCompiledObjects(types(), m_plan, known);
    MarkedObjects marked = value->refusesMarkedObjects() ? MarkedObjects::AreRefused : MarkedObjects::Pass;
    defineTestByBranching(value, [&](BranchTargets targets) {
        BranchTargets targetsOfShape = targets;
        LBasicBlock hasShape = nullptr;
        if (asksForInstance) {
            hasShape = newBlock();
            targetsOfShape = { normalEdge(hasShape), targets.notTaken };
        }
        CellPointer cell = cellOrElse(value, object, known, targets.notTaken);
        emitter<ShapeTestEmitter>().branchOnShapeIn(cell, value->layouts(), known, targetsOfShape, marked);
        if (!asksForInstance)
            return;
        m_out.appendTo(hasShape);
        branch(belongsToInstance(cell), targets);
    });
}

void GuardEmitter::generateIsSlotFilled(const ValueContext& value)
{
    if (value->level() == KnowledgeLevel::Expected) {
        failAndDefineFalse(value, InternalErrorKind::OpcodeWithoutCode);
        return;
    }

    CellPointer object = in<CellPointer>(value->objectOperand());
    FieldAccess access = fieldAccessFor(value->key(), value->receivers(), false);
    if (access.coverage != FieldAccess::Coverage::NoMember && !access.hasLocation) {
        failAndDefineFalse(value, InternalErrorKind::SlotAccessNotPlaced);
        return;
    }

    auto isFilled = [&] {
        SlotEmitter& slots = emitter<SlotEmitter>();
        MemoryLocation location = MemoryLocation::objectField(value->key());
        CellPointer base = emitter<SlotAccessEmitter>().baseOfSlot(object, access, value->receivers());
        if (value->holdsDeclaredType())
            return slots.holdsDeclaredType(base, access.slot, location, &access.extensionWord);
        return slots.isFilled(base, access.slot, location, &access.extensionWord);
    };

    switch (access.coverage) {
    case FieldAccess::Coverage::NoMember:
        defineTest(value, boolean(false));
        return;
    case FieldAccess::Coverage::SomeMembers:
        defineTestByBranching(value, [&](BranchTargets targets) {
            LBasicBlock hasField = newBlock();
            LayoutSetID membersWithField = types().layoutSets().ofSorted(access.membersWithField.span());
            emitter<ShapeTestEmitter>().branchOnShapeAmongReceivers(value, object, membersWithField, { normalEdge(hasField), targets.notTaken });
            m_out.appendTo(hasField);
            branch(isFilled(), targets);
        });
        return;
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
    if (!shapes.mayBeOtherThanCompiledObject(type)) {
        defineTest(value, test());
        return;
    }
    defineTest(value, selectByBranch<Condition>(shapes.isCompiledObject(in<CellPointer>(value->objectOperand())), test, [&] {
        return boolean(false);
    }));
}

void GuardEmitter::generateHasNoExtraProperties(const ValueContext& value)
{
    defineTestOfCompiledObject(value, [&] {
        return orderWordHasNoBits(value, value->hasNoFieldOutsideSlot() ? orderWordHasExtraProperties | orderWordHasFieldOutsideSlot : orderWordHasExtraProperties);
    });
}

Condition GuardEmitter::orderWordHasNoBits(const ValueContext& value, uint32_t bits)
{
    Word32 orderWord = emitter<ShapeTestEmitter>().orderWordOf(in<CellPointer>(value->objectOperand()));
    return hasNoBits(orderWord, bits);
}

void GuardEmitter::generateHasNoFieldOutsideSlot(const ValueContext& value)
{
    defineTestOfCompiledObject(value, [&] {
        Word32 flags = loadByte(in<CellPointer>(value->objectOperand()), m_heaps.JSCell_typeInfoFlags);
        return hasNoBits(flags, TypeInfoPerCellBit);
    });
}

void GuardEmitter::generateHasKeyOrder(const ValueContext& value)
{
    std::optional<uint32_t> expected = m_plan.keyOrders().findOrderWord(value->layout(), value->fields());
    if (!expected) {
        failAndDefineFalse(value, InternalErrorKind::InvalidGuard);
        return;
    }
    defineTestOfCompiledObject(value, [&] {
        Word32 orderWord = emitter<ShapeTestEmitter>().orderWordOf(in<CellPointer>(value->objectOperand()));
        return equal(value->hasNoFlags() ? orderWord : bitAnd(orderWord, orderAndCountMask), static_cast<int32_t>(*expected));
    });
}

void GuardEmitter::generateIsNotFrozen(const ValueContext& value)
{
    defineTestOfCompiledObject(value, [&] {
        uint32_t bits = orderWordIntegrityBits;
        if (value->hasNoExtraProperties())
            bits |= orderWordHasExtraProperties;
        if (value->hasNoFieldOutsideSlot())
            bits |= orderWordHasFieldOutsideSlot;
        return orderWordHasNoBits(value, bits);
    });
}

void GuardEmitter::generateHasLayoutPrototype(const ValueContext& value)
{
    defineTestOfCompiledObject(value, [&] {
        Word32 flags = loadByte(in<CellPointer>(value->objectOperand()), m_heaps.JSCell_typeInfoFlags);
        return hasNoBits(flags, value->hasNoFieldOutsideSlot() ? JSC::IsPristine | TypeInfoPerCellBit : JSC::IsPristine);
    });
}

void GuardEmitter::generateIsPristine(const ValueContext& value)
{
    ValueID subject = value->valueOperand();
    BuiltinClassID builtinClass = value->builtinClass();
    if (m_program.admitsReplacedBuiltins && isClassOfPrimitives(builtinClass) && !typeOf(subject).hasObjects()) {
        defineTest(value, emitter<TypeTestEmitter>().nothingWasWrittenAbove(builtinClass));
        return;
    }
    if (pristineTestOf(m_builtins, builtinClass) == PristineTest::ClosureAsCreated) {
        if (value->aspects().containsAny(aspectsWithoutTest)) {
            defineTest(value, boolean(false));
            return;
        }
        defineTestByBranching(value, [&](BranchTargets targets) {
            branch(isClosureAsCreated(cellOrElse(value, subject, targets.notTaken), builtinClass, value->aspects()), targets);
        });
        return;
    }

    Operand operand = get(subject);
    const TypeTestPlan& instanceTest = typeTestPlanFor(typeIDOf(subject), operand.representation, types().builtinClass(builtinClass));
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
    defineTestByBranching(value, [&](BranchTargets targets) {
        branchOnIsNotThenable(in<Boxed>(operand), type, targets);
    });
}

void GuardEmitter::generateAreMembersUnchanged(const ValueContext& value)
{
    if (value->conditions().isEmpty()) {
        failAndDefineFalse(value, InternalErrorKind::InvalidGuard);
        return;
    }
    defineTest(value, areMembersUnchanged(value->conditions()));
}

Condition GuardEmitter::areMembersUnchanged(PayloadListView<MemberConditionRecord> conditions)
{
    Vector<GuardOfClassAskedByTest, 4> guards;
    for (MemberConditionRecord condition : conditions)
        guards.append(GuardOfClassAskedByTest { condition.layout, condition.side, nameOfGuardAnsweringFor(condition.kind, condition.key), condition.kind == MemberConditionKind::MemberPassedByStores });
    AOT::sort(guards);
    guards.shrink(guards.size() - std::ranges::unique(guards).size());
    RELEASE_ASSERT(!guards.isEmpty());

    auto isAnsweredByGuardOfName = [&](const GuardOfClassAskedByTest& guard) {
        return !guard.name && std::ranges::any_of(guards, [&](const GuardOfClassAskedByTest& other) {
            return other.name && other.isOfSameSideAs(guard);
        });
    };

    struct GuardLeftToAsk {
        LayoutID classLayout;
        ClassSide side;
        ClassGuardKey key;
    };
    Vector<GuardLeftToAsk, 4> guardsLeft;
    StaticsEmitter& statics = emitter<StaticsEmitter>();
    for (std::span<const GuardOfClassAskedByTest> rest = guards.span(); !rest.empty();) {
        const GuardOfClassAskedByTest& first = rest.front();
        size_t numberOfGuardsOfSide = std::ranges::find_if_not(rest, [&](const GuardOfClassAskedByTest& guard) {
            return guard.isOfSameSideAs(first);
        }) - rest.begin();
        std::span<const GuardOfClassAskedByTest> guardsOfSide = rest.first(numberOfGuardsOfSide);
        rest = rest.subspan(numberOfGuardsOfSide);

        if (!first.classLayout || !m_plan.hasClassRecord(first.classLayout))
            return boolean(false);

        auto namesKeyOfProgram = [](const GuardOfClassAskedByTest& guard) {
            return guard.name && !liesInThread(guard.name) && !guard.isAskedByStoresAlone;
        };
        Vector<PropertyKeyID, 4> namesWithOneGuard;
        for (const GuardOfClassAskedByTest& guard : guardsOfSide) {
            if (namesKeyOfProgram(guard))
                namesWithOneGuard.append(guard.name);
        }
        bool namesShareGuard = namesWithOneGuard.size() > 1;
        if (namesShareGuard)
            guardsLeft.append({ first.classLayout, first.side, statics.keyOfGuardOfNames(first.classLayout, first.side, namesWithOneGuard.span()) });
        for (const GuardOfClassAskedByTest& guard : guardsOfSide) {
            if (isAnsweredByGuardOfName(guard) || (namesShareGuard && namesKeyOfProgram(guard)))
                continue;
            guardsLeft.append({ guard.classLayout, guard.side, guard.name ? statics.keyOfGuard(guard.classLayout, guard.side, guard.name, guard.isAskedByStoresAlone) : statics.keyOfGuard(guard.classLayout, guard.side) });
        }
    }
    RELEASE_ASSERT(!guardsLeft.isEmpty());

    auto eachHoldsByItsByte = [&] {
        Condition allHold;
        for (const GuardLeftToAsk& guard : guardsLeft) {
            Condition guardHolds = guardInByteHolds(guard.classLayout, guard.side, statics.guardByte(guard.key));
            allHold = allHold ? logicalAnd(allHold, guardHolds) : guardHolds;
        }
        return allHold;
    };
    if (guardsLeft.size() == 1)
        return eachHoldsByItsByte();

    Vector<ClassGuardKey, 4> keys(guardsLeft.size(), [&](size_t index) {
        return guardsLeft[index].key;
    });
    return isZero(Word32(m_out.load32(statics.countOfGuards(keys.span()))));
}

void GuardEmitter::defineWhetherDeclaredMemberIsFound(const ValueContext& value)
{
    ValueID object = value->objectOperand();
    LayoutSetID receivers = value->receivers();
    bool asksForInstance = !hasOneInstance() && !holdsOnlyCompiledObjects(types(), m_plan, typeOf(object));
    defineTestByBranching(value, [&](BranchTargets targets) {
        CellPointer cell = cellOrElse(value, object, targets.notTaken);
        auto goOnWhere = [&](Condition holds) {
            LBasicBlock next = newBlock();
            branch(holds, { normalEdge(next), targets.notTaken });
            m_out.appendTo(next);
        };
        LBasicBlock hasShape = newBlock();
        emitter<ShapeTestEmitter>().branchOnShapeIn(cell, receivers, typeOf(object), { normalEdge(hasShape), targets.notTaken });
        m_out.appendTo(hasShape);
        if (asksForInstance)
            goOnWhere(belongsToInstance(cell));
        goOnWhere(hasNoBits(loadByte(cell, m_heaps.JSCell_typeInfoFlags), JSC::IsPristine));
        Vector<LayoutID, 4> classes;
        for (LayoutID member : membersOf(types(), receivers)) {
            LayoutID classLayout = declaredLayoutOf(m_program, member);
            if (m_plan.isInUse(member) && !classes.contains(classLayout))
                classes.append(classLayout);
        }
        for (size_t index = 0; index + 1 < classes.size(); ++index)
            goOnWhere(holds(classes[index], ClassSide::Prototype, value->key()));
        branch(classes.isEmpty() ? boolean(false) : holds(classes.last(), ClassSide::Prototype, value->key()), targets);
    });
}

void GuardEmitter::branchOnOwnPropertyByCell(const ValueContext& value, BranchTargets targets, LBasicBlock asksRunTime)
{
    ValueID object = value->objectOperand();
    LayoutSetID receivers = value->receivers();
    FieldAccess access = fieldAccessFor(value->key(), receivers, false);
    bool isFieldOfAll = access.coverage == FieldAccess::Coverage::EveryMember && access.hasLocation;
    if (access.coverage != FieldAccess::Coverage::NoMember && !isFieldOfAll) {
        m_out.jump(asksRunTime);
        return;
    }
    CellPointer cell = cellOrElse(value, object, rareEdge(asksRunTime));
    auto goOnWhere = [&](Condition holds) {
        LBasicBlock next = newBlock();
        branch(holds, { normalEdge(next), rareEdge(asksRunTime) });
        m_out.appendTo(next);
    };
    LBasicBlock hasShape = newBlock();
    emitter<ShapeTestEmitter>().branchOnShapeIn(cell, receivers, typeOf(object), { normalEdge(hasShape), rareEdge(asksRunTime) });
    m_out.appendTo(hasShape);
    Word32 orderWord = emitter<ShapeTestEmitter>().orderWordOf(cell);
    if (!isFieldOfAll) {
        goOnWhere(hasNoBits(orderWord, orderWordHasExtraProperties));
        m_out.jump(targets.notTaken);
        return;
    }
    goOnWhere(hasNoBits(orderWord, orderWordHasFieldOutsideSlot));
    CellPointer base = emitter<SlotAccessEmitter>().baseOfSlot(cell, access);
    branch(emitter<SlotEmitter>().isFilled(base, access.slot, MemoryLocation::objectField(value->key()), &access.extensionWord), targets);
}

void GuardEmitter::generateIsClosureOf(const ValueContext& value)
{
    FunctionID function = value->function();
    const Type& tested = typeOf(value->valueOperand());
    bool everyCellIsClosureOfFunction = singleFunctionOf(types(), tested) == function && !(tested.primitives & ~Primitive::UndefinedOrNull);
    defineTestByBranching(value, [&](BranchTargets targets) {
        CellPointer cell = cellOrElse(value, value->valueOperand(), targets.notTaken);
        if (everyCellIsClosureOfFunction) {
            m_out.jump(targets.taken);
            return;
        }
        m_out.clearDeclaredAccesses();
        emitter<ClosureTestEmitter>().branchOnIsClosureOf(cell, function, targets);
    });
}

Condition GuardEmitter::holds(LayoutID classLayout, ClassSide side, PropertyKeyID name, bool isAskedByStoresAlone)
{
    if (!classLayout || !m_plan.hasClassRecord(classLayout))
        return boolean(false);
    StaticsEmitter& statics = emitter<StaticsEmitter>();
    return guardInByteHolds(classLayout, side, statics.guardByte(name ? statics.keyOfGuard(classLayout, side, name, isAskedByStoresAlone) : statics.keyOfGuard(classLayout, side)));
}

Condition GuardEmitter::guardInByteHolds(LayoutID classLayout, ClassSide side, TypedPointer byteOfGuard)
{
    Word32 byte(m_out.load8ZeroExt32(byteOfGuard));
    return m_plan.bitsOfClassGuardsAreClearWhileTheyHold(classLayout, side) ? hasNoBits(byte, maskOfClassGuardInItsByte) : hasBits(byte, maskOfClassGuardInItsByte);
}

Condition GuardEmitter::isClosureAsCreated(CellPointer cell, BuiltinClassID builtinClass, OptionSet<PristineAspects> aspects)
{
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
    for (unsigned index = 0; index < ClosureWords::numberOfStructuresWithPrototypeProperty; ++index)
        isAsCreated = logicalOr(isAsCreated, equal(structureID, loadFilledOnce32(AOT_INSTANCE_OFFSET(closures.structuresWithPrototypeProperty) + index * sizeof(uint32_t))));
    return isAsCreated;
}

Condition GuardEmitter::isNotThenable(const OperationContext&, Boxed value, const Type& type)
{
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
        m_out.appendToContinuation(isCellBlock);
    }
    CellPointer cell = unboxCell(value);
    if (type.primitives & cellPrimitives) {
        LBasicBlock isObjectBlock = newBlock();
        branch(isObjectCell(cell), { normalEdge(isObjectBlock), targets.taken });
        m_out.appendToContinuation(isObjectBlock);
    }

    Pointer structure = loadStructure(cell);
    Word32 thenableState = bitAnd(loadField<Word32>(structure, m_heaps.Structure_bitField), Structure::s_definitelyNonThenableStateBits);
    Condition isCachedAsNonThenable = equal(thenableState, word32(static_cast<int32_t>(Structure::DefinitelyNonThenableState::NonThenable) << Structure::s_definitelyNonThenableStateShift));

    LBasicBlock cachedStateApplies = newBlock();
    branch(logicalAnd(isCachedAsNonThenable, hasInstanceRealm(structure)), { normalEdge(cachedStateApplies), targets.notTaken });

    m_out.appendToContinuation(cachedStateApplies);
    Pointer realm = loadImmutablePointer(AOT_INSTANCE_OFFSET(globalObject));
    branchOnIsWatched(m_out.address(realm.value(), m_heaps.InlineWatchpointSet_data, OBJECT_OFFSETOF(JSGlobalObject, m_promiseThenWatchpointSet)), targets);
}

Condition GuardEmitter::realmWatchesSpeciesOfLists()
{
    return asBoolean([&](BranchTargets targets) {
        Pointer realm = loadImmutablePointer(AOT_INSTANCE_OFFSET(globalObject));
        branchOnIsWatched(m_out.address(realm.value(), m_heaps.InlineWatchpointSet_data, OBJECT_OFFSETOF(JSGlobalObject, m_arraySpeciesWatchpointSet)), targets);
    });
}

void GuardEmitter::branchOnIsWatched(TypedPointer inlineWatchpointSet, BranchTargets targets)
{
    LBasicBlock isNotThinWatched = newBlock();
    LBasicBlock isInflated = newBlock();
    Pointer data(m_out.loadPtr(inlineWatchpointSet));
    branch(equal(data, pointerConstant(InlineWatchpointSet::encodeState(IsWatched))), { targets.taken, rareEdge(isNotThinWatched) });

    m_out.appendToContinuation(isNotThinWatched);
    branch(hasNoBits(data, InlineWatchpointSet::IsThinFlag), { normalEdge(isInflated), targets.notTaken });

    m_out.appendToContinuation(isInflated);
    branch(equal(loadByte(data, m_heaps.WatchpointSet_state), IsWatched), targets);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
