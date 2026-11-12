/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCode.h"

#if ENABLE(AOT)

#include "AOTCompileOptions.h"
#include "AOTDiagnostics.h"
#include "AOTIRFunction.h"
#include "AOTIRProgram.h"
#include "AOTInliningAcrossFiles.h"
#include "AOTLimits.h"
#include "AOTOpcodeSets.h"
#include "AOTTypedArrayKinds.h"
#include <algorithm>
#include <limits>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet opcodesThatTheWalkLowers {
    Opcode::AppendSpread, Opcode::IteratorOpen, Opcode::IteratorResume, Opcode::IteratorClose,
    Opcode::StepArrayIterator, Opcode::StepMapIterator, Opcode::StepSetIterator, Opcode::CreateIteratorResult,
    Opcode::Await, Opcode::Yield, Opcode::InitialYield, Opcode::ReadScope,
    Opcode::EvaluateModule
};

static constexpr OpcodeSet opcodesWithoutEmitterByList = opcodesThatStateTypes | opcodesThatTheWalkLowers | OpcodeSet {
    Opcode::StringBuilderAppend,
};

static_assert([]() consteval {
    for (Opcode opcode : allMembersOf<Opcode>) {
        if (!mayStandInCode(opcode) && !opcodesWithoutEmitterByList.contains(opcode))
            return false;
    }
    return true;
}(), "The dispatch has no case for an operation that the schema keeps out of Code.");

static constexpr OpcodeSet opcodesThatCodeNeverAppends = opcodesThatStateTypes | opcodesWithoutEmitterByList | OpcodeSet { Opcode::Parameter };

static constexpr uint64_t mostInlineFramesInCode = defaultLimitValue(LimitID::InlineFramesPerFunction, hostArchitecture());
static_assert(mostInlineFramesInCode <= (1u << 24) - 1, "Instruction::inlineFrame has 24 bits.");
static_assert(sizeof(uint32_t*) == 2 * sizeof(uint32_t), "Instruction keeps the address of its side words in two words.");

constinit const OpcodeSet opcodesWithoutEmitterInCode = opcodesWithoutEmitterByList;

bool readsNumberOutOfBytes(const Operation& operation)
{
    if (operation.opcode != Opcode::GetTypedArrayElement || !operation.isInsideView() || operation.representation != Representation::Double)
        return false;
    return operation.typedArrayType() == NotTypedArray || (isFloat(operation.typedArrayType()) && hasInlineElementAccess(operation.typedArrayType()));
}

bool readsNumberOrNothingOutOfBytes(const Operation& operation)
{
    if (operation.opcode != Opcode::GetTypedArrayElement || operation.isInsideView() || operation.representation != Representation::OptionalDouble)
        return false;
    return !isBigInt(operation.typedArrayType()) && hasInlineElementAccess(operation.typedArrayType());
}

bool handsOnBitsOfNaN(const Operation& operation)
{
    constexpr OpcodeSet arithmetic { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Div };
    bool isArithmetic = arithmetic.contains(operation.opcode) || operation.opcode == Opcode::ToNumber;
    return isArithmetic && operation.form() == OperatorForm::Numbers && operation.representation == Representation::Double;
}

bool takesAnyNaN(const Operation& user, uint32_t operandIndex)
{
    constexpr OpcodeSet relationsAndOperatorsOnBits { Opcode::Less, Opcode::LessEq, Opcode::Greater, Opcode::GreaterEq, Opcode::BitAnd, Opcode::BitOr, Opcode::BitXor, Opcode::LShift, Opcode::RShift, Opcode::URShift };
    if (relationsAndOperatorsOnBits.contains(user.opcode))
        return user.form() == OperatorForm::Numbers;
    if (user.opcode == Opcode::ToInt32 || user.opcode == Opcode::ToUint32)
        return true;
    if (user.opcode == Opcode::PutTypedArrayElement)
        return operandIndex == OperandIndex::PutTypedArrayElement::value && hasInlineElementAccess(user.typedArrayType()) && !isClamped(user.typedArrayType());
    return handsOnBitsOfNaN(user);
}

template<typename Tag>
static ALWAYS_INLINE uint32_t checkedIndexInCode(ID<Tag> id, uint32_t size)
{
    RELEASE_ASSERT(id.index() < size);
    return id.index();
}

static ALWAYS_INLINE void copyOriginToRecordOfCode(Instruction& instruction, Origin origin, size_t numberOfInlineFrames)
{
    RELEASE_ASSERT(origin.inlineFrame < numberOfInlineFrames);
    instruction.position = origin.position;
    instruction.inlineFrame = origin.inlineFrame;
    instruction.isTailCall = origin.isTailCall;
    instruction.isPartOfConstruction = origin.isPartOfConstruction;
}

namespace {

struct ShapeOfEntryInCode {
    uint16_t declaredFlags { 0 };
    uint8_t numberOfNamedOperands { 0 };
    bool hasVariadicOperand { false };
    bool isNeverAppended { false };
    bool hasWordOfStorage { false };
};

} // anonymous namespace

static constexpr std::array<ShapeOfEntryInCode, numberOfOpcodes> shapesOfOperationsInCode = []() consteval {
    std::array<ShapeOfEntryInCode, numberOfOpcodes> shapes { };
    for (Opcode opcode : allMembersOf<Opcode>)
        shapes[static_cast<unsigned>(opcode)] = { declaredFlagsOf(opcode), static_cast<uint8_t>(numberOfNamedOperandsOf(opcode)), hasVariadicOperand(opcode), opcodesThatCodeNeverAppends.contains(opcode), !!PayloadWord::storage(opcode) };
    return shapes;
}();

template<typename Entry>
static ALWAYS_INLINE void checkShapeOfDraftForCode(Entry entry, const DraftWords& draft)
{
    if constexpr (std::is_same_v<Entry, Opcode>) {
        const ShapeOfEntryInCode& shape = shapesOfOperationsInCode[static_cast<unsigned>(entry)];
        RELEASE_ASSERT(draft.numberOfNamedOperands() == shape.numberOfNamedOperands);
        RELEASE_ASSERT(shape.hasVariadicOperand || !draft.numberOfVariadicOperands());
        RELEASE_ASSERT(!(draft.flags & ~shape.declaredFlags));
    } else {
        RELEASE_ASSERT(draft.numberOfNamedOperands() == numberOfNamedOperandsOf(entry));
        RELEASE_ASSERT(hasVariadicOperand(entry) || !draft.numberOfVariadicOperands());
        RELEASE_ASSERT(!(draft.flags & ~declaredFlagsOf(entry)));
    }
}

static ALWAYS_INLINE bool allocatesInFrameOfUnit(const Draft& draft)
{
    return shapesOfOperationsInCode[static_cast<unsigned>(draft.opcode)].hasWordOfStorage && draft.storage() != AllocationStorage::Heap;
}

static ALWAYS_INLINE ValueID appendRecordToCode(auto& operations, const Operation& operation)
{
    RELEASE_ASSERT(operations.size() < Reference::indexMask);
    return ValueID(operations.append(operation));
}

ALWAYS_INLINE_INTO_OTHER_FILES void Code::fill(Instruction& instruction, const DraftWords& draft)
{
    std::span<const Reference> operands = draft.operands();
    std::span<const uint32_t> payload = draft.payload();
    RELEASE_ASSERT(operands.size() <= Reference::indexMask && payload.size() < (1u << 28));
    uint32_t numberOfOperands = static_cast<uint32_t>(operands.size());
    uint32_t numberOfNamedOperands = draft.numberOfNamedOperands();
    uint32_t numberOfPayloadWords = static_cast<uint32_t>(payload.size());
    RELEASE_ASSERT(numberOfNamedOperands <= numberOfOperands);

    copyOriginToRecordOfCode(instruction, draft.origin(), m_inlineFrames.size());
    instruction.flags = draft.flags;

    uint32_t* operandWords;
    uint32_t* payloadWords;
    if (numberOfOperands == numberOfNamedOperands && numberOfPayloadWords <= numberOfInlinePayloadWords && numberOfOperands + numberOfPayloadWords <= Instruction::numberOfInlineWords) {
        operandWords = instruction.m_words.data();
        payloadWords = operandWords + numberOfOperands;
        instruction.m_numberOfInlineOperands = numberOfOperands;
        instruction.m_numberOfInlinePayloadWords = numberOfPayloadWords;
    } else {
        static_assert(Instruction::maxNumberOfNamedOperands == (1u << 4) - 1);
        RELEASE_ASSERT(numberOfNamedOperands <= Instruction::maxNumberOfNamedOperands && numberOfPayloadWords <= UINT32_MAX >> 4);
        payloadWords = m_sideWords.allocate(numberOfPayloadWords + numberOfOperands);
        operandWords = payloadWords + numberOfPayloadWords;
        auto address = std::bit_cast<std::array<uint32_t, 2>>(payloadWords);
        instruction.m_words = { address[0], address[1], numberOfPayloadWords << 4 | numberOfNamedOperands, numberOfOperands };
        instruction.m_hasSideWords = true;
    }

    const uint32_t* wordsOfDraft = payload.data();
    uint32_t wordsLeft = numberOfPayloadWords;
    for (; wordsLeft >= 4; wordsLeft -= 4, payloadWords += 4, wordsOfDraft += 4)
        memcpy(payloadWords, wordsOfDraft, 4 * sizeof(uint32_t));
    if (wordsLeft & 2) {
        memcpy(payloadWords, wordsOfDraft, 2 * sizeof(uint32_t));
        payloadWords += 2;
        wordsOfDraft += 2;
    }
    if (wordsLeft & 1)
        *payloadWords = *wordsOfDraft;
    uint32_t numberOfRecords = m_operations.size();
    for (uint32_t index = 0; index < numberOfOperands; ++index) {
        Reference reference = operands[index];
        if (!reference) {
            operandWords[index] = ValueID().bits();
            continue;
        }
        RELEASE_ASSERT(reference.isRecord());
        operandWords[index] = checkedIndexInCode(reference.id(), numberOfRecords);
    }
}

SideWords::~SideWords()
{
    for (const Segment& segment : m_segments)
        fastFree(segment.words);
}

uint32_t* SideWords::allocateInAnotherSegment(uint32_t numberOfWords)
{
    if (m_current < m_segments.size() && numberOfWords > m_segments[m_current].capacity - m_used) {
        ++m_current;
        m_used = 0;
    }
    if (m_current == m_segments.size() || numberOfWords > m_segments[m_current].capacity) {
        uint32_t capacity = std::max(numberOfWords, wordsPerSegment);
        m_segments.insert(m_current, Segment { static_cast<uint32_t*>(fastMalloc(static_cast<size_t>(capacity) * sizeof(uint32_t))), capacity });
    }
    m_wordsOfCurrent = m_segments[m_current].words;
    m_capacityOfCurrent = m_segments[m_current].capacity;
    uint32_t* words = m_wordsOfCurrent + m_used;
    m_used += numberOfWords;
    return words;
}

void SideWords::rewindTo(Mark mark)
{
    RELEASE_ASSERT(mark.segment < m_current || (mark.segment == m_current && mark.used <= m_used));
    for (uint32_t index = m_current; index > mark.segment; --index) {
        if (m_segments[index].capacity > wordsPerSegment) {
            fastFree(m_segments[index].words);
            m_segments.removeAt(index);
        }
    }
    m_current = mark.segment;
    m_used = mark.used;
    bool hasCurrent = m_current < m_segments.size();
    m_wordsOfCurrent = hasCurrent ? m_segments[m_current].words : nullptr;
    m_capacityOfCurrent = hasCurrent ? m_segments[m_current].capacity : 0;
}

void Code::noteEffectsBelieved(ValueID id, const Effects& effects)
{
    while (!m_effectsBelieved.isEmpty() && m_effectsBelieved.last().first.index() >= id.index())
        m_effectsBelieved.removeLast();
    m_effectsBelieved.append({ id, effects });
}

void Code::noteSteppedWithCounter(const SteppedWithCounter& beside)
{
    if (!m_steppedWithCounters.contains(beside))
        m_steppedWithCounters.append(beside);
}

void Code::noteTestsPassedFor(ValueID id, const PassedTest& passed)
{
    while (!m_testsPassed.isEmpty() && m_testsPassed.last().first.index() >= id.index())
        m_testsPassed.removeLast();
    m_testsPassed.append({ id, passed });
}

const PassedTest* Code::testsPassedFor(ValueID id) const
{
    auto found = std::ranges::lower_bound(m_testsPassed, id.index(), { }, [](const auto& noted) { return noted.first.index(); });
    return found != m_testsPassed.end() && found->first == id ? &found->second : nullptr;
}

void Code::noteThatOperationWritesWhatTurnHasMade(ValueID id)
{
    while (!m_operationsThatWriteWhatTurnHasMade.isEmpty() && m_operationsThatWriteWhatTurnHasMade.last().index() >= id.index())
        m_operationsThatWriteWhatTurnHasMade.removeLast();
    m_operationsThatWriteWhatTurnHasMade.append(id);
}

bool Code::writesWhatTurnHasMade(ValueID id) const
{
    return std::ranges::binary_search(m_operationsThatWriteWhatTurnHasMade, id.index(), { }, &ValueID::index);
}

const Effects* Code::effectsBelievedOf(ValueID id) const
{
    auto found = std::ranges::lower_bound(m_effectsBelieved, id.index(), { }, [](const auto& noted) { return noted.first.index(); });
    return found != m_effectsBelieved.end() && found->first == id ? &found->second : nullptr;
}

void Code::reset(CodeUnit unit)
{
    m_unit = unit;
    m_graveyard = { };
    m_changesToOlderParts.shrink(0);
    m_olderBlocksEnded.shrink(0);
    m_numberOfBlocksAtNewestMark = 0;
    m_recordsAbandoned.shrink(0);
    m_partsAbandoned.shrink(0);
    m_keepsChangesToOlderParts = false;
    m_operations.forgetUnit();
    m_blocks.forgetUnit();
    m_edges.forgetUnit();
    m_sideWords.forgetUnit();
    m_inlineFrames.shrink(0);
    m_entries.shrink(0);
    m_factUses.shrink(0);
    m_effectsBelieved.shrink(0);
    m_operationsThatWriteWhatTurnHasMade.shrink(0);
    m_testsPassed.shrink(0);
    m_steppedWithCounters.shrink(0);
    m_keepsTestsPassed = false;
    m_marksLoops = m_unit.role != CodeRole::SlowBody;
    m_testsOfGuardsInFrontOfCopies.shrink(0);
    m_inputsOfHolesReadAsNaN.shrink(0);
    m_readsAnsweredByHolesReadAsNaN.shrink(0);
    m_cellsMadeForOneOperation.shrink(0);
    m_syntheticVariableReads.shrink(0);
    m_valuesThatFoldedArithmeticGives.shrink(0);
    m_pollsWithFates.shrink(0);
    m_opcodesOfValues = { };
    m_numberOfFrameWords = 0;
    m_hasHandlers = false;
    m_hasBoxesForHandlers = false;
    m_hasSuspensions = false;
    m_hasLoops = false;
    m_holdsFrameCells = false;
    m_holdsCallThatPlanHasNotCounted = false;
    m_callsOfFunctionNeverCollect = false;
    m_readsNumbersOutOfBytes = false;
    m_callsItself = false;
    m_isEntryThatWalksBody = false;
    m_mayHoldReturnedCalls = false;
    m_mechanismsWithheld = { };
    m_frequenciesOfBlocks = FrequenciesOfBlocks::AsGiven;
    m_seedOfFrequencies = 0;
    m_numberOfBlocksPlacedOtherwise = 0;

    m_isSwept = false;
    m_marks.shrink(0);
    m_storesWhoseBarriersAreCountedOn = BitVector();
    m_useCounts.shrink(0);
    m_onlyUsers.shrink(0);
    m_valuesBehindRowsOfNarrowed.clear();
    m_parametersFed.shrink(0);
    m_order.shrink(0);
    m_numberOfOperationsInOrder = 0;
    m_reachedBlocks = BitVector();
    m_blocksWithoutThrows = BitVector();
    m_suspensions.shrink(0);
    m_liveValues.shrink(0);

    InlineFrame functionItself { };
    functionItself.callee = unit.function;
    m_inlineFrames.append(functionItself);
}

Wire::SourceLocation Code::locationOf(const Program& program, Origin origin) const
{
    if (origin.position == Instruction::noPosition)
        return noSourceLocation;

    uint32_t position = origin.position;
    uint32_t frameIndex = origin.inlineFrame;
    while (frameIndex) {
        if (frameIndex >= m_inlineFrames.size())
            return noSourceLocation;
        const InlineFrame& frame = m_inlineFrames[frameIndex];
        if (frame.callee) {
            Wire::SourceLocation location = program.at(frame.callee).locationAt(position);
            if (location.module != noSourceLocation.module)
                return location;
        }
        if (frame.parent >= frameIndex)
            return noSourceLocation;
        position = frame.callPosition;
        frameIndex = frame.parent;
    }
    return program.at(m_unit.function).locationAt(position);
}

std::span<const ValueID> Code::liveAt(ValueID suspension) const
{
    RELEASE_ASSERT(m_isSwept);
    std::span<const LiveAtSuspension> records = m_suspensions.span();
    auto found = std::ranges::lower_bound(records, suspension, { }, &LiveAtSuspension::suspension);
    if (found == records.end() || found->suspension != suspension)
        return { };
    return m_liveValues.span().subspan(found->first, found->count);
}

BlockFrequency Code::frequencyTakenFor(uint32_t block, BlockFrequency given)
{
    if (m_frequenciesOfBlocks == FrequenciesOfBlocks::AsGiven) [[likely]]
        return given;
    BlockFrequency taken = BlockFrequency::Normal;
    if (m_frequenciesOfBlocks == FrequenciesOfBlocks::DrawnBySeed) {
        if (!block)
            return given;
        static constexpr std::array<BlockFrequency, 4> drawn { BlockFrequency::Normal, BlockFrequency::Normal, BlockFrequency::Unlikely, BlockFrequency::Rare };
        uint32_t wordOfUnit = m_unit.function.index() << 4 | static_cast<uint32_t>(m_unit.role);
        taken = drawn[drawBySeed(m_seedOfFrequencies, DrawnFor::BlockFrequency, wordOfUnit, block) % drawn.size()];
    }
    m_numberOfBlocksPlacedOtherwise += taken != given;
    return taken;
}

bool Code::loopMarkIsDrawnAway(uint32_t block) const
{
    if (m_frequenciesOfBlocks != FrequenciesOfBlocks::DrawnBySeed) [[likely]]
        return false;
    uint32_t wordOfUnit = m_unit.function.index() << 4 | static_cast<uint32_t>(m_unit.role);
    return !(drawBySeed(m_seedOfFrequencies, DrawnFor::LoopMark, wordOfUnit, block) % 4);
}

BlockID Code::addBlock(BlockRole role, BlockFrequency frequency)
{
    RELEASE_ASSERT(!m_isSwept);
    RELEASE_ASSERT(m_blocks.size() < Operation::noBlock);
    BlockID id(m_blocks.size());
    Block& block = m_blocks.appendNew();
    block.role = role;
    block.frequency = frequencyTakenFor(id.index(), frequency);
    if (block.isHandlerBlock())
        m_hasHandlers = true;
    return id;
}

ALWAYS_INLINE_INTO_OTHER_FILES BlockID Code::addBlockBehindEdge(EdgeID edgeID, BlockFrequency frequency, bool isInLoop)
{
    RELEASE_ASSERT(!m_isSwept);
    RELEASE_ASSERT(m_blocks.size() < Operation::noBlock);
    Edge& edge = m_edges[checkedIndexInCode(edgeID, m_edges.size())];
    RELEASE_ASSERT(edge.isOpen());
    Block block;
    block.frequency = frequencyTakenFor(m_blocks.size(), frequency);
    if (isInLoop) {
        block.isInLoop = m_marksLoops && !loopMarkIsDrawnAway(m_blocks.size());
        m_hasLoops = true;
    }
    block.m_firstEdgeIn = edgeID;
    block.m_lastEdgeIn = edgeID;
    edge.to = BlockID(m_blocks.append(block));
    edge.m_arguments = nullptr;
    edge.m_numberOfArguments = 0;
    if (m_keepsChangesToOlderParts)
        m_changesToOlderParts.append(ChangeToOlderPart { edgeID, edge.to, { }, { }, { } });
    return edge.to;
}

void Code::makeBlocksRare(uint32_t first, uint32_t end)
{
    RELEASE_ASSERT(!m_isSwept && first <= end && end <= m_blocks.size());
    for (uint32_t index = first; index < end; ++index) {
        keepMarksOfOlderBlock(index);
        m_blocks[index].frequency = frequencyTakenFor(index, BlockFrequency::Rare);
        m_blocks[index].isInLoop = false;
    }
}

void Code::makeBlockUnlikely(BlockID id, bool thoughRare)
{
    RELEASE_ASSERT(!m_isSwept);
    Block& block = m_blocks[checkedIndexInCode(id, m_blocks.size())];
    if ((block.frequency == BlockFrequency::Normal || (thoughRare && block.frequency == BlockFrequency::Rare)) && !block.isEntry) {
        keepMarksOfOlderBlock(id.index());
        block.frequency = frequencyTakenFor(id.index(), BlockFrequency::Unlikely);
    }
}

void Code::makeBlockRare(BlockID block)
{
    RELEASE_ASSERT(!m_isSwept);
    keepMarksOfOlderBlock(checkedIndexInCode(block, m_blocks.size()));
    m_blocks[checkedIndexInCode(block, m_blocks.size())].frequency = frequencyTakenFor(block.index(), BlockFrequency::Rare);
}

void Code::setLoopMarks(BlockID blockID, bool isInLoop, bool isLoopHead)
{
    RELEASE_ASSERT(!m_isSwept);
    Block& block = m_blocks[checkedIndexInCode(blockID, m_blocks.size())];
    block.isInLoop = isInLoop && m_marksLoops && !loopMarkIsDrawnAway(blockID.index());
    block.isLoopHead = isLoopHead;
    if (isInLoop || isLoopHead)
        m_hasLoops = true;
}

void Code::noteBeliefAheadOfEdges(BlockID blockID, BeliefAheadOfEdges belief)
{
    RELEASE_ASSERT(!m_isSwept);
    m_blocks[checkedIndexInCode(blockID, m_blocks.size())].beliefAheadOfEdges = belief;
}

void Code::markRareBlockThatKeepsSites(BlockID blockID)
{
    RELEASE_ASSERT(!m_isSwept);
    m_blocks[checkedIndexInCode(blockID, m_blocks.size())].keepsSitesThoughRare = true;
}

void Code::markMeetingOfStepWrittenByHand(BlockID blockID)
{
    RELEASE_ASSERT(!m_isSwept);
    m_blocks[checkedIndexInCode(blockID, m_blocks.size())].meetsStepWrittenByHand = true;
}

void Code::setSubject(BlockID blockID, ValueID subject)
{
    RELEASE_ASSERT(!m_isSwept);
    RELEASE_ASSERT(!subject || subject.index() < m_operations.size());
    m_blocks[checkedIndexInCode(blockID, m_blocks.size())].subject = subject;
}

NEVER_INLINE size_t Block::placeAmongManyParameters(ValueID parameter) const
{
    std::span<const ValueID> all = parameters();
    auto found = std::ranges::lower_bound(all, parameter);
    if (found != all.end() && *found == parameter)
        return found - all.begin();
    return std::ranges::find(all, parameter) - all.begin();
}

void Code::setParameters(BlockID blockID, std::span<const ValueID> parameters)
{
    RELEASE_ASSERT(!m_isSwept);
    Block& block = m_blocks[checkedIndexInCode(blockID, m_blocks.size())];
    RELEASE_ASSERT(!block.m_numberOfParameters && !block.m_last && !block.hasEnded());
    for (ValueID parameter : parameters) {
        const Operation& record = m_operations[checkedIndexInCode(parameter, m_operations.size())];
        RELEASE_ASSERT(record.opcode == Opcode::Parameter && record.blockIndex == blockID.index());
    }
    std::span<const ValueID> kept = m_sideWords.copy(parameters);
    block.m_parameters = kept.data();
    block.m_numberOfParameters = static_cast<uint32_t>(kept.size());
}

void Code::setTypeOfJoinParameter(ValueID parameter, TypeID type)
{
    RELEASE_ASSERT(!m_isSwept && type);
    Operation& record = m_operations[checkedIndexInCode(parameter, m_operations.size())];
    const Block& block = m_blocks[record.blockIndex];
    RELEASE_ASSERT(record.opcode == Opcode::Parameter && !block.isEntry && !block.isLoopHead && !block.m_last && !block.hasEnded());
    if (m_keepsChangesToOlderParts)
        m_changesToOlderParts.append(ChangeToOlderPart { { }, { }, { }, parameter, record });
    record.type = type;
}

Code::AllIncomingEdges Code::seal(BlockID blockID)
{
    RELEASE_ASSERT(!m_isSwept);
    Block& block = m_blocks[checkedIndexInCode(blockID, m_blocks.size())];
    if (!block.isSealed) {
        if (m_keepsChangesToOlderParts)
            m_changesToOlderParts.append(ChangeToOlderPart { { }, { }, { }, { }, { }, blockID, block.frequency, block.isInLoop, false });
        block.isSealed = true;
    }
    return { *this, blockID };
}

void Code::setCheckedTypeOfProof(ValueID check, TypeID type)
{
    RELEASE_ASSERT(!m_isSwept && type);
    Operation& record = m_operations[checkedIndexInCode(check, m_operations.size())];
    RELEASE_ASSERT(record.opcode == Opcode::CheckProof && !record.m_hasSideWords);
    if (m_keepsChangesToOlderParts)
        m_changesToOlderParts.append(ChangeToOlderPart { { }, { }, { }, check, record });
    const_cast<uint32_t&>(record.payload()[*PayloadWord::checkedType(Opcode::CheckProof)]) = encodePayloadWord(type);
    record.flags &= ~*FlagMask::isLeftToTestBehind(Opcode::CheckProof);
}

void Code::makeTestAskInFull(ValueID test)
{
    RELEASE_ASSERT(!m_isSwept);
    Operation& record = m_operations[checkedIndexInCode(test, m_operations.size())];
    RELEASE_ASSERT(FlagMask::asksInFull(record.opcode));
    if (m_keepsChangesToOlderParts)
        m_changesToOlderParts.append(ChangeToOlderPart { { }, { }, { }, test, record });
    record.flags |= *FlagMask::asksInFull(record.opcode);
}

void Code::makeTypeTestAskInFull(ValueID test)
{
    RELEASE_ASSERT(!m_isSwept);
    Operation& record = m_operations[checkedIndexInCode(test, m_operations.size())];
    RELEASE_ASSERT(record.opcode == Opcode::TypeTest);
    if (m_keepsChangesToOlderParts)
        m_changesToOlderParts.append(ChangeToOlderPart { { }, { }, { }, test, record });
    record.flags |= *FlagMask::asksInFull(Opcode::TypeTest);
}

void Code::letJoinFailWhereItsReaderFails(ValueID join)
{
    RELEASE_ASSERT(!m_isSwept);
    Operation& record = m_operations[checkedIndexInCode(join, m_operations.size())];
    RELEASE_ASSERT(operationsThatJoinTexts.contains(record.opcode));
    if (m_keepsChangesToOlderParts)
        m_changesToOlderParts.append(ChangeToOlderPart { { }, { }, { }, join, record });
    record.traitBits = record.traits().withoutThrowByRule().bits();
}

void Code::turnFinishIntoUnusedString(ValueID finish)
{
    RELEASE_ASSERT(!m_isSwept);
    Operation& record = m_operations[checkedIndexInCode(finish, m_operations.size())];
    RELEASE_ASSERT(record.opcode == Opcode::StackStringBuilderFinish || record.opcode == Opcode::StringBuilderFinish);
    if (m_keepsChangesToOlderParts)
        m_changesToOlderParts.append(ChangeToOlderPart { { }, { }, { }, finish, record });
    record.opcode = record.opcode == Opcode::StackStringBuilderFinish ? Opcode::StackStringBuilderToString : Opcode::StringBuilderToString;
    Effects ofContents = Effects::none();
    ofContents.writes = LocationMasks(AbstractHeapSet { AbstractHeap::StringBuilderContents });
    record.traitBits = record.traits().withoutThrowByRule().bits() & ~EffectTraits(ofContents).bits();
    m_opcodesOfValues.add(record.opcode);
}

void Code::turnBranchIntoJump(BlockID block, EdgeID lastOnWay)
{
    Terminator& terminator = m_blocks[checkedIndexInCode(block, m_blocks.size())].terminator;
    RELEASE_ASSERT(m_isSwept && terminator.kind == TerminatorKind::Branch);
    m_edges[checkedIndexInCode(lastOnWay, m_edges.size())].from = block;
    for (EdgeID written : { terminator.taken(), terminator.notTaken() }) {
        if (written != lastOnWay)
            m_edges[written.index()].to = BlockID();
    }
    terminator.m_numberOfInlineOperands = 0;
    terminator.flags = 0;
    terminator.kind = TerminatorKind::Jump;
    terminator.firstEdge = lastOnWay;
    terminator.numberOfEdges = 1;
}

void Code::leadJumpPastBlockThatJumps(BlockID block)
{
    Terminator& terminator = m_blocks[checkedIndexInCode(block, m_blocks.size())].terminator;
    RELEASE_ASSERT(m_isSwept && terminator.kind == TerminatorKind::Jump);
    Edge& left = m_edges[terminator.firstEdge.index()];
    const Terminator& passed = m_blocks[left.to.index()].terminator;
    RELEASE_ASSERT(passed.kind == TerminatorKind::Jump);
    left.to = BlockID();
    m_edges[passed.firstEdge.index()].from = block;
    terminator.firstEdge = passed.firstEdge;
}

void Code::layClosureInFrame(ValueID closure, LoanOfClosure loan)
{
    RELEASE_ASSERT(m_isSwept && loan != LoanOfClosure::None);
    Operation& record = m_operations[checkedIndexInCode(closure, m_operations.size())];
    RELEASE_ASSERT(record.opcode == Opcode::NewClosure && record.storage() == AllocationStorage::Heap);
    std::span<const uint32_t> payload = record.payload();
    const_cast<uint32_t&>(payload[*PayloadWord::storage(Opcode::NewClosure)]) = encodePayloadWord(AllocationStorage::Frame);
    const_cast<uint32_t&>(payload[*PayloadWord::loan(Opcode::NewClosure)]) = encodePayloadWord(loan);
    m_holdsFrameCells = true;
}

void Code::layObjectInFrame(ValueID object)
{
    RELEASE_ASSERT(m_isSwept);
    Operation& record = m_operations[checkedIndexInCode(object, m_operations.size())];
    RELEASE_ASSERT(record.opcode == Opcode::NewObject && record.storage() == AllocationStorage::Heap);
    const_cast<uint32_t&>(record.payload()[*PayloadWord::storage(Opcode::NewObject)]) = encodePayloadWord(AllocationStorage::Frame);
    m_holdsFrameCells = true;
}

void Code::leaveCheckOfProofToTestBehind(ValueID check)
{
    RELEASE_ASSERT(!m_isSwept);
    Operation& record = m_operations[checkedIndexInCode(check, m_operations.size())];
    RELEASE_ASSERT(record.opcode == Opcode::CheckProof);
    if (m_keepsChangesToOlderParts)
        m_changesToOlderParts.append(ChangeToOlderPart { { }, { }, { }, check, record });
    record.flags |= *FlagMask::isLeftToTestBehind(Opcode::CheckProof);
}

void Code::bearListWithStorage(ValueID list, ArrayStorage storage)
{
    RELEASE_ASSERT(!m_isSwept);
    const Operation& record = m_operations[checkedIndexInCode(list, m_operations.size())];
    RELEASE_ASSERT(record.opcode == Opcode::NewArrayLike);
    const uint32_t& word = record.payload()[*PayloadWord::arrayStorage(Opcode::NewArrayLike)];
    RELEASE_ASSERT(std::bit_cast<uintptr_t>(&word) - std::bit_cast<uintptr_t>(&record) < sizeof(Operation));
    if (m_keepsChangesToOlderParts)
        m_changesToOlderParts.append(ChangeToOlderPart { { }, { }, { }, list, record });
    const_cast<uint32_t&>(word) = encodePayloadWord(storage);
}

void Code::letListRememberStorageAtSite(ValueID list, bool takesAnyValueAtFirst)
{
    RELEASE_ASSERT(!m_isSwept);
    Operation& record = m_operations[checkedIndexInCode(list, m_operations.size())];
    RELEASE_ASSERT(record.opcode == Opcode::NewArrayLike);
    RELEASE_ASSERT(!record.remembersStorageAtSite());
    if (m_keepsChangesToOlderParts)
        m_changesToOlderParts.append(ChangeToOlderPart { { }, { }, { }, list, record });
    record.flags |= *FlagMask::remembersStorageAtSite(Opcode::NewArrayLike);
    if (takesAnyValueAtFirst)
        record.flags |= *FlagMask::takesAnyValueAtFirst(Opcode::NewArrayLike);
}

void Code::letListForgetStorageAtSite(ValueID list)
{
    RELEASE_ASSERT(!m_isSwept);
    Operation& record = m_operations[checkedIndexInCode(list, m_operations.size())];
    RELEASE_ASSERT(record.opcode == Opcode::NewArrayLike);
    RELEASE_ASSERT(record.remembersStorageAtSite());
    if (m_keepsChangesToOlderParts)
        m_changesToOlderParts.append(ChangeToOlderPart { { }, { }, { }, list, record });
    record.flags &= ~(*FlagMask::remembersStorageAtSite(Opcode::NewArrayLike) | *FlagMask::takesAnyValueAtFirst(Opcode::NewArrayLike));
}

ValueID Code::appendParameter(BlockID blockID, TypeID type, Representation representation, Origin origin)
{
    RELEASE_ASSERT(!m_isSwept);
    RELEASE_ASSERT(type && representation != Representation::Unchosen);
    const Block& block = m_blocks[checkedIndexInCode(blockID, m_blocks.size())];
    RELEASE_ASSERT(!block.m_numberOfParameters && !block.m_last && !block.hasEnded());

    Operation parameter;
    copyOriginToRecordOfCode(parameter, origin, m_inlineFrames.size());
    parameter.representation = representation;
    parameter.opcode = Opcode::Parameter;
    parameter.type = type;
    parameter.blockIndex = blockID.index();
    m_opcodesOfValues.add(Opcode::Parameter);
    return appendRecordToCode(m_operations, parameter);
}

ValueID Code::append(BlockID blockID, const Draft& draft, TypeID proven, EffectTraits traits)
{
    RELEASE_ASSERT(!m_isSwept);
    RELEASE_ASSERT(!shapesOfOperationsInCode[static_cast<unsigned>(draft.opcode)].isNeverAppended);
    RELEASE_ASSERT(draft.representation != Representation::Unchosen);
    checkShapeOfDraftForCode(draft.opcode, draft);
    Block& block = m_blocks[checkedIndexInCode(blockID, m_blocks.size())];
    RELEASE_ASSERT(!block.hasEnded());

    Operation operation;
    fill(operation, draft);
    operation.representation = draft.representation;
    operation.opcode = draft.opcode;
    operation.type = proven;
    operation.previous = block.m_last;
    operation.blockIndex = blockID.index();
    operation.traitBits = traits.bits();

    ValueID id = appendRecordToCode(m_operations, operation);
    block.m_last = id;
    m_opcodesOfValues.add(draft.opcode);
    if (allocatesInFrameOfUnit(draft))
        m_holdsFrameCells = true;
    if (traits.maySuspend())
        m_hasSuspensions = true;
    if (draft.opcode == Opcode::GetTypedArrayElement && (readsNumberOutOfBytes(operation) || readsNumberOrNothingOutOfBytes(operation)))
        m_readsNumbersOutOfBytes = true;
    if (draft.opcode == Opcode::CallDirect && operation.function() == m_unit.function)
        m_callsItself = true;
    return id;
}

ValueID Code::valueBehindManyNames(ValueID name, const OpcodeSet& names, SourcesOfNames sources) const
{
    while (name && names.contains(at(name).opcode)) {
        ValueID source = valueBehindRowOfNarrowed(name);
        if (sources == SourcesOfNames::StandEarlier && source.index() >= name.index())
            return { };
        name = source;
    }
    return name;
}

ValueID Code::valueBehindRowOfNarrowed(ValueID name) const
{
    ValueID behind = at(name).operand(0);
    if (m_isSwept || at(name).opcode != Opcode::Narrowed || at(behind).opcode != Opcode::Narrowed)
        return behind;
    if (auto kept = m_valuesBehindRowsOfNarrowed.find(name.index() + 1); kept != m_valuesBehindRowsOfNarrowed.end())
        return ValueID(kept->value);
    while (at(behind).opcode == Opcode::Narrowed) {
        if (auto kept = m_valuesBehindRowsOfNarrowed.find(behind.index() + 1); kept != m_valuesBehindRowsOfNarrowed.end()) {
            behind = ValueID(kept->value);
            break;
        }
        behind = at(behind).operand(0);
    }
    m_valuesBehindRowsOfNarrowed.add(name.index() + 1, behind.index());
    return behind;
}

ValueID Code::appendConversion(BlockID blockID, Opcode opcode, ValueID operand, Representation representation, TypeID proven, Origin origin)
{
    static constexpr OpcodeSet conversionsOfOneWord { Opcode::Box, Opcode::Unbox, Opcode::Compress };
    static_assert((conversionsOfOneWord | conversionsOfForm) == conversionsOfForm && !conversionsOfOneWord.intersects(opcodesThatCodeNeverAppends));
    static_assert([]() consteval {
        bool haveOneOperandAlone = true;
        conversionsOfOneWord.forEach([&](Opcode conversion) {
            haveOneOperandAlone &= numberOfNamedOperandsOf(conversion) == 1 && !hasVariadicOperand(conversion) && !declaredFlagsOf(conversion) && !PayloadWord::storage(conversion);
        });
        return haveOneOperandAlone;
    }());
    RELEASE_ASSERT(!m_isSwept && conversionsOfOneWord.contains(opcode) && representation != Representation::Unchosen);
    ASSERT(defaultPayloadOf(opcode).empty());
    Block& block = m_blocks[checkedIndexInCode(blockID, m_blocks.size())];
    RELEASE_ASSERT(!block.hasEnded());

    Operation operation;
    copyOriginToRecordOfCode(operation, origin, m_inlineFrames.size());
    operation.m_words[0] = checkedIndexInCode(operand, m_operations.size());
    operation.m_numberOfInlineOperands = 1;
    operation.representation = representation;
    operation.opcode = opcode;
    operation.type = proven;
    operation.previous = block.m_last;
    operation.blockIndex = blockID.index();

    ValueID id = appendRecordToCode(m_operations, operation);
    block.m_last = id;
    m_opcodesOfValues.add(opcode);
    return id;
}

ValueID Code::addConstant(const Draft& draft, TypeID proven)
{
    RELEASE_ASSERT(!m_isSwept);
    RELEASE_ASSERT(!opcodesThatCodeNeverAppends.contains(draft.opcode));
    RELEASE_ASSERT(draft.representation != Representation::Unchosen);
    checkShapeOfDraftForCode(draft.opcode, draft);

    Operation constant;
    fill(constant, draft);
    constant.representation = draft.representation;
    constant.opcode = draft.opcode;
    constant.type = proven;
    for (ValueID operand : constant.operands())
        RELEASE_ASSERT(!operand || isConstant(operand));

    m_opcodesOfValues.add(draft.opcode);
    if (allocatesInFrameOfUnit(draft))
        m_holdsFrameCells = true;
    return appendRecordToCode(m_operations, constant);
}

ALWAYS_INLINE_INTO_OTHER_FILES ValueID Code::addCopyOfConstant(ValueID constant, Representation representation)
{
    RELEASE_ASSERT(!m_isSwept && representation != Representation::Unchosen);
    Operation copy = m_operations[checkedIndexInCode(constant, m_operations.size())];
    RELEASE_ASSERT(copy.blockIndex == Operation::noBlock && !copy.numberOfOperands() && !PayloadWord::storage(copy.opcode));
    copy.representation = representation;
    return appendRecordToCode(m_operations, copy);
}

void Code::end(BlockID blockID, const TerminatorDraft& draft, uint32_t numberOfEdges)
{
    RELEASE_ASSERT(!m_isSwept);
    RELEASE_ASSERT(mayStandInCode(draft.kind));
    checkShapeOfDraftForCode(draft.kind, draft);
    if (std::optional<uint32_t> numberOfTargets = numberOfFixedTargetsOf(draft.kind))
        RELEASE_ASSERT(numberOfEdges == *numberOfTargets);
    else
        RELEASE_ASSERT(numberOfEdges);
    Block& block = m_blocks[checkedIndexInCode(blockID, m_blocks.size())];
    RELEASE_ASSERT(!block.hasEnded());

    Terminator terminator;
    fill(terminator, draft);
    terminator.kind = draft.kind;
    m_mayHoldReturnedCalls |= draft.kind == TerminatorKind::ReturnCall;
    terminator.isSet = true;
    terminator.numberOfEdges = numberOfEdges;
    if (numberOfEdges) {
        terminator.firstEdge = EdgeID(m_edges.size());
        Edge edge;
        edge.from = blockID;
        for (uint32_t index = 0; index < numberOfEdges; ++index)
            m_edges.append(edge);
    }
    setTerminator(blockID, block, terminator);
}

EdgeID Code::endWithJump(BlockID blockID, Origin origin)
{
    static_assert(mayStandInCode(TerminatorKind::Jump) && !numberOfNamedOperandsOf(TerminatorKind::Jump) && !hasVariadicOperand(TerminatorKind::Jump) && numberOfFixedTargetsOf(TerminatorKind::Jump) == 1u);
    RELEASE_ASSERT(!m_isSwept);
    ASSERT(defaultPayloadOf(TerminatorKind::Jump).empty());
    Block& block = m_blocks[checkedIndexInCode(blockID, m_blocks.size())];
    RELEASE_ASSERT(!block.hasEnded());

    Terminator terminator;
    copyOriginToRecordOfCode(terminator, origin, m_inlineFrames.size());
    terminator.kind = TerminatorKind::Jump;
    terminator.isSet = true;
    terminator.numberOfEdges = 1;
    Edge edge;
    edge.from = blockID;
    terminator.firstEdge = EdgeID(m_edges.append(edge));
    setTerminator(blockID, block, terminator);
    return terminator.firstEdge;
}

void Code::endWithBranch(BlockID blockID, ValueID condition, Origin origin)
{
    static_assert(mayStandInCode(TerminatorKind::Branch) && numberOfNamedOperandsOf(TerminatorKind::Branch) == 1 && !hasVariadicOperand(TerminatorKind::Branch) && numberOfFixedTargetsOf(TerminatorKind::Branch) == 2u);
    RELEASE_ASSERT(!m_isSwept);
    ASSERT(defaultPayloadOf(TerminatorKind::Branch).empty());
    Block& block = m_blocks[checkedIndexInCode(blockID, m_blocks.size())];
    RELEASE_ASSERT(!block.hasEnded());

    Terminator terminator;
    copyOriginToRecordOfCode(terminator, origin, m_inlineFrames.size());
    terminator.m_words[0] = checkedIndexInCode(condition, m_operations.size());
    terminator.m_numberOfInlineOperands = 1;
    terminator.kind = TerminatorKind::Branch;
    terminator.isSet = true;
    terminator.numberOfEdges = 2;
    Edge edge;
    edge.from = blockID;
    terminator.firstEdge = EdgeID(m_edges.append(edge));
    m_edges.append(edge);
    setTerminator(blockID, block, terminator);
}

ALWAYS_INLINE_INTO_OTHER_FILES void Code::appendEdgeInto(BlockID from, BlockID to, std::span<const ValueID> arguments)
{
    Block& entered = m_blocks[checkedIndexInCode(to, m_blocks.size())];
    RELEASE_ASSERT(!entered.isSealed);
    Edge edge;
    edge.from = from;
    edge.to = to;
    edge.m_arguments = arguments.data();
    edge.m_numberOfArguments = static_cast<uint32_t>(arguments.size());
    EdgeID added(m_edges.append(edge));
    if (m_keepsChangesToOlderParts)
        m_changesToOlderParts.append(ChangeToOlderPart { added, to, entered.m_lastEdgeIn, { }, { } });
    if (!entered.m_firstEdgeIn)
        entered.m_firstEdgeIn = added;
    else
        m_edges[entered.m_lastEdgeIn.index()].m_nextInto = added;
    entered.m_lastEdgeIn = added;
}

void Code::endWithJump(BlockID blockID, Origin origin, BlockID to, const ArgumentsOfEdge& brought)
{
    std::span<const ValueID> arguments = brought.span();
    RELEASE_ASSERT(!m_isSwept);
    Block& block = m_blocks[checkedIndexInCode(blockID, m_blocks.size())];
    RELEASE_ASSERT(!block.hasEnded());
    RELEASE_ASSERT(m_blocks[checkedIndexInCode(to, m_blocks.size())].beliefAheadOfEdges == BeliefAheadOfEdges::NoEdgeIsAwaited || brought.m_heldFor == to);
    for (ValueID argument : arguments)
        RELEASE_ASSERT(argument.index() < m_operations.size());

    Terminator terminator;
    copyOriginToRecordOfCode(terminator, origin, m_inlineFrames.size());
    terminator.kind = TerminatorKind::Jump;
    terminator.isSet = true;
    terminator.numberOfEdges = 1;
    terminator.firstEdge = EdgeID(m_edges.size());
    setTerminator(blockID, block, terminator);
    appendEdgeInto(blockID, to, m_sideWords.copy(arguments));
}

void Code::endWithBranch(BlockID blockID, ValueID condition, Origin origin, BlockID taken, BlockID notTaken)
{
    RELEASE_ASSERT(!m_isSwept);
    Block& block = m_blocks[checkedIndexInCode(blockID, m_blocks.size())];
    RELEASE_ASSERT(!block.hasEnded());

    Terminator terminator;
    copyOriginToRecordOfCode(terminator, origin, m_inlineFrames.size());
    terminator.m_words[0] = checkedIndexInCode(condition, m_operations.size());
    terminator.m_numberOfInlineOperands = 1;
    terminator.kind = TerminatorKind::Branch;
    terminator.isSet = true;
    terminator.numberOfEdges = 2;
    terminator.firstEdge = EdgeID(m_edges.size());
    setTerminator(blockID, block, terminator);
    appendEdgeInto(blockID, taken, { });
    appendEdgeInto(blockID, notTaken, { });
}

EdgeID Code::addHandlerEdge(BlockID blockID)
{
    RELEASE_ASSERT(!m_isSwept);
    Block& block = m_blocks[checkedIndexInCode(blockID, m_blocks.size())];
    RELEASE_ASSERT(!block.handler && !block.hasEnded());
    Edge edge;
    edge.from = blockID;
    block.handler = EdgeID(m_edges.append(edge));
    m_hasHandlers = true;
    return block.handler;
}

void Code::closeEdge(EdgeID edgeID, BlockID to, const ArgumentsOfEdge& brought)
{
    std::span<const ValueID> arguments = brought.span();
    RELEASE_ASSERT(!m_isSwept);
    Edge& edge = m_edges[checkedIndexInCode(edgeID, m_edges.size())];
    RELEASE_ASSERT(edge.isOpen());
    RELEASE_ASSERT(to.index() < m_blocks.size() && !m_blocks[to.index()].isSealed);
    RELEASE_ASSERT(m_blocks[to.index()].beliefAheadOfEdges == BeliefAheadOfEdges::NoEdgeIsAwaited || brought.m_heldFor == to);
    for (ValueID argument : arguments)
        RELEASE_ASSERT(argument.index() < m_operations.size());
    std::span<const ValueID> kept = m_sideWords.copy(arguments);
    edge.to = to;
    Block& entered = m_blocks[to.index()];
    if (m_keepsChangesToOlderParts)
        m_changesToOlderParts.append(ChangeToOlderPart { edgeID, to, entered.m_lastEdgeIn, { }, { } });
    if (!entered.m_firstEdgeIn)
        entered.m_firstEdgeIn = edgeID;
    else
        m_edges[entered.m_lastEdgeIn.index()].m_nextInto = edgeID;
    entered.m_lastEdgeIn = edgeID;
    edge.m_arguments = kept.data();
    edge.m_numberOfArguments = static_cast<uint32_t>(kept.size());
}

Code::Mark Code::mark() const
{
    Mark mark;
    mark.numberOfOperations = m_operations.size();
    mark.numberOfBlocks = m_blocks.size();
    mark.numberOfEdges = m_edges.size();
    mark.numberOfInlineFrames = static_cast<uint32_t>(m_inlineFrames.size());
    mark.numberOfEntries = static_cast<uint32_t>(m_entries.size());
    mark.numberOfFrameWords = m_numberOfFrameWords;
    mark.numberOfFactUses = static_cast<uint32_t>(m_factUses.size());
    mark.numberOfEffectsBelieved = static_cast<uint32_t>(m_effectsBelieved.size());
    mark.numberOfOperationsThatWriteWhatTurnHasMade = static_cast<uint32_t>(m_operationsThatWriteWhatTurnHasMade.size());
    mark.numberOfTestsPassed = static_cast<uint32_t>(m_testsPassed.size());
    mark.numberOfSteppedCounters = static_cast<uint32_t>(m_steppedWithCounters.size());
    mark.numberOfTestsOfGuardsInFrontOfCopies = static_cast<uint32_t>(m_testsOfGuardsInFrontOfCopies.size());
    mark.numberOfInputsOfHolesReadAsNaN = static_cast<uint32_t>(m_inputsOfHolesReadAsNaN.size());
    mark.numberOfReadsAnsweredByHolesReadAsNaN = static_cast<uint32_t>(m_readsAnsweredByHolesReadAsNaN.size());
    mark.numberOfValuesThatFoldedArithmeticGives = static_cast<uint32_t>(m_valuesThatFoldedArithmeticGives.size());
    mark.numberOfPollsWithFates = static_cast<uint32_t>(m_pollsWithFates.size());
    mark.numberOfChangesToOlderParts = static_cast<uint32_t>(m_changesToOlderParts.size());
    mark.numberOfOlderBlocksEnded = static_cast<uint32_t>(m_olderBlocksEnded.size());
    m_numberOfBlocksAtNewestMark = m_blocks.size();
    mark.opcodesOfValues = m_opcodesOfValues;
    mark.hasHandlers = m_hasHandlers;
    mark.hasSuspensions = m_hasSuspensions;
    mark.hasLoops = m_hasLoops;
    mark.holdsFrameCells = m_holdsFrameCells;
    mark.holdsCallThatPlanHasNotCounted = m_holdsCallThatPlanHasNotCounted;
    mark.callsOfFunctionNeverCollect = m_callsOfFunctionNeverCollect;
    mark.readsNumbersOutOfBytes = m_readsNumbersOutOfBytes;
    mark.callsItself = m_callsItself;
    return mark;
}

Code::OpenBlock Code::stateOf(BlockID blockID) const
{
    const Block& block = m_blocks[checkedIndexInCode(blockID, m_blocks.size())];
    return { blockID, block.m_last, block.hasEnded(), !!block.handler };
}

void Code::abandonBackTo(const Mark& mark, std::span<const OpenBlock> openBlocks)
{
    RELEASE_ASSERT(!m_isSwept && m_keepsChangesToOlderParts);
    RELEASE_ASSERT(mark.numberOfOperations <= m_operations.size() && mark.numberOfBlocks <= m_blocks.size() && mark.numberOfEdges <= m_edges.size());
    RELEASE_ASSERT(mark.numberOfInlineFrames && mark.numberOfInlineFrames <= m_inlineFrames.size() && mark.numberOfEntries <= m_entries.size());
    RELEASE_ASSERT(mark.numberOfChangesToOlderParts <= m_changesToOlderParts.size());

    while (m_changesToOlderParts.size() > mark.numberOfChangesToOlderParts) {
        ChangeToOlderPart change = m_changesToOlderParts.takeLast();
        if (change.marked) {
            if (change.marked.index() < mark.numberOfBlocks) {
                m_blocks[change.marked.index()].frequency = change.frequencyBefore;
                m_blocks[change.marked.index()].isInLoop = change.wasInLoop;
                m_blocks[change.marked.index()].isSealed = change.wasSealed;
            }
            continue;
        }
        if (change.changed) {
            if (change.changed.index() < mark.numberOfOperations)
                m_operations[change.changed.index()] = change.recordBefore;
            continue;
        }
        if (change.entered.index() < mark.numberOfBlocks) {
            Block& entered = m_blocks[change.entered.index()];
            entered.m_lastEdgeIn = change.lastEdgeInBefore;
            if (change.lastEdgeInBefore)
                m_edges[change.lastEdgeInBefore.index()].m_nextInto = EdgeID();
            else
                entered.m_firstEdgeIn = EdgeID();
        }
        if (change.closed.index() < mark.numberOfEdges) {
            Edge& edge = m_edges[change.closed.index()];
            edge.to = BlockID();
            edge.m_arguments = nullptr;
            edge.m_numberOfArguments = 0;
            edge.m_nextInto = EdgeID();
        }
    }

    Vector<PartsAbandoned, 4> deadAlready;
    while (!m_partsAbandoned.isEmpty() && m_partsAbandoned.last().operations.first >= mark.numberOfOperations)
        deadAlready.append(m_partsAbandoned.takeLast());
    deadAlready.reverse();
    auto forEachLeftAlive = [&](std::pair<uint32_t, uint32_t> PartsAbandoned::* part, uint32_t first, uint32_t end, const auto& functor) {
        for (const PartsAbandoned& dead : deadAlready) {
            for (uint32_t index = first; index < (dead.*part).first; ++index)
                functor(index);
            first = std::max(first, (dead.*part).second);
        }
        for (uint32_t index = first; index < end; ++index)
            functor(index);
    };

    forEachLeftAlive(&PartsAbandoned::operations, mark.numberOfOperations, m_operations.size(), [&](uint32_t index) {
        const Operation& operation = m_operations[index];
        if (operation.blockIndex >= mark.numberOfBlocks)
            return;
        Block& block = m_blocks[operation.blockIndex];
        if (operation.opcode == Opcode::Parameter) {
            block.m_parameters = nullptr;
            block.m_numberOfParameters = 0;
        } else if (!operation.previous || operation.previous.index() < mark.numberOfOperations)
            block.m_last = operation.previous;
    });
    forEachLeftAlive(&PartsAbandoned::edges, mark.numberOfEdges, m_edges.size(), [&](uint32_t index) {
        const Edge& edge = m_edges[index];
        if (!edge.from || edge.from.index() >= mark.numberOfBlocks)
            return;
        Block& block = m_blocks[edge.from.index()];
        if (block.handler == EdgeID(index))
            block.handler = EdgeID();
        else if (block.terminator.isSet && block.terminator.numberOfEdges && block.terminator.firstEdge.index() <= index && index < block.terminator.firstEdge.index() + block.terminator.numberOfEdges)
            block.terminator = Terminator();
    });
    for (uint32_t index = mark.numberOfEntries; index < m_entries.size(); ++index) {
        BlockID entered = m_entries[index].block;
        if (entered.index() < mark.numberOfBlocks)
            m_blocks[entered.index()].isEntry = false;
    }
    while (m_olderBlocksEnded.size() > mark.numberOfOlderBlocksEnded) {
        OlderBlockEnded ended = m_olderBlocksEnded.takeLast();
        if (ended.block.index() < mark.numberOfBlocks)
            m_blocks[ended.block.index()].terminator = ended.before;
    }
    for (const OpenBlock& open : openBlocks) {
        Block& block = m_blocks[checkedIndexInCode(open.block, mark.numberOfBlocks)];
        if (!open.hadEnded)
            block.terminator = Terminator();
        RELEASE_ASSERT(block.m_last == open.last);
        RELEASE_ASSERT(block.hasEnded() == open.hadEnded);
        RELEASE_ASSERT(!!block.handler == open.hadHandler);
    }

    Block dead;
    dead.frequency = BlockFrequency::Rare;
    dead.terminator.isSet = true;
    if (!m_graveyard)
        m_graveyard = BlockID(m_blocks.append(dead));
    forEachLeftAlive(&PartsAbandoned::blocks, mark.numberOfBlocks, m_blocks.size(), [&](uint32_t index) {
        m_blocks[index] = dead;
    });
    forEachLeftAlive(&PartsAbandoned::edges, mark.numberOfEdges, m_edges.size(), [&](uint32_t index) {
        Edge& edge = m_edges[index];
        edge.from = m_graveyard;
        edge.to = m_graveyard;
        edge.m_arguments = nullptr;
        edge.m_numberOfArguments = 0;
        edge.m_nextInto = EdgeID();
    });
    while (!m_recordsAbandoned.isEmpty() && m_recordsAbandoned.last().first >= mark.numberOfOperations)
        m_recordsAbandoned.removeLast();
    if (mark.numberOfOperations < m_operations.size())
        m_recordsAbandoned.append({ mark.numberOfOperations, m_operations.size() });
    Operation deadRecord;
    deadRecord.opcode = Opcode::Undefined;
    deadRecord.type = TypeTable::undefined;
    deadRecord.representation = Representation::JSValue;
    forEachLeftAlive(&PartsAbandoned::operations, mark.numberOfOperations, m_operations.size(), [&](uint32_t index) {
        if (m_operations[index].blockIndex != Operation::noBlock)
            m_operations[index] = deadRecord;
        else
            m_operations[index].inlineFrame = 0;
    });
    m_partsAbandoned.append(PartsAbandoned { { mark.numberOfOperations, static_cast<uint32_t>(m_operations.size()) }, { mark.numberOfBlocks, static_cast<uint32_t>(m_blocks.size()) }, { mark.numberOfEdges, static_cast<uint32_t>(m_edges.size()) } });
    m_inlineFrames.shrink(mark.numberOfInlineFrames);
    m_entries.shrink(mark.numberOfEntries);
    m_numberOfFrameWords = mark.numberOfFrameWords;
    m_factUses.shrink(mark.numberOfFactUses);
    m_effectsBelieved.shrink(mark.numberOfEffectsBelieved);
    m_operationsThatWriteWhatTurnHasMade.shrink(mark.numberOfOperationsThatWriteWhatTurnHasMade);
    m_testsPassed.shrink(mark.numberOfTestsPassed);
    m_steppedWithCounters.shrink(mark.numberOfSteppedCounters);
    m_testsOfGuardsInFrontOfCopies.shrink(mark.numberOfTestsOfGuardsInFrontOfCopies);
    m_inputsOfHolesReadAsNaN.shrink(mark.numberOfInputsOfHolesReadAsNaN);
    m_readsAnsweredByHolesReadAsNaN.shrink(std::min<size_t>(m_readsAnsweredByHolesReadAsNaN.size(), mark.numberOfReadsAnsweredByHolesReadAsNaN));
    m_valuesThatFoldedArithmeticGives.shrink(mark.numberOfValuesThatFoldedArithmeticGives);
    m_pollsWithFates.shrink(mark.numberOfPollsWithFates);
    m_opcodesOfValues = mark.opcodesOfValues;
    m_hasHandlers = mark.hasHandlers;
    m_hasSuspensions = mark.hasSuspensions;
    m_hasLoops = mark.hasLoops;
    m_holdsFrameCells = mark.holdsFrameCells;
    m_holdsCallThatPlanHasNotCounted = mark.holdsCallThatPlanHasNotCounted;
    m_callsOfFunctionNeverCollect = mark.callsOfFunctionNeverCollect;
    m_readsNumbersOutOfBytes = mark.readsNumbersOutOfBytes;
    m_callsItself = mark.callsItself;
}

uint32_t Code::addInlineFrame(const InlineFrame& frame)
{
    RELEASE_ASSERT(!m_isSwept);
    bool namesFunctionItself = !frame.calledBuiltin && (!frame.callee || frame.callee == m_unit.function);
    if (m_blocks.isEmpty() && m_inlineFrames.size() == 1 && namesFunctionItself && !frame.parent) {
        m_inlineFrames[0] = frame;
        m_inlineFrames[0].callee = m_unit.function;
        return 0;
    }
    RELEASE_ASSERT(frame.callee || frame.calledBuiltin || !walksInputBody(m_unit.role));
    RELEASE_ASSERT(frame.parent < m_inlineFrames.size());
    RELEASE_ASSERT(m_inlineFrames.size() < mostInlineFramesInCode);
    m_inlineFrames.append(frame);
    return static_cast<uint32_t>(m_inlineFrames.size() - 1);
}

uint16_t Code::addEntry(BlockID blockID)
{
    RELEASE_ASSERT(!m_isSwept);
    RELEASE_ASSERT(m_entries.size() <= std::numeric_limits<uint16_t>::max());
    Block& block = m_blocks[checkedIndexInCode(blockID, m_blocks.size())];
    RELEASE_ASSERT(!block.isEntry);
    block.isEntry = true;
    m_entries.append(EntryOfCode { blockID });
    return static_cast<uint16_t>(m_entries.size() - 1);
}

void Code::addFactUse(const FactUse& use)
{
    RELEASE_ASSERT(!m_isSwept);
    RELEASE_ASSERT(use.record.index() < m_operations.size());
    m_factUses.append(use);
}

void Code::addFateOfPoll(ValueID poll, FateOfPoll fate, ValueID unlessKept)
{
    RELEASE_ASSERT(!m_isSwept);
    RELEASE_ASSERT(m_operations[checkedIndexInCode(poll, m_operations.size())].opcode == Opcode::LoopHint);
    RELEASE_ASSERT(!unlessKept || (fate == FateOfPoll::LeavesHandler && m_operations[checkedIndexInCode(unlessKept, m_operations.size())].opcode != Opcode::LoopHint));
    m_pollsWithFates.append(PollWithFate { poll, fate, unlessKept });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
