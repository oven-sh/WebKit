/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTAccessEmitter.h"

#if ENABLE(AOT)

#include "AOTIdentifierNumbering.h"
#include "AOTPropertyCachesFront.h"
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

Boxed AccessEmitter::getById(const OperationContext& context, Boxed receiver, PropertyKeyID key, SiteCacheID cache, std::optional<Origin> origin)
{
    std::optional<uint32_t> identifier = findIdentifierNumber(context, key);
    if (!identifier)
        return undefined();
    if (cache)
        return getByIdThroughSiteCache(context, receiver, *identifier, cache, origin);
    return getByIdThroughTables(context, receiver, *identifier, origin);
}

void AccessEmitter::putById(const OperationContext& context, Boxed receiver, PropertyKeyID key, Boxed value, PutMode mode, SiteCacheID cache)
{
    std::optional<uint32_t> identifier = findIdentifierNumber(context, key);
    if (!identifier)
        return;
    if (cache) {
        putByIdThroughSiteCache(context, receiver, *identifier, value, mode, cache);
        return;
    }
    putByIdThroughTables(context, receiver, *identifier, value, mode);
}

Condition AccessEmitter::hasById(const OperationContext& context, Boxed receiver, PropertyKeyID key)
{
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

void AccessEmitter::generatePropGet(const ValueContext& value)
{
    if (value->isFreeName()) {
        define(value, getFreeName(value));
        return;
    }
    define(value, getById(value, in<Boxed>(value->objectOperand()), value->key(), value->siteCache()));
}

void AccessEmitter::generatePropSet(const ValueContext& value)
{
    if (value->isFreeName()) {
        putFreeName(value);
        return;
    }
    putById(value, in<Boxed>(value->objectOperand()), value->key(), in<Boxed>(value->valueOperand()), putModeOf(value->mode()), value->siteCache());
}

void AccessEmitter::generatePropDefine(const ValueContext& value)
{
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
