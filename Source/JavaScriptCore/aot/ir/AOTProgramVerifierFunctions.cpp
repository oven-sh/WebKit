/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTVerifierInternal.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTProgramCaches.h"
#include "AOTThreads.h"
#include "AOTTypeArena.h"
#include "AOTTypeRelations.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

constexpr OptionSet<FunctionCapabilities> capabilitiesAddedByPasses {
    FunctionCapabilities::HasOnlyDirectCallers,
    FunctionCapabilities::IsNeverCalledByHost,
    FunctionCapabilities::IsHiddenInStackTraces,
};

bool isSentByFrontEnd(FunctionRole role)
{
    return static_cast<unsigned>(role) < numberOfWireEnumerators<FunctionRole>;
}

bool isUntypedPosition(const DeclaredType& declared)
{
    return declared == DeclaredType { .type = TypeTable::anything };
}

bool createsGenerators(const Function& function)
{
    return function.has(FunctionFlags::IsGenerator) && hasSourceBodyOf(function.role);
}

bool hasValidSignature(const Function& function)
{
    return !function.results.isEmpty()
        && (function.has(FunctionFlags::HasThis) || isUntypedPosition(function.thisType))
        && (!function.has(FunctionFlags::HasRestParameter) || !function.parameters.isEmpty())
        && !function.flags.containsAll({ FunctionFlags::HasRestParameter, FunctionFlags::HasHiddenRest })
        && (!createsGenerators(function) || function.has(FunctionFlags::TakesCallee))
        && (!function.has(FunctionFlags::IsNakedConstructor) || function.kind == FunctionKind::BaseConstructor);
}

bool canCrossCalls(const Program& program, const DeclaredType& declared)
{
    std::optional<OpaqueKind> opaque = program.at(declared.type).opaque;
    return !opaque || !isFunctionLocal(*opaque);
}

bool hasOnlyTypesThatCrossCalls(const Program& program, const Function& function)
{
    auto crossesCalls = [&](const DeclaredType& declared) {
        return canCrossCalls(program, declared);
    };
    return std::ranges::all_of(function.captures, crossesCalls)
        && crossesCalls(function.thisType)
        && std::ranges::all_of(function.parameters, crossesCalls)
        && std::ranges::all_of(function.results, crossesCalls);
}

bool hasValidConstructedClass(const Program& program, FunctionID id, const Function& function)
{
    if (!function.constructedClass)
        return true;
    const Layout& constructedClass = program.at(function.constructedClass);
    if (constructedClass.kind != LayoutKind::Instance)
        return false;
    if (constructedClass.constructor == id)
        return true;
    FunctionID source = function.derivation.source;
    return source && program.at(source).constructedClass == function.constructedClass;
}

std::optional<InternalError> findHeaderError(const Program& program, IRForm form, uint64_t maxCaptures, FunctionID id, const Function& function)
{
    if (function.captures.size() > maxCaptures)
        return InternalError { .kind = InternalErrorKind::ListOutOfBounds, .function = id, .limit = LimitID::MaxCaptures, .count = id.index() };

    bool isAsRead = form == IRForm::AsRead && id.index() < program.numberOfFunctionsAsRead();
    if (hasValidSignature(function)
        && hasOnlyTypesThatCrossCalls(program, function)
        && nameOf(function.creator.kind()) == recordOf(function.role)
        && (!isAsRead || isSentByFrontEnd(function.role) || function.role == FunctionRole::EntryChecks)
        && function.capabilities.containsOnly(capabilitiesOf(function.role) | capabilitiesAddedByPasses)
        && hasValidConstructedClass(program, id, function)
        && (!function.enclosingRuntimeFunction || findRuntimeFunction(program, function.enclosingRuntimeFunction)))
        return std::nullopt;
    return InternalError { .kind = InternalErrorKind::InvalidCreatorRecord, .function = id };
}

bool hasRole(const Program& program, FunctionID id, FunctionRole role)
{
    return id && program.at(id).role == role;
}

template<typename Record>
const Record* findCreatorRecord(const Program& program, FunctionID id, FunctionRole role)
{
    return hasRole(program, id, role) ? program.at(id).creator.tryAs<Record>() : nullptr;
}

bool isNullOrObjectLayout(const Program& program, LayoutID layout)
{
    return !layout || !program.at(layout).isEnvironment();
}

bool hasGenericEntrypointHeader(const Function& function, ArgumentPassing arguments)
{
    if (function.captures.size() != 1 || function.results.size() != 1)
        return false;
    if (function.hasAny({ FunctionFlags::TakesCallee, FunctionFlags::HasArgumentCount, FunctionFlags::HasHiddenRest }))
        return false;
    switch (arguments) {
    case ArgumentPassing::Registers:
        return true;
    case ArgumentPassing::Buffer:
        return function.parameters.isEmpty() && !function.has(FunctionFlags::HasNewTarget);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool isValidEntrypointRecord(const Program& program, const Function& function, const CreatorRecord::Entrypoint& record)
{
    auto* constructEntrypoint = findCreatorRecord<CreatorRecord::Entrypoint>(program, record.constructEntrypoint, FunctionRole::GenericEntrypoint);
    return record.target
        && (function.role != FunctionRole::GenericEntrypoint || hasGenericEntrypointHeader(function, record.arguments))
        && (!record.constructEntrypoint || (constructEntrypoint && constructEntrypoint->takesNewTarget));
}

bool isValidCoroutineRecord(const Program& program, FunctionID id, const CreatorRecord::Coroutine& record)
{
    FunctionRole roleOfBody = record.worker ? FunctionRole::AsyncWorker : FunctionRole::CoroutineBody;
    if (!hasRole(program, record.ramp, FunctionRole::CoroutineRamp) || !hasRole(program, record.body, roleOfBody))
        return false;
    if (id != record.ramp && id != record.body)
        return false;
    if (record.worker && (record.worker != record.body || record.entryWithoutPromise || record.promiseParameter || record.kind != CoroutineKind::AsyncFunction))
        return false;

    unsigned numberOfBodyParameters = program.at(record.body).numberOfEntryParameters();
    return !!record.promiseType == (record.kind != CoroutineKind::Generator)
        && (!record.promiseParameter || (numberOfBodyParameters && *record.promiseParameter == numberOfBodyParameters - 1))
        && (!record.entryWithoutPromise || record.entryWithoutPromise == record.body);
}

template<typename Step>
std::optional<FunctionID> findCyclicFunctionChain(const Program& program, const Step& stepOf)
{
    enum class WalkState : uint8_t { Unvisited, OnPath, Done };

    unsigned numberOfFunctions = program.functions.size();
    Vector<WalkState> states(FillWith { }, numberOfFunctions, WalkState::Unvisited);
    for (unsigned index = 0; index < numberOfFunctions; ++index) {
        FunctionID id(index);
        for (; id && states[id.index()] == WalkState::Unvisited; id = stepOf(program.at(id)))
            states[id.index()] = WalkState::OnPath;
        if (id && states[id.index()] == WalkState::OnPath)
            return id;
        for (FunctionID walked(index); walked && states[walked.index()] == WalkState::OnPath; walked = stepOf(program.at(walked)))
            states[walked.index()] = WalkState::Done;
    }
    return std::nullopt;
}

} // anonymous namespace

void ProgramVerifier::verifyFunctionHeaders()
{
    uint64_t maxCaptures = m_build.limit(LimitID::MaxCaptures);
    Vector<uint32_t> invalidFunctions = findIndicesInParallel(m_build.parallelLoops(), m_program.functions.size(), [&](unsigned index) {
        FunctionID id(index);
        const Function& function = m_program.functions[index];
        return findHeaderError(m_program, m_form, maxCaptures, id, function) || !isValidCreatorRecord(id, function) || !isValidDerivation(id, function);
    });
    for (uint32_t index : invalidFunctions)
        verifyFunctionHeader(FunctionID(index), m_program.functions[index]);
}

void ProgramVerifier::verifyFunctionHeader(FunctionID id, const Function& function)
{
    if (auto error = findHeaderError(m_program, m_form, m_build.limit(LimitID::MaxCaptures), id, function))
        return fail(WTF::move(*error));
    if (!isValidCreatorRecord(id, function) || !isValidDerivation(id, function))
        fail({ .kind = InternalErrorKind::InvalidCreatorRecord, .function = id });
}

bool ProgramVerifier::isValidCreatorRecord(FunctionID id, const Function& function) const
{
    return WTF::switchOn(function.creator.cases,
        [](const CreatorRecord::None&) { return true; },
        [&](const CreatorRecord::Entrypoint& record) { return isValidEntrypointRecord(m_program, function, record); },
        [&](const CreatorRecord::TableLookup& record) { return !!record.key; },
        [&](const CreatorRecord::Version& record) { return record.provenVersion && record.provenVersion != id; },
        [&](const CreatorRecord::Coroutine& record) { return isValidCoroutineRecord(m_program, id, record); },
        [](const CreatorRecord::BoundFunction& record) { return record.target && record.numberOfCapturedArguments <= record.numberOfBoundArguments; },
        [&](const CreatorRecord::ResultWrapper& record) {
            auto* worker = findCreatorRecord<CreatorRecord::ResultWorker>(m_program, record.worker, FunctionRole::ResultWorker);
            return worker && worker->origin == id;
        },
        [&](const CreatorRecord::ResultWorker& record) { return record.origin && m_program.at(record.origin).numberOfEntryParameters() == function.numberOfEntryParameters(); },
        [&](const CreatorRecord::ParameterWrapper& record) { return record.body && record.parameter < function.parameters.size(); },
        [&](const CreatorRecord::EntryChecks& record) { return record.body && record.fallback; },
        [&](const CreatorRecord::Serializer& record) { return isNullOrObjectLayout(m_program, record.layout); },
        [&](const CreatorRecord::Parser& record) { return record.type && isNullOrObjectLayout(m_program, record.layout); },
        [](const CreatorRecord::ChainStep&) { return true; },
        [&](const CreatorRecord::StubBody& record) {
            const BuiltinTable& builtins = m_program.builtins();
            return builtins.contains(record.builtin) && isCallableBuiltin(builtins, record.builtin);
        });
}

bool ProgramVerifier::isValidDerivation(FunctionID id, const Function& function) const
{
    const Derivation& derivation = function.derivation;
    if (id.index() < m_program.numberOfFunctionsAsRead())
        return derivation == Derivation { };
    if (derivation.source == id)
        return false;
    if (derivation.parameterMap.isEmpty())
        return true;

    unsigned numberOfSourceParameters = derivation.source ? m_program.at(derivation.source).numberOfEntryParameters() : 0;
    return derivation.parameterMap.size() == function.numberOfEntryParameters()
        && std::ranges::all_of(derivation.parameterMap, [&](const ParameterOrigin& origin) {
            return !origin.original || *origin.original < numberOfSourceParameters;
        });
}

void ProgramVerifier::verifyDerivationChains()
{
    if (std::optional<FunctionID> cyclicFunction = findCyclicFunctionChain(m_program, [](const Function& function) { return function.derivation.source; }))
        fail({ .kind = InternalErrorKind::InvalidCreatorRecord, .function = *cyclicFunction });
}

void ProgramVerifier::verifyFunctionBodies()
{
    std::optional<TypeInterningPhase> scratch;
    if (!m_program.types.hasOpenPhase())
        scratch.emplace(m_program.types, 0u, TypeInterningPhase::Scratch);

    Vector<InternalError> errors;
    SlotsInReceivers slotsInReceivers;
    {
        ProgramCaches caches(m_program, m_build);
        errors = collectInParallel<InternalError>(m_build.parallelLoops(), m_program.functions.size(), largeFunctions(m_program, m_build.parallelLoops()).span(), [&](unsigned index, Vector<InternalError>& found) {
            FunctionID id(index);
            const Function& function = m_program.functions[index];
            if (!function.hasBody())
                return;
#if ENABLE(FUZZILLI)
            // It is the same in every build, and a fuzzer makes millions of builds.
            if (m_form == IRForm::AsRead && isLibraryCode(m_program, function))
                return;
#endif
            FunctionVerifier verifier(m_program, id, function, m_form, m_build, m_inputs);
            verifier.shareAnswers(slotsInReceivers);
            if (!verifier.run())
                found.appendVector(verifier.takeErrors());
        });
    }

    if (scratch)
        scratch->discard();
    for (InternalError& error : errors)
        fail(WTF::move(error));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
