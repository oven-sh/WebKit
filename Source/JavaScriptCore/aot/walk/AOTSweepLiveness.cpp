/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSweep.h"

#if ENABLE(AOT)

#include "AOTEffectsDeclared.h"
#include "AOTHeldOperands.h"
#include "AOTPlan.h"
#include "AOTRepresentation.h"
#include "AOTSort.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <tuple>
#include <utility>
#include <wtf/Scope.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet interiorPointersInSweptCode = [] {
    OpcodeSet opcodes;
    for (Opcode opcode : allMembersOf<Opcode>) {
        if (baseOf(opcode))
            opcodes.add(opcode);
    }
    return opcodes;
}();

// False where clearing a register or a slot lets go of nothing: the collector takes only a form that holds cells for a pointer, and a constant lies in no register of its own.
static bool liesInPlaceThatSweepMayClear(const Code& code, ValueID value)
{
    return value && !code.isConstant(value) && code.isNeeded(value) && holdsCells(code.at(value).representation);
}

// Neither reads what it names: `x = null` must not be what keeps the value alive, and no cell goes through a saved frame for the sake of being handed back behind it.
static constexpr OpcodeSet operationsThatReadNothingInSweptCode { Opcode::ReleaseHint, Opcode::ReleaseEnvironment };
static_assert(OperandIndex::ReleaseHint::value == OperandIndex::ReleaseEnvironment::environment);

// The terminator makes the code of such an operation, or may: valueOwnedByTerminator(), CodeGenerator::testReadByBranchAlone(). So nothing can stand behind it in its block.
static ValueID operationMadeByTerminatorOfSweptBlock(const Terminator& terminator)
{
    if (terminator.isConditionalBranch())
        return terminator.conditionOperand();
    if (terminator.kind == TerminatorKind::ReturnCall)
        return terminator.callOperand();
    return { };
}

// Null: no environment, or one whose cells are not handed back.
static LayoutID layoutOfSweptEnvironmentHandedBack(const SweepInputs& inputs, TypeID type)
{
    if (!type || holdsOnlyPrimitives(type))
        return { };
    LayoutID environment = inputs.types.at(type).environment;
    return environment && inputs.plan.layout(environment).isEscapedOffset ? environment : LayoutID();
}

// The value that the operation gives another name or another form. Null: it does neither.
static ValueID valueNamedAnewBySweptOperation(const Operation& operation)
{
    return opcodesThatGiveSameCell.contains(operation.opcode) ? operation.operand(0) : ValueID();
}
static constexpr OpcodeSet holdersOfSweptEnvironmentsInFrames { Opcode::NewObject, Opcode::NewClosure };

// Whatever the value names is a cell that a NewObject of this code has made in the heap, in this run of the code, or that a callee has handed back among its results: none comes from a caller, a field or a handler.
// `for (let i ..)` makes an environment in each turn, and the one of the turn before comes round the head as a parameter.
static bool namesOnlyEnvironmentsMadeBySweptCode(const Code& code, const CallingConventions& conventions, std::span<const uint32_t> firstIncomingEdge, std::span<const EdgeID> incomingEdges, ValueID named)
{
    Vector<ValueID, 8> parametersSeen;
    Vector<ValueID, 8> worklist;
    worklist.append(named);
    while (!worklist.isEmpty()) {
        ValueID value = worklist.takeLast();
        if (!value || code.isConstant(value))
            return false;
        const Operation& operation = code.at(value);
        if (operation.opcode == Opcode::NewObject) {
            if (operation.storage() != AllocationStorage::Heap)
                return false;
            continue;
        }
        // A callee sets the byte of whatever it lists among its results and has not made for this call.
        if (operation.opcode == Opcode::ResultAt) {
            const Operation& call = code.at(operation.operand(OperandIndex::ResultAt::results));
            if (call.opcode != Opcode::CallDirect || !conventions.handsBackPartsAtEveryReturn(call))
                return false;
            continue;
        }
        if (opcodesThatGiveSameCell.contains(operation.opcode)) {
            worklist.append(operation.operand(0));
            continue;
        }
        if (operation.opcode != Opcode::Parameter)
            return false;
        if (parametersSeen.contains(value))
            continue;
        parametersSeen.append(value);

        BlockID block = operation.block();
        if (code.at(block).isEntry || code.at(block).isHandlerBlock())
            return false;
        std::span<const ValueID> parameters = code.at(block).parameters();
        size_t position = std::ranges::find(parameters, value) - parameters.begin();
        for (uint32_t index = firstIncomingEdge[block.index()]; index < firstIncomingEdge[block.index() + 1]; ++index) {
            // What an edge itself answers stands behind the arguments.
            std::span<const ValueID> arguments = code.at(incomingEdges[index]).arguments();
            if (position >= arguments.size())
                return false;
            worklist.append(arguments[position]);
        }
    }
    return true;
}

// Null: the value points into nothing.
static ValueID ownerOfSweptPointer(const Code& code, ValueID pointer)
{
    const Operation* producer = &code.at(pointer);
    if (producer->opcode == Opcode::ResultAt)
        producer = &code.at(producer->operand(OperandIndex::ResultAt::results));
    std::optional<uint8_t> operandIndex = baseOf(producer->opcode);
    return operandIndex ? producer->operand(*operandIndex) : ValueID();
}

static constexpr uint32_t noReadOfSweptValue = UINT32_MAX;

// The calls come in any order, and `reader` is null for the end of the block alone. A block may come to be listed twice for one value: whoever reads the list takes the later reader.
// False: the list says as much already, of the value and of its owners.
static bool noteReadOfSweptValueInAnyOrder(Vector<uint32_t>& lastReads, Vector<ReadOfSweptValue>& reads, ValueID value, BlockID block, ValueID reader)
{
    uint32_t& last = lastReads[value.index()];
    if (last != noReadOfSweptValue && reads[last].block == block) {
        ValueID& noted = reads[last].reader;
        if (!noted || (reader && reader <= noted))
            return false;
        noted = reader;
        return true;
    }
    reads.append({ block, reader, last });
    last = static_cast<uint32_t>(reads.size() - 1);
    return true;
}

static bool isHeldBySweptSuspension(const Operation& suspension, ValueID value)
{
    std::span<const ValueID> held = heldOperandsOf(suspension);
    return std::ranges::find(held, value) != held.end();
}

namespace {

using SweepLetGo = PlaceWhereSweptValueIsLetGo;

// What SweepLiveRanges::findLastReads() knows of one value at one block.
using SweepWayOfValue = WayOfSweptValueLetGo;

class SweepLiveRanges {
    WTF_MAKE_NONCOPYABLE(SweepLiveRanges);
public:
    // sweepBackward() has filled the two lists.
    SweepLiveRanges(const Code& code, std::span<const uint32_t> firstIncomingEdge, std::span<const EdgeID> incomingEdges, Vector<uint32_t>& lastReads, Vector<ReadOfSweptValue>& reads, Vector<SweepWayOfValue>& ways)
        : m_code(code)
        , m_firstIncomingEdge(firstIncomingEdge)
        , m_incomingEdges(incomingEdges)
        , m_lastRead(lastReads)
        , m_reads(reads)
        , m_ways(ways)
    {
        m_ways.shrink(0);
    }

    bool noteReadInAnyOrder(ValueID value, BlockID block, ValueID reader) { return noteReadOfSweptValueInAnyOrder(m_lastRead, m_reads, value, block, reader); }

    // Of a value whose reads in its own block were noted too. `letGo` names it alone.
    // From where the program has let go of the value back over all that may have run since the value was last read. It looks at blocks and edges, at no operation in between.
    // `lastSuspensions`, by block: the last needed operation that may suspend.
    void findLastReads(std::span<const SweepLetGo> letGo, std::span<const ValueID> lastSuspensions, Vector<ReleaseBehindLastRead>& releases, uint32_t& operationsSwept)
    {
        ValueID value = letGo.front().value;
        bool isHeldByCopiedBodiesAlone = std::ranges::all_of(letGo, &SweepLetGo::isBehindCopiedBody);
        bool isParameter = m_code.at(value).opcode == Opcode::Parameter;
        if (!isParameter)
            ++operationsSwept;
        BlockID definingBlock = m_code.blockOf(value);
        if (m_ways.isEmpty())
            m_ways.fill(SweepWayOfValue(), m_code.numberOfBlocks());
        Vector<BlockID, 32> worklist;

        auto reachHeadOf = [&](BlockID block) {
            if (block != definingBlock && !std::exchange(wayAt(block, value).isLiveAtHead, true))
                worklist.append(block);
        };
        // sweepBackward() may list a block twice, and names a reader that the terminator makes like any other.
        for (uint32_t read = m_lastRead[value.index()]; read != noRead; read = m_reads[read].previous) {
            const ReadOfSweptValue& noted = m_reads[read];
            SweepWayOfValue& way = wayAt(noted.block, value);
            if (!way.isRead || (way.lastReader && (!noted.reader || noted.reader > way.lastReader)))
                way.lastReader = noted.reader;
            way.isRead = true;
            reachHeadOf(noted.block);
        }
        for (uint32_t read = m_lastRead[value.index()]; read != noRead; read = m_reads[read].previous) {
            BlockID block = m_reads[read].block;
            SweepWayOfValue& way = wayAt(block, value);
            if (way.lastReader && way.lastReader == operationMadeByTerminatorOfSweptBlock(m_code.at(block).terminator))
                way.lastReader = { };
            way.isLiveAtEnd |= !way.lastReader;
        }
        while (!worklist.isEmpty()) {
            for (EdgeID edge : incomingEdgesOf(worklist.takeLast())) {
                BlockID from = m_code.at(edge).from;
                wayAt(from, value).isLiveAtEnd = true;
                reachHeadOf(from);
            }
        }

        Vector<BlockID, 16> blocksLetGoAtEnd;
        auto letGoAtEndOf = [&](BlockID block) {
            SweepWayOfValue& way = wayAt(block, value);
            if (way.isLiveAtEnd || std::exchange(way.isLetGoAtEnd, true))
                return;
            if (block == letGo.front().blockOfValueNamedByHint && block != definingBlock && !way.isRead)
                return;
            blocksLetGoAtEnd.append(block);
            if (block == definingBlock || way.isRead)
                return;
            way.isLetGoAtHead = true;
            worklist.append(block);
        };
        for (const SweepLetGo& place : letGo)
            letGoAtEndOf(place.block);
        while (!worklist.isEmpty()) {
            BlockID block = worklist.takeLast();
            for (EdgeID edge : incomingEdgesOf(block)) {
                BlockID from = m_code.at(edge).from;
                if (wayAt(from, value).isLiveAtEnd)
                    wayAt(block, value).isEnteredFromReader = true;
                else
                    letGoAtEndOf(from);
            }
        }

        // The operation behind which nothing in the block reads the value. Null: the head.
        auto lastReadIn = [&](BlockID block) {
            const SweepWayOfValue& way = wayAt(block, value);
            if (way.isRead)
                return way.lastReader;
            return block == definingBlock && !isParameter ? value : ValueID();
        };
        // What a suspension holds lies in the saved frame as a reference, and what it answers is made when the function goes on.
        auto losesValue = [&](ValueID suspension) {
            return suspension != value && !isHeldBySweptSuspension(m_code.at(suspension), value);
        };
        // Records of one block ascend in its order.
        for (BlockID block : blocksLetGoAtEnd) {
            ValueID suspension = lastSuspensions[block.index()];
            if (!suspension)
                continue;
            ValueID lastRead = lastReadIn(block);
            if (!lastRead || suspension > lastRead || (suspension == lastRead && losesValue(suspension)))
                worklist.append(block);
        }
        while (!worklist.isEmpty()) {
            m_code.forEachEdgeOf(worklist.takeLast(), [&](EdgeID, const Edge& edge) {
                SweepWayOfValue& way = wayAt(edge.to, value);
                if (way.isLetGoAtHead && !std::exchange(way.isSuspendedAtHead, true))
                    worklist.append(edge.to);
            });
        }

        for (BlockID block : blocksLetGoAtEnd) {
            const SweepWayOfValue& way = wayAt(block, value);
            if (way.isLetGoAtHead) {
                // The code at the head names the value on every way into the block, so on none of them may it be all that carries the value through a saved frame.
                // What a hint names is made on every way to the hint. Another name of it need not be made on every way into the block.
                if (way.isEnteredFromReader && !way.isSuspendedAtHead && !letGo.front().blockOfValueNamedByHint)
                    releases.append({ block, ValueID(), value });
                continue;
            }
            // Behind `await x` the function has been suspended: what held x are the registers and slots of a run that has ended.
            ValueID lastRead = lastReadIn(block);
            // A call lets go of its arguments by itself: code behind it that names the value would hold the value for as long as the callee runs.
            if (isHeldByCopiedBodiesAlone && lastRead && lastRead != value && effectRuleOf(m_code.at(lastRead).opcode) == EffectRule::Call)
                continue;
            if (!lastRead || !m_code.at(lastRead).traits().maySuspend() || !losesValue(lastRead))
                releases.append({ block, lastRead, value });
        }
    }

private:
    static constexpr uint32_t noRead = noReadOfSweptValue;

    std::span<const EdgeID> incomingEdgesOf(BlockID block) const
    {
        uint32_t first = m_firstIncomingEdge[block.index()];
        return m_incomingEdges.subspan(first, m_firstIncomingEdge[block.index() + 1] - first);
    }

    SweepWayOfValue& wayAt(BlockID block, ValueID value)
    {
        SweepWayOfValue& way = m_ways[block.index()];
        if (way.value != value) {
            way = { };
            way.value = value;
        }
        return way;
    }

    const Code& m_code;
    std::span<const uint32_t> m_firstIncomingEdge;
    std::span<const EdgeID> m_incomingEdges;
    // By value, in m_reads.
    Vector<uint32_t>& m_lastRead;
    Vector<ReadOfSweptValue>& m_reads;
    // By block. Empty until findLastReads() is asked.
    Vector<SweepWayOfValue>& m_ways;
};

// Where nothing suspends, only the operands of ReleaseHints are asked about.
class SweepLiveSets {
public:
    // `releaseHints`, by value: the first hint that names the value, and by hint the next one that names the same value.
    // `valuesLetGo`, by value: those that SweepLiveRanges::findLastReads() may be asked about.
    // `environmentsHandedBack`, by value: what a ReleaseEnvironment asks about.
    SweepLiveSets(const Code& code, std::span<const ValueID> releaseHints, const SweepBits& environmentsHandedBack, const SweepBits* valuesLetGo)
        : m_code(code)
        , m_releaseHints(releaseHints)
        , m_environmentsHandedBack(environmentsHandedBack)
        , m_valuesLetGo(valuesLetGo)
        , m_followsEveryValue(code.hasSuspensions())
        , m_hasInteriorPointers(code.opcodesOfValues().intersects(interiorPointersInSweptCode))
    {
    }

    // Behind sweepBackward(), which has noted what the operations read.
    void noteReadsAtEndOf(BlockID block, SweepLiveRanges& ranges) const
    {
        forEachValueReadAtEndOf(block, [&](ValueID read) {
            readWithOwners(read, [&](ValueID value) {
                // A value is live at no head for a read in its own block.
                if (m_code.blockOf(value) == block && !(m_valuesLetGo && m_valuesLetGo->get(value.index())))
                    return false;
                return !ranges.noteReadInAnyOrder(value, block, ValueID());
            });
        });
    }

private:
    bool isFollowed(ValueID id) const { return m_followsEveryValue || m_releaseHints[id.index()] || m_environmentsHandedBack.get(id.index()) || (m_valuesLetGo && m_valuesLetGo->get(id.index())); }

    // What the handler reads counts as read at every position of the block: an operation that suspends may throw when the function goes on.
    template<typename Functor>
    void forEachValueReadAtEndOf(BlockID id, NOESCAPE const Functor& functor) const
    {
        m_code.forEachEdgeOf(id, [&](EdgeID, const Edge& edge) {
            ASSERT(!edge.isOpen());
            std::span<const ValueID> parameters = m_code.at(edge.to).parameters();
            std::span<const ValueID> arguments = edge.arguments();
            for (size_t index = 0; index < arguments.size(); ++index) {
                if (index >= parameters.size() || m_code.isNeeded(parameters[index]))
                    functor(arguments[index]);
            }
        });
        const Block& block = m_code.at(id);
        block.terminator.forEachChild(functor);
        functor(block.subject);
    }

    ValueID ownerOf(ValueID pointer) const { return ownerOfSweptPointer(m_code, pointer); }

    // An owner outlives each pointer into it. wasRead(ValueID) notes the read of a followed value. True: it was noted before, and those of its owners with it.
    template<typename Functor>
    void readWithOwners(ValueID id, NOESCAPE const Functor& wasRead) const
    {
        if (!m_hasInteriorPointers) {
            if (id && isFollowed(id) && !m_code.isConstant(id) && m_code.isNeeded(id))
                wasRead(id);
            return;
        }
        while (id && !m_code.isConstant(id) && m_code.isNeeded(id)) {
            if (isFollowed(id) && wasRead(id))
                return;
            if (!m_hasInteriorPointers)
                return;
            id = ownerOf(id);
        }
    }

    const Code& m_code;
    std::span<const ValueID> m_releaseHints;
    const SweepBits& m_environmentsHandedBack;
    const SweepBits* m_valuesLetGo;
    bool m_followsEveryValue;
    bool m_hasInteriorPointers;
};

} // anonymous namespace

void Sweep::beginFollowingLiveness()
{
    Code& code = *m_code;
    code.m_suspensions.shrink(0);
    code.m_liveValues.shrink(0);
    code.m_releasesBehindLastReads.shrink(0);
    m_mayHoldHints = code.opcodesOfValues().contains(Opcode::ReleaseHint);
    m_mayHandBackEnvironments = code.opcodesOfValues().contains(Opcode::ReleaseEnvironment);
    m_followsLiveness = code.hasSuspensions() || m_mayHoldHints || m_mayHandBackEnvironments;
    m_looksForLastReads = m_mayHoldHints && (code.hasSuspensions() || code.inlineFrames().size() > 1);
    m_hasInteriorPointers = code.opcodesOfValues().intersects(interiorPointersInSweptCode);
    m_holdsHintsBehindCopiedBodies = false;
    if (!m_followsLiveness)
        return;

    uint32_t numberOfValues = code.numberOfValues();
    uint32_t numberOfBlocks = code.numberOfBlocks();
    m_environmentsFollowed.clearAll();
    m_environmentsHandedBack.shrink(0);
    m_blocksThatSuspend.clearAll();
    m_blocksThatSuspend.ensureSize(numberOfBlocks);
    m_blocksWithHints.clearAll();
    m_blocksWithHints.ensureSize(numberOfBlocks);
    m_lastSuspensions.fill(ValueID(), m_looksForLastReads ? numberOfBlocks : 0);
    m_valuesLetGo.clearAll();
    if (m_looksForLastReads)
        m_valuesLetGo.ensureSize(numberOfValues);
    m_valuesHeldByCopiedBodies.clearAll();
    m_otherNames.shrink(0);
    m_lastReads.fill(noReadOfSweptValue, numberOfValues);
    m_reads.shrink(0);
    m_lastReadersInOwnBlocks.fill(ValueID(), numberOfValues);
    m_liveness.blockOfValue.fill(Operation::noBlock, numberOfValues);
    m_liveness.operationsThatSuspend.clearAll();
    m_liveness.operationsThatSuspend.ensureSize(numberOfValues);
}

void Sweep::followIfEnvironmentIsHandedBack(ValueID id)
{
    const Code& code = *m_code;
    LayoutID layout = layoutOfSweptEnvironmentHandedBack(*m_inputs, code.at(id).type);
    if (!layout)
        return;
    m_environmentsFollowed.set(id.index());
    m_environmentsHandedBack.append({ layout, id });
    // Several results have no type that names them. Whatever gives them holds the environment until the last ResultAt has taken it out, so it counts as a name of it.
    if (code.at(id).opcode == Opcode::ResultAt) {
        ValueID producer = code.at(id).operand(OperandIndex::ResultAt::results);
        m_environmentsFollowed.set(producer.index());
        m_environmentsHandedBack.append({ layout, producer });
    }
}

// A cell of the frame holds an environment without setting its byte. So it counts as a name of what it holds, by itself or through cells of the frame that it holds.
// So do the results of the unit, which hold what the unit has made until the return has read them.
void Sweep::followIfHoldsEnvironmentInFrame(ValueID id)
{
    const Code& code = *m_code;
    Vector<ValueID, 8> cellsOfFrameToLookInto;
    cellsOfFrameToLookInto.append(id);
    while (!cellsOfFrameToLookInto.isEmpty()) {
        const Operation& holder = code.at(cellsOfFrameToLookInto.takeLast());
        bool liesInFrame = holdersOfSweptEnvironmentsInFrames.contains(holder.opcode) && holder.storage() != AllocationStorage::Heap;
        if (!liesInFrame && holder.opcode != Opcode::Results)
            continue;
        for (ValueID held : holder.operands()) {
            if (!held || code.isConstant(held))
                continue;
            if (LayoutID layout = layoutOfSweptEnvironmentHandedBack(*m_inputs, code.at(held).type)) {
                m_environmentsFollowed.set(id.index());
                m_environmentsHandedBack.append({ layout, id });
            }
            cellsOfFrameToLookInto.append(held);
        }
    }
}

// The hint may yet be dropped with its value. Nobody asks about a value that is dropped.
void Sweep::noteValueHeldByCopiedBody(ValueID value)
{
    if (!m_looksForLastReads)
        return;
    m_valuesHeldByCopiedBodies.set(value.index());
    m_holdsHintsBehindCopiedBodies = true;
}

// Whoever is needed needs what it reads, and a pointer needs its owner: none of them is asked whether it is needed, which the way may not have marked yet.
// A value is live at no head for a read in its own block, and most reads are such: they are kept apart, for the few values that a hint turns out to name.
ALWAYS_INLINE void Sweep::noteReadForLiveness(ValueID read, BlockID block, ValueID reader)
{
    const Code& code = *m_code;
    for (ValueID value = read; value; value = ownerOfSweptPointer(code, value)) {
        BlockID definingBlock = code.blockOf(value);
        if (!definingBlock)
            return;
        if (definingBlock == block) {
            ValueID& last = m_lastReadersInOwnBlocks[value.index()];
            if (last && reader <= last)
                return;
            last = reader;
        } else if (!noteReadOfSweptValueInAnyOrder(m_lastReads, m_reads, value, block, reader))
            return;
        if (!m_hasInteriorPointers)
            return;
    }
}

void Sweep::noteNeededOperationForLiveness(ValueID id, const Operation& operation)
{
    const Code& code = *m_code;
    BlockID block = operation.block();
    m_liveness.blockOfValue[id.index()] = block.index();
    if (m_mayHandBackEnvironments) {
        followIfEnvironmentIsHandedBack(id);
        followIfHoldsEnvironmentInFrame(id);
    }
    if (operation.traits().maySuspend()) {
        m_blocksThatSuspend.quickSet(block.index());
        m_liveness.operationsThatSuspend.quickSet(id.index());
        if (m_looksForLastReads) {
            ValueID& last = m_lastSuspensions[block.index()];
            if (!last || last < id)
                last = id;
        }
    }
    // A hint stands behind whatever names its value anew, so the way has come by it.
    if (m_holdsHintsBehindCopiedBodies && !m_valuesHeldByCopiedBodies.get(id.index()) && liesInPlaceThatSweepMayClear(code, id)) {
        for (ValueID named = valueNamedAnewBySweptOperation(operation); named && !code.isConstant(named); named = valueNamedAnewBySweptOperation(code.at(named))) {
            if (!m_valuesHeldByCopiedBodies.get(named.index()))
                continue;
            m_valuesLetGo.quickSet(id.index());
            m_otherNames.append({ named, id });
            break;
        }
    }
    if (operationsThatReadNothingInSweptCode.contains(operation.opcode))
        return;
    // The branch reads the test. What the edge hands over is the value alone.
    if (operation.opcode == Opcode::Narrowed) {
        noteReadForLiveness(operation.operand(OperandIndex::Narrowed::value), block, id);
        return;
    }
    operation.forEachChild([&](ValueID read) {
        noteReadForLiveness(read, block, id);
    });
}

void Sweep::finishLiveness()
{
    Code& code = *m_code;
    if (!m_followsLiveness)
        return;
    RELEASE_ASSERT(!m_reachedInPostorder.isEmpty());

    if (m_mayHandBackEnvironments) {
        for (BlockID block : m_reachedInPostorder) {
            for (ValueID parameter : code.at(block).parameters()) {
                if (code.isNeeded(parameter))
                    followIfEnvironmentIsHandedBack(parameter);
            }
        }
    }

    // Everything needed has been marked, so nothing waits in the worklist. Until this step ends it holds the hints by value.
    ASSERT(m_worklist.isEmpty());
    Vector<ValueID>& releaseHints = m_worklist;
    auto releaseStorage = makeScopeExit([&] {
        m_worklist.shrink(0);
    });
    releaseHints.fill(ValueID(), m_mayHoldHints || m_mayHandBackEnvironments ? code.numberOfValues() : 0);

    Vector<SweepLetGo>& placesLetGo = m_liveness.placesLetGo;
    placesLetGo.shrink(0);
    Vector<ValueID>& hintsBehindLastUses = m_liveness.hintsBehindLastUses;
    hintsBehindLastUses.shrink(0);

    // A join that only hints read, itself or through other such joins, reads what flows into it for nobody's sake. Each edge lets go of what it brings.
    Vector<ValueID>& joinsLetGo = m_liveness.joinsLetGo;
    joinsLetGo.shrink(0);
    auto letGoOfWhatFlowsInto = [&](ValueID join) {
        joinsLetGo.append(join);
        while (!joinsLetGo.isEmpty()) {
            ValueID parameter = joinsLetGo.takeLast();
            if (!parameter || code.isConstant(parameter) || code.isNeeded(parameter) || code.at(parameter).opcode != Opcode::Parameter || m_valuesLetGo.quickSet(parameter.index()))
                continue;
            BlockID block = code.blockOf(parameter);
            std::span<const ValueID> parameters = code.at(block).parameters();
            size_t position = std::ranges::find(parameters, parameter) - parameters.begin();
            for (uint32_t index = m_firstIncomingEdge[block.index()]; index < m_firstIncomingEdge[block.index() + 1]; ++index) {
                const Edge& edge = code.at(m_incomingEdges[index]);
                // What an edge itself answers stands behind the arguments. A throw leaves from anywhere in its block.
                if (position >= edge.arguments().size() || code.at(edge.from).handler == m_incomingEdges[index])
                    continue;
                ValueID argument = edge.arguments()[position];
                if (!liesInPlaceThatSweepMayClear(code, argument)) {
                    joinsLetGo.append(argument);
                    continue;
                }
                m_valuesLetGo.quickSet(argument.index());
                placesLetGo.append({ argument, edge.from });
            }
        }
    };

    std::span<const OperationKeptForOperand> operationsKeptForOperands = m_operationsKeptForOperands.span();
    for (const OperationKeptForOperand& noted : operationsKeptForOperands) {
        ValueID id = noted.operation;
        if (noted.opcode == Opcode::ReleaseEnvironment) {
            if (!code.isNeeded(id))
                continue;
            releaseHints[id.index()] = std::exchange(releaseHints[noted.operand.index()], id);
            m_blocksWithHints.quickSet(noted.block.index());
            if (LayoutID layout = layoutOfSweptEnvironmentHandedBack(*m_inputs, code.at(noted.operand).type))
                m_environmentsHandedBack.append({ layout, id });
            continue;
        }
        if (noted.opcode != Opcode::ReleaseHint)
            continue;
        if (!code.isNeeded(id)) {
            if (m_looksForLastReads && code.hasSuspensions())
                letGoOfWhatFlowsInto(noted.operand);
            continue;
        }
        releaseHints[id.index()] = std::exchange(releaseHints[noted.operand.index()], id);
        m_blocksWithHints.quickSet(noted.block.index());
        if (m_looksForLastReads && (code.hasSuspensions() || noted.isBehindCopiedBody)) {
            m_valuesLetGo.quickSet(noted.operand.index());
            hintsBehindLastUses.append(id);
        }
    }
    // Without the flag only the operands of hints are followed, and a list of what is live would lack the rest.
    RELEASE_ASSERT(code.hasSuspensions() || m_blocksThatSuspend.isEmpty());
    if (m_blocksThatSuspend.isEmpty() && m_blocksWithHints.isEmpty())
        return;
    AOT::sort(m_environmentsHandedBack, { }, [](const EnvironmentHandedBack& environment) {
        return std::tuple(environment.layout, environment.value);
    });

    SweepLiveSets sets(code, releaseHints.span(), m_environmentsFollowed, &m_valuesLetGo);
    SweepLiveRanges ranges(code, m_firstIncomingEdge.span(), m_incomingEdges.span(), m_lastReads, m_reads, m_liveness.waysOfValueLetGo);
    m_valuesLetGo.forEachSetBit([&](size_t index) {
        if (ValueID reader = m_lastReadersInOwnBlocks[index])
            ranges.noteReadInAnyOrder(ValueID(index), code.blockOf(ValueID(index)), reader);
    });
    for (BlockID block : m_reachedInPostorder)
        sets.noteReadsAtEndOf(block, ranges);
    bool hasMarkedReleasesByValue = !code.hasSuspensions() && !m_mayHandBackEnvironments && markReleasesBehindLastUsesByValue(releaseHints.span());
    if (!hasMarkedReleasesByValue) {
        // The way has noted the reads of every value: which of them a hint names it learns only when it comes by the hint.
        SweepBits& valuesToFollow = m_liveness.valuesToFollow;
        valuesToFollow.clearAll();
        if (!code.hasSuspensions()) {
            valuesToFollow.merge(m_environmentsFollowed);
            valuesToFollow.merge(m_valuesLetGo);
        }
        for (const OperationKeptForOperand& noted : operationsKeptForOperands) {
            if (!operationsThatReadNothingInSweptCode.contains(noted.opcode) || !code.isNeeded(noted.operation))
                continue;
            if (!code.hasSuspensions()) {
                valuesToFollow.set(noted.operand.index());
                valuesToFollow.set(noted.operation.index());
            }
            bool hasBitOfItsOwn = code.hasSuspensions() || noted.opcode == Opcode::ReleaseEnvironment;
            if (hasBitOfItsOwn && code.blockOf(noted.operand) != noted.block)
                ranges.noteReadInAnyOrder(noted.operation, noted.block, noted.operation);
        }

        // Every mark is set before a suspension takes one away.
        findLiveSets(releaseHints.span(), code.hasSuspensions() ? nullptr : &valuesToFollow);
        markReleasesBehindLastUsesBySets();
    }
    if (m_blocksThatSuspend.isEmpty() && hintsBehindLastUses.isEmpty())
        return;
    auto releasesOperand = [&](ValueID hint) {
        return code.marksOf(hint).contains(SweepMark::ReleasesOperand);
    };
    hintsBehindLastUses.removeAllMatching([&](ValueID hint) {
        return !releasesOperand(hint);
    });
    if (!hasMarkedReleasesByValue)
        listLiveValuesAtSuspensionsBySets();
    AOT::sort(code.m_suspensions, { }, &LiveAtSuspension::suspension);

    // A hint that a suspension has taken the mark from clears nothing, on no way to it.
    // Nor does a hint behind the body of a callee, whose code would name the value up to the return.
    AOT::sort(m_otherNames, { }, [](const OtherNameOfValue& other) {
        return std::tuple(other.valueNamedByHint, other.name);
    });
    for (ValueID hint : hintsBehindLastUses) {
        ValueID released = code.at(hint).operand(OperandIndex::ReleaseHint::value);
        if (!code.at(hint).isBehindCopiedBody()) {
            if (!releasesOperand(hint))
                placesLetGo.append({ released, code.blockOf(hint) });
            continue;
        }
        OptionSet<SweepMark> marks = code.marksOf(hint);
        marks.remove(SweepMark::ReleasesOperand);
        code.m_marks[hint.index()] = marks.toRaw();
        placesLetGo.append({ released, code.blockOf(hint), true });
        for (const OtherNameOfValue& other : std::ranges::equal_range(m_otherNames.span(), released, { }, &OtherNameOfValue::valueNamedByHint))
            placesLetGo.append({ other.name, code.blockOf(hint), true, code.blockOf(released) });
    }
    if (placesLetGo.isEmpty())
        return;
    AOT::sort(placesLetGo, { }, [](const SweepLetGo& place) {
        return std::tuple(place.value, place.block);
    });
    for (size_t first = 0, end = 0; first < placesLetGo.size(); first = end) {
        while (end < placesLetGo.size() && placesLetGo[end].value == placesLetGo[first].value)
            ++end;
        ranges.findLastReads(placesLetGo.span().subspan(first, end - first), m_lastSuspensions.span(), code.m_releasesBehindLastReads, m_totals.operationsSwept);
    }
    AOT::sort(code.m_releasesBehindLastReads, { }, [](const ReleaseBehindLastRead& release) {
        return std::tuple(release.block, release.behind ? release.behind.index() + 1 : 0u, release.released);
    });
}

// What the handler reads counts as read at every position of the block: an operation that suspends may throw when the function goes on.
static void forEachValueReadAtEndOfSweptBlock(const Code& code, BlockID id, NOESCAPE const Invocable<void(ValueID)> auto& functor)
{
    code.forEachEdgeOf(id, [&](EdgeID, const Edge& edge) {
        std::span<const ValueID> parameters = code.at(edge.to).parameters();
        std::span<const ValueID> arguments = edge.arguments();
        for (size_t index = 0; index < arguments.size(); ++index) {
            if (index >= parameters.size() || code.isNeeded(parameters[index]))
                functor(arguments[index]);
        }
    });
    const Block& block = code.at(id);
    block.terminator.forEachChild(functor);
    functor(block.subject);
}

// A value is defined in front of every read of it. So whatever is live at the head of a loop is defined outside the loop and is live all through it,
// and what is live at any other place is found on the ways that lead forward from there, or is live at the head of a loop around the place.
// Where an edge enters a loop past its head, what is live at the head is live on the edge too.
void Sweep::findLiveSets(std::span<const ValueID> releaseHints, const SweepBits* valuesToFollow)
{
    const Code& code = *m_code;
    SweepLivenessStorage& storage = m_liveness;
    SweepValueSets& sets = storage.sets;
    Vector<uint32_t>& blockOfValue = storage.blockOfValue;
    SweepBits& blocksAskedAbout = storage.blocksAskedAbout;
    SweepBits& valuesReadInOtherBlocks = storage.valuesReadInOtherBlocks;
    uint32_t numberOfValues = code.numberOfValues();
    uint32_t numberOfBlocks = code.numberOfBlocks();
    const bool followsEveryValue = code.hasSuspensions();
    std::span<const OperationKeptForOperand> keptForOperands = m_operationsKeptForOperands.span();

    blocksAskedAbout.clearAll();
    blocksAskedAbout.ensureSize(numberOfBlocks);
    blocksAskedAbout.merge(m_blocksThatSuspend);
    blocksAskedAbout.merge(m_blocksWithHints);
    valuesReadInOtherBlocks.clearAll();
    valuesReadInOtherBlocks.ensureSize(numberOfValues);
    storage.hintsThatStay.clearAll();
    storage.hintsThatStay.ensureSize(numberOfValues);

    SweepFlow& flow = storage.flow;
    flow.compute(code, m_firstIncomingEdge.span(), m_incomingEdges.span(), blocksAskedAbout);
    RELEASE_ASSERT(flow.numberOfBlocks() == m_reachedInPostorder.size());

    SweepBits& blocksAwaited = storage.blocksAwaited;
    blocksAwaited.clearAll();
    blocksAwaited.merge(blocksAskedAbout);
    if (flow.hasLoops()) {
        blocksAskedAbout.forEachSetBit([&](size_t index) {
            uint32_t loop = flow.loopAround(BlockID(static_cast<uint32_t>(index)));
            if (loop == SweepFlow::none)
                return;
            while (flow.loopAroundLoop(loop) != SweepFlow::none)
                loop = flow.loopAroundLoop(loop);
            blocksAwaited.quickSet(flow.headOf(loop).index());
        });
    }
    uint32_t numberOfBlocksAwaited = 0;
    blocksAwaited.forEachSetBit([&](size_t) {
        ++numberOfBlocksAwaited;
    });

    for (BlockID block : m_reachedInPostorder) {
        for (ValueID parameter : code.at(block).parameters())
            blockOfValue[parameter.index()] = block.index();
    }
    // The way has noted the block of whatever it came by. Anything else is a constant, which has none, or stands in a block that nothing reaches.
    auto blockOf = [&](ValueID id) {
        uint32_t& block = blockOfValue[id.index()];
        if (block == Operation::noBlock)
            block = code.at(id).blockIndex;
        return block;
    };
    // The bit of a hint goes back to where the operand of the hint is defined.
    storage.firstKeptForOperandInBlock.fill(SweepFlow::none, keptForOperands.empty() ? 0 : numberOfBlocks);
    for (size_t index = keptForOperands.size(); index--;) {
        const OperationKeptForOperand& kept = keptForOperands[index];
        storage.firstKeptForOperandInBlock[kept.block.index()] = static_cast<uint32_t>(index);
        if (!operationsThatReadNothingInSweptCode.contains(kept.opcode) || !code.isNeeded(kept.operation))
            continue;
        storage.hintsThatStay.quickSet(kept.operation.index());
        uint32_t blockOfOperand = blockOf(kept.operand);
        RELEASE_ASSERT(blockOfOperand != Operation::noBlock);
        blockOfValue[kept.operation.index()] = blockOfOperand;
        if ((followsEveryValue || kept.opcode == Opcode::ReleaseEnvironment) && blockOfOperand != kept.block.index())
            valuesReadInOtherBlocks.quickSet(kept.operand.index());
    }

    // A value is live at no head for a read in its own block. Where the block is asked about, the read says how far down the block the value lives.
    storage.reads.shrink(0);
    storage.firstReadOfBlock.fill(0, numberOfBlocks + 1);
    auto listRead = [&](BlockID block, ValueID value, ValueID reader) {
        storage.reads.append({ block, { value, reader } });
        ++storage.firstReadOfBlock[block.index()];
    };
    for (uint32_t index = 0; index < numberOfValues; ++index) {
        uint32_t read = m_lastReads[index];
        ValueID lastReaderInOwnBlock = m_lastReadersInOwnBlocks[index];
        if ((read == noReadOfSweptValue && !lastReaderInOwnBlock) || (valuesToFollow && !valuesToFollow->get(index)))
            continue;
        uint32_t definingBlock = blockOf(ValueID(index));
        RELEASE_ASSERT(definingBlock != Operation::noBlock);
        if (!flow.mayLeadToBlockAskedAbout(BlockID(definingBlock)))
            continue;
        bool isDefinedWhereAskedAbout = blocksAskedAbout.quickGet(definingBlock);
        if (lastReaderInOwnBlock && isDefinedWhereAskedAbout)
            listRead(BlockID(definingBlock), ValueID(index), lastReaderInOwnBlock);
        for (; read != noReadOfSweptValue; read = m_reads[read].previous) {
            const ReadOfSweptValue& noted = m_reads[read];
            if (noted.block.index() != definingBlock)
                valuesReadInOtherBlocks.quickSet(index);
            else if (!isDefinedWhereAskedAbout)
                continue;
            listRead(noted.block, ValueID(index), noted.reader);
        }
    }
    uint32_t numberOfReads = 0;
    for (uint32_t index = 0; index <= numberOfBlocks; ++index)
        numberOfReads += std::exchange(storage.firstReadOfBlock[index], numberOfReads);
    storage.readsByBlock.resize(numberOfReads);
    for (const SweepLivenessStorage::ReadInBlock& read : storage.reads)
        storage.readsByBlock[storage.firstReadOfBlock[read.block.index()]++] = read.read;
    for (uint32_t index = numberOfBlocks; index; --index)
        storage.firstReadOfBlock[index] = storage.firstReadOfBlock[index - 1];
    storage.firstReadOfBlock[0] = 0;

    sets.clear();
    storage.liveAtHeads.fill(SweepValueSets::empty, numberOfBlocks);
    storage.hints.shrink(0);
    storage.suspensions.shrink(0);

    SweepValueSets::Set live = SweepValueSets::empty;
    auto define = [&](ValueID id) {
        live = sets.without(live, id.index());
        if (releaseHints.empty())
            return;
        for (ValueID hint = releaseHints[id.index()]; hint; hint = releaseHints[hint.index()])
            live = sets.without(live, hint.index());
    };
    auto isFollowed = [&](ValueID id) {
        return followsEveryValue || releaseHints[id.index()] || m_environmentsFollowed.get(id.index()) || m_valuesLetGo.get(id.index());
    };
    // An owner outlives each pointer into it.
    auto readAtEndOfBlock = [&](ValueID read) {
        for (ValueID id = read; id && code.isNeeded(id) && blockOf(id) != Operation::noBlock; id = ownerOfSweptPointer(code, id)) {
            if (isFollowed(id))
                live = sets.with(live, id.index());
            if (!m_hasInteriorPointers)
                return;
        }
    };

    auto readsIn = [&](BlockID block) {
        uint32_t first = storage.firstReadOfBlock[block.index()];
        return storage.readsByBlock.mutableSpan().subspan(first, storage.firstReadOfBlock[block.index() + 1] - first);
    };
    // At the head of a block that leads nowhere and is not asked about its list of reads is what is live. Whoever leads to the block adds the list to its own set, and no set is made of the list.
    constexpr SweepValueSets::Set readsAreWhatIsLive = UINT32_MAX;

    for (BlockID block : flow.blocksBehindTheirSuccessors()) {
        bool isAskedAbout = blocksAskedAbout.quickGet(block.index());
        if (!isAskedAbout && !code.at(block).terminator.numberOfEdges && !code.handlerOf(block)) {
            storage.liveAtHeads[block.index()] = readsAreWhatIsLive;
            continue;
        }

        sets.keepSetsMadeSoFar();
        live = SweepValueSets::empty;
        bool leadsToListOfReads = false;
        // The head of a block that an edge leads back to has no set yet.
        code.forEachEdgeOf(block, [&](EdgeID, const Edge& edge) {
            SweepValueSets::Set liveAtHead = storage.liveAtHeads[edge.to.index()];
            if (liveAtHead == readsAreWhatIsLive) {
                leadsToListOfReads = true;
                return;
            }
            live = sets.unite(live, liveAtHead);
            if (flow.isReducible())
                return;
            if (BlockID head = flow.headOfOutermostLoopEntered(block, edge.to))
                live = sets.unite(live, storage.liveAtHeads[head.index()]);
        });
        if (leadsToListOfReads) {
            code.forEachEdgeOf(block, [&](EdgeID, const Edge& edge) {
                if (storage.liveAtHeads[edge.to.index()] != readsAreWhatIsLive)
                    return;
                for (const SweepLivenessStorage::ReadOfValue& read : readsIn(edge.to))
                    live = sets.with(live, read.value.index());
            });
        }

        std::span<SweepLivenessStorage::ReadOfValue> reads = readsIn(block);
        std::span<const ValueID> order = code.valuesOf(block);
        if (!isAskedAbout) {
            for (const SweepLivenessStorage::ReadOfValue& read : reads)
                live = sets.with(live, read.value.index());
            for (ValueID id : order) {
                if (live == SweepValueSets::empty)
                    break;
                if (valuesReadInOtherBlocks.quickGet(id.index()))
                    define(id);
            }
        } else {
            forEachValueReadAtEndOfSweptBlock(code, block, readAtEndOfBlock);
            // Records of one block ascend in its order, and no record stands behind the end of the block, which has the number of no value.
            // In place: in which order the reads of one reader come changes no set.
            std::ranges::sort(reads, std::ranges::greater { }, &SweepLivenessStorage::ReadOfValue::reader);
            size_t nextRead = 0;
            for (; nextRead < reads.size() && !reads[nextRead].reader; ++nextRead)
                live = sets.with(live, reads[nextRead].value.index());

            // Those of one block stand in a row, last to first.
            size_t nextKeptForOperand = keptForOperands.empty() ? 0 : storage.firstKeptForOperandInBlock[block.index()];
            for (size_t index = order.size(); index--;) {
                ValueID id = order[index];
                if (storage.operationsThatSuspend.quickGet(id.index())) {
                    storage.suspensions.append({ id.index(), block, live });
                    sets.keepSetsMadeSoFar();
                }
                if (storage.hintsThatStay.quickGet(id.index())) {
                    for (;; ++nextKeptForOperand) {
                        RELEASE_ASSERT(nextKeptForOperand < keptForOperands.size());
                        if (keptForOperands[nextKeptForOperand].operation == id)
                            break;
                    }
                    storage.hints.append({ static_cast<uint32_t>(nextKeptForOperand), block, live });
                    sets.keepSetsMadeSoFar();
                    // It reads nothing. Its own number says that it lies ahead and that its operand has not been defined on the way.
                    bool hasNumberOfItsOwn = followsEveryValue || keptForOperands[nextKeptForOperand].opcode == Opcode::ReleaseEnvironment;
                    live = hasNumberOfItsOwn ? sets.with(live, id.index()) : sets.without(live, id.index());
                    for (; nextRead < reads.size() && reads[nextRead].reader == id; ++nextRead) { }
                    continue;
                }
                define(id);
                for (; nextRead < reads.size() && reads[nextRead].reader == id; ++nextRead)
                    live = sets.with(live, reads[nextRead].value.index());
            }
            RELEASE_ASSERT(nextRead == reads.size());
        }
        for (ValueID parameter : code.at(block).parameters()) {
            if (live == SweepValueSets::empty)
                break;
            if (isAskedAbout || valuesReadInOtherBlocks.quickGet(parameter.index()))
                define(parameter);
        }
        storage.liveAtHeads[block.index()] = live;
        if (blocksAwaited.quickGet(block.index()) && !--numberOfBlocksAwaited)
            break;
    }
    sets.keepSetsMadeSoFar();

    storage.liveThroughLoops.fill(SweepValueSets::empty, flow.hasLoops() ? flow.numberOfBlocks() : 0);
    for (uint32_t loop = 0; loop < storage.liveThroughLoops.size(); ++loop) {
        if (!flow.isHeadOfLoop(loop))
            continue;
        uint32_t around = flow.loopAroundLoop(loop);
        storage.liveThroughLoops[loop] = sets.unite(storage.liveAtHeads[flow.headOf(loop).index()], around == SweepFlow::none ? SweepValueSets::empty : storage.liveThroughLoops[around]);
    }
}

SweepValueSets::Set Sweep::liveThroughLoopsAround(BlockID block) const
{
    uint32_t loop = m_liveness.flow.loopAround(block);
    return loop == SweepFlow::none ? SweepValueSets::empty : m_liveness.liveThroughLoops[loop];
}

void Sweep::markReleasesBehindLastUsesBySets()
{
    Code& code = *m_code;
    const SweepValueSets& sets = m_liveness.sets;
    for (const SweepLivenessStorage::PointAskedAbout& point : m_liveness.hints) {
        const OperationKeptForOperand& hint = m_operationsKeptForOperands[point.index];
        SweepValueSets::Set liveThroughLoops = liveThroughLoopsAround(point.block);
        auto isLive = [&](ValueID value) {
            return sets.contains(point.liveBehind, value.index()) || sets.contains(liveThroughLoops, value.index());
        };

        bool releasesOperand = false;
        if (hint.opcode == Opcode::ReleaseHint)
            releasesOperand = !isLive(hint.operand);
        else if (LayoutID layout = layoutOfSweptEnvironmentHandedBack(*m_inputs, code.at(hint.operand).type)) {
            // The unit has done with the cell where nothing that may be the cell under another name is read any more: a parameter of a block, what a call answers.
            // A ReleaseEnvironment that lies ahead counts as such a name, so of several on one way, or round a loop, only the last hands the cell back.
            auto ofSameLayout = std::ranges::equal_range(m_environmentsHandedBack, layout, { }, &EnvironmentHandedBack::layout);
            releasesOperand = std::ranges::none_of(ofSameLayout, [&](const EnvironmentHandedBack& other) {
                return isLive(other.value);
            }) && namesOnlyEnvironmentsMadeBySweptCode(code, m_inputs->plan.conventions(), m_firstIncomingEdge.span(), m_incomingEdges.span(), hint.operand);
        }
        OptionSet<SweepMark> marks = code.marksOf(hint.operation);
        marks.set(SweepMark::ReleasesOperand, releasesOperand);
        code.m_marks[hint.operation.index()] = marks.toRaw();
    }
}

bool Sweep::markReleasesBehindLastUsesByValue(std::span<const ValueID> releaseHints)
{
    Code& code = *m_code;
    Vector<ValueID>& valuesLiveAtHeads = m_liveness.valuesLiveAtHeads;
    Vector<ValueID>& valuesLiveAtEnds = m_liveness.valuesLiveAtEnds;
    Vector<BlockID>& blocksToLookAbove = m_liveness.blocksToLookAbove;
    valuesLiveAtHeads.fill(ValueID(), code.numberOfBlocks());
    valuesLiveAtEnds.fill(ValueID(), code.numberOfBlocks());
    blocksToLookAbove.shrink(0);
    size_t numberOfLooksLeft = m_reachedInPostorder.size() + m_reads.size();

    for (const OperationKeptForOperand& first : m_operationsKeptForOperands) {
        if (first.opcode != Opcode::ReleaseHint || releaseHints[first.operand.index()] != first.operation)
            continue;
        ValueID value = first.operand;
        BlockID definingBlock = code.blockOf(value);
        auto reachHeadOf = [&](BlockID block) {
            if (block != definingBlock && std::exchange(valuesLiveAtHeads[block.index()], value) != value)
                blocksToLookAbove.append(block);
        };
        for (uint32_t read = m_lastReads[value.index()]; read != noReadOfSweptValue; read = m_reads[read].previous)
            reachHeadOf(m_reads[read].block);
        while (!blocksToLookAbove.isEmpty()) {
            if (!numberOfLooksLeft--)
                return false;
            uint32_t block = blocksToLookAbove.takeLast().index();
            for (uint32_t index = m_firstIncomingEdge[block]; index < m_firstIncomingEdge[block + 1]; ++index) {
                BlockID from = code.at(m_incomingEdges[index]).from;
                valuesLiveAtEnds[from.index()] = value;
                reachHeadOf(from);
            }
        }

        for (ValueID hint = first.operation; hint; hint = releaseHints[hint.index()]) {
            BlockID block = code.blockOf(hint);
            bool isLive = valuesLiveAtEnds[block.index()] == value;
            for (uint32_t read = m_lastReads[value.index()]; read != noReadOfSweptValue && !isLive; read = m_reads[read].previous) {
                if (!numberOfLooksLeft--)
                    return false;
                const ReadOfSweptValue& noted = m_reads[read];
                isLive = noted.block == block && (!noted.reader || noted.reader > hint);
            }
            if (!isLive && block == definingBlock) {
                ValueID lastReader = m_lastReadersInOwnBlocks[value.index()];
                isLive = lastReader && lastReader > hint;
            }
            if (!isLive && block == definingBlock) {
                forEachValueReadAtEndOfSweptBlock(code, block, [&](ValueID read) {
                    for (ValueID id = read; id && code.isNeeded(id) && !code.isConstant(id); id = ownerOfSweptPointer(code, id)) {
                        isLive |= id == value;
                        if (!m_hasInteriorPointers)
                            return;
                    }
                });
            }
            OptionSet<SweepMark> marks = code.marksOf(hint);
            marks.set(SweepMark::ReleasesOperand, !isLive);
            code.m_marks[hint.index()] = marks.toRaw();
        }
    }
    return true;
}

void Sweep::listLiveValuesAtSuspensionsBySets()
{
    Code& code = *m_code;
    SweepValueSets& sets = m_liveness.sets;
    // A saved frame keeps the word and not what it points into, which may be gone or elsewhere when the function goes on.
    const bool mayPointIntoStorage = code.opcodesOfValues().contains(Opcode::GetTypedArrayVector) || code.opcodesOfValues().contains(Opcode::GetArrayStorage);
    sets.keepSetsMadeSoFar();
    for (const SweepLivenessStorage::PointAskedAbout& point : m_liveness.suspensions) {
        ValueID suspension(point.index);
        uint32_t numberOfNodes = sets.numberOfNodes();
        SweepValueSets::Set live = sets.unite(point.liveBehind, liveThroughLoopsAround(point.block));
        uint32_t first = static_cast<uint32_t>(code.m_liveValues.size());
        sets.forEachMember(live, [&](uint32_t index) {
            ValueID id(index);
            if (id == suspension)
                return;
            if (!m_liveness.hintsThatStay.quickGet(index)) {
                if (mayPointIntoStorage) {
                    Opcode opcode = code.at(id).opcode;
                    RELEASE_ASSERT(opcode != Opcode::GetTypedArrayVector && opcode != Opcode::GetArrayStorage);
                }
                code.m_liveValues.append(id);
                return;
            }
            // The code of a hint names its operand. Where the operand is neither live behind the suspension nor held by it, the hint alone would carry it through the saved frame, in a word that is no root.
            // The registers and slots that held it are then those of the run that ends here, so there is nothing left for the hint to clear.
            // A ReleaseEnvironment would hand back a cell that the collector may have freed in the meantime.
            m_totals.operationsSwept += 2;
            ValueID released = code.at(id).operand(OperandIndex::ReleaseHint::value);
            if (released == suspension || sets.contains(live, released.index()) || isHeldBySweptSuspension(code.at(suspension), released))
                return;
            OptionSet<SweepMark> marks = code.marksOf(id);
            marks.remove(SweepMark::ReleasesOperand);
            code.m_marks[id.index()] = marks.toRaw();
        });
        code.m_suspensions.append(LiveAtSuspension { suspension, first, static_cast<uint32_t>(code.m_liveValues.size()) - first });
        sets.forgetSetsMadeSince(numberOfNodes);
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
