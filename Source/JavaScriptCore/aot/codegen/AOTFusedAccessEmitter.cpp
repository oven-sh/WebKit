/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFusedAccessEmitter.h"

#if ENABLE(AOT)

#include "AOTAccessEmitter.h"
#include "AOTBarrierEmitter.h"
#include "AOTConstantEmitter.h"
#include "AOTIdentifierNumbering.h"
#include "AOTPropertyCachesFront.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTRuntimeRecords.h"
#include "AOTSiteCachePlacement.h"
#include "AOTStringEmitter.h"
#include "AOTStubsPropertyKeys.h"
#include "AOTVector.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

static_assert(RowDisplacementTable::maxColumn < 1u << secondNameShift);
static_assert(!Slot::offsetOfStructureID() && sizeof(StructureID) == sizeof(uint32_t));
static_assert(OBJECT_OFFSETOF(Slot, transition.newStructureID) == Slot::offsetOfPayload());

namespace {

struct FusedPropertyRun {
    CellPointer object;
    Vector<Boxed, 8> values;
    Request request;
};

class PropertyRunEmitter final : public OperationEmitter {
public:
    explicit PropertyRunEmitter(CodeGenerator& generator)
        : OperationEmitter(generator)
    {
    }

    Condition tryStore(const ValueContext&);

private:
    bool paysWithoutSiteCache(const ValueContext&);
    Condition countMiss(Pointer cache);
    Condition storeByOperation(const ValueContext&, const FusedPropertyRun&, Pointer cache, bool isOnRarePath);
    void storeThroughSiteCache(const ValueContext&, const FusedPropertyRun&, Pointer cache, Integer header, Word32 kind);
};

} // anonymous namespace

static std::optional<uint32_t> columnOf(const IdentifierNumbering& numbering, PropertyKeyID key)
{
    std::optional<uint32_t> number = numbering.numberOf(key);
    if (!number || !RowDisplacementTable::hasColumn(*number))
        return std::nullopt;
    return number;
}

static std::optional<uint32_t> pairedColumnsOf(const IdentifierNumbering& numbering, PropertyKeyID first, PropertyKeyID second)
{
    std::optional<uint32_t> firstColumn = columnOf(numbering, first);
    std::optional<uint32_t> secondColumn = columnOf(numbering, second);
    if (!firstColumn || !secondColumn)
        return std::nullopt;
    return pairedNames(*firstColumn, *secondColumn);
}

static bool literalFitsInRegister(const String& text)
{
    const StubDescriptor& descriptor = descriptorOf(StubFamily::GetByIdEqualsLiteral);
    return text.is8Bit() && text.length() >= descriptor.minCount && text.length() <= descriptor.maxCount;
}

static bool probesSiteCacheInline(CodeShape shape)
{
    switch (shape) {
    case CodeShape::Unchosen:
    case CodeShape::Compact:
        return false;
    case CodeShape::Balanced:
    case CodeShape::Fast:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

FusedAccessEmitter::FusedAccessEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
{
}

void FusedAccessEmitter::generatePropGetChain(const ValueContext& value)
{
    std::optional<uint32_t> columns = pairedColumnsOf(build().identifierNumbering(), value->key(), value->secondKey());
    if (!columns) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        define(value, emitter<ConstantEmitter>().zeroOf(value.representation()));
        return;
    }

    emitter<AccessEmitter>().recordArrival(value, in<Boxed>(value->objectOperand()));
    LValue operands[] = { in<Boxed>(value->objectOperand()).value() };
    CallOptions options { .immediate = *columns };
    define(value, Boxed(emitter<RuntimeCallEmitter>().callStubForTwoReads(value, getByIdChain(), operands, options, value->readOrigin()).first));
}

void FusedAccessEmitter::generatePropGetPair(const ValueContext& value)
{
    std::optional<uint32_t> columns = pairedColumnsOf(build().identifierNumbering(), value->key(), value->secondKey());
    if (!columns) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        Operand zero = emitter<ConstantEmitter>().zeroOf(Representation::JSValue);
        defineResults(value, { zero, zero });
        return;
    }

    ValueID other = value->otherOperand();
    SecondReceiver secondReceiver = value->readsFromFirstResult() ? SecondReceiver::FirstResult : other ? SecondReceiver::Other : SecondReceiver::FirstReceiver;
    Vector<LValue, 2> operands { in<Boxed>(value->objectOperand()).value() };
    if (secondReceiver == SecondReceiver::Other)
        operands.append(in<Boxed>(other).value());
    AccessEmitter& accesses = emitter<AccessEmitter>();
    Boxed receiver = in<Boxed>(value->objectOperand());
    accesses.recordArrival(value, receiver);
    if (secondReceiver != SecondReceiver::FirstResult)
        accesses.recordArrival(value, secondReceiver == SecondReceiver::Other ? in<Boxed>(other) : receiver, 1);
    CallOptions options { .immediate = *columns };
    StubCallResult read = emitter<RuntimeCallEmitter>().callStubForTwoReads(value, getByIdPair(secondReceiver), operands.span(), options, value->readOrigin());
    if (secondReceiver == SecondReceiver::FirstResult)
        accesses.recordArrival(value, Boxed(read.first), 1);
    defineResults(value, { Boxed(read.first), Boxed(read.second) });
}

void FusedAccessEmitter::generatePropGetAndTest(const ValueContext& value)
{
    std::optional<uint32_t> column = columnOf(build().identifierNumbering(), value->key());
    StringID literal = value->comparedString();
    String text = literal ? emitter<StringEmitter>().textOf(literal) : String();
    if (!value.isBranchCondition() || !column || (literal && !literalFitsInRegister(text))) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        defineTest(value, boolean(false));
        return;
    }

    RuntimeCallEmitter& calls = emitter<RuntimeCallEmitter>();
    LValue receiver = in<Boxed>(value->objectOperand()).value();
    emitter<AccessEmitter>().recordArrival(value, Boxed(receiver));
    CallOptions options { .immediate = *column };
    if (!literal) {
        LValue operands[] = { receiver };
        calls.callStubAndBranch(value, getByIdIsTruthy(), operands, options, value.branchTargets());
        return;
    }
    LValue operands[] = { receiver, m_out.constInt64(static_cast<int64_t>(packStringIntoRegister(text))) };
    calls.callStubAndBranch(value, getByIdEqualsLiteral(text.length()), operands, options, value.branchTargets());
}

void FusedAccessEmitter::generateTryPutByIds(const ValueContext& value)
{
    defineTest(value, PropertyRunEmitter(generator()).tryStore(value));
}

Condition PropertyRunEmitter::tryStore(const ValueContext& store)
{
    std::optional<Request> request = store->request();
    if (!request)
        return boolean(false);
    SiteCacheID siteCache = store->siteCache();
    bool probesSiteCache = siteCache && probesSiteCacheInline(store.shape());
    if (!probesSiteCache && !paysWithoutSiteCache(store))
        return boolean(false);

    FusedPropertyRun run { in<CellPointer>(store->objectOperand()), { }, *request };
    for (ValueID stored : store->valuesOperands())
        run.values.append(in<Boxed>(stored));
    RELEASE_ASSERT(run.values.size() == store->keys().size());
    if (!probesSiteCache)
        return storeByOperation(store, run, pointerConstant(0), false);

    Pointer cache = siteCacheAddress(siteCache);
    Integer header(m_out.load64(m_out.address(m_heaps.AOTInstance_siteCaches, cache.value(), Slot::offsetOfStructureID())));
    return either<Condition>(equal(low32(header), loadStructureID(run.object)), [&] {
        Word32 kind = bitAnd(low32(Integer(m_out.lShr(header.value(), m_out.constInt32(Slot::kindShiftInHeader)))), word32(UINT8_MAX));
        return either<Condition>(notEqual(kind, word32(static_cast<int32_t>(Slot::Kind::Uncacheable))), [&] {
            storeThroughSiteCache(store, run, cache, header, kind);
            return boolean(true);
        }, [&] {
            return boolean(false);
        });
    }, [&] {
        // A site that sees several Structures in turn takes the single stores between its refills.
        return either<Condition>(countMiss(cache), [&] {
            return storeByOperation(store, run, cache, true);
        }, [&] {
            return boolean(false);
        });
    });
}

// A run of stores calls the operation to fill its site cache.
bool PropertyRunEmitter::paysWithoutSiteCache(const ValueContext& store)
{
    if (!store->isDefinition())
        return false;
    return !probesSiteCacheInline(store.shape()) || !mayReadSiteCachesAgain(analyses(), store.block());
}

// What countSiteCacheMiss() does in a stub. The answer says whether the site is due for a refill.
Condition PropertyRunEmitter::countMiss(Pointer cache)
{
    Pointer states = loadImmutablePointer(AOT_INSTANCE_OFFSET(propertyCaches.siteCacheStates));
    LValue offset = m_out.bitAnd(m_out.lShr(cache.value(), m_out.constInt32(SiteCacheState::slotAddressShift)), m_out.constIntPtr(SiteCacheState::offsetMask));
    LValue state = m_out.add(states.value(), offset);
    TypedPointer missesLeft = m_out.address(m_heaps.entire(AbstractHeap::RuntimeState), state, SiteCacheState::offsetOfMissesLeft());
    Word32 count(m_out.load8ZeroExt32(missesLeft));
    m_out.store32As8(m_out.sub(count.value(), m_out.constInt32(1)), missesLeft);
    return equal(count, word32(0));
}

Condition PropertyRunEmitter::storeByOperation(const ValueContext& store, const FusedPropertyRun& run, Pointer cache, bool isOnRarePath)
{
    Pointer buffer = argumentBuffer(run.values.span());
    return callOperation<Condition>(store, RuntimeOperation::TryPutProperties, { cache, boxCell(run.object), word32(run.request.value()), buffer, word32(store->isDefinition()) }, { .isOnRarePath = isOnRarePath });
}

void PropertyRunEmitter::storeThroughSiteCache(const ValueContext& store, const FusedPropertyRun& run, Pointer cache, Integer header, Word32 kind)
{
    const B3::AbstractHeap& properties = m_heaps.entire(AbstractHeap::NamedProperties);
    LValue firstWordIndex = m_out.aShr(header.value(), m_out.constInt32(Slot::locationShiftInHeader));
    LValue first = m_out.add(run.object.value(), m_out.shl(firstWordIndex, m_out.constInt32(3)));
    for (size_t i = 0; i < run.values.size(); ++i)
        m_out.store64(run.values[i].value(), m_out.address(properties, first, i * sizeof(EncodedJSValue)));

    Word32 newStructureID(m_out.load32(m_out.address(m_heaps.AOTInstance_siteCaches, cache.value(), Slot::offsetOfPayload())));
    Condition addsProperties = equal(kind, word32(static_cast<int32_t>(Slot::Kind::Transition)));
    storeField(run.object, m_heaps.JSCell_structureID, select(addsProperties, newStructureID, low32(header)));
    // The Structure is a cell the object refers to, so a run of numbers needs the barrier as well.
    emitter<BarrierEmitter>().barrier(store, run.object);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
