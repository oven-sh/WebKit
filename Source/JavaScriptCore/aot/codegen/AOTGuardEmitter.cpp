/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTGuardEmitter.h"

#if ENABLE(AOT)

#include "AOTAccessEmitter.h"
#include "AOTBuiltinMembers.h"
#include "AOTBuiltinPristine.h"
#include "AOTCellFormat.h"
#include "AOTClassGuards.h"
#include "AOTClosureTestEmitter.h"
#include "AOTCompileOptions.h"
#include "AOTCompiledStructure.h"
#include "AOTConstantEmitter.h"
#include "AOTFieldTable.h"
#include "AOTFunctionData.h"
#include "AOTInliningAcrossFiles.h"
#include "AOTMemberLookup.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTSavedFrameEmitter.h"
#include "AOTShapeTestEmitter.h"
#include "AOTSlotAccessEmitter.h"
#include "AOTSlotEmitter.h"
#include "AOTSort.h"
#include "AOTStaleAnswers.h"
#include "AOTStaticsEmitter.h"
#include "AOTStubsCoroutines.h"
#include "AOTStubsPropertyKeys.h"
#include "AOTTruthinessEmitter.h"
#include "AOTTypeArena.h"
#include "AOTTypeTestEmitter.h"
#include "JSArrayBufferView.h"
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
static_assert(maskOfClassGuardsBegun != maskOfClassGuardInItsByte);

struct GuardOfClassAskedByTest {
    bool isOfSameSideAs(const GuardOfClassAskedByTest& other) const { return classLayout == other.classLayout && side == other.side; }

    LayoutID classLayout;
    ClassSide side;
    PropertyKeyID name;
    AskerOfClassGuardOfName asker { AskerOfClassGuardOfName::Reads };

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

ALWAYS_INLINE_INTO_OTHER_FILES CellPointer GuardEmitter::untestedCell(const OperationContext& context, ValueID id)
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
    if (AOT::hasFlag(build().options().testing, Wire::TestingSwitch::SkipUnrolledShapeChecks) && generator().isLayingOutTurns()) {
        defineTest(value, boolean(true));
        return;
    }
    ValueID object = value->objectOperand();
    std::optional<TruthinessEmitter::DeferredTruthTest> deferred = emitter<TruthinessEmitter>().takeTruthTestOf(value.id());
    const Type& known = value->asksInFull() ? types().at(TypeTable::anythingOrAbsent) : typeOf(deferred ? deferred->source : object);
    bool asksForInstance = !hasOneInstance() && !holdsOnlyCompiledObjects(types(), m_plan, known);
    MarkedObjects marked = value->rejectsMarkedObjects() ? MarkedObjects::AreRefused : MarkedObjects::Pass;
    defineTestByBranching(value, [&](BranchTargets targets) {
        BranchTargets targetsOfShape = targets;
        LBasicBlock hasShape = nullptr;
        if (asksForInstance) {
            hasShape = newBlock();
            targetsOfShape = { normalEdge(hasShape), targets.notTaken };
        }
        CellPointer cell = deferred ? unboxCell(deferred->value) : cellOrElse(value, object, known, targets.notTaken);
        if (deferred)
            emitter<TruthinessEmitter>().branchOnShapeOrTruth(value, *deferred, targetsOfShape, targets.notTaken, marked);
        else
            emitter<ShapeTestEmitter>().branchOnShapeIn(cell, value->layouts(), known, targetsOfShape, marked);
        if (!asksForInstance)
            return;
        m_out.appendTo(hasShape);
        branch(belongsToInstance(cell), targets);
    });
}

bool GuardEmitter::isImpliedByNumberTest(const ValueContext& isSlotFilled, const FieldAccess& access)
{
    if (emittersCheckProofs() || !isSlotFilled.isBranchCondition() || isSlotFilled->holdsDeclaredType() || access.slot.representation != SlotRepresentation::JSValue || access.slot.storage != SlotStorage::Inline || access.findsOffsetByShape)
        return false;
    const Terminator& end = m_code.at(isSlotFilled.block()).terminator;
    if (end.kind != TerminatorKind::Branch || end.conditionOperand() != isSlotFilled.id())
        return false;
    const Edge& passes = m_code.at(end.taken());
    const Edge& fails = m_code.at(end.notTaken());
    const Terminator& endOfNumberTest = m_code.at(passes.to).terminator;
    std::span<const ValueID> operations = m_code.valuesOf(passes.to);
    if (operations.size() != 2 || endOfNumberTest.kind != TerminatorKind::Branch || endOfNumberTest.conditionOperand() != operations[1])
        return false;
    const Operation& read = m_code.at(operations[0]);
    const Operation& test = m_code.at(operations[1]);
    if (read.opcode != Opcode::GetSlot || read.objectOperand() != isSlotFilled->objectOperand() || read.key() != isSlotFilled->key() || read.receivers() != isSlotFilled->receivers() || read.representation != Representation::JSValue || read.holdsDeclaredType())
        return false;
    if (test.opcode != Opcode::TypeTest || test.valueOperand() != operations[0] || (test.testedType() != TypeTable::number && test.testedType() != TypeTable::int32) || !types().at(read.type).hasObjects())
        return false;
    const Edge& failsNumberTest = m_code.at(endOfNumberTest.notTaken());
    return failsNumberTest.to == fails.to && fails.arguments().empty() && failsNumberTest.arguments().empty();
}

void GuardEmitter::generateIsSlotFilled(const ValueContext& value)
{
    if (value->level() == KnowledgeLevel::Expected) {
        failAndDefineFalse(value, InternalErrorKind::OpcodeWithoutCode);
        return;
    }

    CellPointer object = in<CellPointer>(value->objectOperand());
    FieldAccess access = fieldAccessFor(value->key(), value->receivers(), false);
    if (access.findsOffsetByShape)
        access.layoutsThatTestInCodeLetThrough = emitter<SlotAccessEmitter>().layoutsThatTestAgainstEveryShapeLetThrough(value->objectOperand());
    if (access.coverage != FieldAccess::Coverage::NoMember && !access.hasLocation) {
        failAndDefineFalse(value, InternalErrorKind::SlotAccessNotPlaced);
        return;
    }

    auto isFilled = [&] {
        SlotEmitter& slots = emitter<SlotEmitter>();
        MemoryLocation location = MemoryLocation::objectField(value->key());
        CellPointer base = emitter<SlotAccessEmitter>().baseOfSlot(object, access, value->receivers());
        if (value->holdsDeclaredType() && !isListedOnlyByKeyOrder(access.slot.representation))
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
        defineTest(value, isImpliedByNumberTest(value, access) ? boolean(true) : isFilled());
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
        if (value->mayMeetUnfinishedObject())
            return orderWordHasNoBits(value, orderWordHasFieldOutsideSlot);
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

void GuardEmitter::generateAreHeldFieldsFilled(const ValueContext& value)
{
    CellPointer cell = in<CellPointer>(value->objectOperand());
    Vector<FieldIndex, 8> held = fieldsHeldFilledInSlots(m_program, m_facts.linked(), m_plan.slots(), value->layout());
    Vector<BeginningsOfKeyOrder, 2> beginnings = m_plan.keyOrders().beginningsThatList(value->layout(), held.span());
    Word32 orderAndCount = bitAnd(emitter<ShapeTestEmitter>().orderWordOf(cell), word32(orderAndCountMask | orderWordHasFieldOutsideSlot));
    auto isAmong = [&](const BeginningsOfKeyOrder& listed) {
        if (!listed.numberOfLongerOnes)
            return equal(orderAndCount, static_cast<int32_t>(listed.orderWordOfShortest));
        Word32 pastShortest = sub(orderAndCount, word32(listed.orderWordOfShortest));
        Word32 countPastShortest = bitOr(shiftRightUnsigned(pastShortest, orderWordCountShift), shiftLeft(pastShortest, 32 - orderWordCountShift));
        return belowOrEqual(countPastShortest, static_cast<int32_t>(listed.numberOfLongerOnes));
    };
    defineTestByBranching(value, [&](BranchTargets targets) {
        for (const BeginningsOfKeyOrder& listed : beginnings) {
            LBasicBlock listsOtherwise = newBlock();
            branch(isAmong(listed), { targets.taken, &listed == &beginnings.last() ? rareEdge(listsOtherwise) : normalEdge(listsOtherwise) });
            m_out.appendToContinuation(listsOtherwise);
        }
        branch(callWithoutSite<Condition>(RuntimeOperation::AreHeldFieldsFilled, { cell }, { .isOnRarePath = true }), targets);
    });
}

void GuardEmitter::generateIsNotFrozen(const ValueContext& value)
{
    defineTestOfCompiledObject(value, [&] {
        uint32_t bits = value->isExtensible() ? orderWordIntegrityBits : orderWordIsFrozen;
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
            CellPointer cell = cellOrElse(value, subject, targets.notTaken);
            if (FunctionID function = singleFunctionOf(types(), typeOf(subject)); function && conventions().exists(conventions().closureEntryOf(function))) {
                LBasicBlock hasOtherStructure = newBlock();
                branch(emitter<ClosureTestEmitter>().hasClosureStructure(cell, function), { targets.taken, unlikelyEdge(hasOtherStructure) });
                m_out.appendTo(hasOtherStructure);
            }
            branch(isClosureAsCreated(cell, builtinClass, value->aspects()), targets);
        });
        return;
    }

    Operand operand = get(subject);
    const TypeTestPlan& instanceTest = typeTestPlanFor(typeIDOf(subject), operand.representation, types().builtinClass(builtinClass));
    auto isPristine = [&] {
        CellPointer cell = untestedCell(value, subject);
        Condition isAsMade = emitter<TypeTestEmitter>().isPristine(cell, builtinClass, value->aspects());
        if (!value->aspects().contains(PristineAspects::Storage) || (!isTypedView(m_builtins.typedArrayTypeOf(builtinClass)) && !standsAboveClassesOfViews(m_builtins, builtinClass)))
            return isAsMade;
        return logicalAnd(isAsMade, hasNoBits(loadByte(cell, m_heaps.JSArrayBufferView_sharingOfMemory), isGrowableSharedMode));
    };

    if (standsAboveClassesOfViews(m_builtins, builtinClass)) {
        defineTest(value, kindsOfViewsOfNumbersIn(m_program, types(), typeIDOf(subject)) ? isPristine() : boolean(false));
        return;
    }
    bool branchesOnView = value->aspects().contains(PristineAspects::Storage) && isTypedView(m_builtins.typedArrayTypeOf(builtinClass));
    switch (instanceTest.verdict) {
    case TypeTestVerdict::NeverPasses:
        defineTest(value, boolean(false));
        return;
    case TypeTestVerdict::AlwaysPasses:
        if (branchesOnView) {
            defineTestByBranching(value, [&](BranchTargets targets) {
                emitter<TypeTestEmitter>().branchOnPristineView(untestedCell(value, subject), builtinClass, value->aspects(), targets);
            });
            return;
        }
        defineTest(value, isPristine());
        return;
    case TypeTestVerdict::NeedsTest:
        defineTestByBranching(value, [&](BranchTargets targets) {
            LBasicBlock isInstance = newBlock();
            branchOnType(operand, instanceTest, { normalEdge(isInstance), targets.notTaken });
            m_out.appendTo(isInstance);
            if (branchesOnView)
                emitter<TypeTestEmitter>().branchOnPristineView(untestedCell(value, subject), builtinClass, value->aspects(), targets);
            else
                branch(isPristine(), targets);
        });
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void GuardEmitter::generateHasStructureOfClass(const ValueContext& value)
{
    LayoutID classLayout = value->layout();
    if (!m_plan.hasClassRecord(classLayout) || !isKnownByStructureOfClass(m_program.at(classLayout))) {
        failAndDefineFalse(value, InternalErrorKind::InvalidGuard);
        return;
    }
    static_assert(sizeof(StructureID) == sizeof(uint32_t));
    defineTestByBranching(value, [&](BranchTargets end) {
        CellPointer cell = cellOrElse(value, value->valueOperand(), end.notTaken);
        Word32 structureID = loadStructureID(cell);
        auto askRunTime = [&] {
            branch(callWithoutSite<Condition>(RuntimeOperation::RememberStructureOfClass, { cell, word32(classLayout.index()), numberOf(m_mentions.add(NumberSpace::HeirLookedAt, classLayout, value->asksElements() ? StructuresOfHeir::asksElements : 0)) }, { .isOnRarePath = true, .accessesCollectorState = true }), end);
        };
        BranchTargets targets = end;
        std::optional<LBasicBlock> isInstance;
        if (value->asksElements()) {
            isInstance = newBlock();
            targets = { normalEdge(*isInstance), end.notTaken };
        }
        auto goOnUnlessItIsTheOneAt = [&](ptrdiff_t offsetOfWord, BlockEdge whereItIs, BlockEdge (*edgeToRest)(LBasicBlock)) {
            LBasicBlock isAnother = newBlock();
            Word32 remembered(m_out.load32(subclassStructuresWord(m_heaps.collectorState, classLayout, offsetOfWord)));
            branch(equal(structureID, remembered), { whereItIs, edgeToRest(isAnother) });
            m_out.appendTo(isAnother);
        };
        goOnUnlessItIsTheOneAt(StructuresOfHeir::offsetOfOneThatPasses(0), targets.taken, unlikelyEdge);
        goOnUnlessItIsTheOneAt(StructuresOfHeir::offsetOfOneRefused(0), targets.notTaken, normalEdge);
        for (unsigned index = 1; index < StructuresOfHeir::numberThatPass; ++index)
            goOnUnlessItIsTheOneAt(StructuresOfHeir::offsetOfOneThatPasses(index), targets.taken, normalEdge);
        for (unsigned index = 1; index < StructuresOfHeir::numberRefused; ++index)
            goOnUnlessItIsTheOneAt(StructuresOfHeir::offsetOfOneRefused(index), targets.notTaken, index + 1 < StructuresOfHeir::numberRefused ? normalEdge : rareEdge);
        askRunTime();
        if (!isInstance)
            return;
        m_out.appendTo(*isInstance);
        CellPointer prototype = unboxCell(loadField<Boxed>(loadStructure(cell), m_heaps.Structure_prototype));
        Word32 withoutElements(m_out.load32(subclassStructuresWord(m_heaps.collectorState, classLayout, StructuresOfHeir::offsetOfPrototypeWithoutElements())));
        LBasicBlock mayHoldElements = newBlock();
        branch(equal(loadStructureID(prototype), withoutElements), { end.taken, rareEdge(mayHoldElements) });
        m_out.appendTo(mayHoldElements);
        askRunTime();
    });
}

TypedPointer GuardEmitter::subclassStructuresWord(const Machine::AbstractHeap& heap, LayoutID classLayout, ptrdiff_t offset)
{
    return instanceWord(heap, m_mentions.add(NumberSpace::HeirLookedAt, classLayout, static_cast<int32_t>(offset)));
}

void GuardEmitter::generateIsNotThenable(const ValueContext& value)
{
    ValueID operand = value->valueOperand();
    if (!generator().takesOpenForm(value, StateOfKnowledge::Always)) {
        LValue asked = in<Boxed>(operand).value();
        defineTest(value, isNotZero(Integer(emitter<RuntimeCallEmitter>().callStubWithoutSite(AOT::isNotThenable(), singleElementSpan(asked)).first)));
        return;
    }
    const Type& type = typeOf(operand);
    defineTestByBranching(value, [&](BranchTargets targets) {
        branchOnIsNotThenable(in<Boxed>(operand), type, targets);
    });
}

void GuardEmitter::generateBindsNoTrapAnswer(const ValueContext& value)
{
    Word32 bitField = loadField<Word32>(loadStructure(in<CellPointer>(value->targetOperand())), m_heaps.Structure_bitField);
    defineTest(value, hasNoBits(bitField, Structure::s_hasNonConfigurableReadOnlyOrGetterSetterPropertiesBits));
}

void GuardEmitter::generateHasNumberOfBoundArguments(const ValueContext& value)
{
    CellPointer function = in<CellPointer>(value->functionOperand());
    defineTest(value, equal(loadField<Word32>(function, m_heaps.JSBoundFunction_boundArgsLength), word32(value->count())));
}

void GuardEmitter::generateIsUnmodifiedAsyncGenerator(const ValueContext& value)
{
    CellPointer generator = in<CellPointer>(value->generatorOperand());
    defineTestByBranching(value, [&](BranchTargets targets) {
        emitter<SavedFrameEmitter>().branchUnlessUnmodifiedAsyncGenerator(generator, targets.notTaken.block());
        m_out.jump(targets.taken);
    });
}

void GuardEmitter::generateMakesPlainGenerators(const ValueContext& value)
{
    CellPointer function = in<CellPointer>(value->functionOperand());
    defineTestByBranching(value, [&](BranchTargets targets) {
        emitter<SavedFrameEmitter>().branchUnlessGeneratorsPlain(function, targets.notTaken.block());
        m_out.jump(targets.taken);
    });
}

void GuardEmitter::generateGetWordOfBoundFunction(const ValueContext& value)
{
    CellPointer function = in<CellPointer>(value->functionOperand());
    switch (value->index()) {
    case 0:
        define(value, loadField<Boxed>(function, m_heaps.JSBoundFunction_targetFunction));
        return;
    case 1:
        define(value, loadField<Boxed>(function, m_heaps.JSBoundFunction_boundThis));
        return;
    case 2:
        define(value, loadField<Boxed>(function, m_heaps.JSBoundFunction_boundArg0));
        return;
    case 3:
        define(value, loadField<Boxed>(function, m_heaps.JSBoundFunction_boundArg1));
        return;
    case 4:
        define(value, loadField<Boxed>(function, m_heaps.JSBoundFunction_boundArg2));
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void GuardEmitter::generateIsMemberAsBuilt(const ValueContext& value)
{
    BuiltinID member = value->builtin();
    m_mentions.builtinCell(cellThatNamesMemberAskedByItsByte(BuiltinTable::singleton(), member));
    Pointer membersWritten = loadImmutablePointer(AOT_INSTANCE_OFFSET(builtins.membersWritten));
    Word32 wasWritten(m_out.load8ZeroExt32(m_out.address(m_heaps.entire(AbstractHeap::NamedProperties), membersWritten.value(), member.index())));
    if (!AOT::hasFlag(build().options().testing, Wire::TestingSwitch::CheckWalkAtRunTime)) {
        defineTest(value, isZero(wasWritten));
        return;
    }
    defineTestByBranching(value, [&](BranchTargets targets) {
        LBasicBlock saysYes = newBlock();
        branch(isZero(wasWritten), { normalEdge(saysYes), targets.notTaken });
        m_out.appendTo(saysYes);
        int32_t kindAndStub = static_cast<int32_t>(kindAndStubOfAnswer(KindOfAnswer::Value, StubThatAnswers::MemberAsBuilt) | (m_program.admitsReplacedBuiltins ? 0 : answerIsYesOfQuestionOfLibrary));
        callVoidOperation(value, RuntimeOperation::StopAtStaleAnswer, { boxInt32(word32(static_cast<int32_t>(member.index()))), undefined(), word32(0), word32(kindAndStub) });
        m_out.jump(targets.taken);
    });
}

void GuardEmitter::generateIsHolderWithoutAddedNames(const ValueContext& value)
{
    Pointer namesOnHolders = loadImmutablePointer(AOT_INSTANCE_OFFSET(builtins.namesOnHolders));
    Word32 whatIsKnown(m_out.load8ZeroExt32(m_out.address(m_heaps.entire(AbstractHeap::NamedProperties), namesOnHolders.value(), value->builtin().index())));
    Condition isAsBuilt = AOT::hasFlag(build().options().testing, Wire::TestingSwitch::AssumeGuardsAndFacts) ? boolean(true) : isZero(whatIsKnown);
    const BuiltinTable& table = BuiltinTable::singleton();
    if (!AOT::hasFlag(build().options().testing, Wire::TestingSwitch::CheckWalkAtRunTime) || table.canonicalID(value->builtin()) != table.canonicalID(Builtins::ObjectPrototype)) {
        defineTest(value, isAsBuilt);
        return;
    }
    defineTestByBranching(value, [&](BranchTargets targets) {
        LBasicBlock saysYes = newBlock();
        branch(isAsBuilt, { normalEdge(saysYes), targets.notTaken });
        m_out.appendTo(saysYes);
        int32_t kindAndStub = static_cast<int32_t>(kindAndStubOfAnswer(KindOfAnswer::Absent, StubThatAnswers::HolderWithoutAddedNames));
        callVoidOperation(value, RuntimeOperation::StopAtStaleAnswer, { boxInt32(word32(static_cast<int32_t>(value->builtin().index()))), undefined(), word32(0), word32(kindAndStub) });
        m_out.jump(targets.taken);
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

bool GuardEmitter::testsMayAddNamesToObjectPrototype() const
{
    return m_program.admitsReplacedBuiltins && m_program.isSomeNameOpenedOn(m_program.builtins().canonicalID(Builtins::ObjectPrototype));
}

Condition GuardEmitter::noNameWasAddedToObjectPrototype()
{
    Pointer namesOnHolders = loadImmutablePointer(AOT_INSTANCE_OFFSET(builtins.namesOnHolders));
    Word32 whatIsKnown(m_out.load8ZeroExt32(m_out.address(m_heaps.entire(AbstractHeap::NamedProperties), namesOnHolders.value(), m_program.builtins().canonicalID(Builtins::ObjectPrototype).index())));
    return isZero(whatIsKnown);
}

void GuardEmitter::defineWhetherPrototypeChainIsPlain(const ValueContext& value)
{
    defineTestByBranching(value, [&](BranchTargets targets) {
        LBasicBlock next = newBlock();
        branch(noNameWasAddedToObjectPrototype(), { normalEdge(next), targets.notTaken });
        m_out.appendTo(next);
        branch(areMembersUnchanged(value->conditions()), targets);
    });
}

Condition GuardEmitter::areMembersUnchanged(PayloadListView<MemberConditionRecord> conditions)
{
    Vector<MemberConditionRecord, 4> copied;
    for (MemberConditionRecord condition : conditions)
        copied.append(condition);
    return areMembersUnchanged(copied.span());
}

bool GuardEmitter::listGuardsAskedBy(std::span<const MemberConditionRecord> conditions, Vector<GuardLeftToAsk, 4>& guardsLeft)
{
    Vector<GuardOfClassAskedByTest, 4> guards;
    for (MemberConditionRecord condition : conditions) {
        AskerOfClassGuardOfName asker = AskerOfClassGuardOfName::Reads;
        if (condition.kind == MemberConditionKind::MemberPassedByStores)
            asker = AskerOfClassGuardOfName::StoresAlone;
        else if (condition.kind == MemberConditionKind::MemberUnchangedAbove)
            asker = AskerOfClassGuardOfName::ReadsBehindLookAtObject;
        guards.append(GuardOfClassAskedByTest { condition.layout, condition.side, nameOfGuardAnsweringFor(condition.kind, condition.key), asker });
    }
    AOT::sort(guards);
    guards.shrink(guards.size() - std::ranges::unique(guards).size());
    RELEASE_ASSERT(!guards.isEmpty());

    StaticsEmitter& statics = emitter<StaticsEmitter>();
    for (std::span<const GuardOfClassAskedByTest> rest = guards.span(); !rest.empty();) {
        const GuardOfClassAskedByTest& first = rest.front();
        size_t numberOfGuardsOfSide = std::ranges::find_if_not(rest, [&](const GuardOfClassAskedByTest& guard) {
            return guard.isOfSameSideAs(first);
        }) - rest.begin();
        std::span<const GuardOfClassAskedByTest> guardsOfSide = rest.first(numberOfGuardsOfSide);
        rest = rest.subspan(numberOfGuardsOfSide);

        if (!first.classLayout || !m_plan.hasClassRecord(first.classLayout))
            return false;

        auto namesKeyOfProgram = [](const GuardOfClassAskedByTest& guard) {
            return guard.name && !liesInThread(guard.name) && guard.asker == AskerOfClassGuardOfName::Reads;
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
            if (namesShareGuard && namesKeyOfProgram(guard))
                continue;
            guardsLeft.append({ guard.classLayout, guard.side, guard.name ? statics.keyOfGuard(guard.classLayout, guard.side, guard.name, guard.asker) : statics.keyOfGuard(guard.classLayout, guard.side) });
        }
    }
    RELEASE_ASSERT(!guardsLeft.isEmpty());
    return true;
}

Condition GuardEmitter::isMemberAssigned(const MemberConditionRecord& condition)
{
    if (!condition.layout || !m_plan.hasClassRecord(condition.layout))
        return boolean(false);
    return Condition(m_out.notEqual(assignedMember(condition).value(), m_out.constInt64(JSValue::ValueUndefined)));
}

Boxed GuardEmitter::assignedMember(const MemberConditionRecord& condition)
{
    StaticsEmitter& statics = emitter<StaticsEmitter>();
    return Boxed(m_out.load64(statics.guardByte(statics.keyOfGuard(condition.layout, condition.side, condition.key, AskerOfClassGuardOfName::AssignedMember))));
}

void GuardEmitter::generateGetAssignedMember(const ValueContext& value)
{
    for (MemberConditionRecord condition : value->conditions()) {
        RELEASE_ASSERT(condition.kind == MemberConditionKind::MemberAssigned && condition.layout && m_plan.hasClassRecord(condition.layout));
        define(value, assignedMember(condition));
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool asksObjectPrototype(const Program& program, const MemberConditionRecord& condition)
{
    return condition.kind == MemberConditionKind::MemberPassedByStores && condition.key && isOnObjectPrototype(program.nameClassOf(condition.key));
}

Condition GuardEmitter::objectPrototypePassesStores()
{
    static_assert(!FieldTableEntry::noName);
    Pointer stoppers = loadImmutablePointer(AOT_INSTANCE_OFFSET(propertyCaches.objectPrototypeStoreStoppers));
    Word32 firstNames(m_out.load8ZeroExt32(m_out.address(m_heaps.AOTInstance_classGuards, stoppers.value(), 0)));
    return hasNoBits(firstNames, 1);
}

Condition GuardEmitter::areMembersUnchanged(std::span<const MemberConditionRecord> conditions)
{
    bool someConditionChecksObjectPrototype = std::ranges::any_of(conditions, [&](const MemberConditionRecord& condition) {
        return asksObjectPrototype(m_program, condition);
    });
    if (someConditionChecksObjectPrototype) {
        Vector<MemberConditionRecord, 4> others;
        for (const MemberConditionRecord& condition : conditions) {
            bool isOfRecord = m_program.at(condition.layout).kind == LayoutKind::Record;
            if (asksObjectPrototype(m_program, condition) && isOfRecord)
                continue;
            others.append(condition);
        }
        Condition passes = objectPrototypePassesStores();
        return others.isEmpty() ? passes : logicalAnd(passes, areClassMembersUnchanged(others.span()));
    }
    return areClassMembersUnchanged(conditions);
}

Condition GuardEmitter::areClassMembersUnchanged(std::span<const MemberConditionRecord> conditions)
{
    size_t numberAssigned = std::ranges::count(conditions, MemberConditionKind::MemberAssigned, &MemberConditionRecord::kind);
    if (numberAssigned) {
        std::optional<Condition> allHold;
        Vector<MemberConditionRecord, 4> unchanged;
        for (const MemberConditionRecord& condition : conditions) {
            if (condition.kind != MemberConditionKind::MemberAssigned) {
                unchanged.append(condition);
                continue;
            }
            Condition holds = isMemberAssigned(condition);
            allHold = allHold ? logicalAnd(*allHold, holds) : holds;
        }
        return unchanged.isEmpty() ? *allHold : logicalAnd(*allHold, areClassMembersUnchanged(unchanged.span()));
    }

    Vector<GuardLeftToAsk, 4> guardsLeft;
    if (!listGuardsAskedBy(conditions, guardsLeft))
        return boolean(false);
    StaticsEmitter& statics = emitter<StaticsEmitter>();
    if (guardsLeft.size() == 1)
        return guardInByteHolds(guardsLeft[0].classLayout, guardsLeft[0].side, statics.guardByte(guardsLeft[0].key));

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
        auto continueIf = [&](Condition holds) {
            LBasicBlock next = newBlock();
            branch(holds, { normalEdge(next), targets.notTaken });
            m_out.appendTo(next);
        };
        LBasicBlock hasShape = newBlock();
        emitter<ShapeTestEmitter>().branchOnShapeIn(cell, receivers, typeOf(object), { normalEdge(hasShape), targets.notTaken });
        m_out.appendTo(hasShape);
        if (asksForInstance)
            continueIf(belongsToInstance(cell));
        continueIf(hasNoBits(loadByte(cell, m_heaps.JSCell_typeInfoFlags), JSC::IsPristine));
        FunctionID declared = value->memberDeclared();
        if (!declared || !conventions().exists(conventions().closureEntryOf(declared))) {
            branch(value->conditions().isEmpty() ? boolean(false) : areMembersUnchanged(value->conditions()), targets);
            return;
        }
        continueIf(value->conditions().isEmpty() ? boolean(false) : areMembersUnchanged(value->conditions()));
        int32_t kindAndStub = static_cast<int32_t>(kindAndStubOfAnswer(KindOfAnswer::Value, StubThatAnswers::FunctionThatNameFinds) | answerIsYesOfQuestionOfLibrary);
        callVoidOperation(value, RuntimeOperation::StopAtStaleAnswer, { boxCell(cell), boxInt32(numberOf(m_mentions.add(NumberSpace::ClosureFunction, declared))), identifierNumber(value->key()), word32(kindAndStub) });
        m_out.jump(targets.taken);
    });
}

std::optional<GuardEmitter::RowOfCountsOfDoor> GuardEmitter::rowOfCountsOfDoor(const ValueContext& value)
{
    if (value->conditions().isEmpty() || AOT::hasFlag(build().options().testing, Wire::TestingSwitch::AssumeGuardsAndFacts))
        return std::nullopt;
    for (MemberConditionRecord condition : value->conditions()) {
        if (condition.kind == MemberConditionKind::MemberAssigned || asksObjectPrototype(m_program, condition))
            return std::nullopt;
    }
    Vector<ShapeTestEmitter::ShapesOfMember, 8> members = emitter<ShapeTestEmitter>().shapesOfMembersOf(value->receivers());
    if (members.size() < 2)
        return std::nullopt;
    uint32_t firstShape = UINT32_MAX;
    uint32_t lastShape = 0;
    for (const ShapeTestEmitter::ShapesOfMember& listed : members) {
        if (listed.shapes.isEmpty())
            return std::nullopt;
        firstShape = std::min(firstShape, listed.shapes.first());
        lastShape = std::max(lastShape, listed.shapes.last());
    }
    Vector<LayoutID, 8> classes(members.size(), [&](size_t index) {
        return declaredLayoutOf(m_program, members[index].member);
    });
    Vector<MemberConditionRecord, 8> common;
    Vector<Vector<MemberConditionRecord, 16>, 8> ofClass(members.size());
    size_t lastPlace = notFound;
    for (MemberConditionRecord condition : value->conditions()) {
        size_t place = notFound;
        if (condition.kind != MemberConditionKind::ClassEvaluatedOnce && condition.side == ClassSide::Prototype)
            place = lastPlace != notFound && classes[lastPlace] == condition.layout ? lastPlace : classes.find(condition.layout);
        if (place == notFound) {
            common.append(condition);
            continue;
        }
        lastPlace = place;
        ofClass[place].append(condition);
    }
    Vector<Vector<ClassGuardKey, 4>> slots(lastShape - firstShape + 1);
    for (size_t place = 0; place < members.size(); ++place) {
        const ShapeTestEmitter::ShapesOfMember& listed = members[place];
        Vector<MemberConditionRecord, 16> conditionsOfMember;
        conditionsOfMember.appendVector(common);
        conditionsOfMember.appendVector(ofClass[classes.find(classes[place])]);
        Vector<GuardLeftToAsk, 4> guardsLeft;
        if (conditionsOfMember.isEmpty() || !listGuardsAskedBy(conditionsOfMember.span(), guardsLeft))
            continue;
        Vector<ClassGuardKey, 4> keys(guardsLeft.size(), [&](size_t index) {
            return guardsLeft[index].key;
        });
        for (uint32_t shape : listed.shapes)
            slots[lastShape - shape] = keys;
    }
    if (std::ranges::all_of(slots, &Vector<ClassGuardKey, 4>::isEmpty))
        return std::nullopt;
    return RowOfCountsOfDoor { emitter<StaticsEmitter>().rowOfCountsOfGuards(slots.span()), lastShape, lastShape - firstShape + 1 };
}

void GuardEmitter::defineWhetherObjectIsPlainInstance(const ValueContext& value)
{
    ValueID object = value->objectOperand();
    bool asksForInstance = !hasOneInstance() && !holdsOnlyCompiledObjects(types(), m_plan, typeOf(object));
    std::optional<RowOfCountsOfDoor> row = rowOfCountsOfDoor(value);
    defineTestByBranching(value, [&](BranchTargets targets) {
        CellPointer cell = cellOrElse(value, object, targets.notTaken);
        auto continueIf = [&](Condition holds) {
            LBasicBlock next = newBlock();
            branch(holds, { normalEdge(next), targets.notTaken });
            m_out.appendTo(next);
        };
        std::optional<Word32> placeInRow;
        if (row) {
            placeInRow = emitter<ShapeTestEmitter>().distanceOfUnmarkedObjectFromShape(cell, value->receivers(), row->lastShape);
            continueIf(below(*placeInRow, row->width));
        } else {
            LBasicBlock hasShape = newBlock();
            emitter<ShapeTestEmitter>().branchOnShapeIn(cell, value->receivers(), typeOf(object), { normalEdge(hasShape), targets.notTaken }, MarkedObjects::AreRefused);
            m_out.appendTo(hasShape);
        }
        if (asksForInstance)
            continueIf(belongsToInstance(cell));
        if (testsMayAddNamesToObjectPrototype())
            continueIf(noNameWasAddedToObjectPrototype());
        if (row)
            continueIf(isZero(Word32(m_out.load32(m_out.baseIndex(*row->words.heap(), row->words.base(), zeroExtended(*placeInRow).value(), ScaleFour, row->words.offset())))));
        else if (!value->conditions().isEmpty() && !AOT::hasFlag(build().options().testing, Wire::TestingSwitch::AssumeGuardsAndFacts))
            continueIf(areMembersUnchanged(value->conditions()));
        if (value->isAskedWhetherAsBorn())
            continueIf(hasNoBits(emitter<ShapeTestEmitter>().orderWordOf(cell), orderWordIntegrityBits | orderWordHasFieldOutsideSlot | (value->mayHoldAddedNames() ? 0 : orderWordHasExtraProperties)));
        if (value->isBornWithoutPrototype()) {
            continueIf(hasNoBits(loadByte(cell, m_heaps.JSCell_typeInfoFlags), TypeInfoPerCellBit));
            branch(equal(boxedConstant(jsNull()), loadField<Boxed>(loadStructure(cell), m_heaps.Structure_prototype)), targets);
            return;
        }
        Condition isUntouched = hasNoBits(loadByte(cell, m_heaps.JSCell_typeInfoFlags), JSC::IsPristine | TypeInfoPerCellBit);
        if (value->isAskedWhetherAsBorn() || (!AOT::hasFlag(build().options().testing, Wire::TestingSwitch::CheckWalkAtRunTime) && !AOT::hasFlag(build().options().testing, Wire::TestingSwitch::CheckProofsAtRunTime))) {
            branch(isUntouched, targets);
            return;
        }
        continueIf(isUntouched);
        int32_t kindAndStub = static_cast<int32_t>(kindAndStubOfAnswer(KindOfAnswer::Absent, StubThatAnswers::DoorOfPlainInstance) | answerIsYesOfQuestionOfLibrary);
        LayoutID classAsked = AOT::hasFlag(build().options().testing, Wire::TestingSwitch::CheckWalkAtRunTime) ? value->classAsked() : LayoutID();
        int32_t indexOfClassAsked = static_cast<int32_t>(classAsked ? classAsked.index() : UINT32_MAX);
        callVoidOperation(value, RuntimeOperation::StopAtStaleAnswer, { boxCell(cell), undefined(), word32(indexOfClassAsked), word32(kindAndStub) });
        m_out.jump(targets.taken);
    });
}

void GuardEmitter::defineWhetherNoMemberIsFound(const ValueContext& value)
{
    if (value->asksObjectByInquiry()) {
        int32_t kindAndStub = static_cast<int32_t>(kindAndStubOfAnswer(KindOfAnswer::Absent, StubThatAnswers::ProofThatLookupFindsNothing));
        callVoidOperation(value, RuntimeOperation::StopAtStaleAnswer, { in<Boxed>(value->objectOperand()), undefined(), identifierNumber(value->key()), word32(kindAndStub) });
        defineTest(value, boolean(true));
        return;
    }
    defineTestByBranching(value, [&](BranchTargets targets) {
        CellPointer cell = cellOrElse(value, value->objectOperand(), targets.notTaken);
        auto continueIf = [&](Condition holds) {
            LBasicBlock next = newBlock();
            branch(holds, { normalEdge(next), targets.notTaken });
            m_out.appendTo(next);
        };
        if (!types().isSubset(typeIDOf(value->objectOperand()), types().forLayoutSet(value->receivers()))) {
            LBasicBlock hasShape = newBlock();
            emitter<ShapeTestEmitter>().branchOnShapeIn(cell, value->receivers(), typeOf(value->objectOperand()), { normalEdge(hasShape), targets.notTaken });
            m_out.appendTo(hasShape);
            if (!hasOneInstance() && !holdsOnlyCompiledObjects(types(), m_plan, typeOf(value->objectOperand())))
                continueIf(belongsToInstance(cell));
        }
        uint32_t bits = (value->asksForNoExtraPropertyUnderKey() ? orderWordHasExtraProperties : 0) | (value->asksForNoFieldOutsideSlot() ? orderWordHasFieldOutsideSlot : 0);
        PropertyKeyID key = value->key();
        std::optional<NumberedImmediate> answers = value->asksForNoExtraPropertyUnderKey() && !value->asksRunTime() ? emitter<AccessEmitter>().answersOf(value, key, SiteUse::AbsenceAmongExtraProperties) : std::nullopt;
        if (answers || value->asksRunTime()) {
            Word32 orderWord = emitter<ShapeTestEmitter>().orderWordOf(cell);
            LBasicBlock holdsNoSuchProperty = newBlock();
            LBasicBlock hasSomeBit = newBlock();
            branch(hasNoBits(orderWord, bits), { normalEdge(holdsNoSuchProperty), rareEdge(hasSomeBit) });
            m_out.appendTo(hasSomeBit);
            if (value->asksForNoFieldOutsideSlot())
                continueIf(hasNoBits(orderWord, orderWordHasFieldOutsideSlot));
            if (answers) {
                LValue operands[] = { cell.value() };
                emitter<RuntimeCallEmitter>().callStubAndBranch(value, findsNoExtraPropertyAtSite(), operands, { .numberedImmediate = *answers }, { normalEdge(holdsNoSuchProperty), targets.notTaken });
            } else
                branch(callWithoutSite<Condition>(RuntimeOperation::HasExtraProperty, { cell, identifierNumber(key) }, { .isOnRarePath = true }), { targets.notTaken, normalEdge(holdsNoSuchProperty) });
            m_out.appendTo(holdsNoSuchProperty);
        } else if (bits)
            continueIf(hasNoBits(emitter<ShapeTestEmitter>().orderWordOf(cell), bits));
        if (value->asksForLayoutPrototype())
            continueIf(hasNoBits(loadByte(cell, m_heaps.JSCell_typeInfoFlags), JSC::IsPristine));
        bool shouldAssumeGuardsHold = AOT::hasFlag(build().options().testing, Wire::TestingSwitch::AssumeGuardsAndFacts);
        Condition guardsHold = value->conditions().isEmpty() || shouldAssumeGuardsHold ? boolean(true) : areMembersUnchanged(value->conditions());
        if (!value->isQuestionOfLibrary() || !emitter<AccessEmitter>().verifiesFastPathAnswers(key)) {
            branch(guardsHold, targets);
            return;
        }
        continueIf(guardsHold);
        int32_t kindAndStub = static_cast<int32_t>(kindAndStubOfAnswer(KindOfAnswer::Absent, StubThatAnswers::ProofThatLookupFindsNothing) | answerIsYesOfQuestionOfLibrary);
        callVoidOperation(value, RuntimeOperation::StopAtStaleAnswer, { boxCell(cell), undefined(), identifierNumber(key), word32(kindAndStub) });
        m_out.jump(targets.taken);
    });
}

void GuardEmitter::branchOnOwnPropertyByCell(const ValueContext& value, BranchTargets targets, LBasicBlock asksRunTime)
{
    ValueID object = value->objectOperand();
    LayoutSetID receivers = value->receivers();
    FieldAccess access = fieldAccessFor(value->key(), receivers, false);
    bool isFieldOfAll = access.coverage == FieldAccess::Coverage::EveryMember && access.hasLocation;
    bool isFieldOfSome = access.coverage == FieldAccess::Coverage::SomeMembers && access.hasLocation && !access.findsOffsetByShape && !access.mayBeFilledWithoutStorage;
    if (access.coverage != FieldAccess::Coverage::NoMember && !isFieldOfAll && !isFieldOfSome) {
        m_out.jump(asksRunTime);
        return;
    }
    CellPointer cell = cellOrElse(value, object, rareEdge(asksRunTime));
    auto continueIf = [&](Condition holds) {
        LBasicBlock next = newBlock();
        branch(holds, { normalEdge(next), rareEdge(asksRunTime) });
        m_out.appendTo(next);
    };
    LBasicBlock hasShape = newBlock();
    emitter<ShapeTestEmitter>().branchOnShapeIn(cell, receivers, typeOf(object), { normalEdge(hasShape), rareEdge(asksRunTime) });
    m_out.appendTo(hasShape);
    Word32 orderWord = emitter<ShapeTestEmitter>().orderWordOf(cell);
    auto finishWithoutField = [&] {
        continueIf(hasNoBits(orderWord, orderWordHasExtraProperties));
        m_out.jump(targets.notTaken);
    };
    if (access.coverage == FieldAccess::Coverage::NoMember) {
        finishWithoutField();
        return;
    }
    if (isFieldOfSome) {
        LBasicBlock hasField = newBlock();
        LBasicBlock lacksField = newBlock();
        LayoutSetID membersWithField = types().layoutSets().ofSorted(access.membersWithField.span());
        emitter<ShapeTestEmitter>().branchOnShapeAmongReceivers(value, cell, membersWithField, { normalEdge(hasField), normalEdge(lacksField) });
        m_out.appendTo(lacksField);
        finishWithoutField();
        m_out.appendTo(hasField);
    }
    continueIf(hasNoBits(orderWord, orderWordHasFieldOutsideSlot));
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

Condition GuardEmitter::holds(LayoutID classLayout, ClassSide side, PropertyKeyID name)
{
    if (!classLayout || !m_plan.hasClassRecord(classLayout))
        return boolean(false);
    StaticsEmitter& statics = emitter<StaticsEmitter>();
    return guardInByteHolds(classLayout, side, statics.guardByte(name ? statics.keyOfGuard(classLayout, side, name) : statics.keyOfGuard(classLayout, side)));
}

Condition GuardEmitter::guardsWereBegun(LayoutID constructorFunction)
{
    StaticsEmitter& statics = emitter<StaticsEmitter>();
    Word32 byte(m_out.load8ZeroExt32(statics.guardByte(statics.keyOfGuard(constructorFunction, ClassSide::Prototype))));
    return hasBits(byte, maskOfClassGuardsBegun);
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
    if (aspects.contains(PristineAspects::InheritedNamesKept)) {
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
