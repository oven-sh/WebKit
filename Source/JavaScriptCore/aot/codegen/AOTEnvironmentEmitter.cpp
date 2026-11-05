/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTEnvironmentEmitter.h"

#if ENABLE(AOT)

#include "AOTFunctionKinds.h"
#include "AOTIRProgram.h"
#include "AOTRepresentationRules.h"
#include "AOTStaticsEmitter.h"
#include "AOTTypeArena.h"

namespace JSC { namespace AOT { namespace Typed {

// In the form in which NewClosure stores the word: ConstantEmitter::captureOf().
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
        // The mark of a closure that is not made: the allocation has laid it, with the scope beside it. Whatever else lies in the slot is a closure of the function, made over the scope of this object or stored over the mark
        // by anybody, the maker among them (`if (from) made.get = from.get`): it says itself what it captures.
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
    define(value, closureCapture(in<CellPointer>(value->closureOperand()), value->function(), value->index()));
}

TypedPointer EnvironmentEmitter::escapeMarkOf(ValueID environment)
{
    std::optional<uint32_t> isEscapedOffset = m_plan.layout(typeOf(environment).environment).isEscapedOffset;
    RELEASE_ASSERT(isEscapedOffset);
    return m_out.address(m_heaps.collectorState, in<CellPointer>(environment).value(), *isEscapedOffset);
}

void EnvironmentEmitter::generateMarkEnvironmentEscaped(const ValueContext& value)
{
    m_out.store32As8(m_out.constInt32(1), escapeMarkOf(value->environmentOperand()));
}

void EnvironmentEmitter::generateReleaseEnvironment(const ValueContext& value)
{
    // Without the mark something may still read the cell, under this name or another.
    if (!m_code.marksOf(value.id()).contains(SweepMark::ReleasesOperand))
        return;

    ValueID environment = value->environmentOperand();
    LayoutID layout = typeOf(environment).environment;
    Word32 isEscaped(m_out.load8ZeroExt32(escapeMarkOf(environment)));
    ifElse(equal(isEscaped, word32(0)), [&] {
        CellPointer cell = in<CellPointer>(environment);
        m_out.store64(cell.value(), emitter<StaticsEmitter>().wordOfEnvironmentHandedBack(layout));
    }, [] { });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
