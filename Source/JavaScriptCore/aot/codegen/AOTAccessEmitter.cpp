/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTAccessEmitter.h"

#if ENABLE(AOT)

#include "AOTFieldTable.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIdentifierNumbering.h"
#include "AOTPlacement.h"
#include "AOTPropertyCachesFront.h"
#include "AOTRuntimeWords.h"
#include "AOTSiteCachePlacement.h"
#include "AOTStubCatalog.h"
#include "AOTStubsGlobalBindings.h"
#include "AOTStubsPropertyInlineCode.h"
#include "AOTStubsPropertyKeys.h"
#include "AllowMacroScratchRegisterUsage.h"
#include "B3PatchpointValue.h"
#include "B3StackmapGenerationParams.h"
#include <initializer_list>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct SiteCacheProbe {
    B3::PatchpointValue* patchpoint;
    LBasicBlock hit;
    LBasicBlock miss;
};

class SiteCacheProbeEmitter final : public OperationEmitter {
public:
    explicit SiteCacheProbeEmitter(CodeGenerator& generator)
        : OperationEmitter(generator)
    {
    }

    SiteCacheProbe loadConstant(Pointer site);
    Boxed joinWithMiss(const SiteCacheProbe&, NOESCAPE const Invocable<Boxed()> auto& readOnMiss);

private:
    using Heaps = std::initializer_list<const B3::AbstractHeap*>;

    SiteCacheProbe createProbe(LType result, std::initializer_list<LValue> operands, uint8_t numberOfScratchRegisters, Heaps reads);
};

} // anonymous namespace

static void jumpToSuccessors(CCallHelpers& jit, const B3::StackmapGenerationParams& params, const CCallHelpers::JumpList& misses)
{
    CCallHelpers::Jump hit;
    if (!params.fallsThroughToSuccessor(0))
        hit = jit.jump();
    Vector<Box<CCallHelpers::Label>> labels = params.successorLabels();
    params.addLatePath([=](CCallHelpers& jit) {
        misses.linkTo(*labels[1], &jit);
        if (hit.isSet())
            hit.linkTo(*labels[0], &jit);
    });
}

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

static RuntimeOperation setFreeNameOperationFor(ECMAMode mode)
{
    switch (mode) {
    case ECMAMode::Strict:
        return RuntimeOperation::SetFreeName;
    case ECMAMode::Sloppy:
        return RuntimeOperation::SetFreeNameSloppy;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static RuntimeOperation setGlobalBindingOperationFor(ECMAMode mode, bool seesScriptBindings)
{
    switch (mode) {
    case ECMAMode::Strict:
        return seesScriptBindings ? RuntimeOperation::SetGlobalBinding : RuntimeOperation::SetGlobalObjectBinding;
    case ECMAMode::Sloppy:
        return seesScriptBindings ? RuntimeOperation::SetGlobalBindingSloppy : RuntimeOperation::SetGlobalObjectBindingSloppy;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static RuntimeOperation deleteByIdOperationFor(ECMAMode mode)
{
    switch (mode) {
    case ECMAMode::Strict:
        return RuntimeOperation::DeleteById;
    case ECMAMode::Sloppy:
        return RuntimeOperation::DeleteByIdSloppy;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

SiteCacheProbe SiteCacheProbeEmitter::createProbe(LType result, std::initializer_list<LValue> operands, uint8_t numberOfScratchRegisters, Heaps reads)
{
    B3::PatchpointValue* patchpoint = m_out.patchpoint(result);
    for (LValue operand : operands)
        patchpoint->appendSomeRegister(operand);
    patchpoint->numGPScratchRegisters = numberOfScratchRegisters;
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    patchpoint->effects = B3::Effects::none();
    patchpoint->effects.terminal = true;
    patchpoint->effects.controlDependent = true;
    m_heaps.decoratePatchpointRead(std::span<const B3::AbstractHeap* const>(reads.begin(), reads.size()), patchpoint);
    patchpoint->setRunsWithoutFrame(true);

    SiteCacheProbe probe { patchpoint, newBlock(), newBlock() };
    m_out.m_block->appendSuccessor(normalEdge(probe.hit));
    m_out.m_block->appendSuccessor(rareEdge(probe.miss));
    return probe;
}

SiteCacheProbe SiteCacheProbeEmitter::loadConstant(Pointer site)
{
    SiteCacheProbe probe = createProbe(B3::Int64, { site.value() }, 1, { &m_heaps.AOTInstance_siteCaches });
    probe.patchpoint->setGenerator([](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        CCallHelpers::Jump isNotConstant = InlinePropertyCode::loadConstantSlotAtSite(jit, params[1].gpr(), params.gpScratch(0), params[0].gpr());
        jumpToSuccessors(jit, params, isNotConstant);
    });
    return probe;
}

Boxed SiteCacheProbeEmitter::joinWithMiss(const SiteCacheProbe& probe, NOESCAPE const Invocable<Boxed()> auto& readOnMiss)
{
    Join<Boxed> join(m_out);
    m_out.appendTo(probe.hit);
    join.add(Boxed(probe.patchpoint));
    m_out.appendTo(probe.miss);
    join.add(readOnMiss());
    return join.finish();
}

AccessEmitter::AccessEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
{
}

PutMode AccessEmitter::putModeOf(ECMAMode mode)
{
    switch (mode) {
    case ECMAMode::Strict:
        return PutMode::Strict;
    case ECMAMode::Sloppy:
        return PutMode::Sloppy;
    }
    RELEASE_ASSERT_NOT_REACHED();
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

Boxed AccessEmitter::getById(const OperationContext& context, Boxed receiver, PropertyKeyID key, SiteCacheID cache, std::optional<Origin> origin)
{
    recordArrival(context, receiver);
    std::optional<uint32_t> identifier = findIdentifierNumber(context, key);
    if (!identifier)
        return undefined();
    if (!comesAgain(context)) {
#if CPU(X86_64)
        // Behind the tables the stub asks the objects themselves and files nothing, so code that runs once takes the way of all other code and leaves nothing behind.
        return getByIdThroughTables(context, receiver, *identifier, origin);
#else
        return callOperation<Boxed>(context, RuntimeOperation::GetById, { receiver, word32(static_cast<int32_t>(*identifier)) }, { .origin = origin });
#endif
    }
    if (cache && m_program.nameClassOf(key) == NameClass::Length)
        return Boxed(callStub(context, getLengthAtSite(), { receiver.value(), siteCacheAddress(cache).value() }, { .immediate = *identifier, .origin = origin }).first);
    // A site cache pays where one kind of receiver comes again and again. Compact code runs seldom: there the tables answer, and the site has one operand less.
    // A name that the program reads in hundreds of places has a stub of its own, which answers for every Structure in about what a hit at a site costs.
#if CPU(X86_64)
    // The tables answer for every Structure alike, in about what a hit at a site and the site's operand cost together.
    bool tablesAnswer = context.shape() != CodeShape::Fast || inputs().stubCatalog.bindsImmediate(AOT::getById(), *identifier);
#else
    bool tablesAnswer = context.shape() == CodeShape::Compact || inputs().stubCatalog.bindsImmediate(AOT::getById(), *identifier);
#endif
    if (cache && (!tablesAnswer || !RowDisplacementTable::hasColumn(*identifier)))
        return getByIdThroughSiteCache(context, receiver, *identifier, cache, origin);
    return getByIdThroughTables(context, receiver, *identifier, origin);
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
        putByIdThroughSiteCache(context, receiver, *identifier, value, mode, cache);
        return;
    }
    putByIdThroughTables(context, receiver, *identifier, value, mode);
}

Condition AccessEmitter::hasById(const OperationContext& context, Boxed receiver, PropertyKeyID key)
{
    recordArrival(context, receiver);
    std::optional<uint32_t> identifier = findIdentifierNumber(context, key);
    if (!identifier)
        return boolean(false);
    return Condition(m_out.castToInt32(callStub(context, inById(PresenceQuestion::In), { receiver.value() }, { .immediate = *identifier }).first));
}

Boxed AccessEmitter::getByIdThroughTables(const OperationContext& context, Boxed receiver, uint32_t identifier, std::optional<Origin> origin)
{
    StubKey stub = RowDisplacementTable::hasColumn(identifier) ? AOT::getById() : getByIdHashed();
    return Boxed(callStub(context, stub, { receiver.value() }, { .immediate = identifier, .origin = origin }).first);
}

void AccessEmitter::putByIdThroughTables(const OperationContext& context, Boxed receiver, uint32_t identifier, Boxed value, PutMode mode)
{
    StubKey stub = RowDisplacementTable::hasColumn(identifier) ? AOT::putById(mode) : putByIdHashed(mode);
    callStub(context, stub, { receiver.value(), value.value() }, { .immediate = identifier });
}

Boxed AccessEmitter::getByIdThroughSiteCache(const OperationContext& context, Boxed receiver, uint32_t identifier, SiteCacheID cache, std::optional<Origin> origin)
{
    Pointer site = siteCacheAddress(cache);
    return Boxed(callStub(context, getByIdAtSite(), { receiver.value(), site.value() }, { .immediate = identifier, .origin = origin }).first);
}

void AccessEmitter::putByIdThroughSiteCache(const OperationContext& context, Boxed receiver, uint32_t identifier, Boxed value, PutMode mode, SiteCacheID cache)
{
    Pointer site = siteCacheAddress(cache);
    callStub(context, putByIdAtSite(mode), { receiver.value(), value.value(), site.value() }, { .immediate = identifier });
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

    SiteCacheProbeEmitter probes(generator());
    return probes.joinWithMiss(probes.loadConstant(site), [&] {
        return readByStub(true);
    });
}

void AccessEmitter::putFreeName(const ValueContext& value)
{
    Boxed stored = in<Boxed>(value->valueOperand());
    SiteCacheID cache = value->siteCache();
    Pointer site = siteCacheAddress(cache);
    if (!cache) {
        callVoidOperation(value, setFreeNameOperationFor(value->mode()), { identifierNumber(value->key()), site, stored });
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
        Integer current = bitOr(zeroExtended(structureID), Integer(m_out.shl(epoch.value(), m_out.constInt32(32))));
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
    auto storeByOperation = [&](bool isOnRarePath) {
        callVoidOperation(value, setGlobalBindingOperationFor(value->mode(), seesScriptBindings), { word32(static_cast<int32_t>(binding.index())), stored }, { .isOnRarePath = isOnRarePath });
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
    if (value->isFreeName()) {
        define(value, getFreeName(value));
        return;
    }
    define(value, getById(value, in<Boxed>(value->objectOperand()), value->key(), value->siteCache()));
}

StubKey AccessEmitter::stubForStoreByRecord(const ValueContext& value, PutMode mode)
{
    return storedFieldTakesAnyValue(m_program, m_placement, m_function, value.value()) ? putKnownFieldThatTakesAnyValue(mode) : putKnownField(mode);
}

std::optional<uint32_t> AccessEmitter::findRecordOfStoredField(const ValueContext& value)
{
    std::optional<uint32_t> identifier = build().identifierNumbering().numberOf(value->key());
    bool nameHasColumn = identifier && FieldTableEntry::hasColumn(*identifier);
    return Typed::findRecordOfStoredField(m_program, m_placement, m_function, value.value(), nameHasColumn ? NameHasColumn::Yes : NameHasColumn::No);
}

void AccessEmitter::generatePropSet(const ValueContext& value)
{
    if (value->isFreeName()) {
        putFreeName(value);
        return;
    }
    if (std::optional<uint32_t> record = findRecordOfStoredField(value)) {
        Boxed object = in<Boxed>(value->objectOperand());
        recordArrival(value, object);
        callStub(value, stubForStoreByRecord(value, putModeOf(value->mode())), { object.value(), in<Boxed>(value->valueOperand()).value() }, { .immediate = *record });
        return;
    }
    putById(value, in<Boxed>(value->objectOperand()), value->key(), in<Boxed>(value->valueOperand()), putModeOf(value->mode()), value->siteCache());
}

void AccessEmitter::generatePropDefine(const ValueContext& value)
{
    if (std::optional<uint32_t> record = findRecordOfStoredField(value)) {
        Boxed object = boxCell(in<CellPointer>(value->objectOperand()));
        recordArrival(value, object);
        callStub(value, stubForStoreByRecord(value, PutMode::Define), { object.value(), in<Boxed>(value->valueOperand()).value() }, { .immediate = *record });
        return;
    }
    putById(value, boxCell(in<CellPointer>(value->objectOperand())), value->key(), in<Boxed>(value->valueOperand()), PutMode::Define, value->siteCache());
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
    define(value, callOperation<Condition>(value, deleteByIdOperationFor(value->mode()), { in<Boxed>(value->objectOperand()), key }));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
