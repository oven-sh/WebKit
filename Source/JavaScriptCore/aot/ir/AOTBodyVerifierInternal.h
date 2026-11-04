/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBodyVerifier.h"
#include "AOTBuiltinTable.h"
#include "AOTDiagnostics.h"
#include "AOTIR.h"
#include "AOTResultRules.h"
#include <optional>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

// Shared by AOTBodyVerifier.cpp, AOTBodyVerifierFlow.cpp, AOTBodyVerifierOperations.cpp, AOTBodyVerifierCalls.cpp and the generated AOTSchemaBodyVerifier.cpp. Nobody else includes this.

class BodyVerifier;

// Specialized in the generated AOTSchemaBodyVerifier.h for each external kind that a wire record or entry names.
template<typename External> bool isValidExternal(uint32_t word, const BuiltinTable&);

// The words of a body, taken one after the other. No word is read that is not there, and after the first failure every take answers zero.
class CheckedBodyWords {
public:
    CheckedBodyWords(BodyVerifier&, const Body&);

    bool isAtEnd() const { return m_offset == m_words.size(); }
    uint32_t offset() const { return static_cast<uint32_t>(m_offset); }
    BodyVerifier& verifier() const { return m_verifier; }

    uint32_t take();
    void skip(uint32_t numberOfWords);
    // The count of a list whose elements have at least that many words each. It fails where so many words are not left.
    uint32_t takeCount(uint32_t minWordsPerElement = 1);

    void takeOperand(bool isOptional);
    void takeVariadicOperands();
    uint32_t takeBlock();
    // In the unit's table, with an entry that is not zero and, where the verifier has a program, a record of that number.
    template<typename IDType> void takeID(bool isOptional);
    template<typename E> uint32_t takeEnumerator(bool isOptional = false);
    template<typename F> uint32_t takeFlagWord();
    uint32_t takeFlags(uint32_t wireMask);
    bool takeBoolean();
    template<typename External> void takeExternal(bool isOptional);

private:
    bool hasWords(uint64_t count) const { return count <= m_end - m_offset; }
    void checkID(RemappedIDKind, uint32_t word);
    // From then on no word is left.
    void fail(InternalErrorKind);
    NEVER_INLINE uint32_t failForLackOfWords();

    BodyVerifier& m_verifier;
    std::span<const uint32_t> m_words;
    const UnitIDRemap& m_unit;
    // By RemappedIDKind. Without a program no number bounds an id.
    const NumbersOfProgramRecords& m_numbersOfRecords;
    uint32_t m_numberOfValues;
    uint32_t m_numberOfBlocks;
    size_t m_offset { 0 };
    // Behind the last word, until the first failure. Then m_offset.
    size_t m_end;
};

class FlowGraphOfVerifiedBody {
    WTF_MAKE_NONCOPYABLE(FlowGraphOfVerifiedBody);
public:
    FlowGraphOfVerifiedBody() = default;

    void build(const BodyIndex&);

    uint32_t numberOfBlocks() const { return static_cast<uint32_t>(m_firstSuccessor.size()) - 1; }
    // The targets of the terminator in order, then the handler.
    std::span<const BlockID> successors(BlockID block) const { return listOf(block, m_successors, m_firstSuccessor); }
    // Ascending. A block with two edges to this one is listed twice.
    std::span<const BlockID> predecessors(BlockID block) const { return listOf(block, m_predecessors, m_firstPredecessor); }
    // Some edge does not lead forward in the stream. Without one there is no cycle.
    bool hasRetreatingEdge() const { return m_hasRetreatingEdge; }

private:
    static std::span<const BlockID> listOf(BlockID block, const Vector<BlockID>& lists, const Vector<uint32_t>& firstOf)
    {
        uint32_t first = firstOf[block.index()];
        return lists.span().subspan(first, firstOf[block.index() + 1] - first);
    }

    Vector<BlockID> m_successors;
    Vector<uint32_t> m_firstSuccessor;
    Vector<BlockID> m_predecessors;
    Vector<uint32_t> m_firstPredecessor;
    Vector<uint32_t> m_nextPredecessor;
    bool m_hasRetreatingEdge { false };
};

// What the look at one body leaves for the look at the next to use again: memory, and what is the same for every body of a program. One for each thread at work.
struct BodyVerifierScratch {
    WTF_MAKE_NONCOPYABLE(BodyVerifierScratch);
public:
    // Null: ids are checked against the unit's tables only.
    explicit BodyVerifierScratch(const Program*);

    NumbersOfProgramRecords numbersOfRecords;
    // Each operand of theirs takes whatever has one result and opens no iteration, a parameter of a block too.
    const OpcodeSet& opcodesThatTakeAnyValue;

    // Built behind verifyEdges().
    FlowGraphOfVerifiedBody graph;
    // By value, from the words alone.
    Vector<BlockID> blocksOfValues;
    Vector<uint32_t> numbersOfUses;
    Vector<BodyUser> firstUsers;
    // By block.
    Vector<BlockID> dominators;
    Vector<uint32_t> lastRoundsThatClimbedPast;
    Vector<uint32_t> firstChildren;
    Vector<uint32_t> nextChildren;
    Vector<BlockID> children;
    Vector<BlockID> blocksInDominatorOrder;
    Vector<uint32_t> lastNumbersBelow;
    Vector<BlockID> path;
    Vector<uint32_t> lastLoopHeadsThatReach;
    Vector<BlockID> worklist;
};

struct MethodDefinitionInBody {
    ValueID classObject;
    uint32_t index { 0 };
    BlockID block;
    ValueID value;
};

struct ModuleEvaluationInBody {
    ModuleID referrer;
    ModuleID target;
    BlockID block;
    ValueID value;
    bool isBare { false };
};

class BodyVerifier {
    WTF_MAKE_NONCOPYABLE(BodyVerifier);
public:
    // Without a program and a function, only runWords() and failWhereOrderMisleadsBodyIndex() may be called. The scratch is made with the same program.
    BodyVerifier(const Program*, FunctionID, const Function*, const Body&, BodyVerifierScratch&);
    ~BodyVerifier();

    // In front of run(): the index is built in this table, under the number of the function, and outlives the verifier. `worker`: as for BodyIndexTable::build().
    void keepIndexIn(BodyIndexTable& table, unsigned worker)
    {
        m_tableThatKeepsIndex = &table;
        m_workerOfTableThatKeepsIndex = worker;
    }
    // In front of run().
    void leaveIndexUncompared() { m_comparesIndexWithPlainComputation = false; }

    bool runWords();
    // Behind runWords(), which has looked for it on its way: BodyIndexTable::build() stops the process at a block that only later blocks lead to, and it looks for the test of a Branch among the values in front of the Branch.
    bool failWhereOrderMisleadsBodyIndex();
    bool run();
    Vector<InternalError> takeErrors();

    void fail(InternalErrorKind, BlockID = { }, ValueID = { }, uint32_t count = 0);
    bool hasFailed() const { return !m_errors.isEmpty(); }
    // The block and the value that the words stage is checking.
    void setSubject(BlockID block, ValueID value)
    {
        m_block = block;
        m_value = value;
    }
    void failAtSubject(InternalErrorKind kind) { fail(kind, m_block, m_value); }

    bool hasProgram() const { return m_program; }
    const Program& program() const { return *m_program; }
    FunctionID id() const { return m_id; }
    const Function& function() const { return *m_function; }
    const Body& body() const { return m_body; }
    // Only after the words stage.
    const BodyIndex& index() const { return *m_index; }
    BodyVerifierScratch& scratch() const { return m_scratch; }
    bool isValidField(LayoutID, uint32_t field) const;

private:
    // AOTBodyVerifier.cpp
    void verifyWords();
    void verifyWordsOfBlock(CheckedBodyWords&, BlockID, uint32_t& nextValue);
    void verifyWordsOfValue(CheckedBodyWords&, BlockID, ValueID);
    void verifyWordsOfTerminator(CheckedBodyWords&, BlockID, uint32_t endOfValues);
    void verifyWordsOfTarget(CheckedBodyWords&, BlockID from);
    void noteOrderThatMisleadsBodyIndex(InternalErrorKind, BlockID, ValueID = { });
    void buildIndex();

    // AOTBodyVerifierFlow.cpp
    void verifyFlow();
    void verifyBlockOrder();
    void verifyDominance();
    // verifyDominance() and the loop of verifyOperations() at once: each value is read once. It finds a fault exactly where one of the two does.
    void verifyUsesAndOperations();
    // Every path to `to` goes through the terminator of `from`, not only its head: a block that is reached through the handler of `from` sees none of the values of `from`.
    bool terminatorDominates(BlockID from, BlockID to);
    // `mayHaveToNameDefinition`: no, where the caller knows that a parameter of a block will do. It answers with the opcode of the definition.
    Opcode verifyUse(ValueID used, BodyUseKind, BlockID blockOfUse, ValueID user, uint32_t operandIndex, bool mayHaveToNameDefinition);
    void verifyUseStepByStep(ValueID used, BodyUseKind, BlockID blockOfUse, ValueID user, uint32_t operandIndex);
    void verifyEdges();
    void verifyEdge(const BodyBlock& from, uint32_t targetIndex, BodyTarget);
    void verifyHandler(const BodyBlock&);
    void verifyBlockRole(const BodyBlock&);
    void verifyHandlerBlocksAreEnteredByHandlerEdges();
    void verifyTargetCount(const BodyBlock&);
    void verifyLoopMarks();
    void verifyEntryParameters();
    void verifyIndexAgainstPlainComputation();

    // AOTBodyVerifierOperations.cpp
    void verifyOperations();
    void verifyOperation(const BodyBlock&, uint32_t indexInBlock, const BodyValue&);
    // All of verifyOperation() behind the look at the fields and at the definitions of the operands.
    void verifyOperationByOpcode(const BodyBlock&, uint32_t indexInBlock, const BodyValue&);
    void verifyTerminatorOperation(const BodyBlock&, const BodyTerminator&);
    void verifyOperandDefinitions(const BodyBlock&, const BodyValue&);
    void verifyOperandDefinitionsOneByOne(const BodyBlock&, const BodyValue&);
    // Of a definition among definitionsThatFewOperandsTakeInVerifiedBodies. No: it gives one value like any other, as a direct call of a function with one result does.
    bool isTakenByFewOperands(const BodyValue& definition) const;
    void verifyNumberOfVariadicOperands(const BodyBlock&, const BodyValue&);
    void verifyCatch(const BodyBlock&, uint32_t indexInBlock, const BodyValue&);
    void verifyCast(const BodyBlock&, const BodyValue&);
    void verifyConvertedAllocation(const BodyBlock&, const BodyValue&);
    void verifyAllocation(const BodyBlock&, const BodyValue&);
    void verifyObjectUnderConstruction(const BodyBlock&, const BodyValue&);
    void verifyMethodIndexes();
    void verifyReturn(const BodyBlock&, const BodyTerminator&);
    void verifyReturnCall(const BodyBlock&, const BodyTerminator&);
    void verifyPositions(const BodyBlock&, const BodyValue&);

    // AOTBodyVerifierCalls.cpp
    void verifyClosureOperation(const BodyBlock&, const BodyValue&);
    void verifyDirectCall(const BodyBlock&, const BodyValue&);
    void verifyArgumentsOperation(const BodyBlock&, const BodyValue&);
    void verifyClassOperation(const BodyBlock&, const BodyValue&);
    void verifyPrivateAccess(const BodyBlock&, const BodyValue&);
    void verifySuspension(const BodyBlock&, const BodyValue&);
    void verifyGeneratorStart();
    void verifyBuiltinConstant(const BodyBlock&, const BodyValue&);
    void verifyDirectEval(const BodyBlock&, const BodyValue&);
    void verifyModuleEvaluation(const BodyBlock&, const BodyValue&);
    void verifyBareEvaluations();
    void verifyRequestsOfEvaluations();

    const Program* m_program;
    FunctionID m_id;
    const Function* m_function;
    Body m_body;
    BodyVerifierScratch& m_scratch;
    std::unique_ptr<BodyIndexTable> m_indexes;
    BodyIndexTable* m_tableThatKeepsIndex { nullptr };
    bool m_comparesIndexWithPlainComputation { true };
    unsigned m_workerOfTableThatKeepsIndex { 0 };
    const BodyIndex* m_index { nullptr };
    Vector<InternalError> m_errors;
    BitVector m_isTargetOfEarlierBlock;
    // By the words stage. Without such an edge there is no cycle.
    bool m_hasEdgeThatDoesNotLeadForward { false };
    bool m_hasHandlerBlock { false };
    // verifyUsesAndOperations() has found no fault.
    bool m_operationsAreVerified { false };
    // The first that the words stage met. InternalErrorKind::Unknown: none.
    InternalErrorKind m_kindOfOrderThatMisleadsBodyIndex { InternalErrorKind::Unknown };
    BlockID m_blockOfOrderThatMisleadsBodyIndex;
    ValueID m_valueOfOrderThatMisleadsBodyIndex;
    BlockID m_block;
    ValueID m_value;
    ValueID m_initialYield;
    Vector<ValueID, 4> m_yields;
    Vector<MethodDefinitionInBody> m_methodDefinitions;
    Vector<ModuleEvaluationInBody> m_moduleEvaluations;
    std::optional<BitVector> m_blocksReachedFromHandlerOfDominator;
};

// AOTBodyVerifierOperations.cpp
const OpcodeSet& opcodesThatTakeAnyValueInVerifiedBodies();

// What they give is no value of JavaScript: only the operations of the iteration take it, and what holds values across a suspension.
inline constexpr OpcodeSet opcodesThatOpenIterationsInVerifiedBodies { Opcode::IteratorOpen, Opcode::ForInOpen };
inline constexpr OpcodeSet definitionsThatFewOperandsTakeInVerifiedBodies = opcodesThatMayHaveSeveralResults | opcodesThatOpenIterationsInVerifiedBodies;

// Generated: AOTSchemaBodyVerifier.cpp. They take the fields of one entry, in declared order, and check each word by its kind.
void verifyFieldWords(CheckedBodyWords&, Opcode);
void verifyFieldWords(CheckedBodyWords&, TerminatorKind);
// After the words stage: what the schema's columns require of the fields together. exactlyOneOf, and a Field that its Layout has.
void verifyFieldRules(BodyVerifier&, const BodyBlock&, const BodyValue&);
void verifyFieldRules(BodyVerifier&, const BodyBlock&, const BodyTerminator&);

inline CheckedBodyWords::CheckedBodyWords(BodyVerifier& verifier, const Body& body)
    : m_verifier(verifier)
    , m_words(body.words())
    , m_unit(body.unit())
    , m_numbersOfRecords(verifier.scratch().numbersOfRecords)
    , m_numberOfValues(body.numberOfValues())
    , m_numberOfBlocks(body.numberOfBlocks())
    , m_end(m_words.size())
{
}

inline void CheckedBodyWords::fail(InternalErrorKind kind)
{
    m_verifier.failAtSubject(kind);
    m_end = m_offset;
}

ALWAYS_INLINE uint32_t CheckedBodyWords::take()
{
    if (m_offset == m_end) [[unlikely]]
        return failForLackOfWords();
    return m_words[m_offset++];
}

ALWAYS_INLINE void CheckedBodyWords::skip(uint32_t numberOfWords)
{
    if (!hasWords(numberOfWords)) [[unlikely]] {
        failForLackOfWords();
        return;
    }
    m_offset += numberOfWords;
}

ALWAYS_INLINE uint32_t CheckedBodyWords::takeCount(uint32_t minWordsPerElement)
{
    uint32_t count = take();
    if (hasWords(static_cast<uint64_t>(count) * minWordsPerElement)) [[likely]]
        return count;
    return failForLackOfWords();
}

ALWAYS_INLINE void CheckedBodyWords::takeOperand(bool isOptional)
{
    uint32_t word = take();
    if (word < m_numberOfValues) [[likely]]
        return;
    if (word != Wire::none)
        fail(InternalErrorKind::IdOutOfRange);
    else if (!isOptional)
        fail(InternalErrorKind::OperandMissing);
}

inline void CheckedBodyWords::takeVariadicOperands()
{
    for (uint32_t remaining = takeCount(); remaining; --remaining)
        takeOperand(false);
}

ALWAYS_INLINE uint32_t CheckedBodyWords::takeBlock()
{
    uint32_t word = take();
    if (word >= m_numberOfBlocks) [[unlikely]]
        fail(InternalErrorKind::IdOutOfRange);
    return word;
}

ALWAYS_INLINE uint32_t CheckedBodyWords::takeFlags(uint32_t wireMask)
{
    uint32_t word = take();
    if (word & ~wireMask) [[unlikely]]
        fail(InternalErrorKind::EngineOnlyOnWire);
    return word;
}

ALWAYS_INLINE bool CheckedBodyWords::takeBoolean()
{
    uint32_t word = take();
    if (word > 1) [[unlikely]]
        fail(InternalErrorKind::UnknownEnumerator);
    return word == 1;
}

ALWAYS_INLINE void CheckedBodyWords::checkID(RemappedIDKind kind, uint32_t word)
{
    const IDRemapTable& table = m_unit.tables[static_cast<size_t>(kind)];
    // An entry of the table of types holds the bits of a TypeID, which are no index, so no number bounds it.
    if (word >= table.size || static_cast<size_t>(table.entries[word]) - 1 >= m_numbersOfRecords[kind]) [[unlikely]]
        fail(InternalErrorKind::IdOutOfRange);
}

template<typename IDType>
void CheckedBodyWords::takeID(bool isOptional)
{
    uint32_t word = take();
    if (isOptional && word == Wire::none)
        return;
    checkID(remappedIDKindOf<IDType>, word);
}

template<typename E>
uint32_t CheckedBodyWords::takeEnumerator(bool isOptional)
{
    static_assert(numberOfEnumerators<E> && !isFlagWord<E>);
    uint32_t word = take();
    if (isOptional && word == Wire::none)
        return word;
    if (word >= numberOfEnumerators<E>) [[unlikely]]
        fail(InternalErrorKind::UnknownEnumerator);
    else if (word >= numberOfWireEnumerators<E>) [[unlikely]]
        fail(InternalErrorKind::EngineOnlyOnWire);
    return word;
}

template<typename F>
uint32_t CheckedBodyWords::takeFlagWord()
{
    static_assert(isFlagWord<F> && numberOfEnumerators<F> <= 32);
    constexpr uint64_t declaredMask = (uint64_t { 1 } << numberOfEnumerators<F>) - 1;
    constexpr uint64_t wireMask = (uint64_t { 1 } << numberOfWireEnumerators<F>) - 1;
    uint32_t word = take();
    if (word & ~declaredMask) [[unlikely]]
        fail(InternalErrorKind::UnknownFlag);
    else if (word & ~wireMask) [[unlikely]]
        fail(InternalErrorKind::EngineOnlyOnWire);
    return word;
}

template<typename External>
void CheckedBodyWords::takeExternal(bool isOptional)
{
    uint32_t word = take();
    if (isOptional && word == Wire::none)
        return;
    if (!isValidExternal<External>(word, BuiltinTable::singleton())) [[unlikely]]
        fail(InternalErrorKind::IdOutOfRange);
}

} } } // namespace JSC::AOT::Typed

#include "AOTSchemaBodyVerifier.h"

#endif // ENABLE(AOT)
