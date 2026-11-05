/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCallEmitter.h"

#if ENABLE(AOT)

#include "AOTCallingConventions.h"
#include "AOTCellFormat.h"
#include "AOTConvention.h"
#include "AOTIRProgram.h"
#include "AOTShapeTestEmitter.h"
#include "AOTStubCatalog.h"
#include "AOTStubsCallAdapters.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

CodeUnit CallEmitter::calleeOf(const ValueContext& callDirect) const
{
    return conventions().unitEnteredBy(callDirect.value());
}

CallEmitter::PlacedArguments CallEmitter::placeArguments(CodeUnit callee, const ArgumentLocations& locations, std::span<const Operand> arguments)
{
    RELEASE_ASSERT(arguments.size() == locations.parameters.size() && locations.convention == EntryConvention::Direct);

    PlacedArguments placed;
    placed.onStack.fill(nullptr, locations.numberOfStackArguments);
    // By place among the argument GPRs of the callee. Null: nothing is put there.
    std::array<LValue, numberOfDirectArgumentGPRs> inGPRs { };
    unsigned numberOfGPRs = 0;
    // The plan counts the stack words of entry 0 of each unit.
    placed.stackWordsToReserve = std::max(conventions().stackWordsReservedByCallersOf(m_program, callee), locations.numberOfStackWords());
    for (size_t i = 0; i < arguments.size(); ++i) {
        const ArgumentLocation& location = locations.parameters[i];
        if (location.kind == ArgumentLocation::Kind::GPR) {
            RELEASE_ASSERT(numberOfGPRs < numberOfDirectArgumentGPRs && location.reg == Reg(directArgumentGPR(numberOfGPRs)));
            ++numberOfGPRs;
        }
        // Nobody hands it over, so nobody has made it.
        if (location.kind == ArgumentLocation::Kind::None || !arguments[i].value)
            continue;
        RELEASE_ASSERT(isPassedAs(arguments[i].representation, location.representation));
        LValue argument = arguments[i].value;
        switch (location.kind) {
        case ArgumentLocation::Kind::None:
            break;
        case ArgumentLocation::Kind::GPR:
            inGPRs[numberOfGPRs - 1] = argument;
            break;
        case ArgumentLocation::Kind::FPR:
            placed.inRegisters.append(Machine::ConstrainedValue(argument, Machine::ValueRep::reg(location.reg)));
            break;
        case ArgumentLocation::Kind::Stack:
            placed.onStack[location.stackIndex] = argument->type() == Machine::Int32 ? m_out.zeroExt(argument, Machine::Int64) : argument;
            break;
        }
    }

    for (unsigned index = 0; index < numberOfGPRs; ++index) {
        if (inGPRs[index])
            placed.inRegisters.append(Machine::ConstrainedValue(inGPRs[index], Machine::ValueRep::reg(directArgumentGPR(index))));
    }
    return placed;
}

Operand CallEmitter::callDirect(const OperationContext& context, CodeUnit callee, std::span<const Operand> arguments, const CallOptions& options)
{
    ArgumentLocations locations = conventions().locationsOf(m_program, callee);
    RELEASE_ASSERT(locations.results.size() == 1);
    Vector<Operand, 2> results = callEntry(context, callee, primaryEntry, locations, arguments, options);
    return results[0];
}

Vector<Operand, 2> CallEmitter::callEntry(const OperationContext& context, CodeUnit callee, uint16_t entry, const ArgumentLocations& locations, std::span<const Operand> arguments, const CallOptions& options)
{
    RELEASE_ASSERT(m_program.at(callee.function).hasBody());
    std::span<const ArgumentLocation> results = locations.results.span();
    PlacedArguments placed = placeArguments(callee, locations, arguments);
    Machine::PatchOperation* call = emitCall(context, CallTarget::unit(callee, entry), placed, results, options);

    // emitCall() returns the result itself where one result is passed, and a tuple where several are.
    auto isPassed = [](const ArgumentLocation& result) {
        return result.kind != ArgumentLocation::Kind::None;
    };
    size_t numberOfResultsPassed = std::ranges::count_if(results, isPassed);
    unsigned indexInTuple = 0;
    Vector<Operand, 2> operands;
    for (const ArgumentLocation& result : results) {
        LValue passed = nullptr;
        if (isPassed(result))
            passed = numberOfResultsPassed == 1 ? call : m_out.extract(call, indexInTuple++);
        operands.append(resultOfCall(result, passed));
    }
    return operands;
}

void CallEmitter::tailCallEntry(const OperationContext& context, CodeUnit callee, uint16_t entry, const ArgumentLocations& locations, std::span<const Operand> arguments)
{
    RELEASE_ASSERT(m_program.at(callee.function).hasBody());
    emitTailCall(context, CallTarget::unit(callee, entry), placeArguments(callee, locations, arguments));
}

void CallEmitter::callStartOfModule(const ValueContext& value, CodeUnit start, const ArgumentLocations& locations)
{
    const ModuleEvaluationPlan& evaluation = m_plan.moduleEvaluation();
    const ModuleStart* planned = evaluation.findStartOfBody(start.function);
    bool takesWhatStubReturns = std::ranges::all_of(locations.results, [](const ArgumentLocation& result) {
        if (result.kind == ArgumentLocation::Kind::None)
            return true;
        return result.kind == ArgumentLocation::Kind::GPR && result.reg == Reg(GPRInfo::returnValueGPR) && result.representation == Representation::JSValue;
    });
    if (!planned || !takesWhatStubReturns) {
        fail(value, InternalErrorKind::InvalidModuleEvaluation);
        return;
    }

    CallTarget stub = CallTarget::stub(startModule(), std::nullopt);
    PlacedArguments placed;
    placed.inRegisters.append(Machine::ConstrainedValue(m_out.constInt32(static_cast<int32_t>(evaluation.numberOf(*planned))), Machine::ValueRep::reg(inputs().stubCatalog.summaryOf(stub.calledStub).signature.operands[0])));
    if (value.returnCall()) {
        emitTailCall(value, stub, placed);
        return;
    }
    defineResultsOfCall(value, locations.results.span(), emitCall(value, stub, placed, locations.results.span(), { }));
}

void CallEmitter::generateCallDirect(const ValueContext& value)
{
    CodeUnit callee = calleeOf(value);
    const Function& calledFunction = m_program.at(callee.function);
    if (!calledFunction.hasBody() || !conventions().exists(callee)) {
        fail(value, InternalErrorKind::CallToFunctionWithoutCode);
        return;
    }

    ArgumentLocations locations = conventions().locationsOf(m_program, callee);
    if (callee.role == CodeRole::ModuleStart) {
        callStartOfModule(value, callee, locations);
        return;
    }
    Vector<Operand, 8> arguments = operandsOf(value->argumentsOperands(), locations);
    if (value.returnCall()) {
        tailCallEntry(value, callee, primaryEntry, locations, arguments.span());
        return;
    }

    CallOptions options { .constructs = value->isConstruction(), .callsOwnFunctionObject = value->callsOwnFunctionObject(), .callerSuspendsAtReturn = value.isAwaitedAtOnce() };
    bool entersConstructEntry = callee.role == CodeRole::AllocatingConstructEntry || callee.role == CodeRole::GenericConstructEntry;
    if (options.constructs && calledFunction.kind == FunctionKind::Plain && !entersConstructEntry)
        options.constructedCallee = callee.function;
    PlacedArguments placed = placeArguments(callee, locations, arguments.span());
    Machine::PatchOperation* call = emitCall(value, CallTarget::unit(callee, primaryEntry), placed, locations.results.span(), options);
    defineResultsOfCall(value, locations.results.span(), call);
}

// The word of a selector is where an object with the shape 0 would find its entry, and the shape word of an object is its shape negated: the entry lies as many words below.
// An entry is the offset of code in the image, in units of codeOffsetUnit. The run time writes the table where the guard of a class begins or ends, so nothing that was read from it outlives a call.
void CallEmitter::generateCallTable(const ValueContext& value)
{
    if (emitter<ShapeTestEmitter>().mayBeOtherThanCompiledObject(typeOf(value->receiverOperand()))) {
        fail(value, InternalErrorKind::UnexpectedOperand);
        return;
    }
    callThroughMethodTable(value, value->selector(), value->operands());
}

bool CallEmitter::tryCallThroughMethodTable(const ValueContext& call, PropertyKeyID key)
{
    // A unit of the table is where the table leads.
    if (isTableRole(generator().unit().role))
        return false;
    const Type& receiver = typeOf(call->thisValueOperand());
    if (!receiver.isObject() || !receiver.layouts || !receiver.hasOnlyLayouts() || emitter<ShapeTestEmitter>().mayBeOtherThanCompiledObject(receiver))
        return false;
    std::optional<uint32_t> selector = conventions().findMethodSelector(key, static_cast<unsigned>(call->argumentsOperands().size()));
    if (!selector)
        return false;
    const MethodFamily& family = conventions().familyOf(conventions().methodSelector(*selector));
    for (LayoutID layout : membersOf(types(), receiver)) {
        if (!std::ranges::binary_search(family.rows, layout, { }, &MethodTableRow::layout))
            return false;
    }
    Vector<ValueID, 8> operands;
    operands.append(call->thisValueOperand());
    operands.append(call->argumentsOperands());
    callThroughMethodTable(call, *selector, operands.span());
    return true;
}

void CallEmitter::callThroughMethodTable(const ValueContext& value, uint32_t placeOfSelector, std::span<const ValueID> receiverAndArguments)
{
    static_assert(AOT::shapeWordOf(1) == static_cast<uint32_t>(-1));

    const MethodSelector& selector = conventions().methodSelector(placeOfSelector);
    CodeUnit takesAndReturnsAlike = conventions().tableLookupOf(selector);
    ShapeTestEmitter& shapes = emitter<ShapeTestEmitter>();

    ArgumentLocations locations = conventions().locationsOf(m_program, takesAndReturnsAlike);
    Vector<Operand, 8> arguments = operandsOf(receiverAndArguments, locations);

    // What the callees do says nothing of what finding them reads. Whatever writes a guard of a class may write the table.
    m_out.clearDeclaredAccesses();
    ShapeTestEmitter::IndexByShape row = shapes.indexByShapeOf(arguments[0].as<CellPointer>());
    TypedPointer wordOfSelector = instanceWord(m_heaps.AOTInstance_classGuards, m_mentions.add(NumberSpace::MethodSelector, placeOfSelector));
    Word32 codeOffset(m_out.load32(m_out.baseIndex(*wordOfSelector.heap(), wordOfSelector.base(), row.index.value(), ScaleFour, wordOfSelector.offset() + row.offset)));
    Pointer entry = offsetBy(loadImmutablePointer(AOT_INSTANCE_OFFSET(image.code)), zeroExtended(codeOffset), WTF::fastLog2(static_cast<unsigned>(codeOffsetUnit)));

    CallTarget target = CallTarget::address(entry.value(), takesAndReturnsAlike);
    PlacedArguments placed = placeArguments(takesAndReturnsAlike, locations, arguments.span());
    if (value.returnCall()) {
        emitTailCall(value, target, placed);
        return;
    }
    defineResultsOfCall(value, locations.results.span(), emitCall(value, target, placed, locations.results.span(), { .callerSuspendsAtReturn = value.isAwaitedAtOnce() }));
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
