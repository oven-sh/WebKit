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
#include "AOTCFG.h"
#include "AOTCallGraph.h"
#include "AOTConvention.h"
#include "AOTDerivedFunctions.h"
#include "AOTDiagnostics.h"
#include "AOTEffectSummaries.h"
#include "AOTEffects.h"
#include "AOTElideStackChecks.h"
#include "AOTFunctionAnalyses.h"
#include "AOTFunctionEditing.h"
#include "AOTIRBuilder.h"
#include "AOTMethodTableEntrypoints.h"
#include "AOTOperatorForms.h"
#include "AOTPassRunner.h"
#include "AOTPhaseTimes.h"
#include "AOTRelocation.h"
#include "AOTRepresentationRules.h"
#include "AOTSchemaWalkers.h"
#include "AOTSort.h"
#include "AOTStubBodyConvention.h"
#include "AOTThreads.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <compare>
#include <functional>
#include <limits>
#include <numeric>
#include <optional>
#include <ranges>
#include <utility>
#include <wtf/Atomics.h>
#include <wtf/BitVector.h>
#include <wtf/CommaPrinter.h>
#include <wtf/EnumTraits.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/PrintStream.h>
#include <wtf/ScopedLambda.h>

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

// Table: the callee is the lookup of the selector. The call enters it or an entrypoint of the selector, all of which return alike.
enum class TailCalleeKind : uint8_t { Unknown, Direct, Table };

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

// Functions that return in one location. WTF::UnionFind shortens the way of the node that is asked about and of no other, so a row of n tail calls took n * n / 2 steps.
class ConventionResultClasses {
public:
    explicit ConventionResultClasses(uint32_t count)
        : m_next(count, [](size_t index) { return static_cast<uint32_t>(index); })
    {
    }

    uint32_t find(uint32_t index)
    {
        while (m_next[index] != index) {
            m_next[index] = m_next[m_next[index]];
            index = m_next[index];
        }
        return index;
    }

    void unify(uint32_t first, uint32_t second)
    {
        m_next[find(first)] = find(second);
    }

private:
    Vector<uint32_t> m_next;
};

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
    case FunctionRole::ChecksThatGiveUp:
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

static std::span<const ImpureParameter> impureParametersOf(const CallingConventionTables& tables, FunctionID id)
{
    if (id.index() >= tables.conventions.size() || !tables.conventions[id.index()].hasImpureParameters)
        return { };
    auto range = std::ranges::equal_range(tables.impureParameters, id, { }, &ImpureParameter::function);
    return { range.begin(), range.end() };
}

static Representation directParameterRepresentation(const Program& program, const Function& callee, unsigned index, std::span<const ImpureParameter> impureParameters)
{
    if (const DeclaredType* declared = callee.declaredEntryParameter(index)) {
        Representation representation = declaredRepresentationOf(program, *declared);
        if (representation == Representation::Double && std::ranges::binary_search(impureParameters, index, { }, &ImpureParameter::entryParameterIndex))
            return Representation::ImpureDouble;
        return representation;
    }

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
    if (representation == Representation::Double && convention.resultIsImpure)
        representation = Representation::ImpureDouble;
    if (isPassedInFPR(representation))
        return { .kind = ArgumentLocation::Kind::FPR, .reg = FPRInfo::returnValueFPR, .representation = representation };
    return { .kind = ArgumentLocation::Kind::GPR, .reg = GPRInfo::returnValueGPR, .representation = representation };
}

static ArgumentLocations computeArgumentLocations(const Program& program, const Function& callee, FunctionConvention convention, ConventionRegisters registers, std::span<const ImpureParameter> impureParameters)
{
    ArgumentLocations locations;
    locations.convention = entryConventionOf(callee);
    unsigned count = callee.numberOfEntryParameters();
    locations.parameters.reserveInitialCapacity(count);
    switch (locations.convention) {
    case EntryConvention::Direct: {
        ArgumentAllocator allocator { registers };
        for (unsigned index = 0; index < count; ++index)
            locations.parameters.append(allocator.allocate(directParameterRepresentation(program, callee, index, impureParameters)));
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
        return directParameterRepresentation(program, callee, entryParameterIndex, impureParametersOf(m_tables, id));
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
    return computeArgumentLocations(program, program.at(id), of(program, id), m_tables.registers, impureParametersOf(m_tables, id));
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

bool CallingConventions::letsGoOfParameter(FunctionID callee, unsigned entryParameterIndex) const
{
    return !m_tables.parametersLetGo.isEmpty() && std::ranges::binary_search(m_tables.parametersLetGo, ParameterLetGo { callee, entryParameterIndex });
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
        for (unsigned parameter = 0; parameter < locations.parameters.size(); ++parameter) {
            if (letsGoOfParameter(id, parameter))
                out.print(" lets-go=", parameter);
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
        TailCalleeKind kind = call.opcode == Opcode::CallDirect ? TailCalleeKind::Direct : call.opcode == Opcode::CallTable ? TailCalleeKind::Table : TailCalleeKind::Unknown;
        sites.append({ caller, block, kind, kind == TailCalleeKind::Unknown ? FunctionID() : call.function(), terminator.requirement(), hasCheckedCast });
    });
}

class EntryParameterFinder {
public:
    explicit EntryParameterFinder(const Function& function)
        : m_function(function)
    {
    }

    std::optional<uint32_t> indexOf(ValueID value)
    {
        for (size_t remainingSteps = m_function.values.size(); remainingSteps--;) {
            ValueID root = sameObjectThroughCasts(m_function, value).valueInFrontOfFacts();
            if (m_function.at(root).opcode != Opcode::Parameter)
                return std::nullopt;
            if (size_t index = m_function.at(entryBlock).parameters.find(root); index != notFound)
                return static_cast<uint32_t>(index);
            findNarrowedCopies();
            auto subject = m_subjectOfNarrowedCopy.find(root.index());
            if (subject == m_subjectOfNarrowedCopy.end())
                return std::nullopt;
            value = subject->value;
        }
        return std::nullopt;
    }

private:
    void findNarrowedCopies()
    {
        if (std::exchange(m_didFindNarrowedCopies, true))
            return;
        for (uint32_t block = 0; block < m_function.blocks.size(); ++block) {
            forEachEdgeResult(m_function, BlockID(block), [&](const EdgeResult& result, ValueID parameter) {
                if (result.kind == EdgeResultKind::NarrowedOperand)
                    m_subjectOfNarrowedCopy.add(parameter.index(), result.subject);
            });
        }
    }

    const Function& m_function;
    UncheckedKeyHashMap<uint32_t, ValueID, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_subjectOfNarrowedCopy;
    bool m_didFindNarrowedCopies { false };
};

std::optional<uint32_t> entryParameterIndexOf(const Function& function, ValueID value)
{
    return EntryParameterFinder(function).indexOf(value);
}

// A parameter that the function sets to null, or that it hands on as such a parameter.
static Vector<ParameterLetGo> findParametersLetGo(const Program& program, const CallGraph& callGraph, const ParallelLoops& loops)
{
    unsigned count = program.functions.size();
    Vector<ParameterLetGo> found = collectInParallel<ParameterLetGo>(loops, count, [&](unsigned index, Vector<ParameterLetGo>& found) {
        EntryParameterFinder parameters(program.functions[index]);
        forEachListedValue(program, FunctionID(index), { Opcode::ReleaseHint }, [&](BlockID, uint32_t, ValueID, const Value& hint) {
            if (std::optional<uint32_t> parameter = parameters.indexOf(hint.valueOperand()))
                found.append({ FunctionID(index), *parameter });
        });
    });
    if (found.isEmpty())
        return found;

    auto keyOf = [](FunctionID function, uint32_t parameter) { return static_cast<uint64_t>(function.index()) << 32 | parameter; };
    UncheckedKeyHashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> known;
    BitVector letsGo(count);
    BitVector isChanged(count);
    Vector<FunctionID> changed;
    Vector<ParameterLetGo> all;
    auto add = [&](FunctionID function, uint32_t parameter) {
        if (!known.add(keyOf(function, parameter)).isNewEntry)
            return;
        all.append({ function, parameter });
        letsGo.quickSet(function.index());
        if (!isChanged.quickSet(function.index()))
            changed.append(function);
    };
    for (ParameterLetGo letGo : found)
        add(letGo.function, letGo.entryParameterIndex);

    // A round reads the callers of the functions that the round before has added to.
    BitVector isListed(count);
    Vector<FunctionID> callers;
    while (!changed.isEmpty()) {
        for (FunctionID callee : changed) {
            isChanged.quickClear(callee.index());
            for (FunctionID caller : callGraph.callersOf(callee)) {
                if (!isListed.quickSet(caller.index()))
                    callers.append(caller);
            }
        }
        changed.shrink(0);
        for (FunctionID caller : callers) {
            isListed.quickClear(caller.index());
            const Function& function = program.at(caller);
            EntryParameterFinder parameters(function);
            forEachDirectCall(program, caller, [&](BlockID, ValueID call, FunctionID callee) {
                if (!letsGo.quickGet(callee.index()))
                    return;
                std::span<const ValueID> arguments = function.at(call).argumentsOperands();
                for (uint32_t index = 0; index < arguments.size(); ++index) {
                    if (!arguments[index] || !known.contains(keyOf(callee, index)))
                        continue;
                    if (std::optional<uint32_t> parameter = parameters.indexOf(arguments[index]))
                        add(caller, *parameter);
                }
            });
        }
        callers.shrink(0);
    }
    AOT::sortInLine(all);
    return all;
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

namespace {

// Lets a double read from a typed array or DataView cross calls and returns without being purified.
// purifyNaN() drops the sign of a NaN and arithmetic NaNs keep theirs, so a parameter or result is only chosen when every value that reaches it comes from bytes.
class ImpureDoubleConventions {
public:
    ImpureDoubleConventions(const Program& program, const OptimizationInputs& inputs, BuildContext& build, const CallingConventionTables& tables, std::span<const ArgumentLocation> results, const ScopedLambda<bool(FunctionID)>& isEnabledFor)
        : m_program(program)
        , m_inputs(inputs)
        , m_build(build)
        , m_tables(tables)
        , m_results(results)
        , m_isEnabledFor(isEnabledFor)
    {
    }

    void choose(std::span<const uint32_t> functionsReadingBytes, CallingConventionTables& chosen)
    {
        m_bodyOf.fill(noBody, m_program.functions.size());
        do {
            for (auto& body : m_bodies) {
                body->valuesFromBytes.clearAll();
                body->parametersFromBytes.clearAll();
                body->resultIsFromBytes = false;
            }
            for (uint32_t index : functionsReadingBytes)
                visitLater(bodyOf(FunctionID(index)));
            while (!m_worklist.isEmpty()) {
                Body& body = *m_bodies[m_worklist.takeLast()];
                body.isOnWorklist = false;
                propagateFromBytes(body);
            }
        } while (rejectMixedJoins());

        do {
            for (auto& body : m_bodies) {
                body->impureValues.clearAll();
                body->impureParameters.clearAll();
                body->resultIsImpure = false;
            }
            for (uint32_t index : functionsReadingBytes)
                visitLater(bodyOf(FunctionID(index)));
            while (!m_worklist.isEmpty()) {
                Body& body = *m_bodies[m_worklist.takeLast()];
                body.isOnWorklist = false;
                propagateImpure(body);
            }
        } while (rejectUnprofitable());

        for (auto& body : m_bodies) {
            for (size_t index : body->impureParameters) {
                chosen.impureParameters.append({ body->id, static_cast<uint32_t>(index) });
                chosen.conventions[body->id.index()].hasImpureParameters = true;
            }
            chosen.conventions[body->id.index()].resultIsImpure = body->resultIsImpure;
        }
        AOT::sort(chosen.impureParameters);
    }

private:
    static constexpr uint32_t noBody = UINT32_MAX;

    struct Body {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Body);

        Body(const AnalysisContext& context, const Function& function)
            : id(context.id)
            , function(function)
            , analyses(context, function)
        {
        }

        FunctionID id;
        const Function& function;
        mutable FunctionAnalyses analyses;
        Vector<std::pair<ValueID, ValueID>, 4> narrowedCopies;
        bool hasOnlyKnownCallers { false };
        bool isOnWorklist { false };
        uint32_t index { 0 };

        BitVector valuesFromBytes;
        BitVector parametersFromBytes;
        bool resultIsFromBytes { false };
        BitVector mixedValues;
        BitVector mixedParameters;
        bool resultIsMixed { false };

        BitVector impureValues;
        BitVector impureParameters;
        bool resultIsImpure { false };
        BitVector purifiedParameters;
        bool resultIsPurified { false };
    };

    Body* findBodyOf(FunctionID id) const
    {
        uint32_t index = m_bodyOf[id.index()];
        return index == noBody ? nullptr : m_bodies[index].get();
    }

    Body& bodyOf(FunctionID id)
    {
        if (Body* body = findBodyOf(id))
            return *body;
        const Function& function = m_program.at(id);
        RELEASE_ASSERT(function.hasBody());
        auto body = makeUnique<Body>(AnalysisContext { m_build, m_program, &m_inputs.flow, &m_inputs.effects, id }, function);
        body->index = m_bodies.size();
        body->hasOnlyKnownCallers = canBoxResult(function)
            && function.role != FunctionRole::TableEntrypoint && function.role != FunctionRole::TableLookup && function.role != FunctionRole::StubBody
            && m_inputs.callGraph.entrySet(id) == EntrySet::DirectCallers && !function.mayBeCalledByHost() && m_isEnabledFor(id);
        for (BlockID block : body->analyses.cfg().reversePostOrder()) {
            forEachOutgoingEdge(function, block, [&](Edge edge, const Target& target) {
                const CompactList<ValueID>& parameters = function.at(target.block).parameters;
                for (uint32_t index = 0; index < parameters.size(); ++index) {
                    IncomingValue incoming = incomingValueOf(function, edge, index);
                    if (incoming.kind != IncomingValue::Kind::EdgeResult)
                        continue;
                    EdgeResult result = edgeResultAt(function, incoming.result);
                    if (result.kind == EdgeResultKind::NarrowedOperand)
                        body->narrowedCopies.append({ result.subject, parameters[index] });
                }
            });
        }
        AOT::sort(body->narrowedCopies);
        m_bodyOf[id.index()] = body->index;
        m_bodies.append(WTF::move(body));
        return *m_bodies.last();
    }

    void visitLater(Body& body)
    {
        if (!std::exchange(body.isOnWorklist, true))
            m_worklist.append(body.index);
    }

    const Type& typeOf(const Body& body, ValueID id) const { return m_program.typeOf(body.function, id); }
    bool isKeptAsDouble(const Body& body, ValueID id) const { return defaultRepresentationOf(typeOf(body, id)) == Representation::Double; }
    bool isPassedAsDouble(FunctionID id, size_t entryParameterIndex) const { return directParameterRepresentation(m_program, m_program.at(id), entryParameterIndex, { }) == Representation::Double; }

    bool returnsDouble(const Body& body) const
    {
        return !m_tables.conventions[body.id.index()].resultIsBoxed && m_results[body.id.index()].kind == ArgumentLocation::Kind::FPR && m_results[body.id.index()].representation == Representation::Double;
    }

    bool takesImpure(FunctionID id, uint32_t entryParameterIndex) const
    {
        Body* body = findBodyOf(id);
        return body && body->impureParameters.get(entryParameterIndex);
    }

    bool isFromBytesOrNeverNaN(const Body* body, const Function& function, ValueID id) const
    {
        return !id || (body && body->valuesFromBytes.get(id.index())) || !m_program.typeOf(function, id).mayBeNaN();
    }

    template<typename Functor>
    void forEachCopyNarrowedFrom(const Body& body, ValueID subject, const Functor& functor) const
    {
        auto* copy = std::lower_bound(body.narrowedCopies.begin(), body.narrowedCopies.end(), std::pair { subject, ValueID(0u) });
        for (; copy != body.narrowedCopies.end() && copy->first == subject; ++copy)
            functor(copy->second);
    }

    void propagateFromBytes(Body& body)
    {
        const Function& function = body.function;
        const UseLists& useLists = body.analyses.useLists();
        Vector<ValueID, 16> worklist;
        auto reach = [&](ValueID id) {
            if (body.mixedValues.get(id.index()) || !typeOf(body, id).mayBeNaN())
                return;
            if (!body.valuesFromBytes.set(id.index()))
                worklist.append(id);
        };

        const CompactList<ValueID>& entryParameters = function.at(entryBlock).parameters;
        for (size_t index : body.parametersFromBytes)
            reach(entryParameters[index]);
        for (BlockID block : body.analyses.cfg().reversePostOrder()) {
            for (ValueID id : function.at(block).values) {
                const Value& value = function.at(id);
                Body* callee = value.opcode == Opcode::CallDirect ? findBodyOf(value.function()) : nullptr;
                if ((callee && callee->resultIsFromBytes) || readsDoubleOutOfBytesBeforeSelection(m_program, value))
                    reach(id);
            }
        }

        while (!worklist.isEmpty()) {
            ValueID id = worklist.takeLast();
            forEachCopyNarrowedFrom(body, id, reach);
            for (const Use& use : useLists.usesOf(id)) {
                switch (use.kind) {
                case Use::Kind::Operand: {
                    const Value& user = function.at(use.user);
                    if (user.opcode == Opcode::CallDirect) {
                        if (std::optional<uint32_t> index = user.variadicOperandIndex(use.operandIndex); index && m_program.at(user.function()).hasBody())
                            didPass(bodyOf(user.function()), *index);
                    } else if (nanUseBeforeSelection(m_program, user, use.operandIndex, OperatorForm::Numbers) == NaNUse::Propagates)
                        reach(use.user);
                    break;
                }
                case Use::Kind::TerminatorOperand: {
                    TerminatorKind kind = function.at(use.block).terminator.kind;
                    if (kind == TerminatorKind::Return || kind == TerminatorKind::ReturnCall)
                        didReturn(body);
                    break;
                }
                case Use::Kind::EdgeArgument:
                case Use::Kind::HandlerArgument:
                    reach(function.at(targetOf(function, use.edge()).block).parameters[use.operandIndex]);
                    break;
                case Use::Kind::BlockSubject:
                    break;
                }
            }
        }
    }

    void didPass(Body& callee, uint32_t entryParameterIndex)
    {
        if (callee.hasOnlyKnownCallers && !callee.mixedParameters.get(entryParameterIndex) && !callee.parametersFromBytes.set(entryParameterIndex))
            visitLater(callee);
    }

    void didReturn(Body& body)
    {
        if (!body.hasOnlyKnownCallers || body.resultIsMixed || std::exchange(body.resultIsFromBytes, true))
            return;
        for (FunctionID caller : m_inputs.callGraph.callersOf(body.id)) {
            if (m_program.at(caller).hasBody())
                visitLater(bodyOf(caller));
        }
    }

    bool isLoopHeader(const Body& body, BlockID block) const
    {
        const CFG& cfg = body.analyses.cfg();
        return std::ranges::any_of(cfg.incomingEdges(block), [&](Edge edge) {
            return cfg.reversePostOrderIndex(edge.from) >= cfg.reversePostOrderIndex(block);
        });
    }

    bool allIncomingAreFromBytes(const Body& body, ValueLocation join) const
    {
        return std::ranges::all_of(body.analyses.cfg().incomingEdges(join.block), [&](Edge edge) {
            IncomingValue incoming = incomingValueOf(body.function, edge, join.index);
            if (incoming.kind == IncomingValue::Kind::Argument)
                return isFromBytesOrNeverNaN(&body, body.function, incoming.argument);
            EdgeResult result = edgeResultAt(body.function, incoming.result);
            return result.kind == EdgeResultKind::NarrowedOperand && isFromBytesOrNeverNaN(&body, body.function, result.subject);
        });
    }

    bool allArgumentsAreFromBytes(const Body& callee, uint32_t entryParameterIndex) const
    {
        bool result = true;
        for (FunctionID callerID : m_inputs.callGraph.callersOf(callee.id)) {
            const Function& caller = m_program.at(callerID);
            if (!caller.hasBody())
                continue;
            forEachDirectCall(m_program, callerID, [&](BlockID, ValueID call, FunctionID called) {
                if (called == callee.id && caller.at(call).function() == called)
                    result &= isFromBytesOrNeverNaN(findBodyOf(callerID), caller, caller.at(call).argumentsOperands()[entryParameterIndex]);
            });
        }
        return result;
    }

    bool allReturnsAreFromBytes(const Body& body) const
    {
        bool result = true;
        forEachBlockEndingWith(m_program, body.id, { TerminatorKind::Return, TerminatorKind::ReturnCall }, [&](BlockID block) {
            const Terminator& terminator = body.function.at(block).terminator;
            result &= isFromBytesOrNeverNaN(&body, body.function, terminator.kind == TerminatorKind::Return ? terminator.valueOperand() : terminator.callOperand());
        });
        return result;
    }

    bool allTailCallersReturnImpure(const Body& callee) const
    {
        bool result = true;
        for (FunctionID callerID : m_inputs.callGraph.callersOf(callee.id)) {
            const Function& caller = m_program.at(callerID);
            if (!caller.hasBody())
                continue;
            forEachBlockEndingWith(m_program, callerID, { TerminatorKind::ReturnCall }, [&](BlockID block) {
                const Value& call = caller.at(caller.at(block).terminator.callOperand());
                if (call.opcode != Opcode::CallDirect || call.function() != callee.id)
                    return;
                Body* body = findBodyOf(callerID);
                result &= body && body->resultIsImpure;
            });
        }
        return result;
    }

    bool hasOnlyArithmeticUses(Body& body, ValueID parameter)
    {
        const Function& function = body.function;
        const UseLists& useLists = body.analyses.useLists();
        ScratchTypeArena types(m_program.types);
        TypingContext typing { m_program, function, types.arena() };
        OperatorSite site { typing, body.analyses.effectContext() };
        BitVector isSeen;
        Vector<ValueID, 16> worklist;
        auto follow = [&](ValueID id) {
            switch (defaultRepresentationOf(typeOf(body, id))) {
            case Representation::Double:
                if (!body.impureValues.get(id.index()))
                    return false;
                if (!isSeen.set(id.index()))
                    worklist.append(id);
                return true;
            case Representation::Void:
            case Representation::Boolean:
            case Representation::Int32:
            case Representation::Int64:
                return true;
            default:
                return false;
            }
        };
        follow(parameter);
        while (!worklist.isEmpty()) {
            ValueID id = worklist.takeLast();
            bool isKept = true;
            forEachCopyNarrowedFrom(body, id, [&](ValueID copy) {
                isKept &= follow(copy);
            });
            if (!isKept)
                return false;
            for (const Use& use : useLists.usesOf(id)) {
                if (function.at(use.block).frequency.predicted() != BlockFrequency::Normal)
                    continue;
                switch (use.kind) {
                case Use::Kind::Operand: {
                    const Value& user = function.at(use.user);
                    if (user.opcode == Opcode::CallDirect) {
                        std::optional<uint32_t> index = user.variadicOperandIndex(use.operandIndex);
                        if (!index || !takesImpure(user.function(), *index))
                            return false;
                        break;
                    }
                    OperatorForm form = hasOperatorForm(user.opcode) ? chooseFormOfOperator(site, user) : OperatorForm::Generic;
                    switch (nanUseBeforeSelection(m_program, user, use.operandIndex, form)) {
                    case NaNUse::Purifies:
                        return false;
                    case NaNUse::Ignores:
                        break;
                    case NaNUse::Propagates:
                        if (!follow(use.user))
                            return false;
                        break;
                    }
                    break;
                }
                case Use::Kind::TerminatorOperand:
                    switch (function.at(use.block).terminator.kind) {
                    case TerminatorKind::Return:
                        if (!body.resultIsImpure)
                            return false;
                        break;
                    case TerminatorKind::Switch:
                        break;
                    default:
                        return false;
                    }
                    break;
                case Use::Kind::EdgeArgument:
                case Use::Kind::HandlerArgument:
                    if (!follow(function.at(targetOf(function, use.edge()).block).parameters[use.operandIndex]))
                        return false;
                    break;
                case Use::Kind::BlockSubject:
                    break;
                }
            }
        }
        return true;
    }

    bool rejectMixedJoins()
    {
        bool hasRejected = false;
        for (auto& owned : m_bodies) {
            Body& body = *owned;
            const UseLists& useLists = body.analyses.useLists();
            for (size_t index : body.valuesFromBytes) {
                ValueLocation location = useLists.locationOf(ValueID(static_cast<uint32_t>(index)));
                if (location.placement != ValuePlacement::Parameter || location.block == entryBlock || isLoopHeader(body, location.block) || allIncomingAreFromBytes(body, location))
                    continue;
                body.mixedValues.set(index);
                hasRejected = true;
            }
            if (body.resultIsFromBytes && !allReturnsAreFromBytes(body)) {
                body.resultIsMixed = true;
                hasRejected = true;
            }
            for (size_t index : body.parametersFromBytes) {
                if (allArgumentsAreFromBytes(body, index))
                    continue;
                body.mixedParameters.set(index);
                hasRejected = true;
            }
        }
        return hasRejected;
    }

    void propagateImpure(Body& body)
    {
        const Function& function = body.function;
        const UseLists& useLists = body.analyses.useLists();
        ScratchTypeArena types(m_program.types);
        TypingContext typing { m_program, function, types.arena() };
        OperatorSite site { typing, body.analyses.effectContext() };
        Vector<ValueID, 16> worklist;
        auto reach = [&](ValueID id) {
            if (body.valuesFromBytes.get(id.index()) && isKeptAsDouble(body, id) && !body.impureValues.set(id.index()))
                worklist.append(id);
        };

        const CompactList<ValueID>& entryParameters = function.at(entryBlock).parameters;
        for (size_t index : body.impureParameters)
            reach(entryParameters[index]);
        for (BlockID block : body.analyses.cfg().reversePostOrder()) {
            for (ValueID id : function.at(block).values) {
                const Value& value = function.at(id);
                Body* callee = value.opcode == Opcode::CallDirect ? findBodyOf(value.function()) : nullptr;
                if ((callee && callee->resultIsImpure) || readsDoubleOutOfBytesBeforeSelection(m_program, value))
                    reach(id);
            }
        }

        while (!worklist.isEmpty()) {
            ValueID id = worklist.takeLast();
            forEachCopyNarrowedFrom(body, id, reach);
            for (const Use& use : useLists.usesOf(id)) {
                switch (use.kind) {
                case Use::Kind::Operand: {
                    const Value& user = function.at(use.user);
                    if (user.opcode == Opcode::CallDirect) {
                        std::optional<uint32_t> index = user.variadicOperandIndex(use.operandIndex);
                        Body* callee = index ? findBodyOf(user.function()) : nullptr;
                        if (callee && callee->parametersFromBytes.get(*index) && !callee->purifiedParameters.get(*index) && isPassedAsDouble(callee->id, *index) && !callee->impureParameters.set(*index))
                            visitLater(*callee);
                        break;
                    }
                    OperatorForm form = hasOperatorForm(user.opcode) ? chooseFormOfOperator(site, user) : OperatorForm::Generic;
                    if (nanUseBeforeSelection(m_program, user, use.operandIndex, form) == NaNUse::Propagates)
                        reach(use.user);
                    break;
                }
                case Use::Kind::TerminatorOperand: {
                    TerminatorKind kind = function.at(use.block).terminator.kind;
                    if ((kind != TerminatorKind::Return && kind != TerminatorKind::ReturnCall) || !body.resultIsFromBytes || body.resultIsPurified || !returnsDouble(body) || std::exchange(body.resultIsImpure, true))
                        break;
                    for (FunctionID caller : m_inputs.callGraph.callersOf(body.id)) {
                        if (Body* callerBody = findBodyOf(caller))
                            visitLater(*callerBody);
                    }
                    break;
                }
                case Use::Kind::EdgeArgument:
                case Use::Kind::HandlerArgument:
                    reach(function.at(targetOf(function, use.edge()).block).parameters[use.operandIndex]);
                    break;
                case Use::Kind::BlockSubject:
                    break;
                }
            }
        }
    }

    bool rejectUnprofitable()
    {
        bool hasChanged = false;
        for (auto& owned : m_bodies) {
            Body& body = *owned;
            if (body.resultIsImpure && !allTailCallersReturnImpure(body)) {
                body.resultIsPurified = true;
                hasChanged = true;
            }
        }
        if (hasChanged)
            return true;
        for (auto& owned : m_bodies) {
            Body& body = *owned;
            for (size_t index : body.impureParameters) {
                if (hasOnlyArithmeticUses(body, body.function.at(entryBlock).parameters[index]))
                    continue;
                body.purifiedParameters.set(index);
                hasChanged = true;
            }
        }
        return hasChanged;
    }

    const Program& m_program;
    const OptimizationInputs& m_inputs;
    BuildContext& m_build;
    const CallingConventionTables& m_tables;
    std::span<const ArgumentLocation> m_results;
    const ScopedLambda<bool(FunctionID)>& m_isEnabledFor;
    Vector<uint32_t> m_bodyOf;
    Vector<std::unique_ptr<Body>> m_bodies;
    Vector<uint32_t> m_worklist;
};

} // anonymous namespace

CallingConventions chooseCallingConventions(ProgramEditor& editor, const OptimizationInputs& inputs, const PassRunner& runner)
{
    RELEASE_ASSERT(inputs.derivedFunctions);
    const Program& program = editor.program();
    BuildContext& build = runner.build();
    const ParallelLoops& loops = build.parallelLoops();
    unsigned count = program.functions.size();
    PhaseTimes times(build, PassID::ChooseCallingConventions);

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
    auto locationsOf = [&](FunctionID id) { return computeArgumentLocations(program, program.at(id), tables.conventions[id.index()], tables.registers, { }); };
    auto locationOfOnlyResult = [&](FunctionID id) {
        const Function& function = program.at(id);
        RELEASE_ASSERT(function.results.size() == 1);
        return singleResultLocation(program, function, tables.conventions[id.index()], entryConventionOf(function));
    };
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

    times.didFinish("collectCalls"_s);

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
        if (tables.conventions[id.index()].resultIsBoxed)
            return;
        tables.conventions[id.index()].resultIsBoxed = true;
        results[id.index()] = locationOfOnlyResult(id);
        ++numberOfBoxedResults;
    };

    // If the callee's tail calls terminate, a caller that calls it in tail position may keep its frame until the callee returns.
    auto isNumberInRegister = [](Representation representation) {
        return representation == Representation::Int32 || isInt64OrMarker(representation) || isDoubleOrMarker(representation);
    };
    std::optional<BitVector> functionsEndingTailCalls;
    auto endsItsTailCalls = [&](FunctionID id) {
        if (!functionsEndingTailCalls) {
            BitVector ends(count);
            for (unsigned index = 0; index < count; ++index) {
                if (recordFactsOf(program, FunctionID(index)).hasBody)
                    ends.quickSet(index);
            }
            // A caller's tail calls terminate once those of each callee do. Each site is visited once, when its callee is known to terminate.
            constexpr uint32_t neverEnds = UINT32_MAX;
            Vector<uint32_t> numberOfSitesToWaitFor(FillWith { }, count, 0u);
            Vector<TailCallEdge> edges;
            edges.reserveInitialCapacity(sites.size());
            for (const TailCallSite& site : sites) {
                ends.quickClear(site.caller.index());
                uint32_t& numberOfSites = numberOfSitesToWaitFor[site.caller.index()];
                if (site.calleeKind != TailCalleeKind::Direct)
                    numberOfSites = neverEnds;
                else if (numberOfSites != neverEnds) {
                    ++numberOfSites;
                    edges.append({ site.callee, site.caller });
                }
            }
            parallelSort(loops, edges.mutableSpan(), std::less<TailCallEdge> { });
            Vector<FunctionID> worklist;
            for (size_t first = 0; first < edges.size();) {
                FunctionID callee = edges[first].callee;
                if (ends.quickGet(callee.index()))
                    worklist.append(callee);
                while (first < edges.size() && edges[first].callee == callee)
                    ++first;
            }
            while (!worklist.isEmpty()) {
                FunctionID callee = worklist.takeLast();
                for (const TailCallEdge& edge : std::ranges::equal_range(edges, callee, { }, &TailCallEdge::callee)) {
                    uint32_t& numberOfSites = numberOfSitesToWaitFor[edge.caller.index()];
                    if (numberOfSites == neverEnds || --numberOfSites)
                        continue;
                    ends.quickSet(edge.caller.index());
                    worklist.append(edge.caller);
                }
            }
            functionsEndingTailCalls = WTF::move(ends);
        }
        return functionsEndingTailCalls->quickGet(id.index());
    };
    auto mayKeepFrameToConvertNumber = [&](const TailCallSite& site) {
        return site.calleeKind == TailCalleeKind::Direct && !site.hasCheckedCast
            && isNumberInRegister(resultOf(site.caller)) && isNumberInRegister(resultOf(site.callee)) && !haveSameRegisterRepresentation(resultOf(site.callee), resultOf(site.caller))
            && program.at(site.caller).hasOrdinaryFrame() && isEnabledFor(site.caller) && endsItsTailCalls(site.callee);
    };
    // A cell next to a boxed value is boxed with it.
    auto returnSameBits = [](Representation first, Representation second) {
        return first == second || (!isCellOrBoxedValue(first) && haveSameRegisterRepresentation(first, second));
    };
    auto boxMixedClasses = [&] {
        unsigned callByValue = count;
        ConventionResultClasses classes(count + 1);
        for (TailCallSite& site : sites) {
            if (site.requirement != TailCallRequirement::Required)
                continue;
            // An integer where the caller returns a double: a common form would box both, and everything else they jump to. The caller calls, converts and returns instead.
            if (mayKeepFrameToConvertNumber(site)) {
                site.requirement = TailCallRequirement::Allowed;
                continue;
            }
            classes.unify(site.caller.index(), site.calleeKind == TailCalleeKind::Unknown ? callByValue : site.callee.index());
        }
        // A call through the method table enters the lookup or any entrypoint of its selector, and reads the result from where the lookup returns it.
        for (uint32_t index : findIndicesInParallel(loops, count, [&](unsigned index) { return program.functions[index].hasBody() && program.functions[index].role == FunctionRole::TableEntrypoint; })) {
            if (FunctionID lookup = inputs.derivedFunctions->find(tableLookupKey(selectorOfTableFunction(program.functions[index]))))
                classes.unify(index, lookup.index());
        }

        Vector<uint32_t> roots(count + 1);
        for (unsigned index = 0; index <= count; ++index)
            roots[index] = classes.find(index);

        BitVector isMixed(count + 1);
        isMixed.quickSet(roots[callByValue]);
        for (unsigned index = 0; index < count; ++index) {
            FunctionID id(index);
            FunctionID root(roots[index]);
            if (root != id && root.index() != callByValue && (!returnSameBits(resultOf(id), resultOf(root)) || returnsNothing(id) != returnsNothing(root)))
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
            results[index] = locationOfOnlyResult(id);
    });
    times.didFinish("locateResults"_s);
    boxMixedClasses();
    times.didFinish("boxMixedClasses"_s);
    boxTailCallees();
    times.didFinish("boxTailCallees"_s);
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
        case TailCalleeKind::Table:
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
        sites.appendVector(collectInParallel<TailCallSite>(loops, editedCallers.size(), [&](unsigned position, Vector<TailCallSite>& found) {
            collectTailCallSitesOf(program, editedCallers[position], found);
        }));
        std::ranges::inplace_merge(sites, sites.begin() + numberOfUneditedSites, { }, &TailCallSite::caller);
    };
    demoteMismatchedTailCalls();
    times.didFinish("demoteMismatchedTailCalls"_s);

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

        // All threads read the callers. Their findings are recorded here, in site order.
        struct ReservingSite {
            TailCallEdge edge;
            std::optional<uint32_t> numberOfWords;
        };
        Vector<TailCallEdge> edges;
        for (const ReservingSite& found : collectInParallel<ReservingSite>(loops, sites.size(), [&](unsigned index, Vector<ReservingSite>& found) {
            const TailCallSite& site = sites[index];
            if (!mayReserve(site.caller))
                return;
            if (site.calleeKind != TailCalleeKind::Unknown) {
                found.append({ { site.callee, site.caller }, std::nullopt });
                return;
            }
            const Function& caller = program.at(site.caller);
            const Value& call = caller.at(caller.at(site.block).terminator.callOperand());
            if (call.opcode != Opcode::Call)
                return;
            size_t numberOfArguments = call.argumentsOperands().size();
            if (numberOfArguments > numberOfArgumentGPRs)
                found.append({ { site.callee, site.caller }, static_cast<uint32_t>(numberOfArguments - numberOfArgumentGPRs) });
        })) {
            if (found.numberOfWords)
                raise(found.edge.caller, *found.numberOfWords);
            else
                edges.append(found.edge);
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
    times.didFinish("reserveStackWords"_s);

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
    times.didFinish("chooseSecondaryEntries"_s);

    Vector<uint32_t> functionsReadingBytes = findIndicesInParallel(loops, count, [&](unsigned index) {
        FunctionID id(index);
        constexpr OpcodeSet reads { Opcode::GetTypedArrayElement, Opcode::CallBuiltin };
        return recordFactsOf(program, id).hasBody && anyValueListedOrNot(program, id, reads, [&](ValueID value, const Value& read) {
            return readsDoubleOutOfBytesBeforeSelection(program, read) && program.typeOf(program.functions[index], value).mayBeNaN();
        });
    });
    if (!functionsReadingBytes.isEmpty()) {
        ScopedLambda<bool(FunctionID)> isEnabled(isEnabledFor);
        ImpureDoubleConventions(program, inputs, build, tables, results.span(), isEnabled).choose(functionsReadingBytes.span(), tables);
    }
    times.didFinish("chooseImpureDoubles"_s);
    tables.parametersLetGo = findParametersLetGo(program, inputs.callGraph, loops);
    times.didFinish("findParametersLetGo"_s);
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
    case FunctionRole::ChecksThatGiveUp:
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
    for (FunctionID target : entrypointsOfTableCall(context.program, value)) {
        auto& callee = context.program.at(target);
        hasUncheckedCallee |= !callee.hasBody() || isCandidate.get(target.index()) || callee.creator.is<CreatorRecord::Entrypoint>();
    }
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
    // The call graph keeps a long set of callees once for all its callers. What a caller does for a callee holds for every caller, so the walk goes through such a set once, not once for each caller.
    struct LongSet {
        Vector<uint32_t> callees;
        // Each callee in front of this index is done, or a caller has already found it in progress.
        uint32_t firstToVisit { 0 };
        // By the height that a caller counts for a callee. Callees that count for nothing are at 0.
        std::array<uint32_t, maxNestedFunctionsWithoutStackCheck + 1> numberOfCalleesByHeight { };
        bool everyCalleeHasCheck { false };
    };
    Vector<LongSet> longSets(callGraph.numberOfLongSets());
    forEachIndexInParallel(build.parallelLoops(), longSets.size(), [&](unsigned set) {
        for (FunctionID callee : callGraph.membersOfLongSet(set)) {
            if (isCandidate.get(callee.index()))
                longSets[set].callees.append(callee.index());
        }
        longSets[set].numberOfCalleesByHeight[0] = longSets[set].callees.size();
    });
    Vector<Vector<uint32_t, 4>> ownCallees(count);
    forEachIndexInParallel(build.parallelLoops(), count, [&](unsigned i) {
        if (!isCandidate.get(i))
            return;
        for (FunctionID callee : callGraph.ownCalleesOf(FunctionID(i))) {
            if (isCandidate.get(callee.index()))
                ownCallees[i].append(callee.index());
        }
        bool hasCallee = !ownCallees[i].isEmpty() || std::ranges::any_of(callGraph.longSetsOf(FunctionID(i)), [&](uint32_t set) { return !longSets[set].callees.isEmpty(); });
        if (!hasCallee && maxNested) {
            state[i] = VisitState::Done;
            height[i] = 1;
        }
    });

    Vector<uint8_t> heightCounted(FillWith { }, longSets.isEmpty() ? 0 : count, static_cast<uint8_t>(0));
    // Follows every change to the state, the height or the candidacy of a function.
    auto didChange = [&](uint32_t function) {
        if (heightCounted.isEmpty())
            return;
        uint8_t counted = !isCandidate.get(function) ? 0 : state[function] == VisitState::InProgress ? 1 : static_cast<uint8_t>(height[function]);
        uint8_t before = std::exchange(heightCounted[function], counted);
        if (before == counted)
            return;
        for (uint32_t set : callGraph.longSetsWith(FunctionID(function))) {
            --longSets[set].numberOfCalleesByHeight[before];
            ++longSets[set].numberOfCalleesByHeight[counted];
        }
    };
    auto requireCheck = [&](uint32_t function) {
        isCandidate.clear(function);
        didChange(function);
    };
    for (uint32_t i = 0; i < heightCounted.size(); ++i)
        didChange(i);

    struct Visit {
        uint32_t function;
        uint32_t nextOwnCallee;
        uint32_t firstPositionInLongSets;
    };
    Vector<Visit> inProgress;
    Vector<uint32_t> positionsInLongSets;
    auto beginVisit = [&](uint32_t function) {
        state[function] = VisitState::InProgress;
        didChange(function);
        inProgress.append({ function, 0, static_cast<uint32_t>(positionsInLongSets.size()) });
        positionsInLongSets.grow(positionsInLongSets.size() + callGraph.longSetsOf(FunctionID(function)).size());
        std::ranges::fill(positionsInLongSets.mutableSpan().subspan(inProgress.last().firstPositionInLongSets), 0u);
    };
    // The callees of the function on top, its own and those of its long sets, in ascending order and each once.
    constexpr uint32_t noCallee = UINT32_MAX;
    auto takeNextCallee = [&]() -> uint32_t {
        Visit& visit = inProgress.last();
        std::span<const uint32_t> own = ownCallees[visit.function].span();
        std::span<const uint32_t> sets = callGraph.longSetsOf(FunctionID(visit.function));
        std::span<uint32_t> positions = positionsInLongSets.mutableSpan().subspan(visit.firstPositionInLongSets);
        uint32_t least = visit.nextOwnCallee < own.size() ? own[visit.nextOwnCallee] : noCallee;
        for (size_t index = 0; index < sets.size(); ++index) {
            LongSet& set = longSets[sets[index]];
            positions[index] = set.firstToVisit = std::max(positions[index], set.firstToVisit);
            if (positions[index] < set.callees.size())
                least = std::min(least, set.callees[positions[index]]);
        }
        if (least == noCallee)
            return noCallee;
        if (visit.nextOwnCallee < own.size() && own[visit.nextOwnCallee] == least)
            ++visit.nextOwnCallee;
        for (size_t index = 0; index < sets.size(); ++index) {
            const LongSet& set = longSets[sets[index]];
            if (positions[index] < set.callees.size() && set.callees[positions[index]] == least)
                ++positions[index];
        }
        return least;
    };
    for (unsigned root = 0; root < count; ++root) {
        if (!isCandidate.get(root) || state[root] != VisitState::NotSeen)
            continue;
        beginVisit(root);
        while (!inProgress.isEmpty()) {
            uint32_t function = inProgress.last().function;
            if (uint32_t callee = takeNextCallee(); callee != noCallee) {
                if (state[callee] == VisitState::InProgress) {
                    if (!isEntrance.get(callee))
                        requireCheck(callee);
                    else if (!isEntranceInCycle.quickSet(callee)) {
                        for (uint32_t entered : ownCallees[callee])
                            requireCheck(entered);
                        numberOfFunctionsBelowEntrancesVisited += ownCallees[callee].size();
                        for (uint32_t set : callGraph.longSetsOf(FunctionID(callee))) {
                            if (std::exchange(longSets[set].everyCalleeHasCheck, true))
                                continue;
                            for (uint32_t entered : longSets[set].callees)
                                requireCheck(entered);
                            numberOfFunctionsBelowEntrancesVisited += longSets[set].callees.size();
                        }
                    }
                } else if (state[callee] == VisitState::NotSeen)
                    beginVisit(callee);
                continue;
            }
            unsigned below = 0;
            for (uint32_t callee : ownCallees[function]) {
                // What is still without a check while it is being visited is an entrance in a cycle: all that it enters checks, so it is one function and nothing below it.
                if (isCandidate.get(callee))
                    below = std::max<unsigned>(below, state[callee] == VisitState::InProgress ? 1 : height[callee]);
            }
            for (uint32_t set : callGraph.longSetsOf(FunctionID(function))) {
                for (unsigned counted = maxNestedFunctionsWithoutStackCheck; counted > below; --counted) {
                    if (longSets[set].numberOfCalleesByHeight[counted])
                        below = counted;
                }
            }
            if (isCandidate.get(function) && below >= maxNested && isEntrance.get(function)) {
                below = 0;
                auto requireCheckOfHighCallee = [&](uint32_t callee) {
                    if (!isCandidate.get(callee))
                        return;
                    if (height[callee] >= maxNested)
                        requireCheck(callee);
                    else
                        below = std::max<unsigned>(below, height[callee]);
                };
                for (uint32_t callee : ownCallees[function])
                    requireCheckOfHighCallee(callee);
                for (uint32_t set : callGraph.longSetsOf(FunctionID(function))) {
                    if (longSets[set].everyCalleeHasCheck)
                        continue;
                    for (uint32_t callee : longSets[set].callees)
                        requireCheckOfHighCallee(callee);
                }
            }
            if (isCandidate.get(function) && below >= maxNested) {
                build.gaveUp(LimitID::UncheckedStackBytes, Subject { .function = FunctionID(function) });
                isCandidate.clear(function);
            }
            height[function] = isCandidate.get(function) ? static_cast<uint16_t>(below + 1) : 0;
            state[function] = VisitState::Done;
            didChange(function);
            positionsInLongSets.shrink(inProgress.last().firstPositionInLongSets);
            inProgress.removeLast();
        }
    }
    build.destroyLater(WTF::move(ownCallees));
    build.destroyLater(WTF::move(longSets));

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
    unknownSummaries.keepAnswersForCalleeLists();
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
