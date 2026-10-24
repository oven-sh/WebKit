/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsFieldsInternal.h"

#if ENABLE(AOT)

#include "AOTThreshold.h"

namespace JSC { namespace AOT { namespace Typed {

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
        solver.forEachMadeNodeAskedForByStatements(task * statementsPerTask, std::min(numberOfStatements, (task + 1) * statementsPerTask), [&](uint32_t statement) {
            m_first[statement] = words.size();
        }, [&](uint32_t index, const ValueFlowNodeKey& key, ValueFlowStatementID made) {
            RELEASE_ASSERT(index <= maskOfIndex);
            words.append(static_cast<uint32_t>(key.kind) << (bitsOfIndex + 1) | (made ? hasStatement : 0) | index);
        });
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
            case ValueFlowMadeNodeKind::Constant:
            case ValueFlowMadeNodeKind::Read:
            case ValueFlowMadeNodeKind::Lookup:
            case ValueFlowMadeNodeKind::Hub:
            case ValueFlowMadeNodeKind::Masked:
            case ValueFlowMadeNodeKind::WrittenUnderKind:
            case ValueFlowMadeNodeKind::ReadUnderKind:
            case ValueFlowMadeNodeKind::Instance:
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
    AOT_THRESHOLD(size_t, fewestNodesToRemember, 4);
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
    case ValueFlowObjectKind::Primitive:
    case ValueFlowObjectKind::UnknownObject:
    case ValueFlowObjectKind::ResultOfBuiltin:
    case ValueFlowObjectKind::Key:
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
static bool statementCallsValue(const ValueFlowStatement& statement)
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
                if (!statementCallsValue(statements[index]))
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

void forEachBuiltinApplicationInParallel(const ValueFlowResultInputs& inputs, const ValueFlowHeapView& heap, ValueSetMakers& makers, const BitVector& onlyOfBuiltins, const ValueFlowBuiltinApplicationOnWorkerFunctor& functor, const ValueFlowBuiltinApplicationsOnWorkerFunctor& functorForAllCallees)
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

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
