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
#include "AOTConstantEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTConvention.h"
#include "AOTElementAccessEmitter.h"
#include "AOTFieldTable.h"
#include "AOTIRProgram.h"
#include "AOTNameClass.h"
#include "AOTPropertyCachesFront.h"
#include "AOTResultRules.h"
#include "AOTRuntimeWords.h"
#include "AOTSlotEmitter.h"
#include "AOTStubCalls.h"
#include "AOTStubsBoxedCalls.h"
#include "AOTStubsCompiledMethods.h"
#include "AOTStubsElements.h"
#include "AOTStubsGlobalBindings.h"
#include "AOTStubsPropertyKeys.h"
#include "AOTTypeArena.h"
#include "ECMAMode.h"
#include "GetterSetter.h"
#include "PropertyName.h"
#include <limits>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

// Image gives a name that has a column (SlotPlan::hasColumn()) a number of at most RowDisplacementTable::maxColumn. getById() and putById() look in the access table and then in the field table under that number, and do not test that the number fits an entry there.
// Where columns are left over, a name without a promise gets such a number too, and the run time files its slots by the number. getByIdCheckingColumn() and putByIdHashed() ask the number, and go on as the two above where it has a column.
static_assert(RowDisplacementTable::maxColumn <= FieldTableEntry::maxName);

bool AccessEmitter::isSymbolOfRealm(PropertyKeyID key) const
{
    return !liesInThread(key) && m_program.at(key).kind == PropertyKeyKind::CreatedSymbol && !hasOneInstance();
}

Boxed AccessEmitter::cellOfKey(const OperationContext& context, PropertyKeyID key)
{
    return boxCell(emitter<ConstantEmitter>().keyConstant(context, key));
}

// What a table remembers of an access pays where the access comes again. The body of a CommonJS module is not proven to run once, and runs again only after its entry left the cache of `require`.
// It chooses between two roads to one answer, so nothing goes wrong where the program runs in several instances.
bool AccessEmitter::comesAgain(const OperationContext& context) const
{
    return m_generator.isInLoop(context.block()) || !(m_facts.runsAtMostOnce(m_id) || m_program.at(m_id).isModuleBody());
}

// A site pays where one kind of receiver comes again and again, which is in a loop. Elsewhere the front tables answer for every Structure alike, in about what a hit at a site and the operand that names the site cost together.
// A name without a column in them is looked for by its hash. Outside of loops the call names the property and nothing else.
std::optional<Pointer> AccessEmitter::siteOf(const OperationContext& context, PropertyKeyID key, SlotAccess access)
{
    if (isSymbolOfRealm(key) || !m_generator.paysForCodeInLine(context.block()))
        return std::nullopt;
    return siteCacheAddress(context, key, access);
}

std::optional<NumberedImmediate> AccessEmitter::answersOf(const OperationContext& context, PropertyKeyID key, SiteUse use)
{
    if (isSymbolOfRealm(key) || !comesAgain(context))
        return std::nullopt;
    // The arm behind a test that seldom fails: it is come by too seldom for the bytes of a place of its own. So is the closed side of an operation that tests its own guess.
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

Boxed AccessEmitter::getById(const OperationContext& context, Boxed receiver, PropertyKeyID key, std::optional<Pointer> site, ValueID receiverID)
{
    if (isSymbolOfRealm(key))
        return Boxed(callStub(context, getByIdOfSymbolOfRealm(), { receiver.value() }, { .numberedImmediate = NumberedImmediate { mentionOfName(key) } }).first);
    CallOptions options { .numberedImmediate = NumberedImmediate { mentionOfName(key) } };
    const bool hasColumn = m_plan.slots().hasColumn(key);
    const bool isLength = hasColumn && m_program.nameClassOf(key) == NameClass::Length;
    if (site) {
        StubKey stub = isLength ? getLengthWithSite() : stubThatReadsAtSite(key, receiverID);
        return Boxed(callStub(context, stub, { receiver.value(), site->value() }, options).first);
    }
    if (std::optional<NumberedImmediate> answers = answersOf(context, key, SiteUse::Read)) {
        NameAtSite name = hasColumn ? NameAtSite::HasColumn : NameAtSite::IsAnyName;
        ReceiverAtSite receiverIs = receiverID && typeOf(receiverID).isObject() ? ReceiverAtSite::IsCell : ReceiverAtSite::IsAnyValue;
        StubKey stub = m_program.nameClassOf(key) == NameClass::Length ? readLengthAtSite(name) : tryReadAtSiteOfNarrowField(m_plan.slots().narrowFormOf(key), receiverIs, name).value_or(readAtSite(receiverIs, name));
        return Boxed(callStub(context, stub, { receiver.value() }, { .numberedImmediate = *answers }).first);
    }
    if (isLength)
        return Boxed(callStub(context, getLengthAtSite(), { receiver.value() }, options).first);
    StubKey stub = !hasColumn ? getByIdCheckingColumn() : m_plan.slots().narrowFormOf(key) == SlotRepresentation::JSValue ? AOT::getById() : getByIdAskingFieldTableFirst();
    return Boxed(callStub(context, stub, { receiver.value() }, options).first);
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
        // The table of fields answers for a compiled object without a first time. Nothing else is asked, and nothing is filed.
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
        callStub(context, putByIdAtSite(mode), { receiver.value(), value.value(), site->value() }, options);
        return;
    }
    StubKey stub = hasColumn ? AOT::putById(mode) : putByIdHashed(mode);
    callStub(context, stub, { receiver.value(), value.value() }, options);
}

Boxed AccessEmitter::loadBySlotAtSite(CellPointer object, Pointer site, Slot::Kind kind, LBasicBlock whenSlotIsOfOtherKind, LBasicBlock whenSlotIsOfOtherStructure)
{
    RELEASE_ASSERT(Slot::payloadOf(kind) == SlotPayload::Holder && Slot::locationOf(kind) == SlotLocation::WordIndex);
    // Each part of the slot is compared where it lies.
    auto partOfSlot = [&](ptrdiff_t offset) {
        return m_out.address(m_heaps.AOTInstance_siteCaches, site.value(), offset);
    };
    Word32 structureIDOfSlot(m_out.load32(partOfSlot(Slot::offsetOfStructureID())));
    branchUnless(equal(structureIDOfSlot, loadStructureID(object)), whenSlotIsOfOtherStructure);
    branchUnless(equal(Word32(m_out.load8ZeroExt32(partOfSlot(Slot::offsetOfKind()))), static_cast<int32_t>(kind)), whenSlotIsOfOtherKind);
    // An own property has no holder.
    Pointer holder(m_out.load64(partOfSlot(Slot::offsetOfPayload())));
    Pointer owner = select(isZero(Integer(holder.value())), asPointer(object), holder);
    Pointer butterfly = loadField<Pointer>(owner, m_heaps.JSObject_butterfly);
    Pointer storage = select(hasBits(Word32(m_out.load8ZeroExt32(partOfSlot(Slot::offsetOfFlags()))), Slot::isOutOfLine), butterfly, owner);
    Integer wordIndex = signExtended(Word32(m_out.load16SignExt32(partOfSlot(Slot::offsetOfLocation()))));
    return Boxed(m_out.load64(m_out.address(m_heaps.root, offsetBy(storage, wordIndex, 3).value())));
}

// No layout of the receivers has the name, so the answer lies above the object: `class Job extends EventEmitter { }`, `job.listenerCount`. The site is asked in line.
Boxed AccessEmitter::getMemberAboveLayouts(const ValueContext& value, Boxed receiver, Pointer site)
{
    LBasicBlock asksStub = newBlock();
    Join<Boxed> join(m_out);
    {
        // The holder may keep the value in a field or among its properties, and the effects of the read name neither.
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
        // The slot of an accessor holds its functions, and a field that is declared with other attributes than an assignment gives is created beside its slot. A field that Link appended has no record.
        std::optional<FieldIndex> index = m_plan.slots().findField(m_program, member, key);
        const Layout& layout = m_program.at(member);
        if (!index || (*index < layout.fields.size() && !layout.fields[*index].form.isEmpty()))
            return std::nullopt;
    }
    return slot;
}

// Every receiver has the slot of the field at one place in its cell, and what it holds is the value as it is. Filled, it is the answer.
// `Message.prototype.retries = 3`: empty, the object may still have the property elsewhere, or something above it has, and the read goes by name.
Boxed AccessEmitter::getMemberInSlotOrByName(const ValueContext& value, Boxed receiver, const PlacedSlot& slot)
{
    PropertyKeyID key = value->key();
    LBasicBlock isFilled = newBlock();
    LBasicBlock isEmpty = newBlock();
    Join<Boxed> join(m_out);
    // The operation says that it reads whatever there is, and names no field.
    Boxed word(m_out.load64(m_out.address(m_heaps.root, unboxCell(receiver).value(), slot.offset)));
    ScopeFork fork = m_out.forkScope();
    m_out.branch(isZero(Integer(word.value())).value(), unlikelyEdge(isEmpty), normalEdge(isFilled));
    m_out.appendToArm(isFilled, fork);
    join.add(word);

    m_out.appendToArm(isEmpty, fork);
    join.add(getById(value, receiver, key, std::nullopt, value->objectOperand()));
    return join.finish();
}

Boxed AccessEmitter::getNamedProperty(const ValueContext& value)
{
    ValueID receiverID = value->objectOperand();
    PropertyKeyID key = value->key();
    Boxed receiver = in<Boxed>(receiverID);
    Type receiverType = typeOf(receiverID);
    bool receiversAreListed = value->receivers() || (receiverType.layouts && receiverType.hasOnlyLayouts());
    bool isLength = !liesInThread(key) && m_program.nameClassOf(key) == NameClass::Length;
    bool asksSiteInLine = m_generator.paysForCodeInLine(value.block()) && receiverType.isObject() && receiversAreListed && !isLength;
    if (std::optional<PlacedSlot> slot = asksSiteInLine ? findSlotInCellOfEveryReceiver(receiverType, key) : std::nullopt) {
        m_out.noteForm(Form::Open, StateOfKnowledge::Always, Place::Loop);
        return getMemberInSlotOrByName(value, receiver, *slot);
    }
    std::optional<Pointer> site = asksSiteInLine ? siteOf(value, key, SlotAccess::Read) : std::nullopt;
    if (!site || !asksSiteInLine) {
        m_out.noteForm(Form::Closed, StateOfKnowledge::Always);
        return getById(value, receiver, key, site, receiverID);
    }
    m_out.noteForm(Form::Open, StateOfKnowledge::Always, Place::Loop);

    // What a slot says changes wherever the run time comes by, and no effect of an operation speaks of that: what one look at the site has read, no other look finds again.
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

    // The stub looks at the record's constant and then at the filed location. A record that files its location alone has no constant but the undefined that typeof answers.
    if (!m_generator.takesOpenForm(value)) {
        GlobalBindingScope scope = seesScriptBindings ? GlobalBindingScope::ScriptsAndGlobalObject : GlobalBindingScope::GlobalObject;
        define(value, Boxed(callStub(value, AOT::getGlobalBinding(scope), { }, { .numberedImmediate = NumberedImmediate { m_mentions.globalBinding(key, flags) } }).first));
        return;
    }

    Pointer record = globalBindingAddress(key, flags);
    LBasicBlock slowPath = newBlock();
    Join<Boxed> join(m_out);

    // For a name that nothing is bound to, the run time files the undefined that typeof answers as the record's constant (GlobalBindings::fileAbsence()), even if the program stores to the name through a binding.
    // A name that the program does not store through a binding is read as the record's constant, and else at the filed location.
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
    // The first store through a record files it, so a store that never comes again has nothing to find there.
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
    barrierAfterStore(value, globalObject(), stored, typeOf(value->valueOperand()));
    m_out.jump(continuation);

    m_out.appendTo(slowPath);
    callVoidOperation(value, operation, { numberOf(m_mentions.globalBinding(key, flags)), stored }, { .isOnRarePath = true });
    m_out.jump(continuation);

    m_out.appendTo(continuation);
}

// What the front cell of the Structure and the name says holds of every object with that Structure: an accessor that lies in an object above, in the cell or in the butterfly. The stub that reads by name goes by the same cell, and files it.
// Everything else is no answer. An accessor of the object's own is none: a compiled object keeps it in a field or among its extra properties, and the cell then says so.
CellPointer AccessEmitter::accessorAboveObjectByFrontCell(CellPointer object, PropertyKeyID key, LBasicBlock findsNoGetter)
{
    constexpr ptrdiff_t front = AOT_INSTANCE_OFFSET(propertyCaches) + PropertyCacheWords::offsetOfFront(SlotAccess::Read);
    constexpr int32_t everyBitButIsOutOfLine = static_cast<uint8_t>(~PackedSlot::isOutOfLine);
    static_assert(sizeof(PackedSlot) == sizeof(uint64_t) && Slot::payloadOf(Slot::Kind::Getter) == SlotPayload::Holder && Slot::locationOf(Slot::Kind::Getter) == SlotLocation::WordIndex);
    const Machine::AbstractHeap& tables = m_heaps.AOTPropertyCaches_front;
    // The table grows by moving, and the pages that it leaves are unmapped. Many a stub that declares no write files in it behind a miss. So no lookup goes by what an earlier one has read of it.
    Machine::PatchOperation* endsEarlierReads = m_out.patchpoint(Machine::Void);
    endsEarlierReads->effects = Machine::Effects::none();
    m_heaps.decoratePatchpointWrite(&tables, endsEarlierReads);
    endsEarlierReads->append(object.value(), Machine::ValueRep::ColdAny);
    endsEarlierReads->setRunsWithoutFrame(true);
    endsEarlierReads->onlyKeepsChildrenAlive = true;
    const Machine::AbstractHeap& values = m_heaps.properties.atAnyNumber();

    Word32 name = identifierNumber(key);
    // Without a promise of a column Image may give the name a number behind the last column, and no row reaches that far.
    if (!m_plan.slots().hasColumn(key))
        branchUnless(belowOrEqual(name, static_cast<int32_t>(RowDisplacementTable::maxColumn)), findsNoGetter);

    Integer granule = zeroExtended(shiftRightUnsigned(loadStructureID(object), static_cast<int32_t>(StructureRowWords::granuleShift)));
    Pointer rows = loadImmutablePointer(front + OBJECT_OFFSETOF(FrontWords, rowsForStructureIDZero));
    Word32 row(m_out.load32(m_out.address(tables, offsetBy(rows, granule, StructureRowWords::wordShift).value())));
    Pointer plane(m_out.loadPtr(instanceField(tables, front + OBJECT_OFFSETOF(FrontWords, plane))));
    Pointer cell = offsetBy(plane, zeroExtended(add(row, name)), 3);
    auto partOfCell = [&](ptrdiff_t offset) {
        return m_out.address(tables, cell.value(), offset);
    };

    branchUnless(equal(Word32(m_out.load16ZeroExt32(partOfCell(PackedSlot::offsetOfName()))), name), findsNoGetter);
    Word32 kindAndFlags = bitXor(Word32(m_out.load8ZeroExt32(partOfCell(PackedSlot::offsetOfBiasedKindAndFlags()))), static_cast<int32_t>(PackedSlot::ownInlineValueBits));
    branchUnless(equal(bitAnd(kindAndFlags, everyBitButIsOutOfLine), static_cast<int32_t>(Slot::Kind::Getter)), findsNoGetter);

    CellPointer holder = decode(NarrowReference(m_out.load32(partOfCell(PackedSlot::offsetOfPayload()))));
    Pointer butterfly = loadField<Pointer>(holder, m_heaps.JSObject_butterfly);
    Pointer storage = select(hasBits(kindAndFlags, PackedSlot::isOutOfLine), butterfly, asPointer(holder));
    Integer wordIndex = signExtended(Word32(m_out.load8SignExt32(partOfCell(PackedSlot::offsetOfLocation()))));
    return CellPointer(m_out.load64(m_out.address(values, offsetBy(storage, wordIndex, 3).value())));
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

    // AccessTable::answerOfGetterInOwnStorage(): the read by name behind a miss comes to the handler of the getter, which files it.
    CellPointer object = in<CellPointer>(value->objectOperand());
    if (std::optional<NumberedImmediate> answers = answersOf(value, key, SiteUse::Read)) {
        NameAtSite name = m_plan.slots().hasColumn(key) ? NameAtSite::HasColumn : NameAtSite::IsAnyName;
        define(value, Boxed(callStub(value, getterAtSite(name), { object.value() }, { .numberedImmediate = *answers }).first));
        return;
    }
    define(value, Boxed(callStub(value, getterByAccessTable(), { object.value() }, { .numberedImmediate = NumberedImmediate { mentionOfName(key) } }).first));
}

// The tests in front of it say that the lookup finds the accessor where its class has defined it. Where the front cell does not say where that is, the run time looks.
void AccessEmitter::generateGetAccessorOfMember(const ValueContext& value)
{
    if (value.representation() == Representation::Void) {
        define(value, { });
        return;
    }
    PropertyKeyID key = value->key();
    // The walk builds none under such a key: canCallAccessorOfMember().
    if (isSymbolOfRealm(key)) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        define(value, emitter<ConstantEmitter>().zeroOf(value.representation()));
        return;
    }

    CellPointer object = in<CellPointer>(value->objectOperand());
    LBasicBlock asksRunTime = newBlock();
    Join<CellPointer> join(m_out);
    CellPointer accessor = accessorAboveObjectByFrontCell(object, key, asksRunTime);
    ptrdiff_t offsetOfHalf = value->isSetter() ? GetterSetter::offsetOfSetter() : GetterSetter::offsetOfGetter();
    join.add(CellPointer(m_out.load64(m_out.address(m_heaps.properties.atAnyNumber(), accessor.value(), offsetOfHalf))));

    m_out.appendTo(asksRunTime);
    Word32 isSetter(m_out.constInt32(value->isSetter()));
    join.add(callOperation<CellPointer>(value, RuntimeOperation::FindAccessorOfMember, { object, identifierNumber(key), isSetter }, { .isOnRarePath = true }));
    define(value, join.finish());
}

// It stands in front of a loop and runs once for all the turns, so it asks the run time. The empty value: only a read by name can answer.
void AccessEmitter::generateGetCachedValue(const ValueContext& value)
{
    if (value.representation() == Representation::Void) {
        define(value, { });
        return;
    }
    PropertyKeyID key = value->key();
    Boxed nothing = boxedConstant(JSValue());
    if (isSymbolOfRealm(key)) {
        define(value, nothing);
        return;
    }

    Boxed receiver = in<Boxed>(value->objectOperand());
    LBasicBlock isNoCell = newBlock();
    Join<Boxed> join(m_out);
    branchUnless(isCell(receiver), isNoCell);
    join.add(callOperation<Boxed>(value, RuntimeOperation::ReadValueWithoutCode, { unboxCell(receiver), identifierNumber(key) }));

    m_out.appendTo(isNoCell);
    join.add(nothing);
    define(value, join.finish());
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
    // In a copy the mark of a tail call says what a stack trace lists. A call that the unit itself ends with is returnWhatMethodOfBuiltinClassGives()'s.
    const Terminator& terminator = m_code.at(read.block()).terminator;
    bool endsUnit = terminator.kind == TerminatorKind::ReturnCall && terminator.callOperand() == behind.front();
    if (call.isTailCall && (endsUnit || !findPatternLeftToCall(read)))
        return false;
    if (isSymbolOfRealm(read->key()) || !typeOf(read.id()).hasObjects())
        return false;
    // `list.map(x => x + 1)`, `table.get(key)` on a cell, `text.slice(i + 1)`: a closure that is made, a value that is put into another form and whatever touches no memory and cannot throw
    // do nothing that the read could see, and see nothing that the read could do.
    auto standsUnseenInBetween = [&](const Operation& operation) {
        return operation.opcode == Opcode::Box || operation.opcode == Opcode::NewClosure || operation.opcode == Opcode::NewRegExp || operation.traits().isPure();
    };
    while (!behind.empty() && behind.front() != onlyUser && standsUnseenInBetween(operationAt(behind.front())))
        behind = behind.subspan(1);
    if (behind.empty() || behind.front() != onlyUser)
        return false;
    std::span<const ValueID> arguments = call.argumentsOperands();
    if (arguments.size() > numberOfArgumentGPRs)
        return false;
    for (ValueID argument : arguments) {
        if (argument == read.id())
            return false;
        // A closure that is lent goes through the stub that knows whether the callee borrows it.
        const Operation* made = &operationAt(argument);
        if (made->opcode == Opcode::Box)
            made = &operationAt(made->operand(0));
        if (made->opcode == Opcode::NewClosure && made->isLent())
            return false;
    }
    return true;
}

// isReadOfMethodCalledNext() has said yes. `text.replace(/a/g, "b")`: the pattern is written for the call alone, and the stub that goes by the class of the receiver hands it to nobody and leaves it as it was made.
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
    if (pattern.opcode != Opcode::NewRegExp || pattern.isNeverToldApart() || pattern.regExpFlags().contains(RegExpFlags::Sticky) || m_code.onlyUserOf(made) != boxed)
        return { };
    // What stands in front of the read is made already.
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
    // Whoever is handed a pattern under such a name is a string, whatever else has a method of that name.
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

    // Behind the read stands only what nobody sees, and each operand is at hand already, but for its boxed form.
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
    if (!read.pattern && calls.tryCallThroughMethodTable(call, read.key))
        return true;
    Vector<LValue, 8> arguments;
    for (ValueID argument : call->argumentsOperands())
        arguments.append(in<Boxed>(argument).value());
    const unsigned count = static_cast<unsigned>(arguments.size());
    // The stub of such a place has no form for a name without a column, whose number may not fit the two bytes that ImageSection::NamesOfSites has for a number.
    // The walk has named what a receiver as its class makes it finds under the name: the stub looks at the receiver, and needs no words at the site.
    std::optional<BuiltinBoxedStub> boxedCall = call->methodOfReceiverAsMade() ? m_program.builtins().boxedCallStubOf(call->methodOfReceiverAsMade()) : std::nullopt;
    bool goesByClass = boxedCall && servesCallsByName(*boxedCall, count);
    // The body of the method is compiled: the stub looks at the receiver and enters it.
    std::optional<CompiledMethodOfArray> compiledMethod = !goesByClass && call->methodOfReceiverAsMade() && count <= maxArgumentCountOfCompiledMethodOfArray ? compiledMethodOfArray(call->methodOfReceiverAsMade()) : std::nullopt;
    // Where only the name speaks for the class, most receivers are of another: they find the words of the site as before.
    std::optional<NumberedImmediate> answers = (!goesByClass || call->methodIsNamedByKeyAlone()) && !compiledMethod && m_plan.slots().hasColumn(read.key) ? answersOf(call, read.key, SiteUse::Call) : std::nullopt;
    RELEASE_ASSERT(!read.pattern || goesByClass);
    StubKey byClass = !goesByClass ? StubKey() : read.pattern ? (answers ? callMethodOfBuiltinClassAtSiteWithSharedPattern(*boxedCall, count) : callMethodOfBuiltinClassWithSharedPattern(*boxedCall, count))
        : answers ? callMethodOfBuiltinClassAtSite(*boxedCall, count) : callMethodOfBuiltinClass(*boxedCall, count);
    ConventionCall conventionCall {
        .stub = goesByClass ? byClass : compiledMethod ? callCompiledMethodOfArray(*compiledMethod, count) : answers ? callMethodAtSite(count) : callMethodById(count),
        .thisValue = in<Boxed>(call->thisValueOperand()).value(),
        .arguments = arguments.span(),
    };
    // `const nodes = list.slice(); nodes.pos = pos`: the first named property of a list asks for storage of its own, unless the list was born with the room.
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

void AccessEmitter::generatePropGet(const ValueContext& value)
{
    if (value->isFreeName()) {
        m_out.noteForm(Form::Runtime, StateOfKnowledge::FreeName);
        define(value, getFreeName(value));
        return;
    }
    if (isReadOfMethodCalledNext(value)) {
        // Nobody looks at it: the call reads.
        m_out.noteForm(Form::Closed, StateOfKnowledge::Always);
        m_readLeftToCall = ReadLeftToCall { value.id(), value->key(), value.origin(), findPatternLeftToCall(value) };
        define(value, undefined());
        return;
    }
    returnWhatMethodOfBuiltinClassGives(value);
    define(value, getNamedProperty(value));
}

std::optional<AccessEmitter::StoreOfKnownField> AccessEmitter::findStoreOfKnownField(const Type& receiver, PropertyKeyID key, PutMode mode)
{
    // A type that lists layouts does not say that the receiver is of the realm that runs, where alone the number names the symbol.
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
    std::optional<Pointer> site = value.opcode() == Opcode::PropSet ? siteOf(value, key, accessFor(mode)) : std::nullopt;
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
    // `{ 2: value }` defines an element.
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
    // The tables have slots for objects alone.
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
    case Representation::LiteralString:
    case Representation::RawWord:
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// A stub that expects one kind of view goes on to the stub that takes everything where it meets anything else.
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
    if (key->isInteger || value->keyIsIntegral()) {
        std::optional<AOT::ExpectedElements> expected = stubFormOfExpectedElements(value->expectedElements());
        stub = expected ? getByValExpecting(key->isInteger, *expected) : AOT::getByVal(key->isInteger);
    } else if (typeOf(value->objectOperand()).isObject() && typeOf(value->propertyOperand()).isString())
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
        // The elements lie in another object than the receiver, and the effects of the read speak of the receiver.
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
    if (key->isInteger || value->keyIsIntegral()) {
        JSC::ECMAMode mode = forMode(value->mode(), JSC::ECMAMode::strict(), JSC::ECMAMode::sloppy());
        std::optional<AOT::ExpectedElements> expected = stubFormOfExpectedElements(value->expectedElements());
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
