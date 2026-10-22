/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTArgumentLocations.h"

#if ENABLE(AOT)

#include "AOTChooseCallingConventions.h"
#include "AOTConvention.h"
#include "AOTRepresentation.h"
#include <wtf/PrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

ArgumentLocation ArgumentAllocator::allocate(Representation representation)
{
    return isPassedInFPR(representation) ? allocateFPR(representation) : allocateGPR(representation);
}

ArgumentLocation ArgumentAllocator::allocateGPR(Representation representation)
{
    if (m_nextGPR >= m_registers.numberOfGPRs)
        return allocateStackWord(representation);
    RELEASE_ASSERT(m_nextGPR < numberOfDirectArgumentGPRs);
    return { .kind = ArgumentLocation::Kind::GPR, .reg = directArgumentGPR(m_nextGPR++), .representation = representation };
}

ArgumentLocation ArgumentAllocator::allocateFPR(Representation representation)
{
    if (m_nextFPR >= m_registers.numberOfFPRs)
        return allocateStackWord(representation);
    RELEASE_ASSERT(m_nextFPR < numberOfDirectArgumentFPRs);
    return { .kind = ArgumentLocation::Kind::FPR, .reg = directArgumentFPR(m_nextFPR++), .representation = representation };
}

ArgumentLocation ArgumentAllocator::allocateStackWord(Representation representation)
{
    return { .kind = ArgumentLocation::Kind::Stack, .stackIndex = m_nextStackIndex++, .representation = representation };
}

static EntryConvention genericEntryConvention(ArgumentPassing arguments)
{
    switch (arguments) {
    case ArgumentPassing::Buffer:
        return EntryConvention::GenericBuffer;
    case ArgumentPassing::Registers:
        return EntryConvention::GenericRegisters;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

EntryConvention entryConventionOf(const Function& callee)
{
    switch (callee.role) {
    case FunctionRole::GenericEntrypoint:
        return genericEntryConvention(callee.creator.as<CreatorRecord::Entrypoint>().arguments);
    case FunctionRole::Source:
    case FunctionRole::Library:
    case FunctionRole::Model:
    case FunctionRole::Signature:
    case FunctionRole::TableEntrypoint:
    case FunctionRole::TableLookup:
    case FunctionRole::GenericVersion:
    case FunctionRole::AllocatedThisVersion:
    case FunctionRole::ConstructBody:
    case FunctionRole::CallSiteVersion:
    case FunctionRole::CalleeVersion:
    case FunctionRole::CoroutineRamp:
    case FunctionRole::CoroutineBody:
    case FunctionRole::AsyncWorker:
    case FunctionRole::BoundFunction:
    case FunctionRole::ResultWrapper:
    case FunctionRole::ResultWorker:
    case FunctionRole::ParameterWrapper:
    case FunctionRole::EntryChecks:
    case FunctionRole::OutlinedBody:
    case FunctionRole::Serializer:
    case FunctionRole::Parser:
    case FunctionRole::ValidatingParser:
    case FunctionRole::Validator:
    case FunctionRole::ChainStep:
    case FunctionRole::StubBody:
        return EntryConvention::Direct;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ConventionRegisters maxConventionRegisters()
{
    return { numberOfDirectArgumentGPRs, numberOfDirectArgumentFPRs };
}

Representation declaredRepresentationOf(const Program& program, const DeclaredType& declared)
{
    const Type& type = program.at(declared.type);
    if (declared.flags.contains(PositionFlags::IsExpected) && type.isJavaScriptValue())
        return Representation::JSValue;
    Representation representation = defaultRepresentationOf(type);
    if (representation != Representation::Void)
        return representation;

    // A position whose type has one value still holds a word: a closure cell stores every capture and an entry reads every parameter.
    RELEASE_ASSERT(type.isJavaScriptValue());
    return Representation::JSValue;
}

static Representation directParameterRepresentation(const Program& program, const Function& callee, unsigned index)
{
    if (const DeclaredType* declared = callee.declaredEntryParameter(index))
        return declaredRepresentationOf(program, *declared);

    switch (callee.entryParameterKind(index)) {
    case EntryParameter::Callee:
    case EntryParameter::HiddenRest:
        return Representation::Cell;
    case EntryParameter::NewTarget:
        return Representation::JSValue;
    case EntryParameter::ArgumentCount:
        return Representation::Int64;
    case EntryParameter::Capture:
    case EntryParameter::This:
    case EntryParameter::Parameter:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void computeDirectLocations(const Program& program, const Function& callee, ConventionRegisters registers, ArgumentLocations& locations)
{
    ArgumentAllocator allocator(registers);
    unsigned count = callee.numberOfEntryParameters();
    locations.parameters.reserveInitialCapacity(count);
    for (unsigned index = 0; index < count; ++index)
        locations.parameters.append(allocator.allocate(directParameterRepresentation(program, callee, index)));
    locations.numberOfStackArguments = allocator.numberOfStackWords();
}

static Representation genericParameterRepresentation(EntryParameter kind)
{
    switch (kind) {
    case EntryParameter::Capture:
        return Representation::Cell;
    case EntryParameter::This:
    case EntryParameter::NewTarget:
    case EntryParameter::Parameter:
        return Representation::JSValue;
    case EntryParameter::Callee:
    case EntryParameter::ArgumentCount:
    case EntryParameter::HiddenRest:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ArgumentLocation genericArgumentLocation(unsigned argumentIndex, Representation representation)
{
    if (argumentIndex < numberOfArgumentGPRs)
        return { .kind = ArgumentLocation::Kind::GPR, .reg = argumentGPR(argumentIndex), .representation = representation };
    return { .kind = ArgumentLocation::Kind::Stack, .stackIndex = argumentIndex - numberOfArgumentGPRs, .representation = representation };
}

static ArgumentLocation allocateGenericLocation(EntryParameter kind, unsigned& nextArgument)
{
    Representation representation = genericParameterRepresentation(kind);
    switch (kind) {
    case EntryParameter::Capture:
        return { .kind = ArgumentLocation::Kind::GPR, .reg = calleeGPR, .representation = representation };
    case EntryParameter::This:
        return { .kind = ArgumentLocation::Kind::GPR, .reg = thisGPR, .representation = representation };
    case EntryParameter::NewTarget:
    case EntryParameter::Parameter:
        return genericArgumentLocation(nextArgument++, representation);
    case EntryParameter::Callee:
    case EntryParameter::ArgumentCount:
    case EntryParameter::HiddenRest:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void computeGenericLocations(const Function& callee, ArgumentLocations& locations)
{
    RELEASE_ASSERT(callee.captures.size() == 1);
    unsigned nextArgument = 0;
    unsigned count = callee.numberOfEntryParameters();
    locations.parameters.reserveInitialCapacity(count);
    for (unsigned index = 0; index < count; ++index)
        locations.parameters.append(allocateGenericLocation(callee.entryParameterKind(index), nextArgument));
    locations.numberOfStackArguments = nextArgument > numberOfArgumentGPRs ? nextArgument - numberOfArgumentGPRs : 0;
}

static ArgumentLocation computeSingleResultLocation(const Program& program, const Function& callee, FunctionConvention convention, EntryConvention entryConvention)
{
    if (convention.resultIsBoxed || entryConvention != EntryConvention::Direct)
        return { .kind = ArgumentLocation::Kind::GPR, .reg = GPRInfo::returnValueGPR, .representation = Representation::JSValue };

    const DeclaredType& declared = callee.result();
    const Type& type = program.at(declared.type);
    bool isProven = !declared.flags.contains(PositionFlags::IsExpected);
    if (isProven && (type.isNever() || type.hasOnlyPrimitives(Primitive::Undefined)))
        return { .kind = ArgumentLocation::Kind::None, .representation = Representation::JSValue };

    Representation representation = declaredRepresentationOf(program, declared);
    if (isPassedInFPR(representation))
        return { .kind = ArgumentLocation::Kind::FPR, .reg = FPRInfo::returnValueFPR, .representation = representation };
    return { .kind = ArgumentLocation::Kind::GPR, .reg = GPRInfo::returnValueGPR, .representation = representation };
}

static void computeResultLocations(const Program& program, const Function& callee, FunctionConvention convention, ConventionRegisters registers, ArgumentLocations& locations)
{
    if (callee.results.size() == 1) {
        locations.results.append(computeSingleResultLocation(program, callee, convention, locations.convention));
        return;
    }

    RELEASE_ASSERT(locations.convention == EntryConvention::Direct);
    ArgumentAllocator allocator(registers);
    locations.results.reserveInitialCapacity(callee.results.size());
    for (const DeclaredType& result : callee.results)
        locations.results.append(allocator.allocate(declaredRepresentationOf(program, result)));
    locations.numberOfStackResults = allocator.numberOfStackWords();
}

ArgumentLocations computeArgumentLocations(const Program& program, const Function& callee, FunctionConvention convention, ConventionRegisters registers)
{
    ArgumentLocations locations;
    locations.convention = entryConventionOf(callee);
    switch (locations.convention) {
    case EntryConvention::Direct:
        computeDirectLocations(program, callee, registers, locations);
        break;
    case EntryConvention::GenericRegisters:
    case EntryConvention::GenericBuffer:
        computeGenericLocations(callee, locations);
        break;
    }
    computeResultLocations(program, callee, convention, registers, locations);
    return locations;
}

Representation parameterRepresentationOf(const Program& program, const Function& callee, unsigned entryParameterIndex)
{
    switch (entryConventionOf(callee)) {
    case EntryConvention::Direct:
        return directParameterRepresentation(program, callee, entryParameterIndex);
    case EntryConvention::GenericRegisters:
    case EntryConvention::GenericBuffer:
        return genericParameterRepresentation(callee.entryParameterKind(entryParameterIndex));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Representation resultRepresentationOf(const Program& program, const Function& callee, FunctionConvention convention, unsigned resultIndex)
{
    if (callee.results.size() != 1)
        return declaredRepresentationOf(program, callee.results[resultIndex]);
    RELEASE_ASSERT(!resultIndex);
    return computeSingleResultLocation(program, callee, convention, entryConventionOf(callee)).representation;
}

void dump(PrintStream& out, const ArgumentLocation& location)
{
    switch (location.kind) {
    case ArgumentLocation::Kind::None:
        out.print("none"_s);
        break;
    case ArgumentLocation::Kind::GPR:
    case ArgumentLocation::Kind::FPR:
        out.print(location.reg.debugName());
        break;
    case ArgumentLocation::Kind::Stack:
        out.print("stack"_s, location.stackIndex);
        break;
    }
    out.print(":"_s, nameOf(location.representation));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
