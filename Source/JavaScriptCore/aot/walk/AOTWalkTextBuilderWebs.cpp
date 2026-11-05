/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// The scan in front of AOTWalkTextBuilders.cpp: which parameters of the head of a loop hold a text that only grows, and which values of the loop name it. It reads the blocks of the loop once and builds nothing.
// At a suspension: nobody asks it about a loop that may suspend. At a throw: an edge to a handler brings the text as it stood at the head of the block that throws.
// Withheld, TypedOperations or LoopInvariants: nobody asks it.

#include "config.h"
#include "AOTWalkTextBuilders.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
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

// A text is initialized, and is neither undefined nor null: each of them hands it on and does nothing else. RequireObject throws for a text, so it reads.
static constexpr OpcodeSet castsInTextBuilderWebs = opcodesThatStateTypes | OpcodeSet { Opcode::CheckInitialized, Opcode::RequireObjectCoercible };

// For a claim the walk builds a test, and a promise of an int32 is held against what enters the position: both look at a string.
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

static ValueID operandPassedThroughInTextBuilderWebs(const BodyValue& cast)
{
    Opcode opcode = cast.opcode();
    for (uint32_t operandIndex = 0; operandIndex < numberOfNamedOperandsOf(opcode); ++operandIndex) {
        if (passesOperandThroughInTextBuilderWebs(cast, operandIndex))
            return cast.operand(operandIndex);
    }
    return { };
}

namespace {

// A value of the loop that holds a text. Names of one version hold the same characters.
struct NameInTextBuilderWeb {
    ValueID value;
    uint32_t text { 0 };
    // The head parameter, a growth, or a parameter where several edges meet.
    ValueID version;
};

struct CandidateInTextBuilderWeb {
    uint32_t parameterIndex { 0 };
    bool grows { false };
    bool isRefused { false };
    bool isOnlyConcatenated { true };
    bool takesTextOfJSONStringify { false };
    bool hasPartsThatMayRunCode { false };
};

// One forward scan of the blocks of a loop of the input, for all the parameters of its head at once.
// A builder holds one text. So at each point in the loop a text has one current version, and only names of that version may be mentioned there: a mention of an older name means that two states are live at once.
// Blocks come in reverse postorder and the flow is reducible, so some edge has brought each text to a block by the time the block is scanned.
// An edge that arrives later is a back edge of a loop inside, and has to bring what the first edge brought.
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
    // What holds in every turn. Of a value from in front of the loop: the type of what the walk has built for it.
    TypeID typeInLooks(ValueID) const;
    bool isProvenToBeText(ValueID) const;
    bool hasTextByItselfInLooks(ValueID) const;
    // The walk makes a jump of a terminator whose operand is a constant by then. Of a value from in front of the loop that is known now, and the walk asks the same function. Nothing: it may lead every edge.
    std::optional<uint32_t> onlyTargetThatWalkLeadsTo(const BodyTerminator&) const;
    void passOverEdge(ValueID, uint32_t parameterIndex);
    void followEdge(const BodyTerminator&, uint32_t targetIndex, BodyTarget);
    void holdBackEdgeToBringingTexts();
    void followHandlerEdge(BodyBlock);
    void arriveAt(BlockID, uint32_t text, uint32_t parameterIndex);
    // functor(const NameInTextBuilderWeb&). Whatever is live there dominates the block, so the scan has come by it.
    template<typename Functor> void forEachNameLiveAtHeadOf(BlockID, const Functor&);

    NameInTextBuilderWeb* findName(ValueID);
    // Null: the value names no text that is still a candidate.
    NameInTextBuilderWeb* noteMention(ValueID);
    void giveName(ValueID, uint32_t text, ValueID version);
    void refuse(uint32_t text);
    void refuseTextNamedBy(ValueID);
    // `log(s + ": " + x)`: what the join makes goes, through casts, conversions to a text and joins that have it in front, to one operation that hands it on to nobody.
    // So it is never the variable again, and the join reads the text.
    bool makesTextThatIsOnlyRead(const BodyValue& join);
    // Among the parts behind the first, through casts, conversions to a text and joins. Each value on the way has one mention, so no value is looked at twice.
    bool takesTextOfJSONStringifyBehindFront(const BodyValue& join);
    // The name is looked at as it stands, or goes where the scan does not follow it. Behind a growth it is a string whatever has entered the loop.
    void noteThatStateIsSeen(const NameInTextBuilderWeb& name) { m_candidates[name.text].isOnlyConcatenated &= !m_index.isParameter(name.version); }

    Walker& m_walker;
    const BodyIndex& m_index;
    const LoopPreview& m_preview;
    LoopID m_loop;
    BodyBlock m_head;
    BlockID m_last;
    // The loop's values are numbered from here on.
    uint32_t m_firstValue;
    // ThinSummary::blocksNeverEntered: the walk leads no edge there.
    std::span<const BlockID> m_blocksNeverEntered;
    BodyLiveness m_liveness;
    bool m_standsUnderHandlers { false };

    Vector<CandidateInTextBuilderWeb, 2> m_candidates;
    uint32_t m_numberOfOpenCandidates { 0 };
    Vector<NameInTextBuilderWeb, 16> m_names;
    // Indexed by value minus m_firstValue: the index in m_names.
    Vector<uint32_t, 128> m_namesOfValues;
    // Indexed by value minus m_firstValue: makesTextThatIsOnlyRead() has come by the value. So a row of n joins is followed once, not n times.
    Vector<bool, 128> m_isInRowThatWasFollowed;
    // Indexed by block minus the head, then by text. Null: no edge has arrived yet.
    Vector<ValueID, 32> m_versionsAtEntries;
    // Indexed by text: the current version where the scan stands.
    Vector<ValueID, 2> m_versions;
    // Indexed by text: the parameter of the target that takes the text on the edge being followed.
    Vector<uint32_t, 2> m_parametersThatTakeTexts;
    // Indexed by text: in the block being scanned, something that may throw stands behind a growth.
    Vector<bool, 2> m_mayThrowBehindGrowth;
    // The texts that have grown in the block being scanned since the last operation that may throw.
    Vector<uint32_t, 2> m_textsGrownSinceSomethingMayHaveThrown;
    // Indexed by text: the handler of the block being scanned reads a name that it is not handed.
    Vector<bool, 2> m_isReadByHandler;
    // While takesTextOfJSONStringifyBehindFront() runs: what is still to be looked at.
    Vector<ValueID, 8> m_partsToLookAt;
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

bool TextBuilderWebScan::hasTextByItselfInLooks(ValueID value) const
{
    TypeID type = typeInLooks(value);
    return type && hasTextByItself(m_walker.types().at(type));
}

std::optional<uint32_t> TextBuilderWebScan::onlyTargetThatWalkLeadsTo(const BodyTerminator& terminator) const
{
    ValueID operand = terminator.numberOfTargets() > 1 ? terminator.operand(0) : ValueID();
    if (!operand || operand.index() >= m_firstValue)
        return std::nullopt;
    return onlyTargetKnownToFolding(m_walker, terminator);
}

// Decided by the back edges alone, so a loop in which no text grows costs one look at its terminators.
void TextBuilderWebScan::findCandidates()
{
    uint32_t numberOfParameters = m_head.numberOfParameters();
    Vector<bool, 8> cannotGrow;
    Vector<bool, 8> changesOnSomeBackEdge;
    cannotGrow.fill(false, numberOfParameters);
    changesOnSomeBackEdge.fill(false, numberOfParameters);
    // `let n = 0; for (..) n += "x"`. A counter is written so too, and a loop that only counts is not scanned: what comes round has to be joined to a text where it is made.
    Vector<bool, 8> isWrittenWithoutText;
    isWrittenWithoutText.fill(false, numberOfParameters);
    for (uint32_t parameterIndex = 0; parameterIndex < numberOfParameters; ++parameterIndex) {
        TypeID written = m_head.parameterType(parameterIndex);
        isWrittenWithoutText[parameterIndex] = written && holdsOnlyPrimitives(written) && !(primitivesInID(written) & Primitive::String);
        // The scan of a loop around has given the names of this loop too.
        cannotGrow[parameterIndex] = isNameOfTextInStringBuilder(m_walker, m_head.parameter(parameterIndex));
    }
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
}

bool TextBuilderWebScan::makesTextThatIsOnlyRead(const BodyValue& join)
{
    ValueID made = join.id();
    for (;;) {
        // Only a row that goes on to be the variable has the text in front a second time.
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

bool TextBuilderWebScan::takesTextOfJSONStringifyBehindFront(const BodyValue& join)
{
    m_partsToLookAt.shrink(0);
    join.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (operandIndex != frontOperandInTextBuilderWebs)
            m_partsToLookAt.append(operand);
    });
    TypeArena& types = m_walker.types();
    const BuiltinTable& builtins = m_walker.program().builtins();
    while (!m_partsToLookAt.isEmpty()) {
        ValueID part = m_partsToLookAt.takeLast();
        if (!part || m_index.isParameter(part) || m_index.useCount(part) != 1)
            continue;
        BodyValue made = m_index.value(part);
        Opcode opcode = made.opcode();
        if (opcode == Opcode::Call) {
            TypeID callee = typeInLooks(made.as<Opcode::Call>().calleeOperand());
            BuiltinID called = callee ? singleBuiltinOf(types, types.at(callee)) : BuiltinID();
            if (called && builtins.canonicalID(called) == Builtins::JSONStringify)
                return true;
            continue;
        }
        if (ValueID passedThrough = operandPassedThroughInTextBuilderWebs(made)) {
            m_partsToLookAt.append(passedThrough);
            continue;
        }
        if (opcode != Opcode::ToString && !operationsThatJoinTexts.contains(opcode))
            continue;
        made.forEachOperand([&](ValueID operand, uint32_t) {
            m_partsToLookAt.append(operand);
        });
    }
    return false;
}

// Any other use of the current version than a cast, a conversion to a text and a growth is a read, and refuses nothing: `if (x.flush) out.push(s)`. The walk has a string made of what the builder holds there, and the builder goes on.
void TextBuilderWebScan::scanOperation(const BodyValue& operation)
{
    Opcode opcode = operation.opcode();
    // It reads nothing: no value is live for the sake of a hint.
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
        // `s += s`: no text is appended to itself.
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

    // An Add joins only where one of its operands is a string. Nobody looks again: a sum that is not proven to give a string in every turn is no growth.
    bool givesText = opcode == Opcode::Add ? isProvenToBeText(operation.id()) : operation.variadicOperands().size() > 1;
    if (!givesText) {
        refuse(textInFront);
        return;
    }
    CandidateInTextBuilderWeb& candidate = m_candidates[textInFront];
    operation.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (operandIndex != frontOperandInTextBuilderWebs)
            candidate.hasPartsThatMayRunCode |= !hasTextByItselfInLooks(operand);
    });
    giveName(operation.id(), textInFront, operation.id());
    m_versions[textInFront] = operation.id();
    m_textsGrownSinceSomethingMayHaveThrown.append(textInFront);
    candidate.grows = true;
    if (!candidate.takesTextOfJSONStringify)
        candidate.takesTextOfJSONStringify = takesTextOfJSONStringifyBehindFront(operation);
}

void TextBuilderWebScan::arriveAt(BlockID block, uint32_t text, uint32_t parameterIndex)
{
    ValueID arriving = m_versions[text];
    if (parameterIndex != none) {
        BodyBlock target = m_index.block(block);
        if (parameterIndex >= target.numberOfParameters()) {
            refuse(text);
            return;
        }
        ValueID parameter = target.parameter(parameterIndex);
        if (m_index.numberOfIncomingEdges(block) > 1)
            arriving = parameter;
        giveName(parameter, text, arriving);
    }

    ValueID& atEntry = versionsAtEntryOf(block)[text];
    if (!atEntry)
        atEntry = arriving;
    // An edge that comes round a loop inside and hands the parameter that took the text something else brings the text to nobody.
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

void TextBuilderWebScan::holdBackEdgeToBringingTexts()
{
    for (uint32_t text = 0; text < m_candidates.size(); ++text) {
        if (isOpen(text) && m_parametersThatTakeTexts[text] != m_candidates[text].parameterIndex)
            refuse(text);
    }
}

// `if (s)` hands the text on to the target under another name: the parameter that takes the proof.
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
        // Both edges of a branch on a test bear a proof, whether or not anybody reads the parameter that takes it.
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
        for (uint32_t text = 0; text < m_candidates.size(); ++text) {
            if (isOpen(text))
                arriveAt(to, text, m_parametersThatTakeTexts[text]);
        }
        return;
    }

    // The string is made on the edge, of what the builder holds there. Whoever reads an older name behind the edge would read that.
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

// A throw can leave the block at any of its operations, and the handler can name only what is available at the head of the block: it is handed the version that enters the block.
// The builder holds that version wherever the block can throw, unless something that may throw stands behind a growth of the block. The append itself completes or leaves the builder as it was.
// A handler block in the loop goes on with the builder. One behind the loop reads the string, which is made behind the throw.
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
    // An edge that is followed brings every text that is still a candidate. So only edges that the walk does not lead come here.
    if (std::ranges::none_of(atEntry, [](ValueID version) { return !!version; }))
        return;
    for (uint32_t text = 0; text < m_candidates.size(); ++text) {
        if (!atEntry[text])
            refuse(text);
        m_versions[text] = atEntry[text];
    }
    m_mayThrowBehindGrowth.fill(false, m_candidates.size());
    m_textsGrownSinceSomethingMayHaveThrown.shrink(0);
    // A version that has left the one at the entry does not come back to it.
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
        if (candidate.isRefused || !candidate.grows)
            continue;
        GrowingText grown;
        grown.loopHead = m_head.id();
        grown.parameterIndex = candidate.parameterIndex;
        grown.isOnlyConcatenated = candidate.isOnlyConcatenated;
        grown.takesTextOfJSONStringify = candidate.takesTextOfJSONStringify;
        grown.standsUnderHandlers = m_standsUnderHandlers;
        grown.hasPartsThatMayRunCode = candidate.hasPartsThatMayRunCode;
        for (const NameInTextBuilderWeb& name : m_names) {
            if (name.text != text)
                continue;
            grown.names.append(NameOfGrowingText { name.value, name.version });
            // Several edges meet at the head of a loop, so the parameter that takes the text there is a version of its own: arriveAt().
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
    // The looks found no invariant: nothing is known of what a sum gives.
    if (preview.typesOfValues.isEmpty() || !index.holdsAnyOf(operationsThatJoinTexts) || index.hasIrreducibleFlow())
        return;
    if (!index.isLoopHead(inputHead) || index.isEnteredByHandlerEdge(inputHead))
        return;
    TextBuilderWebScan(walker, inputHead, preview).run(texts);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
