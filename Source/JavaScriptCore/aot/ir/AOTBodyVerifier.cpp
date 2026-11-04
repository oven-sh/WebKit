/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBodyVerifier.h"

#if ENABLE(AOT)

#include "AOTBodyVerifierInternal.h"
#include "AOTBuildContext.h"
#include "AOTCurrentWork.h"
#include "AOTProgramVerifier.h"
#include "AOTThreads.h"
#include <algorithm>
#include <cstddef>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t verifiedWordsPerParameter = sizeof(Wire::ParameterHead) / sizeof(uint32_t);

// The words stage takes the words of each head one by one, in this order.
static_assert(BodyBlock::numberOfHeadWords == 5 && verifiedWordsPerParameter == 1 && BodyValue::numberOfHeadWords == 2 && BodyTerminator::numberOfHeadWords == 3 && BodyTarget::numberOfHeadWords == 3);
static_assert(!offsetof(Wire::ValueHead, opcode) && offsetof(Wire::ValueHead, position) == 4);
static_assert(!offsetof(Wire::TerminatorHead, kind) && offsetof(Wire::TerminatorHead, position) == 4 && offsetof(Wire::TerminatorHead, targetCount) == 8);
static_assert(!offsetof(Wire::TargetHead, block) && offsetof(Wire::TargetHead, argumentCount) == 4 && offsetof(Wire::TargetHead, proofCount) == 8);

static_assert(std::ranges::none_of(allMembersOf<TerminatorKind>, [](TerminatorKind kind) { return wireFlagsOf(kind); }), "A terminator has no flags word on the wire.");
static_assert(std::ranges::none_of(allMembersOf<Opcode>, [](Opcode opcode) { return wireFlagsOf(opcode) & ~declaredFlagsOf(opcode); }), "A flag of the wire is a declared flag.");
static_assert(std::ranges::none_of(allMembersOf<Opcode>, [](Opcode opcode) { return wireFlagsOf(opcode) > UINT8_MAX; }));

static constexpr uint8_t wireFlagsInByteOf(Opcode opcode) { return static_cast<uint8_t>(wireFlagsOf(opcode)); }

void (*printerOfBodiesForVerifier)(const Program&, FunctionID, ASCIILiteral heading) = nullptr;

// Whoever finds nothing here may build an index of the body.
Vector<InternalError> findErrorsInWords(FunctionID id, const Body& body)
{
    BodyVerifierScratch scratch(nullptr);
    BodyVerifier verifier(nullptr, id, nullptr, body, scratch);
    if (verifier.runWords())
        verifier.failWhereOrderMisleadsBodyIndex();
    return verifier.takeErrors();
}

Vector<InternalError> findErrorsInBody(const Program& program, FunctionID id, const Function& function)
{
    CurrentWorkScope work(program, id, "verify"_s);
    BodyVerifierScratch scratch(&program);
    BodyVerifier verifier(&program, id, &function, function.body, scratch);
    verifier.run();
    return verifier.takeErrors();
}

static void printBodyThatVerifierRefused(const Program& program, FunctionID id)
{
    if (!printerOfBodiesForVerifier || id.index() >= program.functions.size())
        return;
    // A printer reads through cursors, and those are safe only behind the words stage.
    const Function& function = program.at(id);
    BodyVerifierScratch scratch(&program);
    if (BodyVerifier(&program, id, &function, function.body, scratch).runWords())
        printerOfBodiesForVerifier(program, id, "REFUSED BY THE VERIFIER OF BODIES"_s);
}

bool verifyBody(const Program& program, FunctionID id, const Function& function, BuildContext& build)
{
    Vector<InternalError> errors = findErrorsInBody(program, id, function);
    if (errors.isEmpty())
        return true;
    build.diagnostics().add(errors.first());
    printBodyThatVerifierRefused(program, id);
    return false;
}

bool verifyBodies(const Program& program, BuildContext& build)
{
    return verifyBodies(program, build, nullptr);
}

bool verifyBodies(const Program& program, BuildContext& build, BodyIndexTable* tableThatKeepsIndexes)
{
#if ASSERT_ENABLED || ASAN_ENABLED
    constexpr bool isCheckingBuild = true;
#else
    constexpr bool isCheckingBuild = false;
#endif
    bool comparesIndexes = !tableThatKeepsIndexes || isCheckingBuild || build.validates();
    // As collectInParallel() has it, with the number of the thread.
    const ParallelLoops& loops = build.parallelLoops();
    ChunksOfIndices division = divideIntoChunks(static_cast<unsigned>(program.functions.size()));
    Vector<Vector<InternalError>> errorsOfChunks(division.size());
    forEachIndexInParallel(loops, division.size(), [&](unsigned chunk, unsigned worker) {
        BodyVerifierScratch scratch(&program);
        for (unsigned index = division.firstIndexOf[chunk]; index < division.firstIndexOf[chunk + 1]; ++index) {
            const Function& function = program.functions[index];
#if ENABLE(FUZZILLI)
            // It is the same in every build, and a fuzzer makes millions of builds.
            if (isLibraryCode(program, function))
                continue;
#endif
            FunctionID id(index);
            CurrentWorkScope work(program, id, "verify"_s);
            BodyVerifier verifier(&program, id, &function, function.body, scratch);
            if (tableThatKeepsIndexes)
                verifier.keepIndexIn(*tableThatKeepsIndexes, worker);
            if (!comparesIndexes)
                verifier.leaveIndexUncompared();
            if (!verifier.run())
                errorsOfChunks[chunk].append(WTF::move(verifier.takeErrors().first()));
        }
    });
    Vector<InternalError> errors = joinChunks<InternalError>(loops, WTF::move(errorsOfChunks));
    if (errors.isEmpty())
        return verifyModulesNamedInBodies(program, build);

    FunctionID firstRefused = errors.first().function;
    build.diagnostics().append(WTF::move(errors));
    printBodyThatVerifierRefused(program, firstRefused);
    return false;
}

uint32_t CheckedBodyWords::failForLackOfWords()
{
    fail(InternalErrorKind::ListOutOfBounds);
    return 0;
}

BodyVerifierScratch::BodyVerifierScratch(const Program* program)
    : opcodesThatTakeAnyValue(opcodesThatTakeAnyValueInVerifiedBodies())
{
    numbersOfRecords.sizes.fill(SIZE_MAX);
    if (!program)
        return;
    numbersOfRecords[RemappedIDKind::String] = program->strings.size();
    numbersOfRecords[RemappedIDKind::PropertyKey] = program->types.numberOfPropertyKeys();
    numbersOfRecords[RemappedIDKind::TypeSite] = program->typeSites.size();
    numbersOfRecords[RemappedIDKind::ExpectedResult] = program->expectedResults.size();
    numbersOfRecords[RemappedIDKind::Layout] = program->layouts.size();
    numbersOfRecords[RemappedIDKind::Variable] = program->variables.size();
    numbersOfRecords[RemappedIDKind::Module] = program->modules.size();
    numbersOfRecords[RemappedIDKind::ModuleFailure] = program->moduleFailures.size();
    numbersOfRecords[RemappedIDKind::EvalScope] = program->evalScopes.size();
    numbersOfRecords[RemappedIDKind::Function] = program->functions.size();
    numbersOfRecords[RemappedIDKind::Data] = program->data.size();
    static_assert(numberOfRemappedIDKinds == 12, "Every kind but RemappedIDKind::Type has a number above: the bits of a TypeID are no index.");
}

BodyVerifier::BodyVerifier(const Program* program, FunctionID id, const Function* function, const Body& body, BodyVerifierScratch& scratch)
    : m_program(program)
    , m_id(id)
    , m_function(function)
    , m_body(body)
    , m_scratch(scratch)
{
    RELEASE_ASSERT(!program == !function);
    // The index is built of the function's body, which has to be the one whose words were checked.
    RELEASE_ASSERT(!function || function->body.words().data() == body.words().data());
}

BodyVerifier::~BodyVerifier() = default;

bool BodyVerifier::runWords()
{
    RELEASE_ASSERT(!hasFailed() && !m_index);
    verifyWords();
    return !hasFailed();
}

bool BodyVerifier::run()
{
    RELEASE_ASSERT(m_program && m_function);
    if (!runWords())
        return false;
    if (!m_body)
        return true;

    if (!failWhereOrderMisleadsBodyIndex())
        return false;

    buildIndex();
    verifyFlow();
    if (!hasFailed())
        verifyOperations();
    return !hasFailed();
}

void BodyVerifier::noteOrderThatMisleadsBodyIndex(InternalErrorKind kind, BlockID block, ValueID value)
{
    if (m_kindOfOrderThatMisleadsBodyIndex != InternalErrorKind::Unknown)
        return;
    m_kindOfOrderThatMisleadsBodyIndex = kind;
    m_blockOfOrderThatMisleadsBodyIndex = block;
    m_valueOfOrderThatMisleadsBodyIndex = value;
}

bool BodyVerifier::failWhereOrderMisleadsBodyIndex()
{
    RELEASE_ASSERT(!hasFailed() && !m_index);
    if (m_kindOfOrderThatMisleadsBodyIndex == InternalErrorKind::Unknown)
        return true;
    fail(m_kindOfOrderThatMisleadsBodyIndex, m_blockOfOrderThatMisleadsBodyIndex, m_valueOfOrderThatMisleadsBodyIndex);
    return false;
}

Vector<InternalError> BodyVerifier::takeErrors()
{
    return std::exchange(m_errors, { });
}

void BodyVerifier::fail(InternalErrorKind kind, BlockID block, ValueID value, uint32_t count)
{
    SourceLocation location = noSourceLocation;
    ASCIILiteral operation;
    if (!m_index) {
        // What the words stage takes behind its first failure is zero and means nothing.
        if (hasFailed())
            return;
        // Its messages count records, and the record of a body is its function. failWhereOrderMisleadsBodyIndex() fails here too, and its two messages show no count.
        count = m_id.index();
    } else if (value.index() < m_body.numberOfValues()) {
        // Diagnostics looks the operation up only for a record that comes without a location.
        BodyValue subject = m_index->value(value);
        location = m_function->locationOf(subject);
        operation = nameOf(subject.opcode());
    }

    m_errors.append(InternalError {
        .kind = kind,
        .function = m_id,
        .block = block,
        .value = value,
        .count = count,
        .location = location,
        .operation = operation,
    });
}

bool BodyVerifier::isValidField(LayoutID layout, uint32_t field) const
{
    return layout.index() < m_program->layouts.size() && field < m_program->at(layout).fields.size();
}

void BodyVerifier::verifyWords()
{
    setSubject({ }, { });
    constexpr uint64_t minWordsPerBlock = BodyBlock::numberOfHeadWords + BodyTerminator::numberOfHeadWords;
    uint64_t numberOfWords = m_body.words().size();
    if (m_body.numberOfBlocks() * minWordsPerBlock > numberOfWords || m_body.numberOfValues() > numberOfWords) {
        failAtSubject(InternalErrorKind::ListOutOfBounds);
        return;
    }
    // A body without blocks need not have a unit.
    if (!m_body.numberOfBlocks()) {
        if (numberOfWords)
            failAtSubject(InternalErrorKind::ListOutOfBounds);
        return;
    }

    m_isTargetOfEarlierBlock = BitVector(m_body.numberOfBlocks());

    CheckedBodyWords words(*this, m_body);
    uint32_t nextValue = 0;
    for (uint32_t block = 0; block < m_body.numberOfBlocks() && !hasFailed(); ++block)
        verifyWordsOfBlock(words, BlockID(block), nextValue);

    setSubject({ }, { });
    if (!words.isAtEnd() || nextValue != m_body.numberOfValues())
        failAtSubject(InternalErrorKind::ListOutOfBounds);
    if (hasFailed())
        m_errors.first().detail = makeString("after "_s, words.offset(), " of "_s, numberOfWords, " words"_s);
}

void BodyVerifier::verifyWordsOfBlock(CheckedBodyWords& words, BlockID block, uint32_t& nextValue)
{
    setSubject(block, { });
    if (block != entryBlock && !m_isTargetOfEarlierBlock.quickGet(block.index()))
        noteOrderThatMisleadsBodyIndex(InternalErrorKind::BlockOrderInvalid, block);
    uint32_t numberOfParameters = words.takeCount(verifiedWordsPerParameter);
    uint32_t numberOfValues = words.takeCount(BodyValue::numberOfHeadWords);
    m_hasHandlerBlock |= words.takeEnumerator<BlockRole>() != static_cast<uint32_t>(BlockRole::Ordinary);
    words.takeOperand(true);
    bool hasHandler = words.takeBoolean();
    if (static_cast<uint64_t>(nextValue) + numberOfParameters + numberOfValues > m_body.numberOfValues())
        failAtSubject(InternalErrorKind::IdOutOfRange);
    if (hasFailed())
        return;

    for (; numberOfParameters && !hasFailed(); --numberOfParameters) {
        setSubject(block, ValueID(nextValue++));
        words.takeID<TypeID>(true);
    }

    setSubject(block, { });
    if (hasHandler)
        verifyWordsOfTarget(words, block);

    for (; numberOfValues && !hasFailed(); --numberOfValues)
        verifyWordsOfValue(words, block, ValueID(nextValue++));

    verifyWordsOfTerminator(words, block, nextValue);
}

void BodyVerifier::verifyWordsOfValue(CheckedBodyWords& words, BlockID block, ValueID value)
{
    setSubject(block, value);
    uint32_t opcodeWord = words.take();
    words.skip(1);
    uint32_t number = opcodeWord & BodyValue::opcodeMask;
    if (number >= numberOfEnumerators<Opcode>)
        failAtSubject(InternalErrorKind::UnknownEnumerator);
    else if (number >= numberOfWireEnumerators<Opcode> || number == static_cast<uint32_t>(Opcode::Parameter))
        failAtSubject(InternalErrorKind::EngineOnlyOnWire);
    else if (opcodeWord & Wire::WrittenPositionIsForCallers && !(opcodeWord & Wire::WrittenPositionFollows))
        failAtSubject(InternalErrorKind::FlagNotDeclared);
    if (hasFailed())
        return;

    auto opcode = static_cast<Opcode>(number);
    if (uint32_t wireMask = CaseTable<Opcode, wireFlagsInByteOf>::at(opcode)) {
        // A bit that the entry does not declare lies outside the wire's mask too, so the take has just reported it as the engine's.
        if (words.takeFlags(wireMask) & ~static_cast<uint32_t>(declaredFlagsOf(opcode)))
            m_errors.last().kind = InternalErrorKind::UnknownFlag;
    }
    verifyFieldWords(words, opcode);
    if (opcodeWord & Wire::WrittenPositionFollows)
        words.skip(1);
}

void BodyVerifier::verifyWordsOfTerminator(CheckedBodyWords& words, BlockID block, uint32_t endOfValues)
{
    setSubject(block, { });
    uint32_t offsetOfHead = words.offset();
    uint32_t kind = words.takeEnumerator<TerminatorKind>();
    words.skip(1);
    uint32_t numberOfTargets = words.takeCount(BodyTarget::numberOfHeadWords);
    if (hasFailed())
        return;

    verifyFieldWords(words, static_cast<TerminatorKind>(kind));
    for (; numberOfTargets && !hasFailed(); --numberOfTargets)
        verifyWordsOfTarget(words, block);
    if (hasFailed())
        return;

    // Its words have just been checked, so a cursor may read them.
    BodyTerminator(&m_body.words()[offsetOfHead], &m_body.unit(), block).forEachOperand([&](ValueID operand, uint32_t) {
        if (operand.index() >= endOfValues)
            noteOrderThatMisleadsBodyIndex(InternalErrorKind::UseNotDominated, block, operand);
    });
}

void BodyVerifier::verifyWordsOfTarget(CheckedBodyWords& words, BlockID from)
{
    uint32_t target = words.takeBlock();
    if (target > from.index() && target < m_body.numberOfBlocks())
        m_isTargetOfEarlierBlock.quickSet(target);
    else
        m_hasEdgeThatDoesNotLeadForward = true;
    uint32_t numberOfArguments = words.takeCount();
    uint32_t numberOfProofs = words.takeCount();
    for (; numberOfArguments; --numberOfArguments)
        words.takeOperand(false);
    // What a proof word may say depends on the terminator: the flow stage asks BodyIndex::edgeResult().
    words.skip(numberOfProofs);
}

void BodyVerifier::buildIndex()
{
    if (m_tableThatKeepsIndex) {
        m_index = &m_tableThatKeepsIndex->build(m_id, *m_function, m_workerOfTableThatKeepsIndex);
        return;
    }
    // The table holds this function alone, which has the first place in it whatever its number in the program.
    m_indexes = makeUniqueWithoutFastMallocCheck<BodyIndexTable>(1u, 1u);
    m_index = &m_indexes->build(FunctionID(0u), *m_function, 0);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
