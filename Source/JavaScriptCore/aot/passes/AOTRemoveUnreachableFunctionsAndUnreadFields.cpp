/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRemoveUnreachableFunctions.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTCallGraph.h"
#include "AOTEffectsOfOperands.h"
#include "AOTFlowResults.h"
#include "AOTFunctionEditing.h"
#include "AOTIRBuilder.h"
#include "AOTPassRunner.h"
#include "AOTPhaseTimes.h"
#include "AOTRemoveUnreadExports.h"
#include "AOTRemoveUnreadFields.h"
#include "AOTSchemaWalkers.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <algorithm>
#include <atomic>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <wtf/Atomics.h>
#include <wtf/BitVector.h>
#include <wtf/UnionFind.h>

namespace JSC { namespace AOT { namespace Typed {

BitVector findReachableFunctions(const Program& program, const CallGraph& callGraph, const BuildContext& build)
{
    const ParallelLoops& loops = build.parallelLoops();
    unsigned count = program.functions.size();

    // The entrypoints that have a body. They come in their own order, which the counting sort by target keeps.
    using EntrypointLink = std::pair<FunctionID, FunctionID>;
    Vector<EntrypointLink> links = collectInParallel<EntrypointLink>(loops, count, [&](unsigned index, Vector<EntrypointLink>& found) {
        const Function& function = program.functions[index];
        const auto* record = function.creator.tryAs<CreatorRecord::Entrypoint>();
        if (record && function.hasBody() && record->target.index() < count)
            found.append({ record->target, FunctionID(index) });
    });
    Vector<uint32_t> offsets(FillWith { }, count + 1, 0u);
    for (const EntrypointLink& link : links)
        ++offsets[link.first.index() + 1];
    std::partial_sum(offsets.begin(), offsets.end(), offsets.begin());
    Vector<uint32_t> cursors(offsets.subspan(0, count));
    Vector<FunctionID> entrypoints(links.size());
    for (const EntrypointLink& link : links)
        entrypoints[cursors[link.first.index()]++] = link.second;
    // Every reference to a closure asks for those of its function.
    auto entrypointsOf = [&](FunctionID target) -> std::span<const FunctionID> {
        size_t index = target.index();
        if (index + 1 >= offsets.size())
            return { };
        return entrypoints.subspan(offsets[index], offsets[index + 1] - offsets[index]);
    };

    // A byte a function, written by all threads. Of all threads that reach a function in one round, one lists it.
    std::unique_ptr<std::atomic<uint8_t>[]> isReached(new std::atomic<uint8_t>[count]());
    auto reach = [&](FunctionID function, Vector<FunctionID>& reachedBodies) {
        if (function.index() >= count)
            return;
        std::atomic<uint8_t>& flag = isReached[function.index()];
        if (!flag.load(std::memory_order_relaxed) && !flag.exchange(1, std::memory_order_relaxed) && hasBody(program, function))
            reachedBodies.append(function);
    };

    Vector<FunctionID> frontier;
    // The tables take the body of a module or of a function that is registered for the host as the sign that it can run.
    BitVector isCalledByHost = functionsCalledByHost(program, loops);
    for (FunctionID root : callGraph.roots()) {
        if (isCalledByHost.get(root.index()) || entrypointsOf(root).empty())
            reach(root, frontier);
    }
    for (unsigned index = 0; index < count; ++index) {
        if (!build.isEnabled(PassID::RemoveUnreachableFunctions, FunctionID(index)))
            reach(FunctionID(index), frontier);
    }

    while (!frontier.isEmpty()) {
        frontier = collectInParallel<FunctionID>(loops, frontier.size(), [&](unsigned position, Vector<FunctionID>& reachedBodies) {
            // An operation that may enter a function has the effects of that body, also where it enters through an entrypoint: a function without a body counts as unknown code.
            for (FunctionID callee : callGraph.calleesOf(frontier[position]))
                reach(callee, reachedBodies);
            forEachFunctionReference(program, frontier[position], [&](FunctionID target, FunctionReferenceKind kind) {
                switch (kind) {
                case FunctionReferenceKind::Call:
                case FunctionReferenceKind::Dispatch:
                    reach(target, reachedBodies);
                    return;
                case FunctionReferenceKind::Closure:
                case FunctionReferenceKind::Constructor:
                    break;
                }
                // Whoever has the closure of a function enters one of its entrypoints, so a closure alone does not keep a body that the entrypoints hold a copy of.
                if (entrypointsOf(target).empty())
                    reach(target, reachedBodies);
                for (FunctionID entrypoint : entrypointsOf(target))
                    reach(entrypoint, reachedBodies);
                // Asked about its instances, a bound function that captures no closure of its target has the run time make one.
                const auto* bound = program.at(target).creator.tryAs<CreatorRecord::BoundFunction>();
                if (!bound || bound->capturesTargetClosure)
                    return;
                for (FunctionID entrypoint : entrypointsOf(bound->target))
                    reach(entrypoint, reachedBodies);
            });
        });
    }

    BitVector result(count);
    for (unsigned index = 0; index < count; ++index) {
        if (isReached[index].load(std::memory_order_relaxed))
            result.quickSet(index);
    }
    return result;
}

void removeUnreachableFunctions(ProgramEditor& editor, const CallGraph& callGraph, BuildContext& build)
{
    if (callGraph.roots().empty())
        return;

    PhaseTimes times(build, PassID::RemoveUnreachableFunctions);
    const Program& program = editor.program();
    BitVector isReachable = findReachableFunctions(program, callGraph, build);
    times.didFinish("findReachableFunctions"_s);
    const ParallelLoops& loops = build.parallelLoops();
    Vector<uint32_t> unreachableFunctions = findIndicesInParallel(loops, program.functions.size(), [&](unsigned index) {
        return !isReachable.get(index) && hasBody(program, FunctionID(index));
    });
    forEachIndexInParallel(loops, unreachableFunctions.size(), [&](unsigned position) {
        editor.removeBody(FunctionID(unreachableFunctions[position]));
    });
    build.statistics().count(StatisticID::UnreachableFunctionsRemoved, unreachableFunctions.size());
    times.didFinish("remove bodies"_s);
}

// Every table names entrypoints, for closures and for the host alike.
static bool isEnteredOnlyByCalls(FunctionRole role)
{
    switch (role) {
    case FunctionRole::GenericEntrypoint:
    case FunctionRole::TableEntrypoint:
    case FunctionRole::TableLookup:
    case FunctionRole::CoroutineBody:
    case FunctionRole::StubBody:
    case FunctionRole::Model:
        return false;
    case FunctionRole::Source:
    case FunctionRole::Library:
    case FunctionRole::Signature:
    case FunctionRole::GenericVersion:
    case FunctionRole::AllocatedThisVersion:
    case FunctionRole::ConstructBody:
    case FunctionRole::CallSiteVersion:
    case FunctionRole::CalleeVersion:
    case FunctionRole::CoroutineRamp:
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
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

BitVector findBodiesWithoutCode(const Program& program, const BuildContext& build, std::span<const Vector<FunctionID>> calledFunctions)
{
    const ParallelLoops& loops = build.parallelLoops();
    unsigned count = program.functions.size();
    BitVector isCalledByHost = functionsCalledByHost(program, loops);
    BitVector hasNoCode(count);
    for (uint32_t index : findIndicesInParallel(loops, count, [&](unsigned index) {
        RecordFacts facts = recordFactsOf(program, FunctionID(index));
        return facts.hasBody && isEnteredOnlyByCalls(facts.role) && !isCalledByHost.get(index) && build.isEnabled(PassID::RemoveUnreachableFunctions, FunctionID(index));
    }))
        hasNoCode.quickSet(index);
    if (hasNoCode.isEmpty())
        return hasNoCode;

    Vector<FunctionID> frontier = collectInParallel<FunctionID>(loops, count, [&](unsigned index, Vector<FunctionID>& found) {
        if (!hasNoCode.quickGet(index) && hasBody(program, FunctionID(index)))
            found.append(FunctionID(index));
    });
    while (!frontier.isEmpty()) {
        Vector<FunctionID> called = collectInParallel<FunctionID>(loops, frontier.size(), [&](unsigned position, Vector<FunctionID>& found) {
            auto note = [&](FunctionID target) {
                if (hasNoCode.get(target.index()))
                    found.append(target);
            };
            if (calledFunctions.empty())
                forEachCalledFunction(program, frontier[position], note);
            else
                std::ranges::for_each(calledFunctions[frontier[position].index()], note);
        });
        frontier.shrink(0);
        for (FunctionID function : called) {
            if (hasNoCode.quickClear(function.index()))
                frontier.append(function);
        }
    }
    return hasNoCode;
}

static constexpr OperandUses keyedStoreUses { OperandUse::StoresByKey, OperandUse::AssignsFrom, OperandUse::DefinesName, OperandUse::TakesDescriptor };

static bool supportsRemoval(LayoutKind kind)
{
    switch (kind) {
    case LayoutKind::Record:
    case LayoutKind::Instance:
    case LayoutKind::Environment:
        return true;
    case LayoutKind::Container:
    case LayoutKind::Closure:
    case LayoutKind::CoroutineFrame:
    case LayoutKind::BuiltinResult:
    case LayoutKind::ModuleNamespace:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isUnread(const BuildContext& build, const Layout& layout, const Field& field)
{
    constexpr OptionSet<FieldFlags> excludedFlags { FieldFlags::IsInherited, FieldFlags::IsPrivate, FieldFlags::IsBrand, FieldFlags::IsHidden };
    if (field.flags.containsAny(excludedFlags) || field.form.contains(FieldForm::IsAccessor))
        return false;
    if (!layout.isEnvironment() && !field.key)
        return false;
    if (field.flow.location.writers != WriterState::AllKnown)
        return false;
    // It has no storage as it is, whatever flow says of the names of its stores. Only a candidate gets its storage back.
    if (hasNoAccesses(field.flow.location))
        return true;
    // A store that the scan cannot find would define the key at run time on an object without the field.
    if (field.flow.location.storeNaming != StoreNaming::EveryStoreNamesLocation)
        return false;
    switch (field.flow.location.reads) {
    case ReadState::OnlyCalled:
    case ReadState::NeverRead:
        return true;
    case ReadState::Unknown:
        return build.isSabotaged(Sabotage::RemovedFieldsHaveUnknownReaders);
    case ReadState::OnlyAccessed:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool mayHaveNamespaceObject(NamespaceUse use)
{
    switch (use) {
    case NamespaceUse::Unknown:
    case NamespaceUse::NamedReadsOnly:
        return true;
    case NamespaceUse::NeverCreated:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static constexpr bool isRemovableSlotStore(Opcode opcode)
{
    return accessClassOf(opcode) == AccessClass::Slot && accessOf(opcode) == Access::Store && resultRuleOf(opcode) == ResultRule::None;
}

static_assert(isRemovableSlotStore(Opcode::PutSlot) && isRemovableSlotStore(Opcode::PutSlotOrAdd));

static constexpr bool isSlotRead(Opcode opcode)
{
    return accessClassOf(opcode) == AccessClass::Slot && accessOf(opcode) == Access::Read;
}

static_assert(isSlotRead(Opcode::GetSlot) && isSlotRead(Opcode::GetDeferredClosureSlot));
static_assert(resultRuleOf(Opcode::PutEnvironmentField) == ResultRule::None);

static constexpr bool findsFieldByKey(Opcode opcode)
{
    return Value::opcodesWithReceivers.contains(opcode) && !Value::opcodesWithField.contains(opcode) && !Value::opcodesWithPlacedKeys.contains(opcode);
}

static bool callsBuiltin(EffectRule rule)
{
    switch (rule) {
    case EffectRule::Builtin:
    case EffectRule::Inline:
        return true;
    case EffectRule::Declared:
    case EffectRule::Operator:
    case EffectRule::Access:
    case EffectRule::Call:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static BitVector findUsedValues(const Function& function)
{
    BitVector isUsed(function.values.size());
    auto markUsed = [&](ValueID value) {
        isUsed.quickSet(value.index());
    };
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values)
            function.at(id).forEachChild(markUsed);
        block.forEachChildOutsideValues(markUsed);
    }
    return isUsed;
}

void removeUnreadFields(ProgramEditor& editor, const OptimizationInputs& inputs, const PassRunner& runner)
{
    const Program& program = editor.program();
    BuildContext& build = runner.build();
    const ParallelLoops& loops = build.parallelLoops();

    // A candidate is a field, by its place in the ascending list, or a variable, by its number behind the fields.
    Vector<FieldReference> fields;
    BitVector variables;
    bool areReceiverSetsEnabled = true;
    for (uint32_t function = 0; function < program.numberOfFunctionsAsRead() && areReceiverSetsEnabled; ++function)
        areReceiverSetsEnabled = build.isEnabled(FlowResultKind::ReceiverSets, function);
    for (uint32_t layoutIndex = 0; areReceiverSetsEnabled && layoutIndex < program.layouts.size(); ++layoutIndex) {
        const Layout& layout = program.layouts[layoutIndex];
        if (!supportsRemoval(layout.kind) || layout.flow.allocation == AllocationState::NeverAllocated)
            continue;
        for (uint32_t field = 0; field < layout.fields.size(); ++field) {
            if (isUnread(build, layout, layout.fields[field]))
                fields.append(FieldReference { LayoutID(layoutIndex), field });
        }
    }
    // Only GetModuleVariable reads a variable that no module, eval scope or class object names, so the scan sees every read of it.
    if (inputs.flow.hasResults()) {
        BitVector isReadOutsideBodies(program.variables.size());
        auto add = [&](VariableID variable) {
            if (variable)
                isReadOutsideBodies.quickSet(variable.index());
        };
        BitVector isReadByHost(program.modules.size());
        for (const Entry& entry : program.entries) {
            if (!entry.hostReads.isEmpty())
                isReadByHost.quickSet(entry.module.index());
        }
        for (unsigned index = 0; index < program.modules.size(); ++index) {
            const Module& module = program.modules[index];
            bool isEveryExportRead = isReadByHost.quickGet(index) || mayHaveNamespaceObject(inputs.flow.module(ModuleID(index)).namespaceUse);
            for (const ModuleExport& entry : module.exports) {
                if (entry.variable && (isEveryExportRead || program.at(entry.variable).flow.reads == ReadState::Unknown))
                    add(entry.variable);
            }
            for (const SyntheticExport& entry : module.syntheticExports)
                add(entry.variable);
            // The run time reads the variable that holds `module.exports` for whoever asks for the value of the module, takes its names from that value, or holds its module object.
            if (module.exportsVariable && program.at(module.exportsVariable).flow.reads == ReadState::Unknown)
                add(module.exportsVariable);
        }
        for (const EvalScope& scope : program.evalScopes) {
            for (const EvalBinding& binding : scope.bindings)
                add(binding.variable);
        }

        variables.ensureSize(program.variables.size());
        for (unsigned index = 0; index < program.variables.size(); ++index) {
            const Variable& variable = program.variables[index];
            if (!variable.owner && variable.flow.writers == WriterState::AllKnown && !isReadOutsideBodies.quickGet(index))
                variables.quickSet(index);
        }
    }
    if (fields.isEmpty() && variables.isEmpty())
        return;

    auto candidateOf = [&](LayoutID layout, uint32_t field) -> std::optional<uint32_t> {
        FieldReference declaration = declarationOf(program, { layout, field });
        auto found = std::ranges::lower_bound(fields, declaration);
        if (found == fields.end() || *found != declaration)
            return std::nullopt;
        return static_cast<uint32_t>(found - fields.begin());
    };
    auto candidateOfVariable = [&](size_t index) {
        return static_cast<uint32_t>(fields.size() + index);
    };
    Vector<uint8_t> liveCandidates(FillWith { }, fields.size() + variables.size(), static_cast<uint8_t>(0));
    auto isLive = [&](uint32_t candidate) -> bool {
        return WTF::atomicLoad(&liveCandidates[candidate], std::memory_order_relaxed);
    };
    auto markLive = [&](uint32_t candidate) {
        if (!isLive(candidate))
            WTF::atomicStore(&liveCandidates[candidate], static_cast<uint8_t>(1), std::memory_order_relaxed);
    };
    auto markFieldsLive = [&](LayoutID id) {
        size_t candidate = std::ranges::lower_bound(fields, id, { }, &FieldReference::layout) - fields.begin();
        for (; candidate < fields.size() && fields[candidate].layout == id; ++candidate)
            markLive(static_cast<uint32_t>(candidate));

        const Layout& layout = program.at(id);
        for (uint32_t field = 0; field < layout.fields.size(); ++field) {
            if (!layout.fields[field].flags.contains(FieldFlags::IsInherited))
                continue;
            if (auto inherited = candidateOf(id, field))
                markLive(*inherited);
        }
    };

    OpcodeSet opcodesStoringThroughOperands;
    OpcodeSet scannedOpcodes = Value::opcodesWithLayout | Value::opcodesWithVariable;
    for (Opcode opcode : allMembersOf<Opcode>) {
        bool mayStore = callsBuiltin(effectRuleOf(opcode));
        for (uint32_t operandIndex = 0, end = numberOfNamedOperandsOf(opcode) + hasVariadicOperand(opcode); operandIndex < end; ++operandIndex)
            mayStore |= operandUsesOf(opcode, operandIndex).containsAny(keyedStoreUses);
        if (mayStore)
            opcodesStoringThroughOperands.add(opcode);
        if (mayStore || findsFieldByKey(opcode))
            scannedOpcodes.add(opcode);
    }

    // The keys under which a lookup finds a candidate, or a field that was inherited from one.
    BitVector keysOfCandidates;
    Vector<PropertyKeyID> keysFound = collectInParallel<PropertyKeyID>(loops, program.layouts.size(), [&](unsigned index, Vector<PropertyKeyID>& keys) {
        const auto& fieldsOfLayout = program.layouts[index].fields;
        for (uint32_t field = 0; field < fieldsOfLayout.size(); ++field) {
            if (fieldsOfLayout[field].key && candidateOf(LayoutID(index), field))
                keys.append(fieldsOfLayout[field].key);
        }
    });
    for (PropertyKeyID key : keysFound)
        keysOfCandidates.set(key.index());

    for (const EvalScope& scope : program.evalScopes) {
        for (const EvalBinding& binding : scope.bindings) {
            if (!binding.layout || !binding.field)
                continue;
            if (auto candidate = candidateOf(binding.layout, *binding.field))
                markLive(*candidate);
        }
    }
    BitVector isLayoutOfData(program.layouts.size());
    for (const Data& data : program.data) {
        forEachLayout(data, [&](LayoutID layout) {
            if (!isLayoutOfData.quickSet(layout.index()))
                markFieldsLive(layout);
        });
    }
    for (uint32_t layoutIndex = 0; layoutIndex < program.layouts.size(); ++layoutIndex) {
        if (program.layouts[layoutIndex].flow.observedUses.contains(ObservedUse::ReachesTypedParse))
            markFieldsLive(LayoutID(layoutIndex));
    }

    struct RemovableAccess {
        BlockID block;
        ValueID access;
        Vector<uint32_t, 1> candidates;
    };
    using FunctionAccesses = std::pair<FunctionID, Vector<RemovableAccess>>;
    Vector<FunctionAccesses> functions = collectInParallel<FunctionAccesses>(loops, program.functions.size(), [&](unsigned index, Vector<FunctionAccesses>& collected) {
        FunctionID id(index);
        const Function& function = program.at(id);
        if (!mayListValueWith(program, id, scannedOpcodes) || !function.hasBody())
            return;
        bool canChangeFunction = build.isEnabled(PassID::RemoveUnreadFields, id);
        std::optional<BitVector> usedValues;
        Vector<RemovableAccess> accesses;
        forEachListedValue(program, id, scannedOpcodes, [&](BlockID block, uint32_t, ValueID valueID, const Value& value) {
            Opcode opcode = value.opcode;
            auto addAccess = [&](Vector<uint32_t, 1>&& candidates, bool isRemovable) {
                if (isRemovable && canChangeFunction)
                    accesses.append(RemovableAccess { block, valueID, WTF::move(candidates) });
                else
                    std::ranges::for_each(candidates, markLive);
            };

            // The read of a deferred closure fills the slot, so dead code elimination leaves it where nothing uses its value.
            auto isUnusedSlotRead = [&] {
                if (!isSlotRead(opcode))
                    return false;
                if (!usedValues)
                    usedValues = findUsedValues(function);
                return !usedValues->quickGet(valueID.index());
            };
            auto scanAccessByKey = [&] {
                std::span<const LayoutID> members;
                bool reachesOnlyCandidates = false;
                if (LayoutSetID stamp = value.receivers()) {
                    members = program.at(stamp).members.span();
                    reachesOnlyCandidates = program.at(stamp).isComplete;
                } else if (std::optional<uint8_t> receiver = receiverOf(opcode))
                    members = membersOf(program, program.typeOf(function, value.operand(*receiver)));

                PropertyKeyID key = Value::opcodesWithKey.contains(opcode) ? value.key() : PropertyKeyID();
                if (!key) {
                    std::ranges::for_each(members, markFieldsLive);
                    return;
                }
                // Most keys are the key of no candidate, and then no member has a candidate under the key.
                if (!keysOfCandidates.get(key.index()))
                    return;

                Vector<uint32_t, 1> found;
                for (LayoutID member : members) {
                    std::optional<uint32_t> candidate;
                    if (auto field = findField(program, member, key, BaseChain::Proven))
                        candidate = candidateOf(field->layout, field->field);
                    if (candidate)
                        found.append(*candidate);
                    else
                        reachesOnlyCandidates = false;
                }
                if (found.isEmpty())
                    return;
                AOT::sort(found);
                removeRepeatedElements(found);
                addAccess(WTF::move(found), reachesOnlyCandidates && (isRemovableSlotStore(opcode) || isUnusedSlotRead()));
            };

            if (LayoutID layout = Value::opcodesWithLayout.contains(opcode) ? value.layout() : LayoutID()) {
                if (resultRuleOf(opcode) == ResultRule::Allocation && opcode != Opcode::NewObject)
                    markFieldsLive(layout);
                auto addNamedField = [&](uint32_t field) {
                    if (auto candidate = candidateOf(layout, field))
                        addAccess({ *candidate }, opcode == Opcode::NewObject || opcode == Opcode::PutEnvironmentField);
                };
                for (const PayloadFieldInfo& field : payloadFieldsOf(opcode)) {
                    if (field.kind == PayloadKind::Field) {
                        if (auto namedField = decodePayloadWord<std::optional<uint32_t>>(value.payloadWordAt(field.firstWord)))
                            addNamedField(*namedField);
                        continue;
                    }
                    if (field.kind != PayloadKind::List || field.elementKind != PayloadKind::Field)
                        continue;
                    for (uint32_t namedField : value.payloadListFrom(field.firstWord))
                        addNamedField(namedField);
                }
            }
            if (Value::opcodesWithVariable.contains(opcode) && variables.get(value.variable().index()))
                addAccess({ candidateOfVariable(value.variable().index()) }, opcode == Opcode::PutModuleVariable);
            if (findsFieldByKey(opcode))
                scanAccessByKey();
            if (opcodesStoringThroughOperands.contains(opcode)) {
                value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                    std::span<const LayoutID> members = membersOf(program, program.typeOf(function, operand));
                    if (!members.empty() && summarizeOperand(EffectContext { program, inputs.effects }, function, value, operandIndex).uses.containsAny(keyedStoreUses))
                        std::ranges::for_each(members, markFieldsLive);
                });
            }
        });
        if (!accesses.isEmpty())
            collected.append({ id, WTF::move(accesses) });
    });

    // An access goes only if each candidate that it may reach goes.
    struct CandidateSet : WTF::UnionFind<CandidateSet> { };
    Vector<CandidateSet> sets(liveCandidates.size());
    bool hasSharedAccess = false;
    for (const auto& [id, accesses] : functions) {
        for (const RemovableAccess& site : accesses) {
            for (uint32_t candidate : site.candidates.span().subspan(1)) {
                sets[candidate].unify(&sets[site.candidates[0]]);
                hasSharedAccess = true;
            }
        }
    }
    if (hasSharedAccess) {
        auto rootOf = [&](uint32_t candidate) {
            return static_cast<size_t>(sets[candidate].find() - sets.span().data());
        };
        BitVector hasLiveMember(liveCandidates.size());
        for (uint32_t candidate = 0; candidate < liveCandidates.size(); ++candidate) {
            if (isLive(candidate))
                hasLiveMember.quickSet(rootOf(candidate));
        }
        for (uint32_t candidate = 0; candidate < liveCandidates.size(); ++candidate) {
            if (hasLiveMember.quickGet(rootOf(candidate)))
                markLive(candidate);
        }
    }

    uint64_t numberOfRemovedFields = 0;
    for (uint32_t candidate = 0; candidate < fields.size(); ++candidate) {
        bool hadNoAccesses = hasNoAccesses(fieldAt(program, fields[candidate]).flow.location);
        if (isLive(candidate) != hadNoAccesses)
            continue;
        Field& field = editor.at(fields[candidate]);
        if (hadNoAccesses) {
            field.flow.location.stores = StoreCount::Unknown;
            continue;
        }
        field.flow.location.stores = StoreCount::Never;
        field.flow.location.reads = ReadState::NeverRead;
        // No allocation lists it any more: an object is finished without it.
        if (field.flow.occupancy == Occupancy::FilledAtBirth || field.flow.occupancy == Occupancy::FilledWithValueAtBirth)
            field.flow.occupancy = Occupancy::Unknown;
        // A validating build tells the run time which names it took away, so that a lookup of one from outside compiled code ends the process.
        if (build.validates())
            field.flags.add(FieldFlags::IsRemovedAsUnread);
        ++numberOfRemovedFields;
    }
    build.statistics().count(StatisticID::UnreadFieldsRemoved, numberOfRemovedFields);
    for (size_t index : variables) {
        if (isLive(candidateOfVariable(index)))
            continue;
        LocationFlow& flow = editor.at(VariableID(index)).flow;
        flow.stores = StoreCount::Never;
        flow.reads = ReadState::NeverRead;
    }

    functions.removeAllMatching([&](FunctionAccesses& entry) {
        entry.second.removeAllMatching([&](const RemovableAccess& site) {
            return std::ranges::any_of(site.candidates, isLive);
        });
        return entry.second.isEmpty();
    });
    uint64_t numberOfVariableStores = 0;
    Vector<FunctionID> ids;
    ids.reserveInitialCapacity(functions.size());
    for (const auto& [id, accesses] : functions) {
        ids.append(id);
        for (const RemovableAccess& site : accesses)
            numberOfVariableStores += program.at(id).at(site.access).opcode == Opcode::PutModuleVariable;
    }
    build.statistics().count(StatisticID::UnreadVariableStoresRemoved, numberOfVariableStores);

    editFunctionsInParallel(editor, build, ids.span(), [&](unsigned index, FunctionID id, Function& function, TaskArenas& arenas) {
        IRBuilder builder(AnalysisContext { build, program, &inputs.flow, &inputs.effects, id }, function, arenas.types);
        std::span<const RemovableAccess> accesses = functions[index].second.span();
        auto isNext = [&](BlockID block) {
            return !accesses.empty() && accesses.front().block == block;
        };
        while (!accesses.empty()) {
            BlockID block = accesses.front().block;
            const BlockValues& values = function.at(block).values;
            BitVector erasedIndexes;
            for (uint32_t position = 0; position < values.size() && isNext(block); ++position) {
                if (accesses.front().access != values[position])
                    continue;
                while (isNext(block) && accesses.front().access == values[position])
                    accesses = accesses.subspan(1);

                Value& access = function.edit(values[position]);
                if (access.opcode != Opcode::NewObject) {
                    erasedIndexes.set(position);
                    continue;
                }
                RELEASE_ASSERT(access.fields().size() == access.variadicOperands().size());
                Vector<uint32_t, 16> remainingFields;
                Vector<ValueID, 16> remainingValues;
                for (size_t listed = 0; listed < access.fields().size(); ++listed) {
                    auto candidate = candidateOf(access.layout(), access.fields()[listed]);
                    if (candidate && !isLive(*candidate))
                        continue;
                    remainingFields.append(access.fields()[listed]);
                    remainingValues.append(access.variadicOperands()[listed]);
                }
                access.setFields(remainingFields.span());
                access.setVariadicOperands(remainingValues.span());
            }
            RELEASE_ASSERT(!isNext(block));
            if (!erasedIndexes.isEmpty())
                builder.eraseValues(block, erasedIndexes);
        }
        simplifyEditedFunction(runner, program, inputs, id, function, arenas);
    });
}

static bool mayBeReadByUnknownKey(NamespaceUse use)
{
    switch (use) {
    case NamespaceUse::Unknown:
        return true;
    case NamespaceUse::NamedReadsOnly:
    case NamespaceUse::NeverCreated:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void removeUnreadExports(ProgramEditor& editor, const FlowResults& flow, BuildContext& build)
{
    if (!flow.hasResults())
        return;

    const Program& program = editor.program();
    BitVector exportedVariables(program.variables.size());
    auto addExport = [&](VariableID variable) {
        if (variable)
            exportedVariables.quickSet(variable.index());
    };
    for (const Module& module : program.modules) {
        for (const ModuleExport& entry : module.exports)
            addExport(entry.variable);
        for (const SyntheticExport& entry : module.syntheticExports)
            addExport(entry.variable);
    }
    BitVector unreadVariables;
    for (unsigned index = 0; index < program.variables.size(); ++index) {
        const Variable& variable = program.variables[index];
        bool mayBeReadThroughNamespace = exportedVariables.quickGet(index) && mayBeReadByUnknownKey(flow.module(variable.module).namespaceUse);
        // Whether a static field exists decides where a store by name to its class or to a subclass goes, so its definition is no dead store.
        if (variable.flow.reads == ReadState::NeverRead && variable.flow.writers == WriterState::AllKnown && !variable.owner && !mayBeReadThroughNamespace)
            unreadVariables.set(index);
    }
    if (unreadVariables.isEmpty())
        return;

    const ParallelLoops& loops = build.parallelLoops();
    using FunctionStores = std::pair<FunctionID, Vector<VariableID, 4>>;
    Vector<FunctionStores> stores = collectInParallel<FunctionStores>(loops, program.functions.size(), [&](unsigned index, Vector<FunctionStores>& found) {
        Vector<VariableID, 4> storedVariables;
        forEachListedValue(program, FunctionID(index), { Opcode::PutModuleVariable }, [&](BlockID, uint32_t, ValueID, const Value& value) {
            if (unreadVariables.get(value.variable().index()))
                storedVariables.append(value.variable());
        });
        if (!storedVariables.isEmpty())
            found.append({ FunctionID(index), WTF::move(storedVariables) });
    });

    Vector<Function*> editedFunctions;
    BitVector variablesWithRemovedStores;
    BitVector variablesWithRemainingStores;
    uint64_t numberOfRemovedStores = 0;
    for (const auto& [function, variables] : stores) {
        bool isEdited = build.isEnabled(PassID::RemoveUnreadExports, function);
        for (VariableID variable : variables)
            (isEdited ? variablesWithRemovedStores : variablesWithRemainingStores).set(variable.index());
        if (!isEdited)
            continue;
        editedFunctions.append(&editor.at(function));
        numberOfRemovedStores += variables.size();
    }

    forEachIndexInParallel(loops, editedFunctions.size(), [&](unsigned index) {
        Function& function = *editedFunctions[index];
        for (Block& block : function.editAllBlocks()) {
            block.values.removeAllMatching([&](ValueID id) {
                return function.at(id).opcode == Opcode::PutModuleVariable && unreadVariables.get(function.at(id).variable().index());
            });
        }
        // The annotation says that the call may go when nobody uses its answer. An argument can be such a call too, and a cast that tests nothing is no use by itself.
        for (bool didRemove = true; std::exchange(didRemove, false);) {
            BitVector usedValues = findUsedValues(function);
            for (Block& block : function.editAllBlocks()) {
                didRemove |= !!block.values.removeAllMatching([&](ValueID id) {
                    const Value& value = function.at(id);
                    bool isAnnotatedPure = (value.opcode == Opcode::Call || value.opcode == Opcode::Construct) && value.isAnnotatedPure();
                    return (isAnnotatedPure || isCastWithoutTest(value.opcode)) && !usedValues.quickGet(id.index());
                });
            }
        }
    });

    variablesWithRemovedStores.exclude(variablesWithRemainingStores);
    for (size_t index : variablesWithRemovedStores)
        editor.at(VariableID(index)).flow.stores = StoreCount::Never;
    build.statistics().count(StatisticID::UnreadVariableStoresRemoved, numberOfRemovedStores);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
