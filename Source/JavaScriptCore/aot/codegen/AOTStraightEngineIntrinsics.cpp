/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStraightEmitter.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTFacts.h"
#include "AOTIRProgram.h"
#include "AOTPlan.h"
#include "AOTSlotPlan.h"
#include "AOTStubCatalog.h"
#include "AOTStubsAllocation.h"
#include "AOTStubsCoroutines.h"
#include "AOTStubsPropertyKeys.h"
#include <array>

namespace JSC { namespace AOT { namespace Typed {

OpcodeSet StraightEmitter::opcodesOfEngineIntrinsics()
{
    return {
        Opcode::IsConstructor, Opcode::ReadsIteratorOfListUnseen, Opcode::ArraySpeciesCreate, Opcode::GetByValWithThis, Opcode::GetOwnProperty, Opcode::ToLength,
        Opcode::ThrowNewError, Opcode::ResumeGenerator, Opcode::StepGenerator, Opcode::IsGeneratorSuspendedAtStart,
        Opcode::CreateIteratorResult, Opcode::StepArrayIterator, Opcode::StepMapIterator, Opcode::StepSetIterator,
    };
}

static void generateStepGenerator(StraightEmitter& emitter, const BodyValue& value)
{
    GeneratorStepCheck check = value.check();
    if (check != GeneratorStepCheck::IsUnmodifiedGenerator && check != GeneratorStepCheck::IsGenerator) {
        emitter.fail(value);
        return;
    }
    StubKey stub = stepGenerator(check);
    const StubSignature& signature = emitter.catalog().summaryOf(stub).signature;
    emitter.callStub(value, stub, { value.generatorOperand(), StraightArgument::word(static_cast<uint64_t>(value.resumeMode())), value.valueOperand() });
    emitter.defineResult(value, static_cast<uint32_t>(StepGeneratorResult::Value), signature.firstResult.gpr());
    if (!emitter.resultIsRead(value, static_cast<uint32_t>(StepGeneratorResult::Status)))
        return;
    GPRReg status = signature.secondResult.gpr();
    emitter.boxInForm(Representation::Int32, status);
    emitter.defineResult(value, static_cast<uint32_t>(StepGeneratorResult::Status), status);
}

static void generateIsGeneratorSuspendedAtStart(StraightEmitter& emitter, const BodyValue& value)
{
    CCallHelpers& jit = emitter.jit();
    GPRReg generator = StraightEmitter::temporary(1);
    emitter.load(value.generatorOperand(), generator);
    jit.move(CCallHelpers::TrustedImm32(0), emitter.takeResultRegister());
    CCallHelpers::Jump isNotCell = jit.branchIfNotCell(generator);
    emitter.callOperation(value, RuntimeOperation::IsGeneratorSuspendedAtStart, { StraightArgument::inRegister(generator) });
    isNotCell.link(&jit);
    emitter.defineBoolean(value);
}

static bool takesBoxedValueOfIteratorResult(const PlacedSlot& slot)
{
    return slot.storage == SlotStorage::Inline && slot.representation == SlotRepresentation::JSValue && !slot.typeInLayoutRecord() && !slot.holdsAccessor;
}

static bool staysEmptyInZeroedIteratorResult(const PlacedSlot& slot)
{
    SlotTraits traits = traitsOf(slot.representation);
    return slot.storage != SlotStorage::Inline || !traits.hasAbsent || !traits.absentBits;
}

static void generateCreateIteratorResult(StraightEmitter& emitter, const BodyValue& value)
{
    const Program& program = emitter.program();
    LayoutID layout = program.layoutOf(BuiltinLayouts::IteratorResult);
    std::optional<uint32_t> valueField = program.at(layout).findField(program.keyOf(BuiltinNames::value));
    std::optional<uint32_t> doneField = program.at(layout).findField(program.keyOf(BuiltinNames::done));
    RELEASE_ASSERT(valueField && doneField);
    std::array<FieldIndex, 2> fields { *valueField, *doneField };

    const PlannedLayout& planned = emitter.plan().layout(layout);
    std::optional<PlacedAllocation> placed = emitter.plan().allocation(program, layout, fields);
    bool hasStraightForm = placed && !placed->isBornUnfinished && !planned.cellsAreHandedBack;
    for (FieldIndex field = 0; hasStraightForm && field < planned.slots.size(); ++field) {
        const PlacedSlot& slot = planned.slots[field];
        if (field == *valueField || field == *doneField)
            hasStraightForm = takesBoxedValueOfIteratorResult(slot) && !planned.holdsRawValueFromBirth(field) && !emitter.facts().field({ layout, field }).slotType;
        else
            hasStraightForm = staysEmptyInZeroedIteratorResult(slot);
    }
    if (!hasStraightForm) {
        emitter.fail(value);
        return;
    }

    CCallHelpers& jit = emitter.jit();
    GPRReg stored = StraightEmitter::temporary(1);
    emitter.callStub(value, allocateCell(CellInitialization::HeaderAndWordsAndZeros), { StraightArgument::number(emitter.mentions().allocation(*placed, fields)) });
    GPRReg cell = emitter.resultOfCall();
    emitter.load(value.valueOperand(), stored);
    jit.store64(stored, CCallHelpers::Address(cell, static_cast<int32_t>(planned.slots[*valueField].offset)));
    emitter.load(value.doneOperand(), stored);
    jit.store64(stored, CCallHelpers::Address(cell, static_cast<int32_t>(planned.slots[*doneField].offset)));
    if (!isX86_64())
        jit.memoryFence();
    emitter.define(value, cell);
}

static void generateStepOfEngineIterator(StraightEmitter& emitter, const BodyValue& value, RuntimeOperation step)
{
    emitter.callOperation(value, step, { value.iteratorOperand() });
    emitter.defineResult(value, static_cast<uint32_t>(StepArrayIteratorResult::Done), emitter.resultOfCall());
    emitter.defineResult(value, static_cast<uint32_t>(StepArrayIteratorResult::Value), GPRInfo::returnValueGPR2);
}

void StraightEmitter::generateEngineIntrinsic(const BodyValue& value)
{
    switch (value.opcode()) {
    case Opcode::IsConstructor:
        callOperation(value, RuntimeOperation::IsConstructor, { value.valueOperand() });
        defineBoolean(value);
        return;
    case Opcode::ReadsIteratorOfListUnseen:
        callOperation(value, RuntimeOperation::ReadsIteratorOfListUnseen, { value.valueOperand() });
        defineBoolean(value);
        return;
    case Opcode::ArraySpeciesCreate:
        callOperation(value, RuntimeOperation::ArraySpeciesCreate, { value.lengthOperand(), value.arrayOperand() });
        define(value);
        return;
    case Opcode::GetByValWithThis:
        callStub(value, getSuperByVal(), { value.objectOperand(), value.propertyOperand(), value.thisValueOperand() });
        define(value);
        return;
    case Opcode::GetOwnProperty:
        callOperation(value, RuntimeOperation::GetOwnProperty, { value.objectOperand(), StraightArgument::number(mentionOfName(value.key())) });
        define(value);
        return;
    case Opcode::ToLength:
        callOperation(value, RuntimeOperation::ToLength, { value.valueOperand() });
        define(value);
        return;
    case Opcode::ThrowNewError:
        throwError(value.origin(), value.errorKind(), value.message());
        return;
    case Opcode::ResumeGenerator:
        callOperation(value, RuntimeOperation::ResumeGenerator, { value.generatorOperand(), StraightArgument::word(static_cast<uint64_t>(value.resumeMode())), value.valueOperand() });
        define(value);
        return;
    case Opcode::StepGenerator:
        generateStepGenerator(*this, value);
        return;
    case Opcode::IsGeneratorSuspendedAtStart:
        generateIsGeneratorSuspendedAtStart(*this, value);
        return;
    case Opcode::CreateIteratorResult:
        generateCreateIteratorResult(*this, value);
        return;
    case Opcode::StepArrayIterator:
        generateStepOfEngineIterator(*this, value, RuntimeOperation::StraightStepArrayIterator);
        return;
    case Opcode::StepMapIterator:
        generateStepOfEngineIterator(*this, value, RuntimeOperation::StraightStepMapIterator);
        return;
    case Opcode::StepSetIterator:
        generateStepOfEngineIterator(*this, value, RuntimeOperation::StraightStepSetIterator);
        return;
    default:
        fail(value);
        return;
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
