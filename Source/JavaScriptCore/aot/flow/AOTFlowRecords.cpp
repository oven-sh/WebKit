/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowRecords.h"

#if ENABLE(AOT)

#include <algorithm>
#include <bit>
#include <limits>
#include <span>
#include <tuple>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/HexNumber.h>
#include <wtf/OptionSet.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT { namespace Typed {

using FlowWitnessMap = HashMap<std::pair<FlowObjectID, uint16_t>, Witness>;

template<typename T>
static T& ensureEntry(Vector<T>& table, size_t index)
{
    if (table.size() <= index)
        table.grow(index + 1);
    return table[index];
}

template<typename T>
static T entryOrDefault(const Vector<T>& table, size_t index)
{
    return index < table.size() ? table[index] : T { };
}

template<typename Flag>
static bool addFlag(Vector<OptionSet<Flag>>& table, FlowObjectID object, Flag flag)
{
    OptionSet<Flag>& recorded = ensureEntry(table, object.index());
    bool isNew = !recorded.contains(flag);
    recorded.add(flag);
    return isNew;
}

template<typename Flag>
static std::pair<FlowObjectID, uint16_t> witnessKey(FlowObjectID object, Flag flag)
{
    static_assert(sizeof(Flag) <= sizeof(uint16_t));
    return { object, static_cast<uint16_t>(flag) };
}

template<typename Flag>
static Witness findWitness(const FlowWitnessMap& witnesses, FlowObjectID object, Flag flag)
{
    auto found = witnesses.find(witnessKey(object, flag));
    return found == witnesses.end() ? noWitness : found->value;
}

template<typename Flag>
static void addWitness(FlowWitnessMap& witnesses, FlowObjectID object, Flag flag, const Witness& witness)
{
    if (hasFunction(witness))
        keepLowerWitness(witnesses.add(witnessKey(object, flag), noWitness).iterator->value, witness);
}

static size_t indexOf(ObjectMutation mutation)
{
    return std::countr_zero(static_cast<uint16_t>(mutation));
}

static const FlowBuiltinCallLink& sortKeyOf(const FlowBuiltinCallLink& link LIFETIME_BOUND)
{
    return link;
}

static const FlowUnknownCallee& sortKeyOf(const FlowUnknownCallee& callee LIFETIME_BOUND)
{
    return callee;
}

static auto sortKeyOf(const FlowBuiltinOperandValue& value)
{
    unsigned argumentIndex = value.operand.isReceiver() ? 0 : value.operand.argumentIndex();
    return std::tuple { value.callerInstance, value.site, value.overload, value.operand.kind(), argumentIndex, value.node };
}

static auto sortKeyOf(const FlowClassEvaluation& evaluation)
{
    return std::tuple { evaluation.layout, evaluation.instance, evaluation.origin };
}

static auto sortKeyOf(const FlowMemberDefinition& definition)
{
    return std::tuple { definition.layout, definition.side, definition.key, definition.kind, definition.function, definition.holder, definition.index, definition.constantKey };
}

static auto sortKeyOf(const Witness& witness)
{
    return std::tuple { witness.function, witness.value };
}

static auto sortKeyOf(const UnseenCodeRecord& record)
{
    return std::tuple { record.cause, record.builtin };
}

template<typename T>
static void sortAndRemoveDuplicates(Vector<T>& list)
{
    std::ranges::stable_sort(list.mutableSpan(), [](const T& a, const T& b) {
        return sortKeyOf(a) < sortKeyOf(b);
    });
    removeRepeatedElements(list, [](const T& a, const T& b) {
        return sortKeyOf(a) == sortKeyOf(b);
    });
}

static void sortAndMergeDuplicates(Vector<FlowBuiltinOperandValue>& values)
{
    std::ranges::sort(values.mutableSpan(), [](const FlowBuiltinOperandValue& a, const FlowBuiltinOperandValue& b) {
        return sortKeyOf(a) < sortKeyOf(b);
    });

    size_t numberOfValues = 0;
    for (const FlowBuiltinOperandValue& value : values) {
        if (numberOfValues && sortKeyOf(values[numberOfValues - 1]) == sortKeyOf(value)) {
            values[numberOfValues - 1].roles.add(value.roles);
            continue;
        }
        values[numberOfValues++] = value;
    }
    values.shrink(numberOfValues);
}

static Witness witnessOfLocal(const FlowGraph& graph, FlowNodeID node)
{
    if (graph.kindOf(node) != FlowNodeKind::Local)
        return noWitness;
    FlowInstanceID instance = graph.instanceOf(node);
    return graph.witnessOf(instance, graph.summaryOf(instance).originOf(graph.localOf(node)));
}

OptionSet<ObjectMutation> FlowRecords::possibleMutations(FlowObjectID object) const
{
    return entryOrDefault(m_objects.mutations, object.index());
}

unsigned FlowRecords::numberOfObjectsWith(ObjectMutation mutation) const
{
    return m_objects.numberOfObjectsWithMutation[indexOf(mutation)];
}

OptionSet<FlowObjectUse> FlowRecords::usesOf(FlowObjectID object) const
{
    return entryOrDefault(m_objects.uses, object.index());
}

OptionSet<FlowObjectEvent> FlowRecords::eventsOf(FlowObjectID object) const
{
    return entryOrDefault(m_objects.events, object.index());
}

Witness FlowRecords::witnessOfReach(FlowObjectID object) const
{
    for (ObjectMutation mutation : { ObjectMutation::ReachUnknownJavaScript, ObjectMutation::ReachNativeCode }) {
        Witness witness = witnessOf(object, mutation);
        if (hasFunction(witness))
            return witness;
    }
    return noWitness;
}

// Where nobody saw the operation, an object that came into a location which was open already, the allocation says which object it is.
Witness FlowRecords::witnessOf(FlowObjectID object, ObjectMutation mutation) const
{
    Witness witness = findWitness(m_objects.mutationWitnesses, object, mutation);
    if (hasFunction(witness) || !possibleMutations(object).contains(mutation))
        return witness;
    return witnessOf(object, FlowObjectEvent::Allocated);
}

Witness FlowRecords::witnessOf(FlowObjectID object, FlowObjectEvent event) const
{
    return findWitness(m_objects.eventWitnesses, object, event);
}

std::span<const Vector<uint32_t>> FlowRecords::initialFieldLists(FlowObjectID object) const
{
    if (!object)
        return { };
    auto found = m_objects.initialFieldLists.find(object);
    if (found == m_objects.initialFieldLists.end())
        return { };
    return found->value.span();
}

OperandUses FlowRecords::directUsesOf(FlowNodeID node) const
{
    return entryOrDefault(m_nodes.directUses, node.index());
}

const FlowLocationRecord& FlowRecords::recordOf(FlowNodeID location) const
{
    LocationRecordID record = entryOrDefault(m_nodes.locationRecordIDs, location.index());
    return record ? m_nodes.locationRecords[record.index()] : m_nodes.emptyLocationRecord;
}

Witness FlowRecords::unknownObjectWitness(FlowNodeID node) const
{
    UnknownObjectOriginID label = entryOrDefault(m_nodes.unknownObjectLabels, node.index());
    return label ? m_nodes.unknownObjectOrigins[label.index()].witness : noWitness;
}

const FlowBuiltinUse& FlowRecords::builtinUse(BuiltinID builtin) const
{
    return builtin.index() < m_program.builtinUses.size() ? m_program.builtinUses[builtin.index()] : m_program.unusedBuiltin;
}

bool FlowRecords::hasUnseenCode(UnseenCodeCause cause) const
{
    return m_program.unseenCode.containsIf([&](const UnseenCodeRecord& record) {
        return record.cause == cause;
    });
}

OptionSet<ObjectMutation> FlowRecords::addMutations(FlowObjectID object, OptionSet<ObjectMutation> mutations, const Witness& witness)
{
    for (ObjectMutation mutation : mutations)
        addWitness(m_objects.mutationWitnesses, object, mutation, witness);

    OptionSet<ObjectMutation>& recorded = ensureEntry(m_objects.mutations, object.index());
    OptionSet<ObjectMutation> added = mutations - recorded;
    recorded.add(mutations);
    for (ObjectMutation mutation : added)
        ++m_objects.numberOfObjectsWithMutation[indexOf(mutation)];
    return added;
}

bool FlowRecords::addUse(FlowObjectID object, FlowObjectUse use)
{
    return addFlag(m_objects.uses, object, use);
}

bool FlowRecords::addEvent(FlowObjectID object, FlowObjectEvent event, const Witness& witness)
{
    addWitness(m_objects.eventWitnesses, object, event, witness);
    bool isNew = addFlag(m_objects.events, object, event);
    if (isNew && event == FlowObjectEvent::Allocated)
        ++m_objects.numberOfAllocatedObjects;
    return isNew;
}

void FlowRecords::addInitialFieldList(FlowObjectID object, std::span<const uint32_t> fields)
{
    RELEASE_ASSERT(object);
    m_objects.initialFieldLists.add(object, Vector<Vector<uint32_t>> { }).iterator->value.append(Vector<uint32_t>(fields));
}

void FlowRecords::addDirectUses(FlowNodeID node, OperandUses uses)
{
    ensureEntry(m_nodes.directUses, node.index()).add(uses);
}

// False for a record that the list has.
template<typename Record>
static bool appendIfAbsent(Vector<Record>& records, HashedIndex& index, uint32_t hash, const Record& record, NOESCAPE const Invocable<bool(const Record&)> auto& isSame)
{
    bool isListed = !!index.find(hash, [&](uint32_t position) {
        return isSame(records[position]);
    });
    if (isListed)
        return false;
    index.add(hash, records.size());
    records.append(record);
    return true;
}

template<typename Record>
static bool appendIfAbsent(Vector<Record>& records, HashedIndex& index, uint32_t hash, const Record& record)
{
    return appendIfAbsent(records, index, hash, record, [&](const Record& listed) {
        return listed == record;
    });
}

static uint64_t numberOf(uint32_t first, uint32_t second)
{
    return static_cast<uint64_t>(first) << 32 | second;
}

void FlowRecords::addContentsRead(FlowNodeID container, FlowNodeID reader)
{
    if (!container || !reader)
        return;
    appendIfAbsent(m_calls.contentsReads, m_calls.contentsReadIndex, hashOfNumbers(container.index(), reader.index()), FlowContentsRead { container, reader });
}

void FlowRecords::addHoleCopy(FlowNodeID source, FlowObjectID result)
{
    if (!source || !result)
        return;
    if (m_calls.recordedHoleCopies.add({ source, result }).isNewEntry)
        m_calls.holeCopies.append({ source, result });
}

FlowLocationRecord& FlowRecords::ensureLocationRecord(FlowNodeID location)
{
    LocationRecordID& record = ensureEntry(m_nodes.locationRecordIDs, location.index());
    if (!record) {
        record = LocationRecordID(m_nodes.locationRecords.size());
        m_nodes.locationRecords.append(FlowLocationRecord { });
    }
    return m_nodes.locationRecords[record.index()];
}

void FlowRecords::noteUnknownObjectOrigin(FlowNodeID node, const Witness& witness)
{
    appendIfAbsent(m_nodes.unknownObjectOrigins, m_nodes.unknownObjectOriginIndex, hashOfNumbers(node.index(), numberOf(witness.function, witness.value)), FlowUnknownObjectOrigin { node, witness }, [&](const FlowUnknownObjectOrigin& listed) {
        return listed.firstNode == node && listed.witness.function == witness.function && listed.witness.value == witness.value;
    });
}

FlowCallLinkID FlowRecords::addCallLink(const FlowCallLink& link)
{
    m_calls.links.append(link);
    return FlowCallLinkID(m_calls.links.size() - 1);
}

bool FlowRecords::addEvaluatedModule(ModuleID module)
{
    return m_program.evaluatedModules.add(module.index());
}

void FlowRecords::addModuleLoad(const FlowModuleLoadRecord& load)
{
    if (!m_program.moduleLoads.contains(load))
        m_program.moduleLoads.append(load);
}

void FlowRecords::addLoadByUnknownName(const Witness& witness)
{
    if (hasFunction(witness))
        m_program.loadsByUnknownName.append(witness);
}

void FlowRecords::addStaticFieldAccess(const FlowStaticFieldAccess& access)
{
    uint32_t hash = hashOfNumbers(numberOf(access.variable.index(), access.function.index()), numberOf(access.origin.index(), access.isStore));
    appendIfAbsent(m_program.staticFieldAccesses, m_program.staticFieldAccessIndex, hash, access);
}

void FlowRecords::addMemberAccess(const FlowMemberAccess& access)
{
    const FlowMemberOperation& operation = access.operation;
    uint32_t hashOfOperation = hashOfNumbers(numberOf(operation.function.index(), operation.origin.index()), numberOf(operation.callSiteInstance.index(), access.isStore));
    appendIfAbsent(m_program.memberAccesses, m_program.memberAccessIndex, hashOfNumbers(numberOf(access.holder.index(), access.key.index()), hashOfOperation), access);
}

void FlowRecords::addUncertainMemberStore(const FlowMemberOperation& operation)
{
    appendIfAbsent(m_program.uncertainMemberStores, m_program.uncertainMemberStoreIndex, hashOfNumbers(numberOf(operation.function.index(), operation.origin.index()), operation.callSiteInstance.index()), operation);
}

bool FlowRecords::addCreatedNamespace(ModuleID module)
{
    return m_program.createdNamespaces.add(module.index());
}

bool FlowRecords::addFunctionPastWorkLimit(FunctionID function)
{
    return m_program.functionsPastWorkLimit.add(function.index());
}

OptionSet<BuiltinUse> FlowRecords::addBuiltinUse(BuiltinID builtin, OptionSet<BuiltinUse> uses, const Witness& witness)
{
    RELEASE_ASSERT(builtin);
    FlowBuiltinUse& record = ensureEntry(m_program.builtinUses, builtin.index());
    if (!uses.containsOnly(BuiltinUse::Tested))
        keepLowerWitness(record.firstUse, witness);
    if (uses.contains(BuiltinUse::Written))
        keepLowerWitness(record.firstWrite, witness);

    OptionSet<BuiltinUse> added = uses - record.uses;
    record.uses.add(uses);
    return added;
}

void FlowRecords::addLockedBuiltinWrite(BuiltinID holder, PropertyKeyID key, const Witness& witness)
{
    size_t existing = m_program.lockedBuiltinWrites.findIf([&](const BuiltinLocationFlow& write) {
        return write.holder == holder && write.key == key;
    });
    if (existing != notFound) {
        keepLowerWitness(m_program.lockedBuiltinWrites[existing].witness, witness);
        return;
    }
    m_program.lockedBuiltinWrites.append(BuiltinLocationFlow { holder, key, BuiltinLocationState::Constant, witness });
}

bool FlowRecords::addUnseenCode(const UnseenCodeRecord& record)
{
    size_t existing = m_program.unseenCode.findIf([&](const UnseenCodeRecord& other) {
        return sortKeyOf(other) == sortKeyOf(record);
    });
    if (existing != notFound) {
        keepLowerWitness(m_program.unseenCode[existing].witness, record.witness);
        return false;
    }
    m_program.unseenCode.append(record);
    return true;
}

void FlowRecords::finish(const FlowGraph& graph)
{
    for (Vector<Vector<uint32_t>>& lists : m_objects.initialFieldLists.values()) {
        std::ranges::sort(lists.mutableSpan(), [](const Vector<uint32_t>& a, const Vector<uint32_t>& b) {
            return std::ranges::lexicographical_compare(a.span(), b.span());
        });
        removeRepeatedElements(lists);
    }

    sortAndRemoveDuplicates(m_calls.builtinLinks);
    sortAndRemoveDuplicates(m_calls.unknownCallees);
    sortAndMergeDuplicates(m_calls.builtinOperandValues);
    sortAndRemoveDuplicates(m_program.classEvaluations);
    sortAndRemoveDuplicates(m_program.memberDefinitions);
    sortAndRemoveDuplicates(m_program.unseenCode);
    sortAndRemoveDuplicates(m_program.loadsByUnknownName);
    std::ranges::sort(m_program.lockedBuiltinWrites.mutableSpan(), { }, [](const BuiltinLocationFlow& write) {
        return std::tuple { write.holder, write.key };
    });

    computeUnknownObjectLabels(graph);
}

FlowRecords FlowRecords::copy() const
{
    FlowRecords records;
    records.m_objects = m_objects;
    records.m_nodes = m_nodes;
    records.m_calls = m_calls;
    records.m_program = m_program;
    return records;
}

void FlowRecords::retractEvaluatedModule(ModuleID module)
{
    m_program.evaluatedModules.clear(module.index());
}

FlowBuiltinUse FlowRecords::retractBuiltinUse(BuiltinID builtin)
{
    if (builtin.index() >= m_program.builtinUses.size())
        return { };
    return std::exchange(m_program.builtinUses[builtin.index()], FlowBuiltinUse { });
}

void FlowRecords::retractEvent(FlowObjectID object, FlowObjectEvent event)
{
    OptionSet<FlowObjectEvent>& events = ensureEntry(m_objects.events, object.index());
    if (event == FlowObjectEvent::Allocated && events.contains(event))
        --m_objects.numberOfAllocatedObjects;
    events.remove(event);
}

namespace {

using NameAddition = ScopedLambda<void(const String&)>;

} // anonymous namespace

template<typename Flag>
static void nameAddedFlags(const Vector<OptionSet<Flag>>& earlier, const Vector<OptionSet<Flag>>& later, ASCIILiteral prefix, ASCIILiteral what, const NameAddition& name)
{
    for (size_t index = 0; index < later.size(); ++index) {
        OptionSet<Flag> added = later[index] - entryOrDefault(earlier, index);
        if (!added.isEmpty())
            name(makeString(prefix, index, " gets "_s, what, ' ', hex(static_cast<uint64_t>(added.toRaw()))));
    }
}

static void nameAddedBits(const BitVector& earlier, const BitVector& later, ASCIILiteral prefix, ASCIILiteral what, const NameAddition& name)
{
    for (size_t index : later) {
        if (!earlier.get(index))
            name(makeString(prefix, index, ' ', what));
    }
}

template<typename T>
static void nameAddedElements(const Vector<T>& earlier, const Vector<T>& later, NOESCAPE const auto& keyOf, NOESCAPE const auto& describe, const NameAddition& name)
{
    using Key = decltype(keyOf(later[0]));
    Vector<Key> keys = earlier.map(keyOf);
    std::ranges::sort(keys);
    Vector<Key> namedKeys;
    for (const T& element : later) {
        Key key = keyOf(element);
        if (std::ranges::binary_search(keys, key) || namedKeys.contains(key))
            continue;
        namedKeys.append(key);
        name(describe(element));
    }
}

static String describeSite(FlowInstanceID instance, ValueID site)
{
    if (!site)
        return makeString('i', instance.index());
    return makeString('v', site.index(), " in i"_s, instance.index());
}

void FlowRecords::forEachAdditionSince(const FlowRecords& earlier, NOESCAPE const NameAddition& name) const
{
    nameAddedFlags(earlier.m_objects.mutations, m_objects.mutations, "o"_s, "mutations"_s, name);
    nameAddedFlags(earlier.m_objects.uses, m_objects.uses, "o"_s, "uses"_s, name);
    nameAddedFlags(earlier.m_objects.events, m_objects.events, "o"_s, "events"_s, name);
    for (auto& [object, lists] : m_objects.initialFieldLists) {
        std::span<const Vector<uint32_t>> earlierLists = earlier.initialFieldLists(object);
        for (const Vector<uint32_t>& list : lists) {
            if (std::ranges::find(earlierLists, list) == earlierLists.end())
                name(makeString('o', object.index(), " gets another list of initial fields"_s));
        }
    }

    nameAddedFlags(earlier.m_nodes.directUses, m_nodes.directUses, "n"_s, "direct uses"_s, name);
    for (size_t index = 0; index < m_nodes.locationRecordIDs.size(); ++index) {
        FlowNodeID location(static_cast<uint32_t>(index));
        const FlowLocationRecord& record = recordOf(location);
        const FlowLocationRecord& earlierRecord = earlier.recordOf(location);
        OptionSet<FlowLocationEvent> added = record.events - earlierRecord.events;
        if (!added.isEmpty())
            name(makeString('n', index, " gets location events "_s, hex(static_cast<uint64_t>(added.toRaw()))));
        if (record.deferrableFunction != earlierRecord.deferrableFunction || record.deferrableCapture != earlierRecord.deferrableCapture)
            name(makeString('n', index, " holds another deferrable closure"_s));
    }

    nameAddedElements(earlier.m_calls.links, m_calls.links, [](const FlowCallLink& link) {
        return std::tuple { link.callerInstance, link.site, link.calleeInstance, link.kind, link.times, link.isInCycle, link.result, link.thrown };
    }, [](const FlowCallLink& link) {
        return makeString(describeSite(link.callerInstance, link.site), " is linked to i"_s, link.calleeInstance.index());
    }, name);
    nameAddedElements(earlier.m_calls.operandFlows, m_calls.operandFlows, [](const FlowOperandFlow& flow) {
        return std::tuple { flow.from, flow.parameter, flow.link };
    }, [](const FlowOperandFlow& flow) {
        return makeString('n', flow.from.index(), " is passed to n"_s, flow.parameter.index(), " by call link "_s, flow.link.index());
    }, name);
    nameAddedElements(earlier.m_calls.contentsReads, m_calls.contentsReads, [](const FlowContentsRead& read) {
        return std::tuple { read.container, read.reader };
    }, [](const FlowContentsRead& read) {
        return makeString("the contents of n"_s, read.container.index(), " are read into n"_s, read.reader.index());
    }, name);
    nameAddedElements(earlier.m_calls.builtinLinks, m_calls.builtinLinks, [](const FlowBuiltinCallLink& link) {
        return link;
    }, [](const FlowBuiltinCallLink& link) {
        return makeString(describeSite(link.callerInstance, link.site), " calls built-in "_s, link.builtin.index(), " by another overload or in another way"_s);
    }, name);
    nameAddedElements(earlier.m_calls.unknownCallees, m_calls.unknownCallees, [](const FlowUnknownCallee& callee) {
        return callee;
    }, [](const FlowUnknownCallee& callee) {
        return makeString(describeSite(callee.callerInstance, callee.site), " gets an unknown callee"_s);
    }, name);
    nameAddedElements(earlier.m_calls.builtinOperandValues, m_calls.builtinOperandValues, [](const FlowBuiltinOperandValue& value) {
        return std::tuple { sortKeyOf(value), value.roles.toRaw() };
    }, [](const FlowBuiltinOperandValue& value) {
        return makeString(describeSite(value.callerInstance, value.site), " passes another operand value to a built-in"_s);
    }, name);

    nameAddedElements(earlier.m_program.classEvaluations, m_program.classEvaluations, [](const FlowClassEvaluation& evaluation) {
        return std::tuple { sortKeyOf(evaluation), evaluation.classObject, evaluation.prototypeObject, evaluation.isInCycle };
    }, [](const FlowClassEvaluation& evaluation) {
        return makeString(describeSite(evaluation.instance, evaluation.origin), " evaluates a class in another way"_s);
    }, name);
    nameAddedElements(earlier.m_program.memberDefinitions, m_program.memberDefinitions, [](const FlowMemberDefinition& definition) {
        return sortKeyOf(definition);
    }, [](const FlowMemberDefinition& definition) {
        return makeString('o', definition.holder.index(), " gets another member definition"_s);
    }, name);
    nameAddedBits(earlier.m_program.evaluatedModules, m_program.evaluatedModules, "m"_s, "is evaluated"_s, name);
    nameAddedElements(earlier.m_program.moduleLoads, m_program.moduleLoads, [](const FlowModuleLoadRecord& load) {
        return std::tuple { load.caller, load.origin, load.module };
    }, [](const FlowModuleLoadRecord& load) {
        return makeString(describeSite(load.caller, load.origin), " loads m"_s, load.module.index());
    }, name);
    nameAddedElements(earlier.m_program.loadsByUnknownName, m_program.loadsByUnknownName, [](const Witness& witness) {
        return sortKeyOf(witness);
    }, [](const Witness& witness) {
        return makeString("f"_s, witness.function, " loads a module by an unknown name"_s);
    }, name);
    nameAddedElements(earlier.m_program.staticFieldAccesses, m_program.staticFieldAccesses, [](const FlowStaticFieldAccess& access) {
        return std::tuple { access.variable, access.function, access.origin, access.isStore };
    }, [](const FlowStaticFieldAccess& access) {
        return makeString("static field variable "_s, access.variable.index(), " gets another access"_s);
    }, name);
    nameAddedElements(earlier.m_program.memberAccesses, m_program.memberAccesses, [](const FlowMemberAccess& access) {
        return std::tuple { access.holder, access.key, access.operation.function, access.operation.origin, access.operation.callSiteInstance, access.isStore };
    }, [](const FlowMemberAccess& access) {
        return makeString("a member of o"_s, access.holder.index(), " gets another access"_s);
    }, name);
    nameAddedElements(earlier.m_program.uncertainMemberStores, m_program.uncertainMemberStores, [](const FlowMemberOperation& operation) {
        return std::tuple { operation.function, operation.origin, operation.callSiteInstance };
    }, [](const FlowMemberOperation& operation) {
        return makeString("a store in f"_s, operation.function.index(), " may miss its member"_s);
    }, name);
    nameAddedBits(earlier.m_program.createdNamespaces, m_program.createdNamespaces, "m"_s, "gets a namespace object"_s, name);
    nameAddedBits(earlier.m_program.functionsPastWorkLimit, m_program.functionsPastWorkLimit, "f"_s, "goes past the work limit"_s, name);
    for (size_t index = 0; index < m_program.builtinUses.size(); ++index) {
        OptionSet<BuiltinUse> added = m_program.builtinUses[index].uses - earlier.builtinUse(BuiltinID(static_cast<uint32_t>(index))).uses;
        if (!added.isEmpty())
            name(makeString("built-in "_s, index, " gets uses "_s, hex(static_cast<uint64_t>(added.toRaw()))));
    }
    nameAddedElements(earlier.m_program.lockedBuiltinWrites, m_program.lockedBuiltinWrites, [](const BuiltinLocationFlow& write) {
        return std::tuple { write.holder, write.key };
    }, [](const BuiltinLocationFlow& write) {
        return makeString("locked built-in "_s, write.holder.index(), " gets another write"_s);
    }, name);
    nameAddedElements(earlier.m_program.unseenCode, m_program.unseenCode, [](const UnseenCodeRecord& record) {
        return sortKeyOf(record);
    }, [](const UnseenCodeRecord& record) {
        return makeString("unseen code gets cause "_s, static_cast<unsigned>(record.cause));
    }, name);
    if (m_program.unseenCodeCanRun && !earlier.m_program.unseenCodeCanRun)
        name("unseen code can run"_s);
    if (m_program.mayRunUnknownJavaScript && !earlier.m_program.mayRunUnknownJavaScript)
        name("unknown JavaScript may run"_s);
}

namespace {

using UnknownObjectArrival = FlowUnknownObjectOrigin;
using UnknownObjectDerivation = FlowUnknownObjectDerivation;

// An operation looks through its receiver, its callee, what it awaits or iterates over: what it finds there is unknown because that subject is.
// An argument is only passed along. A callee that is known may answer the unknown object whatever it is handed.
enum class SubjectRole : uint8_t { LookedThrough, PassedAlong };

struct OperationSubject {
    ValueID origin;
    FlowLocalID subject;
    SubjectRole role { SubjectRole::LookedThrough };
};

enum class ArrivalCause : uint8_t { Operation, OperationOrOperand, Subject };

// A node that holds the object already takes the label of no arrival. An origin is listed all the same.
enum class ArrivalKind : uint8_t { Origin, Other };

// Labels each node that holds the unknown object with the operation that let the object in: of all that lead to the node, the nearest.
class UnknownObjectLabeler {
public:
    static constexpr uint32_t noOrigin = std::numeric_limits<uint32_t>::max();

    UnknownObjectLabeler(const FlowGraph& graph, const FlowRecords& records)
        : m_graph(graph)
        , m_records(records)
        , m_labels(FillWith { }, graph.numberOfNodes(), noOrigin)
        , m_visited(graph.numberOfNodes())
    {
    }

    void run(Vector<UnknownObjectArrival>&& arrivals);

    Vector<UnknownObjectArrival> takeOrigins() { return WTF::move(m_origins); }
    Vector<FlowUnknownObjectEntry> takeEntries() { return WTF::move(m_entries); }
    Vector<UnknownObjectDerivation> takeDerivations() { return WTF::move(m_derivations); }
    std::span<const uint32_t> labels() const LIFETIME_BOUND { return m_labels.span(); }

private:
    bool holdsUnknownObject(FlowNodeID node) const { return m_graph.valueOf(node).hasUnknownObject(); }
    Witness witnessWithoutArrival(FlowNodeID) const;
    uint32_t addOrigin(FlowNodeID, const Witness&);
    ArrivalCause addDerivation(FlowInstanceID, const OperationSubject&, FlowNodeID result);
    void splitArrivalsOfOperation(std::span<const UnknownObjectArrival>, std::span<const OperationSubject> subjectsOfOperation);
    void addArrival(const UnknownObjectArrival&, ArrivalCause);
    void splitArrivals(std::span<const UnknownObjectArrival> ascendingArrivals);
    bool visit(FlowNodeID, uint32_t labelOfSource);
    void spread();
    void spreadFrom(std::span<const UnknownObjectArrival> ascendingArrivals, ArrivalKind);
    void labelRemainingNodes();

    const FlowGraph& m_graph;
    const FlowRecords& m_records;
    Vector<UnknownObjectArrival> m_origins;
    HashMap<uint64_t, uint32_t> m_originOfOperation;
    Vector<FlowUnknownObjectEntry> m_entries;
    Vector<uint32_t> m_labels;
    BitVector m_visited;
    Vector<UnknownObjectArrival> m_sources;
    Vector<UnknownObjectArrival> m_arrivalsBesideUnknownOperands;
    Vector<UnknownObjectArrival> m_derivedArrivals;
    Vector<UnknownObjectArrival> m_arrivalsWithoutOperation;
    Vector<UnknownObjectDerivation> m_derivations;
    Vector<FlowNodeID> m_frontier;
};

} // anonymous namespace

static auto arrivalOrderKey(const UnknownObjectArrival& arrival)
{
    return std::tuple { arrival.witness.function, arrival.witness.value, arrival.firstNode };
}

// What an operation finds through the unknown object is unknown because its subject is.
static Vector<OperationSubject> collectSubjects(const FlowStatements& statements)
{
    Vector<OperationSubject> subjects;
    auto add = [&](ValueID origin, FlowLocalID subject, SubjectRole role = SubjectRole::LookedThrough) {
        if (origin && subject)
            subjects.append(OperationSubject { origin, subject, role });
    };
    for (const FlowFieldAccess& access : statements.fieldAccesses)
        add(access.origin, access.receiver);
    for (const FlowPropertyAccess& access : statements.propertyAccesses)
        add(access.origin, access.receiver);
    for (const FlowElementAccess& access : statements.elementAccesses)
        add(access.origin, access.receiver);
    for (const FlowSlotAccess& access : statements.slotAccesses)
        add(access.origin, access.receiver);
    for (const FlowPrototypeAccess& access : statements.prototypeAccesses)
        add(access.origin, access.object);
    for (const FlowPropertyCopy& copy : statements.propertyCopies)
        add(copy.origin, copy.source);
    // What a built-in does with an operand nobody knows, and what an unknown callee gives back, come of the operand.
    for (const FlowCall& call : statements.calls) {
        add(call.origin, call.callee);
        add(call.origin, call.thisValue, call.calleeKey ? SubjectRole::LookedThrough : SubjectRole::PassedAlong);
        add(call.origin, call.argumentList, SubjectRole::PassedAlong);
        for (FlowLocalID argument : statements.list(call.arguments))
            add(call.origin, argument, SubjectRole::PassedAlong);
    }
    for (const FlowOperandUse& use : statements.operandUses)
        add(use.origin, use.operand);
    for (const FlowAwait& awaited : statements.awaits)
        add(awaited.origin, awaited.value);
    for (const FlowIteration& iteration : statements.iterations)
        add(iteration.origin, iteration.step == FlowIterationStep::Open ? iteration.iterable : iteration.state);

    std::ranges::sort(subjects, [](const OperationSubject& a, const OperationSubject& b) {
        return std::tuple { a.origin, a.subject } < std::tuple { b.origin, b.subject };
    });
    return subjects;
}

// Code that nobody sees hands the unknown object to whatever it can reach, and no operation of the program does that. The operation that let the holder of a location,
// or the function of a parameter, get there is the one to look at. Failing that, the allocation of the holder, or the function.
Witness UnknownObjectLabeler::witnessWithoutArrival(FlowNodeID node) const
{
    if (m_graph.kindOf(node) != FlowNodeKind::Local) {
        FlowObjectID holder = m_graph.objectOf(node);
        if (!holder || holder == unknownFlowObject)
            return noWitness;
        Witness witness = m_records.witnessOfReach(holder);
        return hasFunction(witness) ? witness : m_records.witnessOf(holder, FlowObjectEvent::Allocated);
    }

    Witness own = witnessOfLocal(m_graph, node);
    if (own.value != Wire::none || !hasFunction(own))
        return own;
    Witness lowest = noWitness;
    for (FlowObjectID closure : m_graph.objects().closuresOf(FunctionID(own.function)))
        keepLowerWitness(lowest, m_records.witnessOfReach(closure));
    return hasFunction(lowest) ? lowest : own;
}

uint32_t UnknownObjectLabeler::addOrigin(FlowNodeID node, const Witness& witness)
{
    if (!hasFunction(witness))
        return noOrigin;
    uint64_t key = ((static_cast<uint64_t>(witness.function) << 32) | witness.value) + 1;
    auto known = m_originOfOperation.add(key, static_cast<uint32_t>(m_origins.size()));
    if (known.isNewEntry)
        m_origins.append({ node, witness });
    uint32_t origin = known.iterator->value;
    m_entries.append({ node, UnknownObjectOriginID(origin) });
    return origin;
}

ArrivalCause UnknownObjectLabeler::addDerivation(FlowInstanceID instance, const OperationSubject& operationSubject, FlowNodeID result)
{
    if (m_graph.at(instance).state == FlowInstanceState::Created)
        return ArrivalCause::Operation;
    FlowNodeID subject = m_graph.nodeOf(instance, operationSubject.subject);
    if (!subject || subject == result || !holdsUnknownObject(subject))
        return ArrivalCause::Operation;
    m_derivations.append(UnknownObjectDerivation { subject, result });
    return operationSubject.role == SubjectRole::LookedThrough ? ArrivalCause::Subject : ArrivalCause::OperationOrOperand;
}

void UnknownObjectLabeler::addArrival(const UnknownObjectArrival& arrival, ArrivalCause cause)
{
    switch (cause) {
    case ArrivalCause::Operation:
        m_sources.append(arrival);
        return;
    case ArrivalCause::OperationOrOperand:
        m_arrivalsBesideUnknownOperands.append(arrival);
        return;
    case ArrivalCause::Subject:
        m_derivedArrivals.append(arrival);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// One store through the unknown object arrives at every location of every escaped object. Such arrivals, which lie in no instance of the function, have the subjects of all its instances
// in common: the first takes them, and the others follow the first, so that the work is the sum of the two numbers and not their product.
void UnknownObjectLabeler::splitArrivalsOfOperation(std::span<const UnknownObjectArrival> arrivals, std::span<const OperationSubject> subjectsOfOperation)
{
    FunctionID function(arrivals.front().witness.function);
    FlowNodeID firstArrivalOutside;
    ArrivalCause causeOutside = ArrivalCause::Operation;
    for (const UnknownObjectArrival& arrival : arrivals) {
        FlowNodeID node = arrival.firstNode;
        FlowInstanceID ownInstance = m_graph.instanceOf(node);
        if (ownInstance && m_graph.at(ownInstance).function == function) {
            ArrivalCause cause = ArrivalCause::Operation;
            for (const OperationSubject& subject : subjectsOfOperation)
                cause = std::max(cause, addDerivation(ownInstance, subject, node));
            addArrival(arrival, cause);
            continue;
        }

        if (firstArrivalOutside) {
            if (causeOutside != ArrivalCause::Operation)
                m_derivations.append(UnknownObjectDerivation { firstArrivalOutside, node });
            addArrival(arrival, causeOutside);
            continue;
        }
        firstArrivalOutside = node;
        for (const OperationSubject& subject : subjectsOfOperation) {
            m_graph.forEachInstanceOf(function, [&](FlowInstanceID instance) {
                causeOutside = std::max(causeOutside, addDerivation(instance, subject, node));
            });
        }
        addArrival(arrival, causeOutside);
    }
}

void UnknownObjectLabeler::splitArrivals(std::span<const UnknownObjectArrival> ascendingArrivals)
{
    while (!ascendingArrivals.empty()) {
        uint32_t function = ascendingArrivals.front().witness.function;
        size_t count = std::ranges::find_if(ascendingArrivals, [&](const UnknownObjectArrival& arrival) {
            return arrival.witness.function != function;
        }) - ascendingArrivals.begin();

        Vector<OperationSubject> subjects = collectSubjects(m_graph.summaryOf(FunctionID(function)).statements);
        std::span<const UnknownObjectArrival> arrivalsOfFunction = ascendingArrivals.first(count);
        while (!arrivalsOfFunction.empty()) {
            uint32_t value = arrivalsOfFunction.front().witness.value;
            size_t countOfOperation = std::ranges::find_if(arrivalsOfFunction, [&](const UnknownObjectArrival& arrival) {
                return arrival.witness.value != value;
            }) - arrivalsOfFunction.begin();
            if (value == Wire::none)
                m_arrivalsWithoutOperation.append(arrivalsOfFunction.first(countOfOperation));
            else
                splitArrivalsOfOperation(arrivalsOfFunction.first(countOfOperation), std::ranges::equal_range(subjects, ValueID(value), { }, &OperationSubject::origin));
            arrivalsOfFunction = arrivalsOfFunction.subspan(countOfOperation);
        }
        ascendingArrivals = ascendingArrivals.subspan(count);
    }
    std::ranges::sort(m_derivations, [](const UnknownObjectDerivation& a, const UnknownObjectDerivation& b) {
        return std::tuple { a.subject, a.result } < std::tuple { b.subject, b.result };
    });
}

bool UnknownObjectLabeler::visit(FlowNodeID node, uint32_t labelOfSource)
{
    if (m_visited.quickGet(node.index()) || !holdsUnknownObject(node))
        return false;
    m_visited.quickSet(node.index());
    m_labels[node.index()] = labelOfSource != noOrigin ? labelOfSource : addOrigin(node, witnessWithoutArrival(node));
    m_frontier.append(node);
    return true;
}

// Level by level, and in each level in the order of the labels, so that of two origins at the same distance the lower witness wins whatever the order of the arrivals.
void UnknownObjectLabeler::spread()
{
    Vector<FlowNodeID> level;
    while (!m_frontier.isEmpty()) {
        level = std::exchange(m_frontier, { });
        std::ranges::sort(level, [&](FlowNodeID a, FlowNodeID b) {
            return std::tuple { m_labels[a.index()], a } < std::tuple { m_labels[b.index()], b };
        });
        for (FlowNodeID node : level) {
            uint32_t label = m_labels[node.index()];
            m_graph.forEachCopyTarget(node, [&](FlowNodeID target) {
                visit(target, label);
            });
            for (const UnknownObjectDerivation& derivation : std::ranges::equal_range(m_derivations, node, { }, &UnknownObjectDerivation::subject))
                visit(derivation.result, label);
        }
    }
}

void UnknownObjectLabeler::spreadFrom(std::span<const UnknownObjectArrival> ascendingArrivals, ArrivalKind kind)
{
    for (const UnknownObjectArrival& arrival : ascendingArrivals) {
        if (kind == ArrivalKind::Origin || !m_visited.quickGet(arrival.firstNode.index()))
            visit(arrival.firstNode, addOrigin(arrival.firstNode, arrival.witness));
    }
    spread();
}

void UnknownObjectLabeler::labelRemainingNodes()
{
    for (uint32_t index = 0; index < m_graph.numberOfNodes(); ++index)
        visit(FlowNodeID(index), noOrigin);
    m_frontier.clear();
}

void UnknownObjectLabeler::run(Vector<UnknownObjectArrival>&& arrivals)
{
    std::ranges::sort(arrivals, [](const UnknownObjectArrival& a, const UnknownObjectArrival& b) {
        return arrivalOrderKey(a) < arrivalOrderKey(b);
    });
    static_assert(Wire::none == std::numeric_limits<uint32_t>::max());
    size_t numberOfWitnessedArrivals = std::ranges::find_if(arrivals, [](const UnknownObjectArrival& arrival) {
        return !hasFunction(arrival.witness);
    }) - arrivals.begin();

    splitArrivals(arrivals.span().first(numberOfWitnessedArrivals));
    spreadFrom(m_sources.span(), ArrivalKind::Origin);
    // The label says the operand, which is the nearer cause where both are one.
    spreadFrom(m_arrivalsBesideUnknownOperands.span(), ArrivalKind::Origin);
    // A cycle of operations that each find the object through the next has no origin outside: each names itself.
    spreadFrom(m_derivedArrivals.span(), ArrivalKind::Other);
    // A witness that names a function alone says the least.
    spreadFrom(m_arrivalsWithoutOperation.span(), ArrivalKind::Origin);
    spreadFrom(arrivals.span().subspan(numberOfWitnessedArrivals), ArrivalKind::Other);
    labelRemainingNodes();
}

void FlowRecords::computeUnknownObjectLabels(const FlowGraph& graph)
{
    UnknownObjectLabeler labeler(graph, *this);
    labeler.run(std::exchange(m_nodes.unknownObjectOrigins, { }));
    m_nodes.unknownObjectOriginIndex = { };

    m_nodes.unknownObjectLabels.fill(UnknownObjectOriginID(), graph.numberOfNodes());
    std::span<const uint32_t> labels = labeler.labels();
    for (size_t index = 0; index < labels.size(); ++index) {
        if (labels[index] != UnknownObjectLabeler::noOrigin)
            m_nodes.unknownObjectLabels[index] = UnknownObjectOriginID(labels[index]);
    }
    m_nodes.unknownObjectOrigins = labeler.takeOrigins();
    m_nodes.unknownObjectEntries = labeler.takeEntries();
    m_nodes.unknownObjectDerivations = labeler.takeDerivations();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
