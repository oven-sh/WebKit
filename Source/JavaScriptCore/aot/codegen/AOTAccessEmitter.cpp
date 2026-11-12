/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTAccessEmitter.h"

#if ENABLE(AOT)

#include "AOTArrayBuiltinEmitter.h"
#include "AOTBarrierEmitter.h"
#include "AOTBuiltinIDs.h"
#include "AOTBuiltinCallEmitter.h"
#include "AOTCallEmitter.h"
#include "AOTClosuresLent.h"
#include "AOTConstantEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTConvention.h"
#include "AOTElementAccessEmitter.h"
#include "AOTFieldTable.h"
#include "AOTGuardEmitter.h"
#include "AOTIRProgram.h"
#include "AOTInliningAcrossFiles.h"
#include "AOTNameClass.h"
#include "AOTOperatorEmitter.h"
#include "AOTPropertyCachesFront.h"
#include "AOTResolvedCallEmitter.h"
#include "AOTResultRules.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTRuntimeWords.h"
#include "AOTSlotAccessEmitter.h"
#include "AOTSlotEmitter.h"
#include "AOTState.h"
#include "AOTStubCalls.h"
#include "AOTStubsBoxedCalls.h"
#include "AOTStubsCompiledMethods.h"
#include "AOTStubsElements.h"
#include "AOTStubsGlobalBindings.h"
#include "AOTStubsPropertyKeys.h"
#include "AOTThrowEmitter.h"
#include "AOTTruthinessEmitter.h"
#include "AOTTypeArena.h"
#include "ClonedArguments.h"
#include "ECMAMode.h"
#include "GetterSetter.h"
#include "PropertyName.h"
#include "RegExpObject.h"
#include <limits>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(RowDisplacementTable::maxColumn <= FieldTableEntry::maxName);

Boxed AccessEmitter::cellOfKey(const OperationContext& context, PropertyKeyID key)
{
    return boxCell(emitter<ConstantEmitter>().keyConstant(context, key));
}

bool AccessEmitter::comesAgain(const OperationContext& context) const
{
    return m_functionComesAgain || m_generator.isInLoop(context.block());
}

std::optional<Pointer> AccessEmitter::siteOf(const OperationContext& context, PropertyKeyID key, SlotAccess access)
{
    if (isSymbolOfRealm(key) || !m_generator.paysForCodeInLine(context.block()))
        return std::nullopt;
    return siteCacheAddress(context, key, access);
}

std::optional<Pointer> AccessEmitter::siteOfStore(const OperationContext& context, PropertyKeyID key, SlotAccess access)
{
    if (isSymbolOfRealm(key) || !comesAgain(context))
        return std::nullopt;
    if ((m_code.at(context.block()).frequency == BlockFrequency::Rare && !m_code.at(context.block()).keepsSitesThoughRare) || (context.valueID() && testsItsOwnGuess(m_code.at(context.valueID()))))
        return std::nullopt;
    return siteCacheAddress(context, key, access);
}

std::optional<NumberedImmediate> AccessEmitter::answersOf(const OperationContext& context, PropertyKeyID key, SiteUse use)
{
    if (isSymbolOfRealm(key))
        return std::nullopt;
    return answersOfKeyThatIsNoSymbolOfRealm(context, key, use);
}

bool AccessEmitter::keepsSite(const OperationContext& context) const
{
    if (!comesAgain(context))
        return false;
    return (m_code.at(context.block()).frequency != BlockFrequency::Rare || m_code.at(context.block()).keepsSitesThoughRare) && !(context.valueID() && testsItsOwnGuess(m_code.at(context.valueID())));
}

std::optional<NumberedImmediate> AccessEmitter::answersOfKeyThatIsNoSymbolOfRealm(const OperationContext& context, PropertyKeyID key, SiteUse use)
{
    return keepsSite(context) ? answersAtSite(context, key, use) : std::nullopt;
}

StubKey AccessEmitter::stubThatReadsAtSite(PropertyKeyID key, ValueID receiver)
{
    UNUSED_PARAM(key);
    return receiver && typeOf(receiver).isObject() ? getByIdOfObjectAtSite() : getByIdAtSite();
}

bool namesSizeOfTables(const Program& program, const Facts& facts, PropertyKeyID key)
{
    if (key != program.keyOf(BuiltinNames::size)) [[likely]]
        return false;
    const BuiltinTable& table = program.builtins();
    for (BuiltinBoxedStub getter : { BuiltinBoxedStub::MapSize, BuiltinBoxedStub::SetSize }) {
        for (BuiltinID member : table.membersThatHoldMethodOf(getter)) {
            if (table.at(member).name != BuiltinNames::size)
                return false;
            if (program.admitsReplacedBuiltins)
                continue;
            bool isLocked = table.at(member).mutability == BuiltinMutability::AlwaysLocked || table.isLocationOfLockedObject(member) || facts.builtinLocation(member) == BuiltinLocationState::Constant;
            if (!isLocked || table.has(BuiltinMark::MayBeAbsent, member) || program.isUnlocked(member) || program.isUnlocked(table.canonicalID(member)))
                return false;
        }
    }
    return true;
}

bool AccessEmitter::verifiesFastPathAnswers(PropertyKeyID key)
{
    return AOT::hasFlag(build().options().testing, Wire::TestingSwitch::CheckWalkAtRunTime) && !isSymbolOfRealm(key);
}

Word32 AccessEmitter::recordPendingRead(Boxed receiver, PropertyKeyID key)
{
    Word32 count = slowPathReadCount();
    LValue state = m_out.loadPtr(mutableInstanceField(AOT_INSTANCE_OFFSET(state)));
    m_out.store64(receiver.value(), m_out.address(m_heaps.AOTInstance_mutableFields, state, OBJECT_OFFSETOF(State, receiverOfPendingRead)));
    m_out.store32(identifierNumber(key).value(), m_out.address(m_heaps.AOTInstance_mutableFields, state, OBJECT_OFFSETOF(State, keyOfPendingRead)));
    m_out.store32(count.value(), m_out.address(m_heaps.AOTInstance_mutableFields, state, OBJECT_OFFSETOF(State, slowPathReadCountAtPendingRead)));
    return count;
}

void AccessEmitter::verifyFastPathAnswer(const OperationContext& context, Boxed receiver, PropertyKeyID key, Boxed answer, StubThatAnswers stub, Word32 countBeforeRead)
{
    int32_t kindAndStub = static_cast<int32_t>(kindAndStubOfAnswer(KindOfAnswer::Value, stub));
    Word32 kindAndStubWithSlowPathBit = select(equal(slowPathReadCount(), countBeforeRead), word32(kindAndStub), word32(kindAndStub | static_cast<int32_t>(answerMayComeFromSlowPath)));
    callVoidOperation(context, RuntimeOperation::StopAtStaleAnswer, { receiver, answer, identifierNumber(key), kindAndStubWithSlowPathBit });
}

LValue AccessEmitter::newestAnswerOfSite(Boxed receiver, ValueID receiverID, NumberedImmediate answers, LBasicBlock asksStub, LBasicBlock hasOtherKey)
{
    static_assert(SiteAnswers::mostCommonForm(SiteUse::Read) == SiteAnswers::Form::OwnValueInStorage && !SiteAnswers::bitsOf(SiteUse::Read, SiteAnswers::Form::OwnValueInStorage));
    static_assert(!OBJECT_OFFSETOF(SiteAnswers::Answer, key) && OBJECT_OFFSETOF(SiteAnswers::Answer, operand) == sizeof(uint32_t) && InstanceLayout::sizeOfIndexOfSiteAnswers == sizeof(uint16_t));
    auto goOnIf = [&](Condition condition) {
        LBasicBlock next = newBlock();
        m_out.branch(condition.value(), normalEdge(next), unlikelyEdge(asksStub));
        m_out.appendToContinuation(next);
    };
    Type known = typeOf(receiverID);
    if (!known.isObject()) {
        Condition isPresentCell = isCell(receiver);
        if (known.primitives & Primitive::Absent)
            isPresentCell = logicalAnd(isPresentCell, isNotEmpty(receiver));
        goOnIf(isPresentCell);
    }
    LValue index = m_out.zeroExtPtr(m_out.load16ZeroExt32(instanceWord(m_heaps.AOTInstance_siteCaches, answers.mention)));
    LValue answer = m_out.load64(m_out.baseIndex(m_heaps.AOTInstance_siteCaches, m_instance, index, ScaleEight, InstanceLayout::offsetOfSiteAnswers + OBJECT_OFFSETOF(SiteAnswers, second)));
    Condition isAnswered = equal(Word32(m_out.castToInt32(answer)), loadStructureID(unboxCell(receiver)));
    if (!hasOtherKey) {
        goOnIf(isAnswered);
        return answer;
    }
    LBasicBlock next = newBlock();
    m_out.branch(isAnswered.value(), normalEdge(next), normalEdge(hasOtherKey));
    m_out.appendToContinuation(next);
    return answer;
}

std::optional<NumberedImmediate> AccessEmitter::inlineReadAnswers(const ValueContext& read)
{
    PropertyKeyID key = read->key();
    if (read.opcode() != Opcode::PropGet || !m_generator.usesLoopForms(read.block()) || isSymbolOfRealm(key) || verifiesFastPathAnswers(key) || isErrorMessageRead(read))
        return std::nullopt;
    if (m_program.nameClassOf(key) == NameClass::Length || namesSizeOfTables(m_program, m_facts, key) || m_plan.slots().someLayoutHasField(key))
        return std::nullopt;
    return answersOfKeyThatIsNoSymbolOfRealm(read, key, SiteUse::Read);
}

Boxed AccessEmitter::readWithInlineSiteCheck(const ValueContext& read, Boxed receiver, ValueID receiverID, NumberedImmediate answers)
{
    build().statistics().count(StatisticID::SiteCachedReadsEmittedInline);
    LBasicBlock asksStub = newBlock();
    LBasicBlock liesInCell = newBlock();
    LBasicBlock liesInStorage = newBlock();
    Join<Boxed> join(m_out);
    {
        Machine::DeclaredAccessesSuspension suspension(m_out);
        LValue place = m_out.aShr(newestAnswerOfSite(receiver, receiverID, answers, asksStub), 32);
        CellPointer object = unboxCell(receiver);
        m_out.branch(m_out.greaterThanOrEqual(place, m_out.int64Zero), normalEdge(liesInCell), unlikelyEdge(liesInStorage));
        m_out.appendToContinuation(liesInCell);
        join.add(Boxed(m_out.load64(m_out.baseIndex(m_heaps.root, object.value(), place, ScaleEight))));

        m_out.appendTo(liesInStorage);
        join.add(Boxed(m_out.load64(m_out.baseIndex(m_heaps.root, loadField<Pointer>(object, m_heaps.JSObject_butterfly).value(), place, ScaleEight))));
    }

    m_out.appendTo(asksStub);
    ReceiverAtSite receiverIs = typeOf(receiverID).isObject() ? ReceiverAtSite::IsCell : ReceiverAtSite::IsAnyValue;
    join.add(Boxed(callStub(read, readAtSiteAskingStructureFirst(receiverIs, SiteUse::Read, read->receiverIsNoObjectOfCompiledCode()), { receiver.value() }, { .numberedImmediate = answers }).first));
    return join.finish();
}

Boxed AccessEmitter::getById(const OperationContext& context, Boxed receiver, PropertyKeyID key, ValueID receiverID)
{
    if (!verifiesFastPathAnswers(key)) [[likely]]
        return getByIdThroughStub(context, receiver, key, receiverID);
    Word32 countBeforeRead = recordPendingRead(receiver, key);
    Boxed result = getByIdThroughStub(context, receiver, key, receiverID);
    verifyFastPathAnswer(context, receiver, key, result, StubThatAnswers::ReadByName, countBeforeRead);
    return result;
}

bool AccessEmitter::isErrorMessageRead(const OperationContext& context) const
{
    if (!context.valueID() || !m_generator.isInLoop(context.block()) || m_code.at(context.block()).frequency == BlockFrequency::Rare)
        return false;
    const Operation& read = static_cast<const Operation&>(context.instruction());
    switch (read.opcode) {
    case Opcode::PropGet:
    case Opcode::GetValueAtSiteOrAbsent:
        return read.isErrorMessageExpected();
    case Opcode::CallBuiltin: {
        const BuiltinOverload& overload = m_program.builtins().at(read.overload());
        return overload.code.implementation.kind == BuiltinImplementationKind::OwnSlot && m_program.builtins().at(overload.builtin).name == BuiltinNames::message;
    }
    default:
        return false;
    }
}

Boxed AccessEmitter::loadErrorMessage(Boxed receiver, ValueID receiverID, LBasicBlock whenNotKept)
{
    Type known = receiverID ? typeOf(receiverID) : Type();
    if (!receiverID || known.primitives & ~cellPrimitives) {
        Condition isPresentCell = isCell(receiver);
        if (!receiverID || known.primitives & Primitive::Absent)
            isPresentCell = logicalAnd(isPresentCell, isNotEmpty(receiver));
        branchUnless(isPresentCell, whenNotKept);
    }
    CellPointer object = unboxCell(receiver);
    Machine::DeclaredAccessesSuspension suspension(m_out);
    Word32 structure = loadStructureID(object);
    constexpr unsigned shift = PropertyCacheWords::shiftOfErrorWithMessage;
    LValue place = m_out.zeroExtPtr(m_out.bitAnd(m_out.lShr(structure.value(), shift), m_out.constInt32(PropertyCacheWords::numberOfErrorsWithMessage - 1)));
    TypedPointer kept = instanceField(m_heaps.collectorState, AOT_INSTANCE_OFFSET(propertyCaches.errorsWithMessage));
    Word32 structureKept(m_out.load32(m_out.baseIndex(*kept.heap(), kept.base(), place, ScaleFour, kept.offset())));
    branchUnless(equal(structureKept, structure), whenNotKept);
    Pointer butterfly = loadField<Pointer>(object, m_heaps.JSObject_butterfly);
    return Boxed(m_out.load64(m_out.address(m_heaps.properties.atAnyNumber(), butterfly.value(), offsetInButterfly(firstOutOfLineOffset) * sizeof(EncodedJSValue))));
}

Boxed AccessEmitter::getByIdThroughStub(const OperationContext& context, Boxed receiver, PropertyKeyID key, ValueID receiverID)
{
    if (isSymbolOfRealm(key))
        return Boxed(callStub(context, getByIdOfSymbolOfRealm(), { receiver.value() }, { .numberedImmediate = NumberedImmediate { mentionOfName(key) } }).first);
    const NumberedImmediate mention { mentionOfName(key) };
    if (m_program.nameClassOf(key) == NameClass::Length) {
        const Operation* read = context.valueID() ? &static_cast<const Operation&>(context.instruction()) : nullptr;
        std::optional<NumberedImmediate> answers = read && read->opcode == Opcode::PropGet && read->receiverIsAskedForNameOfNoBuiltin() ? answersOfKeyThatIsNoSymbolOfRealm(context, key, SiteUse::Read) : std::nullopt;
        if (answers)
            return Boxed(callStub(context, readLengthAtSite(!m_plan.slots().someLayoutWrittenInHasField(m_program.at(m_unit.function).module, key)), { receiver.value() }, { .numberedImmediate = *answers }).first);
        if (!read || read->opcode != Opcode::PropGet || !read->receiverIsExpectedToBeArguments() || !m_generator.isInLoop(context.block()))
            return Boxed(callStub(context, getLengthAtSite(), { receiver.value() }, { .numberedImmediate = mention }).first);
        LBasicBlock isNotUnmappedAsMade = newBlock();
        LBasicBlock isNotAsMade = newBlock();
        Join<Boxed> join(m_out);
        Type known = receiverID ? typeOf(receiverID) : Type();
        if (!receiverID || known.primitives & ~cellPrimitives) {
            Condition isPresentCell = isCell(receiver);
            if (!receiverID || known.primitives & Primitive::Absent)
                isPresentCell = logicalAnd(isPresentCell, isNotEmpty(receiver));
            branchUnless(isPresentCell, isNotAsMade);
        }
        CellPointer object = unboxCell(receiver);
        Word32 structureAsMade(m_out.load32(mutableInstanceField(AOT_INSTANCE_OFFSET(allocation.inlineAllocations[static_cast<unsigned>(InlineAllocation::Arguments)].header))));
        branchUnless(equal(loadStructureID(object), structureAsMade), isNotUnmappedAsMade);
        Pointer butterfly = loadField<Pointer>(object, m_heaps.JSObject_butterfly);
        join.add(Boxed(m_out.load64(m_out.address(m_heaps.properties.atAnyNumber(), butterfly.value(), offsetInButterfly(clonedArgumentsLengthPropertyOffset) * sizeof(EncodedJSValue)))));

        m_out.appendTo(isNotUnmappedAsMade);
        branchUnless(hasCellType(object, DirectArgumentsType), isNotAsMade);
        branchUnless(isZero(Integer(loadField<Pointer>(object, m_heaps.DirectArguments_mappedArguments).value())), isNotAsMade);
        join.add(boxInt32(loadField<Word32>(object, m_heaps.DirectArguments_length)));

        m_out.appendTo(isNotAsMade);
        join.add(Boxed(callStub(context, getLengthAtSite(), { receiver.value() }, { .numberedImmediate = mention, .isOnRarePath = true }).first));
        return join.finish();
    }
    const bool readsSizeOfTable = namesSizeOfTables(m_program, m_facts, key);
    const Operation* read = context.valueID() ? &static_cast<const Operation&>(context.instruction()) : nullptr;
    const bool receiverIsNoObjectOfCompiledCode = read && read->opcode == Opcode::PropGet && read->receiverIsNoObjectOfCompiledCode();
    const bool asksShapeFirst = m_plan.slots().someLayoutWrittenInHasField(m_program.at(m_unit.function).module, key) && !m_code.at(context.block()).keepsSitesThoughRare && !receiverIsNoObjectOfCompiledCode;
    const SiteUse use = !readsSizeOfTable && !asksShapeFirst && m_plan.slots().someLayoutHasField(key) ? SiteUse::ReadUnderNameOfField : SiteUse::Read;
    if (std::optional<NumberedImmediate> answers = answersOfKeyThatIsNoSymbolOfRealm(context, key, use)) {
        ReceiverAtSite receiverIs = receiverID && typeOf(receiverID).isObject() ? ReceiverAtSite::IsCell : ReceiverAtSite::IsAnyValue;
        StubKey stub = readsSizeOfTable ? readSizeOfTableAtSite() : asksShapeFirst ? readAtSite(receiverIs) : readAtSiteAskingStructureFirst(receiverIs, use, receiverIsNoObjectOfCompiledCode);
        if (!isErrorMessageRead(context))
            return Boxed(callStub(context, stub, { receiver.value() }, { .numberedImmediate = *answers }).first);
        LBasicBlock asksStub = newBlock();
        Join<Boxed> join(m_out);
        join.add(loadErrorMessage(receiver, receiverID, asksStub));

        m_out.appendTo(asksStub);
        join.add(Boxed(callStub(context, stub, { receiver.value() }, { .numberedImmediate = *answers, .isOnRarePath = true }).first));
        return join.finish();
    }
    if (readsSizeOfTable)
        return Boxed(callStub(context, getSizeOfTable(), { receiver.value() }, { .numberedImmediate = mention }).first);
    return Boxed(callStub(context, getByIdCalledByCode(), { receiver.value() }, { .numberedImmediate = mention }).first);
}

void AccessEmitter::putById(const OperationContext& context, Boxed receiver, PropertyKeyID key, Boxed value, PutMode mode, std::optional<Pointer> site)
{
    if (isSymbolOfRealm(key)) {
        callStub(context, putByIdOfSymbolOfRealm(mode), { receiver.value(), value.value() }, { .numberedImmediate = NumberedImmediate { mentionOfName(key) } });
        return;
    }
    CallOptions options { .numberedImmediate = NumberedImmediate { mentionOfName(key) } };
    if (!comesAgain(context)) {
        callStub(context, putByIdOnce(mode), { receiver.value(), value.value() }, options);
        return;
    }
    if (site) {
        callStub(context, m_plan.slots().someLayoutHasField(key) ? putByIdAtSite(mode) : putByIdAtSiteAskingNoTableOfFields(mode), { receiver.value(), value.value(), site->value() }, options);
        return;
    }
    callStub(context, putByIdHashed(mode), { receiver.value(), value.value() }, options);
}

Boxed AccessEmitter::loadBySlotAtSite(CellPointer object, Pointer site, Slot::Kind kind, LBasicBlock whenSlotIsOfOtherKind, LBasicBlock whenSlotIsOfOtherStructure)
{
    RELEASE_ASSERT(Slot::payloadOf(kind) == SlotPayload::Holder && Slot::locationOf(kind) == SlotLocation::WordIndex);
    auto partOfSlot = [&](ptrdiff_t offset) {
        return m_out.address(m_heaps.AOTInstance_siteCaches, site.value(), offset);
    };
    Word32 structureIDOfSlot(m_out.load32(partOfSlot(Slot::offsetOfStructureID())));
    branchUnless(equal(structureIDOfSlot, loadStructureID(object)), whenSlotIsOfOtherStructure);
    branchUnless(equal(Word32(m_out.load8ZeroExt32(partOfSlot(Slot::offsetOfKind()))), static_cast<int32_t>(kind)), whenSlotIsOfOtherKind);
    Pointer holder(m_out.load64(partOfSlot(Slot::offsetOfPayload())));
    Pointer owner = select(isZero(Integer(holder.value())), asPointer(object), holder);
    Pointer butterfly = loadField<Pointer>(owner, m_heaps.JSObject_butterfly);
    Pointer storage = select(hasBits(Word32(m_out.load8ZeroExt32(partOfSlot(Slot::offsetOfFlags()))), Slot::isOutOfLine), butterfly, owner);
    Integer wordIndex = signExtended(Word32(m_out.load16SignExt32(partOfSlot(Slot::offsetOfLocation()))));
    return Boxed(m_out.load64(m_out.address(m_heaps.root, offsetBy(storage, wordIndex, 3).value())));
}

Boxed AccessEmitter::getMemberAboveLayouts(const ValueContext& value, Boxed receiver, Pointer site)
{
    LBasicBlock asksStub = newBlock();
    Join<Boxed> join(m_out);
    std::optional<Word32> countBeforeRead = verifiesFastPathAnswers(value->key()) ? std::optional(recordPendingRead(receiver, value->key())) : std::nullopt;
    {
        Machine::DeclaredAccessesSuspension suspension(m_out);
        join.add(loadBySlotAtSite(unboxCell(receiver), site, Slot::Kind::Value, asksStub, asksStub));
    }

    m_out.appendTo(asksStub);
    join.add(Boxed(callStub(value, stubThatReadsAtSite(value->key(), value->objectOperand()), { receiver.value(), site.value() }, { .numberedImmediate = NumberedImmediate { mentionOfName(value->key()) }, .isOnRarePath = true }).first));
    Boxed member = join.finish();
    if (countBeforeRead) [[unlikely]]
        verifyFastPathAnswer(value, receiver, value->key(), member, StubThatAnswers::ReadByName, *countBeforeRead);
    return member;
}

std::optional<PlacedSlot> AccessEmitter::findSlotInCellOfEveryReceiver(const Type& receiver, PropertyKeyID key)
{
    if (!receiver.isObject() || !receiver.layouts || !receiver.hasOnlyLayouts())
        return std::nullopt;
    FieldAccess access = fieldAccessFor(key, receiver.layouts, false);
    if (!access.hasLocation || access.coverage != FieldAccess::Coverage::EveryMember || access.findsOffsetByShape || access.mayBeFilledWithoutStorage)
        return std::nullopt;
    const PlacedSlot& slot = access.slot;
    if (!slot.hasStorage() || slot.holdsAccessor || slot.isOutOfLine() || slot.mayExistWithoutStorage || (slot.representation != SlotRepresentation::JSValue && slot.representation != SlotRepresentation::Cell))
        return std::nullopt;
    for (LayoutID member : access.membersWithField) {
        if (!m_plan.layout(member).holdsCompiledObjects())
            return std::nullopt;
        std::optional<FieldIndex> index = m_plan.slots().findField(m_program, member, key);
        const Layout& layout = m_program.at(member);
        if (!index || (*index < layout.fields.size() && !layout.fields[*index].form.isEmpty()))
            return std::nullopt;
    }
    return slot;
}

Boxed AccessEmitter::getMemberInSlotOrByName(const ValueContext& value, Boxed receiver, const PlacedSlot& slot)
{
    PropertyKeyID key = value->key();
    LBasicBlock isFilled = newBlock();
    LBasicBlock isEmpty = newBlock();
    Join<Boxed> join(m_out);
    Boxed word(m_out.load64(m_out.address(m_heaps.root, unboxCell(receiver).value(), slot.offset)));
    ScopeFork fork = m_out.forkScope();
    m_out.branch(isZero(Integer(word.value())).value(), unlikelyEdge(isEmpty), normalEdge(isFilled));
    m_out.appendToArm(isFilled, fork);
    join.add(word);

    m_out.appendToArm(isEmpty, fork);
    join.add(getById(value, receiver, key, value->objectOperand()));
    return join.finish();
}

Boxed AccessEmitter::getNamedProperty(const ValueContext& value)
{
    ValueID receiverID = value->objectOperand();
    PropertyKeyID key = value->key();
    Boxed receiver = in<Boxed>(receiverID);
    if (!m_generator.paysForCodeInLine(value.block())) {
        m_out.noteForm(Form::Closed, StateOfKnowledge::Always);
        return getById(value, receiver, key, receiverID);
    }
    Type receiverType = typeOf(receiverID);
    bool receiversAreListed = value->receivers() || (receiverType.layouts && receiverType.hasOnlyLayouts());
    bool isLength = !liesInThread(key) && m_program.nameClassOf(key) == NameClass::Length;
    bool asksSiteInLine = receiverType.isObject() && receiversAreListed && !isLength;
    if (std::optional<PlacedSlot> slot = asksSiteInLine ? findSlotInCellOfEveryReceiver(receiverType, key) : std::nullopt) {
        m_out.noteForm(Form::Open, StateOfKnowledge::Always, Place::Loop);
        return getMemberInSlotOrByName(value, receiver, *slot);
    }
    bool shapesTakeTurnsAtSite = asksSiteInLine && receiverType.layouts && !holdsOneMember(receiverType.layouts) && std::ranges::any_of(membersOf(types(), receiverType), [&](LayoutID member) {
        return !!m_plan.slots().findField(m_program, member, key);
    });
    std::optional<Pointer> site = asksSiteInLine && !shapesTakeTurnsAtSite ? siteOf(value, key, SlotAccess::Read) : std::nullopt;
    if (std::optional<NumberedImmediate> answers = site ? std::nullopt : inlineReadAnswers(value)) {
        m_out.noteForm(Form::Open, StateOfKnowledge::Always, Place::Loop);
        return readWithInlineSiteCheck(value, receiver, receiverID, *answers);
    }
    if (!site) {
        m_out.noteForm(Form::Closed, StateOfKnowledge::Always);
        return getById(value, receiver, key, receiverID);
    }
    m_out.noteForm(Form::Open, StateOfKnowledge::Always, Place::Loop);

    Pointer siteOfThisLook(m_out.opaque(site->value()));
    return getMemberAboveLayouts(value, receiver, siteOfThisLook);
}

Condition AccessEmitter::hasById(const OperationContext& context, Boxed receiver, PropertyKeyID key)
{
    if (isSymbolOfRealm(key))
        return hasByVal(context, receiver, cellOfKey(context, key));
    Condition isThere = findsName(context, receiver, key, PresenceQuestion::In);
    if (verifiesFastPathAnswers(key)) [[unlikely]]
        verifyFastPathAnswer(context, receiver, key, boxBoolean(isThere), StubThatAnswers::PresenceByName, slowPathReadCount());
    return isThere;
}

Condition AccessEmitter::findsName(const OperationContext& context, Boxed receiver, PropertyKeyID key, PresenceQuestion question)
{
    const bool hasSite = m_code.at(context.block()).frequency != BlockFrequency::Rare && !isSymbolOfRealm(key) && keepsSite(context);
    std::optional<NumberedImmediate> answers = hasSite ? presenceTestSiteAnswers(context.origin(), key) : std::nullopt;
    if (!answers)
        return Condition(m_out.castToInt32(callStub(context, inById(question), { receiver.value() }, { .numberedImmediate = NumberedImmediate { mentionOfName(key) } }).first));
    bool asksShapeFirst = m_plan.slots().someLayoutWrittenInHasField(m_program.at(m_unit.function).module, key);
    auto askStub = [&] {
        return Condition(m_out.castToInt32(callStub(context, findsNameAtSite(question, asksShapeFirst), { receiver.value() }, { .numberedImmediate = *answers }).first));
    };
    const Operation* asked = context.valueID() ? &static_cast<const Operation&>(context.instruction()) : nullptr;
    if (asksShapeFirst || !asked || asked->opcode != Opcode::PropHas || !m_generator.usesLoopForms(context.block()))
        return askStub();
    LBasicBlock asksStub = newBlock();
    Join<Condition> join(m_out);
    {
        Machine::DeclaredAccessesSuspension suspension(m_out);
        LBasicBlock hasOtherKey = newBlock();
        LBasicBlock isFoundAbove = newBlock();
        LBasicBlock isFoundNowhere = newBlock();
        LValue answer = newestAnswerOfSite(receiver, asked->objectOperand(), *answers, asksStub, hasOtherKey);
        join.add(boolean(true));

        build().statistics().count(StatisticID::AbsencesAnsweredInline);
        m_out.appendTo(hasOtherKey);
        Word32 cagedValueKey = add(loadStructureID(unboxCell(receiver)), word32(SiteAnswers::bitsOf(SiteUse::Read, SiteAnswers::Form::ValueInWordOfCage)));
        branch(equal(Word32(m_out.castToInt32(answer)), cagedValueKey), { normalEdge(isFoundAbove), unlikelyEdge(asksStub) });
        m_out.appendTo(isFoundAbove);
        Word32 word(m_out.castToInt32(m_out.lShr(answer, 32)));
        branch(equal(word, loadFilledOnce32(AOT_INSTANCE_OFFSET(propertyCaches.wordOfCageThatHoldsUndefined))), { normalEdge(isFoundNowhere), unlikelyEdge(asksStub) });
        m_out.appendTo(isFoundNowhere);
        join.add(boolean(false));
    }

    m_out.appendTo(asksStub);
    join.add(askStub());
    return join.finish();
}

Boxed AccessEmitter::getFreeName(const ValueContext& value)
{
    RuntimeOperation operation = value->isForTypeOf() ? RuntimeOperation::GetFreeNameForTypeOf : RuntimeOperation::GetFreeName;
    Word32 name = identifierNumber(value->key());
    Pointer noSiteCache = pointerConstant(0);
    return callOperation<Boxed>(value, operation, { name, noSiteCache });
}

void AccessEmitter::putFreeName(const ValueContext& value)
{
    Boxed stored = in<Boxed>(value->valueOperand());
    Word32 name = identifierNumber(value->key());
    Pointer noSiteCache = pointerConstant(0);
    callVoidOperation(value, forMode(value->mode(), RuntimeOperation::SetFreeName, RuntimeOperation::SetFreeNameSloppy), { name, noSiteCache, stored });
}

CellPointer AccessEmitter::globalObject()
{
    return CellPointer(loadImmutablePointer(AOT_INSTANCE_OFFSET(globalObject)).value());
}

Pointer AccessEmitter::globalBindingAddress(PropertyKeyID key, OptionSet<GlobalBindingFlags> flags)
{
    return Pointer(m_out.valueOf(instanceWord(m_heaps.AOTInstance_globalBindings, m_mentions.globalBinding(key, flags))));
}

Pointer AccessEmitter::propertyAddressOrElse(Pointer record, bool seesScriptBindings, LBasicBlock miss)
{
    static_assert(OBJECT_OFFSETOF(GlobalBinding, lexicalBindingEpoch) == OBJECT_OFFSETOF(GlobalBinding, structureID) + sizeof(uint32_t));
    const Machine::AbstractHeap& records = m_heaps.AOTInstance_globalBindings;
    TypedPointer filedStructureID = m_out.address(records, record.value(), OBJECT_OFFSETOF(GlobalBinding, structureID));
    CellPointer global = globalObject();
    Word32 structureID = loadStructureID(global);
    if (seesScriptBindings) {
        Integer epoch = zeroExtended(loadField<Word32>(global, m_heaps.JSGlobalObject_globalLexicalBindingEpoch));
        Integer wideStructureID = zeroExtended(structureID);
        Integer current = bitOr(wideStructureID, Integer(m_out.shl(epoch.value(), 32)));
        branchUnless(equal(current, Integer(m_out.load64(filedStructureID))), miss);
    } else
        branchUnless(equal(structureID, Word32(m_out.load32(filedStructureID))), miss);

    Integer location(m_out.load64(m_out.address(records, record.value(), OBJECT_OFFSETOF(GlobalBinding, location))));
    Integer butterfly(loadField<Pointer>(global, m_heaps.JSObject_butterfly).value());
    Integer butterflyIfNegative = bitAnd(butterfly, Integer(m_out.aShr(location.value(), m_out.constInt32(63))));
    return Pointer(add(location, butterflyIfNegative).value());
}

TypedPointer AccessEmitter::globalBindingLocationOrElse(Pointer record, bool seesScriptBindings, LBasicBlock miss)
{
    return TypedPointer(m_heaps.properties.atAnyNumber(), propertyAddressOrElse(record, seesScriptBindings, miss).value());
}

void AccessEmitter::generateGetGlobalBinding(const ValueContext& value)
{
    PropertyKeyID key = value->key();
    bool seesScriptBindings = value->seesScriptBindings();
    bool isForTypeOf = value->isForTypeOf();
    OptionSet<GlobalBindingFlags> flags;
    if (seesScriptBindings)
        flags.add(GlobalBindingFlags::SeesScriptBindings);
    if (isForTypeOf)
        flags.add(GlobalBindingFlags::IsForTypeOf);

    if (!m_generator.takesOpenForm(value)) {
        NumberedImmediate binding { m_mentions.globalBinding(key, flags) };
        if (emitter<OperatorEmitter>().leaveOperandToInstanceOf(value, seesScriptBindings ? InstanceOfOperand::NameSeenByScripts : InstanceOfOperand::NameOfGlobalObject, binding)) {
            define(value, undefined());
            return;
        }
        GlobalBindingScope scope = seesScriptBindings ? GlobalBindingScope::ScriptsAndGlobalObject : GlobalBindingScope::GlobalObject;
        define(value, Boxed(callStub(value, AOT::getGlobalBinding(scope), { }, { .numberedImmediate = binding }).first));
        return;
    }

    Pointer record = globalBindingAddress(key, flags);
    LBasicBlock slowPath = newBlock();
    Join<Boxed> join(m_out);

    bool filesLocationAlone = m_plan.globalName(key).isStoredThroughBinding;
    if (isForTypeOf || !filesLocationAlone) {
        LBasicBlock constantCase = newBlock();
        LBasicBlock otherCase = newBlock();
        Boxed constant(m_out.load64(m_out.address(m_heaps.AOTInstance_globalBindings, record.value(), OBJECT_OFFSETOF(GlobalBinding, constant))));
        branch(isNotEmpty(constant), { normalEdge(constantCase), unlikelyEdge(otherCase) });

        m_out.appendTo(constantCase);
        join.add(constant);

        m_out.appendTo(otherCase);
    }

    Boxed read(m_out.load64(globalBindingLocationOrElse(record, seesScriptBindings, slowPath)));
    branchUnless(isNotEmpty(read), slowPath);
    join.add(read);

    m_out.appendTo(slowPath);
    RuntimeOperation operation = seesScriptBindings ? RuntimeOperation::GetGlobalBinding : RuntimeOperation::GetGlobalObjectBinding;
    join.add(callOperation<Boxed>(value, operation, { numberOf(m_mentions.globalBinding(key, flags)) }, { .isOnRarePath = true }));

    define(value, join.finish());
}

void AccessEmitter::generateGetGlobalPropertyAddress(const ValueContext& value)
{
    PropertyKeyID key = value->key();
    OptionSet<GlobalBindingFlags> flags;
    if (value->precedesStore())
        flags.add(GlobalBindingFlags::IsStore);
    if (value->seesScriptBindings())
        flags.add(GlobalBindingFlags::SeesScriptBindings);
    Pointer record = globalBindingAddress(key, flags);
    LBasicBlock isNotFiled = newBlock();
    Join<Pointer> join(m_out);
    if (!value->precedesStore() && !m_plan.globalName(key).isStoredThroughBinding) {
        LBasicBlock isConstant = newBlock();
        LBasicBlock isNotConstant = newBlock();
        Pointer constant(m_out.add(record.value(), m_out.constInt64(OBJECT_OFFSETOF(GlobalBinding, constant))));
        branch(isNotEmpty(Boxed(m_out.load64(m_out.address(m_heaps.AOTInstance_globalBindings, constant.value(), 0)))), { normalEdge(isConstant), normalEdge(isNotConstant) });

        m_out.appendTo(isConstant);
        join.add(constant);

        m_out.appendTo(isNotConstant);
    }
    Pointer address = propertyAddressOrElse(record, value->seesScriptBindings(), isNotFiled);
    branchUnless(isNotEmpty(Boxed(m_out.load64(TypedPointer(m_heaps.properties.atAnyNumber(), address.value())))), isNotFiled);
    join.add(address);

    m_out.appendTo(isNotFiled);
    join.add(pointerConstant(0));
    define(value, join.finish());
}

void AccessEmitter::generateIsPropertyFiled(const ValueContext& value)
{
    defineTest(value, Condition(m_out.notZero64(in<Pointer>(value->addressOperand()).value())));
}

void AccessEmitter::generateGetGlobalProperty(const ValueContext& value)
{
    define(value, Boxed(m_out.load64(TypedPointer(m_heaps.properties.atAnyNumber(), in<Pointer>(value->addressOperand()).value()))));
}

void AccessEmitter::generatePutGlobalBinding(const ValueContext& value)
{
    Boxed stored = in<Boxed>(value->valueOperand());
    m_out.store64(stored.value(), TypedPointer(m_heaps.properties.atAnyNumber(), in<Pointer>(value->addressOperand()).value()));
    barrierAfterStore(value, globalObject(), typeOf(value->valueOperand()));
}

void AccessEmitter::generateGetKeyedPropertyAddress(const ValueContext& value)
{
    Boxed object = in<Boxed>(value->objectOperand());
    if (PropertyKeyID key = value->key(); key && !value->findsOnlyFields()) {
        bool asksTableOfFields = m_plan.slots().someLayoutHasField(key);
        bool isFieldOfThisFile = asksTableOfFields && m_plan.slots().someLayoutWrittenInHasField(m_program.at(m_unit.function).module, key);
        std::optional<Pointer> site = isFieldOfThisFile ? std::nullopt : siteOfStore(value, key, SlotAccess::Store);
        auto search = [&](Pointer cache) {
            return callOperation<Integer>(value, RuntimeOperation::OwnNamedPropertyAddress, { object, identifierNumber(key), word32(value->fitsFieldTypes()), cache });
        };
        if (!site) {
            define(value, Pointer(search(Pointer(m_out.constInt64(0))).value()));
            return;
        }
        LBasicBlock searches = newBlock();
        bool asksTables = m_generator.isInLoop(value.block());
        LBasicBlock hasOtherStructure = asksTables ? newBlock() : searches;
        OperandJoin addresses(m_out, Integer::representation);
        checkStoreSiteInline(object, *site, { typeOf(value->objectOperand()), asksTableOfFields, hasOtherStructure, searches }, [&](CellPointer, Pointer place) {
            addresses.add(Integer(place.value()));
        });
        if (asksTables) {
            m_out.appendTo(hasOtherStructure);
            Integer filed(callStub(value, findReplaceableValue(), { object.value(), site->value() }, { .numberedImmediate = NumberedImmediate { mentionOfName(key) } }).first);
            branchUnless(isNotZero(filed), searches);
            addresses.add(filed);
        }
        m_out.appendTo(searches);
        addresses.add(search(*site));
        define(value, Pointer(addresses.finish().as<Integer>().value()));
        return;
    }
    Integer address = callLeaf<Integer>(RuntimeOperation::OwnPropertyAddress, { object, in<Boxed>(value->propertyOperand()), word32(value->findsOnlyFields()), word32(value->fitsFieldTypes()), word32(value->takesFunctions()) });
    define(value, Pointer(address.value()));
}

void AccessEmitter::generateFinishAccessThroughProxy(const ValueContext& value)
{
    Boxed stored = value->storedOperand() ? in<Boxed>(value->storedOperand()) : boxedConstant(JSValue());
    callVoidOperation(value, RuntimeOperation::FinishAccessThroughProxy, { in<Boxed>(value->targetOperand()), in<Boxed>(value->answerOperand()), stored, in<Boxed>(value->nameOperand()), word32(value->mode() == ECMAMode::Strict) });
}

void AccessEmitter::generateGetTemplateObjectRaw(const ValueContext& value)
{
    Pointer butterfly = loadField<Pointer>(in<CellPointer>(value->templateObjectOperand()), m_heaps.JSObject_butterfly);
    define(value, unboxCell(Boxed(m_out.load64(m_out.address(m_heaps.properties.atAnyNumber(), butterfly.value(), offsetInButterfly(firstOutOfLineOffset) * static_cast<ptrdiff_t>(sizeof(EncodedJSValue)))))));
}

void AccessEmitter::generateGetKeyedValueOrAbsent(const ValueContext& value)
{
    if (AOT::hasFlag(build().options().testing, Wire::TestingSwitch::UseGenericOwnKeyReads)) {
        define(value, getByVal(value, in<Boxed>(value->objectOperand()), in<Boxed>(value->propertyOperand())));
        return;
    }
    define(value, callLeaf<Boxed>(RuntimeOperation::OwnValueByKeyOrEmpty, { in<Boxed>(value->objectOperand()), in<Boxed>(value->propertyOperand()), word32(value->findsOnlyFields()) }));
}

std::array<const Machine::AbstractHeap*, 2> AccessEmitter::heapsOfKeyedProperty() const
{
    return { &m_heaps.entire(AbstractHeap::ObjectFields), &m_heaps.entire(AbstractHeap::NamedProperties) };
}

void AccessEmitter::generateGetKeyedProperty(const ValueContext& value)
{
    CellPointer object(in<Boxed>(value->objectOperand()).value());
    Pointer address = in<Pointer>(value->addressOperand());
    LValue property;
    if (operationAt(value->addressOperand()).findsOnlyFields())
        property = m_out.load64(TypedPointer(m_heaps.entire(AbstractHeap::ObjectFields), address.value()));
    else {
        Machine::PatchOperation* load = m_out.patchpoint(Machine::Int64);
        load->effects = Machine::Effects::none();
        m_heaps.decoratePatchpointRead(heapsOfKeyedProperty(), load);
        load->appendSomeRegister(address.value());
        load->setRunsWithoutFrame(true);
        load->setGenerator([](CCallHelpers& jit, const Machine::PatchParameters& params) {
            jit.load64(CCallHelpers::Address(params[1].gpr()), params[0].gpr());
        });
        property = load;
    }
    ensureStillAliveHere(object);
    define(value, Boxed(property));
}

void AccessEmitter::generatePutKeyedProperty(const ValueContext& value)
{
    Boxed object = in<Boxed>(value->objectOperand());
    Boxed stored = in<Boxed>(value->valueOperand());
    CellPointer cell(object.value());
    Pointer address = in<Pointer>(value->addressOperand());
    bool isField = operationAt(value->addressOperand()).findsOnlyFields();
    if (isField)
        m_out.store64(stored.value(), TypedPointer(m_heaps.entire(AbstractHeap::ObjectFields), address.value()));
    else {
        Machine::PatchOperation* store = m_out.patchpoint(Machine::Void);
        store->effects = Machine::Effects::none();
        m_heaps.decoratePatchpointWrite(heapsOfKeyedProperty(), store);
        store->appendSomeRegister(address.value());
        store->appendSomeRegister(stored.value());
        store->setRunsWithoutFrame(true);
        store->setGenerator([](CCallHelpers& jit, const Machine::PatchParameters& params) {
            jit.store64(params[1].gpr(), CCallHelpers::Address(params[0].gpr()));
        });
    }
    ensureStillAliveHere(cell);
    if (!typeOf(value->valueOperand()).hasCells())
        return;
    if (isField) {
        barrierAfterStore(value, cell, typeOf(value->valueOperand()));
        return;
    }
    LBasicBlock storesCell = newBlock();
    LBasicBlock ownsPlace = newBlock();
    LBasicBlock callsBarrier = newBlock();
    LBasicBlock done = newBlock();
    branch(isCell(stored), { normalEdge(storesCell), normalEdge(done) });
    m_out.appendTo(storesCell);
    branch(hasCellTypeIn(cell, { ObjectType, FinalObjectType }), { normalEdge(ownsPlace), normalEdge(callsBarrier) });
    m_out.appendTo(ownsPlace);
    branch(emitter<BarrierEmitter>().isBeyondBarrierThreshold(cell), { normalEdge(done), rareEdge(callsBarrier) });
    m_out.appendTo(callsBarrier);
    callLeaf(RuntimeOperation::BarrierAfterKeyedStore, std::array<Operand, 1> { object }, Representation::Void);
    m_out.jump(done);
    m_out.appendTo(done);
}

void AccessEmitter::generateSetGlobalBinding(const ValueContext& value)
{
    PropertyKeyID key = value->key();
    bool seesScriptBindings = value->seesScriptBindings();
    OptionSet<GlobalBindingFlags> flags { GlobalBindingFlags::IsStore };
    if (seesScriptBindings)
        flags.add(GlobalBindingFlags::SeesScriptBindings);
    Boxed stored = in<Boxed>(value->valueOperand());
    RuntimeOperation operation = seesScriptBindings
        ? forMode(value->mode(), RuntimeOperation::SetGlobalBinding, RuntimeOperation::SetGlobalBindingSloppy)
        : forMode(value->mode(), RuntimeOperation::SetGlobalObjectBinding, RuntimeOperation::SetGlobalObjectBindingSloppy);
    if (!comesAgain(value)) {
        m_out.noteForm(Form::Runtime, StateOfKnowledge::Always, Place::CodeThatRunsOnce);
        callVoidOperation(value, operation, { numberOf(m_mentions.globalBinding(key, flags)), stored });
        return;
    }
    if (!m_generator.takesOpenForm(value)) {
        GlobalBindingScope scope = seesScriptBindings ? GlobalBindingScope::ScriptsAndGlobalObject : GlobalBindingScope::GlobalObject;
        callStub(value, AOT::setGlobalBinding(scope, forMode(value->mode(), true, false)), { stored.value() }, { .numberedImmediate = NumberedImmediate { m_mentions.globalBinding(key, flags) } });
        return;
    }

    LBasicBlock slowPath = newBlock();
    LBasicBlock continuation = newBlock();
    m_out.store64(stored.value(), globalBindingLocationOrElse(globalBindingAddress(key, flags), seesScriptBindings, slowPath));
    barrierAfterStore(value, globalObject(), typeOf(value->valueOperand()));
    m_out.jump(continuation);

    m_out.appendTo(slowPath);
    callVoidOperation(value, operation, { numberOf(m_mentions.globalBinding(key, flags)), stored }, { .isOnRarePath = true });
    m_out.jump(continuation);

    m_out.appendTo(continuation);
}

void AccessEmitter::generateGetCachedGetter(const ValueContext& value)
{
    if (value.representation() == Representation::Void) {
        define(value, { });
        return;
    }
    PropertyKeyID key = value->key();
    if (isSymbolOfRealm(key)) {
        define(value, undefined());
        return;
    }

    CellPointer object = in<CellPointer>(value->objectOperand());
    auto verified = [&](Boxed getter) {
        if (verifiesFastPathAnswers(key)) [[unlikely]]
            verifyFastPathAnswer(value, Boxed(object.value()), key, getter, StubThatAnswers::CachedGetterByName, slowPathReadCount());
        return getter;
    };
    if (std::optional<NumberedImmediate> answers = answersOf(value, key, SiteUse::Call)) {
        define(value, verified(Boxed(callStub(value, getterAtSite(), { object.value() }, { .numberedImmediate = *answers }).first)));
        return;
    }
    define(value, verified(Boxed(callStub(value, getterByAccessTable(), { object.value() }, { .numberedImmediate = NumberedImmediate { mentionOfName(key) } }).first)));
}

void AccessEmitter::generateElemGetOrOwnAccessor(const ValueContext& value)
{
    CellPointer object = in<CellPointer>(value->objectOperand());
    Boxed property = in<Boxed>(value->propertyOperand());
    define(value, Boxed(callStub(value, getByValCachedOrOwnAccessor(), { object.value(), property.value() }).first));
}

void AccessEmitter::generateGetGetterOfAccessorOrUndefined(const ValueContext& value)
{
    if (value.representation() == Representation::Void) {
        define(value, { });
        return;
    }
    Boxed found = in<Boxed>(value->valueOperand());
    LBasicBlock isNoAccessor = newBlock();
    Join<Boxed> join(m_out);
    branchUnless(isCell(found), isNoAccessor);
    CellPointer cell = unboxCell(found);
    branchUnless(hasCellType(cell, GetterSetterType), isNoAccessor);
    join.add(Boxed(loadField<Pointer>(cell, m_heaps.GetterSetter_getter).value()));

    m_out.appendTo(isNoAccessor);
    join.add(undefined());
    define(value, join.finish());
}

std::optional<NumberedImmediate> AccessEmitter::answersOfReadOutOfRow(BlockID block, const Operation& read)
{
    PropertyKeyID key = read.key();
    if (m_program.nameClassOf(key) == NameClass::Length || isSymbolOfRealm(key) || m_code.at(block).frequency == BlockFrequency::Rare || !m_generator.isInLoop(block))
        return std::nullopt;
    if (liesInThread(m_unit.function))
        return std::nullopt;
    return answersAtSite(read.origin(), key, SiteUse::Read);
}

StubKey AccessEmitter::stubOfEffectFreeRead(const Operation& read)
{
    bool asksShapeFirst = !read.receiverIsNoObjectOfCompiledCode() && m_plan.slots().someLayoutWrittenInHasField(m_program.at(m_unit.function).module, read.key());
    return readAtSiteRunningNoCode(typeOf(read.objectOperand()).isObject() ? ReceiverAtSite::IsCell : ReceiverAtSite::IsAnyValue, asksShapeFirst);
}

void AccessEmitter::readGroupOffOneValue(const ValueContext& value, Boxed receiver, bool mayBeNoCell, NumberedImmediate answersOfFirst)
{
    Vector<ReadOfGroup, ReadGroupCache::numberOfPlaces> reads;
    reads.append({ value.id(), value->key(), answersOfFirst });
    {
        BlockID block = value.block();
        std::span<const ValueID> behind = m_code.valuesOf(block);
        behind = behind.subspan(std::ranges::find(behind, value.id()) - behind.begin() + 1);
        for (bool goesOn = true; goesOn;) {
            for (ValueID id : behind) {
                const Operation& operation = m_code.at(id);
                if (operation.opcode == Opcode::GetValueAtSiteOrAbsent && !operation.mayHoldLength() && operation.objectOperand() == value->objectOperand()) {
                    if (std::optional<NumberedImmediate> answers = answersOfReadOutOfRow(block, operation))
                        reads.append({ id, operation.key(), *answers });
                    if (reads.size() == ReadGroupCache::numberOfPlaces) {
                        goesOn = false;
                        break;
                    }
                    continue;
                }
                EffectTraits traits = operation.traits();
                if (traits.mayWriteObservableState() || traits.mayRunOtherCode() || traits.mayCollect()) {
                    goesOn = false;
                    break;
                }
            }
            if (!goesOn)
                break;
            const Terminator& terminator = m_code.at(block).terminator;
            BlockID next;
            for (uint32_t index = 0; index < terminator.numberOfEdges; ++index) {
                BlockID to = m_code.at(terminator.edge(index)).to;
                if (m_code.at(to).frequency != BlockFrequency::Normal)
                    continue;
                if (next || m_generator.onlyPredecessorOf(to) != block) {
                    goesOn = false;
                    break;
                }
                next = to;
            }
            if (!goesOn || !next)
                break;
            block = next;
            behind = m_code.valuesOf(block);
        }
    }

    build().statistics().count(StatisticID::SiteCachedReadGroupsEmitted);
    build().statistics().count(StatisticID::SiteCachedReadsGrouped, reads.size());
    std::array<uint32_t, 3> keyOfGroup { m_unit.function.bits(), static_cast<uint32_t>(m_unit.role) | static_cast<uint32_t>(m_unit.numberOfBoundArguments) << 8 | static_cast<uint32_t>(m_unit.highByteOfPlaceOfSelector) << 24, value.id().index() };
    CellPointer record = unboxCell(receiver);
    LBasicBlock asksRows = newBlock();
    LBasicBlock byStubs = newBlock();
    LBasicBlock byStubsOrByStructure = newBlock();
    LBasicBlock hasNoRow = newBlock();
    LBasicBlock waits = newBlock();
    LBasicBlock claims = newBlock();
    LBasicBlock found = newBlock();
    LBasicBlock done = newBlock();
    auto askStubThenStructure = [&](const ReadOfGroup& read) {
        LBasicBlock readsByStructure = newBlock();
        Join<Boxed> join(m_out);
        Boxed answer(callStub(value, stubOfEffectFreeRead(m_code.at(read.id)), { receiver.value() }, { .numberedImmediate = read.answers }).first);
        branchUnless(isNotEmpty(answer), readsByStructure);
        join.add(answer);
        m_out.appendTo(readsByStructure);
        join.add(callOperation<Boxed>(value, RuntimeOperation::ReadRunningNoCode, { record, identifierNumber(read.key) }, { .isOnRarePath = true }));
        return join.finish();
    };
    Vector<ValueFromBlock, ReadGroupCache::numberOfRows + 1> rows;
    Vector<Vector<ValueFromBlock, 3>, ReadGroupCache::numberOfPlaces> results(reads.size());
    const bool mayLieInExtension = !!m_cellCageBase;
    {
        Machine::DeclaredAccessesSuspension suspension(m_out);
        TypedPointer wordOfGroup = instanceWord(m_heaps.entire(AbstractHeap::RuntimeState), m_mentions.add(NumberSpace::ReadGroupCache, keyOfGroup));
        LValue group = m_out.valueOf(wordOfGroup);
        LBasicBlock allLieInCell = newBlock();
        LBasicBlock someMayBeElsewhere = newBlock();
        if (mayBeNoCell)
            branchUnless(isCell(receiver), byStubs);
        Word32 structure = loadStructureID(record);
        constexpr ptrdiff_t offsetOfFirstRow = OBJECT_OFFSETOF(ReadGroupCache, rows);
        Word32 structureThatHoldsAll(m_out.load32(m_out.address(m_heaps.root, group, offsetOfFirstRow + OBJECT_OFFSETOF(ReadGroupCache::Row, structureThatHoldsAll))));
        m_out.branch(equal(structureThatHoldsAll, structure).value(), normalEdge(allLieInCell), unlikelyEdge(someMayBeElsewhere));

        m_out.appendTo(allLieInCell);
        for (unsigned index = 0; index < reads.size(); ++index) {
            LValue place = m_out.load16SignExt32(m_out.address(m_heaps.root, group, offsetOfFirstRow + OBJECT_OFFSETOF(ReadGroupCache::Row, places) + index * sizeof(int16_t)));
            results[index].append(m_out.anchor(m_out.load64(m_out.baseIndex(m_heaps.root, record.value(), m_out.zeroExtPtr(place), ScaleEight))));
        }
        m_out.jump(done);

        m_out.appendTo(someMayBeElsewhere);
        LBasicBlock hasNotGivenUp = newBlock();
        m_out.branch(m_out.isZero32(m_out.load32(wordOfGroup)), normalEdge(hasNotGivenUp), unlikelyEdge(byStubs));
        m_out.appendTo(hasNotGivenUp);
        auto loadHolderInExtension = [&](LValue word) {
            LBasicBlock hasExtension = newBlock();
            LValue reference = m_out.load32(m_out.baseIndex(m_heaps.root, record.value(), m_out.zeroExtPtr(word), ScaleFour));
            m_out.branch(m_out.isZero32(reference), rareEdge(byStubsOrByStructure), normalEdge(hasExtension));
            m_out.appendTo(hasExtension);
            return decode(NarrowReference(reference));
        };
        LValue mark = m_out.bitXor(m_out.load32(m_out.address(m_heaps.root, group, offsetOfFirstRow + OBJECT_OFFSETOF(ReadGroupCache::Row, structureThatHoldsAll))), structure.value());
        LBasicBlock someLieInStorage = newBlock();
        LBasicBlock noneIsProvenInStorage = newBlock();
        m_out.branch(equal(Word32(mark), word32(ReadGroupCache::markOfStorage)).value(), normalEdge(someLieInStorage), normalEdge(noneIsProvenInStorage));

        m_out.appendTo(someLieInStorage);
        {
            LValue storage = loadField<Pointer>(record, m_heaps.JSObject_butterfly).value();
            for (unsigned index = 0; index < reads.size(); ++index) {
                LValue place = signExtended(Word32(m_out.load16SignExt32(m_out.address(m_heaps.root, group, offsetOfFirstRow + OBJECT_OFFSETOF(ReadGroupCache::Row, places) + index * sizeof(int16_t))))).value();
                LValue holder = reads.size() == 1 ? storage : m_out.select(m_out.lessThan(place, m_out.int64Zero), storage, record.value());
                results[index].append(m_out.anchor(m_out.load64(m_out.baseIndex(m_heaps.root, holder, place, ScaleEight))));
            }
            m_out.jump(done);
        }

        m_out.appendTo(noneIsProvenInStorage);
        if (mayLieInExtension) {
            LBasicBlock allLieInExtension = newBlock();
            LBasicBlock someLieOutside = newBlock();
            m_out.branch(equal(Word32(mark), word32(ReadGroupCache::markOfExtension)).value(), normalEdge(allLieInExtension), normalEdge(someLieOutside));
            m_out.appendTo(allLieInExtension);
            CellPointer extension = loadHolderInExtension(m_out.load16ZeroExt32(m_out.address(m_heaps.root, group, offsetOfFirstRow + OBJECT_OFFSETOF(ReadGroupCache::Row, wordOfExtension))));
            for (unsigned index = 0; index < reads.size(); ++index) {
                LValue place = m_out.load16SignExt32(m_out.address(m_heaps.root, group, offsetOfFirstRow + OBJECT_OFFSETOF(ReadGroupCache::Row, places) + index * sizeof(int16_t)));
                results[index].append(m_out.anchor(m_out.load64(m_out.baseIndex(m_heaps.root, extension.value(), m_out.zeroExtPtr(place), ScaleEight))));
            }
            m_out.jump(done);
            m_out.appendTo(someLieOutside);
        }
        m_out.jump(asksRows);

        m_out.appendTo(asksRows);
        for (unsigned index = 0; index < ReadGroupCache::numberOfRows; ++index) {
            ptrdiff_t offset = OBJECT_OFFSETOF(ReadGroupCache, rows) + index * sizeof(ReadGroupCache::Row);
            LBasicBlock isRow = newBlock();
            LBasicBlock isOther = index + 1 < ReadGroupCache::numberOfRows ? newBlock() : hasNoRow;
            m_out.branch(equal(Word32(m_out.load32(m_out.address(m_heaps.root, group, offset + OBJECT_OFFSETOF(ReadGroupCache::Row, structure)))), structure).value(), normalEdge(isRow), unlikelyEdge(isOther));
            m_out.appendTo(isRow);
            rows.append(m_out.anchor(m_out.add(group, m_out.constIntPtr(offset))));
            m_out.jump(found);
            m_out.appendTo(isOther);
        }
        LValue missesLeft = m_out.load32(m_out.address(m_heaps.root, group, OBJECT_OFFSETOF(ReadGroupCache, missesLeft)));
        m_out.branch(m_out.isZero32(missesLeft), rareEdge(claims), normalEdge(waits));

        m_out.appendTo(waits);
        m_out.store32(m_out.sub(missesLeft, m_out.int32One), m_out.address(m_heaps.root, group, OBJECT_OFFSETOF(ReadGroupCache, missesLeft)));
        m_out.jump(byStubsOrByStructure);

        m_out.appendTo(claims);
        for (unsigned index = 0; index < reads.size(); ++index)
            m_out.store32(identifierNumber(reads[index].key).value(), m_out.address(m_heaps.root, group, OBJECT_OFFSETOF(ReadGroupCache, keys) + index * sizeof(uint32_t)));
        callVoidOperation(value, RuntimeOperation::ClaimReadGroupRow, { receiver, Pointer(group), word32(static_cast<int32_t>(reads.size())) }, { .isOnRarePath = true });
        m_out.jump(byStubsOrByStructure);

        m_out.appendTo(found);
        LValue row = m_out.phi(Machine::Int64, rows);
        LValue holder = record.value();
        if (mayLieInExtension) {
            LBasicBlock liesInExtension = newBlock();
            LBasicBlock knowsHolder = newBlock();
            LValue word = m_out.load16ZeroExt32(m_out.address(m_heaps.root, row, OBJECT_OFFSETOF(ReadGroupCache::Row, wordOfExtension)));
            Vector<ValueFromBlock, 2> holders;
            holders.append(m_out.anchor(record.value()));
            m_out.branch(m_out.isZero32(word), normalEdge(knowsHolder), normalEdge(liesInExtension));
            m_out.appendTo(liesInExtension);
            holders.append(m_out.anchor(loadHolderInExtension(word).value()));
            m_out.jump(knowsHolder);
            m_out.appendTo(knowsHolder);
            holder = m_out.phi(Machine::Int64, holders);
        }
        CellPointer holderOfPlaces(holder);
        LValue storage = loadField<Pointer>(holderOfPlaces, m_heaps.JSObject_butterfly).value();
        Vector<LValue, ReadGroupCache::numberOfPlaces> outOfRow;
        static_assert(!ReadGroupCache::placeIsAskedOfStub);
        for (unsigned index = 0; index < reads.size(); ++index) {
            LBasicBlock isKnown = newBlock();
            LBasicBlock asksStub = newBlock();
            Join<Boxed> join(m_out);
            LValue place = signExtended(Word32(m_out.load16SignExt32(m_out.address(m_heaps.root, row, OBJECT_OFFSETOF(ReadGroupCache::Row, places) + index * sizeof(int16_t))))).value();
            m_out.branch(m_out.notZero64(place), normalEdge(isKnown), unlikelyEdge(asksStub));

            m_out.appendTo(isKnown);
            LValue base = m_out.select(m_out.lessThan(place, m_out.int64Zero), storage, holderOfPlaces.value());
            join.add(Boxed(m_out.load64(m_out.baseIndex(m_heaps.root, base, place, ScaleEight))));

            m_out.appendTo(asksStub);
            join.add(Boxed(callStub(value, stubOfEffectFreeRead(m_code.at(reads[index].id)), { receiver.value() }, { .numberedImmediate = reads[index].answers }).first));
            outOfRow.append(join.finish().value());
        }
        for (unsigned index = 0; index < reads.size(); ++index)
            results[index].append(m_out.anchor(outOfRow[index]));
        m_out.jump(done);
    }

    m_out.appendTo(byStubsOrByStructure);
    {
        Vector<LValue, ReadGroupCache::numberOfPlaces> ofStubsOrByStructure;
        for (const ReadOfGroup& read : reads)
            ofStubsOrByStructure.append(askStubThenStructure(read).value());
        for (unsigned index = 0; index < reads.size(); ++index)
            results[index].append(m_out.anchor(ofStubsOrByStructure[index]));
        m_out.jump(done);
    }

    m_out.appendTo(byStubs);
    Vector<LValue, ReadGroupCache::numberOfPlaces> ofStubs;
    for (const ReadOfGroup& read : reads)
        ofStubs.append(callStub(value, stubOfEffectFreeRead(m_code.at(read.id)), { receiver.value() }, { .numberedImmediate = read.answers }).first);
    for (unsigned index = 0; index < reads.size(); ++index)
        results[index].append(m_out.anchor(ofStubs[index]));
    m_out.jump(done);

    m_out.appendTo(done);
    for (unsigned index = 0; index < reads.size(); ++index) {
        LValue read = m_out.phi(Machine::Int64, results[index]);
        if (index)
            m_valuesReadInFront.set(reads[index].id.index(), read);
        else
            define(value, Boxed(read));
    }
}

void AccessEmitter::generateGetValueAtSiteOrAbsent(const ValueContext& value)
{
    if (LValue readInFront = m_valuesReadInFront.take(value.id().index())) {
        build().statistics().count(StatisticID::SiteCachedReadsEmitted);
        define(value, Boxed(readInFront));
        return;
    }
    PropertyKeyID key = value->key();
    bool hasNoAnswers = isSymbolOfRealm(key) || m_code.at(value.block()).frequency == BlockFrequency::Rare;
    std::optional<NumberedImmediate> answers = hasNoAnswers ? std::nullopt : answersAtSite(value, key, SiteUse::Read);
    if (!answers) {
        define(value, boxedConstant(JSValue()));
        return;
    }
    ReceiverAtSite receiverIs = typeOf(value->objectOperand()).isObject() ? ReceiverAtSite::IsCell : ReceiverAtSite::IsAnyValue;
    bool asksShapeFirst = !value->receiverIsNoObjectOfCompiledCode() && m_plan.slots().someLayoutWrittenInHasField(m_program.at(m_unit.function).module, key);
    build().statistics().count(StatisticID::SiteCachedReadsEmitted);
    Boxed receiver = in<Boxed>(value->objectOperand());
    if (isErrorMessageRead(value)) {
        LBasicBlock asksStub = newBlock();
        Join<Boxed> join(m_out);
        join.add(loadErrorMessage(receiver, value->objectOperand(), asksStub));

        m_out.appendTo(asksStub);
        join.add(Boxed(callStub(value, readAtSiteRunningNoCode(receiverIs, asksShapeFirst), { receiver.value() }, { .numberedImmediate = *answers, .isOnRarePath = true }).first));
        define(value, join.finish());
        return;
    }
    if (!value->mayHoldLength() && answersOfReadOutOfRow(value.block(), value.value())) {
        readGroupOffOneValue(value, receiver, receiverIs == ReceiverAtSite::IsAnyValue, *answers);
        return;
    }
    auto readAtSite = [&]() -> Boxed {
        Boxed atSite(callStub(value, readAtSiteRunningNoCode(receiverIs, asksShapeFirst), { receiver.value() }, { .numberedImmediate = *answers }).first);
        bool asksStructure = !m_generator.isInLoop(value.block());
        if (!asksStructure)
            return atSite;
        LBasicBlock missed = newBlock();
        LBasicBlock hasStructure = newBlock();
        LBasicBlock isPrimitive = newBlock();
        Join<Boxed> read(m_out);
        branchUnless(isNotEmpty(atSite), missed);
        read.add(atSite);

        m_out.appendTo(missed);
        branch(isCell(receiver), { normalEdge(hasStructure), rareEdge(isPrimitive) });

        m_out.appendTo(isPrimitive);
        read.add(boxedConstant(JSValue()));

        m_out.appendTo(hasStructure);
        read.add(callOperation<Boxed>(value, RuntimeOperation::ReadBeforeLoop, { unboxCell(receiver), identifierNumber(key) }, { .isOnRarePath = true }));
        return read.finish();
    };
    if (!value->mayHoldLength()) {
        define(value, readAtSite());
        return;
    }
    LBasicBlock isCellOfAnyKind = newBlock();
    LBasicBlock hasOwnLength = newBlock();
    LBasicBlock hasProperty = newBlock();
    Join<Boxed> join(m_out);
    branch(isCell(receiver), { normalEdge(isCellOfAnyKind), normalEdge(hasProperty) });
    m_out.appendTo(isCellOfAnyKind);
    branch(hasCellTypeIn(unboxCell(receiver), { ObjectType, FinalObjectType }), { normalEdge(hasProperty), normalEdge(hasOwnLength) });
    m_out.appendTo(hasOwnLength);
    Boxed length(callStub(value, getLengthRunningNoCode(), { receiver.value() }, { .numberedImmediate = NumberedImmediate { mentionOfName(key) } }).first);
    branchUnless(isNotEmpty(length), hasProperty);
    join.add(length);
    m_out.appendTo(hasProperty);
    join.add(readAtSite());
    define(value, join.finish());
}

void AccessEmitter::generateHasPropertyOrAbsent(const ValueContext& value)
{
    PropertyKeyID key = value->key();
    if (isSymbolOfRealm(key) || m_code.at(value.block()).frequency == BlockFrequency::Rare) {
        define(value, boxedConstant(JSValue()));
        return;
    }
    Boxed receiver = in<Boxed>(value->objectOperand());
    LBasicBlock hasStructure = newBlock();
    LBasicBlock isPrimitive = newBlock();
    Join<Boxed> join(m_out);
    branch(isCell(receiver), { normalEdge(hasStructure), rareEdge(isPrimitive) });

    m_out.appendTo(isPrimitive);
    join.add(boxedConstant(JSValue()));

    m_out.appendTo(hasStructure);
    join.add(callOperation<Boxed>(value, RuntimeOperation::HasBeforeLoop, { unboxCell(receiver), identifierNumber(key) }));
    define(value, join.finish());
}

void AccessEmitter::generateGetOwnValueUnderKeyOrAbsent(const ValueContext& value)
{
    if (AOT::hasFlag(build().options().testing, Wire::TestingSwitch::UseGenericOwnKeyReads)) {
        define(value, getByVal(value, in<Boxed>(value->objectOperand()), in<Boxed>(value->propertyOperand())));
        return;
    }
    define(value, callLeaf<Boxed>(RuntimeOperation::OwnValueUnderKeyOrEmpty, { in<Boxed>(value->objectOperand()), in<Boxed>(value->propertyOperand()) }));
}

void AccessEmitter::generateGetValueUnderKeyOrAbsent(const ValueContext& value)
{
    if (AOT::hasFlag(build().options().testing, Wire::TestingSwitch::UseGenericOwnKeyReads)) {
        define(value, getByVal(value, in<Boxed>(value->objectOperand()), in<Boxed>(value->propertyOperand())));
        return;
    }
    define(value, Boxed(callStub(value, getByValRunningNoCode(), { in<Boxed>(value->objectOperand()).value(), in<Boxed>(value->propertyOperand()).value() }).first));
}

bool AccessEmitter::lookVouchesForPropertyOfHeir(const ValueContext& value) const
{
    const Layout& classLayout = m_program.at(value->layout());
    return m_plan.hasClassRecord(value->layout()) && isKnownByStructureOfClass(classLayout) && value->field() < classLayout.fields.size() && isDecidedByClassStructure(classLayout.fields[value->field()]);
}

TypedPointer AccessEmitter::locationOfPropertyOfHeir(const ValueContext& value, CellPointer object)
{
    Word32 learned(m_out.load32(emitter<GuardEmitter>().subclassStructuresWord(m_heaps.AOTInstance_fieldsFilledOnce, value->layout(), StructuresOfHeir::offsetOfDisplacementOfField(value->field()))));
    if (AOT::hasFlag(build().options().testing, Wire::TestingSwitch::CheckProofsAtRunTime))
        emitter<ThrowEmitter>().requireOrCrash(value, notEqual(learned, word32(0)), std::nullopt, ProofSource::RememberedFact);
    Pointer butterfly = loadField<Pointer>(object, m_heaps.JSObject_butterfly);
    return m_out.baseIndex(m_heaps.entire(AbstractHeap::NamedProperties), butterfly.value(), signExtended(learned).value(), ScaleOne);
}

void AccessEmitter::generateGetPropertyOfHeir(const ValueContext& value)
{
    if (!lookVouchesForPropertyOfHeir(value)) {
        fail(value, InternalErrorKind::SlotAccessNotPlaced);
        define(value, undefined());
        return;
    }
    CellPointer object = in<CellPointer>(value->objectOperand());
    Boxed property(m_out.load64(locationOfPropertyOfHeir(value, object)));
    ensureStillAliveHere(object);
    define(value, property);
}

void AccessEmitter::generatePutPropertyOfHeir(const ValueContext& value)
{
    if (!lookVouchesForPropertyOfHeir(value)) {
        fail(value, InternalErrorKind::SlotAccessNotPlaced);
        return;
    }
    CellPointer object = in<CellPointer>(value->objectOperand());
    Boxed stored = in<Boxed>(value->valueOperand());
    m_out.store64(stored.value(), locationOfPropertyOfHeir(value, object));
    ensureStillAliveHere(object);
    barrierAfterStore(value, object, typeOf(value->valueOperand()));
}

bool AccessEmitter::isReadOfMethodCalledNext(const ValueContext& read)
{
    std::span<const ValueID> behind = read.operationsBehind();
    ValueID onlyUser = behind.empty() ? ValueID() : m_code.onlyUserOf(read.id());
    if (!onlyUser)
        return false;
    auto inFrontOfBox = [&](ValueID id) {
        const Operation& operation = operationAt(id);
        return operation.opcode == Opcode::Box ? operation.operand(0) : id;
    };
    const Operation& call = operationAt(onlyUser);
    if (call.opcode != Opcode::Call || call.calleeOperand() != read.id() || !call.thisValueOperand() || inFrontOfBox(call.thisValueOperand()) != inFrontOfBox(read->objectOperand()) || call.calleeKey() != read->key())
        return false;
    if (emitter<CallEmitter>().mayForwardCallToReceiver(call))
        return false;
    const Terminator& terminator = m_code.at(read.block()).terminator;
    bool endsUnit = terminator.kind == TerminatorKind::ReturnCall && terminator.callOperand() == onlyUser;
    if (call.isTailCall && (endsUnit || (!call.inlineFrame && !findPatternLeftToCall(read))))
        return false;
    if (isSymbolOfRealm(read->key()) || !typeOf(read.id()).hasObjects())
        return false;
    auto standsUnseenInBetween = [&](const Operation& operation) {
        return operation.opcode == Opcode::Box || operation.opcode == Opcode::NewClosure || operation.opcode == Opcode::NewRegExp || operation.traits().isPure();
    };
    while (!behind.empty() && behind.front() != onlyUser && standsUnseenInBetween(operationAt(behind.front())))
        behind = behind.subspan(1);
    if (behind.empty() || behind.front() != onlyUser)
        return false;
    std::span<const ValueID> arguments = call.argumentsOperands();
    return arguments.size() <= numberOfArgumentGPRs && !std::ranges::contains(arguments, read.id());
}

ValueID AccessEmitter::findPatternLeftToCall(const ValueContext& read)
{
    ValueID callID = m_code.onlyUserOf(read.id());
    const Operation& call = operationAt(callID);
    std::span<const ValueID> arguments = call.argumentsOperands();
    std::optional<BuiltinBoxedStub> boxedCall = call.methodOfReceiverAsMade() ? m_program.builtins().boxedCallStubOf(call.methodOfReceiverAsMade()) : std::nullopt;
    if (!boxedCall || arguments.empty() || !leavesPatternAsMade(*boxedCall, static_cast<unsigned>(arguments.size())))
        return { };
    ValueID boxed = arguments[0];
    if (operationAt(boxed).opcode != Opcode::Box || m_code.onlyUserOf(boxed) != callID)
        return { };
    ValueID made = operationAt(boxed).operand(0);
    const Operation& pattern = operationAt(made);
    if (pattern.opcode != Opcode::NewRegExp || pattern.regExpFlags().contains(RegExpFlags::Sticky) || m_code.onlyUserOf(made) != boxed)
        return { };
    if (!std::ranges::contains(read.operationsBehind(), made) || !emitter<BuiltinCallEmitter>().canShareRegExpLiteral(pattern))
        return { };
    return made;
}
void AccessEmitter::returnWhatMethodOfBuiltinClassGives(const ValueContext& read)
{
    const Terminator& terminator = m_code.at(read.block()).terminator;
    if (terminator.kind != TerminatorKind::ReturnCall || terminator.callOperand() != m_code.onlyUserOf(read.id()) || isSymbolOfRealm(read->key()))
        return;
    const Operation& call = operationAt(terminator.callOperand());
    auto inFrontOfBox = [&](ValueID id) {
        const Operation& operation = operationAt(id);
        return operation.opcode == Opcode::Box ? operation.operand(0) : id;
    };
    if (call.opcode != Opcode::Call || !call.methodOfReceiverAsMade() || call.calleeOperand() != read.id() || !call.thisValueOperand() || inFrontOfBox(call.thisValueOperand()) != inFrontOfBox(read->objectOperand()))
        return;
    ValueID pattern = findPatternLeftToCall(read);
    if (call.methodIsNamedByKeyAlone() && !pattern)
        return;
    const unsigned count = static_cast<unsigned>(call.argumentsOperands().size());
    std::optional<BuiltinBoxedStub> boxedCall = m_program.builtins().boxedCallStubOf(call.methodOfReceiverAsMade());
    if (!boxedCall || !servesCallsByName(*boxedCall, count))
        return;
    ArgumentLocations own = m_plan.conventions().locationsOf(m_program, m_unit);
    if (own.results.size() != 1 || own.results[0].representation != Representation::JSValue || m_plan.conventions().spillsArgumentRegisters(m_unit))
        return;

    std::span<const ValueID> behind = read.operationsBehind();
    for (ValueID id : behind) {
        const Operation& operation = operationAt(id);
        if (operation.opcode != Opcode::Box && id != pattern && !operation.traits().isPure())
            return;
    }
    Vector<LValue, 8> arguments;
    auto isMadeAlready = [&](ValueID id) {
        return !operationAt(id).block() || generator().wordOf(id);
    };
    for (ValueID argument : call.argumentsOperands()) {
        ValueID made = inFrontOfBox(argument);
        if (made != pattern && (std::ranges::contains(behind, made) || made == read.id() || !isMadeAlready(made)))
            return;
    }
    if (!isMadeAlready(inFrontOfBox(read->objectOperand())))
        return;
    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    auto boxed = [&](ValueID id) {
        ValueID made = inFrontOfBox(id);
        if (std::optional<Boxed> word = generator().findBoxed(made))
            return word->value();
        return conversions.box(read, get(made), typeOf(made)).value();
    };
    for (ValueID argument : call.argumentsOperands())
        arguments.append(inFrontOfBox(argument) == pattern ? conversions.box(read, emitter<BuiltinCallEmitter>().sharedRegExpLiteral(read, operationAt(pattern)), typeOf(pattern)).value() : boxed(argument));

    CallEmitter& calls = emitter<CallEmitter>();
    ConventionCall conventionCall {
        .stub = tryCallMethodOfBuiltinClass(*boxedCall, count),
        .thisValue = boxed(read->objectOperand()),
        .arguments = arguments.span(),
    };
    Boxed answer = calls.callByConvention(read, conventionCall, { .origin = call.origin() });
    LBasicBlock returns = newBlock();
    LBasicBlock goesOn = newBlock();
    branch(isNotEmpty(answer), { normalEdge(returns), rareEdge(goesOn) });
    m_out.appendTo(returns);
    Operand operand = answer;
    calls.returnValues(singleElementSpan(operand));
    m_out.appendTo(goesOn);
}

bool AccessEmitter::tryCallMethodByName(const ValueContext& call)
{
    if (!m_readLeftToCall)
        return false;
    ReadLeftToCall read = *std::exchange(m_readLeftToCall, std::nullopt);
    RELEASE_ASSERT(read.read == call->calleeOperand() && !call.returnCall());
    CallEmitter& calls = emitter<CallEmitter>();
    const uint8_t lentArguments = argumentsLentBy(m_code, call.value());
    if (!read.pattern && !lentArguments && m_code.at(call.block()).frequency != BlockFrequency::Rare && calls.tryCallThroughMethodTable(call, read.key))
        return true;
    Vector<LValue, 8> arguments;
    for (ValueID argument : call->argumentsOperands())
        arguments.append(in<Boxed>(argument).value());
    const unsigned count = static_cast<unsigned>(arguments.size());
    std::optional<BuiltinBoxedStub> boxedCall = call->methodOfReceiverAsMade() ? m_program.builtins().boxedCallStubOf(call->methodOfReceiverAsMade()) : std::nullopt;
    bool goesByClass = boxedCall && servesCallsByName(*boxedCall, count);
    std::optional<CompiledMethodOfArray> compiledMethod = !goesByClass && call->methodOfReceiverAsMade() && count <= maxArgumentCountOfCompiledMethodOfArray ? compiledMethodOfArray(call->methodOfReceiverAsMade()) : std::nullopt;
    std::optional<NumberedImmediate> answers = (!goesByClass || call->methodIsNamedByKeyAlone()) && !compiledMethod ? answersOf(call, read.key, SiteUse::Call) : std::nullopt;
    bool standsBehindFailedLookInLoop = goesByClass && m_code.at(call.block()).frequency == BlockFrequency::Rare && m_generator.paysForCodeInLine(call.block());
    if (!answers && standsBehindFailedLookInLoop && !read.pattern && !isSymbolOfRealm(read.key))
        answers = answersAtSite(call, read.key, SiteUse::Call);
    RELEASE_ASSERT((!read.pattern || goesByClass) && (!lentArguments || !goesByClass));
    const bool callsFunctionOfBuiltin = boxedCall && !goesByClass && !compiledMethod && answers && !lentArguments && !cellTypesOfReceiversOf(*boxedCall) && exists(callFunctionOfBuiltinAtSite(*boxedCall, count))
        && m_program.builtins().at(m_program.builtins().at(call->methodOfReceiverAsMade()).holder).kind != BuiltinKind::Prototype;
    StubKey byClass = !goesByClass ? StubKey() : read.pattern ? (answers ? callMethodOfBuiltinClassAtSiteWithSharedPattern(*boxedCall, count) : callMethodOfBuiltinClassWithSharedPattern(*boxedCall, count))
        : answers ? callMethodOfBuiltinClassAtSite(*boxedCall, count)
        : calls.isProvenReceiverOf(*boxedCall, typeOf(call->thisValueOperand())) ? callMethodOfProvenBuiltinClass(*boxedCall, count) : callMethodOfBuiltinClass(*boxedCall, count);
    ConventionCall conventionCall {
        .stub = goesByClass ? byClass : compiledMethod ? callCompiledMethodOfArray(*compiledMethod, count, lentArguments) : callsFunctionOfBuiltin ? callFunctionOfBuiltinAtSite(*boxedCall, count) : answers ? callMethodAtSite(count, lentArguments) : callMethodById(count, lentArguments),
        .thisValue = in<Boxed>(call->thisValueOperand()).value(),
        .arguments = arguments.span(),
    };
    Vector<LValue, 4> operandsOfTable;
    if (std::optional<TableMethod> method = goesByClass && !read.pattern && !answers ? tableMethodOf(*boxedCall) : std::nullopt) {
        const Type& key = typeOf(call->argumentsOperands()[0]);
        KeyComparison comparison = KeyComparison::AnyValue;
        if (key.isJavaScriptValue() && !key.isNever()) {
            if (key.primitives & Primitive::AnyString)
                comparison = key.hasOnlyPrimitives(Primitive::AnyString | Primitive::UndefinedOrNull) ? KeyComparison::String : KeyComparison::AnyValue;
            else if (!(key.primitives & (Primitive::BigInt | Primitive::Number)))
                comparison = KeyComparison::Bits;
        }
        conventionCall.stub = calls.isProvenReceiverOf(*boxedCall, typeOf(call->thisValueOperand())) ? callMethodOfProvenTable(*method, comparison) : callMethodOfTable(*method, comparison);
        operandsOfTable.append(conventionCall.thisValue);
        operandsOfTable.appendVector(arguments);
        conventionCall.stubOperands = operandsOfTable.span();
        conventionCall.thisValue = nullptr;
        conventionCall.arguments = { };
        build().statistics().count(StatisticID::CollectionMethodsCalledByStub);
    }
    if (goesByClass && !read.pattern && *boxedCall == BuiltinBoxedStub::ArraySlice && !count && generator().isStoredIntoByName(call.id())) {
        LBasicBlock closed = newBlock();
        OperandJoin answersMeet(m_out, Representation::JSValue);
        answersMeet.add(emitter<ArrayBuiltinEmitter>().copyWithPropertyRoomInFrontOfClosedCall(call, call->thisValueOperand(), closed));
        m_out.appendTo(closed);
        answersMeet.add(calls.callByConvention(call, conventionCall, {
            .numberedImmediate = answers ? *answers : NumberedImmediate { mentionOfName(read.key) },
            .readBeforeCall = read.origin,
            .throwSite = calls.siteThatNamesCallee(call),
            .isOnRarePath = true,
            .callerSuspendsAtReturn = call.isAwaitedAtOnce(),
        }));
        calls.defineResultOfCallByValue(call, answersMeet.finish().as<Boxed>());
        return true;
    }
    calls.defineResultOfCallByValue(call, calls.callByConvention(call, conventionCall, {
        .numberedImmediate = answers ? *answers : NumberedImmediate { mentionOfName(read.key) },
        .readBeforeCall = read.origin,
        .throwSite = calls.siteThatNamesCallee(call),
        .callerSuspendsAtReturn = call.isAwaitedAtOnce(),
    }));
    return true;
}

bool AccessEmitter::readsTruthAtSite(BlockID block, ValueID id) const
{
    const Operation& read = operationAt(id);
    PropertyKeyID key = read.key();
    if (read.isFreeName() || isSymbolOfRealm(key) || liesInThread(key) || !m_functionComesAgain)
        return false;
    if (m_code.at(block).frequency == BlockFrequency::Rare || testsItsOwnGuess(read))
        return false;
    return m_program.nameClassOf(key) != NameClass::Length;
}

std::optional<unsigned> AccessEmitter::integerThatLengthIsAskedFor(BlockID block, ValueID id, ValueID test)
{
    const Operation& read = operationAt(id);
    const Operation& asked = operationAt(test);
    if (read.opcode != Opcode::PropGet || read.isFreeName() || m_program.nameClassOf(read.key()) != NameClass::Length || verifiesFastPathAnswers(read.key()))
        return std::nullopt;
    if (asked.opcode != Opcode::StrictEq || asked.form() != OperatorForm::Int32AndValue || m_code.at(block).frequency == BlockFrequency::Rare || testsItsOwnGuess(read))
        return std::nullopt;
    ValueID other = asked.operand(0) == id ? asked.operand(1) : asked.operand(0);
    const Operation& written = operationAt(other);
    if (other == id || written.opcode != Opcode::Integer || written.integer() < 0 || written.integer() > static_cast<int64_t>(greatestIntegerThatLengthIsAskedFor))
        return std::nullopt;
    return static_cast<unsigned>(written.integer());
}

void AccessEmitter::branchOnWhetherLengthIs(const ValueContext& read, unsigned integer)
{
    m_out.noteForm(Form::Closed, StateOfKnowledge::Always);
    LValue operands[] = { in<Boxed>(read->objectOperand()).value() };
    emitter<RuntimeCallEmitter>().callStubAndBranch(read, lengthIs(integer), operands, { .numberedImmediate = NumberedImmediate { mentionOfName(read->key()) } }, read.branchTargets());
}

void AccessEmitter::branchOnTruthAtSite(const ValueContext& read)
{
    PropertyKeyID key = read->key();
    ValueID receiverID = read->objectOperand();
    Boxed receiver = in<Boxed>(receiverID);
    m_out.noteForm(Form::Closed, StateOfKnowledge::Always);
    const bool asksShapeFirst = m_plan.slots().someLayoutWrittenInHasField(m_program.at(m_unit.function).module, key) && !(read->opcode == Opcode::PropGet && read->receiverIsNoObjectOfCompiledCode());
    const SiteUse use = !asksShapeFirst && m_plan.slots().someLayoutHasField(key) ? SiteUse::ReadUnderNameOfField : SiteUse::Read;
    std::optional<NumberedImmediate> answers = answersOfKeyThatIsNoSymbolOfRealm(read, key, use);
    if (!answers) {
        emitter<TruthinessEmitter>().branchOnTruthiness(read, getById(read, receiver, key, receiverID), typeOf(read.id()), read.branchTargets());
        return;
    }
    ReceiverAtSite receiverIs = typeOf(receiverID).isObject() ? ReceiverAtSite::IsCell : ReceiverAtSite::IsAnyValue;
    LValue operands[] = { receiver.value() };
    StubKey stub = asksShapeFirst ? readTruthAtSite(receiverIs) : readTruthAtSiteAskingStructureFirst(receiverIs, use, read->opcode == Opcode::PropGet && read->receiverIsNoObjectOfCompiledCode());
    if (!verifiesFastPathAnswers(key)) [[likely]] {
        emitter<RuntimeCallEmitter>().callStubAndBranch(read, stub, operands, { .numberedImmediate = *answers }, read.branchTargets());
        return;
    }
    LBasicBlock isTruthy = newBlock();
    LBasicBlock isFalsy = newBlock();
    Word32 countBeforeRead = recordPendingRead(receiver, key);
    emitter<RuntimeCallEmitter>().callStubAndBranch(read, stub, operands, { .numberedImmediate = *answers }, { normalEdge(isTruthy), normalEdge(isFalsy) });
    m_out.appendTo(isTruthy);
    verifyFastPathAnswer(read, receiver, key, boxedConstant(jsBoolean(true)), StubThatAnswers::TruthByName, countBeforeRead);
    m_out.jump(read.branchTargets().taken);
    m_out.appendTo(isFalsy);
    verifyFastPathAnswer(read, receiver, key, boxedConstant(jsBoolean(false)), StubThatAnswers::TruthByName, countBeforeRead);
    m_out.jump(read.branchTargets().notTaken);
}

void AccessEmitter::generatePropGet(const ValueContext& value)
{
    if (value->isFreeName()) {
        m_out.noteForm(Form::Runtime, StateOfKnowledge::FreeName);
        define(value, getFreeName(value));
        return;
    }
    if (value.isBranchCondition()) {
        if (std::optional<unsigned> integer = integerThatLengthIsAskedFor(value.block(), value.id(), m_code.onlyUserOf(value.id())))
            branchOnWhetherLengthIs(value, *integer);
        else
            branchOnTruthAtSite(value);
        return;
    }
    ValueID onlyUser = m_code.onlyUserOf(value.id());
    if (onlyUser && operationAt(onlyUser).opcode == Opcode::Call && isReadOfMethodCalledNext(value)) {
        m_out.noteForm(Form::Closed, StateOfKnowledge::Always);
        m_readLeftToCall = ReadLeftToCall { value.id(), value->key(), value.origin(), findPatternLeftToCall(value) };
        define(value, undefined());
        return;
    }
    if (m_code.at(value.block()).terminator.kind == TerminatorKind::ReturnCall)
        returnWhatMethodOfBuiltinClassGives(value);
    if (LValue stored = m_valuesStoredBeforeLoop.isEmpty() ? nullptr : m_valuesStoredBeforeLoop.take(value.id().index())) {
        m_out.noteForm(Form::Open, StateOfKnowledge::Always, Place::Loop);
        LBasicBlock isKnown = newBlock();
        LBasicBlock isNotKnown = newBlock();
        Join<Boxed> join(m_out);
        m_out.branch(isNotEmpty(Boxed(stored)).value(), normalEdge(isKnown), unlikelyEdge(isNotKnown));
        m_out.appendTo(isKnown);
        join.add(Boxed(stored));
        m_out.appendTo(isNotKnown);
        join.add(getById(value, in<Boxed>(value->objectOperand()), value->key(), value->objectOperand()));
        define(value, join.finish());
        return;
    }
    if (std::optional<Boxed> method = lookWhetherReceiverIsAsMadeInPlaceOfRead(value)) {
        define(value, *method);
        return;
    }
    define(value, getNamedProperty(value));
}

ALWAYS_INLINE_INTO_OTHER_FILES std::optional<Boxed> AccessEmitter::lookWhetherReceiverIsAsMadeInPlaceOfRead(const ValueContext& read)
{
    ValueID onlyUser = m_code.onlyUserOf(read.id());
    PropertyKeyID key = read->key();
    if (!onlyUser || isSymbolOfRealm(key))
        return std::nullopt;
    const Operation& call = operationAt(onlyUser);
    auto inFrontOfBox = [&](ValueID id) {
        const Operation& operation = operationAt(id);
        return operation.opcode == Opcode::Box ? operation.operand(0) : id;
    };
    if (call.opcode != Opcode::Call || !call.methodOfReceiverAsMade() || call.methodIsNamedByKeyAlone())
        return std::nullopt;
    const Terminator& terminator = m_code.at(m_code.blockOf(onlyUser)).terminator;
    const bool endsUnit = terminator.kind == TerminatorKind::ReturnCall && terminator.callOperand() == onlyUser;
    if (call.isTailCall != endsUnit && !call.inlineFrame)
        return std::nullopt;
    if (endsUnit) {
        bool operandsMayBeSeen = m_code.blockOf(onlyUser) != read.block() || std::ranges::any_of(read.operationsBehind(), [&](ValueID id) {
            const Operation& operation = operationAt(id);
            return operation.opcode != Opcode::Box && !operation.traits().isPure();
        });
        if (!operandsMayBeSeen)
            return std::nullopt;
    }
    if (call.calleeOperand() != read.id() || !call.thisValueOperand() || inFrontOfBox(call.thisValueOperand()) != inFrontOfBox(read->objectOperand()) || std::ranges::contains(call.argumentsOperands(), read.id()))
        return std::nullopt;
    CallEmitter& calls = emitter<CallEmitter>();
    const unsigned count = static_cast<unsigned>(call.argumentsOperands().size());
    std::optional<BuiltinBoxedStub> boxedCall = m_program.builtins().boxedCallStubOf(call.methodOfReceiverAsMade());
    if (!boxedCall || !servesCallsByName(*boxedCall, count) || !cellTypesOfReceiversOf(*boxedCall))
        return std::nullopt;
    const bool receiverIsProven = calls.isProvenReceiverOf(*boxedCall, typeOf(read->objectOperand()));
    if (receiverIsProven && !looksWhetherReceiverIsAsMade(*boxedCall))
        return std::nullopt;
    if (calls.mayForwardCallToReceiver(call) || argumentsLentBy(m_code, call))
        return std::nullopt;
    std::optional<NumberedImmediate> answers = answersOfKeyThatIsNoSymbolOfRealm(read, key, SiteUse::Read);
    if (!answers)
        return std::nullopt;
    m_out.noteForm(Form::Closed, StateOfKnowledge::Always);
    m_methodsThatMayBeUnread.set(read.id().index());
    LValue receiver = in<Boxed>(read->objectOperand()).value();
    const StubKey looks = receiverIsProven ? readAtSiteUnlessAsMade() : readMethodAtSiteUnlessReceiverFindsIt(*boxedCall, m_plan.slots().someLayoutWrittenInHasField(m_program.at(m_unit.function).module, key));
    auto readUnlessAsMade = [&] {
        if (!verifiesFastPathAnswers(key)) [[likely]]
            return Boxed(callStub(read, looks, { receiver }, { .numberedImmediate = *answers }).first);
        Word32 countBeforeRead = recordPendingRead(Boxed(receiver), key);
        Boxed method(callStub(read, looks, { receiver }, { .numberedImmediate = *answers }).first);
        verifyFastPathAnswer(read, Boxed(receiver), key, method, StubThatAnswers::ReadByNameUnlessAsMade, countBeforeRead);
        return method;
    };
    if (!m_program.admitsReplacedBuiltins || !receiverIsProven)
        return readUnlessAsMade();
    Pointer membersWritten = loadImmutablePointer(AOT_INSTANCE_OFFSET(builtins.membersWritten));
    Condition methodIsAsBuilt = boolean(true);
    for (BuiltinID member : BuiltinTable::singleton().membersThatHoldMethodOf(*boxedCall))
        methodIsAsBuilt = logicalAnd(methodIsAsBuilt, isZero(Word32(m_out.load8ZeroExt32(m_out.address(m_heaps.entire(AbstractHeap::NamedProperties), membersWritten.value(), member.index())))));
    LBasicBlock asBuilt = newBlock();
    LBasicBlock replaced = newBlock();
    OperandJoin methods(m_out, Representation::JSValue);
    branch(methodIsAsBuilt, { normalEdge(asBuilt), rareEdge(replaced) });
    m_out.appendTo(asBuilt);
    methods.add(readUnlessAsMade());
    m_out.appendTo(replaced);
    methods.add(getNamedProperty(read));
    return methods.finish().as<Boxed>();
}

std::optional<AccessEmitter::StoreOfKnownField> AccessEmitter::findStoreOfKnownField(const Type& receiver, PropertyKeyID key, PutMode mode)
{
    if (isSymbolOfRealm(key) || !receiver.isObject() || !receiver.layouts || !receiver.hasOnlyLayouts())
        return std::nullopt;
    std::span<const LayoutID> receivers = membersOf(types(), receiver);
    std::optional<uint32_t> record = m_plan.slots().recordOfStoredField(m_program, key, receivers);
    if (!record)
        return std::nullopt;
    switch (m_plan.slots().formOfStoredField(m_program, key, receivers)) {
    case StoredFieldForm::Any:
        return StoreOfKnownField { putKnownField(mode), *record };
    case StoredFieldForm::TakesAnyValue:
        return StoreOfKnownField { putKnownFieldThatTakesAnyValue(mode), *record };
    case StoredFieldForm::AcceptsAnyNumber:
        return StoreOfKnownField { putKnownFieldAcceptingAnyNumber(mode), *record };
    case StoredFieldForm::KeepsNumbersAsValues:
        return StoreOfKnownField { putKnownFieldKeepingNumbersAsValues(mode), *record };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template<typename UsePlace>
void AccessEmitter::checkStoreSiteInline(Boxed receiver, Pointer site, const ShapesOfStoreAtSite& shapes, const UsePlace& usePlace)
{
    static_assert(!Slot::offsetOfStructureID() && Slot::offsetOfFlags() == Slot::offsetOfKind() + 1 && Slot::offsetOfLocation() == Slot::offsetOfKind() + 2);
    static_assert(!runsOwnerBarrierOf(Opcode::PropSet) && Slot::wordIndexFor(0) > 0 && Slot::fitsInLocation(Slot::wordIndexFor(firstOutOfLineOffset - 1)) && !Slot::flagsFixedBy(Slot::Kind::Value) && Slot::wordIndexFor(firstOutOfLineOffset) < 0);
    constexpr int32_t ownWordInCell = static_cast<int32_t>(Slot::Kind::Value) | static_cast<int32_t>(Slot::isOwn) << 8;
    constexpr int32_t ownWordInStorage = ownWordInCell | static_cast<int32_t>(Slot::isOutOfLine) << 8;
    LBasicBlock isNotInCell = newBlock();
    auto goOnIf = [&](Condition condition, LBasicBlock otherwise) {
        LBasicBlock next = newBlock();
        m_out.branch(condition.value(), normalEdge(next), unlikelyEdge(otherwise));
        m_out.appendToContinuation(next);
    };
    Machine::DeclaredAccessesSuspension suspension(m_out);
    CellPointer object = unboxCell(receiver);
    Pointer siteOfThisStore(m_out.opaque(site.value()));
    auto partOfSlot = [&](ptrdiff_t offset) {
        return m_out.address(m_heaps.AOTInstance_siteCaches, siteOfThisStore.value(), offset);
    };
    auto askReceiver = [&] {
        if (!shapes.known.isObject()) {
            Condition isPresentCell = isCell(receiver);
            if (shapes.known.primitives & Primitive::Absent)
                isPresentCell = logicalAnd(isPresentCell, isNotEmpty(receiver));
            goOnIf(isPresentCell, shapes.asksStub);
        }
        if (shapes.asksTableOfFields)
            goOnIf(Condition(m_out.testIsZero32(m_out.load8ZeroExt32(m_out.address(m_heaps.root, object.value(), offsetOfShapeWord + 3)), m_out.constInt32(static_cast<int32_t>(compiledObjectShapeBit >> 24)))), shapes.asksStub);
        goOnIf(equal(Word32(m_out.load32(partOfSlot(Slot::offsetOfStructureID()))), loadStructureID(object)), shapes.hasOtherStructure);
    };
    Word32 kindAndFlags(m_out.load16ZeroExt32(partOfSlot(Slot::offsetOfKind())));
    goOnIf(equal(kindAndFlags, ownWordInCell), isNotInCell);
    askReceiver();
    usePlace(object, offsetBy(asPointer(object), zeroExtended(Word32(m_out.load16ZeroExt32(partOfSlot(Slot::offsetOfLocation())))), 3));

    m_out.appendTo(isNotInCell);
    goOnIf(equal(kindAndFlags, ownWordInStorage), shapes.asksStub);
    askReceiver();
    usePlace(object, offsetBy(loadField<Pointer>(object, m_heaps.JSObject_butterfly), signExtended(Word32(m_out.load16SignExt32(partOfSlot(Slot::offsetOfLocation())))), 3));
}

ValueID AccessEmitter::readAfterStore(const ValueContext& store)
{
    PropertyKeyID key = store->key();
    if (isSymbolOfRealm(key) || verifiesFastPathAnswers(key) || m_program.nameClassOf(key) == NameClass::LastIndex)
        return { };
    std::span<const ValueID> behind = m_code.valuesOf(store.block());
    behind = behind.subspan(std::ranges::find(behind, store.id()) - behind.begin() + 1);
    for (ValueID id : behind) {
        const Operation& operation = m_code.at(id);
        if (operation.opcode == Opcode::PropGet && !operation.isFreeName() && operation.key() == key && operation.objectOperand() == store->objectOperand())
            return id;
        EffectTraits traits = operation.traits();
        if (traits.mayWriteObservableState() || traits.mayRunOtherCode())
            return { };
    }
    return { };
}

bool AccessEmitter::storeAskingSiteInLine(const ValueContext& value, Boxed receiver, Boxed stored, PutMode mode, Pointer site)
{
    if (!m_generator.paysForCodeInLine(value.block()))
        return false;
    PropertyKeyID key = value->key();
    bool asksTableOfFields = m_plan.slots().someLayoutHasField(key);
    if (asksTableOfFields && m_plan.slots().someLayoutWrittenInHasField(m_program.at(m_unit.function).module, key))
        return false;
    if (m_program.nameClassOf(key) == NameClass::Length)
        return false;
    ValueID made = definitionOf(value->objectOperand());
    if (opcodeOf(made) == Opcode::Box)
        made = definitionOf(operationAt(made).operand(0));
    if (Operation::opcodesWithStorage.contains(opcodeOf(made)) && m_generator.isInLoop(m_code.blockOf(made)))
        return false;
    if (opcodeOf(made) == Opcode::GetClassPrototype && mode != PutMode::Define && !asksTableOfFields) {
        callStub(value, putPrototypeMemberAtSite(mode), { receiver.value(), stored.value(), site.value() }, { .numberedImmediate = NumberedImmediate { mentionOfName(key) } });
        return true;
    }

    build().statistics().count(StatisticID::SiteCachedStoresEmittedInline);
    m_out.noteForm(Form::Open, StateOfKnowledge::Always, Place::Loop);
    LBasicBlock hasOtherStructure = newBlock();
    LBasicBlock asksStub = newBlock();
    LBasicBlock done = newBlock();
    const ValueID read = mode == PutMode::Define ? ValueID() : readAfterStore(value);
    Vector<ValueFromBlock, 4> valuesReadAfter;
    auto goOn = [&](LValue valueReadAfter) {
        if (read)
            valuesReadAfter.append(m_out.anchor(valueReadAfter));
        m_out.jump(done);
    };
    LValue isNotKnown = read ? boxedConstant(JSValue()).value() : nullptr;
    checkStoreSiteInline(receiver, site, { typeOf(value->objectOperand()), asksTableOfFields, hasOtherStructure, asksStub }, [&](CellPointer object, Pointer place) {
        m_out.store64(stored.value(), m_out.address(m_heaps.root, place.value()));
        barrierAfterStore(value, object, typeOf(value->valueOperand()));
        goOn(stored.value());
    });

    m_out.appendTo(hasOtherStructure);
    callStub(value, putByIdAtSiteOfOtherStructure(mode), { receiver.value(), stored.value(), site.value() }, { .numberedImmediate = NumberedImmediate { mentionOfName(key) } });
    goOn(isNotKnown);

    m_out.appendTo(asksStub);
    callStub(value, asksTableOfFields ? putByIdAtSite(mode) : putByIdAtSiteAskingNoTableOfFields(mode), { receiver.value(), stored.value(), site.value() }, { .numberedImmediate = NumberedImmediate { mentionOfName(key) } });
    goOn(isNotKnown);

    m_out.appendTo(done);
    if (read) {
        build().statistics().count(StatisticID::ReadsForwardedFromStores);
        m_valuesStoredBeforeLoop.set(read.index(), m_out.phi(Machine::Int64, valuesReadAfter));
    }
    return true;
}

void AccessEmitter::putNamedProperty(const ValueContext& value, Boxed object, PutMode mode)
{
    PropertyKeyID key = value->key();
    Boxed stored = in<Boxed>(value->valueOperand());
    Type receiver = typeOf(value->objectOperand());
    if (std::optional<StoreOfKnownField> store = findStoreOfKnownField(receiver, key, mode)) {
        callStub(value, store->stub, { object.value(), stored.value() }, { .immediate = store->offsetOfRecord });
        return;
    }
    std::optional<Pointer> site = value.opcode() == Opcode::PropSet ? siteOfStore(value, key, accessFor(mode)) : std::nullopt;
    if (site && storeAskingSiteInLine(value, object, stored, mode, *site))
        return;
    putById(value, object, key, stored, mode, site);
}

bool AccessEmitter::storeLastIndexInLine(const ValueContext& value, Boxed object, PutMode mode)
{
    if (!m_generator.runsRepeatedly(value.block()) && !m_facts.linked().functionsCalledInLoops.get(m_unit.function.index()))
        return false;
    const PropertyKey& name = types().at(value->key());
    if (name.kind != PropertyKeyKind::Text || m_program.nameClass(name.text) != NameClass::LastIndex || !typeOf(value->valueOperand()).isNumber())
        return false;
    Type receiver = typeOf(value->objectOperand());
    const ClassPart* onlyClass = receiver.isObject() ? singleBuiltinClassOf(types(), receiver) : nullptr;
    const bool isRegExp = onlyClass && onlyClass->builtinClass == BuiltinClassID(BuiltinClasses::RegExp);
    if (!isRegExp && (onlyClass || !receiver.hasObjects() || findStoreOfKnownField(receiver, value->key(), mode)))
        return false;
    if (isRegExp && emitter<ResolvedCallEmitter>().isDeferredLastIndexStore(value))
        return true;

    Boxed stored = in<Boxed>(value->valueOperand());
    const Machine::AbstractHeap& state = m_heaps.entire(AbstractHeap::RegExpState);
    LBasicBlock isOther = newBlock();
    LBasicBlock done = newBlock();
    auto continueIf = [&](Condition condition) {
        LBasicBlock continuation = newBlock();
        m_out.branch(condition.value(), normalEdge(continuation), rareEdge(isOther));
        m_out.appendTo(continuation);
    };
    LValue regExpObject = unboxCell(object).value();
    if (!isRegExp) {
        if (!receiver.isObject()) {
            Condition isPresentCell = isCell(object);
            if (receiver.primitives & Primitive::Absent)
                isPresentCell = logicalAnd(isPresentCell, isNotEmpty(object));
            continueIf(isPresentCell);
        }
        continueIf(equal(loadStructureID(unboxCell(object)), loadFilledOnce32(Instance::offsetOfReceiverStructureID(Receiver::RegExp))));
    }
    LValue regExpAndFlags = m_out.load64(m_out.address(state, regExpObject, RegExpObject::offsetOfRegExpAndFlags()));
    continueIf(Condition(m_out.testIsZero64(regExpAndFlags, RegExpObject::lastIndexIsNotWritableFlag)));
    m_out.store64(stored.value(), m_out.address(state, regExpObject, RegExpObject::offsetOfLastIndex()));
    m_out.jump(done);
    m_out.appendTo(isOther);
    putNamedProperty(value, object, mode);
    m_out.jump(done);
    m_out.appendTo(done);
    return true;
}

void AccessEmitter::generatePropSet(const ValueContext& value)
{
    if (value->isFreeName()) {
        putFreeName(value);
        return;
    }
    Boxed object = in<Boxed>(value->objectOperand());
    PutMode mode = putModeOf(value->mode());
    if (!storeLastIndexInLine(value, object, mode))
        putNamedProperty(value, object, mode);
}

void AccessEmitter::generatePropDefine(const ValueContext& value)
{
    Boxed object = boxCell(in<CellPointer>(value->objectOperand()));
    const PropertyKey& name = types().at(value->key());
    if (name.kind == PropertyKeyKind::Text && m_program.nameClass(name.text) == NameClass::Index) {
        std::optional<uint32_t> index = parseIndex(byteCast<Latin1Character>(m_program.strings[name.text]));
        if (index && *index <= static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
            Boxed key(m_out.constInt64(JSValue::encode(jsNumber(static_cast<int32_t>(*index)))));
            putByVal(value, object, key, in<Boxed>(value->valueOperand()), PutMode::Define);
            return;
        }
    }
    putNamedProperty(value, object, PutMode::Define);
}

void AccessEmitter::generatePropHas(const ValueContext& value)
{
    defineTest(value, hasById(value, in<Boxed>(value->objectOperand()), value->key()));
}

void AccessEmitter::generatePropDelete(const ValueContext& value)
{
    if (isSymbolOfRealm(value->key())) {
        define(value, callOperation<Condition>(value, forMode(value->mode(), RuntimeOperation::DeleteByVal, RuntimeOperation::DeleteByValSloppy), { in<Boxed>(value->objectOperand()), cellOfKey(value, value->key()) }));
        return;
    }
    Word32 key = identifierNumber(value->key());
    if (value->isFreeName()) {
        define(value, callOperation<Condition>(value, RuntimeOperation::DeleteFreeName, { key }));
        return;
    }
    Boxed object = in<Boxed>(value->objectOperand());
    if (!typeOf(value->objectOperand()).hasObjects()) {
        define(value, callOperation<Condition>(value, forMode(value->mode(), RuntimeOperation::DeleteById, RuntimeOperation::DeleteByIdSloppy), { object, key }));
        return;
    }
    define(value, Condition(m_out.castToInt32(callStub(value, deleteByIdCached(putModeOf(value->mode())), { object.value(), zeroExtended(key).value() }).first)));
}

static StubKey cachedStoreStubFor(PutMode mode)
{
    switch (mode) {
    case PutMode::Strict:
    case PutMode::Sloppy:
        return putByValCached(mode);
    case PutMode::Define:
        return defineByValCached();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Boxed AccessEmitter::getByVal(const OperationContext& context, Boxed receiver, Boxed key)
{
    return Boxed(callStub(context, getByValCached(), { receiver.value(), key.value() }).first);
}

void AccessEmitter::putByVal(const OperationContext& context, Boxed receiver, Boxed key, Boxed value, PutMode mode)
{
    callStub(context, cachedStoreStubFor(mode), { receiver.value(), key.value(), value.value() });
}

Condition AccessEmitter::hasByVal(const OperationContext& context, Boxed receiver, Boxed key)
{
    return Condition(m_out.castToInt32(callStub(context, inByValCached(PresenceQuestion::In), { receiver.value(), key.value() }).first));
}

LValue AccessEmitter::tryUnboxConstantIndex(Boxed key)
{
    if (!key.value()->hasInt64())
        return nullptr;
    JSValue constant = JSValue::decode(key.value()->asInt64());
    if (!constant.isInt32() || constant.asInt32() < 0)
        return nullptr;
    return m_out.constInt64(constant.asInt32());
}

std::optional<AccessEmitter::ElementKey> AccessEmitter::elementKeyOf(const ValueContext& value)
{
    ValueID property = value->propertyOperand();
    switch (representationOf(property)) {
    case Representation::Int32:
    case Representation::Int64:
        return ElementKey { integerOf(property).value(), true };
    case Representation::JSValue:
        return ElementKey { in<Boxed>(property).value(), false };
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Double:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::Cell:
    case Representation::Cell32:
    case Representation::RawWord:
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<AOT::ExpectedElements> stubFormOfExpectedElements(ExpectedElements expected)
{
    switch (expected) {
    case ExpectedElements::Anything:
        return std::nullopt;
    case ExpectedElements::Int32Array:
        return AOT::ExpectedElements::Int32Array;
    case ExpectedElements::Uint8Array:
        return AOT::ExpectedElements::Uint8Array;
    case ExpectedElements::Float64Array:
        return AOT::ExpectedElements::Float64Array;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool AccessEmitter::readsElementsRepeatedly(const ValueContext& value, ValueID object)
{
    bool standsInRareBlock = m_code.at(value.block()).frequency == BlockFrequency::Rare;
    return !standsInRareBlock && m_generator.runsRepeatedly(value.block()) && typeOf(object).hasObjects();
}

Boxed AccessEmitter::presentElementOrElse(const ValueContext& value, ValueID objectOperand, const ElementKey& key, LBasicBlock outOfLine)
{
    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    Boxed object = in<Boxed>(objectOperand);
    Integer index;
    if (key.isInteger)
        index = Integer(key.value);
    else {
        Boxed boxedKey(key.value);
        branchUnless(isInt32(boxedKey), outOfLine);
        index = signExtended(unboxInt32(boxedKey));
    }
    Type known = typeOf(objectOperand);
    if (known.primitives) {
        Condition isPresentCell = isCell(object);
        if (known.primitives & Primitive::Absent)
            isPresentCell = logicalAnd(isPresentCell, isNotEmpty(object));
        branchUnless(isPresentCell, outOfLine);
    }
    CellPointer cell = unboxCell(object);
    bool readsArrayStorage = !m_generator.isInLoop(value.block());
    LBasicBlock hasOtherStorage = readsArrayStorage ? newBlock() : outOfLine;
    Join<Boxed> join(m_out);
    branchUnless(elements.objectHoldsBoxedElements(cell), hasOtherStorage);
    ArrayElements storage = elements.elementsOf(cell);
    LengthTest test = elements.testLength(index, storage);
    branchUnless(test.isInside, outOfLine);
    Boxed element = elements.loadBoxedElement(storage, test.indexInside);
    branchUnless(isNotEmpty(element), outOfLine);
    if (!readsArrayStorage)
        return element;
    join.add(element);

    m_out.appendTo(hasOtherStorage);
    join.add(elements.loadArrayStorageElement(cell, index, outOfLine));
    return join.finish();
}

void AccessEmitter::generateElemGet(const ValueContext& value)
{
    Boxed object = in<Boxed>(value->objectOperand());
    std::optional<ElementKey> key = elementKeyOf(value);
    if (!key) {
        define(value, emitter<ConstantEmitter>().zeroOf(value.representation()));
        return;
    }
    bool isInIterations = m_generator.isInLoop(value.block()) && m_code.at(value.block()).frequency != BlockFrequency::Rare;
    const RegistersOfCaller registersOfCaller = isInIterations ? RegistersOfCaller::AreKept : RegistersOfCaller::AreLost;
    StubKey stub = getByValCached(registersOfCaller);
    std::optional<AOT::ExpectedElements> expected = stubFormOfExpectedElements(value->expectedElements());
    if (expected)
        stub = getByValExpecting(key->isInteger, *expected);
    else if (key->isInteger || value->keyIsIntegral())
        stub = AOT::getByVal(key->isInteger);
    else if (typeOf(value->objectOperand()).isObject() && typeOf(value->propertyOperand()).isString())
        stub = getByValCachedCells(registersOfCaller);

    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    const PlacedSlot* extraPropertiesWord = elements.extraPropertiesWordOfReadByPosition(value);
    if (value->receiverIsExpectedToBeArguments() && !extraPropertiesWord && m_generator.isInLoop(value.block())) {
        LBasicBlock isNotUnmappedAsMade = newBlock();
        LBasicBlock outOfLine = newBlock();
        Join<Boxed> join(m_out);
        Integer index;
        if (key->isInteger)
            index = Integer(key->value);
        else {
            Boxed boxedKey(key->value);
            branchUnless(isInt32(boxedKey), outOfLine);
            index = signExtended(unboxInt32(boxedKey));
        }
        Type known = typeOf(value->objectOperand());
        if (known.primitives & ~cellPrimitives) {
            Condition isPresentCell = isCell(object);
            if (known.primitives & Primitive::Absent)
                isPresentCell = logicalAnd(isPresentCell, isNotEmpty(object));
            branchUnless(isPresentCell, outOfLine);
        }
        CellPointer cell = unboxCell(object);
        Word32 structureAsMade(m_out.load32(mutableInstanceField(AOT_INSTANCE_OFFSET(allocation.inlineAllocations[static_cast<unsigned>(InlineAllocation::Arguments)].header))));
        branchUnless(equal(loadStructureID(cell), structureAsMade), isNotUnmappedAsMade);
        ArrayElements storage = elements.elementsOf(cell);
        LengthTest test = elements.testLength(index, storage);
        branchUnless(test.isInside, outOfLine);
        Boxed element = elements.loadRawElement(arrayStorageFormOf(ArrayStorage::Contiguous), storage, test.indexInside).as<Boxed>();
        branchUnless(isNotEmpty(element), outOfLine);
        join.add(element);

        m_out.appendTo(isNotUnmappedAsMade);
        branchUnless(hasCellType(cell, DirectArgumentsType), outOfLine);
        branchUnless(isZero(Integer(loadField<Pointer>(cell, m_heaps.DirectArguments_mappedArguments).value())), outOfLine);
        branchUnless(below(index, zeroExtended(loadField<Word32>(cell, m_heaps.DirectArguments_length))), outOfLine);
        join.add(Boxed(m_out.load64(m_out.baseIndex(m_heaps.DirectArguments_storage, cell.value(), index.value()))));

        m_out.appendTo(outOfLine);
        join.add(Boxed(callStub(value, stub, { object.value(), key->value }, { .isOnRarePath = true }).first));
        define(value, join.finish());
        return;
    }
    bool keyIsIndex = key->isInteger || value->keyIsIntegral() || typeOf(value->propertyOperand()).isNumber();
    if (!extraPropertiesWord && !expected && keyIsIndex && readsElementsRepeatedly(value, value->objectOperand())) {
        LBasicBlock outOfLine = newBlock();
        Join<Boxed> join(m_out);
        join.add(presentElementOrElse(value, value->objectOperand(), *key, outOfLine));

        m_out.appendTo(outOfLine);
        join.add(Boxed(callStub(value, stub, { object.value(), key->value }, { .isOnRarePath = true }).first));
        define(value, join.finish());
        return;
    }
    if (!extraPropertiesWord) {
        if (stub == getByValCached(RegistersOfCaller::AreKept) || stub == getByValCachedCells(RegistersOfCaller::AreKept))
            build().statistics().count(StatisticID::KeyedReadsKeepingRegisters);
        define(value, Boxed(callStub(value, stub, { object.value(), key->value }).first));
        return;
    }
    LBasicBlock outOfLine = newBlock();
    Join<Boxed> join(m_out);
    if (!typeOf(value->objectOperand()).isObject())
        branchUnless(isCell(object), outOfLine);
    Integer index;
    if (key->isInteger)
        index = Integer(key->value);
    else {
        Boxed boxedKey(key->value);
        branchUnless(isInt32(boxedKey), outOfLine);
        index = signExtended(unboxInt32(boxedKey));
    }
    {
        Machine::DeclaredAccessesSuspension suspension(m_out);
        bool readsArrayStorage = m_generator.runsRepeatedly(value.block()) && !m_generator.isInLoop(value.block());
        join.add(elements.readElementOfExtraProperties(unboxCell(object), *extraPropertiesWord, index, outOfLine, readsArrayStorage));
    }

    m_out.appendTo(outOfLine);
    join.add(Boxed(callStub(value, stub, { object.value(), key->value }).first));
    define(value, join.finish());
}

void AccessEmitter::generateElemGetByKeyParts(const ValueContext& value)
{
    BaseOfKeyedRead base = typeOf(value->objectOperand()).isObject() ? BaseOfKeyedRead::IsCell : BaseOfKeyedRead::IsAnyValue;
    Boxed object = in<Boxed>(value->objectOperand());
    Boxed first = in<Boxed>(value->firstPartOperand());
    Boxed second = in<Boxed>(value->secondPartOperand());
    define(value, Boxed(callStub(value, getByValOfKeyOfPair(base), { object.value(), first.value(), second.value() }).first));
}

void AccessEmitter::overwriteElementOrElse(const ValueContext& value, StubKey stub, Boxed object, const ElementKey& key, Boxed stored)
{
    LBasicBlock outOfLine = newBlock();
    LBasicBlock isStored = newBlock();
    if (!typeOf(value->objectOperand()).isObject())
        branchUnless(isCell(object), outOfLine);
    if (typeOf(value->objectOperand()).primitives & Primitive::Absent)
        branchUnless(isNotEmpty(object), outOfLine);
    Integer index;
    if (key.isInteger)
        index = Integer(key.value);
    else {
        Boxed boxedKey(key.value);
        branchUnless(isInt32(boxedKey), outOfLine);
        index = signExtended(unboxInt32(boxedKey));
    }
    {
        Machine::DeclaredAccessesSuspension suspension(m_out);
        emitter<ElementAccessEmitter>().overwriteElementOfOrdinaryObject(value, unboxCell(object), index, stored, outOfLine);
    }
    build().statistics().count(StatisticID::ElementsOverwrittenInLoops);
    m_out.jump(isStored);

    m_out.appendTo(outOfLine);
    callStub(value, stub, { object.value(), key.value, stored.value() }, { .isOnRarePath = true });
    m_out.jump(isStored);

    m_out.appendTo(isStored);
}

void AccessEmitter::generateElemSet(const ValueContext& value)
{
    Boxed object = in<Boxed>(value->objectOperand());
    Boxed stored = in<Boxed>(value->valueOperand());
    std::optional<ElementKey> key = elementKeyOf(value);
    if (!key)
        return;
    StubKey stub = putByValCached(putModeOf(value->mode()));
    std::optional<AOT::ExpectedElements> expected = stubFormOfExpectedElements(value->expectedElements());
    if (expected || key->isInteger || value->keyIsIntegral()) {
        JSC::ECMAMode mode = forMode(value->mode(), JSC::ECMAMode::strict(), JSC::ECMAMode::sloppy());
        stub = expected ? putByValExpecting(key->isInteger, mode, *expected) : AOT::putByVal(key->isInteger, mode);
    }
    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    const PlacedSlot* extraPropertiesWord = elements.extraPropertiesWordOfReadByPosition(value);
    if (!extraPropertiesWord) {
        LValue constantIndex = key->isInteger || expected ? nullptr : tryUnboxConstantIndex(Boxed(key->value));
        JSC::ECMAMode mode = forMode(value->mode(), JSC::ECMAMode::strict(), JSC::ECMAMode::sloppy());
        if (constantIndex && isKnownIndex(constantIndex->asInt64()))
            callStub(value, putByValAtKnownIndex(static_cast<unsigned>(constantIndex->asInt64()), mode), { object.value(), stored.value() });
        else if (constantIndex)
            callStub(value, putByValAtConstantIndex(mode), { object.value(), constantIndex, stored.value() });
        else if (bool keyIsIndex = key->isInteger || value->keyIsIntegral() || typeOf(value->propertyOperand()).isNumber(); keyIsIndex && !expected && m_generator.usesLoopForms(value.block()) && typeOf(value->objectOperand()).hasObjects())
            overwriteElementOrElse(value, stub, object, *key, stored);
        else
            callStub(value, stub, { object.value(), key->value, stored.value() });
        return;
    }
    LBasicBlock outOfLine = newBlock();
    LBasicBlock isStored = newBlock();
    if (!typeOf(value->objectOperand()).isObject())
        branchUnless(isCell(object), outOfLine);
    Integer index;
    if (key->isInteger)
        index = Integer(key->value);
    else {
        Boxed boxedKey(key->value);
        branchUnless(isInt32(boxedKey), outOfLine);
        index = signExtended(unboxInt32(boxedKey));
    }
    {
        Machine::DeclaredAccessesSuspension suspension(m_out);
        elements.overwriteElementOfExtraProperties(value, unboxCell(object), *extraPropertiesWord, index, stored, outOfLine);
    }
    m_out.jump(isStored);

    m_out.appendTo(outOfLine);
    callStub(value, stub, { object.value(), key->value, stored.value() });
    m_out.jump(isStored);
    m_out.appendTo(isStored);
}

void AccessEmitter::generateElemHas(const ValueContext& value)
{
    Boxed object = in<Boxed>(value->objectOperand());
    Boxed key = in<Boxed>(value->propertyOperand());
    if (!typeOf(value->propertyOperand()).isNumber() || !readsElementsRepeatedly(value, value->objectOperand())) {
        defineTest(value, hasByVal(value, object, key));
        return;
    }
    LBasicBlock outOfLine = newBlock();
    Join<Word32> join(m_out);
    presentElementOrElse(value, value->objectOperand(), ElementKey { key.value(), false }, outOfLine);
    join.add(word32(1));

    m_out.appendTo(outOfLine);
    join.add(Word32(hasByVal(value, object, key).value()));
    defineTest(value, Condition(join.finish().value()));
}

void AccessEmitter::generateElemDelete(const ValueContext& value)
{
    Boxed object = in<Boxed>(value->objectOperand());
    Boxed key = in<Boxed>(value->propertyOperand());
    if (!typeOf(value->objectOperand()).hasObjects()) {
        define(value, callOperation<Condition>(value, forMode(value->mode(), RuntimeOperation::DeleteByVal, RuntimeOperation::DeleteByValSloppy), { object, key }));
        return;
    }
    define(value, Condition(m_out.castToInt32(callStub(value, deleteByValCached(putModeOf(value->mode())), { object.value(), key.value() }).first)));
}

void AccessEmitter::generateElemDefine(const ValueContext& value)
{
    Boxed object = boxCell(in<CellPointer>(value->objectOperand()));
    Boxed key = in<Boxed>(value->propertyOperand());
    Boxed stored = in<Boxed>(value->valueOperand());
    putByVal(value, object, key, stored, PutMode::Define);
    emitter<SlotAccessEmitter>().noteComputedKey(value->objectOperand());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
