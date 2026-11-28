/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkBranches.h"

#if ENABLE(AOT)

#include "AOTEffectsOfOperations.h"
#include "AOTOpcodeSets.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static_assert(boundToOf(Opcode::CatchValue) == BoundTo::Frame && boundToOf(Opcode::CatchException) == BoundTo::Frame);

static constexpr OpcodeSet callsThatMayGoByOneRowInWalkExceptions { Opcode::Call, Opcode::Construct };
static constexpr OpcodeSet constantsTypedAsWrittenInWalkExceptions = primitiveConstants | OpcodeSet { Opcode::BigInt, Opcode::Builtin };

bool readsNothingOfWhatItCatches(const BodyIndex& index, BodyBlock handler)
{
    if (handler.role() != BlockRole::CatchHandler)
        return false;
    for (BodyValue value : handler.values()) {
        if (!catchOpcodes.contains(value.opcode()))
            break;
        if (!index.isUnused(value.id()))
            return false;
    }
    return true;
}

static uint32_t numberOfCatchesAtHeadInWalkExceptions(BodyBlock handler)
{
    uint32_t numberOfCatches = 0;
    for (BodyValue value : handler.values()) {
        if (!catchOpcodes.contains(value.opcode()))
            break;
        ++numberOfCatches;
    }
    return numberOfCatches;
}

bool Walker::handlerInForceIgnoresWhatIsThrown() const
{
    if (isUnreachable())
        return false;
    if (hasStringsInBuilders() || handlerMadeOverCopiesIsInForce())
        return false;
    if (EdgeID led = m_code.at(m_block).handler; led && !m_code.at(led).isOpen())
        return false;

    size_t frameIndex = numberOfFramesUpToHandlerInForce();
    if (m_numberOfFramesOverHandlersOfCode && handlerOfCodeInForce(frameIndex))
        return false;
    if (!frameIndex--)
        return false;

    const Frame& frame = m_frames[frameIndex];
    const BodyIndex& index = *frame.index;
    BlockID handler = index.block(frame.block).handler().block();
    if (index.hasIrreducibleFlow() || index.isLoopHead(handler))
        return false;
    if (!frame.blocks.isEmpty() && frame.blocks[handler.index()].isEntered)
        return false;
    return readsNothingOfWhatItCatches(index, index.block(handler));
}

bool Walker::jumpToHandlerInPlaceOfThrow(const BodyTerminator&)
{
    if (!handlerInForceIgnoresWhatIsThrown())
        return false;
    size_t frameIndex = numberOfFramesUpToHandlerInForce() - 1;
    if (!m_generatorsWalkedInConsumers.isEmpty())
        bindParametersOfHeadForHandlerOfStep(frameIndex);

    noteStateAtThrowForHandler();

    BodyTarget target = m_frames[frameIndex].index->block(m_frames[frameIndex].block).handler();
    Vector<Reference, 4> arguments;
    Vector<TypeID, 4> types;
    for (ValueID argument : target.arguments()) {
        Reference reference = nameBehindStandIn(frameIndex, argument, referenceBoundIn(m_frames[frameIndex], argument));
        TypeID type = ownTypeOf(reference);
        if (!type) {
            fail("a handler edge hands on a value that nothing was built for"_s);
            return true;
        }
        if (m_inputs.options.verifiesCode && !isWhatHeadNamesForHandler(frameIndex, argument, reference)) [[unlikely]] {
            fail("a handler edge passes a placeholder for a value"_s, InternalErrorKind::HandlerEdgePassesPlaceholder);
            return true;
        }
        arguments.append(reference);
        types.append(type);
    }

    uint32_t place = addOpenEdge(frameIndex, target.block());
    if (place == noOpenEdge)
        return true;
    BlockID from = m_block;
    m_code.end(from, TerminatorDraft(TerminatorKind::Jump, m_origin), 1);
    OpenEdge& open = m_openEdges[place];
    open.edge = m_code.at(from).terminator.next();
    open.arguments = WTF::move(arguments);
    open.types = WTF::move(types);
    takeNamesOntoEdge(open, frameIndex);
    if (m_frames[frameIndex].references.size() > m_frames[frameIndex].index->numberOfValues())
        nameCarriedValuesOnEdge(open, frameIndex);
    m_block = { };
    m_state.clear();
    return true;
}

bool Walker::enterBehindHeadOfHandlerBlock(BlockID inputBlock, std::span<const uint32_t> edges)
{
    Vector<uint32_t, 4> thrownEdges;
    Vector<uint32_t, 4> jumpedEdges;
    for (uint32_t place : edges) {
        EdgeID edge = m_openEdges[place].edge;
        (m_code.at(m_code.at(edge).from).handler == edge ? thrownEdges : jumpedEdges).append(place);
    }
    if (jumpedEdges.isEmpty())
        return false;

    BodyBlock input = currentBodyIndex().block(inputBlock);
    uint32_t numberOfCatches = numberOfCatchesAtHeadInWalkExceptions(input);

    if (!thrownEdges.isEmpty()) {
        BlockID catching = addBlockFor(inputBlock, input.role(), BlockFrequency::Unlikely);
        closeEdgesInto(catching, inputBlock, thrownEdges.span(), true);
        if (m_hasFailed)
            return true;
        if (ValueID subject = input.role() == BlockRole::IterationCleanup ? input.subject() : ValueID()) {
            if (Reference reference = referenceOf(subject); reference && reference.isRecord())
                m_code.setSubject(catching, reference.id());
        }
        m_state.clear();
        walkStretch(inputBlock, 0, numberOfCatches);
        if (m_hasFailed)
            return true;

        if (!isUnreachable()) {
            BlockID from = m_block;
            m_code.end(from, TerminatorDraft(TerminatorKind::Jump, m_origin), 1);
            m_block = { };
            uint32_t place = m_firstFreeOpenEdge;
            if (place != noOpenEdge)
                m_firstFreeOpenEdge = m_openEdges[place].next;
            else {
                place = m_openEdges.size();
                m_openEdges.append(OpenEdge { });
            }
            OpenEdge& open = m_openEdges[place];
            open.next = noOpenEdge;
            open.edge = m_code.at(from).terminator.next();
            for (uint32_t index = 0; index < input.numberOfParameters(); ++index) {
                Reference brought = referenceOf(input.parameter(index));
                open.arguments.append(brought);
                open.types.append(ownTypeOf(brought));
            }
            takeNamesOntoEdge(open, m_frames.size() - 1);
            jumpedEdges.insert(0, place);
        }
    }

    closeEdgesInto(addBlockFor(inputBlock, BlockRole::Ordinary, BlockFrequency::Unlikely), inputBlock, jumpedEdges.span(), false);
    if (m_hasFailed)
        return true;

    Reference nothing = constant(Draft(Opcode::Undefined, m_origin));
    for (uint32_t place = 0; place < numberOfCatches; ++place)
        bind(ValueID(input.firstValue().index() + place), nothing);

    m_state.clear();
    Frame& frame = m_frames.last();
    keepStateOfHandlerForUndo(m_frames.size() - 1, inputBlock);
    frame.takeStateOfHandler(inputBlock, m_state);
    enterHandler(*this, inputBlock);
    frame.firstOperationToWalk = numberOfCatches;
    return true;
}

void Walker::walkOperationsOf(BlockID inputBlock)
{
    size_t frameIndex = m_frames.size() - 1;
    const BodyIndex& index = *m_frames[frameIndex].index;
    BodyBlock block = index.block(inputBlock);
    uint32_t first = std::exchange(m_frames[frameIndex].firstOperationToWalk, 0);
    uint32_t end = block.numberOfValues();
    m_frames[frameIndex].hasPassedByWhatFeedsThrow = false;

    BodyTerminator terminator = block.terminator();
    bool endsInThrow = terminator.kind() == TerminatorKind::Throw || terminator.kind() == TerminatorKind::Rethrow;
    if (!endsInThrow || first >= end || !has(Mechanism::Handlers) || !handlerInForceIgnoresWhatIsThrown()) {
        walkStretch(inputBlock, first, end);
        return;
    }

    uint32_t firstValue = block.firstValue().index();
    Vector<uint8_t, 32> mentions;
    mentions.fill(0, end);
    auto countMention = [&](ValueID operand, uint32_t) {
        if (operand.index() >= firstValue && block.holds(operand) && mentions[operand.index() - firstValue] < BodyIndex::maxCountedUses)
            ++mentions[operand.index() - firstValue];
    };
    terminator.forEachOperand(countMention);

    uint32_t firstThatOnlyFeedsThrow = end;
    for (uint32_t place = end; place-- > first;) {
        ValueID id(firstValue + place);
        uint32_t uses = index.useCount(id);
        if (uses == BodyIndex::maxCountedUses || uses != mentions[place])
            break;
        BodyValue operation = index.value(id);
        if (!callsThatMayGoByOneRowInWalkExceptions.contains(operation.opcode()) && !isUnseenWhenUnused(*this, operation))
            break;
        operation.forEachOperand(countMention);
        firstThatOnlyFeedsThrow = place;
    }
    walkStretch(inputBlock, first, firstThatOnlyFeedsThrow);
    if (firstThatOnlyFeedsThrow == end || isUnreachable())
        return;

    uint32_t firstValueNotWalked = firstValue + firstThatOnlyFeedsThrow;
    auto typeOfInput = [&](ValueID input) -> TypeID {
        TypeID type;
        if (input.index() < firstValueNotWalked)
            type = typeOf(referenceOf(input));
        else if (BodyValue written = index.value(input); constantsTypedAsWrittenInWalkExceptions.contains(written.opcode()))
            type = widestTypeOfOperation(*this, written);
        return type ? type : TypeID(TypeTable::anything);
    };
    TypesOfInputs typesOfInputs(typeOfInput);
    auto isUnseenWhateverItThrows = [&](const BodyValue& operation) {
        if (isUnseenWhenUnused(*this, operation))
            return true;
        std::optional<Effects> effects = has(Mechanism::BuiltinCalls) ? effectsOfCallByOneRowAhead(*this, operation, typesOfInputs) : std::nullopt;
        if (!effects)
            return false;
        effects->mayThrow = false;
        return effects->isRemovableWhenUnused();
    };
    uint32_t firstPassedBy = end;
    while (firstPassedBy > firstThatOnlyFeedsThrow && isUnseenWhateverItThrows(index.value(ValueID(firstValue + firstPassedBy - 1))))
        --firstPassedBy;

    walkStretch(inputBlock, firstThatOnlyFeedsThrow, firstPassedBy);
    if (firstPassedBy == end || isUnreachable())
        return;
    if (!handlerInForceIgnoresWhatIsThrown()) {
        walkStretch(inputBlock, firstPassedBy, end);
        return;
    }
    m_frames[frameIndex].hasPassedByWhatFeedsThrow = true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
