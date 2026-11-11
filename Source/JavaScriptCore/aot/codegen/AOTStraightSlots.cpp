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
#include "PureNaN.h"
#include "RememberedSlot.h"
#include <bit>

namespace JSC { namespace AOT { namespace Typed {

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

static void boxNumberInStraightForm(StraightEmitter& emitter, BoxedNumberForm form, GPRReg boxedResult, NOESCAPE const Invocable<void(CCallHelpers&, Reg)> auto& bringNumberInto)
{
    CCallHelpers& jit = emitter.jit();
    AllowMacroScratchRegisterUsage allowScratch(jit);
    StubKey key = AOT::boxNumber(form);
    const StubSignature& signature = emitter.catalog().summaryOf(key).signature;
    bringNumberInto(jit, signature.operands[0]);
    emitter.records().stubCalls.call(jit, key, { });
    jit.move(signature.firstResult.gpr(), boxedResult);
}

static void boxNumberOfSlotInStraightForm(StraightEmitter& emitter, BoxedNumberForm form, CCallHelpers::Address address, GPRReg boxedResult)
{
    boxNumberInStraightForm(emitter, form, boxedResult, [&](CCallHelpers& jit, Reg number) {
        if (number.isFPR())
            jit.loadDouble(address, number.fpr());
        else
            jit.load64(address, number.gpr());
    });
}

static void boxNumberOfVariableInStraightForm(StraightEmitter& emitter, BoxedNumberForm form, VariableID variable, GPRReg boxedResult)
{
    boxNumberInStraightForm(emitter, form, boxedResult, [&](CCallHelpers& jit, Reg number) {
        GPRReg word = number.isFPR() ? boxedResult : number.gpr();
        emitter.relocations().loadInstanceWord({ SymbolSpace::InstanceWord, emitter.mentions().add(NumberSpace::Variable, variable) }, InstanceWordWidth::Word64, instanceGPR, word);
        if (number.isFPR())
            jit.move64ToDouble(word, number.fpr());
    });
}

static std::optional<BoxedNumberForm> formOfRawNumbersInStraightForm(SlotRepresentation representation)
{
    switch (representation) {
    case SlotRepresentation::OptionalInt64:
        return BoxedNumberForm::OptionalInteger;
    case SlotRepresentation::OptionalDouble:
        return BoxedNumberForm::OptionalDouble;
    default:
        return std::nullopt;
    }
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
            ++m_numberOfRawSlotsOfScopesRead;
            boxNumberOfSlotInStraightForm(*this, BoxedNumberForm::OptionalInteger, address, boxedResult);
            return;
        case SlotRepresentation::OptionalDouble:
            ++m_numberOfRawSlotsOfScopesRead;
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
    std::optional<BoxedNumberForm> formOfNumbers = formOfRawNumbersInStraightForm(planned.representation);
    if ((planned.representation != SlotRepresentation::JSValue && (!formOfNumbers || !planned.hasWordInStatics() || isBornAsFunction))
        || plan().deferredDefinitions().findDeferred(variable) || (isBornAsFunction && !closureTableHoldsClosureInStraightForm(*this, planned.function))) {
        fail(Opcode::GetModuleVariable);
        return;
    }

    storeWhatWaitsInRegister();
    if (formOfNumbers) {
        boxNumberOfVariableInStraightForm(*this, *formOfNumbers, variable, boxedResult);
        return;
    }
    CCallHelpers::Jump isStored;
    if (planned.hasWordInStatics()) {
        relocations().loadInstanceWord({ SymbolSpace::InstanceWord, mentions().add(NumberSpace::Variable, variable) }, InstanceWordWidth::Word64, instanceGPR, boxedResult);
        if (!isBornAsFunction)
            return;
        isStored = m_jit->branchTest64(CCallHelpers::NonZero, boxedResult);
    }
    mentions().add(NumberSpace::ClosureCell, planned.function);
    callStub(Origin { }, closureOfTable(), { }, { .numberedImmediate = NumberedImmediate { mentions().add(NumberSpace::ClosureFunction, planned.function) } });
    m_jit->move(resultOfCall(), boxedResult);
    if (isStored.isSet())
        isStored.link(m_jit);
}

static std::optional<int64_t> wordOfNumberInSlot(SlotRepresentation representation, JSValue constant)
{
    if (!constant.isNumber())
        return std::nullopt;
    switch (representation) {
    case SlotRepresentation::OptionalInt64:
        if (!constant.isAnyInt())
            return std::nullopt;
        return constant.asAnyInt();
    case SlotRepresentation::OptionalDouble:
        return std::bit_cast<int64_t>(purifyNaN(constant.asNumber()));
    default:
        return std::nullopt;
    }
}

void StraightEmitter::storeVariable(VariableID variable, ValueID stored)
{
    PlannedVariable planned = plan().variable(variable);
    std::optional<int64_t> word;
    if (std::optional<JSValue> constant = constantOf(stored); constant && planned.hasWordInStatics())
        word = planned.representation == SlotRepresentation::JSValue ? JSValue::encode(*constant) : wordOfNumberInSlot(planned.representation, *constant);
    if (!word) {
        storeVariable(variable, loadIntoResultRegister(stored));
        return;
    }
    GPRReg result = takeResultRegister();
    m_jit->move(CCallHelpers::TrustedImm64(*word), result);
    relocations().storeInstanceWord({ SymbolSpace::InstanceWord, mentions().add(NumberSpace::Variable, variable) }, InstanceWordWidth::Word64, result, instanceGPR);
}

void StraightEmitter::storeVariable(VariableID variable, GPRReg boxed)
{
    PlannedVariable planned = plan().variable(variable);
    bool holdsNumbersRaw = !!formOfRawNumbersInStraightForm(planned.representation);
    if (!planned.hasWordInStatics() || (planned.representation != SlotRepresentation::JSValue && !holdsNumbersRaw)) {
        fail(Opcode::PutModuleVariable);
        return;
    }

    AllowMacroScratchRegisterUsage allowScratch(*m_jit);
    if (holdsNumbersRaw) {
        GPRReg word = temporary(1);
        m_jit->move(boxed, word);
        convertToNumberOfSlotInStraightForm(*m_jit, planned.representation, word, takeResultFPR());
        relocations().storeInstanceWord({ SymbolSpace::InstanceWord, mentions().add(NumberSpace::Variable, variable) }, InstanceWordWidth::Word64, word, instanceGPR);
        return;
    }
    relocations().storeInstanceWord({ SymbolSpace::InstanceWord, mentions().add(NumberSpace::Variable, variable) }, InstanceWordWidth::Word64, boxed, instanceGPR);
    const CodeSymbol card { SymbolSpace::InstanceWord, mentions().add(NumberSpace::CardOfVariable, variable) };
#if CPU(X86_64)
    relocations().withInstanceWordAsOperand(card, instanceGPR, [&](CCallHelpers::Address address) {
        m_jit->store8(CCallHelpers::TrustedImm32(RememberedSlotSet::writtenCard), address);
        return true;
    });
#else
    GPRReg mark = m_jit->scratchRegister();
    m_jit->move(CCallHelpers::TrustedImm32(RememberedSlotSet::writtenCard), mark);
    relocations().storeInstanceWord(card, InstanceWordWidth::Byte, mark, instanceGPR);
#endif
}

void StraightEmitter::barrierAfterStore(GPRReg owner)
{
    StraightArgument ownerInRegister = StraightArgument::inRegister(owner);
    callStub(Origin { }, writeBarrier(), { &ownerInRegister, 1 }, { });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
