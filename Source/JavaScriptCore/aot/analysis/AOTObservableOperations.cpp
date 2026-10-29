/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTObservableOperations.h"

#if ENABLE(AOT)

#include "AOTCFG.h"
#include "AOTDefinitions.h"
#include "AOTEffectsDeclared.h"
#include "AOTTypePropagation.h"
#include <wtf/BitVector.h>
#include <wtf/HashSet.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr bool isSeenByProgram(const Effects& effects)
{
    return effects.mayThrowByRule() || effects.mayRunOtherCode();
}

// What the declared effects of an operation leave open and the types of its operands settle.
static bool cannotHappenGivenOperands(const Program& program, const Function& function, const Value& value)
{
    // A cast that throws takes away of the type what it throws for.
    if (interpretedOf(value.opcode) == Interpretation::Cast)
        return value.type == function.at(value.operand(0)).type;
    if (value.opcode == Opcode::ResolveHeritage)
        return hasOnlyClassesAndBuiltins(program, program.typeOf(function, value.heritageOperand()));
    return false;
}

static bool isSeenByProgram(const EffectContext& context, const Function& function, const Value& value)
{
    return isSeenByProgram(effectsOf(context, function, value).onAnyEdge()) && !cannotHappenGivenOperands(context.program, function, value);
}

static bool hasOperandBehindCheck(const PredictedValues& predicted, const Instruction& instruction)
{
    bool hasOne = false;
    instruction.forEachOperand([&](ValueID operand, uint32_t) {
        hasOne |= predicted.isBehindCheck(operand);
    });
    return hasOne;
}

bool ObservableOperations::isOwedToCheck(const Instruction& instruction)
{
    if (!m_predicted)
        m_predicted.emplace(m_function, CFG(m_function));
    return hasOperandBehindCheck(*m_predicted, instruction);
}

bool ObservableOperations::neitherThrowsNorRunsCode(ValueID id)
{
    const Value& value = m_function.at(id);
    if (!isSeenByProgram(declaredBoundOf(value.opcode)))
        return true;
    return !isSeenByProgram(m_context, m_function, value) && !isOwedToCheck(value);
}

// Kept under the number that the value had when the run began.
void ReasonsForOperationsThatWent::add(ValueID id, InPlaceOfOperation reason)
{
    if (m_outer)
        m_outer->add(id, reason);
    if (!m_currentValues.isEmpty()) {
        size_t index = m_currentValues.find(id);
        if (index == notFound)
            return;
        id = ValueID(index);
    }
    m_explained.set(id.index());
}

void ReasonsForOperationsThatWent::addForOperationThatWent(ValueID idBeforeRun, InPlaceOfOperation reason)
{
    m_explained.set(idBeforeRun.index());
    if (!m_outer)
        return;
    if (m_valuesOfOuterAtStart.isEmpty()) {
        m_outer->addForOperationThatWent(idBeforeRun, reason);
        return;
    }
    size_t index = m_valuesOfOuterAtStart.find(idBeforeRun);
    if (index != notFound)
        m_outer->addForOperationThatWent(ValueID(index), reason);
}

template<typename SomeID>
static void renumber(Vector<SomeID>& current, std::span<const SomeID> newIDs)
{
    if (newIDs.empty())
        return;
    if (current.isEmpty()) {
        current.append(newIDs);
        return;
    }
    for (SomeID& id : current)
        id = id && id.index() < newIDs.size() ? newIDs[id.index()] : SomeID();
}

void ReasonsForOperationsThatWent::didRenumber(std::span<const BlockID> newBlocks, std::span<const ValueID> newValues)
{
    if (m_outer)
        m_outer->didRenumber(newBlocks, newValues);
    renumber(m_currentBlocks, newBlocks);
    renumber(m_currentValues, newValues);
}

void ReasonsForOperationsThatWent::didReplaceBody()
{
    if (m_outer)
        m_outer->didReplaceBody();
    m_hasLostTrack = true;
}

void didReplaceBody(ReasonsForOperationsThatWent& reasons)
{
    reasons.didReplaceBody();
}

Vector<OperationThatWent> findOperationsThatWentWithoutReason(const EffectContext& context, const Function& before, const Function& after, ReasonsForOperationsThatWent& reasons)
{
    Vector<OperationThatWent> result;
    if (reasons.hasLostTrack() || !before.hasBody() || !after.hasBody() || before.bodyIsReleased || after.bodyIsReleased || !hasTypedValues(before) || !hasTypedValues(after))
        return result;

    CFG cfgBefore(before);
    CFG cfgAfter(after);

    // Where each value stands behind the run.
    BitVector isListedAfter(after.values.size());
    BitVector hasHandlerAfter(after.values.size());
    Vector<BlockID> blockAfter(after.values.size());
    for (BlockID id : cfgAfter.reversePostOrder()) {
        const Block& block = after.at(id);
        for (ValueID value : block.values) {
            isListedAfter.quickSet(value.index());
            blockAfter[value.index()] = id;
            if (block.handler)
                hasHandlerAfter.quickSet(value.index());
        }
    }

    // What a pass puts in the place of an operation has the operation's place in the source, where its errors are reported. Looked for when something has gone.
    BitVector wasListedBefore(before.values.size());
    for (BlockID id : cfgBefore.reversePostOrder()) {
        for (ValueID value : before.at(id).values)
            wasListedBefore.quickSet(value.index());
    }
    Vector<ValueID> idsBeforeRun(after.values.size());
    for (size_t index = 0; index < before.values.size(); ++index) {
        ValueID now = reasons.currentIDOf(ValueID(index));
        if (now && now.index() < after.values.size())
            idsBeforeRun[now.index()] = ValueID(index);
    }
    std::optional<HashSet<uint64_t>> placesOfNewOperations;
    auto placeOf = [](const Instruction& instruction) {
        Origin origin = instruction.origin();
        return (static_cast<uint64_t>(origin.inlineFrame) << 32 | origin.position) + 1;
    };
    auto hasNewOperationAtPlaceOf = [&](const Value& value) {
        if (!value.hasPosition())
            return false;
        if (!placesOfNewOperations) {
            placesOfNewOperations.emplace();
            for (BlockID id : cfgAfter.reversePostOrder()) {
                const Block& block = after.at(id);
                for (ValueID now : block.values) {
                    const Value& candidate = after.at(now);
                    ValueID former = idsBeforeRun[now.index()];
                    bool isNew = !former || !wasListedBefore.quickGet(former.index()) || before.at(former).opcode != candidate.opcode;
                    if (isNew && candidate.hasPosition() && isSeenByProgram(declaredBoundOf(candidate.opcode)) && isSeenByProgram(context, after, candidate))
                        placesOfNewOperations->add(placeOf(candidate));
                }
                if (block.terminator.hasPosition() && isSeenByProgram(declaredBoundOf(block.terminator.kind)))
                    placesOfNewOperations->add(placeOf(block.terminator));
            }
        }
        return placesOfNewOperations->contains(placeOf(value));
    };

    std::optional<PredictedValues> predicted;
    auto isOwedToCheck = [&](const Value& value) {
        if (!predicted)
            predicted.emplace(before, cfgBefore);
        return predicted->hasChecks() && hasOperandBehindCheck(*predicted, value);
    };

    for (BlockID blockID : cfgBefore.reversePostOrder()) {
        const Block& block = before.at(blockID);
        // A block that nothing reaches any more went as a whole: whoever decided the branch answers for that.
        BlockID blockNow = reasons.currentIDOf(blockID);
        bool blockIsReached = blockNow && blockNow.index() < after.blocks.size() && cfgAfter.isReachable(blockNow);
        auto findCurrent = [&](ValueID id) {
            ValueID now = reasons.currentIDOf(id);
            return now && now.index() < after.values.size() && isListedAfter.quickGet(now.index()) ? now : ValueID();
        };
        // The end of a block goes where something in front of it never completes: what is left of the block ends without a way on.
        size_t numberOfValuesUpToLastOneLeft = block.values.size();
        while (numberOfValuesUpToLastOneLeft && !findCurrent(block.values[numberOfValuesUpToLastOneLeft - 1]))
            --numberOfValuesUpToLastOneLeft;
        BlockID whereBlockEnds = numberOfValuesUpToLastOneLeft ? blockAfter[findCurrent(block.values[numberOfValuesUpToLastOneLeft - 1]).index()] : blockIsReached ? blockNow : BlockID();
        bool endWasCut = whereBlockEnds && (after.at(whereBlockEnds).terminator.kind == TerminatorKind::Unreachable || after.at(whereBlockEnds).terminator.kind == TerminatorKind::Throw || after.at(whereBlockEnds).terminator.kind == TerminatorKind::Rethrow)
            && after.at(whereBlockEnds).terminator.kind != block.terminator.kind;
        for (size_t index = 0; index < block.values.size(); ++index) {
            ValueID id = block.values[index];
            const Value& value = before.at(id);
            ValueID now = findCurrent(id);
            bool isListed = !!now;
            if (isListed && block.handler && !hasHandlerAfter.quickGet(now.index()) && effectsOf(context, after, after.at(now)).normal.mayThrow && !reasons.hasReasonFor(id))
                result.append({ id, blockID, value.opcode, OperationThatWent::How::LostItsHandler, OperationThatWent::Under::TypesAsTheyStand });
            // An operation that has become another one is the matter of whoever rewrote it.
            if (isListed)
                continue;
            if (!blockIsReached || (endWasCut && index >= numberOfValuesUpToLastOneLeft))
                continue;
            const Effects& bound = declaredBoundOf(value.opcode);
            if (!isSeenByProgram(bound))
                continue;
            Effects effects = effectsOf(context, before, value).onAnyEdge();
            bool isSeen = isSeenByProgram(effects) && !cannotHappenGivenOperands(context.program, before, value);
            if (!isSeen && !isOwedToCheck(value)) {
                // Whoever has listened for longer knows the operands by the types that they had when it began. Each run in between has answered for what it made of them.
                if (reasons.outer())
                    reasons.addForOperationThatWent(id, InPlaceOfOperation::NothingByProofOfThePass);
                continue;
            }
            if (reasons.hasReasonFor(id) || hasNewOperationAtPlaceOf(value))
                continue;
            result.append({ id, blockID, value.opcode, OperationThatWent::How::IsGone, isSeen ? OperationThatWent::Under::TypesAsTheyStand : OperationThatWent::Under::TypesWithoutChecks });
        }
    }
    return result;
}

String describe(const OperationThatWent& operation)
{
    return makeString("v"_s, operation.value.index(), " ("_s, nameOf(operation.opcode), ") in b"_s, operation.block.index(), " may throw or run code"_s,
        operation.under == OperationThatWent::Under::TypesWithoutChecks ? " where a check has failed"_s : ""_s,
        operation.how == OperationThatWent::How::IsGone ? ", and is gone"_s : ", and the handler of its block is gone"_s);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
