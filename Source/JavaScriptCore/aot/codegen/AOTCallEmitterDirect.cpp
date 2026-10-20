/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCallEmitter.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTChooseCallingConventions.h"
#include "AOTClosureTestEmitter.h"
#include "AOTConvention.h"
#include "AOTInstance.h"
#include "AOTStubsClosures.h"
#include "B3ValueInlines.h"
#include <array>
#include <optional>
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(ClosureWord::numberOfOffsetBits == 32 && isPowerOfTwo(ClosureWord::offsetUnit));

static std::optional<unsigned> findDirectArgumentIndex(Reg reg)
{
    for (unsigned index = 0; index < numberOfDirectArgumentGPRs; ++index) {
        if (reg == Reg(directArgumentGPR(index)))
            return index;
    }
    return std::nullopt;
}

CallEmitter::PlacedArguments CallEmitter::placeArguments(FunctionID function, const ArgumentLocations& locations, std::span<const Operand> arguments, OmittedArguments omitted)
{
    RELEASE_ASSERT(arguments.size() == locations.parameters.size() && locations.convention == EntryConvention::Direct);

    PlacedArguments placed;
    placed.onStack.fill(nullptr, locations.numberOfStackArguments);
    placed.stackWordsToReserve = inputs().conventions.stackWordsReservedByCallersOf(m_program, function);
    std::array<LValue, numberOfDirectArgumentGPRs> inGPRs { };
    unsigned numberOfGPRs = 0;
    for (size_t i = 0; i < arguments.size(); ++i) {
        const ArgumentLocation& location = locations.parameters[i];
        RELEASE_ASSERT(arguments[i].representation == location.representation);
        LValue argument = arguments[i].value;
        switch (location.kind) {
        case ArgumentLocation::Kind::None:
            break;
        case ArgumentLocation::Kind::GPR:
            RELEASE_ASSERT(findDirectArgumentIndex(location.reg) == numberOfGPRs);
            inGPRs[numberOfGPRs++] = argument;
            break;
        case ArgumentLocation::Kind::FPR:
            placed.inRegisters.append(B3::ConstrainedValue(argument, B3::ValueRep::reg(location.reg)));
            break;
        case ArgumentLocation::Kind::Stack:
            placed.onStack[location.stackIndex] = argument->type() == B3::Int32 ? m_out.zeroExt(argument, B3::Int64) : argument;
            break;
        }
    }

    unsigned firstUndefined = numberOfGPRs;
    if (omitted == OmittedArguments::MayBeOmitted) {
        while (firstUndefined && isUndefinedConstant(inGPRs[firstUndefined - 1]))
            --firstUndefined;
    }
    SecondaryEntry entry = inputs().conventions.findEntryOmittingArguments(function, firstUndefined, numberOfGPRs);
    RELEASE_ASSERT(firstUndefined <= entry.firstOmittedGPR && entry.firstOmittedGPR <= numberOfGPRs);
    placed.entry = entry.id;
    for (unsigned index = 0; index < entry.firstOmittedGPR; ++index)
        placed.inRegisters.append(B3::ConstrainedValue(inGPRs[index], B3::ValueRep::reg(directArgumentGPR(index))));
    return placed;
}

Operand CallEmitter::callDirect(const OperationContext& context, FunctionID function, std::span<const Operand> arguments, const CallOptions& options)
{
    ArgumentLocations locations = inputs().conventions.locationsOf(m_program, function);
    RELEASE_ASSERT(m_program.at(function).hasBody() && locations.results.size() == 1);
    PlacedArguments placed = placeArguments(function, locations, arguments, OmittedArguments::MayBeOmitted);
    B3::PatchpointValue* call = emitCall(context, CallTarget::function(function, placed.entry), placed, locations.results.span(), options);
    return resultOfCall(locations.results[0], call);
}

void CallEmitter::generateCallDirect(const ValueContext& value)
{
    FunctionID function = value->function();
    if (!m_program.at(function).hasBody()) {
        fail(value, InternalErrorKind::CallToFunctionWithoutCode);
        return;
    }

    ArgumentLocations locations = inputs().conventions.locationsOf(m_program, function);
    Vector<Operand, 8> arguments = operandsOf(value->argumentsOperands(), locations);
    if (value.returnCall()) {
        emitTailCall(value, CallTarget::function(function, primaryEntry), placeArguments(function, locations, arguments.span(), OmittedArguments::MustBePassed));
        return;
    }

    PlacedArguments placed = placeArguments(function, locations, arguments.span(), OmittedArguments::MayBeOmitted);
    CallOptions options { .constructs = value->isConstruction(), .isTailCall = value->isDemotedTailCall(), .isFollowedByResultTest = value->resultCheck() == ResultCheck::ByCallers };
    B3::PatchpointValue* call = emitCall(value, CallTarget::function(function, placed.entry), placed, locations.results.span(), options);
    defineResultsOfCall(value, locations.results.span(), call);
}

Pointer CallEmitter::valueEntryOf(const OperationContext& context, CellPointer closure, ClosureCells cells)
{
    auto entryOffsetOfFullClosure = [&] {
        return loadField<Word32>(closure, m_heaps.AOTClosure_entry);
    };
    auto entryOffsetOfSlimClosure = [&] {
        return either<Word32>(emitter<ClosureTestEmitter>().isUnreifiedSlimClosure(closure), [&] {
            return loadField<Word32>(closure, m_heaps.AOTSlimClosure_word);
        }, [&] {
            return low32(Integer(callStub(context, closureEntry(), { closure.value() }, { .isOnRarePath = true }).first));
        });
    };

    Word32 entryOffset;
    switch (cells) {
    case ClosureCells::AllFull:
        entryOffset = entryOffsetOfFullClosure();
        break;
    case ClosureCells::AllSlim:
        entryOffset = entryOffsetOfSlimClosure();
        break;
    case ClosureCells::Unknown:
        entryOffset = either<Word32>(hasCellType(closure, CompiledClosureType), entryOffsetOfFullClosure, entryOffsetOfSlimClosure);
        break;
    }

    Pointer code = loadImmutablePointer(AOT_INSTANCE_OFFSET(image.code));
    Pointer entry = offsetBy(code, zeroExtended(entryOffset), WTF::fastLog2(ClosureWord::offsetUnit));
    Word32 distance(m_out.load32(m_out.address(m_heaps.AOTImage_tables, entry.value(), ValueEntryWords::offsetOfDistanceFromEntry())));
    return offsetBy(entry, signExtended(distance), 0);
}

void CallEmitter::generateCallGroup(const ValueContext& value)
{
    FunctionID function = value->function();
    ArgumentLocations locations = inputs().conventions.locationsOf(m_program, function);
    Vector<ValueID, 8> operands { value->closureOperand() };
    operands.append(value->argumentsOperands());
    Vector<Operand, 8> arguments = operandsOf(operands.span(), locations);

    Pointer entry = valueEntryOf(value, arguments[0].as<CellPointer>(), inputs().conventions.closureCellsOf(function));
    PlacedArguments placed = placeArguments(function, locations, arguments.span(), OmittedArguments::MustBePassed);
    if (value.returnCall()) {
        emitTailCall(value, CallTarget::address(entry.value()), placed);
        return;
    }

    B3::PatchpointValue* call = emitCall(value, CallTarget::address(entry.value()), placed, locations.results.span(), { .isTailCall = value->isDemotedTailCall() });
    defineResultsOfCall(value, locations.results.span(), call);
}

void CallEmitter::generateResults(const ValueContext& value)
{
    Vector<Operand, 4> results;
    for (ValueID operand : value->valuesOperands())
        results.append(get(operand));
    defineResults(value, results.span());
}

void CallEmitter::generateResultAt(const ValueContext& value)
{
    define(value, generator().resultAt(value->resultsOperand(), value->index()));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
