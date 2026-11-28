/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTEnvironmentEmitter.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTFunctionKinds.h"
#include "AOTHintEmitter.h"
#include "AOTIRProgram.h"
#include "AOTRepresentationRules.h"
#include "AOTStaticsEmitter.h"
#include "AOTTypeArena.h"

namespace JSC { namespace AOT { namespace Typed {

Operand EnvironmentEmitter::closureCapture(CellPointer closure, FunctionID function, unsigned index)
{
    RepresentationContext context { m_program, m_plan, types(), m_unit };
    Representation representation = representationOfCapture(context, function, index);
    const Machine::IndexedAbstractHeap& captureWords = m_plan.functionKind(m_program, conventions().closureEntryOf(function)).isSlim ? m_heaps.AOTSlimClosure_captures : m_heaps.AOTClosure_captures;
    TypedPointer word = m_out.address(closure.value(), captureWords[closureCaptureSlotOf(m_program, conventions(), function, index)]);
    return Operand(representation, m_out.load(word, machineTypeOf(representation)));
}

void EnvironmentEmitter::generateGetCapture(const ValueContext& value)
{
    const Operation& closure = m_code.at(value->closureOperand());
    if (const PlacedSlot* scope = generator().slotOfScopeOfClosureWithoutValue(value->closureOperand())) {
        CellPointer holder = in<CellPointer>(closure.objectOperand());
        FieldAccess access = fieldAccessFor(closure.key(), closure.receivers(), false);
        RELEASE_ASSERT(access.hasLocation && !access.findsOffsetByShape && !access.slot.isOutOfLine());
        static_assert(!uncreatedDeferredClosure);
        Word32 made(m_out.load32(m_out.address(m_heaps.AOTObject_scopeAndClosuresOfSlots, holder.value(), access.slot.offset)));
        define(value, either<CellPointer>(isZero(made), [&] {
            return decode(NarrowReference(m_out.load32(m_out.address(m_heaps.AOTObject_scopeAndClosuresOfSlots, holder.value(), scope->offset))));
        }, [&] {
            return closureCapture(decode(NarrowReference(made.value())), value->function(), value->index()).as<CellPointer>();
        }));
        return;
    }
    CellPointer holder = in<CellPointer>(value->closureOperand());
    emitter<HintEmitter>().crashIfLifetimeEnded(value, holder);
    define(value, closureCapture(holder, value->function(), value->index()));
}

TypedPointer EnvironmentEmitter::byteWithEscapeMarkOf(ValueID environment)
{
    RELEASE_ASSERT(m_plan.layout(typeOf(environment).environment).cellsAreHandedBack);
    return m_out.address(m_heaps.collectorState, in<CellPointer>(environment).value(), offsetOfEnvironmentShapeHighByte);
}

void EnvironmentEmitter::markEscaped(ValueID environment)
{
    uint32_t shape = m_plan.environmentAllocation(typeOf(environment).environment).shape;
    m_out.store32As8(m_out.constInt32(highByteOfEnvironmentShape(shape) | environmentIsEscapedBit), byteWithEscapeMarkOf(environment));
}

TypedPointer EnvironmentEmitter::linkToNextCellHandedBack(CellPointer cell, LayoutID layout)
{
    const PlannedLayout& planned = m_plan.layout(layout);
    RELEASE_ASSERT(planned.cellsAreHandedBack);
    return m_out.address(m_heaps.entire(AbstractHeap::EnvironmentFields), cell.value(), planned.offsetOfLinkToNextCellHandedBack);
}

void EnvironmentEmitter::generateMarkEnvironmentEscaped(const ValueContext& value)
{
    markEscaped(value->environmentOperand());
}

void EnvironmentEmitter::markEnvironmentsThatClosureKeeps(const ValueContext& closure)
{
    if (closure->loan() != LoanOfClosure::None)
        return;
    markEnvironmentsKept(closure->capturesOperands());
}

void EnvironmentEmitter::markEnvironmentsKept(std::span<const ValueID> captures)
{
    for (ValueID capture : captures) {
        if (!capture || m_code.isConstant(capture))
            continue;
        TypeID type = m_code.at(capture).type;
        if (!type || holdsOnlyPrimitives(type))
            continue;
        LayoutID environment = typeOf(capture).environment;
        if (!environment)
            continue;
        const PlannedLayout& planned = m_plan.layout(environment);
        if (planned.cellsAreHandedBack)
            markEscaped(capture);
    }
}

void EnvironmentEmitter::generateReleaseEnvironment(const ValueContext& value)
{
    if (!m_code.marksOf(value.id()).contains(SweepMark::ReleasesOperand))
        return;

    ValueID environment = value->environmentOperand();
    LayoutID layout = typeOf(environment).environment;
    Word32 byteWithEscapeMark(m_out.load8ZeroExt32(byteWithEscapeMarkOf(environment)));
    ifElse(hasNoBits(byteWithEscapeMark, environmentIsEscapedBit), [&] {
        if (emitter<HintEmitter>().shouldEmitLifetimeEnds()) {
            emitter<HintEmitter>().emitLifetimeEnd(value, environment);
            return;
        }
        Machine::DeclaredAccessesSuspension writesToCellThatNobodyHolds(m_out);
        CellPointer cell = in<CellPointer>(environment);
        TypedPointer head = emitter<StaticsEmitter>().wordOfEnvironmentHandedBack(layout);
        m_out.store64(add(Integer(m_out.load64(head)), integer(tagOfLinkBetweenEnvironmentsHandedBack)).value(), linkToNextCellHandedBack(cell, layout));
        m_out.store64(cell.value(), head);
    }, [] { });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
