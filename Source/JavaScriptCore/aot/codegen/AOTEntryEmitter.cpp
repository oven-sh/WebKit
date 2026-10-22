/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTEntryEmitter.h"

#if ENABLE(AOT)

#include "AOTArgumentLocations.h"
#include "AOTBorrowedParameters.h"
#include "AOTChooseCallingConventions.h"
#include "AOTClosureWord.h"
#include "AOTConvention.h"
#include "AOTFlowResults.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static std::optional<uint32_t> firstArgumentInMemory(EntryConvention convention)
{
    switch (convention) {
    case EntryConvention::Direct:
        return std::nullopt;
    case EntryConvention::GenericRegisters:
        return numberOfArgumentGPRs;
    case EntryConvention::GenericBuffer:
        return 0;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static uint8_t numberOfRegisterParameters(const Function& function)
{
    switch (entryConventionOf(function)) {
    case EntryConvention::Direct:
    case EntryConvention::GenericBuffer:
        return 0;
    case EntryConvention::GenericRegisters: {
        size_t numberOfArguments = function.parameters.size() + (function.newTargetParameterIndex() ? 1 : 0);
        return static_cast<uint8_t>(std::min<size_t>(numberOfArguments, numberOfArgumentGPRs));
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

EntryEmitter::EntryEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
{
}

void EntryEmitter::generateEntry()
{
    ArgumentLocations locations = inputs().conventions.locationsOf(m_program, m_id);
    const CompactList<ValueID>& parameters = m_function.at(entryBlock).parameters;
    RELEASE_ASSERT(locations.parameters.size() == parameters.size());

    if (std::optional<uint32_t> firstArgument = firstArgumentInMemory(locations.convention)) {
        m_argumentCount = Word32(m_out.castToInt32(m_out.argumentRegister(countGPR)));
        m_argumentList = offsetBy(Pointer(m_out.framePointer()), offsetOfStackWordsFromFrame);
        m_indexOfFirstArgumentInList = *firstArgument;
    }

    Vector<Operand, 8> passed;
    for (const ArgumentLocation& location : locations.parameters)
        passed.append(read(location));
    for (size_t index = 0; index < parameters.size(); ++index)
        generator().defineParameter(parameters[index], passed[index]);
}

Operand EntryEmitter::read(const ArgumentLocation& location)
{
    switch (location.kind) {
    case ArgumentLocation::Kind::None:
        break;
    case ArgumentLocation::Kind::GPR: {
        LValue value = m_out.argumentRegister(location.reg.gpr());
        if (b3TypeOf(location.representation) == B3::Int32)
            value = m_out.castToInt32(value);
        return Operand(location.representation, value);
    }
    case ArgumentLocation::Kind::FPR:
        return Operand(location.representation, m_out.argumentRegister(location.reg.fpr()));
    case ArgumentLocation::Kind::Stack: {
        auto loadStackWord = [&] {
            TypedPointer word = m_out.address(m_heaps.argumentBuffer, m_out.framePointer(), offsetOfStackWordFromFrame(location.stackIndex));
            return m_out.load(word, b3TypeOf(location.representation));
        };
        switch (entryConventionOf(m_function)) {
        case EntryConvention::Direct:
            RELEASE_ASSERT(hasStackArgumentAreaOf(m_function.role));
            return Operand(location.representation, loadStackWord());
        case EntryConvention::GenericRegisters:
            RELEASE_ASSERT(location.representation == Representation::JSValue);
            return either<Boxed>(below(word32(numberOfArgumentGPRs + location.stackIndex), m_argumentCount), [&] {
                return Boxed(loadStackWord());
            }, [&] {
                return undefined();
            });
        case EntryConvention::GenericBuffer:
            break;
        }
        break;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Word32 EntryEmitter::argumentCount() const
{
    RELEASE_ASSERT(m_argumentCount.value());
    return m_argumentCount;
}

Pointer EntryEmitter::argumentList() const
{
    RELEASE_ASSERT(m_argumentCount.value());
    return m_argumentList;
}

uint32_t EntryEmitter::indexOfFirstArgumentInList() const
{
    RELEASE_ASSERT(m_argumentCount.value());
    return m_indexOfFirstArgumentInList;
}

static uint32_t stackWordsPassedBy(const CodeGenerationInputs& inputs, const Value& tailCall)
{
    if (tailCall.opcode == Opcode::CallDirect)
        return inputs.conventions.stackWordsReservedByCallersOf(inputs.program, tailCall.function());
    if (tailCall.opcode != Opcode::Call)
        return 0;
    size_t numberOfArguments = tailCall.argumentsOperands().size();
    return numberOfArguments > numberOfArgumentGPRs ? static_cast<uint32_t>(numberOfArguments - numberOfArgumentGPRs) : 0;
}

static bool passesStackWordsInTailCall(const CodeGenerationInputs& inputs, const Function& function)
{
    return std::ranges::any_of(function.blocks, [&](const Block& block) {
        const Terminator& terminator = block.terminator;
        return terminator.kind == TerminatorKind::ReturnCall && stackWordsPassedBy(inputs, function.at(terminator.callOperand()));
    });
}

bool readsCallerFrame(const CodeGenerationInputs& inputs, FunctionID id, const Function& function)
{
    ArgumentLocations locations = inputs.conventions.locationsOf(inputs.program, id);
    // Without ChooseCallingConventions no caller reserves the stack words of a tail call, so the call leaves from an argument list frame.
    return locations.convention != EntryConvention::Direct
        || locations.numberOfStackWords()
        || inputs.conventions.of(inputs.program, id).reservedStackWords
        || passesStackWordsInTailCall(inputs, function);
}

// The entrypoint hands its parameters on to the function, so the table says of the entrypoint what holds of both.
static uint8_t argumentsBorrowedBy(const FlowResults& flow, FunctionID id, const Function& entrypoint)
{
    if (entrypoint.role != FunctionRole::GenericEntrypoint || entryConventionOf(entrypoint) != EntryConvention::GenericRegisters)
        return 0;
    if (entrypoint.newTargetParameterIndex() || entrypoint.hasAny({ FunctionFlags::HasArgumentCount, FunctionFlags::HasHiddenRest }))
        return 0;

    uint8_t borrowed = 0;
    unsigned numberOfArguments = std::min<unsigned>(entrypoint.numberOfPositionalParameters(), ClosureWord::numberOfBorrowedArguments);
    for (unsigned argument = 0; argument < numberOfArguments; ++argument) {
        if (flow.framePointerSummary(id, entrypoint.firstParameterIndex() + argument).uses.containsOnly(usesOfBorrowedParameter))
            borrowed |= 1u << argument;
    }
    return borrowed;
}

EntryDescription describeEntry(const CodeGenerationInputs& inputs, FunctionID id, const Function& function)
{
    EntryDescription description;
    description.isGenericEntrypoint = function.role == FunctionRole::GenericEntrypoint;
    description.numberOfParameters = numberOfRegisterParameters(function);
    description.borrowedArguments = argumentsBorrowedBy(inputs.flow, id, function);
    description.endsItsTailCalls = description.isGenericEntrypoint && inputs.flow.frameArrayParameters().endsItsTailCalls(id);
    return description;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
