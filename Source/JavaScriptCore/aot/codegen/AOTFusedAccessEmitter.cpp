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
#include "AOTPlacement.h"
#include "AOTPropertyCachesFront.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTRuntimeRecords.h"
#include "AOTStringEmitter.h"
#include "AOTStubsPropertyKeys.h"
#include "AOTVector.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

static_assert(RowDisplacementTable::maxColumn < 1u << secondNameShift);
static_assert(!Slot::offsetOfStructureID() && sizeof(StructureID) == sizeof(uint32_t));
static_assert(OBJECT_OFFSETOF(Slot, transition.newStructureID) == Slot::offsetOfPayload());

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

void FusedAccessEmitter::generateTryPutByIds(const ValueContext& store)
{
    defineTest(store, tryPutByIds(store));
}

Condition FusedAccessEmitter::tryPutByIds(const ValueContext& store)
{
    std::optional<Request> request = store->request();
    if (!request)
        return boolean(false);
    SiteCacheID siteCache = store->siteCache();
    bool shapeProbesInline = probesSiteCacheInline(store.shape());
    bool probesSiteCache = siteCache && shapeProbesInline;
    // A run of stores calls the operation to fill its site cache.
    if (!probesSiteCache && (!store->isDefinition() || (shapeProbesInline && mayReadSiteCachesAgain(analyses(), store.block()))))
        return boolean(false);

    CellPointer object = in<CellPointer>(store->objectOperand());
    Vector<Boxed, 8> values;
    for (ValueID stored : store->valuesOperands())
        values.append(in<Boxed>(stored));
    RELEASE_ASSERT(values.size() == store->keys().size());
    auto refuse = [&] {
        return boolean(false);
    };
    auto storeByOperation = [&](Pointer cache, bool isOnRarePath) {
        Pointer buffer = argumentBuffer(values.span());
        return callOperation<Condition>(store, RuntimeOperation::TryPutProperties, { cache, boxCell(object), word32(request->value()), buffer, word32(store->isDefinition()) }, { .isOnRarePath = isOnRarePath });
    };
    if (!probesSiteCache)
        return storeByOperation(pointerConstant(0), false);

    Pointer cache = siteCacheAddress(siteCache);
    Integer header(m_out.load64(m_out.address(m_heaps.AOTInstance_siteCaches, cache.value(), Slot::offsetOfStructureID())));
    return either<Condition>(equal(low32(header), loadStructureID(object)), [&] {
        Word32 kind = bitAnd(low32(Integer(m_out.lShr(header.value(), m_out.constInt32(Slot::kindShiftInHeader)))), word32(UINT8_MAX));
        return either<Condition>(notEqual(kind, word32(static_cast<int32_t>(Slot::Kind::Uncacheable))), [&] {
            const B3::AbstractHeap& properties = m_heaps.entire(AbstractHeap::NamedProperties);
            LValue firstWordIndex = m_out.aShr(header.value(), m_out.constInt32(Slot::locationShiftInHeader));
            LValue first = m_out.add(object.value(), m_out.shl(firstWordIndex, m_out.constInt32(3)));
            for (size_t i = 0; i < values.size(); ++i)
                m_out.store64(values[i].value(), m_out.address(properties, first, i * sizeof(EncodedJSValue)));

            Word32 newStructureID(m_out.load32(m_out.address(m_heaps.AOTInstance_siteCaches, cache.value(), Slot::offsetOfPayload())));
            Condition addsProperties = equal(kind, word32(static_cast<int32_t>(Slot::Kind::Transition)));
            storeField(object, m_heaps.JSCell_structureID, select(addsProperties, newStructureID, low32(header)));
            // The Structure is a cell the object refers to, so a run of numbers needs the barrier as well.
            emitter<BarrierEmitter>().barrier(store, object);
            return boolean(true);
        }, refuse);
    }, [&] {
        // A site that sees several Structures in turn takes the single stores between its refills.
        return either<Condition>(emitter<AccessEmitter>().countMissAtSite(cache), [&] {
            return storeByOperation(cache, true);
        }, refuse);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
