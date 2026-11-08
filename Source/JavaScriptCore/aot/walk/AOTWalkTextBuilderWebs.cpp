/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkTextBuilders.h"

#if ENABLE(AOT)

#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfOperations.h"
#include "AOTIRProgram.h"
#include "AOTOpcodeSets.h"
#include "AOTThinSummary.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <optional>
#include <span>
#include <utility>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t frontOperandInTextBuilderWebs = 0;
static_assert(OperandIndex::Add::left == frontOperandInTextBuilderWebs);
static_assert(OperandIndex::Strcat::parts == frontOperandInTextBuilderWebs);

static constexpr OpcodeSet castsInTextBuilderWebs = opcodesThatStateTypes | OpcodeSet { Opcode::CheckInitialized, Opcode::RequireObjectCoercible };

static bool passesOperandThroughInTextBuilderWebs(const BodyValue& operation, uint32_t operandIndex)
{
    Opcode opcode = operation.opcode();
    if (!castsInTextBuilderWebs.contains(opcode) || !operandUsesOf(opcode, operandIndex).contains(OperandUse::Returns))
        return false;
    if (opcode != Opcode::Convert)
        return true;
    DeclaredType declared = operation.as<Opcode::Convert>().declared();
    return !declared.has(PositionFlags::IsClaim) && !declared.has(PositionFlags::IsInt32Promise);
}

namespace {

struct NameInTextBuilderWeb {
    ValueID value;
    uint32_t text { 0 };
    ValueID version;
};

struct CandidateInTextBuilderWeb {
    uint32_t parameterIndex { 0 };
    bool grows { false };
    bool isRefused { false };
    bool isOnlyConcatenated { true };
    bool beginsAnewOnSomeEdge { false };
    Vector<ValueID, 2> parametersThatReadText;
};

class TextBuilderWebScan {
    WTF_MAKE_NONCOPYABLE(TextBuilderWebScan);
public:
    TextBuilderWebScan(Walker&, BlockID inputHead, const LoopPreview&);

    void run(Vector<GrowingText, 2>&);

private:
    static constexpr uint32_t none = UINT32_MAX;

    bool isInLoop(BlockID block) const { return m_index.isInLoop(block, m_loop); }
    bool isOpen(uint32_t text) const { return !m_candidates[text].isRefused; }
    std::span<ValueID> versionsAtEntryOf(BlockID block) LIFETIME_BOUND { return m_versionsAtEntries.mutableSpan().subspan((block.index() - m_head.id().index()) * m_candidates.size(), m_candidates.size()); }

    void findCandidates();
    void scanBlock(BodyBlock);
    void scanOperation(const BodyValue&);
    TypeID typeInLooks(ValueID) const;
    bool isProvenToBeText(ValueID) const;
    bool isOneOfWrittenTexts(ValueID) const;
    std::optional<uint32_t> onlyTargetThatWalkLeadsTo(const BodyTerminator&) const;
    void passOverEdge(ValueID, uint32_t parameterIndex);
    bool someEdgeHandsOverWhatNamesNoText(BlockID, uint32_t parameterIndex) const;
    void followEdge(const BodyTerminator&, uint32_t targetIndex, BodyTarget);
    void holdBackEdgeToBringingTexts();
    void followHandlerEdge(BodyBlock);
    void arriveAt(BlockID, uint32_t text, uint32_t parameterIndex, bool beginsAnew = false);
    void findParametersThatComeRound();
    uint32_t textThatComesRoundIn(ValueID parameter) const;
    template<typename Functor> void forEachNameLiveAtHeadOf(BlockID, const Functor&);

    NameInTextBuilderWeb* findName(ValueID);
    NameInTextBuilderWeb* noteMention(ValueID);
    void giveName(ValueID, uint32_t text, ValueID version);
    void refuse(uint32_t text);
    void refuseTextNamedBy(ValueID);
    bool makesTextThatIsOnlyRead(const BodyValue& join);
    void noteThatStateIsSeen(const NameInTextBuilderWeb& name) { m_candidates[name.text].isOnlyConcatenated &= !m_index.isParameter(name.version); }

    Walker& m_walker;
    const BodyIndex& m_index;
    const LoopPreview& m_preview;
    LoopID m_loop;
    BodyBlock m_head;
    BlockID m_last;
    uint32_t m_firstValue;
    std::span<const BlockID> m_blocksNeverEntered;
    BodyLiveness m_liveness;
    bool m_standsUnderHandlers { false };

    Vector<CandidateInTextBuilderWeb, 2> m_candidates;
    Vector<uint32_t, 128> m_textsThatComeRoundInValues;
    uint32_t m_numberOfOpenCandidates { 0 };
    Vector<NameInTextBuilderWeb, 16> m_names;
    Vector<uint32_t, 128> m_namesOfValues;
    Vector<bool, 128> m_isInRowThatWasFollowed;
    Vector<ValueID, 32> m_versionsAtEntries;
    Vector<ValueID, 2> m_versions;
    Vector<uint32_t, 2> m_parametersThatTakeTexts;
    Vector<bool, 2> m_mayThrowBehindGrowth;
    Vector<uint32_t, 2> m_textsGrownSinceSomethingMayHaveThrown;
    Vector<bool, 2> m_isReadByHandler;
};

TextBuilderWebScan::TextBuilderWebScan(Walker& walker, BlockID inputHead, const LoopPreview& preview)
    : m_walker(walker)
    , m_index(walker.currentBodyIndex())
    , m_preview(preview)
    , m_loop(m_index.loopOf(inputHead))
    , m_head(m_index.block(inputHead))
    , m_last(m_index.loop(m_loop).last)
    , m_firstValue(m_head.firstParameter().index())
    , m_blocksNeverEntered(walker.facts().summaries().at(walker.currentFunction()).blocksNeverEntered.span())
{
}

NameInTextBuilderWeb* TextBuilderWebScan::findName(ValueID value)
{
    if (value.index() < m_firstValue || value.index() - m_firstValue >= m_namesOfValues.size())
        return nullptr;
    uint32_t place = m_namesOfValues[value.index() - m_firstValue];
    return place == none ? nullptr : &m_names[place];
}

NameInTextBuilderWeb* TextBuilderWebScan::noteMention(ValueID value)
{
    NameInTextBuilderWeb* name = findName(value);
    if (!name || !isOpen(name->text))
        return nullptr;
    if (name->version != m_versions[name->text]) {
        refuse(name->text);
        return nullptr;
    }
    return name;
}

void TextBuilderWebScan::giveName(ValueID value, uint32_t text, ValueID version)
{
    uint32_t& place = m_namesOfValues[value.index() - m_firstValue];
    if (place == none) {
        place = static_cast<uint32_t>(m_names.size());
        m_names.append(NameInTextBuilderWeb { value, text, version });
        return;
    }

    const NameInTextBuilderWeb& name = m_names[place];
    if (name.text == text && name.version == version)
        return;
    refuse(name.text);
    refuse(text);
}

void TextBuilderWebScan::refuse(uint32_t text)
{
    if (!isOpen(text))
        return;
    m_candidates[text].isRefused = true;
    --m_numberOfOpenCandidates;
}

void TextBuilderWebScan::refuseTextNamedBy(ValueID value)
{
    if (NameInTextBuilderWeb* name = findName(value))
        refuse(name->text);
}

template<typename Functor>
void TextBuilderWebScan::forEachNameLiveAtHeadOf(BlockID block, const Functor& functor)
{
    if (!m_liveness.isComputedFor(m_index))
        m_liveness.compute(m_index);
    m_liveness.forEachLiveAtHead(block, ValueID(m_firstValue), ValueID(static_cast<uint32_t>(m_firstValue + m_namesOfValues.size())), [&](ValueID value) {
        if (NameInTextBuilderWeb* name = findName(value))
            functor(*name);
    });
}

TypeID TextBuilderWebScan::typeInLooks(ValueID value) const
{
    if (value.index() < m_firstValue) {
        Reference built = m_walker.referenceOf(value);
        return built ? typeOfRecordForLooks(m_walker, m_walker.typeOf(built)) : TypeID();
    }
    size_t place = value.index() - m_preview.firstValue;
    return value.index() >= m_preview.firstValue && place < m_preview.typesOfValues.size() ? m_preview.typesOfValues[place] : TypeID();
}

bool TextBuilderWebScan::isProvenToBeText(ValueID value) const
{
    TypeID type = typeInLooks(value);
    return type && m_walker.types().at(type).isAnyString();
}

bool TextBuilderWebScan::isOneOfWrittenTexts(ValueID value) const
{
    TypeID type = typeInLooks(value);
    return type && m_walker.types().at(type).isAnyString() && longestTextOf(m_walker.types(), m_walker.types().at(type)) != JSString::MaxLength;
}

std::optional<uint32_t> TextBuilderWebScan::onlyTargetThatWalkLeadsTo(const BodyTerminator& terminator) const
{
    ValueID operand = terminator.numberOfTargets() > 1 ? terminator.operand(0) : ValueID();
    if (!operand || operand.index() >= m_firstValue)
        return std::nullopt;
    return onlyTargetKnownToFolding(m_walker, terminator);
}

void TextBuilderWebScan::findCandidates()
{
    uint32_t numberOfParameters = m_head.numberOfParameters();
    Vector<bool, 8> cannotGrow;
    Vector<bool, 8> changesOnSomeBackEdge;
    cannotGrow.fill(false, numberOfParameters);
    changesOnSomeBackEdge.fill(false, numberOfParameters);
    Vector<bool, 8> isWrittenWithoutText;
    isWrittenWithoutText.fill(false, numberOfParameters);
    for (uint32_t parameterIndex = 0; parameterIndex < numberOfParameters; ++parameterIndex) {
        TypeID written = m_head.parameterType(parameterIndex);
        isWrittenWithoutText[parameterIndex] = written && holdsOnlyPrimitives(written) && !(primitivesInID(written) & Primitive::String);
        TypeID inRow = typeInLooks(m_head.parameter(parameterIndex));
        cannotGrow[parameterIndex] = isNameOfTextInStringBuilder(m_walker, m_head.parameter(parameterIndex)) || (inRow && !m_walker.types().overlap(inRow, TypeTable::string));
    }
    if (!cannotGrow.contains(false))
        return;
    auto joinsText = [&](ValueID root) {
        return operationsThatJoinTexts.contains(m_index.opcodeOf(root)) && isProvenToBeText(root);
    };

    auto lookAtBackEdge = [&](std::span<const ValueID> arguments) {
        for (uint32_t parameterIndex = 0; parameterIndex < numberOfParameters; ++parameterIndex) {
            if (cannotGrow[parameterIndex])
                continue;
            if (parameterIndex >= arguments.size() || !arguments[parameterIndex]) {
                cannotGrow[parameterIndex] = true;
                continue;
            }
            ValueID root = m_index.valueBehindCasts(arguments[parameterIndex]);
            if (root == m_head.parameter(parameterIndex))
                continue;
            bool isMadeInLoop = root.index() >= m_head.firstValue().index();
            if (isMadeInLoop && isWrittenWithoutText[parameterIndex] && (m_index.isParameter(root) || !joinsText(root)))
                cannotGrow[parameterIndex] = true;
            else if (isMadeInLoop && (m_index.isParameter(root) || operationsThatJoinTexts.contains(m_index.opcodeOf(root))))
                changesOnSomeBackEdge[parameterIndex] = true;
            else
                cannotGrow[parameterIndex] = true;
        }
    };

    for (BlockID member : m_index.membersOf(m_loop)) {
        for (BodyTarget target : m_index.terminator(member).targets()) {
            if (target.block() == m_head.id())
                lookAtBackEdge(target.arguments());
        }
    }

    for (uint32_t parameterIndex = 0; parameterIndex < numberOfParameters; ++parameterIndex) {
        if (changesOnSomeBackEdge[parameterIndex] && !cannotGrow[parameterIndex])
            m_candidates.append(CandidateInTextBuilderWeb { parameterIndex });
    }
    m_numberOfOpenCandidates = static_cast<uint32_t>(m_candidates.size());
    findParametersThatComeRound();
}

uint32_t TextBuilderWebScan::textThatComesRoundIn(ValueID parameter) const
{
    size_t place = parameter.index() - m_firstValue;
    return parameter.index() >= m_firstValue && place < m_textsThatComeRoundInValues.size() ? m_textsThatComeRoundInValues[place] : none;
}

void TextBuilderWebScan::findParametersThatComeRound()
{
    if (m_candidates.isEmpty())
        return;
    m_textsThatComeRoundInValues.fill(none, m_index.block(m_last).endOfValues().index() - m_firstValue);
    for (uint32_t text = 0; text < m_candidates.size(); ++text)
        m_textsThatComeRoundInValues[m_head.parameter(m_candidates[text].parameterIndex).index() - m_firstValue] = text;
    std::span<const BlockID> members = m_index.membersOf(m_loop);
    for (size_t place = members.size(); place--;) {
        for (BodyTarget target : m_index.terminator(members[place]).targets()) {
            if (!isInLoop(target.block()))
                continue;
            BodyBlock entered = m_index.block(target.block());
            std::span<const ValueID> arguments = target.arguments();
            for (uint32_t argumentIndex = 0; argumentIndex < arguments.size() && argumentIndex < entered.numberOfParameters(); ++argumentIndex) {
                uint32_t text = arguments[argumentIndex] ? textThatComesRoundIn(entered.parameter(argumentIndex)) : none;
                if (text == none)
                    continue;
                ValueID root = m_index.valueBehindCasts(arguments[argumentIndex]);
                if (m_index.isParameter(root) && root.index() >= m_head.firstValue().index() && textThatComesRoundIn(root) == none)
                    m_textsThatComeRoundInValues[root.index() - m_firstValue] = text;
            }
        }
    }
}

bool TextBuilderWebScan::makesTextThatIsOnlyRead(const BodyValue& join)
{
    ValueID made = join.id();
    for (;;) {
        uint32_t place = made.index() - m_firstValue;
        if (place < m_isInRowThatWasFollowed.size() && std::exchange(m_isInRowThatWasFollowed[place], true))
            return false;
        std::optional<BodyUser> user = m_index.onlyUser(made);
        if (!user || user->kind != BodyUseKind::Operand)
            return false;

        BodyValue reader = m_index.value(user->value);
        Opcode opcode = reader.opcode();
        bool handsTextOn = opcode == Opcode::ToString;
        reader.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            if (operand == made)
                handsTextOn |= passesOperandThroughInTextBuilderWebs(reader, operandIndex) || (operandIndex == frontOperandInTextBuilderWebs && operationsThatJoinTexts.contains(opcode));
        });
        if (handsTextOn) {
            made = reader.id();
            continue;
        }
        return true;
    }
}

void TextBuilderWebScan::scanOperation(const BodyValue& operation)
{
    Opcode opcode = operation.opcode();
    if (opcode == Opcode::ReleaseHint)
        return;

    bool joinsTexts = operationsThatJoinTexts.contains(opcode);
    uint32_t textInFront = none;
    uint32_t textPassedThrough = none;
    operation.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        NameInTextBuilderWeb* name = noteMention(operand);
        if (!name)
            return;
        if (operandIndex == frontOperandInTextBuilderWebs && joinsTexts)
            textInFront = name->text;
        else if (passesOperandThroughInTextBuilderWebs(operation, operandIndex) || opcode == Opcode::ToString) {
            textPassedThrough = name->text;
            if (opcode == Opcode::Convert || opcode == Opcode::ToString || checksThatHandOnOrThrow.contains(opcode))
                noteThatStateIsSeen(*name);
        }
        else if (textInFront == name->text)
            refuse(name->text);
        else
            noteThatStateIsSeen(*name);
    });

    if (textPassedThrough != none && isOpen(textPassedThrough))
        giveName(operation.id(), textPassedThrough, m_versions[textPassedThrough]);
    if (textInFront == none || !isOpen(textInFront))
        return;
    if (makesTextThatIsOnlyRead(operation)) {
        noteThatStateIsSeen(*findName(m_versions[textInFront]));
        return;
    }

    bool givesText = opcode == Opcode::Add ? isProvenToBeText(operation.id()) : operation.variadicOperands().size() > 1;
    if (!givesText) {
        refuse(textInFront);
        return;
    }
    CandidateInTextBuilderWeb& candidate = m_candidates[textInFront];
    giveName(operation.id(), textInFront, operation.id());
    m_versions[textInFront] = operation.id();
    m_textsGrownSinceSomethingMayHaveThrown.append(textInFront);
    candidate.grows = true;
}

void TextBuilderWebScan::arriveAt(BlockID block, uint32_t text, uint32_t parameterIndex, bool beginsAnew)
{
    ValueID arriving = m_versions[text];
    if (parameterIndex != none) {
        BodyBlock target = m_index.block(block);
        if (parameterIndex >= target.numberOfParameters()) {
            refuse(text);
            return;
        }
        ValueID parameter = target.parameter(parameterIndex);
        if (m_index.numberOfIncomingEdges(block) > 1 || beginsAnew)
            arriving = parameter;
        giveName(parameter, text, arriving);
    }

    ValueID& atEntry = versionsAtEntryOf(block)[text];
    if (!atEntry)
        atEntry = arriving;
    else if (atEntry != arriving || (parameterIndex == none && m_index.block(block).indexOfParameter(atEntry)))
        refuse(text);
}

void TextBuilderWebScan::passOverEdge(ValueID value, uint32_t parameterIndex)
{
    NameInTextBuilderWeb* name = noteMention(value);
    if (!name)
        return;
    if (m_parametersThatTakeTexts[name->text] != none)
        refuse(name->text);
    m_parametersThatTakeTexts[name->text] = parameterIndex;
}

bool TextBuilderWebScan::someEdgeHandsOverWhatNamesNoText(BlockID block, uint32_t parameterIndex) const
{
    for (const BodyIncomingEdge& edge : m_walker.incomingEdgesOf(m_index, block)) {
        std::span<const ValueID> arguments = edge.target.arguments();
        ValueID handed = parameterIndex < arguments.size() ? arguments[parameterIndex] : ValueID();
        if (!handed || !isInLoop(edge.from))
            continue;
        ValueID root = m_index.valueBehindCasts(handed);
        if (m_index.isParameter(root) || isOneOfWrittenTexts(root))
            continue;
        Opcode opcode = m_index.opcodeOf(root);
        if (!operationsThatJoinTexts.contains(opcode) && !castsInTextBuilderWebs.contains(opcode) && !checksThatHandOnOrThrow.contains(opcode) && opcode != Opcode::ToString)
            return true;
    }
    return false;
}

void TextBuilderWebScan::holdBackEdgeToBringingTexts()
{
    for (uint32_t text = 0; text < m_candidates.size(); ++text) {
        if (isOpen(text) && m_parametersThatTakeTexts[text] != m_candidates[text].parameterIndex)
            refuse(text);
    }
}

void TextBuilderWebScan::followEdge(const BodyTerminator& terminator, uint32_t targetIndex, BodyTarget target)
{
    m_parametersThatTakeTexts.fill(none, m_candidates.size());
    std::span<const ValueID> arguments = target.arguments();
    for (uint32_t argumentIndex = 0; argumentIndex < arguments.size(); ++argumentIndex)
        passOverEdge(arguments[argumentIndex], argumentIndex);
    for (uint32_t proofIndex = 0; proofIndex < target.proofs().size(); ++proofIndex) {
        std::optional<BodyEdgeResult> result = m_index.edgeResult(terminator, targetIndex, target, proofIndex);
        if (!result)
            continue;
        if (m_index.isUnused(result->parameter))
            noteMention(result->subject);
        else
            passOverEdge(result->subject, static_cast<uint32_t>(arguments.size()) + proofIndex);
    }

    BlockID to = target.block();
    if (to == m_head.id()) {
        holdBackEdgeToBringingTexts();
        return;
    }
    if (isInLoop(to)) {
        BodyBlock entered = m_index.block(to);
        bool comesRoundToHeadInside = m_index.isLoopHead(to) && m_index.isInLoop(terminator.block(), m_index.loopOf(to));
        for (uint32_t text = 0; text < m_candidates.size(); ++text) {
            if (!isOpen(text))
                continue;
            bool beginsAnew = false;
            for (uint32_t argumentIndex = 0; !comesRoundToHeadInside && m_parametersThatTakeTexts[text] == none && argumentIndex < arguments.size() && argumentIndex < entered.numberOfParameters(); ++argumentIndex) {
                ValueID begins = arguments[argumentIndex];
                if (!begins || textThatComesRoundIn(entered.parameter(argumentIndex)) != text || findName(begins) || !isOneOfWrittenTexts(begins))
                    continue;
                m_parametersThatTakeTexts[text] = argumentIndex;
                m_candidates[text].beginsAnewOnSomeEdge = true;
                beginsAnew = true;
            }
            uint32_t taking = m_parametersThatTakeTexts[text];
            if (taking != none && !beginsAnew && !m_index.isLoopHead(to) && taking < entered.numberOfParameters() && someEdgeHandsOverWhatNamesNoText(to, taking)) {
                noteThatStateIsSeen(*findName(m_versions[text]));
                if (!m_candidates[text].parametersThatReadText.contains(entered.parameter(taking)))
                    m_candidates[text].parametersThatReadText.append(entered.parameter(taking));
                taking = none;
            }
            arriveAt(to, text, taking, beginsAnew);
        }
        return;
    }

    for (uint32_t text = 0; text < m_candidates.size(); ++text) {
        if (isOpen(text) && m_parametersThatTakeTexts[text] != none)
            noteThatStateIsSeen(*findName(m_versions[text]));
    }
    forEachNameLiveAtHeadOf(to, [&](const NameInTextBuilderWeb& name) {
        if (!isOpen(name.text))
            return;
        if (name.version != m_versions[name.text])
            refuse(name.text);
        else
            noteThatStateIsSeen(name);
    });
}

void TextBuilderWebScan::followHandlerEdge(BodyBlock block)
{
    BodyTarget handler = block.handler();
    std::span<const ValueID> atEntry = versionsAtEntryOf(block.id());
    m_parametersThatTakeTexts.fill(none, m_candidates.size());
    std::span<const ValueID> arguments = handler.arguments();
    for (uint32_t argumentIndex = 0; argumentIndex < arguments.size(); ++argumentIndex) {
        NameInTextBuilderWeb* name = findName(arguments[argumentIndex]);
        if (!name || !isOpen(name->text))
            continue;
        if (name->version != atEntry[name->text] || m_parametersThatTakeTexts[name->text] != none) {
            refuse(name->text);
            continue;
        }
        m_parametersThatTakeTexts[name->text] = argumentIndex;
    }

    bool leavesLoop = !isInLoop(handler.block());
    m_isReadByHandler.fill(false, m_candidates.size());
    if (leavesLoop) {
        forEachNameLiveAtHeadOf(handler.block(), [&](const NameInTextBuilderWeb& name) {
            if (!isOpen(name.text))
                return;
            if (name.version != atEntry[name.text])
                refuse(name.text);
            else
                m_isReadByHandler[name.text] = true;
        });
    }

    for (uint32_t text = 0; text < m_candidates.size(); ++text) {
        if (!isOpen(text))
            continue;
        bool isHanded = m_parametersThatTakeTexts[text] != none || m_isReadByHandler[text];
        bool hasGrown = m_versions[text] != atEntry[text];
        if (hasGrown && (isHanded ? m_mayThrowBehindGrowth[text] : !leavesLoop)) {
            refuse(text);
            continue;
        }
        if (leavesLoop) {
            if (isHanded)
                noteThatStateIsSeen(*findName(atEntry[text]));
            continue;
        }
        ValueID atEnd = std::exchange(m_versions[text], atEntry[text]);
        arriveAt(handler.block(), text, m_parametersThatTakeTexts[text]);
        m_versions[text] = atEnd;
    }
}

void TextBuilderWebScan::scanBlock(BodyBlock block)
{
    std::span<const ValueID> atEntry = versionsAtEntryOf(block.id());
    if (std::ranges::none_of(atEntry, [](ValueID version) { return !!version; }))
        return;
    for (uint32_t text = 0; text < m_candidates.size(); ++text) {
        if (!atEntry[text])
            refuse(text);
        m_versions[text] = atEntry[text];
    }
    m_mayThrowBehindGrowth.fill(false, m_candidates.size());
    m_textsGrownSinceSomethingMayHaveThrown.shrink(0);
    auto noteWhatMayThrow = [&](bool mayThrowHere) {
        if (!mayThrowHere)
            return;
        for (uint32_t text : m_textsGrownSinceSomethingMayHaveThrown)
            m_mayThrowBehindGrowth[text] = true;
        m_textsGrownSinceSomethingMayHaveThrown.shrink(0);
    };

    refuseTextNamedBy(block.subject());
    for (BodyValue operation : block.values()) {
        noteWhatMayThrow(!!declaredBoundInTable(operation.opcode()).mayThrow);
        scanOperation(operation);
    }

    BodyTerminator terminator = block.terminator();
    noteWhatMayThrow(!!declaredBoundOf(terminator.kind()).mayThrow);
    terminator.forEachOperand([&](ValueID operand, uint32_t) {
        if (NameInTextBuilderWeb* name = noteMention(operand))
            noteThatStateIsSeen(*name);
    });
    std::optional<uint32_t> onlyTarget = onlyTargetThatWalkLeadsTo(terminator);
    uint32_t targetIndex = 0;
    for (BodyTarget target : terminator.targets()) {
        bool isLedByWalk = (!onlyTarget || *onlyTarget == targetIndex) && !std::ranges::binary_search(m_blocksNeverEntered, target.block());
        if (isLedByWalk)
            followEdge(terminator, targetIndex, target);
        ++targetIndex;
    }
    if (block.hasHandler()) {
        m_standsUnderHandlers = true;
        followHandlerEdge(block);
    }
}

void TextBuilderWebScan::run(Vector<GrowingText, 2>& texts)
{
    findCandidates();
    if (m_candidates.isEmpty())
        return;

    m_namesOfValues.fill(none, m_index.block(m_last).endOfValues().index() - m_firstValue);
    m_isInRowThatWasFollowed.fill(false, m_namesOfValues.size());
    m_versionsAtEntries.fill(ValueID(), (m_last.index() - m_head.id().index() + 1) * m_candidates.size());
    m_versions.fill(ValueID(), m_candidates.size());
    for (uint32_t text = 0; text < m_candidates.size(); ++text) {
        ValueID parameter = m_head.parameter(m_candidates[text].parameterIndex);
        giveName(parameter, text, parameter);
        versionsAtEntryOf(m_head.id())[text] = parameter;
    }

    for (BlockID member : m_index.membersOf(m_loop)) {
        if (!m_numberOfOpenCandidates)
            return;
        scanBlock(m_index.block(member));
    }

    for (uint32_t text = 0; text < m_candidates.size(); ++text) {
        const CandidateInTextBuilderWeb& candidate = m_candidates[text];
        if (candidate.isRefused || !candidate.grows || (candidate.beginsAnewOnSomeEdge && m_standsUnderHandlers))
            continue;
        GrowingText grown;
        grown.beginsAnewOnSomeEdge = candidate.beginsAnewOnSomeEdge;
        grown.loopHead = m_head.id();
        grown.parameterIndex = candidate.parameterIndex;
        grown.isOnlyConcatenated = candidate.isOnlyConcatenated;
        grown.parametersThatReadText = candidate.parametersThatReadText;
        grown.standsUnderHandlers = m_standsUnderHandlers;
        for (const NameInTextBuilderWeb& name : m_names) {
            if (name.text != text)
                continue;
            grown.names.append(NameOfGrowingText { name.value, name.version });
            if (name.value != name.version || !m_index.isParameter(name.value))
                continue;
            BlockID block = m_index.blockOf(name.value);
            if (block != m_head.id() && m_index.isLoopHead(block))
                grown.parametersOfInnerHeads.append(name.value);
        }
        std::ranges::sort(grown.names, { }, &NameOfGrowingText::value);
        texts.append(WTF::move(grown));
    }
}

} // anonymous namespace

void findGrowingTexts(Walker& walker, BlockID inputHead, const LoopPreview& preview, Vector<GrowingText, 2>& texts)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (preview.typesOfValues.isEmpty() || !index.holdsAnyOf(operationsThatJoinTexts) || index.hasIrreducibleFlow() || walker.textsOfLoopStayOutOfBuilders(inputHead))
        return;
    if (!index.isLoopHead(inputHead) || index.isEnteredByHandlerEdge(inputHead))
        return;
    TextBuilderWebScan(walker, inputHead, preview).run(texts);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
