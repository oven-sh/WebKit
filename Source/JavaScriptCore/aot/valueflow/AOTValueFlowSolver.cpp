/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowSolver.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCFG.h"
#include "AOTIRProgram.h"
#include "AOTPhaseTimes.h"
#include "AOTPrototypesAtCreation.h"
#include "AOTSort.h"
#include "AOTSystemResources.h"
#include "AOTThreshold.h"
#include "AOTValueFlowBodies.h"
#include "AOTValueFlowBuiltinTables.h"
#include "AOTValueFlowBuiltins.h"
#include "Options.h"
#include <algorithm>
#include <tuple>
#include <wtf/CPUTime.h>
#include <wtf/Scope.h>
#include <wtf/StringPrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t noSolverIndex = UINT32_MAX;
AOT_THRESHOLD(uint32_t, mostObjectsOfFew, 8);
AOT_THRESHOLD(uint32_t, mostObjectsLookedUpOneByOne, 64);
AOT_THRESHOLD(uint32_t, mostCellsOfName, 256);
static constexpr uint32_t mostNodesReportedOutsideFamily = 16;

template<typename T, size_t inlineCapacity>
static ALWAYS_INLINE void makeRoomByDoubling(Vector<T, inlineCapacity>& list, size_t more = 1)
{
    if (list.size() + more > list.capacity()) [[unlikely]]
        list.reserveCapacity(std::max<size_t>({ 1024, list.capacity() * 2, list.size() + more }));
}

template<typename T>
static ALWAYS_INLINE void growBy(Vector<T>& list, size_t more)
{
    makeRoomByDoubling(list, more);
    list.grow(list.size() + more);
}

enum class TraitOfMadeNodes : uint8_t {
    ReadsOrWrites = 1 << 0,
    IsMadeForObjects = 1 << 1,
    MaySaturate = 1 << 2,
    MaySaturateUnderSomeNames = 1 << 3,
};

static ALWAYS_INLINE OptionSet<TraitOfMadeNodes> traitsOfMadeNodes(ValueFlowMadeNodeKind kind)
{
    switch (kind) {
    case ValueFlowMadeNodeKind::Read:
    case ValueFlowMadeNodeKind::Written:
    case ValueFlowMadeNodeKind::ReadUnderKind:
    case ValueFlowMadeNodeKind::WrittenUnderKind:
        return { TraitOfMadeNodes::ReadsOrWrites, TraitOfMadeNodes::IsMadeForObjects, TraitOfMadeNodes::MaySaturate };
    case ValueFlowMadeNodeKind::CallGroup:
        return { TraitOfMadeNodes::IsMadeForObjects, TraitOfMadeNodes::MaySaturate };
    case ValueFlowMadeNodeKind::Lookup:
        return { TraitOfMadeNodes::IsMadeForObjects, TraitOfMadeNodes::MaySaturateUnderSomeNames };
    case ValueFlowMadeNodeKind::Constant:
    case ValueFlowMadeNodeKind::Hub:
    case ValueFlowMadeNodeKind::Masked:
    case ValueFlowMadeNodeKind::Marked:
    case ValueFlowMadeNodeKind::Instance:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ALWAYS_INLINE void forEachEdgeToPart(ValueFlowMadeNodeKind kindOfPart, ValueFlowNodeID first, ValueFlowNodeID firstOfPart, NOESCAPE const Invocable<void(ValueFlowEdge)> auto& functor)
{
    if (kindOfPart != ValueFlowMadeNodeKind::CallGroup) {
        bool takesFromPart = kindOfPart == ValueFlowMadeNodeKind::ReadUnderKind || kindOfPart == ValueFlowMadeNodeKind::Lookup;
        functor(takesFromPart ? ValueFlowEdge { firstOfPart, first } : ValueFlowEdge { first, firstOfPart });
        return;
    }
    ValueFlowCallGroup group = callGroupAt(first);
    for (unsigned offset = 0; offset < ValueFlowNodeKey::numberOfNodesOfCallGroup; ++offset) {
        ValueFlowEdge down { ValueFlowNodeID(first.index() + offset), ValueFlowNodeID(firstOfPart.index() + offset) };
        bool comesBack = down.from == group.result || down.from == group.thrown;
        functor(comesBack ? ValueFlowEdge { down.to, down.from } : down);
    }
}

// Tables grow by half at three quarters full, so a large one holds a third more than it needs, not twice as much.
static constexpr size_t grownSizeOfTable(size_t size, size_t smallest) { return std::max(size + size / 2, smallest); }
static constexpr bool isTableFullFor(size_t numberOfKeys, size_t size) { return numberOfKeys * 4 > size * 3; }
static ALWAYS_INLINE size_t firstPositionInPairSet(uint64_t key, size_t size)
{
    return static_cast<size_t>(static_cast<UInt128>(key * 0x9e3779b97f4a7c15ull) * size >> 64);
}

bool ValueFlowPairSet::add(uint32_t first, uint32_t second)
{
    if (isTableFullFor(m_size + 1, m_slots.span().size()))
        grow();
    uint64_t key = (static_cast<uint64_t>(first) << 32 | second) + 1;
    size_t size = m_slots.span().size();
    size_t position = firstPositionInPairSet(key, size);
    for (; m_slots[position]; position = position + 1 == size ? 0 : position + 1) {
        if (m_slots[position] == key)
            return false;
    }
    m_slots[position] = key;
    ++m_size;
    return true;
}

bool ValueFlowPairSet::contains(uint32_t first, uint32_t second) const
{
    if (!m_slots)
        return false;
    uint64_t key = (static_cast<uint64_t>(first) << 32 | second) + 1;
    size_t size = m_slots.span().size();
    for (size_t position = firstPositionInPairSet(key, size); m_slots[position]; position = position + 1 == size ? 0 : position + 1) {
        if (m_slots[position] == key)
            return true;
    }
    return false;
}

void ValueFlowPairSet::prefetch(uint32_t first, uint32_t second) const
{
    if (!m_slots)
        return;
    uint64_t key = (static_cast<uint64_t>(first) << 32 | second) + 1;
    __builtin_prefetch(&m_slots[firstPositionInPairSet(key, m_slots.span().size())], 1);
}

void ValueFlowPairSet::grow()
{
    size_t size = grownSizeOfTable(m_slots.span().size(), 1024);
    MallocSpan<uint64_t> previous = std::exchange(m_slots, MallocSpan<uint64_t>::malloc(size * sizeof(uint64_t)));
    zeroSpan(m_slots.mutableSpan());
    for (uint64_t key : previous.span()) {
        if (!key)
            continue;
        size_t position = firstPositionInPairSet(key, size);
        while (m_slots[position])
            position = position + 1 == size ? 0 : position + 1;
        m_slots[position] = key;
    }
}

ValueFlowNumbersByKey::Added ValueFlowNumbersByKey::add(uint64_t key, uint32_t number)
{
    if (isTableFullFor(m_size + 1, m_slots.span().size()))
        grow();
    size_t size = m_slots.span().size();
    for (size_t position = firstPositionOf(key + 1, size);; position = position + 1 == size ? 0 : position + 1) {
        Slot& slot = m_slots[position];
        if (slot.keyPlusOne() == key + 1)
            return { slot.number, false };
        if (!slot.keyPlusOne()) {
            slot = { static_cast<uint32_t>(key + 1), static_cast<uint32_t>((key + 1) >> 32), number };
            ++m_size;
            return { slot.number, true };
        }
    }
}

size_t ValueFlowNumbersByKey::longestRunOfSlotsInUse() const
{
    size_t longest = 0;
    size_t current = 0;
    for (const Slot& slot : m_slots.span()) {
        current = slot.keyPlusOne() ? current + 1 : 0;
        longest = std::max(longest, current);
    }
    return longest;
}

void ValueFlowNumbersByKey::grow()
{
    size_t size = grownSizeOfTable(m_slots.span().size(), 64);
    MallocSpan<Slot> previous = std::exchange(m_slots, MallocSpan<Slot>::malloc(size * sizeof(Slot)));
    zeroSpan(m_slots.mutableSpan());
    for (const Slot& slot : previous.span()) {
        if (!slot.keyPlusOne())
            continue;
        size_t position = firstPositionOf(slot.keyPlusOne(), size);
        while (m_slots[position].keyPlusOne())
            position = position + 1 == size ? 0 : position + 1;
        m_slots[position] = slot;
    }
}

struct ValueFlowSolver::Thread {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Thread);

    Thread(const ValueFlowSolver& solver, ValueSetMaker& maker)
        : context(solver, log, maker)
    {
    }

    ValueFlowNodeID numberInGraph(ValueFlowNodeID node) const
    {
        return ValueFlowRoundLog::isNumberedByLog(node) ? ValueFlowNodeID(numberOfMadeNode[node.index() & ~ValueFlowRoundLog::numberedByLog]) : node;
    }

    ValueFlowRoundLog log;
    ValueFlowRuleContext context;
    Seconds busyInTurn;
    Seconds longestShareInTurn;
    Seconds longestStatementInRound;
    ValueFlowStatementKind kindOfLongestStatementInRound { ValueFlowStatementKind::Operation };
    Opcode opcodeOfLongestStatementInRound { };
    Vector<ValueFlowNodeID> thrownOfStatementsThatWait;
    uint32_t numberOfDueStatementsThatWait { 0 };
    uint32_t numberOfDueStatementsTaken { 0 };
    Vector<ValueFlowStatementID> statementsFound;
    Vector<ValueFlowStatementID> dueInNextTurn;
    Vector<uint32_t> bodiesReached;
    uint32_t numberOfFunctionsReached { 0 };
    struct PartWatched {
        ValueFlowNodeID node;
        ValueSetID within;
        ValueFlowStatementID statement;
        bool isAskedLast { false };
    };
    std::array<Vector<PartWatched>, numberOfWatchShards> partsWatchedOfShard;
    std::array<Vector<NodeRead>, numberOfReaderShards> nodesReadOfShard;
    std::array<Vector<NodeRead>, numberOfReaderShards> nodesAskedWhetherEmptyOfShard;
    struct NameWatched {
        ValueFlowName name;
        ValueSetID within;
        ValueFlowStatementID statement;
    };
    Vector<NameWatched> namesWatched;
    Vector<uint32_t> numberOfMadeNode;
    size_t numberOfReachedTaken { 0 };
    size_t numberOfMadeTaken { 0 };
    size_t numberOfRunsTaken { 0 };
    size_t numberOfFunctionsAskedTaken { 0 };
    uint32_t numberOfValuesShownTaken { 0 };
    uint32_t numberOfNodesAskedForTaken { 0 };
    struct RunThatAsksAnew {
        size_t run { 0 };
        uint32_t endOfAskedFor { 0 };
        bool addsToEarlierRuns { false };
    };
};

static Vector<std::unique_ptr<ValueSetMaker>> makeMakers(ValueSets& sets, const ParallelLoops& loops)
{
    Vector<std::unique_ptr<ValueSetMaker>> makers(loops.width());
    forEachIndexInParallel(loops, makers.size(), [&](unsigned index) {
        makers[index] = makeUnique<ValueSetMaker>(sets);
    });
    return makers;
}

ValueFlowSolver::ValueFlowSolver(const ValueFlowSolverInput& input, ValueSets& sets, ValueFlowRule rule, const ParallelLoops& loops, Rounds rounds)
    : m_input(input)
    , m_sets(sets)
    , m_rule(rule)
    , m_loops(loops)
    , m_rounds(rounds)
    , m_makers(makeMakers(sets, loops))
    , m_graph(loops, m_makers.span())
{
    m_numberOfUsers.reserveCapacity(2 * static_cast<size_t>(m_input.numberOfNodes));
    m_partsOfMadeNodes.reserveCapacity(2 * static_cast<size_t>(m_input.numberOfNodes));
    m_madeNodesOfNode.reserveCapacity(5 * static_cast<size_t>(m_input.numberOfNodes) / 2);
    m_readersOfNode.reserveCapacity(7 * static_cast<size_t>(m_input.numberOfNodes) / 2);
    m_listenersOfNode.reserveCapacity(7 * static_cast<size_t>(m_input.numberOfNodes) / 2);
    m_madeNodesOfStatement.reserveCapacity(m_input.statements.size() / 2);
    m_fewestSlotsOfIndexOfMadeNodes = std::max<size_t>(64, roundUpToPowerOfTwo(5 * static_cast<size_t>(m_input.numberOfNodes) / 4 / numberOfIndicesOfMadeNodes));

    ValueSetMaker& maker = *m_makers[0];
    const ValueFlowPlaceholders& placeholders = m_input.placeholders;
    m_graph.setPlaceholders(placeholders);
    ValueSetID unknownObjectAlone = placeholders.unknown ? maker.of(placeholders.unknown) : ValueSets::nothing;
    m_untrackedObjectAlone = placeholders.untracked ? maker.of(placeholders.untracked) : ValueSets::nothing;
    m_objectsThatPlaceholdersMayCover = m_input.universe ? m_input.universe->knownObjects() : ValueSets::complement(maker.unite(unknownObjectAlone, m_untrackedObjectAlone));
    m_objectsSaidToBeSomething = m_input.universe ? maker.intersect(m_objectsThatPlaceholdersMayCover, ValueSets::complement(m_input.universe->objectsThatMayBeAnything())) : m_objectsThatPlaceholdersMayCover;
    if (m_input.program && m_input.universe) {
        const BuiltinTable& builtins = m_input.program->builtins();
        Vector<ValueFlowObjectID, 32> objects;
        for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
            if (builtins.canonicalID(BuiltinID(index)) == BuiltinID(index) && builtins.letsInUnseenCode(BuiltinID(index)))
                m_input.universe->appendObjectsOf(BuiltinID(index), objects);
        }
        m_objectsAlwaysFollowed = maker.of(objects.span());
    }
    if (placeholders.objectsWithLayoutOfProgram)
        m_objectsOfProgram = placeholders.objectsWithLayoutOfProgram;
    if (placeholders.closures)
        m_objectsOfProgram = maker.unite(m_objectsOfProgram, placeholders.closures);
    if (m_input.universe) {
        ValueSetID keys = m_input.universe->keys();
        ValueSetID coveredKeys = maker.intersect(keys, ValueSets::complement(m_input.universe->embeddedFilePaths()));
        m_kindsThatCoverKeys.append({ ValueFlowUniverse::primitive(PrimitiveSet::String), ValueSets::complement(maker.intersect(m_input.universe->strings(), coveredKeys)) });
        m_kindsThatCoverKeys.append({ ValueFlowUniverse::primitive(PrimitiveSet::Symbol), ValueSets::complement(maker.intersect(m_input.universe->symbols(), keys)) });
    }

    if (placeholders.unknown && m_input.escaped) {
        m_coverings.append({ placeholders.unknown, m_input.escaped, maker.intersect(m_objectsThatPlaceholdersMayCover, ValueSets::complement(m_objectsAlwaysFollowed)) });
        if (m_input.universe) {
            const ValueFlowHeapTables& heap = m_input.universe->builtinTables().heap();
            m_objectsLookedForBesideUnknownObject = maker.unite(m_input.universe->holdersOfLibraryExports(), maker.intersect(m_objectsAlwaysFollowed, heap.holdersWithMembers()));
            m_objectsLookedForBesideUnknownObject = maker.unite(m_objectsLookedForBesideUnknownObject, m_input.universe->embeddedFilePaths());
        }
        // A name read on Bun keeps that member, so a node lists Bun whether Bun arrives before the unknown object or after it.
        ValueSetID takenByRetiredNodes = maker.unite(m_objectsLookedForBesideUnknownObject, m_objectsAlwaysFollowed);
        ValueSetID keptOutOfNodesMadeForUnknownObject = m_objectsLookedForBesideUnknownObject;
        if (m_input.program && m_input.universe) {
            ValueSetID marks = maker.unite(maker.of(ValueFlowUniverse::markOfNamedBun()), maker.unite(maker.of(ValueFlowUniverse::markOfMemberLeftOut()), maker.of(ValueFlowUniverse::markOfLoadedBun())));
            m_allButMarks = ValueSets::complement(marks);
            takenByRetiredNodes = maker.unite(takenByRetiredNodes, marks);
            if (m_input.program->builtins().contains(Builtins::Bun))
                takenByRetiredNodes = maker.unite(takenByRetiredNodes, maker.of(m_input.universe->objectOf(Builtins::Bun)));
            keptOutOfNodesMadeForUnknownObject = maker.unite(keptOutOfNodesMadeForUnknownObject, marks);
        }
        m_takenByRetiredNodes = takenByRetiredNodes;
        if (m_input.universe) {
            // Start-up locks a member that no statement writes, and a write counts for the holders that its receiver lists. So a node lists such a holder whether it arrives before the unknown object or after it, and a read off the unknown object lists none.
            ValueSetID holders = m_input.universe->builtinTables().heap().holdersOfReplaceableMembers();
            takenByRetiredNodes = maker.unite(takenByRetiredNodes, holders);
            keptOutOfNodesMadeForUnknownObject = maker.unite(keptOutOfNodesMadeForUnknownObject, holders);
            // Whoever lists the own keys of the prototype object of a library class has the names of its members. A read off the unknown object lists no such object.
            keptOutOfNodesMadeForUnknownObject = maker.unite(keptOutOfNodesMadeForUnknownObject, m_input.universe->prototypeObjectsOfLibraryClasses());
        }
        ValueSetID wholeUnknownValue = m_input.universe ? maker.unite(m_input.universe->anyPrimitive(), unknownObjectAlone) : unknownObjectAlone;
        m_graph.retireNodesCoveredBy(placeholders.unknown, m_input.escaped, wholeUnknownValue, ValueSets::complement(takenByRetiredNodes));
        if (m_input.nodes && m_input.universe) {
            // What arrives behind a retired node that carries values out goes straight on to the node that takes them.
            m_graph.retireNodesCoveredBy(placeholders.unknown, m_input.nodes->ofHost(ValueFlowHostNode::ValuesCarriedOut), wholeUnknownValue, ValueSets::complement(takenByRetiredNodes));
        }
        m_keptOutOfNodesMadeForUnknownObject = keptOutOfNodesMadeForUnknownObject;
        if (keptOutOfNodesMadeForUnknownObject != ValueSets::nothing) {
            m_graph.keepOutOfNodesMadeForPlaceholders(keptOutOfNodesMadeForUnknownObject, m_input.escaped, [](const void* solver, ValueFlowNodeID node) {
                return static_cast<const ValueFlowSolver*>(solver)->isMadeForUnknownObject(node) && !static_cast<const ValueFlowSolver*>(solver)->takesArgumentsOfCallGroup(node);
            }, this);
            // A store through the unknown object still stores these values. Claims about fields and reads off escaped objects take what was stored from Written and Read nodes.
            ValueSetID valuesKeptOut = maker.intersect(keptOutOfNodesMadeForUnknownObject, m_allButMarks);
            ValueSetID stringsKeptOut = m_input.universe ? maker.intersect(valuesKeptOut, m_input.universe->strings()) : ValueSets::nothing;
            m_graph.listInsteadOfWhatIsKeptOut(maker.intersect(valuesKeptOut, ValueSets::complement(stringsKeptOut)), placeholders.unknown, stringsKeptOut, ValueFlowUniverse::primitive(PrimitiveSet::String), [](const void* solver, ValueFlowNodeID node) {
                ValueFlowMadeNodeKind kind = static_cast<const ValueFlowSolver*>(solver)->keyOfMadeNode(node)->kind;
                return kind == ValueFlowMadeNodeKind::Written || kind == ValueFlowMadeNodeKind::Read;
            });
        }
    }
    if (placeholders.untracked && m_input.lost) {
        m_coverings.append({ placeholders.untracked, m_input.lost, m_objectsThatPlaceholdersMayCover });
        m_graph.retireNodesCoveredBy(placeholders.untracked, m_input.lost, m_untrackedObjectAlone, maker.intersect(m_objectsThatPlaceholdersMayCover, ValueSets::complement(m_objectsAlwaysFollowed)));
        if (m_input.program && m_input.universe && m_input.nodes) {
            const ValueFlowUniverse& universe = *m_input.universe;
            m_saturates = true;
            uint32_t numberOfLayouts = m_input.program->layouts.size();
            uint32_t numberOfClasses = m_input.program->builtins().classCount();
            m_layoutOrCalleeOfMember.makeRoomToFill(universe.numberOfObjects());
            constexpr uint32_t membersInStretch = 1 << 14;
            forEachIndexInParallel(m_loops, (universe.numberOfObjects() + membersInStretch - 1) / membersInStretch, [&](unsigned stretch) {
                uint32_t end = std::min(universe.numberOfObjects(), (stretch + 1) * membersInStretch);
                for (uint32_t index = stretch * membersInStretch; index < end; ++index) {
                    ValueFlowObjectID member(index);
                    if (!universe.isObject(member) || member == m_input.placeholders.unknown || member == m_input.placeholders.untracked)
                        m_layoutOrCalleeOfMember[index] = 0;
                    else if (LayoutID layout = universe.layoutOf(member))
                        m_layoutOrCalleeOfMember[index] = 1 + layout.index();
                    else if (BuiltinClassID builtinClass = universe.closureFunction(member) ? BuiltinClassID() : universe.classOf(member))
                        m_layoutOrCalleeOfMember[index] = 1 + numberOfLayouts + builtinClass.index();
                    else
                        m_layoutOrCalleeOfMember[index] = 1 + numberOfLayouts + numberOfClasses + index;
                }
            });
            growAndFill(m_lastCountThatSaw, 1 + numberOfLayouts + numberOfClasses + universe.numberOfObjects(), 0u);
        }
    }
    if (m_input.nodes && m_input.universe) {
        for (ValueFlowHostNode taking : { ValueFlowHostNode::PrototypeObjectsThatWritesUnderUnknownKeysReach, ValueFlowHostNode::PrototypeObjectsOfLibraryClassesWhoseOwnKeysAreListed }) {
            m_graph.handOnWhatArrivesAtRetiredNodesTo(m_input.nodes->ofHost(taking), [](const void* solver, ValueFlowNodeID node) {
                return static_cast<const ValueFlowSolver*>(solver)->isMadeForUnknownObject(node);
            }, this);
        }
    }
    constexpr size_t operandsInStretch = 1 << 16;
    Vector<Vector<uint32_t>> masksInStretches((m_input.operands.size() + operandsInStretch - 1) / operandsInStretch);
    forEachIndexInParallel(loops, masksInStretches.size(), [&](unsigned stretch) {
        Vector<uint32_t> masks;
        HashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> isListed;
        size_t first = stretch * operandsInStretch;
        for (const ValueFlowOperand& operand : m_input.operands.subspan(first, std::min(operandsInStretch, m_input.operands.size() - first))) {
            if (operand.maskKind == ValueFlowMaskKind::Plain || !operand.mask || operand.mask == ValueSets::anything)
                continue;
            if (isListed.add(operand.mask.index()).isNewEntry)
                masks.append(operand.mask.index());
        }
        masksInStretches[stretch] = WTF::move(masks);
    });
    Vector<uint32_t> masksOfOperands;
    for (const Vector<uint32_t>& masks : masksInStretches)
        masksOfOperands.appendVector(masks);
    sortAndRemoveDuplicates(loops, masksOfOperands);
    Vector<ValueSetID> familyObjectsOfMasks(masksOfOperands.size());
    forEachIndexInParallel(loops, masksOfOperands.size(), [&](unsigned index, unsigned worker) {
        familyObjectsOfMasks[index] = findFamilyObjectsOf(ValueSetID(masksOfOperands[index]), *m_makers[worker]);
    });
    for (size_t index = 0; index < masksOfOperands.size(); ++index)
        m_familyObjectsOfMask.add(masksOfOperands[index], familyObjectsOfMasks[index]);
    for (unsigned written = 0; written < mostKindsOfNames; ++written) {
        for (unsigned read = 0; read < mostKindsOfNames; ++read) {
            if (!mayBeTheSameName(static_cast<ValueFlowNameKind>(written), static_cast<ValueFlowNameKind>(read)))
                continue;
            m_kindsOfNames[written].readKindsThatMayFindIt |= 1ull << read;
            m_kindsOfNames[read].writtenKindsItMayFind |= 1ull << written;
        }
    }
    for (const std::unique_ptr<ValueSetMaker>& makerOfThread : m_makers)
        m_threads.append(makeUnique<Thread>(*this, *makerOfThread));
    m_isReached.growTo(m_input.ranges.size());
    m_reachedBy.grow(m_input.ranges.size());
    m_isDue.growTo(m_input.statements.size());
    m_hasStatementOfBodyRun.growTo(m_input.statements.size());
    m_statementsWait = m_input.universe && m_input.nodes && m_input.placeholders.unknown;
    if (m_statementsWait) {
        growAndFill(m_firstStatementWaitingForNode, m_input.numberOfNodes, 0u);
        growAndFill(m_listenersOfNode, m_input.numberOfNodes, static_cast<uint8_t>(0));
        growAndFill(m_nextStatementWaiting, m_input.statements.size(), doesNotWait);
    }
}

static bool isNameOfProperty(ValueFlowName name)
{
    return shapeOf(name.kind).reach == ValueFlowNameReach::OneKey;
}

// A computed key is a text or a symbol, never a private name.
bool ValueFlowSolver::mayBeComputedKey(ValueFlowName name) const
{
    if (!m_input.program || !isNameOfProperty(name))
        return true;
    switch (m_input.program->at(PropertyKeyID(name.payload)).kind) {
    case PropertyKeyKind::EnginePrivateName:
    case PropertyKeyKind::ModulePrivateName:
    case PropertyKeyKind::PrivateName:
        return false;
    case PropertyKeyKind::Text:
    case PropertyKeyKind::WellKnownSymbol:
    case PropertyKeyKind::CreatedSymbol:
    case PropertyKeyKind::RegisteredSymbol:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// What lies under a symbol that the library keeps to itself is found under that symbol alone.
bool ValueFlowSolver::isFoundUnderComputedKey(ValueFlowName name) const
{
    return mayBeComputedKey(name) && (!m_input.universe || !isNameOfProperty(name) || !m_input.universe->builtinTables().heap().isSymbolUsedOnlyAsKey(PropertyKeyID(name.payload)));
}

ALWAYS_INLINE ValueFlowNodeID ValueFlowSolver::nodeAwaitedBy(ValueFlowStatementID id, const ValueFlowStatement& statement) const
{
    if (!m_statementsWait || id.index() >= m_input.statements.size() || statement.kind != ValueFlowStatementKind::Operation || !statement.operandCount)
        return { };
    if (statement.opcode != Opcode::PropGet && statement.opcode != Opcode::Call && statement.opcode != Opcode::Construct)
        return { };
    const ValueFlowOperand& first = m_input.operands[statement.firstOperand];
    if (!first.node || (first.constant && first.constant != ValueSets::nothing) || m_graph.valueOf(first.node) != ValueSets::nothing)
        return { };
    if (m_input.instancedFunctions && m_input.instancedFunctions->find(statement.function))
        return { };
    return first.node;
}

ValueFlowSolver::~ValueFlowSolver() = default;

template<typename T>
ListThatStaysInPlace<T>::~ListThatStaysInPlace()
{
    if (!m_elements)
        return;
    releasePages(m_elements, m_numberReserved * sizeof(T));
}

template<typename T>
void ListThatStaysInPlace<T>::release()
{
    if (m_elements)
        releasePages(m_elements, m_numberReserved * sizeof(T));
    m_elements = nullptr;
    m_numberReserved = 0;
    m_numberWithMemory.store(0, std::memory_order_relaxed);
    m_size.store(0, std::memory_order_relaxed);
}

template<typename T>
void ListThatStaysInPlace<T>::takeMemoryFor(size_t size)
{
    constexpr size_t elementsPerStep = 1u << 16;
    static_assert(!(elementsPerStep * sizeof(T) % (64 * KB)) && !(mostElements % elementsPerStep));
    if (size > mostElements) [[unlikely]]
        exitProgramTooLarge();
    Locker locker { m_lock };
    if (!m_elements) {
        // This takes address space alone. A system that is short of it gives less, and the list ends there.
        constexpr size_t fewestElementsToAskFor = 1u << 27;
        m_numberReserved = std::max(elementsPerStep, bytesToReserve(mostElements * sizeof(T)) / sizeof(T) / elementsPerStep * elementsPerStep);
        while (!(m_elements = static_cast<T*>(tryReservePages(m_numberReserved * sizeof(T)))) && m_numberReserved > fewestElementsToAskFor)
            m_numberReserved = m_numberReserved / 2 / elementsPerStep * elementsPerStep;
        if (!m_elements) [[unlikely]]
            exitOutOfMemory();
    }
    if (size > m_numberReserved) [[unlikely]]
        exitOutOfMemory();
    size_t withMemory = m_numberWithMemory.load(std::memory_order_relaxed);
    if (size <= withMemory)
        return;
    size_t end = roundUpToMultipleOf<elementsPerStep>(size);
    commitPages(m_elements + withMemory, (end - withMemory) * sizeof(T));
    m_numberWithMemory.store(end, std::memory_order_release);
}

ValueFlowSolver::IndexOfMadeNodes::~IndexOfMadeNodes()
{
    for (Table table : tables)
        fastFree(table.slots());
}

void ValueFlowSolver::IndexOfMadeNodes::add(std::atomic<Table>& current, uint32_t hash, uint32_t number, size_t fewestSlots, const ListThatStaysInPlace<MadeNodes>& records)
{
    if (number + 1 > maskOfNumber) [[unlikely]]
        exitProgramTooLarge();
    auto slotsOf = [](Table table) {
        return unsafeMakeSpan(table.slots(), table.numberOfSlots());
    };
    auto put = [&](Table table, uint32_t hashOfSlot, uint32_t slot) {
        std::span<Slot> slots = slotsOf(table);
        size_t mask = slots.size() - 1;
        size_t index = hashOfSlot & mask;
        while (slots[index].load(std::memory_order_relaxed))
            index = (index + 1) & mask;
        slots[index].store(slot, std::memory_order_release);
    };
    // Sixteen slots lie in one line of the cache, and five bits of the hash turn away all but one in 32 of the others' slots.
    if (tables.isEmpty() || (size + 1) * 5 > tables.last().numberOfSlots() * 4) {
        size_t numberOfSlots = tables.isEmpty() ? fewestSlots : tables.last().numberOfSlots() * 2;
        Table larger { static_cast<Slot*>(fastAlignedMalloc(64, numberOfSlots * sizeof(Slot))), static_cast<unsigned>(std::countr_zero(numberOfSlots)) };
        zeroSpan(slotsOf(larger));
        if (!tables.isEmpty()) {
            for (const Slot& slot : slotsOf(tables.last())) {
                if (uint32_t taken = slot.load(std::memory_order_relaxed))
                    put(larger, records[(taken & maskOfNumber) - 1].key.hash(), taken);
            }
        }
        tables.append(larger);
        current.store(larger, std::memory_order_release);
    }
    put(tables.last(), hash, bitsOfHashIn(hash) | (number + 1));
    ++size;
}

void ValueFlowSolver::IndexOfMadeNodes::freeOutgrownTables()
{
    while (tables.size() > 1) {
        fastFree(tables.first().slots());
        tables.removeAt(0);
    }
}

ALWAYS_INLINE std::optional<uint32_t> ValueFlowSolver::findNumberOfMadeNodes(const ValueFlowNodeKey& key, uint32_t hash) const
{
    return findInIndexOfMadeNodes(hash, [&](uint32_t candidate) {
        return m_madeNodes[candidate].key == key;
    });
}

void ValueFlowSolver::letGoOfNumbersOfUsers()
{
    RELEASE_ASSERT(m_areRoundsOver);
    m_knowsNumbersOfUsers = false;
    m_numberOfUsers = { };
}

void ValueFlowSolver::letGoOfRecordsAskedFor()
{
    m_knowsRecordsAskedFor = false;
    m_usesOfStatement = { };
    m_uses.release();
}

void ValueFlowSolver::releaseIndexOfMadeNodes()
{
    if (m_input.instancedFunctions) {
        for (const ValueFlowInstancedFunction& instanced : m_input.instancedFunctions->functions()) {
            if (std::optional<ValueFlowNodeID> markers = findMadeNode(ValueFlowNodeKey::markersOf(instanced.function)))
                m_markersOfFunctions.add(instanced.function.index(), *markers);
        }
    }
    m_hasIndexOfMadeNodes.store(false, std::memory_order_relaxed);
    for (unsigned index = 0; index < numberOfIndicesOfMadeNodes; ++index) {
        m_tablesOfMadeNodes[index].store({ }, std::memory_order_relaxed);
        for (IndexOfMadeNodes::Table table : std::exchange(m_madeNodesByKey[index].tables, { }))
            fastFree(table.slots());
        m_madeNodesByKey[index].size = 0;
    }
}

void ValueFlowSolver::makeIndexOfMadeNodesAgain() const
{
    Locker locker { m_lockOfIndexMadeAgain };
    if (m_hasIndexOfMadeNodes.load(std::memory_order_relaxed))
        return;
    for (uint32_t number = 0; number < m_madeNodes.size(); ++number) {
        uint32_t hash = m_madeNodes[number].key.hash();
        indexOfMadeNodesWith(hash).add(tableOfMadeNodesWith(hash), hash, number, m_fewestSlotsOfIndexOfMadeNodes, m_madeNodes);
    }
    m_hasIndexOfMadeNodes.store(true, std::memory_order_release);
}

std::optional<ValueFlowNodeID> ValueFlowSolver::findMadeNode(const ValueFlowNodeKey& key) const
{
    if (!m_hasIndexOfMadeNodes.load(std::memory_order_acquire)) [[unlikely]]
        makeIndexOfMadeNodesAgain();
    std::optional<uint32_t> number = findNumberOfMadeNodes(key);
    if (!number)
        return std::nullopt;
    return m_madeNodes[*number].first;
}

std::optional<ValueFlowNodeID> ValueFlowSolver::findNodeMadeBeforeThisTurn(const ValueFlowNodeKey& key) const
{
    std::optional<uint32_t> number = findNumberOfMadeNodes(key);
    if (!number || m_madeNodes[*number].has(MadeNodes::IsOfThisTurn))
        return std::nullopt;
    return m_madeNodes[*number].first;
}

bool ValueFlowSolver::isAtRest(ValueFlowStatementID statement) const
{
    if (statement.index() < m_input.statements.size())
        return false;
    if (m_areRoundsOver)
        return m_isStatementMadeByRuleAtRest.get(statement.index() - m_input.statements.size());
    uint32_t madeNodes = m_madeNodesOfStatement[statement.index() - m_input.statements.size()];
    return madeNodes != noSolverIndex && !m_numberOfUsers[madeNodes];
}

uint64_t ValueFlowSolver::contentOf(ValueFlowStatementID statement) const
{
    return isMadeByRule(statement) ? contentOf(keyOf(statement)) : statement.index();
}

uint64_t ValueFlowSolver::contentOf(const ValueFlowNodeKey& key) const
{
    uint64_t content = (static_cast<uint64_t>(key.kind) << 40 | static_cast<uint64_t>(key.detail) << 32 | key.payload) * 0x9e3779b97f4a7c15ull;
    content = (content ^ content >> 29 ^ (key.set ? m_sets.hashOfMembers(key.set) : 0)) * 0xbf58476d1ce4e5b9ull;
    return content | 1ull << 63;
}

void ValueFlowSolver::makeDue(ValueFlowStatementID statement)
{
    if (isAtRest(statement) || m_isDue.set(statement.index()))
        return;
    if (hasLongRowBehindIt(statement))
        m_numberDueWithLongRows.fetch_add(1, std::memory_order_relaxed);
    m_due.append(statement);
}

void ValueFlowSolver::listIfItMayBecomeDue(ValueFlowStatementID statement, Vector<ValueFlowStatementID>& list) const
{
    if (isAtRest(statement) || m_isDue.get(statement.index()) || m_isDue.set(statement.index()))
        return;
    list.append(statement);
    if (hasLongRowBehindIt(statement))
        m_numberDueWithLongRows.fetch_add(1, std::memory_order_relaxed);
}

void ValueFlowSolver::makeStatementsDue(uint32_t first, uint32_t end)
{
    makeRoomByDoubling(m_due, end - first);
    for (uint32_t statement = first; statement < end; ++statement)
        makeDue(ValueFlowStatementID(statement));
}

void ValueFlowSolver::makeEveryStatementDue()
{
    for (uint32_t body = 0; body + 1 < m_input.ranges.size(); ++body)
        makeStatementsDue(m_input.ranges[body].statement, m_isReached.get(body) ? m_input.ranges[body + 1].statement : m_input.ranges[body].statementLeavingBody);
    makeStatementsDue(m_input.statements.size(), numberOfStatements());
}

bool ValueFlowSolver::reach(uint32_t firstBody, ValueFlowReachWitness witness)
{
    if (m_isReached.get(firstBody))
        return false;
    Vector<uint32_t, 16> bodies { firstBody };
    m_isReached.set(firstBody);
    witness.round = m_counters.rounds;
    m_reachedBy[firstBody] = witness;
    while (!bodies.isEmpty()) {
        uint32_t body = bodies.takeLast();
        ++m_counters.functionsReached;
        m_bodiesReachedInRound.append(body);
        makeStatementsDue(m_input.ranges[body].statementLeavingBody, m_input.ranges[body + 1].statement);
        auto readers = m_readersOfFunction.find(body);
        if (readers != m_readersOfFunction.end()) {
            for (ValueFlowStatementID statement : readers->value)
                makeDue(statement);
        }
        for (FunctionID entered : m_input.functionsEnteredOnceReached.subspan(m_input.ranges[body].functionEntered, m_input.ranges[body + 1].functionEntered - m_input.ranges[body].functionEntered)) {
            if (!m_isReached.set(entered.index())) {
                m_reachedBy[entered.index()] = ValueFlowReachWitness { { }, body, m_counters.rounds };
                bodies.append(entered.index());
            }
        }
    }
    return true;
}

template<typename T>
void ValueFlowSolver::growAndFill(ListWithRoomToFill<T>& list, size_t size, T value)
{
    size_t before = list.size();
    if (size <= before)
        return;
    list.makeRoomToFill(size - before);
    std::span<T> room = list.mutableSpan().subspan(before);
    constexpr size_t elementsPerShare = 1 << 15;
    forEachIndexInParallel(m_loops, (room.size() + elementsPerShare - 1) / elementsPerShare, [&](unsigned share) {
        std::ranges::fill(room.subspan(share * elementsPerShare, std::min(elementsPerShare, room.size() - share * elementsPerShare)), value);
    });
}

static ALWAYS_INLINE void growByDoubling(ValueFlowSharedBits& bits, size_t size)
{
    if (size > bits.size()) [[unlikely]]
        bits.growTo(std::max(size, bits.size() * 2));
}

static ALWAYS_INLINE void growByDoubling(BitVector& bits, size_t size)
{
    if (size > bits.size()) [[unlikely]]
        bits.ensureSize(std::max(size, bits.size() * 2));
}

static Seconds cpuTimeOfAllThreads()
{
    std::optional<CPUTime> time = CPUTime::get();
    return time ? time->userTime + time->systemTime : Seconds();
}

void ValueFlowSolver::StepTimes::begin()
{
    if (!isOn)
        return;
    startOfStep = MonotonicTime::now();
    cpuAtStartOfStep = CPUTime::forCurrentThread();
    cpuInParallelLoopsAtStartOfStep = ParallelRegionScope::cpuTimeOfCurrentThread();
    cpuOfAllThreadsAtStartOfStep = cpuTimeOfAllThreads();
    byBusyThreadsAtStartOfStep = BusyThreads::times();
}

void ValueFlowSolver::StepTimes::didFinish(ASCIILiteral name)
{
    if (!isOn)
        return;
    MonotonicTime now = MonotonicTime::now();
    Seconds wall = now - std::exchange(startOfStep, now);
    Seconds cpuNow = CPUTime::forCurrentThread();
    Seconds cpu = cpuNow - std::exchange(cpuAtStartOfStep, cpuNow);
    Seconds cpuInParallelLoopsNow = ParallelRegionScope::cpuTimeOfCurrentThread();
    Seconds cpuOutsideParallelLoops = cpu - (cpuInParallelLoopsNow - std::exchange(cpuInParallelLoopsAtStartOfStep, cpuInParallelLoopsNow));
    Seconds cpuOfAllThreadsNow = cpuTimeOfAllThreads();
    Seconds cpuOfAllThreads = cpuOfAllThreadsNow - std::exchange(cpuOfAllThreadsAtStartOfStep, cpuOfAllThreadsNow);
    BusyThreads::Times byBusyThreadsNow = BusyThreads::times();
    BusyThreads::Times byBusyThreads = byBusyThreadsNow;
    for (unsigned bucket = 0; bucket < BusyThreads::numberOfBuckets; ++bucket)
        byBusyThreads[bucket] -= byBusyThreadsAtStartOfStep[bucket];
    byBusyThreadsAtStartOfStep = byBusyThreadsNow;
    for (Step& step : steps) {
        if (step.name.characters() == name.characters()) {
            step.wall += wall;
            step.cpuOfThisThread += cpu;
            step.cpuOutsideParallelLoops += cpuOutsideParallelLoops;
            step.cpuOfAllThreads += cpuOfAllThreads;
            for (unsigned bucket = 0; bucket < BusyThreads::numberOfBuckets; ++bucket)
                step.byBusyThreads[bucket] += byBusyThreads[bucket];
            return;
        }
    }
    steps.append(Step { name, wall, cpu, cpuOutsideParallelLoops, cpuOfAllThreads, byBusyThreads });
}

void ValueFlowSolver::StepTimes::dump() const
{
    if (!isOn)
        return;
    for (const Step& step : steps) {
        StringPrintStream byBusyThreads;
        for (unsigned bucket = 0; bucket < BusyThreads::numberOfBuckets; ++bucket)
            byBusyThreads.print(bucket ? ", " : "", "up to ", bucket + 1 < BusyThreads::numberOfBuckets ? BusyThreads::maxThreadsOfBucket[bucket] : UINT_MAX, ": ", step.byBusyThreads[bucket].milliseconds());
        dataLogLn("AOT flow step | ", step.name, " | ", step.wall.milliseconds(), " ms | this thread's CPU ", step.cpuOfThisThread.milliseconds(), " ms | outside parallel loops ", step.cpuOutsideParallelLoops.milliseconds(), " ms | all threads' CPU ", step.cpuOfAllThreads.milliseconds(), " ms | ms by busy threads ", byBusyThreads.toString());
    }
    dataLogLn("AOT flow loop over due statements | busy on all threads ", busyOnAllThreads.milliseconds(), " ms | the busiest thread of each turn ", busiestThreadOfTurn.milliseconds(), " ms | the longest share of each turn ", longestShareOfTurn.milliseconds(), " ms");
}

void ValueFlowSolver::makeDueInRunningLoop(Thread& thread, ValueFlowStatementID statement)
{
    if (isAtRest(statement) || m_isDue.setInOrder(statement.index()))
        return;
    (m_hasRunInTurn.get(statement.index()) ? thread.dueInNextTurn : thread.statementsFound).append(statement);
}

void ValueFlowSolver::reachFunctionsFoundBy(Thread& thread)
{
    const ValueFlowRoundLog& log = thread.log;
    for (; thread.numberOfReachedTaken < log.reached.size(); ++thread.numberOfReachedTaken) {
        uint32_t firstBody = log.reached[thread.numberOfReachedTaken].index();
        if (m_isReached.get(firstBody) || m_isReached.setInOrder(firstBody))
            continue;
        m_reachedBy[firstBody] = ValueFlowReachWitness { log.whoReached[thread.numberOfReachedTaken].statement, noSolverIndex, m_counters.rounds };
        Vector<uint32_t, 16> bodies { firstBody };
        while (!bodies.isEmpty()) {
            uint32_t body = bodies.takeLast();
            ++thread.numberOfFunctionsReached;
            thread.bodiesReached.append(body);
            for (uint32_t statement = m_input.ranges[body].statementLeavingBody; statement < m_input.ranges[body + 1].statement; ++statement)
                makeDueInRunningLoop(thread, ValueFlowStatementID(statement));
            auto readers = m_readersOfFunction.find(body);
            if (readers != m_readersOfFunction.end()) {
                for (ValueFlowStatementID statement : readers->value)
                    makeDueInRunningLoop(thread, statement);
            }
            for (FunctionID entered : m_input.functionsEnteredOnceReached.subspan(m_input.ranges[body].functionEntered, m_input.ranges[body + 1].functionEntered - m_input.ranges[body].functionEntered)) {
                if (!m_isReached.setInOrder(entered.index())) {
                    m_reachedBy[entered.index()] = ValueFlowReachWitness { { }, body, m_counters.rounds };
                    bodies.append(entered.index());
                }
            }
        }
    }
}

void ValueFlowSolver::runDueStatements(bool isFirstLapOfTurn)
{
    AOT_THRESHOLD(size_t, mostStatementsPerShare, 32);
    AOT_THRESHOLD(size_t, mostFoundLeftToOneThread, 256);
    constexpr size_t sharesPerThread = 4;
    Vector<ValueFlowStatementID> due = std::exchange(m_due, Vector<ValueFlowStatementID>());
    growByDoubling(m_hasRunInTurn, numberOfStatements());
    Vector<ValueFlowStatementID, 16> dearStatements;
    for (ValueFlowStatementID statement : m_dearStatements) {
        if (m_isDue.get(statement.index()) && !m_hasRunInTurn.get(statement.index()) && ((!m_seedOfSchedule && m_dueBehindLongRows.isEmpty()) || due.contains(statement)))
            dearStatements.append(statement);
    }
    growAndFill(m_rememberedByStatement, numberOfStatements(), ValueSetID());
    size_t statementsPerShare = std::clamp<size_t>(due.size() / (m_threads.size() * sharesPerThread), 1, mostStatementsPerShare);
    if (isFirstLapOfTurn) {
        m_nextNode.store(m_graph.numberOfNodes(), std::memory_order_relaxed);
        m_nextStatement.store(numberOfStatements(), std::memory_order_relaxed);
    }
    m_stepTimes.didFinish("rules: in front of the loop"_s);
    size_t numberOfShares = dearStatements.size() + (due.size() + statementsPerShare - 1) / statementsPerShare;
    alignas(128) std::atomic<size_t> nextShare { 0 };
    alignas(128) std::atomic<size_t> numberFound { 0 };
    alignas(128) std::atomic<unsigned> numberAtWork { 0 };
    Lock lockOfFound;
    Vector<ValueFlowStatementID> found;
    forEachIndexInParallel(m_loops, static_cast<unsigned>(std::min<size_t>(m_threads.size(), numberOfShares)), [&](unsigned, unsigned worker) {
        Thread& thread = *m_threads[worker];
        ValueFlowRuleContext& context = thread.context;
        numberAtWork.fetch_add(1, std::memory_order_relaxed);
        Vector<ValueFlowStatementID, 32> taken;
        for (;;) {
            std::span<const ValueFlowStatementID> toRun;
            bool isDear = false;
            bool isOfDueList = false;
            size_t share = nextShare.load(std::memory_order_relaxed);
            if (share < numberOfShares)
                share = nextShare.fetch_add(1, std::memory_order_relaxed);
            if (share < numberOfShares) {
                isDear = share < dearStatements.size();
                isOfDueList = true;
                size_t first = isDear ? share : (share - dearStatements.size()) * statementsPerShare;
                toRun = isDear ? dearStatements.span().subspan(first, 1) : due.subspan(first, std::min(statementsPerShare, due.size() - first));
            } else {
                if (!numberFound.load(std::memory_order_relaxed))
                    break;
                Locker locker { lockOfFound };
                if (unsigned atWork = numberAtWork.load(std::memory_order_relaxed); found.size() > mostFoundLeftToOneThread * atWork && atWork * 2 <= m_threads.size())
                    break;
                size_t count = std::min<size_t>(found.size(), mostStatementsPerShare);
                if (!count)
                    break;
                taken.shrink(0);
                taken.append(found.span().last(count));
                found.shrink(found.size() - count);
                numberFound.store(found.size(), std::memory_order_relaxed);
                toRun = taken.span();
            }
            MonotonicTime startOfShare = m_stepTimes.isOn ? MonotonicTime::now() : MonotonicTime();
            for (ValueFlowStatementID statement : toRun) {
                if (isOfDueList && !isDear && !dearStatements.isEmpty() && m_isDear.get(statement.index()))
                    continue;
                if (m_hasRunInTurn.setInOrder(statement.index())) {
                    thread.dueInNextTurn.append(statement);
                    continue;
                }
                m_isDue.clearInOrder(statement.index());
                if (statement.index() < m_isDueLast.size() && m_isDueLast.get(statement.index()))
                    m_isDueLast.clear(statement.index());
                if (isAtRest(statement))
                    continue;
                ++thread.numberOfDueStatementsTaken;
                ValueFlowStatement statementMadeByRule;
                const ValueFlowStatement& statementToRun = isMadeByRule(statement) ? (statementMadeByRule = statementMadeByRuleAt(statement)) : m_input.statements[statement.index()];
                if (ValueFlowNodeID awaited = nodeAwaitedBy(statement, statementToRun)) {
                    ++thread.numberOfDueStatementsThatWait;
                    m_hasStatementOfBodyRun.set(statement.index());
                    uint32_t& next = m_nextStatementWaiting[statement.index()];
                    if (next != doesNotWait)
                        continue;
                    next = WTF::atomicExchange(&m_firstStatementWaitingForNode[awaited.index()], statement.index() + 1);
                    noteListener(awaited, WaitsForFirstOperand);
                    if (statementToRun.thrown)
                        thread.thrownOfStatementsThatWait.append(statementToRun.thrown);
                    continue;
                }
                MonotonicTime startOfStatement = m_stepTimes.isOn ? MonotonicTime::now() : MonotonicTime();
                context.begin(statement);
                m_rule(statementToRun, context);
                context.end();
                if (m_stepTimes.isOn) {
                    constexpr Seconds longest = 20_ms;
                    Seconds wall = MonotonicTime::now() - startOfStatement;
                    if (wall > thread.longestStatementInRound) {
                        thread.longestStatementInRound = wall;
                        thread.kindOfLongestStatementInRound = statementToRun.kind;
                        thread.opcodeOfLongestStatementInRound = statementToRun.opcode;
                    }
                    if (wall >= longest)
                        dataLogLn("AOT flow long statement | ", wall.milliseconds(), " ms | round ", m_counters.rounds, " | kind ", static_cast<unsigned>(statementToRun.kind), " | opcode ", static_cast<unsigned>(statementToRun.opcode), " | operands ", statementToRun.operandCount, " | function ", statementToRun.function.index());
                }
                if (statement.index() < m_input.statements.size())
                    m_hasStatementOfBodyRun.set(statement.index());
                if (m_statementsWait && statement.index() < m_input.statements.size())
                    m_nextStatementWaiting[statement.index()] = doesNotWait;
                runStatementsMadeBy(thread);
                runStatementsWokenBy(thread);
                reachFunctionsFoundBy(thread);
            }
            if (m_stepTimes.isOn) {
                Seconds wallOfShare = MonotonicTime::now() - startOfShare;
                thread.busyInTurn += wallOfShare;
                thread.longestShareInTurn = std::max(thread.longestShareInTurn, wallOfShare);
            }
            if (!thread.statementsFound.isEmpty()) {
                Locker locker { lockOfFound };
                found.appendVector(thread.statementsFound);
                numberFound.store(found.size(), std::memory_order_relaxed);
                thread.statementsFound.shrink(0);
            }
        }
        numberAtWork.fetch_sub(1, std::memory_order_relaxed);
    });
    m_due = WTF::move(found);
    for (const std::unique_ptr<Thread>& thread : m_threads) {
        m_counters.statementRuns += std::exchange(thread->numberOfDueStatementsTaken, 0u);
        m_counters.statementRuns += std::exchange(thread->log.numberOfRunsWhereMade, 0u);
        m_counters.statementRuns -= std::exchange(thread->numberOfDueStatementsThatWait, 0u);
        m_counters.functionsReached += std::exchange(thread->numberOfFunctionsReached, 0u);
        m_due.appendVector(thread->statementsFound);
        thread->statementsFound.shrink(0);
        m_dueInNextTurn.appendVector(thread->dueInNextTurn);
        thread->dueInNextTurn.shrink(0);
        m_bodiesReachedInRound.appendVector(thread->bodiesReached);
        thread->bodiesReached.shrink(0);
    }
    m_stepTimes.didFinish("rules: the loop over due statements"_s);
    if (m_stepTimes.isOn) {
        Seconds busiest;
        Seconds longest;
        for (const std::unique_ptr<Thread>& thread : m_threads) {
            m_stepTimes.busyOnAllThreads += thread->busyInTurn;
            busiest = std::max(busiest, std::exchange(thread->busyInTurn, 0_s));
            longest = std::max(longest, std::exchange(thread->longestShareInTurn, 0_s));
        }
        m_stepTimes.busiestThreadOfTurn += busiest;
        m_stepTimes.longestShareOfTurn += longest;
    }
}

void ValueFlowSolver::findStatementsWithLongRowsBehindThem()
{
    AOT_THRESHOLD(unsigned, fewestStatementsOfLongRow, 16);
    if (!m_statementsWait)
        return;
    Vector<uint32_t> producerOfNode;
    producerOfNode.fill(noSolverIndex, m_input.numberOfNodes);
    Vector<uint16_t> lengthBehind;
    lengthBehind.fill(static_cast<uint16_t>(0), m_input.statements.size());
    uint32_t numberOfBodies = m_input.ranges.size() - 1;
    constexpr uint32_t bodiesPerShare = 512;
    Vector<Vector<uint32_t>> foundOfShare((numberOfBodies + bodiesPerShare - 1) / bodiesPerShare);
    forEachIndexInParallel(m_loops, foundOfShare.size(), [&](unsigned share) {
        for (uint32_t body = share * bodiesPerShare; body < std::min(numberOfBodies, (share + 1) * bodiesPerShare); ++body) {
            uint32_t first = m_input.ranges[body].statement;
            uint32_t end = m_input.ranges[body + 1].statement;
            if (end - first <= fewestStatementsOfLongRow)
                continue;
            for (uint32_t index = first; index < end; ++index) {
                if (ValueFlowNodeID result = m_input.statements[index].result; result && result.index() < m_input.numberOfNodes)
                    producerOfNode[result.index()] = index;
            }
            for (uint32_t index = end; index-- > first;) {
                const ValueFlowStatement& statement = m_input.statements[index];
                if (lengthBehind[index] >= fewestStatementsOfLongRow)
                    foundOfShare[share].append(index);
                if (statement.kind != ValueFlowStatementKind::Operation || !statement.operandCount || (statement.opcode != Opcode::PropGet && statement.opcode != Opcode::Call && statement.opcode != Opcode::Construct))
                    continue;
                const ValueFlowOperand& operand = m_input.operands[statement.firstOperand];
                if (!operand.node || operand.node.index() >= m_input.numberOfNodes || (operand.constant && operand.constant != ValueSets::nothing))
                    continue;
                uint32_t producer = producerOfNode[operand.node.index()];
                if (producer == noSolverIndex || producer < first || producer >= index)
                    continue;
                lengthBehind[producer] = std::max<uint16_t>(lengthBehind[producer], static_cast<uint16_t>(std::min<unsigned>(lengthBehind[index] + 1, UINT16_MAX)));
            }
        }
    });
    for (const Vector<uint32_t>& found : foundOfShare) {
        for (uint32_t index : found) {
            if (!std::exchange(m_hasLongRows, true))
                m_hasLongRowBehindIt.ensureSize(m_input.statements.size());
            m_hasLongRowBehindIt.quickSet(index);
        }
    }
}

void ValueFlowSolver::letLongRowsGoFirst()
{
    if (m_numberDueWithLongRows.exchange(0, std::memory_order_relaxed)) {
        size_t numberThatRun = 0;
        size_t numberThatWaited = m_dueBehindLongRows.size();
        makeRoomByDoubling(m_dueBehindLongRows, m_due.size());
        for (ValueFlowStatementID statement : m_due) {
            if (hasLongRowBehindIt(statement))
                m_due[numberThatRun++] = statement;
            else
                m_dueBehindLongRows.append(statement);
        }
        if (numberThatRun) {
            m_due.shrink(numberThatRun);
            return;
        }
        m_dueBehindLongRows.shrink(numberThatWaited);
    }
    if (m_dueBehindLongRows.isEmpty())
        return;
    if (m_due.size() > m_dueBehindLongRows.size())
        std::swap(m_due, m_dueBehindLongRows);
    m_dueBehindLongRows.appendVector(m_due);
    m_due = std::exchange(m_dueBehindLongRows, { });
}

void ValueFlowSolver::letSomeDueStatementsWait()
{
    m_due.appendVector(std::exchange(m_dueStatementsThatWait, { }));
    size_t numberThatRun = 0;
    for (ValueFlowStatementID statement : m_due) {
        uint64_t hash = (m_seedOfSchedule * 0x9E3779B97F4A7C15ull) ^ (contentOf(statement) * 0xC2B2AE3D27D4EB4Full) ^ (m_counters.rounds * 0x165667B19E3779F9ull);
        hash ^= hash >> 29;
        hash *= 0xBF58476D1CE4E5B9ull;
        hash ^= hash >> 32;
        if (hash & 1)
            m_dueStatementsThatWait.append(statement);
        else
            m_due[numberThatRun++] = statement;
    }
    if (numberThatRun)
        m_due.shrink(numberThatRun);
    else
        m_dueStatementsThatWait.shrink(0);
}

// A set that grows by a member a round runs its readers each round, and each run leaves nodes and edges behind for a set that the next run makes useless:
// sites times rounds of them. A statement that has run often waits, longer each time, until the set has grown more or nothing else is due.
void ValueFlowSolver::letStatementsThatRunOftenWait()
{
    AOT_THRESHOLD(unsigned, timesRunWithoutWaiting, 16);
    size_t statements = numberOfStatements();
    if (size_t known = m_timesRunOfStatement.size(); known < statements) {
        m_timesRunOfStatement.grow(statements);
        std::ranges::fill(m_timesRunOfStatement.mutableSpan().subspan(known), 0);
    }
    m_due.appendVector(std::exchange(m_statementsThatRunOften, { }));
    uint32_t round = m_counters.rounds;
    size_t numberThatRun = 0;
    for (ValueFlowStatementID statement : m_due) {
        if (m_timesRunOfStatement[statement.index()] > timesRunWithoutWaiting && m_roundOfNextRun.get(statement.index()) > round)
            m_statementsThatRunOften.append(statement);
        else
            m_due[numberThatRun++] = statement;
    }
    m_due.shrink(numberThatRun);
    if (m_due.isEmpty())
        m_due = std::exchange(m_statementsThatRunOften, { });
    for (ValueFlowStatementID statement : m_due) {
        uint8_t& times = m_timesRunOfStatement[statement.index()];
        if (times >= timesRunWithoutWaiting)
            m_roundOfNextRun.set(statement.index(), round + (1u << std::min<unsigned>(times - timesRunWithoutWaiting, 16)));
        if (times < UINT8_MAX)
            ++times;
    }
}

void ValueFlowSolver::runRules()
{
    for (m_turnsOfRound = 0; !m_due.isEmpty(); ++m_turnsOfRound) {
        ++m_numberOfTurn;
        for (bool isFirstLapOfTurn = true; !m_due.isEmpty(); isFirstLapOfTurn = false)
            runDueStatements(isFirstLapOfTurn);
        m_due = std::exchange(m_dueInNextTurn, { });
        m_hasRunInTurn.clearAll();
        takeWhatRulesHaveMade();
        for (const std::unique_ptr<Thread>& thread : m_threads)
            thread->log.recordsOfRunsWhereMade.shrink(0);
        m_stepTimes.didFinish("rules: what the rules have made"_s);
        numberMadeNodes();
        m_stepTimes.didFinish("rules: made nodes numbered"_s);
        for (const std::unique_ptr<Thread>& owned : m_threads) {
            Thread& thread = *owned;
            for (const ValueFlowRoundLog::Run& run : thread.log.runs.subspan(thread.numberOfRunsTaken)) {
                for (; thread.numberOfFunctionsAskedTaken < run.endOfFunctionsAsked; ++thread.numberOfFunctionsAskedTaken) {
                    FunctionID function = thread.log.functionsAsked[thread.numberOfFunctionsAskedTaken];
                    if (m_isReached.get(function.index())) {
                        makeDue(run.statement);
                        continue;
                    }
                    Vector<ValueFlowStatementID>& readers = m_readersOfFunction.add(function.index(), Vector<ValueFlowStatementID>()).iterator->value;
                    readers.appendIfNotContains(run.statement);
                }
            }
        }
        m_stepTimes.didFinish("rules: functions asked"_s);
        noteNodesAskedFor();
        m_stepTimes.didFinish("rules: nodes asked for"_s);
    }
}

void ValueFlowSolver::listToLink(const MadeNodes& made, auto& readToLink, auto& writtenToLink)
{
    const ValueFlowNodeKey& key = made.key;
    bool standsForKind = key.kind == ValueFlowMadeNodeKind::ReadUnderKind || key.kind == ValueFlowMadeNodeKind::WrittenUnderKind;
    bool isRead = key.kind == ValueFlowMadeNodeKind::Read || key.kind == ValueFlowMadeNodeKind::ReadUnderKind;
    (isRead ? readToLink : writtenToLink).append(NodeToLink { { key.set, made.first }, key.name(), standsForKind });
}

class ValueFlowSolver::NodeMaker {
    WTF_MAKE_NONCOPYABLE(NodeMaker);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(NodeMaker);
public:
    struct Wish {
        uint32_t madeNodes { 0 };
        ValueFlowNodeKey key;
        uint32_t granted { noSolverIndex };
    };

    struct Masked {
        uint32_t madeNodes { 0 };
        ValueSetID familyObjects;
    };

    explicit NodeMaker(ValueFlowSolver& solver)
        : m_solver(solver)
        , m_sets(solver.m_sets)
        , m_firstMadeNodes(solver.m_madeNodes.size())
        , m_firstNode(solver.m_graph.numberOfNodes())
        , m_firstStatement(solver.numberOfStatements())
        , m_firstPart(solver.m_partsOfMadeNodes.size())
    {
    }

    static unsigned makerOfName(uint64_t name) { return static_cast<unsigned>(name * 0x9e3779b97f4a7c15ull >> 58); }
    static uint64_t nameOf(const ValueFlowNodeKey& key) { return static_cast<uint64_t>(key.kind) << 40 | static_cast<uint64_t>(key.detail) << 32 | key.payload; }
    static bool readsOrWrites(ValueFlowMadeNodeKind kind) { return traitsOfMadeNodes(kind).contains(TraitOfMadeNodes::ReadsOrWrites); }
    static bool mayBringOthers(const ValueSets& sets, const ValueFlowNodeKey& key)
    {
        return readsOrWrites(key.kind) && !ValueSets::isComplement(key.set) && sets.size(key.set) > mostObjectsLookedUpOneByOne;
    }
    static unsigned makerOf(const ValueSets& sets, const KeyToMake& toMake)
    {
        return makerOfName(mayBringOthers(sets, toMake.key) ? nameOf(toMake.key) : nameOf(toMake.key) ^ toMake.hashOfMembers);
    }

    void run(std::span<KeyToMake> all, ValueSetMaker& maker, bool mayBeThereAsParts = false)
    {
        m_maker = &maker;
        for (uint32_t index : keys) {
            KeyToMake& toMake = all[index];
            if (mayBeThereAsParts && readsOrWrites(toMake.key.kind)) {
                if (std::optional<uint32_t> existing = m_solver.findNumberOfMadeNodes(toMake.key)) {
                    toMake.madeNodes = *existing;
                    continue;
                }
            }
            if (readsOrWrites(toMake.key.kind)) {
                if (uint64_t name = nameOf(toMake.key); name != m_name) {
                    m_name = name;
                    m_firstOfName = madeNodes.size();
                    m_hasMadeMoreOfName = false;
                }
                if (m_hasMadeMoreOfName) {
                    if (std::optional<uint32_t> existing = findAmongNew(toMake.key)) {
                        toMake.madeNodes = *existing;
                        continue;
                    }
                }
            }
            toMake.madeNodes = makeNodes(toMake.key);
        }
    }

    uint32_t movedUp(uint32_t number, uint32_t first, uint32_t distance) const { return number < first ? number : number + distance; }
    uint32_t madeNodesMovedUp(uint32_t number) const { return movedUp(number, m_firstMadeNodes, before.madeNodes); }
    ValueFlowNodeID nodeMovedUp(ValueFlowNodeID node) const { return node ? ValueFlowNodeID(movedUp(node.index(), m_firstNode, before.nodes)) : node; }
    ValueFlowStatementID statementMovedUp(ValueFlowStatementID statement) const { return statement ? ValueFlowStatementID(movedUp(statement.index(), m_firstStatement, before.statements)) : statement; }
    uint32_t partMovedUp(uint32_t position) const { return movedUp(position, m_firstPart, before.parts); }

    Vector<uint32_t> keys;
    Vector<MadeNodes> madeNodes;
    uint32_t numberOfNodes { 0 };
    Vector<uint32_t> madeNodesOfStatement;
    Vector<uint32_t> parts;
    Vector<ValueFlowEdge> edges;
    Vector<NodeToLink> readToLink;
    Vector<NodeToLink> writtenToLink;
    Vector<Wish> wishes;
    Vector<uint32_t> constants;
    Vector<uint32_t> instances;
    Vector<uint32_t> readsOfWhatLetsInUnseenCode;
    Vector<uint32_t> stayingAtRest;
    Vector<Masked> masked;

    struct NumbersMade {
        uint32_t madeNodes { 0 };
        uint32_t nodes { 0 };
        uint32_t statements { 0 };
        uint32_t parts { 0 };
        size_t edges { 0 };
        size_t readToLink { 0 };
        size_t writtenToLink { 0 };
    };
    NumbersMade before;

private:
    const MadeNodes& at(uint32_t number) const LIFETIME_BOUND { return number < m_firstMadeNodes ? m_solver.m_madeNodes[number] : madeNodes[number - m_firstMadeNodes]; }
    MadeNodes& newAt(uint32_t number) LIFETIME_BOUND { return madeNodes[number - m_firstMadeNodes]; }
    void forEachPartOf(const MadeNodes& made, NOESCAPE const Invocable<void(uint32_t)> auto& functor) const
    {
        if (made.has(MadeNodes::HasParts))
            forEachPartFrom(made.firstPart() < m_firstPart ? &m_solver.m_partsOfMadeNodes[made.firstPart()] : &parts[made.firstPart() - m_firstPart], functor);
    }

    std::optional<uint32_t> findAmongNew(const ValueFlowNodeKey& key)
    {
        for (size_t position = std::max(m_numberFoundByKey, m_firstOfName); position < madeNodes.size(); ++position)
            m_byKey.add(madeNodes[position].key.hash(), position);
        m_numberFoundByKey = madeNodes.size();
        std::optional<uint32_t> position = m_byKey.find(key.hash(), [&](uint32_t candidate) {
            return madeNodes[candidate].key == key;
        });
        if (!position)
            return std::nullopt;
        return m_firstMadeNodes + *position;
    }

    uint32_t ensure(const ValueFlowNodeKey& key)
    {
        if (std::optional<uint32_t> existing = m_solver.findNumberOfMadeNodes(key))
            return *existing;
        if (std::optional<uint32_t> existing = findAmongNew(key))
            return *existing;
        m_hasMadeMoreOfName = true;
        return makeNodes(key);
    }

    void setParts(uint32_t number, std::span<const uint32_t> numbersOfParts)
    {
        if (numbersOfParts.empty())
            return;
        newAt(number).setFirstPart(m_firstPart + parts.size());
        parts.append(numbersOfParts);
        parts.last() |= isLastPart;
    }

    void makeStatement(uint32_t number)
    {
        newAt(number).setStatement(ValueFlowStatementID(m_firstStatement + madeNodesOfStatement.size()));
        madeNodesOfStatement.append(number);
    }

    void joinNodeUnderKind(uint32_t number)
    {
        const ValueFlowNodeKey& key = at(number).key;
        bool isRead = key.kind == ValueFlowMadeNodeKind::Read;
        const KindOfNames& kind = m_solver.m_kindsOfNames[static_cast<unsigned>(key.name().kind)];
        if (isRead ? kind.writtenKindsItMayFind && m_solver.mayBeComputedKey(key.name()) : kind.readKindsThatMayFindIt && m_solver.isFoundUnderComputedKey(key.name()))
            wishes.append(Wish { number, ValueFlowNodeKey::underKind(isRead ? ValueFlowMadeNodeKind::ReadUnderKind : ValueFlowMadeNodeKind::WrittenUnderKind, key.set, key.name().kind) });
    }

    uint32_t makeNodes(const ValueFlowNodeKey& key)
    {
        uint32_t number = m_firstMadeNodes + madeNodes.size();
        ValueFlowNodeID first(m_firstNode + numberOfNodes);
        numberOfNodes += key.numberOfNodes();
        madeNodes.append(MadeNodes { key, first });
        if (key.set == ValueSets::nothing && (key.kind == ValueFlowMadeNodeKind::Written || key.kind == ValueFlowMadeNodeKind::CallGroup)) {
            madeNodes.last().set(MadeNodes::StaysAtRest);
            stayingAtRest.append(number);
            return number;
        }

        switch (key.kind) {
        case ValueFlowMadeNodeKind::Constant:
            constants.append(number);
            break;
        case ValueFlowMadeNodeKind::Read:
        case ValueFlowMadeNodeKind::Written:
        case ValueFlowMadeNodeKind::ReadUnderKind:
        case ValueFlowMadeNodeKind::WrittenUnderKind:
            if (key.readsOnlyWhatLetsInUnseenCode())
                readsOfWhatLetsInUnseenCode.append(number);
            if (makeUnionWithEarlierNode(number) || makeUnionOfCells(number))
                break;
            listToLink(at(number), readToLink, writtenToLink);
            if (key.kind == ValueFlowMadeNodeKind::Read || key.kind == ValueFlowMadeNodeKind::Written)
                joinNodeUnderKind(number);
            break;
        case ValueFlowMadeNodeKind::Lookup:
            m_setsOfParts.shrink(0);
            if (key.name().kind == ValueFlowNameKind::PrototypeLink)
                m_maker->appendCoarseParts(key.set, m_setsOfParts);
            for (ValueSetID part : m_setsOfParts)
                wishes.append(Wish { number, ValueFlowNodeKey::named(ValueFlowMadeNodeKind::Lookup, part, key.name()) });
            if (m_setsOfParts.isEmpty())
                makeStatement(number);
            break;
        case ValueFlowMadeNodeKind::CallGroup: {
            m_setsOfParts.shrink(0);
            m_maker->appendParts(key.set, m_setsOfParts);
            for (ValueSetID part : m_setsOfParts)
                wishes.append(Wish { number, ValueFlowNodeKey::callGroup(part, key.callKind(), key.markerOfCallGroup()) });
            if (m_setsOfParts.isEmpty())
                makeStatement(number);
            break;
        }
        case ValueFlowMadeNodeKind::Hub:
            makeStatement(number);
            break;
        case ValueFlowMadeNodeKind::Masked:
            masked.append(Masked { number, key.maskKind() == ValueFlowMaskKind::Plain ? ValueSetID() : m_solver.familyObjectsOf(key.set, *m_maker) });
            edges.append(ValueFlowEdge { key.maskedNode(), first });
            break;
        case ValueFlowMadeNodeKind::Marked:
            break;
        case ValueFlowMadeNodeKind::Instance:
            if (key.isInstanceOfNode())
                instances.append(number);
            break;
        }
        return number;
    }

    bool makeUnionOfCoreAndOthers(uint32_t number, size_t sizeOfEarlierSubset, std::span<const uint32_t> similar)
    {
        ValueFlowNodeKey key = at(number).key;
        size_t size = m_sets.size(key.set);
        uint32_t sharingMost = noSolverIndex;
        size_t sizeOfCore = std::max<size_t>({ sizeOfEarlierSubset, size / 2, mostObjectsLookedUpOneByOne });
        for (uint32_t candidate : similar) {
            if (m_sets.size(at(candidate).key.set) <= sizeOfCore)
                continue;
            size_t shared = m_sets.numberOfCommonMembers(key.set, at(candidate).key.set);
            if (shared != size && shared > sizeOfCore) {
                sharingMost = candidate;
                sizeOfCore = shared;
            }
        }
        if (sharingMost == noSolverIndex)
            return false;
        ValueSetID core = m_maker->intersect(key.set, at(sharingMost).key.set);

        bool isRead = key.kind == ValueFlowMadeNodeKind::Read || key.kind == ValueFlowMadeNodeKind::ReadUnderKind;
        ValueFlowNodeID first = at(number).first;
        std::array<uint32_t, 2> numbersOfParts;
        std::array<ValueSetID, 2> sets { core, m_maker->intersect(key.set, ValueSets::complement(core)) };
        for (size_t index = 0; index < 2; ++index) {
            ValueFlowNodeKey keyOfPart = key;
            keyOfPart.set = sets[index];
            numbersOfParts[index] = ensure(keyOfPart);
            ValueFlowNodeID part = at(numbersOfParts[index]).first;
            edges.append(isRead ? ValueFlowEdge { part, first } : ValueFlowEdge { first, part });
        }
        newAt(number).set(MadeNodes::IsUnionOfParts);
        setParts(number, numbersOfParts);
        return true;
    }

    uint8_t rankOf(uint32_t number) const { return at(number).rank() == MadeNodes::partsAreBlocksOfFallingRank ? 0 : at(number).rank(); }

    void makeUnionOf(uint32_t number, std::span<const uint32_t> blocks, uint8_t rank)
    {
        ValueFlowMadeNodeKind kind = at(number).key.kind;
        bool isRead = kind == ValueFlowMadeNodeKind::Read || kind == ValueFlowMadeNodeKind::ReadUnderKind;
        ValueFlowNodeID first = at(number).first;
        for (uint32_t block : blocks)
            edges.append(isRead ? ValueFlowEdge { at(block).first, first } : ValueFlowEdge { first, at(block).first });
        newAt(number).set(MadeNodes::IsUnionOfParts);
        newAt(number).setRank(rank);
        setParts(number, blocks);
    }

    bool makeUnionWithEarlierNode(uint32_t number)
    {
        if (m_rankOfBlockBeingMade) {
            makeUnionOf(number, m_blocksBeingJoined, m_rankOfBlockBeingMade);
            return true;
        }
        ValueFlowNodeKey key = at(number).key;
        if (m_isMakingLeaf || m_isMakingCell || ValueSets::isComplement(key.set) || m_sets.size(key.set) <= mostObjectsLookedUpOneByOne)
            return false;
        uint64_t name = nameOf(key);
        NodesOfManyObjects& nodes = m_solver.m_nodesOfManyObjects[makerOfName(name)];
        uint32_t position = nodes.madeNodes.size();
        nodes.madeNodes.append(number);
        LatestNodes& latest = nodes.latest.add(name, LatestNodes { noSolverIndex, noSolverIndex, noSolverIndex, noSolverIndex }).iterator->value;
        uint32_t earlier = noSolverIndex;
        Vector<uint32_t, 8> similar;
        size_t sizeOfSet = m_sets.size(key.set);
        auto consider = [&](uint32_t positionOfCandidate) {
            if (positionOfCandidate == noSolverIndex)
                return;
            uint32_t candidate = nodes.madeNodes[positionOfCandidate];
            if (candidate == earlier || similar.contains(candidate))
                return;
            if (m_sets.size(at(candidate).key.set) > sizeOfSet || !m_sets.isSubset(at(candidate).key.set, key.set)) {
                similar.append(candidate);
                return;
            }
            if (earlier == noSolverIndex || m_sets.size(at(candidate).key.set) > m_sets.size(at(earlier).key.set))
                earlier = candidate;
        };
        for (uint32_t candidate : latest)
            consider(candidate);
        std::ranges::copy_backward(latest.begin(), latest.end() - 1, latest.end());
        latest[0] = position;

        for (uint64_t block : m_sets.sampleOfBlocks(key.set)) {
            if (block == ValueSets::noBlock)
                continue;
            uint32_t& latestWithBlock = nodes.latestWithBlock.add(((name * 0x9e3779b97f4a7c15ull) ^ (block * 0xc2b2ae3d27d4eb4full)) >> 1, noSolverIndex).number;
            if (latestWithBlock != noSolverIndex) {
                const ValueFlowNodeKey& other = at(nodes.madeNodes[latestWithBlock]).key;
                if (other.kind == key.kind && other.detail == key.detail && other.payload == key.payload)
                    consider(latestWithBlock);
            }
            latestWithBlock = position;
        }
        if (makeUnionOfCoreAndOthers(number, earlier == noSolverIndex ? 0 : m_sets.size(at(earlier).key.set), similar.span()))
            return true;
        if (earlier == noSolverIndex)
            return false;
        ValueSetID earlierSet = at(earlier).key.set;

        // As a binary counter is counted up: a set that grows a member at a time lays a number of edges that grows with the logarithm of its versions.
        Vector<uint32_t, 16> blocks;
        if (at(earlier).rank() == MadeNodes::partsAreBlocksOfFallingRank)
            forEachPartOf(at(earlier), [&](uint32_t part) {
                blocks.append(part);
            });
        else
            blocks.append(earlier);

        ValueFlowNodeKey keyOfDifference = key;
        keyOfDifference.set = m_maker->intersect(key.set, ValueSets::complement(earlierSet));
        m_isMakingLeaf = true;
        blocks.append(ensure(keyOfDifference));
        m_isMakingLeaf = false;

        uint8_t rank = MadeNodes::partsAreBlocksOfFallingRank;
        while (rankOf(blocks[blocks.size() - 2]) <= rankOf(blocks.last())) {
            uint8_t rankOfUnion = std::min<uint8_t>(rankOf(blocks.last()) + 1, MadeNodes::partsAreBlocksOfFallingRank - 1);
            if (blocks.size() == 2) {
                rank = rankOfUnion;
                break;
            }
            m_blocksBeingJoined = { blocks[blocks.size() - 2], blocks.last() };
            ValueFlowNodeKey keyOfUnion = key;
            keyOfUnion.set = m_maker->unite(at(m_blocksBeingJoined[0]).key.set, at(m_blocksBeingJoined[1]).key.set);
            m_rankOfBlockBeingMade = rankOfUnion;
            uint32_t unionOfBlocks = ensure(keyOfUnion);
            m_rankOfBlockBeingMade = 0;
            blocks.shrink(blocks.size() - 2);
            blocks.append(unionOfBlocks);
        }
        makeUnionOf(number, blocks.span(), rank);
        return true;
    }

    // The sets of very many objects that one name is read or written on overlap. Cells are sets of very many objects that each such set holds whole or not at all.
    // A set is the union of the cells inside it and of what is left. Only these are linked, so nodes that meet what several sets share are connected to it once.
    bool makeUnionOfCells(uint32_t number)
    {
        ValueFlowNodeKey key = at(number).key;
        uint64_t mostObjectsListedByMember = m_solver.mostObjectsListedByMember();
        if (m_isMakingCell || ValueSets::isComplement(key.set) || m_sets.size(key.set) <= mostObjectsListedByMember)
            return false;
        uint64_t name = nameOf(key);
        Vector<ValueSetID>& cells = m_solver.m_nodesOfManyObjects[makerOfName(name)].cells.add(name, Vector<ValueSetID>()).iterator->value;
        Vector<ValueSetID, 16> inside;
        Vector<uint32_t, 16> cut;
        for (uint32_t index = 0; index < cells.size(); ++index) {
            uint32_t common = m_sets.numberOfCommonMembers(cells[index], key.set);
            if (common == m_sets.size(cells[index]))
                inside.append(cells[index]);
            else if (common > mostObjectsListedByMember)
                cut.append(index);
        }
        if (cells.size() + cut.size() >= mostCellsOfName)
            return false;

        for (uint32_t index : cut) {
            ValueSetID cell = cells[index];
            cells[index] = m_maker->intersect(cell, key.set);
            inside.append(cells[index]);
            ValueSetID outside = m_maker->intersect(cell, ValueSets::complement(key.set));
            if (m_sets.size(outside) > mostObjectsListedByMember)
                cells.append(outside);
        }
        if (inside.isEmpty()) {
            cells.append(key.set);
            return false;
        }
        ValueSetID rest = m_maker->intersect(key.set, ValueSets::complement(m_maker->uniteAll(inside.span())));
        if (rest == ValueSets::nothing && inside.size() == 1)
            return false;
        if (m_sets.size(rest) > mostObjectsListedByMember)
            cells.append(rest);
        if (rest != ValueSets::nothing)
            inside.append(rest);

        Vector<uint32_t, 16> parts;
        m_isMakingCell = true;
        for (ValueSetID part : inside) {
            ValueFlowNodeKey keyOfPart = key;
            keyOfPart.set = part;
            parts.append(ensure(keyOfPart));
        }
        m_isMakingCell = false;
        makeUnionOf(number, parts.span(), 0);
        return true;
    }

    ValueFlowSolver& m_solver;
    const ValueSets& m_sets;
    ValueSetMaker* m_maker { nullptr };
    const uint32_t m_firstMadeNodes;
    const uint32_t m_firstNode;
    const uint32_t m_firstStatement;
    const uint32_t m_firstPart;
    HashedIndex m_byKey;
    size_t m_numberFoundByKey { 0 };
    uint64_t m_name { UINT64_MAX };
    size_t m_firstOfName { 0 };
    bool m_hasMadeMoreOfName { false };
    Vector<ValueSetID> m_setsOfParts;
    bool m_isMakingLeaf { false };
    bool m_isMakingCell { false };
    uint8_t m_rankOfBlockBeingMade { 0 };
    std::array<uint32_t, 2> m_blocksBeingJoined { };
};

void ValueFlowSolver::takeWhatWasMade(std::span<const std::unique_ptr<NodeMaker>> makers, std::span<KeyToMake> keys)
{
    uint32_t firstMadeNodes = m_madeNodes.size();
    uint32_t firstStatement = m_madeNodesOfStatement.size();
    uint32_t firstPart = m_partsOfMadeNodes.size();
    size_t firstEdge = m_edgesOfRound.size();
    size_t firstReadToLink = m_readToLink.size();
    size_t firstWrittenToLink = m_writtenToLink.size();
    NodeMaker::NumbersMade total;
    for (const std::unique_ptr<NodeMaker>& maker : makers) {
        maker->before = total;
        total.madeNodes += maker->madeNodes.size();
        total.nodes += maker->numberOfNodes;
        total.statements += maker->madeNodesOfStatement.size();
        total.parts += maker->parts.size();
        total.edges += maker->edges.size();
        total.readToLink += maker->readToLink.size();
        total.writtenToLink += maker->writtenToLink.size();
    }

    uint32_t firstNode = m_graph.addNodes(total.nodes).index();
    m_numberOfNodesMade += total.nodes;
    m_madeNodes.makeRoomToFill(total.madeNodes);
    growAndFill(m_numberOfUsers, m_madeNodes.size(), 0u);
    m_madeNodesOfNode.makeRoomToFill(total.nodes);
    growBy(m_madeNodesOfStatement, total.statements);
    growBy(m_partsOfMadeNodes, total.parts);
    growBy(m_edgesOfRound, total.edges);
    m_readToLink.makeRoomToFill(total.readToLink);
    m_writtenToLink.makeRoomToFill(total.writtenToLink);
    growByDoubling(m_isDue, numberOfStatements());

    m_stepTimes.didFinish("rules: made: taken: sizes summed, room made (one thread)"_s);
    bool isOneMaker = makers.size() == 1;
    auto take = [&](unsigned index, unsigned worker) {
        NodeMaker& maker = *makers[index];
        for (size_t position = 0; position < maker.madeNodes.size(); ++position) {
            MadeNodes made = maker.madeNodes[position];
            uint32_t number = firstMadeNodes + maker.before.madeNodes + position;
            made.first = maker.nodeMovedUp(made.first);
            if (made.has(MadeNodes::HasParts))
                made.setFirstPart(maker.partMovedUp(made.firstPart()));
            else
                made.setStatement(maker.statementMovedUp(made.statement()));
            for (unsigned offset = 0; offset < made.key.numberOfNodes(); ++offset)
                m_madeNodesOfNode[made.first.index() + offset - m_input.numberOfNodes] = number;
            m_madeNodes[number] = made;
            uint32_t hash = made.key.hash();
            IndexOfMadeNodes& madeNodes = indexOfMadeNodesWith(hash);
            Locker locker { madeNodes.lock };
            madeNodes.add(tableOfMadeNodesWith(hash), hash, number, m_fewestSlotsOfIndexOfMadeNodes, m_madeNodes);
        }
        for (size_t position = 0; position < maker.madeNodesOfStatement.size(); ++position)
            m_madeNodesOfStatement[firstStatement + maker.before.statements + position] = maker.madeNodesMovedUp(maker.madeNodesOfStatement[position]);
        for (size_t position = 0; position < maker.parts.size(); ++position)
            m_partsOfMadeNodes[firstPart + maker.before.parts + position] = maker.madeNodesMovedUp(maker.parts[position] & ~isLastPart) | (maker.parts[position] & isLastPart);
        for (size_t position = 0; position < maker.edges.size(); ++position)
            m_edgesOfRound[firstEdge + maker.before.edges + position] = ValueFlowEdge { maker.nodeMovedUp(maker.edges[position].from), maker.nodeMovedUp(maker.edges[position].to) };
        for (size_t position = 0; position < maker.readToLink.size(); ++position) {
            NodeToLink toLink = maker.readToLink[position];
            toLink.node.node = maker.nodeMovedUp(toLink.node.node);
            m_readToLink[firstReadToLink + maker.before.readToLink + position] = toLink;
        }
        for (size_t position = 0; position < maker.writtenToLink.size(); ++position) {
            NodeToLink toLink = maker.writtenToLink[position];
            toLink.node.node = maker.nodeMovedUp(toLink.node.node);
            m_writtenToLink[firstWrittenToLink + maker.before.writtenToLink + position] = toLink;
        }
        for (NodeMaker::Wish& wish : maker.wishes)
            wish.madeNodes = maker.madeNodesMovedUp(wish.madeNodes);
        for (uint32_t key : maker.keys)
            keys[key].madeNodes = maker.madeNodesMovedUp(keys[key].madeNodes);
        for (uint32_t number : maker.stayingAtRest) {
            const MadeNodes& made = m_madeNodes[maker.madeNodesMovedUp(number)];
            m_graph.putToRest(made.first, made.key.numberOfNodes());
        }
        for (uint32_t number : maker.constants) {
            const MadeNodes& made = m_madeNodes[maker.madeNodesMovedUp(number)];
            m_graph.makeConstant(made.first, made.key.set, worker, made.key.unknownSource() ? ValueFlowGraph::EdgesOutOfConstant::AreListed : ValueFlowGraph::EdgesOutOfConstant::AreKeptAtTargetsAlone);
        }
        if (!isOneMaker && maker.before.madeNodes) {
            Vector<uint32_t>& ofManyObjects = m_nodesOfManyObjects[index].madeNodes;
            for (size_t position = ofManyObjects.size(); position && ofManyObjects[position - 1] >= firstMadeNodes; --position)
                ofManyObjects[position - 1] += maker.before.madeNodes;
        }
    };
    if (isOneMaker)
        take(0, 0);
    else
        forEachIndexInParallel(m_loops, makers.size(), take);
    RELEASE_ASSERT(firstNode + total.nodes == m_graph.numberOfNodes());
    m_stepTimes.didFinish("rules: made: taken: moved up"_s);

    for (const std::unique_ptr<NodeMaker>& maker : makers) {
        for (const NodeMaker::Masked& masked : maker->masked) {
            const MadeNodes& made = m_madeNodes[maker->madeNodesMovedUp(masked.madeNodes)];
            m_graph.setMask(made.first, made.key.set, made.key.maskKind(), masked.familyObjects);
            m_graph.startWithValueOf(made.first, made.key.maskedNode());
        }
        for (uint32_t number : maker->readsOfWhatLetsInUnseenCode)
            m_graph.setMask(m_madeNodes[maker->madeNodesMovedUp(number)].first, m_input.universe->builtinTables().heap().objectsThatLetInUnseenCode());
        for (uint32_t number : maker->instances) {
            const MadeNodes& made = m_madeNodes[maker->madeNodesMovedUp(number)];
            ValueFlowNodeID node = made.key.nodeOfInstance();
            if (m_graph.maskOf(node) != ValueSets::anything)
                m_graph.setMask(made.first, m_graph.maskOf(node), m_graph.maskKindOf(node), m_graph.familyObjectsOf(node));
        }
    }
    m_stepTimes.didFinish("rules: made: taken: masks set (one thread)"_s);
}

void ValueFlowSolver::makeNodesOf(std::span<KeyToMake> keys)
{
    if (keys.empty())
        return;
    AOT_THRESHOLD(size_t, fewestKeysForAllThreads, 256);
    Vector<NodeMaker::Wish> wishes;
    if (keys.size() < fewestKeysForAllThreads) {
        Vector<std::unique_ptr<NodeMaker>> makers(1, [&](size_t) {
            return makeUnique<NodeMaker>(*this);
        });
        for (uint32_t index = 0; index < keys.size(); ++index)
            makers[0]->keys.append(index);
        makers[0]->run(keys, *m_makers[0]);
        m_stepTimes.didFinish("rules: made: one maker runs (one thread)"_s);
        takeWhatWasMade(makers.span(), keys);
        wishes = WTF::move(makers[0]->wishes);
    } else {
        // Each creator's keys in order, first for creators of keys that may bring others, then for the rest: counted and then written per share, on all threads.
        constexpr size_t keysPerShare = 256;
        constexpr unsigned numberOfLists = 2 * numberOfNodeMakers;
        size_t numberOfShares = (keys.size() + keysPerShare - 1) / keysPerShare;
        Vector<uint8_t> listOfKey(keys.size());
        static_assert(numberOfLists <= 256);
        Vector<std::array<uint32_t, numberOfLists>> placeInListsOfShare(numberOfShares);
        forEachIndexInParallel(m_loops, numberOfShares, [&](unsigned share) {
            std::array<uint32_t, numberOfLists>& numberInLists = placeInListsOfShare[share];
            numberInLists.fill(0);
            for (size_t index = share * keysPerShare; index < std::min(keys.size(), (share + 1) * keysPerShare); ++index) {
                listOfKey[index] = NodeMaker::makerOf(m_sets, keys[index]) + (NodeMaker::mayBringOthers(m_sets, keys[index].key) ? 0 : numberOfNodeMakers);
                ++numberInLists[listOfKey[index]];
            }
        });
        Vector<Vector<uint32_t>> keysInList(numberOfLists);
        forEachIndexInParallel(m_loops, numberOfLists, [&](unsigned list) {
            uint32_t number = 0;
            for (std::array<uint32_t, numberOfLists>& placeInLists : placeInListsOfShare)
                number += std::exchange(placeInLists[list], number);
            keysInList[list].grow(number);
        });
        forEachIndexInParallel(m_loops, numberOfShares, [&](unsigned share) {
            std::array<uint32_t, numberOfLists>& placeInLists = placeInListsOfShare[share];
            for (size_t index = share * keysPerShare; index < std::min(keys.size(), (share + 1) * keysPerShare); ++index)
                keysInList[listOfKey[index]][placeInLists[listOfKey[index]]++] = static_cast<uint32_t>(index);
        });
        m_stepTimes.didFinish("rules: made: keys dealt to the makers"_s);
        for (bool bringOthers : { true, false }) {
            std::span<Vector<uint32_t>> keysOfCreators = keysInList.mutableSpan().subspan(bringOthers ? 0 : numberOfNodeMakers, numberOfNodeMakers);
            if (std::ranges::all_of(keysOfCreators, &Vector<uint32_t>::isEmpty))
                continue;
            Vector<std::unique_ptr<NodeMaker>> makers(numberOfNodeMakers, [&](size_t index) {
                auto maker = makeUnique<NodeMaker>(*this);
                maker->keys = WTF::move(keysOfCreators[index]);
                return maker;
            });
            m_stepTimes.didFinish("rules: made: makers made (one thread)"_s);
            forEachIndexInParallel(m_loops, makers.size(), [&](unsigned index, unsigned worker) {
                makers[index]->run(keys, *m_makers[worker], !bringOthers);
            });
            m_stepTimes.didFinish("rules: made: all makers run"_s);
            takeWhatWasMade(makers.span(), keys);
            for (const std::unique_ptr<NodeMaker>& maker : makers)
                wishes.appendVector(maker->wishes);
            m_stepTimes.didFinish("rules: made: wishes in one list (one thread)"_s);
        }
        m_stepTimes.didFinish("rules: made: makers destroyed (one thread)"_s);
    }
    if (wishes.isEmpty())
        return;

    constexpr size_t wishesPerShare = 1024;
    unsigned numberOfShares = (wishes.size() + wishesPerShare - 1) / wishesPerShare;
    auto forEachWishInParallel = [&](NOESCAPE const Invocable<void(unsigned share, NodeMaker::Wish&)> auto& functor) {
        forEachIndexInParallel(m_loops, numberOfShares, [&](unsigned share) {
            for (size_t index = share * wishesPerShare; index < std::min(wishes.size(), (share + 1) * wishesPerShare); ++index)
                functor(share, wishes[index]);
        });
    };
    Vector<Vector<KeyToMake>> missingOfShare(numberOfShares);
    forEachWishInParallel([&](unsigned share, NodeMaker::Wish& wish) {
        if (std::optional<uint32_t> existing = findNumberOfMadeNodes(wish.key))
            wish.granted = *existing;
        else
            missingOfShare[share].append(KeyToMake { wish.key, wish.key.set ? m_sets.hashOfMembers(wish.key.set) : 0 });
    });
    m_stepTimes.didFinish("rules: made: wishes looked up"_s);
    Vector<KeyToMake> missing;
    for (const Vector<KeyToMake>& ofShare : missingOfShare)
        missing.appendVector(ofShare);
    if (!missing.isEmpty()) {
        auto orderOf = [](const KeyToMake& toMake) {
            return std::tuple { toMake.key.kind, toMake.key.detail, toMake.key.payload, toMake.hashOfMembers, toMake.key.set.index() };
        };
        parallelSort(m_loops, missing.mutableSpan(), [&](const KeyToMake& first, const KeyToMake& second) {
            return orderOf(first) < orderOf(second);
        });
        missing.shrink(std::unique(missing.begin(), missing.end(), [](const KeyToMake& first, const KeyToMake& second) {
            return first.key == second.key;
        }) - missing.begin());
        m_stepTimes.didFinish("rules: made: missing wishes sorted"_s);
        makeNodesOf(missing.mutableSpan());
        forEachWishInParallel([&](unsigned, NodeMaker::Wish& wish) {
            if (wish.granted == noSolverIndex)
                wish.granted = *findNumberOfMadeNodes(wish.key);
        });
        m_stepTimes.didFinish("rules: made: wishes looked up again"_s);
    }

    size_t firstPart = m_partsOfMadeNodes.size();
    growBy(m_partsOfMadeNodes, wishes.size());
    Vector<size_t> firstEdgeOfShare(numberOfShares + 1);
    firstEdgeOfShare[0] = m_edgesOfRound.size();
    forEachIndexInParallel(m_loops, numberOfShares, [&](unsigned share) {
        size_t edges = 0;
        for (size_t index = share * wishesPerShare; index < std::min(wishes.size(), (share + 1) * wishesPerShare); ++index)
            edges += wishes[index].key.numberOfNodes();
        firstEdgeOfShare[share + 1] = edges;
    });
    for (unsigned share = 0; share < numberOfShares; ++share)
        firstEdgeOfShare[share + 1] += firstEdgeOfShare[share];
    growBy(m_edgesOfRound, firstEdgeOfShare.last() - m_edgesOfRound.size());
    forEachIndexInParallel(m_loops, numberOfShares, [&](unsigned share) {
        size_t edge = firstEdgeOfShare[share];
        for (size_t index = share * wishesPerShare; index < std::min(wishes.size(), (share + 1) * wishesPerShare); ++index) {
            const NodeMaker::Wish& wish = wishes[index];
            MadeNodes& made = m_madeNodes[wish.madeNodes];
            bool isLast = index + 1 == wishes.size() || wishes[index + 1].madeNodes != wish.madeNodes;
            m_partsOfMadeNodes[firstPart + index] = wish.granted | (isLast ? isLastPart : 0);
            if (!index || wishes[index - 1].madeNodes != wish.madeNodes)
                made.setFirstPart(firstPart + index);
            forEachEdgeToPart(wish.key.kind, made.first, m_madeNodes[wish.granted].first, [&](ValueFlowEdge toPart) {
                m_edgesOfRound[edge++] = toPart;
            });
        }
    });
    m_stepTimes.didFinish("rules: made: parts and their edges"_s);
}

ValueFlowStatement ValueFlowSolver::statementMadeByRuleAt(ValueFlowStatementID statement) const
{
    uint32_t madeNodes = m_madeNodesOfStatement[statement.index() - m_input.statements.size()];
    if (madeNodes != noSolverIndex) [[likely]]
        return statementMadeWith(m_madeNodes[madeNodes]);
    ValueFlowStatement escape;
    escape.kind = ValueFlowStatementKind::Escape;
    return escape;
}

ValueFlowStatement ValueFlowSolver::statementMadeWith(const MadeNodes& made)
{
    const ValueFlowNodeKey& key = made.key;
    ValueFlowStatement statement;
    statement.kind = key.kind == ValueFlowMadeNodeKind::Lookup ? ValueFlowStatementKind::Lookup : key.kind == ValueFlowMadeNodeKind::CallGroup ? ValueFlowStatementKind::CallGroup : ValueFlowStatementKind::Behavior;
    statement.result = key.kind == ValueFlowMadeNodeKind::CallGroup ? callGroupAt(made.first).result : made.first;
    if (key.kind == ValueFlowMadeNodeKind::CallGroup)
        statement.thrown = callGroupAt(made.first).thrown;
    return statement;
}

// A trap of a proxy whose handler is followed is reached by the rule that asks the proxy by it. Nothing else may be done to such a proxy, and no rule is trusted to say so: a rule may walk a chain, pass
// an operand by or only hand a value on without a look at what it holds. The graph says where the proxy lies and who may touch it there. What is not known to be harmless opens the proxy.
ValueSetID ValueFlowSolver::followedProxiesThatMoreMayBeDoneTo()
{
    if (!m_input.nodes || !m_input.universe)
        return ValueSets::nothing;
    ValueSetMaker& maker = *m_makers[0];
    const ValueFlowProgramNodes& nodes = *m_input.nodes;
    ValueFlowNodeID listOfFollowed = nodes.ofHost(ValueFlowHostNode::ProxiesWhoseHandlerIsFollowed);
    ValueFlowNodeID listOfOpen = nodes.ofHost(ValueFlowHostNode::ProxiesThatAreOpen);
    ValueFlowNodeID listOfThoseThatWalksPass = nodes.ofHost(ValueFlowHostNode::ProxiesThatHandOnWhatComesFromBelow);
    ValueFlowNodeID listOfThoseWithTrapsForWalks = nodes.ofHost(ValueFlowHostNode::ProxiesWithTrapsForWhatComesFromBelow);
    ValueSetID open = maker.uniteAll({ { m_graph.valueOf(listOfOpen), m_graph.valueOf(ValueFlowProgramNodes::escaped()), m_graph.valueOf(ValueFlowProgramNodes::lost()) } });
    ValueSetID asked = maker.intersect(m_graph.valueOf(listOfFollowed), ValueSets::complement(open));
    if (asked == ValueSets::nothing)
        return ValueSets::nothing;

    // Every node, every record of created nodes and every statement is examined independently, on all threads.
    Vector<ValueSetID> foundByWorker(FillWith { }, m_makers.size(), ValueSets::nothing);
    auto noteFor = [&](unsigned worker, ValueSetID held) {
        ValueSetMaker& makerOfWorker = *m_makers[worker];
        if (held != ValueSets::nothing && makerOfWorker.meet(held, asked))
            foundByWorker[worker] = makerOfWorker.unite(foundByWorker[worker], makerOfWorker.intersect(held, asked));
    };
    auto note = [&](ValueSetID held) {
        noteFor(0, held);
    };
    // A share starts at a word boundary of a bit vector.
    constexpr uint32_t indicesPerShare = 1 << 12;
    auto forEachShare = [&](uint32_t count, NOESCAPE const Invocable<void(uint32_t first, uint32_t end, unsigned share, unsigned worker)> auto& functor) {
        forEachIndexInParallel(m_loops, (count + indicesPerShare - 1) / indicesPerShare, [&](unsigned share, unsigned worker) {
            functor(share * indicesPerShare, std::min(count, (share + 1) * indicesPerShare), share, worker);
        });
    };

    // A lookup of a name asks a proxy by its trap.
    uint32_t firstHub = nodes.hub(ValueFlowBehaviorID(0u)).index();
    auto onlyLooksNamesUp = [](const ValueFlowBehavior& behavior) {
        if (behavior.describesRefusedOperands || !behavior.calledMembers.empty())
            return false;
        return behavior.protocols == Protocols { Protocol::HasInstance } || (behavior.protocols == Protocols { Protocol::Named } && !behavior.namedMembers.empty() && !behavior.isTransitive);
    };
    // In hands that no statement stands for: what the host holds, what a behavior of built-ins takes in.
    for (uint32_t index = nodes.ofHost(static_cast<ValueFlowHostNode>(0)).index(); index < m_input.numberOfNodes; ++index) {
        if (index >= firstHub && (onlyLooksNamesUp(m_input.universe->builtinTables().at(ValueFlowBehaviorID(index - firstHub))) || onlyLists(m_input.universe->builtinTables().at(ValueFlowBehaviorID(index - firstHub)))))
            continue;
        if (index != listOfFollowed.index() && index != listOfOpen.index() && index != listOfThoseThatWalksPass.index() && index != listOfThoseWithTrapsForWalks.index())
            note(m_graph.valueOf(ValueFlowNodeID(index)));
    }

    BitVector holdsProxy(m_input.numberOfNodes);
    HashMap<uint32_t, ValueSetID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> heldByInstancesOfNode;
    forEachShare(nodes.ofHost(static_cast<ValueFlowHostNode>(0)).index(), [&](uint32_t first, uint32_t end, unsigned, unsigned worker) {
        for (uint32_t index = first; index < end; ++index) {
            if (ValueSetID held = m_graph.valueOf(ValueFlowNodeID(index)); held != ValueSets::nothing && m_makers[worker]->meet(held, asked))
                holdsProxy.quickSet(index);
        }
    });

    ValueSetID closures = m_input.universe->closures();
    ValueSetID stopWalks = maker.unite(ValueSets::complement(m_graph.valueOf(listOfThoseThatWalksPass)), m_graph.valueOf(listOfThoseWithTrapsForWalks));
    Vector<const MadeNodes*> links;
    uint32_t numberOfSharesOfMadeNodes = (m_madeNodes.size() + indicesPerShare - 1) / indicesPerShare;
    Vector<Vector<const MadeNodes*>> linksInShare(numberOfSharesOfMadeNodes);
    Vector<Vector<const MadeNodes*>> proxyHoldingInstancesPerShare(numberOfSharesOfMadeNodes);
    // The table says how a data property of a built-in is read. Nothing is called, and the receiver goes nowhere.
    auto readsOnlyDataOfTable = [&](const ValueFlowNodeKey& group) {
        if (group.callKind() != ValueFlowCallKind::Get || ValueSets::isComplement(group.set))
            return false;
        bool readsOnlyData = true;
        m_sets.forEachMember(group.set, [&](ValueFlowObjectID callee) {
            BuiltinID builtin = m_input.universe->builtinOf(callee);
            readsOnlyData &= m_sets.contains(closures, callee) || (builtin && (m_input.program->builtins().at(builtin).kind == BuiltinKind::DataProperty || onlyAsksWhatStandsAbove(m_input.program->builtins(), builtin, BuiltinOperand::receiver(), BuiltinCallMode::Get)));
        });
        return readsOnlyData;
    };
    // A built-in that lists an argument asks a proxy by its traps, whoever calls it.
    auto isOnlyListedByCalleesOf = [&](const ValueFlowNodeKey& key, const ValueFlowCallGroup& group, ValueFlowNodeID node) {
        if (ValueSets::isComplement(key.set) || node.index() < group.firstArgument.index() || node.index() >= group.firstArgument.index() + numberOfGroupedArguments)
            return false;
        bool isOnlyListed = true;
        m_sets.forEachMember(key.set, [&](ValueFlowObjectID callee) {
            BuiltinID builtin = m_sets.contains(m_input.universe->builtinFunctions(), callee) ? m_input.universe->builtinOf(callee) : BuiltinID();
            isOnlyListed &= m_sets.contains(closures, callee) || (builtin && onlyListsOperand(m_input.program->builtins(), builtin, BuiltinOperand::argument(node.index() - group.firstArgument.index())));
        });
        return isOnlyListed;
    };
    // A rule that asks about all objects at once, whatever they are, has taken none of them out of a value.
    ValueSetID everyObjectThatBuiltinsMake = maker.intersect(m_input.universe->objectsThatMayBeAnything(), m_input.universe->knownObjects());
    // Flow follows a value out of a cell that the table names, and out of the cell of a member whose reads are answered from it.
    auto isReadBackOutOfBuiltins = [&](ValueFlowName name, ValueSetID holders) {
        const BuiltinTable& builtins = m_input.program->builtins();
        if ((name.kind != ValueFlowNameKind::Slot && name.kind != ValueFlowNameKind::HiddenSlot) || name.payload < builtins.slotCount())
            return false;
        if (name.payload < builtins.slotCount() + builtins.nameCount())
            return true;
        if (name.payload != cellOfValueFromLibrary(builtins).payload)
            return false;
        bool isReadBack = true;
        m_sets.forEachMember(holders, [&](ValueFlowObjectID holder) {
            BuiltinID entry = m_input.universe->builtinOf(holder);
            isReadBack &= entry && m_input.universe->builtinTables().holdsValueFromLibrary(entry);
        });
        return isReadBack;
    };
    BitVector readsProxy(m_madeNodes.size());
    std::atomic<bool> someReadHoldsProxy { false };
    forEachShare(m_madeNodes.size(), [&](uint32_t firstOfShare, uint32_t endOfShare, unsigned share, unsigned worker) {
        ValueSetMaker& maker = *m_makers[worker];
        auto note = [&](ValueSetID held) {
            noteFor(worker, held);
        };
        bool holdsRead = false;
        for (const MadeNodes& made : m_madeNodes.span().subspan(firstOfShare, endOfShare - firstOfShare)) {
            const ValueFlowNodeKey& key = made.key;
            switch (key.kind) {
            case ValueFlowMadeNodeKind::Constant:
            case ValueFlowMadeNodeKind::Masked:
                break;
            case ValueFlowMadeNodeKind::Instance:
                if (key.isInstanceOfNode() && key.nodeOfInstance().index() < m_input.numberOfNodes && maker.meet(m_graph.valueOf(made.first), asked))
                    proxyHoldingInstancesPerShare[share].append(&made);
                break;
            case ValueFlowMadeNodeKind::Hub:
                note(m_graph.valueOf(made.first));
                break;
            case ValueFlowMadeNodeKind::Marked:
                // The model writes the link from a proxy to its target.
                if (key != ValueFlowNodeKey::objectsWithRareWrite(ValueFlowRareWrite::PrototypeLink))
                    note(m_graph.valueOf(made.first));
                break;
            case ValueFlowMadeNodeKind::CallGroup:
                // Calling a proxy goes through its trap.
                if (!maker.meet(key.set, ValueSets::complement(closures)) || readsOnlyDataOfTable(key))
                    break;
                for (unsigned offset = 0; offset < key.numberOfNodes(); ++offset) {
                    ValueFlowNodeID node(made.first.index() + offset);
                    if (node != callGroupAt(made.first).result && node != callGroupAt(made.first).thrown && !isOnlyListedByCalleesOf(key, callGroupAt(made.first), node))
                        note(m_graph.valueOf(node));
                }
                break;
            case ValueFlowMadeNodeKind::Read:
            case ValueFlowMadeNodeKind::Written:
            case ValueFlowMadeNodeKind::Lookup:
            case ValueFlowMadeNodeKind::WrittenUnderKind:
            case ValueFlowMadeNodeKind::ReadUnderKind: {
                // The heap has been queried about the proxy itself, or the proxy is on an object's prototype chain.
                ValueFlowName name = key.name();
                bool isUnderKind = key.kind == ValueFlowMadeNodeKind::WrittenUnderKind || key.kind == ValueFlowMadeNodeKind::ReadUnderKind;
                bool isOfModel = name.kind == ValueFlowNameKind::AllocatedAs || (name.kind == ValueFlowNameKind::HiddenSlot && (isUnderKind || name.payload == proxyItselfName.payload || name.payload == listedValuesOfProxyName.payload)) || (name.kind == ValueFlowNameKind::Slot && (isUnderKind || name.payload == BuiltinSlots::ProxyHandler.index() || name.payload == BuiltinSlots::ProxyTarget.index()));
                // A walk that reaches a proxy continues at its target, unless the handler may have a trap for the walk's purpose.
                bool isLink = shapeOf(name.kind).sort == ValueFlowNameSort::PrototypeLink;
                isOfModel |= isLink && key.kind == ValueFlowMadeNodeKind::Written;
                // A lookup by name that starts at a proxy goes through its trap.
                bool asksWhereItStarts = key.kind == ValueFlowMadeNodeKind::Lookup && (name.kind == ValueFlowNameKind::Property || name.kind == ValueFlowNameKind::Elements || name == everyPropertyName);
                if (!isOfModel && !asksWhereItStarts && maker.meet(everyObjectThatBuiltinsMake, ValueSets::complement(key.set)))
                    note(isLink ? maker.intersect(key.set, stopWalks) : key.set);
                // A bound function's target is read to be called or to be tested against, and either goes through a proxy's trap.
                bool readsTargetOfBoundFunction = name.kind == ValueFlowNameKind::HiddenSlot && name.payload == BuiltinSlots::BoundFunctionTarget.index();
                if (!isOfModel && !readsTargetOfBoundFunction && key.kind != ValueFlowMadeNodeKind::Written && key.kind != ValueFlowMadeNodeKind::WrittenUnderKind && maker.meet(isLink ? maker.intersect(m_graph.valueOf(made.first), stopWalks) : m_graph.valueOf(made.first), asked)) {
                    readsProxy.quickSet(&made - m_madeNodes.span().data());
                    holdsRead = true;
                }
                if (isLink) {
                    note(maker.intersect(m_graph.valueOf(made.first), stopWalks));
                    if (key.kind == ValueFlowMadeNodeKind::Written)
                        linksInShare[share].append(&made);
                }
                // What is stored into a built-in is visible to native code.
                if (key.kind == ValueFlowMadeNodeKind::Written && maker.meet(key.set, m_input.universe->builtinObjects()) && !isReadBackOutOfBuiltins(name, key.set))
                    note(m_graph.valueOf(made.first));
                break;
            }
            }
        }
        if (holdsRead)
            someReadHoldsProxy.store(true, std::memory_order_relaxed);
    });
    for (uint32_t share = 0; share < numberOfSharesOfMadeNodes; ++share) {
        links.appendVector(linksInShare[share]);
        for (const MadeNodes* made : proxyHoldingInstancesPerShare[share]) {
            holdsProxy.quickSet(made->key.nodeOfInstance().index());
            ValueSetID& held = heldByInstancesOfNode.add(made->key.nodeOfInstance().index(), ValueSets::nothing).iterator->value;
            held = maker.unite(held, m_graph.valueOf(made->first));
        }
    }

    // An operand that an operation only keeps or hands on does nothing to what it holds, unless what the operation calls is no function of the program.
    // The language goes up from an object by the same steps whatever stands above it. Code of the host may look at what stands above an object in ways of its own.
    ValueSetID standingAbove = ValueSets::nothing;
    for (const MadeNodes* link : links)
        standingAbove = maker.unite(standingAbove, maker.intersect(m_graph.valueOf(link->first), asked));
    if (standingAbove != ValueSets::nothing) {
        const BuiltinTable& builtins = m_input.program->builtins();
        const ValueFlowBuiltinTables& tables = m_input.universe->builtinTables();
        BitVector behaviorsOfHost(tables.numberOfBehaviors());
        for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
            if (builtins.at(BuiltinID(index)).origin != BuiltinOrigin::Host)
                continue;
            for (const BuiltinOverload& overload : builtins.overloadsOf(BuiltinID(index))) {
                auto add = [&](const BuiltinParameter& parameter) {
                    if (ValueFlowBehaviorID behavior = tables.behaviorOf(parameter))
                        behaviorsOfHost.quickSet(behavior.index());
                };
                if (overload.receiver)
                    add(*overload.receiver);
                for (const BuiltinParameter& parameter : overload.parameters)
                    add(parameter);
            }
        }
        ValueSetID reached = maker.uniteAll({ { m_graph.valueOf(nodes.ofHost(ValueFlowHostNode::HeldByNativeCode)), m_graph.valueOf(nodes.ofHost(ValueFlowHostNode::ValuesCarriedOut)), m_graph.valueOf(ValueFlowProgramNodes::escaped()), m_graph.valueOf(ValueFlowProgramNodes::lost()) } });
        for (size_t behavior : behaviorsOfHost)
            reached = maker.unite(reached, m_graph.valueOf(nodes.hub(ValueFlowBehaviorID(static_cast<uint32_t>(behavior)))));
        ValueSetID knownObjects = m_input.universe->knownObjects();
        reached = maker.intersect(reached, knownObjects);
        for (ValueSetID last = reached; last != ValueSets::nothing;) {
            ValueSetID above = ValueSets::nothing;
            for (const MadeNodes* link : links) {
                if (maker.meet(link->key.set, last))
                    above = maker.unite(above, m_graph.valueOf(link->first));
            }
            Vector<ValueFlowObjectID, 64> aboveAtCreation;
            m_sets.forEachMember(last, [&](ValueFlowObjectID object) {
                forEachPrototypeAtCreation(*m_input.program, *m_input.universe, object, [&](ValueFlowObjectID prototype) {
                    aboveAtCreation.append(prototype);
                }, [](SilentAboutPrototype) { });
            });
            above = maker.unite(above, maker.of(aboveAtCreation.span()));
            last = maker.intersect(maker.intersect(above, knownObjects), ValueSets::complement(reached));
            reached = maker.unite(reached, last);
        }
        note(reached);
    }

    constexpr OperandUses usesThatHandOn { OperandUse::Retains, OperandUse::Returns };
    ValueSetID objectsThatAreNoClosures = maker.intersect(m_input.universe->callables(), ValueSets::complement(closures));
    // A built-in that links an operand above an object does nothing else to it.
    constexpr OperandUses usesThatLinkAbove { OperandUse::Inspects, OperandUse::BecomesPrototype, OperandUse::Retains };
    auto onlyLinksArgumentAbove = [&](ValueFlowObjectID callee, unsigned argument) {
        const BuiltinTable& builtins = m_input.program->builtins();
        BuiltinID builtin = m_sets.contains(m_input.universe->builtinFunctions(), callee) ? m_input.universe->builtinOf(callee) : BuiltinID();
        if (!builtin)
            return false;
        // Reflect.get hands its receiver to the accessor that it finds.
        if (builtins.canonicalID(builtin) == Builtins::ReflectGet && argument == 2)
            return true;
        if (onlyCallsOperand(builtins, builtin, BuiltinOperand::argument(argument)) || onlyListsOperand(builtins, builtin, BuiltinOperand::argument(argument)) || onlyAsksWhatStandsAbove(builtins, builtin, BuiltinOperand::argument(argument), BuiltinCallMode::Call) || onlyPassesOperandOn(builtins, builtin, BuiltinOperand::argument(argument)))
            return true;
        bool isDescribed = false;
        for (const BuiltinOverload& overload : builtins.overloadsOf(builtins.canonicalID(builtin))) {
            if (overload.mode != BuiltinCallMode::Call)
                continue;
            if (overload.isUndeclared())
                return false;
            auto parameter = std::ranges::find(overload.parameters, BuiltinOperand::argument(argument), &BuiltinParameter::operand);
            if (parameter == overload.parameters.end() || !parameter->uses.contains(OperandUse::BecomesPrototype) || !usesThatLinkAbove.containsAll(parameter->uses) || !parameter->protocols.isEmpty())
                return false;
            isDescribed = true;
        }
        return isDescribed;
    };
    bool searchesForReads = someReadHoldsProxy.load(std::memory_order_relaxed);
    forEachShare(m_input.statements.size(), [&](uint32_t firstOfShare, uint32_t endOfShare, unsigned, unsigned worker) {
        ValueSetMaker& maker = *m_makers[worker];
        auto note = [&](ValueSetID held) {
            noteFor(worker, held);
        };
        for (uint32_t index = firstOfShare; index < endOfShare; ++index) {
            const ValueFlowStatement& statement = m_input.statements[index];
            bool isOperation = statement.kind == ValueFlowStatementKind::Operation;
            std::span<const ValueFlowOperand> operands = operandsOf(statement);
            bool holdsSome = false;
            for (const ValueFlowOperand& operand : operands)
                holdsSome |= operand.node && operand.node.index() < m_input.numberOfNodes && holdsProxy.quickGet(operand.node.index());
            auto forEachProxyReadByStatement = [&](NOESCAPE const Invocable<void(ValueSetID)> auto& functor) {
                if (!isOperation)
                    return;
                forEachRecordAskedForBy(index, [&](uint32_t madeNodes) {
                    if (readsProxy.quickGet(madeNodes))
                        functor(m_graph.valueOf(m_madeNodes[madeNodes].first));
                });
            };
            bool hasReadSome = false;
            bool callsSomething = false;
            for (unsigned position = 0; searchesForReads && isOperation && !callsSomething && position < operands.size(); ++position)
                callsSomething = operandUsesOf(statement.opcode, position).contains(OperandUse::Calls);
            if (callsSomething) {
                hasReadSome = anyUseIn(usesOf(index), [&](uint32_t madeNodes) {
                    return readsProxy.quickGet(madeNodes);
                });
            }
            if (!holdsSome && !hasReadSome)
                continue;
            bool callsOnlyFunctionsOfProgram = true;
            for (unsigned position = 0; isOperation && position < operands.size(); ++position) {
                if (!operandUsesOf(statement.opcode, position).contains(OperandUse::Calls))
                    continue;
                const ValueFlowOperand& callee = operands[position];
                callsOnlyFunctionsOfProgram &= !callee.constant || !maker.meet(callee.constant, objectsThatAreNoClosures);
                callsOnlyFunctionsOfProgram &= !callee.node || !maker.meet(m_graph.valueOf(callee.node), objectsThatAreNoClosures);
            }
            // A call to something that is not a program function passes it whatever the caller itself read from an array or an object.
            bool isTestAgainstConstructor = isOperation && statement.opcode == Opcode::InstanceOf && m_statementsThatAskProxiesByTheirTraps.get(index);
            if (hasReadSome && !callsOnlyFunctionsOfProgram && !isTestAgainstConstructor)
                forEachProxyReadByStatement(note);
            for (unsigned position = 0; position < operands.size(); ++position) {
                const ValueFlowOperand& operand = operands[position];
                if (!operand.node || operand.node.index() >= m_input.numberOfNodes || !holdsProxy.quickGet(operand.node.index()))
                    continue;
                unsigned positionAskedByTraps = isTestAgainstConstructor ? 1 : isOperation && statement.opcode == Opcode::CopyDataProperties ? OperandIndex::CopyDataProperties::source : isOperation && statement.opcode == Opcode::ForInStep ? OperandIndex::ForInStep::object : 0;
                if (position == positionAskedByTraps && m_statementsThatAskProxiesByTheirTraps.get(index))
                    continue;
                if (isOperation && callsOnlyFunctionsOfProgram && usesThatHandOn.containsAll(operandUsesOf(statement.opcode, position)))
                    continue;
                if (isOperation && statement.opcode == Opcode::Call && position == 1) {
                    bool isAskedByTraps = true;
                    ValueSetID callees = maker.unite(operands[0].constant ? operands[0].constant : ValueSets::nothing, operands[0].node ? m_graph.valueOf(operands[0].node) : ValueSets::nothing);
                    m_sets.forEachMember(maker.intersect(callees, m_input.universe->callables()), [&](ValueFlowObjectID callee) {
                        BuiltinID builtin = m_sets.contains(m_input.universe->builtinFunctions(), callee) ? m_input.program->builtins().canonicalID(m_input.universe->builtinOf(callee)) : BuiltinID();
                        isAskedByTraps &= m_sets.contains(closures, callee) || builtin == Builtins::FunctionPrototypeBind || builtin == Builtins::FunctionPrototypeSymbolHasInstance || (builtin && onlyCallsOperand(m_input.program->builtins(), builtin, BuiltinOperand::receiver()));
                    });
                    if (isAskedByTraps)
                        continue;
                }
                if (isOperation && statement.opcode == Opcode::Call && position >= 2) {
                    bool isOnlyLinkedAbove = true;
                    ValueSetID callees = maker.unite(operands[0].constant ? operands[0].constant : ValueSets::nothing, operands[0].node ? m_graph.valueOf(operands[0].node) : ValueSets::nothing);
                    m_sets.forEachMember(maker.intersect(callees, m_input.universe->callables()), [&](ValueFlowObjectID callee) {
                        isOnlyLinkedAbove &= m_sets.contains(closures, callee) || onlyLinksArgumentAbove(callee, position - 2);
                    });
                    if (isOnlyLinkedAbove)
                        continue;
                }
                note(m_graph.valueOf(operand.node));
                if (auto held = heldByInstancesOfNode.find(operand.node.index()); held != heldByInstancesOfNode.end())
                    note(held->value);
            }
        }
    });
    return maker.uniteAll(foundByWorker.span());
}

void ValueFlowSolver::runStatementsMadeBy(Thread& thread)
{
    ValueFlowRoundLog& log = thread.log;
    while (!log.madeWithStatementToRun.isEmpty()) {
        uint32_t number = log.madeWithStatementToRun.takeLast();
        log.recordsOfRunsWhereMade.append(number);
        MadeNodes made = m_madeNodes[number];
        ValueFlowStatement statement = statementMadeWith(made);
        thread.context.beginWhereMade(made.statement(), made.key, number);
        m_rule(statement, thread.context);
        thread.context.end();
        ++log.numberOfRunsWhereMade;
    }
}

void ValueFlowSolver::runStatementsWokenBy(Thread& thread)
{
    ValueFlowRoundLog& log = thread.log;
    while (!log.askedForAtRest.isEmpty()) {
        MadeNodes& made = m_madeNodes[log.askedForAtRest.takeLast()];
        if (made.has(MadeNodes::StaysAtRest) || made.has(MadeNodes::IsOfThisTurn))
            continue;
        forEachPartOf(made, [&](uint32_t part) {
            if (isRecordAtRest(part))
                log.askedForAtRest.append(part);
        });
        if (!made.statement() || m_hasRunInTurn.setInOrder(made.statement().index()))
            continue;
        made.set(MadeNodes::HasStatementRunWhereMade);
        thread.context.begin(made.statement());
        m_rule(statementMadeWith(made), thread.context);
        thread.context.end();
        ++log.numberOfRunsWhereMade;
        runStatementsMadeBy(thread);
    }
}

std::optional<uint32_t> ValueFlowSolver::findOrMakeNodesWhereAskedFor(const ValueFlowNodeKey& key, uint32_t hash, ValueFlowRoundLog& log, ValueSetMaker& maker) const
{
    if (std::optional<uint32_t> number = findNumberOfMadeNodes(key, hash)) [[likely]]
        return number;
    return makeNodesWhereAskedFor(key, hash, log, maker);
}

std::optional<uint32_t> ValueFlowSolver::makeNodesWhereAskedFor(const ValueFlowNodeKey& key, uint32_t hash, ValueFlowRoundLog& log, ValueSetMaker& maker) const
{
    if (NodeMaker::mayBringOthers(m_sets, key))
        return std::nullopt;

    MadeNodes made { key, { } };
    made.setTo(MadeNodes::StaysAtRest, key.set == ValueSets::nothing && (key.kind == ValueFlowMadeNodeKind::Written || key.kind == ValueFlowMadeNodeKind::CallGroup));
    Vector<ValueSetID> setsOfParts;
    bool mayHaveParts = key.kind == ValueFlowMadeNodeKind::CallGroup || (key.kind == ValueFlowMadeNodeKind::Lookup && key.name().kind == ValueFlowNameKind::PrototypeLink);
    if (mayHaveParts && !made.has(MadeNodes::StaysAtRest)) {
        if (key.kind == ValueFlowMadeNodeKind::Lookup)
            maker.appendCoarseParts(key.set, setsOfParts);
        else
            maker.appendParts(key.set, setsOfParts);
    }
    bool hasStatement = !made.has(MadeNodes::StaysAtRest) && setsOfParts.isEmpty() && (key.kind == ValueFlowMadeNodeKind::Lookup || key.kind == ValueFlowMadeNodeKind::CallGroup || key.kind == ValueFlowMadeNodeKind::Hub);

    IndexOfMadeNodes& index = indexOfMadeNodesWith(hash);
    auto give = [](ValueFlowRoundLog::NumbersToGive& numbers, uint32_t count, uint32_t mostInBlock, NOESCAPE const Invocable<uint32_t(uint32_t)> auto& takeBlock) {
        if (numbers.next == numbers.end) {
            numbers.sizeOfLastBlock = std::min(std::max(numbers.sizeOfLastBlock * 2, count), mostInBlock);
            numbers.next = takeBlock(numbers.sizeOfLastBlock);
            numbers.end = numbers.next + numbers.sizeOfLastBlock;
        }
        return std::exchange(numbers.next, numbers.next + count);
    };
    auto takeNodes = [&](uint32_t count) {
        uint32_t first = m_nextNode.fetch_add(count, std::memory_order_relaxed);
        RELEASE_ASSERT(first + count < ValueFlowRoundLog::numberedByLog);
        return first;
    };
    made.set(MadeNodes::IsOfThisTurn);
    uint32_t number;
    {
        Locker locker { index.lock };
        if (std::optional<uint32_t> existing = findNumberOfMadeNodes(key, hash))
            return existing;
        number = give(log.recordsToGive, 1, 64, [&](uint32_t count) {
            return static_cast<uint32_t>(m_madeNodes.makeRoomToFill(count));
        });
        if (key.kind == ValueFlowMadeNodeKind::CallGroup)
            made.first = ValueFlowNodeID(give(log.nodesOfCallGroupsToGive, ValueFlowNodeKey::numberOfNodesOfCallGroup, 16 * ValueFlowNodeKey::numberOfNodesOfCallGroup, takeNodes));
        else
            made.first = ValueFlowNodeID(give(log.nodesToGive, 1, 256, takeNodes));
        if (hasStatement) {
            made.setStatement(ValueFlowStatementID(m_nextStatement.fetch_add(1, std::memory_order_relaxed)));
            made.set(MadeNodes::HasStatementRunWhereMade);
        }
        m_madeNodes[number] = made;
        index.add(tableOfMadeNodesWith(hash), hash, number, m_fewestSlotsOfIndexOfMadeNodes, m_madeNodes);
    }
    ++log.numberMadeWhereAskedFor;
    log.madeWhereAskedFor.append(number);
    if (made.statement())
        log.madeWithStatementToRun.append(number);
    if (made.has(MadeNodes::StaysAtRest))
        return number;

    Vector<uint32_t, 8> parts;
    auto join = [&](const ValueFlowNodeKey& keyOfPart) {
        std::optional<uint32_t> part = findOrMakeNodesWhereAskedFor(keyOfPart, keyOfPart.hash(), log, maker);
        parts.append(*part);
        forEachEdgeToPart(keyOfPart.kind, made.first, m_madeNodes[*part].first, [&](ValueFlowEdge toPart) {
            log.edgesOfMadeNodes.append(toPart);
        });
    };
    switch (key.kind) {
    case ValueFlowMadeNodeKind::Read:
    case ValueFlowMadeNodeKind::Written: {
        bool isRead = key.kind == ValueFlowMadeNodeKind::Read;
        const KindOfNames& kind = m_kindsOfNames[static_cast<unsigned>(key.name().kind)];
        if (isRead ? kind.writtenKindsItMayFind && mayBeComputedKey(key.name()) : kind.readKindsThatMayFindIt && isFoundUnderComputedKey(key.name()))
            join(ValueFlowNodeKey::underKind(isRead ? ValueFlowMadeNodeKind::ReadUnderKind : ValueFlowMadeNodeKind::WrittenUnderKind, key.set, key.name().kind));
        break;
    }
    case ValueFlowMadeNodeKind::Lookup:
        for (ValueSetID part : setsOfParts)
            join(ValueFlowNodeKey::named(ValueFlowMadeNodeKind::Lookup, part, key.name()));
        break;
    case ValueFlowMadeNodeKind::CallGroup:
        for (ValueSetID part : setsOfParts)
            join(ValueFlowNodeKey::callGroup(part, key.callKind(), key.markerOfCallGroup()));
        break;
    case ValueFlowMadeNodeKind::Masked:
        log.edgesOfMadeNodes.append(ValueFlowEdge { key.maskedNode(), made.first });
        break;
    case ValueFlowMadeNodeKind::Constant:
    case ValueFlowMadeNodeKind::Hub:
    case ValueFlowMadeNodeKind::Marked:
    case ValueFlowMadeNodeKind::WrittenUnderKind:
    case ValueFlowMadeNodeKind::ReadUnderKind:
    case ValueFlowMadeNodeKind::Instance:
        break;
    }
    for (uint32_t part : parts)
        log.partsOfMadeNodes.append({ number, part });
    return number;
}

void ValueFlowSolver::takeWhatRulesHaveMade()
{
    for (IndexOfMadeNodes& index : m_madeNodesByKey)
        index.freeOutgrownTables();

    constexpr size_t recordsPerShare = 256;
    struct Share {
        unsigned thread { 0 };
        size_t first { 0 };
        size_t end { 0 };
        uint32_t nodes { 0 };
        Vector<NodeToLink> readToLink;
        Vector<NodeToLink> writtenToLink;
        Vector<NodeMaker::Masked> masked;
        Vector<uint32_t> instances;
        Vector<uint32_t> readsOfWhatLetsInUnseenCode;
        size_t firstReadToLink { 0 };
        size_t firstWrittenToLink { 0 };
    };
    Vector<Share> shares;
    for (unsigned thread = 0; thread < m_threads.size(); ++thread) {
        size_t made = m_threads[thread]->log.madeWhereAskedFor.size();
        for (size_t first = 0; first < made; first += recordsPerShare)
            shares.append(Share { thread, first, std::min(made, first + recordsPerShare), 0, { }, { }, { }, { }, { }, 0, 0 });
    }
    if (shares.isEmpty())
        return;

    uint32_t statements = m_nextStatement.load(std::memory_order_relaxed) - numberOfStatements();
    m_graph.addNodes(m_nextNode.load(std::memory_order_relaxed) - m_graph.numberOfNodes());
    growAndFill(m_numberOfUsers, m_madeNodes.size(), 0u);
    growAndFill(m_madeNodesOfNode, m_graph.numberOfNodes() - m_input.numberOfNodes, noSolverIndex);
    growBy(m_madeNodesOfStatement, statements);
    growByDoubling(m_isDue, numberOfStatements());
    growAndFill(m_rememberedByStatement, numberOfStatements(), ValueSetID());
    for (const std::unique_ptr<Thread>& thread : m_threads) {
        for (auto [statement, remembered] : thread->log.rememberedByStatementsRunWhereMade)
            m_rememberedByStatement[statement.index()] = remembered;
        thread->log.rememberedByStatementsRunWhereMade.shrink(0);
    }

    forEachIndexInParallel(m_loops, shares.size(), [&](unsigned index, unsigned worker) {
        Share& share = shares[index];
        for (uint32_t number : m_threads[share.thread]->log.madeWhereAskedFor.subspan(share.first, share.end - share.first)) {
            MadeNodes& made = m_madeNodes[number];
            const ValueFlowNodeKey& key = made.key;
            share.nodes += key.numberOfNodes();
            for (unsigned offset = 0; offset < key.numberOfNodes(); ++offset)
                m_madeNodesOfNode[made.first.index() + offset - m_input.numberOfNodes] = number;
            if (made.statement())
                m_madeNodesOfStatement[made.statement().index() - m_input.statements.size()] = number;
            made.setTo(MadeNodes::IsOfThisTurn, key.kind == ValueFlowMadeNodeKind::Masked);
            if (made.has(MadeNodes::StaysAtRest)) {
                m_graph.putToRest(made.first, key.numberOfNodes());
                continue;
            }
            switch (key.kind) {
            case ValueFlowMadeNodeKind::Constant:
                m_graph.makeConstant(made.first, key.set, worker, key.unknownSource() ? ValueFlowGraph::EdgesOutOfConstant::AreListed : ValueFlowGraph::EdgesOutOfConstant::AreKeptAtTargetsAlone);
                break;
            case ValueFlowMadeNodeKind::Read:
            case ValueFlowMadeNodeKind::ReadUnderKind:
            case ValueFlowMadeNodeKind::Written:
            case ValueFlowMadeNodeKind::WrittenUnderKind:
                if (key.readsOnlyWhatLetsInUnseenCode())
                    share.readsOfWhatLetsInUnseenCode.append(number);
                listToLink(made, share.readToLink, share.writtenToLink);
                break;
            case ValueFlowMadeNodeKind::Masked:
                share.masked.append(NodeMaker::Masked { number, key.maskKind() == ValueFlowMaskKind::Plain ? ValueSetID() : familyObjectsOf(key.set, *m_makers[worker]) });
                break;
            case ValueFlowMadeNodeKind::Instance:
                if (key.isInstanceOfNode())
                    share.instances.append(number);
                break;
            case ValueFlowMadeNodeKind::Lookup:
            case ValueFlowMadeNodeKind::CallGroup:
            case ValueFlowMadeNodeKind::Hub:
            case ValueFlowMadeNodeKind::Marked:
                break;
            }
        }
    });

    size_t readToLink = m_readToLink.size();
    size_t writtenToLink = m_writtenToLink.size();
    for (Share& share : shares) {
        m_numberOfNodesMade += share.nodes;
        share.firstReadToLink = readToLink;
        share.firstWrittenToLink = writtenToLink;
        readToLink += share.readToLink.size();
        writtenToLink += share.writtenToLink.size();
    }
    m_readToLink.makeRoomToFill(readToLink - m_readToLink.size());
    m_writtenToLink.makeRoomToFill(writtenToLink - m_writtenToLink.size());

    Vector<size_t> firstPartOfThread;
    Vector<size_t> firstEdgeOfThread;
    size_t parts = m_partsOfMadeNodes.size();
    size_t edges = m_edgesOfRound.size();
    for (const std::unique_ptr<Thread>& thread : m_threads) {
        firstPartOfThread.append(parts);
        firstEdgeOfThread.append(edges);
        parts += thread->log.partsOfMadeNodes.size();
        edges += thread->log.edgesOfMadeNodes.size();
    }
    growBy(m_partsOfMadeNodes, parts - m_partsOfMadeNodes.size());
    growBy(m_edgesOfRound, edges - m_edgesOfRound.size());
    forEachIndexInParallel(m_loops, shares.size() + m_threads.size(), [&](unsigned task) {
        if (task < shares.size()) {
            const Share& share = shares[task];
            std::ranges::copy(share.readToLink, m_readToLink.mutableSpan().subspan(share.firstReadToLink).begin());
            std::ranges::copy(share.writtenToLink, m_writtenToLink.mutableSpan().subspan(share.firstWrittenToLink).begin());
            return;
        }
        unsigned number = task - shares.size();
        ValueFlowRoundLog& log = m_threads[number]->log;
        size_t position = firstPartOfThread[number];
        uint32_t whole = noSolverIndex;
        for (ValueFlowRoundLog::PartOfMadeNodes part : log.partsOfMadeNodes) {
            if (std::exchange(whole, part.whole) != part.whole) {
                if (position != firstPartOfThread[number])
                    m_partsOfMadeNodes[position - 1] |= isLastPart;
                m_madeNodes[whole].setFirstPart(position);
            }
            m_partsOfMadeNodes[position++] = part.part;
        }
        if (position != firstPartOfThread[number])
            m_partsOfMadeNodes[position - 1] |= isLastPart;
        std::ranges::copy(log.edgesOfMadeNodes, m_edgesOfRound.mutableSpan().subspan(firstEdgeOfThread[number]).begin());
        log.madeWhereAskedFor.shrink(0);
        log.partsOfMadeNodes.shrink(0);
        log.edgesOfMadeNodes.shrink(0);
    });

    Vector<NodeMaker::Masked> masked;
    for (const Share& share : shares)
        masked.appendVector(share.masked);
    Vector<ValueFlowEdge> toStart;
    Vector<uint32_t> started;
    while (!masked.isEmpty()) {
        size_t numberThatWait = 0;
        constexpr size_t distance = 8;
        toStart.shrink(0);
        started.shrink(0);
        for (size_t index = 0; index < masked.size(); ++index) {
            if (index + distance < masked.size())
                __builtin_prefetch(&m_madeNodes[masked[index + distance].madeNodes], 1);
            NodeMaker::Masked toMask = masked[index];
            MadeNodes& made = m_madeNodes[toMask.madeNodes];
            ValueFlowNodeID node = made.key.maskedNode();
            if (node.index() >= m_input.numberOfNodes && m_madeNodes[m_madeNodesOfNode[node.index() - m_input.numberOfNodes]].has(MadeNodes::IsOfThisTurn)) {
                masked[numberThatWait++] = toMask;
                continue;
            }
            m_graph.setMask(made.first, made.key.set, made.key.maskKind(), toMask.familyObjects);
            toStart.append(ValueFlowEdge { node, made.first });
            started.append(toMask.madeNodes);
        }
        m_graph.startWithValuesOf(toStart.span());
        for (uint32_t number : started)
            m_madeNodes[number].clear(MadeNodes::IsOfThisTurn);
        RELEASE_ASSERT(numberThatWait < masked.size());
        masked.shrink(numberThatWait);
    }
    for (const Share& share : shares) {
        for (uint32_t number : share.readsOfWhatLetsInUnseenCode)
            m_graph.setMask(m_madeNodes[number].first, m_input.universe->builtinTables().heap().objectsThatLetInUnseenCode());
        for (uint32_t number : share.instances) {
            const MadeNodes& made = m_madeNodes[number];
            ValueFlowNodeID node = made.key.nodeOfInstance();
            if (m_graph.maskOf(node) != ValueSets::anything)
                m_graph.setMask(made.first, m_graph.maskOf(node), m_graph.maskKindOf(node), m_graph.familyObjectsOf(node));
        }
    }
}

void ValueFlowSolver::fillNumbersThatNobodyGave()
{
    Vector<uint32_t> records;
    Vector<uint32_t> nodes;
    for (const std::unique_ptr<Thread>& thread : m_threads) {
        ValueFlowRoundLog& log = thread->log;
        for (uint32_t number = log.recordsToGive.next; number < log.recordsToGive.end; ++number)
            records.append(number);
        for (ValueFlowRoundLog::NumbersToGive* numbers : { &log.nodesToGive, &log.nodesOfCallGroupsToGive }) {
            for (uint32_t number = numbers->next; number < numbers->end; ++number)
                nodes.append(number);
            *numbers = { };
        }
        log.recordsToGive = { };
    }
    while (records.size() < nodes.size())
        records.append(m_madeNodes.makeRoomToFill(1));
    if (nodes.size() < records.size()) {
        uint32_t more = records.size() - nodes.size();
        uint32_t first = m_graph.addNodes(more).index();
        for (uint32_t offset = 0; offset < more; ++offset)
            nodes.append(first + offset);
    }
    growAndFill(m_numberOfUsers, m_madeNodes.size(), 0u);
    growAndFill(m_madeNodesOfNode, m_graph.numberOfNodes() - m_input.numberOfNodes, noSolverIndex);
    for (size_t index = 0; index < records.size(); ++index) {
        MadeNodes made { ValueFlowNodeKey::constant(ValueSets::nothing), ValueFlowNodeID(nodes[index]) };
        made.set(MadeNodes::StaysAtRest);
        m_madeNodes[records[index]] = made;
        m_madeNodesOfNode[nodes[index] - m_input.numberOfNodes] = records[index];
        m_graph.putToRest(made.first, 1);
    }
}

ValueFlowNodeID ValueFlowSolver::makeNodes(const ValueFlowNodeKey& key)
{
    KeyToMake toMake { key, key.set ? m_sets.hashOfMembers(key.set) : 0 };
    makeNodesOf(singleElementSpan(toMake));
    return m_madeNodes[toMake.madeNodes].first;
}

ValueFlowNodeID ValueFlowSolver::ensureNodes(const ValueFlowNodeKey& key)
{
    std::optional<ValueFlowNodeID> existing = findMadeNode(key);
    return existing ? *existing : makeNodes(key);
}

void ValueFlowSolver::numberMadeNodes()
{
    struct Candidate {
        ValueFlowNodeKey key;
        uint64_t hashOfMembers;
        uint32_t numberOfMembers;
        uint32_t thread;
        uint32_t position;
    };

    Vector<Vector<uint32_t>> waiting(m_threads.size());
    for (unsigned number = 0; number < m_threads.size(); ++number) {
        Thread& thread = *m_threads[number];
        thread.numberOfMadeNode.insertFill(thread.numberOfMadeNode.size(), noSolverIndex, thread.log.numberOfMadeNodes - thread.numberOfMadeNode.size());
        for (size_t position = thread.numberOfMadeTaken; position < thread.log.made.size(); ++position)
            waiting[number].append(position);
        thread.numberOfMadeTaken = thread.log.made.size();
    }
    m_stepTimes.didFinish("rules: made: lists of what waits"_s);

    constexpr size_t candidatesPerShare = 4096;
    Vector<Candidate> candidates;
    Vector<Vector<Candidate>> candidatesOfThread(m_threads.size());
    Vector<Vector<KeyToMake>> keysToMakeOfShare;
    Vector<size_t> firstKeyToMakeOfShare;
    Vector<KeyToMake> keysToMake;
    Vector<uint32_t> madeNodesOfCandidate;
    for (bool isFirstPass = true;; isFirstPass = false) {
        auto gatherCandidatesOf = [&](unsigned thread) {
            candidatesOfThread[thread].shrink(0);
            waiting[thread].removeAllMatching([&](uint32_t position) {
                ValueFlowNodeKey key = m_threads[thread]->log.made[position].key;
                if (key.kind == ValueFlowMadeNodeKind::Masked) {
                    ValueFlowNodeID node = m_threads[thread]->numberInGraph(key.maskedNode());
                    if (!node)
                        return false;
                    key.payload = node.index();
                }
                candidatesOfThread[thread].append(Candidate { key, key.set ? m_sets.hashOfMembers(key.set) : 0, key.set ? m_sets.size(key.set) : 0, thread, position });
                return true;
            });
        };
        size_t numberWaiting = 0;
        for (const Vector<uint32_t>& ofThread : waiting)
            numberWaiting += ofThread.size();
        if (numberWaiting < candidatesPerShare) {
            for (unsigned thread = 0; thread < m_threads.size(); ++thread)
                gatherCandidatesOf(thread);
        } else
            forEachIndexInParallel(m_loops, m_threads.size(), gatherCandidatesOf);
        m_stepTimes.didFinish("rules: made: candidates gathered"_s);
        candidates.shrink(0);
        for (const Vector<Candidate>& ofThread : candidatesOfThread)
            candidates.appendVector(ofThread);
        m_stepTimes.didFinish("rules: made: candidates in one list (one thread)"_s);
        if (candidates.isEmpty())
            break;

        // The smaller sets of a name first: of sets that each hold the one before, as the sources of a chain of copies, each is then made of the one before and what it adds.
        auto orderOf = [](const Candidate& candidate) {
            return std::tuple { candidate.key.kind, candidate.key.detail, candidate.key.payload, candidate.numberOfMembers, candidate.hashOfMembers, candidate.key.set.index() };
        };
        parallelSort(m_loops, candidates.mutableSpan(), [&](const Candidate& first, const Candidate& second) {
            return orderOf(first) < orderOf(second);
        });
        m_stepTimes.didFinish("rules: made: candidates sorted"_s);
        constexpr uint32_t isToMake = 1u << 31;
        unsigned numberOfShares = (candidates.size() + candidatesPerShare - 1) / candidatesPerShare;
        keysToMakeOfShare.resize(numberOfShares);
        madeNodesOfCandidate.resize(candidates.size());
        forEachIndexInParallel(m_loops, numberOfShares, [&](unsigned share) {
            keysToMakeOfShare[share].shrink(0);
            for (size_t index = share * candidatesPerShare; index < std::min(candidates.size(), (share + 1) * candidatesPerShare); ++index) {
                const Candidate& candidate = candidates[index];
                if (index && candidates[index - 1].key == candidate.key)
                    continue;
                ASSERT(!isFirstPass || !findNumberOfMadeNodes(candidate.key));
                if (std::optional<uint32_t> existing = isFirstPass ? std::nullopt : findNumberOfMadeNodes(candidate.key)) {
                    madeNodesOfCandidate[index] = *existing;
                    continue;
                }
                madeNodesOfCandidate[index] = isToMake | keysToMakeOfShare[share].size();
                keysToMakeOfShare[share].append(KeyToMake { candidate.key, candidate.hashOfMembers });
            }
        });
        m_stepTimes.didFinish("rules: made: candidates looked up"_s);
        keysToMake.shrink(0);
        firstKeyToMakeOfShare.shrink(0);
        for (const Vector<KeyToMake>& ofShare : keysToMakeOfShare) {
            firstKeyToMakeOfShare.append(keysToMake.size());
            keysToMake.appendVector(ofShare);
        }
        m_stepTimes.didFinish("rules: made: keys in one list (one thread)"_s);
        makeNodesOf(keysToMake.mutableSpan());
        forEachIndexInParallel(m_loops, numberOfShares, [&](unsigned share) {
            ValueFlowNodeID first;
            for (size_t index = share * candidatesPerShare; index < std::min(candidates.size(), (share + 1) * candidatesPerShare); ++index) {
                const Candidate& candidate = candidates[index];
                if (!first || candidates[index - 1].key != candidate.key) {
                    size_t firstWithKey = index;
                    while (firstWithKey && candidates[firstWithKey - 1].key == candidate.key)
                        --firstWithKey;
                    uint32_t madeNodes = madeNodesOfCandidate[firstWithKey];
                    if (madeNodes & isToMake)
                        madeNodes = keysToMake[firstKeyToMakeOfShare[firstWithKey / candidatesPerShare] + (madeNodes & ~isToMake)].madeNodes;
                    first = m_madeNodes[madeNodes].first;
                }
                Thread& thread = *m_threads[candidate.thread];
                uint32_t firstInLog = thread.log.made[candidate.position].first;
                for (unsigned offset = 0; offset < candidate.key.numberOfNodes(); ++offset)
                    thread.numberOfMadeNode[firstInLog + offset] = first.index() + offset;
            }
        });
        m_stepTimes.didFinish("rules: made: numbers written back"_s);
    }
    for (const Vector<uint32_t>& left : waiting)
        RELEASE_ASSERT(left.isEmpty());
}

// One maker makes all receivers of a name that have more objects than are looked up one by one, and it alone touches the cells of that name.
uint64_t ValueFlowSolver::mostObjectsListedByMember() const
{
    return std::max<uint64_t>(m_input.mostObjectsListedByMember, mostObjectsLookedUpOneByOne);
}

bool ValueFlowSolver::isListedByMember(ValueSetID set) const
{
    return !ValueSets::isComplement(set) && m_sets.size(set) <= mostObjectsListedByMember();
}

static bool hasFewObjects(const ValueSets& sets, ValueSetID set)
{
    return !ValueSets::isComplement(set) && sets.size(set) <= mostObjectsOfFew;
}

AOT_THRESHOLD(size_t, mostNodesComparedAtOnce, 1024);

// Answers how many of the others are at rest.
size_t ValueFlowSolver::connectWhereSetsMeet(const NodeOfSet& node, bool isRead, std::span<const NodeOfSet> others, Linked& linked)
{
    size_t numberAtRest = 0;
    m_sets.forEachSetMeeting(node.set, others.size(), [&](size_t index) {
        if (m_graph.isAtRest(others[index].node)) {
            ++numberAtRest;
            return ValueSetID();
        }
        ++linked.testsWhetherSetsMeet;
        return others[index].set;
    }, [&](size_t index) {
        linked.edges.append(isRead ? ValueFlowEdge { others[index].node, node.node } : ValueFlowEdge { node.node, others[index].node });
    });
    return numberAtRest;
}

// A node that stands for a kind of names is compared with every node under the kinds it may meet.
void ValueFlowSolver::connectAcrossLongLists(std::span<const Linked::LongList> lists)
{
    struct Piece {
        const Linked::LongList* list { nullptr };
        size_t first { 0 };
        size_t numberAtRest { 0 };
    };
    Vector<Piece> pieces;
    for (const Linked::LongList& list : lists) {
        for (size_t first = 0; first < list.others->size(); first += mostNodesComparedAtOnce)
            pieces.append(Piece { &list, first, 0 });
    }
    if (pieces.isEmpty())
        return;
    Vector<Linked> linkedOfWorker(m_threads.size());
    forEachIndexInParallel(m_loops, pieces.size(), [&](unsigned index, unsigned worker) {
        Piece& piece = pieces[index];
        std::span<const NodeOfSet> others = piece.list->others->span().subspan(piece.first);
        piece.numberAtRest = connectWhereSetsMeet(piece.list->node, piece.list->isRead, others.first(std::min(others.size(), mostNodesComparedAtOnce)), linkedOfWorker[worker]);
    });
    for (const Linked& linked : linkedOfWorker) {
        m_edgesOfRound.appendVector(linked.edges);
        m_counters.testsWhetherSetsMeet += linked.testsWhetherSetsMeet;
    }
    Vector<ShortList<NodeOfSet>*> listsMostlyAtRest;
    size_t numberAtRest = 0;
    for (const Piece& piece : pieces) {
        numberAtRest = piece.numberAtRest + (piece.first ? numberAtRest : 0);
        if (piece.first + mostNodesComparedAtOnce >= piece.list->others->size() && numberAtRest * 2 > piece.list->others->size())
            listsMostlyAtRest.append(piece.list->others);
    }
    AOT::sort(listsMostlyAtRest);
    removeRepeatedElements(listsMostlyAtRest);
    for (ShortList<NodeOfSet>* list : listsMostlyAtRest)
        removeNodesAtRestFrom(*list);
}

void ValueFlowSolver::link(const NodeOfSet& node, bool isRead, NodesOfName& others, uint32_t numberOfName, Linked& linked)
{
    auto connect = [&](ValueFlowNodeID other) {
        if (!m_graph.isAtRest(other))
            linked.edges.append(isRead ? ValueFlowEdge { other, node.node } : ValueFlowEdge { node.node, other });
    };
    auto connectWhereSetsMeet = [&](ShortList<NodeOfSet>& list) {
        if (list.size() > mostNodesComparedAtOnce) {
            linked.longLists.append({ node, isRead, &list });
            return;
        }
        if (this->connectWhereSetsMeet(node, isRead, list.span(), linked) * 2 > list.size())
            linked.listsMostlyAtRest.append(&list);
    };

    auto connectByMembers = [&](bool hasFew) {
        uint32_t byMember = hasFew ? others.ofFewObjectsByMember : others.ofManyObjectsByMember;
        const NodesOfMembers& nodesOfMembers = m_linkShards[linkShardOf(numberOfName)].nodesOfMembers[!isRead];
        m_sets.forEachMemberInLine(node.set, [&](ValueFlowObjectID member) {
            const uint32_t* list = nodesOfMembers.listOfMember.find(byMember, member.index());
            if (!list)
                return;
            nodesOfMembers.lists.forEach(*list, [&](uint32_t other) {
                connect(ValueFlowNodeID(other));
            });
        });
    };

    connectWhereSetsMeet(others.ofVeryManyObjects);
    if (hasFewObjects(m_sets, node.set) && !others.ofManyObjects.isEmpty())
        connectByMembers(false);
    else
        connectWhereSetsMeet(others.ofManyObjects);
    if (ValueSets::isComplement(node.set) || m_sets.size(node.set) > std::max<size_t>(mostObjectsLookedUpOneByOne, others.ofFewObjects.size()))
        connectWhereSetsMeet(others.ofFewObjects);
    else if (!others.ofFewObjects.isEmpty())
        connectByMembers(true);
}

void ValueFlowSolver::removeNodesAtRestFrom(ShortList<NodeOfSet>& list)
{
    list.removeAllMatching([&](const NodeOfSet& other) {
        if (!m_graph.isAtRest(other.node))
            return false;
        m_madeNodes[m_madeNodesOfNode[other.node.index() - m_input.numberOfNodes]].clear(MadeNodes::IsListedUnderName);
        return true;
    });
}

void ValueFlowSolver::numberNamesWatched()
{
    for (const std::unique_ptr<Thread>& thread : m_threads) {
        for (const Thread::NameWatched& watched : thread->namesWatched) {
            uint32_t number = m_numberOfName.ensure(static_cast<uint64_t>(watched.name.kind) << 32 | watched.name.payload, [&] {
                makeRoomByDoubling(m_names);
                m_names.append(Name { watched.name, { }, { } });
                return static_cast<uint32_t>(m_names.size() - 1);
            }).iterator->value;
            m_linkShards[linkShardOf(number)].namesWatchedInRound.append({ number, watched.within, watched.statement });
        }
        thread->namesWatched.shrink(0);
    }
}

void ValueFlowSolver::noteNamesWatched(unsigned numberOfShard, ValueSetMaker& maker)
{
    LinkShard& shard = m_linkShards[numberOfShard];
    uint32_t round = m_counters.rounds;
    for (const LinkShard::NameWatched& watched : shard.namesWatchedInRound) {
        Name& name = m_names[watched.numberOfName];
        if (!name.holders || name.roundOfFirstQuestion == round)
            listIfItMayBecomeDue(watched.statement, shard.woken);
        auto added = shard.partWatchedOf.add(static_cast<uint64_t>(watched.numberOfName) << 32 | watched.statement.index(), shard.partsWatched.size());
        if (!added.isNewEntry) {
            PartOfNameWatched& part = shard.partsWatched[added.iterator->value];
            part.within = part.round == round ? maker.unite(part.within, watched.within) : watched.within;
            part.round = round;
            continue;
        }
        if (!name.holders) {
            name.roundOfFirstQuestion = round;
            Vector<ValueSetID, 16> written { m_input.universe->placeholdersAlone() };
            if (name.name.kind == ValueFlowNameKind::Property)
                written.append(m_input.universe->builtinTables().heap().objectsDeclaringField(PropertyKeyID(name.name.payload)));
            for (const ShortList<NodeOfSet>* list : { &name.written.ofFewObjects, &name.written.ofManyObjects, &name.written.ofVeryManyObjects }) {
                for (const NodeOfSet& node : *list)
                    written.append(node.set);
            }
            name.holders = maker.uniteAll(written.span());
        }
        shard.partsWatched.append(PartOfNameWatched { watched.statement, watched.within, round, std::exchange(name.firstPartWatched, static_cast<uint32_t>(shard.partsWatched.size())) });
    }
    shard.namesWatchedInRound.shrink(0);
}

void ValueFlowSolver::wakeWatchersOfNamesWrittenOnMoreObjects(unsigned numberOfShard, ValueSetMaker& maker)
{
    LinkShard& shard = m_linkShards[numberOfShard];
    Vector<std::pair<uint32_t, ValueSetID>>& written = shard.writtenUnderWatchedNames;
    if (written.isEmpty())
        return;
    AOT::sortInLine(written, { }, [](const std::pair<uint32_t, ValueSetID>& entry) {
        return std::pair(entry.first, entry.second.index());
    });
    Vector<ValueSetID, 16> setsOfName;
    for (size_t first = 0; first < written.size();) {
        Name& name = m_names[written[first].first];
        setsOfName.shrink(0);
        size_t end = first;
        for (; end < written.size() && written[end].first == written[first].first; ++end)
            setsOfName.append(written[end].second);
        first = end;
        ValueSetID whatIsNew = maker.intersect(maker.uniteAll(setsOfName.span()), ValueSets::complement(name.holders));
        if (whatIsNew == ValueSets::nothing)
            continue;
        name.holders = maker.unite(name.holders, whatIsNew);
        for (uint32_t part = name.firstPartWatched; part != noSolverIndex; part = shard.partsWatched[part].next) {
            if (m_sets.meet(whatIsNew, shard.partsWatched[part].within))
                listIfItMayBecomeDue(shard.partsWatched[part].statement, shard.woken);
        }
    }
    written.shrink(0);
}

void ValueFlowSolver::insertNode(const NodeToLink& toLink, bool isRead)
{
    const NodeOfSet& node = toLink.node;
    MadeNodes& madeNodes = m_madeNodes[toLink.madeNodes];
    if (madeNodes.set(MadeNodes::IsListedUnderName))
        return;
    if (!isRead && !toLink.standsForKind && m_names[toLink.numberOfName].holders)
        m_linkShards[linkShardOf(toLink.numberOfName)].writtenUnderWatchedNames.append({ toLink.numberOfName, node.set });
    unsigned kind = static_cast<unsigned>(toLink.name.kind);
    NodesOfName& nodes = toLink.standsForKind ? (isRead ? m_kindsOfNames[kind].read : m_kindsOfNames[kind].written) : isRead ? m_names[toLink.numberOfName].read : m_names[toLink.numberOfName].written;
    if (toLink.size == SizeOfSetToLink::VeryMany) {
        nodes.ofVeryManyObjects.append(node);
        return;
    }
    bool hasFew = toLink.size == SizeOfSetToLink::Few;
    (hasFew ? nodes.ofFewObjects : nodes.ofManyObjects).append(node);
    if (madeNodes.set(MadeNodes::IsListedUnderMembers))
        return;
    uint32_t& byMember = hasFew ? nodes.ofFewObjectsByMember : nodes.ofManyObjectsByMember;
    NodesOfMembers& nodesOfMembers = m_linkShards[linkShardOf(toLink.numberOfName)].nodesOfMembers[isRead];
    auto add = [&](ValueFlowObjectID member) {
        nodesOfMembers.lists.add(nodesOfMembers.listOfMember.ensure(byMember, member.index()), node.node.index());
    };
    if (toLink.onlyMember)
        add(toLink.onlyMember);
    else
        m_sets.forEachMemberInLine(node.set, add);
}

void ValueFlowSolver::linkNamedNodes(unsigned numberOfShard, bool isRead, ValueSetMaker& maker)
{
    Linked& linked = m_linkShards[numberOfShard].linked;
    for (const NodeToLink& toLink : isRead ? m_readToLink : m_writtenToLink) {
        if (linkShardOf(toLink.numberOfName) != numberOfShard || m_graph.isAtRest(toLink.node.node))
            continue;
        linkNode(toLink, isRead, linked);
        for (ShortList<NodeOfSet>* list : linked.listsMostlyAtRest)
            removeNodesAtRestFrom(*list);
        linked.listsMostlyAtRest.shrink(0);
        insertNode(toLink, isRead);
    }
    if (!isRead)
        wakeWatchersOfNamesWrittenOnMoreObjects(numberOfShard, maker);
}

void ValueFlowSolver::linkNode(const NodeToLink& toLink, bool isRead, Linked& linked)
{
    if (!toLink.standsForKind) {
        Name& name = m_names[toLink.numberOfName];
        link(toLink.node, isRead, isRead ? name.written : name.read, toLink.numberOfName, linked);
        return;
    }
    unsigned kind = static_cast<unsigned>(toLink.name.kind);
    for (uint64_t others = isRead ? m_kindsOfNames[kind].writtenKindsItMayFind : m_kindsOfNames[kind].readKindsThatMayFindIt; others; others &= others - 1) {
        unsigned other = std::countr_zero(others);
        link(toLink.node, isRead, isRead ? m_kindsOfNames[other].written : m_kindsOfNames[other].read, numberOfFirstKindAmongNames + other, linked);
    }
}

void ValueFlowSolver::linkInChunks(bool isRead, unsigned numberOfOtherTasks, NOESCAPE const ScopedLambda<void(unsigned task, unsigned worker)>& otherTask)
{
    constexpr size_t nodesPerChunk = 16;
    const ListWithRoomToFill<NodeToLink>& toLink = isRead ? m_readToLink : m_writtenToLink;
    size_t numberOfChunks = (toLink.size() + nodesPerChunk - 1) / nodesPerChunk;
    Vector<uint32_t> dearNodes;
    for (size_t index = 0; index < toLink.size(); ++index) {
        if (toLink[index].isDearToLink)
            dearNodes.append(index);
    }
    Vector<Linked> linkedOfWorker(m_threads.size());
    forEachIndexInParallel(m_loops, dearNodes.size() + numberOfOtherTasks + numberOfChunks, [&](unsigned task, unsigned worker) {
        if (task < dearNodes.size()) {
            if (!m_graph.isAtRest(toLink[dearNodes[task]].node.node))
                linkNode(toLink[dearNodes[task]], isRead, linkedOfWorker[worker]);
            return;
        }
        if (task < dearNodes.size() + numberOfOtherTasks) {
            otherTask(task - dearNodes.size(), worker);
            return;
        }
        size_t chunk = task - numberOfOtherTasks - dearNodes.size();
        for (size_t index = chunk * nodesPerChunk; index < std::min(toLink.size(), (chunk + 1) * nodesPerChunk); ++index) {
            if (!toLink[index].isDearToLink && !m_graph.isAtRest(toLink[index].node.node))
                linkNode(toLink[index], isRead, linkedOfWorker[worker]);
        }
    });

    Vector<Linked::LongList> longLists;
    for (const Linked& linked : linkedOfWorker)
        longLists.appendVector(linked.longLists);
    connectAcrossLongLists(longLists.span());
    Vector<ShortList<NodeOfSet>*> listsMostlyAtRest;
    for (const Linked& linked : linkedOfWorker) {
        m_edgesOfRound.appendVector(linked.edges);
        m_counters.testsWhetherSetsMeet += linked.testsWhetherSetsMeet;
        listsMostlyAtRest.appendVector(linked.listsMostlyAtRest);
    }
    AOT::sort(listsMostlyAtRest);
    removeRepeatedElements(listsMostlyAtRest);
    for (ShortList<NodeOfSet>* list : listsMostlyAtRest)
        removeNodesAtRestFrom(*list);

    for (size_t index = 0; index < toLink.size(); ++index) {
        if (!m_graph.isAtRest(toLink[index].node.node))
            m_nodesToInsertOfShards[linkShardOf(toLink[index].numberOfName)].append(index);
    }
    if (!isRead) {
        insertNodesOfShards(m_writtenToLink, false);
        return;
    }
    std::swap(m_readToLink, m_readToInsert);
    m_readToLink.shrink(0);
    m_insertionOfNodesThatRead = BackgroundTask::start(m_loops, TaskPriority::Foreground, { }, [this] {
        insertNodesOfShards(m_readToInsert, true);
    });
}

void ValueFlowSolver::insertNodesOfShards(const ListWithRoomToFill<NodeToLink>& toInsert, bool isRead)
{
    std::array<uint32_t, numberOfLinkShards> shardsInOrder;
    for (unsigned position = 0; position < numberOfLinkShards; ++position)
        shardsInOrder[position] = numberOfLinkShards - 1 - position;
    AOT::stableSortInLine(std::span(shardsInOrder).subspan(numberOfLinkShardsOfKinds), std::ranges::greater(), [&](uint32_t shard) {
        return m_nodesToInsertOfShards[shard].size();
    });
    forEachIndexInParallel(m_loops, numberOfLinkShards, [&](unsigned task, unsigned worker) {
        Vector<uint32_t>& nodesOfShard = m_nodesToInsertOfShards[shardsInOrder[task]];
        constexpr size_t distance = 8;
        for (size_t position = 0; position < nodesOfShard.size(); ++position) {
            if (position + distance < nodesOfShard.size())
                __builtin_prefetch(&m_madeNodes[toInsert[nodesOfShard[position + distance]].madeNodes], 1);
            insertNode(toInsert[nodesOfShard[position]], isRead);
        }
        nodesOfShard.shrink(0);
        if (!isRead)
            wakeWatchersOfNamesWrittenOnMoreObjects(shardsInOrder[task], *m_makers[worker]);
    });
}

void ValueFlowSolver::noteReaders(unsigned numberOfShard, ValueSetMaker& maker)
{
    ReaderShard& shard = m_readerShards[numberOfShard];
    constexpr size_t distance = 12;
    for (const std::unique_ptr<Thread>& thread : m_threads) {
        Vector<NodeRead>& read = thread->nodesReadOfShard[numberOfShard];
        for (size_t index = 0; index < read.size(); ++index) {
            if (index + 3 * distance < read.size())
                __builtin_prefetch(&m_readersOfNode[read[index + 3 * distance].node.index()], 1);
            if (index + 2 * distance < read.size())
                shard.readersOfNodes.prefetch(m_readersOfNode[read[index + 2 * distance].node.index()]);
            if (index + distance < read.size()) {
                const NodeRead& ahead = read[index + distance];
                shard.readersOfNodes.prefetchPlaceOf(m_readersOfNode[ahead.node.index()], ahead.statement.index());
            }
            auto [node, statement] = read[index];
            uint32_t& readers = m_readersOfNode[node.index()];
            bool hadReaders = readers;
            if (!shard.readersOfNodes.add(readers, statement.index()) || hadReaders)
                continue;
            m_graph.setSeenByReadersOf(node, valueForReadersOf(node, maker));
            noteListener(node, ReadsNode);
        }
        read.shrink(0);
    }

    for (const std::unique_ptr<Thread>& thread : m_threads) {
        for (auto [node, statement] : thread->nodesAskedWhetherEmptyOfShard[numberOfShard]) {
            if (!shard.knownToWaitForSomething.add(node.index(), statement.index()))
                continue;
            uint32_t& first = shard.firstReaderWaitingForSomething.add(node.index(), noSolverIndex).iterator->value;
            shard.readersWaitingForSomething.append(Reader { statement, first });
            first = shard.readersWaitingForSomething.size() - 1;
            noteListener(node, WaitsForNodeToHoldSomething);
        }
        thread->nodesAskedWhetherEmptyOfShard[numberOfShard].shrink(0);
    }
}

void ValueFlowSolver::chainSetsWatchedForCover()
{
    AOT_THRESHOLD(size_t, fewestMembersForThreads, 4096);
    size_t numberOfMembers = 0;
    size_t endOfMembers = 0;
    std::span<WatchShard> shards = std::span(m_watchShards).subspan(numberOfWatchShardsByNode);
    for (WatchShard& shard : shards) {
        for (const Vector<MemberOfSetWatchedForCover>& members : shard.membersToChain)
            numberOfMembers += members.size();
        endOfMembers = std::max<size_t>(endOfMembers, std::exchange(shard.endOfMembersToChain, 0));
    }
    if (!numberOfMembers)
        return;
    Vector<uint32_t>& firstOfMember = m_firstSetWatchedForCoverOfMember;
    if (endOfMembers > firstOfMember.size())
        firstOfMember.insertFill(firstOfMember.size(), noSolverIndex, endOfMembers - firstOfMember.size());
    auto chain = [&](unsigned range) {
        Vector<ChainedWatch>& chained = m_setsWatchedForCoverOfMembers[range];
        for (WatchShard& shard : shards) {
            for (MemberOfSetWatchedForCover member : shard.membersToChain[range])
                chained.append(ChainedWatch { member.set, std::exchange(firstOfMember[member.member], static_cast<uint32_t>(chained.size())) });
            shard.membersToChain[range].shrink(0);
        }
    };
    if (numberOfMembers < fewestMembersForThreads) {
        for (unsigned range = 0; range < numberOfRangesOfMembers; ++range)
            chain(range);
    } else
        forEachIndexInParallel(m_loops, numberOfRangesOfMembers, chain);
}

size_t ValueFlowSolver::numberOfReaders() const
{
    size_t number = 0;
    for (const WatchShard& shard : m_watchShards)
        number += shard.readersOfWatches.numberOfNumbers();
    for (const ReaderShard& shard : m_readerShards)
        number += shard.readersOfNodes.numberOfNumbers() + shard.readersWaitingForSomething.size();
    return number;
}

void ValueFlowSolver::chainByMember(WatchShard& shard, uint32_t watchedNode, uint32_t number)
{
    ValueSetID within = shard.watches[number].within;
    if (shard.watchedNodes[watchedNode].holdsWhatPlaceholderCovers) {
        auto addedSet = shard.numberOfSetWatchedForCover.add(within.index(), shard.watchesOfSetWatchedForCover.size());
        uint32_t numberOfSet = addedSet.iterator->value;
        if (addedSet.isNewEntry) {
            shard.watchesOfSetWatchedForCover.append(Vector<uint32_t, 2>());
            uint32_t nameOfSet = numberOfSet * numberOfWatchShardsBySet + static_cast<uint32_t>(&shard - m_watchShards.data() - numberOfWatchShardsByNode);
            m_sets.forEachMemberInLine(within, [&](ValueFlowObjectID member) {
                shard.membersToChain[rangeOfMember(member.index())].append({ member.index(), nameOfSet });
                shard.endOfMembersToChain = std::max(shard.endOfMembersToChain, member.index() + 1);
            });
        }
        shard.watchesOfSetWatchedForCover[numberOfSet].append(number);
        return;
    }
    m_sets.forEachMemberInLine(within, [&](ValueFlowObjectID member) {
        uint32_t& first = shard.firstWatchOfMember.add(static_cast<uint64_t>(watchedNode) << 32 | member.index(), noSolverIndex).number;
        shard.watchesOfMembers.append(ChainedWatch { number, first });
        first = shard.watchesOfMembers.size() - 1;
    });
}

void ValueFlowSolver::watch(WatchShard& shard, ValueFlowNodeID node, ValueSetID within, ValueFlowStatementID statement, bool isAskedLast)
{
    auto added = shard.watchOfPart.add(static_cast<uint64_t>(node.index()) << 32 | within.index(), shard.watches.size());
    uint32_t number = added.number;
    if (added.isNew) {
        shard.watches.append(Watch { within, 0, 0, 0 });
        uint32_t watchedNode = shard.watchedNodeOfNode.ensure(node.index(), [&] {
            bool holdsWhatPlaceholderCovers = this->holdsWhatPlaceholderCovers(node);
            shard.watchedNodes.append(WatchedNode { node, m_graph.valueOf(node), { }, { }, holdsWhatPlaceholderCovers });
            if (!holdsWhatPlaceholderCovers) {
                m_isWatched.quickSet(node.index());
                noteListener(node, WatchesNode);
            }
            return static_cast<uint32_t>(shard.watchedNodes.size() - 1);
        }).iterator->value;
        shard.watches[number].watchedNode = watchedNode;
        shard.watchedNodes[watchedNode].watches.append(number);
        AOT_THRESHOLD(size_t, mostWatchesWithoutChains, 8);
        WatchedNode& watched = shard.watchedNodes[watchedNode];
        AOT_THRESHOLD(uint32_t, mostObjectsChainedWhereWatchedForCover, 64);
        if (!isListedByMember(within) || (watched.holdsWhatPlaceholderCovers && m_sets.size(within) > mostObjectsChainedWhereWatchedForCover) || (!watched.holdsWhatPlaceholderCovers && !watched.hasChainsByMember && watched.watches.size() <= mostWatchesWithoutChains))
            watched.watchesOfManyObjects.append(number);
        else {
            if (!std::exchange(watched.hasChainsByMember, true)) {
                watched.watchesOfManyObjects.removeAllMatching([&](uint32_t earlier) {
                    if (!isListedByMember(shard.watches[earlier].within) || (watched.holdsWhatPlaceholderCovers && m_sets.size(shard.watches[earlier].within) > mostObjectsChainedWhereWatchedForCover))
                        return false;
                    chainByMember(shard, watchedNode, earlier);
                    return true;
                });
            }
            chainByMember(shard, watchedNode, number);
        }
    }
    Watch& watch = shard.watches[number];
    if (!shard.readersOfWatches.contains(isAskedLast ? watch.readers : watch.readersAskingLast, statement.index()))
        shard.readersOfWatches.add(isAskedLast ? watch.readersAskingLast : watch.readers, statement.index());
}

void ValueFlowSolver::listReadersOfWatch(const WatchShard& shard, uint32_t watch, Vector<ValueFlowStatementID>& readers) const
{
    shard.readersOfWatches.forEach(shard.watches[watch].readers, [&](uint32_t statement) {
        listIfItMayBecomeDue(ValueFlowStatementID(statement), readers);
    });
    shard.readersOfWatches.forEach(shard.watches[watch].readersAskingLast, [&](uint32_t statement) {
        listAsDueLast(ValueFlowStatementID(statement));
    });
}

void ValueFlowSolver::listAsDueLast(ValueFlowStatementID statement) const
{
    if (isAtRest(statement) || m_isDue.get(statement.index()) || m_isDueLast.set(statement.index()))
        return;
    Locker locker { m_lockOfDueLast };
    m_dueLast.append(statement);
}

bool ValueFlowSolver::isChainOfPrototypes(ValueFlowNodeID node) const
{
    const ValueFlowNodeKey* key = keyOfMadeNode(node);
    return key && key->kind == ValueFlowMadeNodeKind::Lookup && shapeOf(key->name().kind).sort == ValueFlowNameSort::PrototypeLink;
}

void ValueFlowSolver::listWatchersOf(ValueFlowNodeID node, ValueSetMaker& maker, Vector<ValueFlowStatementID>& readers, Vector<NewMemberOfWatchedNode>& newMembers, Vector<WatchesToTest>& watchesToTest)
{
    if (holdsWhatPlaceholderCovers(node)) {
        ValueSetID value = m_graph.valueOf(node);
        ValueSetID whatIsNew = maker.intersect(value, ValueSets::complement(std::exchange(seenByWatchersOf(node), value)));
        if (!ValueSets::isComplement(whatIsNew)) {
            m_sets.forEachMemberInLine(whatIsNew, [&](ValueFlowObjectID member) {
                if (member.index() < m_firstSetWatchedForCoverOfMember.size() && m_firstSetWatchedForCoverOfMember[member.index()] != noSolverIndex)
                    newMembers.append({ numberOfWatchShards, node.index(), member });
            });
        }
        for (unsigned numberOfShard = numberOfWatchShardsByNode; numberOfShard < numberOfWatchShards; ++numberOfShard) {
            const WatchShard& shard = m_watchShards[numberOfShard];
            auto number = shard.watchedNodeOfNode.find(node.index());
            if (number == shard.watchedNodeOfNode.end())
                continue;
            const WatchedNode& watchedNode = shard.watchedNodes[number->value];
            if (ValueSets::isComplement(whatIsNew)) {
                for (uint32_t watch : watchedNode.watches)
                    listReadersOfWatch(shard, watch, readers);
            } else if (!watchedNode.watchesOfManyObjects.isEmpty())
                watchesToTest.append({ numberOfShard, number->value, whatIsNew });
        }
        return;
    }
    unsigned numberOfShard = watchShardOf(node, { });
    WatchShard& shard = m_watchShards[numberOfShard];
    uint32_t number = shard.watchedNodeOfNode.get(node.index());
    WatchedNode& watchedNode = shard.watchedNodes[number];
    ValueSetID value = m_graph.valueOf(node);
    ValueSetID whatIsNew = maker.intersect(value, ValueSets::complement(watchedNode.seen));
    watchedNode.seen = value;
    if (ValueSets::isComplement(whatIsNew)) {
        for (uint32_t watch : watchedNode.watches)
            listReadersOfWatch(shard, watch, readers);
        return;
    }
    if (watchedNode.hasChainsByMember) {
        m_sets.forEachMemberInLine(whatIsNew, [&](ValueFlowObjectID member) {
            newMembers.append({ numberOfShard, number, member });
        });
    }
    if (!watchedNode.watchesOfManyObjects.isEmpty())
        watchesToTest.append({ numberOfShard, number, whatIsNew });
}

void ValueFlowSolver::giveBackRoomOfUses(UsesOfThread& uses, uint32_t list, uint64_t room)
{
    unsigned size = UsesOfThread::sizeOf(room);
    m_uses[list] = std::exchange(uses.firstFree[size], list);
    uses.sizesWithFreeLists[size / 64] |= 1ull << size % 64;
}

uint32_t ValueFlowSolver::takeUses(UsesOfThread& uses, uint32_t count)
{
    ASSERT(count);
    uint64_t room = UsesOfThread::roomFor(count);
    auto giveBackWords = [&](uint64_t first, uint64_t words) {
        while (words) {
            uint64_t piece = UsesOfThread::largestRoomIn(words);
            giveBackRoomOfUses(uses, static_cast<uint32_t>(first), piece);
            first += piece;
            words -= piece;
        }
    };
    // The smallest free list that is large enough. What is left of it is free for shorter lists: statements ask for more and more, so that a size once outgrown is seldom asked for again.
    unsigned size = UsesOfThread::sizeOf(room);
    std::array<uint64_t, 2> largeEnough { size < 64 ? uses.sizesWithFreeLists[0] >> size << size : 0, size < 64 ? uses.sizesWithFreeLists[1] : uses.sizesWithFreeLists[1] >> (size - 64) << (size - 64) };
    if (largeEnough[0] | largeEnough[1]) {
        unsigned found = largeEnough[0] ? std::countr_zero(largeEnough[0]) : 64 + std::countr_zero(largeEnough[1]);
        uint32_t list = uses.firstFree[found];
        uses.firstFree[found] = m_uses[list];
        if (uses.firstFree[found] == noSolverIndex)
            uses.sizesWithFreeLists[found / 64] &= ~(1ull << found % 64);
        giveBackWords(list + room, UsesOfThread::roomOf(found) - room);
        return list;
    }
    constexpr size_t usesTakenAtOnce = 4096;
    if (room > usesTakenAtOnce / 4)
        return static_cast<uint32_t>(m_uses.makeRoomToFill(room));
    if (uses.endOfFresh - uses.nextFresh < room) {
        giveBackWords(uses.nextFresh, uses.endOfFresh - uses.nextFresh);
        uses.nextFresh = m_uses.makeRoomToFill(usesTakenAtOnce);
        uses.endOfFresh = uses.nextFresh + usesTakenAtOnce;
    }
    return static_cast<uint32_t>(std::exchange(uses.nextFresh, uses.nextFresh + room));
}

void ValueFlowSolver::takeUpPartsOfNodesWithFirstUser(Vector<uint32_t>&& nodes)
{
    if (nodes.isEmpty())
        return;
    constexpr size_t nodesPerShare = 128;
    struct Share {
        Vector<uint32_t> woken;
        Vector<ValueFlowStatementID> due;
    };
    Vector<Share> shares((nodes.size() + nodesPerShare - 1) / nodesPerShare);
    forEachIndexInParallel(m_loops, shares.size(), [&](unsigned index) {
        Share& share = shares[index];
        Vector<uint32_t, 128> toTakeUp;
        toTakeUp.append(nodes.subspan(index * nodesPerShare, std::min(nodesPerShare, nodes.size() - index * nodesPerShare)));
        while (!toTakeUp.isEmpty()) {
            uint32_t number = toTakeUp.takeLast();
            MadeNodes& madeNodes = m_madeNodes[number];
            if (madeNodes.has(MadeNodes::HasBeenAtRest))
                share.woken.append(number);
            if (madeNodes.statement() && !madeNodes.clear(MadeNodes::HasStatementRunWhereMade))
                share.due.append(madeNodes.statement());
            forEachPartOf(madeNodes, [&](uint32_t part) {
                if (!WTF::atomicExchangeAdd(&m_numberOfUsers[part], 1u, std::memory_order_relaxed))
                    toTakeUp.append(part);
            });
        }
    });
    for (const Share& share : shares) {
        for (uint32_t number : share.woken) {
            const MadeNodes& madeNodes = m_madeNodes[number];
            --m_counters.madeNodesAtRest;
            m_graph.wake(madeNodes.first, madeNodes.key.numberOfNodes());
            if (NodeMaker::readsOrWrites(madeNodes.key.kind) && !madeNodes.has(MadeNodes::IsUnionOfParts))
                listToLink(madeNodes, m_readToLink, m_writtenToLink);
        }
        for (ValueFlowStatementID statement : share.due) {
            if (!m_isDue.set(statement.index()))
                m_due.append(statement);
        }
    }
}

void ValueFlowSolver::letGoOf(Vector<uint32_t>&& records)
{
    if (records.isEmpty())
        return;
    constexpr size_t recordsPerShare = 256;
    struct Share {
        Vector<ValueFlowStatementID> statements;
        uint32_t numberOfNodesPutToRest { 0 };
    };
    Vector<Share> shares((records.size() + recordsPerShare - 1) / recordsPerShare);
    forEachIndexInParallel(m_loops, shares.size(), [&](unsigned index, unsigned worker) {
        Share& share = shares[index];
        Vector<uint32_t, 64> toLetGo;
        Vector<uint32_t, 64> recordsWhosePartsAreLetGo;
        // A record keeps its parts awake from its first user to its last.
        auto letGo = [&](uint32_t number) {
            if (WTF::atomicExchangeAdd(&m_numberOfUsers[number], static_cast<uint32_t>(-1), std::memory_order_relaxed) != 1)
                return;
            ++share.numberOfNodesPutToRest;
            MadeNodes& madeNodes = m_madeNodes[number];
            madeNodes.set(MadeNodes::HasBeenAtRest);
            m_graph.putToRest(madeNodes.first, madeNodes.key.numberOfNodes());
            if (ValueFlowStatementID statement = madeNodes.statement()) {
                if (uint32_t list = std::exchange(m_usesOfStatement[statement.index()], noSolverIndex); list != noSolverIndex)
                    toLetGo.append(list);
                share.statements.append(statement);
            }
            if (madeNodes.has(MadeNodes::HasParts))
                recordsWhosePartsAreLetGo.append(number);
        };
        for (uint32_t number : records.subspan(index * recordsPerShare, std::min(recordsPerShare, records.size() - index * recordsPerShare)))
            letGo(number);
        while (!toLetGo.isEmpty() || !recordsWhosePartsAreLetGo.isEmpty()) {
            if (!recordsWhosePartsAreLetGo.isEmpty()) {
                forEachPartOf(m_madeNodes[recordsWhosePartsAreLetGo.takeLast()], letGo);
                continue;
            }
            uint32_t list = toLetGo.takeLast();
            uint32_t count = 0;
            forEachUseIn(list, [&](uint32_t number) {
                ++count;
                letGo(number);
            });
            giveBackUses(m_usesOfThreads[worker], list, count);
        }
    });
    for (const Share& share : shares) {
        m_counters.madeNodesAtRest += share.numberOfNodesPutToRest;
        if (m_valuesShownToStatement.isEmpty())
            continue;
        for (ValueFlowStatementID statement : share.statements)
            m_valuesShownToStatement.remove(statement.index());
    }
}

void ValueFlowSolver::noteNodesAskedFor()
{
    growAndFill(m_usesOfStatement, numberOfStatements(), noSolverIndex);
    if (m_usesOfThreads.size() < m_threads.size())
        m_usesOfThreads.grow(m_threads.size());
    constexpr size_t runsPerShare = 256;
    struct ShareOfRuns {
        unsigned thread { 0 };
        size_t firstRun { 0 };
        size_t endOfRuns { 0 };
        Vector<Thread::RunThatAsksAnew> runsThatAskAnew;
        Vector<uint32_t> askedForAnew;
        Vector<uint32_t> askedForBefore;
        Vector<uint32_t> nodesWithFirstUser;
    };
    Vector<ShareOfRuns> sharesOfRuns;
    for (unsigned index = 0; index < m_threads.size(); ++index) {
        size_t endOfRuns = m_threads[index]->log.runs.size();
        for (size_t firstRun = m_threads[index]->numberOfRunsTaken; firstRun < endOfRuns; firstRun += runsPerShare)
            sharesOfRuns.append(ShareOfRuns { index, firstRun, std::min(endOfRuns, firstRun + runsPerShare), { }, { }, { }, { } });
    }
    forEachIndexInParallel(m_loops, sharesOfRuns.size(), [&](unsigned index, unsigned worker) {
        ShareOfRuns& share = sharesOfRuns[index];
        const Thread& thread = *m_threads[share.thread];
        AOT_THRESHOLD(size_t, mostComparedOneByOne, 32);
        HashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> seen;
        bool isFirstOfTurn = share.firstRun == thread.numberOfRunsTaken;
        uint32_t valuesShown = isFirstOfTurn ? thread.numberOfValuesShownTaken : thread.log.runs[share.firstRun - 1].endOfValuesShown;
        uint32_t nodeAskedFor = isFirstOfTurn ? thread.numberOfNodesAskedForTaken : thread.log.runs[share.firstRun - 1].endOfNodesAskedFor;
        for (size_t number = share.firstRun; number < share.endOfRuns; ++number) {
            const ValueFlowRoundLog::Run& run = thread.log.runs[number];
            size_t first = share.askedForAnew.size();
            seen.clear();
            for (; nodeAskedFor < run.endOfNodesAskedFor; ++nodeAskedFor) {
                uint32_t madeNodes = thread.log.madeNodesAskedFor[nodeAskedFor];
                if (madeNodes & ValueFlowRoundLog::numberedByLog) [[unlikely]] {
                    madeNodes = m_madeNodesOfNode[thread.numberInGraph(ValueFlowNodeID(madeNodes)).index() - m_input.numberOfNodes];
                    if (!keepsAwake(run.statement, madeNodes))
                        continue;
                }
                std::span<const uint32_t> askedFor = share.askedForAnew.span().subspan(first);
                if (askedFor.size() < mostComparedOneByOne) {
                    if (std::ranges::find(askedFor, madeNodes) != askedFor.end())
                        continue;
                } else {
                    if (seen.isEmpty()) {
                        for (uint32_t earlier : askedFor)
                            seen.add(earlier);
                    }
                    if (!seen.add(madeNodes).isNewEntry)
                        continue;
                }
                share.askedForAnew.append(madeNodes);
            }
            bool addsToEarlierRuns = std::exchange(valuesShown, run.endOfValuesShown) < run.endOfValuesShown;
            if (!addsToEarlierRuns) {
                size_t left = share.askedForAnew.size();
                bool differs = anyUseIn(m_usesOfStatement[run.statement.index()], [&](uint32_t madeNodes) {
                    if (left == first || madeNodes != share.askedForAnew[left - 1])
                        return true;
                    --left;
                    return false;
                });
                if (!differs && left == first) {
                    share.askedForAnew.shrink(first);
                    continue;
                }
            }
            share.runsThatAskAnew.append({ number, static_cast<uint32_t>(share.askedForAnew.size()), addsToEarlierRuns });
        }
        uint32_t next = 0;
        for (const Thread::RunThatAsksAnew& asksAnew : share.runsThatAskAnew) {
            uint32_t& uses = m_usesOfStatement[thread.log.runs[asksAnew.run].statement.index()];
            // What was asked for before is let go of once all that is asked for now has been taken up. The room of the old list is free at once, for the new one.
            if (!asksAnew.addsToEarlierRuns && uses != noSolverIndex) {
                size_t before = share.askedForBefore.size();
                forEachUseIn(uses, [&](uint32_t number) {
                    share.askedForBefore.append(number);
                });
                giveBackUses(m_usesOfThreads[worker], std::exchange(uses, noSolverIndex), share.askedForBefore.size() - before);
            }
            if (next == asksAnew.endOfAskedFor)
                continue;
            uint32_t numberOfEarlierUses = 0;
            forEachUseIn(uses, [&](uint32_t) {
                ++numberOfEarlierUses;
            });
            uint32_t numberOfNewUses = asksAnew.endOfAskedFor - next;
            uint32_t list = takeUses(m_usesOfThreads[worker], numberOfNewUses + numberOfEarlierUses);
            std::span<uint32_t> room = unsafeMakeSpan(&m_uses[list], numberOfNewUses + numberOfEarlierUses);
            if (numberOfEarlierUses) {
                memcpySpan(room.subspan(numberOfNewUses), unsafeMakeSpan<const uint32_t>(&m_uses[uses], numberOfEarlierUses));
                giveBackUses(m_usesOfThreads[worker], uses, numberOfEarlierUses);
            }
            uses = list;
            for (uint32_t position = numberOfNewUses; position--; ++next) {
                constexpr uint32_t distance = 8;
                if (next + distance < share.askedForAnew.size())
                    __builtin_prefetch(&m_numberOfUsers[share.askedForAnew[next + distance]], 1);
                uint32_t number = share.askedForAnew[next];
                if (!WTF::atomicExchangeAdd(&m_numberOfUsers[number], 1u, std::memory_order_relaxed))
                    share.nodesWithFirstUser.append(number);
                room[position] = number;
            }
            if (!numberOfEarlierUses)
                room[numberOfNewUses - 1] |= isLastUse;
        }
    });

    for (ShareOfRuns& share : sharesOfRuns) {
        Thread& thread = *m_threads[share.thread];
        thread.numberOfRunsTaken = share.endOfRuns;
        thread.numberOfNodesAskedForTaken = thread.log.runs[share.endOfRuns - 1].endOfNodesAskedFor;
        for (const Thread::RunThatAsksAnew& asksAnew : share.runsThatAskAnew) {
            const ValueFlowRoundLog::Run& run = thread.log.runs[asksAnew.run];
            for (; thread.numberOfValuesShownTaken < run.endOfValuesShown; ++thread.numberOfValuesShownTaken) {
                ValueFlowNodeSet shown = thread.log.valuesShown[thread.numberOfValuesShownTaken];
                shown.node = thread.numberInGraph(shown.node);
                Vector<ValueFlowNodeSet, 1>& valuesShown = m_valuesShownToStatement.add(run.statement.index(), Vector<ValueFlowNodeSet, 1>()).iterator->value;
                size_t position = valuesShown.findIf([&](const ValueFlowNodeSet& earlier) {
                    return earlier.node == shown.node;
                });
                if (position == notFound)
                    valuesShown.append(shown);
                else
                    valuesShown[position].set = shown.set;
            }
        }
    }

    Vector<uint32_t> nodesWithFirstUser;
    Vector<uint32_t> askedForBefore;
    for (const ShareOfRuns& share : sharesOfRuns) {
        nodesWithFirstUser.appendVector(share.nodesWithFirstUser);
        askedForBefore.appendVector(share.askedForBefore);
    }
    takeUpPartsOfNodesWithFirstUser(WTF::move(nodesWithFirstUser));
    letGoOf(WTF::move(askedForBefore));
}

void ValueFlowSolver::countRunsAndEdges()
{
    AOT_THRESHOLD(uint32_t, fewestConnectionsOfDearStatement, 4096);
    m_runsAndEdges.grow(numberOfOpcodes + 256);
    struct CountsOfLog {
        Vector<RunsAndEdges> runsAndEdges;
        Vector<std::pair<ValueFlowStatementID, bool>> statementsThatTurnDearOrCheap;
        Vector<ValueFlowStatementID> statementsThatWereNotDueAndAdded;
    };
    Vector<CountsOfLog> countsOfLog(m_threads.size());
    forEachIndexInParallel(m_loops, m_threads.size(), [&](unsigned number) {
        CountsOfLog& counts = countsOfLog[number];
        counts.runsAndEdges.grow(m_runsAndEdges.size());
        uint32_t edge = 0;
        uint32_t reached = 0;
        uint32_t made = 0;
        uint32_t partWatched = 0;
        size_t nextWatchedLast = 0;
        uint32_t nodeRead = 0;
        uint32_t nodeAskedWhetherEmpty = 0;
        uint32_t nameWatched = 0;
        Thread& thread = *m_threads[number];
        for (const ValueFlowRoundLog::Run& run : thread.log.runs) {
            for (; nodeRead < run.endOfNodesRead; ++nodeRead) {
                ValueFlowNodeID node = thread.numberInGraph(thread.log.nodesRead[nodeRead]);
                thread.nodesReadOfShard[readerShardOf(node)].append({ node, run.statement });
            }
            for (; nodeAskedWhetherEmpty < run.endOfNodesAskedWhetherEmpty; ++nodeAskedWhetherEmpty) {
                ValueFlowNodeID node = thread.numberInGraph(thread.log.nodesAskedWhetherEmpty[nodeAskedWhetherEmpty]);
                thread.nodesAskedWhetherEmptyOfShard[readerShardOf(node)].append({ node, run.statement });
            }
            for (; nameWatched < run.endOfNamesWatched; ++nameWatched)
                thread.namesWatched.append({ thread.log.namesWatched[nameWatched].name, thread.log.namesWatched[nameWatched].within, run.statement });
            for (; partWatched < run.endOfPartsWatched; ++partWatched) {
                ValueFlowNodeID node = thread.numberInGraph(thread.log.partsWatched[partWatched].node);
                ValueSetID within = thread.log.partsWatched[partWatched].set;
                bool isAskedLast = nextWatchedLast < thread.log.partsWatchedLast.size() && thread.log.partsWatchedLast[nextWatchedLast] == partWatched;
                nextWatchedLast += isAskedLast;
                thread.partsWatchedOfShard[watchShardOf(node, within)].append({ node, within, run.statement, isAskedLast });
            }
            ValueFlowStatement statement = copyOfStatementAt(run.statement);
            unsigned index = statement.kind == ValueFlowStatementKind::Operation ? static_cast<unsigned>(statement.opcode) : numberOfOpcodes + static_cast<unsigned>(statement.kind);
            RunsAndEdges& row = counts.runsAndEdges[index];
            ++row.runs;
            bool hasReached = std::exchange(reached, run.endOfReached) != run.endOfReached;
            bool hasMade = std::exchange(made, run.endOfMade) != run.endOfMade;
            bool hasAdded = hasReached || hasMade || edge != run.endOfEdges;
            row.runsThatAddedNothing += !hasAdded;
            row.runsThatWereNotDueAndAdded += hasAdded && m_isFullRound;
            if (hasAdded && m_isFullRound)
                counts.statementsThatWereNotDueAndAdded.append(run.statement);
            row.connections += run.numberOfConnections;
            bool isDear = run.numberOfConnections > fewestConnectionsOfDearStatement;
            if (m_isDear.get(run.statement.index()) != isDear)
                counts.statementsThatTurnDearOrCheap.append({ run.statement, isDear });
            row.edgesLogged += run.endOfEdges - std::exchange(edge, run.endOfEdges);
        }
    });
    for (const CountsOfLog& counts : countsOfLog) {
        for (unsigned index = 0; index < m_runsAndEdges.size(); ++index) {
            const RunsAndEdges& row = counts.runsAndEdges[index];
            m_runsAndEdges[index].runs += row.runs;
            m_runsAndEdges[index].runsThatAddedNothing += row.runsThatAddedNothing;
            m_runsAndEdges[index].runsThatWereNotDueAndAdded += row.runsThatWereNotDueAndAdded;
            m_runsAndEdges[index].connections += row.connections;
            m_runsAndEdges[index].edgesLogged += row.edgesLogged;
        }
        for (auto [statement, isDear] : counts.statementsThatTurnDearOrCheap) {
            if (m_isDear.get(statement.index()) == isDear)
                continue;
            m_isDear.set(statement.index(), isDear);
            if (isDear)
                m_dearStatements.append(statement);
            else
                m_dearStatements.removeFirst(statement);
        }
    }

    AOT_THRESHOLD(size_t, mostStatementsKept, 16);
    Vector<ValueFlowStatementID> statementsOfRound;
    for (const CountsOfLog& counts : countsOfLog)
        statementsOfRound.appendVector(counts.statementsThatWereNotDueAndAdded);
    AOT::sort(statementsOfRound);
    m_numberOfRunsThatWereNotDueAndAdded += statementsOfRound.size();
    size_t room = mostStatementsKept - m_firstStatementsThatWereNotDueAndAdded.size();
    m_firstStatementsThatWereNotDueAndAdded.append(statementsOfRound.span().first(std::min(room, statementsOfRound.size())));
}

static uint64_t numberOf(ValueFlowEdge edge)
{
    return static_cast<uint64_t>(edge.from.index()) << 32 | edge.to.index();
}

void ValueFlowSolver::rememberWhoLoggedTheEdges()
{
    struct Logged {
        uint64_t edge;
        ValueFlowStatementID statement;
    };
    Vector<Logged> logged;
    Vector<std::pair<uint32_t, ValueFlowStatementID>> askedFor;
    for (const std::unique_ptr<Thread>& thread : m_threads) {
        uint32_t edge = 0;
        uint32_t nodeAskedFor = 0;
        for (const ValueFlowRoundLog::Run& run : thread->log.runs) {
            for (; edge < run.endOfEdges; ++edge)
                logged.append(Logged { numberOf(ValueFlowEdge { thread->numberInGraph(thread->log.edges[edge].from), thread->numberInGraph(thread->log.edges[edge].to) }), run.statement });
            for (; nodeAskedFor < run.endOfNodesAskedFor; ++nodeAskedFor) {
                uint32_t madeNodes = thread->log.madeNodesAskedFor[nodeAskedFor];
                if (madeNodes & ValueFlowRoundLog::numberedByLog)
                    madeNodes = m_madeNodesOfNode[thread->numberInGraph(ValueFlowNodeID(madeNodes)).index() - m_input.numberOfNodes];
                askedFor.append({ madeNodes, run.statement });
            }
        }
    }
    AOT::sort(askedFor);
    for (auto [madeNodes, statement] : askedFor)
        m_firstRequesterOfMadeNodes.add(madeNodes, statement);
    AOT::sort(logged, [](const Logged& first, const Logged& second) {
        return std::tuple { first.edge, first.statement } < std::tuple { second.edge, second.statement };
    });
    for (const Logged& entry : logged) {
        if (m_positionOfLoggedEdge.add(entry.edge, m_edgesInOrderLogged.size()).isNewEntry)
            m_edgesInOrderLogged.append(LoggedEdge { ValueFlowEdge { ValueFlowNodeID(static_cast<uint32_t>(entry.edge >> 32)), ValueFlowNodeID(static_cast<uint32_t>(entry.edge)) }, entry.statement, m_counters.rounds });
    }
}

ValueFlowStatementID ValueFlowSolver::statementThatLogged(ValueFlowEdge edge) const
{
    const LoggedEdge* logged = findLoggedEdge(edge);
    return logged ? logged->statement : ValueFlowStatementID();
}

ValueFlowStatementID ValueFlowSolver::firstRequesterOf(ValueFlowStatementID made) const
{
    return isMadeByRule(made) ? m_firstRequesterOfMadeNodes.get(m_madeNodesOfStatement[made.index() - m_input.statements.size()]) : ValueFlowStatementID();
}

const ValueFlowSolver::LoggedEdge* ValueFlowSolver::findLoggedEdge(ValueFlowEdge edge) const
{
    auto found = m_positionOfLoggedEdge.find(numberOf(edge));
    return found == m_positionOfLoggedEdge.end() ? nullptr : &m_edgesInOrderLogged[found->value];
}

Vector<ValueFlowNodeID> ValueFlowSolver::explain(ValueFlowNodeID start, ValueFlowObjectID member) const
{
    if (!m_sets.contains(m_graph.valueOf(start), member))
        return { };

    struct Step {
        ValueFlowNodeID node;
        ValueFlowObjectID lookedFor;
        uint32_t cameFrom { noSolverIndex };
    };
    Vector<Step> found { Step { start, member, noSolverIndex } };
    ValueFlowPairSet seen;
    seen.add(start.index(), member.index());
    uint32_t end = noSolverIndex;
    for (uint32_t next = 0; next < found.size(); ++next) {
        Step step = found[next];
        const ValueFlowNodeKey* key = keyOfMadeNode(step.node);
        if (key && key->kind == ValueFlowMadeNodeKind::Constant) {
            end = next;
            break;
        }
        Vector<ValueFlowNodeID, 8> predecessors;
        m_graph.forEachPredecessor(step.node, [&](ValueFlowNodeID predecessor) {
            predecessors.append(predecessor);
        });
        AOT::sort(predecessors);
        auto goOnThroughHoldersOf = [&](ValueFlowObjectID lookedFor) {
            bool hasHolder = false;
            for (ValueFlowNodeID predecessor : predecessors) {
                bool hasComeBy = m_sets.contains(m_keptOutOfNodesMadeForUnknownObject, lookedFor) && isMadeForUnknownObject(predecessor);
                hasComeBy |= m_input.nodes && predecessor == m_input.nodes->ofHost(ValueFlowHostNode::ValuesCarriedOut);
                if (!hasComeBy && !m_sets.contains(m_graph.valueOf(predecessor), lookedFor))
                    continue;
                hasHolder = true;
                if (seen.add(predecessor.index(), lookedFor.index()))
                    found.append(Step { predecessor, lookedFor, next });
            }
            return hasHolder;
        };
        if (goOnThroughHoldersOf(step.lookedFor) || m_graph.maskOf(step.node) == ValueSets::anything)
            continue;
        for (ValueFlowObjectID placeholder : { m_input.placeholders.untracked, m_input.placeholders.unknown }) {
            if (placeholder && placeholder != step.lookedFor)
                goOnThroughHoldersOf(placeholder);
        }
    }

    Vector<ValueFlowNodeID> way;
    for (uint32_t step = end; step != noSolverIndex; step = found[step].cameFrom)
        way.append(found[step].node);
    way.reverse();
    return way;
}

std::optional<ValueFlowUnknownSource> ValueFlowSolver::unknownSourceOf(ValueFlowEdge edge) const
{
    const ValueFlowNodeKey* key = keyOfMadeNode(edge.from);
    if (!key || key->kind != ValueFlowMadeNodeKind::Constant)
        return std::nullopt;
    return key->unknownSource();
}

std::optional<ValueFlowUnknownSource> ValueFlowSolver::unknownSourceOf(ValueFlowNodeID node) const
{
    if (node.index() >= m_unknownSourceOfNode.size() || !m_unknownSourceOfNode[node.index()])
        return std::nullopt;
    return static_cast<ValueFlowUnknownSource>(m_unknownSourceOfNode[node.index()] - 1);
}

bool ValueFlowSolver::merge()
{
    bool hasReached = !m_bodiesReachedInRound.isEmpty();

    m_sets.freeOutgrownTables();
    auto takeEdgesOfLogsAndBodies = [&] {
    Vector<size_t> firstEdgeOfLog;
    size_t numberOfEdgesOfRound = m_edgesOfRound.size();
    for (const std::unique_ptr<Thread>& thread : m_threads) {
        m_counters.edgesLogged += thread->log.edges.size();
        firstEdgeOfLog.append(numberOfEdgesOfRound);
        numberOfEdgesOfRound += thread->log.edges.size();
    }
    m_edgesOfRound.grow(numberOfEdgesOfRound);
    forEachIndexInParallel(m_loops, m_threads.size(), [&](unsigned number) {
        const Thread& thread = *m_threads[number];
        size_t position = firstEdgeOfLog[number];
        for (ValueFlowEdge edge : thread.log.edges)
            m_edgesOfRound[position++] = ValueFlowEdge { thread.numberInGraph(edge.from), thread.numberInGraph(edge.to) };
    });

    m_stepTimes.didFinish("merge: edges of the logs copied"_s);
    AOT::sort(m_bodiesReachedInRound);
    Vector<ValueFlowNodeSet> seeds;
    Vector<KeyToMake> constantsToMake;
    HashMap<uint32_t, ValueFlowNodeID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> holding;
    Vector<ValueFlowNodeID> nodesGivenMore;
    for (uint32_t body : m_bodiesReachedInRound) {
        const ValueFlowBodyRange& range = m_input.ranges[body];
        const ValueFlowBodyRange& end = m_input.ranges[body + 1];
        if (m_input.seedOfWidening) [[unlikely]] {
            for (uint32_t node = m_input.nodes->firstNodeOf(FunctionID(body)); node < m_input.nodes->endOfNodesOf(FunctionID(body)); ++node) {
                if (whatIsGivenTo(ValueFlowNodeID(node)) != GivenToNode::Nothing)
                    nodesGivenMore.append(ValueFlowNodeID(node));
            }
        }
        m_edgesOfRound.append(m_input.edgesOnceReached.subspan(range.edgeOnceReached, end.edgeOnceReached - range.edgeOnceReached));
        for (const ValueFlowNodeSet& seed : m_input.seedsOnceReached.subspan(range.seedOnceReached, end.seedOnceReached - range.seedOnceReached)) {
            ValueSetID set = seed.set;
            if (!m_input.unknownSourcesLeftOut.isEmpty() && m_input.placeholders.unknown)
                set = m_makers[0]->intersect(set, ValueSets::complement(m_makers[0]->of(m_input.placeholders.unknown)));
            if (set == ValueSets::nothing)
                continue;
            if (m_input.seedOfWidening && m_input.build->isSabotaged(Sabotage::WhatIsGivenTakesThePlaceOfWhatComes) && whatIsGivenTo(seed.node) != GivenToNode::Nothing) [[unlikely]]
                continue;
            seeds.append(ValueFlowNodeSet { seed.node, set });
            auto added = holding.add(set.index(), ValueFlowNodeID());
            if (!added.isNewEntry)
                continue;
            if (std::optional<ValueFlowNodeID> existing = findMadeNode(ValueFlowNodeKey::constant(set)))
                added.iterator->value = *existing;
            else
                constantsToMake.append(KeyToMake { ValueFlowNodeKey::constant(set), m_sets.hashOfMembers(set) });
        }
    }
    m_bodiesReachedInRound.shrink(0);
    AOT::sortInLine(constantsToMake, [](const KeyToMake& first, const KeyToMake& second) {
        return std::tuple { first.hashOfMembers, first.key.set.index() } < std::tuple { second.hashOfMembers, second.key.set.index() };
    });
    constantsToMake.shrink(std::unique(constantsToMake.begin(), constantsToMake.end(), [](const KeyToMake& first, const KeyToMake& second) {
        return first.key == second.key;
    }) - constantsToMake.begin());
    makeNodesOf(constantsToMake.mutableSpan());
    for (const KeyToMake& made : constantsToMake)
        holding.set(made.key.set.index(), m_madeNodes[made.madeNodes].first);
    for (const ValueFlowNodeSet& seed : seeds)
        m_edgesOfRound.append(ValueFlowEdge { holding.get(seed.set.index()), seed.node });
    for (ValueFlowNodeID node : nodesGivenMore) {
        GivenToNode what = whatIsGivenTo(node);
        if (what == GivenToNode::WhatAnotherNodeOfBodyHolds) {
            m_edgesOfRound.append(ValueFlowEdge { nodeWhoseValueIsGivenTo(node), node });
            continue;
        }
        ValueSetID given = m_input.universe->unknownObjectAlone();
        if (what == GivenToNode::ValueNobodyKnows)
            given = m_makers[0]->unite(given, m_input.universe->anyPrimitive());
        m_edgesOfRound.append(ValueFlowEdge { ensureNodes(ValueFlowNodeKey::constant(given, ValueFlowUnknownSource::DataFromOutside)), node });
    }
    m_numberOfNodesGivenMore += nodesGivenMore.size();
    ValueFlowNodeID holdingWhatRunTimeThrows;
    for (const std::unique_ptr<Thread>& thread : m_threads) {
        if (thread->thrownOfStatementsThatWait.isEmpty())
            continue;
        if (!holdingWhatRunTimeThrows) {
            ValueSetID thrown = m_input.universe->anyPrimitive();
            if (!m_input.unknownSourcesLeftOut.contains(ValueFlowUnknownSource::ThrownByRunTime))
                thrown = m_makers[0]->unite(thrown, m_input.universe->unknownObjectAlone());
            holdingWhatRunTimeThrows = ensureNodes(ValueFlowNodeKey::constant(thrown, ValueFlowUnknownSource::ThrownByRunTime));
        }
        for (ValueFlowNodeID thrown : thread->thrownOfStatementsThatWait)
            m_edgesOfRound.append(ValueFlowEdge { holdingWhatRunTimeThrows, thrown });
        thread->thrownOfStatementsThatWait.shrink(0);
    }
    m_stepTimes.didFinish("merge: bodies reached"_s);
    };
    runTasksInParallel(m_loops, {
        takeEdgesOfLogsAndBodies,
        [&] {
            countRunsAndEdges();
            if (m_input.remembersWhoLoggedEachEdge)
                rememberWhoLoggedTheEdges();
        },
    });
    m_stepTimes.didFinish("merge: runs and edges counted, beside"_s);

    growAndFill(m_readersOfNode, m_graph.numberOfNodes(), 0u);
    growAndFill(m_listenersOfNode, m_graph.numberOfNodes(), static_cast<uint8_t>(0));
    growByDoubling(m_isWatched, m_graph.numberOfNodes());
    auto watchAlways = [&](ValueFlowNodeID node) {
        noteListener(node, WatchesNode);
        return m_isWatched.quickSet(node.index());
    };
    if (m_input.escaped && !watchAlways(m_input.escaped))
        m_seenByWatchersOfEscaped = m_graph.valueOf(m_input.escaped);
    if (m_input.lost && !watchAlways(m_input.lost))
        m_seenByWatchersOfLost = m_graph.valueOf(m_input.lost);
    for (ValueFlowNodeID node : { m_objectsThatAreNotPlain, m_objectsWrittenUnderUnknownKey }) {
        if (node)
            watchAlways(node);
    }
    for (unsigned write = 0; m_firstOfObjectsWithRareWrite && write < numberOfValueFlowRareWrites; ++write)
        watchAlways(ValueFlowNodeID(m_firstOfObjectsWithRareWrite.index() + write));
    constexpr size_t nodesPerShare = 256;
    for (ListWithRoomToFill<NodeToLink>* list : { &m_writtenToLink, &m_readToLink }) {
        Vector<Vector<uint32_t>> withNewNamesOfShare((list->size() + nodesPerShare - 1) / nodesPerShare);
        forEachIndexInParallel(m_loops, withNewNamesOfShare.size(), [&](unsigned share) {
            for (NodeToLink& toLink : list->mutableSpan().subspan(share * nodesPerShare, std::min(nodesPerShare, list->size() - share * nodesPerShare))) {
                ValueSetID set = toLink.node.set;
                toLink.madeNodes = m_madeNodesOfNode[toLink.node.node.index() - m_input.numberOfNodes];
                toLink.size = !isListedByMember(set) ? SizeOfSetToLink::VeryMany : hasFewObjects(m_sets, set) ? SizeOfSetToLink::Few : SizeOfSetToLink::Many;
                toLink.isDearToLink = toLink.size == SizeOfSetToLink::VeryMany;
                toLink.onlyMember = m_sets.onlyMember(set);
                if (toLink.standsForKind) {
                    toLink.numberOfName = numberOfFirstKindAmongNames + static_cast<uint32_t>(toLink.name.kind);
                    continue;
                }
                auto found = m_numberOfName.find(static_cast<uint64_t>(toLink.name.kind) << 32 | toLink.name.payload);
                toLink.numberOfName = found == m_numberOfName.end() ? noSolverIndex : found->value;
                if (toLink.numberOfName == noSolverIndex)
                    withNewNamesOfShare[share].append(static_cast<uint32_t>(&toLink - list->mutableSpan().data()));
            }
        });
        for (const Vector<uint32_t>& withNewNames : withNewNamesOfShare) {
            for (uint32_t index : withNewNames) {
                NodeToLink& toLink = (*list)[index];
                toLink.numberOfName = m_numberOfName.ensure(static_cast<uint64_t>(toLink.name.kind) << 32 | toLink.name.payload, [&] {
                    makeRoomByDoubling(m_names);
                    m_names.append(Name { toLink.name, { }, { } });
                    return static_cast<uint32_t>(m_names.size() - 1);
                }).iterator->value;
            }
        }
    }
    bool areNamesWatched = std::ranges::any_of(m_threads, [](const std::unique_ptr<Thread>& thread) {
        return !thread->namesWatched.isEmpty();
    });
    if (areNamesWatched) {
        numberNamesWatched();
        forEachIndexInParallel(m_loops, numberOfLinkShardsOfNames, [&](unsigned shard, unsigned worker) {
            noteNamesWatched(shard, *m_makers[worker]);
        });
    }
    m_stepTimes.didFinish("merge: tables grown, names numbered"_s);
    AOT_THRESHOLD(size_t, fewestNodesToLinkInChunks, 1024);
    size_t numberOfEdgesAdded = 0;
    auto noteWatchers = [&](unsigned shard) {
        for (const std::unique_ptr<Thread>& thread : m_threads) {
            for (const Thread::PartWatched& part : thread->partsWatchedOfShard[shard])
                watch(m_watchShards[shard], part.node, part.within, part.statement, part.isAskedLast);
            thread->partsWatchedOfShard[shard].shrink(0);
        }
    };
    if (m_writtenToLink.size() + m_readToLink.size() < fewestNodesToLinkInChunks) {
        forEachIndexInParallel(m_loops, 1 + numberOfLinkShardsOfNames + numberOfWatchShards + numberOfReaderShards, [&](unsigned task, unsigned worker) {
            if (task > numberOfLinkShardsOfNames + numberOfWatchShards) {
                noteReaders(task - numberOfLinkShardsOfNames - numberOfWatchShards - 1, *m_makers[worker]);
                return;
            }
            if (task > numberOfLinkShardsOfNames) {
                noteWatchers(task - numberOfLinkShardsOfNames - 1);
                return;
            }
            if (task) {
                linkNamedNodes(task - 1, false, *m_makers[worker]);
                linkNamedNodes(task - 1, true, *m_makers[worker]);
                return;
            }
            for (unsigned shard = numberOfLinkShardsOfNames; shard < numberOfLinkShards; ++shard) {
                linkNamedNodes(shard, false, *m_makers[worker]);
                linkNamedNodes(shard, true, *m_makers[worker]);
            }
        });
        Vector<Linked::LongList> longLists;
        for (LinkShard& shard : m_linkShards) {
            longLists.appendVector(shard.linked.longLists);
            shard.linked.longLists.shrink(0);
        }
        connectAcrossLongLists(longLists.span());
    } else {
        ASSERT(m_edgesInFrontOfLinks.isEmpty());
        std::swap(m_edgesOfRound, m_edgesInFrontOfLinks);
        auto longestFirst = [&]<size_t numberOfShards>(std::array<uint32_t, numberOfShards>& shards, NOESCAPE const Invocable<size_t(const Thread&, unsigned shard)> auto& sizeIn) {
            std::array<size_t, numberOfShards> sizes { };
            for (unsigned shard = 0; shard < numberOfShards; ++shard) {
                shards[shard] = shard;
                for (const std::unique_ptr<Thread>& thread : m_threads)
                    sizes[shard] += sizeIn(*thread, shard);
            }
            AOT::stableSortInLine(shards, std::ranges::greater(), [&](uint32_t shard) {
                return sizes[shard];
            });
        };
        std::array<uint32_t, numberOfWatchShards> watchShardsLongestFirst;
        longestFirst(watchShardsLongestFirst, [](const Thread& thread, unsigned shard) {
            return thread.partsWatchedOfShard[shard].size();
        });
        std::array<uint32_t, numberOfReaderShards> readerShardsLongestFirst;
        longestFirst(readerShardsLongestFirst, [](const Thread& thread, unsigned shard) {
            return thread.nodesReadOfShard[shard].size();
        });
        auto noteReadersOfShard = [&](unsigned position, unsigned worker) {
            noteReaders(readerShardsLongestFirst[position], *m_makers[worker]);
        };
        // One set that very many statements watch makes its shard the longest. Nothing that is linked looks at who watches.
        runTasksInParallel(m_loops, {
            [&] {
                linkInChunks(false, numberOfReaderShards, noteReadersOfShard);
                linkInChunks(true, 0, noteReadersOfShard);
            },
            [&] {
                numberOfEdgesAdded = m_graph.addEdges(m_edgesInFrontOfLinks.span());
            },
            [&] {
                forEachIndexInParallel(m_loops, numberOfWatchShards, [&](unsigned position) {
                    noteWatchers(watchShardsLongestFirst[position]);
                });
            },
        });
        m_edgesInFrontOfLinks.shrink(0);
    }
    chainSetsWatchedForCover();
    m_stepTimes.didFinish("merge: link, watchers, readers"_s);
    for (LinkShard& shard : m_linkShards) {
        m_edgesOfRound.appendVector(shard.linked.edges);
        shard.linked.edges.shrink(0);
        m_counters.testsWhetherSetsMeet += std::exchange(shard.linked.testsWhetherSetsMeet, 0);
        m_due.appendVector(shard.woken);
        shard.woken.shrink(0);
    }
    m_writtenToLink.shrink(0);
    m_readToLink.shrink(0);
    numberOfEdgesAdded += m_graph.addEdges(m_edgesOfRound.span());
    m_edgesOfRound.shrink(0);
    m_stepTimes.didFinish("merge: edges added"_s);

    bool hasUses = false;
    for (const std::unique_ptr<Thread>& thread : m_threads) {
        for (const ValueFlowRoundLog::Use& use : thread->log.uses) {
            if (use.builtin.index() >= m_usesOfBuiltin.size())
                m_usesOfBuiltin.grow(use.builtin.index() + 1);
            OptionSet<BuiltinUse>& uses = m_usesOfBuiltin[use.builtin.index()];
            hasUses |= !uses.containsAll(use.uses);
            uses.add(use.uses);
        }
        for (ValueFlowStatementID statement : thread->log.statementsWithMembersNotToldApart)
            m_statementsWithMembersNotToldApart.set(statement.index());
        for (ValueFlowStatementID statement : thread->log.statementsThatAskProxiesByTheirTraps)
            m_statementsThatAskProxiesByTheirTraps.set(statement.index());
        for (RuntimeFunctionID function : thread->log.libraryFunctionsLacked)
            m_libraryFunctionsLacked.set(function.index());
    }

    for (const std::unique_ptr<Thread>& thread : m_threads) {
        thread->log.clear();
        thread->numberOfMadeNode.shrink(0);
        thread->numberOfReachedTaken = 0;
        thread->numberOfMadeTaken = 0;
        thread->numberOfRunsTaken = 0;
        thread->numberOfFunctionsAskedTaken = 0;
        thread->numberOfValuesShownTaken = 0;
        thread->numberOfNodesAskedForTaken = 0;
    }
    m_stepTimes.didFinish("merge: uses of built-ins, logs cleared"_s);
    return hasReached || hasUses || numberOfEdgesAdded || m_numberOfNodesMade != m_numberOfNodesBeforeRound;
}

ValueSetID ValueFlowSolver::withoutWhatOtherMembersCover(ValueFlowNodeID node, ValueSetMaker& maker) const
{
    return node == m_input.escaped || node == m_input.lost ? withoutMarks(m_graph.valueOf(node), maker) : withoutWhatOtherMembersCover(m_graph.valueOf(node), maker);
}

// Many nodes hold the same value. What is asked of the value alone is remembered under it, the latest value of each slot.
template<typename Answer>
class AnswersAboutLatestValues {
public:
    AOT_THRESHOLD(size_t, numberOfSlots, 1 << 14);

    Answer ensure(ValueSetID value, NOESCAPE const Invocable<Answer()> auto& find)
    {
        if (m_slots.isEmpty())
            m_slots.grow(numberOfSlots);
        Slot& slot = m_slots[(value.index() * 0x9e3779b1u) >> 18];
        if (slot.value != value)
            slot = { value, find() };
        return slot.answer;
    }

private:
    struct Slot {
        ValueSetID value;
        Answer answer { };
    };
    static_assert(numberOfSlots == 1ull << (32 - 18));
    Vector<Slot> m_slots;
};

// The kinds of objects in a block of a set, as a chain of kinds. Sets that grow share most of their blocks, so a set is not walked member by member each time it grows.
class KindsOfBlocks {
public:
    static constexpr uint32_t noKinds = 0;
    static constexpr uint32_t tooMany = UINT32_MAX;
    AOT_THRESHOLD(size_t, numberOfSlots, 1 << 16);

    KindsOfBlocks() { m_chains.append({ }); }

    uint32_t add(uint32_t chain, uint32_t kind, size_t most)
    {
        if (chain == tooMany)
            return tooMany;
        for (uint32_t link = chain; link != noKinds; link = m_chains[link].rest) {
            if (m_chains[link].kind == kind)
                return chain;
        }
        if (m_chains[chain].size >= most)
            return tooMany;
        return m_longer.ensure(static_cast<uint64_t>(chain) << 32 | kind, [&] {
            m_chains.append({ chain, kind, m_chains[chain].size + 1 });
            return static_cast<uint32_t>(m_chains.size() - 1);
        }).iterator->value;
    }

    uint32_t unite(uint32_t chain, uint32_t other, size_t most)
    {
        if (chain == other || other == noKinds)
            return chain;
        if (other == tooMany)
            return tooMany;
        if (chain == noKinds)
            return other;
        for (; other != noKinds && chain != tooMany; other = m_chains[other].rest)
            chain = add(chain, m_chains[other].kind, most);
        return chain;
    }

    uint32_t ofBlock(uint64_t block, NOESCAPE const Invocable<uint32_t()> auto& find)
    {
        if (m_slots.isEmpty())
            m_slots.grow(numberOfSlots);
        Slot& slot = m_slots[(block * 0x9e3779b97f4a7c15ull) >> 48];
        if (slot.block != block)
            slot = { block, find() };
        return slot.chain;
    }

private:
    struct Chain {
        uint32_t rest { noKinds };
        uint32_t kind { 0 };
        uint32_t size { 0 };
    };
    struct Slot {
        uint64_t block { UINT64_MAX };
        uint32_t chain { noKinds };
    };
    static_assert(numberOfSlots == 1ull << (64 - 48));
    Vector<Chain> m_chains;
    HashMap<uint64_t, uint32_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_longer;
    Vector<Slot> m_slots;
};

void ValueFlowSolver::leaveOutWhatIsCoveredInTheEnd()
{
    if (!m_input.nodes || !m_input.universe)
        return;
    uint32_t firstNodeOfHost = m_input.nodes->ofHost(static_cast<ValueFlowHostNode>(0)).index();
    auto isReadByResults = [&](ValueFlowNodeID node) {
        if (node == m_input.escaped || node == m_input.lost)
            return false;
        if (node.index() < firstNodeOfHost)
            return true;
        const ValueFlowNodeKey* key = keyOfMadeNode(node);
        if (!key)
            return false;
        switch (key->kind) {
        case ValueFlowMadeNodeKind::Read:
        case ValueFlowMadeNodeKind::Written:
        case ValueFlowMadeNodeKind::Lookup:
        case ValueFlowMadeNodeKind::ReadUnderKind:
        case ValueFlowMadeNodeKind::WrittenUnderKind:
            return true;
        case ValueFlowMadeNodeKind::Constant:
        case ValueFlowMadeNodeKind::CallGroup:
        case ValueFlowMadeNodeKind::Hub:
        case ValueFlowMadeNodeKind::Masked:
        case ValueFlowMadeNodeKind::Marked:
        case ValueFlowMadeNodeKind::Instance:
            return false;
        }
        RELEASE_ASSERT_NOT_REACHED();
    };
    constexpr uint32_t nodesPerShare = nodesPerShareOfListed;
    uint32_t numberOfNodes = m_graph.numberOfNodes();
    uint32_t numberOfShares = (numberOfNodes + nodesPerShare - 1) / nodesPerShare;
    m_listedWithCoveredMembers.grow(numberOfShares);
    m_functionsOfEngineListed.grow(numberOfShares);

    // What the library constructs and lets escape.
    ValueSetID constructedByLibrary = ValueSets::nothing;
    if (m_input.program && m_input.escaped) {
        ValueSetMaker& maker = *m_makers[0];
        Vector<ValueFlowObjectID> constructed;
        m_sets.forEachMember(maker.intersect(m_graph.valueOf(m_input.escaped), m_input.universe->constructedObjects()), [&](ValueFlowObjectID object) {
            if (isLibraryCode(*m_input.program, m_input.program->at(m_input.universe->functionOf(object))))
                constructed.append(object);
        });
        constructedByLibrary = maker.of(constructed.span());
    }
    m_stepTimes.didFinish("behind the rounds: covered: what the library constructs"_s);
    Vector<uint32_t> seenAfterRounds;
    auto seenBehindTheRoundsAt = [&](ValueFlowNodeID node) {
        return seenAfterRounds.isEmpty() ? ValueSets::nothing : ValueSetID(seenAfterRounds[node.index()]);
    };
    // Whether a rule has read a member of a receiver hangs on which came first, the rule or the escape of the receiver. So a read under a name off a receiver nobody knows sees the objects that are written under that name, and so does whatever the function of the read hands its result on to.
    // The code of the program sees what the program makes, a module of the run time what the run time makes.
    if (m_input.program && m_input.escaped && !m_coverings.isEmpty()) {
        ValueSetMaker& maker = *m_makers[0];
        ValueSetID followed = maker.intersect(ValueSets::complement(m_coverings[0].allButCovered), maker.unite(maker.unite(m_objectsOfProgram, constructedByLibrary), maker.unite(maker.unite(m_input.universe->otherCallables(), m_input.universe->sureInstancesOf(BuiltinClasses::Array)), m_input.universe->builtinFunctions())));
        ValueSetID listsFollowed = maker.intersect(followed, m_input.universe->sureInstancesOf(BuiltinClasses::Array));
        // All arrays count as one kind.
        auto kindOfObject = [&](ValueFlowObjectID object) {
            if (LayoutID layout = m_input.universe->layoutOf(object))
                return layout.index();
            return m_sets.contains(listsFollowed, object) ? UINT32_MAX : UINT32_MAX - 1 - object.index();
        };
        ValueSetID functionsOfEngine = maker.intersect(followed, m_input.universe->builtinFunctions());
        auto isOfLibrary = [&](FunctionID function) {
            return function && isLibraryCode(*m_input.program, m_input.program->at(function));
        };
        Vector<ValueFlowObjectID> madeByLibrary;
        m_sets.forEachMember(followed, [&](ValueFlowObjectID object) {
            FunctionID closureFunction = m_input.universe->closureFunction(object);
            if (isOfLibrary(closureFunction ? closureFunction : m_input.universe->functionOf(object)))
                madeByLibrary.append(object);
        });
        std::array<ValueSetID, 2> followedOnSide { followed, maker.of(madeByLibrary.span()) };
        followedOnSide[0] = maker.unite(maker.intersect(followed, ValueSets::complement(followedOnSide[1])), constructedByLibrary);
        followedOnSide[1] = maker.unite(followedOnSide[1], functionsOfEngine);
        // A read by name: from a statement's first operand, or from the result of a runtime module.
        constexpr uint32_t noStatement = UINT32_MAX;
        struct Read {
            uint32_t keyOnSide;
            uint32_t result;
            uint32_t object;
            uint32_t statement;
        };
        Vector<Read> reads = collectInParallel<Read>(m_loops, m_input.statements.size(), [&](unsigned index, Vector<Read>& found) {
            const ValueFlowStatement& statement = m_input.statements[index];
            if (statement.kind != ValueFlowStatementKind::Operation || (statement.opcode != Opcode::PropGet && statement.opcode != Opcode::ElemGet) || !statement.result || !isReached(statement.function))
                return;
            ValueFlowNodeID object = operandsOf(statement)[0].node;
            if (!object || !m_sets.contains(m_graph.valueOf(statement.result), m_input.placeholders.unknown))
                return;
            PropertyKeyID key;
            if (statement.opcode == Opcode::PropGet)
                key = m_input.program->at(statement.function).at(statement.site).key();
            else {
                const ValueFlowOperand& computed = operandsOf(statement)[1];
                ValueSetID keys = computed.node ? m_graph.valueOf(computed.node) : computed.constant ? computed.constant : ValueSets::nothing;
                if (!ValueSets::isComplement(keys) && m_sets.size(keys) == 1) {
                    m_sets.forEachMember(keys, [&](ValueFlowObjectID only) {
                        key = m_input.universe->keyOf(only);
                    });
                }
            }
            if (key)
                found.append({ key.index() * 2 + isOfLibrary(statement.function), statement.result.index(), object.index(), index });
        });
        StringID defaultName = m_input.program->stringOf(BuiltinNames::defaultKeyword);
        for (const Module& module : m_input.program->modules) {
            if (module.exportRule == SyntheticExportRule::None || module.kind != ModuleKind::RuntimeSource || !module.body || !isReached(module.body) || m_input.program->at(module.body).results.isEmpty())
                continue;
            uint32_t object = m_input.nodes->result(module.body, 0).index();
            for (const SyntheticExport& entry : module.syntheticExports) {
                std::optional<PropertyKeyID> key = entry.variable && entry.name != defaultName ? m_input.program->types.find({ .kind = PropertyKeyKind::Text, .text = entry.name }) : std::nullopt;
                if (!key || !m_sets.contains(m_graph.valueOf(m_input.nodes->variable(entry.variable)), m_input.placeholders.unknown))
                    continue;
                uint32_t keyOnSide = key->index() * 2 + isOfLibrary(module.body);
                reads.append({ keyOnSide, m_input.nodes->variable(entry.variable).index(), object, noStatement });
            }
        }
        // What is stored by name into an object of a layout is stored into that layout's field, and the fields of its parent layouts.
        AOT_THRESHOLD(size_t, mostReceiversOfOneWriteFollowed, 256);
        auto layoutsExpectedAt = [&](const ValueFlowStatement& statement) {
            const Function& function = m_input.program->at(statement.function);
            return m_input.program->expectedOrTypeOf(function, function.at(statement.site).objectOperand()).predicted().layouts;
        };
        auto visibleReceiversAt = [&](ValueFlowNodeID object, ValueSetMaker& makerOfWorker) {
            ValueSetID receivers = m_graph.valueOf(object);
            if (ValueSets::isComplement(receivers) || m_sets.size(receivers) > mostReceiversOfOneWriteFollowed)
                return ValueSets::nothing;
            return m_sets.contains(receivers, m_input.placeholders.unknown) ? makerOfWorker.intersect(receivers, m_coverings[0].allButCovered) : receivers;
        };
        // Reads of one name from receivers that are expected and observed to have the same layouts read the same fields.
        struct ReadInGroup {
            uint32_t keyOnSide;
            uint32_t layoutsExpected;
            uint32_t visibleReceivers;
            uint32_t read;

            constexpr auto operator<=>(const ReadInGroup&) const = default;
        };
        constexpr uint32_t readsPerShare = 1 << 9;
        Vector<ReadInGroup> readsByGroup;
        readsByGroup.grow(reads.size());
        forEachIndexInParallel(m_loops, (reads.size() + readsPerShare - 1) / readsPerShare, [&](unsigned share, unsigned worker) {
            for (uint32_t index = share * readsPerShare; index < std::min<uint32_t>(reads.size(), (share + 1) * readsPerShare); ++index) {
                const Read& read = reads[index];
                LayoutSetID expected = read.statement != noStatement ? layoutsExpectedAt(m_input.statements[read.statement]) : LayoutSetID();
                readsByGroup[index] = { read.keyOnSide, expected.index(), visibleReceiversAt(ValueFlowNodeID(read.object), *m_makers[worker]).index(), index };
            }
        });
        parallelSort(m_loops, readsByGroup.mutableSpan(), std::less<ReadInGroup> { });
        struct Group {
            uint32_t firstRead;
            uint32_t endOfReads;
            uint32_t share { 0 };
            uint32_t firstLayout { 0 };
            uint32_t numberOfLayouts { 0 };
            ValueSetID written { ValueSets::nothing };
            ValueSetID arrived { ValueSets::nothing };
        };
        Vector<Group> groups;
        Vector<uint32_t> groupOfRead(reads.size());
        struct ReadsUnderKey {
            uint32_t first;
            uint32_t end;
        };
        HashMap<uint32_t, ReadsUnderKey, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> readsUnderKey;
        for (uint32_t index = 0; index < readsByGroup.size(); ++index) {
            const ReadInGroup& read = readsByGroup[index];
            const ReadInGroup* previous = index ? &readsByGroup[index - 1] : nullptr;
            if (!previous || previous->keyOnSide != read.keyOnSide) {
                if (previous)
                    readsUnderKey.find(previous->keyOnSide)->value.end = index;
                readsUnderKey.add(read.keyOnSide, ReadsUnderKey { index, static_cast<uint32_t>(readsByGroup.size()) });
            }
            if (!previous || previous->keyOnSide != read.keyOnSide || previous->layoutsExpected != read.layoutsExpected || previous->visibleReceivers != read.visibleReceivers)
                groups.append({ index, index });
            groups.last().endOfReads = index + 1;
            groupOfRead[read.read] = groups.size() - 1;
        }
        auto forEachResultOfReadFromUnknownObject = [&](uint32_t keyOnSide, NOESCAPE const Invocable<void(ValueFlowNodeID)> auto& functor) {
            ReadsUnderKey range = readsUnderKey.get(keyOnSide);
            for (uint32_t index = range.first; index < range.end; ++index) {
                const Read& read = reads[readsByGroup[index].read];
                if (read.statement != noStatement && m_sets.contains(m_graph.valueOf(ValueFlowNodeID(read.object)), m_input.placeholders.unknown))
                    functor(ValueFlowNodeID(read.result));
            }
        };
        m_stepTimes.didFinish("behind the rounds: covered: reads off the unknown object"_s);
        auto makeListOfZeros = [&](size_t size) {
            constexpr size_t entriesPerShare = 1 << 16;
            Vector<uint32_t> list;
            list.grow(size);
            forEachIndexInParallel(m_loops, static_cast<unsigned>((size + entriesPerShare - 1) / entriesPerShare), [&](unsigned share) {
                zeroSpan(list.mutableSpan().subspan(share * entriesPerShare, std::min(entriesPerShare, size - share * entriesPerShare)));
            });
            return list;
        };
        static_assert(!ValueSets::nothing.index());
        seenAfterRounds = makeListOfZeros(numberOfNodes);
        Vector<uint32_t> toVisit;
        // More kinds of objects than a prediction names. What holds them hands that on, so the answer is the same in whatever order the nodes are visited.
        constexpr ValueSetID many = ValueSets::anything;
        AOT_THRESHOLD(size_t, mostKindsSeenAtNode, 64);
        Vector<AnswersAboutLatestValues<bool>> canValuesBeNamedPerWorker(m_makers.size());
        Vector<KindsOfBlocks> kindsOfBlocksPerWorker(m_makers.size());
        auto canBeNamedForWorker = [&](unsigned worker, ValueSetID seen) {
            return canValuesBeNamedPerWorker[worker].ensure(seen, [&] {
                KindsOfBlocks& kinds = kindsOfBlocksPerWorker[worker];
                uint32_t chain = KindsOfBlocks::noKinds;
                if (ValueSets::isComplement(seen)) {
                    m_sets.forEachMemberInLine(seen, [&](ValueFlowObjectID object) {
                        chain = kinds.add(chain, kindOfObject(object), mostKindsSeenAtNode);
                    });
                    return chain != KindsOfBlocks::tooMany;
                }
                m_sets.forEachBlock(seen, [&](uint64_t block) {
                    uint32_t ofBlock = kinds.ofBlock(block, [&] {
                        uint32_t found = KindsOfBlocks::noKinds;
                        m_sets.forEachMemberOfBlock(block, [&](ValueFlowObjectID object) {
                            found = kinds.add(found, kindOfObject(object), mostKindsSeenAtNode);
                        });
                        return found;
                    });
                    chain = kinds.unite(chain, ofBlock, mostKindsSeenAtNode);
                });
                return chain != KindsOfBlocks::tooMany;
            });
        };
        auto canBeNamed = [&](ValueSetID seen) {
            return canBeNamedForWorker(0, seen);
        };
        uint64_t numberOfChangesSeen = 0;
        auto see = [&](ValueFlowNodeID at, ValueSetID seen) {
            seenAfterRounds[at.index()] = seen.index();
            ++numberOfChangesSeen;
            toVisit.append(at.index());
        };
        auto canSeeMoreAt = [&](ValueFlowNodeID to) {
            return seenAfterRounds[to.index()] != many.index() && m_sets.contains(m_graph.valueOf(to), m_input.placeholders.unknown);
        };
        auto arriveAt = [&](ValueFlowNodeID to, ValueSetID come) {
            if (come == ValueSets::nothing || !canSeeMoreAt(to))
                return;
            ValueSetID seen(seenAfterRounds[to.index()]);
            ValueSetID after = many;
            if (come != many) {
                after = maker.unite(seen, maker.intersect(come, m_graph.maskOf(to)));
                if (after == seen)
                    return;
                if (!canBeNamed(after))
                    after = many;
            }
            see(to, after);
        };
        auto holdsUnknownObject = [&](ValueFlowNodeID node) {
            return m_sets.contains(m_graph.valueOf(node), m_input.placeholders.unknown);
        };
        // The effects of stores, calls and visits are computed on all threads from the values seen at that moment, then applied on one thread in order.
        // A step is applied as computed if what it depends on is unchanged. Otherwise it is recomputed on the spot.
        constexpr uint32_t noNode = UINT32_MAX;
        enum class Found : uint8_t { Visit, Store, FieldWritten, CallPlannedAnew, PlanBefore, StepPlanned, Call, EnterFunction, Forward, Arrival, ResultsForwarded, Read, AccessesOfLists };
        struct FoundStep {
            Found kind;
            uint32_t index { 0 };
            uint32_t input { UINT32_MAX };
            ValueSetID seenAtInput { };
            uint32_t to { UINT32_MAX };
            ValueSetID come { };
            ValueSetID seenBefore { };
            ValueSetID seenAfter { };
            uint32_t numberOfFields { 0 };
        };
        auto isSeenAsBefore = [&](const FoundStep& step) {
            return step.input == noNode || seenAfterRounds[step.input] == step.seenAtInput.index();
        };
        auto findArrival = [&](ValueFlowNodeID to, ValueSetID come, unsigned worker, FoundStep& step) {
            ValueSetID seen = seenBehindTheRoundsAt(to);
            if (come == ValueSets::nothing || seen == many || !holdsUnknownObject(to))
                return false;
            ValueSetID after = many;
            if (come != many) {
                come = m_makers[worker]->intersect(come, m_graph.maskOf(to));
                after = m_makers[worker]->unite(seen, come);
                if (after == seen)
                    return false;
                if (!canBeNamedForWorker(worker, after))
                    after = many;
            }
            step.to = to.index();
            step.come = come;
            step.seenBefore = seen;
            step.seenAfter = after;
            return true;
        };
        auto doArrival = [&](const FoundStep& step) {
            ValueFlowNodeID to(step.to);
            ValueSetID seen = seenBehindTheRoundsAt(to);
            if (seen == many)
                return;
            ValueSetID after = step.seenAfter;
            if (seen != step.seenBefore) {
                after = many;
                if (step.come != many) {
                    after = maker.unite(seen, step.come);
                    if (after == seen)
                        return;
                    if (!canBeNamed(after))
                        after = many;
                }
            }
            see(to, after);
        };
        struct LayoutsWithoutRepeats {
            void begin(size_t numberOfLayouts)
            {
                if (turnOfLayout.isEmpty())
                    turnOfLayout.fill(0, numberOfLayouts);
                ++turn;
                list.shrink(0);
            }
            bool add(LayoutID layout)
            {
                if (std::exchange(turnOfLayout[layout.index()], turn) == turn)
                    return false;
                list.append(layout);
                return true;
            }

            Vector<uint32_t> turnOfLayout;
            uint32_t turn { 0 };
            Vector<LayoutID, 64> list;
        };
        size_t numberOfLayouts = m_input.program->layouts.size();
        Vector<LayoutsWithoutRepeats> layoutsOfWorker(m_makers.size());
        auto addWithLayoutsAbove = [&](LayoutID layout, LayoutsWithoutRepeats& layouts) {
            for (; layout && layouts.add(layout); layout = m_input.program->at(layout).base) { }
        };
        struct Field {
            ValueSetID written { ValueSets::nothing };
            uint32_t firstGroup { UINT32_MAX };
            uint32_t endOfGroups { UINT32_MAX };
            Vector<uint32_t> resultsOfLaterReads;
        };
        HashMap<uint64_t, Field, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> fields;
        auto fieldOf = [](uint32_t keyOnSide, LayoutID layout) {
            return static_cast<uint64_t>(keyOnSide) << 32 | layout.index();
        };
        // A function object has no layout. A module of the run time that exports one hangs its other exports on it.
        auto fieldOfClosure = [](uint32_t keyOnSide, ValueFlowObjectID closure) {
            return static_cast<uint64_t>(keyOnSide) << 32 | 1u << 31 | closure.index();
        };
        auto isClosureOfLibrary = [&](ValueFlowObjectID object) {
            return isOfLibrary(m_input.universe->closureFunction(object));
        };
        struct WrittenIntoField {
            uint64_t field;
            ValueSetID written;
        };
        HashMap<uint32_t, ValueSetID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> writtenUnderKey;
        Vector<Vector<WrittenIntoField>> writtenInShare(numberOfShares);
        Vector<Vector<WrittenIntoField>> writtenIntoFieldsInShare(numberOfShares);
        Vector<Vector<WrittenIntoField>> copiedOntoClosuresInShare(numberOfShares);
        forEachIndexInParallel(m_loops, numberOfShares, [&](unsigned share, unsigned worker) {
            ValueSetMaker& makerOfWorker = *m_makers[worker];
            LayoutsWithoutRepeats& layouts = layoutsOfWorker[worker];
            for (uint32_t index = std::max({ firstNodeOfHost, m_input.numberOfNodes, share * nodesPerShare }); index < std::min(numberOfNodes, (share + 1) * nodesPerShare); ++index) {
                const ValueFlowNodeKey* key = keyOfMadeNode(ValueFlowNodeID(index));
                if (key->kind == ValueFlowMadeNodeKind::Written && key->name().kind == ValueFlowNameKind::CopiedFrom && !ValueSets::isComplement(key->set) && m_sets.size(key->set) <= mostReceiversOfOneWriteFollowed) {
                    m_sets.forEachMember(key->set, [&](ValueFlowObjectID receiver) {
                        if (isClosureOfLibrary(receiver))
                            copiedOntoClosuresInShare[share].append({ receiver.index(), m_graph.valueOf(ValueFlowNodeID(index)) });
                    });
                }
                if (key->kind != ValueFlowMadeNodeKind::Written || key->name().kind != ValueFlowNameKind::Property)
                    continue;
                ValueSetID value = m_graph.valueOf(ValueFlowNodeID(index));
                bool hasLayouts = false;
                for (uint32_t side = 0; side < 2; ++side) {
                    uint32_t keyOnSide = key->name().payload * 2 + side;
                    if (!readsUnderKey.contains(keyOnSide))
                        continue;
                    ValueSetID written = makerOfWorker.intersect(value, followedOnSide[side]);
                    if (written != ValueSets::nothing)
                        writtenInShare[share].append({ keyOnSide, written });
                    if (ValueSets::isComplement(key->set) || m_sets.size(key->set) > mostReceiversOfOneWriteFollowed)
                        continue;
                    ValueSetID writtenIntoFields = makerOfWorker.intersect(value, followed);
                    if (writtenIntoFields == ValueSets::nothing || m_sets.contains(value, m_input.placeholders.unknown))
                        continue;
                    if (!std::exchange(hasLayouts, true)) {
                        layouts.begin(numberOfLayouts);
                        m_sets.forEachMember(key->set, [&](ValueFlowObjectID receiver) {
                            addWithLayoutsAbove(m_input.universe->layoutOf(receiver), layouts);
                        });
                    }
                    for (LayoutID layout : layouts.list)
                        writtenIntoFieldsInShare[share].append({ fieldOf(keyOnSide, layout), writtenIntoFields });
                    m_sets.forEachMember(key->set, [&](ValueFlowObjectID receiver) {
                        if (isClosureOfLibrary(receiver))
                            writtenIntoFieldsInShare[share].append({ fieldOfClosure(keyOnSide, receiver), writtenIntoFields });
                    });
                }
            }
        });
        auto uniteByField = [&](Vector<Vector<WrittenIntoField>>&& lists, NOESCAPE const Invocable<void(uint64_t, ValueSetID)> auto& take) {
            ParallelArray<WrittenIntoField> all = ParallelArray<WrittenIntoField>::concatenationOf(m_loops, WTF::move(lists));
            parallelSort(m_loops, all.mutableSpan(), [](const WrittenIntoField& first, const WrittenIntoField& second) {
                return first.field < second.field;
            });
            Vector<uint32_t> firstOfField = findIndicesInParallel(m_loops, all.size(), [&](unsigned index) {
                return !index || all[index - 1].field != all[index].field;
            });
            constexpr uint32_t fieldsPerShare = 1 << 7;
            forEachIndexInParallel(m_loops, (firstOfField.size() + fieldsPerShare - 1) / fieldsPerShare, [&](unsigned share, unsigned worker) {
                for (uint32_t field = share * fieldsPerShare; field < std::min<uint32_t>(firstOfField.size(), (share + 1) * fieldsPerShare); ++field) {
                    WrittenIntoField& united = all[firstOfField[field]];
                    for (uint32_t index = firstOfField[field] + 1; index < (field + 1 < firstOfField.size() ? firstOfField[field + 1] : all.size()); ++index)
                        united.written = m_makers[worker]->unite(united.written, all[index].written);
                }
            });
            for (uint32_t first : firstOfField)
                take(all[first].field, all[first].written);
        };
        uniteByField(WTF::move(writtenInShare), [&](uint64_t keyOnSide, ValueSetID written) {
            writtenUnderKey.add(static_cast<uint32_t>(keyOnSide), written);
        });
        uniteByField(WTF::move(writtenIntoFieldsInShare), [&](uint64_t field, ValueSetID written) {
            fields.add(field, Field { written });
        });
        HashMap<uint32_t, ValueSetID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> copiedOntoClosure;
        uniteByField(WTF::move(copiedOntoClosuresInShare), [&](uint64_t closure, ValueSetID sources) {
            copiedOntoClosure.add(static_cast<uint32_t>(closure), sources);
        });
        m_stepTimes.didFinish("behind the rounds: covered: what is written under the names of reads"_s);
        // A name under which many kinds of objects are written tells nothing about what a read answers.
        AOT_THRESHOLD(size_t, mostKindsToldByName, 4);
        auto tellsSomething = [&](ValueSetID written) {
            Vector<uint32_t, mostKindsToldByName + 1> kinds;
            m_sets.forEachMemberInLine(written, [&](ValueFlowObjectID object) {
                uint32_t kind = kindOfObject(object);
                if (kinds.size() <= mostKindsToldByName && !kinds.contains(kind))
                    kinds.append(kind);
            });
            return kinds.size() <= mostKindsToldByName;
        };
        // Sorted by name.
        Vector<std::pair<uint32_t, ValueSetID>> toldByName;
        {
            Vector<std::pair<uint32_t, ValueSetID>> writtenUnderNames;
            writtenUnderNames.reserveInitialCapacity(writtenUnderKey.size());
            for (auto& [key, written] : writtenUnderKey)
                writtenUnderNames.append({ key, written });
            parallelSort(m_loops, writtenUnderNames.mutableSpan(), [](const auto& first, const auto& second) {
                return first.first < second.first;
            });
            toldByName = findIndicesInParallel(m_loops, writtenUnderNames.size(), [&](unsigned index) {
                return tellsSomething(writtenUnderNames[index].second);
            }).map([&](uint32_t index) {
                return writtenUnderNames[index];
            });
        }
        struct GroupOfField {
            uint64_t field;
            uint32_t group;

            constexpr auto operator<=>(const GroupOfField&) const = default;
        };
        constexpr uint32_t groupsPerShare = 1 << 6;
        uint32_t numberOfSharesOfGroups = (groups.size() + groupsPerShare - 1) / groupsPerShare;
        Vector<Vector<LayoutID>> layoutsOfGroupsInShare(numberOfSharesOfGroups);
        Vector<Vector<GroupOfField>> groupsOfFieldsInShare(numberOfSharesOfGroups);
        Vector<Vector<FoundStep>> arrivalsAtReadsInShare(numberOfSharesOfGroups);
        forEachIndexInParallel(m_loops, numberOfSharesOfGroups, [&](unsigned share, unsigned worker) {
            ValueSetMaker& makerOfWorker = *m_makers[worker];
            LayoutsWithoutRepeats& layouts = layoutsOfWorker[worker];
            for (uint32_t index = share * groupsPerShare; index < std::min<uint32_t>(groups.size(), (share + 1) * groupsPerShare); ++index) {
                Group& group = groups[index];
                const ReadInGroup& first = readsByGroup[group.firstRead];
                layouts.begin(numberOfLayouts);
                ValueSetID writtenOnClosures = ValueSets::nothing;
                if (LayoutSetID expected { first.layoutsExpected }) {
                    for (LayoutID layout : m_input.program->types.at(expected).completeMembers())
                        layouts.add(layout);
                }
                m_sets.forEachMemberInLine(ValueSetID(first.visibleReceivers), [&](ValueFlowObjectID receiver) {
                    if (LayoutID layout = m_input.universe->layoutOf(receiver))
                        layouts.add(layout);
                    else if (isClosureOfLibrary(receiver)) {
                        if (auto found = fields.find(fieldOfClosure(first.keyOnSide, receiver)); found != fields.end())
                            writtenOnClosures = makerOfWorker.unite(writtenOnClosures, found->value.written);
                        ValueSetID sources = copiedOntoClosure.get(receiver.index());
                        if (ValueSets::isComplement(sources))
                            return;
                        m_sets.forEachMember(sources, [&](ValueFlowObjectID source) {
                            LayoutID layout = m_input.universe->layoutOf(source);
                            if (auto found = layout ? fields.find(fieldOf(first.keyOnSide, layout)) : fields.end(); found != fields.end())
                                writtenOnClosures = makerOfWorker.unite(writtenOnClosures, found->value.written);
                        });
                    }
                });
                AOT::sortInLine(layouts.list, { }, [](LayoutID layout) { return layout.index(); });
                group.share = share;
                group.firstLayout = layoutsOfGroupsInShare[share].size();
                group.numberOfLayouts = layouts.list.size();
                layoutsOfGroupsInShare[share].appendVector(layouts.list);
                for (LayoutID layout : layouts.list) {
                    uint64_t field = fieldOf(first.keyOnSide, layout);
                    groupsOfFieldsInShare[share].append({ field, index });
                    if (auto found = fields.find(field); found != fields.end())
                        group.written = makerOfWorker.unite(group.written, found->value.written);
                }
                group.arrived = group.written;
                for (uint32_t read = group.firstRead; read < group.endOfReads; ++read) {
                    FoundStep step { Found::Arrival };
                    const Read& member = reads[readsByGroup[read].read];
                    ValueSetID written = makerOfWorker.unite(group.written, writtenOnClosures);
                    if (findArrival(ValueFlowNodeID(member.result), written, worker, step))
                        arrivalsAtReadsInShare[share].append(step);
                }
            }
        });
        ParallelArray<GroupOfField> groupsOfFields = ParallelArray<GroupOfField>::concatenationOf(m_loops, WTF::move(groupsOfFieldsInShare));
        parallelSort(m_loops, groupsOfFields.mutableSpan(), std::less<GroupOfField> { });
        // A value that has already arrived at a group's reads changes nothing when it arrives again.
        auto arriveAtReadsOf = [&](Group& group, ValueSetID come) {
            ValueSetID arrived = maker.unite(group.arrived, come);
            if (arrived == group.arrived)
                return;
            group.arrived = arrived;
            for (uint32_t index = group.firstRead; index < group.endOfReads; ++index)
                arriveAt(ValueFlowNodeID(reads[readsByGroup[index].read].result), come);
        };
        for (const Vector<FoundStep>& arrivals : arrivalsAtReadsInShare) {
            for (const FoundStep& arrival : arrivals)
                doArrival(arrival);
        }
        auto arriveAtReadsOfField = [&](uint64_t key, Field& field) {
            if (field.firstGroup == UINT32_MAX) {
                std::span<const GroupOfField> all = groupsOfFields.span();
                field.firstGroup = std::ranges::lower_bound(all, key, { }, &GroupOfField::field) - all.begin();
                field.endOfGroups = field.firstGroup;
                while (field.endOfGroups < all.size() && all[field.endOfGroups].field == key)
                    ++field.endOfGroups;
            }
            for (uint32_t index = field.firstGroup; index < field.endOfGroups; ++index)
                arriveAtReadsOf(groups[groupsOfFields[index].group], field.written);
            for (uint32_t result : field.resultsOfLaterReads)
                arriveAt(ValueFlowNodeID(result), field.written);
        };
        // What a field held when a read started reading it has arrived at the read, and so has everything stored into it since.
        HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> fieldsOfLaterReads;
        auto readsFieldSinceTheStart = [&](uint32_t index, LayoutID layout) {
            const Group& group = groups[groupOfRead[index]];
            return std::ranges::binary_search(layoutsOfGroupsInShare[group.share].span().subspan(group.firstLayout, group.numberOfLayouts), layout.index(), { }, [](LayoutID member) { return member.index(); });
        };
        auto readsField = [&](uint32_t index, LayoutID layout) {
            return readsFieldSinceTheStart(index, layout) || fieldsOfLaterReads.contains(static_cast<uint64_t>(index) << 32 | layout.index());
        };
        auto readFields = [&](uint32_t index, std::span<const LayoutID> layouts) {
            const Read& read = reads[index];
            ValueSetID written = ValueSets::nothing;
            for (LayoutID layout : layouts) {
                if (readsFieldSinceTheStart(index, layout) || !fieldsOfLaterReads.add(static_cast<uint64_t>(index) << 32 | layout.index()).isNewEntry)
                    continue;
                Field& field = fields.ensure(fieldOf(read.keyOnSide, layout), [] { return Field(); }).iterator->value;
                field.resultsOfLaterReads.append(read.result);
                written = maker.unite(written, field.written);
            }
            arriveAt(ValueFlowNodeID(read.result), written);
        };
        m_stepTimes.didFinish("behind the rounds: covered: the fields that reads read"_s);
        // What is stored under a read's name flows on from the read's result only within the read's body: each body independently, on all threads.
        Vector<uint32_t> listOfZerosByNode = makeListOfZeros(numberOfNodes);
        {
            Vector<ValueFlowNodeSet> written = collectInParallel<ValueFlowNodeSet>(m_loops, toldByName.size(), [&](unsigned index, Vector<ValueFlowNodeSet>& found) {
                forEachResultOfReadFromUnknownObject(toldByName[index].first, [&](ValueFlowNodeID result) {
                    found.append({ result, toldByName[index].second });
                });
            });
            parallelSort(m_loops, written.mutableSpan(), [](const ValueFlowNodeSet& first, const ValueFlowNodeSet& second) {
                return first.node.index() < second.node.index();
            });
            Vector<uint32_t> firstInBody = findIndicesInParallel(m_loops, written.size(), [&](unsigned index) {
                return !index || m_input.nodes->functionOf(written[index - 1].node) != m_input.nodes->functionOf(written[index].node);
            });
            Vector<Vector<ValueFlowNodeSet>> writtenInBody(firstInBody.size());
            forEachIndexInParallel(m_loops, firstInBody.size(), [&](unsigned body, unsigned worker) {
                ValueSetMaker& makerOfWorker = *m_makers[worker];
                Vector<uint32_t, 32> toFollow;
                Vector<uint32_t, 32> nodes;
                auto isWrittenUnderNameOfRead = [&](ValueFlowNodeID to, ValueSetID come) {
                    if (!m_sets.contains(m_graph.valueOf(to), m_input.placeholders.unknown))
                        return;
                    ValueSetID seen(listOfZerosByNode[to.index()]);
                    ValueSetID after = makerOfWorker.unite(seen, makerOfWorker.intersect(come, m_graph.maskOf(to)));
                    if (after == seen)
                        return;
                    if (seen == ValueSets::nothing)
                        nodes.append(to.index());
                    listOfZerosByNode[to.index()] = after.index();
                    toFollow.append(to.index());
                };
                for (uint32_t index = firstInBody[body]; index < (body + 1 < firstInBody.size() ? firstInBody[body + 1] : written.size()); ++index)
                    isWrittenUnderNameOfRead(written[index].node, written[index].set);
                FunctionID function = m_input.nodes->functionOf(written[firstInBody[body]].node);
                bool isNodeOfBody = function && function.index() < m_input.program->functions.size();
                uint32_t firstNodeOfBody = isNodeOfBody ? m_input.nodes->firstNodeOf(function) : 0;
                uint32_t endOfNodesOfBody = isNodeOfBody ? m_input.nodes->endOfNodesOf(function) : 0;
                while (!toFollow.isEmpty()) {
                    ValueFlowNodeID from(toFollow.takeLast());
                    ValueSetID come(listOfZerosByNode[from.index()]);
                    m_graph.forEachSuccessor(from, [&](ValueFlowNodeID to) {
                        if (to.index() >= firstNodeOfHost || to == m_input.escaped || to == m_input.lost)
                            return;
                        if (isNodeOfBody ? to.index() >= firstNodeOfBody && to.index() < endOfNodesOfBody : m_input.nodes->functionOf(to) == function)
                            isWrittenUnderNameOfRead(to, come);
                    });
                }
                AOT::sort(nodes);
                writtenInBody[body] = nodes.map([&](uint32_t node) {
                    return ValueFlowNodeSet { ValueFlowNodeID(node), ValueSetID(std::exchange(listOfZerosByNode[node], 0u)) };
                });
            });
            m_writtenUnderNamesOfReads = ParallelArray<ValueFlowNodeSet>::concatenationOf(m_loops, WTF::move(writtenInBody));
        }
        // Whether a call has handed its operands to a function hangs on which came first, the call or the escape of the function. So a call hands what its operands are seen to hold to every function that its callee is seen to be.
        AOT_THRESHOLD(size_t, mostCalleesOfOneCallFollowed, 512);
        Vector<uint32_t> calls = collectInParallel<uint32_t>(m_loops, m_input.statements.size(), [&](unsigned index, Vector<uint32_t>& found) {
            const ValueFlowStatement& statement = m_input.statements[index];
            if (statement.kind == ValueFlowStatementKind::Operation && (statement.opcode == Opcode::Call || statement.opcode == Opcode::Construct || statement.opcode == Opcode::SuperConstruct) && isReached(statement.function))
                found.append(index);
        });
        auto seenWith = [&](const ValueFlowOperand& operand, ValueSetMaker& makerOfWorker) {
            ValueSetID value = operand.constant ? operand.constant : ValueSets::nothing;
            if (operand.node)
                value = makerOfWorker.unite(value, holdsUnknownObject(operand.node) ? seenBehindTheRoundsAt(operand.node) : m_graph.valueOf(operand.node));
            if (ValueSets::isComplement(value))
                return many;
            value = makerOfWorker.intersect(value, followed);
            return operand.maskKind == ValueFlowMaskKind::Plain ? makerOfWorker.intersect(value, operand.mask) : value;
        };
        auto seenAt = [&](const ValueFlowOperand& operand) {
            return seenWith(operand, maker);
        };
        struct Store {
            uint32_t key;
            uint32_t statement;
            std::array<bool, 2> isReadOnSide;
            uint32_t share { 0 };
            uint32_t firstLayout { 0 };
            uint32_t numberOfLayouts { 0 };
            ValueSetID writtenLast { };
            ValueSetID receiversSeenLast { };
        };
        Vector<Store> stores = collectInParallel<Store>(m_loops, m_input.statements.size(), [&](unsigned index, Vector<Store>& found) {
            const ValueFlowStatement& statement = m_input.statements[index];
            if (statement.kind != ValueFlowStatementKind::Operation || (statement.opcode != Opcode::PropSet && statement.opcode != Opcode::ElemSet) || !isReached(statement.function) || (!operandsOf(statement)[0].node && !operandsOf(statement)[0].constant))
                return;
            PropertyKeyID key;
            if (statement.opcode == Opcode::PropSet)
                key = m_input.program->at(statement.function).at(statement.site).key();
            else {
                const ValueFlowOperand& computed = operandsOf(statement)[1];
                ValueSetID keys = computed.node ? m_graph.valueOf(computed.node) : computed.constant ? computed.constant : ValueSets::nothing;
                if (!ValueSets::isComplement(keys) && m_sets.size(keys) == 1) {
                    m_sets.forEachMember(keys, [&](ValueFlowObjectID only) {
                        key = m_input.universe->keyOf(only);
                    });
                }
            }
            if (!key)
                return;
            std::array<bool, 2> isReadOnSide { readsUnderKey.contains(key.index() * 2), readsUnderKey.contains(key.index() * 2 + 1) };
            if (isReadOnSide[0] || isReadOnSide[1])
                found.append({ key.index(), index, isReadOnSide });
        });
        // The layouts that a store's receiver is expected and observed to have, with their parent layouts.
        constexpr uint32_t storesPerShare = 1 << 8;
        Vector<Vector<LayoutID>> layoutsOfStoresInShare((stores.size() + storesPerShare - 1) / storesPerShare);
        forEachIndexInParallel(m_loops, layoutsOfStoresInShare.size(), [&](unsigned share, unsigned worker) {
            LayoutsWithoutRepeats& layouts = layoutsOfWorker[worker];
            for (uint32_t index = share * storesPerShare; index < std::min<uint32_t>(stores.size(), (share + 1) * storesPerShare); ++index) {
                Store& store = stores[index];
                const ValueFlowStatement& statement = m_input.statements[store.statement];
                layouts.begin(numberOfLayouts);
                if (LayoutSetID expected = layoutsExpectedAt(statement)) {
                    for (LayoutID layout : m_input.program->types.at(expected).completeMembers())
                        addWithLayoutsAbove(layout, layouts);
                }
                // `const context = Object.create(prototype); context.response = response;`: the receiver is the object that the function created.
                const ValueFlowOperand& object = operandsOf(statement)[0];
                ValueSetID receivers = object.node ? visibleReceiversAt(object.node, *m_makers[worker]) : ValueSets::isComplement(object.constant) || m_sets.size(object.constant) > mostReceiversOfOneWriteFollowed ? ValueSets::nothing : object.constant;
                m_sets.forEachMemberInLine(receivers, [&](ValueFlowObjectID receiver) {
                    addWithLayoutsAbove(m_input.universe->layoutOf(receiver), layouts);
                });
                store.share = share;
                store.firstLayout = layoutsOfStoresInShare[share].size();
                store.numberOfLayouts = layouts.list.size();
                layoutsOfStoresInShare[share].appendVector(layouts.list);
            }
        });
        LayoutsWithoutRepeats layoutsWrittenTo;
        auto listLayoutsWrittenTo = [&](const Store& store, ValueSetID receiversSeen, LayoutsWithoutRepeats& layouts) {
            layouts.begin(numberOfLayouts);
            for (LayoutID layout : layoutsOfStoresInShare[store.share].span().subspan(store.firstLayout, store.numberOfLayouts))
                layouts.add(layout);
            if (receiversSeen != many) {
                m_sets.forEachMember(receiversSeen, [&](ValueFlowObjectID receiver) {
                    addWithLayoutsAbove(m_input.universe->layoutOf(receiver), layouts);
                });
            }
        };
        auto writeIntoField = [&](uint64_t key, ValueSetID written) {
            Field& field = fields.ensure(key, [] { return Field(); }).iterator->value;
            ValueSetID after = maker.unite(field.written, written);
            if (after == field.written)
                return;
            field.written = after;
            arriveAtReadsOfField(key, field);
        };
        auto write = [&](uint32_t index, ValueSetID written, ValueSetID receiversSeen) {
            Store& store = stores[index];
            // The fields hold what was stored into them last time.
            if (written == ValueSets::nothing || written == many || (written == store.writtenLast && receiversSeen == store.receiversSeenLast))
                return;
            store.writtenLast = written;
            store.receiversSeenLast = receiversSeen;
            listLayoutsWrittenTo(store, receiversSeen, layoutsWrittenTo);
            for (uint32_t side = 0; side < 2; ++side) {
                if (!store.isReadOnSide[side])
                    continue;
                for (LayoutID layout : layoutsWrittenTo.list)
                    writeIntoField(fieldOf(store.key * 2 + side, layout), written);
            }
        };
        auto runStore = [&](uint32_t index) {
            std::span<const ValueFlowOperand> operands = operandsOf(m_input.statements[stores[index].statement]);
            write(index, seenAt(operands[operands.size() - 1]), operands[0].node ? seenBehindTheRoundsAt(operands[0].node) : ValueSets::nothing);
        };
        HashMap<uint32_t, Vector<uint32_t, 2>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> statementsAnsweredFrom;
        BitVector answersStatements(numberOfNodes);
        // What a call does depends on what its callee is observed to be. The steps are in execution order. Only a node that holds the unknown object sees anything, and what comes from a node that does not hold it never changes.
        enum class StepOfCall : uint8_t { EnterFunction, HasEnteredFunction, ForwardOperand, ForwardResult };
        struct Step {
            StepOfCall kind;
            bool forwardsSameValueEachTime;
            uint32_t from;
            uint32_t to;
        };
        struct PlanOfCall {
            ValueSetID seenAtCallee { };
            uint32_t share { 0 };
            uint32_t firstStep { 0 };
            uint32_t numberOfSteps { 0 };
            bool hasRun { false };
            bool isFirst { false };
        };
        constexpr uint32_t callsPerShare = 1 << 8;
        uint32_t numberOfSharesOfCalls = (calls.size() + callsPerShare - 1) / callsPerShare;
        Vector<PlanOfCall> plansOfCalls(calls.size());
        Vector<Vector<Step>> stepsOfCallsInShare(numberOfSharesOfCalls + 1);
        auto seenAtCalleeOf = [&](const ValueFlowStatement& statement) {
            ValueFlowNodeID callee = operandsOf(statement)[0].node;
            return callee && holdsUnknownObject(callee) ? seenBehindTheRoundsAt(callee) : ValueSets::nothing;
        };
        PropertyKeyID keyOfBind = m_input.program->keyOf(BuiltinNames::bind);
        auto isCallOfBind = [&](const ValueFlowStatement& statement) {
            return keyOfBind && statement.opcode == Opcode::Call && operandsOf(statement).size() > 1 && m_input.program->at(statement.function).at(statement.site).calleeKey() == keyOfBind;
        };
        auto visibleFunctionsAt = [&](const ValueFlowOperand& operand, ValueSetMaker& makerOfWorker) {
            ValueSetID held = operand.node ? m_graph.valueOf(operand.node) : operand.constant ? operand.constant : ValueSets::nothing;
            if (ValueSets::isComplement(held) || m_sets.contains(held, m_input.placeholders.unknown))
                return ValueSets::nothing;
            return makerOfWorker.intersect(held, m_input.placeholders.closures);
        };
        // The bound functions, each with the call that created it.
        HashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> bindStatements;
        for (uint32_t index : calls) {
            const ValueFlowStatement& statement = m_input.statements[index];
            if (!isCallOfBind(statement) || !statement.result || ValueSets::isComplement(m_graph.valueOf(statement.result)))
                continue;
            m_sets.forEachMember(maker.intersect(m_graph.valueOf(statement.result), m_input.universe->instancesOf(BuiltinClasses::BoundFunction)), [&](ValueFlowObjectID bound) {
                const ValueFlowSite* site = m_input.universe->siteOf(bound);
                if (site && site->value == statement.site && m_input.universe->functionOf(bound) == statement.function)
                    bindStatements.add(bound.index(), index);
            });
        }
        auto planCall = [&](const ValueFlowStatement& statement, PlanOfCall& plan, uint32_t share, Vector<Step>& steps, ValueSetMaker& makerOfWorker) {
            PlanOfCall planBefore = std::exchange(plan, { seenAtCalleeOf(statement), share, static_cast<uint32_t>(steps.size()) });
            // n calls of n callees: a walk over the steps for each callee would make this a cube.
            UncheckedKeyHashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> functionsEntered;
            bool hasNotedPlanBefore = false;
            size_t nextStepToNote = plan.firstStep;
            auto noteStep = [&](const Step& step) {
                if (step.kind == StepOfCall::EnterFunction || step.kind == StepOfCall::HasEnteredFunction)
                    functionsEntered.add(step.from);
            };
            auto hasEntered = [&](FunctionID function) {
                if (!std::exchange(hasNotedPlanBefore, true)) {
                    for (uint32_t step = 0; planBefore.hasRun && step < planBefore.numberOfSteps; ++step)
                        noteStep(stepsOfCallsInShare[planBefore.share][planBefore.firstStep + step]);
                }
                for (; nextStepToNote < steps.size(); ++nextStepToNote)
                    noteStep(steps[nextStepToNote]);
                return functionsEntered.contains(function.index());
            };
            std::span<const ValueFlowOperand> operands = operandsOf(statement);
            ValueSetID callees = seenWith(operands[0], makerOfWorker);
            if (callees != many && operands[0].node) {
                ValueSetID held = m_graph.valueOf(operands[0].node);
                if (!ValueSets::isComplement(held))
                    callees = makerOfWorker.unite(callees, makerOfWorker.intersect(makerOfWorker.intersect(held, m_coverings[0].allButCovered), m_input.placeholders.closures));
            } else if (callees != many && operands[0].constant)
                callees = makerOfWorker.unite(callees, makerOfWorker.intersect(operands[0].constant, m_input.placeholders.closures));
            auto forwardOperand = [&](unsigned operand, ValueFlowNodeID parameter) {
                if (holdsUnknownObject(parameter))
                    steps.append({ StepOfCall::ForwardOperand, !operands[operand].node || !holdsUnknownObject(operands[operand].node), operand, parameter.index() });
            };
            // `handler.bind(receiver, first)`: binding a visible function passes it the receiver and the leading operands of every future call.
            if (isCallOfBind(statement)) {
                m_sets.forEachMember(visibleFunctionsAt(operands[1], makerOfWorker), [&](ValueFlowObjectID closure) {
                    FunctionID function = m_input.universe->closureFunction(closure);
                    if (!function || !isReached(function) || !m_input.program->at(function).hasBody())
                        return;
                    const Function& target = m_input.program->at(function);
                    for (unsigned argument = 0; argument < target.numberOfPositionalParameters() && argument + 3 < operands.size(); ++argument)
                        forwardOperand(argument + 3, m_input.nodes->entryParameter(function, target.firstParameterIndex() + argument));
                    if (std::optional<unsigned> thisParameter = target.thisParameterIndex(); thisParameter && operands.size() > 2)
                        forwardOperand(2, m_input.nodes->entryParameter(function, *thisParameter));
                });
                plan.numberOfSteps = steps.size() - plan.firstStep;
            }
            if (callees == ValueSets::nothing || callees == many || m_sets.size(callees) > mostCalleesOfOneCallFollowed)
                return;
            m_sets.forEachMemberInLine(callees, [&](ValueFlowObjectID closure) {
                FunctionID function = m_input.universe->closureFunction(closure);
                unsigned numberOfBoundOperands = 0;
                bool isBound = false;
                if (auto bound = function ? bindStatements.end() : bindStatements.find(closure.index()); bound != bindStatements.end()) {
                    // The call's operands come after the bound ones.
                    std::span<const ValueFlowOperand> operandsOfBind = operandsOf(m_input.statements[bound->value]);
                    ValueSetID targets = visibleFunctionsAt(operandsOfBind[1], makerOfWorker);
                    if (ValueFlowObjectID only = m_sets.onlyMember(targets)) {
                        function = m_input.universe->closureFunction(only);
                        numberOfBoundOperands = operandsOfBind.size() > 3 ? operandsOfBind.size() - 3 : 0;
                        isBound = true;
                    }
                }
                if (!function || !isReached(function) || !m_input.program->at(function).hasBody())
                    return;
                const Function& callee = m_input.program->at(function);
                if (!callee.results.isEmpty())
                    steps.append({ hasEntered(function) ? StepOfCall::HasEnteredFunction : StepOfCall::EnterFunction, true, function.index(), m_input.nodes->result(function, 0).index() });
                unsigned first = callee.firstParameterIndex() + numberOfBoundOperands;
                for (unsigned argument = 0; argument + numberOfBoundOperands < callee.numberOfPositionalParameters() && argument + 2 < operands.size(); ++argument)
                    forwardOperand(argument + 2, m_input.nodes->entryParameter(function, first + argument));
                if (std::optional<unsigned> thisParameter = callee.thisParameterIndex(); thisParameter && statement.opcode == Opcode::Call && !isBound)
                    forwardOperand(1, m_input.nodes->entryParameter(function, *thisParameter));
                if (statement.result && !callee.results.isEmpty() && holdsUnknownObject(statement.result)) {
                    ValueFlowNodeID answered = m_input.nodes->result(function, 0);
                    steps.append({ StepOfCall::ForwardResult, !holdsUnknownObject(answered), answered.index(), statement.result.index() });
                }
            });
            plan.numberOfSteps = steps.size() - plan.firstStep;
        };
        forEachIndexInParallel(m_loops, numberOfSharesOfCalls, [&](unsigned share, unsigned worker) {
            for (uint32_t index = share * callsPerShare; index < std::min<uint32_t>(calls.size(), (share + 1) * callsPerShare); ++index) {
                planCall(m_input.statements[calls[index]], plansOfCalls[index], share, stepsOfCallsInShare[share], *m_makers[worker]);
                plansOfCalls[index].isFirst = true;
            }
        });
        // The functions that a call's first plan enters are among the uses of the result nodes. They count once that plan has run.
        BitVector hasFirstPlanRun(calls.size());
        struct SeenAtOperand {
            ValueSetID value;
            uint64_t numberOfChangesSeen { UINT64_MAX };
        };
        Vector<SeenAtOperand, 8> seenAtOperands;
        auto stepsOf = [&](const PlanOfCall& plan) {
            return stepsOfCallsInShare[plan.share].span().subspan(plan.firstStep, plan.numberOfSteps);
        };
        auto enterFunction = [&](uint32_t index, const Step& step) {
            if (plansOfCalls[index].isFirst) {
                hasFirstPlanRun.quickSet(index);
                return;
            }
            statementsAnsweredFrom.ensure(step.to, [] { return Vector<uint32_t, 2>(); }).iterator->value.append(calls[index]);
            answersStatements.quickSet(step.to);
        };
        auto runStepsOfCall = [&](uint32_t index, uint32_t firstStep, bool hasRun) {
            std::span<const ValueFlowOperand> operands = operandsOf(m_input.statements[calls[index]]);
            seenAtOperands.fill({ }, operands.size());
            for (const Step& step : stepsOf(plansOfCalls[index]).subspan(firstStep)) {
                if (hasRun && step.forwardsSameValueEachTime)
                    continue;
                switch (step.kind) {
                case StepOfCall::EnterFunction:
                    enterFunction(index, step);
                    break;
                case StepOfCall::HasEnteredFunction:
                    break;
                case StepOfCall::ForwardOperand:
                    if (canSeeMoreAt(ValueFlowNodeID(step.to))) {
                        SeenAtOperand& seen = seenAtOperands[step.from];
                        if (seen.numberOfChangesSeen != numberOfChangesSeen)
                            seen = { seenAt(operands[step.from]), numberOfChangesSeen };
                        arriveAt(ValueFlowNodeID(step.to), seen.value);
                    }
                    break;
                case StepOfCall::ForwardResult:
                    if (canSeeMoreAt(ValueFlowNodeID(step.to))) {
                        ValueFlowNodeID answered(step.from);
                        ValueSetID value = step.forwardsSameValueEachTime ? m_graph.valueOf(answered) : seenBehindTheRoundsAt(answered);
                        arriveAt(ValueFlowNodeID(step.to), ValueSets::isComplement(value) ? many : maker.intersect(value, followed));
                    }
                    break;
                }
            }
        };
        auto runCall = [&](uint32_t index) {
            const ValueFlowStatement& statement = m_input.statements[calls[index]];
            PlanOfCall& plan = plansOfCalls[index];
            if (plan.seenAtCallee != seenAtCalleeOf(statement))
                planCall(statement, plan, numberOfSharesOfCalls, stepsOfCallsInShare[numberOfSharesOfCalls], maker);
            if (plan.numberOfSteps)
                runStepsOfCall(index, 0, std::exchange(plan.hasRun, true));
        };
        // The stores and calls that a node is an operand of, the calls it is the result of, and the reads from it, in that order, each kind in statement order.
        enum class Use : uint64_t { Store, Call, Answer, Read };
        constexpr unsigned bitsOfIndexOfUse = 30;
        RELEASE_ASSERT(std::max({ stores.size(), calls.size(), reads.size() }) < 1u << bitsOfIndexOfUse);
        auto useOf = [](ValueFlowNodeID node, Use use, uint32_t index) {
            return static_cast<uint64_t>(node.index()) << 32 | static_cast<uint64_t>(use) << bitsOfIndexOfUse | index;
        };
        Vector<uint64_t> uses = collectInParallel<uint64_t>(m_loops, stores.size() + calls.size() + reads.size(), [&](unsigned index, Vector<uint64_t>& found) {
            if (index < stores.size()) {
                std::span<const ValueFlowOperand> operands = operandsOf(m_input.statements[stores[index].statement]);
                for (ValueFlowNodeID node : { operands[0].node, operands[operands.size() - 1].node }) {
                    if (node)
                        found.append(useOf(node, Use::Store, index));
                }
                return;
            }
            index -= stores.size();
            if (index < calls.size()) {
                for (const ValueFlowOperand& operand : operandsOf(m_input.statements[calls[index]])) {
                    if (operand.node)
                        found.append(useOf(operand.node, Use::Call, index));
                }
                for (const Step& step : stepsOf(plansOfCalls[index])) {
                    if (step.kind == StepOfCall::EnterFunction && m_input.statements[calls[index]].result)
                        found.append(useOf(ValueFlowNodeID(step.to), Use::Answer, index));
                }
                return;
            }
            index -= calls.size();
            found.append(useOf(ValueFlowNodeID(reads[index].object), Use::Read, index));
        });
        parallelSort(m_loops, uses.mutableSpan(), std::less<uint64_t> { });
        Vector<uint32_t> placeAfterFirstUseOfNode = WTF::move(listOfZerosByNode);
        constexpr uint32_t usesPerShare = 1 << 12;
        forEachIndexInParallel(m_loops, (uses.size() + usesPerShare - 1) / usesPerShare, [&](unsigned share) {
            for (uint32_t index = share * usesPerShare; index < std::min<uint32_t>(uses.size(), (share + 1) * usesPerShare); ++index) {
                if (!index || uses[index - 1] >> 32 != uses[index] >> 32)
                    placeAfterFirstUseOfNode[uses[index] >> 32] = index + 1;
            }
        });
        auto placeOfFirstUseOf = [&](ValueFlowNodeID node) -> size_t {
            return placeAfterFirstUseOfNode[node.index()] ? placeAfterFirstUseOfNode[node.index()] - 1 : uses.size();
        };
        auto takeUseAt = [&](size_t& place, ValueFlowNodeID node, Use use) -> std::optional<uint32_t> {
            if (place == uses.size() || uses[place] >> bitsOfIndexOfUse != useOf(node, use, 0) >> bitsOfIndexOfUse)
                return std::nullopt;
            return static_cast<uint32_t>(uses[place++] & ((1u << bitsOfIndexOfUse) - 1));
        };
        LayoutsWithoutRepeats layoutsSeen;
        auto listLayoutsOf = [&](ValueSetID seen, LayoutsWithoutRepeats& layouts) {
            layouts.begin(numberOfLayouts);
            m_sets.forEachMemberInLine(seen, [&](ValueFlowObjectID object) {
                if (LayoutID layout = m_input.universe->layoutOf(object))
                    layouts.add(layout);
            });
        };
        // What is observed to be put into an escaped array is observed to be read from it by index.
        struct ElementsOfList {
            ValueSetID written { ValueSets::nothing };
            Vector<uint32_t, 2> resultsOfReads;
        };
        HashMap<uint32_t, ElementsOfList, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> elementsOfLists;
        HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> readsOfLists;
        AOT_THRESHOLD(size_t, maxListsFollowedPerAccess, 16);
        // More lists than an access follows count as `many`, whenever they arrived.
        auto listsAt = [&](const ValueFlowOperand& operand) {
            ValueSetID lists = seenAt(operand);
            if (lists == many)
                return many;
            lists = maker.intersect(lists, listsFollowed);
            return m_sets.size(lists) > maxListsFollowedPerAccess ? many : lists;
        };
        auto putIntoLists = [&](ValueSetID lists, ValueSetID written) {
            if (lists == many || written == ValueSets::nothing || written == many)
                return;
            // Built-in objects stored in an array say nothing about what the program creates.
            written = maker.intersect(written, ValueSets::complement(m_input.universe->builtinObjects()));
            if (written == ValueSets::nothing)
                return;
            m_sets.forEachMember(lists, [&](ValueFlowObjectID list) {
                ElementsOfList& elements = elementsOfLists.ensure(list.index(), [] { return ElementsOfList(); }).iterator->value;
                ValueSetID after = maker.unite(elements.written, written);
                if (after == elements.written)
                    return;
                elements.written = after;
                for (uint32_t result : elements.resultsOfReads)
                    arriveAt(ValueFlowNodeID(result), after);
            });
        };
        PropertyKeyID appendingKeys[] { m_input.program->keyOf(BuiltinNames::push), m_input.program->keyOf(BuiltinNames::unshift) };
        auto mayBeKeyOfOneName = [&](const ValueFlowOperand& computed) {
            ValueSetID keys = computed.node ? m_graph.valueOf(computed.node) : computed.constant ? computed.constant : ValueSets::nothing;
            return !ValueSets::isComplement(keys) && m_sets.size(keys) == 1 && m_input.universe->keyOf(m_sets.onlyMember(keys));
        };
        Vector<uint32_t> accessesOfLists = collectInParallel<uint32_t>(m_loops, m_input.statements.size(), [&](unsigned index, Vector<uint32_t>& found) {
            const ValueFlowStatement& statement = m_input.statements[index];
            if (statement.kind != ValueFlowStatementKind::Operation || !isReached(statement.function))
                return;
            std::span<const ValueFlowOperand> operands = operandsOf(statement);
            switch (statement.opcode) {
            case Opcode::ElemGet:
                if (statement.result && holdsUnknownObject(statement.result) && !mayBeKeyOfOneName(operands[1]))
                    found.append(index);
                return;
            case Opcode::ElemSet:
                if (!mayBeKeyOfOneName(operands[1]))
                    found.append(index);
                return;
            case Opcode::NewArray:
                if (statement.result && !operands.empty())
                    found.append(index);
                return;
            case Opcode::Call:
                if (PropertyKeyID key = m_input.program->at(statement.function).at(statement.site).calleeKey(); key && operands.size() > 2 && (key == appendingKeys[0] || key == appendingKeys[1]))
                    found.append(index);
                return;
            default:
                return;
            }
        });
        auto runAccessOfLists = [&](uint32_t index) {
            const ValueFlowStatement& statement = m_input.statements[index];
            std::span<const ValueFlowOperand> operands = operandsOf(statement);
            switch (statement.opcode) {
            case Opcode::ElemGet:
                if (listsAt(operands[0]) == many) {
                    arriveAt(statement.result, many);
                    return;
                }
                m_sets.forEachMember(listsAt(operands[0]), [&](ValueFlowObjectID list) {
                    if (!readsOfLists.add(static_cast<uint64_t>(index) << 32 | list.index()).isNewEntry)
                        return;
                    ElementsOfList& elements = elementsOfLists.ensure(list.index(), [] { return ElementsOfList(); }).iterator->value;
                    elements.resultsOfReads.append(statement.result.index());
                    arriveAt(statement.result, elements.written);
                });
                return;
            case Opcode::ElemSet:
                putIntoLists(listsAt(operands[0]), seenAt(operands[operands.size() - 1]));
                return;
            case Opcode::NewArray: {
                ValueSetID made = m_graph.valueOf(statement.result);
                if (ValueSets::isComplement(made))
                    return;
                made = maker.intersect(made, listsFollowed);
                for (const ValueFlowOperand& element : operands)
                    putIntoLists(made, seenAt(element));
                return;
            }
            case Opcode::Call: {
                ValueSetID lists = listsAt(operands[1]);
                for (const ValueFlowOperand& appended : operands.subspan(2))
                    putIntoLists(lists, seenAt(appended));
                return;
            }
            default:
                RELEASE_ASSERT_NOT_REACHED();
            }
        };
        Vector<uint64_t> accessesOfListsByNode;
        for (uint32_t index : accessesOfLists) {
            for (const ValueFlowOperand& operand : operandsOf(m_input.statements[index])) {
                if (operand.node && holdsUnknownObject(operand.node))
                    accessesOfListsByNode.append(static_cast<uint64_t>(operand.node.index()) << 32 | index);
            }
        }
        AOT::sort(accessesOfListsByNode);
        removeRepeatedElements(accessesOfListsByNode);
        BitVector isOperandOfAccessOfLists(numberOfNodes);
        for (uint64_t access : accessesOfListsByNode)
            isOperandOfAccessOfLists.quickSet(access >> 32);
        auto runAccessesOfListsAt = [&](ValueFlowNodeID node) {
            if (!isOperandOfAccessOfLists.quickGet(node.index()))
                return;
            for (const uint64_t* access = std::ranges::lower_bound(accessesOfListsByNode, static_cast<uint64_t>(node.index()) << 32); access != accessesOfListsByNode.end() && *access >> 32 == node.index(); ++access)
                runAccessOfLists(static_cast<uint32_t>(*access));
        };
        auto visit = [&](ValueFlowNodeID from) {
            ValueSetID come = seenBehindTheRoundsAt(from);
            runAccessesOfListsAt(from);
            size_t place = placeOfFirstUseOf(from);
            auto takeUse = [&](Use use) {
                return takeUseAt(place, from, use);
            };
            while (std::optional<uint32_t> index = takeUse(Use::Store))
                runStore(*index);
            while (std::optional<uint32_t> index = takeUse(Use::Call))
                runCall(*index);
            while (std::optional<uint32_t> index = takeUse(Use::Answer)) {
                if (hasFirstPlanRun.quickGet(*index))
                    arriveAt(m_input.statements[calls[*index]].result, come);
            }
            if (answersStatements.quickGet(from.index())) {
                for (uint32_t index : statementsAnsweredFrom.find(from.index())->value) {
                    if (m_input.statements[index].result)
                        arriveAt(m_input.statements[index].result, come);
                }
            }
            if (come != many) {
                bool hasLayouts = false;
                while (std::optional<uint32_t> index = takeUse(Use::Read)) {
                    if (!std::exchange(hasLayouts, true))
                        listLayoutsOf(come, layoutsSeen);
                    readFields(*index, layoutsSeen.list.span());
                }
            }
            m_graph.forEachSuccessor(from, [&](ValueFlowNodeID to) {
                if (to.index() < firstNodeOfHost && to != m_input.escaped && to != m_input.lost)
                    arriveAt(to, come);
            });
        };
        auto findStore = [&](uint32_t index, unsigned worker, Vector<FoundStep>& found) {
            std::span<const ValueFlowOperand> operands = operandsOf(m_input.statements[stores[index].statement]);
            const ValueFlowOperand& value = operands[operands.size() - 1];
            bool canChange = value.node && holdsUnknownObject(value.node);
            ValueSetID written = seenWith(value, *m_makers[worker]);
            if (!canChange && (written == ValueSets::nothing || written == many))
                return;
            FoundStep step { Found::Store, index };
            if (canChange) {
                step.input = value.node.index();
                step.seenAtInput = seenBehindTheRoundsAt(value.node);
            }
            if (operands[0].node) {
                step.to = operands[0].node.index();
                step.seenBefore = seenBehindTheRoundsAt(operands[0].node);
            } else
                step.seenBefore = ValueSets::nothing;
            step.come = written;
            size_t head = found.size();
            found.append(step);
            // The fields that the store writes, in write order.
            const Store& store = stores[index];
            if (written == ValueSets::nothing || written == many || (written == store.writtenLast && step.seenBefore == store.receiversSeenLast))
                return;
            LayoutsWithoutRepeats& layouts = layoutsOfWorker[worker];
            listLayoutsWrittenTo(store, step.seenBefore, layouts);
            for (uint32_t side = 0; side < 2; ++side) {
                if (!store.isReadOnSide[side])
                    continue;
                for (LayoutID layout : layouts.list)
                    found.append({ Found::FieldWritten, store.key * 2 + side, layout.index() });
            }
            found[head].numberOfFields = found.size() - head - 1;
        };
        auto doStore = [&](std::span<const FoundStep> found, size_t head) {
            const FoundStep& step = found[head];
            if (!isSeenAsBefore(step))
                runStore(step.index);
            else if (step.come != ValueSets::nothing && step.come != many) {
                if (step.to != noNode && seenAfterRounds[step.to] != step.seenBefore.index())
                    runStore(step.index);
                else if (!step.numberOfFields)
                    write(step.index, step.come, step.seenBefore);
                else {
                    Store& store = stores[step.index];
                    if (step.come == store.writtenLast && step.seenBefore == store.receiversSeenLast)
                        return;
                    store.writtenLast = step.come;
                    store.receiversSeenLast = step.seenBefore;
                    for (const FoundStep& field : found.subspan(head + 1, step.numberOfFields))
                        writeIntoField(fieldOf(field.index, LayoutID(field.input)), step.come);
                }
            }
        };
        auto findCallWithPlan = [&](uint32_t index, const PlanOfCall& plan, std::span<const Step> steps, unsigned worker, Vector<FoundStep>& found) {
            const ValueFlowStatement& statement = m_input.statements[calls[index]];
            size_t head = found.size();
            std::span<const ValueFlowOperand> operands = operandsOf(statement);
            bool calleeCanChange = operands[0].node && holdsUnknownObject(operands[0].node);
            if (!plan.numberOfSteps && !calleeCanChange)
                return;
            found.append({ Found::Call, index });
            if (calleeCanChange) {
                found[head].input = operands[0].node.index();
                found[head].seenAtInput = plan.seenAtCallee;
            }
            // An earlier step in the same batch can give this call its first callee, so doCall() must re-check an empty plan too.
            if (!plan.numberOfSteps) {
                found[head].to = 0;
                return;
            }
            found[head].come = plan.hasRun ? ValueSets::anything : ValueSets::nothing;
            Vector<ValueSetID, 8> seenAtOperandsOfCall(operands.size());
            for (uint32_t position = 0; position < steps.size(); ++position) {
                const Step& planned = steps[position];
                if (plan.hasRun && planned.forwardsSameValueEachTime)
                    continue;
                if (planned.kind == StepOfCall::HasEnteredFunction)
                    continue;
                if (planned.kind == StepOfCall::EnterFunction) {
                    found.append({ Found::EnterFunction, position });
                    continue;
                }
                if (seenAfterRounds[planned.to] == many.index())
                    continue;
                FoundStep step { Found::Forward, position };
                ValueSetID come;
                if (planned.kind == StepOfCall::ForwardOperand) {
                    ValueSetID& seen = seenAtOperandsOfCall[planned.from];
                    if (!seen)
                        seen = seenWith(operands[planned.from], *m_makers[worker]);
                    come = seen;
                    if (!planned.forwardsSameValueEachTime)
                        step.input = operands[planned.from].node.index();
                } else {
                    ValueFlowNodeID answered(planned.from);
                    ValueSetID value = planned.forwardsSameValueEachTime ? m_graph.valueOf(answered) : seenBehindTheRoundsAt(answered);
                    come = ValueSets::isComplement(value) ? many : m_makers[worker]->intersect(value, followed);
                    if (!planned.forwardsSameValueEachTime)
                        step.input = planned.from;
                }
                if (step.input != noNode)
                    step.seenAtInput = seenBehindTheRoundsAt(ValueFlowNodeID(step.input));
                if (findArrival(ValueFlowNodeID(planned.to), come, worker, step) || step.input != noNode)
                    found.append(step);
            }
            found[head].to = found.size() - head - 1;
        };
        // A call whose callees have changed is planned anew here too. The plan depends on the callees and on the plan before it, and is taken over if both are as they were.
        // After the head come the plan before, the steps of the new plan, and what the call does by the new plan.
        Vector<Vector<Step>> stepsPlannedByWorker(m_makers.size());
        auto findCall = [&](uint32_t index, unsigned worker, Vector<FoundStep>& found) {
            const ValueFlowStatement& statement = m_input.statements[calls[index]];
            PlanOfCall plan = plansOfCalls[index];
            if (plan.seenAtCallee == seenAtCalleeOf(statement)) {
                findCallWithPlan(index, plan, stepsOf(plan), worker, found);
                return;
            }
            size_t head = found.size();
            found.append({ Found::CallPlannedAnew, index, operandsOf(statement)[0].node.index() });
            found.append({ Found::PlanBefore, plan.share, plan.firstStep, plan.seenAtCallee, plan.numberOfSteps });
            found.last().numberOfFields = plan.hasRun;
            Vector<Step>& steps = stepsPlannedByWorker[worker];
            steps.shrink(0);
            planCall(statement, plan, numberOfSharesOfCalls, steps, *m_makers[worker]);
            found[head].seenAtInput = plan.seenAtCallee;
            found[head].numberOfFields = steps.size();
            for (const Step& step : steps) {
                found.append({ Found::StepPlanned, step.from, static_cast<uint32_t>(step.kind), { }, step.to });
                found.last().numberOfFields = step.forwardsSameValueEachTime;
            }
            findCallWithPlan(index, plan, steps.span(), worker, found);
            found[head].to = found.size() - head - 1;
        };
        // The steps are contiguous; the values seen at their nodes are not.
        auto prefetchSeenValuesFor = [&](std::span<const FoundStep> found, size_t place) {
            constexpr size_t prefetchDistance = 16;
            if (place + prefetchDistance >= found.size())
                return;
            const FoundStep& step = found[place + prefetchDistance];
            if (step.kind == Found::Visit)
                __builtin_prefetch(&seenAfterRounds[step.index]);
            else if (step.kind == Found::Store || step.kind == Found::CallPlannedAnew || step.kind == Found::Call || step.kind == Found::Forward || step.kind == Found::Arrival) {
                if (step.input != noNode)
                    __builtin_prefetch(&seenAfterRounds[step.input]);
                if (step.kind != Found::CallPlannedAnew && step.kind != Found::Call && step.to != noNode)
                    __builtin_prefetch(&seenAfterRounds[step.to]);
            }
        };
        auto doCall = [&](std::span<const FoundStep> found, size_t head) {
            uint32_t index = found[head].index;
            if (!isSeenAsBefore(found[head])) {
                runCall(index);
                return;
            }
            if (!plansOfCalls[index].numberOfSteps)
                return;
            bool hasRun = found[head].come == ValueSets::anything || std::exchange(plansOfCalls[index].hasRun, true);
            for (size_t place = head + 1; place <= head + found[head].to; ++place) {
                const FoundStep& step = found[place];
                prefetchSeenValuesFor(found, place);
                if (step.kind == Found::EnterFunction) {
                    if (!hasRun)
                        enterFunction(index, stepsOf(plansOfCalls[index])[step.index]);
                    continue;
                }
                if (!isSeenAsBefore(step)) {
                    runStepsOfCall(index, step.index, hasRun);
                    return;
                }
                if (step.to != noNode)
                    doArrival(step);
            }
        };
        auto findVisit = [&](ValueFlowNodeID from, unsigned worker, Vector<FoundStep>& found) {
            ValueSetID come = seenBehindTheRoundsAt(from);
            size_t head = found.size();
            found.append({ Found::Visit, from.index() });
            found[head].seenAtInput = come;
            if (isOperandOfAccessOfLists.quickGet(from.index()))
                found.append({ Found::AccessesOfLists, from.index() });
            size_t place = placeOfFirstUseOf(from);
            while (std::optional<uint32_t> index = takeUseAt(place, from, Use::Store))
                findStore(*index, worker, found);
            while (std::optional<uint32_t> index = takeUseAt(place, from, Use::Call))
                findCall(*index, worker, found);
            auto findArrivalOf = [&](ValueFlowNodeID to) {
                FoundStep step { Found::Arrival };
                if (findArrival(to, come, worker, step))
                    found.append(step);
            };
            while (std::optional<uint32_t> index = takeUseAt(place, from, Use::Answer)) {
                if (hasFirstPlanRun.quickGet(*index))
                    findArrivalOf(m_input.statements[calls[*index]].result);
            }
            uint32_t numberOfCallsWithResults = 0;
            if (answersStatements.quickGet(from.index())) {
                const Vector<uint32_t, 2>& answered = std::as_const(statementsAnsweredFrom).find(from.index())->value;
                numberOfCallsWithResults = answered.size();
                for (uint32_t index : answered) {
                    if (m_input.statements[index].result)
                        findArrivalOf(m_input.statements[index].result);
                }
            }
            found.append({ Found::ResultsForwarded, numberOfCallsWithResults });
            if (come != many) {
                bool hasLayouts = false;
                LayoutsWithoutRepeats& layouts = layoutsOfWorker[worker];
                while (std::optional<uint32_t> index = takeUseAt(place, from, Use::Read)) {
                    if (!std::exchange(hasLayouts, true))
                        listLayoutsOf(come, layouts);
                    for (LayoutID layout : layouts.list) {
                        if (!readsField(*index, layout))
                            found.append({ Found::Read, *index, layout.index() });
                    }
                }
            }
            m_graph.forEachSuccessor(from, [&](ValueFlowNodeID to) {
                if (to.index() < firstNodeOfHost && to != m_input.escaped && to != m_input.lost)
                    findArrivalOf(to);
            });
            found[head].to = found.size() - head - 1;
        };
        auto doAll = [&](std::span<const FoundStep> found) {
            ValueFlowNodeID from;
            ValueSetID come;
            for (size_t place = 0; place < found.size(); ++place) {
                const FoundStep& step = found[place];
                prefetchSeenValuesFor(found, place);
                switch (step.kind) {
                case Found::Visit:
                    from = ValueFlowNodeID(step.index);
                    come = step.seenAtInput;
                    if (seenBehindTheRoundsAt(from) != come) {
                        visit(from);
                        place += step.to;
                    }
                    break;
                case Found::Store:
                    doStore(found, place);
                    place += step.numberOfFields;
                    break;
                case Found::CallPlannedAnew: {
                    PlanOfCall& plan = plansOfCalls[step.index];
                    const FoundStep& before = found[place + 1];
                    if (!isSeenAsBefore(step) || plan.share != before.index || plan.firstStep != before.input || plan.seenAtCallee != before.seenAtInput || plan.numberOfSteps != before.to || plan.hasRun != !!before.numberOfFields) {
                        runCall(step.index);
                        place += step.to;
                        break;
                    }
                    Vector<Step>& steps = stepsOfCallsInShare[numberOfSharesOfCalls];
                    plan = { step.seenAtInput, numberOfSharesOfCalls, static_cast<uint32_t>(steps.size()), step.numberOfFields };
                    for (const FoundStep& planned : found.subspan(place + 2, step.numberOfFields))
                        steps.append({ static_cast<StepOfCall>(planned.input), !!planned.numberOfFields, planned.index, planned.to });
                    place += 1 + step.numberOfFields;
                    break;
                }
                case Found::Call:
                    doCall(found, place);
                    place += step.to;
                    break;
                case Found::Arrival:
                    doArrival(step);
                    break;
                case Found::ResultsForwarded:
                    if (answersStatements.quickGet(from.index())) {
                        for (uint32_t index : statementsAnsweredFrom.find(from.index())->value.span().subspan(step.index)) {
                            if (m_input.statements[index].result)
                                arriveAt(m_input.statements[index].result, come);
                        }
                    }
                    break;
                case Found::Read:
                    readFields(step.index, std::array { LayoutID(step.input) });
                    break;
                case Found::AccessesOfLists:
                    runAccessesOfListsAt(ValueFlowNodeID(step.index));
                    break;
                case Found::FieldWritten:
                case Found::PlanBefore:
                case Found::StepPlanned:
                case Found::EnterFunction:
                case Found::Forward:
                    RELEASE_ASSERT_NOT_REACHED();
                }
            }
        };
        AOT_THRESHOLD(uint32_t, minFoundForAllThreads, 256);
        AOT_THRESHOLD(uint32_t, maxFoundAtOnce, 1 << 15);
        Vector<Vector<FoundStep>> foundInShare;
        auto findOnAllThreadsAndDo = [&](uint32_t count, const Invocable<void(uint32_t, unsigned, Vector<FoundStep>&)> auto& find, const Invocable<void(uint32_t)> auto& findAndDo) {
            if (count < minFoundForAllThreads || m_makers.size() == 1) {
                for (uint32_t index = 0; index < count; ++index)
                    findAndDo(index);
                return;
            }
            constexpr uint32_t foundPerShare = 32;
            for (uint32_t first = 0; first < count; first += maxFoundAtOnce) {
                uint32_t end = std::min(count, first + maxFoundAtOnce);
                uint32_t numberOfSharesFound = (end - first + foundPerShare - 1) / foundPerShare;
                if (foundInShare.size() < numberOfSharesFound)
                    foundInShare.grow(numberOfSharesFound);
                forEachIndexInParallel(m_loops, numberOfSharesFound, [&](unsigned share, unsigned worker) {
                    foundInShare[share].shrink(0);
                    for (uint32_t index = first + share * foundPerShare; index < std::min(end, first + (share + 1) * foundPerShare); ++index)
                        find(index, worker, foundInShare[share]);
                });
                for (uint32_t share = 0; share < numberOfSharesFound; ++share)
                    doAll(foundInShare[share].span());
            }
        };
        m_stepTimes.didFinish("behind the rounds: covered: stores, calls and reads listed by node"_s);
        findOnAllThreadsAndDo(calls.size(), findCall, runCall);
        // The class above a class is seen to be the one that its declaration names. Where it names none, that is what the definition of the class is seen to extend.
        HashMap<uint32_t, Vector<uint32_t, 1>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> definitionsOfClasses;
        for (uint32_t index : findIndicesInParallel(m_loops, m_input.statements.size(), [&](unsigned index) {
            const ValueFlowStatement& statement = m_input.statements[index];
            if (statement.kind != ValueFlowStatementKind::Operation || statement.opcode != Opcode::NewClass || !isReached(statement.function))
                return false;
            const Value& definition = m_input.program->at(statement.function).at(statement.site);
            return definition.heritageOperand() && !m_input.program->at(definition.layout()).base;
        })) {
            const ValueFlowStatement& statement = m_input.statements[index];
            definitionsOfClasses.ensure(m_input.program->at(statement.function).at(statement.site).layout().index(), [] { return Vector<uint32_t, 1>(); }).iterator->value.append(index);
        }
        for (uint32_t index : findIndicesInParallel(m_loops, m_input.statements.size(), [&](unsigned index) {
            const ValueFlowStatement& statement = m_input.statements[index];
            return statement.kind == ValueFlowStatementKind::Operation && statement.opcode == Opcode::GetSuperBase && statement.result && isReached(statement.function);
        })) {
            const ValueFlowStatement& statement = m_input.statements[index];
            const Function& function = m_input.program->at(statement.function);
            if (function.at(statement.site).homeKind() != HomeObjectKind::Object || !function.constructedClass)
                continue;
            LayoutID base = m_input.program->at(function.constructedClass).base;
            if (FunctionID constructor = base ? m_input.program->at(base).constructor : FunctionID())
                arriveAt(statement.result, maker.intersect(maker.of(m_input.universe->closureOf(constructor)), followed));
            auto definitions = definitionsOfClasses.find(function.constructedClass.index());
            if (definitions == definitionsOfClasses.end())
                continue;
            for (uint32_t definition : definitions->value) {
                const ValueFlowOperand& heritage = operandsOf(m_input.statements[definition])[OperandIndex::NewClass::heritage];
                arriveAt(statement.result, seenAt(heritage));
                if (!heritage.node || !holdsUnknownObject(heritage.node))
                    continue;
                statementsAnsweredFrom.ensure(heritage.node.index(), [] { return Vector<uint32_t, 2>(); }).iterator->value.append(index);
                answersStatements.quickSet(heritage.node.index());
            }
        }
        // What stands in sight in one node and goes on to a node that holds the unknown object is covered there. It has come all the same.
        Vector<Vector<FoundStep>> arrivalsFromVisibleNodesPerShare((firstNodeOfHost + nodesPerShare - 1) / nodesPerShare);
        forEachIndexInParallel(m_loops, arrivalsFromVisibleNodesPerShare.size(), [&](unsigned share, unsigned worker) {
            ValueSetMaker& makerOfWorker = *m_makers[worker];
            for (uint32_t index = share * nodesPerShare; index < std::min(firstNodeOfHost, (share + 1) * nodesPerShare); ++index) {
                ValueFlowNodeID node(index);
                if (node == m_input.escaped || node == m_input.lost || !m_sets.contains(m_graph.valueOf(node), m_input.placeholders.unknown))
                    continue;
                ValueSetID come = ValueSets::nothing;
                m_graph.forEachPredecessor(node, [&](ValueFlowNodeID from) {
                    if (from == m_input.escaped || from == m_input.lost)
                        return;
                    if (!m_graph.isConstant(from) && from.index() >= firstNodeOfHost) {
                        // What a built-in object has held under a name since its creation.
                        const ValueFlowNodeKey* key = from.index() >= m_input.numberOfNodes ? keyOfMadeNode(from) : nullptr;
                        if (key && (key->kind == ValueFlowMadeNodeKind::Lookup || key->kind == ValueFlowMadeNodeKind::Read) && !ValueSets::isComplement(key->set) && !ValueSets::isComplement(m_graph.valueOf(from)) && makerOfWorker.intersect(key->set, ValueSets::complement(m_input.universe->builtinObjects())) == ValueSets::nothing)
                            come = makerOfWorker.unite(come, makerOfWorker.intersect(m_graph.valueOf(from), functionsOfEngine));
                        return;
                    }
                    ValueSetID value = m_graph.valueOf(from);
                    if (!ValueSets::isComplement(value) && !m_sets.contains(value, m_input.placeholders.unknown))
                        come = makerOfWorker.unite(come, makerOfWorker.intersect(value, followed));
                });
                FoundStep step { Found::Arrival };
                if (findArrival(node, come, worker, step))
                    arrivalsFromVisibleNodesPerShare[share].append(step);
            }
        });
        for (const Vector<FoundStep>& arrivalsFromVisibleNodes : arrivalsFromVisibleNodesPerShare) {
            for (const FoundStep& arrival : arrivalsFromVisibleNodes)
                doArrival(arrival);
        }
        // A declared function type lists the argument types of a callback. A closure that reaches a parameter of that type is seen to receive the escaped objects of those layouts.
        AOT_THRESHOLD(size_t, maxLayoutsPerCallbackArgument, 4);
        struct CallbackArgument {
            uint32_t callback;
            uint32_t index;
            ValueSetID objects;
        };
        Vector<CallbackArgument> callbackArguments;
        {
            std::span<const Program::DeclaredCallback> callbacks = m_input.program->declaredCallbacks.span();
            HashMap<uint32_t, Vector<ValueFlowObjectID>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> objectsByLayout;
            auto declaredLayoutsOf = [&](const DeclaredType& declared) -> std::span<const LayoutID> {
                LayoutSetID layouts = m_input.program->at(declared.expected().predicted()).layouts;
                if (!layouts)
                    return { };
                std::span<const LayoutID> members = m_input.program->at(layouts).completeMembers();
                return members.size() <= maxLayoutsPerCallbackArgument ? members : std::span<const LayoutID>();
            };
            for (const Program::DeclaredCallback& callback : callbacks) {
                if (!isReached(callback.function))
                    continue;
                for (const DeclaredType& declared : m_input.program->at(callback.functionType).parameters) {
                    for (LayoutID layout : declaredLayoutsOf(declared))
                        objectsByLayout.add(layout.index(), Vector<ValueFlowObjectID>());
                }
            }
            if (!objectsByLayout.isEmpty()) {
                m_sets.forEachMember(followed, [&](ValueFlowObjectID object) {
                    LayoutID layout = m_input.universe->layoutOf(object);
                    if (auto found = layout ? objectsByLayout.find(layout.index()) : objectsByLayout.end(); found != objectsByLayout.end())
                        found->value.append(object);
                });
            }
            for (uint32_t index = 0; index < callbacks.size(); ++index) {
                if (!isReached(callbacks[index].function))
                    continue;
                const Vector<DeclaredType>& parameters = m_input.program->at(callbacks[index].functionType).parameters;
                for (uint32_t argument = 0; argument < parameters.size(); ++argument) {
                    ValueSetID objects = ValueSets::nothing;
                    for (LayoutID layout : declaredLayoutsOf(parameters[argument]))
                        objects = maker.unite(objects, maker.of(objectsByLayout.find(layout.index())->value.span()));
                    if (objects == ValueSets::nothing)
                        continue;
                    callbackArguments.append({ index, argument, objects });
                    m_declaredCallbackArguments = maker.unite(m_declaredCallbackArguments, objects);
                }
            }
        }
        auto seedCallbackParameters = [&] {
            for (const CallbackArgument& argument : callbackArguments) {
                const Program::DeclaredCallback& callback = m_input.program->declaredCallbacks[argument.callback];
                ValueFlowNodeID parameter = m_input.nodes->entryParameter(callback.function, m_input.program->at(callback.function).firstParameterIndex() + callback.parameter);
                ValueSetID closures = maker.intersect(visibleReceiversAt(parameter, maker), m_input.placeholders.closures);
                if (ValueSetID seen = seenBehindTheRoundsAt(parameter); seen != many)
                    closures = maker.unite(closures, maker.intersect(seen, m_input.placeholders.closures));
                if (m_sets.size(closures) > mostCalleesOfOneCallFollowed)
                    continue;
                m_sets.forEachMember(closures, [&](ValueFlowObjectID closure) {
                    FunctionID function = m_input.universe->closureFunction(closure);
                    if (!function || !isReached(function) || !m_input.program->at(function).hasBody() || argument.index >= m_input.program->at(function).numberOfPositionalParameters())
                        return;
                    arriveAt(m_input.nodes->entryParameter(function, m_input.program->at(function).firstParameterIndex() + argument.index), argument.objects);
                });
            }
        };
        // The nodes are visited in waves, each in index order, so the order of a node's links does not matter.
        findOnAllThreadsAndDo(stores.size(), findStore, runStore);
        for (uint32_t index = std::max(firstNodeOfHost, m_input.numberOfNodes); index < numberOfNodes; ++index) {
            const ValueFlowNodeKey* key = keyOfMadeNode(ValueFlowNodeID(index));
            if (key->kind != ValueFlowMadeNodeKind::Written || key->name().kind != ValueFlowNameKind::Elements || ValueSets::isComplement(key->set))
                continue;
            ValueSetID value = m_graph.valueOf(ValueFlowNodeID(index));
            if (ValueSetID lists = maker.intersect(key->set, listsFollowed); lists != ValueSets::nothing && !ValueSets::isComplement(value) && m_sets.size(lists) <= maxListsFollowedPerAccess)
                putIntoLists(lists, maker.intersect(value, followed));
        }
        for (uint32_t index : accessesOfLists)
            runAccessOfLists(index);
        m_stepTimes.didFinish("behind the rounds: covered: every call and store run, what stands in sight handed on"_s);
        Vector<uint32_t> wave;
        auto visitUntilAtRest = [&] {
            seedCallbackParameters();
            for (; !toVisit.isEmpty(); seedCallbackParameters()) {
                wave.shrink(0);
                std::swap(wave, toVisit);
                parallelSort(m_loops, wave.mutableSpan(), std::less<uint32_t> { });
                removeRepeatedElements(wave);
                findOnAllThreadsAndDo(wave.size(), [&](uint32_t index, unsigned worker, Vector<FoundStep>& found) {
                    findVisit(ValueFlowNodeID(wave[index]), worker, found);
                }, [&](uint32_t index) {
                    visit(ValueFlowNodeID(wave[index]));
                });
            }
        };
        visitUntilAtRest();
        // `copyDescriptors(app, members); app.handle(request)`: the receiver is visible, but what it was given under unknown keys is not. If nothing was observed to arrive at such a read, the name is resolved as for an unknown receiver.
        Vector<std::pair<uint32_t, ValueSetID>> comeByName;
        for (auto [key, written] : toldByName) {
            ReadsUnderKey range = readsUnderKey.get(key);
            for (uint32_t index = range.first; index < range.end; ++index) {
                const Read& read = reads[readsByGroup[index].read];
                if (read.statement != noStatement && (holdsUnknownObject(ValueFlowNodeID(read.object)) || seenBehindTheRoundsAt(ValueFlowNodeID(read.result)) == ValueSets::nothing))
                    comeByName.append({ read.result, written });
            }
        }
        for (auto [result, written] : comeByName)
            arriveAt(ValueFlowNodeID(result), written);
        visitUntilAtRest();
    }
    std::atomic<bool> sawSomethingAfterRounds { false };
    forEachIndexInParallel(m_loops, seenAfterRounds.isEmpty() ? 0 : numberOfShares, [&](unsigned share) {
        bool sawSomething = false;
        for (uint32_t& seen : seenAfterRounds.mutableSpan().subspan(share * nodesPerShare, std::min(nodesPerShare, numberOfNodes - share * nodesPerShare))) {
            if (seen == ValueSets::anything.index())
                seen = ValueSets::nothing.index();
            sawSomething |= seen != ValueSets::nothing.index();
        }
        if (sawSomething)
            sawSomethingAfterRounds.store(true, std::memory_order_relaxed);
    });
    m_stepTimes.didFinish("behind the rounds: covered: what is written, followed from the reads"_s);
    Vector<AnswersAboutLatestValues<ValueSetID>> leftOfValues(m_makers.size());
    bool hasWrittenUnderNamesOfReads = sawSomethingAfterRounds.load(std::memory_order_relaxed);
    // Only a mark, a placeholder or a kind that covers keys takes something out of a value, and only beside the unknown object is something seen under the name of a read.
    uint64_t lowMembersThatCover = 0;
    bool allThatCoverAreLowMembers = true;
    auto noteMemberThatCovers = [&](ValueFlowObjectID member) {
        allThatCoverAreLowMembers &= member.index() < 64;
        lowMembersThatCover |= 1ull << member.index() % 64;
    };
    if (m_allButMarks != ValueSets::anything) {
        for (ValueFlowObjectID mark : { ValueFlowUniverse::markOfNamedBun(), ValueFlowUniverse::markOfMemberLeftOut(), ValueFlowUniverse::markOfLoadedBun() })
            noteMemberThatCovers(mark);
    }
    for (const Covering& covering : m_coverings)
        noteMemberThatCovers(covering.placeholder);
    for (const KindThatCoversKeys& covering : m_kindsThatCoverKeys)
        noteMemberThatCovers(covering.kind);
    if (hasWrittenUnderNamesOfReads)
        noteMemberThatCovers(m_input.placeholders.unknown);
    ValueSetID staysListed = ValueSets::anything;
    for (const Covering& covering : m_coverings) {
        if (covering.placeholder == m_input.placeholders.unknown)
            staysListed = covering.allButCovered;
    }
    forEachIndexInParallel(m_loops, m_listedWithCoveredMembers.size(), [&](unsigned share, unsigned worker) {
        ValueSetMaker& maker = *m_makers[worker];
        for (uint32_t index = share * nodesPerShare; index < std::min(numberOfNodes, (share + 1) * nodesPerShare); ++index) {
            ValueFlowNodeID node(index);
            ValueSetID value = m_graph.valueOf(node);
            if (value == ValueSets::nothing || (allThatCoverAreLowMembers && !(m_sets.membersBelow64(value) & lowMembersThatCover)) || !isReadByResults(node))
                continue;
            if (hasWrittenUnderNamesOfReads && index < firstNodeOfHost)
                value = maker.unite(value, seenBehindTheRoundsAt(node));
            ValueSetID left = leftOfValues[worker].ensure(value, [&] {
                return maker.unite(withoutWhatOtherMembersCover(value, maker), maker.intersect(value, m_takenByRetiredNodes));
            });
            if (left == value)
                continue;
            m_graph.replaceValueAfterSolving(node, left);
            if (index >= firstNodeOfHost) {
                if (keyOfMadeNode(node)->kind == ValueFlowMadeNodeKind::Written)
                    m_listedWithCoveredMembers[share].append(ValueFlowNodeSet { node, value });
                continue;
            }
            if (ValueSetID listed = m_input.universe ? maker.intersect(value, m_input.universe->builtinFunctions()) : ValueSets::nothing; listed != ValueSets::nothing)
                m_functionsOfEngineListed[share].append(ValueFlowNodeSet { node, listed });
            // Which of the objects that have escaped came to a node before the unknown object did hangs on the order of the rules. They are seen where they were followed to.
            ValueSetID seen = maker.unite(maker.unite(left, maker.intersect(value, staysListed)), seenBehindTheRoundsAt(node));
            if (seen != left)
                m_listedWithCoveredMembers[share].append(ValueFlowNodeSet { node, seen });
        }
        m_listedWithCoveredMembers[share].shrinkToFit();
    });
}

ValueSetID ValueFlowSolver::withoutMarks(ValueSetID value, ValueSetMaker& maker) const
{
    if (m_allButMarks == ValueSets::anything)
        return value;
    return m_sets.contains(value, ValueFlowUniverse::markOfNamedBun()) || m_sets.contains(value, ValueFlowUniverse::markOfMemberLeftOut()) || m_sets.contains(value, ValueFlowUniverse::markOfLoadedBun()) ? maker.intersect(value, m_allButMarks) : value;
}

ValueSetID ValueFlowSolver::withoutWhatOtherMembersCover(ValueSetID value, ValueSetMaker& maker) const
{
    // The marks, the placeholders and the kinds are among the lowest members, which a set holds in one word.
    uint64_t lowestMembers = m_sets.membersBelow64(value);
    auto holds = [&](ValueFlowObjectID member) {
        return member.index() < 64 ? lowestMembers >> member.index() & 1 : m_sets.contains(value, member);
    };
    auto keep = [&](ValueSetID kept) {
        value = maker.intersect(value, kept);
        lowestMembers = m_sets.membersBelow64(value);
    };
    if (m_allButMarks != ValueSets::anything && (holds(ValueFlowUniverse::markOfNamedBun()) || holds(ValueFlowUniverse::markOfMemberLeftOut()) || holds(ValueFlowUniverse::markOfLoadedBun())))
        keep(m_allButMarks);
    for (const Covering& covering : m_coverings) {
        if (holds(covering.placeholder))
            keep(covering.allButCovered);
    }
    for (const KindThatCoversKeys& covering : m_kindsThatCoverKeys) {
        if (holds(covering.kind))
            keep(covering.allButKeys);
    }
    return value;
}

ValueSetID ValueFlowSolver::withWhatMembersCover(ValueSetID value, ValueSetMaker& maker) const
{
    for (const Covering& covering : m_coverings) {
        if (!m_sets.contains(value, covering.placeholder))
            continue;
        value = maker.unite(value, ValueSets::complement(covering.allButCovered));
        // Once the untracked object has escaped, so has all that it covers.
        if (covering.placeholder == m_input.placeholders.unknown && m_input.placeholders.untracked && m_sets.contains(m_graph.valueOf(covering.covered), m_input.placeholders.untracked))
            value = maker.unite(value, m_untrackedObjectAlone);
    }
    for (const KindThatCoversKeys& covering : m_kindsThatCoverKeys) {
        if (m_sets.contains(value, covering.kind))
            value = maker.unite(value, ValueSets::complement(covering.allButKeys));
    }
    return value;
}

// By the function and the place in it, so that what is drawn in one function does not hang on the rest of the program.
static uint32_t drawForWidening(uint32_t seed, FunctionID function, uint32_t numberInFunction, uint32_t question)
{
    uint64_t hash = (seed * 0x9E3779B97F4A7C15ull) ^ ((static_cast<uint64_t>(function.index()) << 32 | numberInFunction) * 0xC2B2AE3D27D4EB4Full) ^ (question * 0x165667B19E3779F9ull);
    hash ^= hash >> 29;
    hash *= 0xBF58476D1CE4E5B9ull;
    hash ^= hash >> 32;
    hash *= 0x94D049BB133111EBull;
    return static_cast<uint32_t>(hash >> 32);
}

// The seed says how many nodes are drawn, one in 4, 16, 128 or 2048, and whether among all bodies or only among those written in the program. Where many nodes are given more, most of the program escapes;
// a rule that goes wrong beside one value nobody knows shows where few are. A node that is drawn gets the unknown object, a value nobody knows, or what another node of its body holds.
ValueFlowSolver::GivenToNode ValueFlowSolver::whatIsGivenTo(ValueFlowNodeID node) const
{
    if (!m_input.seedOfWidening || !m_input.nodes || !m_input.universe || !m_input.program)
        return GivenToNode::Nothing;
    uint32_t seed = *m_input.seedOfWidening;
    constexpr std::array<uint8_t, 4> bitsThatMustBeZero { 2, 4, 7, 11 };
    FunctionID function = m_input.nodes->functionOf(node);
    if (!function || function.index() >= m_input.program->functions.size() || ((seed & 1) && !isWrittenInProgram(m_input.program->at(function))))
        return GivenToNode::Nothing;
    uint32_t drawn = drawForWidening(seed, function, node.index() - m_input.nodes->firstNodeOf(function), 0);
    if (drawn >> 2 & ((1u << bitsThatMustBeZero[seed >> 1 & 3]) - 1))
        return GivenToNode::Nothing;
    return drawn & 2 ? GivenToNode::WhatAnotherNodeOfBodyHolds : drawn & 1 ? GivenToNode::ValueNobodyKnows : GivenToNode::UnknownObject;
}

ValueFlowNodeID ValueFlowSolver::nodeWhoseValueIsGivenTo(ValueFlowNodeID node) const
{
    FunctionID function = m_input.nodes->functionOf(node);
    uint32_t first = m_input.nodes->firstNodeOf(function);
    return ValueFlowNodeID(first + drawForWidening(*m_input.seedOfWidening, function, node.index() - first, 1) % (m_input.nodes->endOfNodesOf(function) - first));
}

ValueSetID ValueFlowSolver::valueForReadersOf(ValueFlowNodeID node, ValueSetMaker& maker) const
{
    if (node.index() >= m_graph.numberOfNodes())
        return ValueSets::nothing;
    // What is covered by now is left out, not what was covered when the readers were last woken: else what a rule is shown hangs on when it happens to run.
    bool hasReaders = m_stateOfCoverings != UINT16_MAX && node.index() < m_coveringStateOfShownView.size() && m_readersOfNode[node.index()];
    if (hasReaders && m_coveringStateOfShownView[node.index()] == m_stateOfCoverings)
        return m_graph.seenByReadersOf(node);
    ValueSetID value = withoutWhatOtherMembersCover(node, maker);
    if (hasReaders && value == m_graph.seenByReadersOf(node))
        m_coveringStateOfShownView[node.index()] = m_stateOfCoverings;
    return value;
}

ValueSetID ValueFlowSolver::valueShownTo(ValueFlowStatementID statement, ValueFlowNodeID node) const
{
    auto valuesShown = m_valuesShownToStatement.find(statement.index());
    if (valuesShown == m_valuesShownToStatement.end())
        return ValueSets::nothing;
    for (const ValueFlowNodeSet& shown : valuesShown->value) {
        if (shown.node == node)
            return shown.set;
    }
    return ValueSets::nothing;
}

ValueSetID ValueFlowSolver::findFamilyObjectsOf(ValueSetID mask, ValueSetMaker& maker) const
{
    ValueSetID listed = maker.intersect(mask, m_objectsSaidToBeSomething);
    return listed != ValueSets::nothing && m_sets.isSubset(listed, m_objectsOfProgram) ? listed : ValueSetID();
}

ValueSetID ValueFlowSolver::familyObjectsOf(ValueSetID mask, ValueSetMaker& maker) const
{
    auto found = m_familyObjectsOfMask.find(mask.index());
    return found != m_familyObjectsOfMask.end() ? found->value : findFamilyObjectsOf(mask, maker);
}

// A node shows less once more of what it holds is covered, and it does not move for that. Its readers run again before the analysis rests.
bool ValueFlowSolver::wakeReadersOfNodesThatShowSomethingElse()
{
    // Only what has been covered since last time removes something from a view.
    ValueSetID coveredSince = ValueSets::nothing;
    while (m_uncoveredAtLastReaderWake.size() < m_coverings.size())
        m_uncoveredAtLastReaderWake.append(ValueSets::anything);
    for (size_t index = 0; index < m_coverings.size(); ++index) {
        coveredSince = m_makers[0]->unite(coveredSince, m_makers[0]->intersect(ValueSets::complement(m_coverings[index].allButCovered), m_uncoveredAtLastReaderWake[index]));
        m_uncoveredAtLastReaderWake[index] = m_coverings[index].allButCovered;
    }
    if (coveredSince == ValueSets::nothing)
        return false;
    constexpr uint32_t nodesPerShare = 1 << 14;
    uint32_t numberOfNodes = m_readersOfNode.size();
    Vector<Vector<uint32_t>> nodesWithChangedView((numberOfNodes + nodesPerShare - 1) / nodesPerShare);
    forEachIndexInParallel(m_loops, nodesWithChangedView.size(), [&](unsigned share, unsigned worker) {
        ValueSetMaker& maker = *m_makers[worker];
        for (uint32_t index = share * nodesPerShare; index < std::min(numberOfNodes, (share + 1) * nodesPerShare); ++index) {
            if (!m_readersOfNode[index] || !maker.meet(m_graph.seenByReadersOf(ValueFlowNodeID(index)), coveredSince))
                continue;
            ValueSetID value = withoutWhatOtherMembersCover(ValueFlowNodeID(index), maker);
            if (value == m_graph.seenByReadersOf(ValueFlowNodeID(index)))
                continue;
            m_graph.setSeenByReadersOf(ValueFlowNodeID(index), value);
            nodesWithChangedView[share].append(index);
        }
    });
    bool hasWoken = false;
    for (const Vector<uint32_t>& nodes : nodesWithChangedView) {
        for (uint32_t index : nodes) {
            forEachReaderOf(ValueFlowNodeID(index), [&](ValueFlowStatementID reader) {
                makeDue(reader);
            });
            hasWoken = true;
        }
    }
    return hasWoken;
}

void ValueFlowSolver::makeReadersDue(std::span<const ValueFlowNodeID> moved)
{
    // Nodes that hold large sets, and members that many sets hold, stand side by side.
    constexpr size_t nodesPerShare = 32;
    constexpr size_t membersPerShare = 32;
    growByDoubling(m_isDueLast, numberOfStatements());
    m_readersOfWorker.grow(m_makers.size());
    m_newMembersOfWorker.grow(m_makers.size());
    m_watchesToTestOfWorker.grow(m_makers.size());
    forEachIndexInParallel(m_loops, (moved.size() + nodesPerShare - 1) / nodesPerShare, [&](unsigned share, unsigned worker) {
        ValueSetMaker& maker = *m_makers[worker];
        Vector<ValueFlowStatementID>& found = m_readersOfWorker[worker];
        for (size_t index = share * nodesPerShare; index < std::min(moved.size(), (share + 1) * nodesPerShare); ++index) {
            ValueFlowNodeID node = moved[index];
            if (node.index() >= m_listenersOfNode.size())
                continue;
            uint8_t listeners = m_listenersOfNode[node.index()];
            if (!listeners)
                continue;
            if (listeners & (WaitsForFirstOperand | WaitsForNodeToHoldSomething))
                m_listenersOfNode[node.index()] = listeners & ~(WaitsForFirstOperand | WaitsForNodeToHoldSomething);
            if (listeners & WaitsForFirstOperand) {
                for (uint32_t waiting = std::exchange(m_firstStatementWaitingForNode[node.index()], 0u); waiting;) {
                    listIfItMayBecomeDue(ValueFlowStatementID(waiting - 1), found);
                    waiting = std::exchange(m_nextStatementWaiting[waiting - 1], hasWaitedAndNotRun);
                }
            }
            if (listeners & WatchesNode)
                listWatchersOf(node, maker, found, m_newMembersOfWorker[worker], m_watchesToTestOfWorker[worker]);
            if (listeners & WaitsForNodeToHoldSomething) {
                const ReaderShard& shard = m_readerShards[readerShardOf(node)];
                for (uint32_t reader = shard.firstReaderWaitingForSomething.get(node.index()); reader != noSolverIndex; reader = shard.readersWaitingForSomething[reader].next)
                    listIfItMayBecomeDue(shard.readersWaitingForSomething[reader].statement, found);
            }
            if (!(listeners & ReadsNode))
                continue;
            ValueSetID value = withoutWhatOtherMembersCover(node, maker);
            if (value == m_graph.seenByReadersOf(node))
                continue;
            m_graph.setSeenByReadersOf(node, value);
            if (node.index() < m_coveringStateOfShownView.size())
                m_coveringStateOfShownView[node.index()] = UINT16_MAX;
            // A long chain is found one link a round. Whoever reads it runs again when nothing else is left to run: by then the chain is whole.
            bool readersRunLast = false;
            if (isChainOfPrototypes(node)) {
                static_assert(mostWakeUpsByGrowingChain << shiftOfWakeUpsByChain <= UINT8_MAX);
                readersRunLast = listeners >> shiftOfWakeUpsByChain == mostWakeUpsByGrowingChain;
                if (!readersRunLast)
                    WTF::atomicExchangeAdd(&m_listenersOfNode[node.index()], static_cast<uint8_t>(1 << shiftOfWakeUpsByChain));
            }
            forEachReaderOf(node, [&](ValueFlowStatementID reader) {
                if (readersRunLast)
                    listAsDueLast(reader);
                else
                    listIfItMayBecomeDue(reader, found);
            });
        }
    });

    Vector<NewMemberOfWatchedNode>& newMembers = m_newMembers;
    for (Vector<NewMemberOfWatchedNode>& ofWorker : m_newMembersOfWorker) {
        makeRoomByDoubling(newMembers, ofWorker.size());
        newMembers.appendVector(ofWorker);
        ofWorker.shrink(0);
        giveBackRoomIfEmpty(ofWorker);
    }
    forEachIndexInParallel(m_loops, (newMembers.size() + membersPerShare - 1) / membersPerShare, [&](unsigned share, unsigned worker) {
        for (size_t index = share * membersPerShare; index < std::min(newMembers.size(), (share + 1) * membersPerShare); ++index) {
            if (newMembers[index].shard == numberOfWatchShards) {
                uint32_t member = newMembers[index].member.index();
                const Vector<ChainedWatch>& chained = m_setsWatchedForCoverOfMembers[rangeOfMember(member)];
                for (uint32_t entry = m_firstSetWatchedForCoverOfMember[member]; entry != noSolverIndex; entry = chained[entry].next) {
                    const WatchShard& shard = m_watchShards[numberOfWatchShardsByNode + chained[entry].watch % numberOfWatchShardsBySet];
                    for (uint32_t watch : shard.watchesOfSetWatchedForCover[chained[entry].watch / numberOfWatchShardsBySet]) {
                        if (shard.watchedNodes[shard.watches[watch].watchedNode].node.index() == newMembers[index].watchedNode)
                            listReadersOfWatch(shard, watch, m_readersOfWorker[worker]);
                    }
                }
                continue;
            }
            const WatchShard& shard = m_watchShards[newMembers[index].shard];
            const uint32_t* first = shard.firstWatchOfMember.find(static_cast<uint64_t>(newMembers[index].watchedNode) << 32 | newMembers[index].member.index());
            if (!first)
                continue;
            for (uint32_t entry = *first; entry != noSolverIndex; entry = shard.watchesOfMembers[entry].next)
                listReadersOfWatch(shard, shard.watchesOfMembers[entry].watch, m_readersOfWorker[worker]);
        }
    });

    constexpr uint32_t watchesPerShare = 128;
    struct ShareOfWatches {
        WatchesToTest watches;
        uint32_t first { 0 };
    };
    Vector<ShareOfWatches> sharesOfWatches;
    for (Vector<WatchesToTest>& ofWorker : m_watchesToTestOfWorker) {
        for (const WatchesToTest& watches : ofWorker) {
            uint32_t count = m_watchShards[watches.shard].watchedNodes[watches.watchedNode].watchesOfManyObjects.size();
            for (uint32_t first = 0; first < count; first += watchesPerShare)
                sharesOfWatches.append({ watches, first });
        }
        ofWorker.shrink(0);
    }
    newMembers.shrink(0);
    forEachIndexInParallel(m_loops, sharesOfWatches.size(), [&](unsigned share, unsigned worker) {
        const WatchesToTest& watches = sharesOfWatches[share].watches;
        const WatchShard& shard = m_watchShards[watches.shard];
        std::span<const uint32_t> all = shard.watchedNodes[watches.watchedNode].watchesOfManyObjects.span();
        for (uint32_t watch : all.subspan(sharesOfWatches[share].first, std::min<size_t>(watchesPerShare, all.size() - sharesOfWatches[share].first))) {
            if (m_sets.meet(watches.whatIsNew, shard.watches[watch].within))
                listReadersOfWatch(shard, watch, m_readersOfWorker[worker]);
        }
    });

    size_t numberFound = 0;
    for (const Vector<ValueFlowStatementID>& found : m_readersOfWorker)
        numberFound += found.size();
    m_due.reserveCapacity(m_due.size() + numberFound);
    for (Vector<ValueFlowStatementID>& found : m_readersOfWorker) {
        m_due.appendVector(found);
        found.shrink(0);
    }
}

ValueSetID ValueFlowSolver::findClosuresFollowedTo(ValueFlowNodeID callee, ValueSetMaker& maker) const
{
    AOT_THRESHOLD(unsigned, mostNodesVisited, 128);
    AOT_THRESHOLD(unsigned, mostPredecessorsFollowed, 8);
    Vector<ValueFlowNodeID, 32> visited { callee };
    Vector<ValueFlowNodeID, 8> predecessors;
    ValueSetID closures = ValueSets::nothing;
    for (unsigned next = 0; next < visited.size(); ++next) {
        predecessors.shrink(0);
        unsigned numberOfPredecessors = 0;
        m_graph.forEachPredecessor(visited[next], [&](ValueFlowNodeID predecessor) {
            if (++numberOfPredecessors <= mostPredecessorsFollowed)
                predecessors.append(predecessor);
        });
        if (!numberOfPredecessors)
            closures = maker.unite(closures, maker.intersect(m_graph.valueOf(visited[next]), m_input.placeholders.closures));
        if (numberOfPredecessors > mostPredecessorsFollowed)
            continue;
        for (ValueFlowNodeID predecessor : predecessors) {
            if (predecessor == m_input.escaped || predecessor == m_input.lost || isMadeForUnknownObject(predecessor) || visited.contains(predecessor))
                continue;
            if (visited.size() >= mostNodesVisited)
                return ValueSets::nothing;
            visited.append(predecessor);
        }
    }
    return m_sets.size(closures) > m_input.mostMembersToldApart ? ValueSets::nothing : closures;
}

void ValueFlowSolver::findClosuresFollowedToConstructions()
{
    if (!m_input.program || !m_input.placeholders.unknown || !m_input.placeholders.closures)
        return;
    constexpr size_t statementsPerShare = 1 << 13;
    Vector<Vector<ValueFlowNodeSet>> followedOfShare((m_input.statements.size() + statementsPerShare - 1) / statementsPerShare);
    forEachIndexInParallel(m_loops, followedOfShare.size(), [&](unsigned share, unsigned worker) {
        size_t first = share * statementsPerShare;
        for (const ValueFlowStatement& statement : m_input.statements.subspan(first, std::min(statementsPerShare, m_input.statements.size() - first))) {
            if (statement.kind != ValueFlowStatementKind::Operation || !OperandPosition::newTarget(statement.opcode) || !isReached(statement.function))
                continue;
            ValueFlowNodeID callee = operandsOf(statement)[0].node;
            if (callee && m_sets.contains(m_graph.valueOf(callee), m_input.placeholders.unknown))
                followedOfShare[share].append(ValueFlowNodeSet { callee, findClosuresFollowedTo(callee, *m_makers[worker]) });
        }
    });
    for (const Vector<ValueFlowNodeSet>& followed : followedOfShare) {
        for (const ValueFlowNodeSet& entry : followed)
            m_closuresFollowedToConstruction.add(entry.node.index(), entry.set);
    }
}

bool ValueFlowSolver::isMadeForUnknownObject(ValueFlowNodeID node) const
{
    const ValueFlowNodeKey* key = keyOfMadeNode(node);
    return key && traitsOfMadeNodes(key->kind).contains(TraitOfMadeNodes::IsMadeForObjects) && key->set && m_sets.onlyMember(key->set) == m_input.placeholders.unknown;
}

// Nothing is read off such a node: it hands what it takes on to the node of values carried out, which tells an argument from a receiver.
bool ValueFlowSolver::takesArgumentsOfCallGroup(ValueFlowNodeID node) const
{
    const ValueFlowNodeKey* key = keyOfMadeNode(node);
    if (!key || key->kind != ValueFlowMadeNodeKind::CallGroup)
        return false;
    ValueFlowCallGroup group = callGroupAt(firstOfMadeNodesAt(indexOfMadeNodes(*key)));
    return node == group.otherArguments || node == group.laterArguments || node.index() >= group.firstArgument.index();
}

bool ValueFlowSolver::maySaturate(ValueFlowNodeID node) const
{
    if (node == m_input.escaped || node == m_input.lost || m_graph.maskOf(node) != ValueSets::anything || m_graph.isRetired(node))
        return false;
    const ValueFlowNodeKey* key = keyOfMadeNode(node);
    if (!key)
        return node.index() < m_input.nodes->ofHost(static_cast<ValueFlowHostNode>(0)).index();
    OptionSet<TraitOfMadeNodes> traits = traitsOfMadeNodes(key->kind);
    if (traits.contains(TraitOfMadeNodes::MaySaturateUnderSomeNames))
        return key->name().kind == ValueFlowNameKind::Property || key->name().kind == ValueFlowNameKind::Elements || key->name().kind == ValueFlowNameKind::UnknownKey;
    return traits.contains(TraitOfMadeNodes::MaySaturate);
}

bool ValueFlowSolver::hasTooManyLayoutsAndCallees(ValueSetID set)
{
    if (ValueSets::isComplement(set) || m_sets.size(set) <= m_input.mostSetMembers)
        return false;
    return m_isTooLarge.ensure(set.index(), [&] {
        uint32_t count = 0;
        ++m_numberOfCounts;
        return m_sets.anyMember(set, [&](ValueFlowObjectID member) {
            uint32_t layoutOrCallee = m_layoutOrCalleeOfMember[member.index()];
            if (layoutOrCallee && std::exchange(m_lastCountThatSaw[layoutOrCallee], m_numberOfCounts) != m_numberOfCounts)
                ++count;
            return count > m_input.mostSetMembers;
        });
    }).iterator->value;
}

void ValueFlowSolver::saturate(std::span<const ValueFlowNodeID> moved)
{
    ValueFlowNodeID holdingUntrackedObject;
    ValueSetMaker& maker = *m_makers[0];
    constexpr size_t nodesPerShare = 4096;
    Vector<Vector<ValueFlowNodeID>> nodesToLookAtOfShare((moved.size() + nodesPerShare - 1) / nodesPerShare);
    forEachIndexInParallel(m_loops, nodesToLookAtOfShare.size(), [&](unsigned share) {
        for (ValueFlowNodeID node : moved.subspan(share * nodesPerShare, std::min(nodesPerShare, moved.size() - share * nodesPerShare))) {
            ValueSetID listed = m_graph.valueOf(node);
            bool isSmall = !ValueSets::isComplement(listed) && m_sets.size(listed) <= m_input.mostSetMembers;
            if (!isSmall && !m_isSaturated.get(node.index()) && maySaturate(node))
                nodesToLookAtOfShare[share].append(node);
        }
    });
    Vector<ValueFlowNodeID> nodesToLookAt;
    for (const Vector<ValueFlowNodeID>& ofShare : nodesToLookAtOfShare)
        nodesToLookAt.appendVector(ofShare);
    for (ValueFlowNodeID node : nodesToLookAt) {
        ValueSetID value = withoutWhatOtherMembersCover(node, maker);
        if (!hasTooManyLayoutsAndCallees(value))
            continue;
        if (!holdingUntrackedObject)
            holdingUntrackedObject = ensureNodes(ValueFlowNodeKey::constant(m_untrackedObjectAlone));
        m_isSaturated.set(node.index());
        ++m_counters.nodesSaturated;
        m_edgesOfRound.append(ValueFlowEdge { node, m_input.lost });
        m_edgesOfRound.append(ValueFlowEdge { holdingUntrackedObject, node });
    }
}

void ValueFlowSolver::spreadMarks()
{
    m_marksOfObject.fill(OptionSet<ObjectMutation>(), m_sets.sizeOfUniverse());
    for (ObjectMutation mutation : OptionSet<ObjectMutation>::all()) {
        std::optional<ValueFlowNodeID> node = findMadeNode(ValueFlowNodeKey::marked(mutation));
        if (!node)
            continue;
        m_sets.forEachMemberInLine(m_graph.valueOf(*node), [&](ValueFlowObjectID object) {
            m_marksOfObject[object.index()].add(mutation);
        });
    }

    if (!m_input.lost || !m_input.placeholders.untracked)
        return;
    OptionSet<ObjectMutation> marksOfUntrackedObject = m_marksOfObject[m_input.placeholders.untracked.index()];
    if (marksOfUntrackedObject.isEmpty())
        return;
    m_sets.forEachMember(m_makers[0]->intersect(m_graph.valueOf(m_input.lost), m_objectsThatPlaceholdersMayCover), [&](ValueFlowObjectID object) {
        m_marksOfObject[object.index()].add(marksOfUntrackedObject);
    });
}

void ValueFlowSolver::findObjectsOutsideFamilies()
{
    Vector<ValueFlowNodeSet> masks;
    for (const ValueFlowNodeMask& mask : m_input.masks) {
        if (mask.kind == ValueFlowMaskKind::Declared)
            masks.append(ValueFlowNodeSet { mask.node, mask.mask });
    }
    for (const MadeNodes& madeNodes : m_madeNodes) {
        if (madeNodes.key.kind == ValueFlowMadeNodeKind::Masked && madeNodes.key.maskKind() == ValueFlowMaskKind::Declared)
            masks.append(ValueFlowNodeSet { madeNodes.first, madeNodes.key.set });
    }
    if (masks.isEmpty())
        return;

    constexpr size_t masksPerShare = 256;
    bool looksAtArrivals = m_input.build && m_input.build->validates();
    ValueSetID whatIsNotFollowed = ValueSets::complement(m_objectsThatPlaceholdersMayCover);
    ValueSetID whatMaskMayTakeAway = m_input.placeholders.unknown ? m_makers[0]->intersect(whatIsNotFollowed, ValueSets::complement(m_makers[0]->of(m_input.placeholders.unknown))) : whatIsNotFollowed;
    Vector<Vector<ObjectOutsideFamily>> foundByWorker(m_makers.size());
    forEachIndexInParallel(m_loops, (masks.size() + masksPerShare - 1) / masksPerShare, [&](unsigned share, unsigned worker) {
        ValueSetMaker& maker = *m_makers[worker];
        auto firstMemberOutside = [&](ValueSetID set, ValueSetID within) {
            ValueFlowObjectID first;
            if (m_sets.isSubset(set, within))
                return first;
            m_sets.forEachMember(maker.intersect(set, ValueSets::complement(within)), [&](ValueFlowObjectID member) {
                if (!first)
                    first = member;
            });
            return first;
        };
        for (const ValueFlowNodeSet& mask : masks.subspan(share * masksPerShare, std::min(masksPerShare, masks.size() - share * masksPerShare))) {
            ValueSetID value = m_graph.valueOf(mask.node);
            ValueFlowObjectID object = firstMemberOutside(value, maker.unite(mask.set, whatIsNotFollowed));
            if (!object && looksAtArrivals && !m_graph.isAtRest(mask.node)) {
                ValueSetID accountedFor = maker.unite(value, whatMaskMayTakeAway);
                for (const Covering& covering : m_coverings) {
                    if (m_sets.contains(value, covering.placeholder))
                        accountedFor = maker.unite(accountedFor, ValueSets::complement(covering.allButCovered));
                }
                m_graph.forEachPredecessor(mask.node, [&](ValueFlowNodeID predecessor) {
                    ValueFlowObjectID takenAway = firstMemberOutside(m_graph.valueOf(predecessor), accountedFor);
                    if (takenAway && (!object || takenAway < object))
                        object = takenAway;
                });
            }
            if (object)
                foundByWorker[worker].append(ObjectOutsideFamily { mask.node, object });
        }
    });

    for (Vector<ObjectOutsideFamily>& found : foundByWorker) {
        found.removeAllMatching([&](const ObjectOutsideFamily& entry) {
            FunctionID function = subjectOf(entry.node).function;
            return function && !isReached(function);
        });
        m_objectsOutsideFamilies.appendVector(found);
    }
    AOT::sort(m_objectsOutsideFamilies, { }, &ObjectOutsideFamily::node);
    for (const ObjectOutsideFamily& entry : m_objectsOutsideFamilies)
        m_holdsObjectOutsideFamily.set(entry.node.index());
    m_counters.nodesHoldingObjectsOutsideFamily = m_objectsOutsideFamilies.size();
}

void ValueFlowSolver::findUnknownSources(NOESCAPE const ScopedLambda<void()>& whatRunsBesideTheWalk)
{
    ValueFlowObjectID unknownObject = m_input.placeholders.unknown;
    if (!unknownObject || !m_input.isAskedHowValuesCame) {
        whatRunsBesideTheWalk();
        return;
    }
    uint32_t numberOfNodes = m_graph.numberOfNodes();
    m_unknownSourceOfNode.fill(uint8_t { 0 }, numberOfNodes);

    constexpr size_t madeNodesPerShare = 1 << 12;
    Vector<uint32_t> found = collectInParallel<uint32_t>(m_loops, static_cast<unsigned>((m_madeNodes.size() + madeNodesPerShare - 1) / madeNodesPerShare), [&](unsigned share, Vector<uint32_t>& foundInShare) {
        size_t first = share * madeNodesPerShare;
        for (const MadeNodes& madeNodes : m_madeNodes.span().subspan(first, std::min(madeNodesPerShare, m_madeNodes.size() - first))) {
            if (madeNodes.key.kind != ValueFlowMadeNodeKind::Constant || !madeNodes.key.unknownSource() || !m_sets.contains(madeNodes.key.set, unknownObject))
                continue;
            m_unknownSourceOfNode[madeNodes.first.index()] = 1 + static_cast<uint8_t>(*madeNodes.key.unknownSource());
            foundInShare.append(madeNodes.first.index());
        }
    });
    if (found.isEmpty()) {
        whatRunsBesideTheWalk();
        return;
    }

    constexpr uint32_t nodesPerShare = 1 << 14;
    unsigned numberOfShares = (numberOfNodes + nodesPerShare - 1) / nodesPerShare;
    auto forEachNodeOfShare = [&](unsigned share, const auto& functor) {
        for (uint32_t node = share * nodesPerShare; node < std::min(numberOfNodes, (share + 1) * nodesPerShare); ++node)
            functor(node);
    };
    BitVector holdsUnknownObject(numberOfNodes);
    forEachIndexInParallel(m_loops, numberOfShares, [&](unsigned share) {
        forEachNodeOfShare(share, [&](uint32_t node) {
            if (m_sets.contains(m_graph.valueOf(ValueFlowNodeID(node)), unknownObject))
                holdsUnknownObject.quickSet(node);
        });
    });
    size_t numberOfConstantsFound = found.size();
    Vector<ValueFlowEdge> edgesOutOfConstantsFound;
    m_graph.forEachListedEdgeOutOfConstants([&](ValueFlowEdge edge) {
        if (m_unknownSourceOfNode[edge.from.index()] && holdsUnknownObject.quickGet(edge.to.index()))
            edgesOutOfConstantsFound.append(edge);
    });
    parallelSort(m_loops, edgesOutOfConstantsFound.mutableSpan(), [](ValueFlowEdge first, ValueFlowEdge second) {
        return std::pair { first.from.index(), first.to.index() } < std::pair { second.from.index(), second.to.index() };
    });
    auto successorsOfConstantFound = [&](uint32_t constant) {
        auto range = std::ranges::equal_range(edgesOutOfConstantsFound.span(), constant, { }, [](ValueFlowEdge edge) {
            return edge.from.index();
        });
        return std::span<const ValueFlowEdge>(range.begin(), range.end());
    };

    Vector<uint32_t> firstSuccessor;
    firstSuccessor.fill(0u, numberOfNodes + 1);
    forEachIndexInParallel(m_loops, numberOfShares, [&](unsigned share) {
        forEachNodeOfShare(share, [&](uint32_t node) {
            if (!holdsUnknownObject.quickGet(node))
                return;
            m_graph.forEachSuccessor(ValueFlowNodeID(node), [&](ValueFlowNodeID successor) {
                firstSuccessor[node + 1] += holdsUnknownObject.quickGet(successor.index());
            });
        });
    });
    Vector<uint32_t> successorsBeforeShare(numberOfShares);
    forEachIndexInParallel(m_loops, numberOfShares, [&](unsigned share) {
        uint32_t numberInShare = 0;
        forEachNodeOfShare(share, [&](uint32_t node) {
            numberInShare += firstSuccessor[node + 1];
        });
        successorsBeforeShare[share] = numberInShare;
    });
    uint32_t numberOfSuccessors = 0;
    for (uint32_t& before : successorsBeforeShare)
        before = std::exchange(numberOfSuccessors, numberOfSuccessors + before);
    forEachIndexInParallel(m_loops, numberOfShares, [&](unsigned share) {
        uint32_t end = successorsBeforeShare[share];
        forEachNodeOfShare(share, [&](uint32_t node) {
            end += firstSuccessor[node + 1];
            firstSuccessor[node + 1] = end;
        });
    });
    Vector<uint32_t> successors;
    successors.grow(firstSuccessor.last());
    forEachIndexInParallel(m_loops, numberOfShares, [&](unsigned share) {
        forEachNodeOfShare(share, [&](uint32_t node) {
            if (firstSuccessor[node] == firstSuccessor[node + 1])
                return;
            uint32_t next = firstSuccessor[node];
            m_graph.forEachSuccessor(ValueFlowNodeID(node), [&](ValueFlowNodeID successor) {
                if (holdsUnknownObject.quickGet(successor.index()))
                    successors[next++] = successor.index();
            });
            AOT::sort(successors.mutableSpan().subspan(firstSuccessor[node], next - firstSuccessor[node]));
        });
    });

    constexpr size_t stepsAheadForRow = 16;
    constexpr size_t stepsAheadForSuccessors = 8;
    constexpr size_t stepsAheadForSources = 4;
    AOT_THRESHOLD(uint32_t, mostSourcesAskedFor, 8);
    auto rankOf = [](uint8_t source) {
        return source == 1 + static_cast<uint8_t>(ValueFlowUnknownSource::UndescribedNativeResult) ? std::numeric_limits<uint8_t>::max() : source;
    };
    BitVector isInNextLevel(numberOfNodes);
    auto reach = [&](uint32_t from, uint32_t to) {
        uint8_t& source = m_unknownSourceOfNode[to];
        if (!source) {
            source = m_unknownSourceOfNode[from];
            isInNextLevel.quickSet(to);
            found.append(to);
        } else if (isInNextLevel.quickGet(to) && rankOf(m_unknownSourceOfNode[from]) < rankOf(source))
            source = m_unknownSourceOfNode[from];
    };
    auto walk = [&] {
        for (size_t next = 0; next < numberOfConstantsFound; ++next) {
            for (ValueFlowEdge edge : successorsOfConstantFound(found[next]))
                reach(found[next], edge.to.index());
        }
        size_t endOfLevel = numberOfConstantsFound;
        for (size_t next = numberOfConstantsFound; next < found.size(); ++next) {
            if (next == endOfLevel) {
                endOfLevel = found.size();
                for (uint32_t node : found.subspan(next))
                    isInNextLevel.quickClear(node);
            }
            if (next + stepsAheadForRow < found.size())
                __builtin_prefetch(&firstSuccessor[found[next + stepsAheadForRow]]);
            if (next + stepsAheadForSuccessors < found.size())
                __builtin_prefetch(successors.span().data() + firstSuccessor[found[next + stepsAheadForSuccessors]]);
            if (next + stepsAheadForSources < found.size()) {
                uint32_t later = found[next + stepsAheadForSources];
                for (uint32_t successor : successors.subspan(firstSuccessor[later], std::min(mostSourcesAskedFor, firstSuccessor[later + 1] - firstSuccessor[later])))
                    __builtin_prefetch(&m_unknownSourceOfNode[successor]);
            }
            uint32_t node = found[next];
            for (uint32_t successor : successors.subspan(firstSuccessor[node], firstSuccessor[node + 1] - firstSuccessor[node]))
                reach(node, successor);
        }
    };
    runTasksInParallel(m_loops, {
        walk,
        [&] {
            whatRunsBesideTheWalk();
        },
    });
}

Subject ValueFlowSolver::subjectOf(ValueFlowNodeID node) const
{
    Subject subject;
    if (!m_input.nodes || !m_input.program)
        return subject;
    if (const ValueFlowNodeKey* key = keyOfMadeNode(node)) {
        if (key->kind != ValueFlowMadeNodeKind::Masked || keyOfMadeNode(key->maskedNode()))
            return subject;
        node = key->maskedNode();
    }
    FunctionID function = m_input.nodes->functionOf(node);
    if (!function || function.index() >= m_input.program->functions.size())
        return subject;
    subject.function = function;

    const Function& body = m_input.program->at(function);
    uint32_t index = node.index() - m_input.nodes->firstNodeOf(function);
    if (body.hasBody() && index < body.numberOfEntryParameters()) {
        subject.value = body.at(entryBlock).parameters[index];
        return subject;
    }
    uint32_t firstStatement = m_input.ranges[function.index()].statement;
    for (const ValueFlowStatement& statement : m_input.statements.subspan(firstStatement, m_input.ranges[function.index() + 1].statement - firstStatement)) {
        if (statement.result == node && statement.function == function) {
            subject.value = statement.site;
            break;
        }
    }
    return subject;
}

void ValueFlowSolver::report()
{
    if (!m_input.build || !m_input.reportsToBuild)
        return;
    BuildContext& build = *m_input.build;
    for (size_t index : m_statementsWithMembersNotToldApart) {
        ValueFlowStatement statement = copyOfStatementAt(ValueFlowStatementID(index));
        build.gaveUp(LimitID::MaxValueFlowMembersToldApart, Subject { .function = statement.function, .value = statement.site });
    }
    build.statistics().count(hitsOf(LimitID::MaxValueFlowSetMembers), m_counters.nodesSaturated);
    build.statistics().count(StatisticID::LayoutsOutsideFamily, m_counters.nodesHoldingObjectsOutsideFamily);
    build.statistics().count(StatisticID::FlowEdgesLogged, m_counters.edgesLogged);

    if (!build.validates())
        return;
    for (const ObjectOutsideFamily& entry : m_objectsOutsideFamilies.span().first(std::min<size_t>(m_objectsOutsideFamilies.size(), mostNodesReportedOutsideFamily))) {
        Subject subject = subjectOf(entry.node);
        build.diagnostics().add(InternalError {
            .kind = InternalErrorKind::MaskCutKnownValue,
            .function = subject.function,
            .value = subject.value,
            .layout = m_input.universe ? m_input.universe->layoutOf(entry.object) : LayoutID(),
            .pass = PassID::AnalyzeFlow,
            .detail = describeWayOf(entry),
        });
    }
}

String ValueFlowSolver::describeWayOf(const ObjectOutsideFamily& entry) const
{
    AOT_THRESHOLD(size_t, mostValuesNamed, 12);
    StringPrintStream out;
    out.print("n", entry.node.index(), ":#", entry.object.index());
    size_t numberOfValuesNamed = 0;
    for (ValueFlowNodeID node : explain(entry.node, entry.object)) {
        Subject subject = subjectOf(node);
        if (!subject.function || !subject.value)
            continue;
        if (++numberOfValuesNamed > mostValuesNamed) {
            out.print(" ..");
            break;
        }
        out.print(numberOfValuesNamed == 1 ? "; from " : " to ", "f", subject.function.index(), ":v", subject.value.index());
    }
    return out.toString();
}

void ValueFlowSolver::solve()
{
    if (m_input.build)
        m_seedOfSchedule = m_input.build->recordedOptions().has(TestingSwitch::DelayFlowRules) + 2 * m_input.build->recordedOptions().has(TestingSwitch::DelayOtherFlowRules) + 4 * m_input.build->recordedOptions().numberOfFlowSchedule;
    m_stepTimes.isOn = m_input.build && m_input.reportsToBuild && m_input.build->options().logging.logsPhaseTimes;
    m_stepTimes.begin();
    if (m_input.program)
        m_roundThatTiedNamespace = std::make_unique<std::atomic<uint32_t>[]>(m_input.program->modules.size() + 1);
    m_graph.addNodes(m_input.numberOfNodes);
    runTasksInParallel(m_loops, {
        [&] {
            for (const ValueFlowNodeMask& mask : m_input.masks)
                m_graph.setMask(mask.node, mask.mask, mask.kind, mask.familyObjects);
            if (m_input.lost)
                m_graph.setMask(m_input.lost, ValueSets::complement(m_objectsAlwaysFollowed));
            if (m_input.nodes && m_input.universe) {
                m_graph.setMask(m_input.nodes->ofHost(ValueFlowHostNode::NamespacesCarriedOutAsValues), m_input.universe->builtinTables().heap().namespacesNotedWhenCarriedOut());
                m_graph.setMask(m_input.nodes->ofHost(ValueFlowHostNode::ClosuresThatDefinitionsCompare), m_input.universe->objectsThatDefinitionsTellApart());
                m_graph.setMask(m_input.nodes->ofHost(ValueFlowHostNode::PrototypeObjectsThatWritesUnderUnknownKeysReach), m_input.universe->builtinTables().heap().prototypeObjectsThatListingDoesNotName());
                m_graph.setMask(m_input.nodes->ofHost(ValueFlowHostNode::PrototypeObjectsOfLibraryClassesWhoseOwnKeysAreListed), m_input.universe->prototypeObjectsOfLibraryClasses());
                m_graph.setMask(m_input.nodes->ofHost(ValueFlowHostNode::LibraryModulesHandedOut), m_input.universe->holdersOfLibraryExportsThatRaiseNoEvent());
            }
        },
        [&] {
            m_graph.addInitialEdges(m_input.edges);
            if (m_input.nodes && m_input.universe && m_input.escaped) {
                ValueFlowNodeID carriedOut = m_input.nodes->ofHost(ValueFlowHostNode::ValuesCarriedOut);
                ValueFlowEdge onward[] { { carriedOut, m_input.escaped }, { carriedOut, m_input.nodes->ofHost(ValueFlowHostNode::NamespacesCarriedOutAsValues) } };
                m_graph.addInitialEdges(onward);
            }
            if (m_input.nodes && m_input.universe) {
                ValueFlowEdge onward[] { { m_input.nodes->ofHost(ValueFlowHostNode::PrototypeObjectsThatWritesUnderUnknownKeysReach), m_input.nodes->ofHost(ValueFlowHostNode::BuiltinsHeldWithoutBeingListed) } };
                m_graph.addInitialEdges(onward);
            }
        },
    });

    uint32_t numberOfBodies = m_input.ranges.size() - 1;
    for (uint32_t body = 0; body < numberOfBodies; ++body)
        makeStatementsDue(m_input.ranges[body].statement, m_input.ranges[body].statementLeavingBody);
    reach(numberOfBodies - 1, { });
    if (m_input.escaped) {
        m_madeNodesOfStatement.append(noSolverIndex);
        growByDoubling(m_isDue, numberOfStatements());
        makeDue(ValueFlowStatementID(numberOfStatements() - 1));
    }

    if (m_input.nodes && m_input.universe) {
        m_objectsThatAreNotPlain = makeNodes(ValueFlowNodeKey::objectsThatAreNotPlain());
        m_objectsWrittenUnderUnknownKey = makeNodes(ValueFlowNodeKey::objectsWrittenUnderUnknownKey());
        for (unsigned write = 0; write < numberOfValueFlowRareWrites; ++write) {
            ValueFlowNodeID node = makeNodes(ValueFlowNodeKey::objectsWithRareWrite(static_cast<ValueFlowRareWrite>(write)));
            if (!write)
                m_firstOfObjectsWithRareWrite = node;
            RELEASE_ASSERT(node.index() == m_firstOfObjectsWithRareWrite.index() + write);
        }
    }

    findStatementsWithLongRowsBehindThem();

    Vector<ValueSetID> heldAtRest;
    m_sets.keepSetsMadeSoFar();
    m_bytesOfSetsBehindLastFreeing = m_sets.bytesOfSetsMade();
    if (m_input.universe)
        m_input.universe->builtinTables().heap().keepEverySetMadeFromNowOn();
    m_freesSetsEveryRound = Options::freeAOTFlowSetsEveryRound();
    Vector<OptionSet<BuiltinUse>> usesAtRest;
    uint32_t functionsReachedAtRest = 0;

    Vector<ValueFlowNodeID> moved;
    bool isFullRound = m_rounds == Rounds::Full;
    bool didMuchInRound = false;
    bool didMuchInRoundBefore = false;
    m_stepTimes.didFinish("in front of the rounds"_s);
    for (;;) {
        ++m_counters.rounds;
        m_counters.fullRounds += isFullRound;
        m_isFullRound = isFullRound;
        Counters before = m_counters;
        ValueFlowGraph::Counters graphBefore = m_graph.counters();
        size_t numberOfEdgesBefore = m_graph.numberOfEdges();

        MonotonicTime start = MonotonicTime::now();
        m_stepTimes.begin();
        m_numberOfNodesBeforeRound = m_numberOfNodesMade;
        if (isFullRound)
            m_due.appendVector(std::exchange(m_statementsThatRunOften, { }));
        else
            letStatementsThatRunOftenWait();
        if (m_due.isEmpty() && m_dueBehindLongRows.isEmpty()) {
            for (ValueFlowStatementID statement : std::exchange(m_dueLast, { })) {
                if (m_isDueLast.clear(statement.index()))
                    makeDue(statement);
            }
        }
        if (m_seedOfSchedule && !isFullRound)
            letSomeDueStatementsWait();
        if (m_hasLongRows && !isFullRound)
            letLongRowsGoFirst();
        runRules();
        MonotonicTime afterRules = MonotonicTime::now();
        bool hasAdded = merge();
        MonotonicTime afterMerge = MonotonicTime::now();
        // There are some 25,000 lists of one round, and programs with thousands of rounds of a few statements each.
        auto listsMayHaveMuchRoom = [&] {
            constexpr uint64_t workThatFillsLists = 1 << 15;
            uint64_t work = m_counters.statementRuns - before.statementRuns + m_graph.counters().evaluations - graphBefore.evaluations + m_graph.numberOfEdges() - numberOfEdgesBefore;
            didMuchInRound |= work >= workThatFillsLists;
            return didMuchInRound || didMuchInRoundBefore;
        };
        if (listsMayHaveMuchRoom())
            giveBackRoomOfEmptyLists(ListsOfLinks::AreInUse);
        m_graph.solve(moved, ValueFlowGraph::RetiredNodes::AreFoundAndConnectedByCaller);
        if (listsMayHaveMuchRoom())
            giveBackRoomOfEmptyLists(ListsOfLinks::AreInUse);
        m_stepTimes.didFinish("values: the graph solved"_s);
        if (m_input.escaped && m_input.lost && m_input.placeholders.untracked && !m_haveLostObjectsEscaped && m_sets.contains(m_graph.valueOf(m_input.escaped), m_input.placeholders.untracked)) {
            m_haveLostObjectsEscaped = true;
            m_edgesOfRound.append(ValueFlowEdge { m_input.lost, m_input.escaped });
        }
        bool coveringsHaveChanged = false;
        for (Covering& covering : m_coverings) {
            ValueSetID before = std::exchange(covering.allButCovered, ValueSets::complement(m_makers[0]->intersect(m_graph.valueOf(covering.covered), covering.objectsThatMayBeCovered)));
            coveringsHaveChanged |= before != covering.allButCovered;
        }
        // The count saturates at its maximum, which no node remembers.
        if (coveringsHaveChanged && m_stateOfCoverings < UINT16_MAX)
            ++m_stateOfCoverings;
        growAndFill(m_coveringStateOfShownView, m_readersOfNode.size(), static_cast<uint16_t>(UINT16_MAX));
        m_graph.takeNodesAtWhichPlaceholdersStopped(m_placeholdersStopped);
        for (auto [node, placeholder] : m_placeholdersStopped) {
            ValueSetID taken = m_makers[0]->intersect(m_graph.familyObjectsOf(node), m_graph.maskOf(node));
            if (taken == ValueSets::nothing)
                continue;
            m_edgesOfRound.append(ValueFlowEdge { ensureNodes(ValueFlowNodeKey::masked(placeholder == m_input.placeholders.unknown ? m_input.escaped : m_input.lost, taken)), node });
        }
        m_stepTimes.didFinish("values: placeholders"_s);
        runTasksInParallel(m_loops, {
            [&] {
                makeReadersDue(moved.span());
            },
            [&] {
                m_graph.retireNodesAmong(moved.span());
            },
        });
        m_stepTimes.didFinish("values: readers made due, beside nodes retired"_s);
        if (m_saturates)
            saturate(moved.span());
        m_stepTimes.didFinish("values: saturation"_s);
        if (RefPtr<BackgroundTask> task = std::exchange(m_insertionOfNodesThatRead, nullptr))
            task->finish();
        if (listsMayHaveMuchRoom())
            giveBackRoomOfEmptyLists(ListsOfLinks::AreLookedAtToo);
        didMuchInRoundBefore = std::exchange(didMuchInRound, false);
        m_stepTimes.didFinish("values: nodes that read listed"_s);
        m_counters.rules += afterRules - start;
        m_counters.merge += afterMerge - afterRules;
        m_counters.values += MonotonicTime::now() - afterMerge;
        if (m_stepTimes.isOn)
            dataLogLn("AOT flow round ", m_counters.rounds, " | rules ", (afterRules - start).milliseconds(), " ms | merge ", (afterMerge - afterRules).milliseconds(), " ms | values ", (MonotonicTime::now() - afterMerge).milliseconds(), " ms | turns ", m_turnsOfRound, " | statement runs ", m_counters.statementRuns - before.statementRuns, " | nodes ", m_numberOfNodesMade - m_numberOfNodesBeforeRound, " | edges ", m_graph.numberOfEdges() - numberOfEdgesBefore, " | moved ", moved.size(), " | waves ", m_graph.counters().waves - graphBefore.waves, " | evaluations ", m_graph.counters().evaluations - graphBefore.evaluations, " | resident ", PhaseTimes::residentMegabytes(), " MB");

        if (m_stepTimes.isOn) {
            // A round of few statements lasts as long as its longest one, however many threads there are.
            const Thread* slowest = nullptr;
            for (const std::unique_ptr<Thread>& thread : m_threads) {
                if (!slowest || thread->longestStatementInRound > slowest->longestStatementInRound)
                    slowest = thread.get();
            }
            if (slowest && afterRules - start >= 1_ms)
                dataLogLn("AOT flow longest statement of round ", m_counters.rounds, " | ", slowest->longestStatementInRound.milliseconds(), " ms of ", (afterRules - start).milliseconds(), " | kind ", static_cast<unsigned>(slowest->kindOfLongestStatementInRound), " | opcode ", static_cast<unsigned>(slowest->opcodeOfLongestStatementInRound));
            for (const std::unique_ptr<Thread>& thread : m_threads)
                thread->longestStatementInRound = 0_s;
        }

        m_roundsDone.append(Round {
            isFullRound,
            m_turnsOfRound,
            static_cast<uint32_t>(m_counters.statementRuns - before.statementRuns),
            m_counters.functionsReached - before.functionsReached,
            m_numberOfNodesMade - m_numberOfNodesBeforeRound,
            static_cast<uint32_t>(m_graph.numberOfEdges() - numberOfEdgesBefore),
            static_cast<uint32_t>(moved.size()),
            static_cast<uint32_t>(m_graph.counters().waves - graphBefore.waves),
            static_cast<uint32_t>(m_graph.counters().evaluations - graphBefore.evaluations),
        });
        giveBackRoomOfLargerRounds(m_roundsDone.last().statementRuns + static_cast<uint64_t>(m_roundsDone.last().edgesAdded) + m_roundsDone.last().evaluations);
        if (heldAtRest.isEmpty())
            freeSetsThatNobodyHolds();

        bool isQuiet = !hasAdded && moved.isEmpty() && m_due.isEmpty() && m_statementsThatRunOften.isEmpty() && m_dueBehindLongRows.isEmpty() && m_dueLast.isEmpty() && m_dueStatementsThatWait.isEmpty() && m_edgesOfRound.isEmpty() && m_readToLink.isEmpty() && m_writtenToLink.isEmpty() && m_bodiesReachedInRound.isEmpty();
        if (ValueSetID proxies = isQuiet ? followedProxiesThatMoreMayBeDoneTo() : ValueSets::nothing; proxies != ValueSets::nothing) {
            m_edgesOfRound.append(ValueFlowEdge { ensureNodes(ValueFlowNodeKey::constant(proxies)), m_input.nodes->ofHost(ValueFlowHostNode::ProxiesThatAreOpen) });
            continue;
        }
        if (isQuiet && !m_hasSaidThatAllElseIsAtRest && m_input.nodes && m_input.placeholders.unknown) {
            m_hasSaidThatAllElseIsAtRest = true;
            m_edgesOfRound.append(ValueFlowEdge { ensureNodes(ValueFlowNodeKey::constant(m_makers[0]->of(m_input.placeholders.unknown))), m_input.nodes->ofHost(ValueFlowHostNode::AllElseIsAtRest) });
            continue;
        }
        if (isQuiet && wakeReadersOfNodesThatShowSomethingElse())
            continue;
        if (isQuiet && isFullRound && m_rounds == Rounds::Due && !m_usesGeneralRulesOnly) {
            m_usesGeneralRulesOnly = true;
            heldAtRest.appendUsingFunctor(m_graph.numberOfNodes(), [&](size_t index) {
                return m_graph.valueOf(ValueFlowNodeID(static_cast<uint32_t>(index)));
            });
            for (uint32_t index = 0; index < heldAtRest.size(); ++index) {
                // A call group only hands on what it takes, and which callees share one hangs on what arrived together. The parameters that it feeds are compared.
                if (m_graph.isAtRest(ValueFlowNodeID(index)) || (index >= m_input.numberOfNodes && (m_madeNodesOfNode[index - m_input.numberOfNodes] == noSolverIndex || keyOfMadeNode(ValueFlowNodeID(index))->kind == ValueFlowMadeNodeKind::CallGroup)))
                    heldAtRest[index] = ValueSetID();
            }
            usesAtRest = m_usesOfBuiltin;
            functionsReachedAtRest = m_counters.functionsReached;
            isFullRound = false;
            makeEveryStatementDue();
            continue;
        }
        if ((isQuiet && (isFullRound || m_rounds == Rounds::DueWithoutCheck)) || m_counters.rounds >= m_input.mostRounds)
            break;
        isFullRound = isQuiet || m_rounds == Rounds::Full;
        if (isFullRound)
            makeEveryStatementDue();
    }
    if (m_usesGeneralRulesOnly) {
        AOT_THRESHOLD(size_t, mostNodesNamed, 8);
        for (uint32_t index = 0; index < heldAtRest.size(); ++index) {
            if (!heldAtRest[index] || m_graph.valueOf(ValueFlowNodeID(index)) == heldAtRest[index])
                continue;
            if (m_addedByGeneralRules.nodesThatHoldMore++ < mostNodesNamed)
                m_addedByGeneralRules.firstNodesThatHoldMore.append(ValueFlowNodeID(index));
        }
        m_addedByGeneralRules.functionsReached = m_counters.functionsReached - functionsReachedAtRest;
        for (size_t index = 0; index < m_usesOfBuiltin.size(); ++index)
            m_addedByGeneralRules.builtinsUsedMore += index < usesAtRest.size() ? m_usesOfBuiltin[index] != usesAtRest[index] : !m_usesOfBuiltin[index].isEmpty();
    }
    if (m_stepTimes.isOn) {
        dumpBytesByTable();
        // The two tables count every reader, use, record and edge on one thread: 8 s for 9 million statements, in a build that is being timed.
        bool countsReadersAndEdges = m_input.build->options().logging.dumpedPass == PassID::AnalyzeFlow;
        if (countsReadersAndEdges)
            dumpReadersByStatement();
        dataLogLn("AOT flow totals at rest | edges ", m_graph.numberOfEdges(), " | edges logged ", m_counters.edgesLogged, " | readers ", numberOfReaders(), " | statements ", numberOfStatements(), " | nodes ", m_graph.numberOfNodes());
        if (countsReadersAndEdges)
            dumpEdgesByEnds();
    }
    m_stepTimes.begin();
    fillNumbersThatNobodyGave();
    m_stepTimes.didFinish("behind the rounds: numbers that nobody gave"_s);
    for (const NodesOfManyObjects& nodes : m_nodesOfManyObjects) {
        for (const Vector<ValueSetID>& cells : nodes.cells.values())
            m_counters.namesWithSeveralCells += cells.size() > 1;
    }
    {
        auto release = [&](auto& state) {
            auto taken = std::exchange(state, { });
            if (m_input.build)
                m_input.build->destroyLater(WTF::move(taken));
        };
        auto releaseInOrder = [&](auto&... states) {
            (release(states), ...);
        };
        releaseInOrder(m_threads, m_readerShards, m_watchShards, m_firstSetWatchedForCoverOfMember, m_setsWatchedForCoverOfMembers, m_isWatched, m_listenersOfNode, m_readersOfFunction, m_valuesShownToStatement, m_readersOfWorker, m_newMembersOfWorker, m_watchesToTestOfWorker, m_newMembers, m_nodesOfManyObjects, m_rememberedByStatement, m_linkShards, m_edgesOfRound, m_edgesInFrontOfLinks, m_readToLink, m_writtenToLink,
            m_readersOfNode, m_coveringStateOfShownView, m_names, m_numberOfName, m_kindsOfNames, m_partsOfMadeNodes, m_layoutOrCalleeOfMember, m_lastCountThatSaw, m_isTooLarge,
            m_firstStatementWaitingForNode, m_nextStatementWaiting, m_timesRunOfStatement, m_roundOfNextRun, m_isDue, m_isDueLast, m_hasRunInTurn, m_hasLongRowBehindIt, m_isDear, m_usesOfThreads, m_uncoveredAtLastReaderWake, m_readToInsert);
    }
    m_stepTimes.didFinish("behind the rounds: what only the rounds read let go of"_s);
    if (m_allButMarks != ValueSets::anything) {
        m_graph.removeFromEveryNode({ ValueFlowUniverse::markOfNamedBun(), ValueFlowUniverse::markOfMemberLeftOut(), ValueFlowUniverse::markOfLoadedBun() });
    }
    m_stepTimes.didFinish("behind the rounds: marks removed from every node"_s);
    m_graph.releaseRows();
    m_stepTimes.didFinish("behind the rounds: rows released"_s);
    if (m_input.leavesOutWhatIsCovered)
        leaveOutWhatIsCoveredInTheEnd();
    m_stepTimes.didFinish("behind the rounds: what is covered left out"_s);
    spreadMarks();
    m_stepTimes.didFinish("behind the rounds: marks spread"_s);
    findObjectsOutsideFamilies();
    m_stepTimes.didFinish("behind the rounds: objects outside families"_s);
    findClosuresFollowedToConstructions();
    m_stepTimes.didFinish("behind the rounds: closures followed to constructions"_s);
    auto dumpStepTimes = makeScopeExit([&] {
        m_stepTimes.didFinish("behind the rounds: unknown sources, beside the release of the solving state"_s);
        m_stepTimes.dump();
        if (m_stepTimes.isOn) {
            dataLogLn("AOT flow statements | ", numberOfStatements(), " | of them made by rules ", m_madeNodesOfStatement.size());
            forEachKindOfStatementRun([&](ValueFlowStatementKind kind, Opcode opcode, const RunsAndEdges& row) {
                dataLogLn("AOT flow runs | ", kind == ValueFlowStatementKind::Operation ? nameOf(opcode) : "kind"_s, " ", static_cast<unsigned>(kind), " | runs ", row.runs, " | added nothing ", row.runsThatAddedNothing, " | connections ", row.connections, " | edges logged ", row.edgesLogged);
            });
        }
    });
    m_isStatementMadeByRuleAtRest.ensureSize(m_madeNodesOfStatement.size());
    for (size_t index = 0; index < m_madeNodesOfStatement.size(); ++index) {
        if (isAtRest(ValueFlowStatementID(static_cast<uint32_t>(m_input.statements.size() + index))))
            m_isStatementMadeByRuleAtRest.quickSet(index);
    }
    m_areRoundsOver = true;
    m_statementsMadeByRules.grow(m_madeNodesOfStatement.size());
    constexpr size_t statementsPerShare = 1 << 14;
    forEachIndexInParallel(m_loops, (m_statementsMadeByRules.size() + statementsPerShare - 1) / statementsPerShare, [&](unsigned share) {
        for (size_t index = share * statementsPerShare; index < std::min(m_statementsMadeByRules.size(), (share + 1) * statementsPerShare); ++index)
            m_statementsMadeByRules[index] = statementMadeByRuleAt(ValueFlowStatementID(static_cast<uint32_t>(m_input.statements.size() + index)));
    });
    findUnknownSources([&] {
        report();
        m_graph.releaseSolvingState(m_input.isAskedHowValuesCame || m_input.remembersWhoLoggedEachEdge ? ValueFlowGraph::Predecessors::AreKept : ValueFlowGraph::Predecessors::AreReleased);
        releaseIndexOfMadeNodes();
    });
}

// Edges by the kinds of their two ends, and how many of them the names with the most take.
void ValueFlowSolver::dumpEdgesByEnds() const
{
    constexpr unsigned kinds = 11;
    constexpr unsigned ends = kinds + 1;
    static constexpr std::array<ASCIILiteral, ends> names { "Constant"_s, "Read"_s, "Written"_s, "Lookup"_s, "CallGroup"_s, "Hub"_s, "Masked"_s, "Marked"_s, "WrittenUnderKind"_s, "ReadUnderKind"_s, "Instance"_s, "Body"_s };
    auto recordOf = [&](ValueFlowNodeID node) {
        return node.index() < m_input.numberOfNodes ? noSolverIndex : m_madeNodesOfNode[node.index() - m_input.numberOfNodes];
    };
    auto endOf = [&](uint32_t record) {
        return record == noSolverIndex ? kinds : static_cast<unsigned>(m_madeNodes[record].key.kind);
    };
    std::array<uint64_t, ends> nodes { };
    std::array<std::array<uint64_t, ends>, ends> edges { };
    HashMap<uint64_t, uint64_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> edgesOfName;
    uint64_t largestFanOut = 0;
    for (uint32_t index = 0; index < m_graph.numberOfNodes(); ++index) {
        uint32_t record = recordOf(ValueFlowNodeID(index));
        unsigned from = endOf(record);
        ++nodes[from];
        uint64_t fanOut = 0;
        m_graph.forEachSuccessor(ValueFlowNodeID(index), [&](ValueFlowNodeID to) {
            ++edges[from][endOf(recordOf(to))];
            ++fanOut;
        });
        largestFanOut = std::max(largestFanOut, fanOut);
        if (from == static_cast<unsigned>(ValueFlowMadeNodeKind::Written) && fanOut) {
            const ValueFlowNodeKey& key = m_madeNodes[record].key;
            edgesOfName.add(static_cast<uint64_t>(key.detail) << 32 | key.payload, 0).iterator->value += fanOut;
        }
    }
    for (unsigned from = 0; from < ends; ++from) {
        dataLog("AOT flow edges | from ", names[from], " | nodes ", nodes[from], " | to");
        for (unsigned to = 0; to < ends; ++to) {
            if (edges[from][to])
                dataLog(" ", names[to], " ", edges[from][to]);
        }
        dataLogLn();
    }
    Vector<uint64_t> byName;
    for (uint64_t count : edgesOfName.values())
        byName.append(count);
    std::ranges::sort(byName, std::greater { });
    dataLog("AOT flow edges | largest fan-out ", largestFanOut, " | names written ", byName.size(), " | edges out of what is written under the names with the most");
    for (uint64_t count : byName.span().first(std::min<size_t>(byName.size(), 16)))
        dataLog(" ", count);
    dataLogLn();
}

void ValueFlowSolver::dumpReadersByStatement() const
{
    Vector<uint32_t> readersOfStatement;
    readersOfStatement.fill(0, numberOfStatements());
    auto count = [&](const auto& readers) {
        for (const Reader& reader : readers)
            ++readersOfStatement[reader.statement.index()];
    };
    for (const WatchShard& shard : m_watchShards) {
        for (const Watch& watch : shard.watches) {
            for (uint32_t readers : { watch.readers, watch.readersAskingLast }) {
                shard.readersOfWatches.forEach(readers, [&](uint32_t statement) {
                    ++readersOfStatement[statement];
                });
            }
        }
    }
    for (const ReaderShard& shard : m_readerShards)
        count(shard.readersWaitingForSomething);
    for (uint32_t index = 0; index < m_readersOfNode.size(); ++index) {
        forEachReaderOf(ValueFlowNodeID(index), [&](ValueFlowStatementID reader) {
            ++readersOfStatement[reader.index()];
        });
    }

    struct Row {
        uint64_t statements { 0 };
        uint64_t readers { 0 };
    };
    HashMap<uint32_t, Row, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> rows;
    Vector<uint32_t> byReaders;
    for (uint32_t index = 0; index < readersOfStatement.size(); ++index) {
        if (!readersOfStatement[index])
            continue;
        ValueFlowStatement statement = copyOfStatementAt(ValueFlowStatementID(index));
        Row& row = rows.add(static_cast<uint32_t>(statement.kind) << 16 | static_cast<uint32_t>(statement.opcode), Row { }).iterator->value;
        ++row.statements;
        row.readers += readersOfStatement[index];
        byReaders.append(index);
    }
    for (const auto& entry : rows) {
        auto kind = static_cast<ValueFlowStatementKind>(entry.key >> 16);
        dataLogLn("AOT flow readers | ", kind == ValueFlowStatementKind::Operation ? nameOf(static_cast<Opcode>(entry.key & 0xffff)) : "kind"_s, " ", static_cast<unsigned>(kind), " | statements ", entry.value.statements, " | readers ", entry.value.readers);
    }
    std::sort(byReaders.begin(), byReaders.end(), [&](uint32_t left, uint32_t right) { return readersOfStatement[left] > readersOfStatement[right]; });
    for (uint32_t index : byReaders.span().first(std::min<size_t>(byReaders.size(), 24))) {
        ValueFlowStatement statement = copyOfStatementAt(ValueFlowStatementID(index));
        dataLogLn("AOT flow readers of one statement | ", readersOfStatement[index], " | kind ", static_cast<unsigned>(statement.kind), " | opcode ", nameOf(statement.opcode), " | function ", statement.function.index());
    }
    // Which kinds of nodes the statements read, and for a lookup under which kind of name and of how many receivers.
    constexpr unsigned kindsOfNodes = 12;
    static constexpr std::array<ASCIILiteral, kindsOfNodes> namesOfNodes { "Constant"_s, "Read"_s, "Written"_s, "Lookup"_s, "CallGroup"_s, "Hub"_s, "Masked"_s, "Marked"_s, "WrittenUnderKind"_s, "ReadUnderKind"_s, "Instance"_s, "Body"_s };
    auto kindOfNode = [&](ValueFlowNodeID node) -> unsigned {
        if (node.index() < m_input.numberOfNodes)
            return kindsOfNodes - 1;
        uint32_t record = m_madeNodesOfNode[node.index() - m_input.numberOfNodes];
        return record == noSolverIndex ? kindsOfNodes - 1 : static_cast<unsigned>(m_madeNodes[record].key.kind);
    };
    using ByNode = std::array<uint64_t, kindsOfNodes>;
    HashMap<uint32_t, std::array<ByNode, 2>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> readersByNode;
    auto noteReader = [&](ValueFlowStatementID reader, ValueFlowNodeID node, bool isWatch) {
        ValueFlowStatement statement = copyOfStatementAt(reader);
        ++readersByNode.add(static_cast<uint32_t>(statement.kind) << 16 | static_cast<uint32_t>(statement.opcode), std::array<ByNode, 2> { }).iterator->value[isWatch][kindOfNode(node)];
    };
    for (uint32_t index = 0; index < m_readersOfNode.size(); ++index) {
        forEachReaderOf(ValueFlowNodeID(index), [&](ValueFlowStatementID reader) {
            noteReader(reader, ValueFlowNodeID(index), false);
        });
    }
    for (const WatchShard& shard : m_watchShards) {
        for (const Watch& watch : shard.watches) {
            for (uint32_t readers : { watch.readers, watch.readersAskingLast }) {
                shard.readersOfWatches.forEach(readers, [&](uint32_t statement) {
                    noteReader(ValueFlowStatementID(statement), shard.watchedNodes[watch.watchedNode].node, true);
                });
            }
        }
    }
    for (const auto& entry : readersByNode) {
        uint64_t all = 0;
        for (const ByNode& row : entry.value) {
            for (uint64_t number : row)
                all += number;
        }
        if (all < numberOfReaders() / 200)
            continue;
        auto kind = static_cast<ValueFlowStatementKind>(entry.key >> 16);
        dataLog("AOT flow readers by node | ", kind == ValueFlowStatementKind::Operation ? nameOf(static_cast<Opcode>(entry.key & 0xffff)) : "kind"_s, " ", static_cast<unsigned>(kind));
        for (unsigned isWatch = 0; isWatch < 2; ++isWatch) {
            dataLog(isWatch ? " | watching" : " | reading");
            for (unsigned node = 0; node < kindsOfNodes; ++node) {
                if (entry.value[isWatch][node])
                    dataLog(" ", namesOfNodes[node], " ", entry.value[isWatch][node]);
            }
        }
        dataLogLn();
    }
    HashMap<uint32_t, ByNode, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> usesByRecord;
    uint64_t numberOfUses = 0;
    uint64_t numberOfDistinctUses = 0;
    uint64_t longestList = 0;
    Vector<uint32_t> recordsOfStatement;
    Vector<uint32_t> usersOfRecord;
    usersOfRecord.fill(0, m_madeNodes.size());
    for (uint32_t index = 0; index < m_usesOfStatement.size(); ++index) {
        if (m_usesOfStatement[index] == noSolverIndex)
            continue;
        ValueFlowStatement statement = copyOfStatementAt(ValueFlowStatementID(index));
        ByNode& row = usesByRecord.add(static_cast<uint32_t>(statement.kind) << 16 | static_cast<uint32_t>(statement.opcode), ByNode { }).iterator->value;
        recordsOfStatement.shrink(0);
        forEachRecordAskedForBy(index, [&](uint32_t madeNodes) {
            ++row[static_cast<unsigned>(m_madeNodes[madeNodes].key.kind)];
            ++numberOfUses;
            recordsOfStatement.append(madeNodes);
        });
        AOT::sort(recordsOfStatement);
        removeRepeatedElements(recordsOfStatement);
        numberOfDistinctUses += recordsOfStatement.size();
        longestList = std::max<uint64_t>(longestList, recordsOfStatement.size());
        for (uint32_t record : recordsOfStatement)
            ++usersOfRecord[record];
    }
    dataLogLn("AOT flow uses | all ", numberOfUses, " | without repeats in one statement ", numberOfDistinctUses, " | most of one statement ", longestList, " | records ", m_madeNodes.size());
    // Records by kind: how many there are, how many statements ask for each, how many objects they are asked of, and how many hold nothing at rest.
    struct RecordsOfKind {
        uint64_t all { 0 };
        std::array<uint64_t, 4> byUsers { };
        std::array<uint64_t, 4> byObjects { };
        uint64_t holdingNothing { 0 };
        uint64_t withStatement { 0 };
        uint64_t atRest { 0 };
        uint64_t nodes { 0 };
        uint64_t nodesAtRest { 0 };
        uint64_t edges { 0 };
        uint64_t edgesAtRest { 0 };
    };
    std::array<RecordsOfKind, kindsOfNodes> recordsOfKind { };
    for (uint32_t index = 0; index < m_madeNodes.size(); ++index) {
        const MadeNodes& made = m_madeNodes[index];
        RecordsOfKind& row = recordsOfKind[static_cast<unsigned>(made.key.kind)];
        ++row.all;
        uint32_t users = usersOfRecord[index];
        ++row.byUsers[!users ? 0 : users == 1 ? 1 : users <= 4 ? 2 : 3];
        uint64_t objects = ValueSets::isComplement(made.key.set) ? UINT64_MAX : m_sets.size(made.key.set);
        ++row.byObjects[objects <= 1 ? 0 : objects <= 16 ? 1 : objects <= 1024 ? 2 : 3];
        row.holdingNothing += made.first && m_graph.valueOf(made.first) == ValueSets::nothing;
        row.withStatement += !!made.statement();
        bool isAtRest = !WTF::atomicLoad(const_cast<unsigned*>(&m_numberOfUsers[index]), std::memory_order_relaxed);
        uint64_t edges = 0;
        // Constants, marks and instances are not counted by their users, and a constant may have very many successors.
        bool isCountedByUsers = made.key.kind != ValueFlowMadeNodeKind::Constant && made.key.kind != ValueFlowMadeNodeKind::Marked && made.key.kind != ValueFlowMadeNodeKind::Instance;
        isAtRest = isAtRest && isCountedByUsers;
        for (uint32_t offset = 0; isCountedByUsers && made.first && offset < made.key.numberOfNodes(); ++offset) {
            ValueFlowNodeID node(made.first.index() + offset);
            m_graph.forEachSuccessor(node, [&](ValueFlowNodeID) { ++edges; });
            m_graph.forEachPredecessor(node, [&](ValueFlowNodeID) { ++edges; });
        }
        row.nodes += made.key.numberOfNodes();
        row.edges += edges;
        if (isAtRest) {
            ++row.atRest;
            row.nodesAtRest += made.key.numberOfNodes();
            row.edgesAtRest += edges;
        }
    }
    for (unsigned kind = 0; kind < kindsOfNodes; ++kind) {
        const RecordsOfKind& row = recordsOfKind[kind];
        if (!row.all)
            continue;
        dataLogLn("AOT flow records | ", namesOfNodes[kind], " ", row.all, " | asked for by 0: ", row.byUsers[0], " 1: ", row.byUsers[1], " 2-4: ", row.byUsers[2], " more: ", row.byUsers[3],
            " | asked of objects 0-1: ", row.byObjects[0], " 2-16: ", row.byObjects[1], " 17-1024: ", row.byObjects[2], " more: ", row.byObjects[3], " | holding nothing ", row.holdingNothing, " | with a statement ", row.withStatement,
            " | with no user left ", row.atRest, " | nodes ", row.nodes, ", of records with no user ", row.nodesAtRest, " | ends of edges at them ", row.edges, ", of records with no user ", row.edgesAtRest);
    }
    for (const auto& entry : usesByRecord) {
        uint64_t all = 0;
        for (uint64_t number : entry.value)
            all += number;
        if (all < numberOfUses / 200)
            continue;
        auto kind = static_cast<ValueFlowStatementKind>(entry.key >> 16);
        dataLog("AOT flow uses | ", kind == ValueFlowStatementKind::Operation ? nameOf(static_cast<Opcode>(entry.key & 0xffff)) : "kind"_s, " ", static_cast<unsigned>(kind), " | of records");
        for (unsigned node = 0; node < kindsOfNodes; ++node) {
            if (entry.value[node])
                dataLog(" ", namesOfNodes[node], " ", entry.value[node]);
        }
        dataLogLn();
    }
    // The member calls whose records take the most room, by the name of the member, with how many objects their receivers list.
    struct CallsUnderKey {
        uint64_t calls { 0 };
        uint64_t uses { 0 };
        uint64_t receivers { 0 };
        uint64_t mostReceivers { 0 };
        uint64_t callsBesideUnknownObject { 0 };
        uint64_t mostUses { 0 };
        uint32_t callWithMostUses { 0 };
    };
    HashMap<uint32_t, CallsUnderKey, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> callsUnderKey;
    for (uint32_t index = 0; m_input.program && index < std::min<size_t>(m_usesOfStatement.size(), m_input.statements.size()); ++index) {
        const ValueFlowStatement& statement = m_input.statements[index];
        if (statement.kind != ValueFlowStatementKind::Operation || statement.opcode != Opcode::Call || !statement.key || m_usesOfStatement[index] == noSolverIndex)
            continue;
        CallsUnderKey& row = callsUnderKey.add(statement.key.index(), CallsUnderKey { }).iterator->value;
        ++row.calls;
        uint64_t usesOfCall = 0;
        forEachRecordAskedForBy(index, [&](uint32_t) {
            ++usesOfCall;
        });
        row.uses += usesOfCall;
        if (usesOfCall > row.mostUses) {
            row.mostUses = usesOfCall;
            row.callWithMostUses = index;
        }
        ValueFlowNodeID receiver = operandsOf(statement)[1].node;
        ValueSetID listed = receiver ? m_graph.valueOf(receiver) : ValueSets::nothing;
        uint64_t size = ValueSets::isComplement(listed) ? 0 : m_sets.size(listed);
        row.receivers += size;
        row.mostReceivers = std::max(row.mostReceivers, size);
        row.callsBesideUnknownObject += m_sets.contains(listed, m_input.placeholders.unknown);
    }
    // What the large sets of receivers hold, and how many calls ask the same question: the same receivers under the same name.
    if (m_input.program && m_input.universe) {
        constexpr unsigned kindsOfObjects = 16;
        std::array<uint64_t, kindsOfObjects> membersOfKind { };
        std::array<uint64_t, kindsOfObjects> membersOfKindInLibrary { };
        HashMap<uint32_t, uint64_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> callsOfSet;
        HashSet<uint64_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> questions;
        uint64_t memberCalls = 0;
        uint64_t callsOfLargeSets = 0;
        uint64_t callsOfLargeSetsInLibrary = 0;
        for (uint32_t index = 0; index < m_input.statements.size(); ++index) {
            const ValueFlowStatement& statement = m_input.statements[index];
            if (statement.kind != ValueFlowStatementKind::Operation || statement.opcode != Opcode::Call || !statement.key || !isReached(statement.function))
                continue;
            ++memberCalls;
            ValueFlowNodeID receiver = operandsOf(statement)[1].node;
            ValueSetID listed = receiver ? m_graph.valueOf(receiver) : ValueSets::nothing;
            if (ValueSets::isComplement(listed) || m_sets.size(listed) <= m_input.mostMembersToldApart)
                continue;
            ++callsOfLargeSets;
            callsOfLargeSetsInLibrary += isLibraryCode(*m_input.program, m_input.program->at(statement.function));
            questions.add(static_cast<uint64_t>(listed.index()) << 32 | statement.key.index());
            ++callsOfSet.add(listed.index(), 0).iterator->value;
        }
        for (const auto& entry : callsOfSet) {
            m_sets.forEachMember(ValueSetID(entry.key), [&](ValueFlowObjectID object) {
                unsigned kind = std::min<unsigned>(static_cast<unsigned>(m_input.universe->kindOf(object)), kindsOfObjects - 1);
                membersOfKind[kind] += entry.value;
                FunctionID closureFunction = m_input.universe->closureFunction(object);
                FunctionID maker = closureFunction ? closureFunction : m_input.universe->functionOf(object);
                if (maker && isLibraryCode(*m_input.program, m_input.program->at(maker)))
                    membersOfKindInLibrary[kind] += entry.value;
            });
        }
        dataLogLn("AOT flow large receivers | member calls ", memberCalls, " | of them with more receivers than are told apart ", callsOfLargeSets, " | of those in library code ", callsOfLargeSetsInLibrary, " | different sets ", callsOfSet.size(), " | different pairs of set and name ", questions.size());
        dataLog("AOT flow large receivers | members by kind of object, each counted once a call: all (made by library code)");
        for (unsigned kind = 0; kind < kindsOfObjects; ++kind) {
            if (membersOfKind[kind])
                dataLog(" | kind ", kind, ": ", membersOfKind[kind], " (", membersOfKindInLibrary[kind], ")");
        }
        dataLogLn();
        Vector<std::pair<uint64_t, uint32_t>> setsByCalls;
        for (const auto& entry : callsOfSet)
            setsByCalls.append({ entry.value, entry.key });
        std::sort(setsByCalls.begin(), setsByCalls.end(), std::greater { });
        dataLog("AOT flow large receivers | the sets with the most calls, calls x members:");
        for (auto [calls, set] : setsByCalls.span().first(std::min<size_t>(setsByCalls.size(), 16)))
            dataLog(" ", calls, " x ", m_sets.size(ValueSetID(set)));
        dataLogLn();
    }
    Vector<uint32_t> keysByUses;
    for (uint32_t key : callsUnderKey.keys())
        keysByUses.append(key);
    std::sort(keysByUses.begin(), keysByUses.end(), [&](uint32_t left, uint32_t right) {
        return std::tuple(callsUnderKey.get(left).uses, left) > std::tuple(callsUnderKey.get(right).uses, right);
    });
    for (uint32_t key : keysByUses.span().first(std::min<size_t>(keysByUses.size(), 32))) {
        const CallsUnderKey& row = callsUnderKey.find(key)->value;
        const PropertyKey& name = m_input.program->at(PropertyKeyID(key));
        dataLog("AOT flow calls by member | ");
        if (name.kind == PropertyKeyKind::Text) {
            for (uint8_t character : m_input.program->string(name.text))
                dataLog(static_cast<char>(character));
        } else
            dataLog("(no text)");
        dataLogLn(" | calls ", row.calls, " | uses ", row.uses, " | receivers listed ", row.receivers, " | most at one call ", row.mostReceivers, " | calls whose receiver holds the unknown object ", row.callsBesideUnknownObject);
    }
    // What the call with the most records holds, for the first few of those names: records by kind, by how many objects they are asked of and by the kind of the first of these.
    for (uint32_t key : keysByUses.span().first(std::min<size_t>(keysByUses.size(), 6))) {
        if (!m_input.universe)
            break;
        const CallsUnderKey& row = callsUnderKey.find(key)->value;
        HashMap<uint64_t, uint64_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> recordsOfForm;
        HashSet<uint64_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> names;
        forEachRecordAskedForBy(row.callWithMostUses, [&](uint32_t number) {
            const ValueFlowNodeKey& record = m_madeNodes[number].key;
            uint64_t size = ValueSets::isComplement(record.set) ? 0xffffff : std::min<uint64_t>(m_sets.size(record.set), 0xfffffe);
            uint64_t kindOfFirst = 0xff;
            if (!ValueSets::isComplement(record.set)) {
                m_sets.anyMember(record.set, [&](ValueFlowObjectID object) {
                    kindOfFirst = static_cast<uint64_t>(m_input.universe->kindOf(object));
                    return true;
                });
            }
            ++recordsOfForm.add(static_cast<uint64_t>(record.kind) << 48 | static_cast<uint64_t>(record.detail) << 40 | kindOfFirst << 32 | size, 0).iterator->value;
            names.add(static_cast<uint64_t>(record.detail) << 32 | record.payload);
        });
        dataLog("AOT flow one call | member number ", key, " | records ", row.mostUses, " | different names or nodes ", names.size(), " | count x (record kind, detail, kind of first object, objects):");
        Vector<std::pair<uint64_t, uint64_t>> forms;
        for (const auto& entry : recordsOfForm)
            forms.append({ entry.value, entry.key });
        std::sort(forms.begin(), forms.end(), std::greater { });
        for (auto [count, form] : forms.span().first(std::min<size_t>(forms.size(), 14)))
            dataLog(" ", count, " x (", namesOfNodes[form >> 48], ", ", (form >> 40) & 0xff, ", ", (form >> 32) & 0xff, ", ", form & 0xffffff, ")");
        dataLogLn();
    }
    constexpr unsigned sizes = 7;
    static constexpr std::array<ASCIILiteral, sizes> namesOfSizes { "1"_s, "2-4"_s, "5-16"_s, "17-64"_s, "65-1024"_s, "more"_s, "complement"_s };
    struct LookupsOfSize {
        uint64_t statements { 0 };
        uint64_t readers { 0 };
    };
    HashMap<uint32_t, std::array<LookupsOfSize, sizes>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> lookups;
    for (uint32_t index = m_input.statements.size(); index < readersOfStatement.size(); ++index) {
        uint32_t record = m_madeNodesOfStatement[index - m_input.statements.size()];
        if (record == noSolverIndex || m_madeNodes[record].key.kind != ValueFlowMadeNodeKind::Lookup)
            continue;
        const ValueFlowNodeKey& key = m_madeNodes[record].key;
        uint32_t size = ValueSets::isComplement(key.set) ? 0 : m_sets.size(key.set);
        unsigned column = ValueSets::isComplement(key.set) ? 6 : size <= 1 ? 0 : size <= 4 ? 1 : size <= 16 ? 2 : size <= 64 ? 3 : size <= 1024 ? 4 : 5;
        LookupsOfSize& cell = lookups.add(static_cast<uint32_t>(key.name().kind) << 1 | key.isLookupForCall(), std::array<LookupsOfSize, sizes> { }).iterator->value[column];
        ++cell.statements;
        cell.readers += readersOfStatement[index];
    }
    for (const auto& entry : lookups) {
        dataLog("AOT flow lookups | name kind ", entry.key >> 1, entry.key & 1 ? " for a call" : "", " | receivers: statements readers |");
        for (unsigned column = 0; column < sizes; ++column) {
            if (entry.value[column].statements)
                dataLog(" ", namesOfSizes[column], ": ", entry.value[column].statements, " ", entry.value[column].readers, " |");
        }
        dataLogLn();
    }

    Vector<uint32_t> byNodes;
    auto nodesRead = [&](uint32_t index) {
        const NodesOfName& nodes = m_names[index].read;
        return nodes.ofFewObjects.size() + nodes.ofManyObjects.size() + nodes.ofVeryManyObjects.size();
    };
    for (uint32_t index = 0; index < m_names.size(); ++index)
        byNodes.append(index);
    std::sort(byNodes.begin(), byNodes.end(), [&](uint32_t left, uint32_t right) { return nodesRead(left) > nodesRead(right); });
    uint64_t allNodesRead = 0;
    for (uint32_t index : byNodes)
        allNodesRead += nodesRead(index);
    dataLog("AOT flow nodes read by name | names ", byNodes.size(), " | nodes ", allNodesRead, " | most");
    for (uint32_t index : byNodes.span().first(std::min<size_t>(byNodes.size(), 24)))
        dataLog(" ", nodesRead(index));
    dataLogLn();
}

void ValueFlowSolver::dumpBytesByTable() const
{
    ValueFlowBytesByTable bytes;
    m_graph.addBytesTo(bytes);
    m_sets.addBytesTo(bytes);
    for (const std::unique_ptr<ValueSetMaker>& maker : m_makers)
        maker->addBytesTo(bytes);

    bytes.addList("records: made nodes"_s, m_madeNodes);
    bytes.addList("records: number of users"_s, m_numberOfUsers);
    for (const IndexOfMadeNodes& index : m_madeNodesByKey) {
        for (IndexOfMadeNodes::Table table : index.tables)
            bytes.add("records: index by key"_s, 0, table.numberOfSlots() * sizeof(IndexOfMadeNodes::Slot));
        bytes.add("records: index by key"_s, index.size * sizeof(IndexOfMadeNodes::Slot), 0);
    }
    bytes.addList("records: record of node"_s, m_madeNodesOfNode);
    bytes.addList("records: parts"_s, m_partsOfMadeNodes);
    bytes.addList("records: record of statement"_s, m_madeNodesOfStatement);
    for (const NodesOfManyObjects& nodes : m_nodesOfManyObjects) {
        constexpr ASCIILiteral table = "records: nodes of many objects"_s;
        bytes.addList(table, nodes.madeNodes);
        bytes.addMap("records: nodes of many objects: latest of name"_s, nodes.latest);
        bytes.add("records: nodes of many objects: latest with block"_s, nodes.latestWithBlock.bytesInUse(), nodes.latestWithBlock.memoryInBytes());
        bytes.addMap("records: nodes of many objects: cells"_s, nodes.cells);
    }
    bytes.addList("uses: uses of statement"_s, m_usesOfStatement);
    size_t numberOfUses = 0;
    for (uint32_t list : m_usesOfStatement) {
        forEachUseIn(list, [&](uint32_t) {
            ++numberOfUses;
        });
    }
    bytes.add("uses: records asked for by statements"_s, numberOfUses * sizeof(uint32_t), m_uses.capacity() * sizeof(uint32_t));

    constexpr ASCIILiteral statements = "statements: bits, lists of due ones, who waits"_s;
    auto addBitsOfStatements = [&](const auto&... bits) {
        (bytes.addBits(statements, bits), ...);
    };
    auto addListsOfStatements = [&](const auto&... lists) {
        (bytes.addList(statements, lists), ...);
    };
    addBitsOfStatements(m_isReached, m_hasStatementOfBodyRun, m_isDue, m_isDear, m_hasLongRowBehindIt, m_hasRunInTurn, m_isDueLast, m_statementsWithMembersNotToldApart, m_statementsThatAskProxiesByTheirTraps);
    addListsOfStatements(m_firstStatementWaitingForNode, m_nextStatementWaiting, m_reachedBy, m_dearStatements, m_due, m_dueStatementsThatWait, m_dueBehindLongRows, m_dueInNextTurn, m_dueLast, m_bodiesReachedInRound);
    bytes.addList("statements: set remembered"_s, m_rememberedByStatement);
    bytes.addMap("statements: values shown"_s, m_valuesShownToStatement);
    for (const auto& shown : m_valuesShownToStatement.values())
        bytes.add("statements: values shown"_s, shown.size() > 1 ? shown.size() * sizeof(shown[0]) : 0, shown.capacity() > 1 ? shown.capacity() * sizeof(shown[0]) : 0);
    bytes.addMap("statements: readers of functions"_s, m_readersOfFunction);
    for (const auto& readers : m_readersOfFunction.values())
        bytes.addList("statements: readers of functions"_s, readers);
    bytes.addMap("witnesses: position of logged edge"_s, m_positionOfLoggedEdge);
    bytes.addMap("witnesses: first requester of made nodes"_s, m_firstRequesterOfMadeNodes);
    bytes.addList("witnesses: edges in the order logged"_s, m_edgesInOrderLogged);

    bytes.addList("readers: readers of node"_s, m_readersOfNode);
    bytes.addList("readers: listeners of node"_s, m_listenersOfNode);
    for (const ReaderShard& shard : m_readerShards) {
        bytes.add("readers: lists"_s, shard.readersOfNodes.bytesInUse(), shard.readersOfNodes.memoryInBytes());
        bytes.addList("readers: known to wait for something"_s, shard.readersWaitingForSomething);
        bytes.add("readers: known to wait for something"_s, shard.knownToWaitForSomething.bytesInUse(), shard.knownToWaitForSomething.memoryInBytes());
        bytes.addMap("readers: known to wait for something"_s, shard.firstReaderWaitingForSomething);
    }
    for (const WatchShard& shard : m_watchShards) {
        bytes.add("watches: lists of readers"_s, shard.readersOfWatches.bytesInUse(), shard.readersOfWatches.memoryInBytes());
        bytes.addList("watches: watches"_s, shard.watches);
        bytes.add("watches: watch of part"_s, shard.watchOfPart.bytesInUse(), shard.watchOfPart.memoryInBytes());
        bytes.addList("watches: watched nodes"_s, shard.watchedNodes);
        for (const WatchedNode& node : shard.watchedNodes) {
            bytes.addList("watches: watches of watched nodes"_s, node.watches);
            bytes.addList("watches: watches of watched nodes"_s, node.watchesOfManyObjects);
        }
        bytes.addMap("watches: watched node of node"_s, shard.watchedNodeOfNode);
        bytes.add("watches: first watch of member"_s, shard.firstWatchOfMember.bytesInUse(), shard.firstWatchOfMember.memoryInBytes());
        bytes.addList("watches: watches of members"_s, shard.watchesOfMembers);
        constexpr ASCIILiteral cover = "watches: sets watched for cover"_s;
        bytes.addMap(cover, shard.numberOfSetWatchedForCover);
        bytes.addList(cover, shard.watchesOfSetWatchedForCover);
        for (const auto& watches : shard.watchesOfSetWatchedForCover)
            bytes.add(cover, 0, watches.capacity() > 2 ? watches.capacity() * sizeof(uint32_t) : 0);
    }
    bytes.addList("watches: sets watched for cover"_s, m_firstSetWatchedForCoverOfMember);
    bytes.addLists("watches: sets watched for cover"_s, m_setsWatchedForCoverOfMembers);
    bytes.addBits("watches: sets watched for cover"_s, m_isWatched);

    bytes.addList("names: names"_s, m_names);
    bytes.addMap("names: names"_s, m_numberOfName);
    auto addNodesOfName = [&](const NodesOfName& nodes) {
        bytes.addList("names: nodes of few objects"_s, nodes.ofFewObjects);
        bytes.addList("names: nodes of many objects"_s, nodes.ofManyObjects);
        bytes.addList("names: nodes of very many objects"_s, nodes.ofVeryManyObjects);
    };
    for (const Name& name : m_names) {
        addNodesOfName(name.read);
        addNodesOfName(name.written);
    }
    for (const KindOfNames& kind : m_kindsOfNames) {
        addNodesOfName(kind.read);
        addNodesOfName(kind.written);
    }
    for (const LinkShard& shard : m_linkShards) {
        for (const NodesOfMembers& nodesOfMembers : shard.nodesOfMembers) {
            bytes.add("names: list of member"_s, nodesOfMembers.listOfMember.bytesInUse(), nodesOfMembers.listOfMember.memoryInBytes());
            bytes.add("names: lists of nodes of members"_s, nodesOfMembers.lists.bytesInUse(), nodesOfMembers.lists.memoryInBytes());
        }
        bytes.addList("names: parts watched"_s, shard.partsWatched);
        bytes.addMap("names: parts watched"_s, shard.partWatchedOf);
    }

    constexpr ASCIILiteral members = "members: layout or callee, counts, marks"_s;
    bytes.addList(members, m_layoutOrCalleeOfMember);
    bytes.addList(members, m_lastCountThatSaw);
    bytes.addList(members, m_marksOfObject);
    bytes.addMap(members, m_isTooLarge);
    bytes.addBits(members, m_isSaturated);
    bytes.addMap(members, m_familyObjectsOfMask);

    bytes.add("logs: threads"_s, m_threads.size() * sizeof(Thread));
    forEachListOfOneRound(*this, [&](ASCIILiteral table, const auto& list) {
        bytes.addList(table, list);
    });
    bytes.dump("all tables of the solver"_s);
}

void ValueFlowSolver::forEachListOfOneRound(auto& solver, NOESCAPE const auto& functor)
{
    auto visit = [&](ASCIILiteral table, auto&... lists) {
        (functor(table, lists), ...);
    };
    auto visitEach = [&](ASCIILiteral table, auto& lists) {
        for (auto& list : lists)
            functor(table, list);
    };
    constexpr ASCIILiteral links = "names: links of one round"_s;
    for (auto& shard : solver.m_linkShards)
        visit(links, shard.linked.edges, shard.linked.listsMostlyAtRest, shard.namesWatchedInRound, shard.writtenUnderWatchedNames, shard.woken);
    visit(links, solver.m_readToLink, solver.m_writtenToLink, solver.m_readToInsert, solver.m_edgesOfRound, solver.m_edgesInFrontOfLinks, solver.m_placeholdersStopped);
    visitEach(links, solver.m_nodesToInsertOfShards);

    for (auto& shard : solver.m_watchShards)
        visitEach("watches: members to chain in one round"_s, shard.membersToChain);

    constexpr ASCIILiteral wakers = "readers: lists of one round"_s;
    visitEach(wakers, solver.m_readersOfWorker);
    visitEach(wakers, solver.m_newMembersOfWorker);
    visitEach(wakers, solver.m_watchesToTestOfWorker);
    visit(wakers, solver.m_newMembers);

    for (auto& thread : solver.m_threads) {
        auto& log = thread->log;
        visit("logs: runs"_s, log.runs);
        visit("logs: edges"_s, log.edges, log.edgesOfMadeNodes);
        visit("logs: nodes read"_s, log.nodesRead);
        visit("logs: records asked for"_s, log.madeNodesAskedFor);
        visit("logs: parts watched"_s, log.partsWatched, log.partsWatchedLast);
        visit("logs: the rest"_s, log.made, log.madeWhereAskedFor, log.madeWithStatementToRun, log.askedForAtRest, log.recordsOfRunsWhereMade, log.whoReached, log.rememberedByStatementsRunWhereMade, log.partsOfMadeNodes, log.reached, log.uses, log.statementsWithMembersNotToldApart, log.functionsAsked, log.valuesShown, log.nodesAskedWhetherEmpty, log.namesWatched);
        visit("logs: the rest"_s, thread->thrownOfStatementsThatWait, thread->statementsFound, thread->dueInNextTurn, thread->bodiesReached, thread->namesWatched, thread->numberOfMadeNode);
        constexpr ASCIILiteral sorted = "logs: sorted by shard"_s;
        visitEach(sorted, thread->partsWatchedOfShard);
        visitEach(sorted, thread->nodesReadOfShard);
        visitEach(sorted, thread->nodesAskedWhetherEmptyOfShard);
    }
}

void ValueFlowSolver::freeSetsThatNobodyHolds()
{
    AOT_THRESHOLD(size_t, fewestBytesOfSetsMadeBetweenFreeings, 32 * MB);
    size_t bytesInUse = m_sets.bytesOfSetsMade() - m_sets.bytesOfSetsFree();
    if (!m_freesSetsEveryRound && bytesInUse < std::max(m_bytesOfSetsBehindLastFreeing + m_bytesOfSetsBehindLastFreeing / 2, m_bytesOfSetsBehindLastFreeing + fewestBytesOfSetsMadeBetweenFreeings))
        return;
    MonotonicTime start = MonotonicTime::now();
    m_sets.willFreeSets();
    for (const std::unique_ptr<ValueSetMaker>& maker : m_makers)
        maker->forgetWhatIsRemembered();

    auto keep = [&](ValueSetID set) {
        m_sets.keep(set);
    };
    auto keepEach = [&](const auto& sets) {
        for (ValueSetID set : sets)
            keep(set);
    };
    auto keepSetsOfNodes = [&](const auto& list) {
        for (const auto& entry : list)
            keep(entry.set);
    };
    auto keepInShares = [&](size_t count, NOESCAPE const Invocable<void(size_t)> auto& keepAt) {
        constexpr size_t perShare = 1 << 14;
        forEachIndexInParallel(m_loops, (count + perShare - 1) / perShare, [&](unsigned share) {
            for (size_t index = share * perShare; index < std::min(count, (share + 1) * perShare); ++index)
                keepAt(index);
        });
    };
    m_graph.keepSetsHeld(m_sets);
    keepInShares(m_madeNodes.size(), [&](size_t index) {
        keep(m_madeNodes[index].key.set);
    });
    keepInShares(m_rememberedByStatement.size(), [&](size_t index) {
        keep(m_rememberedByStatement[index]);
    });
    forEachIndexInParallel(m_loops, numberOfWatchShards, [&](unsigned index) {
        const WatchShard& shard = m_watchShards[index];
        for (const Watch& watch : shard.watches)
            keep(watch.within);
        for (const WatchedNode& node : shard.watchedNodes)
            keep(node.seen);
        for (uint32_t set : shard.numberOfSetWatchedForCover.keys())
            keep(ValueSetID(set));
    });
    auto keepNodesOfName = [&](const NodesOfName& nodes) {
        keepSetsOfNodes(nodes.ofFewObjects);
        keepSetsOfNodes(nodes.ofManyObjects);
        keepSetsOfNodes(nodes.ofVeryManyObjects);
    };
    keepInShares(m_names.size(), [&](size_t index) {
        keep(m_names[index].holders);
        keepNodesOfName(m_names[index].read);
        keepNodesOfName(m_names[index].written);
    });
    for (const KindOfNames& kind : m_kindsOfNames) {
        keepNodesOfName(kind.read);
        keepNodesOfName(kind.written);
    }
    for (const LinkShard& shard : m_linkShards) {
        for (const PartOfNameWatched& part : shard.partsWatched)
            keep(part.within);
        for (const LinkShard::NameWatched& watched : shard.namesWatchedInRound)
            keep(watched.within);
        for (const auto& written : shard.writtenUnderWatchedNames)
            keep(written.second);
        for (const Linked::LongList& list : shard.linked.longLists)
            keep(list.node.set);
    }
    for (const auto* toLink : { &m_readToLink, &m_writtenToLink, &m_readToInsert }) {
        for (const NodeToLink& node : *toLink)
            keep(node.node.set);
    }
    for (const NodesOfManyObjects& nodes : m_nodesOfManyObjects) {
        for (const Vector<ValueSetID>& cells : nodes.cells.values())
            keepEach(cells);
    }
    for (const auto& shown : m_valuesShownToStatement.values())
        keepSetsOfNodes(shown);
    for (const Covering& covering : m_coverings) {
        keep(covering.objectsThatMayBeCovered);
        keep(covering.allButCovered);
    }
    for (const KindThatCoversKeys& kind : m_kindsThatCoverKeys)
        keep(kind.allButKeys);
    keepSetsOfNodes(m_writtenUnderNamesOfReads.span());
    for (const auto* lists : { &m_listedWithCoveredMembers, &m_functionsOfEngineListed }) {
        for (const Vector<ValueFlowNodeSet>& list : *lists)
            keepSetsOfNodes(list);
    }
    for (const auto& entry : m_familyObjectsOfMask) {
        keep(ValueSetID(entry.key));
        keep(entry.value);
    }
    for (uint32_t set : m_isTooLarge.keys())
        keep(ValueSetID(set));
    keepEach(m_closuresFollowedToConstruction.values());
    keepEach(m_uncoveredAtLastReaderWake);
    keepEach(m_seenByWatchersOfObjectsWithRareWrite);
    for (ValueSetID set : { m_seenByWatchersOfObjectsThatAreNotPlain, m_seenByWatchersOfObjectsWrittenUnderUnknownKey, m_seenByWatchersOfEscaped, m_seenByWatchersOfLost, m_takenByRetiredNodes, m_keptOutOfNodesMadeForUnknownObject,
        m_declaredCallbackArguments, m_objectsThatPlaceholdersMayCover, m_objectsAlwaysFollowed, m_objectsLookedForBesideUnknownObject, m_allButMarks, m_objectsSaidToBeSomething, m_objectsOfProgram, m_untrackedObjectAlone })
        keep(set);
    for (const std::unique_ptr<Thread>& thread : m_threads) {
        thread->context.keepSetsHeld(m_sets);
        const ValueFlowRoundLog& log = thread->log;
        for (const ValueFlowRoundLog::MadeNodes& made : log.made)
            keep(made.key.set);
        for (const auto& remembered : log.rememberedByStatementsRunWhereMade)
            keep(remembered.second);
        keepSetsOfNodes(log.valuesShown);
        keepSetsOfNodes(log.partsWatched);
        for (const auto& watched : log.namesWatched)
            keep(watched.within);
        for (const auto& parts : thread->partsWatchedOfShard) {
            for (const Thread::PartWatched& part : parts)
                keep(part.within);
        }
        for (const Thread::NameWatched& watched : thread->namesWatched)
            keep(watched.within);
    }
    if (m_input.universe) {
        m_input.universe->keepSetsRemembered(m_sets);
        m_input.universe->builtinTables().heap().keepSetsHeld();
    }

    forEachIndexInParallel(m_loops, ValueSets::sharesOfSetsToFree, [&](unsigned share) {
        m_sets.freeShareOfSets(share);
    });
    m_sets.didFreeSets();
    m_bytesOfSetsBehindLastFreeing = m_sets.bytesOfSetsMade() - m_sets.bytesOfSetsFree();
    if (m_stepTimes.isOn)
        dataLogLn("AOT flow sets freed | round ", m_counters.rounds, " | in use ", bytesInUse / KB, " KB | behind it ", m_bytesOfSetsBehindLastFreeing / KB, " KB | ", (MonotonicTime::now() - start).milliseconds(), " ms");
}

void ValueFlowSolver::giveBackRoomOfEmptyLists(ListsOfLinks listsOfLinks)
{
    forEachListOfOneRound(*this, [&](ASCIILiteral table, auto& list) {
        // The nodes that read are listed under their names beside the steps that follow.
        if (listsOfLinks == ListsOfLinks::AreLookedAtToo || table != "names: links of one round"_s)
            giveBackRoomIfEmpty(list);
    });
    m_graph.giveBackRoomOfEmptyLists();
}

void ValueFlowSolver::giveBackRoomOfLargerRounds(uint64_t sizeOfRound)
{
    m_sizesOfLatestRounds[m_counters.rounds % m_sizesOfLatestRounds.size()] = sizeOfRound;
    uint64_t largestOfLatest = std::ranges::max(m_sizesOfLatestRounds);
    m_sizeOfRoundThatListsHaveRoomFor = std::max(m_sizeOfRoundThatListsHaveRoomFor, largestOfLatest);
    if (largestOfLatest * 4 > m_sizeOfRoundThatListsHaveRoomFor)
        return;
    m_sizeOfRoundThatListsHaveRoomFor = largestOfLatest;
    forEachListOfOneRound(*this, [](ASCIILiteral, auto& list) {
        giveBackMostRoomOf(list);
    });
    m_graph.giveBackMostRoomOfListsOfOneRound();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
