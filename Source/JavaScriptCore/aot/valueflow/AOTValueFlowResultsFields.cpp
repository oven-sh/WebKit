/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTKnownClosures.h"
#include "AOTValueFlowResultsInternal.h"

#if ENABLE(AOT)

#include "AOTBlockCycles.h"
#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTCFG.h"
#include "AOTFlowApply.h"
#include "AOTFlowRecords.h"
#include "AOTFlowSiteData.h"
#include "AOTLayout.h"
#include "AOTPhaseTimes.h"
#include "AOTSchemaOperandIndexes.h"
#include "AOTSort.h"
#include "AOTStringArena.h"
#include "AOTTypeArena.h"
#include "AOTValueFlowBuiltins.h"
#include "AOTValueFlowBuiltinTables.h"
#include "AOTValueFlowPositions.h"
#include "AOTValueFlowResultsUnseenCode.h"
#include "AOTValueFlowRules.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <algorithm>
#include <atomic>
#include <wtf/Atomics.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Lock.h>
#include <wtf/UniqueArray.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr ValueFlowName prototypeLinkName = ValueFlowName::of(ValueFlowNameKind::PrototypeLink);
static constexpr ValueFlowName elementsName = ValueFlowName::of(ValueFlowNameKind::Elements);

// Whoever runs one of these on an object sees which keys it has and in which order.
static constexpr Protocols protocolsListingKeys { Protocol::OwnKeys, Protocol::OwnValues, Protocol::ToJSON };

static constexpr OptionSet<ObjectMutation> reachMutations { ObjectMutation::ReachNativeCode, ObjectMutation::ReachUnknownJavaScript };

static bool hasNodeOfItsOwn(const Field& field)
{
    return field.flags.containsAny({ FieldFlags::IsPrivate, FieldFlags::IsBrand, FieldFlags::IsHidden });
}

static uint64_t numberOf(ValueFlowName name)
{
    return static_cast<uint64_t>(name.kind) << 32 | name.payload;
}

// Nobody has followed the values through such code.
static bool isCompiledWithoutSolve(const ValueFlowResultInputs& inputs, const FlowResultTables& tables, FunctionID id)
{
    return isUnsolvedCode(inputs, tables, id);
}

// The layout whose fields the objects of this one are expected to hold beside their own.
static LayoutID layoutExpectedAbove(const Program& program, LayoutID id)
{
    const Layout& layout = program.at(id);
    return layout.isDerived() ? layout.flow.base : predictedHeritageOf(layout);
}

static LayoutID provenBaseOf(const FlowApplication& application, LayoutID id)
{
    const LayoutFlow& flow = application.layouts[id.index()].flow;
    return flow.baseState == BaseState::Layout ? flow.base : LayoutID();
}

ValueFlowNodesAskedFor::ValueFlowNodesAskedFor(const ValueFlowResultInputs& inputs)
{
    constexpr uint32_t statementsPerTask = 2048;
    const ValueFlowSolver& solver = inputs.solver();
    uint32_t numberOfStatements = solver.numberOfStatements();
    uint32_t numberOfTasks = (numberOfStatements + statementsPerTask - 1) / statementsPerTask;
    m_first = Vector<uint32_t>(numberOfStatements + 1);
    Vector<Vector<uint32_t>> wordsOfTasks(numberOfTasks);
    forEachIndexInParallel(inputs.loops(), numberOfTasks, [&](unsigned task) {
        Vector<uint32_t>& words = wordsOfTasks[task];
        for (uint32_t statement = task * statementsPerTask; statement < std::min(numberOfStatements, (task + 1) * statementsPerTask); ++statement) {
            m_first[statement] = words.size();
            solver.forEachMadeNodeAskedForBy(ValueFlowStatementID(statement), [&](const ValueFlowNodeKey& key, ValueFlowStatementID made) {
                uint32_t index = solver.indexOfMadeNodes(key);
                RELEASE_ASSERT(index <= maskOfIndex);
                words.append(static_cast<uint32_t>(key.kind) << (bitsOfIndex + 1) | (made ? hasStatement : 0) | index);
            });
        }
    });
    Vector<uint32_t> firstWordOfTask(numberOfTasks + 1);
    uint32_t numberOfWords = 0;
    for (uint32_t task = 0; task < numberOfTasks; ++task) {
        firstWordOfTask[task] = numberOfWords;
        numberOfWords += wordsOfTasks[task].size();
    }
    m_first[numberOfStatements] = numberOfWords;
    m_words = Vector<uint32_t>(numberOfWords);
    forEachIndexInParallel(inputs.loops(), numberOfTasks, [&](unsigned task) {
        for (uint32_t statement = task * statementsPerTask; statement < std::min(numberOfStatements, (task + 1) * statementsPerTask); ++statement)
            m_first[statement] += firstWordOfTask[task];
        std::ranges::copy(wordsOfTasks[task], m_words.mutableSpan().subspan(firstWordOfTask[task]).begin());
        wordsOfTasks[task] = { };
    });
}

ValueFlowHeapIndex::ValueFlowHeapIndex(const ValueFlowResultInputs& inputs)
{
    collectNodesWritten(inputs);
    collectCalls(inputs);
}

void ValueFlowHeapIndex::collectNodesWritten(const ValueFlowResultInputs& inputs)
{
    struct NodeUnderName {
        uint64_t name;
        uint32_t node;
        WrittenNode written;
    };

    struct FoundInChunk {
        Vector<NodeUnderName> written;
        Vector<ValueFlowNodeID> firstNodesOfCallGroups;
        Vector<ValueFlowNodeID> nodesOfMarks;
    };

    // The nodes of a name stand in the order of their numbers.
    constexpr uint32_t nodesPerChunk = 2048;
    const ValueFlowSolver& solver = inputs.solver();
    const ParallelLoops& loops = inputs.loops();
    uint32_t firstMadeNode = solver.input().numberOfNodes;
    uint32_t numberOfMadeNodes = solver.graph().numberOfNodes() - firstMadeNode;
    Vector<FoundInChunk> chunks((numberOfMadeNodes + nodesPerChunk - 1) / nodesPerChunk);
    forEachIndexInParallel(loops, chunks.size(), [&](unsigned chunk) {
        FoundInChunk& found = chunks[chunk];
        for (uint32_t position = chunk * nodesPerChunk; position < std::min(numberOfMadeNodes, (chunk + 1) * nodesPerChunk); ++position) {
            ValueFlowNodeID node(firstMadeNode + position);
            const ValueFlowNodeKey& key = *solver.keyOfMadeNode(node);
            switch (key.kind) {
            case ValueFlowMadeNodeKind::Written:
                if (ValueSetID value = solver.valueOf(node); value != ValueSets::nothing)
                    found.written.append({ numberOf(key.name()), node.index(), { key.set, value } });
                break;
            case ValueFlowMadeNodeKind::CallGroup:
                if (solver.firstOfMadeNodesAt(solver.indexOfMadeNodes(key)) == node)
                    found.firstNodesOfCallGroups.append(node);
                break;
            case ValueFlowMadeNodeKind::Marked:
                found.nodesOfMarks.append(node);
                break;
            default:
                break;
            }
        }
    });
    size_t numberOfCallGroups = 0;
    for (const FoundInChunk& found : chunks)
        numberOfCallGroups += found.firstNodesOfCallGroups.size();
    m_firstNodesOfCallGroups.reserveInitialCapacity(numberOfCallGroups);
    Vector<Vector<NodeUnderName>> nodesOfChunks(chunks.size(), [&](size_t chunk) {
        return WTF::move(chunks[chunk].written);
    });
    for (FoundInChunk& found : chunks) {
        m_firstNodesOfCallGroups.appendVector(WTF::move(found.firstNodesOfCallGroups));
        m_nodesOfMarks.appendVector(WTF::move(found.nodesOfMarks));
    }
    chunks.clear();
    ParallelArray<NodeUnderName> nodes = ParallelArray<NodeUnderName>::concatenationOf(loops, WTF::move(nodesOfChunks));
    parallelSort(loops, nodes.mutableSpan(), [](const NodeUnderName& a, const NodeUnderName& b) {
        return std::pair { a.name, a.node } < std::pair { b.name, b.node };
    });

    Vector<uint32_t> firstNodeOfName;
    for (uint32_t index = 0; index < nodes.size(); ++index) {
        if (!index || nodes[index - 1].name != nodes[index].name)
            firstNodeOfName.append(index);
    }
    uint32_t numberOfNames = firstNodeOfName.size();
    firstNodeOfName.append(nodes.size());
    m_namesWritten.grow(numberOfNames);
    m_positionOfNameWritten.reserve(loops, numberOfNames);
    m_nodesOfNamesWritten = ParallelArray<NodesWrittenUnderName>(loops, numberOfNames, [&](unsigned position) {
        std::span<const NodeUnderName> nodesOfName = nodes.span().subspan(firstNodeOfName[position], firstNodeOfName[position + 1] - firstNodeOfName[position]);
        m_namesWritten[position] = nodesOfName[0].name;
        m_positionOfNameWritten.addConcurrently(IntHash<uint64_t>::hash(nodesOfName[0].name), position);
        NodesWrittenUnderName entry;
        entry.number = position;
        entry.nodes = Vector<WrittenNode>(nodesOfName.size(), [&](size_t index) {
            return nodesOfName[index].written;
        });
        indexNodesWrittenUnder(inputs, entry);
        return entry;
    });
    m_positionOfNameWritten.didAddConcurrently(numberOfNames);
    inputs.build.destroyLater(WTF::move(nodes));
}

// On any thread.
void ValueFlowHeapIndex::indexNodesWrittenUnder(const ValueFlowResultInputs& inputs, NodesWrittenUnderName& entry)
{
    constexpr size_t fewestNodesToIndex = 16;
    constexpr uint32_t mostMembersToIndex = 64;
    if (entry.nodes.size() < fewestNodesToIndex)
        return;

    BorrowedValueSetMaker maker(inputs.makerPool());
    const ValueSets& sets = inputs.sets();
    Vector<ValueSetID> throughUnknownObject;
    Vector<ValueSetID> throughUntrackedObject;
    for (uint32_t index = 0; index < entry.nodes.size(); ++index) {
        const WrittenNode& node = entry.nodes[index];
        if (sets.contains(node.objects, ValueFlowUniverse::unknownObject()))
            throughUnknownObject.append(node.value);
        if (sets.contains(node.objects, ValueFlowUniverse::untrackedObject()))
            throughUntrackedObject.append(node.value);
        if (ValueSets::isComplement(node.objects) || sets.size(node.objects) > mostMembersToIndex) {
            entry.nodesOfManyMembers.append(index);
            continue;
        }
        sets.forEachMember(node.objects, [&](ValueFlowObjectID member) {
            entry.nodesOfMembers.append({ member.index(), index });
        });
    }
    AOT::sort(entry.nodesOfMembers);
    entry.writtenThroughUnknownObject = maker.get().uniteAll(throughUnknownObject.span());
    entry.writtenThroughUntrackedObject = maker.get().uniteAll(throughUntrackedObject.span());
    entry.isIndexed = true;
}

ValueFlowHeapView::ValueFlowHeapView(const ValueFlowResultInputs& inputs)
    : m_inputs(inputs)
    , m_ownIndex(inputs.heapIndex ? nullptr : makeUnique<ValueFlowHeapIndex>(inputs))
    , m_index(inputs.heapIndex ? *inputs.heapIndex : *m_ownIndex)
    , m_makerPool(inputs.makerPool())
    , m_maker(m_makerPool.take())
    , m_escaped(inputs.solver().valueOf(ValueFlowProgramNodes::escaped()))
    , m_lost(inputs.solver().valueOf(ValueFlowProgramNodes::lost()))
    , m_escapedAndWritable(m_maker.intersect(m_escaped, ValueSets::complement(unseenCodeChangesRealm(inputs.solution) ? inputs.universe().lockedObjects() : inputs.universe().objectsOfRealm())))
{
}

ValueFlowHeapView::~ValueFlowHeapView()
{
    m_makerPool.giveBack(m_maker);
}

static ValueSetID valueSeenByRulesAt(const ValueFlowResultInputs& inputs, ValueSetMaker& maker, const ValueFlowOperand& operand)
{
    ValueSetID value = operand.constant ? operand.constant : ValueSets::nothing;
    if (operand.node)
        value = maker.unite(value, inputs.solver().valueOf(operand.node));
    return ValueFlowRuleContext::throughMaskOf(operand, value, inputs.solver(), maker);
}

ValueSetID ValueFlowHeapView::valueOf(const ValueFlowOperand& operand)
{
    return valueOf(m_maker, operand);
}

ValueSetID ValueFlowHeapView::valueOf(ValueSetMaker& maker, const ValueFlowOperand& operand) const
{
    return valueSeenByRulesAt(m_inputs, maker, operand);
}

ValueSetID ValueFlowHeapView::expand(ValueSetID set)
{
    return expand(m_maker, set);
}

ValueSetID ValueFlowHeapView::expand(ValueSetMaker& maker, ValueSetID set) const
{
    const ValueSets& sets = m_inputs.sets();
    ValueSetID expanded = set;
    if (sets.contains(set, ValueFlowUniverse::unknownObject()))
        expanded = maker.unite(expanded, m_escaped);
    if (sets.contains(set, ValueFlowUniverse::untrackedObject()))
        expanded = maker.unite(expanded, m_lost);
    return expanded;
}

ValueSetID ValueFlowHeapView::expandAmong(ValueSetMaker& maker, ValueSetID set, ValueSetID among) const
{
    const ValueSets& sets = m_inputs.sets();
    ValueSetID expanded = maker.intersect(set, among);
    if (sets.contains(set, ValueFlowUniverse::unknownObject()))
        expanded = maker.unite(expanded, maker.intersect(m_escaped, among));
    if (sets.contains(set, ValueFlowUniverse::untrackedObject()))
        expanded = maker.unite(expanded, maker.intersect(m_lost, among));
    return expanded;
}

bool ValueFlowHeapView::reaches(ValueSetID writtenThrough, ValueSetID objects) const
{
    const ValueSets& sets = m_inputs.sets();
    if (sets.meet(writtenThrough, objects))
        return true;
    if (sets.contains(writtenThrough, ValueFlowUniverse::unknownObject()) && sets.meet(objects, m_escapedAndWritable))
        return true;
    return sets.contains(writtenThrough, ValueFlowUniverse::untrackedObject()) && sets.meet(objects, m_lost);
}

ValueSetID ValueFlowHeapView::writtenUnder(ValueSetID objects, ValueFlowName name)
{
    return writtenUnder(m_maker, objects, name);
}

ValueSetID ValueFlowHeapView::writtenUnder(ValueSetMaker& maker, ValueSetID objects, ValueFlowName name) const
{
    const NodesWrittenUnderName* nodes = m_index.findNodesWrittenUnder(name);
    if (!nodes)
        return ValueSets::nothing;
    constexpr size_t fewestNodesToRemember = 4;
    if (nodes->nodes.size() < fewestNodesToRemember)
        return findWrittenUnder(maker, objects, *nodes);
    return m_index.writtenUnderNames().ensure(static_cast<uint64_t>(nodes->number) << 32 | objects.index(), [&] {
        return findWrittenUnder(maker, objects, *nodes);
    });
}

ValueSetID ValueFlowHeapView::findWrittenUnder(ValueSetMaker& maker, ValueSetID objects, const NodesWrittenUnderName& entry) const
{
    const ValueSets& sets = m_inputs.sets();
    if (!entry.isIndexed || ValueSets::isComplement(objects) || sets.size(objects) > entry.nodes.size()) {
        ValueSetID written = ValueSets::nothing;
        for (const WrittenNode& node : entry.nodes) {
            if (reaches(node.objects, objects))
                written = maker.unite(written, node.value);
        }
        return written;
    }

    Vector<ValueSetID, 16> parts;
    if (entry.writtenThroughUnknownObject != ValueSets::nothing && sets.meet(objects, m_escapedAndWritable))
        parts.append(entry.writtenThroughUnknownObject);
    if (entry.writtenThroughUntrackedObject != ValueSets::nothing && sets.meet(objects, m_lost))
        parts.append(entry.writtenThroughUntrackedObject);
    for (uint32_t index : entry.nodesOfManyMembers) {
        if (sets.meet(entry.nodes[index].objects, objects))
            parts.append(entry.nodes[index].value);
    }
    sets.forEachMember(objects, [&](ValueFlowObjectID member) {
        auto first = std::ranges::lower_bound(entry.nodesOfMembers, NodeOfMember { member.index(), 0 });
        for (; first != entry.nodesOfMembers.end() && first->member == member.index(); ++first)
            parts.append(entry.nodes[first->node].value);
    });
    AOT::sort(parts);
    removeRepeatedElements(parts);
    return maker.uniteAll(parts.span());
}

static BuiltinClassID classOfFunctionObject(const Function& function)
{
    bool isAsync = function.has(FunctionFlags::IsAsync);
    if (function.has(FunctionFlags::IsGenerator))
        return isAsync ? BuiltinClasses::AsyncGeneratorFunction : BuiltinClasses::GeneratorFunction;
    return isAsync ? BuiltinClasses::AsyncFunction : BuiltinClasses::Function;
}

static BuiltinClassID classAbovePrototypeProperty(const Function& function)
{
    if (!function.has(FunctionFlags::IsGenerator))
        return BuiltinClasses::Object;
    return function.has(FunctionFlags::IsAsync) ? BuiltinClasses::AsyncGenerator : BuiltinClasses::Generator;
}

// As the rule of a lookup has it. The unknown object stands for a prototype that nothing names.
void ValueFlowHeapView::appendPrototypesAtCreation(ValueFlowObjectID object, Vector<ValueFlowObjectID, 8>& prototypes) const
{
    const Program& program = m_inputs.program;
    const BuiltinTable& builtins = program.builtins();
    const ValueFlowUniverse& universe = m_inputs.universe();
    auto appendPrototypeOf = [&](BuiltinClassID builtinClass) {
        if (!builtinClass)
            return;
        BuiltinID prototype = builtins.at(builtinClass).prototype;
        prototypes.append(prototype ? universe.objectOf(prototype) : ValueFlowUniverse::unknownObject());
    };
    auto hasHeritage = [&](LayoutID layout) {
        return layout && program.at(layout).has(LayoutFlags::HasHeritage);
    };

    switch (universe.kindOf(object)) {
    case ValueFlowObjectKind::Builtin:
    case ValueFlowObjectKind::ModuleBuiltin: {
        const Builtin& entry = builtins.at(universe.builtinOf(object));
        switch (entry.prototypeLink) {
        case BuiltinPrototypeLink::Unknown:
            prototypes.append(ValueFlowUniverse::unknownObject());
            return;
        case BuiltinPrototypeLink::Null:
            return;
        case BuiltinPrototypeLink::Builtin:
            prototypes.append(universe.objectOf(entry.inheritsFrom));
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
    case ValueFlowObjectKind::Closure: {
        const Function& function = program.at(universe.closureFunction(object));
        if (!hasHeritage(function.constructedClass))
            appendPrototypeOf(classOfFunctionObject(function));
        return;
    }
    case ValueFlowObjectKind::Instance:
        prototypes.append(universe.prototypeOf(universe.layoutOf(object)));
        return;
    case ValueFlowObjectKind::CoroutineResult:
    case ValueFlowObjectKind::RestArray:
    case ValueFlowObjectKind::FunctionPrototype:
        appendPrototypeOf(universe.classOf(object));
        return;
    case ValueFlowObjectKind::ConstructedObject:
        return;
    case ValueFlowObjectKind::Prototype: {
        LayoutID layout = universe.classLayoutOf(object);
        if (hasHeritage(layout))
            return;
        FunctionID constructor = program.at(layout).constructor;
        appendPrototypeOf(constructor ? classAbovePrototypeProperty(program.at(constructor)) : BuiltinClasses::Object);
        return;
    }
    case ValueFlowObjectKind::Site: {
        const ValueFlowSite& site = *universe.siteOf(object);
        if (site.builtinClass)
            appendPrototypeOf(site.builtinClass == BuiltinClasses::Arguments ? BuiltinClasses::Object : site.builtinClass);
        if (!site.layout) {
            if (!site.builtinClass)
                prototypes.append(ValueFlowUniverse::unknownObject());
            return;
        }
        switch (program.at(site.layout).kind) {
        case LayoutKind::Record:
        case LayoutKind::Container:
        case LayoutKind::BuiltinResult:
            if (!site.namesPrototype)
                appendPrototypeOf(BuiltinClasses::Object);
            return;
        case LayoutKind::Instance:
            prototypes.append(universe.prototypeOf(site.layout));
            return;
        case LayoutKind::Environment:
        case LayoutKind::Closure:
        case LayoutKind::CoroutineFrame:
        case LayoutKind::ModuleNamespace:
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
    case ValueFlowObjectKind::Namespace:
        return;
    default:
        prototypes.append(ValueFlowUniverse::unknownObject());
        return;
    }
}

ValueSetID ValueFlowHeapView::prototypesOf(ValueSetID objects)
{
    auto found = m_prototypesOfSet.find(objects.index());
    if (found != m_prototypesOfSet.end())
        return found->value;
    ValueSetID prototypes = prototypesOf(m_maker, objects);
    m_prototypesOfSet.add(objects.index(), prototypes);
    return prototypes;
}

ValueSetID ValueFlowHeapView::prototypesOfHeirs(ValueSetMaker& maker, ValueSetID heirs) const
{
    Vector<ValueFlowObjectID, 8> atCreation;
    m_inputs.sets().forEachMember(heirs, [&](ValueFlowObjectID object) {
        appendPrototypesAtCreation(object, atCreation);
    });
    return expand(maker, maker.unite(maker.of(atCreation.span()), maker.intersect(writtenUnder(maker, heirs, prototypeLinkName), m_inputs.universe().objects())));
}

ValueSetID ValueFlowHeapView::prototypesOf(ValueSetMaker& maker, ValueSetID objects) const
{
    return m_index.prototypesOfSets().ensure(objects.index(), [&] {
        return findPrototypesOf(maker, objects);
    });
}

ValueSetID ValueFlowHeapView::findPrototypesOf(ValueSetMaker& maker, ValueSetID objects) const
{
    struct Placeholder {
        ValueFlowObjectID object;
        ValueSetID alone;
        PrototypesOfPlaceholder& prototypes;
    };

    const ValueFlowUniverse& universe = m_inputs.universe();
    ValueSetID everyObject = universe.objects();
    ValueSetID heirs = maker.intersect(expand(maker, objects), everyObject);
    if (ValueSets::isComplement(heirs))
        return everyObject;

    Placeholder placeholders[] {
        { ValueFlowUniverse::unknownObject(), universe.unknownObjectAlone(), m_prototypesOfUnknownObject },
        { ValueFlowUniverse::untrackedObject(), universe.untrackedObjectAlone(), m_prototypesOfUntrackedObject },
    };
    ValueSetID otherHeirs = heirs;
    ValueSetID prototypes = ValueSets::nothing;
    for (Placeholder& placeholder : placeholders) {
        if (!m_inputs.sets().contains(objects, placeholder.object))
            continue;
        std::call_once(placeholder.prototypes.isComputed, [&] {
            placeholder.prototypes.heirs = maker.intersect(expand(maker, placeholder.alone), everyObject);
            placeholder.prototypes.prototypes = prototypesOfHeirs(maker, placeholder.prototypes.heirs);
        });
        otherHeirs = maker.intersect(otherHeirs, ValueSets::complement(placeholder.prototypes.heirs));
        prototypes = maker.unite(prototypes, placeholder.prototypes.prototypes);
    }
    prototypes = maker.unite(prototypes, prototypesOfHeirs(maker, otherHeirs));
    if (m_inputs.build.options().verifiesAfterEveryPass)
        RELEASE_ASSERT(prototypes == prototypesOfHeirs(maker, heirs));
    return prototypes;
}

ValueSetID ValueFlowHeapView::chainOf(ValueSetID objects)
{
    auto found = m_chainOfSet.find(objects.index());
    if (found != m_chainOfSet.end())
        return found->value;
    ValueSetID chain = chainOf(m_maker, objects);
    m_chainOfSet.add(objects.index(), chain);
    return chain;
}

// A link that may hold one of several objects says of none of them that the program puts it there.
void ValueFlowHeapView::indexStoresIntoPrototypeLinks()
{
    m_hasIndexedStoresIntoPrototypeLinks = true;
    const ValueFlowUniverse& universe = m_inputs.universe();
    ValueSetID placeholders = m_maker.unite(universe.unknownObjectAlone(), universe.untrackedObjectAlone());
    ValueSetID namedObjects = m_maker.intersect(universe.objects(), ValueSets::complement(placeholders));
    forEachNodeWrittenUnder(prototypeLinkName, [&](ValueSetID writtenThrough, ValueSetID value) {
        if (!m_inputs.sets().onlyMember(m_maker.intersect(value, namedObjects)))
            return;
        if (ValueSets::isComplement(writtenThrough)) {
            m_prototypeLinksStoredThroughComplements.append({ writtenThrough, value });
            return;
        }
        m_inputs.sets().forEachMember(writtenThrough, [&](ValueFlowObjectID object) {
            ValueSetID& stored = m_storedIntoPrototypeLinkOf.add(object.index(), ValueSets::nothing).iterator->value;
            stored = m_maker.unite(stored, value);
        });
    });
}

ValueSetID ValueFlowHeapView::chainBuiltByProgram(ValueSetID objects)
{
    auto found = m_chainBuiltByProgramOfSet.find(objects.index());
    if (found != m_chainBuiltByProgramOfSet.end())
        return found->value;
    if (!m_hasIndexedStoresIntoPrototypeLinks)
        indexStoresIntoPrototypeLinks();

    const ValueFlowUniverse& universe = m_inputs.universe();
    ValueSetID placeholders = m_maker.unite(universe.unknownObjectAlone(), universe.untrackedObjectAlone());
    ValueSetID namedObjects = m_maker.intersect(universe.objects(), ValueSets::complement(placeholders));
    ValueSetID chain = m_maker.intersect(objects, namedObjects);
    ValueSetID frontier = chain;
    while (frontier != ValueSets::nothing) {
        Vector<ValueFlowObjectID, 8> atCreation;
        ValueSetID stored = ValueSets::nothing;
        m_inputs.sets().forEachMember(frontier, [&](ValueFlowObjectID object) {
            appendPrototypesAtCreation(object, atCreation);
            if (auto entry = m_storedIntoPrototypeLinkOf.find(object.index()); entry != m_storedIntoPrototypeLinkOf.end())
                stored = m_maker.unite(stored, entry->value);
        });
        for (const WrittenNode& node : m_prototypeLinksStoredThroughComplements) {
            if (!m_inputs.sets().isEmpty(m_maker.intersect(node.objects, frontier)))
                stored = m_maker.unite(stored, node.value);
        }
        ValueSetID above = m_maker.intersect(m_maker.unite(m_maker.of(atCreation.span()), stored), namedObjects);
        frontier = m_maker.intersect(above, ValueSets::complement(chain));
        chain = m_maker.unite(chain, above);
    }
    m_chainBuiltByProgramOfSet.add(objects.index(), chain);
    return chain;
}

ValueSetID ValueFlowHeapView::chainWalkedInFull(ValueSetMaker& maker, ValueSetID objects) const
{
    ValueSetID chain = maker.intersect(expand(maker, objects), m_inputs.universe().objects());
    ValueSetID frontier = chain;
    while (frontier != ValueSets::nothing) {
        ValueSetID above = prototypesOf(maker, frontier);
        frontier = maker.intersect(above, ValueSets::complement(chain));
        chain = maker.unite(chain, above);
    }
    return chain;
}

ValueSetID ValueFlowHeapView::chainOf(ValueSetMaker& maker, ValueSetID objects) const
{
    return m_index.chainsOfSets().ensure(objects.index(), [&] {
        return walkChainOf(maker, objects);
    });
}

ValueSetID ValueFlowHeapView::walkChainOf(ValueSetMaker& maker, ValueSetID objects) const
{
    struct Placeholder {
        ValueFlowObjectID object;
        ValueSetID alone;
        ChainOfPlaceholder& chain;
        bool isTakenIn { false };
    };

    const ValueSets& sets = m_inputs.sets();
    const ValueFlowUniverse& universe = m_inputs.universe();
    Placeholder placeholders[] {
        { ValueFlowUniverse::unknownObject(), universe.unknownObjectAlone(), m_chainOfUnknownObject },
        { ValueFlowUniverse::untrackedObject(), universe.untrackedObjectAlone(), m_chainOfUntrackedObject },
    };

    ValueSetID chain = maker.intersect(expand(maker, objects), universe.objects());
    if (ValueSets::isComplement(chain))
        return chainWalkedInFull(maker, objects);
    ValueSetID walked = ValueSets::nothing;
    auto takeInChainsOfPlaceholdersIn = [&](ValueSetID set) {
        for (Placeholder& placeholder : placeholders) {
            if (placeholder.isTakenIn || !sets.contains(set, placeholder.object))
                continue;
            placeholder.isTakenIn = true;
            std::call_once(placeholder.chain.isComputed, [&] {
                placeholder.chain.chain = chainWalkedInFull(maker, placeholder.alone);
            });
            chain = maker.unite(chain, placeholder.chain.chain);
            walked = maker.unite(walked, maker.unite(placeholder.chain.chain, placeholder.alone));
        }
    };

    takeInChainsOfPlaceholdersIn(objects);
    while (true) {
        takeInChainsOfPlaceholdersIn(chain);
        if (ValueSets::isComplement(chain))
            return chainWalkedInFull(maker, objects);
        ValueSetID frontier = maker.intersect(chain, ValueSets::complement(walked));
        if (frontier == ValueSets::nothing)
            break;
        walked = maker.unite(walked, chain);
        chain = maker.unite(chain, prototypesOf(maker, frontier));
    }
    if (m_inputs.build.options().verifiesAfterEveryPass)
        RELEASE_ASSERT(chain == chainWalkedInFull(maker, objects));
    return chain;
}

ValueSetID ValueFlowBuiltinApplication::valueAt(BuiltinOperand position, ValueSetMaker& maker) const
{
    switch (position.kind()) {
    case BuiltinOperand::Kind::Receiver:
        return thisValue;
    case BuiltinOperand::Kind::Argument:
        return position.argumentIndex() < arguments.size() ? maker.unite(arguments[position.argumentIndex()], otherArguments) : otherArguments;
    case BuiltinOperand::Kind::ArgumentsFrom: {
        ValueSetID value = otherArguments;
        for (size_t index = position.argumentIndex(); index < arguments.size(); ++index)
            value = maker.unite(value, arguments[index]);
        return value;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

BuiltinCallMode ValueFlowBuiltinApplication::mode() const
{
    switch (kind) {
    case ValueFlowCallKind::Call:
    case ValueFlowCallKind::CallByNativeCode:
    case ValueFlowCallKind::CallByHost:
        return BuiltinCallMode::Call;
    case ValueFlowCallKind::Construct:
    case ValueFlowCallKind::ConstructByNativeCode:
        return BuiltinCallMode::Construct;
    case ValueFlowCallKind::Get:
        return BuiltinCallMode::Get;
    case ValueFlowCallKind::Set:
        return BuiltinCallMode::Set;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The first operand of each is the callee, the second is this or new.target, then come the arguments or the list of them.
static bool callsValue(const ValueFlowStatement& statement)
{
    if (statement.kind != ValueFlowStatementKind::Operation)
        return false;
    switch (statement.opcode) {
    case Opcode::Call:
    case Opcode::CallList:
    case Opcode::Construct:
    case Opcode::ConstructList:
    case Opcode::SuperConstruct:
    case Opcode::SuperConstructList:
        return true;
    default:
        return false;
    }
}

static bool passesList(Opcode opcode)
{
    return opcode == Opcode::CallList || opcode == Opcode::ConstructList || opcode == Opcode::SuperConstructList;
}

static bool passesAnyOf(const ValueFlowBuiltinApplication& application, SetsMeetingOneSet* wanted)
{
    if (!wanted)
        return true;
    return wanted->contains(application.thisValue) || wanted->contains(application.otherArguments) || std::ranges::any_of(application.arguments, [&](ValueSetID argument) {
        return wanted->contains(argument);
    });
}

static void appendBoundFunctionsBindingNoArgument(const ValueFlowResultInputs& inputs, ValueSetMaker& maker, const ValueFlowStatement& call, ValueSetID callees, Vector<ValueFlowObjectID, 4>& boundFunctions)
{
    const ValueFlowUniverse& universe = inputs.universe();
    if (call.opcode != Opcode::Call && call.opcode != Opcode::CallList)
        return;
    inputs.sets().forEachMember(maker.intersect(callees, maker.intersect(universe.otherCallables(), universe.knownObjects())), [&](ValueFlowObjectID callee) {
        if (bindsNoArgument(inputs.program, universe, callee))
            boundFunctions.append(callee);
    });
}

static void findOperandsOf(const ValueFlowResultInputs& inputs, const ValueFlowHeapView& heap, ValueSetMaker& maker, const ValueFlowStatement& call, ValueFlowBuiltinApplication& application)
{
    const ValueFlowUniverse& universe = inputs.universe();
    std::span<const ValueFlowOperand> operands = inputs.bodies().operandsOf(call);
    // Without the operand the callee is new.target.
    bool hasSecondOperand = !isConstruction(application.kind) || !!inputs.program.at(call.function).at(call.site).operand(1);
    application.thisValue = heap.valueOf(maker, operands[hasSecondOperand ? 1 : 0]);
    if (passesList(call.opcode)) {
        // A list that unseen code has made, or has got hold of, holds what unseen code has got hold of: the unknown object stands for that, and no object that has stayed with followed code is in it.
        ValueSetID lists = heap.valueOf(maker, operands[2]);
        bool mayBeListOfUnseenCode = inputs.sets().contains(lists, ValueFlowUniverse::unknownObject());
        if (mayBeListOfUnseenCode)
            lists = maker.intersect(lists, ValueSets::complement(universe.unknownObjectAlone()));
        application.otherArguments = heap.writtenUnder(maker, heap.expand(maker, lists), elementsName);
        if (mayBeListOfUnseenCode)
            application.otherArguments = maker.unite(application.otherArguments, maker.unite(universe.anyPrimitive(), universe.unknownObjectAlone()));
    } else {
        for (const ValueFlowOperand& argument : operands.subspan(2))
            application.arguments.append(heap.valueOf(maker, argument));
    }
}

static ValueFlowCallKind kindOfCallBy(const ValueFlowStatement& call)
{
    return call.opcode == Opcode::Call || call.opcode == Opcode::CallList ? ValueFlowCallKind::Call : ValueFlowCallKind::Construct;
}

void ValueFlowHeapIndex::findOperandsOfCalls(const ValueFlowResultInputs& inputs, const ValueFlowHeapView& heap)
{
    struct FoundInTask {
        Vector<ValueSetID> arguments;
        Vector<ValueFlowObjectID> boundFunctions;
    };

    constexpr size_t callsPerTask = 256;
    std::span<const ValueFlowStatement> statements = inputs.bodies().statements();
    size_t numberOfCalls = m_callsThatMayApplyBuiltins.size();
    Vector<OperandsOfCall> operandsOfCalls(numberOfCalls);
    Vector<FoundInTask> foundInTasks((numberOfCalls + callsPerTask - 1) / callsPerTask);
    ValueSetMakers makers(inputs);
    forEachIndexInParallel(inputs.loops(), foundInTasks.size(), [&](unsigned task, unsigned worker) {
        ValueSetMaker& maker = makers.of(worker);
        FoundInTask& found = foundInTasks[task];
        for (size_t position = task * callsPerTask; position < std::min(numberOfCalls, (task + 1) * callsPerTask); ++position) {
            const ValueFlowStatement& call = statements[m_callsThatMayApplyBuiltins[position].statement];
            ValueFlowBuiltinApplication application;
            application.kind = kindOfCallBy(call);
            findOperandsOf(inputs, heap, maker, call, application);
            Vector<ValueFlowObjectID, 4> boundFunctions;
            appendBoundFunctionsBindingNoArgument(inputs, maker, call, m_callsThatMayApplyBuiltins[position].callees, boundFunctions);
            operandsOfCalls[position] = { application.thisValue, application.otherArguments, static_cast<uint32_t>(found.arguments.size()), static_cast<uint32_t>(application.arguments.size()), static_cast<uint32_t>(found.boundFunctions.size()), static_cast<uint32_t>(boundFunctions.size()) };
            found.arguments.append(application.arguments.span());
            found.boundFunctions.append(boundFunctions.span());
        }
    });
    for (size_t task = 0; task < foundInTasks.size(); ++task) {
        for (size_t position = task * callsPerTask; position < std::min(numberOfCalls, (task + 1) * callsPerTask); ++position) {
            operandsOfCalls[position].firstArgument += m_argumentsOfCalls.size();
            operandsOfCalls[position].firstBoundFunction += m_boundFunctionsOfCalls.size();
        }
        m_argumentsOfCalls.appendVector(foundInTasks[task].arguments);
        m_boundFunctionsOfCalls.appendVector(foundInTasks[task].boundFunctions);
    }
    m_operandsOfCalls = WTF::move(operandsOfCalls);
}

using ValueFlowBuiltinApplicationsFunctor = ScopedLambda<void(const ValueFlowBuiltinApplication&, ValueSetID builtinCallees)>;

static void forEachBuiltinAppliedByOperation(const ValueFlowResultInputs& inputs, const ValueFlowHeapView& heap, ValueSetMaker& maker, const ValueFlowStatement& call, ValueSetID callees, const ValueFlowHeapIndex::OperandsOfCall* knownOperands, ValueSetID amongFunctions, SetsMeetingOneSet* wantedOperands, const ValueFlowBuiltinApplicationFunctor& functor, const ValueFlowBuiltinApplicationsFunctor* functorForAllCallees)
{
    const ValueFlowUniverse& universe = inputs.universe();
    ValueSetID builtinCallees = maker.intersect(callees, amongFunctions);
    Vector<ValueFlowObjectID, 4> boundFunctions;
    if (knownOperands)
        boundFunctions.append(heap.index().boundFunctionsOf(*knownOperands));
    else
        appendBoundFunctionsBindingNoArgument(inputs, maker, call, callees, boundFunctions);
    if (builtinCallees == ValueSets::nothing && boundFunctions.isEmpty())
        return;

    ValueFlowBuiltinApplication application;
    application.kind = kindOfCallBy(call);
    application.function = call.function;
    application.site = call.site;
    if (knownOperands) {
        application.thisValue = knownOperands->thisValue;
        application.otherArguments = knownOperands->otherArguments;
        application.arguments.append(heap.index().argumentsOf(*knownOperands));
    } else
        findOperandsOf(inputs, heap, maker, call, application);
    if (builtinCallees != ValueSets::nothing && passesAnyOf(application, wantedOperands)) {
        if (functorForAllCallees)
            (*functorForAllCallees)(application, builtinCallees);
        else {
            inputs.sets().forEachMember(builtinCallees, [&](ValueFlowObjectID callee) {
                application.builtin = universe.builtinOf(callee);
                functor(application);
            });
        }
    }
    for (ValueFlowObjectID boundFunction : boundFunctions) {
        ValueSetID alone = maker.of(boundFunction);
        ValueSetID targets = maker.intersect(heap.writtenUnder(maker, alone, { ValueFlowNameKind::HiddenSlot, BuiltinSlots::BoundFunctionTarget.index() }), amongFunctions);
        if (targets == ValueSets::nothing)
            continue;
        application.kind = ValueFlowCallKind::CallByNativeCode;
        application.thisValue = heap.writtenUnder(maker, alone, { ValueFlowNameKind::HiddenSlot, BuiltinSlots::BoundFunctionBoundThis.index() });
        if (!passesAnyOf(application, wantedOperands))
            continue;
        inputs.sets().forEachMember(targets, [&](ValueFlowObjectID target) {
            application.builtin = universe.builtinOf(target);
            if (!raisesEventWhenCalled(inputs.program.builtins(), application.builtin))
                functor(application);
        });
    }
}

static void forEachBuiltinAppliedByGroup(const ValueFlowResultInputs& inputs, const ValueFlowHeapView& heap, ValueSetMaker& maker, const ValueFlowNodeKey& key, ValueFlowNodeID first, ValueSetID amongObjects, SetsMeetingOneSet* wantedOperands, const ValueFlowBuiltinApplicationFunctor& functor)
{
    const ValueFlowUniverse& universe = inputs.universe();
    const ValueFlowSolver& solver = inputs.solver();
    ValueSetID builtinCallees = heap.expandAmong(maker, key.set, amongObjects);
    if (builtinCallees == ValueSets::nothing)
        return;

    ValueFlowCallGroup group = callGroupAt(first);
    ValueFlowCallKind kind = key.callKind();
    bool isAccessorCall = kind == ValueFlowCallKind::Get || kind == ValueFlowCallKind::Set;
    ValueFlowBuiltinApplication application;
    application.thisValue = solver.valueOf(isConstruction(kind) ? group.newTarget : group.thisValue);
    for (unsigned index = 0; index < numberOfGroupedArguments; ++index)
        application.arguments.append(solver.valueOf(group.argument(index)));
    application.otherArguments = maker.unite(solver.valueOf(group.otherArguments), solver.valueOf(group.laterArguments));
    if (!passesAnyOf(application, wantedOperands))
        return;
    inputs.sets().forEachMember(builtinCallees, [&](ValueFlowObjectID callee) {
        // A lookup finds accessors and data properties of the table, which are applied in the mode of the group. A function of the table that the program has made an accessor is called like any function.
        bool isFunction = inputs.sets().contains(universe.builtinFunctions(), callee);
        if (!isFunction && !isAccessorCall)
            return;
        application.builtin = universe.builtinOf(callee);
        application.kind = isFunction && isAccessorCall ? ValueFlowCallKind::Call : kind;
        functor(application);
    });
}

// A protocol of the language applies what it finds under one of these names.
static ValueSetID builtinFunctionsOfProtocolsThatProgramHasStored(const ValueFlowResultInputs& inputs, const ValueFlowHeapView& heap, ValueSetMaker& maker)
{
    const Program& program = inputs.program;
    ValueSetID everyObject = inputs.universe().objects();
    ValueSetID found = heap.writtenUnder(maker, everyObject, ValueFlowName::of(ValueFlowNameKind::UnknownKey));
    for (BuiltinName name : { BuiltinNames::iteratorSymbol, BuiltinNames::asyncIteratorSymbol, BuiltinNames::next, BuiltinNames::returnKeyword, BuiltinNames::throwKeyword, BuiltinNames::then })
        found = maker.unite(found, heap.writtenUnder(maker, everyObject, ValueFlowName::property(program.keyOf(name))));
    return maker.intersect(found, inputs.universe().builtinFunctionsOfProtocols());
}

static void forEachBuiltinAppliedByProtocol(const ValueFlowResultInputs& inputs, const ValueFlowHeapView& heap, ValueSetMaker& maker, ValueSetID amongFunctions, const ValueFlowBuiltinApplicationFunctor& functor)
{
    ValueFlowBuiltinApplication byProtocol;
    byProtocol.kind = ValueFlowCallKind::CallByNativeCode;
    byProtocol.thisValue = ValueSets::anything;
    byProtocol.otherArguments = ValueSets::anything;
    inputs.sets().forEachMember(maker.intersect(builtinFunctionsOfProtocolsThatProgramHasStored(inputs, heap, maker), amongFunctions), [&](ValueFlowObjectID callee) {
        byProtocol.builtin = inputs.universe().builtinOf(callee);
        functor(byProtocol);
    });
}

// A call that is left out applies no built-in, whichever built-ins somebody asks about: they are built-in functions, and a bound function is one of the other callables.
void ValueFlowHeapIndex::collectCalls(const ValueFlowResultInputs& inputs)
{
    constexpr uint32_t functionsPerTask = 64;
    const ValueFlowUniverse& universe = inputs.universe();
    const ParallelLoops& loops = inputs.loops();
    uint32_t numberOfFunctions = inputs.program.functions.size();
    std::span<const ValueFlowBodyRange> ranges = inputs.bodies().ranges();
    std::span<const ValueFlowStatement> statements = inputs.bodies().statements();
    ValueSetMakers makers(inputs);
    ValueSetID wantedCallees = makers.of(0).unite(universe.builtinFunctions(), makers.of(0).intersect(universe.otherCallables(), universe.knownObjects()));
    Vector<Vector<CallOfValue>> callsOfTask((numberOfFunctions + functionsPerTask - 1) / functionsPerTask);
    forEachIndexInParallel(loops, callsOfTask.size(), [&](unsigned task, unsigned worker) {
        uint32_t first = task * functionsPerTask;
        for (uint32_t function = first; function < std::min(first + functionsPerTask, numberOfFunctions); ++function) {
            if (!inputs.isReached(FunctionID(function)))
                continue;
            for (uint32_t index = ranges[function].statement; index < ranges[function + 1].statement; ++index) {
                if (!callsValue(statements[index]))
                    continue;
                ValueSetID callees = valueSeenByRulesAt(inputs, makers.of(worker), inputs.bodies().operandsOf(statements[index])[0]);
                if (inputs.sets().meet(callees, wantedCallees))
                    callsOfTask[task].append({ index, callees });
            }
        }
    });
    size_t numberOfCalls = 0;
    for (const Vector<CallOfValue>& calls : callsOfTask)
        numberOfCalls += calls.size();
    m_callsThatMayApplyBuiltins.reserveInitialCapacity(numberOfCalls);
    for (Vector<CallOfValue>& calls : callsOfTask)
        m_callsThatMayApplyBuiltins.appendVector(std::exchange(calls, { }));
}

// What is collected comes in the order of the groups.
template<typename T>
static Vector<T> collectFromCallGroups(const ValueFlowResultInputs& inputs, const ValueFlowHeapView& heap, const Invocable<void(const ValueFlowNodeKey&, ValueFlowNodeID first, Vector<T>&)> auto& collect)
{
    std::span<const ValueFlowNodeID> firstNodes = heap.index().firstNodesOfCallGroups();
    return collectInParallel<T>(inputs.loops(), firstNodes.size(), [&](unsigned position, Vector<T>& collected) {
        collect(*inputs.solver().keyOfMadeNode(firstNodes[position]), firstNodes[position], collected);
    });
}

namespace {

struct BuiltinsToApply {
    ValueSetID functions;
    ValueSetID objects;
};

// The calls of built-ins that operations spell out, function by function, then those that the groups of callees share.
class BuiltinApplicationTasks {
public:
    BuiltinApplicationTasks(const ValueFlowResultInputs& inputs, const ValueFlowHeapView& heap, BuiltinsToApply among)
        : m_inputs(inputs)
        , m_heap(heap)
        , m_among(among)
        , m_calls(heap.index().callsThatMayApplyBuiltins())
        , m_groups(heap.index().firstNodesOfCallGroups())
    {
    }

    unsigned size() const { return numberOfTasksOfCalls() + static_cast<unsigned>((m_groups.size() + groupsPerTask - 1) / groupsPerTask); }
    void run(unsigned task, ValueSetMaker&, SetsMeetingOneSet* wantedOperands, const ValueFlowBuiltinApplicationFunctor&, const ValueFlowBuiltinApplicationsFunctor* functorForAllCallees = nullptr) const;

private:
    static constexpr size_t callsPerTask = 256;
    static constexpr size_t groupsPerTask = 256;

    unsigned numberOfTasksOfCalls() const { return static_cast<unsigned>((m_calls.size() + callsPerTask - 1) / callsPerTask); }

    const ValueFlowResultInputs& m_inputs;
    const ValueFlowHeapView& m_heap;
    BuiltinsToApply m_among;
    std::span<const ValueFlowHeapIndex::CallOfValue> m_calls;
    std::span<const ValueFlowNodeID> m_groups;
};

} // anonymous namespace

void BuiltinApplicationTasks::run(unsigned task, ValueSetMaker& maker, SetsMeetingOneSet* wantedOperands, const ValueFlowBuiltinApplicationFunctor& functor, const ValueFlowBuiltinApplicationsFunctor* functorForAllCallees) const
{
    if (task < numberOfTasksOfCalls()) {
        size_t first = task * callsPerTask;
        std::span<const ValueFlowStatement> statements = m_inputs.bodies().statements();
        for (size_t position = first; position < std::min(first + callsPerTask, m_calls.size()); ++position)
            forEachBuiltinAppliedByOperation(m_inputs, m_heap, maker, statements[m_calls[position].statement], m_calls[position].callees, m_heap.index().operandsOfCallAt(position), m_among.functions, wantedOperands, functor, functorForAllCallees);
        return;
    }
    size_t first = (task - numberOfTasksOfCalls()) * groupsPerTask;
    for (ValueFlowNodeID node : m_groups.subspan(first, std::min(groupsPerTask, m_groups.size() - first)))
        forEachBuiltinAppliedByGroup(m_inputs, m_heap, maker, *m_inputs.solver().keyOfMadeNode(node), node, m_among.objects, wantedOperands, functor);
}

// All threads put the applications together. The functor gets them on this thread, in the order of the tasks.
static void forEachBuiltinApplicationAmong(const ValueFlowResultInputs& inputs, ValueFlowHeapView& heap, BuiltinsToApply among, ValueSetID amongOperands, const ValueFlowBuiltinApplicationFunctor& functor)
{
    const ParallelLoops& loops = inputs.loops();
    BuiltinApplicationTasks tasks(inputs, heap, among);
    Vector<Vector<ValueFlowBuiltinApplication>> applicationsOfTask(tasks.size());
    {
        ValueSetMakers makers(inputs);
        Vector<SetsMeetingOneSet> setsMeetingOperands(loops.width(), [&](size_t) {
            return SetsMeetingOneSet(inputs.sets(), amongOperands);
        });
        forEachIndexInParallel(loops, tasks.size(), [&](unsigned task, unsigned worker) {
            tasks.run(task, makers.of(worker), amongOperands == ValueSets::anything ? nullptr : &setsMeetingOperands[worker], [&](const ValueFlowBuiltinApplication& application) {
                applicationsOfTask[task].append(application);
            });
        });
    }
    for (Vector<ValueFlowBuiltinApplication>& applications : applicationsOfTask) {
        for (const ValueFlowBuiltinApplication& application : applications)
            functor(application);
        applications.clear();
    }
    forEachBuiltinAppliedByProtocol(inputs, heap, heap.maker(), among.functions, functor);
}

static BuiltinsToApply builtinsToApplyAmong(const ValueFlowResultInputs& inputs, ValueSetMaker& maker, const BitVector& onlyOfBuiltins)
{
    const ValueFlowUniverse& universe = inputs.universe();
    Vector<ValueFlowObjectID> objects;
    inputs.sets().forEachMember(maker.unite(universe.builtinFunctions(), universe.builtinObjects()), [&](ValueFlowObjectID object) {
        if (onlyOfBuiltins.quickGet(universe.builtinOf(object).index()))
            objects.append(object);
    });
    ValueSetID among = maker.of(objects.span());
    return { maker.intersect(universe.builtinFunctions(), among), maker.intersect(universe.builtinObjects(), among) };
}

void forEachBuiltinApplication(const ValueFlowResultInputs& inputs, ValueFlowHeapView& heap, NOESCAPE const ValueFlowBuiltinApplicationFunctor& functor)
{
    forEachBuiltinApplicationAmong(inputs, heap, { inputs.universe().builtinFunctions(), inputs.universe().builtinObjects() }, ValueSets::anything, functor);
}

void forEachBuiltinApplication(const ValueFlowResultInputs& inputs, ValueFlowHeapView& heap, const BitVector& onlyOfBuiltins, NOESCAPE const ValueFlowBuiltinApplicationFunctor& functor)
{
    forEachBuiltinApplication(inputs, heap, onlyOfBuiltins, ValueSets::anything, functor);
}

void forEachBuiltinApplication(const ValueFlowResultInputs& inputs, ValueFlowHeapView& heap, const BitVector& onlyOfBuiltins, ValueSetID onlyPassingAnyOf, NOESCAPE const ValueFlowBuiltinApplicationFunctor& functor)
{
    forEachBuiltinApplicationAmong(inputs, heap, builtinsToApplyAmong(inputs, heap.maker(), onlyOfBuiltins), onlyPassingAnyOf, functor);
}

// The same applications, handed out on all threads and in no order. `worker` is what forEachIndexInParallel() hands to the body of a loop.
static void forEachBuiltinApplicationInParallel(const ValueFlowResultInputs& inputs, const ValueFlowHeapView& heap, ValueSetMakers& makers, const BitVector& onlyOfBuiltins, NOESCAPE const Invocable<void(unsigned worker, const ValueFlowBuiltinApplication&)> auto& functor, NOESCAPE const Invocable<void(unsigned worker, const ValueFlowBuiltinApplication&, ValueSetID builtinCallees)> auto& functorForAllCallees)
{
    BuiltinsToApply among = builtinsToApplyAmong(inputs, makers.of(0), onlyOfBuiltins);
    BuiltinApplicationTasks tasks(inputs, heap, among);
    forEachIndexInParallel(inputs.loops(), tasks.size(), [&](unsigned task, unsigned worker) {
        auto handOnAllCallees = [&](const ValueFlowBuiltinApplication& application, ValueSetID builtinCallees) {
            functorForAllCallees(worker, application, builtinCallees);
        };
        ValueFlowBuiltinApplicationsFunctor forAllCallees(handOnAllCallees);
        tasks.run(task, makers.of(worker), nullptr, [&](const ValueFlowBuiltinApplication& application) {
            functor(worker, application);
        }, &forAllCallees);
    });
    forEachBuiltinAppliedByProtocol(inputs, heap, makers.of(0), among.functions, [&](const ValueFlowBuiltinApplication& application) {
        functor(0, application);
    });
}

void forEachBuiltinApplicationOnAllThreads(const ValueFlowResultInputs& inputs, const ValueFlowHeapView& heap, ValueSetMakers& makers, const BitVector& onlyOfBuiltins, ValueSetID onlyPassingAnyOf, NOESCAPE const ScopedLambda<void(unsigned worker, const ValueFlowBuiltinApplication&)>& functor)
{
    const ParallelLoops& loops = inputs.loops();
    BuiltinsToApply among = builtinsToApplyAmong(inputs, makers.of(0), onlyOfBuiltins);
    BuiltinApplicationTasks tasks(inputs, heap, among);
    Vector<SetsMeetingOneSet> setsMeetingOperands(loops.width(), [&](size_t) {
        return SetsMeetingOneSet(inputs.sets(), onlyPassingAnyOf);
    });
    forEachIndexInParallel(loops, tasks.size(), [&](unsigned task, unsigned worker) {
        tasks.run(task, makers.of(worker), onlyPassingAnyOf == ValueSets::anything ? nullptr : &setsMeetingOperands[worker], [&](const ValueFlowBuiltinApplication& application) {
            functor(worker, application);
        });
    });
    forEachBuiltinAppliedByProtocol(inputs, heap, makers.of(0), among.functions, [&](const ValueFlowBuiltinApplication& application) {
        functor(0, application);
    });
}

namespace {

using SetsByNumber = HashMap<uint32_t, ValueSetID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;

// Each of the sets comes once.
ValueSetID unionOfDistinctSets(ValueSetMaker& maker, Vector<ValueSetID, 4>&& parts)
{
    if (parts.contains(ValueSets::anything))
        return ValueSets::anything;
    Vector<ValueSetID, 4> complements;
    parts.removeAllMatching([&](ValueSetID part) {
        if (!ValueSets::isComplement(part))
            return false;
        complements.append(part);
        return true;
    });
    ValueSetID united = maker.uniteAll(parts.span());
    for (ValueSetID complement : complements)
        united = maker.unite(united, complement);
    return united;
}

class UnionOfSets {
public:
    void add(ValueSetID set)
    {
        if (set == ValueSets::nothing || (!m_parts.isEmpty() && m_parts.last() == set))
            return;
        m_parts.append(set);
        if (m_parts.size() < m_sizeToRemoveRepeatsAt)
            return;
        removeRepeats();
        m_sizeToRemoveRepeatsAt = std::max<size_t>(m_sizeToRemoveRepeatsAt, 2 * m_parts.size());
    }

    void takeFrom(UnionOfSets& other)
    {
        for (ValueSetID part : std::exchange(other.m_parts, { }))
            add(part);
    }

    ValueSetID take(ValueSetMaker& maker)
    {
        removeRepeats();
        return unionOfDistinctSets(maker, std::exchange(m_parts, { }));
    }

    // With all that the placeholders in the union cover.
    ValueSetID takeExpanded(const ValueFlowHeapView& heap, ValueSetMaker& maker)
    {
        return heap.expand(maker, take(maker));
    }

    // The objects that the parts list themselves: nothing that a placeholder among them stands for, and nothing of a part that says what it leaves out.
    ValueSetID takeAsListed(ValueSetMaker& maker)
    {
        removeRepeats();
        Vector<ValueSetID, 4> parts = std::exchange(m_parts, { });
        parts.removeAllMatching([](ValueSetID part) {
            return ValueSets::isComplement(part);
        });
        return maker.uniteAll(parts.span());
    }

private:
    void removeRepeats()
    {
        AOT::sort(m_parts);
        removeRepeatedElements(m_parts);
    }

    Vector<ValueSetID, 4> m_parts;
    size_t m_sizeToRemoveRepeatsAt { 64 };
};

struct PartOfNumber {
    uint32_t number;
    ValueSetID set;

    friend auto operator<=>(const PartOfNumber&, const PartOfNumber&) = default;
};

// Sets that are to be united number by number, each listed once. Each thread of a loop fills one of its own.
class PartsByNumber {
public:
    void add(uint32_t number, ValueSetID set)
    {
        if (set == ValueSets::nothing || (!m_parts.isEmpty() && m_parts.last() == PartOfNumber { number, set }))
            return;
        if (m_isListed.add(static_cast<uint64_t>(number) << 32 | set.index()).isNewEntry)
            m_parts.append({ number, set });
    }

    Vector<PartOfNumber> take()
    {
        m_isListed.clear();
        return std::exchange(m_parts, { });
    }

private:
    Vector<PartOfNumber> m_parts;
    HashSet<uint64_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_isListed;
};

// The parts of every thread: ascending, each once, the parts of a number side by side.
class PartsOfNumbers {
public:
    PartsOfNumbers(const ParallelLoops& loops, Vector<PartOfNumber>&& parts)
        : m_parts(WTF::move(parts))
    {
        sortAndRemoveDuplicates(loops, m_parts);
        for (uint32_t index = 0; index < m_parts.size(); ++index) {
            if (!index || m_parts[index - 1].number != m_parts[index].number)
                m_firstPartOfNumber.append(index);
        }
        m_firstPartOfNumber.append(m_parts.size());
    }

    // The numbers stand at the positions from zero up, ascending.
    unsigned size() const { return m_firstPartOfNumber.size() - 1; }
    uint32_t numberAt(unsigned position) const { return m_parts[m_firstPartOfNumber[position]].number; }
    std::span<const PartOfNumber> partsAt(unsigned position) const LIFETIME_BOUND { return m_parts.subspan(m_firstPartOfNumber[position], m_firstPartOfNumber[position + 1] - m_firstPartOfNumber[position]); }

    ValueSetID unionAt(unsigned position, ValueSetMaker& maker) const
    {
        std::span<const PartOfNumber> parts = partsAt(position);
        return unionOfDistinctSets(maker, Vector<ValueSetID, 4>(parts.size(), [&](size_t index) {
            return parts[index].set;
        }));
    }

private:
    Vector<PartOfNumber> m_parts;
    Vector<uint32_t> m_firstPartOfNumber;
};

using ListsOfSetsByNumber = HashMap<uint32_t, Vector<ValueSetID>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;

ValueSetID setAt(const SetsByNumber& sets, uint32_t number)
{
    auto found = sets.find(number);
    return found == sets.end() ? ValueSets::nothing : found->value;
}

// An allocation in the constructor of a class answers with an instance of the class whose prototype object it is given, so one object of the universe stands for the instances of a class and of the classes below it.
class LayoutsOfObjects {
public:
    LayoutsOfObjects(const ValueFlowResultInputs&, ValueFlowHeapView&);

    // Both may name a layout more than once.
    void forEachLayoutOf(ValueFlowObjectID object, NOESCAPE const Invocable<void(LayoutID)> auto& functor) const
    {
        forEachLayoutOfEach([&](const auto& visit) {
            visit(object);
        }, functor);
    }

    void forEachLayoutOfMembers(ValueSetID objects, NOESCAPE const Invocable<void(LayoutID)> auto& functor) const
    {
        forEachLayoutOfEach([&](const auto& visit) {
            m_inputs.sets().forEachMember(objects, visit);
        }, functor);
    }

    struct SharedLists {
        bool everyEscapedClass { false };
        bool everyLostClass { false };
    };

    // Without the classes of the two shared lists: the answer says which of the lists count as well.
    SharedLists forEachLayoutListedForMembers(ValueSetID objects, NOESCAPE const Invocable<void(LayoutID)> auto& functor) const
    {
        return forEachListedLayoutOfEach([&](const auto& visit) {
            m_inputs.sets().forEachMember(objects, visit);
        }, functor);
    }

    // Ascending.
    std::span<const LayoutID> escapedClasses() const LIFETIME_BOUND { return m_escapedClasses.span(); }
    std::span<const LayoutID> lostClasses() const LIFETIME_BOUND { return m_lostClasses.span(); }

    // Every object that may have the layout.
    ValueSetID objectsWith(LayoutID layout) const { return m_objectsOfLayout[layout.index()]; }

private:
    // Thousands of allocations may be given the prototype object of any class that has escaped, so those classes are listed once.
    struct SelectedLayouts {
        Vector<LayoutID, 2> listed;
        bool everyEscapedClass { false };
        bool everyLostClass { false };
    };

    SharedLists forEachListedLayoutOfEach(NOESCAPE const auto& forEachObject, NOESCAPE const Invocable<void(LayoutID)> auto& functor) const
    {
        SharedLists lists;
        forEachObject([&](ValueFlowObjectID object) {
            LayoutID layout = namedLayoutOf(object);
            if (!layout)
                return;
            functor(layout);
            auto selected = m_selectedLayouts.find(object.index());
            if (selected == m_selectedLayouts.end())
                return;
            for (LayoutID other : selected->value.listed)
                functor(other);
            lists.everyEscapedClass |= selected->value.everyEscapedClass;
            lists.everyLostClass |= selected->value.everyLostClass;
        });
        return lists;
    }

    void forEachLayoutOfEach(NOESCAPE const auto& forEachObject, NOESCAPE const Invocable<void(LayoutID)> auto& functor) const
    {
        SharedLists lists = forEachListedLayoutOfEach(forEachObject, functor);
        if (lists.everyEscapedClass) {
            for (LayoutID layout : m_escapedClasses)
                functor(layout);
        }
        if (lists.everyLostClass) {
            for (LayoutID layout : m_lostClasses)
                functor(layout);
        }
    }

    LayoutID namedLayoutOf(ValueFlowObjectID) const;
    bool selectsLayoutByPrototype(ValueFlowObjectID) const;
    ValueSetID collectClassLayouts(ValueFlowHeapView&);
    SetsByNumber collectWrittenPrototypeLinks(std::span<const ValueFlowObjectID> objects, ValueFlowHeapView&) const;
    SelectedLayouts layoutsSelectedBy(ValueSetID prototypes, LayoutID named, ValueSetID prototypeObjectsOfClasses, ValueFlowHeapView&) const;

    const ValueFlowResultInputs& m_inputs;
    HashMap<uint32_t, SelectedLayouts, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_selectedLayouts;
    Vector<LayoutID> m_escapedClasses;
    Vector<LayoutID> m_lostClasses;
    Vector<ValueSetID> m_objectsOfLayout;
};

} // anonymous namespace

static bool isClassLayout(const Layout& layout)
{
    return layout.kind == LayoutKind::Instance && layout.constructor && !layout.has(LayoutFlags::IsConstructorFunction);
}

LayoutID LayoutsOfObjects::namedLayoutOf(ValueFlowObjectID object) const
{
    ValueFlowObjectKind kind = m_inputs.universe().kindOf(object);
    return kind == ValueFlowObjectKind::Site || kind == ValueFlowObjectKind::Instance ? m_inputs.universe().layoutOf(object) : LayoutID();
}

bool LayoutsOfObjects::selectsLayoutByPrototype(ValueFlowObjectID object) const
{
    const ValueFlowSite* site = m_inputs.universe().siteOf(object);
    if (!site || !site->layout || site->ordinal || !m_inputs.program.prototypeMaySelectOtherClass(site->layout))
        return false;
    const Value& allocation = m_inputs.program.at(m_inputs.universe().functionOf(object)).at(site->value);
    // What a base class that is no class of the program makes for `super(..)` has the prototype that new.target says, like the allocation in the constructor of a base class.
    if (site->origin == ValueFlowSiteOrigin::BuiltinResult)
        return OperandPosition::parent(allocation.opcode).has_value();
    return site->origin == ValueFlowSiteOrigin::Operation && allocation.opcode == Opcode::NewObject && allocation.prototypeOperand();
}

// The classes whose layout the prototype that an allocation of another class is given may select.
ValueSetID LayoutsOfObjects::collectClassLayouts(ValueFlowHeapView& heap)
{
    const Program& program = m_inputs.program;
    const ValueFlowUniverse& universe = m_inputs.universe();
    const ValueSets& sets = m_inputs.sets();
    Vector<ValueFlowObjectID> prototypeObjects;
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        const Layout& layout = program.layouts[index];
        if (!isClassLayout(layout) || !layout.has(LayoutFlags::HasHeritage))
            continue;
        ValueFlowObjectID prototype = universe.prototypeOf(LayoutID(index));
        ValueFlowObjectID classObject = universe.closureOf(layout.constructor);
        prototypeObjects.append(prototype);
        if (sets.contains(heap.escaped(), prototype) || sets.contains(heap.escaped(), classObject))
            m_escapedClasses.append(LayoutID(index));
        if (sets.contains(heap.lost(), prototype) || sets.contains(heap.lost(), classObject))
            m_lostClasses.append(LayoutID(index));
    }
    return heap.maker().of(prototypeObjects.span());
}

SetsByNumber LayoutsOfObjects::collectWrittenPrototypeLinks(std::span<const ValueFlowObjectID> objects, ValueFlowHeapView& heap) const
{
    const ValueSets& sets = m_inputs.sets();
    ValueSetMaker& maker = heap.maker();
    ValueSetID all = maker.of(objects);
    ValueSetID throughUnknownObject = ValueSets::nothing;
    ValueSetID throughUntrackedObject = ValueSets::nothing;
    SetsByNumber links;
    auto add = [&](ValueFlowObjectID object, ValueSetID value) {
        ValueSetID& link = links.add(object.index(), ValueSets::nothing).iterator->value;
        link = maker.unite(link, value);
    };
    heap.forEachNodeWrittenUnder(prototypeLinkName, [&](ValueSetID writtenThrough, ValueSetID value) {
        sets.forEachMember(maker.intersect(writtenThrough, all), [&](ValueFlowObjectID object) {
            add(object, value);
        });
        if (sets.contains(writtenThrough, ValueFlowUniverse::unknownObject()))
            throughUnknownObject = maker.unite(throughUnknownObject, value);
        if (sets.contains(writtenThrough, ValueFlowUniverse::untrackedObject()))
            throughUntrackedObject = maker.unite(throughUntrackedObject, value);
    });
    for (ValueFlowObjectID object : objects) {
        if (sets.contains(heap.escaped(), object))
            add(object, throughUnknownObject);
        if (sets.contains(heap.lost(), object))
            add(object, throughUntrackedObject);
    }
    return links;
}

auto LayoutsOfObjects::layoutsSelectedBy(ValueSetID prototypes, LayoutID named, ValueSetID prototypeObjectsOfClasses, ValueFlowHeapView& heap) const -> SelectedLayouts
{
    const ValueSets& sets = m_inputs.sets();
    SelectedLayouts selected;
    sets.forEachMember(heap.maker().intersect(prototypes, prototypeObjectsOfClasses), [&](ValueFlowObjectID prototype) {
        selected.listed.append(m_inputs.universe().classLayoutOf(prototype));
    });
    AOT::sort(selected.listed);
    removeRepeatedElements(selected.listed);
    selected.listed.removeFirst(named);
    selected.everyEscapedClass = !m_escapedClasses.isEmpty() && sets.contains(prototypes, ValueFlowUniverse::unknownObject());
    selected.everyLostClass = !m_lostClasses.isEmpty() && sets.contains(prototypes, ValueFlowUniverse::untrackedObject());
    return selected;
}

LayoutsOfObjects::LayoutsOfObjects(const ValueFlowResultInputs& inputs, ValueFlowHeapView& heap)
    : m_inputs(inputs)
{
    struct ObjectOfLayout {
        LayoutID layout;
        ValueFlowObjectID object;
    };

    const ValueFlowUniverse& universe = inputs.universe();
    const ParallelLoops& loops = inputs.loops();
    uint32_t numberOfLayouts = inputs.program.layouts.size();
    Vector<ValueFlowObjectID> objectsThatSelectLayout = collectInParallel<ValueFlowObjectID>(loops, universe.numberOfObjects(), [&](unsigned index, Vector<ValueFlowObjectID>& objects) {
        if (namedLayoutOf(ValueFlowObjectID(index)) && selectsLayoutByPrototype(ValueFlowObjectID(index)))
            objects.append(ValueFlowObjectID(index));
    });
    Vector<ValueFlowObjectID> objectsOfEveryEscapedClass;
    Vector<ValueFlowObjectID> objectsOfEveryLostClass;
    if (!objectsThatSelectLayout.isEmpty()) {
        ValueSetID prototypeObjectsOfClasses = collectClassLayouts(heap);
        SetsByNumber links = collectWrittenPrototypeLinks(objectsThatSelectLayout.span(), heap);
        for (ValueFlowObjectID object : objectsThatSelectLayout) {
            SelectedLayouts selected = layoutsSelectedBy(setAt(links, object.index()), namedLayoutOf(object), prototypeObjectsOfClasses, heap);
            if (selected.everyEscapedClass)
                objectsOfEveryEscapedClass.append(object);
            if (selected.everyLostClass)
                objectsOfEveryLostClass.append(object);
            if (!selected.listed.isEmpty() || selected.everyEscapedClass || selected.everyLostClass)
                m_selectedLayouts.add(object.index(), WTF::move(selected));
        }
    }

    // The objects of a layout side by side.
    Vector<ObjectOfLayout> objectsWithLayouts = collectInParallel<ObjectOfLayout>(loops, universe.numberOfObjects(), [&](unsigned index, Vector<ObjectOfLayout>& found) {
        ValueFlowObjectID object(index);
        LayoutID named = namedLayoutOf(object);
        if (!named)
            return;
        found.append({ named, object });
        auto selected = m_selectedLayouts.find(index);
        if (selected == m_selectedLayouts.end())
            return;
        for (LayoutID layout : selected->value.listed)
            found.append({ layout, object });
    });
    Vector<uint32_t> firstObjectOfLayout;
    firstObjectOfLayout.fill(0u, numberOfLayouts + 1);
    for (const ObjectOfLayout& entry : objectsWithLayouts)
        ++firstObjectOfLayout[entry.layout.index() + 1];
    for (uint32_t index = 0; index < numberOfLayouts; ++index)
        firstObjectOfLayout[index + 1] += firstObjectOfLayout[index];
    Vector<ValueFlowObjectID> objectsOfLayouts(objectsWithLayouts.size());
    Vector<uint32_t> nextObjectOfLayout(firstObjectOfLayout.span().first(numberOfLayouts));
    for (const ObjectOfLayout& entry : objectsWithLayouts)
        objectsOfLayouts[nextObjectOfLayout[entry.layout.index()]++] = entry.object;

    m_objectsOfLayout.fill(ValueSets::nothing, numberOfLayouts);
    {
        ValueSetMakers makers(inputs);
        forEachIndexInParallel(loops, numberOfLayouts, [&](unsigned index, unsigned worker) {
            if (firstObjectOfLayout[index] != firstObjectOfLayout[index + 1])
                m_objectsOfLayout[index] = makers.of(worker).of(objectsOfLayouts.subspan(firstObjectOfLayout[index], firstObjectOfLayout[index + 1] - firstObjectOfLayout[index]));
        });
    }
    auto addToEach = [&](const Vector<LayoutID>& layouts, const Vector<ValueFlowObjectID>& objects) {
        if (objects.isEmpty())
            return;
        ValueSetID set = heap.maker().of(objects.span());
        for (LayoutID layout : layouts)
            m_objectsOfLayout[layout.index()] = heap.maker().unite(m_objectsOfLayout[layout.index()], set);
    };
    addToEach(m_escapedClasses, objectsOfEveryEscapedClass);
    addToEach(m_lostClasses, objectsOfEveryLostClass);
}

namespace {

struct StoredUnderKey {
    ValueSetID value { ValueSets::nothing };
    // By something else than the operation that allocates the object.
    bool isStoredAfterCreation { false };
    bool mayBeAccessor { false };
    // What the operations of the program do, for a guess at what the property is.
    ValueSetID valueFromOperations { ValueSets::nothing };
    ValueSetID getters { ValueSets::nothing };
    uint8_t accessorForms { 0 };
    // An operation puts a value there itself: a store, or an allocation that lists the field.
    bool isGivenValueByOperation { false };
    // An operation of the program stores under the key and spells it, or computes a key that can be no other. The code of a built-in and a store under a key that may be one of several do not count.
    bool isNamedByOperation { false };
    // A constructor function stores it into its `this`, so the objects that pass the constructor have it.
    bool isStoredByConstructor { false };
    // The numbers in `value`, by what the operations that store them make. Any number as soon as something else than an operation with types stores one.
    bool mayBeOtherThanIntegers { false };
    Range integers { Range::empty() };

    ValueFlowNumbers numbers() const { return { mayBeOtherThanIntegers, integers }; }

    void uniteNumbers(const ValueFlowNumbers& other)
    {
        mayBeOtherThanIntegers |= other.mayBeOtherThanIntegers;
        integers = integers.unionWith(other.integers);
    }
};
static_assert(sizeof(StoredUnderKey) == 40);

// Most objects have a few keys, which are searched one after the other. A long list has an index.
class StoresUnderKeys {
public:
    struct Entry {
        uint32_t key;
        StoredUnderKey value;
    };

    struct AddResult {
        Entry* iterator;
        bool isNewEntry;
    };

    size_t size() const { return m_entries.size(); }
    bool isEmpty() const { return m_entries.isEmpty(); }
    const Entry* begin() const LIFETIME_BOUND { return m_entries.span().data(); }
    const Entry* end() const LIFETIME_BOUND { return begin() + m_entries.size(); }

    const Entry* find(uint32_t key) const LIFETIME_BOUND
    {
        if (m_index) {
            auto found = m_index->find(key);
            return found == m_index->end() ? end() : begin() + found->value;
        }
        for (const Entry& entry : m_entries) {
            if (entry.key == key)
                return &entry;
        }
        return end();
    }

    AddResult add(uint32_t key, StoredUnderKey&& value) LIFETIME_BOUND
    {
        if (const Entry* found = find(key); found != end())
            return { m_entries.mutableSpan().data() + (found - begin()), false };
        if (m_index)
            m_index->add(key, m_entries.size());
        else if (m_entries.size() == mostEntriesWithoutIndex) {
            m_index = makeUnique<Index>();
            for (uint32_t position = 0; position < m_entries.size(); ++position)
                m_index->add(m_entries[position].key, position);
            m_index->add(key, m_entries.size());
        }
        m_entries.append(Entry { key, WTF::move(value) });
        return { &m_entries.last(), true };
    }

private:
    static constexpr size_t mostEntriesWithoutIndex = 12;
    using Index = HashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;

    Vector<Entry, 0, CrashOnOverflow, 1> m_entries;
    std::unique_ptr<Index> m_index;
};

// A bit for each form that a definition may give an accessor, and one for a definition of which nobody can tell.
struct AccessorForms {
    static constexpr uint8_t unknown = 1 << 4;

    static constexpr uint8_t of(bool isEnumerable, bool isConfigurable) { return 1 << ((isEnumerable ? 0 : 2) + (isConfigurable ? 0 : 1)); }

    static std::optional<OptionSet<FieldForm>> onlyFormIn(uint8_t forms)
    {
        for (bool isEnumerable : { true, false }) {
            for (bool isConfigurable : { true, false }) {
                if (forms != of(isEnumerable, isConfigurable))
                    continue;
                OptionSet<FieldForm> form { FieldForm::IsAccessor };
                if (!isEnumerable)
                    form.add(FieldForm::IsNotEnumerable);
                if (!isConfigurable)
                    form.add(FieldForm::IsNotConfigurable);
                return form;
            }
        }
        return std::nullopt;
    }
};

// What is written into the objects that one layout, or one placeholder, stands for.
struct StoresIntoObjects {
    StoresUnderKeys underKey;
    ValueSetID underUnknownKey { ValueSets::nothing };
    ValueFlowNumbers numbersUnderUnknownKey;
    // A property under an index, an accessor under a key nobody knows, or the properties of other objects, of which nobody has followed which.
    bool isStoredUnderIndex { false };
    bool getsAccessorUnderUnknownKey { false };
    bool getsCopiedProperties { false };
    // What the operations of the program do under a key nobody knows: as StoredUnderKey has it.
    ValueSetID valueFromOperationsUnderUnknownKey { ValueSets::nothing };
    bool isGivenValueByOperationUnderUnknownKey { false };
    ValueSetID gettersUnderUnknownKey { ValueSets::nothing };
    uint8_t formsOfAccessorsUnderUnknownKey { 0 };

    bool getsNameNobodyLists() const { return underUnknownKey != ValueSets::nothing || isStoredUnderIndex || getsAccessorUnderUnknownKey || getsCopiedProperties; }
};

// One run of a statement that writes into objects, as far as the results about fields care.
struct Writer {
    const ValueFlowStatement& statement;
    // The objects that the statement writes into are those that it allocates.
    bool isAllocation { false };
    // The layouts that the type of the receiver admits. Empty where the type says nothing.
    std::span<const LayoutID> layoutsOfReceiver;
    LayoutSetID familyOfReceiver;
};

// A property is written through a set of objects under a name. This sorts what is written by the layouts of the objects.
class StoresByLayout {
public:
    StoresByLayout(const ValueFlowResultInputs&, const FlowResultTables&, ValueFlowHeapView&, const LayoutsOfObjects&);
    void noteListedWrites();

    // All that a store may put under the key into an object of the layout.
    ValueSetID storedUnder(LayoutID layout, PropertyKeyID key) { return storedUnder(m_heap.maker(), layout, key); }
    ValueSetID storedUnder(ValueSetMaker&, LayoutID, PropertyKeyID) const;
    // The numbers among them.
    ValueFlowNumbers numbersStoredUnder(LayoutID, PropertyKeyID) const;
    bool isStoredAfterCreation(LayoutID, PropertyKeyID) const;
    // The properties of other objects are copied onto an object of the layout, and nobody has followed which.
    bool getsCopiedProperties(LayoutID) const;
    // Something may lie on an object of the layout under a name that forEachKeyStoredInto() does not list.
    bool getsNameNobodyLists(LayoutID) const;
    // Ascending, each once.
    Vector<PropertyKeyID> keysStoredInto(LayoutID) const;
    Vector<PropertyKeyID> keysStoredIntoKnownObjects(LayoutID) const;
    Vector<PropertyKeyID> keysNamedByStoresIntoKnownObjects(LayoutID) const;
    bool isStoredByConstructor(LayoutID, PropertyKeyID) const;
    const Vector<PropertyKeyID>& keysStoredThroughPlaceholders() const LIFETIME_BOUND { return m_keysThroughPlaceholders; }
    bool isReachedThroughPlaceholders(LayoutID layout) const
    {
        return hasEscapedObject(layout) || hasLostObject(layout) || !m_keysIntoEveryObject.isEmpty()
            || (m_isEscapedClass.quickGet(layout.index()) && !m_keysIntoEveryEscapedClass.isEmpty())
            || (m_isLostClass.quickGet(layout.index()) && !m_keysIntoEveryLostClass.isEmpty());
    }
    bool mayGetAccessorUnder(LayoutID, PropertyKeyID) const;
    // Where all that was seen to be defined under the key in the known objects of the layout are accessors of one form.
    FlowAccessorField accessorDefinedUnder(LayoutID layout, PropertyKeyID key) const { return accessorDefinedUnder(m_heap.maker(), layout, key); }
    FlowAccessorField accessorDefinedUnder(ValueSetMaker&, LayoutID, PropertyKeyID) const;
    // The keys that a definition of an accessor names and for which accessorDefinedUnder() has an answer, ascending.
    Vector<PropertyKeyID> keysOfAccessorsDefinedIn(ValueSetMaker&, LayoutID) const;
    // Whether accessorDefinedUnder() may have an answer for a key that no definition names.
    bool getsAccessorsUnderKeysNobodyKnows(LayoutID) const;
    // By key, the objects that a read by that name lists among its receivers. An accessor gets a slot for them alone: a slot does nothing for a read that starts below the object or that does not know its receiver.
    void setReceiversOfReads(ListsOfSetsByNumber&&);
    bool isReceiverOfReads(LayoutID, PropertyKeyID) const;
    // For a layout that gets accessors under keys nobody knows: the names of the reads that list one of its objects among their receivers, ascending.
    std::span<const PropertyKeyID> keysReadFrom(LayoutID layout) const LIFETIME_BOUND { return m_keysReadFrom[layout.index()].span(); }

    // An object of the layout is in hands that nobody follows.
    bool hasEscapedObject(LayoutID layout) const { return m_hasEscapedObject.quickGet(layout.index()); }
    bool hasLostObject(LayoutID layout) const { return m_hasLostObject.quickGet(layout.index()); }
    bool hasObjectOfCodeWithoutSolve(LayoutID layout) const { return m_hasObjectOfCodeWithoutSolve.quickGet(layout.index()); }

    // Every statement that has run and has written something into an object, with the node that took it.
    void forEachWrite(NOESCAPE const Invocable<void(const Writer&, const ValueFlowNodeKey&, ValueSetID value)> auto&);
    // The same on all threads, in no order. `worker` is what forEachIndexInParallel() hands to the body of a loop.
    void forEachWriteInParallel(NOESCAPE const Invocable<void(unsigned worker, const Writer&, const ValueFlowNodeKey&, ValueSetID value)> auto&) const;
    void forEachLayoutWrittenThrough(ValueSetID objects, const Writer&, NOESCAPE const Invocable<void(LayoutID)> auto&);
    // The same for the set that a write goes through, on any thread.
    void forEachLayoutWrittenThroughSetOfWrite(ValueSetID objects, const Writer&, NOESCAPE const Invocable<void(LayoutID)> auto&);

    // The same in two parts, for whoever looks at every write: thousands of writes go through a placeholder, and a placeholder stands for the objects of thousands of layouts.
    struct PlaceholdersInSet {
        bool unknownObject { false };
        bool untrackedObject { false };
        bool almostEverything { false };
    };
    PlaceholdersInSet placeholdersIn(ValueSetID objects);
    void forEachLayoutListedIn(ValueSetID objects, const Writer&, NOESCAPE const Invocable<void(LayoutID)> auto&);
    // Behind this the two functions below answer for these sets too.
    void lookAt(std::span<const ValueSetID>);
    // For the set that a write goes through, on any thread.
    PlaceholdersInSet placeholdersInSetOfWrite(ValueSetID objects) const;
    void forEachLayoutListedInSetOfWrite(ValueSetID objects, const Writer&, NOESCAPE const Invocable<void(LayoutID)> auto&) const;

private:
    struct WrittenNode {
        uint8_t detail;
        uint32_t payload;
        uint32_t set;
        ValueSetID value;

        auto order() const { return std::tuple { detail, payload, set }; }
    };

    struct ObjectsOfSet {
        // Without the classes of the two lists that LayoutsOfObjects keeps once.
        Vector<LayoutID, 4> layouts;
        bool holdsObjectOfEveryEscapedClass { false };
        bool holdsObjectOfEveryLostClass { false };
        bool holdsUnknownObject { false };
        bool holdsUntrackedObject { false };
        bool holdsAlmostEverything { false };
    };

    struct WritesThroughSet {
        uint32_t set;
        uint32_t payload;
        uint32_t familyOfReceiver;
        uint32_t familyDeclaredForReceiver;
        uint8_t detail;
        bool fillsTypedResult;
        bool createsData;
        bool isStoredAfterCreation;
        bool isNamedByOperation;
        // By a constructor function into its `this`.
        bool isStoredByConstructor;
        bool isWrittenByOperation;
        bool givesValueItself;
        uint8_t accessorForms;
        ValueSetID value;
        ValueFlowNumbers numbers;

        ValueFlowName name() const { return { static_cast<ValueFlowNameKind>(detail), payload }; }
        auto group() const { return std::tuple { set, detail, payload, familyOfReceiver, familyDeclaredForReceiver, fillsTypedResult, createsData }; }
        uint32_t hashOfGroup() const
        {
            uint64_t hash = (static_cast<uint64_t>(payload) << 32 | set) * 0x9e3779b97f4a7c15ull;
            hash = (hash ^ hash >> 32 ^ (static_cast<uint64_t>(familyOfReceiver) << 32 | familyDeclaredForReceiver)) * 0xbf58476d1ce4e5b9ull;
            hash = (hash ^ hash >> 32 ^ static_cast<uint64_t>(detail << 2 | fillsTypedResult << 1 | createsData)) * 0x94d049bb133111ebull;
            return static_cast<uint32_t>(hash >> 32);
        }
    };

    // What hangs on the statement and the name of a write, whatever set it goes through.
    struct StoreUnderName {
        const ValueFlowStatement* statement { nullptr };
        ValueFlowName name;
        bool spellsKey { false };
        bool isIntoThisOfConstructor { false };
        uint8_t accessorForms { 0 };
        uint32_t familyDeclaredForReceiver { UINT32_MAX };
        // None until a node under the name holds a number.
        std::optional<ValueFlowNumbers> numbers;
    };

    // A store that spells its key, through a set with a placeholder, whose receiver has a type that lists layouts.
    struct NameStoredThroughDeclaredReceiver {
        uint32_t family;
        uint8_t placeholders;
        bool isStoredByConstructor;
        PropertyKeyID key;

        auto order() const { return std::tuple { family, placeholders, isStoredByConstructor, key }; }
    };

    void findReceiversBehindChecks();
    static constexpr uint32_t isStatementInListOfWrites = 1u << 31;

    void listWrites();
    void forEachWriteAmong(std::span<const uint32_t>, NOESCAPE const Invocable<void(const Writer&, const ValueFlowNodeKey&, ValueSetID value)> auto&) const;
    ObjectsOfSet findObjectsOf(ValueSetID) const;
    const ObjectsOfSet& objectsOf(ValueSetID);
    std::optional<uint32_t> positionOfSetLookedAt(ValueSetID) const;
    void noteObjectsOf(ValueSetID, ObjectsOfSet&&);
    const ObjectsOfSet& objectsOfSetAt(uint32_t position) const;
    const ObjectsOfSet& objectsOfSetOfWrite(ValueSetID) const;
    void forEachLayoutListedAmong(const ObjectsOfSet&, const Writer&, NOESCAPE const Invocable<void(LayoutID)> auto&) const;
    bool admits(std::span<const LayoutID> layoutsOfReceiver, LayoutID) const;
    bool admits(const Writer& writer, LayoutID layout) const { return admits(writer.layoutsOfReceiver, layout); }
    const Vector<LayoutID>& layoutsAdmittedBy(LayoutSetID familyOfReceiver);
    uint32_t familyDeclaredForReceiverOf(const Writer&) const;
    uint8_t formsOfAccessorDefinedBy(ValueSetMaker&, const Writer&, ValueFlowName) const;
    StoreUnderName describeStore(ValueSetMaker&, const Writer&, ValueFlowName) const;
    // The store is what the call before has left there: the description of the last statement and name that were looked at.
    WritesThroughSet describeWrite(ValueSetMaker&, StoreUnderName&, const Writer&, const ValueFlowNodeKey&, ValueSetID value) const;
    ParallelArray<WritesThroughSet> collectWrites(ValueSetMakers&) const;
    void noteWrites();
    void noteWritesInto(ValueSetMaker&, LayoutID, const WritesThroughSet&);
    void noteWritesThroughPlaceholder(ValueSetMaker&, StoresIntoObjects&, const WritesThroughSet&);
    void noteNamesStoredThroughDeclaredReceivers();
    Vector<LayoutID> receiversNamedBy(LayoutSetID family, uint8_t placeholders);
    // By the index of the key in the upper half and that of the layout in the lower.
    using AccessesByKeyAndLayout = HashMap<uint64_t, uint32_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;
    AccessesByKeyAndLayout countAccessesThroughTypesOfFewLayouts(const BitVector& keys, uint64_t mostLayouts);
    void unite(ValueSetID& into, ValueSetID value) { into = m_heap.maker().unite(into, value); }
    bool spellsKey(ValueSetMaker&, const Writer&, ValueFlowName) const;
    void note(ValueSetMaker&, StoresIntoObjects&, ValueFlowName, ValueSetID value, bool isAllocation, bool isNamedByOperation);
    bool someReceiverDeclares(PropertyKeyID, const ObjectsOfSet&) const;
    ValueFlowNumbers numbersStoredBy(const Writer&, ValueFlowName) const;
    void indexNumbersInData();
    ValueFlowNumbers numbersInData(LayoutID, ValueFlowName) const;
    void noteNumbers(StoresIntoObjects&, ValueFlowName, const ValueFlowNumbers&);
    void noteCodeWithoutSolve(const Function&);
    std::optional<Vector<LayoutID, 4>> layoutsCopiedOnto(LayoutID);
    void followCopiedProperties();
    void forEachSourceOf(LayoutID, NOESCAPE const Invocable<void(const StoresIntoObjects&)> auto&) const;

    const ValueFlowResultInputs& m_inputs;
    ValueFlowHeapView& m_heap;
    ListsOfSetsByNumber m_receiversOfReads;
    Vector<Vector<PropertyKeyID>> m_keysReadFrom;
    ValueSetID m_objectsPutAboveOthers { ValueSets::nothing };
    const LayoutsOfObjects& m_layoutsOfObjects;
    Vector<uint32_t> m_setsLookedAt;
    Vector<ObjectsOfSet> m_objectsOfSetsOfWrites;
    Vector<ObjectsOfSet> m_objectsOfOtherSets;
    HashedIndex m_positionOfSetLookedAt;
    Vector<StoresIntoObjects> m_storesOfLayout;
    Vector<Vector<LayoutID>> m_layoutsExpectedDirectlyBelow;
    HashMap<uint32_t, Vector<LayoutID>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_layoutsAdmittedBy;
    Lock m_lockOfLayoutsAdmittedBy;
    // What is stored through the unknown object is stored into every object that has escaped, and what is stored through the untracked object into every object that the analysis has stopped following.
    StoresIntoObjects m_storesThroughUnknownObject;
    StoresIntoObjects m_storesThroughUntrackedObject;
    // Through a set that holds all of the universe but a few members.
    StoresIntoObjects m_storesIntoEveryObject;
    StoresIntoObjects m_storesIntoEveryEscapedClass;
    StoresIntoObjects m_storesIntoEveryLostClass;
    // The keys of the three, ascending: every layout with an object that has escaped asks for hundreds of them.
    Vector<PropertyKeyID> m_keysThroughUnknownObject;
    Vector<PropertyKeyID> m_keysThroughUntrackedObject;
    Vector<PropertyKeyID> m_keysIntoEveryObject;
    Vector<PropertyKeyID> m_keysIntoEveryEscapedClass;
    Vector<PropertyKeyID> m_keysIntoEveryLostClass;
    Vector<PropertyKeyID> m_keysThroughPlaceholders;
    BitVector m_hasEscapedObject;
    BitVector m_hasLostObject;
    BitVector m_isEscapedClass;
    BitVector m_isLostClass;
    Vector<NameStoredThroughDeclaredReceiver> m_namesStoredThroughDeclaredReceivers;
    // By function. Empty where no store of the function asks.
    Vector<BitVector> m_valuesBehindChecks;
    Vector<uint8_t> m_hasChecksNotLookedAt;
    // Every write of a statement that has run, in the order of the statements: the statement, marked, then what ValueFlowSolver::indexOfMadeNodes() says of each node it writes to.
    Vector<uint32_t> m_writes;
    BitVector m_hasObjectOfCodeWithoutSolve;
    BitVector m_keysDeclaredByEscapedClasses;
    BitVector m_keysDeclaredByLostClasses;
    ValueSetID m_unknownValue;
    // By layout and key: the numbers that the constant data of the program lists. Made when the first operation that creates data is looked at.
    std::optional<HashMap<uint64_t, ValueFlowNumbers, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>> m_numbersInData;
};

} // anonymous namespace

static bool haveCommonMember(std::span<const LayoutID> ascending, std::span<const LayoutID> otherAscending)
{
    if (ascending.size() > otherAscending.size())
        std::swap(ascending, otherAscending);
    return std::ranges::any_of(ascending, [&](LayoutID layout) {
        return std::ranges::binary_search(otherAscending, layout);
    });
}

static Vector<PropertyKeyID> sortedKeysOf(const StoresIntoObjects& stores)
{
    Vector<PropertyKeyID> keys;
    keys.reserveInitialCapacity(stores.underKey.size());
    for (const auto& [key, stored] : stores.underKey)
        keys.append(PropertyKeyID(key));
    AOT::sort(keys);
    return keys;
}

StoresByLayout::StoresByLayout(const ValueFlowResultInputs& inputs, const FlowResultTables& tables, ValueFlowHeapView& heap, const LayoutsOfObjects& layoutsOfObjects)
    : m_inputs(inputs)
    , m_heap(heap)
    , m_layoutsOfObjects(layoutsOfObjects)
    , m_storesOfLayout(inputs.program.layouts.size())
    , m_hasEscapedObject(inputs.program.layouts.size())
    , m_hasLostObject(inputs.program.layouts.size())
    , m_isEscapedClass(inputs.program.layouts.size())
    , m_isLostClass(inputs.program.layouts.size())
    , m_hasObjectOfCodeWithoutSolve(inputs.program.layouts.size())
    , m_unknownValue(heap.maker().unite(inputs.universe().anyPrimitive(), inputs.universe().unknownObjectAlone()))
{
    const Program& program = inputs.program;
    for (LayoutID layout : layoutsOfObjects.escapedClasses())
        m_isEscapedClass.quickSet(layout.index());
    for (LayoutID layout : layoutsOfObjects.lostClasses())
        m_isLostClass.quickSet(layout.index());
    PhaseTimes times(inputs.build, PassID::AnalyzeFlow);
    findReceiversBehindChecks();
    times.didFinish("stores by layout: receivers behind checks"_s);
    listWrites();
    times.didFinish("stores by layout: writes listed"_s);
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        if (isCompiledWithoutSolve(inputs, tables, FunctionID(index)))
            noteCodeWithoutSolve(program.functions[index]);
    }
    auto noteLayoutsOfObjectsIn = [&](ValueSetID objects, BitVector& layouts) {
        auto note = [&](LayoutID layout) {
            layouts.quickSet(layout.index());
        };
        if (!ValueSets::isComplement(objects)) {
            layoutsOfObjects.forEachLayoutOfMembers(objects, note);
            return;
        }
        for (uint32_t index = 0; index < inputs.universe().numberOfObjects(); ++index) {
            if (inputs.sets().contains(objects, ValueFlowObjectID(index)))
                layoutsOfObjects.forEachLayoutOf(ValueFlowObjectID(index), note);
        }
    };
    noteLayoutsOfObjectsIn(heap.escaped(), m_hasEscapedObject);
    noteLayoutsOfObjectsIn(heap.lost(), m_hasLostObject);
    m_hasEscapedObject.merge(m_hasObjectOfCodeWithoutSolve);
    times.didFinish("stores by layout: code without solve, layouts of what escaped"_s);
}

void StoresByLayout::noteListedWrites()
{
    PhaseTimes times(m_inputs.build, PassID::AnalyzeFlow);
    noteWrites();
    times.didFinish("stores by layout: writes noted"_s);
    noteNamesStoredThroughDeclaredReceivers();
    times.didFinish("stores by layout: names through declared receivers"_s);
    m_keysThroughUnknownObject = sortedKeysOf(m_storesThroughUnknownObject);
    m_keysThroughUntrackedObject = sortedKeysOf(m_storesThroughUntrackedObject);
    m_keysIntoEveryObject = sortedKeysOf(m_storesIntoEveryObject);
    m_keysIntoEveryEscapedClass = sortedKeysOf(m_storesIntoEveryEscapedClass);
    m_keysIntoEveryLostClass = sortedKeysOf(m_storesIntoEveryLostClass);
    m_keysThroughPlaceholders.appendVector(m_keysThroughUnknownObject);
    m_keysThroughPlaceholders.appendVector(m_keysThroughUntrackedObject);
    m_keysThroughPlaceholders.appendVector(m_keysIntoEveryObject);
    m_keysThroughPlaceholders.appendVector(m_keysIntoEveryEscapedClass);
    m_keysThroughPlaceholders.appendVector(m_keysIntoEveryLostClass);
    AOT::sort(m_keysThroughPlaceholders);
    removeRepeatedElements(m_keysThroughPlaceholders);
    times.didFinish("stores by layout: keys sorted"_s);
    followCopiedProperties();
    times.didFinish("stores by layout: copied properties"_s);
}

// The layouts of the objects whose properties are copied onto an object of the layout. None where one of those objects may hold a property that nobody lists, or one that answers by running code.
std::optional<Vector<LayoutID, 4>> StoresByLayout::layoutsCopiedOnto(LayoutID target)
{
    const ValueSets& sets = m_inputs.sets();
    ValueSetID sources = m_heap.writtenUnder(m_layoutsOfObjects.objectsWith(target), ValueFlowName::of(ValueFlowNameKind::CopiedFrom));
    if (ValueSets::isComplement(sources) || sets.meet(sources, m_inputs.universe().placeholdersAlone()))
        return std::nullopt;

    Vector<LayoutID, 4> layouts;
    bool holdOnlyWhatIsWritten = true;
    sets.forEachMember(sources, [&](ValueFlowObjectID source) {
        size_t numberOfOtherLayouts = layouts.size();
        m_layoutsOfObjects.forEachLayoutOf(source, [&](LayoutID layout) {
            layouts.append(layout);
        });
        holdOnlyWhatIsWritten &= layouts.size() != numberOfOtherLayouts && !m_inputs.classOf(source);
    });
    if (!holdOnlyWhatIsWritten)
        return std::nullopt;
    AOT::sort(layouts);
    removeRepeatedElements(layouts);

    for (LayoutID layout : layouts) {
        LayoutKind kind = m_inputs.program.at(layout).kind;
        if (kind != LayoutKind::Record && kind != LayoutKind::Instance)
            return std::nullopt;
        if (m_hasEscapedObject.quickGet(layout.index()) || m_hasLostObject.quickGet(layout.index()) || getsNameNobodyLists(layout))
            return std::nullopt;
        for (PropertyKeyID key : keysStoredInto(layout)) {
            if (mayGetAccessorUnder(layout, key))
                return std::nullopt;
        }
    }
    return layouts;
}

// The rules let the target of a copy find what the source holds as it finds what it inherits.
void StoresByLayout::followCopiedProperties()
{
    Vector<LayoutID> targets;
    for (uint32_t index = 0; index < m_storesOfLayout.size(); ++index) {
        if (m_storesOfLayout[index].getsCopiedProperties)
            targets.append(LayoutID(index));
    }
    for (bool hasChanged = !targets.isEmpty(); hasChanged;) {
        hasChanged = false;
        for (LayoutID target : targets) {
            StoresIntoObjects& stores = m_storesOfLayout[target.index()];
            if (!stores.getsCopiedProperties)
                continue;
            // An object of the layout may be the source as well.
            stores.getsCopiedProperties = false;
            std::optional<Vector<LayoutID, 4>> sources = layoutsCopiedOnto(target);
            if (!sources) {
                stores.getsCopiedProperties = true;
                continue;
            }
            for (LayoutID source : *sources) {
                for (PropertyKeyID key : keysStoredInto(source)) {
                    ValueSetID value = storedUnder(source, key);
                    ValueFlowNumbers numbers = numbersStoredUnder(source, key);
                    StoredUnderKey& stored = stores.underKey.add(key.index(), StoredUnderKey { }).iterator->value;
                    unite(stored.value, value);
                    stored.uniteNumbers(numbers);
                    stored.isStoredAfterCreation = true;
                }
            }
            hasChanged = true;
        }
    }
}

// Only a function with a Check and with a store through a receiver whose type lists layouts is looked at.
void StoresByLayout::findReceiversBehindChecks()
{
    const Program& program = m_inputs.program;
    m_valuesBehindChecks.grow(program.functions.size());
    m_hasChecksNotLookedAt.fill(0, program.functions.size());
    forEachIndexInParallel(m_inputs.loops(), program.functions.size(), [&](unsigned index) {
        const Function& function = program.functions[index];
        if (!m_inputs.isReached(FunctionID(index)))
            return;
        bool hasCheck = false;
        bool hasTypedReceiver = false;
        for (const Value& value : function.values) {
            hasCheck |= value.opcode == Opcode::Check;
            if (hasTypedReceiver || accessOf(value.opcode) == Access::Read)
                continue;
            if (std::optional<uint8_t> receiver = receiverOf(value.opcode); receiver && value.operand(*receiver))
                hasTypedReceiver = program.typeOf(function, value.operand(*receiver)).hasOnlyLayouts(program);
            // The type of a stored value is asked for its integers.
            if (receiverOf(value.opcode) || value.opcode == Opcode::NewObject) {
                value.forEachOperand([&](ValueID operand, uint32_t) {
                    hasTypedReceiver |= !program.typeOf(function, operand).integers.isEmpty();
                });
            }
        }
        bool isTypedBody = m_inputs.universe().genericVersionOf(FunctionID(index)) && function.role != FunctionRole::EntryChecks;
        if (hasCheck && hasTypedReceiver && !isTypedBody)
            m_valuesBehindChecks[index] = findValuesBehindChecks(program, function, CFG(function));
        m_hasChecksNotLookedAt[index] = hasCheck && !hasTypedReceiver && !isTypedBody;
    });
}

// The solver is asked once, statement by statement, on all threads.
void StoresByLayout::listWrites()
{
    constexpr uint32_t madeStatementsPerIndex = 512;
    constexpr size_t mostWritesNotToldApart = 64;
    const ValueFlowSolver& solver = m_inputs.solver();
    std::span<const ValueFlowStatement> statements = m_inputs.bodies().statements();
    std::span<const ValueFlowBodyRange> ranges = m_inputs.bodies().ranges();
    uint32_t numberOfBodies = m_inputs.bodies().numberOfBodies();
    uint32_t numberOfStatements = solver.numberOfStatements();
    uint32_t numberOfMadeStatements = numberOfStatements - static_cast<uint32_t>(statements.size());
    uint32_t numberOfRunsOfMadeStatements = (numberOfMadeStatements + madeStatementsPerIndex - 1) / madeStatementsPerIndex;
    m_writes = collectInParallel<uint32_t>(m_inputs.loops(), numberOfBodies + numberOfRunsOfMadeStatements, [&](unsigned position, Vector<uint32_t>& writes) {
        auto listWritesOf = [&](ValueFlowStatementID id) {
            bool isListed = false;
            size_t firstWrite = 0;
            // A statement that keeps what all its runs have asked for names the same nodes again and again. In a long list a node stands once.
            HashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> nodesListed;
            m_inputs.forEachMadeNodeAskedForBy(id, kindsOfMadeNodes({ ValueFlowMadeNodeKind::Written }), false, [&](const ValueFlowNodeKey& key, ValueFlowStatementID, ValueFlowNodeID node) {
                if (solver.valueOf(node) == ValueSets::nothing)
                    return;
                uint32_t index = solver.indexOfMadeNodes(key);
                RELEASE_ASSERT(!((index | id.index()) & isStatementInListOfWrites));
                if (!std::exchange(isListed, true)) {
                    writes.append(id.index() | isStatementInListOfWrites);
                    firstWrite = writes.size();
                }
                if (writes.size() - firstWrite >= mostWritesNotToldApart) {
                    if (nodesListed.isEmpty()) {
                        for (uint32_t earlier : writes.subspan(firstWrite))
                            nodesListed.add(earlier);
                    }
                    if (!nodesListed.add(index).isNewEntry)
                        return;
                }
                writes.append(index);
            });
        };
        if (position < numberOfBodies) {
            if (position != m_inputs.bodies().hostBody() && !m_inputs.isReached(FunctionID(position)))
                return;
            for (uint32_t index = ranges[position].statement; index < ranges[position + 1].statement; ++index) {
                m_inputs.prefetchMadeNodesAskedForBy(index + 8, kindsOfMadeNodes({ ValueFlowMadeNodeKind::Written }), false);
                listWritesOf(ValueFlowStatementID(index));
            }
            return;
        }
        uint32_t first = static_cast<uint32_t>(statements.size()) + (position - numberOfBodies) * madeStatementsPerIndex;
        for (uint32_t index = first; index < std::min(first + madeStatementsPerIndex, numberOfStatements); ++index) {
            if (!solver.isAtRest(ValueFlowStatementID(index)))
                listWritesOf(ValueFlowStatementID(index));
        }
    });
}

// The span holds all the writes of each of its statements.
void StoresByLayout::forEachWriteAmong(std::span<const uint32_t> writes, NOESCAPE const Invocable<void(const Writer&, const ValueFlowNodeKey&, ValueSetID value)> auto& functor) const
{
    const Program& program = m_inputs.program;
    const ValueFlowSolver& solver = m_inputs.solver();
    std::span<const ValueFlowStatement> statements = m_inputs.bodies().statements();
    for (size_t first = 0; first < writes.size();) {
        ValueFlowStatementID id(writes[first++] & ~isStatementInListOfWrites);
        const ValueFlowStatement& statement = id.index() < statements.size() ? statements[id.index()] : m_inputs.solver().statementAt(id);
        Writer writer { statement };
        if (statement.kind == ValueFlowStatementKind::Operation) {
            writer.isAllocation = resultRuleOf(statement.opcode) == ResultRule::Allocation || statement.opcode == Opcode::Data;
            if (std::optional<uint8_t> receiver = receiverOf(statement.opcode)) {
                const Function& function = program.at(statement.function);
                ValueID object = function.at(statement.site).operand(*receiver);
                const Type& type = program.typeOf(function, object);
                if (type.hasOnlyLayouts(program) && !m_valuesBehindChecks[statement.function.index()].get(object.index())) {
                    writer.layoutsOfReceiver = membersOf(program, type);
                    writer.familyOfReceiver = type.layouts;
                }
            }
        }
        for (; first < writes.size() && !(writes[first] & isStatementInListOfWrites); ++first)
            functor(writer, solver.keyOfMadeNodesAt(writes[first]), solver.valueOf(solver.firstOfMadeNodesAt(writes[first])));
    }
}

void StoresByLayout::forEachWrite(NOESCAPE const Invocable<void(const Writer&, const ValueFlowNodeKey&, ValueSetID value)> auto& functor)
{
    forEachWriteAmong(m_writes.span(), functor);
}

void StoresByLayout::forEachWriteInParallel(NOESCAPE const Invocable<void(unsigned worker, const Writer&, const ValueFlowNodeKey&, ValueSetID value)> auto& functor) const
{
    constexpr size_t writesPerIndex = 2048;
    std::span<const uint32_t> writes = m_writes.span();
    // A run begins with the first statement that begins in it.
    auto firstOfRun = [&](size_t run) {
        size_t first = std::min(run * writesPerIndex, writes.size());
        while (first < writes.size() && !(writes[first] & isStatementInListOfWrites))
            ++first;
        return first;
    };
    forEachIndexInParallel(m_inputs.loops(), static_cast<unsigned>((writes.size() + writesPerIndex - 1) / writesPerIndex), [&](unsigned run, unsigned worker) {
        size_t first = firstOfRun(run);
        forEachWriteAmong(writes.subspan(first, firstOfRun(run + 1) - first), [&](const Writer& writer, const ValueFlowNodeKey& key, ValueSetID value) {
            functor(worker, writer, key, value);
        });
    });
}

auto StoresByLayout::findObjectsOf(ValueSetID set) const -> ObjectsOfSet
{
    ObjectsOfSet objects;
    if (ValueSets::isComplement(set)) {
        objects.holdsAlmostEverything = true;
        return objects;
    }
    objects.holdsUnknownObject = m_inputs.sets().contains(set, ValueFlowUniverse::unknownObject());
    objects.holdsUntrackedObject = m_inputs.sets().contains(set, ValueFlowUniverse::untrackedObject());
    LayoutsOfObjects::SharedLists lists = m_layoutsOfObjects.forEachLayoutListedForMembers(set, [&](LayoutID layout) {
        objects.layouts.append(layout);
    });
    objects.holdsObjectOfEveryEscapedClass = lists.everyEscapedClass;
    objects.holdsObjectOfEveryLostClass = lists.everyLostClass;
    AOT::sort(objects.layouts);
    removeRepeatedElements(objects.layouts);
    return objects;
}

auto StoresByLayout::objectsOf(ValueSetID set) -> const ObjectsOfSet&
{
    if (std::optional<uint32_t> position = positionOfSetLookedAt(set))
        return objectsOfSetAt(*position);
    noteObjectsOf(set, findObjectsOf(set));
    return m_objectsOfOtherSets.last();
}

auto StoresByLayout::objectsOfSetAt(uint32_t position) const -> const ObjectsOfSet&
{
    return position < m_objectsOfSetsOfWrites.size() ? m_objectsOfSetsOfWrites[position] : m_objectsOfOtherSets[position - m_objectsOfSetsOfWrites.size()];
}

std::optional<uint32_t> StoresByLayout::positionOfSetLookedAt(ValueSetID set) const
{
    return m_positionOfSetLookedAt.find(IntHash<uint32_t>::hash(set.index()), [&](uint32_t position) {
        return m_setsLookedAt[position] == set.index();
    });
}

void StoresByLayout::noteObjectsOf(ValueSetID set, ObjectsOfSet&& objects)
{
    m_positionOfSetLookedAt.add(IntHash<uint32_t>::hash(set.index()), m_setsLookedAt.size());
    m_setsLookedAt.append(set.index());
    m_objectsOfOtherSets.append(WTF::move(objects));
}

// noteWrites() has looked at the set of every write.
auto StoresByLayout::objectsOfSetOfWrite(ValueSetID set) const -> const ObjectsOfSet&
{
    std::optional<uint32_t> position = positionOfSetLookedAt(set);
    RELEASE_ASSERT(position);
    return objectsOfSetAt(*position);
}

// The type of a receiver is proven, so the object has one of the layouts that it lists, or one that is expected to lie below one of them: an allocation is typed with the layout that it names.
bool StoresByLayout::admits(std::span<const LayoutID> layoutsOfReceiver, LayoutID layout) const
{
    if (layoutsOfReceiver.empty())
        return true;
    const Program& program = m_inputs.program;
    LayoutID current = layout;
    for (size_t steps = 0; current && steps < program.layouts.size(); ++steps) {
        if (std::ranges::binary_search(layoutsOfReceiver, current))
            return true;
        current = layoutExpectedAbove(program, current);
    }
    return false;
}

const Vector<LayoutID>& StoresByLayout::layoutsAdmittedBy(LayoutSetID familyOfReceiver)
{
    return m_layoutsAdmittedBy.ensure(familyOfReceiver.index(), [&] {
        const Program& program = m_inputs.program;
        if (m_layoutsExpectedDirectlyBelow.isEmpty()) {
            m_layoutsExpectedDirectlyBelow.grow(program.layouts.size());
            for (uint32_t index = 0; index < program.layouts.size(); ++index) {
                if (LayoutID above = layoutExpectedAbove(program, LayoutID(index)))
                    m_layoutsExpectedDirectlyBelow[above.index()].append(LayoutID(index));
            }
        }
        Vector<LayoutID> admitted;
        BitVector isAdmitted(program.layouts.size());
        Vector<LayoutID, 16> worklist(program.at(familyOfReceiver).members.span());
        while (!worklist.isEmpty()) {
            LayoutID layout = worklist.takeLast();
            if (isAdmitted.quickSet(layout.index()))
                continue;
            admitted.append(layout);
            worklist.appendVector(m_layoutsExpectedDirectlyBelow[layout.index()]);
        }
        AOT::sort(admitted);
        return admitted;
    }).iterator->value;
}

void StoresByLayout::forEachLayoutWrittenThrough(ValueSetID set, const Writer& writer, NOESCAPE const Invocable<void(LayoutID)> auto& functor)
{
    PlaceholdersInSet placeholders = placeholdersIn(set);
    bool holdsPlaceholder = placeholders.almostEverything || placeholders.unknownObject || placeholders.untrackedObject;
    auto isReached = [&](uint32_t index) {
        return placeholders.almostEverything || (placeholders.unknownObject && m_hasEscapedObject.quickGet(index)) || (placeholders.untrackedObject && m_hasLostObject.quickGet(index));
    };
    if (holdsPlaceholder && writer.layoutsOfReceiver.empty()) {
        for (uint32_t index = 0; index < m_inputs.program.layouts.size(); ++index) {
            if (isReached(index))
                functor(LayoutID(index));
        }
    } else if (holdsPlaceholder) {
        for (LayoutID layout : layoutsAdmittedBy(writer.familyOfReceiver)) {
            if (isReached(layout.index()))
                functor(layout);
        }
    }
    forEachLayoutListedIn(set, writer, functor);
}

void StoresByLayout::forEachLayoutWrittenThroughSetOfWrite(ValueSetID set, const Writer& writer, NOESCAPE const Invocable<void(LayoutID)> auto& functor)
{
    PlaceholdersInSet placeholders = placeholdersInSetOfWrite(set);
    bool holdsPlaceholder = placeholders.almostEverything || placeholders.unknownObject || placeholders.untrackedObject;
    auto isReached = [&](uint32_t index) {
        return placeholders.almostEverything || (placeholders.unknownObject && m_hasEscapedObject.quickGet(index)) || (placeholders.untrackedObject && m_hasLostObject.quickGet(index));
    };
    if (holdsPlaceholder && writer.layoutsOfReceiver.empty()) {
        for (uint32_t index = 0; index < m_inputs.program.layouts.size(); ++index) {
            if (isReached(index))
                functor(LayoutID(index));
        }
    } else if (holdsPlaceholder) {
        // The list lies in a table that moves when another family comes in.
        Vector<LayoutID> admitted;
        {
            Locker locker { m_lockOfLayoutsAdmittedBy };
            admitted = layoutsAdmittedBy(writer.familyOfReceiver);
        }
        for (LayoutID layout : admitted) {
            if (isReached(layout.index()))
                functor(layout);
        }
    }
    forEachLayoutListedInSetOfWrite(set, writer, functor);
}

auto StoresByLayout::placeholdersIn(ValueSetID set) -> PlaceholdersInSet
{
    const ObjectsOfSet& objects = objectsOf(set);
    return { objects.holdsUnknownObject, objects.holdsUntrackedObject, objects.holdsAlmostEverything };
}

void StoresByLayout::lookAt(std::span<const ValueSetID> sets)
{
    Vector<ValueSetID> unseen;
    for (ValueSetID set : sets) {
        if (!positionOfSetLookedAt(set))
            unseen.append(set);
    }
    Vector<ObjectsOfSet> objects(unseen.size());
    forEachIndexInParallel(m_inputs.loops(), unseen.size(), [&](unsigned position) {
        objects[position] = findObjectsOf(unseen[position]);
    });
    for (size_t position = 0; position < unseen.size(); ++position)
        noteObjectsOf(unseen[position], WTF::move(objects[position]));
}

auto StoresByLayout::placeholdersInSetOfWrite(ValueSetID set) const -> PlaceholdersInSet
{
    const ObjectsOfSet& objects = objectsOfSetOfWrite(set);
    return { objects.holdsUnknownObject, objects.holdsUntrackedObject, objects.holdsAlmostEverything };
}

void StoresByLayout::forEachLayoutListedIn(ValueSetID set, const Writer& writer, NOESCAPE const Invocable<void(LayoutID)> auto& functor)
{
    forEachLayoutListedAmong(objectsOf(set), writer, functor);
}

void StoresByLayout::forEachLayoutListedInSetOfWrite(ValueSetID set, const Writer& writer, NOESCAPE const Invocable<void(LayoutID)> auto& functor) const
{
    forEachLayoutListedAmong(objectsOfSetOfWrite(set), writer, functor);
}

void StoresByLayout::forEachLayoutListedAmong(const ObjectsOfSet& objects, const Writer& writer, NOESCAPE const Invocable<void(LayoutID)> auto& functor) const
{
    for (LayoutID layout : objects.layouts) {
        if (admits(writer, layout))
            functor(layout);
    }
    if (objects.holdsObjectOfEveryEscapedClass) {
        for (LayoutID layout : m_layoutsOfObjects.escapedClasses()) {
            if (!std::ranges::binary_search(objects.layouts, layout) && admits(writer, layout))
                functor(layout);
        }
    }
    if (objects.holdsObjectOfEveryLostClass) {
        for (LayoutID layout : m_layoutsOfObjects.lostClasses()) {
            bool isNamedAlready = std::ranges::binary_search(objects.layouts, layout) || (objects.holdsObjectOfEveryEscapedClass && std::ranges::binary_search(m_layoutsOfObjects.escapedClasses(), layout));
            if (!isNamedAlready && admits(writer, layout))
                functor(layout);
        }
    }
}

bool StoresByLayout::spellsKey(ValueSetMaker& maker, const Writer& writer, ValueFlowName name) const
{
    const ValueFlowStatement& statement = writer.statement;
    if (name.kind != ValueFlowNameKind::Property || statement.kind != ValueFlowStatementKind::Operation)
        return false;
    // A computed key that can be one key alone gets that key as its type, and the store becomes a store by name: `holder[kState] = value`.
    // `const { kState } = require(..)` may find nothing as far as anybody knows. SplitUnions tests such a key, and with the symbol the store is a store by name all the same.
    if (statement.opcode == Opcode::ElemSet || statement.opcode == Opcode::ElemDefine) {
        static_assert(OperandIndex::ElemSet::property == OperandIndex::ElemDefine::property);
        ValueSetID keys = std::as_const(m_heap).valueOf(maker, m_inputs.bodies().operandsOf(statement)[OperandIndex::ElemSet::property]);
        if (ValueSets::isComplement(keys))
            return false;
        ValueFlowObjectID key = m_inputs.sets().onlyMember(maker.intersect(keys, ValueSets::complement(maker.of(std::array { ValueFlowUniverse::primitive(PrimitiveSet::Undefined), ValueFlowUniverse::primitive(PrimitiveSet::Absent) }))));
        return key && m_inputs.universe().kindOf(key) == ValueFlowObjectKind::Key && m_inputs.universe().keyOf(key).index() == name.payload;
    }
    if (!Value::opcodesWithKey.contains(statement.opcode))
        return false;
    PropertyKeyID key = m_inputs.program.at(statement.function).at(statement.site).key();
    return key && key.index() == name.payload;
}

void StoresByLayout::note(ValueSetMaker& maker, StoresIntoObjects& stores, ValueFlowName name, ValueSetID value, bool isAllocation, bool isNamedByOperation)
{
    switch (name.kind) {
    case ValueFlowNameKind::Property: {
        StoredUnderKey& stored = stores.underKey.add(name.payload, StoredUnderKey { }).iterator->value;
        stored.value = maker.unite(stored.value, value);
        stored.isStoredAfterCreation |= !isAllocation;
        stored.isNamedByOperation |= isNamedByOperation;
        return;
    }
    case ValueFlowNameKind::Getter:
    case ValueFlowNameKind::Setter: {
        StoredUnderKey& stored = stores.underKey.add(name.payload, StoredUnderKey { }).iterator->value;
        stored.isStoredAfterCreation = true;
        stored.mayBeAccessor = true;
        return;
    }
    case ValueFlowNameKind::UnknownKey:
        stores.underUnknownKey = maker.unite(stores.underUnknownKey, value);
        return;
    case ValueFlowNameKind::Elements:
        stores.isStoredUnderIndex = true;
        return;
    case ValueFlowNameKind::GetterUnderUnknownKey:
    case ValueFlowNameKind::SetterUnderUnknownKey:
        stores.getsAccessorUnderUnknownKey = true;
        return;
    case ValueFlowNameKind::CopiedFrom:
        stores.getsCopiedProperties = true;
        return;
    default:
        return;
    }
}

// None where no operand of the operation is the value.
static ValueID valueStoredUnder(const Program& program, const Value& operation, ValueFlowName name)
{
    if (std::optional<uint8_t> stored = storedOf(operation.opcode))
        return operation.operand(*stored);
    switch (operation.opcode) {
    case Opcode::ElemSet:
        return operation.operand(OperandIndex::ElemSet::value);
    case Opcode::ElemDefine:
        return operation.operand(OperandIndex::ElemDefine::value);
    case Opcode::NewObject: {
        if (name.kind != ValueFlowNameKind::Property)
            return { };
        const Layout& layout = program.at(operation.layout());
        auto fields = operation.fields();
        for (uint32_t position = 0; position < fields.size(); ++position) {
            PropertyKeyID key = layout.fields[fields[position]].key;
            if (key && key.index() == name.payload)
                return operation.operand(OperandIndex::NewObject::values + position);
        }
        return { };
    }
    default:
        return { };
    }
}

// The node holds what every statement that has asked for it has written, and each of them is looked at. For a node that holds a number.
ValueFlowNumbers StoresByLayout::numbersStoredBy(const Writer& writer, ValueFlowName name) const
{
    const ValueFlowStatement& statement = writer.statement;
    if (statement.kind != ValueFlowStatementKind::Operation)
        return ValueFlowNumbers::any();
    const Program& program = m_inputs.program;
    const Function& function = program.at(statement.function);
    ValueID stored = valueStoredUnder(program, function.at(statement.site), name);
    if (!stored)
        return ValueFlowNumbers::any();
    // What was found about the positions that the value is computed from bounds it too: the parameter of a constructor has no type of its own in untyped code.
    ValueFlowNumbers found = m_inputs.numbersOfValues ? m_inputs.numbersOfValues->numbersOf(statement.function, stored) : ValueFlowNumbers::any();
    // Where a check has failed, the generic version stores what the same operations make of whatever the check has turned away.
    if (m_hasChecksNotLookedAt[statement.function.index()] || m_valuesBehindChecks[statement.function.index()].get(stored.index()))
        return found;
    ValueFlowNumbers numbers;
    numbers.add(program.typeOf(function, stored));
    return numbers.meetWith(found);
}

void StoresByLayout::indexNumbersInData()
{
    m_numbersInData.emplace();
    for (const Data& data : m_inputs.program.data) {
        forEachDataNodeInPreorder(data.root, [&](uint32_t, const DataNode& node) {
            const auto* object = node.tryAs<DataNode::Object>();
            if (!object)
                return;
            for (const DataMember& member : object->members) {
                const auto* number = member.value.tryAs<DataNode::Number>();
                if (!number)
                    continue;
                ValueFlowNumbers& numbers = m_numbersInData->add(static_cast<uint64_t>(object->layout.index()) << 32 | member.key.index(), ValueFlowNumbers { }).iterator->value;
                if (isTrackedInteger(number->value))
                    numbers.integers = numbers.integers.unionWith(Range::one(static_cast<int64_t>(number->value)));
                else
                    numbers.mayBeOtherThanIntegers = true;
            }
        });
    }
}

ValueFlowNumbers StoresByLayout::numbersInData(LayoutID layout, ValueFlowName name) const
{
    if (name.kind != ValueFlowNameKind::Property)
        return ValueFlowNumbers::any();
    auto found = m_numbersInData->find(static_cast<uint64_t>(layout.index()) << 32 | name.payload);
    return found == m_numbersInData->end() ? ValueFlowNumbers { } : found->value;
}

void StoresByLayout::noteNumbers(StoresIntoObjects& stores, ValueFlowName name, const ValueFlowNumbers& numbers)
{
    if (numbers.isEmpty())
        return;
    if (name.kind == ValueFlowNameKind::Property)
        stores.underKey.add(name.payload, StoredUnderKey { }).iterator->value.uniteNumbers(numbers);
    else if (name.kind == ValueFlowNameKind::UnknownKey)
        stores.numbersUnderUnknownKey.unite(numbers);
}

// One object stands for what the constructor of a base class allocates for the class and for every class below it, so a store to a field of one of them reaches the layouts of all.
bool StoresByLayout::someReceiverDeclares(PropertyKeyID key, const ObjectsOfSet& objects) const
{
    if (objects.holdsObjectOfEveryEscapedClass && m_keysDeclaredByEscapedClasses.get(key.index()))
        return true;
    if (objects.holdsObjectOfEveryLostClass && m_keysDeclaredByLostClasses.get(key.index()))
        return true;
    return std::ranges::any_of(objects.layouts, [&](LayoutID layout) {
        return findField(m_inputs.program, layout, key, BaseChain::Predicted).has_value();
    });
}

// The names under which findField() finds a field from one of the layouts along the predicted chain.
static BitVector collectKeysDeclaredBy(const Program& program, std::span<const LayoutID> layouts)
{
    BitVector keys(program.types.numberOfPropertyKeys());
    BitVector isVisited(program.layouts.size());
    for (LayoutID layout : layouts) {
        for (LayoutID current = layout; current && !isVisited.quickSet(current.index());) {
            const Layout& visited = program.at(current);
            for (const Field& field : visited.fields) {
                if (field.key)
                    keys.quickSet(field.key.index());
            }
            switch (visited.flow.baseState) {
            case BaseState::Layout:
                current = visited.flow.base;
                break;
            case BaseState::Unknown:
                current = predictedHeritageOf(visited);
                break;
            case BaseState::None:
            case BaseState::BuiltinClass:
                current = { };
                break;
            }
        }
    }
    return keys;
}

// The call has one callee, a built-in with a typed result, which stores into nothing that it is handed. So the write is what the compiled parser or validator does that takes the place of the call.
static bool fillsTypedResult(const ValueFlowResultInputs& inputs, const ValueFlowHeapView& heap, ValueSetMaker& maker, const Writer& writer, ValueSetID objects)
{
    const ValueFlowStatement& statement = writer.statement;
    if (statement.kind != ValueFlowStatementKind::Operation || statement.opcode != Opcode::Call || ValueSets::isComplement(objects))
        return false;
    ValueFlowObjectID object = inputs.sets().onlyMember(objects);
    const ValueFlowSite* site = object ? inputs.universe().siteOf(object) : nullptr;
    if (!site || site->origin != ValueFlowSiteOrigin::TypedResult || !inputs.universe().isSiteObjectOf(statement, object))
        return false;
    ValueSetID callees = heap.valueOf(maker, inputs.bodies().operandsOf(statement)[0]);
    return !ValueSets::isComplement(callees) && inputs.sets().onlyMember(callees);
}

static bool storesIntoThisOfConstructor(const Program&, const Writer&);

// The type that the receiver of a store has or is tested for, where it lists layouts and nothing else.
uint32_t StoresByLayout::familyDeclaredForReceiverOf(const Writer& writer) const
{
    if (!writer.layoutsOfReceiver.empty())
        return writer.familyOfReceiver.index();
    std::optional<uint8_t> receiver = receiverOf(writer.statement.opcode);
    if (!receiver)
        return UINT32_MAX;
    const Program& program = m_inputs.program;
    const Function& function = program.at(writer.statement.function);
    const Type& type = program.typeOf(function, function.at(writer.statement.site).operand(*receiver));
    if (type.hasOnlyLayouts(program))
        return type.layouts.index();
    // `this[kState] = value` in a constructor function: the receiver of a computed key is converted to any object, and the function says what its `this` is expected to be.
    if (!storesIntoThisOfConstructor(program, writer))
        return UINT32_MAX;
    const Type& typeOfThis = program.at(function.thisType.type);
    return typeOfThis.hasOnlyLayouts(program) ? typeOfThis.layouts.index() : UINT32_MAX;
}

static bool storesIntoThisOfConstructor(const Program& program, const Writer& writer)
{
    std::optional<uint8_t> receiver = receiverOf(writer.statement.opcode);
    if (writer.statement.kind != ValueFlowStatementKind::Operation || !receiver)
        return false;
    const Function& function = program.at(writer.statement.function);
    std::optional<unsigned> thisParameter = function.thisParameterIndex();
    if (!function.constructedClass || !thisParameter)
        return false;
    return definitionThroughSameValues(function, function.at(writer.statement.site).operand(*receiver)) == function.at(entryBlock).parameters[*thisParameter];
}

// `{ get name() { .. } }` makes a property that is enumerable and configurable. `Object.defineProperty(object, key, { get, enumerable: true })` makes what the literal says: an attribute that it leaves out is false.
uint8_t StoresByLayout::formsOfAccessorDefinedBy(ValueSetMaker& maker, const Writer& writer, ValueFlowName name) const
{
    switch (name.kind) {
    case ValueFlowNameKind::Getter:
    case ValueFlowNameKind::Setter:
    case ValueFlowNameKind::GetterUnderUnknownKey:
    case ValueFlowNameKind::SetterUnderUnknownKey:
        break;
    default:
        return 0;
    }
    const ValueFlowStatement& statement = writer.statement;
    if (statement.kind != ValueFlowStatementKind::Operation)
        return 0;
    if (statement.opcode == Opcode::DefineGetter || statement.opcode == Opcode::DefineSetter)
        return AccessorForms::of(true, true);
    constexpr size_t positionOfDescriptor = OperandIndex::Call::arguments + 2;
    std::span<const ValueFlowOperand> operands = m_inputs.bodies().operandsOf(statement);
    if (statement.opcode != Opcode::Call || operands.size() != positionOfDescriptor + 1)
        return AccessorForms::unknown;
    ValueSetID descriptors = std::as_const(m_heap).valueOf(maker, operands[positionOfDescriptor]);
    const ValueSets& sets = m_inputs.sets();
    if (sets.isEmpty(descriptors) || !sets.isSubset(descriptors, maker.intersect(m_inputs.universe().knownObjects(), ValueSets::complement(m_inputs.universe().builtinObjects()))))
        return AccessorForms::unknown;
    auto attribute = [&](BuiltinName member) -> std::optional<bool> {
        ValueSetID written = std::as_const(m_heap).writtenUnder(maker, descriptors, ValueFlowName::property(m_inputs.program.keyOf(member)));
        if (sets.isEmpty(written))
            return false;
        ValueFlowObjectID only = sets.onlyMember(written);
        if (only == ValueFlowUniverse::primitive(PrimitiveSet::True))
            return true;
        if (only == ValueFlowUniverse::primitive(PrimitiveSet::False))
            return false;
        return std::nullopt;
    };
    std::optional<bool> isEnumerable = attribute(BuiltinNames::enumerable);
    std::optional<bool> isConfigurable = attribute(BuiltinNames::configurable);
    if (!isEnumerable || !isConfigurable)
        return AccessorForms::unknown;
    return AccessorForms::of(*isEnumerable, *isConfigurable);
}

auto StoresByLayout::describeStore(ValueSetMaker& maker, const Writer& writer, ValueFlowName name) const -> StoreUnderName
{
    bool spellsItsKey = spellsKey(maker, writer, name);
    return {
        .statement = &writer.statement,
        .name = name,
        .spellsKey = spellsItsKey,
        .isIntoThisOfConstructor = spellsItsKey && storesIntoThisOfConstructor(m_inputs.program, writer),
        .accessorForms = formsOfAccessorDefinedBy(maker, writer, name),
        .familyDeclaredForReceiver = spellsItsKey ? familyDeclaredForReceiverOf(writer) : UINT32_MAX,
        .numbers = std::nullopt,
    };
}

auto StoresByLayout::describeWrite(ValueSetMaker& maker, StoreUnderName& store, const Writer& writer, const ValueFlowNodeKey& key, ValueSetID value) const -> WritesThroughSet
{
    ValueFlowName name = key.name();
    if (store.statement != &writer.statement || store.name != name)
        store = describeStore(maker, writer, name);
    bool holdsNumber = std::ranges::any_of(std::array { PrimitiveSet::Integer, PrimitiveSet::WholeNumber, PrimitiveSet::FractionalNumber }, [&](PrimitiveSet kind) {
        return m_inputs.sets().contains(value, ValueFlowUniverse::primitive(kind));
    });
    if (holdsNumber && !store.numbers)
        store.numbers = numbersStoredBy(writer, name);
    ValueFlowNumbers numbers = holdsNumber ? *store.numbers : ValueFlowNumbers { };
    bool fillsResult = fillsTypedResult(m_inputs, m_heap, maker, writer, key.set);
    bool isNamedByOperation = !fillsResult && store.spellsKey;
    return {
        .set = key.set.index(),
        .payload = key.payload,
        .familyOfReceiver = writer.layoutsOfReceiver.empty() ? UINT32_MAX : writer.familyOfReceiver.index(),
        .familyDeclaredForReceiver = isNamedByOperation ? store.familyDeclaredForReceiver : UINT32_MAX,
        .detail = key.detail,
        .fillsTypedResult = fillsResult,
        .createsData = !fillsResult && writer.statement.kind == ValueFlowStatementKind::Operation && writer.statement.opcode == Opcode::Data && !numbers.isEmpty(),
        .isStoredAfterCreation = !writer.isAllocation,
        .isNamedByOperation = isNamedByOperation,
        .isStoredByConstructor = isNamedByOperation && store.isIntoThisOfConstructor,
        .isWrittenByOperation = writer.statement.kind == ValueFlowStatementKind::Operation,
        .givesValueItself = writer.statement.kind == ValueFlowStatementKind::Operation && writer.statement.opcode != Opcode::Call,
        .accessorForms = store.accessorForms,
        .value = value,
        .numbers = numbers,
    };
}

auto StoresByLayout::collectWrites(ValueSetMakers& makers) const -> ParallelArray<WritesThroughSet>
{
    const ParallelLoops& loops = m_inputs.loops();
    auto isInEarlierGroup = [](const WritesThroughSet& a, const WritesThroughSet& b) {
        return a.group() < b.group();
    };
    // In whatever order the writes of a group are folded, the same record comes out: the node of the group gives the value.
    auto fold = [](WritesThroughSet& merged, const WritesThroughSet& write) {
        merged.isStoredAfterCreation |= write.isStoredAfterCreation;
        merged.isNamedByOperation |= write.isNamedByOperation;
        merged.isStoredByConstructor |= write.isStoredByConstructor;
        merged.isWrittenByOperation |= write.isWrittenByOperation;
        merged.givesValueItself |= write.givesValueItself;
        merged.accessorForms |= write.accessorForms;
        merged.numbers.unite(write.numbers);
    };
    // Many statements write through one set under one name. Each thread has one record for each group that it has met, so nobody holds a record per statement.
    struct GroupsOfWorker {
        Vector<WritesThroughSet> writes;
        HashedIndex positionOfGroup;
        StoreUnderName lastStore;
    };
    Vector<GroupsOfWorker> groupsOfWorker(loops.width());
    forEachWriteInParallel([&](unsigned worker, const Writer& writer, const ValueFlowNodeKey& key, ValueSetID value) {
        GroupsOfWorker& groups = groupsOfWorker[worker];
        WritesThroughSet write = describeWrite(makers.of(worker), groups.lastStore, writer, key, value);
        uint32_t hash = write.hashOfGroup();
        std::optional<uint32_t> position = groups.positionOfGroup.find(hash, [&](uint32_t candidate) {
            return groups.writes[candidate].group() == write.group();
        });
        if (position) {
            fold(groups.writes[*position], write);
            return;
        }
        groups.positionOfGroup.add(hash, groups.writes.size());
        groups.writes.append(write);
    });
    Vector<Vector<WritesThroughSet>> writesOfWorkers(groupsOfWorker.size(), [&](size_t worker) {
        return WTF::move(groupsOfWorker[worker].writes);
    });
    ParallelArray<WritesThroughSet> writes = ParallelArray<WritesThroughSet>::concatenationOf(loops, WTF::move(writesOfWorkers));
    forEachIndexInParallel(loops, groupsOfWorker.size(), [&](unsigned worker) {
        groupsOfWorker[worker] = { };
    });
    parallelSort(loops, writes.mutableSpan(), isInEarlierGroup);

    constexpr size_t writesPerShare = 1 << 13;
    size_t numberOfWrites = writes.size();
    unsigned numberOfShares = static_cast<unsigned>((numberOfWrites + writesPerShare - 1) / writesPerShare);
    Vector<size_t> firstWriteOfShare(numberOfShares + 1, [&](size_t share) {
        size_t first = std::min(share * writesPerShare, numberOfWrites);
        while (first && first < numberOfWrites && writes[first - 1].group() == writes[first].group())
            ++first;
        return first;
    });
    Vector<Vector<WritesThroughSet>> groupsOfShares(numberOfShares);
    forEachIndexInParallel(loops, numberOfShares, [&](unsigned share) {
        Vector<WritesThroughSet>& groups = groupsOfShares[share];
        if (firstWriteOfShare[share] >= firstWriteOfShare[share + 1])
            return;
        for (const WritesThroughSet& write : writes.span().subspan(firstWriteOfShare[share], firstWriteOfShare[share + 1] - firstWriteOfShare[share])) {
            if (groups.isEmpty() || groups.last().group() != write.group())
                groups.append(write);
            else
                fold(groups.last(), write);
        }
    });
    m_inputs.build.destroyLater(WTF::move(writes));
    return ParallelArray<WritesThroughSet>::concatenationOf(loops, WTF::move(groupsOfShares));
}

void StoresByLayout::noteWritesInto(ValueSetMaker& maker, LayoutID layout, const WritesThroughSet& writes)
{
    StoresIntoObjects& stores = m_storesOfLayout[layout.index()];
    ValueFlowName name = writes.name();
    // A parser puts a member that the layout declares into its field: a required one as it allocates the object, any other behind the allocation, if the text has it.
    if (writes.fillsTypedResult) {
        if (name.kind == ValueFlowNameKind::UnknownKey) {
            stores.isStoredUnderIndex = true;
            return;
        }
        std::optional<FieldReference> field = name.kind == ValueFlowNameKind::Property ? findField(m_inputs.program, layout, PropertyKeyID(name.payload), BaseChain::Predicted) : std::nullopt;
        bool isFilledByAllocation = field && fieldAt(m_inputs.program, *field).flags.contains(FieldFlags::IsRequired);
        note(maker, stores, name, writes.value, isFilledByAllocation, false);
        noteNumbers(stores, name, writes.numbers);
        return;
    }
    if (writes.familyOfReceiver != UINT32_MAX && !admits(m_inputs.program.at(LayoutSetID(writes.familyOfReceiver)).members.span(), layout))
        return;
    note(maker, stores, name, writes.value, !writes.isStoredAfterCreation, writes.isNamedByOperation);
    noteNumbers(stores, name, writes.createsData ? numbersInData(layout, name) : writes.numbers);
    if (writes.isStoredByConstructor && name.kind == ValueFlowNameKind::Property)
        stores.underKey.add(name.payload, StoredUnderKey { }).iterator->value.isStoredByConstructor = true;
    if (!writes.isWrittenByOperation)
        return;
    if (name.kind == ValueFlowNameKind::Property) {
        StoredUnderKey& stored = stores.underKey.add(name.payload, StoredUnderKey { }).iterator->value;
        stored.valueFromOperations = maker.unite(stored.valueFromOperations, writes.value);
        stored.isGivenValueByOperation |= writes.givesValueItself;
    }
    if (name.kind == ValueFlowNameKind::UnknownKey) {
        stores.valueFromOperationsUnderUnknownKey = maker.unite(stores.valueFromOperationsUnderUnknownKey, writes.value);
        stores.isGivenValueByOperationUnderUnknownKey |= writes.givesValueItself;
    }
    if (!writes.accessorForms)
        return;
    if (name.kind == ValueFlowNameKind::GetterUnderUnknownKey || name.kind == ValueFlowNameKind::SetterUnderUnknownKey) {
        stores.formsOfAccessorsUnderUnknownKey |= writes.accessorForms;
        if (name.kind == ValueFlowNameKind::GetterUnderUnknownKey)
            stores.gettersUnderUnknownKey = maker.unite(stores.gettersUnderUnknownKey, writes.value);
        return;
    }
    StoredUnderKey& stored = stores.underKey.add(name.payload, StoredUnderKey { }).iterator->value;
    stored.accessorForms |= writes.accessorForms;
    if (name.kind == ValueFlowNameKind::Getter)
        stored.getters = maker.unite(stored.getters, writes.value);
}

// Where the proven type of the receiver lists layouts, the object that a placeholder stands for has one of them: noteWrites() puts the write down for those.
void StoresByLayout::noteWritesThroughPlaceholder(ValueSetMaker& maker, StoresIntoObjects& stores, const WritesThroughSet& writes)
{
    if (writes.fillsTypedResult || writes.familyOfReceiver != UINT32_MAX)
        return;
    note(maker, stores, writes.name(), writes.value, false, writes.isNamedByOperation);
    noteNumbers(stores, writes.name(), writes.numbers);
}

void StoresByLayout::noteWrites()
{
    const ParallelLoops& loops = m_inputs.loops();
    uint32_t numberOfLayouts = m_inputs.program.layouts.size();
    ValueSetMakers makers(m_inputs);
    ParallelArray<WritesThroughSet> writes = collectWrites(makers);

    Vector<uint32_t> firstWriteOfSet;
    for (uint32_t index = 0; index < writes.size(); ++index) {
        if (!index || writes[index - 1].set != writes[index].set)
            firstWriteOfSet.append(index);
    }
    uint32_t numberOfSets = firstWriteOfSet.size();
    firstWriteOfSet.append(writes.size());
    auto writesThrough = [&](uint32_t position) {
        return writes.mutableSpan().subspan(firstWriteOfSet[position], firstWriteOfSet[position + 1] - firstWriteOfSet[position]);
    };

    m_keysDeclaredByEscapedClasses = collectKeysDeclaredBy(m_inputs.program, m_layoutsOfObjects.escapedClasses());
    m_keysDeclaredByLostClasses = collectKeysDeclaredBy(m_inputs.program, m_layoutsOfObjects.lostClasses());

    // A store by name whose receiver may have a layout that declares the name is a store to that field. It gives the name no room in the other layouts.
    Vector<ObjectsOfSet> objectsOfSet(numberOfSets);
    forEachIndexInParallel(loops, numberOfSets, [&](unsigned position) {
        objectsOfSet[position] = findObjectsOf(ValueSetID(writes[firstWriteOfSet[position]].set));
        for (WritesThroughSet& write : writesThrough(position)) {
            if (write.isNamedByOperation && someReceiverDeclares(PropertyKeyID(write.payload), objectsOfSet[position]))
                write.isNamedByOperation = false;
        }
    });
    if (std::ranges::any_of(writes.span(), &WritesThroughSet::createsData))
        indexNumbersInData();

    Vector<uint32_t> firstSetOfLayout;
    firstSetOfLayout.fill(0u, numberOfLayouts + 1);
    for (const ObjectsOfSet& objects : objectsOfSet) {
        for (LayoutID layout : objects.layouts)
            ++firstSetOfLayout[layout.index() + 1];
    }
    for (uint32_t index = 0; index < numberOfLayouts; ++index)
        firstSetOfLayout[index + 1] += firstSetOfLayout[index];
    Vector<uint32_t> setsOfLayouts;
    setsOfLayouts.grow(firstSetOfLayout.last());
    Vector<uint32_t> nextSetOfLayout(firstSetOfLayout.span().first(numberOfLayouts));
    for (uint32_t position = 0; position < numberOfSets; ++position) {
        for (LayoutID layout : objectsOfSet[position].layouts)
            setsOfLayouts[nextSetOfLayout[layout.index()]++] = position;
    }

    auto isSameForEveryClass = [](const WritesThroughSet& write) {
        ValueFlowNameKind kind = write.name().kind;
        bool liesUnderKey = kind == ValueFlowNameKind::Property || kind == ValueFlowNameKind::Getter || kind == ValueFlowNameKind::Setter;
        return liesUnderKey && write.familyOfReceiver == UINT32_MAX && !write.fillsTypedResult && !write.createsData;
    };
    struct WritesIntoEveryClass {
        StoresIntoObjects& stores;
        bool ObjectsOfSet::* isHeld;
        const BitVector& classes;
        Vector<uint32_t> notedOnce { };
        Vector<uint32_t> notedForEachClass { };
    };
    WritesIntoEveryClass writesIntoEveryClass[] {
        { m_storesIntoEveryEscapedClass, &ObjectsOfSet::holdsObjectOfEveryEscapedClass, m_isEscapedClass },
        { m_storesIntoEveryLostClass, &ObjectsOfSet::holdsObjectOfEveryLostClass, m_isLostClass },
    };
    for (uint32_t position = 0; position < numberOfSets; ++position) {
        for (WritesIntoEveryClass& list : writesIntoEveryClass) {
            if (!(objectsOfSet[position].*list.isHeld))
                continue;
            for (uint32_t index = firstWriteOfSet[position]; index < firstWriteOfSet[position + 1]; ++index)
                (isSameForEveryClass(writes[index]) ? list.notedOnce : list.notedForEachClass).append(index);
        }
    }

    Vector<Vector<uint32_t>> writesThroughPlaceholdersInto(numberOfLayouts);
    for (uint32_t position = 0; position < numberOfSets; ++position) {
        const ObjectsOfSet& objects = objectsOfSet[position];
        if (!objects.holdsUnknownObject && !objects.holdsUntrackedObject && !objects.holdsAlmostEverything)
            continue;
        for (uint32_t index = firstWriteOfSet[position]; index < firstWriteOfSet[position + 1]; ++index) {
            if (writes[index].fillsTypedResult || writes[index].familyOfReceiver == UINT32_MAX)
                continue;
            for (LayoutID layout : layoutsAdmittedBy(LayoutSetID(writes[index].familyOfReceiver))) {
                bool isReached = objects.holdsAlmostEverything || (objects.holdsUnknownObject && m_hasEscapedObject.quickGet(layout.index())) || (objects.holdsUntrackedObject && m_hasLostObject.quickGet(layout.index()));
                if (isReached)
                    writesThroughPlaceholdersInto[layout.index()].append(index);
            }
        }
    }

    struct Placeholder {
        StoresIntoObjects& stores;
        bool ObjectsOfSet::* isHeld;
    };
    Placeholder placeholders[] {
        { m_storesThroughUnknownObject, &ObjectsOfSet::holdsUnknownObject },
        { m_storesThroughUntrackedObject, &ObjectsOfSet::holdsUntrackedObject },
        { m_storesIntoEveryObject, &ObjectsOfSet::holdsAlmostEverything },
    };
    constexpr size_t numberOfSharedTables = std::size(placeholders) + std::size(writesIntoEveryClass);
    forEachIndexInParallel(loops, numberOfSharedTables + numberOfLayouts, [&](unsigned index, unsigned worker) {
        ValueSetMaker& maker = makers.of(worker);
        if (index < std::size(placeholders)) {
            for (uint32_t position = 0; position < numberOfSets; ++position) {
                if (!(objectsOfSet[position].*placeholders[index].isHeld))
                    continue;
                for (const WritesThroughSet& write : writesThrough(position))
                    noteWritesThroughPlaceholder(maker, placeholders[index].stores, write);
            }
            return;
        }
        if (index < numberOfSharedTables) {
            WritesIntoEveryClass& list = writesIntoEveryClass[index - std::size(placeholders)];
            for (uint32_t position : list.notedOnce) {
                const WritesThroughSet& write = writes[position];
                note(maker, list.stores, write.name(), write.value, !write.isStoredAfterCreation, false);
                noteNumbers(list.stores, write.name(), write.numbers);
            }
            return;
        }
        uint32_t layout = index - numberOfSharedTables;
        auto noteWritesThroughSets = [&](std::span<const uint32_t> positions) {
            for (uint32_t position : positions) {
                for (const WritesThroughSet& write : writesThrough(position))
                    noteWritesInto(maker, LayoutID(layout), write);
            }
        };
        noteWritesThroughSets(setsOfLayouts.subspan(firstSetOfLayout[layout], firstSetOfLayout[layout + 1] - firstSetOfLayout[layout]));
        for (const WritesIntoEveryClass& list : writesIntoEveryClass) {
            if (!list.classes.quickGet(layout))
                continue;
            for (uint32_t position : list.notedForEachClass)
                noteWritesInto(maker, LayoutID(layout), writes[position]);
        }
        for (uint32_t position : writesThroughPlaceholdersInto[layout]) {
            note(maker, m_storesOfLayout[layout], writes[position].name(), writes[position].value, false, false);
            noteNumbers(m_storesOfLayout[layout], writes[position].name(), writes[position].numbers);
        }
    });

    for (uint32_t position = 0; position < numberOfSets; ++position) {
        const ObjectsOfSet& objects = objectsOfSet[position];
        uint8_t held = objects.holdsUnknownObject | objects.holdsUntrackedObject << 1 | objects.holdsAlmostEverything << 2;
        if (!held)
            continue;
        for (const WritesThroughSet& write : writesThrough(position)) {
            if (write.isNamedByOperation && write.familyDeclaredForReceiver != UINT32_MAX)
                m_namesStoredThroughDeclaredReceivers.append({ write.familyDeclaredForReceiver, held, write.isStoredByConstructor, PropertyKeyID(write.payload) });
        }
    }

    RELEASE_ASSERT(m_setsLookedAt.isEmpty());
    m_objectsOfSetsOfWrites = WTF::move(objectsOfSet);
    m_setsLookedAt.grow(numberOfSets);
    m_positionOfSetLookedAt.reserve(loops, numberOfSets);
    forEachIndexInParallel(loops, numberOfSets, [&](unsigned position) {
        uint32_t set = writes[firstWriteOfSet[position]].set;
        m_setsLookedAt[position] = set;
        m_positionOfSetLookedAt.addConcurrently(IntHash<uint32_t>::hash(set), position);
    });
    m_positionOfSetLookedAt.didAddConcurrently(numberOfSets);
    m_inputs.build.destroyLater(WTF::move(writes));
    m_inputs.build.destroyLater(WTF::move(writesThroughPlaceholdersInto));
}

// The layouts that the type of a receiver admits and that have an object which the placeholders may stand for.
Vector<LayoutID> StoresByLayout::receiversNamedBy(LayoutSetID family, uint8_t placeholders)
{
    Vector<LayoutID> receivers;
    for (LayoutID layout : layoutsAdmittedBy(family)) {
        bool mayBeReceiver = (placeholders & 1 && m_hasEscapedObject.quickGet(layout.index())) || (placeholders & 2 && m_hasLostObject.quickGet(layout.index()))
            || (placeholders & 4 && !m_inputs.sets().isEmpty(m_layoutsOfObjects.objectsWith(layout)));
        if (mayBeReceiver)
            receivers.append(layout);
    }
    return receivers;
}

// How many operations of reached functions name one of the keys on a receiver whose type admits the layout. A type that admits more layouts than `mostLayouts` says nothing about any of them.
auto StoresByLayout::countAccessesThroughTypesOfFewLayouts(const BitVector& keys, uint64_t mostLayouts) -> AccessesByKeyAndLayout
{
    struct Access {
        PropertyKeyID key;
        LayoutSetID familyOfReceiver;
    };

    const Program& program = m_inputs.program;
    Vector<Access> accesses = collectInParallel<Access>(m_inputs.loops(), program.functions.size(), [&](unsigned index, Vector<Access>& found) {
        if (!m_inputs.isReached(FunctionID(index)))
            return;
        const Function& function = program.functions[index];
        for (const Value& value : function.values) {
            if (!Value::opcodesWithKey.contains(value.opcode) || !value.key() || !keys.quickGet(value.key().index()))
                continue;
            std::optional<uint8_t> receiver = receiverOf(value.opcode);
            if (!receiver)
                continue;
            const Type& type = program.typeOf(function, value.operand(*receiver));
            if (type.hasOnlyLayouts(program))
                found.append({ value.key(), type.layouts });
        }
    });
    AccessesByKeyAndLayout counts;
    for (const Access& access : accesses) {
        const Vector<LayoutID>& admitted = layoutsAdmittedBy(access.familyOfReceiver);
        if (admitted.size() > mostLayouts)
            continue;
        for (LayoutID layout : admitted)
            ++counts.add(static_cast<uint64_t>(access.key.index()) << 32 | layout.index(), 0).iterator->value;
    }
    return counts;
}

// A placeholder names no layout, but the type of the receiver does: the object is expected to have one of the layouts that it lists.
void StoresByLayout::noteNamesStoredThroughDeclaredReceivers()
{
    Vector<NameStoredThroughDeclaredReceiver> names = std::exchange(m_namesStoredThroughDeclaredReceivers, { });
    if (names.isEmpty())
        return;
    AOT::sort(names, { }, &NameStoredThroughDeclaredReceiver::order);
    removeRepeatedElements(names, [](const NameStoredThroughDeclaredReceiver& a, const NameStoredThroughDeclaredReceiver& b) {
        return a.order() == b.order();
    });

    const Program& program = m_inputs.program;
    uint64_t mostLayoutsGivenRoom = m_inputs.build.limit(LimitID::MaxValueFlowMembersToldApart);
    auto forEachNameWithReceivers = [&](NOESCAPE const Invocable<void(const NameStoredThroughDeclaredReceiver&, const Vector<LayoutID>&, bool areTooMany)> auto& functor) {
        // Whoever constructs with a function that has escaped makes objects of its layout that nobody has seen escape: the placeholder stands for them too.
        auto placeholdersOf = [](const NameStoredThroughDeclaredReceiver& name) -> uint8_t {
            constexpr uint8_t everyPlaceholder = 7;
            return name.isStoredByConstructor ? everyPlaceholder : name.placeholders;
        };
        Vector<LayoutID> receivers;
        for (size_t index = 0; index < names.size(); ++index) {
            const NameStoredThroughDeclaredReceiver& name = names[index];
            if (!index || names[index - 1].family != name.family || placeholdersOf(names[index - 1]) != placeholdersOf(name))
                receivers = receiversNamedBy(LayoutSetID(name.family), placeholdersOf(name));
            functor(name, receivers, !name.isStoredByConstructor && receivers.size() > mostLayoutsGivenRoom);
        }
    };
    auto hasNameAnyway = [&](LayoutID layout, PropertyKeyID key) {
        const auto& underKey = m_storesOfLayout[layout.index()].underKey;
        auto found = underKey.find(key.index());
        return (found != underKey.end() && found->value.isNamedByOperation) || findField(program, layout, key, BaseChain::Predicted);
    };
    auto noteName = [&](LayoutID layout, const NameStoredThroughDeclaredReceiver& name) {
        StoredUnderKey& stored = m_storesOfLayout[layout.index()].underKey.add(name.key.index(), StoredUnderKey { }).iterator->value;
        stored.isStoredAfterCreation = true;
        stored.isNamedByOperation = true;
        stored.isStoredByConstructor |= name.isStoredByConstructor;
    };

    BitVector keysWithTooManyReceivers(program.types.numberOfPropertyKeys());
    forEachNameWithReceivers([&](const NameStoredThroughDeclaredReceiver& name, const Vector<LayoutID>&, bool areTooMany) {
        if (areTooMany)
            keysWithTooManyReceivers.quickSet(name.key.index());
    });
    AccessesByKeyAndLayout counts;
    if (!keysWithTooManyReceivers.isEmpty())
        counts = countAccessesThroughTypesOfFewLayouts(keysWithTooManyReceivers, mostLayoutsGivenRoom);

    forEachNameWithReceivers([&](const NameStoredThroughDeclaredReceiver& name, const Vector<LayoutID>& receivers, bool areTooMany) {
        PropertyKeyID key = name.key;
        if (!areTooMany) {
            for (LayoutID layout : receivers)
                noteName(layout, name);
            return;
        }
        auto countOf = [&](LayoutID layout) {
            return counts.get(static_cast<uint64_t>(key.index()) << 32 | layout.index());
        };
        Vector<LayoutID> layoutsGivenRoom;
        LayoutID firstLayoutWithoutRoom;
        for (LayoutID layout : receivers) {
            if (hasNameAnyway(layout, key))
                noteName(layout, name);
            else if (countOf(layout))
                layoutsGivenRoom.append(layout);
            else if (!firstLayoutWithoutRoom)
                firstLayoutWithoutRoom = layout;
        }
        if (layoutsGivenRoom.size() > mostLayoutsGivenRoom) {
            AOT::sort(layoutsGivenRoom, [&](LayoutID a, LayoutID b) {
                return std::pair { countOf(b), a } < std::pair { countOf(a), b };
            });
            LayoutID firstLayoutPastLimit = *std::ranges::min_element(layoutsGivenRoom.span().subspan(mostLayoutsGivenRoom));
            if (!firstLayoutWithoutRoom || firstLayoutPastLimit < firstLayoutWithoutRoom)
                firstLayoutWithoutRoom = firstLayoutPastLimit;
            layoutsGivenRoom.shrink(mostLayoutsGivenRoom);
        }
        if (firstLayoutWithoutRoom)
            m_inputs.build.gaveUp(LimitID::MaxValueFlowMembersToldApart, Subject { .layout = firstLayoutWithoutRoom });
        for (LayoutID layout : layoutsGivenRoom)
            noteName(layout, name);
    });
}

// Whatever such code takes has escaped, so what it stores it stores through the unknown object. What it allocates is in nobody's set, and it fills the fields with values that nobody has looked at.
void StoresByLayout::noteCodeWithoutSolve(const Function& function)
{
    const Program& program = m_inputs.program;
    for (const Value& value : function.values) {
        if (value.opcode == Opcode::NewObject) {
            const Layout& layout = program.at(value.layout());
            if (layout.isEnvironment())
                continue;
            m_hasObjectOfCodeWithoutSolve.quickSet(value.layout().index());
            for (uint32_t field : value.fields()) {
                PropertyKeyID key = layout.fields[field].key;
                note(m_heap.maker(), m_storesOfLayout[value.layout().index()], key ? ValueFlowName::property(key) : ValueFlowName::of(ValueFlowNameKind::UnknownKey), m_unknownValue, true, false);
                noteNumbers(m_storesOfLayout[value.layout().index()], key ? ValueFlowName::property(key) : ValueFlowName::of(ValueFlowNameKind::UnknownKey), ValueFlowNumbers::any());
            }
            continue;
        }
        bool storesByName = accessOf(value.opcode) == Access::Store || accessOf(value.opcode) == Access::Define;
        bool storesInOtherWays = false;
        for (uint32_t index = 0; index < value.numberOfOperands() && !storesByName; ++index)
            storesInOtherWays |= operandUsesOf(value.opcode, index).containsAny({ OperandUse::StoresByKey, OperandUse::AssignsFrom, OperandUse::DefinesName, OperandUse::TakesDescriptor });
        if (storesByName && Value::opcodesWithKey.contains(value.opcode) && value.key()) {
            note(m_heap.maker(), m_storesThroughUnknownObject, ValueFlowName::property(value.key()), m_unknownValue, false, true);
            noteNumbers(m_storesThroughUnknownObject, ValueFlowName::property(value.key()), ValueFlowNumbers::any());
        } else if (storesInOtherWays) {
            note(m_heap.maker(), m_storesThroughUnknownObject, ValueFlowName::of(ValueFlowNameKind::UnknownKey), m_unknownValue, false, false);
            noteNumbers(m_storesThroughUnknownObject, ValueFlowName::of(ValueFlowNameKind::UnknownKey), ValueFlowNumbers::any());
            note(m_heap.maker(), m_storesThroughUnknownObject, ValueFlowName::of(ValueFlowNameKind::GetterUnderUnknownKey), m_unknownValue, false, false);
        }
    }
}

void StoresByLayout::forEachSourceOf(LayoutID layout, NOESCAPE const Invocable<void(const StoresIntoObjects&)> auto& functor) const
{
    functor(m_storesOfLayout[layout.index()]);
    functor(m_storesIntoEveryObject);
    if (m_isEscapedClass.quickGet(layout.index()))
        functor(m_storesIntoEveryEscapedClass);
    if (m_isLostClass.quickGet(layout.index()))
        functor(m_storesIntoEveryLostClass);
    if (m_hasEscapedObject.quickGet(layout.index()))
        functor(m_storesThroughUnknownObject);
    if (m_hasLostObject.quickGet(layout.index()))
        functor(m_storesThroughUntrackedObject);
}

ValueFlowNumbers StoresByLayout::numbersStoredUnder(LayoutID layout, PropertyKeyID key) const
{
    ValueFlowNumbers numbers;
    forEachSourceOf(layout, [&](const StoresIntoObjects& stores) {
        auto found = stores.underKey.find(key.index());
        if (found != stores.underKey.end())
            numbers.unite(found->value.numbers());
        numbers.unite(stores.numbersUnderUnknownKey);
    });
    return numbers;
}

ValueSetID StoresByLayout::storedUnder(ValueSetMaker& maker, LayoutID layout, PropertyKeyID key) const
{
    ValueSetID stored = ValueSets::nothing;
    forEachSourceOf(layout, [&](const StoresIntoObjects& stores) {
        auto found = stores.underKey.find(key.index());
        if (found != stores.underKey.end())
            stored = maker.unite(stored, found->value.value);
        stored = maker.unite(stored, stores.underUnknownKey);
    });
    return stored;
}

bool StoresByLayout::isStoredAfterCreation(LayoutID layout, PropertyKeyID key) const
{
    bool isStored = false;
    forEachSourceOf(layout, [&](const StoresIntoObjects& stores) {
        auto found = stores.underKey.find(key.index());
        isStored |= (found != stores.underKey.end() && found->value.isStoredAfterCreation) || stores.underUnknownKey != ValueSets::nothing || stores.getsAccessorUnderUnknownKey || stores.getsCopiedProperties;
    });
    return isStored;
}

bool StoresByLayout::getsCopiedProperties(LayoutID layout) const
{
    bool getsThem = false;
    forEachSourceOf(layout, [&](const StoresIntoObjects& stores) {
        getsThem |= stores.getsCopiedProperties;
    });
    return getsThem;
}

bool StoresByLayout::getsNameNobodyLists(LayoutID layout) const
{
    bool getsOne = false;
    forEachSourceOf(layout, [&](const StoresIntoObjects& stores) {
        getsOne |= stores.getsNameNobodyLists();
    });
    return getsOne;
}

static void uniteSortedKeys(Vector<PropertyKeyID>& keys, const Vector<PropertyKeyID>& others)
{
    if (others.isEmpty())
        return;
    Vector<PropertyKeyID> united;
    united.reserveInitialCapacity(keys.size() + others.size());
    size_t index = 0;
    for (PropertyKeyID other : others) {
        while (index < keys.size() && keys[index] < other)
            united.append(keys[index++]);
        if (index < keys.size() && keys[index] == other)
            ++index;
        united.append(other);
    }
    united.append(keys.subspan(index));
    keys = WTF::move(united);
}

Vector<PropertyKeyID> StoresByLayout::keysStoredInto(LayoutID layout) const
{
    Vector<PropertyKeyID> keys = sortedKeysOf(m_storesOfLayout[layout.index()]);
    uniteSortedKeys(keys, m_keysIntoEveryObject);
    if (m_isEscapedClass.quickGet(layout.index()))
        uniteSortedKeys(keys, m_keysIntoEveryEscapedClass);
    if (m_isLostClass.quickGet(layout.index()))
        uniteSortedKeys(keys, m_keysIntoEveryLostClass);
    if (m_hasEscapedObject.quickGet(layout.index()))
        uniteSortedKeys(keys, m_keysThroughUnknownObject);
    if (m_hasLostObject.quickGet(layout.index()))
        uniteSortedKeys(keys, m_keysThroughUntrackedObject);
    return keys;
}

Vector<PropertyKeyID> StoresByLayout::keysStoredIntoKnownObjects(LayoutID layout) const
{
    return sortedKeysOf(m_storesOfLayout[layout.index()]);
}

Vector<PropertyKeyID> StoresByLayout::keysNamedByStoresIntoKnownObjects(LayoutID layout) const
{
    Vector<PropertyKeyID> keys;
    for (const auto& [key, stored] : m_storesOfLayout[layout.index()].underKey) {
        if (stored.isNamedByOperation)
            keys.append(PropertyKeyID(key));
    }
    AOT::sort(keys);
    return keys;
}

bool StoresByLayout::isStoredByConstructor(LayoutID layout, PropertyKeyID key) const
{
    const auto& underKey = m_storesOfLayout[layout.index()].underKey;
    auto found = underKey.find(key.index());
    return found != underKey.end() && found->value.isStoredByConstructor;
}

bool StoresByLayout::mayGetAccessorUnder(LayoutID layout, PropertyKeyID key) const
{
    bool mayGetOne = false;
    forEachSourceOf(layout, [&](const StoresIntoObjects& stores) {
        auto found = stores.underKey.find(key.index());
        mayGetOne |= found != stores.underKey.end() && found->value.mayBeAccessor;
    });
    return mayGetOne;
}

// `for (const key of names) Object.defineProperty(object, key, { get: getterFor(key) })`: whatever the names are, each of them is an accessor of that form.
FlowAccessorField StoresByLayout::accessorDefinedUnder(ValueSetMaker& maker, LayoutID layout, PropertyKeyID key) const
{
    const StoresIntoObjects& stores = m_storesOfLayout[layout.index()];
    const ValueSets& sets = m_inputs.sets();
    // What is defined under a key nobody knows speaks for the key only in an object that is born empty: see getsAccessorsUnderKeysNobodyKnows(). It speaks against the key in any.
    ValueSetID getters = m_inputs.program.at(layout).fields.isEmpty() ? stores.gettersUnderUnknownKey : ValueSets::nothing;
    ValueSetID values = stores.valueFromOperationsUnderUnknownKey;
    uint8_t forms = stores.formsOfAccessorsUnderUnknownKey;
    bool isGivenValue = stores.isGivenValueByOperationUnderUnknownKey;
    if (auto found = stores.underKey.find(key.index()); found != stores.underKey.end()) {
        getters = maker.unite(getters, found->value.getters);
        values = maker.unite(values, found->value.valueFromOperations);
        forms |= found->value.accessorForms;
        isGivenValue |= found->value.isGivenValueByOperation;
    }
    // A descriptor without `value` defines none.
    if (isGivenValue || getters == ValueSets::nothing || !sets.isSubset(values, m_inputs.universe().undefinedValue()))
        return { };
    std::optional<OptionSet<FieldForm>> form = AccessorForms::onlyFormIn(forms);
    if (!form)
        return { };
    // From an heir the run time finds an accessor faster among the extra properties of the object above than in a slot of it.
    if (sets.meet(m_layoutsOfObjects.objectsWith(layout), m_objectsPutAboveOthers))
        return { };

    // A helper that defines the getters of every caller under one roof has hundreds: there the slot takes any function.
    FlowAccessorField accessor { *form, { } };
    if (ValueSets::isComplement(getters) || sets.size(getters) > m_inputs.build.limit(LimitID::MaxValueFlowMembersToldApart))
        return accessor;
    // The closures among them are what to expect.
    sets.forEachMember(getters, [&](ValueFlowObjectID getter) {
        if (FunctionID function = m_inputs.universe().closureFunction(getter))
            accessor.getters.append(function);
    });
    return accessor;
}

bool StoresByLayout::isReceiverOfReads(LayoutID layout, PropertyKeyID key) const
{
    auto found = m_receiversOfReads.find(key.index());
    if (found == m_receiversOfReads.end())
        return false;
    ValueSetID objects = m_layoutsOfObjects.objectsWith(layout);
    return std::ranges::any_of(found->value, [&](ValueSetID receivers) {
        return m_inputs.sets().meet(objects, receivers);
    });
}

void StoresByLayout::setReceiversOfReads(ListsOfSetsByNumber&& receivers)
{
    struct KeyReadFromLayout {
        LayoutID layout;
        PropertyKeyID key;
    };

    m_receiversOfReads = WTF::move(receivers);
    // The objects that a link lists. What code that nobody sees may hang above an object that has escaped is a placeholder there.
    UnionOfSets above;
    m_heap.forEachNodeWrittenUnder(prototypeLinkName, [&](ValueSetID, ValueSetID value) {
        above.add(value);
    });
    m_objectsPutAboveOthers = above.takeAsListed(m_heap.maker());

    // Each list of receivers is walked once, for the layouts that ask: never the names once for each layout.
    const ValueSets& sets = m_inputs.sets();
    ValueSetMaker& maker = m_heap.maker();
    uint32_t numberOfLayouts = m_inputs.program.layouts.size();
    m_keysReadFrom = Vector<Vector<PropertyKeyID>>(numberOfLayouts);
    BitVector asks(numberOfLayouts);
    Vector<ValueSetID> objectsOfLayoutsThatAsk;
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        if (!getsAccessorsUnderKeysNobodyKnows(LayoutID(index)))
            continue;
        asks.quickSet(index);
        objectsOfLayoutsThatAsk.append(m_layoutsOfObjects.objectsWith(LayoutID(index)));
    }
    if (objectsOfLayoutsThatAsk.isEmpty())
        return;
    ValueSetID objects = maker.uniteAll(objectsOfLayoutsThatAsk.span());
    const ParallelLoops& loops = m_inputs.loops();
    Vector<const ListsOfSetsByNumber::KeyValuePairType*> receiversUnderKeys;
    receiversUnderKeys.reserveInitialCapacity(m_receiversOfReads.size());
    for (const auto& receiversUnderKey : m_receiversOfReads)
        receiversUnderKeys.append(&receiversUnderKey);
    ValueSetMakers makers(m_inputs);
    Vector<Vector<KeyReadFromLayout>> keysFoundBy(loops.width());
    forEachIndexInParallel(loops, receiversUnderKeys.size(), [&](unsigned index, unsigned worker) {
        for (ValueSetID listed : receiversUnderKeys[index]->value) {
            if (!sets.meet(listed, objects))
                continue;
            m_layoutsOfObjects.forEachLayoutOfMembers(makers.of(worker).intersect(listed, objects), [&](LayoutID layout) {
                if (asks.quickGet(layout.index()))
                    keysFoundBy[worker].append({ layout, PropertyKeyID(receiversUnderKeys[index]->key) });
            });
        }
    });
    for (const Vector<KeyReadFromLayout>& keysFound : keysFoundBy) {
        for (const KeyReadFromLayout& found : keysFound)
            m_keysReadFrom[found.layout.index()].append(found.key);
    }
    for (Vector<PropertyKeyID>& keys : m_keysReadFrom) {
        AOT::sort(keys);
        removeRepeatedElements(keys);
    }
}

bool StoresByLayout::getsAccessorsUnderKeysNobodyKnows(LayoutID layout) const
{
    // The guess is for an object that is born empty and filled by a loop.
    if (!m_inputs.program.at(layout).fields.isEmpty())
        return false;
    // What is known under a key comes on top of this, so what fails here fails under every key.
    const StoresIntoObjects& stores = m_storesOfLayout[layout.index()];
    return stores.gettersUnderUnknownKey != ValueSets::nothing && !stores.isGivenValueByOperationUnderUnknownKey && m_inputs.sets().isSubset(stores.valueFromOperationsUnderUnknownKey, m_inputs.universe().undefinedValue())
        && AccessorForms::onlyFormIn(stores.formsOfAccessorsUnderUnknownKey) && !m_inputs.sets().meet(m_layoutsOfObjects.objectsWith(layout), m_objectsPutAboveOthers);
}

Vector<PropertyKeyID> StoresByLayout::keysOfAccessorsDefinedIn(ValueSetMaker& maker, LayoutID layout) const
{
    Vector<PropertyKeyID> keys;
    for (const auto& [key, stored] : m_storesOfLayout[layout.index()].underKey) {
        if (stored.getters != ValueSets::nothing && accessorDefinedUnder(maker, layout, PropertyKeyID(key)).isAccessor())
            keys.append(PropertyKeyID(key));
    }
    AOT::sort(keys);
    return keys;
}

namespace {

// Who looks at the objects in ways that no mark records.
class UsesOfObjects {
public:
    UsesOfObjects(const ValueFlowResultInputs&, const ValueFlowHeapView&);

    // Somebody may list the keys of one of the objects.
    bool mayBeEnumerated(ValueSetID objects) const { return m_inputs.sets().meet(objects, m_enumerated); }
    ValueSetID objectsThatMayBeEnumerated() const { return m_enumerated; }
    // Somebody may read what lies under the key in one of the objects, or find out whether anything does. A read that mayBeCalledUnder() answers for does not count.
    bool mayBeObservedUnder(ValueSetID objects, PropertyKeyID) const;
    // An operation of emitted code reads by name what lies under the key in one of the objects, which is among its receivers, and does nothing with it but call it.
    bool mayBeCalledUnder(ValueSetID objects, PropertyKeyID key) const { return m_inputs.sets().meet(objects, setAt(m_calledUnderKey, key.index())); }
    // The same of an operation that does nothing with what it reads but read and store properties of it by name. mayBeObservedUnder() does not count that read either.
    bool mayBeAccessedUnder(ValueSetID objects, PropertyKeyID key) const { return m_inputs.sets().meet(objects, setAt(m_accessedUnderKey, key.index())); }
    // By key, the objects that a read by that name lists among its receivers.
    ListsOfSetsByNumber takeReceiversOfReads() { return std::exchange(m_receiversOfReads, { }); }

private:
    // Who has asked for a node that reads.
    enum class Reader : uint8_t {
        TakesValue = 1 << 0,
        OnlyCalls = 1 << 1,
        OnlyAccesses = 1 << 2,
    };

    // What one thread of a loop has noted. All of it is united in the end, so it makes no difference which thread notes a part, and when.
    struct Notes {
        UnionOfSets enumerated;
        UnionOfSets observedUnderAnyKey;
        PartsByNumber observedUnderKey;
        // The objects at which a test starts that goes on to what lies above them.
        PartsByNumber startsOfTestsUnderKey;
        PartsByNumber calledUnderKey;
        PartsByNumber accessedUnderKey;
        PartsByNumber receiversOfReads;
    };

    // A thread of a loop: where it notes, and what it makes sets with.
    struct Noter {
        Notes& notes;
        ValueSetMaker& maker;
    };

    // `worker` is what forEachIndexInParallel() hands to the body of the loop.
    Noter noterOf(ValueSetMakers& makers, unsigned worker) { return { m_notes[worker], makers.of(worker) }; }
    UnionOfSets takeUnion(UnionOfSets Notes::*);
    Vector<PartOfNumber> takeParts(PartsByNumber Notes::*);
    SetsByNumber takeExpanded(ValueSetMakers&, Vector<PartOfNumber>&&);
    void noteReads(ValueSetMakers&);
    void noteRead(Noter, const ValueFlowNodeKey&, Reader, ValueSetID startOfLookup);
    void noteOperations(ValueSetMakers&);
    void noteOperationsWithoutStatement();
    void noteBehaviors();
    void noteObjectsOfRuntimeModules();
    void noteApplication(Noter, const ValueFlowBuiltinApplication&);
    void noteRefusedOperands(Noter, const ValueFlowBuiltinApplication&);

    struct NotesOfBuiltins {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(NotesOfBuiltins);

        struct TestUnderName {
            BuiltinOperand object;
            uint32_t key;
            bool looksAtOwnPropertyAlone;

            friend bool operator==(const TestUnderName&, const TestUnderName&) = default;
        };
        struct TestUnderOperand {
            BuiltinOperand object;
            BuiltinOperand key;
            bool looksAtOwnPropertyAlone;

            friend bool operator==(const TestUnderOperand&, const TestUnderOperand&) = default;
        };
        struct RefusedOperand {
            BuiltinOperand operand;
            ValueSetID taken;
        };

        bool areToldApart { false };
        bool enumeratesAnything { false };
        Vector<BuiltinOperand, 4> enumerated;
        Vector<BuiltinOperand, 4> observedUnderAnyKey;
        Vector<TestUnderName, 4> testsUnderNames;
        Vector<TestUnderOperand, 2> testsUnderOperands;
        Vector<RefusedOperand, 4> refusedOperands;
    };
    class NotesOfSetsOfBuiltins {
    public:
        const NotesOfBuiltins& ensure(uint64_t question, NOESCAPE const Invocable<std::unique_ptr<NotesOfBuiltins>()> auto& find)
        {
            Part& part = m_parts[(question * 0x9e3779b97f4a7c15ull) >> 60];
            {
                Locker locker { part.lock };
                auto found = part.notes.find(question);
                if (found != part.notes.end())
                    return *found->value;
            }
            std::unique_ptr<NotesOfBuiltins> notes = find();
            Locker locker { part.lock };
            return *part.notes.add(question, WTF::move(notes)).iterator->value;
        }

    private:
        struct alignas(64) Part {
            Lock lock;
            HashMap<uint64_t, std::unique_ptr<NotesOfBuiltins>, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> notes WTF_GUARDED_BY_LOCK(lock);
        };

        std::array<Part, 16> m_parts;
    };
    std::unique_ptr<NotesOfBuiltins> findNotesOf(ValueSetMaker&, ValueSetID builtinFunctions, BuiltinCallMode) const;
    void noteApplicationOfAll(Noter, const ValueFlowBuiltinApplication&, ValueSetID builtinCallees);
    void noteTestUnderKeys(Noter, const ValueFlowBuiltinApplication&, BuiltinOperand object, BuiltinOperand key, bool looksAtOwnPropertyAlone);
    void findHostFunctionsThatProgramHasStored(ValueSetMakers&);
    void noteParameter(Noter, const ValueFlowBuiltinApplication&, const BuiltinParameter&);
    void noteParameterWithoutAction(Noter, const ValueFlowBuiltinApplication&, const BuiltinOverload&, const BuiltinParameter&);
    void noteTest(Noter, const ValueFlowBuiltinApplication&, const BuiltinOverload&, const BuiltinPropertyAction&);
    void goOnToPrototypes(ValueSetMakers&);
    void findObjectsObservedUnderKeys(ValueSetMakers&);
    void listReceiversOfReads();

    const ValueFlowResultInputs& m_inputs;
    const ValueFlowHeapView& m_heap;
    ValueSetID m_enumerated { ValueSets::nothing };
    ValueSetID m_observedUnderAnyKey { ValueSets::nothing };
    SetsByNumber m_observedUnderKey;
    SetsByNumber m_calledUnderKey;
    SetsByNumber m_accessedUnderKey;
    ListsOfSetsByNumber m_receiversOfReads;
    // By worker. Empty once the unions are made.
    Vector<Notes> m_notes;
    ValueFlowAnswersAboutSets m_foundAboveReceivers;
    NotesOfSetsOfBuiltins m_notesOfSetsOfBuiltins;
    BitVector m_builtinsThatLookAtObjects;
    // By the key under which some call finds its callee: the functions of the host that refuse a receiver and that the program has stored under the key somewhere. No entry where there is none.
    SetsByNumber m_hostFunctionsStoredUnderKey;
};

} // anonymous namespace

static bool looksAtObjects(const BuiltinOverload& overload)
{
    auto listsKeys = [](const BuiltinParameter& parameter) {
        return parameter.uses != OperandUses::all() && parameter.protocols.containsAny(protocolsListingKeys);
    };
    if ((overload.receiver && listsKeys(*overload.receiver)) || std::ranges::any_of(overload.parameters, listsKeys))
        return true;
    return std::ranges::any_of(overload.flow.propertyActions, [](const BuiltinPropertyAction& action) {
        switch (action.kind) {
        case BuiltinPropertyActionKind::TestsByKey:
        case BuiltinPropertyActionKind::ReadsGetter:
        case BuiltinPropertyActionKind::ReadsSetter:
        case BuiltinPropertyActionKind::AssignsFrom:
        case BuiltinPropertyActionKind::DefinesFrom:
        case BuiltinPropertyActionKind::TakesDescriptorMap:
            return true;
        default:
            return false;
        }
    });
}

static BitVector findBuiltinsThatLookAtObjects(const BuiltinTable& builtins)
{
    BitVector builtinsThatLook(builtins.builtinCount());
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        if (builtins.at(BuiltinID(index)).origin == BuiltinOrigin::Host || std::ranges::any_of(builtins.overloadsOf(BuiltinID(index)), looksAtObjects))
            builtinsThatLook.quickSet(index);
    }
    return builtinsThatLook;
}

UsesOfObjects::UsesOfObjects(const ValueFlowResultInputs& inputs, const ValueFlowHeapView& heap)
    : m_inputs(inputs)
    , m_heap(heap)
    , m_notes(inputs.loops().width())
    , m_builtinsThatLookAtObjects(findBuiltinsThatLookAtObjects(inputs.program.builtins()))
{
    PhaseTimes times(inputs.build, PassID::AnalyzeFlow);
    ValueSetMakers makers(inputs);
    noteReads(makers);
    times.didFinish("uses of the heap: reads"_s);
    noteOperations(makers);
    noteOperationsWithoutStatement();
    noteBehaviors();
    noteObjectsOfRuntimeModules();
    findHostFunctionsThatProgramHasStored(makers);
    times.didFinish("uses of the heap: operations, behaviors, modules"_s);
    forEachBuiltinApplicationInParallel(inputs, heap, makers, m_builtinsThatLookAtObjects, [&](unsigned worker, const ValueFlowBuiltinApplication& application) {
        noteApplication(noterOf(makers, worker), application);
    }, [&](unsigned worker, const ValueFlowBuiltinApplication& application, ValueSetID builtinCallees) {
        noteApplicationOfAll(noterOf(makers, worker), application, builtinCallees);
    });
    times.didFinish("uses of the heap: applications of built-ins"_s);
    goOnToPrototypes(makers);
    m_notes.clear();
    times.didFinish("uses of the heap: unions made, prototypes"_s);
}

bool UsesOfObjects::mayBeObservedUnder(ValueSetID objects, PropertyKeyID key) const
{
    const ValueSets& sets = m_inputs.sets();
    return sets.meet(objects, m_enumerated) || sets.meet(objects, m_observedUnderAnyKey) || sets.meet(objects, setAt(m_observedUnderKey, key.index()));
}

UnionOfSets UsesOfObjects::takeUnion(UnionOfSets Notes::* parts)
{
    UnionOfSets all;
    for (Notes& notes : m_notes)
        all.takeFrom(notes.*parts);
    return all;
}

Vector<PartOfNumber> UsesOfObjects::takeParts(PartsByNumber Notes::* parts)
{
    Vector<PartOfNumber> all;
    for (Notes& notes : m_notes)
        all.appendVector((notes.*parts).take());
    return all;
}

SetsByNumber UsesOfObjects::takeExpanded(ValueSetMakers& makers, Vector<PartOfNumber>&& parts)
{
    PartsOfNumbers partsOfNumbers(m_inputs.loops(), WTF::move(parts));
    Vector<ValueSetID> expanded(partsOfNumbers.size());
    forEachIndexInParallel(m_inputs.loops(), partsOfNumbers.size(), [&](unsigned position, unsigned worker) {
        ValueSetMaker& maker = makers.of(worker);
        expanded[position] = m_heap.expand(maker, partsOfNumbers.unionAt(position, maker));
    });
    SetsByNumber sets;
    sets.reserveInitialCapacity(partsOfNumbers.size());
    for (unsigned position = 0; position < partsOfNumbers.size(); ++position)
        sets.add(partsOfNumbers.numberAt(position), expanded[position]);
    return sets;
}

// A walk over the keys of an object may go on to the keys that it inherits, as for-in and the formatter of console.log do, and so does a test of a name that is not a test of an own property.
void UsesOfObjects::goOnToPrototypes(ValueSetMakers& makers)
{
    runTasksInParallel(m_inputs.loops(), {
        [&] {
            findObjectsObservedUnderKeys(makers);
        },
        [&] {
            BorrowedValueSetMaker maker(m_inputs.makerPool());
            m_enumerated = m_heap.chainOf(maker.get(), takeUnion(&Notes::enumerated).takeExpanded(m_heap, maker.get()));
        },
        [&] {
            BorrowedValueSetMaker maker(m_inputs.makerPool());
            m_observedUnderAnyKey = m_heap.chainOf(maker.get(), takeUnion(&Notes::observedUnderAnyKey).takeExpanded(m_heap, maker.get()));
        },
        [&] {
            ValueSetMakers makersBeside(m_inputs);
            m_calledUnderKey = takeExpanded(makersBeside, takeParts(&Notes::calledUnderKey));
            m_accessedUnderKey = takeExpanded(makersBeside, takeParts(&Notes::accessedUnderKey));
        },
        [&] {
            listReceiversOfReads();
        },
    });
}

void UsesOfObjects::findObjectsObservedUnderKeys(ValueSetMakers& makers)
{
    const ParallelLoops& loops = m_inputs.loops();
    // Tests of many names start at the same objects, whose chain is walked once.
    PartsOfNumbers startsOfTests(loops, takeParts(&Notes::startsOfTestsUnderKey));
    Vector<ValueSetID> starts(startsOfTests.size());
    forEachIndexInParallel(loops, startsOfTests.size(), [&](unsigned position, unsigned worker) {
        starts[position] = m_heap.expand(makers.of(worker), startsOfTests.unionAt(position, makers.of(worker)));
    });
    Vector<ValueSetID> distinctStarts = starts;
    AOT::sort(distinctStarts);
    removeRepeatedElements(distinctStarts);
    Vector<ValueSetID> chains(distinctStarts.size());
    forEachIndexInParallel(loops, distinctStarts.size(), [&](unsigned index, unsigned worker) {
        chains[index] = m_heap.chainOf(makers.of(worker), distinctStarts[index]);
    });
    Vector<PartOfNumber> observedUnderKey = takeParts(&Notes::observedUnderKey);
    for (unsigned position = 0; position < startsOfTests.size(); ++position) {
        size_t index = std::ranges::lower_bound(distinctStarts, starts[position]) - distinctStarts.begin();
        if (chains[index] != ValueSets::nothing)
            observedUnderKey.append({ startsOfTests.numberAt(position), chains[index] });
    }

    m_observedUnderKey = takeExpanded(makers, WTF::move(observedUnderKey));
}

void UsesOfObjects::listReceiversOfReads()
{
    // The parts that list their members, each once. Nobody pays for a union that is asked for a handful of objects.
    PartsOfNumbers receiversOfReads(m_inputs.loops(), takeParts(&Notes::receiversOfReads));
    m_receiversOfReads.reserveInitialCapacity(receiversOfReads.size());
    for (unsigned position = 0; position < receiversOfReads.size(); ++position) {
        Vector<ValueSetID> receivers;
        for (const PartOfNumber& part : receiversOfReads.partsAt(position)) {
            if (!ValueSets::isComplement(part.set))
                receivers.append(part.set);
        }
        m_receiversOfReads.add(receiversOfReads.numberAt(position), WTF::move(receivers));
    }
}

// The values that go nowhere but into the callee position of calls.
static BitVector findValuesOnlyCalled(const Function& function)
{
    struct Cast {
        uint32_t result;
        uint32_t operand;
    };

    BitVector isCalled(function.values.size());
    BitVector isUsedOtherwise(function.values.size());
    Vector<Cast, 8> casts;
    for (uint32_t index = 0; index < function.values.size(); ++index) {
        const Value& value = function.values[index];
        value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            OperandUses uses = operandUsesOf(value.opcode, operandIndex);
            if (isCast(value.opcode) && uses.contains(OperandUse::Returns))
                casts.append({ index, operand.index() });
            else if (uses.contains(OperandUse::Calls) && !uses.containsAny({ OperandUse::Retains, OperandUse::Returns }))
                isCalled.quickSet(operand.index());
            else
                isUsedOtherwise.quickSet(operand.index());
        });
    }
    for (const Block& block : function.blocks) {
        block.forEachChildOutsideValues([&](ValueID child) {
            isUsedOtherwise.quickSet(child.index());
        });
    }

    // A cast hands on what it takes.
    for (bool hasChanged = !casts.isEmpty(); hasChanged;) {
        hasChanged = false;
        for (const Cast& cast : casts)
            hasChanged |= isCalled.quickGet(cast.result) && !isCalled.quickSet(cast.operand);
    }
    for (bool hasChanged = !casts.isEmpty(); hasChanged;) {
        hasChanged = false;
        for (const Cast& cast : casts)
            hasChanged |= (isUsedOtherwise.quickGet(cast.result) || !isCalled.quickGet(cast.result)) && !isUsedOtherwise.quickSet(cast.operand);
    }
    isCalled.exclude(isUsedOtherwise);
    return isCalled;
}

// The values that go nowhere but into the receiver position of reads and stores by name.
static BitVector findValuesOnlyAccessed(const Function& function)
{
    struct Cast {
        uint32_t result;
        uint32_t operand;
    };

    BitVector isAccessed(function.values.size());
    BitVector isUsedOtherwise(function.values.size());
    Vector<Cast, 8> casts;
    for (uint32_t index = 0; index < function.values.size(); ++index) {
        const Value& value = function.values[index];
        value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            if (isCast(value.opcode) && operandUsesOf(value.opcode, operandIndex).contains(OperandUse::Returns))
                casts.append({ index, operand.index() });
            else if ((value.opcode == Opcode::PropGet && operandIndex == OperandIndex::PropGet::object) || (value.opcode == Opcode::PropSet && operandIndex == OperandIndex::PropSet::object))
                isAccessed.quickSet(operand.index());
            else
                isUsedOtherwise.quickSet(operand.index());
        });
    }
    for (const Block& block : function.blocks) {
        block.forEachChildOutsideValues([&](ValueID child) {
            isUsedOtherwise.quickSet(child.index());
        });
    }

    // A cast hands on what it takes.
    for (bool hasChanged = !casts.isEmpty(); hasChanged;) {
        hasChanged = false;
        for (const Cast& cast : casts)
            hasChanged |= isAccessed.quickGet(cast.result) && !isAccessed.quickSet(cast.operand);
    }
    for (bool hasChanged = !casts.isEmpty(); hasChanged;) {
        hasChanged = false;
        for (const Cast& cast : casts)
            hasChanged |= (isUsedOtherwise.quickGet(cast.result) || !isAccessed.quickGet(cast.result)) && !isUsedOtherwise.quickSet(cast.operand);
    }
    isAccessed.exclude(isUsedOtherwise);
    return isAccessed;
}

// A location that is only read as a callee loses its value once the operations that read it are gone, so a pass has to find each of them from the location.
void UsesOfObjects::noteRead(Noter noter, const ValueFlowNodeKey& key, Reader reader, ValueSetID startOfLookup)
{
    switch (key.name().kind) {
    case ValueFlowNameKind::Property:
        if (reader != Reader::TakesValue) {
            (reader == Reader::OnlyCalls ? noter.notes.calledUnderKey : noter.notes.accessedUnderKey).add(key.payload, noter.maker.intersect(key.set, startOfLookup));
            noter.notes.observedUnderKey.add(key.payload, noter.maker.intersect(key.set, ValueSets::complement(startOfLookup)));
        } else
            noter.notes.observedUnderKey.add(key.payload, key.set);
        break;
    case ValueFlowNameKind::EveryProperty:
        noter.notes.observedUnderAnyKey.add(key.set);
        break;
    default:
        // A read of what was stored under a key nobody knows finds nothing that was stored by name. A read under such a key takes every property.
        break;
    }
}

// Every read of a value goes through a node that is keyed by the objects it looks into: those that the lookup starts at and all that lies above them.
void UsesOfObjects::noteReads(ValueSetMakers& makers)
{
    struct Visit {
        ValueFlowStatementID statement;
        Reader reader;
    };

    // The nodes that a thread has looked at for the statement that it follows, by ValueFlowSolver::indexOfMadeNodes().
    struct NodesLookedAt {
        BitVector isLookedAt;
        Vector<uint32_t> numbers;
    };

    constexpr uint32_t madeStatementsPerIndex = 512;
    constexpr uint32_t mostNodesNotToldApart = 64;
    const Program& program = m_inputs.program;
    const ParallelLoops& loops = m_inputs.loops();
    const ValueFlowSolver& solver = m_inputs.solver();
    const ValueFlowBodies& bodies = m_inputs.bodies();
    std::span<const ValueFlowStatement> statements = bodies.statements();
    std::span<const ValueFlowBodyRange> ranges = bodies.ranges();
    ValueSetID placeholders = m_inputs.universe().placeholdersAlone();
    uint32_t numberOfStatements = solver.numberOfStatements();
    uint32_t numberOfMadeStatements = numberOfStatements - static_cast<uint32_t>(statements.size());

    // Bits of Reader. The thread that sets a bit is the one that lists the visit.
    Vector<uint8_t> readersOfMadeStatement;
    readersOfMadeStatement.fill(0, numberOfMadeStatements);
    auto readersOf = [&](ValueFlowStatementID made) {
        return AtomicRef(readersOfMadeStatement[made.index() - statements.size()]);
    };
    auto addReader = [&](ValueFlowStatementID made, Reader reader) {
        uint8_t bit = static_cast<uint8_t>(reader);
        return !(readersOf(made).load(std::memory_order_relaxed) & bit) && !(readersOf(made).fetch_or(bit, std::memory_order_relaxed) & bit);
    };
    Vector<Vector<Visit>> visitsListedBy(loops.width());
    Vector<NodesLookedAt> nodesLookedAtBy(loops.width());
    // A number stands for one made node or for several that were made together.
    uint32_t numberOfMadeNodes = solver.graph().numberOfNodes() - solver.input().numberOfNodes;
    auto follow = [&](unsigned worker, ValueFlowStatementID statement, Reader reader, ValueSetID startOfLookup) {
        Noter noter = noterOf(makers, worker);
        NodesLookedAt& nodesLookedAt = nodesLookedAtBy[worker];
        uint32_t numberOfNodesAskedFor = 0;
        m_inputs.forEachMadeNodeAskedForBy(statement, kindsOfMadeNodes({ ValueFlowMadeNodeKind::Read, ValueFlowMadeNodeKind::Lookup }), true, [&](const ValueFlowNodeKey& key, ValueFlowStatementID made) {
            // A statement that keeps what all its runs have asked for lists the same nodes again and again. In a long list a node is looked at once.
            if (numberOfNodesAskedFor++ >= mostNodesNotToldApart) {
                nodesLookedAt.isLookedAt.ensureSize(numberOfMadeNodes);
                uint32_t number = solver.indexOfMadeNodes(key);
                if (nodesLookedAt.isLookedAt.set(number))
                    return;
                nodesLookedAt.numbers.append(number);
            }
            bool readsByName = (key.kind == ValueFlowMadeNodeKind::Read || key.kind == ValueFlowMadeNodeKind::Lookup) && key.name().kind == ValueFlowNameKind::Property;
            // Nobody lists the receivers of a read through a placeholder.
            bool onlyCalls = (reader == Reader::OnlyCalls || isLookupOfCalleeByCall(m_inputs, solver.statementAt(statement), key)) && readsByName && !m_inputs.sets().meet(key.set, placeholders);
            bool onlyAccesses = reader == Reader::OnlyAccesses && readsByName && !m_inputs.sets().meet(key.set, placeholders);
            Reader readerThatLeavesNoValue = onlyCalls ? Reader::OnlyCalls : onlyAccesses ? Reader::OnlyAccesses : Reader::TakesValue;
            if (key.kind == ValueFlowMadeNodeKind::Read)
                noteRead(noter, key, startOfLookup ? readerThatLeavesNoValue : Reader::TakesValue, startOfLookup);
            if (key.kind == ValueFlowMadeNodeKind::Lookup && readsByName)
                noter.notes.receiversOfReads.add(key.payload, key.set);
            if (!made)
                return;
            Reader readerOfMade = key.kind == ValueFlowMadeNodeKind::Lookup ? readerThatLeavesNoValue : Reader::TakesValue;
            if (addReader(made, readerOfMade))
                visitsListedBy[worker].append({ made, readerOfMade });
        });
        for (uint32_t number : nodesLookedAt.numbers)
            nodesLookedAt.isLookedAt.quickClear(number);
        nodesLookedAt.numbers.shrink(0);
    };
    auto followMadeStatements = [&] {
        Vector<Visit> visits;
        for (;;) {
            visits.shrink(0);
            for (Vector<Visit>& listed : visitsListedBy)
                visits.appendVector(std::exchange(listed, { }));
            if (visits.isEmpty())
                return;
            forEachIndexInParallel(loops, visits.size(), [&](unsigned index, unsigned worker) {
                Visit visit = visits[index];
                follow(worker, visit.statement, visit.reader, visit.reader != Reader::TakesValue ? solver.keyOf(visit.statement).set : ValueSetID());
            });
        }
    };

    forEachIndexInParallel(loops, bodies.numberOfBodies(), [&](unsigned body, unsigned worker) {
        if (body != bodies.hostBody() && !m_inputs.isReached(FunctionID(body)))
            return;
        // No code is emitted for a model: native code reads what it reads, and no pass turns its calls into direct calls.
        bool isEmitted = body != bodies.hostBody() && program.functions[body].role != FunctionRole::Model;
        std::optional<BitVector> valuesOnlyCalled;
        std::optional<BitVector> valuesOnlyAccessed;
        for (uint32_t index = ranges[body].statement; index < ranges[body + 1].statement; ++index) {
            m_inputs.prefetchMadeNodesAskedForBy(index + 8, kindsOfMadeNodes({ ValueFlowMadeNodeKind::Read, ValueFlowMadeNodeKind::Lookup }), true);
            const ValueFlowStatement& statement = statements[index];
            Reader reader = Reader::TakesValue;
            if (isEmitted && statement.kind == ValueFlowStatementKind::Operation && statement.opcode == Opcode::PropGet && statement.function == FunctionID(body)) {
                if (!valuesOnlyCalled) {
                    valuesOnlyCalled = findValuesOnlyCalled(program.functions[body]);
                    valuesOnlyAccessed = findValuesOnlyAccessed(program.functions[body]);
                }
                if (valuesOnlyCalled->quickGet(statement.site.index()))
                    reader = Reader::OnlyCalls;
                else if (valuesOnlyAccessed->quickGet(statement.site.index()))
                    reader = Reader::OnlyAccesses;
            }
            follow(worker, ValueFlowStatementID(index), reader, { });
        }
    });
    followMadeStatements();
    // Whatever is reached from here on is reached by a reader that takes the value: no made statement is an operation, and only an operation brings another reader up.
    forEachIndexInParallel(loops, (numberOfMadeStatements + madeStatementsPerIndex - 1) / madeStatementsPerIndex, [&](unsigned run, unsigned worker) {
        uint32_t first = static_cast<uint32_t>(statements.size()) + run * madeStatementsPerIndex;
        for (uint32_t index = first; index < std::min(first + madeStatementsPerIndex, numberOfStatements); ++index) {
            ValueFlowStatementID statement(index);
            RELEASE_ASSERT(solver.statementAt(statement).kind != ValueFlowStatementKind::Operation);
            if (readersOf(statement).load(std::memory_order_relaxed) || solver.isAtRest(statement))
                continue;
            if (addReader(statement, Reader::TakesValue))
                follow(worker, statement, Reader::TakesValue, { });
        }
    });
    followMadeStatements();
}

// Where an import links a module of the run time, the loader lists the names of the object that the body of the module answers.
void UsesOfObjects::noteObjectsOfRuntimeModules()
{
    const Program& program = m_inputs.program;
    for (const Module& module : program.modules) {
        if (module.kind != ModuleKind::RuntimeSource || module.exportRule == SyntheticExportRule::None || !module.body || !m_inputs.isReached(module.body))
            continue;
        if (!program.at(module.body).results.isEmpty())
            m_notes[0].enumerated.add(m_inputs.solver().valueOf(m_inputs.bodies().nodes().result(module.body, 0)));
    }
}

void UsesOfObjects::noteOperations(ValueSetMakers& makers)
{
    const Program& program = m_inputs.program;
    forEachIndexInParallel(m_inputs.loops(), program.functions.size(), [&](unsigned index, unsigned worker) {
        if (!m_inputs.isReached(FunctionID(index)))
            return;
        Noter noter = noterOf(makers, worker);
        const Function& function = program.functions[index];
        for (const ValueFlowStatement& statement : m_inputs.bodies().statementsOf(index)) {
            if (statement.kind != ValueFlowStatementKind::Operation)
                continue;
            std::span<const ValueFlowOperand> operands = m_inputs.bodies().operandsOf(statement);
            for (uint32_t operand = 0; operand < operands.size(); ++operand) {
                if (protocolsOf(statement.opcode, operand).containsAny(protocolsListingKeys))
                    noter.notes.enumerated.add(m_heap.valueOf(noter.maker, operands[operand]));
            }
            switch (statement.opcode) {
            case Opcode::PropHas:
            case Opcode::ScopeHasBinding:
                noter.notes.startsOfTestsUnderKey.add(function.at(statement.site).key().index(), m_heap.valueOf(noter.maker, operands[0]));
                break;
            case Opcode::ElemHas:
                noter.notes.observedUnderAnyKey.add(m_heap.valueOf(noter.maker, operands[OperandIndex::ElemHas::object]));
                break;
            case Opcode::CopyDataProperties:
                noter.notes.enumerated.add(m_heap.valueOf(noter.maker, operands[OperandIndex::CopyDataProperties::source]));
                break;
            case Opcode::CopyObject:
                noter.notes.enumerated.add(m_heap.valueOf(noter.maker, operands[OperandIndex::CopyObject::source]));
                break;
            default:
                break;
            }
        }
    });
}

// The bodies keep nothing of an operand that only such a protocol gets hold of where the operation has no rule of its own. No operation of a program as read is like that.
void UsesOfObjects::noteOperationsWithoutStatement()
{
    OpcodeSet opcodes;
    for (Opcode opcode : allMembersOf<Opcode>) {
        if (hasRuleOfItsOwn(opcode))
            continue;
        uint32_t numberOfDeclaredOperands = numberOfNamedOperandsOf(opcode) + (hasVariadicOperand(opcode) ? 1 : 0);
        for (uint32_t operand = 0; operand < numberOfDeclaredOperands; ++operand) {
            if (protocolsOf(opcode, operand).contains(Protocol::OwnKeys))
                opcodes.add(opcode);
        }
    }
    if (opcodes.isEmpty())
        return;
    const Program& program = m_inputs.program;
    std::atomic<bool> listsKeys { false };
    forEachIndexInParallel(m_inputs.loops(), program.functions.size(), [&](unsigned index) {
        if (!m_inputs.isReached(FunctionID(index)) || listsKeys.load(std::memory_order_relaxed))
            return;
        bool functionListsKeys = std::ranges::any_of(program.functions[index].values, [&](const Value& value) {
            return opcodes.contains(value.opcode);
        });
        if (functionListsKeys)
            listsKeys.store(true, std::memory_order_relaxed);
    });
    if (listsKeys.load(std::memory_order_relaxed))
        m_notes[0].enumerated.add(ValueSets::anything);
}

// The hub of a behavior holds whatever has reached an operand with the behavior, and where the behavior goes on to what an object holds, that too.
void UsesOfObjects::noteBehaviors()
{
    const ValueFlowBuiltinTables& tables = m_inputs.universe().builtinTables();
    for (uint32_t index = 0; index < tables.numberOfBehaviors(); ++index) {
        const ValueFlowBehavior& behavior = tables.at(ValueFlowBehaviorID(index));
        bool readsEveryMember = (behavior.protocols.contains(Protocol::Named) && behavior.namedMembers.empty()) || (behavior.protocols.contains(Protocol::CallsNamed) && behavior.calledMembers.empty());
        if (behavior.protocols.containsAny(protocolsListingKeys) || readsEveryMember)
            m_notes[0].enumerated.add(m_inputs.solver().valueOf(m_inputs.bodies().nodes().hub(ValueFlowBehaviorID(index))));
    }
}

void UsesOfObjects::noteParameter(Noter noter, const ValueFlowBuiltinApplication& application, const BuiltinParameter& parameter)
{
    // Such an operand has escaped.
    if (parameter.uses == OperandUses::all())
        return;
    if (!parameter.protocols.containsAny(protocolsListingKeys) || parameter.roles.contains(BuiltinOperandRole::KeysAreNotListed))
        return;
    // What the operand holds is listed too, and only a hub follows that.
    bool goesOnToContents = parameter.uses.containsAny({ OperandUse::AppliesTransitively, OperandUse::AppliesToContents });
    if (goesOnToContents && !m_inputs.universe().builtinTables().behaviorOf(parameter)) {
        noter.notes.enumerated.add(ValueSets::anything);
        return;
    }
    noter.notes.enumerated.add(application.valueAt(parameter.operand, noter.maker));
}

// The built-in looks at single properties of the operand and shows nobody the order of the keys. Where no action says under which key it looks, as for Reflect.has, the key may be any.
void UsesOfObjects::noteParameterWithoutAction(Noter noter, const ValueFlowBuiltinApplication& application, const BuiltinOverload& overload, const BuiltinParameter& parameter)
{
    if (parameter.uses == OperandUses::all() || !parameter.roles.contains(BuiltinOperandRole::KeysAreNotListed) || !parameter.protocols.containsAny(protocolsListingKeys))
        return;
    bool hasAction = std::ranges::any_of(overload.flow.propertyActions, [&](const BuiltinPropertyAction& action) {
        return action.object == parameter.operand;
    });
    if (!hasAction)
        noter.notes.observedUnderAnyKey.add(application.valueAt(parameter.operand, noter.maker));
}

// The built-in finds out whether something lies under a key, or what kind of property does. An operation on one own property runs the traps of a proxy, and one that goes on to the prototypes declares the walk.
void UsesOfObjects::noteTest(Noter noter, const ValueFlowBuiltinApplication& application, const BuiltinOverload& overload, const BuiltinPropertyAction& action)
{
    const Program& program = m_inputs.program;
    const ValueSets& sets = m_inputs.sets();
    const ValueFlowUniverse& universe = m_inputs.universe();
    const ValueFlowHeapTables& tables = universe.builtinTables().heap();
    ValueSetID objects = application.valueAt(action.object, noter.maker);
    const BuiltinParameter* parameter = program.builtins().parameterFor(overload, action.object);
    bool looksAtOwnPropertyAlone = parameter && parameter->roles.contains(BuiltinOperandRole::KeysAreNotListed) && !parameter->protocols.contains(Protocol::PrototypeChain);
    PartsByNumber& underKey = looksAtOwnPropertyAlone ? noter.notes.observedUnderKey : noter.notes.startsOfTestsUnderKey;

    switch (action.key.kind) {
    case BuiltinKeyKind::None:
        return;
    case BuiltinKeyKind::Names:
        for (BuiltinName name : action.key.names)
            underKey.add(program.keyOf(name).index(), objects);
        return;
    case BuiltinKeyKind::Operand: {
        ValueSetID keys = application.valueAt(*action.key.operand, noter.maker);
        // An operand that is left out is undefined, which names a property like any other value.
        if (sets.isEmpty(keys) || sets.meet(keys, tables.keysNobodyKnows()) || sets.meet(keys, tables.numberKinds()))
            break;
        sets.forEachMember(keys, [&](ValueFlowObjectID constant) {
            underKey.add(universe.keyOf(constant).index(), objects);
        });
        return;
    }
    case BuiltinKeyKind::Any:
    case BuiltinKeyKind::Indexes:
    case BuiltinKeyKind::ElementsOf:
        break;
    }
    noter.notes.observedUnderAnyKey.add(objects);
}

// Which overload a call selects depends on its operands. Every overload of the mode counts.
void UsesOfObjects::findHostFunctionsThatProgramHasStored(ValueSetMakers& makers)
{
    const Program& program = m_inputs.program;
    const ValueSets& sets = m_inputs.sets();
    // Most functions of the host take whatever they are called on.
    const BuiltinTable& builtins = program.builtins();
    Vector<ValueFlowObjectID> functionsThatRefuseReceivers;
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        if (builtins.at(BuiltinID(index)).origin != BuiltinOrigin::Host)
            continue;
        bool refusesReceivers = std::ranges::any_of(builtins.overloadsOf(BuiltinID(index)), [&](const BuiltinOverload& overload) {
            return overload.receiver && overload.receiver->accepted && !builtins.at(overload.receiver->accepted).isAnything();
        });
        if (refusesReceivers)
            functionsThatRefuseReceivers.append(m_inputs.universe().objectOf(BuiltinID(index)));
    }
    ValueSetID builtinFunctions = makers.of(0).intersect(makers.of(0).of(functionsThatRefuseReceivers.span()), m_inputs.universe().builtinFunctions());
    std::span<const ValueFlowStatement> statements = m_inputs.bodies().statements();
    Vector<uint32_t> keys;
    for (const ValueFlowHeapIndex::CallOfValue& call : m_heap.index().callsThatMayApplyBuiltins()) {
        const ValueFlowStatement& statement = statements[call.statement];
        if ((statement.opcode != Opcode::Call && statement.opcode != Opcode::CallList) || !sets.meet(call.callees, builtinFunctions))
            continue;
        if (PropertyKeyID key = program.at(statement.function).at(statement.site).calleeKey())
            keys.append(key.index());
    }
    AOT::sort(keys);
    removeRepeatedElements(keys);

    Vector<ValueSetID> stored(FillWith { }, keys.size(), ValueSets::nothing);
    forEachIndexInParallel(m_inputs.loops(), keys.size(), [&](unsigned position, unsigned worker) {
        UnionOfSets values;
        m_heap.forEachNodeWrittenUnder(ValueFlowName::property(PropertyKeyID(keys[position])), [&](ValueSetID, ValueSetID value) {
            if (sets.meet(value, builtinFunctions))
                values.add(value);
        });
        stored[position] = makers.of(worker).intersect(values.take(makers.of(worker)), builtinFunctions);
    });
    for (size_t position = 0; position < keys.size(); ++position) {
        if (stored[position] != ValueSets::nothing)
            m_hostFunctionsStoredUnderKey.add(keys[position], stored[position]);
    }
}

// A function of the host names a value that it refuses in the message of its error: `Received an instance of Point`, `Received {}`, `Received [Object]`.
void UsesOfObjects::noteRefusedOperands(Noter noter, const ValueFlowBuiltinApplication& application)
{
    const BuiltinTable& builtins = m_inputs.program.builtins();
    if (builtins.at(application.builtin).origin != BuiltinOrigin::Host)
        return;
    const ValueFlowBuiltinTables& tables = m_inputs.universe().builtinTables();
    struct TakenAtOperand {
        BuiltinOperand operand;
        ValueSetID taken;
    };
    Vector<TakenAtOperand, 8> takenAtOperands;
    auto note = [&](const BuiltinParameter& parameter) {
        ValueSetID taken = parameter.accepted ? tables.membersSurelyOf(parameter.accepted) : ValueSets::anything;
        if (parameter.converted)
            taken = noter.maker.unite(taken, tables.membersSurelyOf(parameter.converted));
        for (TakenAtOperand& entry : takenAtOperands) {
            if (entry.operand == parameter.operand) {
                entry.taken = noter.maker.unite(entry.taken, taken);
                return;
            }
        }
        takenAtOperands.append({ parameter.operand, taken });
    };
    // A call of a bound function that an operation spells out comes with what was bound as `this`.
    bool mayRunOnObjectOfProgram = application.function && application.kind == ValueFlowCallKind::CallByNativeCode;
    if (PropertyKeyID key = application.function && application.kind == ValueFlowCallKind::Call ? m_inputs.program.at(application.function).at(application.site).calleeKey() : PropertyKeyID()) {
        const ValueFlowUniverse& universe = m_inputs.universe();
        ValueFlowObjectID function = universe.objectOf(application.builtin);
        auto stored = m_hostFunctionsStoredUnderKey.find(key.index());
        if (stored != m_hostFunctionsStoredUnderKey.end() && m_inputs.sets().contains(stored->value, function)) {
            // One call has many such callees where its receiver is little known, and many calls have the same receivers: the chain above them is walked once for a name.
            ValueSetID found = m_foundAboveReceivers.ensure(static_cast<uint64_t>(application.thisValue.index()) << 32 | key.index(), [&] {
                ValueSetID ofProgram = noter.maker.intersect(application.thisValue, noter.maker.intersect(universe.knownObjects(), ValueSets::complement(universe.builtinObjects())));
                return ofProgram == ValueSets::nothing ? ValueSets::nothing : m_heap.writtenUnder(noter.maker, m_heap.chainOf(noter.maker, ofProgram), ValueFlowName::property(key));
            });
            mayRunOnObjectOfProgram = m_inputs.sets().contains(found, function);
        }
    }
    for (const BuiltinOverload& overload : builtins.overloadsOf(application.builtin)) {
        if (overload.mode != application.mode())
            continue;
        if (overload.receiver && mayRunOnObjectOfProgram)
            note(*overload.receiver);
        for (const BuiltinParameter& parameter : overload.parameters)
            note(parameter);
    }
    for (const TakenAtOperand& entry : takenAtOperands) {
        if (entry.taken != ValueSets::anything)
            noter.notes.enumerated.add(noter.maker.intersect(application.valueAt(entry.operand, noter.maker), ValueSets::complement(entry.taken)));
    }
}

void UsesOfObjects::noteApplication(Noter noter, const ValueFlowBuiltinApplication& application)
{
    if (!m_builtinsThatLookAtObjects.quickGet(application.builtin.index()))
        return;
    noteRefusedOperands(noter, application);
    for (const BuiltinOverload& overload : m_inputs.program.builtins().overloadsOf(application.builtin)) {
        if (overload.mode != application.mode())
            continue;
        if (overload.receiver) {
            noteParameter(noter, application, *overload.receiver);
            noteParameterWithoutAction(noter, application, overload, *overload.receiver);
        }
        for (const BuiltinParameter& parameter : overload.parameters) {
            noteParameter(noter, application, parameter);
            noteParameterWithoutAction(noter, application, overload, parameter);
        }
        for (const BuiltinPropertyAction& action : overload.flow.propertyActions) {
            switch (action.kind) {
            case BuiltinPropertyActionKind::TestsByKey:
            case BuiltinPropertyActionKind::ReadsGetter:
            case BuiltinPropertyActionKind::ReadsSetter:
                noteTest(noter, application, overload, action);
                break;
            case BuiltinPropertyActionKind::AssignsFrom:
            case BuiltinPropertyActionKind::DefinesFrom:
            case BuiltinPropertyActionKind::TakesDescriptorMap:
                if (action.value.operand)
                    noter.notes.enumerated.add(application.valueAt(*action.value.operand, noter.maker));
                else
                    noter.notes.enumerated.add(ValueSets::anything);
                break;
            default:
                break;
            }
        }
    }
}

auto UsesOfObjects::findNotesOf(ValueSetMaker& maker, ValueSetID builtinFunctions, BuiltinCallMode mode) const -> std::unique_ptr<NotesOfBuiltins>
{
    const Program& program = m_inputs.program;
    const BuiltinTable& builtins = program.builtins();
    const ValueFlowBuiltinTables& tables = m_inputs.universe().builtinTables();
    auto notes = makeUnique<NotesOfBuiltins>();
    auto addOnce = [](auto& list, const auto& entry) {
        if (!list.contains(entry))
            list.append(entry);
    };
    auto noteParameter = [&](const BuiltinOverload& overload, const BuiltinParameter& parameter) {
        if (parameter.uses == OperandUses::all() || !parameter.protocols.containsAny(protocolsListingKeys))
            return;
        if (parameter.roles.contains(BuiltinOperandRole::KeysAreNotListed)) {
            bool hasAction = std::ranges::any_of(overload.flow.propertyActions, [&](const BuiltinPropertyAction& action) {
                return action.object == parameter.operand;
            });
            if (!hasAction)
                addOnce(notes->observedUnderAnyKey, parameter.operand);
            return;
        }
        bool goesOnToContents = parameter.uses.containsAny({ OperandUse::AppliesTransitively, OperandUse::AppliesToContents });
        if (goesOnToContents && !tables.behaviorOf(parameter))
            notes->enumeratesAnything = true;
        else
            addOnce(notes->enumerated, parameter.operand);
    };
    auto noteTest = [&](const BuiltinOverload& overload, const BuiltinPropertyAction& action) {
        const BuiltinParameter* parameter = builtins.parameterFor(overload, action.object);
        bool looksAtOwnPropertyAlone = parameter && parameter->roles.contains(BuiltinOperandRole::KeysAreNotListed) && !parameter->protocols.contains(Protocol::PrototypeChain);
        switch (action.key.kind) {
        case BuiltinKeyKind::None:
            return;
        case BuiltinKeyKind::Names:
            for (BuiltinName name : action.key.names)
                addOnce(notes->testsUnderNames, NotesOfBuiltins::TestUnderName { action.object, program.keyOf(name).index(), looksAtOwnPropertyAlone });
            return;
        case BuiltinKeyKind::Operand:
            addOnce(notes->testsUnderOperands, NotesOfBuiltins::TestUnderOperand { action.object, *action.key.operand, looksAtOwnPropertyAlone });
            return;
        case BuiltinKeyKind::Any:
        case BuiltinKeyKind::Indexes:
        case BuiltinKeyKind::ElementsOf:
            addOnce(notes->observedUnderAnyKey, action.object);
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    };

    m_inputs.sets().forEachMember(builtinFunctions, [&](ValueFlowObjectID callee) {
        BuiltinID builtin = m_inputs.universe().builtinOf(callee);
        if (!m_builtinsThatLookAtObjects.quickGet(builtin.index()))
            return;
        Vector<NotesOfBuiltins::RefusedOperand, 8> takenAtOperands;
        bool isOfHost = builtins.at(builtin).origin == BuiltinOrigin::Host;
        for (const BuiltinOverload& overload : builtins.overloadsOf(builtin)) {
            if (overload.mode != mode)
                continue;
            if (overload.receiver)
                noteParameter(overload, *overload.receiver);
            for (const BuiltinParameter& parameter : overload.parameters) {
                noteParameter(overload, parameter);
                if (!isOfHost)
                    continue;
                notes->areToldApart |= parameter.operand.isReceiver();
                ValueSetID taken = parameter.accepted ? tables.membersSurelyOf(parameter.accepted) : ValueSets::anything;
                if (parameter.converted)
                    taken = maker.unite(taken, tables.membersSurelyOf(parameter.converted));
                size_t position = takenAtOperands.findIf([&](const NotesOfBuiltins::RefusedOperand& entry) {
                    return entry.operand == parameter.operand;
                });
                if (position == notFound)
                    takenAtOperands.append({ parameter.operand, taken });
                else
                    takenAtOperands[position].taken = maker.unite(takenAtOperands[position].taken, taken);
            }
            for (const BuiltinPropertyAction& action : overload.flow.propertyActions) {
                switch (action.kind) {
                case BuiltinPropertyActionKind::TestsByKey:
                case BuiltinPropertyActionKind::ReadsGetter:
                case BuiltinPropertyActionKind::ReadsSetter:
                    noteTest(overload, action);
                    break;
                case BuiltinPropertyActionKind::AssignsFrom:
                case BuiltinPropertyActionKind::DefinesFrom:
                case BuiltinPropertyActionKind::TakesDescriptorMap:
                    if (action.value.operand)
                        addOnce(notes->enumerated, *action.value.operand);
                    else
                        notes->enumeratesAnything = true;
                    break;
                default:
                    break;
                }
            }
        }
        for (const NotesOfBuiltins::RefusedOperand& entry : takenAtOperands) {
            if (entry.taken == ValueSets::anything)
                continue;
            size_t position = notes->refusedOperands.findIf([&](const NotesOfBuiltins::RefusedOperand& known) {
                return known.operand == entry.operand;
            });
            if (position == notFound)
                notes->refusedOperands.append(entry);
            else
                notes->refusedOperands[position].taken = maker.intersect(notes->refusedOperands[position].taken, entry.taken);
        }
    });
    return notes;
}

void UsesOfObjects::noteTestUnderKeys(Noter noter, const ValueFlowBuiltinApplication& application, BuiltinOperand object, BuiltinOperand key, bool looksAtOwnPropertyAlone)
{
    const ValueSets& sets = m_inputs.sets();
    const ValueFlowUniverse& universe = m_inputs.universe();
    const ValueFlowHeapTables& tables = universe.builtinTables().heap();
    ValueSetID objects = application.valueAt(object, noter.maker);
    ValueSetID keys = application.valueAt(key, noter.maker);
    if (sets.isEmpty(keys) || sets.meet(keys, tables.keysNobodyKnows()) || sets.meet(keys, tables.numberKinds())) {
        noter.notes.observedUnderAnyKey.add(objects);
        return;
    }
    PartsByNumber& underKey = looksAtOwnPropertyAlone ? noter.notes.observedUnderKey : noter.notes.startsOfTestsUnderKey;
    sets.forEachMember(keys, [&](ValueFlowObjectID constant) {
        underKey.add(universe.keyOf(constant).index(), objects);
    });
}

void UsesOfObjects::noteApplicationOfAll(Noter noter, const ValueFlowBuiltinApplication& application, ValueSetID builtinCallees)
{
    const NotesOfBuiltins& notes = m_notesOfSetsOfBuiltins.ensure(static_cast<uint64_t>(builtinCallees.index()) << 8 | static_cast<uint64_t>(application.mode()), [&] {
        return findNotesOf(noter.maker, builtinCallees, application.mode());
    });
    ValueFlowBuiltinApplication applicationOfOne = application;
    if (notes.areToldApart) {
        m_inputs.sets().forEachMember(builtinCallees, [&](ValueFlowObjectID callee) {
            applicationOfOne.builtin = m_inputs.universe().builtinOf(callee);
            noteApplication(noter, applicationOfOne);
        });
        return;
    }
    if (notes.enumeratesAnything)
        noter.notes.enumerated.add(ValueSets::anything);
    for (BuiltinOperand operand : notes.enumerated)
        noter.notes.enumerated.add(application.valueAt(operand, noter.maker));
    for (BuiltinOperand operand : notes.observedUnderAnyKey)
        noter.notes.observedUnderAnyKey.add(application.valueAt(operand, noter.maker));
    for (const NotesOfBuiltins::TestUnderName& test : notes.testsUnderNames)
        (test.looksAtOwnPropertyAlone ? noter.notes.observedUnderKey : noter.notes.startsOfTestsUnderKey).add(test.key, application.valueAt(test.object, noter.maker));
    for (const NotesOfBuiltins::TestUnderOperand& test : notes.testsUnderOperands)
        noteTestUnderKeys(noter, application, test.object, test.key, test.looksAtOwnPropertyAlone);
    for (const NotesOfBuiltins::RefusedOperand& refused : notes.refusedOperands)
        noter.notes.enumerated.add(noter.maker.intersect(application.valueAt(refused.operand, noter.maker), ValueSets::complement(refused.taken)));

    PropertyKeyID key = application.kind == ValueFlowCallKind::Call ? m_inputs.program.at(application.function).at(application.site).calleeKey() : PropertyKeyID();
    auto stored = key ? m_hostFunctionsStoredUnderKey.find(key.index()) : m_hostFunctionsStoredUnderKey.end();
    if (stored == m_hostFunctionsStoredUnderKey.end())
        return;
    m_inputs.sets().forEachMember(noter.maker.intersect(builtinCallees, stored->value), [&](ValueFlowObjectID callee) {
        applicationOfOne.builtin = m_inputs.universe().builtinOf(callee);
        noteRefusedOperands(noter, applicationOfOne);
    });
}

struct ValueFlowUsesOfHeap {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(ValueFlowUsesOfHeap);

    explicit ValueFlowUsesOfHeap(const ValueFlowResultInputs& inputs)
        : heap(inputs)
        , uses(inputs, heap)
    {
    }

    ValueFlowHeapView heap;
    UsesOfObjects uses;
    std::optional<LayoutsOfObjects> layoutsOfObjects;
    std::optional<StoresByLayout> stores;
};

void ValueFlowUsesOfHeapDeleter::operator()(ValueFlowUsesOfHeap* usesOfHeap) const
{
    delete usesOfHeap;
}

std::unique_ptr<ValueFlowUsesOfHeap, ValueFlowUsesOfHeapDeleter> computeUsesOfHeap(const ValueFlowResultInputs& inputs)
{
    return std::unique_ptr<ValueFlowUsesOfHeap, ValueFlowUsesOfHeapDeleter>(new ValueFlowUsesOfHeap(inputs));
}

ValueFlowHeapView& heapViewOf(ValueFlowUsesOfHeap& usesOfHeap)
{
    return usesOfHeap.heap;
}

ValueSetID objectsThatMayBeEnumerated(const ValueFlowUsesOfHeap& usesOfHeap)
{
    return usesOfHeap.uses.objectsThatMayBeEnumerated();
}

namespace {

// What the steps below share.
struct FieldResultsWithoutStores {
    const ValueFlowResultInputs& inputs;
    FlowResultTables& tables;
    FlowApplication& application;
    const ValueFlowConstructions& constructions;

    const Program& program() const { return inputs.program; }
    bool hasObjects(LayoutID id) const { return hasObjectsThatCodeAlters(program().at(id).kind) && application.layouts[id.index()].flow.allocation != AllocationState::NeverAllocated; }
    OptionSet<ObjectMutation> possibleMutationsOf(LayoutID id) const
    {
        const LayoutFlow& flow = application.layouts[id.index()].flow;
        return flow.allocationClasses.isEmpty() ? allObjectMutations : allObjectMutations - flow.impossibleMutations;
    }
};

struct FieldResults : FieldResultsWithoutStores {
    ValueFlowHeapView& heap;
    const LayoutsOfObjects& layoutsOfObjects;
    StoresByLayout& stores;

    // The view remembers these chains and makes them with its own maker, which no thread of a loop uses for anything else. So the threads of a loop ask one at a time.
    mutable Lock lockOfChainsBuiltByProgram { };

    ValueSetID chainBuiltByProgram(ValueSetID objects) const
    {
        Locker locker { lockOfChainsBuiltByProgram };
        return heap.chainBuiltByProgram(objects);
    }
};

// A NewObject or a Data in a function that is reached or that is compiled without a solve.
struct AllocatingOperation {
    FunctionID function;
    ValueID value;
    // See findAllocationsThatGetMoreFields().
    bool getsMoreFields { false };
};

} // anonymous namespace

// The objects of a layout hold the fields of the layouts above it too.
static void forEachFieldOfObjectsWith(const Program& program, const FlowApplication& application, LayoutID id, NOESCAPE const Invocable<void(FieldReference declaration, const Field&)> auto& functor)
{
    Vector<LayoutID, 4> layoutsBelow;
    Vector<FieldReference, 16> visited;
    LayoutID current = id;
    for (size_t steps = 0; current && steps < program.layouts.size(); ++steps) {
        const Layout& layout = program.at(current);
        for (uint32_t index = 0; index < layout.fields.size(); ++index) {
            const Field& field = layout.fields[index];
            bool isHidden = field.key && std::ranges::any_of(layoutsBelow, [&](LayoutID below) {
                return program.at(below).findField(field.key).has_value();
            });
            if (isHidden)
                continue;
            FieldReference declaration = declarationOf(program, { current, index });
            if (!layoutsBelow.isEmpty() && visited.contains(declaration))
                continue;
            visited.append(declaration);
            functor(declaration, field);
        }
        layoutsBelow.append(current);
        current = provenBaseOf(application, current);
    }
}

static bool declaresFieldOnChain(const Program& program, const FlowApplication& application, LayoutID id, PropertyKeyID key)
{
    LayoutID current = id;
    for (size_t steps = 0; current && steps < program.layouts.size(); ++steps) {
        if (program.at(current).findField(key))
            return true;
        current = provenBaseOf(application, current);
    }
    return false;
}

// Own keys other than array indexes are listed in creation order, so the members behind a name that is no field are defined after the allocation.
static Vector<uint32_t, 16> fieldsFilledBy(const Program& program, const Layout& declared, const DataNode::Object& object)
{
    Vector<uint32_t, 16> fields;
    for (const DataMember& member : object.members) {
        if (std::optional<uint32_t> field = declared.findField(member.key)) {
            fields.append(*field);
            continue;
        }
        if (program.nameClassOf(member.key) != NameClass::Index)
            break;
    }
    return fields;
}

// `{ wrap: "nowrap", ...style, overflow: .. }` allocates an object with `wrap` and defines `overflow` in it before anybody else sees it: no finished object has the keys of the allocation alone.
static BitVector findAllocationsThatGetMoreFields(const Program& program, const Function& function)
{
    BitVector allocations(function.values.size());
    for (const Value& value : function.values) {
        if (value.opcode != Opcode::PropDefine || !value.key())
            continue;
        ValueID object = value.operand(OperandIndex::PropDefine::object);
        const Value& allocation = function.at(object);
        if (allocation.opcode != Opcode::NewObject)
            continue;
        std::optional<uint32_t> field = program.at(declaredLayoutOf(program, allocation.layout())).findField(value.key());
        if (!field || !std::ranges::contains(allocation.fields(), *field))
            allocations.quickSet(object.index());
    }
    return allocations;
}

// In the order of the functions, and in a function in the order of its values. The steps that look at the allocations walk this list and not the program.
static Vector<AllocatingOperation> collectAllocatingOperations(const FieldResultsWithoutStores& results)
{
    const ValueFlowResultInputs& inputs = results.inputs;
    const Program& program = inputs.program;
    return collectInParallel<AllocatingOperation>(inputs.loops(), program.functions.size(), [&](unsigned index, Vector<AllocatingOperation>& operations) {
        if (!inputs.isReached(FunctionID(index)) && !isUnsolvedCode(inputs, results.tables, FunctionID(index)))
            return;
        const Function& function = program.functions[index];
        size_t numberOfOtherOperations = operations.size();
        bool allocatesObject = false;
        bool definesProperty = false;
        for (uint32_t valueIndex = 0; valueIndex < function.values.size(); ++valueIndex) {
            Opcode opcode = function.values[valueIndex].opcode;
            allocatesObject |= opcode == Opcode::NewObject;
            definesProperty |= opcode == Opcode::PropDefine;
            if (opcode == Opcode::NewObject || opcode == Opcode::Data)
                operations.append({ FunctionID(index), ValueID(valueIndex) });
        }
        if (!allocatesObject || !definesProperty)
            return;
        BitVector allocationsThatGetMoreFields = findAllocationsThatGetMoreFields(program, function);
        for (AllocatingOperation& operation : operations.mutableSpan().subspan(numberOfOtherOperations))
            operation.getsMoreFields = allocationsThatGetMoreFields.quickGet(operation.value.index());
    });
}

// The fields that an object may lack when somebody else than the code that makes it gets hold of it.
static void noteFieldsMissingAtCreation(const FieldResults& results, std::span<const AllocatingOperation> operations, Vector<BitVector>& isMissingAtCreation)
{
    const ValueFlowResultInputs& inputs = results.inputs;
    const Program& program = inputs.program;
    const ParallelLoops& loops = inputs.loops();
    auto noteMissing = [&](const Vector<FieldReference>& declarations) {
        for (FieldReference declaration : declarations)
            isMissingAtCreation[declaration.layout.index()].quickSet(declaration.field);
    };

    // A constructor that the analysis has followed fills a field before the object leaves its hands, or it does not.
    noteMissing(collectInParallel<FieldReference>(loops, program.layouts.size(), [&](unsigned index, Vector<FieldReference>& missing) {
        LayoutID id(index);
        const Layout& layout = program.layouts[index];
        if (!results.hasObjects(id))
            return;
        if (results.constructions.isFollowed(id)) {
            forEachFieldOfObjectsWith(program, results.application, id, [&](FieldReference declaration, const Field& field) {
                if (!results.constructions.isStoredBeforeObjectLeaks(id, field.key, declaration))
                    missing.append(declaration);
            });
            return;
        }
        if (!layout.flags.containsAny({ LayoutFlags::HasHeritage, LayoutFlags::IsConstructorFunction }))
            return;
        for (uint32_t field = 0; field < layout.fields.size(); ++field) {
            if (!layout.fields[field].flags.contains(FieldFlags::IsInherited))
                missing.append(declarationOf(program, { id, field }));
        }
        // The constructors above fill their fields by name, and an accessor of this class takes such a store.
        forEachFieldOfObjectsWith(program, results.application, id, [&](FieldReference declaration, const Field& field) {
            if (declaration.layout != id && results.constructions.mayBeKeptFromCreatingInheritedField(id, field.key))
                missing.append(declaration);
        });
    }));

    noteMissing(collectInParallel<FieldReference>(loops, operations.size(), [&](unsigned position, Vector<FieldReference>& missing) {
        const Value& value = program.at(operations[position].function).at(operations[position].value);
        // Constant data is whole when the operation hands it out. ExpandData makes an allocation of it and, for the members behind a name that is no field, definitions that find their slots empty.
        if (value.opcode == Opcode::Data) {
            forEachDataNodeInPreorder(program.at(value.data()).root, [&](uint32_t, const DataNode& node) {
                const DataNode::Object* object = node.tryAs<DataNode::Object>();
                if (!object || !object->layout || results.constructions.isFollowed(object->layout))
                    return;
                const Layout& layout = program.at(object->layout);
                Vector<uint32_t, 16> listed = fieldsFilledBy(program, layout, *object);
                for (uint32_t field = 0; field < layout.fields.size(); ++field) {
                    if (!listed.contains(field))
                        missing.append(declarationOf(program, { object->layout, field }));
                }
            });
            return;
        }
        if (program.at(value.layout()).isEnvironment() || results.constructions.isFollowed(value.layout()))
            return;
        std::span<const uint32_t> listed = value.fields();
        for (uint32_t field = 0; field < program.at(value.layout()).fields.size(); ++field) {
            if (!std::ranges::contains(listed, field))
                missing.append(declarationOf(program, { value.layout(), field }));
        }
    }));

    // An object that has its layout from something else than an allocation with that layout.
    const ValueFlowUniverse& universe = inputs.universe();
    Vector<LayoutID> layoutsOfOtherObjects = collectInParallel<LayoutID>(loops, universe.numberOfObjects(), [&](unsigned index, Vector<LayoutID>& layouts) {
        ValueFlowObjectID object(index);
        if (universe.kindOf(object) != ValueFlowObjectKind::Site || !universe.layoutOf(object))
            return;
        const ValueFlowSite& site = *universe.siteOf(object);
        if (results.constructions.isFollowed(site.layout) || isSiteOfTypedResultThatNothingAllocates(inputs, object) || !results.hasObjects(site.layout))
            return;
        if (site.origin == ValueFlowSiteOrigin::DataNode)
            return;
        if (site.origin != ValueFlowSiteOrigin::TypedResult && !inputs.isReached(universe.functionOf(object)) && !isUnsolvedCode(inputs, results.tables, universe.functionOf(object)))
            return;
        if (site.origin == ValueFlowSiteOrigin::Operation && !site.ordinal && program.at(universe.functionOf(object)).at(site.value).opcode == Opcode::NewObject)
            return;
        layouts.append(site.layout);
    });
    AOT::sort(layoutsOfOtherObjects);
    removeRepeatedElements(layoutsOfOtherObjects);
    for (LayoutID layout : layoutsOfOtherObjects) {
        forEachFieldOfObjectsWith(program, results.application, layout, [&](FieldReference declaration, const Field&) {
            isMissingAtCreation[declaration.layout.index()].quickSet(declaration.field);
        });
    }
}

// An object that nobody but followed code gets hold of.
static bool isSeenByFollowedCodeAlone(const FieldResults& results, LayoutID id)
{
    return !results.possibleMutationsOf(id).containsAny(reachMutations) && !results.stores.hasObjectOfCodeWithoutSolve(id);
}

// All that lies in such an object was written there, or the table of built-ins lists it.
static bool holdsOnlyWhatIsWrittenUnder(const ValueFlowResultInputs& inputs, ValueFlowObjectID object, PropertyKeyID key)
{
    const ValueFlowUniverse& universe = inputs.universe();
    switch (universe.kindOf(object)) {
    case ValueFlowObjectKind::Builtin:
    case ValueFlowObjectKind::ModuleBuiltin:
        return true;
    case ValueFlowObjectKind::Site:
    case ValueFlowObjectKind::Instance: {
        LayoutID layout = universe.layoutOf(object);
        return layout && hasObjectsThatCodeAlters(inputs.program.at(layout).kind) && !inputs.classOf(object);
    }
    case ValueFlowObjectKind::Prototype:
        return key != universe.builtinTables().heap().constructorKey();
    default:
        return false;
    }
}

namespace {

// What a thread of a loop asks the heap about the objects of one layout.
class ObjectsOfLayoutInHeap {
public:
    // The chains that the thread has walked, by the objects that they start at: the objects of most layouts have the same prototypes.
    using Chains = SetsByNumber;

    ObjectsOfLayoutInHeap(const FieldResults& results, ValueSetMaker& maker, Chains& chains, LayoutID id)
        : m_results(results)
        , m_maker(maker)
        , m_chains(chains)
        , m_id(id)
        , m_objects(results.layoutsOfObjects.objectsWith(id))
    {
    }

    ValueSetID objects() const { return m_objects; }
    // A value, an accessor or a member of a built-in object may lie under the key in an object that one of these inherits from.
    bool mayFindNameAbove(PropertyKeyID);
    bool isObservedByStoresOfHeirs(PropertyKeyID);
    bool mayBeDeletedUnder(PropertyKeyID);
    bool mayBeRedefinedUnder(PropertyKeyID);

private:
    bool isWrittenUnder(ValueSetID objects, ValueFlowName name) { return !m_results.inputs.sets().isEmpty(std::as_const(m_results.heap).writtenUnder(m_maker, objects, name)); }
    ValueSetID objectsAbove();

    const FieldResults& m_results;
    ValueSetMaker& m_maker;
    Chains& m_chains;
    LayoutID m_id;
    ValueSetID m_objects;
    // None until the first field asks.
    ValueSetID m_objectsAbove;
};

} // anonymous namespace

ValueSetID ObjectsOfLayoutInHeap::objectsAbove()
{
    if (!m_objectsAbove) {
        const ValueFlowHeapView& heap = m_results.heap;
        ValueSetID prototypes = heap.prototypesOf(m_maker, m_objects);
        m_objectsAbove = m_chains.ensure(prototypes.index(), [&] {
            return heap.chainOf(m_maker, prototypes);
        }).iterator->value;
    }
    return m_objectsAbove;
}

bool ObjectsOfLayoutInHeap::mayFindNameAbove(PropertyKeyID key)
{
    const ValueFlowResultInputs& inputs = m_results.inputs;
    const ValueSets& sets = inputs.sets();
    ValueSetID above = objectsAbove();
    if (ValueSets::isComplement(above) || sets.meet(above, inputs.universe().placeholdersAlone()))
        return true;
    const ValueFlowMembersUnderKey& members = inputs.universe().builtinTables().heap().under(key);
    if (sets.meet(above, members.holders) || sets.meet(above, members.holdersOfUndeclaredProperties))
        return true;

    ValueFlowName names[] {
        ValueFlowName::property(key),
        { ValueFlowNameKind::Getter, key.index() },
        { ValueFlowNameKind::Setter, key.index() },
        ValueFlowName::of(ValueFlowNameKind::UnknownKey),
        ValueFlowName::of(ValueFlowNameKind::GetterUnderUnknownKey),
        ValueFlowName::of(ValueFlowNameKind::SetterUnderUnknownKey),
        ValueFlowName::of(ValueFlowNameKind::CopiedFrom),
    };
    for (ValueFlowName name : names) {
        if (std::as_const(m_results.heap).writtenUnder(m_maker, above, name) != ValueSets::nothing)
            return true;
    }
    bool mayHoldMore = false;
    sets.forEachMember(above, [&](ValueFlowObjectID object) {
        mayHoldMore |= !holdsOnlyWhatIsWrittenUnder(inputs, object, key);
    });
    return mayHoldMore;
}

// A store through an object that inherits from this one ends its walk at the field.
bool ObjectsOfLayoutInHeap::isObservedByStoresOfHeirs(PropertyKeyID key)
{
    OptionSet<ObjectMutation> possible = m_results.possibleMutationsOf(m_id);
    if (!possible.contains(ObjectMutation::BecomePrototype))
        return false;
    return possible.containsAny({ ObjectMutation::ChangeIntegrity, ObjectMutation::RedefineProperty }) || mayFindNameAbove(key);
}

// HeapRule::noteDeletedMembers() has put down every deletion, under its key or as one under a key nobody knows.
bool ObjectsOfLayoutInHeap::mayBeDeletedUnder(PropertyKeyID key)
{
    return isWrittenUnder(m_objects, { ValueFlowNameKind::DeletedMember, key.index() }) || isWrittenUnder(m_objects, ValueFlowName::of(ValueFlowNameKind::DeletedMemberUnderUnknownKey));
}

// HeapRule::markRedefinition() has put down every redefinition but those through a proxy, under its key or as one under a key nobody knows.
bool ObjectsOfLayoutInHeap::mayBeRedefinedUnder(PropertyKeyID key)
{
    return isWrittenUnder(m_objects, { ValueFlowNameKind::RedefinedMember, key.index() }) || isWrittenUnder(m_objects, ValueFlowName::of(ValueFlowNameKind::RedefinedMemberUnderUnknownKey));
}

static void computeKeyEnumerations(const FieldResults& results, const UsesOfObjects& uses)
{
    const Program& program = results.program();
    Vector<uint32_t> layoutsNeverEnumerated = findIndicesInParallel(results.inputs.loops(), program.layouts.size(), [&](unsigned index) {
        LayoutID id(index);
        if (!results.hasObjects(id) || results.application.layouts[index].flow.allocationClasses.isEmpty() || !results.inputs.build.isEnabled(FlowResultKind::ObjectUses, index))
            return false;
        return isSeenByFollowedCodeAlone(results, id) && !results.possibleMutationsOf(id).contains(ObjectMutation::BecomePrototype) && !uses.mayBeEnumerated(results.layoutsOfObjects.objectsWith(id));
    });
    for (uint32_t index : layoutsNeverEnumerated) {
        LayoutFlow& flow = results.application.layouts[index].flow;
        flow.keys = KeyEnumeration::NeverEnumerated;
        for (AllocationClassID allocationClass : flow.allocationClasses)
            results.tables.sites.allocationClasses[allocationClass.index()].keys = KeyEnumeration::NeverEnumerated;
    }
}

// The objects of several layouts may hold one field. It is an accessor where they all say so, in one form.
static void uniteAccessors(FlowAccessorField& known, FlowAccessorField&& other, bool isFirst)
{
    if (isFirst) {
        known = WTF::move(other);
        return;
    }
    if (known.form != other.form) {
        known = { };
        return;
    }
    if (known.getters.isEmpty() || other.getters.isEmpty()) {
        known.getters.clear();
        return;
    }
    for (FunctionID getter : other.getters)
        insertInUniquedSortedVector(known.getters, getter);
}

static ValueFlowFieldContents computeDeclaredFields(const FieldResults& results, const UsesOfObjects& uses, std::span<const AllocatingOperation> allocatingOperations)
{
    // What the objects of one layout say about a field that they hold.
    struct HeldField {
        FieldReference declaration;
        OptionSet<ObjectMutation> possibleMutations;
        bool mayBeObserved { false };
        bool mayBeCalled { false };
        bool mayBeAccessed { false };
        bool isStoredAfterCreation { false };
        bool isAskedForAccessor { false };
        bool isAskedForStoredValues { false };
        FlowAccessorField accessor;
        ValueSetID stored { ValueSets::nothing };
        ValueFlowNumbers storedNumbers;
    };

    const ValueFlowResultInputs& inputs = results.inputs;
    const Program& program = inputs.program;
    const ParallelLoops& loops = inputs.loops();
    BuildContext& build = inputs.build;
    FlowApplication& application = results.application;
    const StoresByLayout& stores = results.stores;
    uint32_t numberOfLayouts = program.layouts.size();

    ValueFlowFieldContents contents;
    contents.stored = Vector<Vector<ValueSetID>>(numberOfLayouts);
    contents.storedNumbers = Vector<Vector<ValueFlowNumbers>>(numberOfLayouts);
    contents.storedInAppendedFields = Vector<Vector<ValueSetID>>(numberOfLayouts);
    Vector<Vector<OptionSet<ObjectMutation>>> possibleMutations(numberOfLayouts);
    forEachIndexInParallel(loops, numberOfLayouts, [&](unsigned index) {
        size_t numberOfFields = program.layouts[index].fields.size();
        contents.stored[index] = Vector<ValueSetID>(FillWith { }, numberOfFields, ValueSets::nothing);
        contents.storedNumbers[index] = Vector<ValueFlowNumbers>(numberOfFields);
        possibleMutations[index] = Vector<OptionSet<ObjectMutation>>(numberOfFields);
    });
    Vector<BitVector> hasObjects(numberOfLayouts, [&](size_t index) {
        return BitVector(program.layouts[index].fields.size());
    });
    Vector<BitVector> isMissingAtCreation = hasObjects;
    Vector<BitVector> mayBeObserved = hasObjects;
    Vector<BitVector> mayBeCalled = hasObjects;
    Vector<BitVector> mayBeAccessed = hasObjects;
    Vector<BitVector> isStoredAfterCreation = hasObjects;
    Vector<BitVector> isAskedForAccessor = hasObjects;
    noteFieldsMissingAtCreation(results, allocatingOperations, isMissingAtCreation);
    ValueSetMakers makers(inputs);

    // What happens to the objects of a layout happens to every field that they hold.
    Vector<Vector<HeldField>> fieldsHeldByObjectsWith(numberOfLayouts);
    Vector<ObjectsOfLayoutInHeap::Chains> chainsOfWorker(loops.width());
    forEachIndexInParallel(loops, numberOfLayouts, [&](unsigned index, unsigned worker) {
        LayoutID id(index);
        if (!results.hasObjects(id))
            return;
        ValueSetMaker& maker = makers.of(worker);
        ObjectsOfLayoutInHeap inHeap(results, maker, chainsOfWorker[worker], id);
        OptionSet<ObjectMutation> possible = stores.getsCopiedProperties(id) ? allObjectMutations : results.possibleMutationsOf(id);
        bool isSeenByOthers = !isSeenByFollowedCodeAlone(results, id);
        bool isDeletedByStatementsAlone = !isSeenByOthers || (results.heap.reaches(inputs.universe().unknownObjectAlone(), inHeap.objects()) && !stores.hasObjectOfCodeWithoutSolve(id) && !possible.contains(ObjectMutation::ReachUnknownJavaScript));
        ValueSetID objects = inHeap.objects();
        // Exact for a layout without a proven base.
        fieldsHeldByObjectsWith[index].reserveInitialCapacity(program.layouts[index].fields.size());
        forEachFieldOfObjectsWith(program, application, id, [&](FieldReference declaration, const Field& field) {
            HeldField held { .declaration = declaration };
            OptionSet<ObjectMutation> possibleForField = possible;
            // A deletion takes the field that it names and leaves the others in their slots. Which keys are named is known of the objects that no unknown JavaScript gets hold of.
            if (field.key && isDeletedByStatementsAlone && !stores.getsCopiedProperties(id) && !inHeap.mayBeDeletedUnder(field.key))
                possibleForField.remove(ObjectMutation::DeleteField);
            // So does a redefinition.
            if (field.key && !isSeenByOthers && !stores.getsCopiedProperties(id) && !inHeap.mayBeRedefinedUnder(field.key))
                possibleForField.remove(ObjectMutation::RedefineProperty);
            held.possibleMutations = possibleForField;
            if (!field.key) {
                held.possibleMutations = allObjectMutations;
                held.mayBeObserved = true;
                fieldsHeldByObjectsWith[index].append(WTF::move(held));
                return;
            }
            held.mayBeObserved = isSeenByOthers || uses.mayBeObservedUnder(objects, field.key) || inHeap.isObservedByStoresOfHeirs(field.key);
            held.mayBeCalled = uses.mayBeCalledUnder(objects, field.key);
            held.mayBeAccessed = uses.mayBeAccessedUnder(objects, field.key);
            held.isStoredAfterCreation = stores.isStoredAfterCreation(id, field.key);
            if (!hasNodeOfItsOwn(field) && build.isEnabled(FlowResultKind::AppendedFields, declaration.layout.index())) {
                held.isAskedForAccessor = true;
                held.accessor = stores.accessorDefinedUnder(maker, id, field.key);
            }
            if (!hasNodeOfItsOwn(field)) {
                held.isAskedForStoredValues = true;
                held.stored = stores.storedUnder(maker, id, field.key);
                held.storedNumbers = stores.numbersStoredUnder(id, field.key);
            }
            fieldsHeldByObjectsWith[index].append(WTF::move(held));
        });
    });
    chainsOfWorker.clear();

    for (Vector<HeldField>& heldFields : fieldsHeldByObjectsWith) {
        for (HeldField& held : heldFields) {
            uint32_t layout = held.declaration.layout.index();
            uint32_t field = held.declaration.field;
            auto note = [&](bool holds, Vector<BitVector>& fields) {
                if (holds)
                    fields[layout].quickSet(field);
            };
            hasObjects[layout].quickSet(field);
            note(held.mayBeObserved, mayBeObserved);
            note(held.mayBeCalled, mayBeCalled);
            note(held.mayBeAccessed, mayBeAccessed);
            note(held.isStoredAfterCreation, isStoredAfterCreation);
            possibleMutations[layout][field].add(held.possibleMutations);
            if (held.isAskedForAccessor)
                uniteAccessors(application.layouts[layout].fields[field].accessor, WTF::move(held.accessor), !isAskedForAccessor[layout].quickSet(field));
            if (held.isAskedForStoredValues) {
                ValueSetID& stored = contents.stored[layout][field];
                stored = makers.of(0).unite(stored, held.stored);
                contents.storedNumbers[layout][field].unite(held.storedNumbers);
            }
        }
        heldFields.clear();
    }

    forEachIndexInParallel(loops, numberOfLayouts, [&](unsigned index) {
        const Layout& layout = program.layouts[index];
        if (!hasObjectsThatCodeAlters(layout.kind))
            return;
        for (uint32_t field = 0; field < layout.fields.size(); ++field) {
            if (!hasObjects[index].quickGet(field) || layout.fields[field].flags.contains(FieldFlags::IsInherited))
                continue;
            if (hasNodeOfItsOwn(layout.fields[field])) {
                contents.stored[index][field] = inputs.solver().valueOf(inputs.bodies().nodes().field(LayoutID(index), field));
                contents.storedNumbers[index][field] = ValueFlowNumbers::any();
            }

            OptionSet<ObjectMutation> possible = possibleMutations[index][field];
            FieldFlow& flow = application.layouts[index].fields[field].flow;
            if (!possible.contains(ObjectMutation::ReachUnknownJavaScript) && build.isEnabled(FlowResultKind::StoreCounts, index))
                flow.location.writers = WriterState::AllKnown;
            if (build.isEnabled(FlowResultKind::HolderMutations, index)) {
                if (!possible.contains(ObjectMutation::DeleteField))
                    flow.stability.add(FieldStability::IsNeverDeleted);
                if (!possible.contains(ObjectMutation::RedefineProperty))
                    flow.stability.add(FieldStability::IsNeverRedefined);
            }
            // Which allocations and constructors fill a field is read from them.
            if (!isMissingAtCreation[index].quickGet(field) && flow.location.writers == WriterState::AllKnown && build.isEnabled(FlowResultKind::Initialization, index))
                flow.location.initialization = InitializationState::SetAtCreation;
            // Nothing puts a value there but the operation that allocates the object. A field that no allocation lists either is never stored, which the results about types say.
            bool isChangedAfterCreation = hasNodeOfItsOwn(layout.fields[field]) || isStoredAfterCreation[index].quickGet(field) || possible.containsAny({ ObjectMutation::DeleteField, ObjectMutation::RedefineProperty });
            if (!isChangedAfterCreation && flow.location.writers == WriterState::AllKnown) {
                flow.location.stores = StoreCount::AtMostOnce;
                flow.location.isUnchangedDuringActivation = true;
            }

            bool isNeverObserved = !hasNodeOfItsOwn(layout.fields[field]) && !mayBeObserved[index].quickGet(field);
            // A field that nobody reads or stores gets no slot. An access without a stamp names every layout that the type of its receiver admits, also those that never arrive there.
            if (isNeverObserved && flow.location.writers == WriterState::AllKnown && build.isEnabled(FlowResultKind::ObjectUses, index) && build.isEnabledEverywhere(FlowResultKind::ReceiverSets)) {
                bool isCalled = mayBeCalled[index].quickGet(field);
                bool isAccessed = mayBeAccessed[index].quickGet(field);
                flow.location.reads = isCalled && isAccessed ? ReadState::Unknown : isCalled ? ReadState::OnlyCalled : isAccessed ? ReadState::OnlyAccessed : ReadState::NeverRead;
            }
        }
    });
    return contents;
}

static bool canAppendFieldsTo(const FieldResults& results, LayoutID id)
{
    switch (results.program().at(id).kind) {
    case LayoutKind::Record:
    case LayoutKind::Instance:
        break;
    case LayoutKind::Container:
    case LayoutKind::Environment:
    case LayoutKind::Closure:
    case LayoutKind::CoroutineFrame:
    case LayoutKind::BuiltinResult:
    case LayoutKind::ModuleNamespace:
        return false;
    }
    // A derived layout has the fields of the layout it was derived from, and that one takes the names.
    if (results.program().at(id).isDerived() || !results.hasObjects(id) || results.application.layouts[id.index()].flow.allocationClasses.isEmpty())
        return false;
    return results.inputs.build.isEnabled(FlowResultKind::AppendedFields, id.index());
}

static bool canNameField(const Program& program, const BitVector& symbolsCreatedOnce, PropertyKeyID key)
{
    switch (program.at(key).kind) {
    case PropertyKeyKind::Text:
        return !program.isIndexLike(key);
    case PropertyKeyKind::WellKnownSymbol:
    case PropertyKeyKind::RegisteredSymbol:
        return true;
    case PropertyKeyKind::CreatedSymbol:
        // The key of a call that may run again stands for several symbols, and a field has one key.
        return symbolsCreatedOnce.get(key.index());
    case PropertyKeyKind::EnginePrivateName:
    case PropertyKeyKind::PrivateName:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

namespace {

// How many operations of reached functions name each key. Counted when the first layout has more names than it may keep.
class AccessCounts {
public:
    explicit AccessCounts(const ValueFlowResultInputs& inputs)
        : m_inputs(inputs)
    {
    }

    uint32_t of(PropertyKeyID key)
    {
        if (!m_isCounted)
            count();
        return m_counts[key.index()];
    }

private:
    void count()
    {
        m_isCounted = true;
        size_t numberOfKeys = m_inputs.program.types.numberOfPropertyKeys();
        Vector<Vector<uint32_t>> countsOfWorker(m_inputs.loops().width());
        forEachIndexInParallel(m_inputs.loops(), m_inputs.program.functions.size(), [&](unsigned index, unsigned worker) {
            if (!m_inputs.isReached(FunctionID(index)))
                return;
            Vector<uint32_t>& counts = countsOfWorker[worker];
            if (counts.isEmpty())
                counts.fill(0, numberOfKeys);
            for (const Value& value : m_inputs.program.functions[index].values) {
                if (Value::opcodesWithKey.contains(value.opcode) && value.key())
                    ++counts[value.key().index()];
            }
        });
        m_counts.fill(0, numberOfKeys);
        for (const Vector<uint32_t>& counts : countsOfWorker) {
            for (size_t key = 0; key < counts.size(); ++key)
                m_counts[key] += counts[key];
        }
    }

    const ValueFlowResultInputs& m_inputs;
    Vector<uint32_t> m_counts;
    bool m_isCounted { false };
};

// The layouts by the proven chains of bases.
struct ProvenHierarchy {
    ProvenHierarchy(const Program& program, const FlowApplication& application)
        : layoutsDirectlyBelow(program.layouts.size())
    {
        Vector<uint32_t> depth(FillWith { }, program.layouts.size(), 0u);
        uint32_t greatestDepth = 0;
        for (uint32_t index = 0; index < program.layouts.size(); ++index) {
            if (LayoutID base = provenBaseOf(application, LayoutID(index)))
                layoutsDirectlyBelow[base.index()].append(LayoutID(index));
            LayoutID current = provenBaseOf(application, LayoutID(index));
            for (size_t steps = 0; current && steps < program.layouts.size(); ++steps) {
                ++depth[index];
                current = provenBaseOf(application, current);
            }
            greatestDepth = std::max(greatestDepth, depth[index]);
        }
        // By depth, and at one depth in the order of the layouts.
        Vector<uint32_t> nextAtDepth(FillWith { }, greatestDepth + 2, 0u);
        for (uint32_t depthOfLayout : depth)
            ++nextAtDepth[depthOfLayout + 1];
        for (uint32_t atDepth = 0; atDepth <= greatestDepth; ++atDepth)
            nextAtDepth[atDepth + 1] += nextAtDepth[atDepth];
        fromBasesDown.grow(program.layouts.size());
        for (uint32_t index = 0; index < program.layouts.size(); ++index)
            fromBasesDown[nextAtDepth[depth[index]]++] = LayoutID(index);
    }

    void forEachLayoutBelow(LayoutID layout, NOESCAPE const Invocable<void(LayoutID)> auto& functor) const
    {
        Vector<LayoutID, 8> worklist { layout };
        while (!worklist.isEmpty()) {
            LayoutID current = worklist.takeLast();
            functor(current);
            worklist.appendVector(layoutsDirectlyBelow[current.index()]);
        }
    }

    Vector<Vector<LayoutID>> layoutsDirectlyBelow;
    Vector<LayoutID> fromBasesDown;
};

} // anonymous namespace

// One object of the universe stands for the instances of a class and of the classes below it, so what a class below stores into its own field seems to be stored into the instances of this class too.
static Vector<Vector<PropertyKeyID>> collectKeysDeclaredBelow(const Program& program, const BitVector& isCandidate)
{
    Vector<Vector<PropertyKeyID>> keysDeclaredBelow(program.layouts.size());
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        Vector<PropertyKeyID, 8> ownKeys;
        for (const Field& field : program.layouts[index].fields) {
            if (field.key && !field.flags.contains(FieldFlags::IsInherited) && isCandidate.quickGet(field.key.index()))
                ownKeys.append(field.key);
        }
        if (ownKeys.isEmpty())
            continue;
        LayoutID above = layoutExpectedAbove(program, LayoutID(index));
        for (size_t steps = 0; above && steps < program.layouts.size(); ++steps) {
            keysDeclaredBelow[above.index()].appendVector(ownKeys);
            above = layoutExpectedAbove(program, above);
        }
    }
    for (Vector<PropertyKeyID>& keys : keysDeclaredBelow) {
        AOT::sort(keys);
        removeRepeatedElements(keys);
    }
    return keysDeclaredBelow;
}

using LayoutsByKey = HashMap<uint32_t, Vector<LayoutID>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;

// The layouts that have a field under each of the names, ascending. A family of a large program has thousands of members, and few names are asked about.
static LayoutsByKey collectLayoutsWithFields(const Program& program, const BitVector& isCandidate)
{
    LayoutsByKey layoutsWithField;
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        for (const Field& field : program.layouts[index].fields) {
            if (!field.key || !isCandidate.quickGet(field.key.index()))
                continue;
            Vector<LayoutID>& layouts = layoutsWithField.add(field.key.index(), Vector<LayoutID>()).iterator->value;
            if (layouts.isEmpty() || layouts.last() != LayoutID(index))
                layouts.append(LayoutID(index));
        }
    }
    return layoutsWithField;
}

// A store that finds an accessor above the object runs the setter, or does nothing where there is none: it fills no slot.
static bool findsAccessorAbove(const FieldResults& results, ValueSetMaker& maker, LayoutID id, PropertyKeyID key)
{
    const ValueSets& sets = results.inputs.sets();
    // Few names have an accessor anywhere in the program. The others ask for no chain.
    Vector<ValueSetID, 4> receiversOfDefinitions;
    for (ValueFlowName name : { ValueFlowName::getter(key), ValueFlowName::setter(key) }) {
        results.heap.forEachNodeWrittenUnder(name, [&](ValueSetID writtenThrough, ValueSetID value) {
            if (!sets.isEmpty(value))
                receiversOfDefinitions.append(writtenThrough);
        });
    }
    if (receiversOfDefinitions.isEmpty())
        return false;

    ValueSetID objects = results.inputs.universe().objectsWith(id);
    ValueSetID above = maker.intersect(results.chainBuiltByProgram(objects), ValueSets::complement(objects));
    return std::ranges::any_of(receiversOfDefinitions, [&](ValueSetID writtenThrough) {
        return !sets.isEmpty(maker.intersect(writtenThrough, above));
    });
}

static void removeKeysWithoutRoom(const FieldResults& results, ValueSetMaker& maker, const BitVector& symbolsCreatedOnce, std::span<const PropertyKeyID> keysDeclaredBelow, const LayoutsByKey& layoutsWithField, std::span<const LayoutID> declaredViews, LayoutID id, Vector<PropertyKeyID>& keys)
{
    const Program& program = results.program();
    // Where the program declares types for the objects, a name that none of them has gets no room: neither a layout of their families nor a view through which the program may see the objects.
    Vector<LayoutSetID, 2> families = declaredFamiliesOf(program, id);
    keys.removeAllMatching([&](PropertyKeyID key) {
        auto declarers = layoutsWithField.find(key.index());
        // The type of `Symbol.for("name")` is `symbol`, so no type has a member under it.
        PropertyKeyKind kind = program.at(key).kind;
        bool canBeDeclared = kind != PropertyKeyKind::CreatedSymbol && kind != PropertyKeyKind::RegisteredSymbol;
        bool isOutsideFamilies = canBeDeclared && !families.isEmpty() && (declarers == layoutsWithField.end() || (!haveCommonMember(declaredViews, declarers->value.span()) && std::ranges::none_of(families, [&](LayoutSetID family) {
            return haveCommonMember(program.at(family).members.span(), declarers->value.span());
        })));
        if (isOutsideFamilies || std::ranges::binary_search(keysDeclaredBelow, key))
            return true;
        if (!canNameField(program, symbolsCreatedOnce, key) || declaresFieldOnChain(program, results.application, id, key))
            return true;
        if (results.stores.accessorDefinedUnder(maker, id, key).isAccessor())
            return false;
        return results.stores.mayGetAccessorUnder(id, key) || results.inputs.sets().isEmpty(std::as_const(results.stores).storedUnder(maker, id, key)) || findsAccessorAbove(results, maker, id, key);
    });
}

// The names that the program mentions least go to the extra properties.
static void keepMostAccessedKeys(const FieldResults& results, AccessCounts& counts, LimitID limit, LayoutID layout, uint64_t numberOfOtherFields, Vector<PropertyKeyID>& keys)
{
    BuildContext& build = results.inputs.build;
    if (keys.isEmpty() || !build.exceeds(limit, numberOfOtherFields + keys.size(), Subject { .layout = layout }))
        return;
    uint64_t allowed = build.limit(limit);
    size_t numberOfKeysKept = allowed > numberOfOtherFields ? allowed - numberOfOtherFields : 0;
    auto rankOf = [&](PropertyKeyID key) {
        return std::pair { results.stores.isStoredByConstructor(layout, key), counts.of(key) };
    };
    std::ranges::nth_element(keys, keys.begin() + numberOfKeysKept, [&](PropertyKeyID a, PropertyKeyID b) {
        return std::pair { rankOf(b), a } < std::pair { rankOf(a), b };
    });
    keys.shrink(numberOfKeysKept);
    AOT::sort(keys);
}

// The names of the accessors that the objects of the layout get.
static Vector<PropertyKeyID> keysOfAccessorsIn(const FieldResults& results, ValueSetMaker& maker, LayoutID id)
{
    Vector<PropertyKeyID> keys = results.stores.keysOfAccessorsDefinedIn(maker, id);
    if (!results.stores.getsAccessorsUnderKeysNobodyKnows(id))
        return keys;
    // A name that the program puts above the objects is more likely read from there.
    const Program& program = results.program();
    const ValueFlowUniverse& universe = results.inputs.universe();
    const ValueSets& sets = results.inputs.sets();
    const ValueFlowHeapView& heap = results.heap;
    ValueSetID objects = results.layoutsOfObjects.objectsWith(id);
    ValueSetID above = maker.intersect(results.chainBuiltByProgram(objects), ValueSets::complement(objects));
    auto liesAbove = [&](PropertyKeyID key) {
        if (!sets.isEmpty(heap.writtenUnder(maker, above, ValueFlowName::property(key))) || !sets.isEmpty(heap.writtenUnder(maker, above, ValueFlowName::getter(key))))
            return true;
        BuiltinName name = program.builtinNameOf(key);
        if (!name)
            return false;
        bool isMemberOfBuiltin = false;
        sets.forEachMember(above, [&](ValueFlowObjectID holder) {
            if (BuiltinID builtin = universe.builtinOf(holder))
                isMemberOfBuiltin |= !!program.builtins().findOwnMember(builtin, name);
        });
        return isMemberOfBuiltin;
    };
    Vector<PropertyKeyID> read(results.stores.keysReadFrom(id));
    read.removeAllMatching([&](PropertyKeyID key) {
        return !results.stores.accessorDefinedUnder(maker, id, key).isAccessor() || liesAbove(key);
    });
    uniteSortedKeys(keys, read);
    return keys;
}

static Vector<Vector<PropertyKeyID>> chooseAppendedKeys(const FieldResults& results, const ProvenHierarchy& hierarchy)
{
    const Program& program = results.program();
    uint32_t numberOfLayouts = program.layouts.size();
    BitVector noSymbols;
    const BitVector& symbolsCreatedOnce = results.inputs.symbolsCreatedOnce ? *results.inputs.symbolsCreatedOnce : noSymbols;

    const ParallelLoops& loops = results.inputs.loops();
    ValueSetMakers makers(results.inputs);
    Vector<Vector<PropertyKeyID>> keysByLayout(numberOfLayouts);
    Vector<uint64_t> fieldCountsAsAnalyzed(FillWith { }, numberOfLayouts, static_cast<uint64_t>(0));
    forEachIndexInParallel(loops, numberOfLayouts, [&](unsigned index, unsigned worker) {
        LayoutID current(index);
        for (size_t steps = 0; current && steps < numberOfLayouts; ++steps) {
            fieldCountsAsAnalyzed[index] += program.at(current).fields.size();
            current = provenBaseOf(results.application, current);
        }
        if (!canAppendFieldsTo(results, LayoutID(index)))
            return;
        // What only a store through a placeholder, the code of a built-in or a store under a key that may be one of several puts there goes to the extra properties.
        Vector<PropertyKeyID> accessors = keysOfAccessorsIn(results, makers.of(worker), LayoutID(index));
        // What Object.create makes stays an ordinary object unless it is made to hold accessors.
        if (accessors.isEmpty() && program.layouts[index].has(LayoutFlags::IsOfCreatedObject))
            return;
        keysByLayout[index] = results.stores.keysNamedByStoresIntoKnownObjects(LayoutID(index));
        uniteSortedKeys(keysByLayout[index], accessors);
    });
    BitVector isCandidate(program.types.numberOfPropertyKeys());
    for (const Vector<PropertyKeyID>& keys : keysByLayout) {
        for (PropertyKeyID key : keys)
            isCandidate.quickSet(key.index());
    }
    Vector<Vector<PropertyKeyID>> keysDeclaredBelow;
    LayoutsByKey layoutsWithField;
    Vector<Vector<LayoutID>> declaredViews;
    runTasksInParallel(loops, {
        [&] {
            keysDeclaredBelow = collectKeysDeclaredBelow(program, isCandidate);
        },
        [&] {
            layoutsWithField = collectLayoutsWithFields(program, isCandidate);
        },
        [&] {
            BitVector declaredLayoutsWithKeys(numberOfLayouts);
            for (uint32_t index = 0; index < numberOfLayouts; ++index) {
                if (!keysByLayout[index].isEmpty())
                    declaredLayoutsWithKeys.quickSet(declaredLayoutOf(program, LayoutID(index)).index());
            }
            declaredViews = declaredViewsOf(program, declaredLayoutsWithKeys);
        },
    });
    Vector<uint32_t> layoutsWithKeys;
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        if (!keysByLayout[index].isEmpty())
            layoutsWithKeys.append(index);
    }
    AOT::stableSort(layoutsWithKeys, std::greater { }, [&](uint32_t index) {
        return keysByLayout[index].size();
    });
    forEachIndexInParallel(loops, layoutsWithKeys.size(), [&](unsigned position, unsigned worker) {
        uint32_t index = layoutsWithKeys[position];
        removeKeysWithoutRoom(results, makers.of(worker), symbolsCreatedOnce, keysDeclaredBelow[index].span(), layoutsWithField, declaredViews[declaredLayoutOf(program, LayoutID(index)).index()].span(), LayoutID(index), keysByLayout[index]);
    });

    AccessCounts counts(results.inputs);
    for (LayoutID layout : hierarchy.fromBasesDown) {
        Vector<PropertyKeyID>& keys = keysByLayout[layout.index()];
        if (keys.isEmpty())
            continue;
        uint64_t numberOfInheritedKeys = 0;
        LayoutID ancestor = provenBaseOf(results.application, layout);
        for (size_t steps = 0; ancestor && steps < numberOfLayouts; ++steps) {
            const Vector<PropertyKeyID>& inheritedKeys = keysByLayout[ancestor.index()];
            numberOfInheritedKeys += inheritedKeys.size();
            keys.removeAllMatching([&](PropertyKeyID key) {
                return std::ranges::binary_search(inheritedKeys, key);
            });
            ancestor = provenBaseOf(results.application, ancestor);
        }
        // A layout below inherits the appended fields on top of the fields it has.
        uint64_t largestFieldCountBelow = 0;
        hierarchy.forEachLayoutBelow(layout, [&](LayoutID below) {
            largestFieldCountBelow = std::max(largestFieldCountBelow, fieldCountsAsAnalyzed[below.index()]);
        });
        // The slot of an accessor is a narrow reference, and the objects that hold many of them are few.
        Vector<PropertyKeyID> accessors;
        keys.removeAllMatching([&](PropertyKeyID key) {
            bool isAccessor = results.stores.accessorDefinedUnder(layout, key).isAccessor();
            if (isAccessor)
                accessors.append(key);
            return isAccessor;
        });
        keepMostAccessedKeys(results, counts, LimitID::MaxAppendedFieldsPerLayout, layout, 0, keys);
        uniteSortedKeys(keys, accessors);
        keepMostAccessedKeys(results, counts, LimitID::FieldsInKeyOrder, layout, largestFieldCountBelow + numberOfInheritedKeys, keys);
    }
    return keysByLayout;
}

// A name that followed code stores into the objects of a layout and that the layout does not declare becomes a field that no allocation fills.
static void appendFields(const FieldResults& results, const ProvenHierarchy& hierarchy, std::span<const Vector<PropertyKeyID>> keysByLayout, ValueFlowFieldContents& contents)
{
    BuildContext& build = results.inputs.build;
    const StoresByLayout& stores = results.stores;
    ValueSetMakers makers(results.inputs);
    forEachIndexInParallel(results.inputs.loops(), keysByLayout.size(), [&](unsigned index, unsigned worker) {
        if (keysByLayout[index].isEmpty())
            return;
        ValueSetMaker& maker = makers.of(worker);
        FlowLayoutUpdate& update = results.application.layouts[index];
        for (PropertyKeyID key : keysByLayout[index]) {
            OptionSet<ObjectMutation> possible;
            ValueSetID stored = ValueSets::nothing;
            bool isStoredBeforeObjectsLeak = true;
            hierarchy.forEachLayoutBelow(LayoutID(index), [&](LayoutID below) {
                if (!results.hasObjects(below))
                    return;
                possible.add(stores.getsCopiedProperties(below) ? allObjectMutations : results.possibleMutationsOf(below));
                stored = maker.unite(stored, stores.storedUnder(maker, below, key));
                isStoredBeforeObjectsLeak &= results.constructions.isFollowed(below) && results.constructions.isStoredBeforeObjectLeaks(below, key, { });
            });

            FlowAppendedField field;
            field.key = key;
            bool isFirst = true;
            hierarchy.forEachLayoutBelow(LayoutID(index), [&](LayoutID below) {
                if (results.hasObjects(below))
                    uniteAccessors(field.accessor, stores.accessorDefinedUnder(maker, below, key), std::exchange(isFirst, false));
            });
            if (!possible.contains(ObjectMutation::ReachUnknownJavaScript) && build.isEnabled(FlowResultKind::StoreCounts, index))
                field.flow.location.writers = WriterState::AllKnown;
            if (isStoredBeforeObjectsLeak && field.flow.location.writers == WriterState::AllKnown && build.isEnabled(FlowResultKind::Initialization, index))
                field.flow.location.initialization = InitializationState::SetAtCreation;
            if (build.isEnabled(FlowResultKind::HolderMutations, index)) {
                if (!possible.contains(ObjectMutation::DeleteField))
                    field.flow.stability.add(FieldStability::IsNeverDeleted);
                if (!possible.contains(ObjectMutation::RedefineProperty))
                    field.flow.stability.add(FieldStability::IsNeverRedefined);
            }
            update.appendedFields.append(WTF::move(field));
            contents.storedInAppendedFields[index].append(stored);
        }
    });
    uint64_t numberOfAppendedFields = 0;
    for (const Vector<PropertyKeyID>& keys : keysByLayout)
        numberOfAppendedFields += keys.size();
    build.statistics().count(StatisticID::FlowAppendedFields, numberOfAppendedFields);
}

static bool isFieldOrAppendedField(const FieldResults& results, std::span<const Vector<PropertyKeyID>> appendedKeys, LayoutID id, PropertyKeyID key)
{
    const Program& program = results.program();
    LayoutID current = id;
    for (size_t steps = 0; current && steps < program.layouts.size(); ++steps) {
        if (program.at(current).findField(key) || std::ranges::binary_search(appendedKeys[current.index()], key))
            return true;
        current = provenBaseOf(results.application, current);
    }
    return false;
}

// What native code constructs it may fill with properties that no node takes: the species of an array gets a length.
static BitVector findLayoutsConstructedByNativeCode(const FieldResults& results)
{
    const ValueFlowSolver& solver = results.inputs.solver();
    BitVector layouts(results.program().layouts.size());
    Vector<ValueSetID> resultsOfConstructions = collectFromCallGroups<ValueSetID>(results.inputs, results.heap, [&](const ValueFlowNodeKey& key, ValueFlowNodeID first, Vector<ValueSetID>& sets) {
        if (key.callKind() == ValueFlowCallKind::ConstructByNativeCode)
            sets.append(solver.valueOf(callGroupAt(first).result));
    });
    if (resultsOfConstructions.isEmpty())
        return layouts;

    AOT::sort(resultsOfConstructions);
    removeRepeatedElements(resultsOfConstructions);
    ValueSetMaker& maker = results.heap.maker();
    ValueSetID made = maker.intersect(results.heap.expand(maker.uniteAll(resultsOfConstructions.span())), results.inputs.universe().knownObjects());
    if (ValueSets::isComplement(made)) {
        for (size_t layout = 0; layout < results.program().layouts.size(); ++layout)
            layouts.quickSet(layout);
        return layouts;
    }
    results.layoutsOfObjects.forEachLayoutOfMembers(made, [&](LayoutID layout) {
        layouts.quickSet(layout.index());
    });
    return layouts;
}

// The rules mark an object as soon as something is stored into it under a name that the layout of its site does not declare.
static void computeAddedKeys(const FieldResults& results, std::span<const Vector<PropertyKeyID>> appendedKeys)
{
    const Program& program = results.program();
    BuildContext& build = results.inputs.build;
    BitVector isConstructedByNativeCode = findLayoutsConstructedByNativeCode(results);
    const Vector<PropertyKeyID>& sharedKeys = results.stores.keysStoredThroughPlaceholders();
    results.tables.wholeProgram.keysStoredThroughUnknownReceivers = sharedKeys;
    const Vector<PropertyKeyID>& keysOfSeveralSymbols = results.tables.wholeProgram.keysOfSeveralSymbols;
    // A layout has its allocation classes to itself.
    forEachIndexInParallel(results.inputs.loops(), program.layouts.size(), [&](unsigned index) {
        LayoutID id(index);
        LayoutFlow& flow = results.application.layouts[index].flow;
        LayoutKind kind = program.layouts[index].kind;
        if (!results.hasObjects(id) || flow.allocationClasses.isEmpty() || (kind != LayoutKind::Record && kind != LayoutKind::Instance))
            return;

        bool listsEveryName = !results.stores.getsNameNobodyLists(id) && !isConstructedByNativeCode.quickGet(index)
            && !results.possibleMutationsOf(id).contains(ObjectMutation::ReachUnknownJavaScript);
        Vector<PropertyKeyID> addedKeys;
        bool mayGainSharedKey = false;
        if (listsEveryName) {
            auto isField = [&](PropertyKeyID key) {
                return !std::ranges::binary_search(keysOfSeveralSymbols, key) && isFieldOrAppendedField(results, appendedKeys, id, key);
            };
            addedKeys = results.stores.keysStoredIntoKnownObjects(id);
            addedKeys.removeAllMatching(isField);
            mayGainSharedKey = results.stores.isReachedThroughPlaceholders(id) && !std::ranges::all_of(sharedKeys, isField);
        }
        bool mayAddProperty = !listsEveryName || !addedKeys.isEmpty() || mayGainSharedKey;

        auto apply = [&](OptionSet<ObjectMutation>& impossibleMutations, Vector<PropertyKeyID>& keys, bool& keysAreComplete, bool& isReachedThroughUnknownReceivers) {
            impossibleMutations.set(ObjectMutation::AddProperty, !mayAddProperty && build.isEnabled(FlowResultKind::HolderMutations, index));
            if (!build.isEnabled(FlowResultKind::OwnNames, index))
                return;
            keys = addedKeys;
            keysAreComplete = listsEveryName;
            isReachedThroughUnknownReceivers = mayGainSharedKey;
        };
        apply(flow.impossibleMutations, flow.addedKeys, flow.addedKeysAreComplete, flow.isReachedThroughUnknownReceivers);
        for (AllocationClassID allocationClass : flow.allocationClasses) {
            AllocationClass& record = results.tables.sites.allocationClasses[allocationClass.index()];
            apply(record.impossibleMutations, record.addedKeys, record.addedKeysAreComplete, record.isReachedThroughUnknownReceivers);
        }
    });
}

// The layouts with an object that neither an allocation with the layout nor constant data makes. No order lists the keys of such an object.
static BitVector findLayoutsOfObjectsWithoutListedOrder(const FieldResults& results)
{
    const ValueFlowResultInputs& inputs = results.inputs;
    const Program& program = inputs.program;
    const ValueFlowUniverse& universe = inputs.universe();
    Vector<LayoutID> layoutsOfSites = collectInParallel<LayoutID>(inputs.loops(), universe.numberOfObjects(), [&](unsigned index, Vector<LayoutID>& found) {
        ValueFlowObjectID object(index);
        if (universe.kindOf(object) != ValueFlowObjectKind::Site || !universe.layoutOf(object) || isSiteOfTypedResultThatNothingAllocates(inputs, object))
            return;
        const ValueFlowSite& site = *universe.siteOf(object);
        bool isAllocation = site.origin == ValueFlowSiteOrigin::Operation && !site.ordinal && program.at(universe.functionOf(object)).at(site.value).opcode == Opcode::NewObject;
        if (!isAllocation && site.origin != ValueFlowSiteOrigin::DataNode)
            found.append(site.layout);
    });
    BitVector layouts(program.layouts.size());
    for (LayoutID layout : layoutsOfSites) {
        layouts.quickSet(layout.index());
        layouts.quickSet(declaredLayoutOf(program, layout).index());
    }
    return layouts;
}

// Nothing creates a field behind the construction of an object: no name is added, and no field that the construction may leave out is stored later.
static FieldCreation ownFieldCreationOf(const FieldResults& results, const BitVector& layoutsOfObjectsWithoutListedOrder, LayoutID id)
{
    const Program& program = results.program();
    const Layout& layout = program.at(id);
    switch (layout.kind) {
    case LayoutKind::Record:
        if (layoutsOfObjectsWithoutListedOrder.quickGet(id.index()))
            return FieldCreation::Unknown;
        break;
    case LayoutKind::Instance:
        // The construction speaks for the constructors and their helpers alone, and a method may create a field that they left out.
        if (!results.constructions.createsFieldsOnlyInListedOrders(id))
            return FieldCreation::Unknown;
        break;
    case LayoutKind::Container:
    case LayoutKind::Environment:
    case LayoutKind::Closure:
    case LayoutKind::CoroutineFrame:
    case LayoutKind::BuiltinResult:
    case LayoutKind::ModuleNamespace:
        return FieldCreation::Unknown;
    }
    if (results.possibleMutationsOf(id).contains(ObjectMutation::AddProperty) || !results.application.layouts[id.index()].appendedFields.isEmpty())
        return FieldCreation::Unknown;

    for (uint32_t index = 0; index < layout.fields.size(); ++index) {
        const Field& field = layout.fields[index];
        if (field.flags.contains(FieldFlags::IsInherited))
            continue;
        bool isFilledByConstruction = results.application.layouts[id.index()].fields[index].flow.location.initialization == InitializationState::SetAtCreation;
        if (!isFilledByConstruction && (!field.key || hasNodeOfItsOwn(field) || results.stores.isStoredAfterCreation(id, field.key)))
            return FieldCreation::Unknown;
    }
    return FieldCreation::OnlyAtConstruction;
}

static void computeFieldCreations(const FieldResults& results)
{
    const Program& program = results.program();
    const ParallelLoops& loops = results.inputs.loops();
    BitVector layoutsOfObjectsWithoutListedOrder = findLayoutsOfObjectsWithoutListedOrder(results);
    Vector<FieldCreation> ownFieldCreation(FillWith { }, program.layouts.size(), FieldCreation::Unknown);
    forEachIndexInParallel(loops, program.layouts.size(), [&](unsigned index) {
        LayoutID id(index);
        if (results.hasObjects(id) && results.inputs.build.isEnabled(FlowResultKind::ConstructorFieldOrders, index))
            ownFieldCreation[index] = ownFieldCreationOf(results, layoutsOfObjectsWithoutListedOrder, id);
    });
    forEachIndexInParallel(loops, program.layouts.size(), [&](unsigned index) {
        bool isCreatedAtConstruction = true;
        LayoutID current(index);
        for (size_t steps = 0; current && steps < program.layouts.size(); ++steps) {
            const LayoutFlow& flow = results.application.layouts[current.index()].flow;
            bool hasProvenLink = flow.baseState == BaseState::Layout || flow.baseState == BaseState::None;
            // A base that only the classes below it construct creates its fields in their objects.
            bool createsOwnFieldsAtConstruction = ownFieldCreation[current.index()] == FieldCreation::OnlyAtConstruction
                || (current != LayoutID(index) && !results.hasObjects(current) && results.constructions.createsFieldsOnlyInListedOrders(current));
            isCreatedAtConstruction &= hasProvenLink && createsOwnFieldsAtConstruction;
            current = provenBaseOf(results.application, current);
        }
        if (isCreatedAtConstruction)
            results.application.layouts[index].flow.fieldCreation = FieldCreation::OnlyAtConstruction;
    });
}

// The orders in which the allocations and the constant data of a layout without a constructor list its fields: those that a finished object may have come first, and of them the most frequent.
static void computeInitialFieldOrders(const FieldResultsWithoutStores& results, std::span<const AllocatingOperation> operations)
{
    struct CountedOrder {
        Vector<uint32_t> fields;
        uint32_t numberOfSites { 0 };
        bool isOrderOfFinishedObjects { false };
    };

    const ValueFlowResultInputs& inputs = results.inputs;
    const Program& program = inputs.program;
    Vector<Vector<CountedOrder>> ordersOfLayout(program.layouts.size());
    auto listsOrdersOf = [&](LayoutID declared) {
        return hasObjectsThatCodeAlters(program.at(declared).kind) && !program.at(declared).constructor;
    };
    auto count = [&](LayoutID declared, std::span<const uint32_t> fields, bool getsMoreFields) {
        Vector<CountedOrder>& orders = ordersOfLayout[declared.index()];
        auto found = std::ranges::find_if(orders, [&](const CountedOrder& order) {
            return std::ranges::equal(order.fields, fields);
        });
        if (found == orders.end()) {
            orders.append({ Vector<uint32_t>(fields), 1, !getsMoreFields });
            return;
        }
        ++found->numberOfSites;
        found->isOrderOfFinishedObjects |= !getsMoreFields;
    };
    for (const AllocatingOperation& operation : operations) {
        const Value& value = program.at(operation.function).at(operation.value);
        // An allocation lists fields of the declared layout, whichever layout of the family it names.
        if (value.opcode == Opcode::NewObject) {
            LayoutID declared = declaredLayoutOf(program, value.layout());
            if (listsOrdersOf(declared))
                count(declared, value.fields(), operation.getsMoreFields);
            continue;
        }
        forEachDataNodeInPreorder(program.at(value.data()).root, [&](uint32_t, const DataNode& node) {
            const DataNode::Object* object = node.tryAs<DataNode::Object>();
            if (!object)
                return;
            LayoutID declared = declaredLayoutOf(program, object->layout);
            if (listsOrdersOf(declared))
                count(declared, fieldsFilledBy(program, program.at(declared), *object).span(), false);
        });
    }
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        Vector<CountedOrder>& orders = ordersOfLayout[index];
        if (orders.isEmpty() || !inputs.build.isEnabled(FlowResultKind::ConstructorFieldOrders, index))
            continue;
        AOT::sort(orders, [](const CountedOrder& a, const CountedOrder& b) {
            if (a.isOrderOfFinishedObjects != b.isOrderOfFinishedObjects)
                return a.isOrderOfFinishedObjects;
            if (a.numberOfSites != b.numberOfSites)
                return a.numberOfSites > b.numberOfSites;
            return std::ranges::lexicographical_compare(a.fields, b.fields);
        });
        for (CountedOrder& order : orders)
            results.application.layouts[index].flow.constructorOrders.append(FieldOrder { WTF::move(order.fields) });
    }
}

static void computeFieldsStoredOnceByConstructors(const FieldResults& results)
{
    // The one operation that stores by name, as far as somebody has looked.
    struct StoreSeen {
        FunctionID function;
        ValueID store;
        bool isStoredOtherwise { false };

        void note(FunctionID otherFunction, ValueID otherStore)
        {
            isStoredOtherwise |= store && (function != otherFunction || store != otherStore);
            function = otherFunction;
            store = otherStore;
        }
    };

    struct Candidate : StoreSeen {
        LayoutID layout;
        uint32_t field { 0 };
    };

    const Program& program = results.program();
    const ParallelLoops& loops = results.inputs.loops();
    FlowApplication& application = results.application;
    const StoresByLayout& stores = results.stores;

    // The objects of a layout below hold the field too, and another constructor runs for them.
    BitVector hasObjectsBelow(program.layouts.size());
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        if (!results.hasObjects(LayoutID(index)))
            continue;
        for (bool isProven : { true, false }) {
            auto above = [&](LayoutID layout) {
                return isProven ? provenBaseOf(application, layout) : layoutExpectedAbove(program, layout);
            };
            LayoutID current = above(LayoutID(index));
            for (size_t steps = 0; current && steps < program.layouts.size(); ++steps) {
                hasObjectsBelow.quickSet(current.index());
                current = above(current);
            }
        }
    }

    Vector<Candidate> candidates;
    HashMap<uint32_t, Vector<uint32_t, 2>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> candidatesUnderKey;
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        LayoutID id(index);
        const Layout& layout = program.layouts[index];
        if (layout.kind != LayoutKind::Instance || !layout.constructor || !results.hasObjects(id) || hasObjectsBelow.quickGet(index) || !results.constructions.isFollowed(id) || !results.inputs.build.isEnabled(FlowResultKind::StoreCounts, index))
            continue;
        // Code without a solve stores through the unknown object, and no statement stands for its stores.
        if (stores.hasEscapedObject(id) || stores.hasLostObject(id))
            continue;
        if (stores.getsNameNobodyLists(id) || results.heap.writtenUnder(results.layoutsOfObjects.objectsWith(id), ValueFlowName::of(ValueFlowNameKind::CopiedFrom)) != ValueSets::nothing)
            continue;
        for (uint32_t field = 0; field < layout.fields.size(); ++field) {
            const Field& declaration = layout.fields[field];
            const FieldFlow& flow = application.layouts[index].fields[field].flow;
            if (!declaration.key || declaration.flags.contains(FieldFlags::IsInherited) || hasNodeOfItsOwn(declaration) || stores.mayGetAccessorUnder(id, declaration.key))
                continue;
            bool staysProperty = flow.stability.containsAll({ FieldStability::IsNeverDeleted, FieldStability::IsNeverRedefined });
            if (flow.location.writers != WriterState::AllKnown || flow.location.stores != StoreCount::Unknown || flow.location.initialization != InitializationState::SetAtCreation || !staysProperty)
                continue;
            candidatesUnderKey.add(declaration.key.index(), Vector<uint32_t, 2>()).iterator->value.append(candidates.size());
            Candidate candidate;
            candidate.layout = id;
            candidate.field = field;
            candidates.append(candidate);
        }
    }
    if (candidates.isEmpty())
        return;

    // A candidate stays one where every write is a store by name of one operation. Each thread looks at the writes that it takes, and whether all of them have seen the same operation shows in the end.
    Vector<Vector<StoreSeen>> storesSeenBy(loops.width());
    stores.forEachWriteInParallel([&](unsigned worker, const Writer& writer, const ValueFlowNodeKey& key, ValueSetID) {
        if (key.name().kind != ValueFlowNameKind::Property)
            return;
        auto found = candidatesUnderKey.find(key.payload);
        if (found == candidatesUnderKey.end())
            return;
        Vector<StoreSeen>& storesSeen = storesSeenBy[worker];
        if (storesSeen.isEmpty())
            storesSeen.grow(candidates.size());
        const ValueFlowStatement& statement = writer.statement;
        bool storesByName = statement.kind == ValueFlowStatementKind::Operation && (statement.opcode == Opcode::PropSet || statement.opcode == Opcode::PropDefine);
        // No candidate has an object that has escaped or is lost, and the candidates under a key stand in the order of their layouts.
        if (stores.placeholdersInSetOfWrite(key.set).almostEverything) {
            for (uint32_t index : found->value)
                storesSeen[index].isStoredOtherwise = true;
        }
        stores.forEachLayoutListedInSetOfWrite(key.set, writer, [&](LayoutID layout) {
            auto candidate = std::ranges::lower_bound(found->value, layout, { }, [&](uint32_t index) {
                return candidates[index].layout;
            });
            if (candidate == found->value.end() || candidates[*candidate].layout != layout)
                return;
            storesSeen[*candidate].isStoredOtherwise |= !storesByName;
            storesSeen[*candidate].note(statement.function, statement.site);
        });
    });
    for (const Vector<StoreSeen>& storesSeen : storesSeenBy) {
        for (uint32_t index = 0; index < storesSeen.size(); ++index) {
            candidates[index].isStoredOtherwise |= storesSeen[index].isStoredOtherwise;
            if (storesSeen[index].store)
                candidates[index].note(storesSeen[index].function, storesSeen[index].store);
        }
    }

    // A constructor function that somebody calls without new can run again on an object that exists already.
    std::optional<ValueSetID> calleesOfCalls;
    Vector<uint8_t> isCalledDirectly;
    auto isOnlyConstructed = [&](FunctionID function) {
        const ValueFlowResultInputs& inputs = results.inputs;
        if (!calleesOfCalls) {
            Vector<ValueSetID> calleesOfGroups = collectFromCallGroups<ValueSetID>(inputs, results.heap, [&](const ValueFlowNodeKey& key, ValueFlowNodeID, Vector<ValueSetID>& sets) {
                if (!isConstruction(key.callKind()))
                    sets.append(key.set);
            });
            UnionOfSets callees;
            for (ValueSetID set : calleesOfGroups)
                callees.add(set);
            calleesOfCalls = callees.take(results.heap.maker());
            isCalledDirectly.fill(0, program.functions.size());
            forEachIndexInParallel(loops, program.functions.size(), [&](unsigned index) {
                if (!inputs.isReached(FunctionID(index)) && !isUnsolvedCode(inputs, results.tables, FunctionID(index)))
                    return;
                for (const Value& value : program.functions[index].values) {
                    if (accessClassOf(value.opcode) == AccessClass::DirectCall && value.function())
                        AtomicRef(isCalledDirectly[value.function().index()]).store(1, std::memory_order_relaxed);
                }
            });
        }
        ValueFlowObjectID closure = inputs.universe().closureOf(function);
        return !inputs.hasEscaped(closure) && !inputs.isLost(closure) && !inputs.sets().contains(*calleesOfCalls, closure) && !isCalledDirectly[function.index()];
    };
    candidates.removeAllMatching([&](const Candidate& candidate) {
        if (candidate.isStoredOtherwise || !candidate.store)
            return true;
        switch (program.at(candidate.function).kind) {
        case FunctionKind::BaseConstructor:
        case FunctionKind::DerivedConstructor:
            return program.at(candidate.layout).constructor != candidate.function;
        case FunctionKind::Initializer:
            return false;
        case FunctionKind::Plain:
            return program.at(candidate.layout).constructor != candidate.function || !isOnlyConstructed(candidate.function);
        default:
            return true;
        }
    });
    AOT::sort(candidates, { }, [](const Candidate& candidate) {
        return std::tuple { candidate.function, candidate.layout, candidate.field };
    });
    for (size_t first = 0; first < candidates.size();) {
        const Function& function = program.at(candidates[first].function);
        CFG cfg(function);
        BlockCycles cycles(cfg);
        BitVector isInCycle(function.values.size());
        for (uint32_t block = 0; block < function.blocks.size(); ++block) {
            if (!cycles.isInCycle(BlockID(block)))
                continue;
            for (ValueID value : function.blocks[block].values)
                isInCycle.quickSet(value.index());
        }
        size_t end = first;
        for (; end < candidates.size() && candidates[end].function == candidates[first].function; ++end) {
            if (!isInCycle.quickGet(candidates[end].store.index()))
                application.layouts[candidates[end].layout.index()].fields[candidates[end].field].flow.location.stores = StoreCount::AtMostOnce;
        }
        first = end;
    }
}

static void publishConstructions(const FieldResultsWithoutStores& results)
{
    struct FieldAtBirth {
        FieldReference declaration;
        bool isStored;
        bool isStoredWithValue;
    };

    const Program& program = results.program();
    BuildContext& build = results.inputs.build;
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        if (build.isEnabled(FlowResultKind::ConstructorFieldOrders, index))
            results.application.layouts[index].flow.constructorOrders = results.constructions.ordersOf(LayoutID(index));
    }
    Vector<FieldAtBirth> fieldsAtBirth = collectInParallel<FieldAtBirth>(results.inputs.loops(), program.layouts.size(), [&](unsigned index, Vector<FieldAtBirth>& fields) {
        LayoutID id(index);
        if (!results.hasObjects(id) || !build.isEnabled(FlowResultKind::Initialization, index))
            return;
        forEachFieldOfObjectsWith(program, results.application, id, [&](FieldReference declaration, const Field& field) {
            fields.append({ declaration, results.constructions.isStoredAtBirth(id, field.key, declaration), results.constructions.isStoredWithValueAtBirth(id, field.key) });
        });
    });
    Vector<BitVector> isStoredAtBirth(program.layouts.size());
    Vector<BitVector> isLeftOutAtBirth(program.layouts.size());
    Vector<BitVector> isLeftWithoutValueAtBirth(program.layouts.size());
    for (const FieldAtBirth& field : fieldsAtBirth) {
        (field.isStored ? isStoredAtBirth : isLeftOutAtBirth)[field.declaration.layout.index()].set(field.declaration.field);
        if (!field.isStoredWithValue)
            isLeftWithoutValueAtBirth[field.declaration.layout.index()].set(field.declaration.field);
    }
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        for (size_t field : isStoredAtBirth[index]) {
            FieldFlow& flow = results.application.layouts[index].fields[field].flow;
            if (!isLeftOutAtBirth[index].get(field) && flow.occupancy == Occupancy::Unknown)
                flow.occupancy = isLeftWithoutValueAtBirth[index].get(field) ? Occupancy::FilledAtBirth : Occupancy::FilledWithValueAtBirth;
        }
    }

    BitVector& functions = results.tables.functions.functionsSeeingOnlyFinishedObjects;
    functions = BitVector(program.functions.size());
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        if (build.isEnabled(FlowResultKind::Initialization, index) && results.constructions.seesOnlyFinishedObjects(FunctionID(index)))
            functions.quickSet(index);
    }
}

void listStoresByLayout(const ValueFlowResultInputs& inputs, ValueFlowUsesOfHeap& usesOfHeap, const FlowResultTables& tables)
{
    PhaseTimes times(inputs.build, PassID::AnalyzeFlow);
    usesOfHeap.layoutsOfObjects.emplace(inputs, usesOfHeap.heap);
    times.didFinish("fields: layouts of objects"_s);
    usesOfHeap.stores.emplace(inputs, tables, usesOfHeap.heap, *usesOfHeap.layoutsOfObjects);
}

ValueFlowFieldContents computeFieldResults(const ValueFlowResultInputs& inputs, ValueFlowUsesOfHeap& usesOfHeap, const ValueFlowConstructions& constructions, FlowResultTables& tables)
{
    PhaseTimes times(inputs.build, PassID::AnalyzeFlow);
    ValueFlowHeapView& heap = usesOfHeap.heap;
    const UsesOfObjects& uses = usesOfHeap.uses;
    FieldResultsWithoutStores resultsWithoutStores { inputs, tables, *tables.application, constructions };
    Vector<AllocatingOperation> allocatingOperations;
    runTasksInParallel(inputs.loops(), {
        [&] {
            usesOfHeap.stores->noteListedWrites();
            usesOfHeap.stores->setReceiversOfReads(usesOfHeap.uses.takeReceiversOfReads());
            times.didFinish("fields: stores by layout"_s);
        },
        [&] {
            PhaseTimes timesBeside(inputs.build, PassID::AnalyzeFlow);
            publishConstructions(resultsWithoutStores);
            allocatingOperations = collectAllocatingOperations(resultsWithoutStores);
            computeInitialFieldOrders(resultsWithoutStores, allocatingOperations.span());
            timesBeside.didFinish("fields: initial orders, beside stores by layout"_s);
        },
    });
    const LayoutsOfObjects& layoutsOfObjects = *usesOfHeap.layoutsOfObjects;
    StoresByLayout& stores = *usesOfHeap.stores;
    FieldResults results { resultsWithoutStores, heap, layoutsOfObjects, stores };
    times.didFinish("fields: the rest of the initial orders"_s);
    ValueFlowFieldContents contents = computeDeclaredFields(results, uses, allocatingOperations.span());
    allocatingOperations.clear();
    times.didFinish("fields: declared fields"_s);
    computeFieldsStoredOnceByConstructors(results);
    computeKeyEnumerations(results, uses);
    times.didFinish("fields: stored once, enumerations"_s);

    ProvenHierarchy hierarchy(inputs.program, *tables.application);
    Vector<Vector<PropertyKeyID>> appendedKeys = chooseAppendedKeys(results, hierarchy);
    times.didFinish("fields: appended keys chosen"_s);
    appendFields(results, hierarchy, appendedKeys.span(), contents);
    times.didFinish("fields: fields appended"_s);
    computeAddedKeys(results, appendedKeys.span());
    times.didFinish("fields: added keys"_s);
    computeFieldCreations(results);
    times.didFinish("fields: creations"_s);
    return contents;
}

// The type of an appended field is made of all that is stored under its name, so no store brings a value outside it.
static void typeAppendedFields(const ValueFlowResultInputs& inputs, const ValueFlowFieldContents& contents, TypeTable& types, FlowApplication& application)
{
    bool hasAppendedFields = std::ranges::any_of(application.layouts, [](const FlowLayoutUpdate& update) {
        return !update.appendedFields.isEmpty();
    });
    if (!hasAppendedFields)
        return;

    // By layout: the appended fields for whose key the families of the layout declare a type.
    Vector<BitVector> isDeclaredInFamilies = findAppendedFieldsDeclaredInFamilies(inputs.program, application.layouts, inputs.loops());

    ValueFlowPositionCounts counts(nullptr);
    BorrowedValueSetMaker borrowedMaker(inputs.makerPool());
    ValueSetMaker& maker = borrowedMaker.get();
    Type asRead = Type::anything();
    TypeInterningPhase phase(types, 1);
    {
        TypeArena arena(phase, 0);
        for (uint32_t index = 0; index < application.layouts.size(); ++index) {
            Vector<FlowAppendedField>& fields = application.layouts[index].appendedFields;
            if (fields.isEmpty())
                continue;
            ValueFlowPositions positions(inputs, arena, counts, index, &maker);
            for (uint32_t field = 0; field < fields.size(); ++field) {
                FieldFlow& flow = fields[field].flow;
                if (flow.location.writers != WriterState::AllKnown)
                    continue;
                ValueSetID stored = maker.unite(contents.storedInAppendedFields[index][field], maker.of(ValueFlowUniverse::primitive(PrimitiveSet::Absent)));
                ValueFlowPositionResult result = positions.resultAt(asRead, stored);
                if (result.proof == ValueFlowProof::Proven && result.narrowed) {
                    fields[field].position.type = arena.intern(*result.narrowed);
                    fields[field].position.isProven = true;
                }
                // Nothing is ever stored that lies outside `anything`.
                bool takesDeclaredType = !fields[field].position.isProven && isDeclaredInFamilies[index].get(field);
                // Which getters the slot of an accessor takes is a guess: a pair with another getter, or with a setter alone, is turned away and lies beside the slots.
                if (inputs.build.isEnabled(FlowResultKind::HolderMutations, index) && !fields[field].accessor.isAccessor() && !takesDeclaredType)
                    flow.stability.add(FieldStability::IsNeverStoredWithOtherType);
                if (flow.location.initialization == InitializationState::SetAtCreation && flow.stability == everyFlagOf<FieldStability>() && !inputs.sets().isEmpty(contents.storedInAppendedFields[index][field]))
                    flow.occupancy = Occupancy::AlwaysFilled;
            }
        }
    }
    TypeRemap remap = phase.finish(StringRemap { }, &inputs.loops());
    for (FlowLayoutUpdate& update : application.layouts) {
        for (FlowAppendedField& field : update.appendedFields)
            remapTypes(field.position, remap);
    }
}

void computeLayoutsNeverStoredOtherTypes(const ValueFlowResultInputs& inputs, const ValueFlowFieldContents& contents, TypeTable& types, FlowResultTables& tables)
{
    const Program& program = inputs.program;
    FlowApplication& application = *tables.application;
    typeAppendedFields(inputs, contents, types, application);

    // Whether it holds of a layout is read off the fields, which nothing in here writes to.
    for (uint32_t index : findIndicesInParallel(inputs.loops(), program.layouts.size(), [&](unsigned index) {
        const LayoutFlow& flow = application.layouts[index].flow;
        if (flow.allocationClasses.isEmpty())
            return false;
        bool isNeverStoredOtherTypes = true;
        forEachFieldOfObjectsWith(program, application, LayoutID(index), [&](FieldReference declaration, const Field&) {
            isNeverStoredOtherTypes &= application.layouts[declaration.layout.index()].fields[declaration.field].flow.stability.contains(FieldStability::IsNeverStoredWithOtherType);
        });
        LayoutID current(index);
        for (size_t steps = 0; current && steps < program.layouts.size(); ++steps) {
            for (const FlowAppendedField& field : application.layouts[current.index()].appendedFields)
                isNeverStoredOtherTypes &= field.flow.stability.contains(FieldStability::IsNeverStoredWithOtherType);
            current = provenBaseOf(application, current);
        }
        return isNeverStoredOtherTypes;
    })) {
        LayoutFlow& flow = application.layouts[index].flow;
        flow.impossibleMutations.add(ObjectMutation::StoreOfOtherType);
        for (AllocationClassID allocationClass : flow.allocationClasses)
            tables.sites.allocationClasses[allocationClass.index()].impossibleMutations.add(ObjectMutation::StoreOfOtherType);
    }
}

static bool leavesReadsToBeFound(ReadState reads)
{
    return reads == ReadState::OnlyCalled || reads == ReadState::OnlyAccessed;
}

// A field that is only called goes away once the reads that are left in the program do not reach it.
static void keepFieldsOnlyCalledWhoseReadsHaveStamps(const ValueFlowResultInputs& inputs, FlowApplication& application, ValueFlowHeapView* heap, StoresByLayout* stores)
{
    const Program& program = inputs.program;
    BitVector namesFieldOnlyCalled(program.types.numberOfPropertyKeys());
    bool hasFieldOnlyCalled = false;
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        const Layout& layout = program.layouts[index];
        if (!hasObjectsThatCodeAlters(layout.kind))
            continue;
        for (uint32_t field = 0; field < layout.fields.size(); ++field) {
            LocationFlow& location = application.layouts[index].fields[field].flow.location;
            if (!leavesReadsToBeFound(location.reads))
                continue;
            if (!stores) {
                location.reads = ReadState::Unknown;
                continue;
            }
            namesFieldOnlyCalled.quickSet(layout.fields[field].key.index());
            hasFieldOnlyCalled = true;
        }
    }
    if (!hasFieldOnlyCalled)
        return;

    // What is read through a placeholder is collected first, and each layout is looked at once.
    BitVector readThroughUnknownObject;
    BitVector readThroughUntrackedObject;
    BitVector readFromEveryObject;
    struct Read {
        uint32_t key;
        uint32_t receivers;
        uint32_t statement;
    };
    const ParallelLoops& loops = inputs.loops();
    const ValueFlowHeapView& heapForAllThreads = *heap;
    std::span<const ValueFlowStatement> statements = inputs.bodies().statements();
    std::span<const ValueFlowBodyRange> ranges = inputs.bodies().ranges();
    // The reads that no pass will find by a stamp are listed on all threads.
    Vector<Read> readsWithoutStamp;
    {
        ValueSetMakers makers(inputs);
        readsWithoutStamp = collectWithMakers<Read>(inputs, makers, program.functions.size(), [&](unsigned body, ValueSetMaker& maker, Vector<Read>& found) {
            if (!inputs.isReached(FunctionID(body)))
                return;
            std::span<const FlowAccessStamp> stamps = application.bodies[body].accesses.span();
            // The generic version of the function repeats a read behind a check by name, and that read has no stamp: see GenericVersionBuilder::eraseProofsInCopy().
            std::optional<BitVector> valuesBehindChecks;
            auto isRepeatedWithoutStamp = [&](ValueID read) {
                if (!valuesBehindChecks) {
                    const Function& function = program.functions[body];
                    BitVector checksThatTestNothing(function.values.size());
                    for (const FlowClaimUpdate& claim : application.bodies[body].claims) {
                        if (claim.isRedundant || claim.arriving)
                            checksThatTestNothing.quickSet(claim.claim.index());
                    }
                    bool hasCheckThatStays = false;
                    for (uint32_t index = 0; index < function.values.size() && !hasCheckThatStays; ++index)
                        hasCheckThatStays = function.values[index].opcode == Opcode::Check && !checksThatTestNothing.quickGet(index);
                    valuesBehindChecks = hasCheckThatStays ? findValuesBehindChecks(program, function, CFG(function), &checksThatTestNothing) : BitVector();
                }
                return valuesBehindChecks->get(read.index());
            };
            for (uint32_t index = ranges[body].statement; index < ranges[body + 1].statement; ++index) {
                const ValueFlowStatement& statement = statements[index];
                if (statement.kind != ValueFlowStatementKind::Operation || statement.opcode != Opcode::PropGet)
                    continue;
                PropertyKeyID key = program.at(statement.function).at(statement.site).key();
                if (!namesFieldOnlyCalled.quickGet(key.index()))
                    continue;
                if (findByValue(stamps, statement.site, &FlowAccessStamp::value) && !isRepeatedWithoutStamp(statement.site))
                    continue;
                found.append(Read { key.index(), heapForAllThreads.valueOf(maker, inputs.bodies().operandsOf(statement)[OperandIndex::PropGet::object]).index(), index });
            }
        });
    }
    // One set of receivers is looked at once for a name, on behalf of the first read that has it.
    parallelSort(loops, readsWithoutStamp.mutableSpan(), [](const Read& first, const Read& second) {
        return std::tuple { first.key, first.receivers, first.statement } < std::tuple { second.key, second.receivers, second.statement };
    });
    readsWithoutStamp.shrink(std::ranges::unique(readsWithoutStamp, [](const Read& first, const Read& second) {
        return first.key == second.key && first.receivers == second.receivers;
    }).begin() - readsWithoutStamp.begin());

    Vector<ValueSetID> setsOfReceivers(readsWithoutStamp.size(), [&](size_t position) {
        return ValueSetID(readsWithoutStamp[position].receivers);
    });
    AOT::sort(setsOfReceivers, { }, [](ValueSetID set) { return set.index(); });
    removeRepeatedElements(setsOfReceivers);
    stores->lookAt(setsOfReceivers.span());

    // A field has one name, so no two threads write one field.
    Vector<uint32_t> firstReadOfName;
    for (uint32_t position = 0; position < readsWithoutStamp.size(); ++position) {
        if (!position || readsWithoutStamp[position].key != readsWithoutStamp[position - 1].key)
            firstReadOfName.append(position);
    }
    uint32_t numberOfNames = firstReadOfName.size();
    firstReadOfName.append(readsWithoutStamp.size());
    Vector<StoresByLayout::PlaceholdersInSet> placeholdersReadThrough(numberOfNames);
    const StoresByLayout& storesForAllThreads = *stores;
    forEachIndexInParallel(loops, numberOfNames, [&](unsigned name) {
        PropertyKeyID key(readsWithoutStamp[firstReadOfName[name]].key);
        StoresByLayout::PlaceholdersInSet& all = placeholdersReadThrough[name];
        BitVector layoutsSearched(program.layouts.size());
        for (const Read& read : readsWithoutStamp.span().subspan(firstReadOfName[name], firstReadOfName[name + 1] - firstReadOfName[name])) {
            Writer reader { statements[read.statement] };
            StoresByLayout::PlaceholdersInSet placeholders = storesForAllThreads.placeholdersInSetOfWrite(ValueSetID(read.receivers));
            all.unknownObject |= placeholders.unknownObject;
            all.untrackedObject |= placeholders.untrackedObject;
            all.almostEverything |= placeholders.almostEverything;
            storesForAllThreads.forEachLayoutListedInSetOfWrite(ValueSetID(read.receivers), reader, [&](LayoutID layout) {
                if (layoutsSearched.quickSet(layout.index()))
                    return;
                // The lowest layout on the chain that has the name hides the ones above it.
                LayoutID current = layout;
                for (size_t steps = 0; current && steps < program.layouts.size(); ++steps) {
                    if (std::optional<uint32_t> found = program.at(current).findField(key)) {
                        FieldReference declaration = declarationOf(program, { current, *found });
                        LocationFlow& location = application.layouts[declaration.layout.index()].fields[declaration.field].flow.location;
                        if (leavesReadsToBeFound(location.reads))
                            location.reads = ReadState::Unknown;
                        break;
                    }
                    current = provenBaseOf(application, current);
                }
            });
        }
    });
    for (uint32_t name = 0; name < numberOfNames; ++name) {
        size_t key = readsWithoutStamp[firstReadOfName[name]].key;
        if (placeholdersReadThrough[name].unknownObject)
            readThroughUnknownObject.set(key);
        if (placeholdersReadThrough[name].untrackedObject)
            readThroughUntrackedObject.set(key);
        if (placeholdersReadThrough[name].almostEverything)
            readFromEveryObject.set(key);
    }
    if (readThroughUnknownObject.isEmpty() && readThroughUntrackedObject.isEmpty() && readFromEveryObject.isEmpty())
        return;
    forEachIndexInParallel(loops, program.layouts.size(), [&](unsigned index) {
        LayoutID layout(index);
        bool hasEscapedObject = storesForAllThreads.hasEscapedObject(layout);
        bool hasLostObject = storesForAllThreads.hasLostObject(layout);
        forEachFieldOfObjectsWith(program, application, layout, [&](FieldReference declaration, const Field& field) {
            AtomicRef reads(application.layouts[declaration.layout.index()].fields[declaration.field].flow.location.reads);
            if (!field.key || !leavesReadsToBeFound(reads.load(std::memory_order_relaxed)))
                return;
            size_t key = field.key.index();
            if (readFromEveryObject.get(key) || (hasEscapedObject && readThroughUnknownObject.get(key)) || (hasLostObject && readThroughUntrackedObject.get(key)))
                reads.store(ReadState::Unknown, std::memory_order_relaxed);
        });
    });
}

// A pass that takes a field away has to find every store to it. It finds an allocation that lists the field, and a store or a definition by name whose stamp lists the layouts of all its receivers.
void computeStoreNamingResults(const ValueFlowResultInputs& inputs, ValueFlowUsesOfHeap& usesOfHeap, FlowResultTables& tables)
{
    const Program& program = inputs.program;
    FlowApplication& application = *tables.application;
    if (!inputs.build.isEnabledEverywhere(FlowResultKind::ReceiverSets)) {
        keepFieldsOnlyCalledWhoseReadsHaveStamps(inputs, application, nullptr, nullptr);
        return;
    }

    ValueFlowHeapView& heap = usesOfHeap.heap;
    StoresByLayout& stores = *usesOfHeap.stores;
    keepFieldsOnlyCalledWhoseReadsHaveStamps(inputs, application, &heap, &stores);

    // A byte a field: whoever finds such a store sets it, on any thread.
    Vector<uint32_t> firstFieldOfLayout(FillWith { }, program.layouts.size() + 1, 0u);
    for (uint32_t index = 0; index < program.layouts.size(); ++index)
        firstFieldOfLayout[index + 1] = firstFieldOfLayout[index] + program.layouts[index].fields.size();
    Vector<uint8_t> isStoredWithoutName(FillWith { }, firstFieldOfLayout.last(), static_cast<uint8_t>(0));
    auto noteField = [&](FieldReference declaration) {
        AtomicRef(isStoredWithoutName[firstFieldOfLayout[declaration.layout.index()] + declaration.field]).store(1, std::memory_order_relaxed);
    };
    auto namesLocation = [&](const Writer& writer, ValueFlowName name) {
        const ValueFlowStatement& statement = writer.statement;
        if (statement.kind != ValueFlowStatementKind::Operation || name.kind != ValueFlowNameKind::Property)
            return false;
        if (statement.opcode == Opcode::NewObject)
            return true;
        if (statement.opcode != Opcode::PropSet && statement.opcode != Opcode::PropDefine)
            return false;
        if (program.at(statement.function).at(statement.site).key().index() != name.payload)
            return false;
        return !!findByValue(application.bodies[statement.function.index()].accesses.span(), statement.site, &FlowAccessStamp::value);
    };
    struct FieldUnderKey {
        uint32_t key;
        FieldReference declaration;
    };
    // Listed by the first thread that asks.
    struct FieldsOfLayout {
        Lock lock;
        std::atomic<bool> isListed { false };
        Vector<FieldUnderKey> fields;
    };
    UniqueArray<FieldsOfLayout> fieldsByLayout = makeUniqueArray<FieldsOfLayout>(program.layouts.size());
    auto fieldsOfObjectsWith = [&](LayoutID layout) -> const Vector<FieldUnderKey>& {
        FieldsOfLayout& entry = fieldsByLayout[layout.index()];
        if (entry.isListed.load(std::memory_order_acquire))
            return entry.fields;
        Locker locker { entry.lock };
        if (!entry.isListed.load(std::memory_order_relaxed)) {
            forEachFieldOfObjectsWith(program, application, layout, [&](FieldReference declaration, const Field& field) {
                entry.fields.append({ field.key ? field.key.index() : UINT32_MAX, declaration });
            });
            AOT::stableSort(entry.fields, { }, &FieldUnderKey::key);
            entry.isListed.store(true, std::memory_order_release);
        }
        return entry.fields;
    };
    // What goes through a placeholder is collected first, and each layout is looked at once.
    struct NamesStored {
        BitVector names;
        bool mayBeAnyName { false };
        bool isEmpty { true };
    };
    struct SeenByWorker {
        NamesStored throughUnknownObject;
        NamesStored throughUntrackedObject;
        NamesStored intoEveryObject;
        HashSet<uint64_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> nodesAndTypesOfReceiversSeen;
    };
    Vector<SeenByWorker> seenBy(inputs.loops().width());
    // Many statements write to one node. Which fields a write that names no location reaches hangs on the node and on the type of the receiver alone.
    const ValueFlowSolver& solver = inputs.solver();
    Vector<uint8_t> nodesSeenWithoutTypeOfReceiver(FillWith { }, solver.graph().numberOfNodes() - solver.input().numberOfNodes, static_cast<uint8_t>(0));
    stores.forEachWriteInParallel([&](unsigned worker, const Writer& writer, const ValueFlowNodeKey& key, ValueSetID) {
        SeenByWorker& seen = seenBy[worker];
        ValueFlowName name = key.name();
        bool mayBeAnyName = false;
        switch (name.kind) {
        case ValueFlowNameKind::Property:
        case ValueFlowNameKind::Getter:
        case ValueFlowNameKind::Setter:
            break;
        case ValueFlowNameKind::UnknownKey:
        case ValueFlowNameKind::GetterUnderUnknownKey:
        case ValueFlowNameKind::SetterUnderUnknownKey:
        case ValueFlowNameKind::CopiedFrom:
            mayBeAnyName = true;
            break;
        default:
            return;
        }
        if (namesLocation(writer, name))
            return;
        uint32_t node = solver.indexOfMadeNodes(key);
        bool isSeen = writer.layoutsOfReceiver.empty() ? AtomicRef(nodesSeenWithoutTypeOfReceiver[node]).exchange(1, std::memory_order_relaxed) : !seen.nodesAndTypesOfReceiversSeen.add(static_cast<uint64_t>(node) << 32 | writer.familyOfReceiver.index()).isNewEntry;
        if (isSeen)
            return;
        auto noteFieldsOf = [&](LayoutID layout) {
            const Vector<FieldUnderKey>& fields = fieldsOfObjectsWith(layout);
            if (mayBeAnyName) {
                for (const FieldUnderKey& field : fields)
                    noteField(field.declaration);
                return;
            }
            auto first = std::ranges::lower_bound(fields, name.payload, { }, &FieldUnderKey::key);
            for (; first != fields.end() && first->key == name.payload; ++first)
                noteField(first->declaration);
            for (auto withoutKey = fields.end(); withoutKey != fields.begin() && (withoutKey - 1)->key == UINT32_MAX; --withoutKey)
                noteField((withoutKey - 1)->declaration);
        };
        // The type of the receiver picks among the layouts.
        if (!writer.layoutsOfReceiver.empty()) {
            stores.forEachLayoutWrittenThroughSetOfWrite(key.set, writer, noteFieldsOf);
            return;
        }
        auto noteName = [&](NamesStored& stored) {
            stored.isEmpty = false;
            if (mayBeAnyName)
                stored.mayBeAnyName = true;
            else
                stored.names.set(name.payload);
        };
        StoresByLayout::PlaceholdersInSet placeholders = stores.placeholdersInSetOfWrite(key.set);
        if (placeholders.unknownObject)
            noteName(seen.throughUnknownObject);
        if (placeholders.untrackedObject)
            noteName(seen.throughUntrackedObject);
        if (placeholders.almostEverything)
            noteName(seen.intoEveryObject);
        stores.forEachLayoutListedInSetOfWrite(key.set, writer, noteFieldsOf);
    });
    NamesStored throughUnknownObject;
    NamesStored throughUntrackedObject;
    NamesStored intoEveryObject;
    for (const SeenByWorker& seen : seenBy) {
        auto take = [](NamesStored& all, const NamesStored& some) {
            all.names.merge(some.names);
            all.mayBeAnyName |= some.mayBeAnyName;
            all.isEmpty &= some.isEmpty;
        };
        take(throughUnknownObject, seen.throughUnknownObject);
        take(throughUntrackedObject, seen.throughUntrackedObject);
        take(intoEveryObject, seen.intoEveryObject);
    }
    forEachIndexInParallel(inputs.loops(), program.layouts.size(), [&](unsigned index) {
        LayoutID layout(index);
        const NamesStored* reaching[] {
            &intoEveryObject,
            stores.hasEscapedObject(layout) ? &throughUnknownObject : nullptr,
            stores.hasLostObject(layout) ? &throughUntrackedObject : nullptr,
        };
        auto reaches = [](const NamesStored* stored) {
            return stored && !stored->isEmpty;
        };
        if (std::ranges::none_of(reaching, reaches))
            return;
        forEachFieldOfObjectsWith(program, application, layout, [&](FieldReference declaration, const Field& field) {
            bool isStored = std::ranges::any_of(reaching, [&](const NamesStored* stored) {
                return reaches(stored) && (stored->mayBeAnyName || !field.key || stored->names.get(field.key.index()));
            });
            if (isStored)
                noteField(declaration);
        });
    });

    forEachIndexInParallel(inputs.loops(), program.layouts.size(), [&](unsigned index) {
        const Layout& layout = program.layouts[index];
        if (!hasObjectsThatCodeAlters(layout.kind))
            return;
        for (uint32_t field = 0; field < layout.fields.size(); ++field) {
            LocationFlow& location = application.layouts[index].fields[field].flow.location;
            bool isRedefined = !application.layouts[index].fields[field].flow.stability.contains(FieldStability::IsNeverRedefined);
            if (location.writers == WriterState::AllKnown && !isRedefined && !isStoredWithoutName[firstFieldOfLayout[index] + field] && !layout.fields[field].flags.contains(FieldFlags::IsInherited))
                location.storeNaming = StoreNaming::EveryStoreNamesLocation;
        }
    });
    // What code without a solve stores under a name it stores by an operation with that name, whose receiver has no stamp and a type that lists nothing.
    forEachIndexInParallel(inputs.loops(), program.layouts.size(), [&](unsigned index) {
        if (!stores.hasEscapedObject(LayoutID(index)))
            return;
        forEachFieldOfObjectsWith(program, application, LayoutID(index), [&](FieldReference declaration, const Field&) {
            AtomicRef(application.layouts[declaration.layout.index()].fields[declaration.field].flow.location.storeNaming).store(StoreNaming::Unknown, std::memory_order_relaxed);
        });
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
