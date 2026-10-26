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
#include "AOTEffectSummaries.h"
#include "AOTFunctionEditing.h"
#include "AOTIRBuilder.h"
#include "AOTParameterUses.h"
#include "AOTPassRunner.h"
#include "AOTResolveCallsDirectCall.h"
#include "AOTThreads.h"
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

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
