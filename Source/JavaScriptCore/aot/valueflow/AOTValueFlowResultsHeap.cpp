/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsFieldsInternal.h"

#if ENABLE(AOT)

#include "AOTPrototypesAtCreation.h"
#include "AOTSort.h"
#include "AOTThreshold.h"
#include <algorithm>
#include <optional>
#include <wtf/Vector.h>

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
#if ASSERT_ENABLED
    : m_program(inputs.program)
#endif
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
                if (ValueSetID value = solver.possibleValuesOf(node); value != ValueSets::nothing)
                    found.written.append({ numberOf(key.name()), node.index(), { key.set, value, solver.seenValuesOf(node) } });
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
    Vector<uint32_t> nodesOfManyMembers;
    for (uint32_t index = 0; index < entry.nodes.size(); ++index) {
        const WrittenNode& node = entry.nodes[index];
        if (sets.contains(node.objects, ValueFlowUniverse::unknownObject()))
            throughUnknownObject.append(node.value);
        if (sets.contains(node.objects, ValueFlowUniverse::untrackedObject()))
            throughUntrackedObject.append(node.value);
        if (ValueSets::isComplement(node.objects) || sets.size(node.objects) > mostMembersToIndex) {
            nodesOfManyMembers.append(index);
            continue;
        }
        sets.forEachMemberInLine(node.objects, [&](ValueFlowObjectID member) {
            entry.nodesOfMembers.append({ member.index(), index });
        });
    }
    AOT::sortInLine(entry.nodesOfMembers);
    entry.writtenThroughUnknownObject = maker.get().uniteAll(throughUnknownObject.span());
    entry.writtenThroughUntrackedObject = maker.get().uniteAll(throughUntrackedObject.span());
    // The same value is written through one set after another as the set grows: whoever meets any of them finds that value, so they count as one.
    AOT::sortInLine(nodesOfManyMembers, { }, [&](uint32_t index) {
        return std::pair { entry.nodes[index].value, index };
    });
    for (size_t first = 0; first < nodesOfManyMembers.size();) {
        ValueSetID value = entry.nodes[nodesOfManyMembers[first]].value;
        Vector<ValueSetID, 8> setsWrittenThrough;
        for (; first < nodesOfManyMembers.size() && entry.nodes[nodesOfManyMembers[first]].value == value; ++first)
            setsWrittenThrough.append(entry.nodes[nodesOfManyMembers[first]].objects);
        entry.writtenThroughManyMembers.append({ maker.get().uniteAll(setsWrittenThrough.span()), value });
    }
    // A few sets hold nearly every object that has escaped, the others little: the small ones stand together, so that whoever is in none of them passes them by group.
    AOT::sort(entry.writtenThroughManyMembers, { }, [&](const NodesWrittenUnderName::WrittenThroughManyMembers& written) {
        return std::pair { sets.size(written.objects), written.value };
    });
    for (size_t first = 0; first < entry.writtenThroughManyMembers.size(); first += NodesWrittenUnderName::nodesInGroup) {
        Vector<ValueSetID, NodesWrittenUnderName::nodesInGroup> setsOfGroup;
        for (const auto& written : entry.writtenThroughManyMembers.subspan(first, std::min<size_t>(NodesWrittenUnderName::nodesInGroup, entry.writtenThroughManyMembers.size() - first)))
            setsOfGroup.append(written.objects);
        entry.membersOfGroupsOfNodes.append(maker.get().uniteAll(setsOfGroup.span()));
    }
    entry.membersOfNodesOfManyMembers = maker.get().uniteAll(entry.membersOfGroupsOfNodes.span());
    entry.isIndexed = true;
}

ValueFlowHeapView::ValueFlowHeapView(const ValueFlowResultInputs& inputs)
    : m_inputs(inputs)
    , m_ownIndex(inputs.heapIndex ? nullptr : makeUnique<ValueFlowHeapIndex>(inputs))
    , m_index(inputs.heapIndex ? *inputs.heapIndex : *m_ownIndex)
    , m_makerPool(inputs.makerPool())
    , m_maker(m_makerPool.take())
    , m_escaped(inputs.solver().possibleValuesOf(ValueFlowProgramNodes::escaped()))
    , m_lost(inputs.solver().possibleValuesOf(ValueFlowProgramNodes::lost()))
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
        value = maker.unite(value, inputs.solver().possibleValuesOf(operand.node));
    return ValueFlowRuleContext::throughMaskOf(operand, value, inputs.solver(), maker);
}

ValueSetID ValueFlowHeapView::possibleValuesOf(const ValueFlowOperand& operand)
{
    return possibleValuesOf(m_maker, operand);
}

ValueSetID ValueFlowHeapView::possibleValuesOf(ValueSetMaker& maker, const ValueFlowOperand& operand) const
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
    if (sets.meet(entry.membersOfNodesOfManyMembers, objects)) {
        for (size_t group = 0; group < entry.membersOfGroupsOfNodes.size(); ++group) {
            if (!sets.meet(entry.membersOfGroupsOfNodes[group], objects))
                continue;
            size_t first = group * NodesWrittenUnderName::nodesInGroup;
            for (const auto& written : entry.writtenThroughManyMembers.subspan(first, std::min<size_t>(NodesWrittenUnderName::nodesInGroup, entry.writtenThroughManyMembers.size() - first))) {
                if (sets.meet(written.objects, objects))
                    parts.append(written.value);
            }
        }
    }
    sets.forEachMemberInLine(objects, [&](ValueFlowObjectID member) {
        auto first = std::ranges::lower_bound(entry.nodesOfMembers, NodeOfMember { member.index(), 0 });
        for (; first != entry.nodesOfMembers.end() && first->member == member.index(); ++first)
            parts.append(entry.nodes[first->node].value);
    });
    AOT::sort(parts);
    removeRepeatedElements(parts);
    return maker.uniteAll(parts.span());
}

void ValueFlowHeapView::appendPrototypesAtCreation(ValueFlowObjectID object, Vector<ValueFlowObjectID, 8>& prototypes) const
{
    forEachPrototypeAtCreation(m_inputs.program, m_inputs.universe(), object, [&](ValueFlowObjectID prototype) {
        prototypes.append(prototype);
    }, [&](SilentAboutPrototype) {
        prototypes.append(ValueFlowUniverse::unknownObject());
    });
}

ValueSetID ValueFlowHeapView::prototypesOf(ValueSetID objects)
{
    return m_prototypesOfSet.ensure(objects.index(), [&] {
        return prototypesOf(m_maker, objects);
    }).iterator->value;
}

ValueSetID ValueFlowHeapView::prototypesOfHeirs(ValueSetMaker& maker, ValueSetID heirs) const
{
    Vector<ValueFlowObjectID, 8> atCreation;
    m_inputs.sets().forEachMemberInLine(heirs, [&](ValueFlowObjectID object) {
        appendPrototypesAtCreation(object, atCreation);
    });
    return expand(maker, maker.unite(maker.of(atCreation.span()), maker.intersect(writtenUnder(maker, heirs, prototypeLinkName), m_inputs.universe().objects())));
}

// What lies above a set is what lies above its members. Large sets share most of their members, so each member is asked once for all of them.
ValueSetID ValueFlowHeapView::prototypesOfEachHeir(ValueSetMaker& maker, ValueSetID heirs) const
{
    AOT_THRESHOLD(uint32_t, fewestHeirsAskedOneByOne, 2);
    AOT_THRESHOLD(uint32_t, fewestHeirsAskedByBlock, 128);
    const ValueSets& sets = m_inputs.sets();
    if (sets.size(heirs) < fewestHeirsAskedOneByOne)
        return prototypesOfHeirs(maker, heirs);
    Vector<ValueFlowObjectID, 64> onlyPrototypes;
    Vector<ValueSetID, 8> severalPrototypes;
    auto add = [&](ValueFlowObjectID heir) {
        ValueSetID above = prototypesOf(maker, maker.of(heir));
        if (ValueFlowObjectID prototype = sets.onlyMember(above))
            onlyPrototypes.append(prototype);
        else if (above != ValueSets::nothing && (severalPrototypes.isEmpty() || severalPrototypes.last() != above))
            severalPrototypes.append(above);
    };
    auto takeAllAdded = [&] {
        AOT::sort(severalPrototypes);
        removeRepeatedElements(severalPrototypes);
        ValueSetID all = maker.unite(maker.of(onlyPrototypes.span()), maker.uniteAll(severalPrototypes.span()));
        onlyPrototypes.shrink(0);
        severalPrototypes.shrink(0);
        return all;
    };
    if (sets.size(heirs) < fewestHeirsAskedByBlock) {
        sets.forEachMember(heirs, add);
        return takeAllAdded();
    }
    // Sets that share members share the blocks that hold them, so each block is walked once for all of them.
    Vector<ValueSetID, 16> prototypesOfBlocks;
    sets.forEachBlock(heirs, [&](uint64_t nameOfBlock) {
        prototypesOfBlocks.append(m_index.prototypesOfBlocks().ensure(nameOfBlock, [&] {
            sets.forEachMemberOfBlock(nameOfBlock, add);
            return takeAllAdded();
        }));
    });
    AOT::sort(prototypesOfBlocks);
    removeRepeatedElements(prototypesOfBlocks);
    return maker.uniteAll(prototypesOfBlocks.span());
}

ValueSetID ValueFlowHeapView::prototypesOf(ValueSetMaker& maker, ValueSetID objects) const
{
    return m_index.prototypesOfSets().ensure(objects.index(), [&] {
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
        prototypes = maker.unite(prototypes, prototypesOfEachHeir(maker, otherHeirs));
        if (m_inputs.build.options().verifiesAfterEveryPass)
            RELEASE_ASSERT(prototypes == prototypesOfHeirs(maker, heirs));
        return prototypes;
    });
}

ValueSetID ValueFlowHeapView::chainOf(ValueSetID objects)
{
    return m_chainOfSet.ensure(objects.index(), [&] {
        return chainOf(m_maker, objects);
    }).iterator->value;
}

ValueSetID ValueFlowHeapView::chainBuiltByProgram(ValueSetID objects)
{
    auto found = m_chainBuiltByProgramOfSet.find(objects.index());
    if (found != m_chainBuiltByProgramOfSet.end())
        return found->value;

    const ValueFlowUniverse& universe = m_inputs.universe();
    ValueSetID placeholders = m_maker.unite(universe.unknownObjectAlone(), universe.untrackedObjectAlone());
    ValueSetID namedObjects = m_maker.intersect(universe.objects(), ValueSets::complement(placeholders));
    if (!std::exchange(m_hasIndexedStoresIntoPrototypeLinks, true)) {
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

        // All that a step has added, and whether what lies above it was new as a whole: then the chain of the step is the step and the chain of the next one.
        struct Step {
            ValueSetID added;
            bool meetsNothingWalked;
        };
        AOT_THRESHOLD(size_t, fewestStepsToKeep, 8);
        Vector<Step, 16> steps;
        ValueSetID chainOfLastStep = ValueSets::nothing;

        takeInChainsOfPlaceholdersIn(objects);
        while (true) {
            takeInChainsOfPlaceholdersIn(chain);
            if (ValueSets::isComplement(chain))
                return chainWalkedInFull(maker, objects);
            ValueSetID frontier = maker.intersect(chain, ValueSets::complement(walked));
            if (frontier == ValueSets::nothing)
                break;
            if (std::optional<ValueSetID> known = frontier != objects ? m_index.chainsOfSets().find(frontier.index()) : std::nullopt) {
                chain = maker.unite(chain, *known);
                if (ValueSets::isComplement(chain))
                    return chainWalkedInFull(maker, objects);
                chainOfLastStep = *known;
                break;
            }
            walked = maker.unite(walked, chain);
            ValueSetID above = prototypesOf(maker, frontier);
            steps.append({ frontier, !sets.meet(above, chain) });
            chain = maker.unite(chain, above);
        }
        if (m_inputs.build.options().verifiesAfterEveryPass)
            RELEASE_ASSERT(chain == chainWalkedInFull(maker, objects));
        bool hasTakenInPlaceholder = std::ranges::any_of(placeholders, &Placeholder::isTakenIn);
        if (steps.size() < fewestStepsToKeep || hasTakenInPlaceholder)
            return chain;
        for (size_t index = steps.size(); index-- > 1 && steps[index].meetsNothingWalked;) {
            chainOfLastStep = maker.unite(chainOfLastStep, steps[index].added);
            m_index.chainsOfSets().ensure(steps[index].added.index(), [&] {
                return chainOfLastStep;
            });
            if (m_inputs.build.options().verifiesAfterEveryPass && index == steps.size() / 2)
                RELEASE_ASSERT(chainOfLastStep == chainWalkedInFull(maker, steps[index].added));
        }
        return chain;
    });
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

static bool statementCallsValue(const ValueFlowStatement& statement)
{
    return statement.kind == ValueFlowStatementKind::Operation && flowRuleOf(statement.opcode) == FlowRule::Call;
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
        if (bindsNoArgument(universe, callee))
            boundFunctions.append(callee);
    });
}

static void findOperandsOf(const ValueFlowResultInputs& inputs, const ValueFlowHeapView& heap, ValueSetMaker& maker, const ValueFlowStatement& call, ValueFlowBuiltinApplication& application)
{
    const ValueFlowUniverse& universe = inputs.universe();
    std::span<const ValueFlowOperand> operands = inputs.bodies().operandsOf(call);
    bool hasSecondOperand = !isConstruction(application.kind) || !!inputs.program.at(call.function).at(call.site).operand(1);
    application.thisValue = heap.possibleValuesOf(maker, operands[hasSecondOperand ? 1 : 0]);
    if (passesList(call.opcode)) {
        ValueSetID lists = heap.possibleValuesOf(maker, operands[2]);
        bool mayBeListOfUnseenCode = inputs.sets().contains(lists, ValueFlowUniverse::unknownObject());
        if (mayBeListOfUnseenCode)
            lists = maker.intersect(lists, ValueSets::complement(universe.unknownObjectAlone()));
        application.otherArguments = heap.writtenUnder(maker, heap.expand(maker, lists), elementsName);
        if (mayBeListOfUnseenCode)
            application.otherArguments = maker.unite(application.otherArguments, maker.unite(universe.anyPrimitive(), universe.unknownObjectAlone()));
    } else {
        for (const ValueFlowOperand& argument : operands.subspan(2))
            application.arguments.append(heap.possibleValuesOf(maker, argument));
    }
}

static ValueFlowCallKind kindOfCallBy(const ValueFlowStatement& call)
{
    return call.opcode == Opcode::Call || call.opcode == Opcode::CallList ? ValueFlowCallKind::Call : ValueFlowCallKind::Construct;
}

ValueSetID valuesHandedToCalleesNobodyKnows(const ValueFlowResultInputs& inputs, const ValueFlowHeapView& heap, ValueSetID among)
{
    constexpr uint32_t functionsPerTask = 64;
    const ValueFlowUniverse& universe = inputs.universe();
    uint32_t numberOfFunctions = inputs.program.functions.size();
    std::span<const ValueFlowBodyRange> ranges = inputs.bodies().ranges();
    std::span<const ValueFlowStatement> statements = inputs.bodies().statements();
    ValueSetMakers makers(inputs);
    Vector<ValueSetID> handedOverInTask((numberOfFunctions + functionsPerTask - 1) / functionsPerTask, [](size_t) {
        return ValueSets::nothing;
    });
    forEachIndexInParallel(inputs.loops(), handedOverInTask.size(), [&](unsigned task, unsigned worker) {
        ValueSetMaker& maker = makers.of(worker);
        auto take = [&](ValueSetID values) {
            if (!ValueSets::isComplement(values) && inputs.sets().meet(values, among))
                handedOverInTask[task] = maker.unite(handedOverInTask[task], maker.intersect(values, among));
        };
        uint32_t first = task * functionsPerTask;
        for (uint32_t function = first; function < std::min(first + functionsPerTask, numberOfFunctions); ++function) {
            if (!inputs.isReached(FunctionID(function)))
                continue;
            for (uint32_t index = ranges[function].statement; index < ranges[function + 1].statement; ++index) {
                if (!statementCallsValue(statements[index]))
                    continue;
                ValueSetID callees = valueSeenByRulesAt(inputs, maker, inputs.bodies().operandsOf(statements[index])[0]);
                // A function that text made at run time has made is neither a closure of the program nor a built-in function.
                bool isKnown = !ValueSets::isComplement(callees) && !inputs.sets().contains(callees, ValueFlowUniverse::unknownObject()) && !inputs.sets().contains(callees, ValueFlowUniverse::untrackedObject()) && !inputs.sets().meet(callees, universe.otherCallables());
                if (isKnown)
                    continue;
                ValueFlowBuiltinApplication application;
                application.kind = kindOfCallBy(statements[index]);
                findOperandsOf(inputs, heap, maker, statements[index], application);
                take(application.thisValue);
                take(application.otherArguments);
                for (ValueSetID argument : application.arguments)
                    take(argument);
            }
        }
    });
    ValueSetID handedOver = ValueSets::nothing;
    for (ValueSetID found : handedOverInTask)
        handedOver = makers.of(0).unite(handedOver, found);
    return handedOver;
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

static void forEachBuiltinAppliedThroughBoundFunctions(const ValueFlowResultInputs& inputs, const ValueFlowHeapView& heap, ValueSetMaker& maker, std::span<const ValueFlowObjectID> boundFunctions, ValueFlowBuiltinApplication& application, ValueSetID amongFunctions, SetsMeetingOneSet* wantedOperands, const ValueFlowBuiltinApplicationFunctor& functor)
{
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
            application.builtin = inputs.universe().builtinOf(target);
            if (!raisesEventWhenCalled(inputs.program.builtins(), application.builtin))
                functor(application);
        });
    }
}

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
    application.statement = &call;
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
    forEachBuiltinAppliedThroughBoundFunctions(inputs, heap, maker, boundFunctions.span(), application, amongFunctions, wantedOperands, functor);
}

static void forEachBuiltinAppliedByGroup(const ValueFlowResultInputs& inputs, const ValueFlowHeapView& heap, ValueSetMaker& maker, const ValueFlowNodeKey& key, ValueFlowNodeID first, ValueSetID amongObjects, ValueSetID amongFunctions, SetsMeetingOneSet* wantedOperands, const ValueFlowBuiltinApplicationFunctor& functor)
{
    const ValueFlowUniverse& universe = inputs.universe();
    const ValueFlowSolver& solver = inputs.solver();
    ValueSetID builtinCallees = heap.expandAmong(maker, key.set, amongObjects);
    ValueFlowCallKind kind = key.callKind();
    Vector<ValueFlowObjectID, 4> boundFunctions;
    if (!isConstruction(kind)) {
        inputs.sets().forEachMemberInLine(heap.expandAmong(maker, key.set, maker.intersect(universe.otherCallables(), universe.knownObjects())), [&](ValueFlowObjectID callee) {
            if (bindsNoArgument(universe, callee))
                boundFunctions.append(callee);
        });
    }
    if (builtinCallees == ValueSets::nothing && boundFunctions.isEmpty())
        return;

    ValueFlowCallGroup group = callGroupAt(first);
    bool isAccessorCall = kind == ValueFlowCallKind::Get || kind == ValueFlowCallKind::Set;
    ValueFlowBuiltinApplication application;
    application.thisValue = solver.possibleValuesOf(isConstruction(kind) ? group.newTarget : group.thisValue);
    for (unsigned index = 0; index < numberOfGroupedArguments; ++index)
        application.arguments.append(solver.possibleValuesOf(group.argument(index)));
    application.otherArguments = maker.unite(solver.possibleValuesOf(group.otherArguments), solver.possibleValuesOf(group.laterArguments));
    if (passesAnyOf(application, wantedOperands)) {
        inputs.sets().forEachMemberInLine(builtinCallees, [&](ValueFlowObjectID callee) {
            bool isFunction = inputs.sets().contains(universe.builtinFunctions(), callee);
            if (!isFunction && !isAccessorCall)
                return;
            application.builtin = universe.builtinOf(callee);
            application.kind = isFunction && isAccessorCall ? ValueFlowCallKind::Call : kind;
            application.isCalleeListed = inputs.sets().contains(key.set, callee);
            functor(application);
        });
    }
    forEachBuiltinAppliedThroughBoundFunctions(inputs, heap, maker, boundFunctions.span(), application, amongFunctions, wantedOperands, functor);
}

static void forEachBuiltinAppliedByTest(const ValueFlowResultInputs& inputs, const ValueFlowHeapView& heap, ValueSetMaker& maker, ValueSetID amongFunctions, const ValueFlowBuiltinApplicationFunctor& functor)
{
    const ValueFlowUniverse& universe = inputs.universe();
    PropertyKeyID key = inputs.program.keyOf(BuiltinNames::hasInstanceSymbol);
    auto methodsFoundOn = [&](ValueSetID holders) {
        if (heap.writtenUnder(maker, holders, ValueFlowName::getter(key)) != ValueSets::nothing)
            return amongFunctions;
        ValueSetID stored = maker.unite(heap.writtenUnder(maker, holders, ValueFlowName::of(ValueFlowNameKind::UnknownKey)), heap.writtenUnder(maker, holders, ValueFlowName::property(key)));
        return maker.intersect(stored, amongFunctions);
    };
    if (methodsFoundOn(universe.objects()) == ValueSets::nothing)
        return;

    ValueFlowBuiltinApplication byTest;
    byTest.kind = ValueFlowCallKind::CallByNativeCode;
    byTest.arguments.append(ValueSets::nothing);
    Vector<std::pair<ValueSetID, ValueSetID>, 16> methodsOfConstructors;
    std::span<const ValueFlowBodyRange> ranges = inputs.bodies().ranges();
    std::span<const ValueFlowStatement> statements = inputs.bodies().statements();
    for (uint32_t function = 0; function < inputs.program.functions.size(); ++function) {
        if (!inputs.isReached(FunctionID(function)))
            continue;
        for (uint32_t index = ranges[function].statement; index < ranges[function + 1].statement; ++index) {
            if (statements[index].kind != ValueFlowStatementKind::Operation || statements[index].opcode != Opcode::InstanceOf)
                continue;
            std::span<const ValueFlowOperand> operands = inputs.bodies().operandsOf(statements[index]);
            ValueSetID constructors = valueSeenByRulesAt(inputs, maker, operands[1]);
            size_t known = methodsOfConstructors.findIf([&](const auto& entry) {
                return entry.first == constructors;
            });
            if (known == notFound) {
                known = methodsOfConstructors.size();
                methodsOfConstructors.append({ constructors, methodsFoundOn(heap.chainOf(maker, heap.expand(maker, constructors))) });
            }
            ValueSetID methods = methodsOfConstructors[known].second;
            if (methods == ValueSets::nothing)
                continue;
            byTest.thisValue = constructors;
            byTest.arguments[0] = valueSeenByRulesAt(inputs, maker, operands[0]);
            inputs.sets().forEachMember(methods, [&](ValueFlowObjectID callee) {
                byTest.builtin = universe.builtinOf(callee);
                functor(byTest);
            });
        }
    }
}

static void forEachBuiltinAppliedByProtocol(const ValueFlowResultInputs& inputs, const ValueFlowHeapView& heap, ValueSetMaker& maker, ValueSetID amongFunctions, const ValueFlowBuiltinApplicationFunctor& functor)
{
    forEachBuiltinAppliedByTest(inputs, heap, maker, amongFunctions, functor);
    ValueFlowBuiltinApplication byProtocol;
    byProtocol.kind = ValueFlowCallKind::CallByNativeCode;
    byProtocol.thisValue = ValueSets::anything;
    byProtocol.otherArguments = ValueSets::anything;
    ValueSetID everyObject = inputs.universe().objects();
    ValueSetID stored = heap.writtenUnder(maker, everyObject, unknownKeyName);
    for (BuiltinName name : { BuiltinNames::iteratorSymbol, BuiltinNames::asyncIteratorSymbol, BuiltinNames::next, BuiltinNames::returnKeyword, BuiltinNames::throwKeyword, BuiltinNames::then })
        stored = maker.unite(stored, heap.writtenUnder(maker, everyObject, ValueFlowName::property(inputs.program, inputs.program.keyOf(name))));
    inputs.sets().forEachMember(maker.intersect(maker.intersect(stored, inputs.universe().builtinFunctionsOfProtocols()), amongFunctions), [&](ValueFlowObjectID callee) {
        byProtocol.builtin = inputs.universe().builtinOf(callee);
        functor(byProtocol);
    });
}

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

} // anonymous namespace

static constexpr size_t callsOrGroupsPerTask = 256;

static unsigned numberOfRunsOf(size_t numberOfCallsOrGroups)
{
    return static_cast<unsigned>((numberOfCallsOrGroups + callsOrGroupsPerTask - 1) / callsOrGroupsPerTask);
}

static void runBuiltinApplicationTasks(const ValueFlowResultInputs& inputs, const ValueFlowHeapView& heap, ValueSetMakers& makers, BuiltinsToApply among, ValueSetID onlyPassingAnyOf, NOESCAPE const Invocable<void(unsigned task, unsigned worker, const ValueFlowBuiltinApplication&)> auto& functor, const ValueFlowBuiltinApplicationsOnWorkerFunctor* functorForAllCallees = nullptr)
{
    const ParallelLoops& loops = inputs.loops();
    std::span<const ValueFlowStatement> statements = inputs.bodies().statements();
    std::span<const ValueFlowHeapIndex::CallOfValue> calls = heap.index().callsThatMayApplyBuiltins();
    std::span<const ValueFlowNodeID> groups = heap.index().firstNodesOfCallGroups();
    Vector<SetsMeetingOneSet> setsMeetingOperands(onlyPassingAnyOf == ValueSets::anything ? 0 : loops.width(), [&](size_t) {
        return SetsMeetingOneSet(inputs.sets(), onlyPassingAnyOf);
    });
    forEachIndexInParallel(loops, numberOfRunsOf(calls.size()) + numberOfRunsOf(groups.size()), [&](unsigned task, unsigned worker) {
        ValueSetMaker& maker = makers.of(worker);
        SetsMeetingOneSet* wantedOperands = setsMeetingOperands.isEmpty() ? nullptr : &setsMeetingOperands[worker];
        auto handOn = [&](const ValueFlowBuiltinApplication& application) {
            functor(task, worker, application);
        };
        if (task >= numberOfRunsOf(calls.size())) {
            size_t first = (task - numberOfRunsOf(calls.size())) * callsOrGroupsPerTask;
            for (ValueFlowNodeID node : groups.subspan(first, std::min(callsOrGroupsPerTask, groups.size() - first)))
                forEachBuiltinAppliedByGroup(inputs, heap, maker, *inputs.solver().keyOfMadeNode(node), node, among.objects, among.functions, wantedOperands, handOn);
            return;
        }
        auto handOnAllCallees = [&](const ValueFlowBuiltinApplication& application, ValueSetID builtinCallees) {
            (*functorForAllCallees)(worker, application, builtinCallees);
        };
        ValueFlowBuiltinApplicationsFunctor forAllCallees(handOnAllCallees);
        size_t first = task * callsOrGroupsPerTask;
        for (size_t position = first; position < std::min(first + callsOrGroupsPerTask, calls.size()); ++position)
            forEachBuiltinAppliedByOperation(inputs, heap, maker, statements[calls[position].statement], calls[position].callees, heap.index().operandsOfCallAt(position), among.functions, wantedOperands, handOn, functorForAllCallees ? &forAllCallees : nullptr);
    });
}

static void forEachBuiltinApplicationAmong(const ValueFlowResultInputs& inputs, ValueFlowHeapView& heap, BuiltinsToApply among, ValueSetID amongOperands, const ValueFlowBuiltinApplicationFunctor& functor)
{
    Vector<Vector<ValueFlowBuiltinApplication>> applicationsOfTask(numberOfRunsOf(heap.index().callsThatMayApplyBuiltins().size()) + numberOfRunsOf(heap.index().firstNodesOfCallGroups().size()));
    {
        ValueSetMakers makers(inputs);
        runBuiltinApplicationTasks(inputs, heap, makers, among, amongOperands, [&](unsigned task, unsigned, const ValueFlowBuiltinApplication& application) {
            applicationsOfTask[task].append(application);
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
    inputs.sets().forEachMemberInLine(maker.unite(universe.builtinFunctions(), universe.builtinObjects()), [&](ValueFlowObjectID object) {
        if (onlyOfBuiltins.quickGet(universe.builtinOf(object).index()) && !universe.builtinTables().isDescribedByCodeAlone(universe.builtinOf(object)))
            objects.append(object);
    });
    ValueSetID among = maker.of(objects.span());
    return { maker.intersect(universe.targetsOfBoundFunctionsAmongBuiltins(), among), maker.intersect(universe.builtinObjects(), among) };
}

void forEachBuiltinApplication(const ValueFlowResultInputs& inputs, ValueFlowHeapView& heap, NOESCAPE const ValueFlowBuiltinApplicationFunctor& functor)
{
    forEachBuiltinApplicationAmong(inputs, heap, { inputs.universe().targetsOfBoundFunctionsAmongBuiltins(), inputs.universe().builtinObjects() }, ValueSets::anything, functor);
}

void forEachBuiltinApplication(const ValueFlowResultInputs& inputs, ValueFlowHeapView& heap, const BitVector& onlyOfBuiltins, NOESCAPE const ValueFlowBuiltinApplicationFunctor& functor)
{
    forEachBuiltinApplication(inputs, heap, onlyOfBuiltins, ValueSets::anything, functor);
}

void forEachBuiltinApplication(const ValueFlowResultInputs& inputs, ValueFlowHeapView& heap, const BitVector& onlyOfBuiltins, ValueSetID onlyPassingAnyOf, NOESCAPE const ValueFlowBuiltinApplicationFunctor& functor)
{
    forEachBuiltinApplicationAmong(inputs, heap, builtinsToApplyAmong(inputs, heap.maker(), onlyOfBuiltins), onlyPassingAnyOf, functor);
}

static void forEachBuiltinApplicationOnWorkers(const ValueFlowResultInputs& inputs, const ValueFlowHeapView& heap, ValueSetMakers& makers, const BitVector& onlyOfBuiltins, ValueSetID onlyPassingAnyOf, const ValueFlowBuiltinApplicationOnWorkerFunctor& functor, const ValueFlowBuiltinApplicationsOnWorkerFunctor* functorForAllCallees)
{
    BuiltinsToApply among = builtinsToApplyAmong(inputs, makers.of(0), onlyOfBuiltins);
    runBuiltinApplicationTasks(inputs, heap, makers, among, onlyPassingAnyOf, [&](unsigned, unsigned worker, const ValueFlowBuiltinApplication& application) {
        functor(worker, application);
    }, functorForAllCallees);
    forEachBuiltinAppliedByProtocol(inputs, heap, makers.of(0), among.functions, [&](const ValueFlowBuiltinApplication& application) {
        functor(0, application);
    });
}

void forEachBuiltinApplicationInParallel(const ValueFlowResultInputs& inputs, const ValueFlowHeapView& heap, ValueSetMakers& makers, const BitVector& onlyOfBuiltins, const ValueFlowBuiltinApplicationOnWorkerFunctor& functor, const ValueFlowBuiltinApplicationsOnWorkerFunctor& functorForAllCallees)
{
    forEachBuiltinApplicationOnWorkers(inputs, heap, makers, onlyOfBuiltins, ValueSets::anything, functor, &functorForAllCallees);
}

void forEachBuiltinApplicationOnAllThreads(const ValueFlowResultInputs& inputs, const ValueFlowHeapView& heap, ValueSetMakers& makers, const BitVector& onlyOfBuiltins, ValueSetID onlyPassingAnyOf, NOESCAPE const ScopedLambda<void(unsigned worker, const ValueFlowBuiltinApplication&)>& functor)
{
    forEachBuiltinApplicationOnWorkers(inputs, heap, makers, onlyOfBuiltins, onlyPassingAnyOf, functor, nullptr);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
