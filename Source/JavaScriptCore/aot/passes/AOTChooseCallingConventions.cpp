/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTChooseCallingConventions.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTCallGraph.h"
#include "AOTConvention.h"
#include "AOTDerivedFunctions.h"
#include "AOTDiagnostics.h"
#include "AOTEffectSummaries.h"
#include "AOTEffects.h"
#include "AOTElideStackChecks.h"
#include "AOTFunctionEditing.h"
#include "AOTIRBuilder.h"
#include "AOTPassRunner.h"
#include "AOTPhaseTimes.h"
#include "AOTRelocation.h"
#include "AOTSchemaWalkers.h"
#include "AOTStubBodyConvention.h"
#include "AOTThreads.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <algorithm>
#include <compare>
#include <limits>
#include <numeric>
#include <ranges>
#include <utility>
#include <wtf/Atomics.h>
#include <wtf/BitVector.h>
#include <wtf/CommaPrinter.h>
#include <wtf/EnumTraits.h>
#include <wtf/PrintStream.h>
#include <wtf/UnionFind.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(sizeof(FunctionConvention) == 4);
static_assert(defaultLimitValue(LimitID::MaxReservedStackArguments, hostArchitecture()) <= std::numeric_limits<decltype(FunctionConvention::reservedStackWords)>::max());
static_assert(StubBodyConvention::maxNumberOfOperandGPRs <= numberOfDirectArgumentGPRs);
static_assert(StubBodyConvention::maxNumberOfOperandFPRs <= numberOfDirectArgumentFPRs);
static_assert([] {
    for (unsigned index = 0; index < StubBodyConvention::maxNumberOfOperandGPRs; ++index) {
        if (StubBodyConvention::operandGPR(index) != directArgumentGPR(index))
            return false;
    }
    for (unsigned index = 0; index < StubBodyConvention::maxNumberOfOperandFPRs; ++index) {
        if (StubBodyConvention::operandFPR(index) != directArgumentFPR(index))
            return false;
    }
    return true;
}());
static_assert(StubBodyConvention::resultGPR(0) == GPRInfo::returnValueGPR);
static_assert(StubBodyConvention::resultFPR() == FPRInfo::returnValueFPR);

namespace {

struct ArgumentAllocator {
    ArgumentLocation allocate(Representation representation)
    {
        if (isPassedInFPR(representation)) {
            if (nextFPR < registers.numberOfFPRs) {
                RELEASE_ASSERT(nextFPR < numberOfDirectArgumentFPRs);
                return { .kind = ArgumentLocation::Kind::FPR, .reg = directArgumentFPR(nextFPR++), .representation = representation };
            }
        } else if (nextGPR < registers.numberOfGPRs) {
            RELEASE_ASSERT(nextGPR < numberOfDirectArgumentGPRs);
            return { .kind = ArgumentLocation::Kind::GPR, .reg = directArgumentGPR(nextGPR++), .representation = representation };
        }
        return { .kind = ArgumentLocation::Kind::Stack, .stackIndex = numberOfStackWords++, .representation = representation };
    }

    ConventionRegisters registers;
    unsigned nextGPR { 0 };
    unsigned nextFPR { 0 };
    uint32_t numberOfStackWords { 0 };
};

enum class TailCalleeKind : uint8_t { Unknown, Direct };

struct TailCallSite {
    FunctionID caller;
    BlockID block;
    TailCalleeKind calleeKind;
    FunctionID callee;
    TailCallRequirement requirement;
    bool hasCheckedCast;
};

struct TailCallEdge {
    friend auto operator<=>(const TailCallEdge&, const TailCallEdge&) = default;

    FunctionID callee;
    FunctionID caller;
};

struct EntryRequest {
    friend auto operator<=>(const EntryRequest&, const EntryRequest&) = default;

    FunctionID callee;
    uint8_t firstOmittedGPR;
};

struct ConventionResultNode : WTF::UnionFind<ConventionResultNode> { };

enum class CalledCodeKind : uint8_t { Unknown, None, FunctionWithStackCheck };

} // anonymous namespace

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
    case FunctionRole::ChainStep:
    case FunctionRole::StubBody:
        return EntryConvention::Direct;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Representation declaredRepresentationOf(const Program& program, const DeclaredType& declared)
{
    const Type& type = program.at(declared.written());
    if (declared.isExpected() && type.isJavaScriptValue())
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

static ArgumentLocation genericParameterLocation(EntryParameter kind, unsigned& nextArgument)
{
    switch (kind) {
    case EntryParameter::Capture:
        return { .kind = ArgumentLocation::Kind::GPR, .reg = calleeGPR, .representation = Representation::Cell };
    case EntryParameter::This:
        return { .kind = ArgumentLocation::Kind::GPR, .reg = thisGPR, .representation = Representation::JSValue };
    case EntryParameter::NewTarget:
    case EntryParameter::Parameter: {
        unsigned argument = nextArgument++;
        if (argument < numberOfArgumentGPRs)
            return { .kind = ArgumentLocation::Kind::GPR, .reg = argumentGPR(argument), .representation = Representation::JSValue };
        return { .kind = ArgumentLocation::Kind::Stack, .stackIndex = argument - numberOfArgumentGPRs, .representation = Representation::JSValue };
    }
    case EntryParameter::Callee:
    case EntryParameter::ArgumentCount:
    case EntryParameter::HiddenRest:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ArgumentLocation singleResultLocation(const Program& program, const Function& callee, FunctionConvention convention, EntryConvention entryConvention)
{
    if (convention.resultIsBoxed || entryConvention != EntryConvention::Direct)
        return { .kind = ArgumentLocation::Kind::GPR, .reg = GPRInfo::returnValueGPR, .representation = Representation::JSValue };

    const DeclaredType& declared = callee.result();
    const Type& type = program.at(declared.written());
    bool isProven = !declared.isExpected();
    if (isProven && (type.isNever() || type.hasOnlyPrimitives(Primitive::Undefined)))
        return { .kind = ArgumentLocation::Kind::None, .representation = Representation::JSValue };

    Representation representation = declaredRepresentationOf(program, declared);
    if (isPassedInFPR(representation))
        return { .kind = ArgumentLocation::Kind::FPR, .reg = FPRInfo::returnValueFPR, .representation = representation };
    return { .kind = ArgumentLocation::Kind::GPR, .reg = GPRInfo::returnValueGPR, .representation = representation };
}

static ArgumentLocations computeArgumentLocations(const Program& program, const Function& callee, FunctionConvention convention, ConventionRegisters registers)
{
    ArgumentLocations locations;
    locations.convention = entryConventionOf(callee);
    unsigned count = callee.numberOfEntryParameters();
    locations.parameters.reserveInitialCapacity(count);
    switch (locations.convention) {
    case EntryConvention::Direct: {
        ArgumentAllocator allocator { registers };
        for (unsigned index = 0; index < count; ++index)
            locations.parameters.append(allocator.allocate(directParameterRepresentation(program, callee, index)));
        locations.numberOfStackArguments = allocator.numberOfStackWords;
        break;
    }
    case EntryConvention::GenericRegisters:
    case EntryConvention::GenericBuffer: {
        RELEASE_ASSERT(callee.captures.size() == 1);
        unsigned nextArgument = 0;
        for (unsigned index = 0; index < count; ++index)
            locations.parameters.append(genericParameterLocation(callee.entryParameterKind(index), nextArgument));
        locations.numberOfStackArguments = nextArgument > numberOfArgumentGPRs ? nextArgument - numberOfArgumentGPRs : 0;
        break;
    }
    }

    if (callee.results.size() == 1) {
        locations.results.append(singleResultLocation(program, callee, convention, locations.convention));
        return locations;
    }
    RELEASE_ASSERT(locations.convention == EntryConvention::Direct);
    ArgumentAllocator allocator { registers };
    locations.results.reserveInitialCapacity(callee.results.size());
    for (const DeclaredType& result : callee.results)
        locations.results.append(allocator.allocate(declaredRepresentationOf(program, result)));
    locations.numberOfStackResults = allocator.numberOfStackWords;
    return locations;
}

CallingConventions::CallingConventions()
{
    m_tables.registers = { numberOfDirectArgumentGPRs, numberOfDirectArgumentFPRs };
}

CallingConventions::CallingConventions(CallingConventionTables&& tables)
    : m_tables(WTF::move(tables))
{
}

FunctionConvention CallingConventions::of(const Program& program, FunctionID id) const
{
    const Function& function = program.at(id);
    FunctionConvention convention = id.index() < m_tables.conventions.size() ? m_tables.conventions[id.index()] : FunctionConvention();
    bool didChooseConventions = !m_tables.conventions.isEmpty();
    if (!didChooseConventions && function.results.size() == 1 && function.hasOrdinaryFrame())
        convention.resultIsBoxed = true;
    convention.spillsArgumentRegisters = entryConventionOf(function) == EntryConvention::GenericBuffer;
    return convention;
}

Representation CallingConventions::parameterRepresentation(const Program& program, FunctionID id, unsigned entryParameterIndex) const
{
    const Function& callee = program.at(id);
    switch (entryConventionOf(callee)) {
    case EntryConvention::Direct:
        return directParameterRepresentation(program, callee, entryParameterIndex);
    case EntryConvention::GenericRegisters:
    case EntryConvention::GenericBuffer: {
        unsigned nextArgument = 0;
        return genericParameterLocation(callee.entryParameterKind(entryParameterIndex), nextArgument).representation;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Representation CallingConventions::resultRepresentation(const Program& program, FunctionID id, unsigned resultIndex) const
{
    const Function& callee = program.at(id);
    if (callee.results.size() != 1)
        return declaredRepresentationOf(program, callee.results[resultIndex]);
    RELEASE_ASSERT(!resultIndex);
    return singleResultLocation(program, callee, of(program, id), entryConventionOf(callee)).representation;
}

ArgumentLocations CallingConventions::locationsOf(const Program& program, FunctionID id) const
{
    return computeArgumentLocations(program, program.at(id), of(program, id), m_tables.registers);
}

uint32_t CallingConventions::stackWordsReservedByCallersOf(const Program& program, FunctionID callee) const
{
    return stackWordsReservedByCallersOf(program, callee, locationsOf(program, callee));
}

uint32_t CallingConventions::stackWordsReservedByCallersOf(const Program& program, FunctionID callee, const ArgumentLocations& calleeLocations) const
{
    return std::max<uint32_t>(calleeLocations.numberOfStackWords(), of(program, callee).reservedStackWords);
}

std::span<const SecondaryEntry> CallingConventions::secondaryEntriesOf(FunctionID id) const
{
    size_t index = id.index();
    if (index + 1 >= m_tables.firstSecondaryEntry.size())
        return { };
    uint32_t first = m_tables.firstSecondaryEntry[index];
    return m_tables.secondaryEntries.subspan(first, m_tables.firstSecondaryEntry[index + 1] - first);
}

SecondaryEntry CallingConventions::findEntryOmittingArguments(FunctionID callee, unsigned firstUndefinedGPR, unsigned numberOfGPRArguments) const
{
    RELEASE_ASSERT(firstUndefinedGPR <= numberOfGPRArguments && numberOfGPRArguments <= m_tables.registers.numberOfGPRs);
    for (SecondaryEntry entry : secondaryEntriesOf(callee)) {
        if (entry.firstOmittedGPR >= firstUndefinedGPR)
            return entry;
    }
    return { primaryEntry, static_cast<uint8_t>(numberOfGPRArguments) };
}

void CallingConventions::dump(PrintStream& out, const Program& program) const
{
    auto dumpLocations = [&](std::span<const ArgumentLocation> locations) {
        CommaPrinter comma;
        out.print("(");
        for (const ArgumentLocation& location : locations) {
            out.print(comma);
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
        out.print(")");
    };

    for (unsigned index = 0; index < program.functions.size(); ++index) {
        if (!program.functions[index].hasBody())
            continue;

        FunctionID id(index);
        FunctionConvention convention = of(program, id);
        ArgumentLocations locations = locationsOf(program, id);
        out.print("f", index, " ", enumName(locations.convention), " ");
        dumpLocations(locations.parameters.span());
        out.print(" -> ");
        dumpLocations(locations.results.span());
        if (convention.resultIsBoxed)
            out.print(" boxed-result");
        if (convention.spillsArgumentRegisters)
            out.print(" spills");
        if (convention.reservedStackWords)
            out.print(" reserved=", convention.reservedStackWords);
        if (std::span<const SecondaryEntry> entries = secondaryEntriesOf(id); !entries.empty()) {
            CommaPrinter comma;
            out.print(" entries=[");
            for (SecondaryEntry entry : entries)
                out.print(comma, entry.id, "@", entry.firstOmittedGPR);
            out.print("]");
        }
        out.println();
    }
}

static void collectTailCallSitesOf(const Program& program, FunctionID caller, Vector<TailCallSite>& sites)
{
    const Function& function = program.at(caller);
    forEachBlockEndingWith(program, caller, { TerminatorKind::ReturnCall }, [&](BlockID block) {
        const Terminator& terminator = function.at(block).terminator;
        const Value& call = function.at(terminator.callOperand());
        bool hasCheckedCast = std::ranges::any_of(terminator.casts(), [](const TailCast& cast) {
            switch (cast.resolution) {
            case CastResolution::Unresolved:
            case CastResolution::Check:
                return true;
            case CastResolution::Nothing:
            case CastResolution::Upcast:
            case CastResolution::Assume:
                return false;
            }
            RELEASE_ASSERT_NOT_REACHED();
        });
        bool isDirect = call.opcode == Opcode::CallDirect;
        sites.append({ caller, block, isDirect ? TailCalleeKind::Direct : TailCalleeKind::Unknown, isDirect ? call.function() : FunctionID(), terminator.requirement(), hasCheckedCast });
    });
}

static std::span<const TailCallSite> sitesOf(std::span<const TailCallSite> sites, FunctionID caller)
{
    auto found = std::ranges::equal_range(sites, caller, { }, &TailCallSite::caller);
    return sites.subspan(found.begin() - sites.begin(), found.size());
}

static bool canBoxResult(const Function& function)
{
    return function.results.size() == 1 && function.hasOrdinaryFrame() && entryConventionOf(function) == EntryConvention::Direct;
}

CallingConventions chooseCallingConventions(ProgramEditor& editor, const OptimizationInputs& inputs, const PassRunner& runner)
{
    RELEASE_ASSERT(inputs.derivedFunctions);
    const Program& program = editor.program();
    BuildContext& build = runner.build();
    const ParallelLoops& loops = build.parallelLoops();
    unsigned count = program.functions.size();

    CallingConventionTables tables { };
    tables.registers = { static_cast<unsigned>(std::min<uint64_t>(build.limit(LimitID::MaxArgumentRegisters), numberOfDirectArgumentGPRs)), numberOfDirectArgumentFPRs };
    tables.conventions.grow(count);
    Vector<TailCallSite> sites = collectInParallel<TailCallSite>(loops, count, [&](unsigned index, Vector<TailCallSite>& found) {
        collectTailCallSitesOf(program, FunctionID(index), found);
    });
    // Where each function with one result returns it, by the conventions chosen so far.
    Vector<ArgumentLocation> results;
    Vector<FunctionID> editedCallers;
    uint64_t numberOfBoxedResults = 0;
    auto isEnabledFor = [&](FunctionID id) { return build.isEnabled(PassID::ChooseCallingConventions, findFunctionAsRead(&program, id)); };
    auto locationsOf = [&](FunctionID id) { return computeArgumentLocations(program, program.at(id), tables.conventions[id.index()], tables.registers); };
    auto returnsNothing = [&](FunctionID id) { return results[id.index()].kind == ArgumentLocation::Kind::None; };
    auto resultOf = [&](FunctionID id) { return results[id.index()].representation; };

    BitVector isCalledAndReturnedTo(count);
    BitVector isTailCalled(count);
    Vector<uint8_t> returnedTo(FillWith { }, count, static_cast<uint8_t>(0));
    Vector<uint8_t> tailCalled(FillWith { }, count, static_cast<uint8_t>(0));
    forEachIndexInParallel(loops, count, [&](unsigned index) {
        const Function& caller = program.functions[index];
        Vector<std::pair<BlockID, ValueID>, 8> tailCalls;
        forEachBlockEndingWith(program, FunctionID(index), { TerminatorKind::ReturnCall }, [&](BlockID block) {
            tailCalls.append({ block, caller.at(block).terminator.callOperand() });
        });
        forEachDirectCall(program, FunctionID(index), [&](BlockID block, ValueID id, FunctionID callee) {
            uint8_t& isCalled = (tailCalls.contains(std::pair { block, id }) ? tailCalled : returnedTo)[callee.index()];
            if (!WTF::atomicLoad(&isCalled, std::memory_order_relaxed))
                WTF::atomicStore(&isCalled, static_cast<uint8_t>(1), std::memory_order_relaxed);
        });
    });
    for (unsigned index = 0; index < count; ++index) {
        if (returnedTo[index])
            isCalledAndReturnedTo.quickSet(index);
        if (tailCalled[index])
            isTailCalled.quickSet(index);
    }

    for (const RuntimeFunctionEntry& entry : program.runtimeFunctions) {
        const RuntimeFunctionData& data = program.builtins().at(entry.id);
        FunctionID id = entry.function;
        if (!id || !data.flags.contains(RuntimeFunctionFlag::UsesStubConvention) || data.implements.empty())
            continue;
        const Function& function = program.at(id);
        if (!function.hasBody() || function.role != FunctionRole::Library || !isEnabledFor(id))
            continue;

        // Nothing but direct calls that are returned to enters it, and it makes no tail call.
        if (inputs.callGraph.entrySet(id) != EntrySet::DirectCallers
            || inputs.derivedFunctions->find(derivedFunctionKey(FunctionRole::GenericEntrypoint, id))
            || !isCalledAndReturnedTo.get(id.index()) || isTailCalled.get(id.index())
            || std::ranges::binary_search(sites, id, { }, &TailCallSite::caller))
            continue;

        if (function.results.size() != 1)
            continue;
        ArgumentLocations locations = locationsOf(id);
        if (locations.numberOfStackWords())
            continue;
        unsigned numberOfGPRs = 0;
        unsigned numberOfFPRs = 0;
        for (const ArgumentLocation& location : locations.parameters) {
            switch (location.kind) {
            case ArgumentLocation::Kind::GPR:
                ++numberOfGPRs;
                break;
            case ArgumentLocation::Kind::FPR:
                ++numberOfFPRs;
                break;
            case ArgumentLocation::Kind::None:
            case ArgumentLocation::Kind::Stack:
                break;
            }
        }
        if (numberOfGPRs > StubBodyConvention::maxNumberOfOperandGPRs || numberOfFPRs > StubBodyConvention::maxNumberOfOperandFPRs)
            continue;

        Function& edited = editor.at(id);
        edited.role = FunctionRole::StubBody;
        edited.creator = CreatorRecord::StubBody { .builtin = data.implements[0] };
        edited.capabilities = capabilitiesOf(FunctionRole::StubBody);
    }

    auto box = [&](FunctionID id) {
        if (std::exchange(tables.conventions[id.index()].resultIsBoxed, true))
            return;
        results[id.index()] = locationsOf(id).results[0];
        ++numberOfBoxedResults;
    };

    auto boxMixedClasses = [&] {
        unsigned callByValue = count;
        Vector<ConventionResultNode> nodes(count + 1);
        for (const TailCallSite& site : sites) {
            if (site.requirement == TailCallRequirement::Required)
                nodes[site.caller.index()].unify(&nodes[site.calleeKind == TailCalleeKind::Unknown ? callByValue : site.callee.index()]);
        }

        Vector<uint32_t> roots(count + 1);
        for (unsigned index = 0; index <= count; ++index)
            roots[index] = static_cast<uint32_t>(nodes[index].find() - nodes.span().data());

        BitVector isMixed(count + 1);
        isMixed.quickSet(roots[callByValue]);
        for (unsigned index = 0; index < count; ++index) {
            FunctionID id(index);
            FunctionID root(roots[index]);
            if (root != id && root.index() != callByValue && (resultOf(id) != resultOf(root) || returnsNothing(id) != returnsNothing(root)))
                isMixed.quickSet(root.index());
        }
        for (unsigned index = 0; index < count; ++index) {
            FunctionID id(index);
            if (!isMixed.quickGet(roots[index]))
                continue;
            if (canBoxResult(program.at(id)))
                box(id);
            else if (resultOf(id) != Representation::JSValue)
                build.diagnostics().add(InternalError { .kind = InternalErrorKind::RequiredTailCallNotConverted, .function = id, .pass = PassID::ChooseCallingConventions });
        }
    };

    auto boxTailCallees = [&] {
        // boxMixedClasses() left the caller and the callee of a required tail call with equal results, so the caller is never boxed alone.
        Vector<uint32_t> boxable = findIndicesInParallel(loops, count, [&](unsigned index) {
            FunctionID id(index);
            const Function& function = program.at(id);
            return isTailCalled.get(index)
                && !tables.conventions[index].resultIsBoxed
                && canBoxResult(function)
                && (!isCalledAndReturnedTo.get(index) || returnsNothing(id))
                && !function.mayBeCalledByHost()
                && std::ranges::none_of(sitesOf(sites.span(), id), [](const TailCallSite& site) { return site.requirement == TailCallRequirement::Required; })
                && isEnabledFor(id);
        });
        BitVector callees(count);
        Vector<FunctionID> worklist;
        for (uint32_t index : boxable) {
            callees.quickSet(index);
            worklist.append(FunctionID(index));
        }
        Vector<TailCallEdge> edges;
        for (const TailCallSite& site : sites) {
            if (site.callee && callees.get(site.callee.index()))
                edges.append({ site.callee, site.caller });
        }
        sortAndRemoveDuplicates(loops, edges);

        while (!worklist.isEmpty()) {
            FunctionID callee = worklist.takeLast();
            if (tables.conventions[callee.index()].resultIsBoxed)
                continue;
            bool hasOnlyBoxedCallers = std::ranges::all_of(std::ranges::equal_range(edges, callee, { }, &TailCallEdge::callee), [&](const TailCallEdge& edge) {
                return resultOf(edge.caller) == Representation::JSValue;
            });
            if (!hasOnlyBoxedCallers)
                continue;
            box(callee);
            for (const TailCallSite& site : sitesOf(sites.span(), callee)) {
                if (site.callee && callees.get(site.callee.index()))
                    worklist.append(site.callee);
            }
        }
    };

    results.grow(count);
    forEachIndexInParallel(loops, count, [&](unsigned index) {
        FunctionID id(index);
        if (!isEnabledFor(id) && canBoxResult(program.functions[index]))
            tables.conventions[index].resultIsBoxed = true;
        if (recordFactsOf(program, id).numberOfResults == 1)
            results[index] = locationsOf(id).results[0];
    });
    boxMixedClasses();
    boxTailCallees();
    build.statistics().count(StatisticID::ResultsBoxedForTailCalls, numberOfBoxedResults);

    auto canRemainTailCall = [&](const TailCallSite& site) {
        if (site.hasCheckedCast)
            return false;
        if (returnsNothing(site.caller))
            return true;

        Representation expected = resultOf(site.caller);
        switch (site.calleeKind) {
        case TailCalleeKind::Unknown:
            return haveSameRegisterRepresentation(Representation::JSValue, expected);
        case TailCalleeKind::Direct:
            return !returnsNothing(site.callee) && haveSameRegisterRepresentation(resultOf(site.callee), expected);
        }
        RELEASE_ASSERT_NOT_REACHED();
    };

    auto demoteMismatchedTailCalls = [&] {
        Vector<TailCallSite> demotedSites;
        for (const TailCallSite& site : sites) {
            if (canRemainTailCall(site))
                continue;
            switch (site.requirement) {
            case TailCallRequirement::Required:
                build.diagnostics().add(InternalError { .kind = InternalErrorKind::RequiredTailCallNotConverted, .function = site.caller, .block = site.block, .value = program.at(site.caller).at(site.block).terminator.callOperand(), .pass = PassID::ChooseCallingConventions });
                break;
            case TailCallRequirement::Allowed:
                demotedSites.append(site);
                break;
            }
        }
        if (demotedSites.isEmpty())
            return;

        BitVector isEdited(count);
        for (const TailCallSite& site : demotedSites) {
            if (!isEdited.quickSet(site.caller.index()))
                editedCallers.append(site.caller);
        }
        editFunctionsInParallel(editor, build, editedCallers.span(), [&](unsigned, FunctionID caller, Function& function, TaskArenas& arenas) {
            {
                IRBuilder builder(AnalysisContext { build, program, &inputs.flow, &inputs.effects, caller }, function, arenas.types);
                for (const TailCallSite& site : sitesOf(demotedSites.span(), caller) | std::views::reverse)
                    builder.demoteReturnCall(site.block);
            }
            simplifyEditedFunction(runner, program, inputs, caller, function, arenas);
        });
        build.statistics().count(StatisticID::ReturnCallsDemoted, demotedSites.size());

        // Simplification renumbers the blocks of an edited function, so its remaining sites are collected again.
        sites.removeAllMatching([&](const TailCallSite& site) {
            return isEdited.quickGet(site.caller.index());
        });
        size_t numberOfUneditedSites = sites.size();
        for (FunctionID caller : editedCallers)
            collectTailCallSitesOf(program, caller, sites);
        std::ranges::inplace_merge(sites, sites.begin() + numberOfUneditedSites, { }, &TailCallSite::caller);
    };
    demoteMismatchedTailCalls();

    auto reserveStackWords = [&] {
        auto mayReserve = [&](FunctionID id) {
            RecordFacts facts = recordFactsOf(program, id);
            return facts.hasBody && hasStackArgumentAreaOf(facts.role);
        };
        Vector<uint32_t> reserved(FillWith { }, count, 0u);
        Vector<FunctionID> worklist = collectInParallel<FunctionID>(loops, count, [&](unsigned index, Vector<FunctionID>& functionsWithWords) {
            FunctionID id(index);
            if (!mayReserve(id))
                return;
            reserved[index] = locationsOf(id).numberOfStackWords();
            if (reserved[index])
                functionsWithWords.append(id);
        });
        auto raise = [&](FunctionID id, uint32_t numberOfWords) {
            if (reserved[id.index()] >= numberOfWords)
                return;
            reserved[id.index()] = numberOfWords;
            worklist.append(id);
        };

        Vector<TailCallEdge> edges;
        for (const TailCallSite& site : sites) {
            if (!mayReserve(site.caller))
                continue;
            if (site.calleeKind != TailCalleeKind::Unknown) {
                edges.append({ site.callee, site.caller });
                continue;
            }
            const Function& caller = program.at(site.caller);
            const Value& call = caller.at(caller.at(site.block).terminator.callOperand());
            if (call.opcode != Opcode::Call)
                continue;
            size_t numberOfArguments = call.argumentsOperands().size();
            if (numberOfArguments > numberOfArgumentGPRs)
                raise(site.caller, static_cast<uint32_t>(numberOfArguments - numberOfArgumentGPRs));
        }
        if (worklist.isEmpty())
            return;
        sortAndRemoveDuplicates(loops, edges);

        while (!worklist.isEmpty()) {
            FunctionID callee = worklist.takeLast();
            uint32_t numberOfWords = reserved[callee.index()];
            for (const TailCallEdge& edge : std::ranges::equal_range(edges, callee, { }, &TailCallEdge::callee))
                raise(edge.caller, numberOfWords);
        }

        uint64_t limit = build.limit(LimitID::MaxReservedStackArguments);
        for (unsigned index = 0; index < count; ++index) {
            if (reserved[index] > limit)
                build.reportLimitExceeded(LimitID::MaxReservedStackArguments, reserved[index], Subject { .function = FunctionID(index) });
            else
                tables.conventions[index].reservedStackWords = static_cast<uint16_t>(reserved[index]);
        }
    };
    reserveStackWords();

    Vector<EntryRequest> requests = collectInParallel<EntryRequest>(loops, count, [&](unsigned callerIndex, Vector<EntryRequest>& collected) {
        FunctionID callerID(callerIndex);
        // An argument that is undefined is an operation of the caller.
        if (!mayHoldValueWith(program, callerID, { Opcode::Undefined }))
            return;
        const Function& caller = program.functions[callerID.index()];
        auto isUndefined = [&](ValueID id) {
            return caller.at(definitionThroughUpcasts(caller, id)).opcode == Opcode::Undefined;
        };
        forEachListedValue(program, callerID, { Opcode::CallDirect }, [&](BlockID block, uint32_t, ValueID id, const Value& call) {
            std::span<const ValueID> arguments = call.argumentsOperands();
            if (!std::ranges::any_of(arguments, isUndefined))
                return;
            const Terminator& terminator = caller.at(block).terminator;
            if (terminator.kind == TerminatorKind::ReturnCall && terminator.callOperand() == id)
                return;

            FunctionID calleeID = call.function();
            const Function& callee = program.at(calleeID);
            if (!callee.hasBody()
                || entryConventionOf(callee) != EntryConvention::Direct
                || callee.role == FunctionRole::TableEntrypoint
                || callee.role == FunctionRole::TableLookup
                || callee.role == FunctionRole::StubBody
                || !isEnabledFor(calleeID))
                return;
            ArgumentLocations locations = locationsOf(calleeID);
            bool passesIntegersOnStack = std::ranges::any_of(locations.parameters, [](const ArgumentLocation& location) {
                return location.kind == ArgumentLocation::Kind::Stack && !isPassedInFPR(location.representation);
            });
            if (passesIntegersOnStack)
                return;

            RELEASE_ASSERT(arguments.size() == locations.parameters.size());
            unsigned numberOfGPRs = 0;
            unsigned firstOmittedGPR = 0;
            for (size_t index = 0; index < arguments.size(); ++index) {
                const ArgumentLocation& location = locations.parameters[index];
                switch (location.kind) {
                case ArgumentLocation::Kind::GPR:
                    ++numberOfGPRs;
                    if (location.representation != Representation::JSValue || !isUndefined(arguments[index]))
                        firstOmittedGPR = numberOfGPRs;
                    break;
                case ArgumentLocation::Kind::None:
                case ArgumentLocation::Kind::FPR:
                case ArgumentLocation::Kind::Stack:
                    break;
                }
            }
            if (firstOmittedGPR != numberOfGPRs)
                collected.append({ calleeID, static_cast<uint8_t>(firstOmittedGPR) });
        });
    });
    sortAndRemoveDuplicates(loops, requests);

    tables.firstSecondaryEntry.fill(0, count + 1);
    tables.secondaryEntries.reserveInitialCapacity(requests.size());
    for (const EntryRequest& request : requests) {
        uint32_t& numberOfEntries = tables.firstSecondaryEntry[request.callee.index() + 1];
        tables.secondaryEntries.append(SecondaryEntry { static_cast<uint16_t>(++numberOfEntries), request.firstOmittedGPR });
    }
    std::partial_sum(tables.firstSecondaryEntry.begin(), tables.firstSecondaryEntry.end(), tables.firstSecondaryEntry.begin());
    build.statistics().count(StatisticID::SecondaryEntries, requests.size());
    RELEASE_ASSERT(program.functions.size() == count);

    size_t numberOfClosuresNeverCreated = findIndicesInParallel(loops, count, [&](unsigned index) {
        const Function& function = program.functions[index];
        if (!function.hasBody() || !hasSourceBodyOf(function.role) || !canBeCalledByValueOf(function.role) || function.hasOnlyDirectCallers())
            return false;
        return !inputs.derivedFunctions->find(derivedFunctionKey(FunctionRole::GenericEntrypoint, FunctionID(index)));
    }).size();
    build.statistics().count(StatisticID::ClosuresNeverCreated, numberOfClosuresNeverCreated);
    build.statistics().countChanges(PassID::ChooseCallingConventions, editedCallers.size());
    return CallingConventions(WTF::move(tables));
}

static ASCIILiteral nameOf(StackCheck check)
{
    switch (check) {
    case StackCheck::Required:
        return "Required"_s;
    case StackCheck::CoveredByCallers:
        return "CoveredByCallers"_s;
    case StackCheck::DeferredToCallee:
        return "DeferredToCallee"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void StackChecks::dump(PrintStream& out, const Program& program) const
{
    for (unsigned i = 0; i < program.functions.size(); ++i) {
        if (program.functions[i].hasBody())
            out.print("f", i, " stack check: ", nameOf(of(FunctionID(i))), "\n");
    }
}

// A closure names its generic entrypoint, and whatever enters code through the word of a closure tests the stack right in front of it.
static bool isEnteredBehindStackCheck(const Function& function, EntrySet entrySet)
{
    switch (function.role) {
    case FunctionRole::Source:
    case FunctionRole::Library:
    case FunctionRole::ParameterWrapper:
    case FunctionRole::EntryChecks:
    case FunctionRole::ResultWrapper:
    case FunctionRole::GenericEntrypoint:
        return true;
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
    case FunctionRole::ResultWorker:
    case FunctionRole::OutlinedBody:
    case FunctionRole::Serializer:
    case FunctionRole::Parser:
    case FunctionRole::ChainStep:
    case FunctionRole::StubBody:
        return entrySet == EntrySet::DirectCallers && !function.creator.is<CreatorRecord::Entrypoint>();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The tests in front of a body: whichever way they end, the function goes on with a tail call, so no frame of its own is left on the stack.
static bool isEntranceThatLeavesByTailCallsAlone(const Function& function)
{
    if (function.role != FunctionRole::EntryChecks && function.role != FunctionRole::ParameterWrapper)
        return false;
    return std::ranges::all_of(function.blocks, [&](const Block& block) {
        return std::ranges::all_of(block.values, [&](ValueID id) {
            return function.at(id).opcode != Opcode::CallDirect || (block.terminator.kind == TerminatorKind::ReturnCall && block.terminator.callOperand() == id);
        });
    });
}

static CalledCodeKind classifyCalledCode(const EffectContext& context, const Function& function, const Value& value, const BitVector& isCandidate)
{
    Effects effects = effectsOf(context, function, value).onAnyEdge();
    if (effects.maySuspend)
        return CalledCodeKind::Unknown;
    if (!effects.mayCallUnknownCode)
        return CalledCodeKind::None;
    if (effectRuleOf(value.opcode) != EffectRule::Call)
        return CalledCodeKind::Unknown;

    bool hasCallee = false;
    bool hasUncheckedCallee = false;
    forEachFunctionReference(value, [&](FunctionID target, FunctionReferenceKind kind) {
        if (kind != FunctionReferenceKind::Call) {
            hasUncheckedCallee = true;
            return;
        }
        hasCallee = true;
        auto& callee = context.program.at(target);
        hasUncheckedCallee |= !callee.hasBody() || isCandidate.get(target.index()) || callee.creator.is<CreatorRecord::Entrypoint>();
    });
    return hasCallee && !hasUncheckedCallee ? CalledCodeKind::FunctionWithStackCheck : CalledCodeKind::Unknown;
}

static bool canDeferStackCheck(const EffectContext& context, const Function& function, const BitVector& isCandidate)
{
    bool hasDirectCall = false;
    for (auto& block : function.blocks) {
        for (ValueID id : block.values) {
            switch (classifyCalledCode(context, function, function.at(id), isCandidate)) {
            case CalledCodeKind::Unknown:
                return false;
            case CalledCodeKind::None:
                break;
            case CalledCodeKind::FunctionWithStackCheck:
                hasDirectCall = true;
                break;
            }
        }
        Effects effects = effectsOf(context, function, block.terminator).onAnyEdge();
        if (effects.mayCallUnknownCode || effects.maySuspend)
            return false;
    }
    return hasDirectCall;
}

StackChecks elideStackChecks(const Program& program, const CallGraph& callGraph, BuildContext& build)
{
    PhaseTimes times(build, PassID::ElideStackChecks);
    unsigned count = program.functions.size();
    unsigned maxNested = static_cast<unsigned>(std::min<uint64_t>(maxNestedFunctionsWithoutStackCheck, build.limit(LimitID::UncheckedStackBytes) / 2 / (maxFrameSizeWithoutStackCheck + 2 * sizeof(void*))));
    const ParallelLoops& loops = build.parallelLoops();

    // The entrypoint of the method table tests its arguments and goes on with a tail call.
    BitVector isTailCalledByTableEntrypoint(count);
    for (uint32_t index : findIndicesInParallel(loops, count, [&](unsigned index) {
        const Function& function = program.functions[index];
        return function.hasBody() && function.role == FunctionRole::TableEntrypoint;
    })) {
        const Function& function = program.functions[index];
        for (const Block& block : function.blocks) {
            if (block.terminator.kind != TerminatorKind::ReturnCall)
                continue;
            const Value& call = function.at(block.terminator.callOperand());
            if (call.opcode == Opcode::CallDirect)
                isTailCalledByTableEntrypoint.set(call.function().index());
        }
    }

    BitVector isCandidate(count);
    for (uint32_t i : findIndicesInParallel(loops, count, [&](unsigned i) {
        FunctionID id(i);
        auto& function = program.at(id);
        return function.hasBody()
            && isEnteredBehindStackCheck(function, callGraph.entrySet(id))
            && function.hasOrdinaryFrame()
            && !isTailCalledByTableEntrypoint.get(i)
            && build.isEnabled(PassID::ElideStackChecks, id);
    }))
        isCandidate.set(i);

    BitVector isEntrance(count);
    for (uint32_t i : findIndicesInParallel(loops, count, [&](unsigned i) {
        const Function& function = program.functions[i];
        return function.hasBody() && isEntranceThatLeavesByTailCallsAlone(function);
    }))
        isEntrance.set(i);

    // An entrance needs no frame but for a check of its own: the frame, the check, and a jump to the body where it would fall into it.
    BitVector isEntranceInCycle(count);
    uint64_t numberOfFunctionsBelowEntrancesVisited = 0;
    enum class VisitState : uint8_t { NotSeen, InProgress, Done };
    Vector<VisitState> state(FillWith { }, count, VisitState::NotSeen);
    Vector<uint16_t> height(FillWith { }, count, static_cast<uint16_t>(0));
    static_assert(maxNestedFunctionsWithoutStackCheck < std::numeric_limits<uint16_t>::max());
    Vector<Vector<uint32_t, 4>> callees(count);
    forEachIndexInParallel(build.parallelLoops(), count, [&](unsigned i) {
        if (!isCandidate.get(i))
            return;
        for (FunctionID callee : callGraph.calleesOf(FunctionID(i))) {
            if (isCandidate.get(callee.index()))
                callees[i].append(callee.index());
        }
        if (callees[i].isEmpty() && maxNested) {
            state[i] = VisitState::Done;
            height[i] = 1;
        }
    });
    Vector<std::pair<uint32_t, uint32_t>> inProgress;
    for (unsigned root = 0; root < count; ++root) {
        if (!isCandidate.get(root) || state[root] != VisitState::NotSeen)
            continue;
        state[root] = VisitState::InProgress;
        inProgress.append({ root, 0 });
        while (!inProgress.isEmpty()) {
            auto [function, next] = inProgress.last();
            if (next < callees[function].size()) {
                ++inProgress.last().second;
                uint32_t callee = callees[function][next];
                if (state[callee] == VisitState::InProgress) {
                    if (!isEntrance.get(callee))
                        isCandidate.clear(callee);
                    else if (!isEntranceInCycle.quickSet(callee)) {
                        for (uint32_t entered : callees[callee])
                            isCandidate.clear(entered);
                        numberOfFunctionsBelowEntrancesVisited += callees[callee].size();
                    }
                } else if (state[callee] == VisitState::NotSeen) {
                    state[callee] = VisitState::InProgress;
                    inProgress.append({ callee, 0 });
                }
                continue;
            }
            unsigned below = 0;
            for (uint32_t callee : callees[function]) {
                // What is still without a check while it is being visited is an entrance in a cycle: all that it enters checks, so it is one function and nothing below it.
                if (isCandidate.get(callee))
                    below = std::max<unsigned>(below, state[callee] == VisitState::InProgress ? 1 : height[callee]);
            }
            if (isCandidate.get(function) && below >= maxNested && isEntrance.get(function)) {
                below = 0;
                for (uint32_t callee : callees[function]) {
                    if (!isCandidate.get(callee))
                        continue;
                    if (height[callee] >= maxNested)
                        isCandidate.clear(callee);
                    else
                        below = std::max<unsigned>(below, height[callee]);
                }
            }
            if (isCandidate.get(function) && below >= maxNested) {
                build.gaveUp(LimitID::UncheckedStackBytes, Subject { .function = FunctionID(function) });
                isCandidate.clear(function);
            }
            height[function] = isCandidate.get(function) ? static_cast<uint16_t>(below + 1) : 0;
            state[function] = VisitState::Done;
            inProgress.removeLast();
        }
    }
    build.destroyLater(WTF::move(callees));

    // An entrance that lies in a cycle of calls has a check of its own.
    for (uint32_t i : findIndicesInParallel(build.parallelLoops(), program.functions.size(), [&](unsigned i) {
        return isEntrance.get(i) && build.isEnabled(PassID::ElideStackChecks, FunctionID(i));
    })) {
        const Function& function = program.functions[i];
        if (isCandidate.get(i))
            continue;
        Vector<uint32_t, 2> rarelyEntered;
        bool mostlyEntersFunctionWithCheck = true;
        for (const Block& block : function.blocks) {
            if (block.terminator.kind != TerminatorKind::ReturnCall)
                continue;
            const Value& call = function.at(block.terminator.callOperand());
            if (call.opcode != Opcode::CallDirect || !isCandidate.get(call.function().index()))
                continue;
            if (block.frequency.predicted() == BlockFrequency::Normal)
                mostlyEntersFunctionWithCheck = false;
            rarelyEntered.append(call.function().index());
        }
        if (!mostlyEntersFunctionWithCheck)
            continue;
        for (uint32_t callee : rarelyEntered)
            isCandidate.clear(callee);
    }

    EffectSummaries unknownSummaries;
    EffectContext context { program, unknownSummaries };
    Vector<uint32_t> deferringEntrypoints = findIndicesInParallel(build.parallelLoops(), count, [&](unsigned i) {
        FunctionID id(i);
        auto& function = program.at(id);
        return function.hasBody()
            && (function.creator.is<CreatorRecord::Entrypoint>() || isEntrance.get(i))
            && !isCandidate.get(i)
            && build.isEnabled(PassID::ElideStackChecks, id)
            && canDeferStackCheck(context, function, isCandidate);
    });
    BitVector mayLieInCycle(program.functions.size());
    for (uint32_t i : deferringEntrypoints)
        mayLieInCycle.set(i);
    for (bool changed = true; changed;) {
        changed = false;
        for (uint32_t i : deferringEntrypoints) {
            if (!mayLieInCycle.get(i))
                continue;
            const Function& function = program.functions[i];
            bool entersOneThatMay = std::ranges::any_of(function.blocks, [&](const Block& block) {
                return std::ranges::any_of(block.values, [&](ValueID id) {
                    const Value& value = function.at(id);
                    return value.opcode == Opcode::CallDirect && mayLieInCycle.get(value.function().index());
                });
            });
            if (!entersOneThatMay) {
                mayLieInCycle.clear(i);
                changed = true;
            }
        }
    }
    deferringEntrypoints.removeAllMatching([&](uint32_t i) {
        return mayLieInCycle.get(i);
    });

    Vector<StackCheck> byFunction(FillWith { }, count, StackCheck::Required);
    for (size_t i : isCandidate)
        byFunction[i] = StackCheck::CoveredByCallers;
    for (uint32_t i : deferringEntrypoints)
        byFunction[i] = StackCheck::DeferredToCallee;
    build.statistics().count(StatisticID::StackChecksCoveredByCallers, isCandidate.bitCount());
    build.statistics().count(StatisticID::StackChecksDeferredToCallee, deferringEntrypoints.size());
    build.statistics().count(StatisticID::FunctionsBelowEntrancesInCyclesVisited, numberOfFunctionsBelowEntrancesVisited);
    times.didFinish("elideStackChecks"_s);
    return StackChecks(WTF::move(byFunction));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
