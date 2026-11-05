/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTAccessEmitter.h"

#if ENABLE(AOT)

#include "AOTArrayBuiltinEmitter.h"
#include "AOTBuiltinCallEmitter.h"
#include "AOTCallEmitter.h"
#include "AOTClosuresLent.h"
#include "AOTConstantEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTConvention.h"
#include "AOTElementAccessEmitter.h"
#include "AOTFieldTable.h"
#include "AOTIRProgram.h"
#include "AOTNameClass.h"
#include "AOTPropertyCachesFront.h"
#include "AOTResultRules.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTRuntimeWords.h"
#include "AOTSlotEmitter.h"
#include "AOTStubCalls.h"
#include "AOTStubsBoxedCalls.h"
#include "AOTStubsCompiledMethods.h"
#include "AOTStubsElements.h"
#include "AOTStubsGlobalBindings.h"
#include "AOTStubsPropertyKeys.h"
#include "AOTTruthinessEmitter.h"
#include "AOTTypeArena.h"
#include "ECMAMode.h"
#include "GetterSetter.h"
#include "PropertyName.h"
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
    if (m_code.at(context.block()).frequency == BlockFrequency::Rare || (context.valueID() && testsItsOwnGuess(m_code.at(context.valueID()))))
        return std::nullopt;
    return siteCacheAddress(context, key, access);
}

std::optional<NumberedImmediate> AccessEmitter::answersOf(const OperationContext& context, PropertyKeyID key, SiteUse use)
{
    if (isSymbolOfRealm(key))
        return std::nullopt;
    return answersOfKeyThatIsNoSymbolOfRealm(context, key, use);
}

std::optional<NumberedImmediate> AccessEmitter::answersOfKeyThatIsNoSymbolOfRealm(const OperationContext& context, PropertyKeyID key, SiteUse use)
{
    if (!comesAgain(context))
        return std::nullopt;
    if (m_code.at(context.block()).frequency == BlockFrequency::Rare || (context.valueID() && testsItsOwnGuess(m_code.at(context.valueID()))))
        return std::nullopt;
    return answersAtSite(context, key, use);
}

StubKey AccessEmitter::stubThatReadsAtSite(PropertyKeyID key, ValueID receiver)
{
    if (!m_plan.slots().hasColumn(key))
        return getByIdAtSiteHashed();
    return receiver && typeOf(receiver).isObject() ? getByIdOfObjectAtSite() : getByIdAtSite();
}

Boxed AccessEmitter::getById(const OperationContext& context, Boxed receiver, PropertyKeyID key, ValueID receiverID)
{
    if (isSymbolOfRealm(key))
        return Boxed(callStub(context, getByIdOfSymbolOfRealm(), { receiver.value() }, { .numberedImmediate = NumberedImmediate { mentionOfName(key) } }).first);
    const NumberedImmediate mention { mentionOfName(key) };
    const bool hasColumn = m_plan.slots().hasColumn(key);
    const bool isLength = hasColumn && m_program.nameClassOf(key) == NameClass::Length;
    if (std::optional<NumberedImmediate> answers = answersOfKeyThatIsNoSymbolOfRealm(context, key, SiteUse::Read)) {
        NameAtSite name = hasColumn ? NameAtSite::HasColumn : NameAtSite::IsAnyName;
        ReceiverAtSite receiverIs = receiverID && typeOf(receiverID).isObject() ? ReceiverAtSite::IsCell : ReceiverAtSite::IsAnyValue;
        const bool namesLength = hasColumn ? isLength : m_program.nameClassOf(key) == NameClass::Length;
        StubKey stub = namesLength ? readLengthAtSite(name) : tryReadAtSiteOfNarrowField(m_plan.slots().narrowFormOf(key), receiverIs, name).value_or(readAtSite(receiverIs, name));
        return Boxed(callStub(context, stub, { receiver.value() }, { .numberedImmediate = *answers }).first);
    }
    if (isLength)
        return Boxed(callStub(context, getLengthAtSite(), { receiver.value() }, { .numberedImmediate = mention }).first);
    StubKey stub = !hasColumn ? getByIdCheckingColumn() : m_plan.slots().narrowFormOf(key) == SlotRepresentation::JSValue ? AOT::getById() : getByIdAskingFieldTableFirst();
    return Boxed(callStub(context, stub, { receiver.value() }, { .numberedImmediate = mention }).first);
}

static RuntimeOperation storeOperationWithoutCachesFor(PutMode mode)
{
    switch (mode) {
    case PutMode::Strict:
        return RuntimeOperation::PutById;
    case PutMode::Sloppy:
        return RuntimeOperation::PutByIdSloppy;
    case PutMode::Define:
        return RuntimeOperation::DefineById;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void AccessEmitter::putById(const OperationContext& context, Boxed receiver, PropertyKeyID key, Boxed value, PutMode mode, std::optional<Pointer> site)
{
    if (isSymbolOfRealm(key)) {
        callStub(context, putByIdOfSymbolOfRealm(mode), { receiver.value(), value.value() }, { .numberedImmediate = NumberedImmediate { mentionOfName(key) } });
        return;
    }
    CallOptions options { .numberedImmediate = NumberedImmediate { mentionOfName(key) } };
    const bool hasColumn = m_plan.slots().hasColumn(key);
    if (!comesAgain(context)) {
        if (hasColumn) {
            callStub(context, putByIdOnce(mode), { receiver.value(), value.value() }, options);
            return;
        }
        static constexpr OpcodeSet storesByName { Opcode::PropSet, Opcode::PropDefine };
        m_out.noteFormOf(storesByName, Form::Runtime, StateOfKnowledge::Always, Place::CodeThatRunsOnce);
        callVoidOperation(context, storeOperationWithoutCachesFor(mode), { receiver, identifierNumber(key), value });
        return;
    }
    if (site) {
        NameOfStoreAtSite name = !m_plan.slots().namesField(key) ? NameOfStoreAtSite::NamesNoField : hasColumn ? NameOfStoreAtSite::NamesFieldWithColumn : NameOfStoreAtSite::NamesField;
        callStub(context, putByIdAtSite(mode, name), { receiver.value(), value.value(), site->value() }, options);
        return;
    }
    StubKey stub = hasColumn ? AOT::putById(mode) : putByIdHashed(mode);
    callStub(context, stub, { receiver.value(), value.value() }, options);
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
    {
        Machine::DeclaredAccessesSuspension suspension(m_out);
        join.add(loadBySlotAtSite(unboxCell(receiver), site, Slot::Kind::Value, asksStub, asksStub));
    }

    m_out.appendTo(asksStub);
    join.add(Boxed(callStub(value, stubThatReadsAtSite(value->key(), value->objectOperand()), { receiver.value(), site.value() }, { .numberedImmediate = NumberedImmediate { mentionOfName(value->key()) }, .isOnRarePath = true }).first));
    return join.finish();
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
    return Condition(m_out.castToInt32(callStub(context, inById(PresenceQuestion::In), { receiver.value() }, { .numberedImmediate = NumberedImmediate { mentionOfName(key) } }).first));
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

TypedPointer AccessEmitter::globalBindingLocationOrElse(Pointer record, bool seesScriptBindings, LBasicBlock miss)
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
    return TypedPointer(m_heaps.properties.atAnyNumber(), add(location, butterflyIfNegative).value());
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
        GlobalBindingScope scope = seesScriptBindings ? GlobalBindingScope::ScriptsAndGlobalObject : GlobalBindingScope::GlobalObject;
        define(value, Boxed(callStub(value, AOT::getGlobalBinding(scope), { }, { .numberedImmediate = NumberedImmediate { m_mentions.globalBinding(key, flags) } }).first));
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
    if (std::optional<NumberedImmediate> answers = answersOf(value, key, SiteUse::Read)) {
        NameAtSite name = m_plan.slots().hasColumn(key) ? NameAtSite::HasColumn : NameAtSite::IsAnyName;
        define(value, Boxed(callStub(value, getterAtSite(name), { object.value() }, { .numberedImmediate = *answers }).first));
        return;
    }
    define(value, Boxed(callStub(value, getterByAccessTable(), { object.value() }, { .numberedImmediate = NumberedImmediate { mentionOfName(key) } }).first));
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
    bool endsUnit = terminator.kind == TerminatorKind::ReturnCall && terminator.callOperand() == behind.front();
    if (call.isTailCall && (endsUnit || !findPatternLeftToCall(read)))
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
    for (ValueID argument : call.argumentsOperands()) {
        ValueID made = inFrontOfBox(argument);
        if ((std::ranges::contains(behind, made) && made != pattern) || made == read.id())
            return;
    }
    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    auto boxed = [&](ValueID id) {
        ValueID made = inFrontOfBox(id);
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
    Boxed answer = calls.callByConvention(read, conventionCall);
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
    if (!read.pattern && !lentArguments && calls.tryCallThroughMethodTable(call, read.key))
        return true;
    Vector<LValue, 8> arguments;
    for (ValueID argument : call->argumentsOperands())
        arguments.append(in<Boxed>(argument).value());
    const unsigned count = static_cast<unsigned>(arguments.size());
    std::optional<BuiltinBoxedStub> boxedCall = call->methodOfReceiverAsMade() ? m_program.builtins().boxedCallStubOf(call->methodOfReceiverAsMade()) : std::nullopt;
    bool goesByClass = boxedCall && servesCallsByName(*boxedCall, count);
    std::optional<CompiledMethodOfArray> compiledMethod = !goesByClass && call->methodOfReceiverAsMade() && count <= maxArgumentCountOfCompiledMethodOfArray ? compiledMethodOfArray(call->methodOfReceiverAsMade()) : std::nullopt;
    std::optional<NumberedImmediate> answers = (!goesByClass || call->methodIsNamedByKeyAlone()) && !compiledMethod && m_plan.slots().hasColumn(read.key) ? answersOf(call, read.key, SiteUse::Call) : std::nullopt;
    bool standsBehindFailedLookInLoop = goesByClass && m_code.at(call.block()).frequency == BlockFrequency::Rare && m_generator.paysForCodeInLine(call.block());
    if (!answers && standsBehindFailedLookInLoop && !read.pattern && m_plan.slots().hasColumn(read.key) && !isSymbolOfRealm(read.key) && comesAgain(call))
        answers = answersAtSite(call, read.key, SiteUse::Call);
    RELEASE_ASSERT((!read.pattern || goesByClass) && (!lentArguments || !goesByClass));
    StubKey byClass = !goesByClass ? StubKey() : read.pattern ? (answers ? callMethodOfBuiltinClassAtSiteWithSharedPattern(*boxedCall, count) : callMethodOfBuiltinClassWithSharedPattern(*boxedCall, count))
        : answers ? callMethodOfBuiltinClassAtSite(*boxedCall, count)
        : calls.isProvenReceiverOf(*boxedCall, typeOf(call->thisValueOperand())) ? callMethodOfProvenBuiltinClass(*boxedCall, count) : callMethodOfBuiltinClass(*boxedCall, count);
    ConventionCall conventionCall {
        .stub = goesByClass ? byClass : compiledMethod ? callCompiledMethodOfArray(*compiledMethod, count, lentArguments) : answers ? callMethodAtSite(count, lentArguments) : callMethodById(count, lentArguments),
        .thisValue = in<Boxed>(call->thisValueOperand()).value(),
        .arguments = arguments.span(),
    };
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

void AccessEmitter::branchOnTruthAtSite(const ValueContext& read)
{
    PropertyKeyID key = read->key();
    ValueID receiverID = read->objectOperand();
    Boxed receiver = in<Boxed>(receiverID);
    m_out.noteForm(Form::Closed, StateOfKnowledge::Always);
    std::optional<NumberedImmediate> answers = answersOfKeyThatIsNoSymbolOfRealm(read, key, SiteUse::Read);
    if (!answers) {
        emitter<TruthinessEmitter>().branchOnTruthiness(read, getById(read, receiver, key, receiverID), typeOf(read.id()), read.branchTargets());
        return;
    }
    NameAtSite name = m_plan.slots().hasColumn(key) ? NameAtSite::HasColumn : NameAtSite::IsAnyName;
    ReceiverAtSite receiverIs = typeOf(receiverID).isObject() ? ReceiverAtSite::IsCell : ReceiverAtSite::IsAnyValue;
    LValue operands[] = { receiver.value() };
    emitter<RuntimeCallEmitter>().callStubAndBranch(read, readTruthAtSite(receiverIs, name), operands, { .numberedImmediate = *answers }, read.branchTargets());
}

void AccessEmitter::generatePropGet(const ValueContext& value)
{
    if (value->isFreeName()) {
        m_out.noteForm(Form::Runtime, StateOfKnowledge::FreeName);
        define(value, getFreeName(value));
        return;
    }
    if (value.isBranchCondition()) {
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
    if (std::optional<Boxed> method = lookWhetherReceiverIsAsMadeInPlaceOfRead(value)) {
        define(value, *method);
        return;
    }
    define(value, getNamedProperty(value));
}

std::optional<Boxed> AccessEmitter::lookWhetherReceiverIsAsMadeInPlaceOfRead(const ValueContext& read)
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
    if (call.opcode != Opcode::Call || !call.methodOfReceiverAsMade() || call.methodIsNamedByKeyAlone() || call.isTailCall)
        return std::nullopt;
    if (call.calleeOperand() != read.id() || !call.thisValueOperand() || inFrontOfBox(call.thisValueOperand()) != inFrontOfBox(read->objectOperand()) || std::ranges::contains(call.argumentsOperands(), read.id()))
        return std::nullopt;
    CallEmitter& calls = emitter<CallEmitter>();
    const unsigned count = static_cast<unsigned>(call.argumentsOperands().size());
    std::optional<BuiltinBoxedStub> boxedCall = m_program.builtins().boxedCallStubOf(call.methodOfReceiverAsMade());
    if (!boxedCall || !servesCallsByName(*boxedCall, count) || !looksWhetherReceiverIsAsMade(*boxedCall) || !calls.isProvenReceiverOf(*boxedCall, typeOf(read->objectOperand())))
        return std::nullopt;
    if (calls.mayForwardCallToReceiver(call) || argumentsLentBy(m_code, call))
        return std::nullopt;
    const Terminator& terminator = m_code.at(m_code.blockOf(onlyUser)).terminator;
    if (terminator.kind == TerminatorKind::ReturnCall && terminator.callOperand() == onlyUser)
        return std::nullopt;
    std::optional<NumberedImmediate> answers = answersOfKeyThatIsNoSymbolOfRealm(read, key, SiteUse::Read);
    if (!answers)
        return std::nullopt;
    m_out.noteForm(Form::Closed, StateOfKnowledge::Always);
    m_methodsThatMayBeUnread.set(read.id().index());
    NameAtSite name = m_plan.slots().hasColumn(key) ? NameAtSite::HasColumn : NameAtSite::IsAnyName;
    return Boxed(callStub(read, readAtSiteUnlessAsMade(name), { in<Boxed>(read->objectOperand()).value() }, { .numberedImmediate = *answers }).first);
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
    putById(value, object, key, stored, mode, site);
}

void AccessEmitter::generatePropSet(const ValueContext& value)
{
    if (value->isFreeName()) {
        putFreeName(value);
        return;
    }
    putNamedProperty(value, in<Boxed>(value->objectOperand()), putModeOf(value->mode()));
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
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void AccessEmitter::generateElemGet(const ValueContext& value)
{
    Boxed object = in<Boxed>(value->objectOperand());
    std::optional<ElementKey> key = elementKeyOf(value);
    if (!key) {
        define(value, emitter<ConstantEmitter>().zeroOf(value.representation()));
        return;
    }
    StubKey stub = getByValCached();
    std::optional<AOT::ExpectedElements> expected = stubFormOfExpectedElements(value->expectedElements());
    if (expected)
        stub = getByValExpecting(key->isInteger, *expected);
    else if (key->isInteger || value->keyIsIntegral())
        stub = AOT::getByVal(key->isInteger);
    else if (typeOf(value->objectOperand()).isObject() && typeOf(value->propertyOperand()).isString())
        stub = getByValCachedCells();

    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    const PlacedSlot* extraPropertiesWord = elements.extraPropertiesWordOfReadByPosition(value);
    if (!extraPropertiesWord) {
        define(value, Boxed(callStub(value, stub, { object.value(), key->value }).first));
        return;
    }
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
    {
        Machine::DeclaredAccessesSuspension suspension(m_out);
        join.add(elements.readElementOfExtraProperties(unboxCell(object), *extraPropertiesWord, index, outOfLine));
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
    callStub(value, stub, { object.value(), key->value, stored.value() });
}

void AccessEmitter::generateElemHas(const ValueContext& value)
{
    Boxed object = in<Boxed>(value->objectOperand());
    defineTest(value, hasByVal(value, object, in<Boxed>(value->propertyOperand())));
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
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
