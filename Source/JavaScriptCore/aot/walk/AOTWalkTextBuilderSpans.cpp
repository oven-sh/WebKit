/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkTextBuilders.h"

#if ENABLE(AOT)

#include "AOTDeclaredType.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfOperations.h"
#include "AOTIRProgram.h"
#include "AOTOpcodeSets.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

static bool namesTextInTextBuilderSpans(const GrowingText& web, ValueID value)
{
    auto found = std::ranges::lower_bound(web.names, value, { }, &NameOfGrowingText::value);
    return found != web.names.end() && found->value == value;
}

static bool comesRoundAsJoinInTextBuilderSpans(Walker& walker, const BodyIndex& index, BlockID head, uint32_t parameterIndex)
{
    LoopID loop = index.loopOf(head);
    BodyBlock block = index.block(head);
    ValueID parameter = block.parameter(parameterIndex);
    bool changesOnSomeEdge = false;
    for (const BodyIncomingEdge& edge : walker.incomingEdgesOf(index, head)) {
        if (!index.isInLoop(edge.from, loop))
            continue;
        std::span<const ValueID> arguments = edge.target.arguments();
        if (parameterIndex >= arguments.size() || !arguments[parameterIndex])
            return false;
        ValueID root = index.valueBehindCasts(arguments[parameterIndex]);
        if (root == parameter)
            continue;
        bool isMadeInLoop = root.index() >= block.firstValue().index();
        if (!isMadeInLoop || (!index.isParameter(root) && !operationsThatJoinTexts.contains(index.opcodeOf(root))))
            return false;
        changesOnSomeEdge = true;
    }
    return changesOnSomeEdge;
}

bool textStaysInStringBuilderOnEdge(Walker& walker, const TextInStringBuilder& text, BlockID to, BodyTarget target)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (text.frameIndex != walker.indexOfFrameOnTop() || text.web.standsUnderHandlers || index.hasIrreducibleFlow())
        return false;
    LoopID left = index.loopOf(text.web.loopHead);
    if (!left || index.isInLoop(to, left))
        return false;
    if (walker.edgeLeavesLoopBeingBuiltTwice(to))
        return false;
    LoopID around = index.loop(left).parent;

    Vector<ValueID, 4> handedOn;
    auto holdsText = [&](ValueID value) {
        return value && (namesTextInTextBuilderSpans(text.web, value) || handedOn.contains(value));
    };

    std::optional<uint32_t> taking;
    for (;;) {
        if (!target.proofs().empty())
            return false;
        taking = std::nullopt;
        std::span<const ValueID> arguments = target.arguments();
        for (uint32_t argumentIndex = 0; argumentIndex < arguments.size(); ++argumentIndex) {
            if (!holdsText(arguments[argumentIndex]))
                continue;
            if (taking)
                return false;
            taking = argumentIndex;
        }

        BodyBlock block = index.block(to);
        if (block.isHandlerBlock() || block.hasHandler() || (taking && *taking >= block.numberOfParameters()))
            return false;
        if (index.isLoopHead(to))
            break;
        if (index.loopOf(to) != around || index.numberOfIncomingEdges(to) != 1)
            return false;
        for (BodyValue operation : block.values()) {
            if (!declaredBoundInTable(operation.opcode()).isUnobservable())
                return false;
            bool mentionsText = false;
            operation.forEachOperand([&](ValueID operand, uint32_t) {
                mentionsText |= holdsText(operand);
            });
            if (mentionsText)
                return false;
        }
        BodyTerminator terminator = block.terminator();
        if (terminator.kind() != TerminatorKind::Jump)
            return false;
        if (taking)
            handedOn.append(block.parameter(*taking));
        target = terminator.next();
        to = target.block();
    }

    LoopID entered = index.loopOf(to);
    if (!taking || index.loop(entered).parent != around || index.isEnteredByHandlerEdge(to))
        return false;
    size_t numberOfEntries = std::ranges::count_if(walker.incomingEdgesOf(index, to), [&](const BodyIncomingEdge& edge) {
        return !index.isInLoop(edge.from, entered);
    });
    if (numberOfEntries != 1)
        return false;

    for (const NameOfGrowingText& name : text.web.names) {
        if (walker.isLiveAtHeadOf(name.value, to))
            return false;
    }
    for (ValueID parameter : handedOn) {
        if (walker.isLiveAtHeadOf(parameter, to))
            return false;
    }

    if (text.liesInFrame) {
        for (BlockID member : index.membersOf(entered)) {
            if (index.block(member).hasHandler())
                return false;
        }
    }
    return comesRoundAsJoinInTextBuilderSpans(walker, index, to, *taking);
}

namespace {

struct LinkInTextBuilderSpans {
    ValueID join;
    uint32_t positionOfPart { 0 };
};

enum class PartInSightInTextBuilderSpans : uint8_t { No, Primitive, Text };

struct PartAppendedOnEdgesInTextBuilderSpans {
    uint32_t parameterIndex { 0 };
    ValueID front;
    ValueID lastLink;
    bool everyEdgeBringsString { false };
};

} // anonymous namespace

static LinkInTextBuilderSpans linkThatAloneReadsPartInTextBuilderSpans(const BodyIndex& index, BodyBlock block, ValueID part)
{
    while (true) {
        ValueID user = index.onlyUsingValue(part);
        if (!user || !block.holds(user))
            return { };
        BodyValue operation = index.value(user);
        Opcode opcode = operation.opcode();
        if (operationsThatJoinTexts.contains(opcode)) {
            LinkInTextBuilderSpans link { user, 0 };
            operation.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                if (operand == part)
                    link.positionOfPart = operandIndex;
            });
            return link;
        }
        if (!opcodesThatStateTypes.contains(opcode))
            return { };
        if (opcode == Opcode::Convert) {
            DeclaredType declared = operation.as<Opcode::Convert>().declared();
            if (declared.has(PositionFlags::IsClaim) || declared.has(PositionFlags::IsInt32Promise))
                return { };
        }
        part = user;
    }
}

static PartInSightInTextBuilderSpans classOfPartInSightInTextBuilderSpans(Walker& walker, const BodyIndex& index, BodyBlock block, ValueID value)
{
    using PartInSight = PartInSightInTextBuilderSpans;
    if (!block.holds(value)) {
        Reference part = walker.referenceOf(value);
        if (!part || !part.isRecord())
            return PartInSight::No;
        Type type = walker.types().at(walker.code().at(part.id()).type);
        if (!hasTextByItself(type))
            return PartInSight::No;
        return type.isAnyString() ? PartInSight::Text : PartInSight::Primitive;
    }
    if (index.isParameter(value))
        return PartInSight::No;

    static constexpr OpcodeSet constantsWithoutText { Opcode::Undefined, Opcode::Null, Opcode::Boolean, Opcode::Integer, Opcode::Double };
    BodyValue operation = index.value(value);
    Opcode opcode = operation.opcode();
    if (opcode == Opcode::String)
        return PartInSight::Text;
    if (constantsWithoutText.contains(opcode))
        return PartInSight::Primitive;
    if (!operationsThatJoinTexts.contains(opcode) && opcode != Opcode::ToString)
        return PartInSight::No;
    bool everyOperandIsInSight = true;
    bool someOperandIsText = false;
    operation.forEachOperand([&](ValueID operand, uint32_t) {
        PartInSight operandClass = everyOperandIsInSight ? classOfPartInSightInTextBuilderSpans(walker, index, block, operand) : PartInSight::No;
        everyOperandIsInSight &= operandClass != PartInSight::No;
        someOperandIsText |= operandClass == PartInSight::Text;
    });
    return everyOperandIsInSight && (someOperandIsText || opcode != Opcode::Add) ? PartInSight::Text : PartInSight::No;
}

static std::optional<PartAppendedOnEdgesInTextBuilderSpans> findLinksToGrowthInTextBuilderSpans(Walker& walker, const BodyIndex& index, BodyBlock block, uint32_t parameterIndex, Vector<ValueID, 8>& links)
{
    using PartInSight = PartInSightInTextBuilderSpans;
    PartAppendedOnEdgesInTextBuilderSpans found;
    found.parameterIndex = parameterIndex;
    bool partIsText = false;
    for (LinkInTextBuilderSpans link = linkThatAloneReadsPartInTextBuilderSpans(index, block, block.parameter(parameterIndex)); link.join; link = linkThatAloneReadsPartInTextBuilderSpans(index, block, link.join)) {
        BodyValue join = index.value(link.join);
        links.append(link.join);
        ValueID first = join.operand(0);
        bool isLastLink = link.positionOfPart && first && !block.holds(first) && isNameOfTextInStringBuilder(walker, first) && isGrowthOfTextInStringBuilder(walker, join);
        if (link.positionOfPart != (isLastLink ? 1u : 0u))
            return std::nullopt;

        bool restIsInSight = true;
        bool restHasText = false;
        join.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            if (operandIndex <= link.positionOfPart)
                return;
            PartInSight operandClass = classOfPartInSightInTextBuilderSpans(walker, index, block, operand);
            restIsInSight &= operandClass != PartInSight::No;
            restHasText |= operandClass == PartInSight::Text;
        });
        if (!restIsInSight)
            return std::nullopt;
        if (join.opcode() == Opcode::Add && !partIsText && !isLastLink)
            found.everyEdgeBringsString |= !restHasText;
        partIsText = true;

        if (isLastLink) {
            found.front = first;
            found.lastLink = link.join;
            return found;
        }
    }
    return std::nullopt;
}

void Walker::appendPartsOfTextsOnEdgesInto(BlockID inputBlock, BlockID entered, std::span<const uint32_t> edges)
{
    size_t frameIndex = m_frames.size() - 1;
    const BodyIndex& index = *m_frames[frameIndex].index;
    BodyBlock block = index.block(inputBlock);
    if (edges.size() < 2 || index.isLoopHead(inputBlock) || block.hasHandler() || block.isHandlerBlock() || !index.loopOf(inputBlock) || m_numberOfCopiesBuiltForSize)
        return;

    Vector<ValueID, 8> links;
    for (uint32_t parameterIndex = 0; parameterIndex < block.numberOfParameters(); ++parameterIndex) {
        links.shrink(0);
        std::optional<PartAppendedOnEdgesInTextBuilderSpans> found = findLinksToGrowthInTextBuilderSpans(*this, index, block, parameterIndex, links);
        TextInStringBuilder* text = found ? findTextNamedBy(*this, found->front) : nullptr;
        if (!text || text->web.standsUnderHandlers || !index.isInLoop(inputBlock, index.loopOf(text->web.loopHead)))
            continue;

        bool onlyJoinsInFrontOfLastLink = true;
        for (BodyValue operation : block.values()) {
            if (operation.id() == found->lastLink)
                break;
            Opcode opcode = operation.opcode();
            bool readsText = false;
            operation.forEachOperand([&](ValueID operand, uint32_t) {
                readsText |= namesTextInTextBuilderSpans(text->web, operand);
            });
            bool isSeenByNobody = links.contains(operation.id()) || opcodesThatStateTypes.contains(opcode) || declaredBoundInTable(opcode).isUnobservable()
                || classOfPartInSightInTextBuilderSpans(*this, index, block, operation.id()) != PartInSightInTextBuilderSpans::No;
            if (readsText || !isSeenByNobody) {
                onlyJoinsInFrontOfLastLink = false;
                break;
            }
        }
        if (!onlyJoinsInFrontOfLastLink)
            continue;

        bool everyArgumentFits = true;
        bool someArgumentHasNarrowerForm = false;
        for (uint32_t place : edges) {
            Reference argument = m_openEdges[place].arguments[parameterIndex];
            TypeID proven = m_openEdges[place].edge ? ownTypeOf(argument) : TypeID();
            Type type = proven ? m_types.at(proven) : Type();
            everyArgumentFits &= proven && hasTextByItself(type) && (!found->everyEdgeBringsString || type.isAnyString());
            someArgumentHasNarrowerForm |= argument.isConstant() || !type.isAnyString();
        }
        if (!everyArgumentFits || !someArgumentHasNarrowerForm)
            continue;

        Draft emptyTextDraft(Opcode::String, m_origin);
        emptyTextDraft.setString(program().strings.emptyString());
        Reference emptyText = constant(WTF::move(emptyTextDraft));
        Reference builder = text->builder;
        for (uint32_t place : edges) {
            Reference argument = m_openEdges[place].arguments[parameterIndex];
            BlockID onEdge = m_code.addBlock(BlockRole::Ordinary, m_code.at(entered).frequency);
            m_code.setLoopMarks(onEdge, m_code.at(entered).isInLoop, false);
            m_code.closeEdge(m_openEdges[place].edge, onEdge, { });
            m_block = onEdge;
            m_state = WTF::move(m_openEdges[place].state);
            appendPartsToStringBuilder(*this, *text, builder, singleElementSpan(argument), m_origin);
            if (m_hasFailed || isUnreachable()) {
                fail("an append on an edge ends the way to the block that the edge enters"_s);
                return;
            }
            BlockID behindAppend = m_block;
            m_code.end(behindAppend, TerminatorDraft(TerminatorKind::Jump, m_origin), 1);
            OpenEdge& open = m_openEdges[place];
            open.edge = m_code.at(behindAppend).terminator.next();
            open.state = WTF::move(m_state);
            open.arguments[parameterIndex] = emptyText;
            open.types[parameterIndex] = ownTypeOf(emptyText);
            m_block = { };
        }
        m_state.clear();
        return;
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
