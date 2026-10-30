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
#include "AOTHeldOperands.h"
#include "AOTIR.h"
#include "AOTLargeVectors.h"
#include "AOTLayout.h"
#include "AOTOpcodesNamingRecords.h"
#include "AOTPropertyAccessElements.h"
#include "AOTSort.h"
#include "AOTThreshold.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <concepts>
#include <optional>
#include <tuple>
#include <wtf/Atomics.h>
#include <wtf/BitVector.h>
#include <wtf/CycleForest.h>
#include <wtf/FrequencyEstimator.h>
#include <wtf/HashMap.h>
#include <wtf/Hasher.h>
#include <wtf/Lock.h>
#include <wtf/PrintStream.h>
#include <wtf/ThreadSafeRefCounted.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(HeatModel::eventsPerRun == frequencyConstants.maxMultiplier);

namespace {

struct WeightedCall {
    uint32_t node;
    double weight;
    // How surely the call is made once its function is entered, where an even branch counts as taken.
    double share { 0 };
    // The caller also calls a long set that holds the node. The weight and the share are those of both together.
    bool isAlsoInLongSet { false };
};

using WeightedCallList = Vector<WeightedCall, 4>;

struct SharedCallList : ThreadSafeRefCounted<SharedCallList> {
    explicit SharedCallList(WeightedCallList&& calls)
        : calls(WTF::move(calls))
    {
    }

    WeightedCallList calls;
};

class SharedCallLists {
public:
    Ref<SharedCallList> intern(WeightedCallList&& calls)
    {
        Hasher hasher;
        for (const WeightedCall& call : calls)
            add(hasher, call.node, call.weight, call.share, call.isAlsoInLongSet);
        auto isSame = [](const WeightedCall& a, const WeightedCall& b) {
            return a.node == b.node && a.weight == b.weight && a.share == b.share && a.isAlsoInLongSet == b.isAlsoInLongSet;
        };

        Locker locker { m_lock };
        auto& lists = m_listsByHash.add(hasher.hash(), Vector<Ref<SharedCallList>, 1> { }).iterator->value;
        for (const Ref<SharedCallList>& list : lists) {
            if (std::ranges::equal(list->calls, calls, isSame))
                return list;
        }
        calls.shrinkToFit();
        lists.append(adoptRef(*new SharedCallList(WTF::move(calls))));
        return lists.last();
    }

private:
    Lock m_lock;
    UncheckedKeyHashMap<unsigned, Vector<Ref<SharedCallList>, 1>, DefaultHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>> m_listsByHash;
};

// The calls of one node. n functions that each may call any of the n have n equal lists of n calls, so nodes with equal long lists share one.
class CallsOfNode {
public:
    std::span<const WeightedCall> span() const LIFETIME_BOUND { return m_shared ? m_shared->calls.span() : m_own.span(); }
    auto begin() const LIFETIME_BOUND { return span().begin(); }
    auto end() const LIFETIME_BOUND { return span().end(); }
    size_t size() const { return span().size(); }
    const SharedCallList* sharedList() const { return m_shared.get(); }

    CallsOfNode& operator=(WeightedCallList&& calls)
    {
        m_shared = nullptr;
        m_own = WTF::move(calls);
        return *this;
    }

    void take(WeightedCallList&& calls, SharedCallLists& sharedLists)
    {
        AOT_THRESHOLD(size_t, minCallsOfSharedList, 16);
        if (calls.size() < minCallsOfSharedList) {
            *this = WTF::move(calls);
            return;
        }
        m_own = { };
        m_shared = sharedLists.intern(WTF::move(calls));
    }

    void clear() { *this = WeightedCallList(); }

    void removeAllMatching(NOESCAPE const Invocable<bool(const WeightedCall&)> auto& matches)
    {
        if (m_shared) {
            if (std::ranges::none_of(m_shared->calls, matches))
                return;
            *this = WeightedCallList(m_shared->calls);
        }
        m_own.removeAllMatching(matches);
    }

private:
    WeightedCallList m_own;
    RefPtr<SharedCallList> m_shared;
};

enum class CallOfHost : uint8_t { StaysUnlessCalleeRunsOnce, IsReplaced };

// Ranks say which of two functions runs more often: a call of a name shares its weight among the functions under the name.
// Classes say whether a function runs at all, and about how often: what a call of a name may enter is entered as a loop would enter it, or as often as events come.
enum class EstimateOf : uint8_t { Ranks, Classes };

} // anonymous namespace


static void mergeEdges(WeightedCallList& calls)
{
    AOT::stableSortInLine(calls, [](const WeightedCall& a, const WeightedCall& b) {
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
    CalleeList list = calleesOfCall(program, call);
    std::optional<ProvenValue<Callees>> callees = list.all();
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

private:
    Vector<PropertyKeyID> m_names;
    Vector<uint32_t> m_first;
    Vector<FunctionID> m_functions;
    BitVector m_isUnderSomeName;
    Vector<uint32_t> m_firstInHolder;
    Vector<FunctionID> m_functionsInHolders;
};

} // anonymous namespace

// A value that may be one of more functions than this tells nothing about what lies under the name.
AOT_THRESHOLD(size_t, maxFunctionsOfStoredValue, 8);

static_assert(OperandIndex::PropSet::object == OperandIndex::PropDefine::object && OperandIndex::PropSet::value == OperandIndex::PropDefine::value);
static_assert(OperandIndex::PutSlot::object == OperandIndex::PutSlotOrAdd::object && OperandIndex::PutSlot::value == OperandIndex::PutSlotOrAdd::value);

FunctionsUnderNames::FunctionsUnderNames(const Program& program, const CallGraph& callGraph, const ParallelLoops& loops)
{
    auto packedKeyOf = [](PropertyKeyID name, FunctionID function) {
        return static_cast<uint64_t>(name.index()) << 32 | function.index();
    };
    auto isWanted = [&](FunctionID function) {
        return program.at(function).hasBody() && callGraph.callerSet(function) != CallerSet::Complete;
    };
    // Behind every name in the order of the packed keys.
    constexpr uint64_t namesHolder = 1ull << 63;
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
                }
            }
        }
    }
    sortAndRemoveDuplicates(loops, allStored);
    size_t numberUnderNames = std::ranges::lower_bound(allStored, namesHolder) - allStored.begin();
    m_firstInHolder.fill(0u, program.layouts.size() + 2);
    m_functionsInHolders.reserveInitialCapacity(allStored.size() - numberUnderNames);
    for (uint64_t stored : allStored.span().subspan(numberUnderNames)) {
        ++m_firstInHolder[static_cast<uint32_t>((stored & ~namesHolder) >> 32) + 1];
        m_functionsInHolders.append(FunctionID(static_cast<uint32_t>(stored)));
    }
    for (size_t holder = 1; holder < m_firstInHolder.size(); ++holder)
        m_firstInHolder[holder] += m_firstInHolder[holder - 1];
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

static bool readsUnderName(Opcode opcode) { return accessOf(opcode) == Access::Read && PayloadWord::key(opcode); }

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
    const Value& callee = function.at(call.operand(0));
    if (readsUnderName(callee.opcode))
        return callee.key();
    if (callee.opcode != Opcode::ElemGet)
        return { };
    ConstantKey key = constantKeyOf(program, program.typeOf(function, callee.operand(OperandIndex::ElemGet::property)));
    return key.kind == ConstantKeyKind::Symbol ? key.symbol : PropertyKeyID();
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

// The record of a module that only compiled code evaluates has no body.
static bool isModuleBodyFunction(const Function& function) { return function.module && function.isModuleBody(); }

// A module is loaded by the entry, or by a module that names it. A closure is made by a body that runs. A call of a callee that nobody knows reaches what has been made and may stand there.
static SeenCode findWhatSeenCodeReaches(const Program& program, const CallGraph& callGraph, const FlowResults& flow, const HeatGraph& graph, const ParallelArray<CallsOfNode>& calls, const FunctionsUnderNames& names, const Vector<Vector<FunctionID>>& functionsMadeBy, const BitVector& isCalledByHost, const BitVector& hasOnlySeenCallers, const Vector<Vector<uint32_t>>& namesReadBy, const Vector<Vector<uint32_t>>& holdersReadBy, const Vector<Vector<ModuleID>>& modulesLoadedBy, const Vector<uint8_t>& constructsUnknownValue, EstimateOf estimate)
{
    unsigned count = graph.numberOfFunctions();
    bool isForClasses = estimate == EstimateOf::Classes;
    SeenCode seen;
    BitVector& isReached = seen.isReached;
    isReached.ensureSize(calls.size());
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
    BitVector isLoaded(program.modules.size());
    BitVector hasCode(program.modules.size());
    for (unsigned index = 0; index < count; ++index) {
        if (program.functions[index].hasBody() && isModuleBodyFunction(program.functions[index]))
            hasCode.quickSet(program.functions[index].module.index());
    }
    auto moduleIsLoaded = [&](FunctionID function) {
        ModuleID module = program.at(function).module;
        return !module || !hasCode.quickGet(module.index()) || isLoaded.quickGet(module.index());
    };
    auto exists = [&](FunctionID function) {
        return hasMaker.quickGet(function.index()) ? isMade.quickGet(function.index()) : moduleIsLoaded(function);
    };
    auto isBodyOfModule = [&](FunctionID function) {
        return isModuleBodyFunction(program.at(function));
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
    BitVector isUnderNameThatIsCalled(count);
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
        if (!isStartedByHost.quickGet(function.index()) && !isUnderNameThatIsCalled.quickGet(function.index()) && !(isReached.quickGet(graph.unknownCodeNode().index()) && mayStandWhereValueIsCalled(function)))
            return;
        if (exists(function))
            reach(function.index());
        else if (!hasMaker.quickGet(function.index()) && !isWaiting.quickSet(function.index()))
            functionsWaitingFor[program.at(function).module.index()].append(function);
    };
    Vector<ModuleID> modulesToLoad;
    auto load = [&](ModuleID first) {
        modulesToLoad.append(first);
        while (!modulesToLoad.isEmpty()) {
            ModuleID module = modulesToLoad.takeLast();
            if (isLoaded.quickSet(module.index()))
                continue;
            if (FunctionID body = program.at(module).body)
                reach(body.index());
            for (const ModuleRequest& request : program.at(module).requests)
                modulesToLoad.append(request.target);
            for (FunctionID waiting : std::exchange(functionsWaitingFor[module.index()], { }))
                reachIfCalled(waiting);
        }
    };
    for (const Entry& entry : program.entries)
        load(entry.module);
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
    auto followCallsOf = [&](uint32_t node) {
        for (const WeightedCall& call : calls[node]) {
            if (isForClasses && call.node < count && hasMaker.quickGet(call.node) && !isMade.quickGet(call.node)) {
                isAwaited.quickSet(call.node);
                continue;
            }
            reach(call.node);
        }
    };
    while (!worklist.isEmpty()) {
        uint32_t node = worklist.takeLast();
        if (node >= graph.numNodes()) {
            followCallsOf(node);
            continue;
        }
        if (node == graph.unknownCodeNode().index()) {
            for (size_t function : hasEscaped)
                reachIfCalled(FunctionID(function));
            continue;
        }
        if (node >= count) {
            for (FunctionID function : names.at(node - graph.nodeOfName(0).index())) {
                if (hasOnlySeenCallers.get(function.index()))
                    continue;
                isUnderNameThatIsCalled.quickSet(function.index());
                reachIfCalled(function);
            }
            continue;
        }
        FunctionID function(node);
        constructsValue |= !!constructsUnknownValue[node];
        for (ModuleID module : modulesLoadedBy[node])
            load(module);
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
        for (uint32_t holder : holdersReadBy[node]) {
            if (isReadUnderUnknownKey.quickSet(holder))
                continue;
            for (FunctionID read : names.inHolder(holder)) {
                isUnderNameReadAsValue.quickSet(read.index());
                reachIfCalled(read);
            }
        }
        followCallsOf(node);
        if (isBodyOfModule(function))
            load(program.at(function).module);
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
    seen.hasSiteThatMayEnter = WTF::move(isUnderNameThatIsCalled);
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
static Vector<double> estimateHowSurelyNodesAreReached(const Program& program, const FlowResults& flow, const HeatGraph& graph, const ParallelArray<CallsOfNode>& calls, const FunctionsUnderNames& names, const SeenCode& seen, const BitVector& hasOnlySeenCallers, const Vector<Vector<ModuleID>>& modulesLoadedBy)
{
    unsigned count = graph.numberOfFunctions();
    Vector<double> sureness(FillWith { }, calls.size(), 0.0);
    Vector<std::pair<double, uint32_t>> heap;
    auto enter = [&](uint32_t node, double howSurely) {
        if (howSurely <= sureness[node] || (node < count && !seen.reaches(FunctionID(node))))
            return;
        sureness[node] = howSurely;
        heap.append({ howSurely, node });
        std::ranges::push_heap(heap);
    };
    Vector<double> surenessOfModules(FillWith { }, program.modules.size(), 0.0);
    Vector<ModuleID> modulesToLoad;
    auto load = [&](ModuleID first, double howSurely) {
        modulesToLoad.append(first);
        while (!modulesToLoad.isEmpty()) {
            ModuleID module = modulesToLoad.takeLast();
            if (howSurely <= surenessOfModules[module.index()])
                continue;
            surenessOfModules[module.index()] = howSurely;
            if (FunctionID body = program.at(module).body)
                enter(body.index(), howSurely);
            for (const ModuleRequest& request : program.at(module).requests)
                modulesToLoad.append(request.target);
        }
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
        load(entry.module, 1);
    while (!heap.isEmpty()) {
        std::ranges::pop_heap(heap);
        auto [howSurely, node] = heap.takeLast();
        if (howSurely < sureness[node])
            continue;
        if (node == graph.hostNode().index())
            continue;
        if (node >= graph.numNodes()) {
            for (const WeightedCall& call : calls[node])
                enter(call.node, howSurely);
            continue;
        }
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
            load(module, howSurely);
        if (isModuleBodyFunction(program.at(function)))
            load(program.at(function).module, howSurely);
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
static Vector<double> estimateEntriesPerEvent(const Program& program, const HeatGraph& graph, const ParallelArray<CallsOfNode>& calls, const FunctionsUnderNames& names, const BitVector& isCalledPerEvent, const SeenCode& seen, const BitVector& hasOnlySeenCallers, const Vector<double>& eventsOfHandlers)
{
    unsigned count = graph.numberOfFunctions();
    Vector<double> entries(FillWith { }, calls.size(), 0.0);
    Vector<uint8_t> isReachedByKnownCalls(FillWith { }, calls.size(), static_cast<uint8_t>(0));
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
        if (node < count || node >= graph.numNodes()) {
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

static WeightedCallList collectHostCalls(const Program& program, const CallGraph& callGraph, const FlowResults& flow, const HeatGraph& graph, const ParallelArray<CallsOfNode>& callsOfProgram, const BitVector& isCalledPerEvent, const Vector<uint8_t>& isCountedByProgram, const SeenCode& seen, const ParallelLoops& loops, EstimateOf estimate, const BitVector& hasOnlySeenCallers, const Vector<double>& entriesPerEvent, const Vector<double>& eventsOfHandlers)
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
    struct HostCallee {
        FunctionID function;
        std::optional<WeightedCall> call;
    };
    Vector<HostCallee> calleesWithBody = collectInParallel<HostCallee>(loops, callees.size(), [&](unsigned position, Vector<HostCallee>& found) {
        FunctionID callee = callees[position];
        if (!program.at(callee).hasBody() || !seen.reaches(callee) || hasOnlySeenCallers.get(callee.index()))
            return;
        if (isCountedByProgram[callee.index()] && !comesWithEvents(callee))
            found.append({ callee, std::nullopt });
        else
            found.append({ callee, callOf(callee) });
    });
    WeightedCallList calls;
    calls.reserveInitialCapacity(calleesWithBody.size());
    Vector<FunctionID> calleesCountedByProgram;
    for (const HostCallee& callee : calleesWithBody) {
        if (callee.call)
            calls.append(*callee.call);
        else
            calleesCountedByProgram.append(callee.function);
    }
    if (calleesCountedByProgram.isEmpty())
        return calls;

    Vector<uint8_t> isReached(FillWith { }, callsOfProgram.size(), static_cast<uint8_t>(0));
    auto reach = [&](uint32_t node) {
        return !AtomicRef(isReached[node]).load(std::memory_order_relaxed) && !AtomicRef(isReached[node]).exchange(1, std::memory_order_relaxed);
    };
    auto visitCalleesOf = [&](uint32_t node, Vector<uint32_t>& found) {
        for (const WeightedCall& call : callsOfProgram[node]) {
            if (reach(call.node))
                found.append(call.node);
        }
    };
    // The set of reachable nodes does not depend on the visit order.
    AOT_THRESHOLD(size_t, minNodesForThreadPool, 512);
    Vector<uint32_t> worklist;
    auto drainWorklist = [&] {
        while (!worklist.isEmpty()) {
            if (worklist.size() < minNodesForThreadPool) {
                visitCalleesOf(worklist.takeLast(), worklist);
                continue;
            }
            worklist = collectInParallel<uint32_t>(loops, worklist.size(), [&](unsigned position, Vector<uint32_t>& found) {
                visitCalleesOf(worklist[position], found);
            });
        }
    };
    for (const WeightedCall& call : calls) {
        if (reach(call.node))
            worklist.append(call.node);
    }
    drainWorklist();
    size_t numberOfSortedCalls = calls.size();
    for (FunctionID callee : calleesCountedByProgram) {
        if (!reach(graph.nodeOf(callee).index()))
            continue;
        calls.append(callOf(callee));
        worklist.append(graph.nodeOf(callee).index());
        drainWorklist();
    }
    if (calls.size() > numberOfSortedCalls)
        AOT::sortInLine(calls, { }, &WeightedCall::node);
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
    // A long callee set has a list behind those of the nodes. A caller has one call of the set, with the weight of each member, and the set has a call of weight 1 of each member.
    const unsigned numberOfLongSets = callGraph.numberOfLongSets();
    ParallelArray<CallsOfNode> calls(loops, numberOfNodes + numberOfLongSets, [](unsigned) {
        return CallsOfNode();
    });
    SharedCallLists sharedCallLists;
    Vector<uint8_t> isCountedByProgram(FillWith { }, count, static_cast<uint8_t>(0));
    BitVector isCalledOnceAfterReturn;
    BitVector isCalledPerEvent;
    for (const FlowIndirectCall& call : flow.indirectCalls()) {
        if (call.times.predicted() == BuiltinCallbackTimes::Later)
            isCalledOnceAfterReturn.set(call.callee.index());
        if (call.times.predicted() == BuiltinCallbackTimes::PerEvent)
            isCalledPerEvent.set(call.callee.index());
    }
    forEachIndexInParallel(loops, numberOfLongSets, [&](unsigned set) {
        WeightedCallList members;
        for (FunctionID member : callGraph.membersOfLongSet(set)) {
            if (program.at(member).hasBody() && !isCalledPerEvent.get(member.index()))
                members.append(WeightedCall { graph.nodeOf(member).index(), 1.0, 1.0 });
        }
        calls[numberOfNodes + set] = WTF::move(members);
    });
    auto isInLongSet = [&](uint32_t set, uint32_t node) {
        return std::ranges::binary_search(calls[numberOfNodes + set].span(), node, { }, &WeightedCall::node);
    };
    Vector<uint8_t> isLongSetCalled(FillWith { }, numberOfLongSets, static_cast<uint8_t>(0));
    Lock lockOfLongSetsInCommon;
    UncheckedKeyHashMap<uint64_t, bool, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> haveMemberInCommon;
    auto longSetsHaveMemberInCommon = [&](uint32_t a, uint32_t b) {
        uint64_t key = static_cast<uint64_t>(std::min(a, b)) << 32 | std::max(a, b);
        {
            Locker locker { lockOfLongSetsInCommon };
            if (auto known = haveMemberInCommon.find(key); known != haveMemberInCommon.end())
                return known->value;
        }
        std::span<const WeightedCall> first = calls[numberOfNodes + a].span();
        std::span<const WeightedCall> second = calls[numberOfNodes + b].span();
        bool answer = false;
        while (!first.empty() && !second.empty() && !answer) {
            if (first.front().node == second.front().node)
                answer = true;
            else if (first.front().node < second.front().node)
                first = first.subspan(1);
            else
                second = second.subspan(1);
        }
        Locker locker { lockOfLongSetsInCommon };
        haveMemberInCommon.add(key, answer);
        return answer;
    };
    // What the callees of a long set do with an operand is the same for all calls of one form.
    Lock lockOfUsesByLongSets;
    UncheckedKeyHashMap<uint64_t, OperandUses, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> usesByLongSets;
    auto usesByLongSet = [&](uint32_t set, const Value& call, uint32_t operandIndex, NOESCAPE const Invocable<OperandUses()> auto& summarize) {
        constexpr unsigned bitsOfOperandIndex = 15;
        static_assert(sizeof(Opcode) <= 2);
        // The type of an operand whose contents are retained counts too.
        if (operandIndex >= 1u << bitsOfOperandIndex || declaredBehaviorOf(call.opcode, operandIndex).uses.contains(OperandUse::RetainsContents))
            return summarize();
        uint64_t key = static_cast<uint64_t>(set) << 32 | static_cast<uint64_t>(call.opcode) << (bitsOfOperandIndex + 1) | operandIndex << 1 | isHeldOperand(call, operandIndex);
        {
            Locker locker { lockOfUsesByLongSets };
            if (auto known = usesByLongSets.find(key); known != usesByLongSets.end())
                return known->value;
        }
        OperandUses uses = summarize();
        Locker locker { lockOfUsesByLongSets };
        usesByLongSets.add(key, uses);
        return uses;
    };
    const bool verifies = build.options().verifiesAfterEveryPass;
    const EntriesThatRunNoCode entriesThatRunNoCode = findEntriesThatRunNoCode();
    graph.m_blockGroups = ParallelArray<Vector<HeatBlockGroup, 4>>(loops, count, largeFunctions(program, loops).span(), [&](unsigned index) -> Vector<HeatBlockGroup, 4> {
        FunctionID id(index);
        const Function& function = program.at(id);
        if (!function.hasBody()) {
            graph.m_isPresent[index] = false;
            return { };
        }
        WeightedCallList callsOfBody;
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
        auto addCallsBesideCandidates = [&](const auto& instruction, double weight, bool callsUnknownCode, std::optional<uint32_t> longSetOfCallees = std::nullopt) {
            if (callsUnknownCode)
                addUnknownCallee(instruction, weight);
            instruction.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                auto summarize = [&] {
                    return summarizeOperand(analyses.effectContext(), function, instruction, operandIndex, UsesAskedFor::AllButPassesToUserCode).uses;
                };
                OperandUses uses;
                if constexpr (std::same_as<std::remove_cvref_t<decltype(instruction)>, Value>)
                    uses = longSetOfCallees ? usesByLongSet(*longSetOfCallees, instruction, operandIndex, summarize) : summarize();
                else
                    uses = summarize();
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
        auto addCallsOfSite = [&](const auto& instruction, double weight, std::span<const FunctionID> candidates, bool callsUnknownCode) {
            for (FunctionID candidate : candidates)
                add(candidate, weight / candidates.size());
            addCallsBesideCandidates(instruction, weight, callsUnknownCode);
        };
        auto addCallsOfLongSet = [&](const auto& instruction, double weight) {
            CalleeSetID id = numberOfLongSets ? longCalleeSetRunBy(program, instruction) : CalleeSetID();
            std::optional<uint32_t> set = id ? callGraph.findLongSet(id) : std::nullopt;
            if (!set)
                return false;
            double weightOfMember = weight / program.at(id).written().functions.size();
            callsOfBody.append(WeightedCall { numberOfNodes + *set, weightOfMember, shareOf(weightOfMember) });
            AtomicRef(isLongSetCalled[*set]).store(1, std::memory_order_relaxed);
            // A call that first looks its callee up in a table hands its operands to the lookup.
            bool handsOperandsToSet = false;
            if constexpr (std::same_as<std::remove_cvref_t<decltype(instruction)>, Value>)
                handsOperandsToSet = calleeSetOfCall(program, instruction) == id;
            addCallsBesideCandidates(instruction, weight, !program.at(id).listsEveryCallee(), handsOperandsToSet ? set : std::nullopt);
            return true;
        };
        // A member that the body also calls some other way has a weight of its own: all that comes to it, added up in the order of the sites. The call of the set stays for the other members.
        // A body that calls two sets with a member in common lists the members as it would list those of short sets.
        auto addUpCallsOfMembersCalledOtherwise = [&] {
            if (!numberOfLongSets)
                return;
            Vector<uint32_t, 4> sets;
            for (const WeightedCall& call : callsOfBody) {
                if (call.node >= numberOfNodes && !sets.contains(call.node - numberOfNodes))
                    sets.append(call.node - numberOfNodes);
            }
            if (sets.isEmpty())
                return;
            bool setsHaveMemberInCommon = false;
            for (size_t first = 0; first < sets.size() && !setsHaveMemberInCommon; ++first) {
                for (size_t second = first + 1; second < sets.size() && !setsHaveMemberInCommon; ++second)
                    setsHaveMemberInCommon = longSetsHaveMemberInCommon(sets[first], sets[second]);
            }
            if (setsHaveMemberInCommon) {
                WeightedCallList listed;
                for (const WeightedCall& call : callsOfBody) {
                    if (call.node < numberOfNodes) {
                        listed.append(call);
                        continue;
                    }
                    for (const WeightedCall& member : calls[call.node])
                        listed.append(WeightedCall { member.node, call.weight, call.share });
                }
                callsOfBody = WTF::move(listed);
                return;
            }
            WeightedCallList addedUp;
            auto isAddedUp = [&](uint32_t node) {
                return addedUp.containsIf([&](const WeightedCall& call) { return call.node == node; });
            };
            for (size_t index = 0; index < callsOfBody.size(); ++index) {
                uint32_t node = callsOfBody[index].node;
                if (node >= count || isAddedUp(node))
                    continue;
                size_t position = sets.findIf([&](uint32_t set) { return isInLongSet(set, node); });
                if (position == notFound)
                    continue;
                // An earlier sum holds what came through the set.
                bool hasEarlierSum = callsOfBody.containsIf([&](const WeightedCall& call) { return call.node == node && call.isAlsoInLongSet; });
                std::optional<WeightedCall> sum;
                for (const WeightedCall& call : callsOfBody) {
                    if (call.node != node && (hasEarlierSum || call.node != numberOfNodes + sets[position]))
                        continue;
                    if (!sum) {
                        sum = WeightedCall { node, call.weight, call.share, true };
                        continue;
                    }
                    sum->weight += call.weight;
                    sum->share = std::max(sum->share, call.share);
                }
                addedUp.append(*sum);
            }
            if (addedUp.isEmpty())
                return;
            callsOfBody.removeAllMatching([&](const WeightedCall& call) { return isAddedUp(call.node); });
            callsOfBody.appendVector(addedUp);
        };
        auto mergeCallsOfBody = [&] {
            addUpCallsOfMembersCalledOtherwise();
            mergeEdges(callsOfBody);
        };
        auto addCalls = [&](const auto& instruction, double weight) {
            if (entriesThatRunNoCode.contains(instruction) || addCallsOfLongSet(instruction, weight))
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
            mergeCallsOfBody();
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
            mergeCallsOfBody();
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
            AOT::stableSortInLine(countedByFlow, { }, [](const auto& entry) {
                return entry.first.index();
            });

            size_t mergedSize = callsOfBody.size();
            auto callsCountedByWalk = [&](FunctionID callee) {
                std::span<const WeightedCall> edges = callsOfBody.span().first(mergedSize);
                auto edge = std::ranges::lower_bound(edges, graph.nodeOf(callee).index(), { }, &WeightedCall::node);
                if (edge != edges.end() && edge->node == graph.nodeOf(callee).index())
                    return edge->weight;
                for (; !edges.empty() && edges.back().node >= numberOfNodes; edges = edges.first(edges.size() - 1)) {
                    if (isInLongSet(edges.back().node - numberOfNodes, graph.nodeOf(callee).index()))
                        return edges.back().weight;
                }
                return 0.0;
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
                mergeCallsOfBody();
        }

        AOT::sortInLine(blockGroups, std::ranges::greater { }, &HeatBlockGroup::frequency);
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
        calls[index].take(WTF::move(callsOfBody), sharedCallLists);
        return blockGroups;
    });
    forEachIndexInParallel(loops, numberOfLongSets, [&](unsigned set) {
        if (!isLongSetCalled[set])
            return;
        for (const WeightedCall& member : calls[numberOfNodes + set]) {
            if (flow.runsAtMostOnce(program, FunctionID(member.node)))
                AtomicRef(isCountedByProgram[member.node]).store(1, std::memory_order_relaxed);
        }
    });
    BitVector noFunction(count);
    Vector<double> entriesPerEvent;
    auto addCallsOfNodes = [&](EstimateOf estimate) {
        bool isForClasses = estimate == EstimateOf::Classes;
        const BitVector& hasOnlySeenCallers = isForClasses ? hasOnlySeenCallersByFlow : noFunction;
        for (unsigned node = count; node < numberOfNodes; ++node)
            calls[node].clear();
        SeenCode seen;
        if (!followsNames) {
            seen.reachesEverything = true;
            if (flow.mayRunUnseenCode())
                calls[graph.unknownCodeNode().index()] = shareAmong(program, flow, graph, isCalledPerEvent, seen, callGraph.functionsCalledByUnknownCode());
        } else
            seen = findWhatSeenCodeReaches(program, callGraph, flow, graph, calls, names, functionsMadeBy, isCalledByHost, hasOnlySeenCallers, namesReadBy, holdersReadBy, modulesLoadedBy, constructsUnknownValue, estimate);
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
            forEachIndexInParallel(loops, numberOfLongSets, [&](unsigned set) {
                calls[numberOfNodes + set].removeAllMatching([&](const WeightedCall& call) {
                    return !seen.isReached.get(call.node);
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
    // The calls of long sets come last in a list.
    auto numberOfCallsOfNodes = [&](unsigned index) {
        std::span<const WeightedCall> list = calls[index].span();
        size_t size = list.size();
        while (size && list[size - 1].node >= numberOfNodes)
            --size;
        return size;
    };
    auto callsOfNodes = [&](unsigned index) {
        return calls[index].span().first(numberOfCallsOfNodes(index));
    };
    auto forEachEdgeOf = [&](unsigned index, const auto& functor) {
        for (const WeightedCall& call : callsOfNodes(index)) {
            if (!call.isAlsoInLongSet)
                functor(call);
        }
    };
    auto fillLongSets = [&](HeatGraph& filled) {
        if (!numberOfLongSets)
            return;
        auto forEachCallOfLongSet = [&](const auto& functor) {
            for (unsigned index = 0; index < numberOfNodes; ++index) {
                for (const WeightedCall& call : calls[index].span().subspan(numberOfCallsOfNodes(index)))
                    functor(index, call);
            }
        };
        groupByIndex(numberOfNodes, filled.m_firstLongSetOf, filled.m_longSetsOf, [&](const auto& add) {
            forEachCallOfLongSet([&](unsigned index, const WeightedCall& call) {
                add(index, HeatCallOfLongSet { call.node - numberOfNodes, call.weight });
            });
        });
        groupByIndex(numberOfNodes, filled.m_firstOtherCallOfMember, filled.m_otherCallsOfMembers, [&](const auto& add) {
            for (unsigned index = 0; index < count; ++index) {
                std::span<const WeightedCall> callsOfLongSets = calls[index].span().subspan(numberOfCallsOfNodes(index));
                if (callsOfLongSets.empty())
                    continue;
                for (const WeightedCall& call : callsOfNodes(index)) {
                    if (!call.isAlsoInLongSet)
                        continue;
                    auto callOfSet = std::ranges::find_if(callsOfLongSets, [&](const WeightedCall& candidate) { return isInLongSet(candidate.node - numberOfNodes, call.node); });
                    RELEASE_ASSERT(callOfSet != callsOfLongSets.end());
                    add(index, HeatCallOfMember { HeatNodeID(call.node), call.weight, callOfSet->weight });
                }
            }
        });
        groupByIndex(numberOfLongSets, filled.m_firstUserOfLongSet, filled.m_usersOfLongSets, [&](const auto& add) {
            forEachCallOfLongSet([&](unsigned index, const WeightedCall& call) {
                add(call.node - numberOfNodes, HeatNodeID(index));
            });
        });
        // A set that nobody calls says nothing about its members.
        auto forEachMember = [&](const auto& functor) {
            for (unsigned set = 0; set < numberOfLongSets; ++set) {
                if (filled.m_firstUserOfLongSet[set] == filled.m_firstUserOfLongSet[set + 1])
                    continue;
                for (const WeightedCall& member : calls[numberOfNodes + set])
                    functor(set, member.node);
            }
        };
        groupByIndex(numberOfLongSets, filled.m_firstMemberOfLongSet, filled.m_membersOfLongSets, [&](const auto& add) {
            forEachMember([&](unsigned set, uint32_t member) {
                add(set, HeatNodeID(member));
            });
        });
        groupByIndex(numberOfNodes, filled.m_firstLongSetWith, filled.m_longSetsWith, [&](const auto& add) {
            forEachMember([&](unsigned set, uint32_t member) {
                add(member, set);
            });
        });
    };
    auto fillEdges = [&](HeatGraph& filled) {
        fillLongSets(filled);
        filled.m_firstSuccessor.fill(0u, numberOfNodes);
        filled.m_numberOfSuccessors.fill(0u, numberOfNodes);
        filled.m_firstPredecessor.fill(0u, numberOfNodes + 1);
        forEachIndexInParallel(loops, numberOfNodes, [&](unsigned index) {
            forEachEdgeOf(index, [&](const WeightedCall& call) {
                AtomicRef(filled.m_firstPredecessor[call.node + 1]).fetch_add(1, std::memory_order_relaxed);
            });
        });
        uint32_t numberOfStoredSuccessors = 0;
        BitVector storesSuccessors(numberOfNodes);
        UncheckedKeyHashMap<const SharedCallList*, uint32_t> firstSuccessorOfSharedList;
        for (unsigned index = 0; index < numberOfNodes; ++index) {
            filled.m_firstPredecessor[index + 1] += filled.m_firstPredecessor[index];
            filled.m_numberOfSuccessors[index] = std::ranges::count_if(callsOfNodes(index), [](const WeightedCall& call) { return !call.isAlsoInLongSet; });
            filled.m_firstSuccessor[index] = numberOfStoredSuccessors;
            if (const SharedCallList* shared = calls[index].sharedList()) {
                auto added = firstSuccessorOfSharedList.add(shared, numberOfStoredSuccessors);
                filled.m_firstSuccessor[index] = added.iterator->value;
                if (!added.isNewEntry)
                    continue;
            }
            storesSuccessors.quickSet(index);
            numberOfStoredSuccessors += filled.m_numberOfSuccessors[index];
        }
        filled.m_successors.shrink(0);
        filled.m_multipliers.shrink(0);
        growInParallel(loops, filled.m_successors, numberOfStoredSuccessors);
        growInParallel(loops, filled.m_multipliers, numberOfStoredSuccessors);
        filled.m_predecessors.shrink(0);
        growInParallel(loops, filled.m_predecessors, filled.m_firstPredecessor[numberOfNodes]);
        Vector<uint32_t> nextPredecessor(filled.m_firstPredecessor.span().first(numberOfNodes));
        forEachIndexInParallel(loops, numberOfNodes, [&](unsigned index) {
            uint32_t position = filled.m_firstSuccessor[index];
            bool storesOwnSuccessors = storesSuccessors.quickGet(index);
            forEachEdgeOf(index, [&](const WeightedCall& call) {
                if (storesOwnSuccessors) {
                    filled.m_successors[position] = HeatNodeID(call.node);
                    filled.m_multipliers[position++] = call.weight;
                }
                filled.m_predecessors[AtomicRef(nextPredecessor[call.node]).fetch_add(1, std::memory_order_relaxed)] = HeatNodeID(index);
            });
        });
        // A node's predecessors are sorted by index.
        forEachIndexInParallel(loops, numberOfNodes, [&](unsigned index) {
            AOT::sort(filled.m_predecessors.mutableSpan().subspan(filled.m_firstPredecessor[index], filled.m_firstPredecessor[index + 1] - filled.m_firstPredecessor[index]));
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
    std::atomic<uint64_t> numberOfValues { 0 };
    constexpr unsigned functionsInShare = 4096;
    unsigned count = program.functions.size();
    forEachIndexInParallel(build.parallelLoops(), (count + functionsInShare - 1) / functionsInShare, [&](unsigned share) {
        uint64_t numberInShare = 0;
        for (unsigned index = share * functionsInShare; index < std::min(count, (share + 1) * functionsInShare); ++index)
            numberInShare += numberOfValuesIn(program, FunctionID(index));
        numberOfValues.fetch_add(numberInShare, std::memory_order_relaxed);
    });
    return numberOfValues.load(std::memory_order_relaxed) > maxValuesOfOrdinaryProgram(build);
}

// sum + addend + addend + .., each sum rounded as the machine rounds it, in a number of steps that grows with the logarithm of count.
// While the exponent of the sum stays, the machine adds the same multiple of the last place each time. Where the addend lies halfway between two multiples that holds from the second sum on, which starts at an even one.
static double addRepeatedly(double sum, double addend, uint64_t count)
{
    unsigned sumsWithSameExponent = 0;
    while (count) {
        double next = sum + addend;
        if (next == sum)
            return sum;
        --count;
        sumsWithSameExponent = std::ilogb(next) == std::ilogb(sum) ? sumsWithSameExponent + 1 : 0;
        double step = next - sum;
        sum = next;
        if (sumsWithSameExponent < 2)
            continue;
        double stepsToNextExponent = (std::ldexp(1.0, std::ilogb(sum) + 1) - sum) / step;
        if (stepsToNextExponent < 4)
            continue;
        uint64_t steps = std::min(count, static_cast<uint64_t>(stepsToNextExponent) - 2);
        sum += static_cast<double>(steps) * step;
        count -= steps;
    }
    return sum;
}

namespace {

// WTF::CycleForest and WTF::estimateMultipliedFrequencies() for a graph with long sets. Both read a graph edge by edge, and a long set stands for as many edges as it has users times members.
// This finds the cycles, the headers and the order that they would find if each user listed the members among its successors, and reads each set once for each cycle that holds some of it.
class ActivationsThroughLongSets {
public:
    ActivationsThroughLongSets(HeatGraph& graph, unsigned maxDepth)
        : m_graph(graph)
        , m_maxDepth(maxDepth)
        , m_numberOfNodes(graph.numNodes())
        , m_numberOfLongSets(graph.numberOfLongSets())
    {
    }

    Vector<double> run()
    {
        findCycles();
        Forest::Parts parts;
        parts.cycles = WTF::move(m_cycles);
        parts.innermost = WTF::Vector<unsigned>(m_numberOfNodes, [&](size_t index) {
            return m_states[index].innermost;
        });
        Forest cycles(m_graph, WTF::move(parts));
        return propagate(cycles, computePostOrder(cycles));
    }

private:
    using Forest = WTF::CycleForest<HeatGraph>;
    using Node = HeatNodeID;
    static constexpr unsigned none = Forest::none;

    struct Frame {
        unsigned node;
        unsigned next;
    };

    struct Pending {
        unsigned cycle;
        Vector<unsigned> nodes;
    };

    // Of a node, and behind the nodes of a long set. A call passes through a set in whichever cycle it stays.
    struct State {
        unsigned number { 0 };
        unsigned lowest { 0 };
        unsigned innermost { none };
        bool isOnStack { false };
    };

    void findCycles()
    {
        m_states.fill(State { }, m_numberOfNodes + m_numberOfLongSets);
        m_membersInScope.grow(m_numberOfLongSets);
        m_usersInCycle.fill(0u, m_numberOfLongSets);
        m_membersInCycle.fill(0u, m_numberOfLongSets);
        Vector<unsigned> nodes;
        for (unsigned index = 0; index < m_numberOfNodes; ++index) {
            if (m_graph.node(index))
                nodes.append(index);
        }
        decompose(nodes, none);
        while (!m_pending.isEmpty()) {
            Pending pending = m_pending.takeLast();
            decompose(pending.nodes, pending.cycle);
        }
    }

    void enter(unsigned node)
    {
        State& state = m_states[node];
        state.number = state.lowest = ++m_nextNumber;
        state.isOnStack = true;
        m_stack.append(node);
        m_frames.append(Frame { node, 0 });
    }

    void decompose(const Vector<unsigned>& nodes, unsigned scope)
    {
        unsigned scopeHeader = scope == none ? none : m_cycles[scope].header().index();
        for (unsigned node : nodes) {
            m_states[node].number = 0;
            for (const HeatCallOfLongSet& call : m_graph.longSetsOf(Node(node)))
                m_states[m_numberOfNodes + call.set].number = 0;
            for (uint32_t set : m_graph.longSetsWith(Node(node)))
                m_membersInScope[set].append(node);
        }
        for (unsigned start : nodes) {
            if (m_states[start].number)
                continue;
            enter(start);
            while (!m_frames.isEmpty())
                walkOn(scope, scopeHeader);
        }
        for (unsigned node : nodes) {
            for (uint32_t set : m_graph.longSetsWith(Node(node)))
                m_membersInScope[set].shrink(0);
        }
    }

    void walkOn(unsigned scope, unsigned scopeHeader)
    {
        unsigned node = m_frames.last().node;
        unsigned next = m_frames.last().next;
        State& state = m_states[node];
        bool isLongSet = node >= m_numberOfNodes;
        std::span<const Node> successors = isLongSet ? std::span<const Node>() : m_graph.successors(Node(node));
        std::span<const HeatCallOfLongSet> longSets = isLongSet ? std::span<const HeatCallOfLongSet>() : m_graph.longSetsOf(Node(node));
        std::span<const unsigned> members = isLongSet ? m_membersInScope[node - m_numberOfNodes].span() : std::span<const unsigned>();
        unsigned size = successors.size() + longSets.size() + members.size();
        while (next < size) {
            unsigned position = next++;
            unsigned successor;
            if (isLongSet)
                successor = members[position];
            else if (position < successors.size()) {
                successor = successors[position].index();
                if (m_states[successor].innermost != scope || successor == scopeHeader)
                    continue;
            } else
                successor = m_numberOfNodes + longSets[position - successors.size()].set;
            const State& stateOfSuccessor = m_states[successor];
            if (!stateOfSuccessor.number) {
                m_frames.last().next = next;
                enter(successor);
                return;
            }
            if (stateOfSuccessor.isOnStack)
                state.lowest = std::min(state.lowest, stateOfSuccessor.number);
        }
        m_frames.removeLast();
        if (!m_frames.isEmpty()) {
            State& parent = m_states[m_frames.last().node];
            parent.lowest = std::min(parent.lowest, state.lowest);
        }
        if (state.lowest != state.number)
            return;
        unsigned first = m_stack.size() - 1;
        while (m_stack[first] != node)
            --first;
        Vector<unsigned, 8> nodes;
        bool passesThroughLongSet = false;
        for (unsigned member : m_stack.span().subspan(first)) {
            m_states[member].isOnStack = false;
            if (member < m_numberOfNodes)
                nodes.append(member);
            else
                passesThroughLongSet = true;
        }
        m_stack.shrink(first);
        if (nodes.size() > 1 || (nodes.size() == 1 && (passesThroughLongSet || std::ranges::contains(m_graph.successors(Node(nodes[0])), Node(nodes[0])))))
            addCycle(nodes.span(), scope);
    }

    void addCycle(std::span<const unsigned> members, unsigned parent)
    {
        unsigned cycle = m_cycles.size();
        m_cycles.append(Forest::Cycle { });
        m_cycles[cycle].parent = parent;
        m_cycles[cycle].depth = (parent == none ? 0 : m_cycles[parent].depth) + 1;
        auto isInCycle = [&](Node node) {
            return m_states[node.index()].innermost == cycle;
        };
        for (unsigned member : members) {
            m_states[member].innermost = cycle;
            for (const HeatCallOfLongSet& call : m_graph.longSetsOf(Node(member)))
                ++m_usersInCycle[call.set];
            for (uint32_t set : m_graph.longSetsWith(Node(member)))
                ++m_membersInCycle[set];
        }

        Vector<unsigned, 2> entries;
        for (unsigned member : members) {
            bool isEntered = !std::ranges::all_of(m_graph.predecessors(Node(member)), isInCycle) || std::ranges::any_of(m_graph.longSetsWith(Node(member)), [&](uint32_t set) {
                return m_usersInCycle[set] < m_graph.usersOfLongSet(set).size();
            });
            if (isEntered)
                entries.append(member);
        }
        unsigned header = none;
        if (entries.size() == 1)
            header = entries[0];
        else {
            int64_t mostPaths = 0;
            for (unsigned member : members) {
                int64_t predecessors = std::ranges::count_if(m_graph.predecessors(Node(member)), isInCycle);
                for (uint32_t set : m_graph.longSetsWith(Node(member)))
                    predecessors += m_usersInCycle[set];
                int64_t successors = std::ranges::count_if(m_graph.successors(Node(member)), isInCycle);
                for (const HeatCallOfLongSet& call : m_graph.longSetsOf(Node(member)))
                    successors += m_membersInCycle[call.set];
                int64_t paths = predecessors * successors;
                if (header != none && (paths < mostPaths || (paths == mostPaths && header < member)))
                    continue;
                header = member;
                mostPaths = paths;
            }
        }
        for (unsigned member : members) {
            for (const HeatCallOfLongSet& call : m_graph.longSetsOf(Node(member)))
                m_usersInCycle[call.set] = 0;
            for (uint32_t set : m_graph.longSetsWith(Node(member)))
                m_membersInCycle[set] = 0;
        }

        auto& result = m_cycles[cycle].entries;
        result.append(Node(header));
        for (unsigned entry : entries) {
            if (entry != header)
                result.append(Node(entry));
        }
        if (members.size() == 1 || m_cycles[cycle].depth >= m_maxDepth)
            return;
        Pending pending { cycle, { } };
        pending.nodes.reserveInitialCapacity(members.size() - 1);
        for (unsigned member : members) {
            if (member != header)
                pending.nodes.append(member);
        }
        m_pending.append(WTF::move(pending));
    }

    static std::optional<Node> targetInOrder(const Forest& cycles, Node from, Node to)
    {
        if (!cycles.size())
            return to;
        unsigned cycle = cycles.innermost(to);
        unsigned common = cycles.nearestCommon(cycles.innermost(from), cycle);
        if (cycle == common) {
            if (cycles.isHeader(to))
                return std::nullopt;
            return to;
        }
        while (cycles[cycle].parent != common)
            cycle = cycles[cycle].parent;
        return cycles[cycle].header();
    }

    // A walk enters a cycle at its header and sees all of the cycle before it leaves. So a successor that has been seen leads nowhere new from any node, and one that has not leads somewhere new from every node
    // that has it. What is seen stays seen, so all the users of a set go on where the last one stopped.
    Vector<Node> computePostOrder(const Forest& cycles)
    {
        Vector<Node> postOrder;
        postOrder.reserveInitialCapacity(m_numberOfNodes);
        Vector<bool> isSeen(FillWith { }, m_numberOfNodes, false);
        Vector<unsigned> firstNotSeen(FillWith { }, m_numberOfLongSets, 0u);
        isSeen[m_graph.root().index()] = true;
        m_frames.append(Frame { m_graph.root().index(), 0 });
        while (!m_frames.isEmpty()) {
            Node node(m_frames.last().node);
            std::span<const Node> successors = m_graph.successors(node);
            unsigned& next = m_frames.last().next;
            std::optional<Node> target;
            for (; next < successors.size(); ++next) {
                target = targetInOrder(cycles, node, successors[next]);
                if (target && !isSeen[target->index()])
                    break;
            }
            Node least = next < successors.size() ? successors[next] : Node();
            for (const HeatCallOfLongSet& call : m_graph.longSetsOf(node)) {
                std::span<const Node> members = m_graph.membersOfLongSet(call.set);
                unsigned& first = firstNotSeen[call.set];
                for (; first < members.size(); ++first) {
                    if (isSeen[members[first].index()])
                        continue;
                    std::optional<Node> targetOfMember = targetInOrder(cycles, node, members[first]);
                    ASSERT(targetOfMember && !isSeen[targetOfMember->index()]);
                    if (!targetOfMember || isSeen[targetOfMember->index()])
                        continue;
                    if (!least || members[first] < least) {
                        least = members[first];
                        target = targetOfMember;
                    }
                    break;
                }
            }
            if (!least) {
                postOrder.append(node);
                m_frames.removeLast();
                continue;
            }
            isSeen[target->index()] = true;
            m_frames.append(Frame { target->index(), 0 });
        }
        return postOrder;
    }

    Vector<double> propagate(const Forest& cycles, const Vector<Node>& postOrder)
    {
        const double maxFlow = frequencyConstants.maxMultipliedFrequency;
        // What has come to the members of a set that lie right in one cycle, or in none: what the users inside that cycle have sent.
        struct MembersInCycle {
            unsigned cycle;
            uint32_t numberOfMembers;
            double incoming;
        };
        Vector<uint32_t> firstGroup(FillWith { }, m_numberOfLongSets + 1, 0u);
        Vector<MembersInCycle> groups;
        // The members of a set that lie anywhere in a cycle. What a user outside the cycle sends to them enters the cycle.
        struct MembersInside {
            unsigned cycle;
            uint32_t set;
            uint32_t numberOfMembers;
            uint32_t sum { 0 };
        };
        Vector<MembersInside> inside;
        // What the users of a set have sent so far to so many of its members, added up one member after the other as a walk over the edges adds it.
        struct SentToMembers {
            uint32_t numberOfMembers;
            double sum;
        };
        Vector<uint32_t> firstSum(FillWith { }, m_numberOfLongSets + 1, 0u);
        Vector<SentToMembers> sums;
        Vector<unsigned> cyclesOfMembers;
        for (uint32_t set = 0; set < m_numberOfLongSets; ++set) {
            cyclesOfMembers.shrink(0);
            for (Node member : m_graph.membersOfLongSet(set))
                cyclesOfMembers.append(cycles.innermost(member));
            AOT::sort(cyclesOfMembers);
            for (unsigned cycle : cyclesOfMembers) {
                if (groups.size() > firstGroup[set] && groups.last().cycle == cycle)
                    ++groups.last().numberOfMembers;
                else
                    groups.append(MembersInCycle { cycle, 1, 0 });
            }
            firstGroup[set + 1] = groups.size();
            for (const MembersInCycle& group : groups.span().subspan(firstGroup[set])) {
                for (unsigned cycle = group.cycle; cycle != none; cycle = cycles[cycle].parent)
                    inside.append(MembersInside { cycle, set, group.numberOfMembers });
            }
        }
        AOT::sortInLine(inside, [](const MembersInside& a, const MembersInside& b) {
            return std::tuple(a.set, a.cycle) < std::tuple(b.set, b.cycle);
        });
        size_t numberOfMerged = 0;
        for (const MembersInside& entry : inside) {
            if (numberOfMerged && inside[numberOfMerged - 1].set == entry.set && inside[numberOfMerged - 1].cycle == entry.cycle)
                inside[numberOfMerged - 1].numberOfMembers += entry.numberOfMembers;
            else
                inside[numberOfMerged++] = entry;
        }
        inside.shrink(numberOfMerged);
        AOT::sortInLine(inside, [](const MembersInside& a, const MembersInside& b) {
            return std::tuple(a.set, a.numberOfMembers) < std::tuple(b.set, b.numberOfMembers);
        });
        uint32_t nextSet = 0;
        for (MembersInside& entry : inside) {
            while (nextSet <= entry.set)
                firstSum[nextSet++] = sums.size();
            if (sums.size() == firstSum[entry.set] || sums.last().numberOfMembers != entry.numberOfMembers)
                sums.append(SentToMembers { entry.numberOfMembers, 0 });
            entry.sum = sums.size() - 1;
        }
        while (nextSet <= m_numberOfLongSets)
            firstSum[nextSet++] = sums.size();
        AOT::sortInLine(inside, [](const MembersInside& a, const MembersInside& b) {
            return std::tuple(a.cycle, a.set) < std::tuple(b.cycle, b.set);
        });
        Vector<uint32_t> firstInside(FillWith { }, cycles.size() + 1, 0u);
        for (const MembersInside& entry : inside)
            ++firstInside[entry.cycle + 1];
        for (unsigned cycle = 0; cycle < cycles.size(); ++cycle)
            firstInside[cycle + 1] += firstInside[cycle];
        auto groupOf = [&](uint32_t set, unsigned cycle) -> MembersInCycle* {
            std::span<MembersInCycle> ofSet = groups.mutableSpan().subspan(firstGroup[set], firstGroup[set + 1] - firstGroup[set]);
            auto found = std::ranges::lower_bound(ofSet, cycle, { }, &MembersInCycle::cycle);
            return found != ofSet.end() && found->cycle == cycle ? &*found : nullptr;
        };

        Vector<double> frequencies(FillWith { }, m_numberOfNodes, 0.0);
        Vector<double> incoming(FillWith { }, m_numberOfNodes, 0.0);
        Vector<double> entering(FillWith { }, cycles.size(), 0.0);
        Node root = m_graph.root();
        incoming[root.index()] = 1;
        for (unsigned cycle = cycles.innermost(root); cycle != none; cycle = cycles[cycle].parent)
            entering[cycle] = 1;
        for (unsigned index = postOrder.size(); index--;) {
            Node node = postOrder[index];
            unsigned nodeCycle = cycles.innermost(node);
            double arriving;
            if (cycles.isHeader(node)) {
                // Every user that has been visited lies outside: the header is the first node of its cycle in this order.
                arriving = entering[nodeCycle];
                for (const MembersInside& entry : inside.span().subspan(firstInside[nodeCycle], firstInside[nodeCycle + 1] - firstInside[nodeCycle]))
                    arriving += sums[entry.sum].sum;
                arriving *= frequencyConstants.iterationsPerLoop;
            } else {
                arriving = incoming[node.index()];
                for (uint32_t set : m_graph.longSetsWith(node))
                    arriving += groupOf(set, nodeCycle)->incoming;
            }
            double flow = std::min(arriving, maxFlow);
            frequencies[node.index()] = std::clamp(flow, 0.0, maxFlow);
            auto amountFor = [&](double multiplier) {
                return flow * std::clamp(multiplier, 0.0, frequencyConstants.maxMultiplier);
            };
            auto send = [&](Node successor, double amount) {
                unsigned cycle = cycles.innermost(successor);
                unsigned common = cycles.nearestCommon(nodeCycle, cycle);
                if (cycle == common) {
                    incoming[successor.index()] += amount;
                    return;
                }
                for (; cycle != common; cycle = cycles[cycle].parent)
                    entering[cycle] += amount;
            };
            std::span<const Node> successors = m_graph.successors(node);
            for (unsigned successorIndex = 0; successorIndex < successors.size(); ++successorIndex)
                send(successors[successorIndex], amountFor(m_graph.multiplierOf(node, successorIndex)));
            // The member gets its share of what goes to the set below.
            for (const HeatCallOfMember& call : m_graph.otherCallsOfMembers(node))
                send(call.member, amountFor(call.multiplier) - amountFor(call.multiplierOfSet));
            for (const HeatCallOfLongSet& call : m_graph.longSetsOf(node)) {
                double amount = amountFor(call.multiplier);
                for (SentToMembers& sent : sums.mutableSpan().subspan(firstSum[call.set], firstSum[call.set + 1] - firstSum[call.set]))
                    sent.sum = addRepeatedly(sent.sum, amount, sent.numberOfMembers);
                for (unsigned cycle = nodeCycle;; cycle = cycles[cycle].parent) {
                    if (MembersInCycle* group = groupOf(call.set, cycle))
                        group->incoming += amount;
                    if (cycle == none)
                        break;
                }
            }
        }
        return frequencies;
    }

    HeatGraph& m_graph;
    unsigned m_maxDepth;
    unsigned m_numberOfNodes;
    unsigned m_numberOfLongSets;
    WTF::Vector<Forest::Cycle> m_cycles;
    Vector<State> m_states;
    Vector<unsigned, 32> m_stack;
    Vector<Frame, 32> m_frames;
    Vector<Pending> m_pending;
    Vector<Vector<unsigned>> m_membersInScope;
    Vector<uint32_t> m_usersInCycle;
    Vector<uint32_t> m_membersInCycle;
    unsigned m_nextNumber { 0 };
};

} // anonymous namespace

Vector<double> estimateActivations(HeatGraph& graph)
{
    AOT_THRESHOLD(unsigned, maxDepthOfCallCycles, 16);
    if (graph.numberOfLongSets()) {
        Vector<double> result = ActivationsThroughLongSets(graph, maxDepthOfCallCycles).run();
        result.shrink(graph.numberOfFunctions());
        return result;
    }
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
        Vector<std::pair<Node, double>> callees;
        for (unsigned index = 0; index < successors(node).size(); ++index)
            callees.append({ successors(node)[index], multiplierOf(node, index) });
        for (const HeatCallOfLongSet& call : longSetsOf(node)) {
            for (Node member : membersOfLongSet(call.set)) {
                auto other = std::ranges::find(otherCallsOfMembers(node), member, &HeatCallOfMember::member);
                callees.append({ member, other != otherCallsOfMembers(node).end() ? other->multiplier : call.multiplier });
            }
        }
        AOT::sort(callees, { }, [](const auto& callee) { return callee.first; });
        for (auto [callee, multiplier] : callees) {
            if (FunctionID function = functionOf(callee))
                out.print(" f", function.index());
            else if (callee == unknownCodeNode())
                out.print(" unknown");
            else
                out.print(" name", callee.index() - nodeOfName(0).index());
            out.print("*", multiplier);
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
