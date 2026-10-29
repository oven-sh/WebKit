/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCallEmitter.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTChooseCallingConventions.h"
#include "AOTConvention.h"
#include "AOTImageFormat.h"
#include "AOTInstance.h"
#include "AOTMethodTableEntrypoints.h"
#include "AOTPlacement.h"
#include "AOTShapeTestEmitter.h"
#include "AOTVector.h"
#include "B3UpsilonValue.h"
#include "B3ValueInlines.h"
#include <array>
#include <optional>
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT { namespace Typed {

CallEmitter::PlacedArguments CallEmitter::placeArguments(FunctionID function, const ArgumentLocations& locations, std::span<const Operand> arguments, OmittedArguments omitted)
{
    RELEASE_ASSERT(arguments.size() == locations.parameters.size() && locations.convention == EntryConvention::Direct);

    PlacedArguments placed;
    placed.onStack.fill(nullptr, locations.numberOfStackArguments);
    placed.stackWordsToReserve = inputs().conventions.stackWordsReservedByCallersOf(m_program, function, locations);
    std::array<LValue, numberOfDirectArgumentGPRs> inGPRs { };
    unsigned numberOfGPRs = 0;
    for (size_t i = 0; i < arguments.size(); ++i) {
        const ArgumentLocation& location = locations.parameters[i];
        RELEASE_ASSERT(arguments[i].representation == location.representation);
        // Null where the callee may find anything: nothing is put there.
        LValue argument = arguments[i].value;
        switch (location.kind) {
        case ArgumentLocation::Kind::None:
            break;
        case ArgumentLocation::Kind::GPR:
            RELEASE_ASSERT(numberOfGPRs < numberOfDirectArgumentGPRs && location.reg == Reg(directArgumentGPR(numberOfGPRs)));
            inGPRs[numberOfGPRs++] = argument;
            break;
        case ArgumentLocation::Kind::FPR:
            if (argument)
                placed.inRegisters.append(B3::ConstrainedValue(argument, B3::ValueRep::reg(location.reg)));
            break;
        case ArgumentLocation::Kind::Stack:
            if (argument)
                placed.onStack[location.stackIndex] = argument->type() == B3::Int32 ? m_out.zeroExt(argument, B3::Int64) : argument;
            break;
        }
    }

    unsigned firstUndefined = numberOfGPRs;
    if (omitted == OmittedArguments::MayBeOmitted) {
        while (firstUndefined && inGPRs[firstUndefined - 1] && isUndefinedConstant(inGPRs[firstUndefined - 1]))
            --firstUndefined;
    }
    SecondaryEntry entry = inputs().conventions.findEntryOmittingArguments(function, firstUndefined, numberOfGPRs);
    RELEASE_ASSERT(firstUndefined <= entry.firstOmittedGPR && entry.firstOmittedGPR <= numberOfGPRs);
    placed.entry = entry.id;
    for (unsigned index = 0; index < entry.firstOmittedGPR; ++index) {
        if (inGPRs[index])
            placed.inRegisters.append(B3::ConstrainedValue(inGPRs[index], B3::ValueRep::reg(directArgumentGPR(index))));
    }
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
    for (size_t index = 0; index < arguments.size(); ++index) {
        if (m_function.at(value->argumentsOperands()[index]).opcode == Opcode::Unspecified)
            arguments[index].value = nullptr;
    }
    if (value.returnCall()) {
        emitTailCall(value, CallTarget::function(function, primaryEntry), placeArguments(function, locations, arguments.span(), OmittedArguments::MustBePassed));
        return;
    }

    PlacedArguments placed = placeArguments(function, locations, arguments.span(), OmittedArguments::MayBeOmitted);
    CallOptions options { .keptCalleePlusOne = value->calleeIsKeptInFrame() ? m_records.sites.addKeptCallee(m_function, value->position, value->inlineFrame) : 0, .constructs = value->isConstruction(), .callsOwnFunctionObject = value->callsOwnFunctionObject(), .hasLandingOfMisfit = generator().calleeTestsResultOf(value) };
    if (const Function& callee = m_program.at(function); options.constructs && callee.kind == FunctionKind::Plain && callee.role != FunctionRole::ConstructBody)
        options.constructedCallee = function;
    B3::PatchpointValue* call = emitCall(value, CallTarget::function(function, placed.entry), placed, locations.results.span(), options);
    if (options.hasLandingOfMisfit) {
        meetLandingOfMisfit(value, call);
        return;
    }
    defineResultsOfCall(value, locations.results.span(), call);
}

// The back end defines the result of a call with two ways out on the first alone, so the two meet. The test of the result, which is generated next, fills the landing.
void CallEmitter::meetLandingOfMisfit(const ValueContext& value, LValue returned)
{
    CodeGenerator::LandingOfMisfit landing { .landing = std::exchange(m_landingOfLastCall, nullptr), .meeting = newBlock() };
    RELEASE_ASSERT(landing.landing);
    LBasicBlock afterReturn = m_out.m_block;
    m_out.appendTo(landing.meeting);
    landing.result = m_out.phi(B3::Int64);
    landing.hasTestedType = m_out.phi(B3::Int32);

    m_out.appendTo(afterReturn);
    m_out.m_block->appendNew<B3::UpsilonValue>(m_proc, m_out.origin(), returned, landing.result);
    m_out.m_block->appendNew<B3::UpsilonValue>(m_proc, m_out.origin(), m_out.constInt32(1), landing.hasTestedType);
    m_out.jump(landing.meeting);

    m_out.appendTo(landing.meeting);
    define(value, Boxed(landing.result));
    generator().setLandingOfMisfit(value.id(), landing);
}

// The instance has its own copy of the table at a fixed distance, and the word of the shape is 0 - shape: one load finds the entry.
void CallEmitter::generateCallTable(const ValueContext& value)
{
    FunctionID function = value->function();
    std::optional<uint32_t> selectorOffset = m_placement.methodTable().offsetOf(selectorOf(m_program, value.value()));
    if (!selectorOffset) {
        fail(value, InternalErrorKind::CallToFunctionWithoutCode);
        return;
    }

    ArgumentLocations locations = inputs().conventions.locationsOf(m_program, function);
    Vector<Operand, 8> arguments = operandsOf(value->argumentsOperands(), locations);
    checkIsCompiledObjectOf(arguments[0].as<CellPointer>(), value->receivers());
    Word32 shapeWord = emitter<ShapeTestEmitter>().shapeWordOf(arguments[0].as<CellPointer>());
    Pointer row = offsetBy(Pointer(m_instance), signExtended(shapeWord), WTF::fastLog2(static_cast<unsigned>(sizeof(uint32_t))));
    ptrdiff_t offsetOfRow = InstanceLayout::offsetOfMethodTable(inputs().instanceSizes) + static_cast<ptrdiff_t>(*selectorOffset * sizeof(uint32_t));
    Word32 codeOffset(m_out.load32(m_out.address(m_heaps.AOTInstance_mutableFields, row.value(), offsetOfRow)));
    Pointer code = loadImmutablePointer(AOT_INSTANCE_OFFSET(image.code));
    Pointer entry = offsetBy(code, zeroExtended(codeOffset), WTF::fastLog2(static_cast<unsigned>(codeOffsetUnit)));

    PlacedArguments placed = placeArguments(function, locations, arguments.span(), OmittedArguments::MustBePassed);
    B3::PatchpointValue* call = emitCall(value, CallTarget::address(entry.value()), placed, locations.results.span(), { });
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
