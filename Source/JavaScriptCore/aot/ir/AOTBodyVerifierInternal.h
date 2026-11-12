/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBitVector.h"
#include "AOTBodyIndex.h"
#include "AOTBodyIndexBuilder.h"
#include "AOTBodyVerifier.h"
#include "AOTBuiltinTable.h"
#include "AOTDiagnostics.h"
#include "AOTIR.h"
#include "AOTProgramVerifier.h"
#include "AOTResultRules.h"
#include <algorithm>
#include <optional>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

class BodyVerifier;

template<typename External> bool isValidExternal(uint32_t word, const BuiltinTable&);

using OperandKindSet = EnumeratorBitSet<OperandKind, numberOfEnumerators<OperandKind>>;

enum class TakenByTargets : uint8_t { Differs, Nothing, Proofs };

enum class WordsReport : bool { No, Yes };

template<WordsReport report>
class CheckedBodyWords {
public:
    CheckedBodyWords(BodyVerifier&, const Body&);

    bool isAtEnd() const { return m_offset == m_numberOfWords; }
    uint32_t offset() const { return static_cast<uint32_t>(m_offset); }

    bool hasFailed() const;
    void setSubject(BlockID, ValueID);
    void failAtSubject(InternalErrorKind);

    uint32_t take();
    void skip(uint32_t numberOfWords);
    uint32_t takeCount(uint32_t minWordsPerElement = 1);

    void fileOperandsIn(BodyIndexBuilder& builder) { m_builder = &builder; }
    void fileOperandsAs(BodyUseKind, uint32_t user);
    void takeOperand(bool isOptional);
    void takeOperandThatIsFiledLater();
    void takeVariadicOperands();
    uint32_t takeBlock();
    template<typename IDType> void takeID(bool isOptional);
    template<typename E> uint32_t takeEnumerator(bool isOptional = false);
    template<typename F> uint32_t takeFlagWord();
    uint32_t takeFlags(uint32_t wireMask);
    bool takeBoolean();
    template<typename External> void takeExternal(bool isOptional);

private:
    bool hasWords(uint64_t count) const { return count <= m_end - m_offset; }
    void checkID(RemappedIDKind, uint32_t word);
    COLD NEVER_INLINE void fail(InternalErrorKind);
    uint32_t failForLackOfWords();

    BodyVerifier& m_verifier;
    const uint32_t* m_words;
    const UnitIDRemap& m_unit;
    const NumbersOfProgramRecords& m_numbersOfRecords;
    const BuiltinTable& m_builtins;
    uint32_t m_numberOfValues;
    uint32_t m_numberOfBlocks;
    size_t m_numberOfWords;
    size_t m_offset { 0 };
    size_t m_end;
    bool m_hasFailed { false };
    BodyIndexBuilder* m_builder { nullptr };
    BodyUseKind m_kindOfUse { BodyUseKind::Operand };
    uint32_t m_user { 0 };
};

class FlowGraphOfVerifiedBody {
    WTF_MAKE_NONCOPYABLE(FlowGraphOfVerifiedBody);
public:
    FlowGraphOfVerifiedBody() = default;

    void build(const BodyIndex&);

    uint32_t numberOfBlocks() const { return static_cast<uint32_t>(m_firstSuccessor.size()) - 1; }
    std::span<const BlockID> successors(BlockID block) const { return listOf(block, m_successors, m_firstSuccessor); }
    std::span<const BlockID> predecessors(BlockID block) const { return listOf(block, m_predecessors, m_firstPredecessor); }
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

class NoteOnVerifiedValue {
public:
    NoteOnVerifiedValue() = default;
    static NoteOnVerifiedValue ofParameter(BlockID block) { return NoteOnVerifiedValue(block.index() << numberOfMarks | isParameterMark); }
    static NoteOnVerifiedValue ofOperation(BlockID block, bool mayBeTakenByFewUses) { return NoteOnVerifiedValue(block.index() << numberOfMarks | (mayBeTakenByFewUses ? mayBeTakenByFewUsesMark : 0)); }

    BlockID block() const { return BlockID(m_bits >> numberOfMarks); }
    bool isParameter() const { return m_bits & isParameterMark; }
    bool mayBeTakenByFewUses() const { return m_bits & mayBeTakenByFewUsesMark; }

private:
    static constexpr uint32_t isParameterMark = 1 << 0;
    static constexpr uint32_t mayBeTakenByFewUsesMark = 1 << 1;
    static constexpr uint32_t numberOfMarks = 2;

    explicit NoteOnVerifiedValue(uint32_t bits)
        : m_bits(bits)
    {
    }

    uint32_t m_bits { 0 };
};

struct BodyVerifierScratch {
    WTF_MAKE_NONCOPYABLE(BodyVerifierScratch);
public:
    explicit BodyVerifierScratch(const Program*);

    NumbersOfProgramRecords numbersOfRecords;
    const BuiltinTable& builtins;

    FlowGraphOfVerifiedBody graph;
    Vector<uint64_t> targetsOfEarlierBlocks;
    Vector<NoteOnVerifiedValue> notesOnValues;
    Vector<ValueID> subjectsOfProofs;
    Vector<uint32_t> numbersOfUses;
    Vector<BodyUser> firstUsers;
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
    std::optional<MentionsInCode> mentionsInCode;
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
    BodyVerifier(const Program*, FunctionID, const Function*, const Body&, BodyVerifierScratch&);
    ~BodyVerifier();

    void keepIndexIn(BodyIndexTable& table, unsigned worker)
    {
        m_tableThatKeepsIndex = &table;
        m_workerOfTableThatKeepsIndex = worker;
    }
    void leaveIndexUncompared() { m_comparesIndexWithPlainComputation = false; }

    bool runWords();
    bool failWhereOrderMisleadsBodyIndex();
    bool run();
    Vector<InternalError> takeErrors();

    COLD NEVER_INLINE void fail(InternalErrorKind, BlockID = { }, ValueID = { }, uint32_t count = 0);
    bool hasFailed() const { return !m_errors.isEmpty(); }
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
    const BodyIndex& index() const { return *m_index; }
    BodyVerifierScratch& scratch() const { return m_scratch; }
    bool isValidField(LayoutID, uint32_t field) const;

private:
    void verifyWords();
    template<WordsReport> bool verifyWordsInOrder();
    template<WordsReport report> void verifyWordsOfBlock(CheckedBodyWords<report>&, BlockID, uint32_t& nextValue);
    template<WordsReport report> void verifyWordsOfValue(CheckedBodyWords<report>&, BlockID, ValueID, NoteOnVerifiedValue&);
    template<WordsReport report> void verifyWordsOfTerminator(CheckedBodyWords<report>&, BlockID, BodyTarget handler, uint32_t endOfValues);
    template<WordsReport report> bool verifyWordsOfTarget(CheckedBodyWords<report>&, BlockID from);
    bool isTargetOfEarlierBlock(uint32_t block) const { return m_scratch.targetsOfEarlierBlocks.span().data()[block / 64] >> (block % 64) & 1; }
    void noteTargetOfEarlierBlock(uint32_t block) { m_scratch.targetsOfEarlierBlocks.mutableSpan().data()[block / 64] |= uint64_t { 1 } << (block % 64); }
    void noteOrderThatMisleadsBodyIndex(InternalErrorKind, BlockID, ValueID = { });
    void buildIndex();

    void verifyFlow();
    const FlowGraphOfVerifiedBody& flowGraph();
    void verifyBlockOrder();
    void verifyDominance();
    void verifyUsesAndOperations();
    void lookAtUsesAndOperations();
    void verifyOperandsStepByStep(const BodyBlock&, const BodyValue&);
    bool givesOneValue(ValueID) const;
    bool isAvailableInOtherBlock(NoteOnVerifiedValue definition, BlockID blockOfUse);
    bool terminatorDominates(BlockID from, BlockID to);
    void verifyUseStepByStep(ValueID used, BodyUseKind, BlockID blockOfUse, ValueID user, uint32_t operandIndex);
    void verifyEdges();
    bool verifyTargetCount(const BodyBlock&, const BodyTerminator&);
    bool verifyEdge(const BodyBlock& from, const BodyTerminator&, TakenByTargets, uint32_t targetIndex, BodyTarget, const BodyBlock& to);
    void verifyHandler(const BodyBlock&);
    void verifyBlockRole(const BodyBlock&);
    void verifyLoopMarks();
    void verifyEntryParameters();
    void verifyIndexAgainstPlainComputation();

    void verifyOperations();
    void verifyOperation(const BodyBlock&, uint32_t indexInBlock, const BodyValue&);
    void verifyOperationByOpcode(const BodyBlock&, uint32_t indexInBlock, const BodyValue&);
    void verifyRuleOfOpcode(const BodyBlock&, uint32_t indexInBlock, const BodyValue&);
    void verifyTerminatorOperation(const BodyBlock&, const BodyTerminator&);
    void verifyOperandDefinitions(const BodyBlock&, const BodyValue&);
    void verifyOperandDefinitionsOneByOne(const BodyBlock&, const BodyValue&);
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
    std::optional<BodyIndexBuilder> m_builderOfIndex;
    Vector<InternalError> m_errors;
    bool m_hasFlowGraph { false };
    bool m_operationsAreVerified { false };
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

inline constexpr OpcodeSet opcodesWithoutResultInVerifiedBodies = opcodesWhere([](Opcode opcode) { return resultRuleOf(opcode) == ResultRule::None; });
inline constexpr OpcodeSet opcodesThatOpenIterationsInVerifiedBodies { Opcode::IteratorOpen, Opcode::ForInOpen };
inline constexpr OpcodeSet definitionsThatMayGiveSeveralResultsInVerifiedBodies = opcodesWhere([](Opcode opcode) {
    return opcodesThatMayHaveSeveralResults.contains(opcode) && (resultRuleOf(opcode) == ResultRule::Results || opcode == Opcode::Results || opcode == Opcode::CallDirect);
});
inline constexpr OpcodeSet definitionsThatFewOperandsTakeInVerifiedBodies = definitionsThatMayGiveSeveralResultsInVerifiedBodies | opcodesThatOpenIterationsInVerifiedBodies;

inline constexpr OpcodeSet opcodesWithRuleOfTheirOwnInVerifiedBodies {
    Opcode::CatchValue, Opcode::CatchException, Opcode::NarrowByTest, Opcode::Convert, Opcode::NewObject, Opcode::NewArray,
    Opcode::FinishObject, Opcode::GetClassPrototype, Opcode::DefineMethod, Opcode::FinishClass, Opcode::NewClass, Opcode::SuperConstruct, Opcode::SuperConstructList,
    Opcode::NewClosure, Opcode::GetCapture, Opcode::IsClosureOf, Opcode::CallDirect, Opcode::Results,
    Opcode::CreateUnmappedArguments, Opcode::CreateMappedArguments, Opcode::GetMappedArgument, Opcode::SetMappedArgument,
    Opcode::PrivateGet, Opcode::PrivateSet, Opcode::PrivateDefine, Opcode::PrivateHas, Opcode::Await, Opcode::Yield, Opcode::InitialYield,
    Opcode::Builtin, Opcode::ModuleBuiltin, Opcode::NativeBinding, Opcode::DirectEval, Opcode::EvaluateModule, Opcode::RecordRequire,
    Opcode::GetEnvironmentField, Opcode::PutEnvironmentField, Opcode::CountExecution
};
inline constexpr TerminatorKindSet terminatorsWithRuleOfTheirOwnInVerifiedBodies { TerminatorKind::Switch, TerminatorKind::SwitchString, TerminatorKind::Return, TerminatorKind::ReturnCall };

void verifyFieldRules(BodyVerifier&, const BodyBlock&, const BodyValue&);
void verifyFieldRules(BodyVerifier&, const BodyBlock&, const BodyTerminator&);

template<WordsReport report>
inline CheckedBodyWords<report>::CheckedBodyWords(BodyVerifier& verifier, const Body& body)
    : m_verifier(verifier)
    , m_words(body.words().data())
    , m_unit(body.unit())
    , m_numbersOfRecords(verifier.scratch().numbersOfRecords)
    , m_builtins(verifier.scratch().builtins)
    , m_numberOfValues(body.numberOfValues())
    , m_numberOfBlocks(body.numberOfBlocks())
    , m_numberOfWords(body.words().size())
    , m_end(m_numberOfWords)
{
}

template<WordsReport report>
ALWAYS_INLINE bool CheckedBodyWords<report>::hasFailed() const
{
    if constexpr (report == WordsReport::Yes)
        return m_verifier.hasFailed();
    else
        return m_hasFailed;
}

template<WordsReport report>
ALWAYS_INLINE void CheckedBodyWords<report>::setSubject(BlockID block, ValueID value)
{
    if constexpr (report == WordsReport::Yes)
        m_verifier.setSubject(block, value);
}

template<WordsReport report>
ALWAYS_INLINE void CheckedBodyWords<report>::failAtSubject(InternalErrorKind kind)
{
    if constexpr (report == WordsReport::Yes)
        m_verifier.failAtSubject(kind);
    else
        m_hasFailed = true;
}

template<WordsReport report>
ALWAYS_INLINE void CheckedBodyWords<report>::fail(InternalErrorKind kind)
{
    failAtSubject(kind);
    m_end = m_offset;
}

template<WordsReport report>
ALWAYS_INLINE uint32_t CheckedBodyWords<report>::failForLackOfWords()
{
    fail(InternalErrorKind::ListOutOfBounds);
    return 0;
}

template<WordsReport report>
ALWAYS_INLINE uint32_t CheckedBodyWords<report>::take()
{
    if (m_offset == m_end) [[unlikely]]
        return failForLackOfWords();
    return m_words[m_offset++];
}

template<WordsReport report>
ALWAYS_INLINE void CheckedBodyWords<report>::skip(uint32_t numberOfWords)
{
    if (!hasWords(numberOfWords)) [[unlikely]] {
        failForLackOfWords();
        return;
    }
    m_offset += numberOfWords;
}

template<WordsReport report>
ALWAYS_INLINE uint32_t CheckedBodyWords<report>::takeCount(uint32_t minWordsPerElement)
{
    uint32_t count = take();
    if (hasWords(static_cast<uint64_t>(count) * minWordsPerElement)) [[likely]]
        return count;
    return failForLackOfWords();
}

template<WordsReport report>
ALWAYS_INLINE void CheckedBodyWords<report>::fileOperandsAs(BodyUseKind kind, uint32_t user)
{
    if constexpr (report == WordsReport::No) {
        m_kindOfUse = kind;
        m_user = user;
    }
}

template<WordsReport report>
ALWAYS_INLINE void CheckedBodyWords<report>::takeOperand(bool isOptional)
{
    uint32_t word = take();
    if (word < m_numberOfValues) [[likely]] {
        if constexpr (report == WordsReport::No)
            m_builder->fileUse(ValueID(word), m_kindOfUse, m_user);
        return;
    }
    if (word != Wire::none)
        fail(InternalErrorKind::IdOutOfRange);
    else if (!isOptional)
        fail(InternalErrorKind::OperandMissing);
}

template<WordsReport report>
ALWAYS_INLINE void CheckedBodyWords<report>::takeOperandThatIsFiledLater()
{
    uint32_t word = take();
    if (word < m_numberOfValues) [[likely]]
        return;
    fail(word != Wire::none ? InternalErrorKind::IdOutOfRange : InternalErrorKind::OperandMissing);
}

template<WordsReport report>
ALWAYS_INLINE void CheckedBodyWords<report>::takeVariadicOperands()
{
    for (uint32_t remaining = takeCount(); remaining; --remaining)
        takeOperand(false);
}

template<WordsReport report>
ALWAYS_INLINE uint32_t CheckedBodyWords<report>::takeBlock()
{
    uint32_t word = take();
    if (word >= m_numberOfBlocks) [[unlikely]]
        fail(InternalErrorKind::IdOutOfRange);
    return word;
}

template<WordsReport report>
ALWAYS_INLINE uint32_t CheckedBodyWords<report>::takeFlags(uint32_t wireMask)
{
    uint32_t word = take();
    if (word & ~wireMask) [[unlikely]]
        fail(InternalErrorKind::EngineOnlyOnWire);
    return word;
}

template<WordsReport report>
ALWAYS_INLINE bool CheckedBodyWords<report>::takeBoolean()
{
    uint32_t word = take();
    if (word > 1) [[unlikely]]
        fail(InternalErrorKind::UnknownEnumerator);
    return word == 1;
}

template<WordsReport report>
ALWAYS_INLINE void CheckedBodyWords<report>::checkID(RemappedIDKind kind, uint32_t word)
{
    const IDRemapTable& table = m_unit.tables[static_cast<size_t>(kind)];
    if (word >= table.size || static_cast<size_t>(table.entries[word]) - 1 >= m_numbersOfRecords[kind]) [[unlikely]]
        fail(InternalErrorKind::IdOutOfRange);
}

template<WordsReport report>
template<typename IDType>
ALWAYS_INLINE void CheckedBodyWords<report>::takeID(bool isOptional)
{
    uint32_t word = take();
    if (isOptional && word == Wire::none)
        return;
    checkID(remappedIDKindOf<IDType>, word);
}

template<WordsReport report>
template<typename E>
ALWAYS_INLINE uint32_t CheckedBodyWords<report>::takeEnumerator(bool isOptional)
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

template<WordsReport report>
template<typename F>
ALWAYS_INLINE uint32_t CheckedBodyWords<report>::takeFlagWord()
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

template<WordsReport report>
template<typename External>
ALWAYS_INLINE void CheckedBodyWords<report>::takeExternal(bool isOptional)
{
    uint32_t word = take();
    if (isOptional && word == Wire::none)
        return;
    if (!isValidExternal<External>(word, m_builtins)) [[unlikely]]
        fail(InternalErrorKind::IdOutOfRange);
}

} } } // namespace JSC::AOT::Typed

#include "AOTSchemaBodyVerifier.h"

namespace JSC { namespace AOT { namespace Typed {

inline constexpr OperandKindSet operandKindsThatFewDefinitionsFitInVerifiedBodies { OperandKind::Results, OperandKind::IteratorState, OperandKind::Enumerator };
static_assert(std::ranges::none_of(allMembersOf<OperandKind>, [](OperandKind kind) { return mustBeDefinition(kind) && !operandKindsThatFewDefinitionsFitInVerifiedBodies.contains(kind); }));

inline constexpr OpcodeSet opcodesThatTakeAnyValueInVerifiedBodies = opcodesWhere([](Opcode opcode) { return !kindsOfOperandsOf(opcode).intersects(operandKindsThatFewDefinitionsFitInVerifiedBodies); });

template<typename Holds>
constexpr TerminatorKindSet terminatorsInVerifiedBodiesWhere(const Holds& holds)
{
    TerminatorKindSet kinds;
    for (TerminatorKind kind : allMembersOf<TerminatorKind>) {
        if (holds(kind))
            kinds.add(kind);
    }
    return kinds;
}

inline constexpr TerminatorKindSet terminatorsThatTakeAnyValueInVerifiedBodies = terminatorsInVerifiedBodiesWhere([](TerminatorKind kind) { return !kindsOfOperandsOf(kind).intersects(operandKindsThatFewDefinitionsFitInVerifiedBodies); });
inline constexpr TerminatorKindSet terminatorsAskedOnlyForTheirUsesInVerifiedBodies = terminatorsInVerifiedBodiesWhere([](TerminatorKind kind) {
    return !hasFieldRulesInEveryFunction(kind) && !isIntrinsicOf(kind) && !terminatorsWithRuleOfTheirOwnInVerifiedBodies.contains(kind);
});

struct QuestionsOfVerifiedOperation {
    static constexpr uint8_t kindsOfOperands = 1 << 0;
    static constexpr uint8_t fieldRules = 1 << 1;
    static constexpr uint8_t libraryCode = 1 << 2;
    static constexpr uint8_t convertedAllocation = 1 << 3;
    static constexpr uint8_t ruleOfOpcode = 1 << 4;
};

constexpr uint8_t questionsOfVerifiedOperation(Opcode opcode)
{
    uint8_t questions = 0;
    if (!opcodesThatTakeAnyValueInVerifiedBodies.contains(opcode))
        questions |= QuestionsOfVerifiedOperation::kindsOfOperands;
    if (hasFieldRulesInEveryFunction(opcode))
        questions |= QuestionsOfVerifiedOperation::fieldRules;
    if (isIntrinsicOf(opcode))
        questions |= QuestionsOfVerifiedOperation::libraryCode;
    if (opcode == Opcode::Convert)
        questions |= QuestionsOfVerifiedOperation::convertedAllocation;
    else if (opcodesWithRuleOfTheirOwnInVerifiedBodies.contains(opcode))
        questions |= QuestionsOfVerifiedOperation::ruleOfOpcode;
    return questions;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
