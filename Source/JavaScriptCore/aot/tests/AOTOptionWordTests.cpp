/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCompileOptionWords.h"
#include "AOTCompileOptions.h"
#include "AOTDiagnostics.h"
#include "AOTIR.h"
#include "AOTLimits.h"
#include "AOTNativeTest.h"
#include "AOTPassID.h"
#include "AOTProvisionalFunctions.h"
#include "AOTSchemaEnums.h"
#include "AOTSelectionRules.h"
#include "AOTVector.h"
#include "AOTWire.h"
#include "B3Policy.h"
#include "CPUFeatureSet.h"
#include "Options.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <wtf/ASCIICType.h>
#include <wtf/DataLog.h>
#include <wtf/NumberOfCores.h>
#include <wtf/StdLibExtras.h>
#include <wtf/StringPrintStream.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

using Typed::FunctionID;
using Typed::Opcode;
using Typed::ProvisionalFunctions;

using WireOptions = Wire::CompileOptions;

namespace {

struct OptionWordCase {
    ASCIILiteral word;
    void (*setExpectedMembers)(WireOptions&);
};

struct InvalidMemberCase {
    size_t offset;
    void (*corrupt)(WireOptions&);
    void (*setSafeValues)(CompileOptions&) { nullptr };
};

struct ReadOptions {
    CompileOptions options;
    Vector<InternalError> errors;
};

struct ListedWords {
    Vector<String> engineWords;
    Vector<String> frontEndKeys;
};

} // anonymous namespace

static constexpr unsigned bitsPerSwitchWord = 64;
static constexpr uint64_t lastSwitchBit = 1ull << (bitsPerSwitchWord - 1);
static constexpr uint32_t sampleLayoutSeed = 7;
static constexpr uint32_t nullID = UINT32_MAX;
static constexpr ASCIILiteral cannotBeLoweredMark = " (cannot be lowered)"_s;
static constexpr ASCIILiteral frontEndHeading = "front end only:"_s;
static constexpr Architecture otherArchitecture = hostArchitecture() == Architecture::ARM64 ? Architecture::X86_64 : Architecture::ARM64;

static_assert(!FunctionID(nullID));
static_assert(defaultLimitValue(LimitID::MaxLayoutsInType, hostArchitecture()) == 1024);
static_assert(canBeLowered(LimitID::MaxLayoutsInType));
static_assert(canBeLowered(LimitID::MaxLiteralsInType));
static_assert(canBeLowered(LimitID::MinSwitchCases));
static_assert(failsBuild(LimitID::MaxDataNodes) && !canBeLowered(LimitID::MaxDataNodes));
static_assert(numberOfEnumerators<Wire::FlowResultKind> < bitsPerSwitchWord);
static_assert(numberOfEnumerators<Wire::SelectionRuleID> < bitsPerSwitchWord);
static_assert(B3::numberOfSwitches < bitsPerSwitchWord);
static_assert(numberOfPasses < 8 * sizeof(Wire::BisectionSwitches::disabledPasses));
static_assert(numberOfCPUFeatures < 64);

static bool isSame(const WireOptions& first, const WireOptions& second)
{
    return equalSpans(asByteSpan(first), asByteSpan(second));
}

static bool isSame(const CompileOptions& first, const CompileOptions& second)
{
    return first.recorded == second.recorded
        && first.limits == second.limits
        && first.executableIdentity == second.executableIdentity
        && first.sizeClassProgression == second.sizeClassProgression
        && first.threadCount == second.threadCount
        && first.action == second.action
        && first.verifiesAfterEveryPass == second.verifiesAfterEveryPass
        && first.verifiesAfterEveryStage == second.verifiesAfterEveryStage
        && first.validatesBackEnd == second.validatesBackEnd
        && first.validatesReferences == second.validatesReferences
        && first.validatesReferenceGuards == second.validatesReferenceGuards
        && first.reports == second.reports
        && first.excludedCode == second.excludedCode
        && first.output == second.output;
}

static void applyWord(WireOptions& options, StringView word)
{
    std::optional<String> sentence = applyOptionWord(options, word);
    if (sentence)
        dataLogLn("Option word ", word, ": ", *sentence);
    AOT_TEST_CHECK(!sentence);
}

static WireOptions optionsWith(std::initializer_list<StringView> words)
{
    WireOptions options = makeDefaultCompileOptions();
    for (StringView word : words)
        applyWord(options, word);
    return options;
}

static String applyRejectedWord(const WireOptions& options, StringView word)
{
    WireOptions changedOptions = options;
    std::optional<String> sentence = applyOptionWord(changedOptions, word);
    if (!sentence || !isSame(changedOptions, options))
        dataLogLn("Option word ", word, " was not rejected");
    AOT_TEST_CHECK(sentence);
    AOT_TEST_CHECK(!sentence->isEmpty());
    AOT_TEST_CHECK(isSame(changedOptions, options));
    return *sentence;
}

static String rejectionOf(StringView word)
{
    return applyRejectedWord(makeDefaultCompileOptions(), word);
}

static ReadOptions readOptions(const WireOptions& wire)
{
    Diagnostics diagnostics;
    CompileOptions options = CompileOptions::fromWire(wire, diagnostics);
    diagnostics.finish(nullptr);
    AOT_TEST_CHECK(diagnostics.userDiagnostics().isEmpty());
    return { WTF::move(options), diagnostics.internalErrors() };
}

static CompileOptions readValidOptions(const WireOptions& wire)
{
    ReadOptions result = readOptions(wire);
    AOT_TEST_CHECK(result.errors.isEmpty());
    return WTF::move(result.options);
}

static void expectValidOptions(const WireOptions& wire, StringView lastWord)
{
    bool isValid = readOptions(wire).errors.isEmpty();
    if (!isValid)
        dataLogLn("Option word ", lastWord, " wrote options that fromWire reports");
    AOT_TEST_CHECK(isValid);
}

static bool reportsOnly(const Vector<InternalError>& errors, size_t offset)
{
    return errors.size() == 1 && errors[0].kind == InternalErrorKind::InvalidCompileOptions && errors[0].count == offset;
}

static unsigned defaultThreadCount()
{
    constexpr unsigned mostUsefulThreads = 16;
    return std::clamp<unsigned>(WTF::numberOfProcessorCores(), 1, std::min<unsigned>(Limits::maxWorkers, mostUsefulThreads));
}

static CompileOptions createDefaultOptions(uint32_t layoutSeed)
{
    CompileOptions options;
    options.recorded.architecture = static_cast<uint8_t>(hostArchitecture());
    options.recorded.targetFeatures = CPUFeatureSet::buildTarget().bits();
    options.recorded.layoutSeed = layoutSeed;
    options.sizeClassProgression = Options::sizeClassProgressionWithAOT();
    options.threadCount = defaultThreadCount();
    return options;
}

template<typename Flags>
static constexpr Flags flagSet(std::initializer_list<Flags> flags)
{
    uint32_t bits = 0;
    for (Flags flag : flags)
        bits |= static_cast<uint32_t>(flag);
    return static_cast<Flags>(bits);
}

template<typename Flags>
static constexpr Flags withInvalidFlag(Flags flags)
{
    static_assert(numberOfEnumerators<Flags> < 32);
    return static_cast<Flags>(static_cast<uint32_t>(flags) | 1u << numberOfEnumerators<Flags>);
}

template<typename E>
static constexpr E firstInvalidEnumerator = static_cast<E>(numberOfEnumerators<E>);

template<typename E>
static constexpr uint64_t switchBit(E member)
{
    return 1ull << static_cast<unsigned>(member);
}

template<typename SwitchWords>
static void disablePass(SwitchWords& disabledPasses, PassID pass)
{
    unsigned bit = static_cast<unsigned>(pass);
    disabledPasses[bit / bitsPerSwitchWord] |= 1ull << (bit % bitsPerSwitchWord);
}

static bool isDisabled(const WireOptions& options, PassID pass)
{
    unsigned bit = static_cast<unsigned>(pass);
    return (options.bisection.disabledPasses[bit / bitsPerSwitchWord] >> (bit % bitsPerSwitchWord)) & 1;
}

static StringView firstPartOf(StringView text, char16_t separator)
{
    auto parts = text.split(separator);
    AOT_TEST_CHECK(parts.begin() != parts.end());
    return *parts.begin();
}

static String listOptions()
{
    StringPrintStream out;
    listOptionWords(out);
    return out.toString();
}

static constexpr OptionWordCase optionWordCases[] = {
    { "validate"_s, [](WireOptions& options) { options.checks = Wire::CheckSet::Validate; } },
    { "check=verify-each-pass,ValidateBackEnd"_s, [](WireOptions& options) { options.checks = flagSet({ Wire::CheckSet::VerifyEachPass, Wire::CheckSet::ValidateBackEnd }); } },
    { "verify-only"_s, [](WireOptions& options) { options.action = Wire::CompileAction::VerifyOnly; } },
    { "check-only"_s, [](WireOptions& options) { options.action = Wire::CompileAction::CheckOnly; } },
    { "disable=flow-results"_s, [](WireOptions& options) {
        options.bisection.disabledFlowResults = (1ull << numberOfEnumerators<Wire::FlowResultKind>) - 1;
        disablePass(options.bisection.disabledPasses, PassID::AnalyzeFlow);
    } },
    { "disable=flow:NarrowedTypes"_s, [](WireOptions& options) { options.bisection.disabledFlowResults = switchBit(Wire::FlowResultKind::NarrowedTypes); } },
    { "disable=pass:FoldConstants"_s, [](WireOptions& options) { disablePass(options.bisection.disabledPasses, PassID::FoldConstants); } },
    { "disable=rule:DescriptorLiteral"_s, [](WireOptions& options) { options.bisection.disabledSelectionRules = switchBit(Wire::SelectionRuleID::DescriptorLiteral); } },
    { "disable=b3:RotateLoops"_s, [](WireOptions& options) { options.bisection.disabledBackEndSwitches = switchBit(B3::PolicySwitch::RotateLoops); } },
    { "disable-in=5:3"_s, [](WireOptions& options) {
        options.bisection.switchRange.firstID = 5;
        options.bisection.switchRange.count = 3;
    } },
    { "disable-in=7"_s, [](WireOptions& options) { options.bisection.switchRange.firstID = 7; } },
    { "limit=MaxLayoutsInType:1"_s, [](WireOptions& options) {
        options.loweredLimit = LimitID::MaxLayoutsInType;
        options.loweredLimitValue = 1;
    } },
    { "heat=EverythingIsCold"_s, [](WireOptions& options) { options.heatOverride = Wire::HeatOverride::EverythingIsCold; } },
    { "heat=everything-is-cold"_s, [](WireOptions& options) { options.heatOverride = Wire::HeatOverride::EverythingIsCold; } },
    { "testing=ignore-branch-hints"_s, [](WireOptions& options) { options.testing = Wire::TestingSwitch::IgnoreBranchHints; } },
    { "testing=IgnoreExpectedTypes"_s, [](WireOptions& options) { options.testing = Wire::TestingSwitch::IgnoreExpectedTypes; } },
    { "testing=FailPredictions"_s, [](WireOptions& options) {
        options.testing = Wire::TestingSwitch::FailPredictions;
        options.bisection.predictionFailure = { .period = 1, .seed = 0 };
    } },
    { "testing=fail-predictions:7"_s, [](WireOptions& options) {
        options.testing = Wire::TestingSwitch::FailPredictions;
        options.bisection.predictionFailure = { .period = 7, .seed = 0 };
    } },
    { "testing=FailPredictions:100:3"_s, [](WireOptions& options) {
        options.testing = Wire::TestingSwitch::FailPredictions;
        options.bisection.predictionFailure = { .period = 100, .seed = 3 };
    } },
    { "sabotage=InnerCellLetGoKeepsItsHeader"_s, [](WireOptions& options) { options.sabotage = Wire::Sabotage::InnerCellLetGoKeepsItsHeader; } },
    { "sabotage=room-is-not-in-reference-list"_s, [](WireOptions& options) { options.sabotage = Wire::Sabotage::RoomIsNotInReferenceList; } },
    { "count-branches"_s, [](WireOptions& options) { options.instrumentation = Wire::Instrumentation::CountBranches; } },
    { "count-arrivals"_s, [](WireOptions& options) { options.instrumentation = Wire::Instrumentation::CountArrivals; } },
    { "drop-guard=TypeTest:2"_s, [](WireOptions& options) {
        options.testing = Wire::TestingSwitch::DropGuard;
        options.bisection.droppedGuard.opcode = static_cast<uint32_t>(Opcode::TypeTest);
        options.bisection.droppedGuard.ordinal = 2;
    } },
    { "references=narrow"_s, [](WireOptions& options) { options.target.referenceWidth = Wire::ReferenceWidth::Narrow; } },
    { "image-form=position-independent"_s, [](WireOptions& options) { options.target.imageForm = Wire::ImageForm::PositionIndependent; } },
    { "image-form=absolute"_s, [](WireOptions& options) { options.target.imageForm = Wire::ImageForm::Absolute; } },
    { "type-errors=log"_s, [](WireOptions& options) { options.mismatchPolicy = Wire::MismatchPolicy::Report; } },
    { "layout-seed=9"_s, [](WireOptions& options) { options.layoutSeed = 9; } },
    { "threads=3"_s, [](WireOptions& options) { options.threadCount = 3; } },
    { "report=Declines,Flow"_s, [](WireOptions& options) { options.reports = flagSet({ Wire::ReportSet::Declines, Wire::ReportSet::Flow }); } },
    { "admit=Inspector"_s, [](WireOptions& options) { options.admittedCode = Wire::AdmittedCode::Inspector; } },
    { "admit=inspector"_s, [](WireOptions& options) { options.admittedCode = Wire::AdmittedCode::Inspector; } },
    { "exclude=EvaluatedText"_s, [](WireOptions& options) { options.excludedCode = Wire::ExcludedCode::EvaluatedText; } },
    { "exclude=evaluated-text,FunctionSource"_s, [](WireOptions& options) { options.excludedCode = flagSet({ Wire::ExcludedCode::EvaluatedText, Wire::ExcludedCode::FunctionSource }); } },
    { "obfuscate"_s, [](WireOptions& options) { options.output = Wire::OutputOptions::Obfuscate; } },
};

static constexpr ASCIILiteral threadJumpsWords[] = {
    "disable=pass:ThreadJumps"_s,
    "disable=b3:ThreadJumps"_s,
};

static constexpr ASCIILiteral rejectedWords[] = {
    "nothing"_s,
    "validate=1"_s,
    "check"_s,
    "check="_s,
    "check=Validate,"_s,
    "check=Validate,Nothing"_s,
    "verify-only=1"_s,
    "disable=Nothing"_s,
    "disable=ThreadJumps"_s,
    "disable=pass:None"_s,
    "disable=pass:VerifyProgram"_s,
    "disable=rule:None"_s,
    "disable=rule:PlannedSlots"_s,
    "disable-in=x"_s,
    "disable-in=1:0"_s,
    "limit=None:1"_s,
    "limit=Nothing:1"_s,
    "limit=MaxLayoutsInType:1025"_s,
    "limit=MaxLayoutsInType"_s,
    "limit=MaxDataNodes:1"_s,
    "heat=Warm"_s,
    "testing=DropGuard"_s,
    "testing=IgnorePredictions"_s,
    "sabotage=Everything"_s,
    "sabotage"_s,
    "count-arrivals=1"_s,
    "drop-guard=Nothing:1"_s,
    "drop-guard=TypeTest"_s,
    "references=tiny"_s,
    "image-form=relative"_s,
    "type-errors=ignore"_s,
    "layout-seed=-1"_s,
    "threads=many"_s,
    "threads=0"_s,
    "report=Everything"_s,
    "admit"_s,
    "admit=Everything"_s,
    "admit=RuntimeCode"_s,
    "exclude=Everything"_s,
    "obfuscate=1"_s,
    "erase-types"_s,
    "out=x"_s,
};

static void testOptionWordsSetMembers()
{
    for (const OptionWordCase& testCase : optionWordCases) {
        WireOptions expected = makeDefaultCompileOptions();
        testCase.setExpectedMembers(expected);
        bool isExpected = isSame(optionsWith({ testCase.word }), expected);
        if (!isExpected)
            dataLogLn("Option word ", testCase.word, " set other members");
        AOT_TEST_CHECK(isExpected);
    }

    WireOptions bothChecks = makeDefaultCompileOptions();
    bothChecks.checks = flagSet({ Wire::CheckSet::Validate, Wire::CheckSet::ValidateReferences });
    AOT_TEST_CHECK(isSame(optionsWith({ "check=Validate"_s, "check=ValidateReferences"_s }), bothChecks));

    AOT_TEST_CHECK(isSame(optionsWith({ "heat=EverythingIsHot"_s, "heat=EverythingIsCold"_s }), optionsWith({ "heat=EverythingIsCold"_s })));
}

static void testOptionWordsRejected()
{
    WireOptions options = optionsWith({ "layout-seed=9"_s });
    for (ASCIILiteral word : rejectedWords)
        applyRejectedWord(options, word);

    AOT_TEST_CHECK(applyRejectedWord(options, "nothing"_s) == "unknown option nothing"_s);
    AOT_TEST_CHECK(applyRejectedWord(options, "validate=1"_s) == "validate takes no value"_s);
    AOT_TEST_CHECK(applyRejectedWord(options, "check"_s) == "check needs a value"_s);
    AOT_TEST_CHECK(applyRejectedWord(options, "threads=0"_s) == "the number of threads is at least 1"_s);
    AOT_TEST_CHECK(applyRejectedWord(options, "testing=FailPredictions:0"_s) == "a prediction fails one time in n, and n is at least 1"_s);
    AOT_TEST_CHECK(applyRejectedWord(options, "testing=FailPredictions:x"_s) == "x is not a number"_s);
    AOT_TEST_CHECK(applyRejectedWord(options, "testing=FailPredictions:2:65536"_s) == "65536 is not a number"_s);
    AOT_TEST_CHECK(applyRejectedWord(options, "testing=IgnoreBranchHints:2"_s) == "IgnoreBranchHints takes no number"_s);
    AOT_TEST_CHECK(applyRejectedWord(options, "erase-types"_s) == "erase-types is an option of the front end"_s);
    AOT_TEST_CHECK(applyRejectedWord(options, "out=x"_s) == "out is an option of the front end"_s);

    WireOptions loweredOptions = optionsWith({ "limit=MaxLayoutsInType:1"_s });
    applyRejectedWord(loweredOptions, "limit=MaxLiteralsInType:1"_s);
    applyWord(loweredOptions, "limit=MaxLayoutsInType:0"_s);

    AOT_TEST_CHECK(!isSame(optionsWith({ threadJumpsWords[0] }), optionsWith({ threadJumpsWords[1] })));
}

static ListedWords collectListedWords()
{
    String listing = listOptions();
    StringView listingView = listing;

    ListedWords words;
    StringView key;
    bool isFrontEndPart = false;
    for (StringView line : listingView.split('\n')) {
        if (line == frontEndHeading) {
            isFrontEndPart = true;
            continue;
        }
        if (!isTabOrSpace(line[0])) {
            key = firstPartOf(line, '=');
            if (isFrontEndPart)
                words.frontEndKeys.append(key.toString());
            else if (key == line)
                words.engineWords.append(key.toString());
            continue;
        }
        AOT_TEST_CHECK(!isFrontEndPart);
        if (line.endsWith(cannotBeLoweredMark))
            continue;
        StringView name = firstPartOf(line, ' ');
        words.engineWords.append(key == "limit"_s ? makeString(key, '=', name, ":0"_s) : makeString(key, '=', name));
    }
    return words;
}

static void testOptionWordsListedNamesAccepted()
{
    static constexpr ASCIILiteral listedWords[] = {
        "validate"_s,
        "check=Validate"_s,
        "verify-only"_s,
        "disable=flow-results"_s,
        "disable=flow:ProvenLayoutSets"_s,
        "disable=pass:FoldConstants"_s,
        "disable=rule:DescriptorLiteral"_s,
        "disable=b3:ThreadJumps"_s,
        "only-passes=FoldConstants"_s,
        "limit=MaxLayoutsInType:0"_s,
        "limit=MinSwitchCases:0"_s,
        "heat=None"_s,
        "testing=IgnoreBranchHints"_s,
        "testing=IgnoreExpectedTypes"_s,
        "sabotage=None"_s,
        "count-branches"_s,
        "count-arrivals"_s,
        "references=narrow"_s,
        "type-errors=log"_s,
        "report=Flow"_s,
        "admit=Inspector"_s,
        "exclude=EvaluatedText"_s,
        "exclude=FunctionSource"_s,
        "obfuscate"_s,
    };
    static constexpr ASCIILiteral unlistedWords[] = {
        "disable=pass:None"_s,
        "disable=pass:VerifyProgram"_s,
        "disable=rule:None"_s,
        "disable=rule:PlannedSlots"_s,
        "only-passes=None"_s,
        "only-passes=VerifyProgram"_s,
        "testing=DropGuard"_s,
        "limit=None:0"_s,
        "limit=MaxDataNodes:0"_s,
    };

    ListedWords words = collectListedWords();
    for (ASCIILiteral word : listedWords)
        AOT_TEST_CHECK(words.engineWords.contains(word));
    for (ASCIILiteral word : unlistedWords)
        AOT_TEST_CHECK(!words.engineWords.contains(word));
    AOT_TEST_CHECK((words.frontEndKeys == Vector<String> { "erase-types"_s, "out"_s }));

    for (const String& word : words.engineWords)
        optionsWith({ word });
}

static void testOptionWordsPassFromWire()
{
    WireOptions combinedOptions = makeDefaultCompileOptions();
    for (const OptionWordCase& testCase : optionWordCases) {
        expectValidOptions(optionsWith({ testCase.word }), testCase.word);
        applyWord(combinedOptions, testCase.word);
        expectValidOptions(combinedOptions, testCase.word);
    }

    for (ASCIILiteral word : threadJumpsWords)
        expectValidOptions(optionsWith({ word }), word);
    for (const String& word : collectListedWords().engineWords)
        expectValidOptions(optionsWith({ word }), word);
}

static void expectOptionalPasses(const WireOptions& options, std::initializer_list<PassID> enabledPasses)
{
    for (PassID pass : allMembersOf<PassID>) {
        bool isListed = std::ranges::find(enabledPasses, pass) != enabledPasses.end();
        AOT_TEST_CHECK(isDisabled(options, pass) == (isOptionalPass(pass) && !isListed));
    }
}

static void testOnlyPassesDisablesOtherOptionalPasses()
{
    WireOptions options = optionsWith({ "only-passes=ResolveCalls,inline-calls"_s });
    expectOptionalPasses(options, { PassID::AnalyzeFlow, PassID::ResolveCalls, PassID::InlineCalls });

    options.bisection.disabledPasses = { };
    AOT_TEST_CHECK(isSame(options, makeDefaultCompileOptions()));

    expectOptionalPasses(optionsWith({ "only-passes=AnalyzeFlow"_s }), { PassID::AnalyzeFlow });
    expectOptionalPasses(optionsWith({ "only-passes=VerifyProgram"_s }), { PassID::AnalyzeFlow });
}

static void testOnlyPassesKeepsDisabledPasses()
{
    WireOptions disabledFirst = optionsWith({ "disable=pass:ResolveCalls"_s, "only-passes=ResolveCalls,InlineCalls"_s });
    WireOptions disabledLast = optionsWith({ "only-passes=ResolveCalls,InlineCalls"_s, "disable=pass:ResolveCalls"_s });
    expectOptionalPasses(disabledFirst, { PassID::AnalyzeFlow, PassID::InlineCalls });
    AOT_TEST_CHECK(isSame(disabledFirst, disabledLast));

    expectOptionalPasses(optionsWith({ "only-passes=ResolveCalls"_s, "disable=pass:AnalyzeFlow"_s }), { PassID::ResolveCalls });
}

static void testOnlyPassesAcceptedByFromWire()
{
    RecordedOptions recorded = readValidOptions(optionsWith({ "only-passes=ResolveCalls"_s })).recorded;

    AOT_TEST_CHECK(recorded.isEnabled(PassID::ResolveCalls, FunctionID(0u)));
    AOT_TEST_CHECK(recorded.isEnabled(PassID::AnalyzeFlow, FunctionID()));
    AOT_TEST_CHECK(recorded.isEnabled(PassID::VerifyProgram, FunctionID()));
    AOT_TEST_CHECK(!recorded.isEnabled(PassID::InlineCalls, FunctionID(0u)));
    AOT_TEST_CHECK(!recorded.isEnabled(PassID::EstimateHeat, FunctionID()));
}

static void testOnlyPassesRejectedWords()
{
    AOT_TEST_CHECK(rejectionOf("only-passes"_s) == "only-passes needs a value"_s);
    AOT_TEST_CHECK(rejectionOf("only-passes="_s) == "only-passes needs a value"_s);
    AOT_TEST_CHECK(rejectionOf("only-passes=Nothing"_s) == "unknown pass Nothing"_s);
    AOT_TEST_CHECK(rejectionOf("only-passes=None"_s) == "unknown pass None"_s);
    AOT_TEST_CHECK(rejectionOf("only-passes=ResolveCalls,Nothing"_s) == "unknown pass Nothing"_s);
    AOT_TEST_CHECK(rejectionOf("only-passes=pass:ResolveCalls"_s) == "unknown pass pass:ResolveCalls"_s);
    AOT_TEST_CHECK(rejectionOf("only-passes=ResolveCalls,"_s) == "empty name in only-passes=ResolveCalls,"_s);
}

static String listedLineOf(LimitID limit)
{
    return makeString("  "_s, nameOf(limit), ' ', defaultLimitValue(limit, hostArchitecture()), ' ', nameOf(stageOf(limit)), ' ', nameOf(protectsOf(limit)), ' ', nameOf(degradesToOf(limit)),
        canBeLowered(limit) ? ""_s : cannotBeLoweredMark, '\n');
}

static void testLimitWordFollowsCanBeLowered()
{
    AOT_TEST_CHECK(rejectionOf("limit=MaxDataNodes:1"_s) == "the limit MaxDataNodes fails the build and cannot be lowered"_s);
    AOT_TEST_CHECK(rejectionOf("limit=MaxLayoutsInType"_s) == "limit takes <LimitID>:<n>"_s);

    String listing = listOptions();
    AOT_TEST_CHECK(listing.contains("\nlimit=<LimitID>:<n>\n"_s));
    for (LimitID limit : allMembersOf<LimitID>) {
        WireOptions options = makeDefaultCompileOptions();
        bool isAccepted = !applyOptionWord(options, makeString("limit="_s, nameOf(limit), ":0"_s));
        AOT_TEST_CHECK(isAccepted == canBeLowered(limit));
        AOT_TEST_CHECK(canBeLowered(limit) == (limit != LimitID::None && degradesToOf(limit) != Wire::DegradesTo::BuildError));
        AOT_TEST_CHECK(listing.contains(listedLineOf(limit)) == (limit != LimitID::None));
    }
}

static void testRequiredPassesStayEnabled()
{
    unsigned numberOfRemovingPasses = 0;
    for (Opcode opcode : allMembersOf<Opcode>) {
        PassID pass = removedByOf(opcode);
        if (pass == PassID::None)
            continue;
        ++numberOfRemovingPasses;
        AOT_TEST_CHECK(!isOptionalPass(pass));
        AOT_TEST_CHECK(rejectionOf(makeString("disable=pass:"_s, nameOf(pass))) == makeString("pass:"_s, nameOf(pass), " is required and cannot be disabled"_s));

        WireOptions wire = makeDefaultCompileOptions();
        disablePass(wire.bisection.disabledPasses, pass);
        AOT_TEST_CHECK(reportsOnly(readOptions(wire).errors, offsetof(WireOptions, bisection.disabledPasses)));
    }
    AOT_TEST_CHECK(numberOfRemovingPasses);
}

static void testCompileOptionsDefault()
{
    WireOptions expected { };
    expected.size = sizeof(WireOptions);
    expected.threadCount = defaultThreadCount();
    expected.target.architecture = hostArchitecture();
    expected.target.features = CPUFeatureSet::buildTarget().bits();
    expected.target.imageForm = imageFormOfThisRuntime();
    expected.sizeClassProgression = Options::sizeClassProgressionWithAOT();
    AOT_TEST_CHECK(isSame(makeDefaultCompileOptions(), expected));

    CompileOptions options = readValidOptions(makeDefaultCompileOptions());
    AOT_TEST_CHECK(isSame(options, createDefaultOptions(0)));
    AOT_TEST_CHECK(options.limits == LimitValues(hostArchitecture()));
    AOT_TEST_CHECK(options.recorded.isCompatibleWithProcess());
}

static constexpr InvalidMemberCase invalidMemberCases[] = {
    { offsetof(WireOptions, target.referenceWidth), [](WireOptions& wire) { wire.target.referenceWidth = firstInvalidEnumerator<Wire::ReferenceWidth>; } },
    { offsetof(WireOptions, target.imageForm), [](WireOptions& wire) { wire.target.imageForm = firstInvalidEnumerator<Wire::ImageForm>; }, [](CompileOptions& options) { options.recorded.imageForm = static_cast<uint8_t>(Wire::ImageForm { }); } },
    { offsetof(WireOptions, target.reserved), [](WireOptions& wire) { wire.target.reserved = 1; } },
    { offsetof(WireOptions, action), [](WireOptions& wire) { wire.action = firstInvalidEnumerator<Wire::CompileAction>; } },
    { offsetof(WireOptions, mismatchPolicy), [](WireOptions& wire) { wire.mismatchPolicy = firstInvalidEnumerator<Wire::MismatchPolicy>; } },
    { offsetof(WireOptions, heatOverride), [](WireOptions& wire) { wire.heatOverride = firstInvalidEnumerator<Wire::HeatOverride>; } },
    { offsetof(WireOptions, loweredLimit), [](WireOptions& wire) { wire.loweredLimit = firstInvalidEnumerator<LimitID>; } },
    { offsetof(WireOptions, loweredLimit), [](WireOptions& wire) { wire.loweredLimit = LimitID::MaxDataNodes; } },
    { offsetof(WireOptions, loweredLimit), [](WireOptions& wire) { wire.loweredLimitValue = 5; } },
    { offsetof(WireOptions, loweredLimit), [](WireOptions& wire) {
        wire.loweredLimit = LimitID::MaxLayoutsInType;
        wire.loweredLimitValue = defaultLimitValue(LimitID::MaxLayoutsInType, hostArchitecture()) + 1;
    } },
    { offsetof(WireOptions, checks), [](WireOptions& wire) { wire.checks = withInvalidFlag(Wire::CheckSet::Validate); } },
    { offsetof(WireOptions, reports), [](WireOptions& wire) { wire.reports = withInvalidFlag(Wire::ReportSet::Flow); } },
    { offsetof(WireOptions, testing), [](WireOptions& wire) { wire.testing = withInvalidFlag(Wire::TestingSwitch::IgnoreBranchHints); } },
    { offsetof(WireOptions, instrumentation), [](WireOptions& wire) { wire.instrumentation = withInvalidFlag(Wire::Instrumentation::CountBranches); } },
    { offsetof(WireOptions, admittedCode), [](WireOptions& wire) { wire.admittedCode = withInvalidFlag(Wire::AdmittedCode::Inspector); } },
    { offsetof(WireOptions, excludedCode), [](WireOptions& wire) { wire.excludedCode = withInvalidFlag(Wire::ExcludedCode::EvaluatedText); } },
    { offsetof(WireOptions, output), [](WireOptions& wire) { wire.output = withInvalidFlag(Wire::OutputOptions::Obfuscate); } },
    { offsetof(WireOptions, bisection.disabledFlowResults), [](WireOptions& wire) { wire.bisection.disabledFlowResults = lastSwitchBit; } },
    { offsetof(WireOptions, bisection.disabledSelectionRules), [](WireOptions& wire) { wire.bisection.disabledSelectionRules = lastSwitchBit; } },
    { offsetof(WireOptions, bisection.disabledSelectionRules), [](WireOptions& wire) { wire.bisection.disabledSelectionRules = switchBit(Wire::SelectionRuleID::None); } },
    { offsetof(WireOptions, bisection.disabledSelectionRules), [](WireOptions& wire) { wire.bisection.disabledSelectionRules = switchBit(Wire::SelectionRuleID::PlannedSlots); } },
    { offsetof(WireOptions, bisection.disabledBackEndSwitches), [](WireOptions& wire) { wire.bisection.disabledBackEndSwitches = lastSwitchBit; } },
    { offsetof(WireOptions, bisection.disabledPasses), [](WireOptions& wire) { wire.bisection.disabledPasses[std::size(wire.bisection.disabledPasses) - 1] = lastSwitchBit; } },
    { offsetof(WireOptions, bisection.disabledPasses), [](WireOptions& wire) { disablePass(wire.bisection.disabledPasses, PassID::None); } },
    { offsetof(WireOptions, bisection.disabledPasses), [](WireOptions& wire) { disablePass(wire.bisection.disabledPasses, PassID::VerifyProgram); } },
    { offsetof(WireOptions, bisection.droppedGuard.opcode), [](WireOptions& wire) {
        wire.testing = Wire::TestingSwitch::DropGuard;
        wire.bisection.droppedGuard.opcode = numberOfEnumerators<Opcode>;
    } },
    { offsetof(WireOptions, bisection.predictionFailure.period), [](WireOptions& wire) { wire.testing = Wire::TestingSwitch::FailPredictions; } },
    { offsetof(WireOptions, bisection.predictionFailure.period), [](WireOptions& wire) {
        wire.testing = Wire::TestingSwitch::FailPredictions;
        wire.bisection.predictionFailure = { .period = 1, .seed = UINT16_MAX + 1 };
    } },
    { offsetof(WireOptions, threadCount), [](WireOptions& wire) { wire.threadCount = 0; }, [](CompileOptions& options) { options.threadCount = 1; } },
    { offsetof(WireOptions, target.architecture), [](WireOptions& wire) { wire.target.architecture = otherArchitecture; }, [](CompileOptions& options) {
        options.recorded.architecture = static_cast<uint8_t>(otherArchitecture);
        options.limits = LimitValues(otherArchitecture);
    } },
    { offsetof(WireOptions, target.architecture), [](WireOptions& wire) { wire.target.architecture = firstInvalidEnumerator<Architecture>; }, [](CompileOptions& options) {
        options.recorded.architecture = static_cast<uint8_t>(Architecture { });
        options.limits = LimitValues(Architecture { });
    } },
    { offsetof(WireOptions, target.features), [](WireOptions& wire) { wire.target.features |= 1ull << numberOfCPUFeatures; }, [](CompileOptions& options) { options.recorded.targetFeatures = 0; } },
    { offsetof(WireOptions, sizeClassProgression), [](WireOptions& wire) { wire.sizeClassProgression = 1; }, [](CompileOptions& options) { options.sizeClassProgression = 0; } },
    { offsetof(WireOptions, sizeClassProgression), [](WireOptions& wire) { wire.sizeClassProgression = std::numeric_limits<double>::infinity(); }, [](CompileOptions& options) { options.sizeClassProgression = 0; } },
    { offsetof(WireOptions, sizeClassProgression), [](WireOptions& wire) { wire.sizeClassProgression = std::numeric_limits<double>::quiet_NaN(); }, [](CompileOptions& options) { options.sizeClassProgression = 0; } },
};

static void expectInvalidMember(const InvalidMemberCase& testCase)
{
    WireOptions wire = makeDefaultCompileOptions();
    wire.layoutSeed = sampleLayoutSeed;
    testCase.corrupt(wire);

    CompileOptions expected = createDefaultOptions(sampleLayoutSeed);
    if (testCase.setSafeValues)
        testCase.setSafeValues(expected);

    auto [options, errors] = readOptions(wire);
    bool isReported = reportsOnly(errors, testCase.offset);
    bool hasSafeValues = isSame(options, expected);
    if (!isReported || !hasSafeValues)
        dataLogLn("Invalid member of Wire::CompileOptions at offset ", testCase.offset);
    AOT_TEST_CHECK(isReported);
    AOT_TEST_CHECK(hasSafeValues);
}

static void testCompileOptionsInvalidMembers()
{
    WireOptions wireWithoutSize = makeDefaultCompileOptions();
    wireWithoutSize.layoutSeed = sampleLayoutSeed;
    wireWithoutSize.size = 0;
    auto [options, errors] = readOptions(wireWithoutSize);
    AOT_TEST_CHECK(reportsOnly(errors, offsetof(WireOptions, size)));
    AOT_TEST_CHECK(isSame(options, CompileOptions { }));

    for (const InvalidMemberCase& testCase : invalidMemberCases)
        expectInvalidMember(testCase);
}

static void testCompileOptionsIgnoredWords()
{
    WireOptions first = makeDefaultCompileOptions();
    WireOptions second = first;
    first.bisection.droppedGuard.ordinal = 1;
    second.bisection.droppedGuard.ordinal = 2;
    AOT_TEST_CHECK(readValidOptions(first).recorded == readValidOptions(second).recorded);

    first.testing = Wire::TestingSwitch::DropGuard;
    second.testing = Wire::TestingSwitch::DropGuard;
    AOT_TEST_CHECK(readValidOptions(first).recorded != readValidOptions(second).recorded);
}

static void testThreadCountFollowsMaxWorkers()
{
    WireOptions wire = makeDefaultCompileOptions();
    wire.threadCount = UINT32_MAX;
    AOT_TEST_CHECK(readValidOptions(wire).threadCount == Limits::maxWorkers);

    wire.loweredLimit = LimitID::MaxWorkers;
    wire.loweredLimitValue = 2;
    AOT_TEST_CHECK(readValidOptions(wire).threadCount == 2);

    wire.threadCount = 1;
    AOT_TEST_CHECK(readValidOptions(wire).threadCount == 1);
}

static void testWireOptionsFollowCanBeLowered()
{
    for (LimitID limit : allMembersOf<LimitID>) {
        if (limit == LimitID::None)
            continue;
        WireOptions wire = makeDefaultCompileOptions();
        wire.loweredLimit = limit;
        wire.loweredLimitValue = 0;

        auto [options, errors] = readOptions(wire);
        if (canBeLowered(limit)) {
            AOT_TEST_CHECK(errors.isEmpty());
            AOT_TEST_CHECK(options.recorded.loweredLimit == limit);
            AOT_TEST_CHECK(!options.limits[limit]);
            continue;
        }
        AOT_TEST_CHECK(reportsOnly(errors, offsetof(WireOptions, loweredLimit)));
        AOT_TEST_CHECK(isSame(options, createDefaultOptions(0)));
    }

    WireOptions all = makeDefaultCompileOptions();
    all.loweredLimit = LimitID::All;
    all.loweredLimitValue = 2;
    CompileOptions together = readValidOptions(all);
    for (LimitID limit : allMembersOf<LimitID>) {
        uint64_t usual = defaultLimitValue(limit, hostArchitecture());
        AOT_TEST_CHECK(together.limits[limit] == (canBeLowered(limit) ? std::min(usual, std::max<uint64_t>(2, smallestOf(limit))) : usual));
    }

    WireOptions wire = makeDefaultCompileOptions();
    wire.loweredLimit = LimitID::MaxDataNodes;
    wire.loweredLimitValue = 1;
    BuildContext build(wire);
    AOT_TEST_CHECK(build.diagnostics().hasErrors());
    AOT_TEST_CHECK(build.limit(LimitID::MaxDataNodes) == defaultLimitValue(LimitID::MaxDataNodes, hostArchitecture()));
}

static void checkSwitchRange(uint32_t firstID, uint32_t idCount, std::initializer_list<uint32_t> disabledIDs, std::initializer_list<uint32_t> enabledIDs)
{
    RecordedOptions options;
    options.switches.firstID = firstID;
    options.switches.idCount = idCount;
    disablePass(options.switches.disabledPasses, PassID::FoldConstants);
    options.switches.disabledFlowResults = switchBit(Wire::FlowResultKind::ProvenLayoutSets);
    options.switches.disabledSelectionRules = switchBit(Wire::SelectionRuleID::DescriptorLiteral);
    options.switches.disabledBackEndSwitches = switchBit(B3::PolicySwitch::RotateLoops);

    auto isFlowResultEnabled = [&](Wire::FlowResultKind kind, uint32_t id) {
        return id == nullID ? options.isEnabledForProgram(kind) : options.isEnabled(kind, id);
    };
    auto checkFunctionSwitches = [&](FunctionID function, bool expectsEnabled) {
        AOT_TEST_CHECK(options.isEnabled(PassID::FoldConstants, function) == expectsEnabled);
        AOT_TEST_CHECK(options.isEnabled(Wire::SelectionRuleID::DescriptorLiteral, function) == expectsEnabled);
        AOT_TEST_CHECK(options.isEnabled(B3::PolicySwitch::RotateLoops, function) == expectsEnabled);
        AOT_TEST_CHECK(options.isEnabled(PassID::VerifyProgram, function));
        AOT_TEST_CHECK(options.isEnabled(Wire::SelectionRuleID::PropertyRun, function));
        AOT_TEST_CHECK(options.isEnabled(B3::PolicySwitch::ThreadJumps, function));
    };
    auto isEnabled = [&](uint32_t id) {
        bool isDisabledResultEnabled = isFlowResultEnabled(Wire::FlowResultKind::ProvenLayoutSets, id);
        AOT_TEST_CHECK(isFlowResultEnabled(Wire::FlowResultKind::NarrowedTypes, id));
        if (!ProvisionalFunctions::contains(FunctionID(id)))
            checkFunctionSwitches(FunctionID(id), isDisabledResultEnabled);
        return isDisabledResultEnabled;
    };
    for (uint32_t id : disabledIDs)
        AOT_TEST_CHECK(!isEnabled(id));
    for (uint32_t id : enabledIDs)
        AOT_TEST_CHECK(isEnabled(id));
}

static void testSwitchRange()
{
    constexpr uint32_t lastFunctionIndex = ProvisionalFunctions::firstIndex - 1;

    checkSwitchRange(0, 0, { 0, 5, lastFunctionIndex, 0xfffffffe, nullID }, { });
    checkSwitchRange(5, 0, { 5, 6, lastFunctionIndex, 0xfffffffe }, { 4, nullID });
    checkSwitchRange(5, 3, { 5, 7 }, { 4, 8, lastFunctionIndex, nullID });
    checkSwitchRange(0xfffffff0, 0x20, { 0xfffffff0, 0xfffffffe }, { lastFunctionIndex, 0xffffffef, nullID });
}

static void testRecordedOptionsHash()
{
    using Bytes = std::array<uint8_t, sizeof(RecordedOptions)>;

    auto readSampleOptions = [] {
        return readValidOptions(optionsWith({ "validate"_s, "layout-seed=9"_s, "disable=flow-results"_s })).recorded;
    };
    RecordedOptions options = readSampleOptions();
    AOT_TEST_CHECK(options != RecordedOptions { });
    AOT_TEST_CHECK(options == readSampleOptions());
    AOT_TEST_CHECK(options.hash() == readSampleOptions().hash());

    for (size_t i = 0; i < sizeof(RecordedOptions); ++i) {
        Bytes bytes = std::bit_cast<Bytes>(options);
        bytes[i] ^= 1;
        AOT_TEST_CHECK(std::bit_cast<RecordedOptions>(bytes).hash() != options.hash());
    }
}

static CompileOptions readWords(std::initializer_list<StringView> words)
{
    return readValidOptions(optionsWith(words));
}

static void testAdmittedCodeIsRecorded()
{
    AOT_TEST_CHECK(readWords({ }).recorded.admitted() == Wire::AdmittedCode { });
    AOT_TEST_CHECK(!readWords({ }).admitsUnseenCode());
    AOT_TEST_CHECK(readWords({ "admit=Inspector"_s }).recorded.admitted() == Wire::AdmittedCode::Inspector);
    AOT_TEST_CHECK(readWords({ "admit=Inspector"_s }).admitsUnseenCode());
    for (ASCIILiteral word : { "exclude=EvaluatedText"_s, "exclude=FunctionSource"_s }) {
        AOT_TEST_CHECK(readWords({ word }).recorded.admitted() == Wire::AdmittedCode { });
        AOT_TEST_CHECK(!readWords({ word }).admitsUnseenCode());
    }
}

static void testExcludedFunctionSourceIsRecorded()
{
    AOT_TEST_CHECK(!readWords({ }).recorded.omitsFunctionSource && readWords({ }).recorded.adviceToKeepFunctionSource().isEmpty());
    AOT_TEST_CHECK(!readWords({ "exclude=EvaluatedText"_s }).recorded.omitsFunctionSource && readWords({ "exclude=EvaluatedText"_s }).excludesEvaluatedText());
    CompileOptions withoutSource = readWords({ "exclude=FunctionSource"_s });
    AOT_TEST_CHECK(withoutSource.recorded.omitsFunctionSource && !withoutSource.excludesEvaluatedText() && !withoutSource.recorded.adviceToKeepFunctionSource().isEmpty());
}

static void testInstrumentationAndTestingSwitchesAreRecorded()
{
    AOT_TEST_CHECK(!readWords({ }).recorded.countsBranches && !readWords({ }).recorded.countsArrivals && readWords({ }).recorded.usesExpectedTypes());
    AOT_TEST_CHECK(readWords({ "count-branches"_s }).recorded.countsBranches && !readWords({ "count-branches"_s }).recorded.countsArrivals);
    AOT_TEST_CHECK(readWords({ "count-arrivals"_s }).recorded.countsArrivals && !readWords({ "count-arrivals"_s }).recorded.countsBranches);
    RecordedOptions both = readWords({ "count-branches"_s, "count-arrivals"_s }).recorded;
    AOT_TEST_CHECK(both.countsBranches && both.countsArrivals);

    RecordedOptions ignoring = readWords({ "testing=IgnoreExpectedTypes"_s }).recorded;
    AOT_TEST_CHECK(!ignoring.usesExpectedTypes() && ignoring.has(Wire::TestingSwitch::IgnoreExpectedTypes) && !ignoring.has(Wire::TestingSwitch::DropGuard) && !ignoring.droppedGuard());
    RecordedOptions sabotaged = readWords({ "sabotage=StackNamesNoInnerCell"_s }).recorded;
    AOT_TEST_CHECK(sabotaged.isSabotaged(Wire::Sabotage::StackNamesNoInnerCell) && !sabotaged.isSabotaged(Wire::Sabotage::InnerCellLetGoKeepsItsHeader) && !sabotaged.isSabotaged(Wire::Sabotage::None));
    AOT_TEST_CHECK(readWords({ }).recorded.isSabotaged(Wire::Sabotage::None) && !readWords({ }).recorded.sabotage);
    RecordedOptions dropping = readWords({ "drop-guard=TypeTest:2"_s }).recorded;
    AOT_TEST_CHECK(dropping.has(Wire::TestingSwitch::DropGuard) && dropping.usesExpectedTypes());
    AOT_TEST_CHECK(dropping.droppedGuard() && dropping.droppedGuard()->opcode == Opcode::TypeTest && dropping.droppedGuard()->ordinal == 2);
    AOT_TEST_CHECK(!readWords({ }).recorded.has(Wire::TestingSwitch::FailPredictions) && !readWords({ }).recorded.predictionFailurePeriod && !readWords({ }).recorded.predictionFailureSeed);
    RecordedOptions failing = readWords({ "testing=FailPredictions:7:3"_s }).recorded;
    AOT_TEST_CHECK(failing.has(Wire::TestingSwitch::FailPredictions) && failing.predictionFailurePeriod == 7 && failing.predictionFailureSeed == 3);
}

std::span<const NativeTest> optionWordTests()
{
    static constexpr NativeTest tests[] = {
        { "option-words/set-members"_s, testOptionWordsSetMembers },
        { "option-words/rejected"_s, testOptionWordsRejected },
        { "option-words/listed-names-accepted"_s, testOptionWordsListedNamesAccepted },
        { "option-words/pass-from-wire"_s, testOptionWordsPassFromWire },
        { "option-words/only-passes-disables-other-optional-passes"_s, testOnlyPassesDisablesOtherOptionalPasses },
        { "option-words/only-passes-keeps-disabled-passes"_s, testOnlyPassesKeepsDisabledPasses },
        { "option-words/only-passes-accepted-by-from-wire"_s, testOnlyPassesAcceptedByFromWire },
        { "option-words/only-passes-rejected-words"_s, testOnlyPassesRejectedWords },
        { "option-words/limit-word-follows-can-be-lowered"_s, testLimitWordFollowsCanBeLowered },
        { "option-words/required-passes-stay-enabled"_s, testRequiredPassesStayEnabled },
        { "compile-options/default"_s, testCompileOptionsDefault },
        { "compile-options/invalid-members"_s, testCompileOptionsInvalidMembers },
        { "compile-options/ignored-words"_s, testCompileOptionsIgnoredWords },
        { "compile-options/thread-count-follows-max-workers"_s, testThreadCountFollowsMaxWorkers },
        { "compile-options/follow-can-be-lowered"_s, testWireOptionsFollowCanBeLowered },
        { "recorded-options/switch-range"_s, testSwitchRange },
        { "recorded-options/hash"_s, testRecordedOptionsHash },
        { "recorded-options/admitted-code"_s, testAdmittedCodeIsRecorded },
        { "recorded-options/excluded-function-source"_s, testExcludedFunctionSourceIsRecorded },
        { "recorded-options/instrumentation-and-testing-switches"_s, testInstrumentationAndTestingSwitchesAreRecorded },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
