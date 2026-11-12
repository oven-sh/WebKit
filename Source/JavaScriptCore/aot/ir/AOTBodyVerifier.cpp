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

static_assert(BodyBlock::numberOfHeadWords == 6 && verifiedWordsPerParameter == 1 && BodyValue::numberOfHeadWords == 2 && BodyTerminator::numberOfHeadWords == 3 && BodyTarget::numberOfHeadWords == 3);
static_assert(!offsetof(Wire::ValueHead, opcode) && offsetof(Wire::ValueHead, position) == 4);
static_assert(!offsetof(Wire::TerminatorHead, kind) && offsetof(Wire::TerminatorHead, position) == 4 && offsetof(Wire::TerminatorHead, targetCount) == 8);
static_assert(!offsetof(Wire::TargetHead, block) && offsetof(Wire::TargetHead, argumentCount) == 4 && offsetof(Wire::TargetHead, proofCount) == 8);

static_assert(std::ranges::none_of(allMembersOf<TerminatorKind>, [](TerminatorKind kind) { return wireFlagsOf(kind); }), "A terminator has no flags word on the wire.");
static_assert(std::ranges::none_of(allMembersOf<Opcode>, [](Opcode opcode) { return wireFlagsOf(opcode) & ~declaredFlagsOf(opcode); }), "A flag of the wire is a declared flag.");
static_assert(std::ranges::none_of(allMembersOf<Opcode>, [](Opcode opcode) { return wireFlagsOf(opcode) > UINT8_MAX; }));

static constexpr uint8_t wireFlagsInByteOf(Opcode opcode) { return static_cast<uint8_t>(wireFlagsOf(opcode)); }
static constexpr uint8_t mayBeTakenByFewUsesMarkOfOpcode = 1 << 0;
static constexpr uint8_t isFiledInBodyIndexMarkOfOpcode = 1 << 1;
static constexpr uint8_t namesVariableOrLoadsModuleMarkOfOpcode = 1 << 2;
static constexpr bool mayBeTakenByFewUsesInVerifiedBody(Opcode opcode) { return opcodesWithoutResultInVerifiedBodies.contains(opcode) || definitionsThatFewOperandsTakeInVerifiedBodies.contains(opcode); }

static constexpr uint8_t marksOfOpcodeInVerifiedBody(Opcode opcode)
{
    return (mayBeTakenByFewUsesInVerifiedBody(opcode) ? mayBeTakenByFewUsesMarkOfOpcode : 0) | (opcodesFiledInBodyIndex.contains(opcode) ? isFiledInBodyIndexMarkOfOpcode : 0)
        | (namesVariableOrLoadsModule(opcode) ? namesVariableOrLoadsModuleMarkOfOpcode : 0);
}

void (*printerOfBodiesForVerifier)(const Program&, FunctionID, ASCIILiteral heading) = nullptr;

Vector<InternalError> findErrorsInWords(FunctionID id, const Body& body)
{
    BodyVerifierScratch scratch(nullptr);
    BodyVerifier verifier(nullptr, id, nullptr, body, scratch);
    if (verifier.runWords())
        verifier.failWhereOrderMisleadsBodyIndex();
    return verifier.takeErrors();
}

static void printBodyThatVerifierRefused(const Program& program, FunctionID id)
{
    if (!printerOfBodiesForVerifier || id.index() >= program.functions.size())
        return;
    const Function& function = program.at(id);
    BodyVerifierScratch scratch(&program);
    if (BodyVerifier(&program, id, &function, function.body, scratch).runWords())
        printerOfBodiesForVerifier(program, id, "REFUSED BY THE VERIFIER OF BODIES"_s);
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
#if ENABLE(FUZZILLI)
    constexpr bool leavesLibraryCodeOut = true;
#else
    constexpr bool leavesLibraryCodeOut = false;
#endif
    bool comparesIndexes = !tableThatKeepsIndexes || isCheckingBuild || build.validates();
    const ParallelLoops& loops = build.parallelLoops();
    ChunksOfIndices division = divideIntoChunks(static_cast<unsigned>(program.functions.size()));
    Vector<Vector<InternalError>> errorsOfChunks(division.size());
    Vector<Vector<MentionInCode>> mentionsOfChunks(division.size());
    forEachIndexInParallel(loops, division.size(), [&](unsigned chunk, unsigned worker) {
        BodyVerifierScratch scratch(&program);
        for (unsigned index = division.firstIndexOf[chunk]; index < division.firstIndexOf[chunk + 1]; ++index) {
            const Function& function = program.functions[index];
            if (leavesLibraryCodeOut && isLibraryCode(program, function)) {
                scratch.mentionsInCode->noteWhatFunctionNames(function);
                continue;
            }
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
        mentionsOfChunks[chunk] = scratch.mentionsInCode->take();
    });
    Vector<InternalError> errors = joinChunks<InternalError>(loops, WTF::move(errorsOfChunks));
    if (errors.isEmpty())
        return verifyModulesNamedInBodies(program, build, joinChunks<MentionInCode>(loops, WTF::move(mentionsOfChunks)));

    FunctionID firstRefused = errors.first().function;
    build.diagnostics().append(WTF::move(errors));
    printBodyThatVerifierRefused(program, firstRefused);
    return false;
}

BodyVerifierScratch::BodyVerifierScratch(const Program* program)
    : builtins(BuiltinTable::singleton())
{
    numbersOfRecords.sizes.fill(SIZE_MAX);
    if (!program)
        return;
    mentionsInCode.emplace(*program);
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
        if (hasFailed())
            return;
        count = m_id.index();
    } else if (value.index() < m_body.numberOfValues()) {
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

template<WordsReport report>
ALWAYS_INLINE bool BodyVerifier::verifyWordsOfTarget(CheckedBodyWords<report>& words, BlockID from)
{
    uint32_t target = words.takeBlock();
    if (target > from.index() && target < m_body.numberOfBlocks())
        noteTargetOfEarlierBlock(target);
    uint32_t numberOfArguments = words.takeCount();
    uint32_t numberOfProofs = words.takeCount();
    for (; numberOfArguments; --numberOfArguments)
        words.takeOperandThatIsFiledLater();
    words.skip(numberOfProofs);
    return numberOfProofs;
}

template<WordsReport report>
ALWAYS_INLINE void BodyVerifier::verifyWordsOfValue(CheckedBodyWords<report>& words, BlockID block, ValueID value, NoteOnVerifiedValue& note)
{
    words.setSubject(block, value);
    [[maybe_unused]] uint32_t offsetOfHead = words.offset();
    uint32_t opcodeWord = words.take();
    words.skip(1);
    uint32_t number = opcodeWord & BodyValue::opcodeMask;
    if (number >= numberOfEnumerators<Opcode>)
        words.failAtSubject(InternalErrorKind::UnknownEnumerator);
    else if (number >= numberOfWireEnumerators<Opcode> || number == static_cast<uint32_t>(Opcode::Parameter))
        words.failAtSubject(InternalErrorKind::EngineOnlyOnWire);
    else if (opcodeWord & Wire::WrittenPositionIsForCallers && !(opcodeWord & Wire::WrittenPositionFollows))
        words.failAtSubject(InternalErrorKind::FlagNotDeclared);
    if (words.hasFailed())
        return;

    auto opcode = static_cast<Opcode>(number);
    uint8_t marks = CaseTable<Opcode, marksOfOpcodeInVerifiedBody>::at(opcode);
    note = NoteOnVerifiedValue::ofOperation(block, marks & mayBeTakenByFewUsesMarkOfOpcode);
    if constexpr (report == WordsReport::No) {
        m_builderOfIndex->fileOperation(value, offsetOfHead, opcode);
        words.fileOperandsAs(BodyUseKind::Operand, value.index());
    }
    if (uint32_t wireMask = CaseTable<Opcode, wireFlagsInByteOf>::at(opcode)) {
        [[maybe_unused]] uint32_t flags = words.takeFlags(wireMask);
        if constexpr (report == WordsReport::Yes) {
            if (flags & ~static_cast<uint32_t>(declaredFlagsOf(opcode)))
                m_errors.last().kind = InternalErrorKind::UnknownFlag;
        }
    }
    verifyFieldWords(words, opcode);
    if (opcodeWord & Wire::WrittenPositionFollows)
        words.skip(1);
    if constexpr (report == WordsReport::No) {
        if ((marks & (isFiledInBodyIndexMarkOfOpcode | namesVariableOrLoadsModuleMarkOfOpcode) || opcodeWord & Wire::WrittenPositionFollows) && !words.hasFailed()) [[unlikely]] {
            BodyValue operation = BodyValue::operation(&m_body.words()[offsetOfHead], &m_body.unit(), value);
            if (marks & namesVariableOrLoadsModuleMarkOfOpcode)
                m_scratch.mentionsInCode->noteWhatValueNames(operation);
            if (marks & isFiledInBodyIndexMarkOfOpcode || opcodeWord & Wire::WrittenPositionFollows)
                m_builderOfIndex->fileWhatFewOperationsBring(operation, block);
        }
    }
}

template<WordsReport report>
ALWAYS_INLINE void BodyVerifier::verifyWordsOfTerminator(CheckedBodyWords<report>& words, BlockID block, [[maybe_unused]] BodyTarget handler, uint32_t endOfValues)
{
    words.setSubject(block, { });
    uint32_t offsetOfHead = words.offset();
    uint32_t kind = words.template takeEnumerator<TerminatorKind>();
    words.skip(1);
    uint32_t numberOfTargets = words.takeCount(BodyTarget::numberOfHeadWords);
    if (words.hasFailed())
        return;

    words.fileOperandsAs(BodyUseKind::TerminatorOperand, block.index());
    verifyFieldWords(words, static_cast<TerminatorKind>(kind));
    bool someTargetBringsProofs = false;
    for (uint32_t targetIndex = 0; targetIndex < numberOfTargets && !words.hasFailed(); ++targetIndex) {
        [[maybe_unused]] uint32_t offsetOfTarget = words.offset();
        someTargetBringsProofs |= verifyWordsOfTarget(words, block);
        if constexpr (report == WordsReport::No) {
            if (!words.hasFailed()) [[likely]]
                m_builderOfIndex->fileEdge(block, BodyTarget(&m_body.words()[offsetOfTarget]), targetIndex, false);
        }
    }
    if (words.hasFailed())
        return;

    BodyTerminator terminator(&m_body.words()[offsetOfHead], &m_body.unit(), block);
    terminator.forEachOperand([&](ValueID operand, uint32_t) {
        if (operand.index() >= endOfValues)
            noteOrderThatMisleadsBodyIndex(InternalErrorKind::UseNotDominated, block, operand);
    });
    if constexpr (report == WordsReport::No) {
        m_builderOfIndex->fileTerminator(block, offsetOfHead);
        if (handler)
            m_builderOfIndex->fileEdge(block, handler, 0, true);
        if (someTargetBringsProofs && m_kindOfOrderThatMisleadsBodyIndex == InternalErrorKind::Unknown)
            m_builderOfIndex->fileProofs(block, terminator);
    }
}

template<WordsReport report>
ALWAYS_INLINE void BodyVerifier::verifyWordsOfBlock(CheckedBodyWords<report>& words, BlockID block, uint32_t& nextValue)
{
    words.setSubject(block, { });
    if (block != entryBlock && !isTargetOfEarlierBlock(block.index()))
        noteOrderThatMisleadsBodyIndex(InternalErrorKind::BlockOrderInvalid, block);
    uint32_t offsetOfHead = words.offset();
    uint32_t firstParameter = nextValue;
    uint32_t numberOfParameters = words.takeCount(verifiedWordsPerParameter);
    uint32_t numberOfValues = words.takeCount(BodyValue::numberOfHeadWords);
    words.template takeEnumerator<BlockRole>();
    words.fileOperandsAs(BodyUseKind::BlockSubject, block.index());
    words.takeOperand(true);
    bool hasHandler = words.takeBoolean();
    words.template takeEnumerator<BlockFrequency>();
    if (static_cast<uint64_t>(nextValue) + numberOfParameters + numberOfValues > m_body.numberOfValues())
        words.failAtSubject(InternalErrorKind::IdOutOfRange);
    if (words.hasFailed())
        return;
    if constexpr (report == WordsReport::No)
        m_builderOfIndex->fileBlock(block, offsetOfHead, firstParameter, numberOfParameters, numberOfValues);

    NoteOnVerifiedValue* notes = m_scratch.notesOnValues.mutableSpan().data();
    for (; numberOfParameters && !words.hasFailed(); --numberOfParameters) {
        notes[nextValue] = NoteOnVerifiedValue::ofParameter(block);
        words.setSubject(block, ValueID(nextValue++));
        words.template takeID<TypeID>(true);
    }

    words.setSubject(block, { });
    BodyTarget handler;
    if (hasHandler) {
        uint32_t offsetOfHandler = words.offset();
        verifyWordsOfTarget(words, block);
        if (!words.hasFailed())
            handler = BodyTarget(&m_body.words()[offsetOfHandler]);
    }

    for (; numberOfValues && !words.hasFailed(); --numberOfValues) {
        verifyWordsOfValue(words, block, ValueID(nextValue), notes[nextValue]);
        ++nextValue;
    }

    verifyWordsOfTerminator(words, block, handler, nextValue);
}

template<WordsReport report>
bool BodyVerifier::verifyWordsInOrder()
{
    m_kindOfOrderThatMisleadsBodyIndex = InternalErrorKind::Unknown;
    m_scratch.targetsOfEarlierBlocks.fill(0, (static_cast<size_t>(m_body.numberOfBlocks()) + 63) / 64);
    m_scratch.notesOnValues.grow(std::max<size_t>(m_scratch.notesOnValues.size(), m_body.numberOfValues()));

    CheckedBodyWords<report> words(*this, m_body);
    if constexpr (report == WordsReport::No) {
        m_builderOfIndex->begin();
        words.fileOperandsIn(*m_builderOfIndex);
    }
    uint32_t nextValue = 0;
    for (uint32_t block = 0; block < m_body.numberOfBlocks() && !words.hasFailed(); ++block)
        verifyWordsOfBlock(words, BlockID(block), nextValue);

    words.setSubject({ }, { });
    if (!words.isAtEnd() || nextValue != m_body.numberOfValues())
        words.failAtSubject(InternalErrorKind::ListOutOfBounds);
    if constexpr (report == WordsReport::Yes) {
        uint64_t numberOfWords = m_body.words().size();
        if (hasFailed())
            m_errors.first().detail = makeString("after "_s, words.offset(), " of "_s, numberOfWords, " words"_s);
    }
    return !words.hasFailed();
}

void BodyVerifier::verifyWords()
{
    setSubject({ }, { });
    constexpr uint64_t minWordsPerBlock = BodyBlock::numberOfHeadWords + BodyTerminator::numberOfHeadWords;
    static_assert(minWordsPerBlock >= 4, "NoteOnVerifiedValue holds the number of a block beside two marks.");
    uint64_t numberOfWords = m_body.words().size();
    if (m_body.numberOfBlocks() * minWordsPerBlock > numberOfWords || m_body.numberOfValues() > numberOfWords) {
        failAtSubject(InternalErrorKind::ListOutOfBounds);
        return;
    }
    if (!m_body.numberOfBlocks()) {
        if (numberOfWords)
            failAtSubject(InternalErrorKind::ListOutOfBounds);
        return;
    }

    bool wordsAreInOrder = false;
    if (m_function) {
        if (!m_tableThatKeepsIndex)
            m_indexes = makeUniqueWithoutFastMallocCheck<BodyIndexTable>(1u, 1u);
        m_builderOfIndex.emplace(*m_function, m_tableThatKeepsIndex ? *m_tableThatKeepsIndex : *m_indexes, m_tableThatKeepsIndex ? m_workerOfTableThatKeepsIndex : 0);
        m_scratch.mentionsInCode->beginFunction(*m_function);
        wordsAreInOrder = verifyWordsInOrder<WordsReport::No>();
    }
    if (!wordsAreInOrder)
        verifyWordsInOrder<WordsReport::Yes>();
}

void BodyVerifier::buildIndex()
{
    m_index = &m_builderOfIndex->finish();
    if (m_tableThatKeepsIndex)
        m_tableThatKeepsIndex->adopt(m_id, *m_index);
    else
        m_indexes->adopt(FunctionID(0u), *m_index);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
