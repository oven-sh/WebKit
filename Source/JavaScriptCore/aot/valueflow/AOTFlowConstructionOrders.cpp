/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsInternal.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTEffectsOfOperands.h"
#include "AOTFlowSiteData.h"
#include "AOTForwardDataflow.h"
#include "AOTFunctionAnalyses.h"
#include "AOTLayout.h"
#include "AOTSort.h"
#include "AOTSortedSet.h"
#include "AOTStronglyConnectedComponents.h"
#include "AOTThreads.h"
#include "AOTValueFlowBuiltinTables.h"
#include "AOTValueFlowHeapTables.h"
#include "AOTValueFlowResultsUnseenCode.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <algorithm>
#include <memory>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

template<typename Key, typename Mapped> using MapByNumber = HashMap<Key, Mapped, IntHash<Key>, WTF::UnsignedWithZeroKeyHashTraits<Key>>;

struct ConstructionName {
    PropertyKeyID key;
    FieldReference field;
    bool standsForValue { false };

    friend auto operator<=>(const ConstructionName&, const ConstructionName&) = default;
};

using ConstructionNames = SortedSet<ConstructionName>;

enum class ConstructionEventKind : uint8_t { Store, Read, Leak, Call, Exit };

struct ConstructionEvent {
    ConstructionEventKind kind { ConstructionEventKind::Leak };
    bool isExact { false };
    bool storesSelfReference { false };
    bool storesValue { false };
    ProgramPoint point;
    ValueID origin;
    ValueID operand;
    uint32_t operandIndex { 0 };
    std::optional<ConstructionName> name;
    ValueID holder;

    friend bool operator==(const ConstructionEvent&, const ConstructionEvent&) = default;
};

struct ConstructionSummary {
    std::span<const ConstructionEvent> eventsAt(ProgramPoint point) const LIFETIME_BOUND
    {
        auto found = std::ranges::equal_range(events, std::pair { point.block, point.index }, { }, [](const ConstructionEvent& event) {
            return std::pair { event.point.block, event.point.index };
        });
        return events.subspan(found.begin() - events.begin(), found.size());
    }

    ValueID root;
    Vector<ConstructionEvent> events;
};

struct FunctionConstruction {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(FunctionConstruction);

    const ConstructionSummary* find(ValueID root) const LIFETIME_BOUND
    {
        auto found = std::ranges::find(summaries, root, &ConstructionSummary::root);
        return found == summaries.end() ? nullptr : &*found;
    }

    const ValueFlowStatement* findStatement(ValueID site) const
    {
        auto found = std::ranges::lower_bound(statements, site, { }, &ValueFlowStatement::site);
        return found != statements.end() && (*found)->site == site ? *found : nullptr;
    }

    std::optional<FunctionAnalyses> analyses;
    Vector<ConstructionSummary> summaries;
    Vector<const ValueFlowStatement*> statements;
    bool returnsOnlyConstructedObject { false };
};

struct SolvedConstruction {
    bool mayLeak { true };
    bool storesOnlyInInitialOrder { false };
    ConstructionNames storedBeforeLeak;
    ConstructionNames storedAtExit;
    ConstructionNames readBeforeStore;
    ConstructionNames readNames;
    ConstructionNames selfReferenceNames;
    Vector<ConstructionName> initialOrder;
};

struct ResolvedConstructionEvent {
    bool storesOnlyData() const { return !mayLeak && !otherCodeMayRun && callees.isEmpty(); }

    bool mayLeak { true };
    bool otherCodeMayRun { true };
    bool resultHoldsObject { false };
    bool holderIsEmpty { false };
    Vector<uint32_t, 1> callees;
};

struct ConstructionNode {
    FunctionID function;
    ValueID root;
    bool isEnteredByNew { false };
    FunctionConstruction* construction { nullptr };
    const ConstructionSummary* summary { nullptr };
    Vector<ResolvedConstructionEvent> resolvedEvents;
    Vector<uint32_t> callees;
    SolvedConstruction solved;
};

struct ConstructionGraph {
    static uint64_t keyOf(FunctionID function, ValueID root, bool isEnteredByNew) { return static_cast<uint64_t>(isEnteredByNew) << 63 | static_cast<uint64_t>(function.index()) << 32 | root.index(); }

    Vector<ConstructionNode> nodes;
    MapByNumber<uint64_t, uint32_t> nodeIndexes;
};

constexpr OperandUses confinedRetentionUses { OperandUse::Inspects, OperandUse::Retains };
constexpr OperandUses namedStoreUses = confinedRetentionUses | OperandUses { OperandUse::PassesToUserCode };
constexpr OperandUses unnamedWritingUses = operandWritingUses | OperandUses { OperandUse::DefinesName, OperandUse::TakesDescriptor };
constexpr OperandUses contentsLeakingUses = callingUses | OperandUses { OperandUse::RetainsContents, OperandUse::AppliesTransitively, OperandUse::AppliesToContents };
constexpr Protocols enumeratingProtocols { Protocol::OwnKeys, Protocol::OwnValues };

bool isDirectOrIndirectCall(Opcode opcode)
{
    AccessClass accessClass = accessClassOf(opcode);
    return accessClass == AccessClass::DirectCall || accessClass == AccessClass::IndirectCall;
}

bool isPrivateKey(const Program& program, PropertyKeyID key)
{
    PropertyKeyKind kind = program.at(key).kind;
    return kind == PropertyKeyKind::PrivateName || kind == PropertyKeyKind::EnginePrivateName || kind == PropertyKeyKind::ModulePrivateName;
}

std::optional<ConstructionEventKind> accessEventOf(Opcode opcode, uint32_t operandIndex)
{
    Access access = accessOf(opcode);
    if (receiverOf(opcode) != operandIndex || access == Access::None)
        return std::nullopt;
    return access == Access::Store || access == Access::Define ? ConstructionEventKind::Store : ConstructionEventKind::Read;
}

ConstructionName fieldName(const Program& program, FieldReference reference)
{
    if (PropertyKeyID key = fieldAt(program, reference).key)
        return { key, { } };
    return { { }, declarationOf(program, reference) };
}

bool allocatesConstructedObject(const Program& program, const Function& function, const Value& value)
{
    return value.opcode == Opcode::NewObject && function.kind == FunctionKind::BaseConstructor && program.at(value.layout()).kind == LayoutKind::Instance;
}

bool allocatesObjectOfOwnClass(const Program& program, const Function& function, const Value& value)
{
    if (!allocatesConstructedObject(program, function, value) && !(value.opcode == Opcode::NewObject && function.has(FunctionFlags::AllocatesOwnThis)))
        return false;
    return function.constructedClass && declaredLayoutOf(program, value.layout()) == function.constructedClass;
}

bool isConstructorFunctionOfOwnLayout(const Program& program, FunctionID id)
{
    LayoutID constructed = program.at(id).constructedClass;
    return constructed && program.at(constructed).constructor == id && program.at(constructed).flags.contains(LayoutFlags::IsConstructorFunction) && program.at(id).thisParameterIndex();
}

Vector<ValueID, 2> constructorRootsOf(const Program& program, const Function& function)
{
    Vector<ValueID, 2> roots;
    if (!function.hasBody() || function.isUnsplitCoroutine() || function.constructorForm() == ConstructorForm::NotConstructor)
        return roots;
    if (std::optional<unsigned> index = function.thisParameterIndex()) {
        roots.append(function.at(entryBlock).parameters[*index]);
        return roots;
    }
    if (function.kind == FunctionKind::Plain)
        return roots;

    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            if (allocatesConstructedObject(program, function, function.at(id)) || OperandPosition::parent(function.at(id).opcode))
                roots.append(id);
        }
    }
    return roots;
}

bool isConstructedObject(const Program& program, FunctionAnalyses& analyses, std::span<const ValueID> constructorRoots, ValueID result)
{
    const Function& constructor = analyses.function();
    BitVector isVisited(constructor.values.size());
    Vector<ValueID, 4> valuesToVisit { result };
    while (!valuesToVisit.isEmpty()) {
        ValueID value = valuesToVisit.takeLast();
        if (isVisited.set(value.index()) || !program.typeOf(constructor, value).hasObjects())
            continue;
        ValueID object = sameObjectThroughConversionsToObject(constructor, value);
        if (std::ranges::contains(constructorRoots, object))
            continue;

        ValueLocation location = analyses.useLists().locationOf(object);
        if (location.placement != ValuePlacement::Parameter || location.block == entryBlock)
            return false;
        for (Edge edge : analyses.cfg().incomingEdges(location.block)) {
            ValueID passed = valuePassedOn(constructor, edge, location.index);
            if (!passed)
                return false;
            valuesToVisit.append(passed);
        }
    }
    return true;
}

bool returnsOnlyConstructedObject(const Program& program, FunctionAnalyses& analyses, std::span<const ValueID> constructorRoots)
{
    return std::ranges::all_of(analyses.cfg().reversePostOrder(), [&](BlockID block) {
        const Terminator& terminator = analyses.function().at(block).terminator;
        if (terminator.kind != TerminatorKind::Return)
            return terminator.kind != TerminatorKind::ReturnCall;
        return !terminator.valueOperand() || isConstructedObject(program, analyses, constructorRoots, terminator.valueOperand());
    });
}

struct ConstructionEventCollector {
    void collectInto(Vector<ConstructionSummary>& summaries)
    {
        std::optional<ObjectID> defined = escape.gaveUp() ? std::nullopt : escape.objectDefinedBy(root);
        if (!defined)
            return;
        object = *defined;

        addContainersOf(object);
        for (size_t next = 0; next < containers.size(); ++next)
            addContainersOf(containers[next]);
        Vector<ValueID> calls;
        for (const ObjectUse& objectUse : escape.usesOf(object)) {
            if (objectUse.use.kind == Use::Kind::Operand && isDirectOrIndirectCall(function.at(objectUse.use.user).opcode))
                calls.append(objectUse.use.user);
        }
        callsTakingObject = SortedSet<ValueID>(calls.span());

        addDefinitionEvents();
        for (const ObjectUse& objectUse : escape.usesOf(object)) {
            if (!isReadUnderNameOfObject(usedValueOf(function, objectUse.use)))
                addUse(objectUse);
        }
        for (ObjectID container : containers) {
            for (const ObjectUse& objectUse : escape.usesOf(container)) {
                if (isReadUnderNameOfObject(usedValueOf(function, objectUse.use)) || isRetainedByContainers(objectUse) || isStoredUnderNameOfObject(objectUse))
                    continue;
                if (objectUse.isEscape || mayLeakContents(objectUse))
                    addLeak(objectUse.use);
            }
        }
        for (ValueID call : callsTakingObject)
            addUsesOfResult(call);
        for (BlockID block : analyses.cfg().reversePostOrder()) {
            TerminatorKind kind = function.at(block).terminator.kind;
            if ((kind == TerminatorKind::Return || kind == TerminatorKind::ReturnCall) && !handsConstructionToAllocatingCopy(block))
                events.append({ .kind = ConstructionEventKind::Exit, .point = pointBeforeTerminatorOf(function, block) });
        }

        AOT::sort(events, { }, [](const ConstructionEvent& event) {
            return std::tuple { event.point.block, event.point.index, event.kind, event.operandIndex, event.name, event.operand };
        });
        removeRepeatedElements(events);
        summaries.append({ root, WTF::move(events) });
    }

    bool mayBeValue(ValueID stored) const { return !program.typeOf(function, stored).isUndefinedOrNull(); }

    bool isObject(ValueID value) const { return escape.singleObject(value) == object || sameObjectThroughConversionsToObject(function, value) == root; }

    ConstructionEvent eventAt(ConstructionEventKind kind, const Use& use) const
    {
        return { .kind = kind, .point = useLists.pointOf(use), .origin = use.user, .operand = usedValueOf(function, use), .operandIndex = use.operandIndex };
    }

    void addLeak(const Use& use) { events.append(eventAt(ConstructionEventKind::Leak, use)); }

    void addContainersOf(ObjectID content)
    {
        for (ObjectID container : escape.containersOf(content)) {
            if (container != object && escape.at(container).kind != ObjectKind::EntryParameter && !isContainer.set(container.index()))
                containers.append(container);
        }
    }

    void addDefinitionEvents()
    {
        const Value& allocation = function.at(root);
        if (allocation.opcode == Opcode::Parameter)
            return;
        ValueLocation location = useLists.locationOf(root);
        RELEASE_ASSERT(location.placement == ValuePlacement::InBlock);
        ConstructionEvent event { .kind = ConstructionEventKind::Call, .isExact = true, .point = { location.block, location.index }, .origin = root, .operand = root };
        if (allocation.opcode != Opcode::NewObject) {
            events.append(event);
            return;
        }
        std::span<const uint32_t> fields = allocation.fields();
        std::span<const ValueID> values = allocation.variadicOperands();
        event.kind = ConstructionEventKind::Store;
        for (size_t index = 0; index < fields.size(); ++index) {
            event.name = fieldName(program, { allocation.layout(), fields[index] });
            event.storesValue = index < values.size() && mayBeValue(values[index]);
            events.append(event);
        }
    }

    bool mayRunOtherCode(ValueID operation)
    {
        const EffectsOnEdges& effects = analyses.effectsOf(operation);
        return effects.normal.mayRunOtherCode() || effects.handler.mayRunOtherCode();
    }

    bool isRetainedByContainers(const ObjectUse& objectUse) const
    {
        if (!objectUse.retainedBy || !objectUse.uses.containsOnly(confinedRetentionUses))
            return false;
        PointsToSet retainers = escape.pointsTo(objectUse.retainedBy);
        bool isConfined = !retainers.mayBeUntracked && !retainers.objects.isEmpty();
        for (ObjectID retainer : retainers.objects)
            isConfined &= isContainer.get(retainer.index());
        return isConfined;
    }

    bool isStoredUnderNameOfObject(const ObjectUse& objectUse) const
    {
        const Use& use = objectUse.use;
        if (use.kind != Use::Kind::Operand || storedOf(function.at(use.user).opcode) != use.operandIndex)
            return false;
        return objectUse.retainedBy && objectUse.uses.containsOnly(namedStoreUses) && isObject(objectUse.retainedBy);
    }

    bool isStoredUnderNameOfObject(const Use& use) const
    {
        if (use.kind != Use::Kind::Operand)
            return false;
        const Value& user = function.at(use.user);
        std::optional<uint32_t> receiver = receiverOf(user.opcode);
        if (storedOf(user.opcode) != use.operandIndex || !receiver || accessEventOf(user.opcode, *receiver) != ConstructionEventKind::Store)
            return false;
        return isObject(user.operand(*receiver));
    }

    bool isReadUnderNameOfObject(ValueID value) const
    {
        const Value& definition = function.at(sameObjectThroughCasts(function, value));
        return accessOf(definition.opcode) == Access::Read && isObject(definition.operand(*receiverOf(definition.opcode)));
    }

    bool mayHoldObject(ValueID value) const
    {
        if (isReadUnderNameOfObject(value))
            return false;
        for (ObjectID member : escape.pointsTo(value).objects) {
            if (member == object || isContainer.get(member.index()))
                return true;
        }
        return false;
    }

    void addAccess(ConstructionEventKind kind, const Use& use)
    {
        const Value& access = function.at(use.user);
        ConstructionEvent event = eventAt(kind, use);
        event.name = Value::opcodesWithField.contains(access.opcode) ? fieldName(program, { access.layout(), access.field() }) : ConstructionName { access.key(), { } };
        event.isExact = isObject(event.operand) || isObjectWhereverItIsAllocated(event.operand);
        if (kind == ConstructionEventKind::Store) {
            ValueID stored = access.operand(*storedOf(access.opcode));
            event.storesValue = mayBeValue(stored);
            event.storesSelfReference = mayHoldObject(stored);
            if (ValueID definition = sameObjectThroughCasts(function, stored); !event.storesSelfReference && callsTakingObject.contains(definition)) {
                event.holder = definition;
                event.storesSelfReference = true;
            }
        }
        events.append(event);
    }

    bool isObjectWhereverItIsAllocated(ValueID value)
    {
        ValueLocation location = useLists.locationOf(sameObjectThroughCasts(function, value));
        if (location.placement != ValuePlacement::Parameter || location.block == entryBlock || analyses.isInCycle(location.block))
            return false;
        bool isOneOfThem = false;
        for (Edge edge : analyses.cfg().incomingEdges(location.block)) {
            ValueID passed = valuePassedOn(function, edge, location.index);
            if (!passed || analyses.isInCycle(edge.from))
                return false;
            ValueID allocation = sameObjectThroughCasts(function, passed);
            std::optional<ObjectID> allocated = escape.objectDefinedBy(allocation);
            if (!allocated || function.at(allocation).opcode != Opcode::NewObject || useLists.locationOf(allocation).block != edge.from)
                return false;
            isOneOfThem |= *allocated == object;
        }
        return isOneOfThem;
    }

    void addOperandUse(const ObjectUse& objectUse)
    {
        const Use& use = objectUse.use;
        Opcode opcode = function.at(use.user).opcode;
        if (isCast(opcode))
            return;
        if (std::optional<ConstructionEventKind> access = accessEventOf(opcode, use.operandIndex)) {
            if (opcode == Opcode::PrivateSet)
                addAccess(ConstructionEventKind::Read, use);
            addAccess(*access, use);
            return;
        }
        if (protocolsOf(opcode, use.operandIndex).containsAny(enumeratingProtocols)) {
            addLeak(use);
            return;
        }
        if (isRetainedByContainers(objectUse))
            return;
        if (storedOf(opcode) == use.operandIndex) {
            if (objectUse.isEscape && !isStoredUnderNameOfObject(objectUse))
                addLeak(use);
            return;
        }
        if (isDirectOrIndirectCall(opcode)) {
            ConstructionEvent event = eventAt(ConstructionEventKind::Call, use);
            event.isExact = isObject(event.operand) && escape.isOnlyOperandHolding(object, use);
            events.append(event);
            return;
        }
        if (objectUse.isEscape || mayRunOtherCode(use.user) || objectUse.uses.containsAny(unnamedWritingUses))
            addLeak(use);
    }

    void addUse(const ObjectUse& objectUse)
    {
        const Use& use = objectUse.use;
        if (use.kind == Use::Kind::Operand) {
            addOperandUse(objectUse);
            return;
        }
        bool leaks = objectUse.isEscape;
        if (use.kind == Use::Kind::TerminatorOperand) {
            TerminatorKind kind = function.at(use.block).terminator.kind;
            leaks = kind == TerminatorKind::Return ? function.at(root).opcode == Opcode::Parameter : kind != TerminatorKind::Branch && kind != TerminatorKind::Switch && kind != TerminatorKind::SwitchString;
        }
        if (leaks)
            addLeak(use);
    }

    bool mayLeakContents(const ObjectUse& objectUse)
    {
        if (objectUse.uses.containsAny(contentsLeakingUses))
            return true;
        return objectUse.use.kind == Use::Kind::Operand && (isDirectOrIndirectCall(function.at(objectUse.use.user).opcode) || mayRunOtherCode(objectUse.use.user));
    }

    void addUsesOfResult(ValueID call)
    {
        Vector<ValueID, 2> values { call };
        while (!values.isEmpty()) {
            for (const Use& use : useLists.usesOf(values.takeLast())) {
                if (use.kind == Use::Kind::Operand && isCastOrConversion(function.at(use.user).opcode)) {
                    values.append(use.user);
                    continue;
                }
                if (isStoredUnderNameOfObject(use))
                    continue;
                ConstructionEvent event = eventAt(ConstructionEventKind::Leak, use);
                event.holder = call;
                events.append(event);
            }
        }
    }

    bool handsConstructionToAllocatingCopy(BlockID block) const
    {
        const Terminator& terminator = function.at(block).terminator;
        if (terminator.kind != TerminatorKind::ReturnCall)
            return false;
        const Value& call = function.at(terminator.callOperand());
        if (call.opcode != Opcode::CallDirect || !call.isConstruction())
            return false;
        const Function& callee = program.at(call.function());
        return callee.has(FunctionFlags::AllocatesOwnThis) && callee.derivation.source == analyses.context().id;
    }

    const Program& program;
    FunctionAnalyses& analyses;
    ValueID root;
    const Function& function { analyses.function() };
    const EscapeAnalysis& escape { analyses.escape() };
    const UseLists& useLists { analyses.useLists() };
    ObjectID object { };
    Vector<ObjectID, 4> containers { };
    BitVector isContainer { };
    SortedSet<ValueID> callsTakingObject { };
    Vector<ConstructionEvent> events { };
};

std::unique_ptr<FunctionConstruction> buildFunctionConstruction(const ValueFlowResultInputs& inputs, FunctionID id)
{
    const Program& program = inputs.program;
    const Function& function = program.at(id);
    AnalysisContext context { inputs.build, program, nullptr, nullptr, id, CallResults::NotFollowed };
    auto result = makeUnique<FunctionConstruction>();
    result->analyses.emplace(context, function);
    if (!function.hasBody())
        return result;

    Vector<ValueID, 2> roots = constructorRootsOf(program, function);
    result->returnsOnlyConstructedObject = returnsOnlyConstructedObject(program, *result->analyses, roots.span());
    for (ValueID parameter : function.at(entryBlock).parameters) {
        if (mayBeReference(program.typeOf(function, parameter)) && !roots.contains(parameter))
            roots.append(parameter);
    }
    for (ValueID root : roots)
        ConstructionEventCollector { program, *result->analyses, root }.collectInto(result->summaries);
    result->analyses.emplace(context, function);

    for (const ValueFlowStatement& statement : inputs.bodies().statementsOf(id.index())) {
        if (statement.kind == ValueFlowStatementKind::Operation)
            result->statements.append(&statement);
    }
    AOT::sort(result->statements, { }, &ValueFlowStatement::site);
    return result;
}

bool isFollowedCode(const ValueFlowResultInputs& inputs, FunctionID function)
{
    return inputs.isReached(function) && inputs.program.at(function).hasBody();
}

constexpr OperandUses confinedUses { OperandUse::Inspects, OperandUse::ReadsContents, OperandUse::WritesContents, OperandUse::MovesElements, OperandUse::PassesToUserCode };

constexpr OptionSet<ObjectMutation> mutationsStoppingStores { ObjectMutation::RedefineProperty, ObjectMutation::ChangeIntegrity, ObjectMutation::ReachUnknownJavaScript };

enum class BuiltinEffectOnObject : uint8_t { MayLeak, HeldByResult, Confined, NoBuiltin };

struct ChainOfReceivers {
    ValueSetID chain;
    ValueSetID above;
    bool isKnown { false };
};

enum class UnknownChains : uint8_t { MayTakeObject, HoldNothingUnderName };

struct ConstructionGraphBuilder {
    ConstructionGraph build()
    {
        Vector<uint32_t> constructors = findIndicesInParallel(inputs.loops(), program.functions.size(), [&](unsigned index) {
            return program.functions[index].constructedClass && isFollowedCode(inputs, FunctionID(index));
        });
        for (uint32_t index : constructors) {
            for (ValueID root : constructorRootsOf(program, program.functions[index]))
                addNode(FunctionID(index), root, isConstructorFunctionOfOwnLayout(program, FunctionID(index)));
        }
        for (uint32_t next = 0; next < graph.nodes.size();) {
            buildMissingFunctions(next);
            for (uint32_t end = graph.nodes.size(); next < end; ++next)
                resolveNode(next);
        }
        for (ConstructionNode& node : graph.nodes) {
            for (const ResolvedConstructionEvent& event : node.resolvedEvents)
                node.callees.appendVector(event.callees);
            AOT::sort(node.callees);
            removeRepeatedElements(node.callees);
        }
        return WTF::move(graph);
    }

    bool mayBeStoppedAboveObjectsOf(LayoutID layout, PropertyKeyID key)
    {
        const std::optional<ChainOfReceivers>& chain = chainOfObjectsBeforeTheyLeak(maker.of(universe.instanceOf(layout)));
        return !chain || !chain->isKnown || mayBeStoppedAbove(*chain, key);
    }

    uint32_t addNode(FunctionID function, ValueID root, bool isEnteredByNew = false)
    {
        return graph.nodeIndexes.ensure(ConstructionGraph::keyOf(function, root, isEnteredByNew), [&] {
            graph.nodes.append(ConstructionNode { function, root, isEnteredByNew });
            return static_cast<uint32_t>(graph.nodes.size() - 1);
        }).iterator->value;
    }

    void buildMissingFunctions(uint32_t firstNode)
    {
        Vector<FunctionID> listed;
        for (const ConstructionNode& node : graph.nodes.span().subspan(firstNode)) {
            if (!functions[node.function.index()])
                listed.append(node.function);
        }
        SortedSet<FunctionID> missing(listed.span());
        forEachIndexInParallel(inputs.loops(), static_cast<unsigned>(missing.size()), [&](unsigned index) {
            functions[missing[index].index()] = buildFunctionConstruction(inputs, missing[index]);
        });
    }

    void resolveNode(uint32_t index)
    {
        FunctionID function = graph.nodes[index].function;
        ValueID root = graph.nodes[index].root;
        bool isEnteredByNew = graph.nodes[index].isEnteredByNew;
        FunctionConstruction* construction = functions[function.index()].get();
        graph.nodes[index].construction = construction;
        const ConstructionSummary* summary = construction->find(root);
        if (!summary || !isFollowedCode(inputs, function))
            return;

        std::span<const ConstructionEvent> events = summary->events.span();
        Vector<ResolvedConstructionEvent> resolvedEvents = summary->events.map([&](const ConstructionEvent& event) {
            return resolveEvent(*construction, root, isEnteredByNew, event);
        });
        Vector<ValueID, 2> holders;
        for (size_t position = 0; position < events.size(); ++position) {
            if (resolvedEvents[position].resultHoldsObject)
                holders.append(events[position].origin);
        }
        for (size_t position = 0; position < events.size(); ++position) {
            if (!events[position].holder || holders.contains(events[position].holder))
                continue;
            resolvedEvents[position].holderIsEmpty = true;
            resolvedEvents[position].mayLeak &= events[position].kind != ConstructionEventKind::Leak;
        }
        graph.nodes[index].summary = summary;
        graph.nodes[index].resolvedEvents = WTF::move(resolvedEvents);
    }

    ResolvedConstructionEvent resolveEvent(const FunctionConstruction& construction, ValueID root, bool isEnteredByNew, const ConstructionEvent& event)
    {
        ResolvedConstructionEvent resolved;
        switch (event.kind) {
        case ConstructionEventKind::Leak:
            break;
        case ConstructionEventKind::Exit:
            resolved.mayLeak = false;
            break;
        case ConstructionEventKind::Store:
        case ConstructionEventKind::Read:
            resolveAccess(construction, isEnteredByNew, event, resolved);
            break;
        case ConstructionEventKind::Call:
            if (event.origin == root && event.operand == root)
                resolveParentConstruction(construction, event, resolved);
            else
                resolveCall(construction, event, resolved);
            break;
        }
        AOT::sort(resolved.callees);
        removeRepeatedElements(resolved.callees);
        return resolved;
    }

    Vector<ValueSetID, 4> operandsOf(const FunctionConstruction& construction, ValueID site)
    {
        Vector<ValueSetID, 4> values;
        if (const ValueFlowStatement* statement = construction.findStatement(site)) {
            for (const ValueFlowOperand& operand : inputs.bodies().operandsOf(*statement))
                values.append(heap.possibleValuesOf(operand));
        }
        return values;
    }

    bool holdsPlaceholder(ValueSetID set) const
    {
        return ValueSets::isComplement(set) || sets.contains(set, ValueFlowUniverse::unknownObject()) || sets.contains(set, ValueFlowUniverse::untrackedObject());
    }

    void resolveParentConstruction(const FunctionConstruction& construction, const ConstructionEvent& event, ResolvedConstructionEvent& resolved)
    {
        resolved.otherCodeMayRun = false;
        Vector<ValueSetID, 4> operands = operandsOf(construction, event.origin);
        if (operands.isEmpty() || holdsPlaceholder(operands[0]))
            return;

        bool leaks = false;
        bool hasBuiltinParent = false;
        sets.forEachMember(operands[0], [&](ValueFlowObjectID parent) {
            if (sets.contains(universe.builtinFunctions(), parent)) {
                hasBuiltinParent = true;
                return;
            }
            FunctionID function = universe.closureFunction(parent);
            if (!function || !isFollowedCode(inputs, function)) {
                leaks |= function || sets.contains(universe.otherCallables(), parent);
                return;
            }
            Vector<ValueID, 2> roots = constructorRootsOf(program, program.at(function));
            const FunctionConstruction* parentConstruction = functions[function.index()].get();
            leaks |= roots.isEmpty() || !parentConstruction || !parentConstruction->returnsOnlyConstructedObject;
            for (ValueID root : roots)
                resolved.callees.append(addNode(function, root));
        });
        resolved.mayLeak = leaks || (hasBuiltinParent && !resolved.callees.isEmpty());
    }

    const ChainOfReceivers& chainOf(ValueSetID receivers)
    {
        return chains.ensure(receivers.index(), [&] {
            ChainOfReceivers result;
            result.above = heap.chainOf(heap.prototypesOf(receivers));
            result.chain = maker.unite(maker.intersect(receivers, universe.objects()), result.above);
            result.isKnown = !holdsPlaceholder(result.chain);
            return result;
        }).iterator->value;
    }

    ValueSetID accessorsUnder(ValueSetID holders, PropertyKeyID key, bool getters)
    {
        holders = maker.intersect(holders, ValueSets::complement(universe.builtinTables().heap().under(key).holdersWithLockedLocation));
        ValueSetID underKey = heap.writtenUnder(holders, getters ? ValueFlowName::getter(key) : ValueFlowName::setter(key));
        return maker.unite(underKey, heap.writtenUnder(holders, ValueFlowName::of(getters ? ValueFlowNameKind::GetterUnderUnknownKey : ValueFlowNameKind::SetterUnderUnknownKey)));
    }

    bool builtinMembersArePlainValues(ValueSetID holders, PropertyKeyID key) const
    {
        const BuiltinTable& builtins = program.builtins();
        const ValueFlowMembersUnderKey& members = universe.builtinTables().heap().under(key);
        if (sets.meet(holders, members.holdersOfUndeclaredProperties))
            return false;
        bool arePlain = true;
        sets.forEachMember(maker.intersect(holders, members.holders), [&](ValueFlowObjectID holder) {
            for (BuiltinID member : builtins.membersOf(universe.builtinOf(holder))) {
                if (program.keyOf(builtins.at(member).name) == key)
                    arePlain &= isObjectKind(builtins.at(member).kind) || builtins.at(member).kind == BuiltinKind::Constant;
            }
        });
        return arePlain;
    }

    bool mayBeStoppedAbove(const ChainOfReceivers& chain, PropertyKeyID key)
    {
        ValueSetID above = chain.above;
        const BuiltinTable& builtins = program.builtins();
        const ValueFlowMembersUnderKey& members = universe.builtinTables().heap().under(key);
        if (accessorsUnder(above, key, true) != ValueSets::nothing || accessorsUnder(above, key, false) != ValueSets::nothing || sets.meet(above, members.holdersOfUndeclaredProperties))
            return true;
        bool mayBeStopped = false;
        sets.forEachMember(above, [&](ValueFlowObjectID holder) {
            BuiltinID builtin = universe.builtinOf(holder);
            if (!builtin) {
                mayBeStopped |= markedMutationsOf(inputs, holder).containsAny(mutationsStoppingStores);
                return;
            }
            if (!sets.contains(members.holders, holder))
                return;
            mayBeStopped |= sets.contains(members.holdersWithLockedLocation, holder);
            for (BuiltinID member : builtins.membersOf(builtin))
                mayBeStopped |= program.keyOf(builtins.at(member).name) == key && !isObjectKind(builtins.at(member).kind);
        });
        return mayBeStopped;
    }

    void enterWithObjectAsThis(ValueSetID callees, ResolvedConstructionEvent& resolved)
    {
        if (holdsPlaceholder(callees)) {
            resolved.mayLeak = true;
            return;
        }
        sets.forEachMember(callees, [&](ValueFlowObjectID callee) {
            if (FunctionID function = universe.closureFunction(callee))
                enter(function, program.at(function).thisParameterIndex(), resolved);
            else
                resolved.mayLeak |= universe.isObject(callee);
        });
    }

    void enter(FunctionID function, std::optional<uint32_t> entryParameter, ResolvedConstructionEvent& resolved)
    {
        const Function& callee = program.at(function);
        if (!entryParameter)
            resolved.otherCodeMayRun = true;
        else if (!isFollowedCode(inputs, function) || callee.isUnsplitCoroutine() || *entryParameter >= callee.at(entryBlock).parameters.size())
            resolved.mayLeak = true;
        else
            resolved.callees.append(addNode(function, callee.at(entryBlock).parameters[*entryParameter]));
    }

    std::optional<ValueSetID> prototypesGivenAtCreation(ValueFlowObjectID object)
    {
        if (universe.kindOf(object) == ValueFlowObjectKind::Instance) {
            LayoutID layout = universe.layoutOf(object);
            FunctionID constructor = program.at(layout).constructor;
            if (!constructor)
                return std::nullopt;
            ValueFlowObjectID atCreation[] { universe.prototypeOf(layout), universe.objectOf(Builtins::ObjectPrototype) };
            ValueSetID closure = maker.of(universe.closureOf(constructor));
            if (heap.writtenUnder(closure, ValueFlowName::of(ValueFlowNameKind::CopiedFrom)) != ValueSets::nothing)
                return std::nullopt;
            ValueSetID assigned = maker.unite(heap.writtenUnder(closure, ValueFlowName::property(universe.builtinTables().heap().prototypeKey())), heap.writtenUnder(closure, ValueFlowName::of(ValueFlowNameKind::UnknownKey)));
            return maker.unite(maker.of(std::span<const ValueFlowObjectID>(atCreation)), assigned);
        }
        if (universe.kindOf(object) != ValueFlowObjectKind::Site)
            return std::nullopt;
        const ValueFlowSite& site = *universe.siteOf(object);
        if (site.origin != ValueFlowSiteOrigin::Operation || site.ordinal)
            return std::nullopt;
        FunctionID function = universe.functionOf(object);
        const Value& allocation = program.at(function).at(site.value);
        if (allocation.opcode != Opcode::NewObject || !allocation.prototypeOperand())
            return std::nullopt;
        for (const ValueFlowStatement& statement : inputs.bodies().statementsOf(function.index())) {
            if (statement.kind == ValueFlowStatementKind::Operation && statement.site == site.value)
                return heap.possibleValuesOf(inputs.bodies().operandsOf(statement)[OperandIndex::NewObject::prototype]);
        }
        return std::nullopt;
    }

    const std::optional<ChainOfReceivers>& chainOfObjectsBeforeTheyLeak(ValueSetID receivers)
    {
        return chainsBeforeLeak.ensure(receivers.index(), [&]() -> std::optional<ChainOfReceivers> {
            ValueSetID objects = maker.intersect(receivers, universe.objects());
            if (holdsPlaceholder(objects) || sets.isEmpty(objects) || sets.size(objects) > inputs.build.limit(LimitID::MaxValueFlowMembersToldApart))
                return std::nullopt;
            std::optional<ValueSetID> prototypes = ValueSets::nothing;
            sets.forEachMember(objects, [&](ValueFlowObjectID object) {
                std::optional<ValueSetID> given = prototypes ? prototypesGivenAtCreation(object) : std::nullopt;
                prototypes = given ? std::optional(maker.unite(*prototypes, *given)) : std::nullopt;
            });
            if (!prototypes)
                return std::nullopt;
            ChainOfReceivers result;
            result.above = heap.chainOf(*prototypes);
            result.chain = maker.unite(objects, result.above);
            result.isKnown = !holdsPlaceholder(result.above) && !sets.meet(result.above, objects);
            return result;
        }).iterator->value;
    }

    void resolveAccess(const FunctionConstruction& construction, bool isEnteredByNew, const ConstructionEvent& event, ResolvedConstructionEvent& resolved)
    {
        const Function& function = construction.analyses->function();
        const Value& access = function.at(event.origin);
        Access kind = accessOf(access.opcode);
        if (access.opcode == Opcode::NewObject || Value::opcodesWithField.contains(access.opcode) || kind == Access::Define) {
            resolved.mayLeak = resolved.otherCodeMayRun = false;
            return;
        }

        Vector<ValueSetID, 4> operands = operandsOf(construction, event.origin);
        if (operands.isEmpty())
            return;
        PropertyKeyID key = access.key();
        ValueSetID receivers = operands[*receiverOf(access.opcode)];
        bool isExactStore = event.kind == ConstructionEventKind::Store && event.isExact && access.opcode == Opcode::PropSet;
        std::optional<ChainOfReceivers> chainOfRoot;
        if (event.isExact)
            chainOfRoot = chainOfObjectsBeforeTheyLeak(isEnteredByNew ? universe.objectsWith(function.constructedClass) : receivers);
        const ChainOfReceivers& chain = chainOfRoot ? *chainOfRoot : chainOf(receivers);
        resolved.mayLeak = resolved.otherCodeMayRun = !chain.isKnown && !(unknownChains == UnknownChains::HoldNothingUnderName && isExactStore);
        if (!chain.isKnown)
            return;

        if (kind == Access::Read || kind == Access::Store) {
            bool getters = kind == Access::Read;
            ValueSetID accessors;
            if (chainOfRoot) {
                ValueSetID own = accessorsUnder(maker.intersect(chain.chain, ValueSets::complement(chain.above)), key, getters);
                accessors = maker.unite(maker.intersect(own, ValueSets::complement(universe.placeholdersAlone())), accessorsUnder(chain.above, key, getters));
            } else
                accessors = accessorsUnder(chain.chain, key, getters);
            resolved.mayLeak = !builtinMembersArePlainValues(chain.chain, key);
            enterWithObjectAsThis(accessors, resolved);
        }
        resolved.otherCodeMayRun |= isExactStore && !isPrivateKey(program, key) && mayBeStoppedAbove(chain, key);
    }

    static bool collectsArgument(const Function& callee, uint32_t argument)
    {
        return callee.hasAny({ FunctionFlags::HasArgumentCount, FunctionFlags::HasHiddenRest }) || (callee.has(FunctionFlags::HasRestParameter) && argument >= callee.numberOfPositionalParameters());
    }

    void resolveCall(const FunctionConstruction& construction, const ConstructionEvent& event, ResolvedConstructionEvent& resolved)
    {
        const Value& call = construction.analyses->function().at(event.origin);
        if (accessClassOf(call.opcode) == AccessClass::DirectCall) {
            resolved.mayLeak = resolved.otherCodeMayRun = false;
            if (inputs.isReached(call.function())) {
                std::optional<uint32_t> entryParameter = entryParameterOf(program.at(call.function()), call, event.operandIndex);
                resolved.mayLeak = !entryParameter;
                enter(call.function(), entryParameter, resolved);
            }
            return;
        }

        Vector<ValueSetID, 4> operands = operandsOf(construction, event.origin);
        if (operands.isEmpty() || holdsPlaceholder(operands[0]))
            return;

        resolved.mayLeak = resolved.otherCodeMayRun = false;
        bool hasDirectCallee = false;
        Vector<BuiltinID, 1> builtinCallees;
        std::optional<uint32_t> argument = call.variadicOperandIndex(event.operandIndex);
        sets.forEachMember(operands[0], [&](ValueFlowObjectID callee) {
            if (FunctionID function = universe.closureFunction(callee)) {
                hasDirectCallee = true;
                if (argument && collectsArgument(program.at(function), *argument))
                    resolved.mayLeak = true;
                else
                    enter(function, entryParameterOf(program.at(function), call, event.operandIndex), resolved);
            } else if (sets.contains(universe.builtinFunctions(), callee))
                builtinCallees.append(universe.builtinOf(callee));
            else
                resolved.mayLeak |= sets.contains(universe.otherCallables(), callee);
        });

        BuiltinEffectOnObject builtinEffect = resolveBuiltinCallees(call, event, operands.span(), builtinCallees.span(), resolved);
        resolved.resultHoldsObject = builtinEffect == BuiltinEffectOnObject::HeldByResult;
        bool builtinRunsCallees = !resolved.callees.isEmpty() && !hasDirectCallee;
        resolved.otherCodeMayRun |= builtinEffect != BuiltinEffectOnObject::NoBuiltin && !builtinRunsCallees;
        resolved.mayLeak |= builtinEffect == BuiltinEffectOnObject::MayLeak;
    }

    static bool covers(const BuiltinPlace& place, BuiltinOperand position)
    {
        if (place.kind != BuiltinPlaceKind::Operand || !place.operand)
            return false;
        return *place.operand == position || (position.kind() == BuiltinOperand::Kind::Argument && place.operand->coversArgument(position.argumentIndex()));
    }

    std::optional<ValueSetID> valueAt(const Value& call, std::span<const ValueSetID> operands, BuiltinOperand position)
    {
        if (position.isReceiver())
            return operands[1];
        if (!VariadicOperandPosition::arguments(call.opcode))
            return std::nullopt;
        std::span<const ValueSetID> arguments = operands.subspan(2);
        ValueSetID value = ValueSets::nothing;
        for (size_t index = 0; index < arguments.size(); ++index) {
            if (position.coversArgument(index))
                value = maker.unite(value, arguments[index]);
        }
        return value;
    }

    void enterCallbacksTaking(const BuiltinOverload& overload, BuiltinOperand position, const Value& call, std::span<const ValueSetID> operands, ResolvedConstructionEvent& resolved)
    {
        bool isHandedOn = false;
        for (const BuiltinCallback& callback : overload.flow.callbacks) {
            bool isThis = covers(callback.thisValue, position);
            std::optional<uint32_t> argument;
            for (uint32_t index = 0; index < callback.arguments.size(); ++index) {
                if (!covers(callback.arguments[index], position))
                    continue;
                BuiltinOperand handedOn = *callback.arguments[index].operand;
                argument = index + (handedOn.kind() == BuiltinOperand::Kind::ArgumentsFrom ? position.argumentIndex() - handedOn.argumentIndex() : 0);
            }
            bool isTakenInOtherWays = covers(callback.spreadArguments, position) || covers(callback.newTarget, position) || callback.callee == position;
            if (!isThis && !argument && !isTakenInOtherWays)
                continue;

            isHandedOn = true;
            std::optional<ValueSetID> callees = valueAt(call, operands, callback.callee);
            if (isTakenInOtherWays || (isThis && argument) || callback.times != BuiltinCallbackTimes::Once || callback.constructs || !callees || holdsPlaceholder(*callees)) {
                resolved.mayLeak = true;
                continue;
            }
            sets.forEachMember(*callees, [&](ValueFlowObjectID callee) {
                FunctionID function = universe.closureFunction(callee);
                if (!function) {
                    resolved.mayLeak |= sets.contains(universe.callables(), callee);
                    return;
                }
                const Function& entry = program.at(function);
                if (isThis)
                    enter(function, entry.thisParameterIndex(), resolved);
                else if (collectsArgument(entry, *argument))
                    resolved.mayLeak = true;
                else
                    enter(function, *argument < entry.numberOfPositionalParameters() ? std::optional<uint32_t>(entry.firstParameterIndex() + *argument) : std::nullopt, resolved);
            });
        }
        resolved.mayLeak |= !isHandedOn || !overload.flow.handlerCalls.empty();
    }

    BuiltinEffectOnObject resolveBuiltinCallees(const Value& call, const ConstructionEvent& event, std::span<const ValueSetID> operands, std::span<const BuiltinID> builtinCallees, ResolvedConstructionEvent& resolved)
    {
        if (builtinCallees.empty())
            return BuiltinEffectOnObject::NoBuiltin;
        std::optional<BuiltinOperand> position;
        if (std::optional<uint32_t> argument = call.variadicOperandIndex(event.operandIndex)) {
            if (VariadicOperandPosition::arguments(call.opcode) && *argument <= BuiltinOperand::maxArgumentIndex)
                position = BuiltinOperand::argument(*argument);
        } else if (OperandPosition::thisValue(call.opcode) == event.operandIndex)
            position = BuiltinOperand::receiver();
        if (builtinCallees.size() > 1 || !position)
            return BuiltinEffectOnObject::MayLeak;

        BuiltinCallMode mode = call.opcode == Opcode::Call || call.opcode == Opcode::CallList ? BuiltinCallMode::Call : BuiltinCallMode::Construct;
        bool isHeldByResult = false;
        OperandUses uses;
        for (const BuiltinOverload& overload : program.builtins().overloadsOf(builtinCallees[0])) {
            if (overload.mode != mode)
                continue;
            OperandBehavior behavior = declaredBehaviorOf(overload, *position);
            OperandUses declaredUses = behavior.protocols.isEmpty() ? behavior.uses : OperandUses::all();
            if (declaredUses.contains(OperandUse::Retains) && behavior.retainedIn == RetainedIn::Result) {
                BuiltinFreshness freshness = program.builtins().at(overload.result.type).freshness;
                if (freshness == BuiltinFreshness::Unaliased || freshness == BuiltinFreshness::Yes) {
                    declaredUses.remove(OperandUse::Retains);
                    isHeldByResult = true;
                }
            }
            if (declaredUses.containsOnly(confinedUses) && declaredUses.contains(OperandUse::PassesToUserCode))
                enterCallbacksTaking(overload, *position, call, operands, resolved);
            uses.add(declaredUses);
        }
        if (!uses.containsOnly(confinedUses) || (uses.contains(OperandUse::PassesToUserCode) && resolved.otherCodeMayRun))
            return BuiltinEffectOnObject::MayLeak;
        return isHeldByResult ? BuiltinEffectOnObject::HeldByResult : BuiltinEffectOnObject::Confined;
    }

    const ValueFlowResultInputs& inputs;
    ValueFlowHeapView& heap;
    Vector<std::unique_ptr<FunctionConstruction>>& functions;
    UnknownChains unknownChains;
    const Program& program { inputs.program };
    const ValueFlowUniverse& universe { inputs.universe() };
    const ValueSets& sets { inputs.sets() };
    ValueSetMaker& maker { heap.maker() };
    MapByNumber<uint32_t, ChainOfReceivers> chains { };
    MapByNumber<uint32_t, std::optional<ChainOfReceivers>> chainsBeforeLeak { };
    ConstructionGraph graph { };
};

struct NamesAtPoint {
    ConstructionNames stored;
    ConstructionNames selfReferences;
};

bool storesSelfReference(const ConstructionEvent& event, const ResolvedConstructionEvent& resolved)
{
    return event.storesSelfReference && !resolved.holderIsEmpty;
}

struct StoredNamesProblem {
    using State = NamesAtPoint;

    static constexpr bool readsEffects() { return false; }
    State stateAtEntry() const { return { }; }
    void enterEdge(State&, Edge) const { }
    void execute(State& state, const DataflowStep& step) { applyEventsAt(state, { step.block, step.index }); }
    void executeTerminator(State& state, const DataflowTerminatorStep& step) { applyEventsAt(state, pointBeforeTerminatorOf(node.construction->analyses->function(), step.block)); }

    bool merge(State& into, const State& from) const
    {
        size_t storedCount = into.stored.size();
        into.stored.intersect(from.stored);
        bool hasMoreSelfReferences = into.selfReferences.unite(from.selfReferences);
        return hasMoreSelfReferences || into.stored.size() != storedCount;
    }

    static void keepCommon(std::optional<ConstructionNames>& names, const ConstructionNames& stored)
    {
        if (names)
            names->intersect(stored);
        else
            names = stored;
    }

    void applyLeak(const ConstructionNames& storedAtLeak)
    {
        keepCommon(storedBeforeLeak, storedAtLeak);
        solved.mayLeak = true;
    }

    void applyCompose(uint32_t calleeNode, State& state)
    {
        const SolvedConstruction& callee = graph.nodes[calleeNode].solved;
        ConstructionNames readEarly = callee.readBeforeStore;
        readEarly.subtract(state.stored);
        solved.readBeforeStore.unite(readEarly);
        solved.readNames.unite(callee.readNames);
        solved.selfReferenceNames.unite(callee.selfReferenceNames);
        if (callee.mayLeak) {
            ConstructionNames storedAtLeak = state.stored;
            storedAtLeak.unite(callee.storedBeforeLeak);
            applyLeak(storedAtLeak);
        }
        state.stored.unite(callee.storedAtExit);
        state.selfReferences.unite(callee.selfReferenceNames);
    }

    void applyCallees(State& state, const State& stateBeforeOperation, const ResolvedConstructionEvent& resolved, bool runsOneOfThem)
    {
        if (resolved.mayLeak)
            applyLeak(stateBeforeOperation.stored);
        std::optional<State> stateAfterCall;
        for (uint32_t callee : resolved.callees) {
            State stateAfterCallee = stateBeforeOperation;
            applyCompose(callee, stateAfterCallee);
            if (stateAfterCall)
                merge(*stateAfterCall, stateAfterCallee);
            else
                stateAfterCall = WTF::move(stateAfterCallee);
        }
        if (!stateAfterCall)
            return;
        if (runsOneOfThem)
            state.stored.unite(stateAfterCall->stored);
        state.selfReferences.unite(stateAfterCall->selfReferences);
    }

    bool reachesSelfReference(const State& stateBeforeOperation, const ConstructionEvent& event, const ResolvedConstructionEvent& resolved) const
    {
        if (event.kind == ConstructionEventKind::Store && storesSelfReference(event, resolved) && !resolved.storesOnlyData())
            return true;
        if (event.kind == ConstructionEventKind::Read && stateBeforeOperation.selfReferences.contains(*event.name))
            return true;
        return std::ranges::any_of(resolved.callees, [&](uint32_t callee) {
            return graph.nodes[callee].solved.readNames.overlaps(stateBeforeOperation.selfReferences);
        });
    }

    void applyEvent(State& state, const State& stateBeforeOperation, uint32_t position)
    {
        const ConstructionEvent& event = node.summary->events[position];
        const ResolvedConstructionEvent& resolved = node.resolvedEvents[position];
        if (reachesSelfReference(stateBeforeOperation, event, resolved)) {
            applyLeak(stateBeforeOperation.stored);
            eventsReachingSelfReference.set(position);
        }

        switch (event.kind) {
        case ConstructionEventKind::Leak:
            if (resolved.mayLeak)
                applyLeak(stateBeforeOperation.stored);
            return;
        case ConstructionEventKind::Store:
            applyCallees(state, stateBeforeOperation, resolved, false);
            if (!resolved.storesOnlyData()) {
                if (!stateBeforeOperation.stored.contains(*event.name))
                    solved.readBeforeStore.add(*event.name);
                return;
            }
            if (event.isExact) {
                state.stored.add(*event.name);
                if (event.storesValue)
                    state.stored.add({ event.name->key, event.name->field, true });
            }
            if (storesSelfReference(event, resolved)) {
                state.selfReferences.add(*event.name);
                solved.selfReferenceNames.add(*event.name);
            }
            return;
        case ConstructionEventKind::Read:
            applyCallees(state, stateBeforeOperation, resolved, false);
            solved.readNames.add(*event.name);
            if (!stateBeforeOperation.stored.contains(*event.name))
                solved.readBeforeStore.add(*event.name);
            return;
        case ConstructionEventKind::Call:
            applyCallees(state, stateBeforeOperation, resolved, event.isExact && !resolved.otherCodeMayRun);
            return;
        case ConstructionEventKind::Exit:
            keepCommon(storedBeforeLeak, state.stored);
            keepCommon(storedAtExit, state.stored);
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    void applyEventsAt(State& state, ProgramPoint point)
    {
        std::span<const ConstructionEvent> events = node.summary->eventsAt(point);
        if (events.empty())
            return;
        State stateBeforeOperation = state;
        for (const ConstructionEvent& event : events)
            applyEvent(state, stateBeforeOperation, static_cast<uint32_t>(&event - node.summary->events.span().data()));
    }

    const ConstructionGraph& graph;
    const ConstructionNode& node;
    SolvedConstruction solved { .mayLeak = false };
    std::optional<ConstructionNames> storedBeforeLeak { };
    std::optional<ConstructionNames> storedAtExit { };
    BitVector eventsReachingSelfReference { };
};

void solveStoredNames(const ConstructionGraph& graph, ConstructionNode& node)
{
    StoredNamesProblem problem { graph, node };
    node.construction->analyses->solveForward(problem);
    node.solved = WTF::move(problem.solved);
    if (problem.storedAtExit) {
        problem.storedBeforeLeak->subtract(node.solved.readBeforeStore);
        node.solved.storedBeforeLeak = WTF::move(*problem.storedBeforeLeak);
        node.solved.storedAtExit = WTF::move(*problem.storedAtExit);
    }
    for (size_t position : problem.eventsReachingSelfReference)
        node.resolvedEvents[position].mayLeak = true;
}

struct InitialOrderBuilder {
    void build()
    {
        reachableAfter.fill(std::nullopt, analyses.function().blocks.size());
        collectOrderPoints();

        bool everyPointAccepted = true;
        for (uint32_t index = 0; index < creatingPoints.size(); ++index) {
            uint32_t position = creatingPoints[index];
            if (!isOrderCandidate(position)) {
                everyPointAccepted = false;
                passedOverPoints.append(position);
                continue;
            }
            if (!isOrderPointAccepted(index) || !appendCreatedNames(position)) {
                everyPointAccepted = false;
                break;
            }
        }
        node.solved.storesOnlyInInitialOrder = !node.solved.mayLeak && everyPointAccepted;
    }

    const ConstructionEvent& eventAt(uint32_t position) const { return summary.events[position]; }
    uint32_t positionOf(const ConstructionEvent& event) const { return static_cast<uint32_t>(&event - summary.events.span().data()); }
    const ResolvedConstructionEvent& resolvedAt(uint32_t position) const { return node.resolvedEvents[position]; }
    const ConstructionName& nameAt(uint32_t position) const { return *eventAt(position).name; }
    const SolvedConstruction& solvedOf(uint32_t callee) const { return graph.nodes[callee].solved; }

    bool createsOwnKey(uint32_t position) const
    {
        if (eventAt(position).kind != ConstructionEventKind::Store || resolvedAt(position).mayLeak)
            return false;
        PropertyKeyID key = nameAt(position).key;
        return key && !isPrivateKey(analyses.program(), key);
    }

    bool mayCreateKeys(uint32_t position) const
    {
        return createsOwnKey(position) || std::ranges::any_of(resolvedAt(position).callees, [&](uint32_t callee) {
            return !solvedOf(callee).storesOnlyInInitialOrder || !solvedOf(callee).initialOrder.isEmpty();
        });
    }

    bool mayLeak(uint32_t position) const
    {
        return resolvedAt(position).mayLeak || std::ranges::any_of(resolvedAt(position).callees, [&](uint32_t callee) {
            return solvedOf(callee).mayLeak;
        });
    }

    bool mustCreateKeys(uint32_t position) const
    {
        const ConstructionEvent& event = eventAt(position);
        if (!event.isExact || resolvedAt(position).otherCodeMayRun || !(resolvedAt(position).callees.isEmpty() || event.kind == ConstructionEventKind::Call))
            return false;
        return std::ranges::all_of(summary.eventsAt(event.point), [&](const ConstructionEvent& other) {
            return positionOf(other) == position || !mayCreateKeys(positionOf(other));
        });
    }

    void collectOrderPoints()
    {
        for (BlockID block : cfg.reversePostOrder()) {
            uint32_t firstCreatingPoint = creatingPoints.size();
            for (const ConstructionEvent& event : std::ranges::equal_range(summary.events, block, { }, [](const ConstructionEvent& event) { return event.point.block; })) {
                uint32_t position = positionOf(event);
                if (mayCreateKeys(position))
                    creatingPoints.append(position);
                if (mayLeak(position))
                    leakPoints.append(position);
                if (event.kind == ConstructionEventKind::Exit)
                    exitPoints.append(position);
            }
            if (firstCreatingPoint < creatingPoints.size())
                creatingBlocks.append({ block, firstCreatingPoint, static_cast<uint32_t>(creatingPoints.size()) });
        }
    }

    const BitVector& blocksReachableAfter(BlockID block)
    {
        std::optional<BitVector>& reachable = reachableAfter[block.index()];
        if (!reachable) {
            reachable = BitVector(reachableAfter.size());
            for (BlockID successor : cfg.successors(block))
                reachable->merge(cfg.blocksReachableFrom(successor, { }));
        }
        return *reachable;
    }

    bool reaches(uint32_t from, uint32_t to)
    {
        ProgramPoint fromPoint = eventAt(from).point;
        ProgramPoint toPoint = eventAt(to).point;
        return (fromPoint.block == toPoint.block && fromPoint.index < toPoint.index) || blocksReachableAfter(fromPoint.block).get(toPoint.block.index());
    }

    bool isOrderCandidate(uint32_t position)
    {
        ProgramPoint point = eventAt(position).point;
        auto comesLater = [&](uint32_t later) {
            ProgramPoint laterPoint = eventAt(later).point;
            if (later == position || point.block == laterPoint.block)
                return later == position || point.index < laterPoint.index;
            return analyses.dominators().terminatorDominates(point.block, laterPoint.block);
        };
        return mustCreateKeys(position) && !exitPoints.isEmpty() && std::ranges::all_of(exitPoints, comesLater) && std::ranges::all_of(leakPoints, comesLater);
    }

    bool storesOnlyAccepted(uint32_t point, uint32_t candidate) const
    {
        auto isAccepted = [&](const ConstructionName& name) {
            return acceptedNames.contains(name) || (resolvedAt(candidate).callees.isEmpty() && nameAt(candidate) == name);
        };
        if (createsOwnKey(point) && !isAccepted(nameAt(point)))
            return false;
        return std::ranges::all_of(resolvedAt(point).callees, [&](uint32_t callee) {
            return solvedOf(callee).storesOnlyInInitialOrder && std::ranges::all_of(solvedOf(callee).initialOrder, isAccepted);
        });
    }

    bool isOrderPointAccepted(uint32_t index)
    {
        uint32_t position = creatingPoints[index];
        for (uint32_t leak : leakPoints) {
            if (reaches(leak, position))
                return false;
        }
        for (uint32_t point : passedOverPoints) {
            if (reaches(point, position) && !storesOnlyAccepted(point, position))
                return false;
        }
        for (const CreatingBlock& creatingBlock : creatingBlocks) {
            if (creatingBlock.end <= index + 1 || !blocksReachableAfter(creatingBlock.block).get(eventAt(position).point.block.index()))
                continue;
            for (uint32_t later = std::max(creatingBlock.begin, index + 1); later < creatingBlock.end; ++later) {
                if (!storesOnlyAccepted(creatingPoints[later], position))
                    return false;
            }
        }
        return true;
    }

    bool appendCreatedNames(uint32_t position)
    {
        auto addToOrder = [&](const ConstructionName& name) {
            if (acceptedNames.add(name))
                node.solved.initialOrder.append(name);
        };
        const Vector<uint32_t, 1>& callees = resolvedAt(position).callees;
        if (callees.isEmpty()) {
            addToOrder(nameAt(position));
            return true;
        }
        const SolvedConstruction& first = solvedOf(callees[0]);
        bool calleesAgree = callees.size() == 1 || std::ranges::all_of(callees, [&](uint32_t callee) {
            return solvedOf(callee).storesOnlyInInitialOrder && solvedOf(callee).initialOrder == first.initialOrder;
        });
        if (!calleesAgree)
            return false;
        for (const ConstructionName& name : first.initialOrder)
            addToOrder(name);
        return first.storesOnlyInInitialOrder;
    }

    struct CreatingBlock {
        BlockID block;
        uint32_t begin { 0 };
        uint32_t end { 0 };
    };

    const ConstructionGraph& graph;
    ConstructionNode& node;
    const ConstructionSummary& summary { *node.summary };
    FunctionAnalyses& analyses { *node.construction->analyses };
    const CFG& cfg { analyses.cfg() };
    Vector<uint32_t> creatingPoints { };
    Vector<CreatingBlock> creatingBlocks { };
    Vector<uint32_t> passedOverPoints { };
    Vector<uint32_t> leakPoints { };
    Vector<uint32_t> exitPoints { };
    Vector<std::optional<BitVector>> reachableAfter { };
    ConstructionNames acceptedNames { };
};

void solveConstructions(ConstructionGraph& graph)
{
    StronglyConnectedComponents components = computeStronglyConnectedComponents(graph.nodes.size(), [&](unsigned node) -> const Vector<uint32_t>& {
        return graph.nodes[node].callees;
    });
    for (uint32_t index : components.members) {
        ConstructionNode& node = graph.nodes[index];
        if (components.isInCycle.get(index))
            node.summary = nullptr;
        if (!node.summary)
            continue;
        solveStoredNames(graph, node);
        InitialOrderBuilder { graph, node }.build();
    }
}

size_t parentOrderLength(const ConstructionGraph& graph, const ConstructionNode& node)
{
    if (!node.summary)
        return 0;
    std::span<const ConstructionEvent> events = node.summary->events.span();
    auto rootEvent = std::ranges::find_if(events, [&](const ConstructionEvent& event) {
        return event.origin == node.root && event.operand == node.root;
    });
    if (rootEvent == events.end() || node.resolvedEvents[rootEvent - events.begin()].callees.isEmpty())
        return 0;
    return std::min(graph.nodes[node.resolvedEvents[rootEvent - events.begin()].callees[0]].solved.initialOrder.size(), node.solved.initialOrder.size());
}

std::pair<FieldOrder, bool> ownOrderOf(const Program& program, const ConstructionGraph& graph, const ConstructionNode& node, LayoutID layout)
{
    FieldOrder order;
    for (ConstructionName name : node.solved.initialOrder.span().subspan(parentOrderLength(graph, node))) {
        std::optional<uint32_t> field = name.key ? program.at(layout).findField(name.key) : name.field.layout == layout ? std::optional(name.field.field) : std::nullopt;
        if (!field)
            return { WTF::move(order), false };
        order.fields.append(*field);
    }
    return { WTF::move(order), true };
}

ValueSetID findNewTargetsOfOtherFunctions(const ValueFlowResultInputs& inputs, ValueFlowHeapView& heap)
{
    const Program& program = inputs.program;
    const ValueFlowUniverse& universe = inputs.universe();
    ValueSetMaker& maker = heap.maker();
    Vector<ValueFlowObjectID> newTargetsFound;
    auto allocatingClassOf = [&](ValueFlowObjectID closure) {
        LayoutID current = program.at(universe.closureFunction(closure)).constructedClass;
        for (size_t steps = 0; current && program.at(current).has(LayoutFlags::HasHeritage) && steps < program.layouts.size(); ++steps)
            current = predictedHeritageOf(program.at(current));
        return current;
    };
    auto withWhatOtherCallablesConstruct = [&](ValueSetID callees) {
        callees = heap.expand(callees);
        if (inputs.sets().meet(callees, universe.otherCallables()))
            callees = maker.unite(callees, heap.escaped());
        ValueFlowName target { ValueFlowNameKind::HiddenSlot, BuiltinSlots::BoundFunctionTarget.index() };
        for (ValueSetID bound = maker.intersect(callees, universe.otherCallables()); bound != ValueSets::nothing;) {
            ValueSetID targets = maker.intersect(heap.expand(heap.writtenUnder(bound, target)), ValueSets::complement(callees));
            callees = maker.unite(callees, targets);
            bound = maker.intersect(targets, universe.otherCallables());
        }
        return callees;
    };
    auto add = [&](ValueSetID callees, ValueSetID newTargets) {
        constexpr uint32_t severalCallees = UINT32_MAX;
        MapByNumber<uint32_t, uint32_t> calleeByAllocatingClass;
        inputs.sets().forEachMember(maker.intersect(withWhatOtherCallablesConstruct(callees), universe.closures()), [&](ValueFlowObjectID callee) {
            LayoutID allocatingClass = allocatingClassOf(callee);
            if (allocatingClass && !calleeByAllocatingClass.add(allocatingClass.index(), callee.index()).isNewEntry)
                calleeByAllocatingClass.set(allocatingClass.index(), severalCallees);
        });
        inputs.sets().forEachMember(maker.intersect(heap.expand(newTargets), universe.closures()), [&](ValueFlowObjectID newTarget) {
            LayoutID allocatingClass = allocatingClassOf(newTarget);
            auto callee = allocatingClass ? calleeByAllocatingClass.find(allocatingClass.index()) : calleeByAllocatingClass.end();
            if (!allocatingClass || (callee != calleeByAllocatingClass.end() && callee->value != newTarget.index()))
                newTargetsFound.append(newTarget);
        });
    };
    if (unseenCodeCanRun(inputs.solution)) {
        ValueSetID reached = maker.unite(universe.unknownObjectAlone(), inputs.solver().possibleValuesOf(inputs.bodies().nodes().ofHost(ValueFlowHostNode::ConstructorsAboveEscapedInstances)));
        add(reached, reached);
    }
    Vector<const ValueFlowStatement*> constructions = collectInParallel<const ValueFlowStatement*>(inputs.loops(), program.functions.size(), [&](unsigned index, Vector<const ValueFlowStatement*>& found) {
        if (!inputs.isReached(FunctionID(index)))
            return;
        for (const ValueFlowStatement& statement : inputs.bodies().statementsOf(index)) {
            if (statement.kind != ValueFlowStatementKind::Operation || (statement.opcode != Opcode::Construct && statement.opcode != Opcode::ConstructList))
                continue;
            const Value& construction = program.functions[index].at(statement.site);
            ValueID newTarget = construction.operand(*OperandPosition::newTarget(statement.opcode));
            if (newTarget && newTarget != construction.calleeOperand())
                found.append(&statement);
        }
    });
    for (const ValueFlowStatement* statement : constructions) {
        std::span<const ValueFlowOperand> operands = inputs.bodies().operandsOf(*statement);
        add(heap.possibleValuesOf(operands[0]), heap.possibleValuesOf(operands[*OperandPosition::newTarget(statement->opcode)]));
    }
    auto constructsWithNewTarget = [](const BuiltinCallback& callback) {
        return callback.constructs && callback.newTarget.kind != BuiltinPlaceKind::None;
    };
    BitVector builtinsThatConstruct = builtinsWithOverload(program.builtins(), [&](const BuiltinOverload& overload) {
        return std::ranges::any_of(overload.flow.callbacks, constructsWithNewTarget);
    });
    forEachBuiltinApplication(inputs, heap, builtinsThatConstruct, [&](const ValueFlowBuiltinApplication& application) {
        for (const BuiltinOverload& overload : program.builtins().overloadsOf(application.builtin)) {
            if (overload.mode != application.mode())
                continue;
            for (const BuiltinCallback& callback : overload.flow.callbacks) {
                if (!constructsWithNewTarget(callback))
                    continue;
                bool namesOperand = callback.newTarget.kind == BuiltinPlaceKind::Operand && callback.newTarget.operand;
                add(application.valueAt(callback.callee, maker), namesOperand ? application.valueAt(*callback.newTarget.operand, maker) : ValueSets::anything);
            }
        }
    });
    return maker.of(newTargetsFound.span());
}

BitVector findLayoutsAllocatedElsewhere(const ValueFlowResultInputs& inputs, const FlowResultTables& tables)
{
    const Program& program = inputs.program;
    Vector<LayoutID> allocated = collectInParallel<LayoutID>(inputs.loops(), program.functions.size(), [&](unsigned index, Vector<LayoutID>& found) {
        FunctionID id(index);
        bool isFollowed = inputs.isReached(id);
        if (!inputs.bodies().holdsAnyOf(id, notedOperations({ ValueFlowNotedOperation::NewObject })) || (!isFollowed && !isUnsolvedCode(inputs, tables, id)))
            return;
        const Function& function = program.functions[index];
        for (const Value& value : function.values) {
            if (value.opcode == Opcode::NewObject && program.at(value.layout()).kind == LayoutKind::Instance && !(isFollowed && allocatesObjectOfOwnClass(program, function, value)))
                found.append(value.layout());
        }
    });
    BitVector layouts(program.layouts.size());
    for (LayoutID layout : allocated) {
        layouts.quickSet(layout.index());
        layouts.quickSet(declaredLayoutOf(program, layout).index());
    }
    return layouts;
}

BitVector findFunctionsSeeingOnlyFinishedObjects(const Program& program, const ParallelLoops& loops, const ConstructionGraph& graph)
{
    BitVector holdsUnfinishedObject(program.functions.size());
    for (const ConstructionNode& node : graph.nodes)
        holdsUnfinishedObject.quickSet(node.function.index());

    BitVector functions(program.functions.size());
    Vector<uint32_t> found = findIndicesInParallel(loops, program.functions.size(), [&](unsigned index) {
        const Function& function = program.functions[index];
        return !function.constructedClass && function.kind != FunctionKind::Initializer && !holdsUnfinishedObject.quickGet(index);
    });
    for (uint32_t index : found)
        functions.quickSet(index);
    return functions;
}

Vector<uint32_t, 2> findMakers(const ValueFlowResultInputs& inputs, const ConstructionGraph& graph, LayoutID id)
{
    const Program& program = inputs.program;
    FunctionID constructor = program.at(id).constructor;
    Vector<uint32_t, 2> makers;
    if (!isFollowedCode(inputs, constructor))
        return makers;
    for (ValueID root : constructorRootsOf(program, program.at(constructor))) {
        std::optional<uint32_t> node = graph.nodeIndexes.getOptional(ConstructionGraph::keyOf(constructor, root, isConstructorFunctionOfOwnLayout(program, constructor)));
        if (!node)
            return { };
        makers.append(*node);
    }
    return makers;
}

void listNamesStoredByEveryMaker(const ConstructionGraph& graph, std::span<const uint32_t> makers, Vector<PropertyKeyID>& keys, Vector<FieldReference>& fields, Vector<PropertyKeyID>* keysStoredWithValue)
{
    ConstructionNames stored = graph.nodes[makers[0]].solved.storedBeforeLeak;
    for (uint32_t maker : makers)
        stored.intersect(graph.nodes[maker].solved.storedBeforeLeak);
    for (ConstructionName name : stored) {
        if (name.standsForValue) {
            if (name.key && keysStoredWithValue)
                keysStoredWithValue->append(name.key);
        } else if (name.key)
            keys.append(name.key);
        else
            fields.append(name.field);
    }
}

void addBirthsOfLiterals(const ValueFlowResultInputs& inputs, const FlowResultTables& tables, ValueFlowConstructions& result)
{
    using KeysByLayout = MapByNumber<uint32_t, Vector<PropertyKeyID>>;
    const Program& program = inputs.program;
    auto keepCommonKeys = [](KeysByLayout& keysOfEveryLiteral, uint32_t layout, Vector<PropertyKeyID>&& keys) {
        auto entry = keysOfEveryLiteral.add(layout, Vector<PropertyKeyID>());
        if (entry.isNewEntry) {
            entry.iterator->value = WTF::move(keys);
            return;
        }
        entry.iterator->value.removeAllMatching([&](PropertyKeyID key) {
            return !std::ranges::binary_search(keys, key);
        });
    };
    Vector<KeysByLayout> keysOfWorker(inputs.loops().width());
    forEachIndexInParallel(inputs.loops(), program.functions.size(), [&](unsigned index, unsigned worker) {
        FunctionID id(index);
        if (!inputs.bodies().holdsAnyOf(id, notedOperations({ ValueFlowNotedOperation::NewObject, ValueFlowNotedOperation::Data, ValueFlowNotedOperation::PieceOutsideSlots })))
            return;
        if (!inputs.isReached(id) && !isUnsolvedCode(inputs, tables, id))
            return;
        const Function& function = program.functions[index];
        for (const Value& value : function.values) {
            if (value.opcode == Opcode::Data) {
                forEachDataNodeInPreorder(program.at(value.data()).root, [&](uint32_t, const DataNode& node) {
                    const DataNode::Object* object = node.tryAs<DataNode::Object>();
                    if (!object || !object->layout || program.at(object->layout).kind != LayoutKind::Record || program.at(object->layout).isDerived())
                        return;
                    Vector<PropertyKeyID> keys;
                    for (const DataMember& member : object->members) {
                        if (program.at(object->layout).findField(member.key))
                            keys.append(member.key);
                        else if (program.nameClassOf(member.key) != NameClass::Index)
                            break;
                    }
                    AOT::sort(keys);
                    keepCommonKeys(keysOfWorker[worker], object->layout.index(), WTF::move(keys));
                });
                continue;
            }
            if (value.opcode == Opcode::DefineGetter || value.opcode == Opcode::DefineSetter || value.opcode == Opcode::ElemDefine || value.opcode == Opcode::CopyDataProperties) {
                const Value& object = function.at(value.operand(0));
                if (object.opcode == Opcode::NewObject && program.at(object.layout()).kind == LayoutKind::Record)
                    keepCommonKeys(keysOfWorker[worker], object.layout().index(), { });
                continue;
            }
            if (value.opcode != Opcode::NewObject)
                continue;
            const Layout& layout = program.at(value.layout());
            if (layout.kind != LayoutKind::Record || layout.isDerived())
                continue;
            Vector<PropertyKeyID> keys;
            for (uint32_t field : value.fields()) {
                if (PropertyKeyID key = layout.fields[field].key)
                    keys.append(key);
            }
            AOT::sort(keys);
            keepCommonKeys(keysOfWorker[worker], value.layout().index(), WTF::move(keys));
        }
    });
    for (KeysByLayout& keysOfOneWorker : keysOfWorker.mutableSpan().subspan(1)) {
        for (auto& [layout, keys] : keysOfOneWorker)
            keepCommonKeys(keysOfWorker[0], layout, WTF::move(keys));
    }
    for (auto& [layout, keys] : keysOfWorker[0])
        result.layouts[layout].keysStoredAtBirth = WTF::move(keys);
}

} // anonymous namespace

ValueFlowConstructions computeConstructions(const ValueFlowResultInputs& inputs, ValueFlowHeapView& heap, const FlowResultTables& tables)
{
    const Program& program = inputs.program;
    Vector<std::unique_ptr<FunctionConstruction>> functions(program.functions.size());
    ConstructionGraphBuilder builder { inputs, heap, functions, UnknownChains::MayTakeObject };
    ConstructionGraph graph = builder.build();
    solveConstructions(graph);

    ValueFlowConstructions result;
    result.layouts = Vector<ValueFlowConstructions::OfLayout>(program.layouts.size());
    result.functionsSeeingOnlyFinishedObjects = findFunctionsSeeingOnlyFinishedObjects(program, inputs.loops(), graph);

    ValueSetID newTargetsOfOtherFunctions = findNewTargetsOfOtherFunctions(inputs, heap);
    BitVector isAllocatedElsewhere = findLayoutsAllocatedElsewhere(inputs, tables);
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        const Layout& layout = program.layouts[index];
        if (layout.kind != LayoutKind::Instance || !layout.constructor || layout.isDerived())
            continue;
        Vector<uint32_t, 2> makers = findMakers(inputs, graph, LayoutID(index));
        if (makers.isEmpty())
            continue;

        ValueFlowConstructions::OfLayout& construction = result.layouts[index];
        bool createsFieldsOnlyInListedOrders = true;
        for (uint32_t maker : makers) {
            const ConstructionNode& node = graph.nodes[maker];
            auto [order, isComplete] = ownOrderOf(program, graph, node, LayoutID(index));
            createsFieldsOnlyInListedOrders &= !node.solved.mayLeak && node.solved.storesOnlyInInitialOrder && isComplete;
            if (!construction.orders.contains(order))
                construction.orders.append(WTF::move(order));
        }
        AOT::sort(construction.orders, [](const FieldOrder& a, const FieldOrder& b) {
            return std::ranges::lexicographical_compare(a.fields, b.fields);
        });

        if (isAllocatedElsewhere.quickGet(index) || inputs.sets().contains(newTargetsOfOtherFunctions, inputs.universe().closureOf(layout.constructor)))
            continue;
        construction.isFollowed = true;
        construction.createsFieldsOnlyInListedOrders = createsFieldsOnlyInListedOrders;
        listNamesStoredByEveryMaker(graph, makers.span(), construction.keysStoredBeforeObjectLeaks, construction.fieldsStoredBeforeObjectLeaks, nullptr);
    }

    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        const Layout& layout = program.layouts[index];
        if (layout.kind != LayoutKind::Instance || !layout.has(LayoutFlags::HasHeritage) || result.layouts[index].isFollowed)
            continue;
        Vector<PropertyKeyID>& keys = result.layouts[index].inheritedKeysThatStoresMayNotCreate;
        LayoutID above = predictedHeritageOf(layout);
        for (size_t steps = 0; above && steps < program.layouts.size(); ++steps) {
            for (const Field& field : program.at(above).fields) {
                if (field.key && !keys.contains(field.key) && builder.mayBeStoppedAboveObjectsOf(LayoutID(index), field.key))
                    keys.append(field.key);
            }
            above = predictedHeritageOf(program.at(above));
        }
        AOT::sort(keys);
    }

    ConstructionGraph graphUnderGuards = ConstructionGraphBuilder { inputs, heap, functions, UnknownChains::HoldNothingUnderName }.build();
    solveConstructions(graphUnderGuards);
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        const Layout& layout = program.layouts[index];
        if (layout.kind != LayoutKind::Instance || !layout.constructor || layout.isDerived() || layout.has(LayoutFlags::HasHeritage))
            continue;
        ValueFlowConstructions::OfLayout& construction = result.layouts[index];
        if (Vector<uint32_t, 2> makers = findMakers(inputs, graphUnderGuards, LayoutID(index)); !makers.isEmpty())
            listNamesStoredByEveryMaker(graphUnderGuards, makers.span(), construction.keysStoredAtBirth, construction.fieldsStoredAtBirth, &construction.keysStoredWithValueAtBirth);
    }
    addBirthsOfLiterals(inputs, tables, result);
    for (const TaggedFamily& family : program.taggedFamilies) {
        const ValueFlowConstructions::OfLayout& maker = result.layouts[family.maker.index()];
        for (const TaggedLayout& member : family.members) {
            ValueFlowConstructions::OfLayout& construction = result.layouts[member.layout.index()];
            construction.isFollowed = maker.isFollowed;
            construction.keysStoredBeforeObjectLeaks = maker.keysStoredBeforeObjectLeaks;
            construction.fieldsStoredBeforeObjectLeaks = maker.fieldsStoredBeforeObjectLeaks;
            construction.keysStoredAtBirth = maker.keysStoredAtBirth;
            construction.keysStoredWithValueAtBirth = maker.keysStoredWithValueAtBirth;
        }
    }
    return result;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
