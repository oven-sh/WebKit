/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTAccessEmitter.h"

#if ENABLE(AOT)

#include "AOTArrivalSiteNumbers.h"
#include "AOTConstantEmitter.h"
#include "AOTFieldTable.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIdentifierNumbering.h"
#include "AOTNameClass.h"
#include "AOTPlacement.h"
#include "AOTPropertyCachesFront.h"
#include "AOTRuntimeWords.h"
#include "AOTShapeTestEmitter.h"
#include "AOTStubCatalog.h"
#include "AOTStubsElements.h"
#include "AOTStubsGlobalBindings.h"
#include "AOTStubsPropertyInlineCode.h"
#include "AOTStubsPropertyKeys.h"
#include "AOTVector.h"
#include "AllowMacroScratchRegisterUsage.h"
#include "B3PatchpointValue.h"
#include "B3StackmapGenerationParams.h"
#include "ECMAMode.h"
#include "GetterSetter.h"
#include "PropertyName.h"
#include <limits>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static bool probesFreeNameInline(CodeShape shape)
{
    switch (shape) {
    case CodeShape::Balanced:
    case CodeShape::Fast:
        return true;
    case CodeShape::Unchosen:
    case CodeShape::Compact:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// A binding that is no constant is one that the program stores to. The stub behind a read tests the record as code in line does. So does the operation behind a store, in front of everything else.
static bool probesBindingLocationInline(CodeShape shape)
{
    return shape == CodeShape::Fast;
}

std::optional<uint32_t> AccessEmitter::findIdentifierNumber(const OperationContext& context, PropertyKeyID key)
{
    std::optional<uint32_t> number = build().identifierNumbering().numberOf(key);
    if (!number)
        fail(context, InternalErrorKind::OpcodeWithoutCode);
    return number;
}

// What a table remembers of an access pays where the access comes again. The body of a CommonJS module is not proven to run once, and runs again only after its entry left the cache of `require`.
bool AccessEmitter::comesAgain(const OperationContext& context)
{
    return mayReadSiteCachesAgain(analyses(), context.block());
}

// The call has the effects of the operation and not those of the access it stands before. An access that writes nothing would leave a call that writes nothing, which B3 removes.
void AccessEmitter::recordArrival(const OperationContext& context, Boxed receiver, uint32_t part)
{
    const ArrivalSiteNumbers& sites = inputs().arrivalSites;
    if (!sites.isCounting())
        return;
    std::optional<uint32_t> number = sites.numberOf(m_id, context.valueID(), part);
    if (!number)
        return;
    Operand arguments[] { receiver, word32(static_cast<int32_t>(*number)) };
    callWithoutSite(RuntimeOperation::RecordArrival, arguments, Representation::Void, { .isOnRarePath = true });
}

static RuntimeOperation putOperationWithoutCachesFor(PutMode mode)
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

StubKey AccessEmitter::stubThatReadsAtSite(uint32_t identifierNumber, ValueID receiver)
{
    if (!RowDisplacementTable::hasColumn(identifierNumber))
        return getByIdAtSiteHashed();
    return receiver && typeOf(receiver).isObject() ? getByIdOfObjectAtSite() : getByIdAtSite();
}

Boxed AccessEmitter::getById(const OperationContext& context, Boxed receiver, PropertyKeyID key, SiteCacheID cache, std::optional<Origin> origin, ValueID receiverID)
{
    recordArrival(context, receiver);
    std::optional<uint32_t> identifier = findIdentifierNumber(context, key);
    if (!identifier)
        return undefined();
    CallOptions options { .immediate = *identifier, .origin = origin };
    const bool hasColumn = RowDisplacementTable::hasColumn(*identifier);
    auto readThroughTables = [&] {
        StubKey stub = hasColumn ? AOT::getById() : getByIdHashed();
        return Boxed(callStub(context, stub, { receiver.value() }, options).first);
    };
    // Behind the tables the stub asks the objects themselves and files nothing, so code that runs once takes the way of all other code and leaves nothing behind.
    if (!comesAgain(context))
        return readThroughTables();
    if (cache && hasColumn && m_program.nameClassOf(key) == NameClass::Length)
        return Boxed(callStub(context, getLengthAtSite(), { receiver.value(), siteCacheAddress(cache).value() }, options).first);
    // A site cache pays where one kind of receiver comes again and again.
    // The tables answer for every Structure alike, in about what a hit at a site and the site's operand cost together.
    bool tablesAnswer = context.shape() != CodeShape::Fast || inputs().stubCatalog.bindsImmediate(AOT::getById(), *identifier);
    if (cache && (!tablesAnswer || !hasColumn)) {
        StubKey stub = stubThatReadsAtSite(*identifier, receiverID);
        return Boxed(callStub(context, stub, { receiver.value(), siteCacheAddress(cache).value() }, options).first);
    }
    return readThroughTables();
}

// No layout of the receivers has the name, so the answer lies above the object: in fast code the site is asked in line.
Boxed AccessEmitter::getMemberAboveLayouts(const ValueContext& value)
{
    Boxed receiver = in<Boxed>(value->objectOperand());
    SiteCacheID cache = value->siteCache();
    std::optional<uint32_t> identifier = build().identifierNumbering().numberOf(value->key());
    bool asksSiteInLine = cache && identifier && value.shape() == CodeShape::Fast && value->receivers() && typeOf(value->objectOperand()).isObject() && m_program.nameClassOf(value->key()) != NameClass::Length;
    if (!asksSiteInLine || !comesAgain(value))
        return getById(value, receiver, value->key(), cache, std::nullopt, value->objectOperand());

    recordArrival(value, receiver);
    Pointer site = siteCacheAddress(cache);
    LBasicBlock asksStub = newBlock();
    Join<Boxed> join(m_out);
    join.add(loadBySlotAtSite(unboxCell(receiver), site, Slot::Kind::Value, asksStub, asksStub));

    m_out.appendTo(asksStub);
    join.add(Boxed(callStub(value, stubThatReadsAtSite(*identifier, value->objectOperand()), { receiver.value(), site.value() }, { .immediate = *identifier, .isOnRarePath = true }).first));
    return join.finish();
}

void AccessEmitter::putById(const OperationContext& context, Boxed receiver, PropertyKeyID key, Boxed value, PutMode mode, SiteCacheID cache)
{
    recordArrival(context, receiver);
    std::optional<uint32_t> identifier = findIdentifierNumber(context, key);
    if (!identifier)
        return;
    if (!comesAgain(context)) {
        // The table of fields answers for a compiled object without a first time. Nothing else is asked, and nothing is filed.
        if (FieldTableEntry::hasColumn(*identifier)) {
            callStub(context, putByIdOnce(mode), { receiver.value(), value.value() }, { .immediate = *identifier });
            return;
        }
        callVoidOperation(context, putOperationWithoutCachesFor(mode), { receiver, word32(static_cast<int32_t>(*identifier)), value });
        return;
    }
    if (cache) {
        callStub(context, putByIdAtSite(mode), { receiver.value(), value.value(), siteCacheAddress(cache).value() }, { .immediate = *identifier });
        return;
    }
    StubKey stub = RowDisplacementTable::hasColumn(*identifier) ? AOT::putById(mode) : putByIdHashed(mode);
    callStub(context, stub, { receiver.value(), value.value() }, { .immediate = *identifier });
}

Condition AccessEmitter::hasById(const OperationContext& context, Boxed receiver, PropertyKeyID key)
{
    recordArrival(context, receiver);
    std::optional<uint32_t> identifier = findIdentifierNumber(context, key);
    if (!identifier)
        return boolean(false);
    return Condition(m_out.castToInt32(callStub(context, inById(PresenceQuestion::In), { receiver.value() }, { .immediate = *identifier }).first));
}

Boxed AccessEmitter::getFreeName(const ValueContext& value)
{
    bool isForTypeOf = value->isForTypeOf();
    SiteCacheID cache = value->siteCache();
    Pointer site = siteCacheAddress(cache);
    if (!cache) {
        RuntimeOperation operation = isForTypeOf ? RuntimeOperation::GetFreeNameForTypeOf : RuntimeOperation::GetFreeName;
        return callOperation<Boxed>(value, operation, { identifierNumber(value->key()), site });
    }

    std::optional<uint32_t> identifier = findIdentifierNumber(value, value->key());
    if (!identifier)
        return undefined();
    auto readByStub = [&](bool isOnRarePath) {
        StubKey stub = AOT::getFreeName(isForTypeOf ? FreeNameRead::TypeOf : FreeNameRead::Read);
        return Boxed(callStub(value, stub, { site.value() }, { .immediate = *identifier, .isOnRarePath = isOnRarePath }).first);
    };
    if (!probesFreeNameInline(value.shape()))
        return readByStub(false);

    B3::PatchpointValue* probe = m_out.patchpoint(B3::Int64);
    probe->appendSomeRegister(site.value());
    probe->numGPScratchRegisters = 1;
    probe->clobber(RegisterSet::macroClobberedGPRs());
    probe->effects = B3::Effects::none();
    probe->effects.terminal = true;
    probe->effects.controlDependent = true;
    const B3::AbstractHeap* const reads[] { &m_heaps.AOTInstance_siteCaches };
    m_heaps.decoratePatchpointRead(std::span<const B3::AbstractHeap* const>(reads), probe);
    probe->setRunsWithoutFrame(true);
    LBasicBlock holdsConstant = newBlock();
    LBasicBlock holdsNoConstant = newBlock();
    m_out.m_block->appendSuccessor(normalEdge(holdsConstant));
    m_out.m_block->appendSuccessor(rareEdge(holdsNoConstant));
    probe->setGenerator([](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        CCallHelpers::Jump isNotConstant = InlinePropertyCode::loadConstantSlotAtSite(jit, params[1].gpr(), params.gpScratch(0), params[0].gpr());
        CCallHelpers::Jump isConstant;
        if (!params.fallsThroughToSuccessor(0))
            isConstant = jit.jump();
        WTF::Vector<Box<CCallHelpers::Label>> labels = params.successorLabels();
        params.addLatePath([=](CCallHelpers& jit) {
            isNotConstant.linkTo(*labels[1], &jit);
            if (isConstant.isSet())
                isConstant.linkTo(*labels[0], &jit);
        });
    });

    Join<Boxed> join(m_out);
    m_out.appendTo(holdsConstant);
    join.add(Boxed(probe));
    m_out.appendTo(holdsNoConstant);
    join.add(readByStub(true));
    return join.finish();
}

Condition AccessEmitter::countMissAtSite(Pointer site)
{
    Pointer states = loadImmutablePointer(AOT_INSTANCE_OFFSET(propertyCaches.siteCacheStates));
    LValue offset = m_out.bitAnd(m_out.lShr(site.value(), SiteCacheState::slotAddressShift), SiteCacheState::offsetMask);
    LValue state = m_out.add(states.value(), offset);
    TypedPointer missesLeft = m_out.address(m_heaps.entire(AbstractHeap::RuntimeState), state, 0);
    Word32 count(m_out.load16ZeroExt32(missesLeft));
    m_out.store32As16(m_out.sub(count.value(), m_out.constInt32(1)), missesLeft);
    return equal(count, word32(0));
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
    Pointer owner = select(isZero(Integer(holder.value())), Pointer(object.value()), holder);
    Pointer butterfly(m_out.loadPtr(m_out.address(m_heaps.root, owner.value(), JSObject::butterflyOffset())));
    Pointer storage = select(hasBits(Word32(m_out.load8ZeroExt32(partOfSlot(Slot::offsetOfFlags()))), Slot::isOutOfLine), butterfly, owner);
    LValue offset = m_out.shl(m_out.signExt32To64(m_out.load16SignExt32(partOfSlot(Slot::offsetOfLocation()))), 3);
    return Boxed(m_out.load64(m_out.address(m_heaps.root, m_out.add(storage.value(), offset), 0)));
}

// A site that has no answer for the Structure of the object has the name looked up, so the first object of a Structure is answered like those behind it.
// Only a target that is not extensible or has a property that is not configurable binds what a trap may answer. The Structure of a compiled object says yes for all of them: what was done to one object stands in the object, and what its fields are born with in the layout.
Condition AccessEmitter::mayBindAnswersOfTraps(ValueID id, uint32_t bitsOfStructureThatMayBind)
{
    CellPointer target = in<CellPointer>(id);
    auto isNoLongerAsBorn = [&] {
        return hasBits(emitter<ShapeTestEmitter>().orderWordOf(target), ~orderAndCountMask);
    };
    auto askStructure = [&] {
        Pointer structure = loadStructure(target);
        return either<Condition>(hasBits(loadField<Word32>(structure, m_heaps.Structure_bitField), bitsOfStructureThatMayBind), [&] {
            return either<Condition>(hasCellType(target, CompiledObjectType), [&] {
                Condition isBornFree = hasBits(loadByte(structure, m_heaps.AOTCompiledStructure_flags), static_cast<uint8_t>(CompiledStructure::Flag::EveryFieldIsBornConfigurable));
                return logicalNot(logicalAnd(isBornFree, logicalNot(isNoLongerAsBorn())));
            }, [&] {
                return boolean(true);
            });
        }, [&] {
            return boolean(false);
        });
    };
    // What a layout that has all its fields itself says of them is known here.
    const Type& type = typeOf(id);
    bool isBornFree = type.hasOnlyLayouts() && std::ranges::none_of(membersOf(m_program, type), [&](LayoutID id) {
        const Layout& layout = m_program.at(id);
        return expectedBaseOf(layout) || layout.flow.baseState != BaseState::None || std::ranges::any_of(layout.fields, [](const Field& field) {
            return field.form.contains(FieldForm::IsNotConfigurable);
        });
    });
    if (!isBornFree)
        return askStructure();
    return either<Condition>(hasCellType(target, CompiledObjectType), isNoLongerAsBorn, askStructure);
}

void AccessEmitter::generateValidateAnswerOfGetTrap(const ValueContext& value)
{
    callVoidOperation(value, RuntimeOperation::ValidateAnswerOfGetTrap, { in<Boxed>(value->targetOperand()), in<Boxed>(value->propertyOperand()), in<Boxed>(value->answerOperand()) }, { .isOnRarePath = true });
}

void AccessEmitter::generateValidateDenialOfHasTrap(const ValueContext& value)
{
    callVoidOperation(value, RuntimeOperation::ValidateDenialOfHasTrap, { in<Boxed>(value->targetOperand()), in<Boxed>(value->propertyOperand()) }, { .isOnRarePath = true });
}

void AccessEmitter::generateValidateConsentOfDeleteTrap(const ValueContext& value)
{
    callVoidOperation(value, RuntimeOperation::ValidateConsentOfDeleteTrap, { in<Boxed>(value->targetOperand()), in<Boxed>(value->propertyOperand()) }, { .isOnRarePath = true });
}

void AccessEmitter::generateHasOwnByAnswerOfDescriptorTrap(const ValueContext& value)
{
    define(value, callOperation<Condition>(value, RuntimeOperation::HasOwnByAnswerOfDescriptorTrap, { in<Boxed>(value->targetOperand()), in<Boxed>(value->propertyOperand()), in<Boxed>(value->answerOperand()) }));
}

void AccessEmitter::generateFinishStoreByTrap(const ValueContext& value)
{
    callVoidOperation(value, RuntimeOperation::FinishStoreByTrap, { in<Boxed>(value->targetOperand()), in<Boxed>(value->propertyOperand()), in<Boxed>(value->valueOperand()), in<Boxed>(value->answerOperand()), word32(value->mode() == ECMAMode::Strict) }, { .isOnRarePath = true });
}

void AccessEmitter::generateGetCachedGetter(const ValueContext& value)
{
    SiteCacheID cache = value->siteCache();
    std::optional<uint32_t> identifier = findIdentifierNumber(value, value->key());
    if (!cache || !identifier || value.shape() != CodeShape::Fast) {
        define(value, undefined());
        return;
    }

    CellPointer object = in<CellPointer>(value->objectOperand());
    Pointer site = siteCacheAddress(cache);
    LBasicBlock findsNoGetter = newBlock();
    LBasicBlock hasOtherStructure = newBlock();
    Join<Boxed> join(m_out);
    Boxed accessor = loadBySlotAtSite(object, site, Slot::Kind::Getter, findsNoGetter, hasOtherStructure);
    join.add(Boxed(m_out.load64(m_out.address(m_heaps.root, accessor.value(), GetterSetter::offsetOfGetter()))));

    m_out.appendTo(hasOtherStructure);
    branchUnless(countMissAtSite(site), findsNoGetter);
    join.add(callOperation<Boxed>(value, RuntimeOperation::FindGetterAndFillSiteCache, { object, word32(static_cast<int32_t>(*identifier)), site }, { .isOnRarePath = true }));

    m_out.appendTo(findsNoGetter);
    join.add(undefined());
    define(value, join.finish());
}

void AccessEmitter::putFreeName(const ValueContext& value)
{
    Boxed stored = in<Boxed>(value->valueOperand());
    SiteCacheID cache = value->siteCache();
    Pointer site = siteCacheAddress(cache);
    if (!cache) {
        callVoidOperation(value, forMode(value->mode(), RuntimeOperation::SetFreeName, RuntimeOperation::SetFreeNameSloppy), { identifierNumber(value->key()), site, stored });
        return;
    }

    std::optional<uint32_t> identifier = findIdentifierNumber(value, value->key());
    if (!identifier)
        return;
    callStub(value, setFreeName(putModeOf(value->mode())), { stored.value(), site.value() }, { .immediate = *identifier });
}

CellPointer AccessEmitter::globalObject()
{
    return CellPointer(loadImmutablePointer(AOT_INSTANCE_OFFSET(globalObject)).value());
}

Pointer AccessEmitter::globalBindingAddress(GlobalBindingID binding)
{
    ptrdiff_t offsetInTable = static_cast<ptrdiff_t>(binding.index() * sizeof(GlobalBinding));
    if (InstanceLayout::holdsGlobalBindings(m_placement.globalBindings().bindings.size() * sizeof(GlobalBinding)))
        return offsetBy(instance(), static_cast<ptrdiff_t>(InstanceLayout::offsetOfGlobalBindings) + offsetInTable);
    return offsetBy(loadImmutablePointer(AOT_INSTANCE_OFFSET(runtime.counters.globalBindings)), offsetInTable);
}

TypedPointer AccessEmitter::globalBindingLocationOrElse(Pointer record, bool seesScriptBindings, LBasicBlock miss)
{
    static_assert(OBJECT_OFFSETOF(GlobalBinding, lexicalBindingEpoch) == OBJECT_OFFSETOF(GlobalBinding, structureID) + sizeof(uint32_t));
    const B3::AbstractHeap& records = m_heaps.AOTInstance_globalBindings;
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

void AccessEmitter::validateGlobalBinding(const ValueContext& value, Boxed read)
{
    if (generator().validates())
        callVoidOperation(value, RuntimeOperation::ValidateGlobalBinding, { word32(static_cast<int32_t>(value->binding().index())), read });
}

void AccessEmitter::generateGetGlobalBinding(const ValueContext& value)
{
    GlobalBindingID binding = value->binding();
    if (!binding) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        define(value, undefined());
        return;
    }

    bool seesScriptBindings = value->seesScriptBindings();
    Pointer record = globalBindingAddress(binding);
    LBasicBlock constantCase = newBlock();
    LBasicBlock slowPath = newBlock();
    Join<Boxed> join(m_out);

    Boxed constant(m_out.load64(m_out.address(m_heaps.AOTInstance_globalBindings, record.value(), OBJECT_OFFSETOF(GlobalBinding, constant))));
    if (probesBindingLocationInline(value.shape())) {
        LBasicBlock locationCase = newBlock();
        branch(isNotEmpty(constant), { normalEdge(constantCase), unlikelyEdge(locationCase) });

        m_out.appendTo(locationCase);
        Boxed read(m_out.load64(globalBindingLocationOrElse(record, seesScriptBindings, slowPath)));
        branchUnless(isNotEmpty(read), slowPath);
        validateGlobalBinding(value, read);
        join.add(read);
    } else
        branch(isNotEmpty(constant), { normalEdge(constantCase), rareEdge(slowPath) });

    m_out.appendTo(constantCase);
    validateGlobalBinding(value, constant);
    join.add(constant);

    m_out.appendTo(slowPath);
    if (probesBindingLocationInline(value.shape())) {
        RuntimeOperation operation = seesScriptBindings ? RuntimeOperation::GetGlobalBinding : RuntimeOperation::GetGlobalObjectBinding;
        join.add(callOperation<Boxed>(value, operation, { word32(static_cast<int32_t>(binding.index())) }, { .isOnRarePath = true }));
    } else {
        GlobalBindingScope scope = seesScriptBindings ? GlobalBindingScope::ScriptsAndGlobalObject : GlobalBindingScope::GlobalObject;
        join.add(Boxed(callStub(value, AOT::getGlobalBinding(scope), { }, { .immediate = binding.index(), .isOnRarePath = true }).first));
    }

    define(value, join.finish());
}

void AccessEmitter::generateSetGlobalBinding(const ValueContext& value)
{
    GlobalBindingID binding = value->binding();
    if (!binding) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        return;
    }

    bool seesScriptBindings = value->seesScriptBindings();
    Boxed stored = in<Boxed>(value->valueOperand());
    RuntimeOperation operation = seesScriptBindings
        ? forMode(value->mode(), RuntimeOperation::SetGlobalBinding, RuntimeOperation::SetGlobalBindingSloppy)
        : forMode(value->mode(), RuntimeOperation::SetGlobalObjectBinding, RuntimeOperation::SetGlobalObjectBindingSloppy);
    auto storeByOperation = [&](bool isOnRarePath) {
        callVoidOperation(value, operation, { word32(static_cast<int32_t>(binding.index())), stored }, { .isOnRarePath = isOnRarePath });
    };
    if (!probesBindingLocationInline(value.shape())) {
        storeByOperation(false);
        return;
    }

    LBasicBlock slowPath = newBlock();
    LBasicBlock continuation = newBlock();
    m_out.store64(stored.value(), globalBindingLocationOrElse(globalBindingAddress(binding), seesScriptBindings, slowPath));
    barrierAfterStore(value, globalObject(), stored, typeOf(value->valueOperand()));
    m_out.jump(continuation);

    m_out.appendTo(slowPath);
    storeByOperation(true);
    m_out.jump(continuation);

    m_out.appendTo(continuation);
}

void AccessEmitter::generatePropGet(const ValueContext& value)
{
    define(value, value->isFreeName() ? getFreeName(value) : getMemberAboveLayouts(value));
}

void AccessEmitter::putNamedProperty(const ValueContext& value, Boxed object, PutMode mode)
{
    std::optional<uint32_t> identifier = build().identifierNumbering().numberOf(value->key());
    NameHasColumn nameHasColumn = identifier && FieldTableEntry::hasColumn(*identifier) ? NameHasColumn::Yes : NameHasColumn::No;
    std::optional<uint32_t> record = findRecordOfStoredField(m_program, m_placement, m_function, value.value(), nameHasColumn);
    if (!record) {
        putById(value, object, value->key(), in<Boxed>(value->valueOperand()), mode, value->siteCache());
        return;
    }
    recordArrival(value, object);
    StubKey stub = putKnownField(mode);
    if (storedFieldTakesAnyValue(m_program, m_placement, m_function, value.value()))
        stub = putKnownFieldThatTakesAnyValue(mode);
    else if (storedFieldTakesEveryNumber(m_program, m_placement, m_function, value.value()))
        stub = putKnownFieldThatTakesEveryNumber(mode);
    callStub(value, stub, { object.value(), in<Boxed>(value->valueOperand()).value() }, { .immediate = *record });
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
    if (m_program.nameClassOf(value->key()) == NameClass::Index) {
        std::optional<uint32_t> index = parseIndex(byteCast<Latin1Character>(m_program.strings[m_program.at(value->key()).text]));
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
    Word32 key = identifierNumber(value->key());
    if (value->isFreeName()) {
        define(value, callOperation<Condition>(value, RuntimeOperation::DeleteFreeName, { key }));
        return;
    }
    if (!typeOf(value->objectOperand()).hasObjects()) {
        define(value, callOperation<Condition>(value, forMode(value->mode(), RuntimeOperation::DeleteById, RuntimeOperation::DeleteByIdSloppy), { in<Boxed>(value->objectOperand()), key }));
        return;
    }
    define(value, Condition(m_out.castToInt32(callStub(value, deleteByIdCached(putModeOf(value->mode())), { in<Boxed>(value->objectOperand()).value(), key.value() }).first)));
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

static std::optional<AOT::ExpectedElements> stubFormOf(ExpectedElements expected)
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

Boxed AccessEmitter::getByVal(const OperationContext& context, Boxed receiver, Boxed key)
{
    recordArrival(context, receiver);
    return Boxed(callStub(context, getByValCached(), { receiver.value(), key.value() }).first);
}

void AccessEmitter::putByVal(const OperationContext& context, Boxed receiver, Boxed key, Boxed value, PutMode mode)
{
    recordArrival(context, receiver);
    callStub(context, cachedStoreStubFor(mode), { receiver.value(), key.value(), value.value() });
}

Condition AccessEmitter::hasByVal(const OperationContext& context, Boxed receiver, Boxed key)
{
    recordArrival(context, receiver);
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
    case Representation::DoubleWithPayload:
    case Representation::ImpureDouble:
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
        std::optional<AOT::ExpectedElements> expected = stubFormOf(value->expectedElements());
        stub = expected ? getByValExpecting(key->isInteger, *expected) : AOT::getByVal(key->isInteger);
    } else if (typeOf(value->objectOperand()).isObject() && typeOf(value->propertyOperand()).isString())
        stub = getByValCachedCells();
    recordArrival(value, object);
    define(value, Boxed(callStub(value, stub, { object.value(), key->value }).first));
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
        std::optional<AOT::ExpectedElements> expected = stubFormOf(value->expectedElements());
        stub = expected ? putByValExpecting(key->isInteger, mode, *expected) : AOT::putByVal(key->isInteger, mode);
    }
    recordArrival(value, object);
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
