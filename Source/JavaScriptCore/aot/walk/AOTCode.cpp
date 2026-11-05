/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCode.h"

#if ENABLE(AOT)

#include "AOTDiagnostics.h"
#include "AOTIRFunction.h"
#include "AOTIRProgram.h"
#include "AOTLimits.h"
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
    Opcode::ReportMismatch,
    Opcode::StringBuilderAppend,
    Opcode::MayBindAnswersOfGetAndSetTraps, Opcode::MayBindAnswersOfHasTraps, Opcode::ValidateAnswerOfGetTrap, Opcode::ValidateDenialOfHasTrap, Opcode::ValidateConsentOfDeleteTrap,
    Opcode::HasOwnByAnswerOfDescriptorTrap, Opcode::FinishStoreByTrap,
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
    return isFloat(operation.typedArrayType()) && hasInlineElementAccess(operation.typedArrayType());
}

bool handsOnBitsOfNaN(const Operation& operation)
{
    constexpr OpcodeSet arithmetic { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Div };
    return arithmetic.contains(operation.opcode) && operation.form() == OperatorForm::Numbers && operation.representation == Representation::Double;
}

bool takesAnyNaN(const Operation& user, uint32_t operandIndex)
{
    constexpr OpcodeSet relationsAndOperatorsOnBits { Opcode::Less, Opcode::LessEq, Opcode::Greater, Opcode::GreaterEq, Opcode::BitAnd, Opcode::BitOr, Opcode::BitXor, Opcode::LShift, Opcode::RShift, Opcode::URShift };
    if (relationsAndOperatorsOnBits.contains(user.opcode))
        return user.form() == OperatorForm::Numbers;
    if (user.opcode == Opcode::ToInt32 || user.opcode == Opcode::ToUint32)
        return true;
    if (user.opcode == Opcode::PutTypedArrayElement)
        return operandIndex == OperandIndex::PutTypedArrayElement::value && user.isInsideView() && hasInlineElementAccess(user.typedArrayType()) && !isClamped(user.typedArrayType());
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
};

} // anonymous namespace

static constexpr std::array<ShapeOfEntryInCode, numberOfOpcodes> shapesOfOperationsInCode = []() consteval {
    std::array<ShapeOfEntryInCode, numberOfOpcodes> shapes { };
    for (Opcode opcode : allMembersOf<Opcode>)
        shapes[static_cast<unsigned>(opcode)] = { declaredFlagsOf(opcode), static_cast<uint8_t>(numberOfNamedOperandsOf(opcode)), hasVariadicOperand(opcode) };
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
    return PayloadWord::storage(draft.opcode) && draft.storage() != AllocationStorage::Heap;
}

static ALWAYS_INLINE ValueID appendRecordToCode(StableArray<Operation, 10>& operations, const Operation& operation)
{
    RELEASE_ASSERT(operations.size() < Reference::indexMask);
    return ValueID(operations.append(operation));
}

ALWAYS_INLINE void Code::fill(Instruction& instruction, const DraftWords& draft)
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
        RELEASE_ASSERT(numberOfNamedOperands <= Instruction::maxNumberOfNamedOperands);
        payloadWords = m_sideWords.allocate(numberOfPayloadWords + numberOfOperands);
        operandWords = payloadWords + numberOfPayloadWords;
        auto address = std::bit_cast<std::array<uint32_t, 2>>(payloadWords);
        instruction.m_words = { address[0], address[1], numberOfPayloadWords << 4 | numberOfNamedOperands, numberOfOperands };
        instruction.m_hasSideWords = true;
    }

    for (uint32_t index = 0; index < numberOfPayloadWords; ++index)
        payloadWords[index] = payload[index];
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

const Effects* Code::effectsBelievedOf(ValueID id) const
{
    auto found = std::ranges::lower_bound(m_effectsBelieved, id.index(), { }, [](const auto& noted) { return noted.first.index(); });
    return found != m_effectsBelieved.end() && found->first == id ? &found->second : nullptr;
}

void Code::reset(CodeUnit unit)
{
    m_unit = unit;
    m_operations.clear();
    m_blocks.clear();
    m_edges.clear();
    m_sideWords.clear();
    m_inlineFrames.shrink(0);
    m_entries.shrink(0);
    m_factUses.shrink(0);
    m_effectsBelieved.shrink(0);
    m_pollsWithFates.shrink(0);
    m_opcodesOfValues = { };
    m_numberOfFrameWords = 0;
    m_hasHandlers = false;
    m_hasSuspensions = false;
    m_hasLoops = false;
    m_holdsFrameCells = false;
    m_holdsCallThatPlanHasNotCounted = false;
    m_callsOfFunctionNeverCollect = false;
    m_readsNumbersOutOfBytes = false;

    m_isSwept = false;
    m_marks.shrink(0);
    m_useCounts.shrink(0);
    m_onlyUsers.shrink(0);
    m_parametersFed.shrink(0);
    m_order.shrink(0);
    m_reachedBlocks.clearAll();
    m_blocksWithoutThrows.clearAll();
    m_suspensions.shrink(0);
    m_liveValues.shrink(0);
    m_releasesBehindLastReads.shrink(0);

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

BlockID Code::addBlock(BlockRole role, BlockFrequency frequency)
{
    RELEASE_ASSERT(!m_isSwept);
    RELEASE_ASSERT(m_blocks.size() < Operation::noBlock);
    Block block;
    block.role = role;
    block.frequency = frequency;
    if (block.isHandlerBlock())
        m_hasHandlers = true;
    return BlockID(m_blocks.append(block));
}

void Code::setLoopMarks(BlockID blockID, bool isInLoop, bool isLoopHead)
{
    RELEASE_ASSERT(!m_isSwept);
    Block& block = m_blocks[checkedIndexInCode(blockID, m_blocks.size())];
    block.isInLoop = isInLoop && m_unit.role != CodeRole::SlowBody;
    block.isLoopHead = isLoopHead;
    if (isInLoop || isLoopHead)
        m_hasLoops = true;
}

void Code::setSubject(BlockID blockID, ValueID subject)
{
    RELEASE_ASSERT(!m_isSwept);
    RELEASE_ASSERT(!subject || subject.index() < m_operations.size());
    m_blocks[checkedIndexInCode(blockID, m_blocks.size())].subject = subject;
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
    record.type = type;
}

void Code::makeTypeTestAskInFull(ValueID test)
{
    RELEASE_ASSERT(!m_isSwept);
    Operation& record = m_operations[checkedIndexInCode(test, m_operations.size())];
    RELEASE_ASSERT(record.opcode == Opcode::TypeTest);
    record.flags |= *FlagMask::asksInFull(Opcode::TypeTest);
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
    RELEASE_ASSERT(!opcodesThatCodeNeverAppends.contains(draft.opcode));
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
    if (readsNumberOutOfBytes(operation))
        m_readsNumbersOutOfBytes = true;
    return id;
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

ValueID Code::addCopyOfConstant(ValueID constant, Representation representation)
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
    terminator.isSet = true;
    terminator.numberOfEdges = numberOfEdges;
    if (numberOfEdges) {
        terminator.firstEdge = EdgeID(m_edges.size());
        Edge edge;
        edge.from = blockID;
        for (uint32_t index = 0; index < numberOfEdges; ++index)
            m_edges.append(edge);
    }
    block.terminator = terminator;
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
    block.terminator = terminator;
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
    block.terminator = terminator;
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

void Code::closeEdge(EdgeID edgeID, BlockID to, std::span<const ValueID> arguments)
{
    RELEASE_ASSERT(!m_isSwept);
    Edge& edge = m_edges[checkedIndexInCode(edgeID, m_edges.size())];
    RELEASE_ASSERT(edge.isOpen());
    RELEASE_ASSERT(to.index() < m_blocks.size());
    for (ValueID argument : arguments)
        RELEASE_ASSERT(argument.index() < m_operations.size());
    std::span<const ValueID> kept = m_sideWords.copy(arguments);
    edge.to = to;
    Block& entered = m_blocks[to.index()];
    if (!entered.m_firstEdgeIn)
        entered.m_firstEdgeIn = edgeID;
    else
        m_edges[entered.m_lastEdgeIn.index()].m_nextInto = edgeID;
    entered.m_lastEdgeIn = edgeID;
    edge.m_arguments = kept.data();
    edge.m_numberOfArguments = static_cast<uint32_t>(kept.size());
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
    RELEASE_ASSERT(!namesFunctionItself || !walksInputBody(m_unit.role));
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

void Code::addFateOfPoll(ValueID poll, FateOfPoll fate)
{
    RELEASE_ASSERT(!m_isSwept);
    RELEASE_ASSERT(m_operations[checkedIndexInCode(poll, m_operations.size())].opcode == Opcode::LoopHint);
    m_pollsWithFates.append(PollWithFate { poll, fate });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
