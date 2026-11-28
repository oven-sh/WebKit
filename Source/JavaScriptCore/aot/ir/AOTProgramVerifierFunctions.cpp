/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTProgramVerifier.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>
#include <wtf/StdLibExtras.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

template<typename Enumeration>
std::optional<InternalErrorKind> findErrorInEnumeratorOfFunctionRecord(Enumeration enumerator)
{
    static_assert(numberOfEnumerators<Enumeration> && !isFlagWord<Enumeration>);
    auto number = static_cast<unsigned>(enumerator);
    if (number >= numberOfEnumerators<Enumeration>)
        return InternalErrorKind::UnknownEnumerator;
    if (number >= numberOfWireEnumerators<Enumeration>)
        return InternalErrorKind::EngineOnlyOnWire;
    return std::nullopt;
}

template<typename Flag>
std::optional<InternalErrorKind> findErrorInFlagsOfFunctionRecord(OptionSet<Flag> flags)
{
    static_assert(isFlagWord<Flag> && numberOfEnumerators<Flag> <= 32);
    constexpr uint64_t declaredFlags = (uint64_t { 1 } << numberOfEnumerators<Flag>) - 1;
    constexpr uint64_t flagsOfWire = (uint64_t { 1 } << numberOfWireEnumerators<Flag>) - 1;
    uint64_t word = flags.toRaw();
    if (word & ~declaredFlags)
        return InternalErrorKind::UnknownFlag;
    if (word & ~flagsOfWire)
        return InternalErrorKind::EngineOnlyOnWire;
    return std::nullopt;
}

std::optional<InternalErrorKind> findErrorInWordsOfFunctionRecord(const Function& function)
{
    if (auto kind = findErrorInEnumeratorOfFunctionRecord(function.role))
        return kind;
    if (auto kind = findErrorInEnumeratorOfFunctionRecord(function.kind))
        return kind;
    if (auto kind = findErrorInFlagsOfFunctionRecord(function.flags))
        return kind;
    return findErrorInFlagsOfFunctionRecord(function.syntax);
}

bool isUntypedPositionOfFunctionRecord(const DeclaredType& declared)
{
    return declared.written() == TypeTable::anything && !declared.site && declared.writtenFlags().isEmpty();
}

bool hasOnlyTypesThatCrossCalls(const Program& program, const Function& function)
{
    auto crossesCalls = [&](const DeclaredType& declared) {
        std::optional<OpaqueKind> opaque = program.at(declared.written()).opaque;
        return !opaque || !isFunctionLocal(*opaque);
    };
    return std::ranges::all_of(function.captures, crossesCalls)
        && crossesCalls(function.thisType)
        && std::ranges::all_of(function.parameters, crossesCalls)
        && std::ranges::all_of(function.results, crossesCalls);
}

ASCIILiteral findBrokenRuleOfFunctionRecord(const Program& program, FunctionID id, const Function& function)
{
    if (!function.capabilities.containsOnly(capabilitiesOf(function.role)))
        return "it has a capability that its role does not give"_s;
    if (function.hasBody() != hasSourceBodyOf(function.role))
        return function.hasBody() ? "its role says that it has no body, and it has one"_s : "its role says that it has a body, and it has none"_s;

    if (function.results.isEmpty())
        return "it has no result"_s;
    if (!function.has(FunctionFlags::HasThis) && !isUntypedPositionOfFunctionRecord(function.thisType))
        return "it takes no `this`, and a type is written for `this`"_s;
    if (function.has(FunctionFlags::HasRestParameter) && function.parameters.isEmpty())
        return "it has a rest parameter, and its list of parameters is empty"_s;
    if (function.flags.containsAll({ FunctionFlags::HasRestParameter, FunctionFlags::HasHiddenRest }))
        return "it has a rest parameter and a hidden one"_s;
    if (function.has(FunctionFlags::IsGenerator) && hasSourceBodyOf(function.role) && !function.has(FunctionFlags::TakesCallee))
        return "it creates generators and does not take its callee"_s;
    if (function.has(FunctionFlags::IsNakedConstructor) && function.kind != FunctionKind::BaseConstructor)
        return "it is a naked constructor and not the constructor of a base class"_s;
    if (!hasOnlyTypesThatCrossCalls(program, function))
        return "its signature names a type whose values cannot leave the function that makes them"_s;

    if (function.constructedClass) {
        const Layout& constructedClass = program.at(function.constructedClass);
        if (constructedClass.kind != LayoutKind::Instance || constructedClass.constructor != id)
            return "the layout that it is said to construct does not name it as its constructor"_s;
    }
    if (function.enclosingRuntimeFunction && !findRuntimeFunction(program, function.enclosingRuntimeFunction))
        return "the program does not list the library function that encloses it"_s;
    return { };
}

std::optional<InternalError> findErrorInFunctionRecord(const Program& program, uint64_t maxCaptures, FunctionID id, const Function& function)
{
    if (auto kind = findErrorInWordsOfFunctionRecord(function))
        return InternalError { .kind = *kind, .function = id, .count = id.index() };
    if (function.captures.size() > maxCaptures)
        return InternalError { .kind = InternalErrorKind::ListOutOfBounds, .function = id, .limit = LimitID::MaxCaptures, .count = id.index() };
    if (ASCIILiteral brokenRule = findBrokenRuleOfFunctionRecord(program, id, function); !brokenRule.isNull())
        return InternalError { .kind = InternalErrorKind::InvalidCreatorRecord, .function = id, .detail = brokenRule };
    return std::nullopt;
}

} // anonymous namespace

void ProgramVerifier::verifyFunctionHeaders()
{
    uint64_t maxCaptures = m_build.limit(LimitID::MaxCaptures);
    Vector<uint32_t> invalidFunctions = findIndicesInParallel(m_build.parallelLoops(), m_program.functions.size(), [&](unsigned index) {
        return findErrorInFunctionRecord(m_program, maxCaptures, FunctionID(index), m_program.functions[index]).has_value();
    });
    for (uint32_t index : invalidFunctions)
        verifyFunctionHeader(FunctionID(index), m_program.functions[index]);
}

void ProgramVerifier::verifyFunctionHeader(FunctionID id, const Function& function)
{
    if (auto error = findErrorInFunctionRecord(m_program, m_build.limit(LimitID::MaxCaptures), id, function))
        fail(WTF::move(*error));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
