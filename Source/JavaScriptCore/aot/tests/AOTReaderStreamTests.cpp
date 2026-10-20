/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTReaderTestSupport.h"

namespace JSC { namespace AOT {

namespace {

using namespace ReaderTests;

size_t wordAfter(size_t head, unsigned numberOfWords) { return head + numberOfWords * wordSize; }

size_t codeStartOf(const ReaderFixture& fixture, FunctionID function)
{
    return fixture.offsetOf<uint32_t>(List::Code, fixture.spanAt(functionOffset(fixture, function, offsetof(Wire::Function, body.code))).start);
}

namespace CallWord {
constexpr unsigned flags = 2;
constexpr unsigned callee = 3;
constexpr unsigned thisValue = 4;
constexpr unsigned argumentCount = 5;
constexpr unsigned firstArgument = 6;
constexpr unsigned calleeKeyAfterTwoArguments = 8;
constexpr unsigned calleeNameAfterTwoArguments = 9;
constexpr unsigned expectedResultAfterTwoArguments = 10;
}

void testStreamHeads()
{
    static_assert(numberOfWireEnumerators<Opcode> < numberOfEnumerators<Opcode>);
    static_assert(!isWire(Opcode::Parameter) && !isWire(Opcode::Unspecified));
    ReaderFixture fixture;
    size_t integer = fixture.offsetOfHeadAt(f7, 1002);
    expectEach(fixture, {
        { "opcode past the table"_s, integer, numberOfEnumerators<Opcode>, Kind::UnknownEnumerator },
        { "huge opcode"_s, integer, Wire::none, Kind::UnknownEnumerator },
        { "opcode of the engine"_s, integer, numberOfWireEnumerators<Opcode>, Kind::EngineOnlyOnWire },
        { "parameter as a value"_s, integer, static_cast<uint32_t>(Opcode::Parameter), Kind::EngineOnlyOnWire },
        { "opcode with fewer words"_s, integer, static_cast<uint32_t>(Opcode::Undefined), Kind::UnknownEnumerator },
        { "opcode of a later form"_s, fixture.offsetOfHeadAt(f7, 1011), static_cast<uint32_t>(Opcode::Unspecified), Kind::EngineOnlyOnWire },
    });

    size_t call = fixture.offsetOfHeadAt(f7, 1004);
    constexpr uint32_t declaredFlags = declaredFlagsOf(Opcode::Call);
    constexpr uint32_t wireFlags = wireFlagsOf(Opcode::Call);
    constexpr uint32_t engineFlags = declaredFlags & ~wireFlags;
    static_assert(wireFlags != 0 && engineFlags != 0 && !(wireFlags & ~declaredFlags) && !(declaredFlags & (declaredFlags + 1)));
    AOT_TEST_CHECK(fixture.word(wordAfter(call, CallWord::flags)) && !(fixture.word(wordAfter(call, CallWord::flags)) & ~wireFlags));
    expectEach(fixture, {
        { "undeclared flag"_s, wordAfter(call, CallWord::flags), declaredFlags + 1, Kind::UnknownFlag },
        { "high flag"_s, wordAfter(call, CallWord::flags), highBit, Kind::UnknownFlag },
        { "flag of the engine"_s, wordAfter(call, CallWord::flags), engineFlags, Kind::EngineOnlyOnWire },
        { "flag of the engine beside a flag of the wire"_s, wordAfter(call, CallWord::flags), declaredFlags, Kind::EngineOnlyOnWire },
    });
}

void testStreamOperands()
{
    ReaderFixture fixture;
    uint32_t numberOfValues = fixture.word(functionOffset(fixture, f7, offsetof(Wire::Function, body.valueCount)));
    size_t call = fixture.offsetOfHeadAt(f7, 1004);
    AOT_TEST_CHECK(fixture.word(wordAfter(call, CallWord::thisValue)) == Wire::none && fixture.word(wordAfter(call, CallWord::argumentCount)) == 2);
    expectEach(fixture, {
        { "operand past the end"_s, wordAfter(call, CallWord::callee), numberOfValues, Kind::IdOutOfRange },
        { "required operand absent"_s, wordAfter(call, CallWord::callee), Wire::none, Kind::OperandMissing },
        { "optional operand past the end"_s, wordAfter(call, CallWord::thisValue), numberOfValues, Kind::IdOutOfRange },
        { "huge number of operands"_s, wordAfter(call, CallWord::argumentCount), Wire::none, Kind::ListOutOfBounds },
        { "variadic operand past the end"_s, wordAfter(call, CallWord::firstArgument), numberOfValues, Kind::IdOutOfRange },
        { "variadic operand absent"_s, wordAfter(call, CallWord::firstArgument), Wire::none, Kind::IdOutOfRange },
        { "operand defined later"_s, wordAfter(call, CallWord::firstArgument), 9, Kind::UseNotDominated },
        { "operand that is the value itself"_s, wordAfter(call, CallWord::firstArgument), 4, Kind::UseNotDominated },
    });
}

void testStreamPayloads()
{
    ReaderFixture fixture;
    size_t call = fixture.offsetOfHeadAt(f7, 1004);
    size_t newObject = fixture.offsetOfHeadAt(f7, 1006);
    size_t parameterType = codeStartOf(fixture, f7) + sizeof(Wire::BlockHead);
    expectEach(fixture, {
        { "Call.calleeKey"_s, wordAfter(call, CallWord::calleeKeyAfterTwoArguments), fixture.countOf(List::PropertyKeys), Kind::IdOutOfRange },
        { "Call.calleeName"_s, wordAfter(call, CallWord::calleeNameAfterTwoArguments), fixture.countOf(List::Strings), Kind::IdOutOfRange },
        { "Call.expectedResult"_s, wordAfter(call, CallWord::expectedResultAfterTwoArguments), fixture.countOf(List::ExpectedResults), Kind::IdOutOfRange },
        { "NewObject.layout"_s, wordAfter(newObject, 2), numberOfLayoutIDs(fixture), Kind::IdOutOfRange },
        { "NewObject.fields"_s, wordAfter(newObject, 3), Wire::none, Kind::ListOutOfBounds },
        { "field past the layout"_s, wordAfter(newObject, 4), 1, Kind::PayloadOutOfRange },
        { "SuperSet.mode"_s, wordAfter(fixture.offsetOfHeadAt(f7, 1007), 2), numberOfEnumerators<ECMAMode>, Kind::UnknownEnumerator },
        { "SuperSet with both keys"_s, wordAfter(fixture.offsetOfHeadAt(f7, 1007), 7), 0, Kind::NotExactlyOneParameter },
        { "NewTemplateObject.templateStrings"_s, wordAfter(fixture.offsetOfHeadAt(f7, 1008), 2), Wire::none, Kind::ListOutOfBounds },
        { "TemplateString.raw"_s, wordAfter(fixture.offsetOfHeadAt(f7, 1008), 4), Wire::none, Kind::IdOutOfRange },
        { "PropGet.key"_s, wordAfter(fixture.offsetOfHeadAt(f7, 1009), 4), Wire::none, Kind::IdOutOfRange },
        { "Predict.predicted"_s, wordAfter(fixture.offsetOfHeadAt(f7, 1010), 3), fixture.countOf(List::Types), Kind::IdOutOfRange },
        { "Builtin.builtin"_s, wordAfter(fixture.offsetOfHeadAt(f7, 1011), 2), invalidExternal, Kind::IdOutOfRange },
        { "TypeTest.testedType"_s, wordAfter(fixture.offsetOfHeadAt(f7, 1013), 3), fixture.countOf(List::Types), Kind::IdOutOfRange },
        { "Data.data"_s, wordAfter(fixture.offsetOfHeadAt(f7, 1014), 2), fixture.countOf(List::Data), Kind::IdOutOfRange },
        { "NewRegExp.pattern"_s, wordAfter(fixture.offsetOfHeadAt(f7, 1016), 2), fixture.countOf(List::Strings), Kind::IdOutOfRange },
        { "NewRegExp.regExpFlags"_s, wordAfter(fixture.offsetOfHeadAt(f7, 1016), 3), 1u << numberOfEnumerators<RegExpFlags>, Kind::UnknownFlag },
        { "ParameterHead.type"_s, parameterType, fixture.countOf(List::Types), Kind::IdOutOfRange },
    });
}

constexpr ASCIILiteral scopeReferencesText = R"IR(module m0 path="/a.ts" displayPath="a.ts"
modulefailure q0 importer=m0 specifier="./q"
evalscope e0
function f0 name="f" module=m0 parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = DirectEval v0, v0 evalScope=e0 @10
    v2 = ThrowModuleFailure failure=q0 @11
    Unreachable @12
}
entry module=m0
)IR"_s;

void testStreamScopeReferences()
{
    ReaderFixture fixture(scopeReferencesText);
    constexpr FunctionID f0 { 0u };
    expectAccepted("scope references"_s, readBytes(fixture.bytes.span()));
    expectEach(fixture, {
        { "DirectEval.evalScope"_s, wordAfter(fixture.offsetOfHeadAt(f0, 10), 2), fixture.countOf(List::EvalScopes), Kind::IdOutOfRange },
        { "DirectEval.evalScope absent"_s, wordAfter(fixture.offsetOfHeadAt(f0, 10), 2), Wire::none, Kind::IdOutOfRange },
        { "ThrowModuleFailure.failure"_s, wordAfter(fixture.offsetOfHeadAt(f0, 11), 2), fixture.countOf(List::ModuleFailures), Kind::IdOutOfRange },
        { "ThrowModuleFailure.failure absent"_s, wordAfter(fixture.offsetOfHeadAt(f0, 11), 2), Wire::none, Kind::IdOutOfRange },
    });
}

namespace BranchWord {
constexpr unsigned targetCount = 2;
constexpr unsigned condition = 3;
constexpr unsigned firstTarget = 4;
constexpr unsigned secondTargetAfterEmptyTarget = 7;
}

constexpr unsigned wordsInTargetHead = sizeof(Wire::TargetHead) / wordSize;

void testStreamTerminators()
{
    static_assert(!isWire(TerminatorKind::TestAndBranch));
    ReaderFixture fixture;
    size_t branch = fixture.offsetOfHeadAt(f7, 2000);
    size_t firstBlock = wordAfter(branch, BranchWord::firstTarget) + offsetof(Wire::TargetHead, block);
    size_t secondBlock = wordAfter(branch, BranchWord::secondTargetAfterEmptyTarget) + offsetof(Wire::TargetHead, block);
    uint32_t numberOfBlocks = fixture.word(functionOffset(fixture, f7, offsetof(Wire::Function, body.blockCount)));
    AOT_TEST_CHECK(fixture.word(firstBlock) == 1 && fixture.word(secondBlock) == 2 && numberOfBlocks == 4);
    expectEach(fixture, {
        { "terminator kind"_s, branch, numberOfEnumerators<TerminatorKind>, Kind::UnknownEnumerator },
        { "terminator of the engine"_s, branch, static_cast<uint32_t>(TerminatorKind::TestAndBranch), Kind::EngineOnlyOnWire },
        { "targetCount"_s, wordAfter(branch, BranchWord::targetCount), Wire::none, Kind::ListOutOfBounds },
        { "condition absent"_s, wordAfter(branch, BranchWord::condition), Wire::none, Kind::OperandMissing },
        { "target past the end"_s, firstBlock, numberOfBlocks, Kind::IdOutOfRange },
        { "argumentCount"_s, wordAfter(branch, BranchWord::firstTarget) + offsetof(Wire::TargetHead, argumentCount), Wire::none, Kind::ListOutOfBounds },
        { "proofCount"_s, wordAfter(branch, BranchWord::firstTarget) + offsetof(Wire::TargetHead, proofCount), Wire::none, Kind::ListOutOfBounds },
        { "block without an earlier predecessor"_s, firstBlock, 2, Kind::BlockOrderInvalid },
        { "first edge to the entry block"_s, firstBlock, 0, Kind::EdgeToEntryBlock },
        { "second edge to the entry block"_s, secondBlock, 0, Kind::EdgeToEntryBlock },
        { "targets of another terminator"_s, fixture.offsetOfHeadAt(f8, 4001), static_cast<uint32_t>(TerminatorKind::Unreachable), Kind::TargetCountMismatch },
        { "Switch.cases"_s, wordAfter(fixture.offsetOfHeadAt(f7, 2001), 4), Wire::none, Kind::ListOutOfBounds },
        { "SwitchString.strings"_s, wordAfter(fixture.offsetOfHeadAt(f7, 2002), 5), fixture.countOf(List::Strings), Kind::IdOutOfRange },
        { "argument past the end"_s, wordAfter(fixture.offsetOfHeadAt(f7, 2002), 6 + wordsInTargetHead), Wire::none, Kind::IdOutOfRange },
    });

    size_t firstCaseBlock = wordAfter(fixture.offsetOfHeadAt(f7, 2002), 6) + offsetof(Wire::TargetHead, block);
    AOT_TEST_CHECK(fixture.word(firstCaseBlock) == 3);
    expectEachRejectedByVerifier(fixture, {
        { "argument for a block without parameters"_s, wordAfter(fixture.offsetOfHeadAt(f7, 2001), 9) + offsetof(Wire::TargetHead, block), 2, Kind::EdgeArgumentCountMismatch },
        { "equal cases"_s, wordAfter(fixture.offsetOfHeadAt(f7, 2001), 7), 1, Kind::DuplicateListEntry },
    });
}

void testStreamBlocks()
{
    ReaderFixture fixture;
    constexpr size_t handlerSize = sizeof(Wire::TargetHead) + wordSize;
    size_t handler = fixture.offsetOfHeadAt(f8, 3003) - handlerSize;
    size_t blockHead = handler - sizeof(Wire::ParameterHead) - sizeof(Wire::BlockHead);
    AOT_TEST_CHECK(fixture.word(blockHead + offsetof(Wire::BlockHead, parameterCount)) == 1 && fixture.word(blockHead + offsetof(Wire::BlockHead, hasHandler)) == 1);
    AOT_TEST_CHECK(fixture.word(handler + offsetof(Wire::TargetHead, block)) == 3 && fixture.word(handler + offsetof(Wire::TargetHead, argumentCount)) == 1);
    expectEach(fixture, {
        { "BlockHead.parameterCount"_s, blockHead + offsetof(Wire::BlockHead, parameterCount), Wire::none, Kind::ListOutOfBounds },
        { "BlockHead.valueCount"_s, blockHead + offsetof(Wire::BlockHead, valueCount), Wire::none, Kind::ListOutOfBounds },
        { "BlockHead.role"_s, blockHead + offsetof(Wire::BlockHead, role), numberOfEnumerators<BlockRole>, Kind::UnknownEnumerator },
        { "BlockHead.subject"_s, blockHead + offsetof(Wire::BlockHead, subject), 6, Kind::IdOutOfRange },
        { "BlockHead.frequency"_s, blockHead + offsetof(Wire::BlockHead, frequency), numberOfEnumerators<BlockFrequency>, Kind::UnknownEnumerator },
        { "frequency of the engine"_s, blockHead + offsetof(Wire::BlockHead, frequency), static_cast<uint32_t>(BlockFrequency::Rare), Kind::EngineOnlyOnWire },
        { "BlockHead.hasHandler"_s, blockHead + offsetof(Wire::BlockHead, hasHandler), 2, Kind::UnknownEnumerator },
        { "handler past the end"_s, handler + offsetof(Wire::TargetHead, block), 4, Kind::IdOutOfRange },
        { "entry block as handler"_s, handler + offsetof(Wire::TargetHead, block), 0, Kind::EdgeToEntryBlock },
        { "argument of the handler past the end"_s, handler + sizeof(Wire::TargetHead), 6, Kind::IdOutOfRange },
    });

    size_t branch = fixture.offsetOfHeadAt(f8, 4000);
    size_t proof = wordAfter(branch, BranchWord::firstTarget + wordsInTargetHead);
    AOT_TEST_CHECK(fixture.word(wordAfter(branch, BranchWord::firstTarget) + offsetof(Wire::TargetHead, proofCount)) == 1 && !fixture.word(proof));
    expectEachRejectedByVerifier(fixture, {
        { "proof about an operand the test does not have"_s, proof, 5, Kind::InvalidProof },
        { "ordinary block as handler"_s, fixture.offsetOfHeadAt(f8, 3005) - sizeof(Wire::ParameterHead) - sizeof(Wire::BlockHead) + offsetof(Wire::BlockHead, role), 0, Kind::InvalidHandler },
    });
}

} // anonymous namespace

std::span<const NativeTest> readerStreamTests()
{
    static constexpr NativeTest tests[] = {
        { "reader/stream-heads"_s, testStreamHeads },
        { "reader/stream-operands"_s, testStreamOperands },
        { "reader/stream-payloads"_s, testStreamPayloads },
        { "reader/stream-scope-references"_s, testStreamScopeReferences },
        { "reader/stream-terminators"_s, testStreamTerminators },
        { "reader/stream-blocks"_s, testStreamBlocks },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
