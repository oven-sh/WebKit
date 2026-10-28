/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTProgramPasses.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTCallGraph.h"
#include "AOTChooseCallingConventions.h"
#include "AOTEffectSummaries.h"
#include "AOTFunctionAnalyses.h"
#include "AOTFlowApply.h"
#include "AOTFunctionEditing.h"
#include "AOTIRBuilder.h"
#include "AOTMethodTableEntrypoints.h"
#include "AOTParameterUses.h"
#include "AOTPassRunner.h"
#include "AOTResolveCallsDirectCall.h"
#include "AOTResultRules.h"
#include "AOTThreads.h"
#include "AOTTypePropagation.h"
#include "AOTValueSubstitution.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

static bool countsEntryParameters(CreatorRecord::Kind kind)
{
    switch (kind) {
    case CreatorRecord::Kind::Entrypoint:
    case CreatorRecord::Kind::TableLookup:
    case CreatorRecord::Kind::Coroutine:
    case CreatorRecord::Kind::BoundFunction:
    case CreatorRecord::Kind::ResultWrapper:
    case CreatorRecord::Kind::ResultWorker:
    case CreatorRecord::Kind::ParameterWrapper:
    case CreatorRecord::Kind::EntryChecks:
    case CreatorRecord::Kind::StubBody:
        return true;
    case CreatorRecord::Kind::None:
    case CreatorRecord::Kind::Version:
    case CreatorRecord::Kind::Serializer:
    case CreatorRecord::Kind::Parser:
    case CreatorRecord::Kind::ChainStep:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isRemovable(const Function& function, EntryParameter kind)
{
    switch (kind) {
    case EntryParameter::Capture:
    case EntryParameter::This:
    case EntryParameter::ArgumentCount:
    case EntryParameter::Parameter:
    case EntryParameter::HiddenRest:
        return true;
    case EntryParameter::Callee:
        return !function.has(FunctionFlags::IsGenerator);
    case EntryParameter::NewTarget:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template<typename List>
static void removeAtPositions(List& list, const BitVector& removed)
{
    size_t newSize = 0;
    for (size_t index = 0; index < list.size(); ++index) {
        if (!removed.get(index))
            list[newSize++] = list[index];
    }
    list.shrink(newSize);
}

static bool canEditParameters(const Program& program, const CallGraph& callGraph, FunctionID id)
{
    if (!callGraph.numberOfDirectCallSites(id))
        return false;
    RecordFacts facts = recordFactsOf(program, id);
    if (!facts.hasBody || !facts.has(FunctionCapabilities::MayChangeSignature) || !facts.has(FunctionCapabilities::MayBeCloned) || !facts.hasOrdinaryFrame())
        return false;
    auto& function = program.at(id);
    return !function.constructedClass && !function.isUnsplitCoroutine() && !function.isModuleBody() && !countsEntryParameters(function.creator.kind());
}

static Vector<BitVector> findUnusedParameters(const Program& program, const CallGraph& callGraph, BuildContext& build)
{
    unsigned count = program.functions.size();
    Vector<BitVector> unused(count);
    Vector<Vector<ForwardedParameter>> forwarded(count);
    forEachIndexInParallel(build.parallelLoops(), count, [&](unsigned index) {
        FunctionID id(index);
        if (!canEditParameters(program, callGraph, id) || !build.isEnabled(PassID::RemoveUnusedParameters, id))
            return;
        auto& function = program.functions[index];
        unsigned numberOfEntryParameters = function.numberOfEntryParameters();
        BitVector removableEntryParameters(numberOfEntryParameters);
        for (unsigned i = 0; i < numberOfEntryParameters; ++i) {
            if (isRemovable(function, function.entryParameterKind(i)))
                removableEntryParameters.quickSet(i);
        }
        auto uses = classifyParameterUses(function, removableEntryParameters, &forwarded[index]);
        for (unsigned i = 0; i < uses.size(); ++i) {
            if (uses[i] != ParameterUse::Used)
                unused[index].set(i);
        }
    });
    Vector<unsigned> forwardingFunctions;
    for (unsigned index = 0; index < count; ++index) {
        if (!forwarded[index].isEmpty())
            forwardingFunctions.append(index);
    }
    for (bool changed = true; changed;) {
        changed = false;
        for (unsigned index : forwardingFunctions) {
            for (auto& forwardedParameter : forwarded[index]) {
                if (!unused[index].get(forwardedParameter.parameter) || unused[forwardedParameter.callee.index()].get(forwardedParameter.calleeParameter))
                    continue;
                unused[index].clear(forwardedParameter.parameter);
                changed = true;
            }
        }
    }
    return unused;
}

void removeEntryParameters(Function& function, const BitVector& removed)
{
    unsigned firstParameter = function.firstParameterIndex();
    Vector<std::pair<unsigned, EntryParameter>, 8> positions;
    for (size_t index : removed)
        positions.append({ static_cast<unsigned>(index), function.entryParameterKind(index) });

    for (size_t i = positions.size(); i--;) {
        auto [index, kind] = positions[i];
        switch (kind) {
        case EntryParameter::Capture:
            function.captures.removeAt(index);
            break;
        case EntryParameter::This:
            function.flags.remove(FunctionFlags::HasThis);
            function.thisType = DeclaredType(Proven::byPosition(TypeTable::anything));
            break;
        case EntryParameter::Parameter: {
            unsigned parameter = index - firstParameter;
            if (function.has(FunctionFlags::HasRestParameter) && parameter + 1 == function.parameters.size())
                function.flags.remove(FunctionFlags::HasRestParameter);
            function.parameters.removeAt(parameter);
            break;
        }
        case EntryParameter::Callee:
            function.flags.remove(FunctionFlags::TakesCallee);
            break;
        case EntryParameter::ArgumentCount:
            function.flags.remove(FunctionFlags::HasArgumentCount);
            break;
        case EntryParameter::HiddenRest:
            function.flags.remove(FunctionFlags::HasHiddenRest);
            break;
        case EntryParameter::NewTarget:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }

    if (function.hasBody()) {
        auto& entryParameters = function.edit(entryBlock, BlockEdit::Parameters).parameters;
        for (size_t index : removed)
            becomeUndefined(function, entryParameters[index]);
        removeAtPositions(entryParameters, removed);
    }
    removeAtPositions(function.derivation.parameterMap, removed);
}

static void becomeForwarder(const AnalysisContext& context, Function& original, FunctionID reduced, const BitVector& removed, TypeArena& types)
{
    Function forwarder;
    static_cast<FunctionRecord&>(forwarder) = original;
    forwarder.inlineFrames.clear();
    forwarder.capabilities.add(FunctionCapabilities::IsHiddenInStackTraces);
    forwarder.capabilities.remove({ FunctionCapabilities::MayChangeSignature, FunctionCapabilities::MayBeInlined });

    {
        IRBuilder builder(context, forwarder, types);
        builder.setOrigin({ original.position, 0 });
        BlockID entry = builder.addBlock();
        auto& entryParameters = original.at(entryBlock).parameters;
        Vector<ValueID, 8> arguments;
        for (size_t index = 0; index < entryParameters.size(); ++index) {
            auto& parameter = original.at(entryParameters[index]);
            ValueID copy = builder.addParameter(entry, parameter.type);
            forwarder.edit(copy, ValueEdit::Type).setExpected(parameter.expected());
            if (!removed.get(index))
                arguments.append(copy);
        }
        builder.moveToEndOf(entry);

        Value value = IRBuilder::make(Opcode::CallDirect);
        value.setFunction(reduced);
        value.setArgumentsOperands(arguments.span());
        ValueID call = builder.append(WTF::move(value));
        if (forwarder.at(call).type == TypeTable::never)
            builder.emitUnreachable();
        else if (forwarder.results.size() != 1)
            builder.emitReturn(call);
        else {
            Terminator terminator = IRBuilder::makeTerminator(TerminatorKind::ReturnCall, { call });
            terminator.setRequirement(tailCallRequirementOf(original));
            builder.emitTerminator(WTF::move(terminator));
        }
    }
    original = WTF::move(forwarder);
}

void removeUnusedParameters(ProgramEditor& editor, const OptimizationInputs& inputs, const PassRunner& runner)
{
    BuildContext& build = runner.build();
    const Program& program = editor.program();
    const ParallelLoops& loops = build.parallelLoops();

    Vector<BitVector> removed = findUnusedParameters(program, inputs.callGraph, build);
    Vector<FunctionID> candidates;
    uint64_t numberOfRemovedParameters = 0;
    for (size_t index = 0; index < removed.size(); ++index) {
        if (size_t count = removed[index].bitCount()) {
            candidates.append(FunctionID(index));
            numberOfRemovedParameters += count;
        }
    }
    if (candidates.isEmpty())
        return;

    Vector<Function> copies(candidates.size());
    forEachIndexInParallel(loops, candidates.size(), [&](unsigned index) {
        Function reduced = program.at(candidates[index]);
        if (!reduced.derivation.source) {
            reduced.derivation.parameterMap = Vector<ParameterOrigin>(reduced.numberOfEntryParameters(), [](size_t parameter) {
                return ParameterOrigin { static_cast<uint32_t>(parameter) };
            });
        }
        removeEntryParameters(reduced, removed[candidates[index].index()]);
        reduced.capabilities.add({ FunctionCapabilities::HasOnlyDirectCallers, FunctionCapabilities::IsNeverCalledByHost });
        copies[index] = WTF::move(reduced);
    });
    Vector<FunctionID> reducedFunctions(candidates.size(), [&](size_t index) {
        return editor.addFunctionLike(candidates[index], WTF::move(copies[index]));
    });
    auto reducedFunctionOf = [&](FunctionID function) -> FunctionID {
        if (function.index() >= removed.size() || removed[function.index()].isEmpty())
            return { };
        return reducedFunctions[std::ranges::lower_bound(candidates, function) - candidates.begin()];
    };

    RELEASE_ASSERT(reducedFunctions.last().index() - reducedFunctions.first().index() + 1 == reducedFunctions.size());
    Vector<FunctionSummary> summaries(candidates.size(), [&](size_t index) {
        return inputs.effects.of(candidates[index]).withoutParameters(removed[candidates[index].index()]);
    });
    EffectSummaries effects(inputs.effects, reducedFunctions.first(), summaries.span());
    OptimizationInputs inputsOfCallers = inputs.withEffects(effects);

    BitVector isCaller;
    for (FunctionID candidate : candidates) {
        for (FunctionID caller : inputs.callGraph.callersOf(candidate))
            isCaller.set(caller.index());
    }
    for (FunctionID candidate : candidates)
        isCaller.clear(candidate.index());
    Vector<FunctionID> callers;
    for (size_t index : isCaller) {
        if (build.isEnabled(PassID::RemoveUnusedParameters, FunctionID(index)))
            callers.append(FunctionID(index));
    }
    callers.appendVector(reducedFunctions);

    editFunctionsInParallel(editor, build, callers.span(), [&](unsigned, FunctionID id, Function& caller, TaskArenas& arenas) {
        bool didRetarget = false;
        for (auto& block : caller.blocks) {
            for (ValueID value : block.values) {
                auto& call = caller.edit(value);
                if (call.opcode != Opcode::CallDirect)
                    continue;
                FunctionID reduced = reducedFunctionOf(call.function());
                if (!reduced)
                    continue;
                RELEASE_ASSERT(call.opcode == Opcode::CallDirect);
                const BitVector& removedArguments = removed[call.function().index()];
                for (uint32_t position = call.numberOfVariadicOperands(); position--;) {
                    if (removedArguments.get(position))
                        call.removeVariadicOperand(position);
                }
                call.setFunction(reduced);
                didRetarget = true;
            }
        }
        if (didRetarget)
            simplifyEditedFunction(runner, program, inputsOfCallers, id, caller, arenas);
    });
    editFunctionsInParallel(editor, build, candidates.span(), [&](unsigned index, FunctionID id, Function& function, TaskArenas& arenas) {
        becomeForwarder({ build, program, &inputs.flow, &inputs.effects, id }, function, reducedFunctions[index], removed[id.index()], arenas.types);
    });

    // An inline frame holds a copy of the body, which the reduced function has. Stack traces hide the forwarder.
    auto isReduced = [&](const InlineFrame& frame) {
        return frame.callee && reducedFunctionOf(frame.callee);
    };
    forEachIndexInParallel(loops, program.functions.size(), [&](unsigned index) {
        if (std::ranges::none_of(program.functions[index].inlineFrames, isReduced))
            return;
        for (InlineFrame& frame : editor.at(FunctionID(index)).inlineFrames) {
            if (isReduced(frame))
                frame.callee = reducedFunctionOf(frame.callee);
        }
    });
    build.statistics().count(StatisticID::UnusedParametersRemoved, numberOfRemovedParameters);
}

namespace {

struct ResultCallSite {
    ValueID call;
    FunctionID callee;
    BitVector readResults;
};

struct ResultCallSiteList {
    FunctionID caller;
    Vector<ResultCallSite> sites;
};

} // anonymous namespace

static void setAllResults(BitVector& results, size_t resultCount)
{
    results.ensureSize(resultCount);
    for (size_t index = 0; index < resultCount; ++index)
        results.quickSet(index);
}

static std::optional<ValueID> returnedValueOf(const Terminator& terminator)
{
    switch (terminator.kind) {
    case TerminatorKind::Return:
        return terminator.valueOperand();
    case TerminatorKind::ReturnCall:
        return terminator.callOperand();
    case TerminatorKind::Jump:
    case TerminatorKind::Branch:
    case TerminatorKind::Switch:
    case TerminatorKind::SwitchString:
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
    case TerminatorKind::Unreachable:
    case TerminatorKind::SwitchOnShape:
    case TerminatorKind::TryCallInline:
    case TerminatorKind::TestAndBranch:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Vector<ResultCallSite> collectResultCallSites(const Program& program, const Function& caller)
{
    using SiteIndex = ID<struct SiteIndexTag>;
    Vector<ResultCallSite> sites;
    Vector<SiteIndex> siteByValue;
    for (auto& block : caller.blocks) {
        for (ValueID id : block.values) {
            auto& value = caller.at(id);
            if (value.opcode != Opcode::CallDirect || program.at(value.function()).results.size() <= 1)
                continue;
            if (sites.isEmpty())
                siteByValue.grow(caller.values.size());
            siteByValue[id.index()] = SiteIndex(sites.size());
            sites.append({ id, value.function(), { } });
        }
    }
    if (sites.isEmpty())
        return sites;

    auto siteOf = [&](ValueID producer) -> ResultCallSite* {
        SiteIndex site = siteByValue[producer.index()];
        return site ? &sites[site.index()] : nullptr;
    };
    for (auto& block : caller.blocks) {
        for (ValueID id : block.values) {
            auto& value = caller.at(id);
            if (value.opcode != Opcode::ResultAt)
                continue;
            if (auto* site = siteOf(value.resultsOperand()))
                site->readResults.set(value.index());
        }
        std::optional<ValueID> returned = returnedValueOf(block.terminator);
        if (!returned || !*returned)
            continue;
        if (auto* site = siteOf(*returned))
            setAllResults(site->readResults, program.at(site->callee).results.size());
    }
    return sites;
}

static bool returnsOnlyResults(const Function& function)
{
    return std::ranges::all_of(function.blocks, [&](auto& block) {
        std::optional<ValueID> returned = returnedValueOf(block.terminator);
        if (!returned)
            return true;
        if (!*returned)
            return false;
        auto& value = function.at(*returned);
        return value.opcode == Opcode::Results && value.numberOfVariadicOperands() == function.results.size();
    });
}

static bool canEditResults(const Program& program, const CallGraph& callGraph, FunctionID id)
{
    auto& function = program.at(id);
    if (!function.hasBody() || function.results.size() <= 1 || !function.mayChangeSignature() || callGraph.entrySet(id) != EntrySet::DirectCallers)
        return false;
    return returnsOnlyResults(function) && std::ranges::none_of(function.blocks, [&](auto& block) {
        return std::ranges::any_of(block.values, [&](ValueID read) {
            auto& value = function.at(read);
            return value.opcode == Opcode::ResultAt && function.at(value.resultsOperand()).opcode == Opcode::Results;
        });
    });
}

static void retainResults(Function& function, const BitVector& retained)
{
    ASSERT(returnsOnlyResults(function));
    size_t retainedCount = retained.bitCount();
    RELEASE_ASSERT(retainedCount);
    if (retainedCount == function.results.size())
        return;

    BitVector returnedValues(function.values.size());
    for (auto& block : function.blocks) {
        if (block.terminator.kind == TerminatorKind::Return)
            returnedValues.quickSet(block.terminator.valueOperand().index());
    }
    if (retainedCount > 1) {
        for (size_t returned : returnedValues) {
            Value& results = function.edit(ValueID(returned));
            Vector<ValueID, 8> retainedOperands;
            for (size_t index : retained)
                retainedOperands.append(results.valuesOperands()[index]);
            results.setValuesOperands(retainedOperands.span());
        }
    } else {
        size_t retainedIndex = *retained.begin();
        for (auto& block : function.editAllBlocks()) {
            auto& terminator = block.terminator;
            if (terminator.kind == TerminatorKind::Return)
                terminator.setValueOperand(function.at(terminator.valueOperand()).valuesOperands()[retainedIndex]);
            block.values.removeAllMatching([&](ValueID id) {
                return returnedValues.quickGet(id.index());
            });
        }
    }

    size_t newSize = 0;
    for (size_t index : retained)
        function.results[newSize++] = function.results[index];
    function.results.shrink(newSize);
}

void removeUnusedResults(ProgramEditor& editor, const OptimizationInputs& inputs, const PassRunner& runner)
{
    BuildContext& build = runner.build();
    const Program& program = editor.program();
    const ParallelLoops& loops = build.parallelLoops();
    unsigned count = program.functions.size();
    auto isEnabledFor = [&](FunctionID id) {
        return build.isEnabled(PassID::RemoveUnusedResults, findFunctionAsRead(&program, id));
    };

    // The callers that call some function with a body and several results. A function without a body keeps its results, whoever reads them.
    BitVector callsSuchFunction(count);
    for (uint32_t index : findIndicesInParallel(loops, count, [&](unsigned index) {
        RecordFacts callee = recordFactsOf(program, FunctionID(index));
        return callee.hasBody && callee.numberOfResults > 1;
    })) {
        for (FunctionID caller : inputs.callGraph.callersOf(FunctionID(index)))
            callsSuchFunction.quickSet(caller.index());
    }
    Vector<ResultCallSiteList> callers = collectInParallel<ResultCallSiteList>(loops, count, [&](unsigned index, Vector<ResultCallSiteList>& lists) {
        if (!callsSuchFunction.quickGet(index))
            return;
        auto sites = collectResultCallSites(program, program.functions[index]);
        if (!sites.isEmpty())
            lists.append({ FunctionID(index), WTF::move(sites) });
    });

    Vector<BitVector> usedResults(count);
    for (auto& list : callers) {
        bool canRetargetCaller = isEnabledFor(list.caller);
        for (auto& site : list.sites) {
            auto& used = usedResults[site.callee.index()];
            if (canRetargetCaller)
                used.merge(site.readResults);
            else
                setAllResults(used, program.at(site.callee).results.size());
        }
    }

    Vector<TypeID> singleResultTypes(count);
    BitVector isCandidate(count);
    Vector<FunctionID> candidates;
    uint64_t numberOfRemovedResults = 0;
    // Few functions have a result that nobody reads: all threads look for them.
    for (uint32_t index : findIndicesInParallel(loops, count, [&](unsigned index) {
        FunctionID id(index);
        if (usedResults[index].bitCount() >= program.at(id).results.size())
            return false;
        return canEditResults(program, inputs.callGraph, id) && isEnabledFor(id);
    })) {
        FunctionID id(index);
        auto& results = program.at(id).results;
        auto& used = usedResults[index];
        if (used.isEmpty())
            used.set(0);
        if (used.bitCount() == 1)
            singleResultTypes[index] = program.provenTypeOf(results[*used.begin()]);
        numberOfRemovedResults += results.size() - used.bitCount();
        isCandidate.quickSet(index);
        candidates.append(id);
    }
    if (candidates.isEmpty())
        return;

    forEachIndexInParallel(loops, callers.size(), [&](unsigned index) {
        auto isRetargeted = [&](const ResultCallSite& site) {
            return isCandidate.get(site.callee.index());
        };
        const ResultCallSiteList& list = callers[index];
        if (std::ranges::none_of(list.sites, isRetargeted))
            return;

        Function& caller = editor.at(list.caller);
        Vector<FunctionID> retargetedCalleeOf(caller.values.size());
        for (auto& site : list.sites) {
            if (!isRetargeted(site))
                continue;
            retargetedCalleeOf[site.call.index()] = site.callee;
            auto& call = caller.edit(site.call);
            if (TypeID type = singleResultTypes[site.callee.index()])
                call.type = type;
        }

        std::optional<ValueSubstitution> substitution;
        for (auto& block : caller.blocks) {
            for (ValueID id : block.values) {
                auto& read = caller.edit(id);
                if (read.opcode != Opcode::ResultAt)
                    continue;
                FunctionID callee = retargetedCalleeOf[read.resultsOperand().index()];
                if (!callee)
                    continue;
                if (!singleResultTypes[callee.index()]) {
                    uint32_t numberOfRetainedResultsBelow = 0;
                    for (size_t retained : usedResults[callee.index()]) {
                        if (retained >= read.index())
                            break;
                        ++numberOfRetainedResultsBelow;
                    }
                    read.setIndex(numberOfRetainedResultsBelow);
                    continue;
                }
                auto& call = caller.edit(read.resultsOperand());
                if (!call.expected().predicted())
                    call.setExpected(read.expected());
                if (!substitution)
                    substitution.emplace(caller);
                substitution->replace(id, read.resultsOperand());
            }
        }
        if (!substitution)
            return;
        for (auto& block : caller.editAllBlocks()) {
            block.values.removeAllMatching([&](ValueID id) {
                return substitution->resolve(id) != id;
            });
        }
        substitution->apply(caller, nullptr);
    });
    forEachIndexInParallel(loops, candidates.size(), [&](unsigned index) {
        retainResults(editor.at(candidates[index]), usedResults[candidates[index].index()]);
    });
    editFunctionsInParallel(editor, build, candidates.span(), [&](unsigned, FunctionID id, Function& function, TaskArenas& arenas) {
        simplifyEditedFunction(runner, program, inputs, id, function, arenas);
    });
    build.statistics().count(StatisticID::UnusedResultsRemoved, numberOfRemovedResults);
}

static bool hasResultThatMayBeProven(const Program& program, const Function& function)
{
    return function.hasBody() && !function.isModuleBody() && function.results.size() == 1 && !function.isUnsplitCoroutine()
        && function.result().isExpected() && program.at(function.result().written()).isJavaScriptValue();
}

// By the types that the values have in the body as it stands: its tests and the code for what fails them are part of it.
static bool returnsOnlyWhatIsExpected(const Program& program, const Function& function)
{
    const Type& expected = program.at(function.result().expected().predicted());
    return std::ranges::all_of(function.blocks, [&](const Block& block) {
        std::optional<ValueID> returned = returnedValueOf(block.terminator);
        if (!returned)
            return true;
        return (*returned ? program.typeOf(function, *returned) : program.at(TypeTable::undefined)).isSubsetOf(expected, program);
    });
}

namespace {

// Numbers, booleans, undefined and null: what a register holds without a box.
struct ReturnedNumbers {
    void add(const ReturnedNumbers& other)
    {
        primitives |= other.primitives;
        integers = integers.unionWith(other.integers);
        hasOtherValues |= other.hasOtherValues;
    }
    void add(const Type& type)
    {
        if (!type.hasOnlyPrimitives(Primitive::NumberLike)) {
            hasOtherValues = true;
            return;
        }
        primitives |= type.primitives;
        integers = integers.unionWith(type.integers);
    }
    Type type() const
    {
        Type type = Type::fromPrimitives(primitives);
        type.integers = integers;
        type.canonicalize();
        return type;
    }

    friend bool operator==(const ReturnedNumbers&, const ReturnedNumbers&) = default;

    uint16_t primitives { 0 };
    Range integers { Range::empty() };
    bool hasOtherValues { false };
};

struct ReturnsOfFunction {
    FunctionID function;
    ReturnedNumbers values;
    // Whose result it returns as it gets it.
    Vector<FunctionID, 2> handedOn;
    bool hasLoop { false };
};

struct DirectTailCall {
    friend auto operator<=>(const DirectTailCall&, const DirectTailCall&) = default;

    FunctionID callee;
    FunctionID caller;
};

// A call through the method table enters the lookup of its selector or one of the entrypoints: they return in one place.
struct TableFunctions {
    struct Lookup {
        FunctionID function;
        // What flow says at every call under the selector about the result. Whatever the lookup finds and calls, it returns to one of those calls.
        ReturnedNumbers claimedAtCalls;
        Vector<FunctionID, 4> entrypoints;
    };

    Lookup* find(FunctionID lookup)
    {
        auto found = std::ranges::lower_bound(lookups, lookup, { }, &Lookup::function);
        return found != lookups.end() && found->function == lookup ? &*found : nullptr;
    }
    const Lookup* find(FunctionID lookup) const { return const_cast<TableFunctions*>(this)->find(lookup); }

    Vector<Lookup> lookups;
    // By the index of an entrypoint.
    Vector<FunctionID> lookupOfEntrypoint;
};

} // anonymous namespace

static Representation representationOfResult(const Program& program, const Function& function)
{
    return entryConventionOf(function) == EntryConvention::Direct ? declaredRepresentationOf(program, function.result()) : Representation::JSValue;
}

static bool isTableFunction(const Function& function)
{
    return function.role == FunctionRole::TableEntrypoint || function.role == FunctionRole::TableLookup;
}

static TableFunctions collectTableFunctions(const Program& program, BuildContext& build)
{
    const ParallelLoops& loops = build.parallelLoops();
    unsigned count = program.functions.size();
    using Claim = std::pair<FunctionID, ReturnedNumbers>;
    Vector<Claim> claims = collectInParallel<Claim>(loops, count, [&](unsigned caller, Vector<Claim>& found) {
        const Function& function = program.functions[caller];
        forEachListedValue(program, FunctionID(caller), { Opcode::CallTable }, [&](BlockID block, uint32_t index, ValueID id, const Value& call) {
            const BlockValues& values = function.at(block).values;
            const Value* next = index + 1 < values.size() ? &function.at(values[index + 1]) : nullptr;
            ReturnedNumbers claimed;
            if (next && next->opcode == Opcode::AssumeType && next->valueOperand() == id && !next->factOperand())
                claimed.add(program.at(next->provenType()));
            else
                claimed.hasOtherValues = true;
            found.append({ call.function(), claimed });
        });
    });
    TableFunctions tables;
    if (claims.isEmpty())
        return tables;

    AOT::sort(claims, [](const Claim& a, const Claim& b) { return a.first < b.first; });
    for (const Claim& claim : claims) {
        if (tables.lookups.isEmpty() || tables.lookups.last().function != claim.first)
            tables.lookups.append({ claim.first, { }, { } });
        tables.lookups.last().claimedAtCalls.add(claim.second);
    }
    struct Selected {
        MethodSelector selector;
        FunctionID function;
    };
    Vector<Selected> lookupsBySelector = tables.lookups.map([&](const TableFunctions::Lookup& lookup) { return Selected { selectorOfTableFunction(program.at(lookup.function)), lookup.function }; });
    AOT::sort(lookupsBySelector, [](const Selected& a, const Selected& b) { return a.selector < b.selector; });
    tables.lookupOfEntrypoint.grow(count);
    for (uint32_t index : findIndicesInParallel(loops, count, [&](unsigned index) { return program.functions[index].hasBody() && program.functions[index].role == FunctionRole::TableEntrypoint; })) {
        MethodSelector selector = selectorOfTableFunction(program.functions[index]);
        auto found = std::ranges::lower_bound(lookupsBySelector, selector, { }, &Selected::selector);
        if (found == lookupsBySelector.end() || found->selector != selector)
            continue;
        tables.lookupOfEntrypoint[index] = found->function;
        tables.find(found->function)->entrypoints.append(FunctionID(index));
    }
    return tables;
}

// Whoever calls it reads where the result lies from its record.
static bool mayReturnInAnotherRegister(const Program& program, const Function& function, const TableFunctions* tables = nullptr)
{
    if (!function.hasBody() || function.isModuleBody() || function.results.size() != 1 || function.isUnsplitCoroutine() || !function.hasOrdinaryFrame() || function.mayBeCalledByHost())
        return false;
    if (entryConventionOf(function) != EntryConvention::Direct || !program.at(function.result().written()).isJavaScriptValue())
        return false;
    if (function.role == FunctionRole::StubBody || (isTableFunction(function) && !tables))
        return false;
    Representation representation = representationOfResult(program, function);
    return representation == Representation::JSValue || isDoubleOrMarker(representation) || isInt64OrMarker(representation);
}

static ReturnsOfFunction collectReturnsOf(const Program& program, FunctionID id)
{
    const Function& function = program.at(id);
    ReturnsOfFunction returns { .function = id };
    forEachBlockEndingWith(program, id, { TerminatorKind::Return, TerminatorKind::ReturnCall }, [&](BlockID block) {
        ValueID returned = *returnedValueOf(function.at(block).terminator);
        if (!returned) {
            returns.values.add(program.at(TypeTable::undefined));
            return;
        }
        const Value& value = function.at(returned);
        if (value.opcode == Opcode::CallDirect && program.at(value.function()).results.size() == 1) {
            if (!returns.handedOn.contains(value.function()))
                returns.handedOn.append(value.function());
            return;
        }
        returns.values.add(program.typeOf(function, returned));
    });
    forEachValueListedOrNot(program, id, { Opcode::LoopHint }, [&](auto&&...) {
        returns.hasLoop = true;
    });
    return returns;
}

// A result lies where its type puts it, and its type is what the function returns: numbers that are all integers come back as an integer, a number or undefined comes back in a floating-point register.
// A function that hands on the result of another returns what that one does. A tail call that is a jump stays one: its two ends move together or not at all.
static Vector<std::pair<FunctionID, ReturnedNumbers>> findResultsToNarrow(const Program& program, BuildContext& build, std::span<const FunctionID> functions, const TableFunctions* tables)
{
    const ParallelLoops& loops = build.parallelLoops();
    unsigned count = program.functions.size();
    Vector<ReturnsOfFunction> returns = collectInParallel<ReturnsOfFunction>(loops, functions.size(), [&](unsigned index, Vector<ReturnsOfFunction>& found) {
        FunctionID id = functions[index];
        const Function& function = program.at(id);
        if (!mayReturnInAnotherRegister(program, function, tables) || !build.isEnabled(PassID::ProveResults, findFunctionAsRead(&program, id)))
            return;
        ReturnsOfFunction returnsOfFunction { .function = id };
        if (function.role == FunctionRole::TableLookup) {
            const TableFunctions::Lookup* lookup = tables->find(id);
            if (!lookup)
                return;
            returnsOfFunction.values = lookup->claimedAtCalls;
            returnsOfFunction.handedOn.appendVector(lookup->entrypoints);
        } else
            returnsOfFunction = collectReturnsOf(program, id);
        if (function.role == FunctionRole::TableEntrypoint) {
            FunctionID lookup = tables->lookupOfEntrypoint[id.index()];
            if (!lookup)
                return;
            returnsOfFunction.handedOn.append(lookup);
        }
        if (!returnsOfFunction.values.hasOtherValues)
            found.append(WTF::move(returnsOfFunction));
    });
    if (returns.isEmpty())
        return { };

    using Index = ID<struct ReturnsIndexTag>;
    Vector<Index> indexOf(count);
    for (unsigned index = 0; index < returns.size(); ++index)
        indexOf[returns[index].function.index()] = Index(index);
    // Whoever leaves by a call is gone from the stack when the callee throws. A call that ends its block for want of a result is no way out, so such a callee keeps its result.
    BitVector isLeftBy(count);
    for (FunctionID callee : collectInParallel<FunctionID>(loops, count, [&](unsigned caller, Vector<FunctionID>& found) {
        const Function& function = program.functions[caller];
        forEachBlockEndingWith(program, FunctionID(caller), { TerminatorKind::ReturnCall }, [&](BlockID block) {
            const Terminator& terminator = function.at(block).terminator;
            if (terminator.requirement() != TailCallRequirement::Required)
                return;
            const Value& call = function.at(terminator.callOperand());
            FunctionID callee = call.opcode == Opcode::CallDirect ? call.function() : call.opcode == Opcode::Call ? onlyCalleeOf(program, program.typeOf(function, call.calleeOperand())) : FunctionID();
            if (callee && indexOf[callee.index()])
                found.append(callee);
        });
    }))
        isLeftBy.quickSet(callee.index());
    Vector<DirectTailCall> tailCalls = collectInParallel<DirectTailCall>(loops, count, [&](unsigned caller, Vector<DirectTailCall>& found) {
        const Function& function = program.functions[caller];
        forEachBlockEndingWith(program, FunctionID(caller), { TerminatorKind::ReturnCall }, [&](BlockID block) {
            const Value& call = function.at(function.at(block).terminator.callOperand());
            // An entrypoint of the table whose method returns an integer where others return a fraction calls it and converts.
            if (call.opcode == Opcode::CallDirect && call.function().index() != caller && function.role != FunctionRole::TableEntrypoint && (indexOf[call.function().index()] || indexOf[caller]))
                found.append({ call.function(), FunctionID(caller) });
        });
    });

    auto asDeclared = [&](FunctionID id) {
        ReturnedNumbers declared;
        const DeclaredType& result = program.at(id).result();
        if (result.isExpected())
            declared.hasOtherValues = true;
        else
            declared.add(program.at(result.written()));
        return declared;
    };
    BitVector staysAsDeclared(returns.size());
    Vector<ReturnedNumbers> returned(returns.size());
    Vector<Representation> chosen(returns.size());
    auto representationBehind = [&](FunctionID id) {
        Index index = indexOf[id.index()];
        return index && !staysAsDeclared.quickGet(index.index()) ? chosen[index.index()] : representationOfResult(program, program.at(id));
    };
    for (bool isSettled = false; !isSettled;) {
        for (unsigned index = 0; index < returns.size(); ++index)
            returned[index] = returns[index].values;
        for (bool didGrow = true; didGrow;) {
            didGrow = false;
            for (unsigned index = 0; index < returns.size(); ++index) {
                if (staysAsDeclared.quickGet(index))
                    continue;
                ReturnedNumbers before = returned[index];
                for (FunctionID callee : returns[index].handedOn) {
                    Index calleeIndex = indexOf[callee.index()];
                    returned[index].add(calleeIndex && !staysAsDeclared.quickGet(calleeIndex.index()) ? returned[calleeIndex.index()] : asDeclared(callee));
                }
                didGrow |= before != returned[index];
            }
        }

        isSettled = true;
        auto keepAsDeclared = [&](FunctionID id) {
            Index index = indexOf[id.index()];
            if (index && !staysAsDeclared.quickSet(index.index()))
                isSettled = false;
        };
        for (unsigned index = 0; index < returns.size(); ++index) {
            if (staysAsDeclared.quickGet(index))
                continue;
            chosen[index] = returned[index].hasOtherValues ? Representation::JSValue : defaultRepresentationOf(returned[index].type());
            // A function that comes to an end and returns nowhere throws: it hands nothing back, and a call of it ends its block.
            bool neverReturns = !returned[index].hasOtherValues && returned[index].type().isNever() && !returns[index].hasLoop && returns[index].handedOn.isEmpty() && !isLeftBy.quickGet(returns[index].function.index())
                && !program.at(program.at(returns[index].function).result().written()).isNever();
            if (chosen[index] == Representation::JSValue || (chosen[index] == Representation::Void && !neverReturns) || chosen[index] == representationOfResult(program, program.at(returns[index].function)))
                keepAsDeclared(returns[index].function);
        }
        // A function that only callers with a boxed result enter by a jump boxes its result for them.
        auto isJump = [](Representation returned, Representation expected) {
            return expected == Representation::JSValue || haveSameRegisterRepresentation(returned, expected);
        };
        for (const DirectTailCall& tailCall : tailCalls) {
            bool wasJump = isJump(representationOfResult(program, program.at(tailCall.callee)), representationOfResult(program, program.at(tailCall.caller)));
            if (wasJump && !isJump(representationBehind(tailCall.callee), representationBehind(tailCall.caller))) {
                keepAsDeclared(tailCall.caller);
                keepAsDeclared(tailCall.callee);
            }
        }
    }

    Vector<std::pair<FunctionID, ReturnedNumbers>> narrowed;
    for (unsigned index = 0; index < returns.size(); ++index) {
        if (!staysAsDeclared.quickGet(index))
            narrowed.append({ returns[index].function, returned[index] });
    }
    return narrowed;
}

// The program says what the function returns. A way out with something else is the one that the program takes for impossible: what falls out of a switch over every member of an enum.
static void expectReturnsOfWrittenType(const Program& program, Function& function)
{
    const Type& written = program.at(function.result().expected().predicted());
    for (uint32_t index = 0; index < function.blocks.size(); ++index) {
        const Block& block = function.blocks[index];
        if (block.terminator.kind != TerminatorKind::Return || block.frequency.predicted() == BlockFrequency::Rare || BlockID(index) == entryBlock)
            continue;
        ValueID returned = block.terminator.valueOperand();
        if (!(returned ? program.typeOf(function, returned) : program.at(TypeTable::undefined)).overlaps(written, program))
            function.edit(BlockID(index), BlockEdit::Rest).frequency = BlockFrequency::Rare;
    }
}

// Every call that enters the lookup is told by flow what comes back, so that holds of what the lookup returns.
static void assumeTypeOfReturnedValues(const AnalysisContext& context, Function& function, TypeArena& types, TypeID type)
{
    IRBuilder builder(context, function, types);
    for (uint32_t index = function.blocks.size(); index--;) {
        if (function.blocks[index].terminator.kind == TerminatorKind::ReturnCall)
            builder.demoteReturnCall(BlockID(index));
    }
    for (uint32_t index = 0; index < function.blocks.size(); ++index) {
        BlockID block(index);
        if (function.at(block).terminator.kind != TerminatorKind::Return)
            continue;
        ValueID returned = function.at(block).terminator.valueOperand();
        RELEASE_ASSERT(returned);
        builder.setOrigin(function.at(block).terminator.origin());
        if (context.build.validates())
            builder.insertBefore(pointBeforeTerminatorOf(function, block), makeValidateType(returned, type));
        ValueID assumed = builder.insertBefore(pointBeforeTerminatorOf(function, block), makeAssumeType(returned, Proven::byFlow(type, ProofScope::EveryVersion)));
        function.edit(block, BlockEdit::Terminator).terminator.setValueOperand(assumed);
    }
    builder.applyInsertions();
}

// The arm that made a result uncertain may be gone: a test in front of it was decided. Whoever calls the function then has nothing left to test, and its own result may be certain in turn.
void proveResults(ProgramEditor& editor, const OptimizationInputs& inputs, const PassRunner& runner)
{
    BuildContext& build = runner.build();
    const Program& program = editor.program();
    const ParallelLoops& loops = build.parallelLoops();
    unsigned count = program.functions.size();
    auto findProvenAmong = [&](std::span<const FunctionID> functions) {
        return collectInParallel<FunctionID>(loops, functions.size(), [&](unsigned index, Vector<FunctionID>& found) {
            FunctionID id = functions[index];
            if (hasResultThatMayBeProven(program, program.at(id)) && returnsOnlyWhatIsExpected(program, program.at(id)) && build.isEnabled(PassID::ProveResults, findFunctionAsRead(&program, id)))
                found.append(id);
        });
    };
    TableFunctions tableFunctions = collectTableFunctions(program, build);
    const TableFunctions* tables = tableFunctions.lookups.isEmpty() ? nullptr : &tableFunctions;
    auto isCandidateFunction = [&](const Function& function) {
        return hasResultThatMayBeProven(program, function) || mayReturnInAnotherRegister(program, function, tables);
    };
    // Establishes what is found and answers whose results have another type now.
    uint64_t numberOfProvenResults = 0;
    auto establishAmong = [&](std::span<const FunctionID> functions) {
        Vector<FunctionID> changed = findProvenAmong(functions);
        editFunctionsInParallel(editor, build, changed.span(), [&](unsigned, FunctionID, Function& function, TaskArenas&) {
            function.results[0].establish(Proven::byEveryArrival(function.results[0].expected()));
        });
        Vector<std::pair<FunctionID, ReturnedNumbers>> narrowed = findResultsToNarrow(program, build, functions, std::exchange(tables, nullptr));
        Vector<FunctionID> narrowedFunctions = narrowed.map([](auto& entry) { return entry.first; });
        editFunctionsInParallel(editor, build, narrowedFunctions.span(), [&](unsigned index, FunctionID id, Function& function, TaskArenas& arenas) {
            TypeID type = arenas.types.intern(narrowed[index].second.type());
            if (function.results[0].isExpected() && function.results[0].site)
                expectReturnsOfWrittenType(program, function);
            function.results[0].establish(Proven::byEveryArrival(ExpectedType(type)));
            if (function.role == FunctionRole::TableLookup)
                assumeTypeOfReturnedValues(AnalysisContext { build, program, &inputs.flow, &inputs.effects, id }, function, arenas.types, type);
        });
        for (FunctionID id : narrowedFunctions) {
            if (!changed.contains(id))
                changed.append(id);
        }
        numberOfProvenResults += changed.size();
        return changed;
    };

    Vector<FunctionID> candidates = collectInParallel<FunctionID>(loops, count, [&](unsigned index, Vector<FunctionID>& found) {
        if (isCandidateFunction(program.functions[index]))
            found.append(FunctionID(index));
    });
    Vector<FunctionID> proven = establishAmong(candidates.span());
    if (proven.isEmpty())
        return;

    // Whose values have the type of a candidate's result: a direct call, and a call of a value that can only be one function, whether or not anybody has listed the callee at the call.
    using TypedCall = std::pair<FunctionID, FunctionID>;
    BitVector isCandidate(count);
    for (FunctionID id : candidates)
        isCandidate.quickSet(id.index());
    auto appendTypedCallsOf = [&](FunctionID caller, Vector<TypedCall>& found) {
        const Function& function = program.at(caller);
        auto append = [&](FunctionID callee) {
            if (callee && isCandidate.quickGet(callee.index()) && (found.isEmpty() || found.last() != TypedCall { callee, caller }))
                found.append({ callee, caller });
        };
        forEachDirectCall(program, caller, [&](BlockID, ValueID, FunctionID callee) { append(callee); });
        forEachListedValue(program, caller, { Opcode::CallTable }, [&](BlockID, uint32_t, ValueID, const Value& call) { append(call.function()); });
        forEachListedValue(program, caller, { Opcode::Call }, [&](BlockID, uint32_t, ValueID, const Value& call) {
            append(onlyCalleeOf(program, program.typeOf(function, call.operand(OperandIndex::Call::callee))));
        });
    };
    Vector<TypedCall> typedCalls = collectInParallel<TypedCall>(loops, count, [&](unsigned index, Vector<TypedCall>& found) {
        if (program.functions[index].hasBody())
            appendTypedCallsOf(FunctionID(index), found);
    });
    sortAndRemoveDuplicates(loops, typedCalls);

    while (!proven.isEmpty()) {
        BitVector isCaller(count);
        Vector<FunctionID> callers;
        for (FunctionID id : proven) {
            for (auto first = std::ranges::lower_bound(typedCalls, TypedCall { id, FunctionID(0u) }); first != typedCalls.end() && first->first == id; ++first) {
                if (!isCaller.quickSet(first->second.index()))
                    callers.append(first->second);
            }
        }
        // Where the types cut no code and no test is decided with them there is nothing to clean up.
        editFunctionsInParallel(editor, build, callers.span(), [&](unsigned, FunctionID id, Function& function, TaskArenas& arenas) {
            FunctionAnalyses analyses(AnalysisContext { build, program, &inputs.flow, &inputs.effects, id }, function);
            TypePropagationOutcome typing = typeValuesFromNever(TypePropagationInputs { program, id, function, analyses, arenas.types, build });
            if (!typing.didChange())
                return;
            FunctionPassContext context { program, inputs, id, function, analyses, arenas.types, arenas.strings, build };
            if (runner.runFunctionPass(PassID::SimplifyControlFlow, context) == PassOutcome::Changed || typing.cutBlocks || typing.waitsToCutCode || typing.replacedUpcasts)
                simplifyEditedFunction(runner, program, inputs, id, function, arenas);
        });
        // With narrower types a value may have come to be one function only.
        size_t numberOfSortedCalls = typedCalls.size();
        for (FunctionID caller : callers)
            appendTypedCallsOf(caller, typedCalls);
        if (typedCalls.size() != numberOfSortedCalls)
            sortAndRemoveDuplicates(loops, typedCalls);

        callers.removeAllMatching([&](FunctionID id) { return !isCandidateFunction(program.at(id)); });
        proven = establishAmong(callers.span());
    }
    build.statistics().count(StatisticID::ResultsProvenByReturns, numberOfProvenResults);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
