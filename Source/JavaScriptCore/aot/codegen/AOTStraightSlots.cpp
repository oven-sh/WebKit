/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStraightEmitter.h"

#if ENABLE(AOT)

#include "AOTPlan.h"
#include "AOTStubCatalog.h"
#include "AOTStubsArithmetic.h"
#include "AOTStubsBarriers.h"
#include "AOTStubsClosures.h"
#include "AllowMacroScratchRegisterUsage.h"
#include "RememberedSlot.h"

namespace JSC { namespace AOT { namespace Typed {

// Boxed, undefined, null and absent are 10, 2 and 0. In a slot of numbers the three follow each other in that order, in the upper 16 bits.
static constexpr unsigned shiftOfMarkerInNumberSlot = 48;

static constexpr uint32_t addendOfMarkerInNumberSlot(SlotMarkers markers)
{
    return 6 + (static_cast<uint32_t>(markers.undefined >> shiftOfMarkerInNumberSlot) << 3);
}

static constexpr uint64_t markerInNumberSlot(uint64_t boxed, SlotMarkers markers)
{
    return static_cast<uint64_t>(((static_cast<uint32_t>(boxed) ^ JSValue::ValueUndefined) + addendOfMarkerInNumberSlot(markers)) >> 3) << shiftOfMarkerInNumberSlot;
}

static constexpr bool markersInNumberSlotComeOut(SlotMarkers markers)
{
    return markerInNumberSlot(JSValue::ValueUndefined, markers) == markers.undefined
        && markerInNumberSlot(JSValue::ValueNull, markers) == markers.null
        && markerInNumberSlot(JSValue::ValueEmpty, markers) == markers.absent;
}

static_assert(markersInNumberSlotComeOut(integerMarkers) && markersInNumberSlotComeOut(doubleMarkers));

static void boxNumberOfSlotInStraightForm(StraightEmitter& emitter, BoxedNumberForm form, CCallHelpers::Address address, GPRReg boxedResult)
{
    CCallHelpers& jit = emitter.jit();
    AllowMacroScratchRegisterUsage allowScratch(jit);
    StubKey key = AOT::boxNumber(form);
    const StubSignature& signature = emitter.catalog().summaryOf(key).signature;
    Reg number = signature.operands[0];
    if (number.isFPR())
        jit.loadDouble(address, number.fpr());
    else
        jit.load64(address, number.gpr());
    emitter.records().stubCalls.call(jit, key, { });
    jit.move(signature.firstResult.gpr(), boxedResult);
}

static void convertToNumberOfSlotInStraightForm(CCallHelpers& jit, SlotRepresentation representation, GPRReg word, FPRReg number)
{
    bool holdsIntegers = representation == SlotRepresentation::OptionalInt64;
    CCallHelpers::Jump isMarker = jit.branchIfNotNumber(word);
    CCallHelpers::Jump isInt32 = jit.branchIfInt32(word);
    jit.unboxDoubleWithoutAssertions(word, word, number);
    if (holdsIntegers)
        jit.truncateDoubleToInt64(number, word);
    CCallHelpers::Jump isConvertedFromDouble = jit.jump();

    isInt32.link(&jit);
    if (holdsIntegers)
        jit.signExtend32ToPtr(word, word);
    else {
        jit.convertInt32ToDouble(word, number);
        jit.moveDoubleTo64(number, word);
    }
    CCallHelpers::Jump isConvertedFromInt32 = jit.jump();

    isMarker.link(&jit);
    jit.xor32(CCallHelpers::TrustedImm32(JSValue::ValueUndefined), word);
    jit.add32(CCallHelpers::TrustedImm32(static_cast<int32_t>(addendOfMarkerInNumberSlot(holdsIntegers ? integerMarkers : doubleMarkers))), word);
    jit.urshift32(CCallHelpers::TrustedImm32(3), word);
    jit.lshift64(CCallHelpers::TrustedImm32(shiftOfMarkerInNumberSlot), word);

    isConvertedFromDouble.link(&jit);
    isConvertedFromInt32.link(&jit);
}

static bool closureTableHoldsClosureInStraightForm(StraightEmitter& emitter, FunctionID function)
{
    return function && emitter.conventions().exists(emitter.conventions().closureEntryOf(function)) && emitter.plan().hasClosureCell(function);
}

void StraightEmitter::loadSlot(GPRReg cell, const PlacedSlot& slot, GPRReg boxedResult)
{
    CCallHelpers::Address address(cell, static_cast<int32_t>(slot.offset));
    if (slot.storage == SlotStorage::Inline && !slot.holdsAccessor) {
        switch (slot.representation) {
        case SlotRepresentation::JSValue:
        case SlotRepresentation::Cell:
            m_jit->load64(address, boxedResult);
            return;
        case SlotRepresentation::OptionalInt64:
            boxNumberOfSlotInStraightForm(*this, BoxedNumberForm::OptionalInteger, address, boxedResult);
            return;
        case SlotRepresentation::OptionalDouble:
            boxNumberOfSlotInStraightForm(*this, BoxedNumberForm::OptionalDouble, address, boxedResult);
            return;
        default:
            break;
        }
    }
    fail(Opcode::GetEnvironmentField);
}

void StraightEmitter::storeSlot(GPRReg cell, const PlacedSlot& slot, GPRReg boxed)
{
    CCallHelpers::Address address(cell, static_cast<int32_t>(slot.offset));
    if (slot.storage == SlotStorage::Inline && !slot.holdsAccessor) {
        switch (slot.representation) {
        case SlotRepresentation::JSValue:
        case SlotRepresentation::Cell:
            m_jit->store64(boxed, address);
            barrierAfterStore(cell);
            return;
        case SlotRepresentation::OptionalInt64:
        case SlotRepresentation::OptionalDouble:
            convertToNumberOfSlotInStraightForm(*m_jit, slot.representation, boxed, takeResultFPR());
            m_jit->store64(boxed, address);
            return;
        default:
            break;
        }
    }
    fail(Opcode::PutEnvironmentField);
}

void StraightEmitter::loadVariable(VariableID variable, GPRReg boxedResult)
{
    PlannedVariable planned = plan().variable(variable);
    bool isBornAsFunction = planned.storage == VariableStorage::ClosureTable || planned.encoding == VariableEncoding::FunctionAsZero;
    if (planned.encoding == VariableEncoding::UndefinedAsZero || planned.representation != SlotRepresentation::JSValue
        || plan().deferredDefinitions().findDeferred(variable) || (isBornAsFunction && !closureTableHoldsClosureInStraightForm(*this, planned.function))) {
        fail(Opcode::GetModuleVariable);
        return;
    }

    storeWhatWaitsInRegister();
    CCallHelpers::Jump isStored;
    if (planned.hasWordInStatics()) {
        relocations().loadInstanceWord({ SymbolSpace::InstanceWord, mentions().add(NumberSpace::Variable, variable) }, InstanceWordWidth::Word64, instanceGPR, boxedResult);
        if (!isBornAsFunction)
            return;
        isStored = m_jit->branchTest64(CCallHelpers::NonZero, boxedResult);
    }
    // The stub alone reads the cell of the closure table, and Image numbers the cells that code mentions.
    mentions().add(NumberSpace::ClosureCell, planned.function);
    callStub(Origin { }, closureOfTable(), { }, { .numberedImmediate = NumberedImmediate { mentions().add(NumberSpace::ClosureFunction, planned.function) } });
    m_jit->move(resultOfCall(), boxedResult);
    if (isStored.isSet())
        isStored.link(m_jit);
}

void StraightEmitter::storeVariable(VariableID variable, GPRReg boxed)
{
    PlannedVariable planned = plan().variable(variable);
    if (!planned.hasWordInStatics() || planned.encoding == VariableEncoding::UndefinedAsZero || planned.representation != SlotRepresentation::JSValue) {
        fail(Opcode::PutModuleVariable);
        return;
    }

    AllowMacroScratchRegisterUsage allowScratch(*m_jit);
    relocations().storeInstanceWord({ SymbolSpace::InstanceWord, mentions().add(NumberSpace::Variable, variable) }, InstanceWordWidth::Word64, boxed, instanceGPR);
    GPRReg card = m_jit->scratchRegister();
    m_jit->move(CCallHelpers::TrustedImm32(RememberedSlotSet::writtenCard), card);
    relocations().storeInstanceWord({ SymbolSpace::InstanceWord, mentions().add(NumberSpace::CardOfVariable, variable) }, InstanceWordWidth::Byte, card, instanceGPR);
}

void StraightEmitter::barrierAfterStore(GPRReg owner)
{
    StraightArgument ownerInRegister = StraightArgument::inRegister(owner);
    callStub(Origin { }, tryWithOperandRegister(writeBarrier(), owner).value_or(writeBarrier()), { &ownerInRegister, 1 }, { });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
