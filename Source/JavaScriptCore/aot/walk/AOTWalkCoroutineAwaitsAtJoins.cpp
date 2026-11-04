/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// `await (hit ? cached(k) : load(k))`: the Await at the head of a join is built by each jump into the join, so that a call on one of the ways can go on to it without a promise.
// Which joins is decided by the input alone, so every way through the unit decides alike. At a suspension: each way saves the frame for itself, with what the Await holds under the names of that way.
// At a throw: the jumping blocks stand under the handler of the join. Withheld (DirectCalls): the Await stays in the join.

#include "config.h"
#include "AOTWalkCoroutineCalls.h"

#if ENABLE(AOT)

#include "AOTEffectsDeclared.h"
#include "AOTOpcodeSets.h"
#include "AOTWalkCoroutineCallsInternal.h"

namespace JSC { namespace AOT { namespace Typed {

// ---- By the input

// On an edge nothing of the block has a name but its parameters. So the Await holds nothing that is made in front of it in the block, and nothing that is made there lives on behind it.
static ValueID findAwaitAtHeadInAwaitsAtJoins(const BodyIndex& index, BodyBlock block)
{
    if (!block.numberOfParameters())
        return { };
    ValueID await;
    for (BodyValue operation : block.values()) {
        Opcode opcode = operation.opcode();
        if (opcode == Opcode::Await) {
            await = operation.id();
            break;
        }
        if (!opcodesThatStateTypes.contains(opcode) && !declaredBoundInTable(opcode).isPure())
            return { };
    }
    if (!await || index.value(await).takesExtraTurn())
        return { };

    ValueID awaited = index.value(await).valueOperand();
    ValueID source = awaited;
    while (!index.isParameter(source)) {
        if (!opcodesThatStateTypes.contains(index.opcodeOf(source)) || !index.hasOneUse(source) || !block.holds(source))
            return { };
        source = index.value(source).operand(0);
    }
    if (!block.indexOfParameter(source) || !index.hasOneUse(source))
        return { };

    for (uint32_t value = block.firstValue().index(); value < await.index(); ++value) {
        ValueID made(value);
        if (index.isUnused(made))
            continue;
        ValueID user = index.onlyUsingValue(made);
        if (!user || !block.holds(user) || user.index() > await.index() || (user == await && made != awaited))
            return { };
    }
    return await;
}

// Among the parameters of a block for which awaitTakenByEdgesInto() has answered with the Await.
static uint32_t positionOfAwaitedParameterInAwaitsAtJoins(const BodyIndex& index, BodyBlock block, ValueID await)
{
    if (!block.holds(await))
        return 0;
    return *block.indexOfParameter(index.valueBehindCasts(index.value(await).valueOperand()));
}

// `await (a ? f() : b ? g() : h())`: the inner join has one parameter, casts of it and a jump that hands it on. The name under which it is handed on. Null: the block does more.
static ValueID valueHandedOnInAwaitsAtJoins(const BodyIndex& index, BodyBlock block)
{
    if (block.numberOfParameters() != 1 || block.terminator().kind() != TerminatorKind::Jump)
        return { };
    ValueID handed = block.parameter(0);
    for (BodyValue operation : block.values()) {
        if (!opcodesThatStateTypes.contains(operation.opcode()) || operation.operand(0) != handed || !index.hasOneUse(handed))
            return { };
        handed = operation.id();
    }
    std::optional<BodyUser> user = index.onlyUser(handed);
    if (!user || user->kind != BodyUseKind::EdgeArgument || user->block != block.id())
        return { };
    return handed;
}

static bool standUnderOneHandlerInAwaitsAtJoins(BodyBlock first, BodyBlock second)
{
    if (first.hasHandler() != second.hasHandler())
        return false;
    if (!first.hasHandler())
        return true;
    return first.handler().block() == second.handler().block() && std::ranges::equal(first.handler().arguments(), second.handler().arguments());
}

// What the jump brings is bound anew in front of the jump, so nothing else reads it.
static bool everyEdgeCanTakeAwaitInAwaitsAtJoins(Walker& walker, const BodyIndex& index, BodyBlock block, uint32_t positionOfAwaited)
{
    BlockID id = block.id();
    uint32_t numberOfEdges = index.numberOfIncomingEdges(id);
    if (numberOfEdges < 2 || numberOfEdges == BodyIndex::maxCountedIncomingEdges)
        return false;
    if (!index.immediateDominator(id) || index.hasIrreducibleFlow() || index.isLoopHead(id) || index.isEnteredByHandlerEdge(id) || block.isHandlerBlock())
        return false;
    for (const BodyIncomingEdge& edge : walker.incomingEdgesOf(index, id)) {
        BodyBlock from = index.block(edge.from);
        if (from.terminator().kind() != TerminatorKind::Jump || !edge.target.proofs().empty() || edge.target.arguments().size() != block.numberOfParameters())
            return false;
        if (index.loopOf(edge.from) != index.loopOf(id) || !standUnderOneHandlerInAwaitsAtJoins(from, block))
            return false;
        if (!index.hasOneUse(edge.target.arguments()[positionOfAwaited]))
            return false;
    }
    return true;
}

// An await on each way costs about 60 bytes a way. It pays where a call on one of them may go on to it: a wrong yes costs those bytes, a wrong no the promise.
static bool someEdgeBringsCallInAwaitsAtJoins(Walker& walker, const BodyIndex& index, BodyBlock block, uint32_t positionOfAwaited)
{
    for (const BodyIncomingEdge& edge : walker.incomingEdgesOf(index, block.id())) {
        BodyBlock from = index.block(edge.from);
        ValueID brought = edge.target.arguments()[positionOfAwaited];
        if (valueHandedOnInAwaitsAtJoins(index, from) == brought && everyEdgeCanTakeAwaitInAwaitsAtJoins(walker, index, from, 0)) {
            if (someEdgeBringsCallInAwaitsAtJoins(walker, index, from, 0))
                return true;
            continue;
        }
        ValueID call = index.valueBehindCasts(brought);
        if (index.isParameter(call) || !from.holds(call) || effectRuleOf(index.opcodeOf(call)) != EffectRule::Call)
            continue;
        ValueID name = call;
        if (!followCastsToOnlyTaker(index, name) && name == brought)
            return true;
    }
    return false;
}

static ValueID findAwaitTakenByEdgesInAwaitsAtJoins(Walker& walker, const BodyIndex& index, BodyBlock block)
{
    if (ValueID await = findAwaitAtHeadInAwaitsAtJoins(index, block)) {
        uint32_t position = positionOfAwaitedParameterInAwaitsAtJoins(index, block, await);
        return everyEdgeCanTakeAwaitInAwaitsAtJoins(walker, index, block, position) && someEdgeBringsCallInAwaitsAtJoins(walker, index, block, position) ? await : ValueID();
    }

    // The block leaves the await to the edges into it.
    ValueID handed = valueHandedOnInAwaitsAtJoins(index, block);
    if (!handed || !everyEdgeCanTakeAwaitInAwaitsAtJoins(walker, index, block, 0))
        return { };
    BodyTarget onward = block.terminator().targets()[0];
    ValueID await = awaitTakenByEdgesInto(walker, onward.block());
    if (!await)
        return { };
    uint32_t position = positionOfAwaitedParameterInAwaitsAtJoins(index, index.block(onward.block()), await);
    return position < onward.arguments().size() && onward.arguments()[position] == handed ? await : ValueID();
}

ValueID awaitTakenByEdgesInto(Walker& walker, BlockID inputBlock)
{
    // NO EDGE TAKES AN AWAIT. The await of each edge was built in front of the jump by walkInFrontOfTerminatorOfCoroutine(), which Walker::walkTerminator() alone calls. But a block may be PASSED BY: a switch over a union, tests on a field and others
    // lead its jump on through Walker::endBlockLeadingTo(), which knows nothing of that, and a branch that leads two edges into the join has no place in front of it for the await of either. On such an edge nothing was awaited,
    // and the Await in the join was bound to its operand: `await (slow ? load(k) : (k === 1 || k === 4))` did not suspend.
    constexpr bool edgesMayTakeAwaits = false;
    if (!edgesMayTakeAwaits || walker.depthOfCopies() || !walker.has(Mechanism::DirectCalls))
        return { };
    const BodyIndex& index = walker.currentBodyIndex();
    if (!index.holdsAnyOf(OpcodeSet { Opcode::Await }) || !findAwaitingBodyOfCoroutine(walker))
        return { };

    KeptByCallsOfCoroutines& kept = keptByCallsOfCoroutines(walker);
    if (kept.awaitsTakenByEdges.isEmpty())
        kept.awaitsTakenByEdges.fill(ValueID(), index.numberOfBlocks());
    if (!kept.blocksAskedForAwaitTakenByEdges.get(inputBlock.index())) {
        kept.blocksAskedForAwaitTakenByEdges.set(inputBlock.index());
        ValueID await = findAwaitTakenByEdgesInAwaitsAtJoins(walker, index, index.block(inputBlock));
        kept.awaitsTakenByEdges[inputBlock.index()] = await;
        kept.someAwaitIsTakenByEdges |= !!await;
    }
    return kept.awaitsTakenByEdges[inputBlock.index()];
}

ValueID findAwaitThatJumpBringsValueTo(Walker& walker, const BodyIndex& index, BodyBlock block, ValueID brought)
{
    std::optional<BodyUser> user = index.onlyUser(brought);
    if (!user || user->kind != BodyUseKind::EdgeArgument || user->block != block.id() || block.terminator().kind() != TerminatorKind::Jump)
        return { };
    BodyTarget target = block.terminator().targets()[0];
    ValueID await = awaitTakenByEdgesInto(walker, target.block());
    if (!await || awaitTakenByEdgesInto(walker, block.id()) == await)
        return { };
    uint32_t position = positionOfAwaitedParameterInAwaitsAtJoins(index, index.block(target.block()), await);
    return target.arguments()[position] == brought ? await : ValueID();
}

// ---- Where the walk stands

std::optional<Vector<Reference, 4>> findWhatAwaitHoldsOnWayThrough(Walker& walker, const BodyIndex& index, BodyTarget target, ValueID await)
{
    // By parameter of the block that the way has come to.
    Vector<Reference, 8> brought;
    for (ValueID argument : target.arguments())
        brought.append(walker.referenceOf(argument));
    BlockID reached = target.block();
    while (!index.block(reached).holds(await)) {
        BodyBlock block = index.block(reached);
        ValueID handed = valueHandedOnInAwaitsAtJoins(index, block);
        BodyTarget onward = block.terminator().targets()[0];
        Vector<Reference, 8> broughtOnward;
        for (ValueID argument : onward.arguments())
            broughtOnward.append(argument == handed ? brought[0] : walker.referenceOf(argument));
        brought = WTF::move(broughtOnward);
        reached = onward.block();
    }

    BodyBlock block = index.block(reached);
    Vector<Reference, 4> held;
    for (ValueID operand : index.value(await).heldOperands()) {
        std::optional<uint32_t> parameter = block.indexOfParameter(operand);
        Reference reference = parameter ? brought[*parameter] : walker.referenceOf(operand);
        if (!reference) {
            walker.fail("an await that the edges into its block take holds a value that has no name on one of them"_s);
            return std::nullopt;
        }
        held.append(reference);
    }
    return held;
}

void walkInFrontOfTerminatorOfCoroutine(Walker& walker, const BodyTerminator& terminator)
{
    if (terminator.kind() != TerminatorKind::Jump || walker.depthOfCopies() || walker.isUnreachable())
        return;
    BodyTarget target = terminator.targets()[0];
    ValueID await = awaitTakenByEdgesInto(walker, target.block());
    // A block that only hands on what the edges into it have awaited builds nothing.
    if (!await || awaitTakenByEdgesInto(walker, terminator.block()) == await)
        return;

    const BodyIndex& index = walker.currentBodyIndex();
    ValueID brought = target.arguments()[positionOfAwaitedParameterInAwaitsAtJoins(index, index.block(target.block()), await)];
    if (keptByCallsOfCoroutines(walker).broughtValuesAwaited.removeFirst(brought))
        return;
    Reference awaited = walker.referenceOf(brought);
    if (!awaited) {
        walker.fail("a jump brings a value of the body that nothing was built for"_s);
        return;
    }
    std::optional<Vector<Reference, 4>> held = findWhatAwaitHoldsOnWayThrough(walker, index, target, await);
    if (!held)
        return;
    if (Reference given = buildAwaitBySavingFrame(walker, awaited, held->span(), false, walker.originOf(index.value(await))))
        walker.bind(brought, given);
}

bool takeWhatEdgesHaveAwaited(Walker& walker, const BodyValue& await)
{
    KeptByCallsOfCoroutines* kept = findKeptByCallsOfCoroutines(walker);
    if (await.opcode() != Opcode::Await || walker.depthOfCopies() || !kept)
        return false;
    if (!kept->awaitsTakenInCopies.removeFirst(await.id())) {
        if (!kept->someAwaitIsTakenByEdges || awaitTakenByEdgesInto(walker, walker.currentBodyIndex().blockOf(await.id())) != await.id())
            return false;
    }
    Reference given = walker.referenceOf(await.valueOperand());
    if (!given) {
        walker.fail("nothing was built for what an await takes that was built ahead of its place"_s);
        return true;
    }
    walker.bind(await.id(), given);
    return true;
}

bool nameCastInFrontOfAwaitTakenByEdges(Walker& walker, const BodyValue& cast)
{
    KeptByCallsOfCoroutines* kept = findKeptByCallsOfCoroutines(walker);
    if (!kept || walker.depthOfCopies() || (!kept->someAwaitIsTakenByEdges && kept->awaitsTakenInCopies.isEmpty()))
        return false;

    const BodyIndex& index = walker.currentBodyIndex();
    bool standsBehindAwait = !kept->awaitsTakenInCopies.isEmpty() && kept->awaitsTakenInCopies.contains(findOnlyTakerBehindCasts(index, cast.id()));
    if (!standsBehindAwait && kept->someAwaitIsTakenByEdges) {
        BodyBlock block = index.block(index.blockOf(cast.id()));
        if (ValueID await = awaitTakenByEdgesInto(walker, block.id())) {
            std::optional<uint32_t> parameter = block.indexOfParameter(index.valueBehindCasts(cast.id()));
            standsBehindAwait = parameter && *parameter == positionOfAwaitedParameterInAwaitsAtJoins(index, block, await);
        }
    }
    if (!standsBehindAwait)
        return false;
    Reference given = walker.referenceOf(cast.operand(0));
    if (!given)
        return false;
    walker.bind(cast.id(), given);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
