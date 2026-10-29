/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTHeatGraph.h"

#if ENABLE(AOT)

#include "AOTBlockFrequencies.h"
#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTBuiltinIDs.h"
#include "AOTCallGraph.h"
#include "AOTDiagnostics.h"
#include "AOTEffectSummaries.h"
#include "AOTEffectsDescription.h"
#include "AOTEffectsOfOperands.h"
#include "AOTEffectsOfProtocols.h"
#include "AOTFlowResults.h"
#include "AOTFunctionAnalyses.h"
#include "AOTGrouping.h"
#include "AOTIR.h"
#include "AOTLargeVectors.h"
#include "AOTLayout.h"
#include "AOTOpcodesNamingRecords.h"
#include "AOTPropertyAccessElements.h"
#include "AOTSort.h"
#include "AOTThreshold.h"
#include <algorithm>
#include <atomic>
#include <wtf/Atomics.h>
#include <wtf/BitVector.h>
#include <wtf/CycleForest.h>
#include <wtf/FrequencyEstimator.h>
#include <wtf/PrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(HeatModel::eventsPerRun == frequencyConstants.maxMultiplier);

namespace {

struct WeightedCall {
    uint32_t node;
    double weight;
    // How surely the call is made once its function is entered, where an even branch counts as taken.
    double share { 0 };
};

using WeightedCallList = Vector<WeightedCall, 4>;

enum class CallOfHost : uint8_t { StaysUnlessCalleeRunsOnce, IsReplaced };

// Ranks say which of two functions runs more often: a call of a name shares its weight among the functions under the name.
// Classes say whether a function runs at all, and about how often: what a call of a name may enter is entered as a loop would enter it, or as often as events come.
enum class EstimateOf : uint8_t { Ranks, Classes };

} // anonymous namespace


static void mergeEdges(WeightedCallList& calls)
{
    AOT::stableSort(calls, [](const WeightedCall& a, const WeightedCall& b) {
        return a.node < b.node;
    });
    size_t mergedSize = 0;
    for (size_t index = 0; index < calls.size(); ++index) {
        WeightedCall call = calls[index];
        if (mergedSize && calls[mergedSize - 1].node == call.node) {
            calls[mergedSize - 1].weight += call.weight;
            calls[mergedSize - 1].share = std::max(calls[mergedSize - 1].share, call.share);
        } else
            calls[mergedSize++] = call;
    }
    calls.shrink(mergedSize);
}

static bool mayHaveNoLayout(const Type& object) { return object.anyObject || !object.classes.isEmpty(); }

static bool saysNothingAboutCalls(OperandUses uses) { return uses.containsAll(callingUses); }

static constexpr OpcodeSet opcodesThatMayEnterBuiltins = []() consteval {
    OpcodeSet opcodes;
    for (unsigned index = 0; index < numberOfEnumerators<Opcode>; ++index) {
        EffectRule rule = effectRuleOf(static_cast<Opcode>(index));
        if (rule == EffectRule::Call || rule == EffectRule::Builtin)
            opcodes.add(static_cast<Opcode>(index));
    }
    return opcodes;
}();

static bool entersOnlyBuiltins(const Program& program, const Value& call)
{
    if (effectRuleOf(call.opcode) == EffectRule::Builtin)
        return true;
    std::optional<ProvenValue<Callees>> callees = calleesOfCall(program, call).all();
    return callees && callees->value().functions.empty() && !callees->value().builtins.empty();
}

static double estimateCallbackCalls(OperandUses uses)
{
    if (saysNothingAboutCalls(uses))
        return 1;
    if (uses.contains(OperandUse::CallsRepeatedly))
        return HeatModel::callsOfRepeatedCallback;
    if (uses.contains(OperandUse::CallsPerElement))
        return HeatModel::callsPerElement;
    return 1;
}

static bool isHandedToCodeThatRuns(const Value& user, uint32_t operandIndex)
{
    if (effectRuleOf(user.opcode) != EffectRule::Call)
        return true;
    return !user.operandUses(operandIndex).containsAny(callingUses) && OperandPosition::newTarget(user.opcode) != operandIndex;
}

static bool isHandedToCodeThatRuns(const Terminator&, uint32_t) { return true; }

static double shareOfClass(Expected<BlockFrequency> frequency)
{
    if (frequency.predicted() == BlockFrequency::Normal)
        return 1;
    return frequency.predicted() == BlockFrequency::Unlikely ? frequencyConstants.unlikelyEdgeShare : frequencyConstants.rareEdgeShare;
}

static Vector<WeightOfBlock> computeWeightsOfBlocks(FunctionAnalyses& analyses)
{
    const BlockFrequencies& frequencies = analyses.blockFrequencies();
    Vector<WeightOfBlock> weights;
    weights.reserveInitialCapacity(analyses.cfg().reversePostOrder().size());
    for (BlockID id : analyses.cfg().reversePostOrder())
        weights.append(WeightOfBlock { id, frequencies.frequencyOf(id).predicted(), shareOfClass(frequencies.classOf(id)) });
    return weights;
}

static double estimateIndirectCalls(const FlowIndirectCall& call)
{
    bool describesSite = call.kind == FlowIndirectCallKind::BuiltinCallback || call.times.predicted() == BuiltinCallbackTimes::Once;
    double runsOfSite = call.isInCycle && describesSite ? frequencyConstants.iterationsPerLoop : 1;
    switch (call.times.predicted()) {
    case BuiltinCallbackTimes::PerEvent:
        return 0;
    case BuiltinCallbackTimes::Repeatedly:
        return runsOfSite * HeatModel::callsOfRepeatedCallback;
    case BuiltinCallbackTimes::PerElement:
        return runsOfSite * HeatModel::callsPerElement;
    case BuiltinCallbackTimes::Later:
    case BuiltinCallbackTimes::Once:
        return runsOfSite;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

namespace {

// The functions that a store, a definition or a literal of the program puts under a name, and whose callers are not all known.
class FunctionsUnderNames {
public:
    FunctionsUnderNames() = default;
    FunctionsUnderNames(const Program&, const CallGraph&, const ParallelLoops&);

    size_t size() const { return m_names.size(); }
    std::optional<size_t> find(PropertyKeyID name) const
    {
        auto found = std::ranges::lower_bound(m_names, name);
        if (found == m_names.end() || *found != name)
            return std::nullopt;
        return found - m_names.begin();
    }
    std::span<const FunctionID> at(size_t position) const LIFETIME_BOUND { return m_functions.span().subspan(m_first[position], m_first[position + 1] - m_first[position]); }
    bool isUnderSomeName(FunctionID function) const { return m_isUnderSomeName.get(function.index()); }
    // Of these functions, the ones that lie in objects of one layout, under whatever name. No layout stands for the objects whose layout nobody knows.
    static uint32_t holderOf(LayoutID layout) { return layout ? layout.index() + 1 : 0; }
    size_t numberOfHolders() const { return m_firstInHolder.isEmpty() ? 0 : m_firstInHolder.size() - 1; }
    std::span<const FunctionID> inHolder(uint32_t holder) const LIFETIME_BOUND { return m_functionsInHolders.span().subspan(m_firstInHolder[holder], m_firstInHolder[holder + 1] - m_firstInHolder[holder]); }
    // The getters that a class defines under one name, or that flow files as members of a layout under it, and apart from them the setters: what a read under the name may enter, and what a store may.
    static uint32_t placeOfAccessors(PropertyKeyID name, Access enteredBy) { return name.index() * 2 + (enteredBy == Access::Store); }
    std::optional<size_t> findAccessors(PropertyKeyID name, Access enteredBy) const
    {
        if (!name || (enteredBy != Access::Read && enteredBy != Access::Store))
            return std::nullopt;
        auto found = std::ranges::lower_bound(m_placesOfAccessors, placeOfAccessors(name, enteredBy));
        if (found == m_placesOfAccessors.end() || *found != placeOfAccessors(name, enteredBy))
            return std::nullopt;
        return found - m_placesOfAccessors.begin();
    }
    std::span<const FunctionID> accessorsAt(size_t position) const LIFETIME_BOUND { return m_accessors.span().subspan(m_firstAccessor[position], m_firstAccessor[position + 1] - m_firstAccessor[position]); }

private:
    Vector<PropertyKeyID> m_names;
    Vector<uint32_t> m_first;
    Vector<FunctionID> m_functions;
    BitVector m_isUnderSomeName;
    Vector<uint32_t> m_firstInHolder;
    Vector<FunctionID> m_functionsInHolders;
    Vector<uint32_t> m_placesOfAccessors;
    Vector<uint32_t> m_firstAccessor;
    Vector<FunctionID> m_accessors;
};

} // anonymous namespace

// A value that may be one of more functions than this tells nothing about what lies under the name.
AOT_THRESHOLD(size_t, maxFunctionsOfStoredValue, 8);

static_assert(OperandIndex::PropSet::object == OperandIndex::PropDefine::object && OperandIndex::PropSet::value == OperandIndex::PropDefine::value);
static_assert(OperandIndex::PutSlot::object == OperandIndex::PutSlotOrAdd::object && OperandIndex::PutSlot::value == OperandIndex::PutSlotOrAdd::value);
static Access accessThatEnters(MethodKind kind)
{
    switch (kind) {
    case MethodKind::Method:
        return Access::None;
    case MethodKind::Getter:
        return Access::Read;
    case MethodKind::Setter:
        return Access::Store;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

FunctionsUnderNames::FunctionsUnderNames(const Program& program, const CallGraph& callGraph, const ParallelLoops& loops)
{
    auto packedKeyOf = [](PropertyKeyID name, FunctionID function) {
        return static_cast<uint64_t>(name.index()) << 32 | function.index();
    };
    auto isWanted = [&](FunctionID function) {
        return program.at(function).hasBody() && callGraph.callerSet(function) != CallerSet::Complete;
    };
    // Behind every name in the order of the packed keys: first the accessors, then the holders.
    constexpr uint64_t namesAccessors = 1ull << 62;
    constexpr uint64_t namesHolder = 1ull << 63;
    auto packedAccessorOf = [](PropertyKeyID name, Access enteredBy, FunctionID accessor) {
        return namesAccessors | static_cast<uint64_t>(placeOfAccessors(name, enteredBy)) << 32 | accessor.index();
    };
    auto packedHolderOf = [&](LayoutID layout, FunctionID function) {
        return namesHolder | static_cast<uint64_t>(holderOf(layout ? declaredLayoutOf(program, layout) : layout)) << 32 | function.index();
    };
    Vector<uint64_t> allStored = collectInParallel<uint64_t>(loops, program.functions.size(), [&](unsigned index, Vector<uint64_t>& stored) {
        const Function& function = program.functions[index];
        if (!function.hasBody())
            return;
        auto add = [&](PropertyKeyID name, ValueID value, const auto& forEachLayoutOfHolder) {
            if (!name || !value)
                return;
            const Type& type = program.typeOf(function, value);
            if (type.functions.size() > maxFunctionsOfStoredValue)
                return;
            for (FunctionID callee : type.functions) {
                if (!isWanted(callee))
                    continue;
                stored.append(packedKeyOf(name, callee));
                forEachLayoutOfHolder([&](LayoutID layout) {
                    stored.append(packedHolderOf(layout, callee));
                });
            }
        };
        auto addTo = [&](ValueID holder, PropertyKeyID name, ValueID value) {
            add(name, value, [&](const auto& note) {
                const Type& type = program.typeOf(function, holder);
                for (LayoutID layout : membersOf(program, type))
                    note(layout);
                if (mayHaveNoLayout(type))
                    note(LayoutID());
            });
        };
        auto addAccessor = [&](PropertyKeyID name, ValueID value, Access enteredBy) {
            if (!name || !value || enteredBy == Access::None)
                return;
            const Type& type = program.typeOf(function, value);
            if (type.functions.size() > maxFunctionsOfStoredValue)
                return;
            for (FunctionID accessor : type.functions) {
                if (isWanted(accessor))
                    stored.append(packedAccessorOf(name, enteredBy, accessor));
            }
        };
        forEachValueListedOrNot(program, FunctionID(index), { Opcode::PropSet, Opcode::PropDefine, Opcode::PutSlot, Opcode::PutSlotOrAdd, Opcode::NewObject, Opcode::DefineMethod }, [&](ValueID, const Value& value) {
            if (value.opcode == Opcode::NewObject) {
                const Vector<Field>& fields = program.at(declaredLayoutOf(program, value.layout())).fields;
                std::span<const uint32_t> listed = value.fields();
                for (size_t position = 0; position < listed.size(); ++position)
                    add(fields[listed[position]].key, value.operand(OperandIndex::NewObject::values + position), [&](const auto& note) { note(value.layout()); });
                return;
            }
            if (value.opcode == Opcode::DefineMethod) {
                add(value.key(), value.operand(OperandIndex::DefineMethod::function), [](const auto&) { });
                addAccessor(value.key(), value.operand(OperandIndex::DefineMethod::function), accessThatEnters(value.methodKind()));
                return;
            }
            bool storesInSlot = value.opcode == Opcode::PutSlot || value.opcode == Opcode::PutSlotOrAdd;
            addTo(value.operand(storesInSlot ? OperandIndex::PutSlot::object : OperandIndex::PropSet::object), value.key(), value.operand(storesInSlot ? OperandIndex::PutSlot::value : OperandIndex::PropSet::value));
        });
    });
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        const Layout& layout = program.layouts[index];
        for (const Vector<ClassMember>* members : { &layout.classFlow.members, &layout.classFlow.namedMembers }) {
            for (const ClassMember& member : *members) {
                if (member.key && member.function && isWanted(member.function)) {
                    allStored.append(packedKeyOf(member.key, member.function));
                    allStored.append(packedHolderOf(LayoutID(index), member.function));
                    if (Access enteredBy = accessThatEnters(member.kind); enteredBy != Access::None)
                        allStored.append(packedAccessorOf(member.key, enteredBy, member.function));
                }
            }
        }
    }
    sortAndRemoveDuplicates(loops, allStored);
    size_t numberUnderNames = std::ranges::lower_bound(allStored, namesAccessors) - allStored.begin();
    size_t numberInFrontOfHolders = std::ranges::lower_bound(allStored, namesHolder) - allStored.begin();
    m_firstInHolder.fill(0u, program.layouts.size() + 2);
    m_functionsInHolders.reserveInitialCapacity(allStored.size() - numberInFrontOfHolders);
    for (uint64_t stored : allStored.span().subspan(numberInFrontOfHolders)) {
        ++m_firstInHolder[static_cast<uint32_t>((stored & ~namesHolder) >> 32) + 1];
        m_functionsInHolders.append(FunctionID(static_cast<uint32_t>(stored)));
    }
    for (size_t holder = 1; holder < m_firstInHolder.size(); ++holder)
        m_firstInHolder[holder] += m_firstInHolder[holder - 1];
    m_accessors.reserveInitialCapacity(numberInFrontOfHolders - numberUnderNames);
    for (uint64_t stored : allStored.span().subspan(numberUnderNames, numberInFrontOfHolders - numberUnderNames)) {
        uint32_t place = static_cast<uint32_t>((stored & ~namesAccessors) >> 32);
        if (m_placesOfAccessors.isEmpty() || m_placesOfAccessors.last() != place) {
            m_placesOfAccessors.append(place);
            m_firstAccessor.append(m_accessors.size());
        }
        m_accessors.append(FunctionID(static_cast<uint32_t>(stored)));
    }
    m_firstAccessor.append(m_accessors.size());
    allStored.shrink(numberUnderNames);
    // `x.name(..)` calls what a getter under the name answers, and `x.name()(..)` what a method under it does.
    allStored.appendVector(collectInParallel<uint64_t>(loops, allStored.size(), [&](unsigned position, Vector<uint64_t>& answered) {
        PropertyKeyID name(static_cast<uint32_t>(allStored[position] >> 32));
        FunctionID holder(static_cast<uint32_t>(allStored[position]));
        const Function& function = program.at(holder);
        forEachBlockEndingWith(program, holder, { TerminatorKind::Return }, [&](BlockID block) {
            ValueID result = function.at(block).terminator.valueOperand();
            if (!result)
                return;
            const Type& type = program.typeOf(function, result);
            if (type.functions.size() > maxFunctionsOfStoredValue)
                return;
            for (FunctionID callee : type.functions) {
                if (isWanted(callee))
                    answered.append(packedKeyOf(name, callee));
            }
        });
    }));
    sortAndRemoveDuplicates(loops, allStored);
    m_functions.reserveInitialCapacity(allStored.size());
    for (uint64_t stored : allStored) {
        PropertyKeyID name(static_cast<uint32_t>(stored >> 32));
        if (m_names.isEmpty() || m_names.last() != name) {
            m_names.append(name);
            m_first.append(m_functions.size());
        }
        m_functions.append(FunctionID(static_cast<uint32_t>(stored)));
        m_isUnderSomeName.set(static_cast<uint32_t>(stored));
    }
    m_first.append(m_functions.size());
}

static_assert(OperandIndex::ElemGet::property == OperandIndex::ElemSet::property);

namespace {

struct AccessUnderName {
    PropertyKeyID name;
    Access access { Access::None };
};

} // anonymous namespace

// A key that is one symbol is a name as well.
static AccessUnderName accessUnderNameOf(const Program& program, const Function& function, const Value& access)
{
    if (PayloadWord::key(access.opcode))
        return { access.key(), accessOf(access.opcode) };
    if (access.opcode != Opcode::ElemGet && access.opcode != Opcode::ElemSet)
        return { };
    ConstantKey key = constantKeyOf(program, program.typeOf(function, access.operand(OperandIndex::ElemGet::property)));
    if (key.kind != ConstantKeyKind::Symbol)
        return { };
    return { key.symbol, access.opcode == Opcode::ElemGet ? Access::Read : Access::Store };
}

static AccessUnderName accessUnderNameOf(const Program&, const Function&, const Terminator&) { return { }; }

static PropertyKeyID nameOfCallee(const Program& program, const Function& function, const Value& call)
{
    if (call.opcode == Opcode::CallMethod)
        return call.key();
    if (!PayloadWord::calleeKey(call.opcode))
        return { };
    if (call.calleeKey())
        return call.calleeKey();
    if (call.opcode != Opcode::Call && call.opcode != Opcode::CallList)
        return { };
    AccessUnderName read = accessUnderNameOf(program, function, function.at(call.operand(0)));
    return read.access == Access::Read ? read.name : PropertyKeyID();
}

static PropertyKeyID nameOfCallee(const Program&, const Function&, const Terminator&) { return { }; }

static bool isCall(const Value& value) { return effectRuleOf(value.opcode) == EffectRule::Call; }
static bool isCall(const Terminator& terminator) { return effectRuleOf(terminator.kind) == EffectRule::Call; }

// A read under a name that runs code nobody knows runs what a descriptor holds under `get`, and a store what one holds under `set`.
static PropertyKeyID nameOfAccessorInDescriptor(const Program& program, const Value& access)
{
    if (!PayloadWord::key(access.opcode) || !access.key())
        return { };
    if (accessOf(access.opcode) == Access::Read)
        return program.keyOf(BuiltinNames::get);
    return accessOf(access.opcode) == Access::Store ? program.keyOf(BuiltinNames::set) : PropertyKeyID();
}

static PropertyKeyID nameOfAccessorInDescriptor(const Program&, const Terminator&) { return { }; }

static bool constructsUnknownCallee(const Program& program, const Function& function, const Value& value)
{
    std::optional<unsigned> callee = OperandPosition::callee(value.opcode);
    return callee && OperandPosition::newTarget(value.opcode) && !calleesOf(program, program.typeOf(function, value.operand(*callee))).listsEveryCallee();
}

static bool constructsUnknownCallee(const Program&, const Function&, const Terminator&) { return false; }

enum class EnteredAsValueBy : uint8_t { Nothing, Calls, Constructions };

static EnteredAsValueBy whatEntersAsValue(const Function& function)
{
    switch (function.kind) {
    case FunctionKind::Plain:
    case FunctionKind::Arrow:
    case FunctionKind::Method:
        return EnteredAsValueBy::Calls;
    case FunctionKind::BaseConstructor:
    case FunctionKind::DerivedConstructor:
        return EnteredAsValueBy::Constructions;
    case FunctionKind::Getter:
    case FunctionKind::Setter:
    case FunctionKind::Initializer:
    case FunctionKind::Module:
    case FunctionKind::CommonJsModule:
        return EnteredAsValueBy::Nothing;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

namespace {

struct SeenCode {
    BitVector isReached;
    BitVector mayBeCalledAsValue;
    BitVector hasSiteThatMayEnter;
    bool reachesEverything { false };
    bool seesWhatRuns { false };

    bool reaches(FunctionID function) const { return reachesEverything || isReached.get(function.index()); }
    bool mayEnter(FunctionID function) const { return !seesWhatRuns || hasSiteThatMayEnter.get(function.index()); }
};

} // anonymous namespace

// A module is loaded by the entry, or by a module that names it. A closure is made by a body that runs. A call of a callee that nobody knows reaches what has been made and may stand there.
static SeenCode findWhatSeenCodeReaches(const Program& program, const CallGraph& callGraph, const FlowResults& flow, const HeatGraph& graph, const ParallelArray<WeightedCallList>& calls, const FunctionsUnderNames& names, const Vector<Vector<FunctionID>>& functionsMadeBy, const BitVector& isCalledByHost, const BitVector& hasOnlySeenCallers, const Vector<Vector<uint32_t>>& namesReadBy, const Vector<Vector<uint32_t>>& accessorsEnteredBy, const Vector<Vector<uint32_t>>& holdersReadBy, const Vector<Vector<ModuleID>>& modulesLoadedBy, const Vector<uint8_t>& constructsUnknownValue, EstimateOf estimate)
{
    unsigned count = graph.numberOfFunctions();
    bool isForClasses = estimate == EstimateOf::Classes;
    SeenCode seen;
    BitVector& isReached = seen.isReached;
    isReached.ensureSize(graph.numNodes());
    Vector<FunctionID> entrypointOf(count);
    BitVector isEntrypoint(count);
    if (isForClasses) {
        for (unsigned index = 0; index < count; ++index) {
            const Function& function = program.functions[index];
            if (function.role != FunctionRole::GenericEntrypoint || !function.hasBody())
                continue;
            if (const auto* record = function.creator.tryAs<CreatorRecord::Entrypoint>()) {
                entrypointOf[record->target.index()] = FunctionID(index);
                isEntrypoint.quickSet(index);
            }
        }
    }
    BitVector isMade(count);
    BitVector isAwaited(count);
    BitVector isReadAsValue(names.size() + 1);
    BitVector isReadUnderUnknownKey(names.numberOfHolders());
    BitVector hasMaker(count);
    for (const Vector<FunctionID>& made : functionsMadeBy) {
        for (FunctionID function : made)
            hasMaker.quickSet(function.index());
    }
    Vector<uint32_t> worklist;
    auto reach = [&](uint32_t node) {
        if (!isReached.quickSet(node))
            worklist.append(node);
    };
    auto moduleIsLoaded = [&](FunctionID function) {
        ModuleID module = program.at(function).module;
        if (!module)
            return true;
        FunctionID body = program.at(module).body;
        return !body || isReached.quickGet(body.index());
    };
    auto exists = [&](FunctionID function) {
        return hasMaker.quickGet(function.index()) ? isMade.quickGet(function.index()) : moduleIsLoaded(function);
    };
    auto isBodyOfModule = [&](FunctionID function) {
        ModuleID module = program.at(function).module;
        return module && program.at(module).body == function;
    };
    // Text that the application makes at run time names what the application has made.
    bool evaluatesText = false;
    BitVector evaluatesTextIn(count);
    if (flow.hasResults()) {
        for (const UnseenCodeRecord& record : flow.unseenCode()) {
            if (record.cause == UnseenCodeCause::EvaluatesText && record.witness.function < count)
                evaluatesTextIn.quickSet(record.witness.function);
        }
    }
    BitVector isStartedByHost(count);
    for (FunctionID root : callGraph.roots()) {
        if ((!isForClasses || isCalledByHost.get(root.index())) && !isEntrypoint.quickGet(root.index()) && program.at(root).hasBody() && !isBodyOfModule(root))
            isStartedByHost.quickSet(root.index());
    }
    BitVector hasEscaped(count);
    for (FunctionID function : callGraph.functionsCalledByUnknownCode()) {
        if (!isBodyOfModule(function) && !hasOnlySeenCallers.get(function.index()) && !isEntrypoint.quickGet(function.index()))
            hasEscaped.quickSet(function.index());
    }
    BitVector isEnteredUnderName(count);
    BitVector isUnderNameReadAsValue(count);
    auto mayStandWhereValueIsCalled = [&](FunctionID function) {
        return hasEscaped.quickGet(function.index()) && (!names.isUnderSomeName(function) || isUnderNameReadAsValue.quickGet(function.index()) || (evaluatesText && !isLibraryCode(program, program.at(function))));
    };
    bool constructsValue = false;
    Vector<Vector<FunctionID>> functionsWaitingFor(program.modules.size());
    BitVector isWaiting(count);
    auto reachIfCalled = [&](FunctionID function) {
        if (isReached.quickGet(function.index()))
            return;
        if (!isStartedByHost.quickGet(function.index()) && !isEnteredUnderName.quickGet(function.index()) && !(isReached.quickGet(graph.unknownCodeNode().index()) && mayStandWhereValueIsCalled(function)))
            return;
        if (exists(function))
            reach(function.index());
        else if (!hasMaker.quickGet(function.index()) && !isWaiting.quickSet(function.index()))
            functionsWaitingFor[program.at(function).module.index()].append(function);
    };
    auto enterUnderName = [&](FunctionID function) {
        if (hasOnlySeenCallers.get(function.index()))
            return;
        isEnteredUnderName.quickSet(function.index());
        reachIfCalled(function);
    };
    for (const Entry& entry : program.entries) {
        if (FunctionID body = program.at(entry.module).body)
            reach(body.index());
    }
    if (program.entries.isEmpty()) {
        // Nothing says where such a program starts.
        for (unsigned index = 0; index < count; ++index) {
            if (program.functions[index].hasBody()) {
                reach(index);
                isMade.quickSet(index);
            }
        }
    }
    for (size_t root : isStartedByHost)
        reachIfCalled(FunctionID(root));
    while (!worklist.isEmpty()) {
        uint32_t node = worklist.takeLast();
        if (node == graph.unknownCodeNode().index()) {
            for (size_t function : hasEscaped)
                reachIfCalled(FunctionID(function));
            continue;
        }
        if (node >= count) {
            for (FunctionID function : names.at(node - graph.nodeOfName(0).index()))
                enterUnderName(function);
            continue;
        }
        FunctionID function(node);
        constructsValue |= !!constructsUnknownValue[node];
        for (ModuleID module : modulesLoadedBy[node]) {
            if (FunctionID body = program.at(module).body)
                reach(body.index());
        }
        if (FunctionID entrypoint = entrypointOf[node])
            reach(entrypoint.index());
        for (FunctionID made : functionsMadeBy[node]) {
            if (isMade.quickSet(made.index()))
                continue;
            if (isAwaited.quickGet(made.index()))
                reach(made.index());
            else
                reachIfCalled(made);
        }
        for (uint32_t position : namesReadBy[node]) {
            if (isReadAsValue.quickSet(position))
                continue;
            for (FunctionID read : names.at(position)) {
                isUnderNameReadAsValue.quickSet(read.index());
                reachIfCalled(read);
            }
        }
        // What an accessor reads or stores under its own name does not say that anything else enters it.
        if (isForClasses) {
            for (uint32_t position : accessorsEnteredBy[node]) {
                for (FunctionID accessor : names.accessorsAt(position)) {
                    if (accessor != function)
                        enterUnderName(accessor);
                }
            }
        }
        for (uint32_t holder : holdersReadBy[node]) {
            if (isReadUnderUnknownKey.quickSet(holder))
                continue;
            for (FunctionID read : names.inHolder(holder)) {
                isUnderNameReadAsValue.quickSet(read.index());
                reachIfCalled(read);
            }
        }
        for (const WeightedCall& call : calls[node]) {
            if (isForClasses && call.node < count && hasMaker.quickGet(call.node) && !isMade.quickGet(call.node)) {
                isAwaited.quickSet(call.node);
                continue;
            }
            reach(call.node);
        }
        if (isBodyOfModule(function)) {
            ModuleID module = program.at(function).module;
            for (const ModuleRequest& request : program.at(module).requests) {
                if (FunctionID body = program.at(request.target).body)
                    reach(body.index());
            }
            for (FunctionID waiting : std::exchange(functionsWaitingFor[module.index()], { }))
                reachIfCalled(waiting);
        }
        if (FunctionID functionAsRead = findFunctionAsRead(&program, function)) {
            for (const FlowIndirectCall& call : flow.indirectCallsFrom(functionAsRead)) {
                if (program.at(call.callee).hasBody())
                    reach(call.callee.index());
            }
        }
        if (evaluatesTextIn.quickGet(node) && !evaluatesText) {
            evaluatesText = true;
            reach(graph.unknownCodeNode().index());
            for (size_t escaped : hasEscaped)
                reachIfCalled(FunctionID(escaped));
        }
    }
    seen.mayBeCalledAsValue = BitVector(count);
    seen.hasSiteThatMayEnter = WTF::move(isEnteredUnderName);
    bool callsValue = isReached.quickGet(graph.unknownCodeNode().index());
    seen.seesWhatRuns = flow.hasResults() && !program.entries.isEmpty();
    for (size_t function : hasEscaped) {
        if (!isReached.quickGet(function) || !mayStandWhereValueIsCalled(FunctionID(function)))
            continue;
        seen.mayBeCalledAsValue.quickSet(function);
        EnteredAsValueBy enteredBy = whatEntersAsValue(program.functions[function]);
        if ((enteredBy == EnteredAsValueBy::Calls && callsValue) || (enteredBy == EnteredAsValueBy::Constructions && constructsValue))
            seen.hasSiteThatMayEnter.quickSet(function);
    }
    return seen;
}

// How surely each node is reached: along the surest path from the entries, the product of the weights of its calls, none over 1. A call of a name, or of a value, enters one of the functions that may stand there.
static Vector<double> estimateHowSurelyNodesAreReached(const Program& program, const FlowResults& flow, const HeatGraph& graph, const ParallelArray<WeightedCallList>& calls, const FunctionsUnderNames& names, const SeenCode& seen, const BitVector& hasOnlySeenCallers, const Vector<Vector<ModuleID>>& modulesLoadedBy)
{
    unsigned count = graph.numberOfFunctions();
    Vector<double> sureness(FillWith { }, graph.numNodes(), 0.0);
    Vector<std::pair<double, uint32_t>> heap;
    auto enter = [&](uint32_t node, double howSurely) {
        if (howSurely <= sureness[node] || (node < count && !seen.reaches(FunctionID(node))))
            return;
        sureness[node] = howSurely;
        heap.append({ howSurely, node });
        std::ranges::push_heap(heap);
    };
    auto enterBodyOf = [&](ModuleID module, double howSurely) {
        if (FunctionID body = program.at(module).body)
            enter(body.index(), howSurely);
    };
    Vector<FunctionID> entrypointOf(count);
    for (unsigned index = 0; index < count; ++index) {
        const Function& function = program.functions[index];
        if (function.role != FunctionRole::GenericEntrypoint || !function.hasBody())
            continue;
        if (const auto* record = function.creator.tryAs<CreatorRecord::Entrypoint>())
            entrypointOf[record->target.index()] = FunctionID(index);
    }
    for (const Entry& entry : program.entries)
        enterBodyOf(entry.module, 1);
    while (!heap.isEmpty()) {
        std::ranges::pop_heap(heap);
        auto [howSurely, node] = heap.takeLast();
        if (howSurely < sureness[node])
            continue;
        if (node == graph.hostNode().index())
            continue;
        if (node == graph.unknownCodeNode().index()) {
            double share = howSurely / std::max<size_t>(seen.mayBeCalledAsValue.bitCount(), 1);
            for (size_t function : seen.mayBeCalledAsValue)
                enter(function, share);
            continue;
        }
        if (node >= count) {
            std::span<const FunctionID> functions = names.at(node - graph.nodeOfName(0).index());
            size_t numberThatMayStandThere = std::ranges::count_if(functions, [&](FunctionID function) {
                return seen.reaches(function) && !hasOnlySeenCallers.get(function.index());
            });
            for (FunctionID function : functions) {
                if (!hasOnlySeenCallers.get(function.index()))
                    enter(function.index(), howSurely / std::max<size_t>(numberThatMayStandThere, 1));
            }
            continue;
        }
        FunctionID function(node);
        if (FunctionID entrypoint = entrypointOf[node])
            enter(entrypoint.index(), howSurely);
        for (ModuleID module : modulesLoadedBy[node])
            enterBodyOf(module, howSurely);
        if (ModuleID module = program.at(function).module; module && program.at(module).body == function) {
            for (const ModuleRequest& request : program.at(module).requests)
                enterBodyOf(request.target, howSurely);
        }
        for (const WeightedCall& call : calls[node])
            enter(call.node, howSurely * std::min(call.weight, 1.0));
        if (FunctionID functionAsRead = findFunctionAsRead(&program, function)) {
            for (const FlowIndirectCall& call : flow.indirectCallsFrom(functionAsRead)) {
                if (program.at(call.callee).hasBody())
                    enter(call.callee.index(), howSurely);
            }
        }
    }
    return sureness;
}

// How often a function is entered because an event came. The host enters a handler once per event. A call hands that on as surely as its block is reached where an even branch counts as taken.
// Where only the name of the callee is known, or nothing, each function that may stand there is entered as often. What is entered so hands that on through calls of known callees, and under a name to the library's functions alone.
// A value that is called is one of the application's functions: the library calls its own under their names.
static Vector<double> estimateEntriesPerEvent(const Program& program, const HeatGraph& graph, const ParallelArray<WeightedCallList>& calls, const FunctionsUnderNames& names, const BitVector& isCalledPerEvent, const SeenCode& seen, const BitVector& hasOnlySeenCallers, const Vector<double>& eventsOfHandlers)
{
    unsigned count = graph.numberOfFunctions();
    Vector<double> entries(FillWith { }, graph.numNodes(), 0.0);
    Vector<uint8_t> isReachedByKnownCalls(FillWith { }, graph.numNodes(), static_cast<uint8_t>(0));
    Vector<std::pair<double, uint32_t>> heap;
    auto enter = [&](uint32_t node, double times, bool byKnownCalls) {
        if (times < 1 || times < entries[node] || (times == entries[node] && byKnownCalls <= !!isReachedByKnownCalls[node]))
            return;
        entries[node] = times;
        isReachedByKnownCalls[node] = byKnownCalls;
        heap.append({ times, node });
        std::ranges::push_heap(heap);
    };
    for (size_t handler : isCalledPerEvent) {
        if (seen.reaches(FunctionID(handler)))
            enter(handler, eventsOfHandlers[handler], true);
    }
    while (!heap.isEmpty()) {
        std::ranges::pop_heap(heap);
        auto [times, node] = heap.takeLast();
        if (times < entries[node] || node == graph.hostNode().index())
            continue;
        if (node < count) {
            for (const WeightedCall& call : calls[node])
                enter(call.node, times * call.share, isReachedByKnownCalls[node]);
            continue;
        }
        if (node == graph.unknownCodeNode().index()) {
            if (!isReachedByKnownCalls[node])
                continue;
            for (size_t function : seen.mayBeCalledAsValue) {
                if (!isLibraryCode(program, program.functions[function]))
                    enter(function, times, false);
            }
            continue;
        }
        for (FunctionID function : names.at(node - graph.nodeOfName(0).index())) {
            if (seen.reaches(function) && !hasOnlySeenCallers.get(function.index()) && (isReachedByKnownCalls[node] || isLibraryCode(program, program.at(function))))
                enter(function.index(), times, false);
        }
    }
    return entries;
}

static WeightedCallList collectHostCalls(const Program& program, const CallGraph& callGraph, const FlowResults& flow, const HeatGraph& graph, const ParallelArray<WeightedCallList>& callsOfProgram, const BitVector& isCalledPerEvent, const Vector<uint8_t>& isCountedByProgram, const SeenCode& seen, const ParallelLoops& loops, EstimateOf estimate, const BitVector& hasOnlySeenCallers, const Vector<double>& entriesPerEvent, const Vector<double>& eventsOfHandlers)
{
    Vector<FunctionID> callees;
    callees.append(callGraph.roots());
    callees.append(callGraph.functionsCalledByUnknownCode());
    for (size_t index : isCalledPerEvent)
        callees.append(FunctionID(index));
    sortAndRemoveDuplicates(loops, callees);

    auto comesWithEvents = [&](FunctionID callee) {
        return !entriesPerEvent.isEmpty() && entriesPerEvent[callee.index()] >= 1 && callGraph.callerSet(callee) != CallerSet::Complete;
    };
    auto callOf = [&](FunctionID callee) {
        if (isCalledPerEvent.get(callee.index()))
            return WeightedCall { graph.nodeOf(callee).index(), eventsOfHandlers[callee.index()] };
        bool runsAsLoopDoes = estimate == EstimateOf::Classes && callGraph.callerSet(callee) != CallerSet::Complete && seen.mayEnter(callee) && !flow.runsAtMostOnce(program, callee);
        return WeightedCall { graph.nodeOf(callee).index(), std::max(runsAsLoopDoes ? frequencyConstants.iterationsPerLoop : 1.0, comesWithEvents(callee) ? entriesPerEvent[callee.index()] : 0.0) };
    };
    WeightedCallList calls;
    calls.reserveInitialCapacity(callees.size());
    Vector<FunctionID> calleesCountedByProgram;
    for (FunctionID callee : callees) {
        if (!program.at(callee).hasBody() || !seen.reaches(callee) || hasOnlySeenCallers.get(callee.index()))
            continue;
        if (isCountedByProgram[callee.index()] && !comesWithEvents(callee))
            calleesCountedByProgram.append(callee);
        else
            calls.append(callOf(callee));
    }
    if (calleesCountedByProgram.isEmpty())
        return calls;

    BitVector isReached(callsOfProgram.size());
    Vector<uint32_t> worklist;
    auto reachWithCallees = [&](uint32_t node) {
        if (isReached.quickSet(node))
            return;
        worklist.append(node);
        while (!worklist.isEmpty()) {
            for (const WeightedCall& call : callsOfProgram[worklist.takeLast()]) {
                if (!isReached.quickSet(call.node))
                    worklist.append(call.node);
            }
        }
    };
    for (const WeightedCall& call : calls)
        reachWithCallees(call.node);
    size_t numberOfSortedCalls = calls.size();
    for (FunctionID callee : calleesCountedByProgram) {
        if (isReached.quickGet(graph.nodeOf(callee).index()))
            continue;
        calls.append(callOf(callee));
        reachWithCallees(graph.nodeOf(callee).index());
    }
    if (calls.size() > numberOfSortedCalls)
        AOT::sort(calls, { }, &WeightedCall::node);
    return calls;
}

static WeightedCallList shareAmong(const Program& program, const FlowResults& flow, const HeatGraph& graph, const BitVector& isCalledPerEvent, const SeenCode& seen, std::span<const FunctionID> callees)
{
    WeightedCallList calls;
    for (FunctionID callee : callees) {
        if (!flow.runsAtMostOnce(program, callee) && !isCalledPerEvent.get(callee.index()) && seen.reaches(callee))
            calls.append(WeightedCall { graph.nodeOf(callee).index(), 1.0 });
    }
    for (WeightedCall& call : calls)
        call.weight /= calls.size();
    return calls;
}

void BodyWeights::didChange(std::span<const FunctionID> functions)
{
    for (FunctionID id : functions) {
        if (id.index() < m_blocks.size())
            m_blocks[id.index()].clear();
    }
}

HeatGraph buildHeatGraph(const Program& program, const CallGraph& callGraph, const FlowResults& flow, const EffectSummaries& effects, BuildContext& build, const BodyReferences* references, const BodyWeights* keptWeights, BodyWeights* madeWeights, HeatGraph* graphOfClasses)
{
    const ParallelLoops& loops = build.parallelLoops();
    unsigned count = program.functions.size();
    RELEASE_ASSERT(!references || references->size() == count);
    RELEASE_ASSERT(!keptWeights || keptWeights->m_blocks.size() <= count);
    if (madeWeights) {
        RELEASE_ASSERT(madeWeights->m_blocks.isEmpty());
        madeWeights->m_blocks.grow(count);
    }
    HeatGraph graph;
    graph.m_sizes.fill(0u, count);
    graph.m_masses.fill(0.0, count);
    // In a very large program nobody asks what is reached, and a callee that nobody knows is any function that has escaped.
    const bool followsNames = !isVeryLargeProgram(program, build);
    const unsigned countWhereNamesAreFollowed = followsNames ? count : 0;
    BitVector isCalledByHost;
    BitVector hasOnlySeenCallersByFlow(count);
    FunctionsUnderNames names;
    if (followsNames) {
        isCalledByHost = functionsCalledByHost(program, loops);
        for (const FlowIndirectCall& call : flow.indirectCalls()) {
            if (call.kind == FlowIndirectCallKind::Host)
                isCalledByHost.set(call.callee.index());
        }
        for (const Variable& variable : program.variables) {
            if (variable.function && !isCalledByHost.get(variable.function.index()) && callerStateOf(flow, variable.function) == CallerState::AllKnown)
                hasOnlySeenCallersByFlow.set(variable.function.index());
        }
        names = FunctionsUnderNames(program, callGraph, loops);
    }
    Vector<Vector<uint32_t>> namesReadBy(countWhereNamesAreFollowed);
    Vector<Vector<uint32_t>> accessorsEnteredBy(countWhereNamesAreFollowed);
    Vector<Vector<uint32_t>> holdersReadBy(countWhereNamesAreFollowed);
    Vector<Vector<ModuleID>> modulesLoadedBy(countWhereNamesAreFollowed);
    Vector<uint8_t> constructsUnknownValue(FillWith { }, countWhereNamesAreFollowed, static_cast<uint8_t>(0));
    Vector<ModuleID> moduleOfRuntimeModule(program.builtins().moduleCount());
    for (uint32_t module = 0; module < program.modules.size(); ++module) {
        RuntimeModuleID runtimeModule = program.modules[module].runtimeModule;
        if (!runtimeModule)
            continue;
        moduleOfRuntimeModule[runtimeModule.index()] = ModuleID(module);
    }
    static const OpcodeSet opcodesNamingModules = findOpcodesNaming<ModuleID, RuntimeModuleID>();
    graph.m_numberOfNames = names.size();
    Vector<Vector<FunctionID>> functionsMadeBy(countWhereNamesAreFollowed);
    unsigned numberOfNodes = graph.numNodes();
    graph.m_isPresent.fill(true, numberOfNodes);
    ParallelArray<WeightedCallList> calls(loops, numberOfNodes, [](unsigned) {
        return WeightedCallList();
    });
    Vector<uint8_t> isCountedByProgram(FillWith { }, count, static_cast<uint8_t>(0));
    BitVector isCalledOnceAfterReturn;
    BitVector isCalledPerEvent;
    for (const FlowIndirectCall& call : flow.indirectCalls()) {
        if (call.times.predicted() == BuiltinCallbackTimes::Later)
            isCalledOnceAfterReturn.set(call.callee.index());
        if (call.times.predicted() == BuiltinCallbackTimes::PerEvent)
            isCalledPerEvent.set(call.callee.index());
    }
    const bool verifies = build.options().verifiesAfterEveryPass;
    const EntriesThatRunNoCode entriesThatRunNoCode = findEntriesThatRunNoCode();
    graph.m_blockGroups = ParallelArray<Vector<HeatBlockGroup, 4>>(loops, count, largeFunctions(program, loops).span(), [&](unsigned index) -> Vector<HeatBlockGroup, 4> {
        FunctionID id(index);
        const Function& function = program.at(id);
        if (!function.hasBody()) {
            graph.m_isPresent[index] = false;
            return { };
        }
        WeightedCallList& callsOfBody = calls[index];
        const ReferencesOfBody* referencesOfBody = references ? &references->of(id) : nullptr;
        FunctionAnalyses analyses(AnalysisContext { build, program, &flow, &effects, id }, function);

        double sharePerWeight = 1;
        auto shareOf = [&](double weight) {
            return std::min(weight * sharePerWeight, 1.0);
        };
        auto add = [&](FunctionID callee, double weight, CallOfHost callOfHost = CallOfHost::StaysUnlessCalleeRunsOnce) {
            if (!program.at(callee).hasBody() || isCalledPerEvent.get(callee.index()))
                return;
            callsOfBody.append(WeightedCall { graph.nodeOf(callee).index(), weight, shareOf(weight) });
            if (callOfHost == CallOfHost::IsReplaced || flow.runsAtMostOnce(program, callee))
                AtomicRef(isCountedByProgram[callee.index()]).store(1, std::memory_order_relaxed);
        };
        auto addUnknownCode = [&](double weight) {
            callsOfBody.append(WeightedCall { graph.unknownCodeNode().index(), weight, shareOf(weight) });
        };
        auto addUnknownCallee = [&](const auto& instruction, double weight) {
            if (!followsNames) {
                addUnknownCode(weight);
                return;
            }
            // A read that runs code nobody knows may enter a getter that lies under its name, and a store a setter. No call stands for that, so no rank moves: where classes are estimated, the accessor is reached and the host enters it as a loop would.
            if (!isCall(instruction)) {
                AccessUnderName access = accessUnderNameOf(program, function, instruction);
                if (std::optional<size_t> position = names.findAccessors(access.name, access.access))
                    accessorsEnteredBy[index].append(static_cast<uint32_t>(*position));
            }
            PropertyKeyID name = isCall(instruction) ? nameOfCallee(program, function, instruction) : nameOfAccessorInDescriptor(program, instruction);
            if (!name) {
                if (!isCall(instruction))
                    return;
                addUnknownCode(weight);
                constructsUnknownValue[index] |= !isLibraryCode(program, function) && constructsUnknownCallee(program, function, instruction);
                return;
            }
            if (std::optional<size_t> position = names.find(name))
                callsOfBody.append(WeightedCall { graph.nodeOfName(*position).index(), weight, shareOf(weight) });
        };
        auto isCountedByCallers = [&](ValueID operand) {
            if (callGraph.callerSet(id) != CallerSet::Complete)
                return false;
            size_t parameter = function.at(entryBlock).parameters.find(analyses.identityOf(operand));
            if (parameter == notFound)
                return false;
            return estimateCallbackCalls(analyses.effectContext().summaries.of(id).operand(static_cast<uint32_t>(parameter)).uses) > 1;
        };
        auto addHandOver = [&](ValueID operand, double weight) {
            CalleeList callees = calleesOf(program, program.typeOf(function, operand));
            std::span<const FunctionID> seen = callees.seen().predicted().functions;
            for (FunctionID callee : seen) {
                if (callGraph.callerSet(callee) != CallerSet::Complete && !isCalledOnceAfterReturn.get(callee.index()))
                    add(callee, weight / seen.size(), CallOfHost::IsReplaced);
            }
        };
        auto addCallsOfSite = [&](const auto& instruction, double weight, std::span<const FunctionID> candidates, bool callsUnknownCode) {
            for (FunctionID candidate : candidates)
                add(candidate, weight / candidates.size());
            if (callsUnknownCode)
                addUnknownCallee(instruction, weight);
            instruction.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                OperandUses uses = summarizeOperand(analyses.effectContext(), function, instruction, operandIndex, UsesAskedFor::AllButPassesToUserCode).uses;
                if (verifies) [[unlikely]]
                    RELEASE_ASSERT((uses | OperandUse::PassesToUserCode) == (summarizeOperand(analyses.effectContext(), function, instruction, operandIndex).uses | OperandUse::PassesToUserCode));
                if (saysNothingAboutCalls(uses)) {
                    if (callsUnknownCode && isHandedToCodeThatRuns(instruction, operandIndex))
                        addHandOver(operand, weight);
                    return;
                }
                double additionalCalls = estimateCallbackCalls(uses) - 1;
                if (!additionalCalls || isCountedByCallers(operand))
                    return;
                CalleeList callees = calleesOf(program, program.typeOf(function, operand));
                std::span<const FunctionID> seen = callees.seen().predicted().functions;
                for (FunctionID callee : seen) {
                    if (!isCalledOnceAfterReturn.get(callee.index()))
                        add(callee, weight * additionalCalls / seen.size());
                }
                if (!callees.listsEveryCallee())
                    addUnknownCode(weight * additionalCalls);
            });
        };
        auto addCalls = [&](const auto& instruction, double weight) {
            if (entriesThatRunNoCode.contains(instruction))
                return;
            EffectDescription description = describeEffects(program, function, instruction);
            if (description.functions.isEmpty() && !description.callsUnknownCode)
                return;
            addCallsOfSite(instruction, weight, description.functions.span(), description.callsUnknownCode);
        };
        auto checkNotedSites = [&](BlockID blockID, size_t position) {
            const Block& block = function.at(blockID);
            std::span<const SiteThatRunsCode> sites = referencesOfBody->sitesThatRunCode.span();
            auto check = [&](const auto& instruction, uint32_t indexInBlock) {
                EffectDescription description = describeEffects(program, function, instruction);
                if (description.functions.isEmpty() && !description.callsUnknownCode)
                    return;
                RELEASE_ASSERT(position < sites.size());
                const SiteThatRunsCode& site = sites[position];
                RELEASE_ASSERT(site.block == blockID && site.indexInBlock == indexInBlock && !!site.callsUnknownCode == description.callsUnknownCode);
                RELEASE_ASSERT(site.listsManyFunctions || std::ranges::equal(referencesOfBody->functionsOfSite(position), description.functions));
                ++position;
            };
            for (uint32_t indexInBlock = 0; indexInBlock < block.values.size(); ++indexInBlock)
                check(function.at(block.values[indexInBlock]), indexInBlock);
            check(block.terminator, block.values.size());
            RELEASE_ASSERT(position == sites.size() || sites[position].block != blockID);
        };
        auto addCallsOfNotedSites = [&](BlockID blockID, double weight) {
            const Block& block = function.at(blockID);
            std::span<const SiteThatRunsCode> sites = referencesOfBody->sitesThatRunCode.span();
            size_t position = std::ranges::lower_bound(sites, blockID, { }, &SiteThatRunsCode::block) - sites.begin();
            if (verifies) [[unlikely]]
                checkNotedSites(blockID, position);
            for (; position < sites.size() && sites[position].block == blockID; ++position) {
                const SiteThatRunsCode& site = sites[position];
                auto addCallsOfEntry = [&](const auto& instruction) {
                    if (site.listsManyFunctions)
                        addCalls(instruction, weight);
                    else
                        addCallsOfSite(instruction, weight, referencesOfBody->functionsOfSite(position), site.callsUnknownCode);
                };
                if (site.indexInBlock < block.values.size())
                    addCallsOfEntry(function.at(block.values[site.indexInBlock]));
                else
                    addCallsOfEntry(block.terminator);
            }
        };

        uint32_t size = 0;
        double mass = 0;
        Vector<HeatBlockGroup, 4> blockGroups;
        auto visit = [&](BlockID blockID, double weight, double share) {
            const Block& block = function.at(blockID);
            sharePerWeight = weight > 0 ? share / weight : 0;
            uint32_t blockSize = block.values.size() + 1;
            size += blockSize;
            mass += weight * blockSize;
            blockGroups.append(HeatBlockGroup { weight, blockSize });
            if (referencesOfBody) {
                addCallsOfNotedSites(blockID, weight);
                return;
            }
            for (ValueID value : block.values)
                addCalls(function.at(value), weight);
            addCalls(block.terminator, weight);
        };
        bool hasKeptWeights = keptWeights && index < keptWeights->m_blocks.size() && !keptWeights->m_blocks[index].isEmpty();
        Vector<WeightOfBlock>* made = madeWeights ? &madeWeights->m_blocks[index] : nullptr;
        if (hasKeptWeights) {
            const Vector<WeightOfBlock>& kept = keptWeights->m_blocks[index];
            RELEASE_ASSERT(!verifies || kept == computeWeightsOfBlocks(analyses));
            for (const WeightOfBlock& entry : kept)
                visit(entry.block, entry.weight, entry.share);
            if (made)
                *made = kept;
        } else if (made) {
            *made = computeWeightsOfBlocks(analyses);
            for (const WeightOfBlock& entry : *made)
                visit(entry.block, entry.weight, entry.share);
        } else {
            const BlockFrequencies& frequencies = analyses.blockFrequencies();
            for (BlockID blockID : analyses.cfg().reversePostOrder())
                visit(blockID, frequencies.frequencyOf(blockID).predicted(), shareOfClass(frequencies.classOf(blockID)));
        }
        sharePerWeight = 1;
        if (!followsNames)
            mergeEdges(callsOfBody);
        else {
            Vector<ModuleID>& loaded = modulesLoadedBy[index];
            auto loadRuntimeModule = [&](RuntimeModuleID module) {
                if (module && module.index() < moduleOfRuntimeModule.size() && moduleOfRuntimeModule[module.index()])
                    loaded.append(moduleOfRuntimeModule[module.index()]);
            };
            auto noteLoad = [&]<typename Reference, typename... Role>(const Reference& reference, Role... role) {
                    if constexpr (std::same_as<Reference, ModuleID>) {
                        if (reference && ((role == ModuleReferenceKind::Target) && ...))
                            loaded.append(reference);
                    } else if constexpr (std::same_as<Reference, RuntimeModuleID>)
                        loadRuntimeModule(reference);
            };
            forEachValueListedOrNot(program, id, opcodesNamingModules, [&](ValueID, const Value& value) {
                walkReferences(value, noteLoad);
            });
            AOT::sort(loaded);
            removeRepeatedElements(loaded);
            // The effects of a call of a helper of the compiler are written in its entry, so no site lists the function behind it.
            forEachValueListedOrNot(program, id, { Opcode::Builtin }, [&](ValueID, const Value& value) {
                if (program.builtins().at(value.builtin()).holder != Builtins::CompilerHelpers)
                    return;
                for (const BuiltinOverload& overload : program.builtins().overloadsOf(value.builtin())) {
                    const RuntimeFunctionEntry* behind = findRuntimeFunction(program, overload.libraryFunction());
                    if (behind && behind->function && overload.flags.contains(BuiltinOverloadFlag::RunsOnlyOperandCode))
                        add(behind->function, 1);
                }
            });
            mergeEdges(callsOfBody);
        }
        if (followsNames) {
            Vector<uint32_t, 16> readsOfCallees;
            forEachValueListedOrNot(program, id, { Opcode::Call, Opcode::CallList }, [&](ValueID, const Value& call) {
                if (nameOfCallee(program, function, call))
                    readsOfCallees.append(call.operand(0).index());
            });
            AOT::sort(readsOfCallees);
            forEachValueListedOrNot(program, id, { Opcode::PropGet, Opcode::GetSlot, Opcode::GetSlotOrUndefined }, [&](ValueID read, const Value& value) {
                std::optional<size_t> position = value.key() ? names.find(value.key()) : std::nullopt;
                if (!position || std::ranges::binary_search(readsOfCallees, read.index()))
                    return;
                CalleeList callees = calleesOf(program, program.typeOf(function, read));
                if (!callees.mayRunUserCode())
                    return;
                namesReadBy[index].append(static_cast<uint32_t>(*position));
            });
            auto readHoldersOf = [&](const Type& object) {
                if (mayHaveNoLayout(object))
                    holdersReadBy[index].append(FunctionsUnderNames::holderOf({ }));
                for (LayoutID member : membersOf(program, object)) {
                    LayoutID current = member;
                    for (size_t steps = 0; current && steps < program.layouts.size(); ++steps) {
                        holdersReadBy[index].append(FunctionsUnderNames::holderOf(declaredLayoutOf(program, current)));
                        current = expectedBaseOf(program.at(current));
                    }
                }
            };
            // A read under a name that nobody knows reads what lies in the objects that the receiver may be. A key that may be a number is taken for an index.
            forEachValueListedOrNot(program, id, { Opcode::ElemGet }, [&](ValueID read, const Value& value) {
                const Type& key = program.typeOf(function, value.operand(OperandIndex::ElemGet::property));
                if (!(key.primitives & (Primitive::AnyString | Primitive::AnySymbol)) || (key.primitives & Primitive::Number) || constantKeyOf(program, key))
                    return;
                CalleeList callees = calleesOf(program, program.typeOf(function, read));
                if (!callees.mayRunUserCode())
                    return;
                readHoldersOf(program.typeOf(function, value.operand(OperandIndex::ElemGet::object)));
            });
            // What a built-in makes may later do anything with an object that it holds: it reads what lies in that object.
            forEachValueListedOrNot(program, id, opcodesThatMayEnterBuiltins, [&](ValueID, const Value& call) {
                if (!entersOnlyBuiltins(program, call))
                    return;
                call.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                    OperandSummary summary = summarizeOperand(analyses.effectContext(), function, call, operandIndex);
                    if (summary.uses == OperandUses::all() && summary.retainedIn == RetainedIn::Result)
                        readHoldersOf(program.typeOf(function, operand));
                });
            });
            AOT::sort(holdersReadBy[index]);
            removeRepeatedElements(holdersReadBy[index]);
            AOT::sort(accessorsEnteredBy[index]);
            removeRepeatedElements(accessorsEnteredBy[index]);
        }
        if (!followsNames) {
        } else if (referencesOfBody)
            functionsMadeBy[index] = referencesOfBody->otherReferences;
        else {
            forEachFunctionReference(program, id, [&](FunctionID made, FunctionReferenceKind kind) {
                if (kind == FunctionReferenceKind::Closure || kind == FunctionReferenceKind::Constructor)
                    functionsMadeBy[index].append(made);
            });
        }

        if (FunctionID idAsRead = findFunctionAsRead(&program, id)) {
            Vector<std::pair<FunctionID, double>, 8> countedByFlow;
            for (const FlowIndirectCall& call : flow.indirectCallsFrom(idAsRead)) {
                if (double numberOfCalls = estimateIndirectCalls(call))
                    countedByFlow.append({ call.callee, numberOfCalls });
            }
            AOT::stableSort(countedByFlow, { }, [](const auto& entry) {
                return entry.first.index();
            });

            size_t mergedSize = callsOfBody.size();
            auto callsCountedByWalk = [&](FunctionID callee) {
                std::span<const WeightedCall> edges = callsOfBody.span().first(mergedSize);
                auto edge = std::ranges::lower_bound(edges, graph.nodeOf(callee).index(), { }, &WeightedCall::node);
                return edge != edges.end() && edge->node == graph.nodeOf(callee).index() ? edge->weight : 0;
            };
            for (size_t position = 0; position < countedByFlow.size();) {
                FunctionID callee = countedByFlow[position].first;
                double numberOfCalls = 0;
                for (; position < countedByFlow.size() && countedByFlow[position].first == callee; ++position)
                    numberOfCalls += countedByFlow[position].second;
                double countedByWalk = callsCountedByWalk(callee);
                if (numberOfCalls > countedByWalk)
                    add(callee, numberOfCalls - countedByWalk);
            }
            if (callsOfBody.size() > mergedSize)
                mergeEdges(callsOfBody);
        }

        AOT::sort(blockGroups, std::ranges::greater { }, &HeatBlockGroup::frequency);
        size_t numberOfGroups = 0;
        for (const HeatBlockGroup& group : blockGroups) {
            if (numberOfGroups && blockGroups[numberOfGroups - 1].frequency == group.frequency)
                blockGroups[numberOfGroups - 1].size += group.size;
            else
                blockGroups[numberOfGroups++] = group;
        }
        blockGroups.shrink(numberOfGroups);
        graph.m_sizes[index] = size;
        graph.m_masses[index] = mass;
        return blockGroups;
    });
    BitVector noFunction(count);
    Vector<double> entriesPerEvent;
    auto addCallsOfNodes = [&](EstimateOf estimate) {
        bool isForClasses = estimate == EstimateOf::Classes;
        const BitVector& hasOnlySeenCallers = isForClasses ? hasOnlySeenCallersByFlow : noFunction;
        for (unsigned node = count; node < numberOfNodes; ++node)
            calls[node] = { };
        SeenCode seen;
        if (!followsNames) {
            seen.reachesEverything = true;
            if (flow.mayRunUnseenCode())
                calls[graph.unknownCodeNode().index()] = shareAmong(program, flow, graph, isCalledPerEvent, seen, callGraph.functionsCalledByUnknownCode());
        } else
            seen = findWhatSeenCodeReaches(program, callGraph, flow, graph, calls, names, functionsMadeBy, isCalledByHost, hasOnlySeenCallers, namesReadBy, accessorsEnteredBy, holdersReadBy, modulesLoadedBy, constructsUnknownValue, estimate);
        Vector<double> eventsOfHandlers(FillWith { }, count, 0.0);
        if (!isForClasses) {
            forEachIndexInParallel(loops, graph.m_numberOfNames, [&](unsigned position) {
                calls[graph.nodeOfName(position).index()] = shareAmong(program, flow, graph, isCalledPerEvent, seen, names.at(position));
            });
            for (size_t handler : isCalledPerEvent)
                eventsOfHandlers[handler] = HeatModel::eventsPerRun;
        } else {
            forEachIndexInParallel(loops, count, [&](unsigned index) {
                if (!seen.isReached.get(index)) {
                    calls[index].clear();
                    return;
                }
                calls[index].removeAllMatching([&](const WeightedCall& call) {
                    return call.node < count && !seen.isReached.get(call.node);
                });
            });
            // A handler gets events only as surely as whoever registers it is reached. What is reached as surely as a rare edge is taken counts as reached.
            Vector<double> sureness = estimateHowSurelyNodesAreReached(program, flow, graph, calls, names, seen, hasOnlySeenCallers, modulesLoadedBy);
            for (const FlowIndirectCall& call : flow.indirectCalls()) {
                if (call.times.predicted() != BuiltinCallbackTimes::PerEvent)
                    continue;
                double howSurely = call.caller && !program.entries.isEmpty() ? sureness[call.caller.index()] : 1.0;
                if (howSurely >= frequencyConstants.rareEdgeShare)
                    howSurely = 1;
                eventsOfHandlers[call.callee.index()] = std::max(eventsOfHandlers[call.callee.index()], HeatModel::eventsPerRun * howSurely);
            }
            entriesPerEvent = estimateEntriesPerEvent(program, graph, calls, names, isCalledPerEvent, seen, hasOnlySeenCallers, eventsOfHandlers);
        }
        calls[graph.hostNode().index()] = collectHostCalls(program, callGraph, flow, graph, calls, isCalledPerEvent, isCountedByProgram, seen, loops, estimate, hasOnlySeenCallers, entriesPerEvent, eventsOfHandlers);
    };
    auto fillEdges = [&](HeatGraph& filled) {
        filled.m_firstSuccessor.fill(0u, numberOfNodes + 1);
        for (unsigned index = 0; index < numberOfNodes; ++index)
            filled.m_firstSuccessor[index + 1] = filled.m_firstSuccessor[index] + calls[index].size();
        growInParallel(loops, filled.m_successors, filled.m_firstSuccessor[numberOfNodes]);
        growInParallel(loops, filled.m_multipliers, filled.m_firstSuccessor[numberOfNodes]);
        forEachIndexInParallel(loops, numberOfNodes, [&](unsigned index) {
            uint32_t position = filled.m_firstSuccessor[index];
            for (const WeightedCall& call : calls[index]) {
                filled.m_successors[position] = HeatNodeID(call.node);
                filled.m_multipliers[position++] = call.weight;
            }
        });
        groupByIndex(numberOfNodes, filled.m_firstPredecessor, filled.m_predecessors, [&](const auto& add) {
            for (unsigned index = 0; index < numberOfNodes; ++index) {
                for (HeatNodeID successor : filled.successors(HeatNodeID(index)))
                    add(successor.index(), HeatNodeID(index));
            }
        });
    };
    addCallsOfNodes(EstimateOf::Ranks);
    fillEdges(graph);
    uint64_t numberOfValues = 0;
    for (uint32_t size : graph.m_sizes)
        numberOfValues += size;
    if (graphOfClasses && followsNames && numberOfValues <= maxValuesOfOrdinaryProgram(build)) {
        addCallsOfNodes(EstimateOf::Classes);
        graphOfClasses->m_sizes = graph.m_sizes;
        graphOfClasses->m_numberOfNames = graph.m_numberOfNames;
        graphOfClasses->m_entriesPerEvent = WTF::move(entriesPerEvent);
        graphOfClasses->m_isPresent = graph.m_isPresent;
        fillEdges(*graphOfClasses);
    }
    build.destroyLater(WTF::move(calls));
    build.destroyLater(WTF::move(namesReadBy));
    build.destroyLater(WTF::move(accessorsEnteredBy));
    build.destroyLater(WTF::move(holdersReadBy));
    build.destroyLater(WTF::move(modulesLoadedBy));
    build.destroyLater(WTF::move(functionsMadeBy));
    return graph;
}

uint64_t maxValuesOfOrdinaryProgram(const BuildContext& build)
{
    return static_cast<uint64_t>(HeatModel::warmCodeLimitsInVeryLargeProgram) * build.limit(LimitID::MaxWarmCodeValues);
}

bool isVeryLargeProgram(const Program& program, const BuildContext& build)
{
    uint64_t numberOfValues = 0;
    for (unsigned index = 0; index < program.functions.size(); ++index)
        numberOfValues += numberOfValuesIn(program, FunctionID(index));
    return numberOfValues > maxValuesOfOrdinaryProgram(build);
}

Vector<double> estimateActivations(HeatGraph& graph)
{
    AOT_THRESHOLD(unsigned, maxDepthOfCallCycles, 16);
    WTF::CycleForest<HeatGraph> cycles(graph, [&](std::span<const HeatNodeID> members, const Vector<HeatNodeID, 2>&, const auto& isInCycle) {
        HeatNodeID header;
        int64_t mostPaths = 0;
        for (HeatNodeID member : members) {
            int64_t paths = static_cast<int64_t>(std::ranges::count_if(graph.predecessors(member), isInCycle)) * std::ranges::count_if(graph.successors(member), isInCycle);
            if (header && (paths < mostPaths || (paths == mostPaths && header < member)))
                continue;
            header = member;
            mostPaths = paths;
        }
        return header;
    }, maxDepthOfCallCycles);
    Vector<double> result = WTF::estimateMultipliedFrequencies(graph, cycles, [&](HeatNodeID node, unsigned successorIndex) {
        return graph.multiplierOf(node, successorIndex);
    }, frequencyConstants);
    result.shrink(graph.numberOfFunctions());
    return result;
}

void HeatGraph::dump(PrintStream& out) const
{
    auto printSuccessors = [&](Node node) {
        std::span<const Node> callees = successors(node);
        for (unsigned index = 0; index < callees.size(); ++index) {
            if (FunctionID function = functionOf(callees[index]))
                out.print(" f", function.index());
            else if (callees[index] == unknownCodeNode())
                out.print(" unknown");
            else
                out.print(" name", callees[index].index() - nodeOfName(0).index());
            out.print("*", multiplierOf(node, index));
        }
        out.println();
    };
    for (unsigned index = 0; index < numberOfFunctions(); ++index) {
        if (!m_isPresent[index])
            continue;
        FunctionID function(index);
        out.print("f", index, " size ", sizeOf(function));
        if (!m_masses.isEmpty())
            out.print(" mass ", massOf(function));
        out.print(":");
        printSuccessors(nodeOf(function));
    }
    out.print("host:");
    printSuccessors(hostNode());
    out.print("unknown:");
    printSuccessors(unknownCodeNode());
    for (unsigned position = 0; position < m_numberOfNames; ++position) {
        if (successors(nodeOfName(position)).empty())
            continue;
        out.print("name", position, ":");
        printSuccessors(nodeOfName(position));
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
