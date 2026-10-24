/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValidationEmitter.h"

#if ENABLE(AOT)

#include "AOTAccessEmitter.h"
#include "AOTConstantEmitter.h"
#include "AOTFlowResults.h"
#include "AOTIdentifierNumbering.h"
#include "AOTObjectModelCaches.h"
#include "AOTPlacement.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTStubsPropertyKeys.h"
#include "AOTVector.h"

namespace JSC { namespace AOT { namespace Typed {

static_assert(argumentBufferAccessOf(RuntimeOperation::CloneObjectAs) == ArgumentBufferAccess::Reads);

ValidationEmitter::ValidationEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
{
}

void ValidationEmitter::generateIsPlainDataObject(const ValueContext& value)
{
    ValueID tested = value->valueOperand();
    if (!typeOf(tested).hasCells()) {
        defineTest(value, boolean(false));
        return;
    }
    defineTest(value, isNotZero(callOperation<Integer>(value, RuntimeOperation::IsPlainDataObject, { boxed(tested) })));
}

void ValidationEmitter::generateGetOwnDataProperty(const ValueContext& value)
{
    CellPointer object = in<CellPointer>(value->objectOperand());
    std::optional<uint32_t> identifier = build().identifierNumbering().numberOf(value->key());
    if (!identifier || m_program.isIndexLike(value->key())) {
        define(value, callOperation<Boxed>(value, RuntimeOperation::GetOwnDataProperty, { object, identifierNumber(value->key()) }));
        return;
    }

    // What a plain data object has of its own, an ordinary read finds, and runs nothing. The stubs of ordinary code answer both questions from their tables.
    Boxed receiver = boxCell(object);
    LBasicBlock present = newBlock();
    LBasicBlock done = newBlock();
    LValue hasOwn = m_out.castToInt32(callStub(value, inById(PresenceQuestion::HasOwn), { receiver.value() }, { .immediate = *identifier }).first);
    ValueFromBlock whenAbsent = m_out.anchor(undefined().value());
    m_out.branch(hasOwn, normalEdge(present), normalEdge(done));

    m_out.appendTo(present);
    ValueFromBlock whenPresent = m_out.anchor(emitter<AccessEmitter>().getById(value, receiver, value->key(), { }).value());
    m_out.jump(done);

    m_out.appendTo(done);
    define(value, Boxed(m_out.phi(B3::Int64, whenAbsent, whenPresent)));
}

void ValidationEmitter::generateCloneAs(const ValueContext& value)
{
    std::optional<Request> request = value->request();
    if (!request) {
        fail(value, InternalErrorKind::PlacedPayloadMissing);
        define(value, emitter<ConstantEmitter>().zeroOf(value.representation()));
        return;
    }

    Vector<Boxed, 8> replacedValues;
    for (ValueID replaced : value->valuesOperands())
        replacedValues.append(boxed(replaced));

    Boxed source = boxCell(in<CellPointer>(value->sourceOperand()));
    Pointer values = argumentBuffer(replacedValues.span());
    define(value, callOperation<CellPointer>(value, RuntimeOperation::CloneObjectAs, { source, word32(static_cast<int32_t>(request->value())), values }));
}

void ValidationEmitter::generateFindSourcePlan(const ValueContext& value)
{
    std::optional<Request> request = value->request();
    if (!request) {
        fail(value, InternalErrorKind::PlacedPayloadMissing);
        return;
    }
    Operand arguments[] { in<CellPointer>(value->sourceOperand()), word32(static_cast<int32_t>(request->value())) };
    StubCallResult found = emitter<RuntimeCallEmitter>().callOperationReturningPair(value, RuntimeOperation::FindSourcePlan, arguments);
    Pointer plan(found.first);
    auto wordAt = [&](unsigned index) {
        return Word32(m_out.load32(m_out.address(m_heaps.entire(AbstractHeap::RuntimeState), plan.value(), index * sizeof(uint32_t))));
    };
    defineResults(value, { isNotZero(wordAt(SourcePlan::isUsableWord)), plan, Pointer(zeroExtended(wordAt(SourcePlan::orderWord)).value()), CellPointer(found.second) });
}

void ValidationEmitter::generateGetPlannedProperty(const ValueContext& value)
{
    static_assert(!SourcePlan::absent);
    CellPointer source = in<CellPointer>(value->sourceOperand());
    Pointer plan = in<Pointer>(value->planOperand());
    ptrdiff_t offsetInPlan = (SourcePlan::firstPlaceWord + value->memberIndex()) * sizeof(uint32_t);
    Integer place = signExtended(Word32(m_out.load32(m_out.address(m_heaps.entire(AbstractHeap::RuntimeState), plan.value(), offsetInPlan))));
    Condition isPresent = isNotZero(place);

    LBasicBlock present = newBlock();
    LBasicBlock done = newBlock();
    ValueFromBlock whenAbsent = m_out.anchor(undefined().value());
    m_out.branch(isPresent.value(), normalEdge(present), normalEdge(done));

    m_out.appendTo(present);
    Pointer butterfly = loadField<Pointer>(source, m_heaps.JSObject_butterfly);
    Pointer base = select(Condition(m_out.greaterThan(place.value(), m_out.int64Zero)), Pointer(source.value()), butterfly);
    ValueFromBlock whenPresent = m_out.anchor(m_out.load64(m_out.address(m_heaps.entire(AbstractHeap::NamedProperties), m_out.add(base.value(), place.value()), 0)));
    m_out.jump(done);

    m_out.appendTo(done);
    defineResults(value, { Boxed(m_out.phi(B3::Int64, whenAbsent, whenPresent)), isPresent });
}

void ValidationEmitter::generateGetPreparedPlans(const ValueContext& value)
{
    define(value, callOperation<Boxed>(value, RuntimeOperation::GetPreparedPlans, { }));
}

void ValidationEmitter::generateIsThrownValueIgnored(const ValueContext& value)
{
    defineTest(value, isNotZero(callOperation<Integer>(value, RuntimeOperation::IsThrownValueIgnored, { })));
}

void ValidationEmitter::generateMatchesUnrecorded(const ValueContext& value)
{
    defineTest(value, isNotZero(callOperation<Integer>(value, RuntimeOperation::MatchesUnrecorded, { in<CellPointer>(value->regExpOperand()), in<CellPointer>(value->stringOperand()) })));
}

void ValidationEmitter::generateExpectRows(const ValueContext& value)
{
    LayoutID layout = value->layout();
    const FlowResults& flow = inputs().flow;
    AllocationSiteID site = flow.typedResultSite(layout);
    uint32_t shape = m_placement.shapes().shapeOf(layout, site ? flow.at(site).allocationClass : AllocationClassID());
    if (!shape) {
        fail(value, InternalErrorKind::SlotAccessNotPlaced);
        return;
    }
    callVoidOperation(value, RuntimeOperation::ExpectRows, { boxed(value->targetOperand()), word32(static_cast<int32_t>(shape)) });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
