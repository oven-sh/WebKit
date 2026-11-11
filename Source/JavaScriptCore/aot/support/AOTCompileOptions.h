/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTSchemaEnums.h"
#include "AOTWire.h"
#include "JSExportMacros.h"
#include <array>
#include <type_traits>
#include <wtf/text/CString.h>

namespace JSC {

namespace AOT {

class Diagnostics;

using Wire::AdmittedCode;
using Wire::CheckSet;
using Wire::CompileAction;
using Wire::ExcludedCode;
using Wire::ImageForm;
using Wire::OutputOptions;
using Wire::ReferenceWidth;
using Wire::ReportSet;
using Wire::FaultInjection;
using Wire::TestingSwitch;

template<typename Flags>
constexpr bool hasFlag(Flags set, Flags flag)
{
    using Word = std::underlying_type_t<Flags>;
    return static_cast<Word>(set) & static_cast<Word>(flag);
}

static_assert(hasFlag(allMembersOf<TestingSwitch>.back(), allMembersOf<TestingSwitch>.back()));

enum class DrawnFor : uint8_t { BlockFrequency, LoopMark, ExpectedType, OtherExpectedType, StepOfGuessesThatFail, NodeOfLink, CopyInLine, ParameterGuard, LoopBuiltTwice, PlaceInFrontOfLoop, GuessOutsideLoops, WalkMode, ResultInParts };

constexpr uint32_t drawBySeed(uint32_t seed, DrawnFor question, uint32_t first, uint32_t second)
{
    uint64_t drawn = (static_cast<uint64_t>(seed) << 8 | static_cast<uint64_t>(question)) * 0x9E3779B97F4A7C15ull;
    drawn ^= (static_cast<uint64_t>(first) << 32 | second) * 0xC2B2AE3D27D4EB4Full;
    drawn ^= drawn >> 29;
    drawn *= 0xBF58476D1CE4E5B9ull;
    drawn ^= drawn >> 32;
    drawn *= 0x94D049BB133111EBull;
    return static_cast<uint32_t>(drawn >> 32);
}

#if defined(BUN_HARDENED)
constexpr bool isHardenedBuild = true;
#else
constexpr bool isHardenedBuild = false;
#endif

constexpr ImageForm imageFormOfThisRuntime()
{
#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
    return ImageForm::PositionIndependent;
#else
    return ImageForm::Absolute;
#endif
}

struct RecordedOptions {
    uint64_t targetFeatures { 0 };
    bool validates { false };
    bool omitsFunctionSource { false };
    uint8_t architecture { 0 };
    uint8_t referenceWidth { 0 };
    uint8_t admittedCode { 0 };
    uint8_t imageForm { static_cast<uint8_t>(imageFormOfThisRuntime()) };
    bool verifiesFastPathAnswers { false };
    uint8_t reserved { 0 };

    bool useWideReferences() const { return referenceWidth == static_cast<uint8_t>(ReferenceWidth::Wide); }
    ImageForm form() const { return static_cast<ImageForm>(imageForm); }
    AdmittedCode admitted() const { return static_cast<AdmittedCode>(admittedCode); }

    JS_EXPORT_PRIVATE bool isCompatibleWithProcess() const;

    friend bool operator==(const RecordedOptions&, const RecordedOptions&) = default;
};

static_assert(sizeof(RecordedOptions) == 16);
static_assert(std::is_trivially_copyable_v<RecordedOptions>);
static_assert(std::has_unique_object_representations_v<RecordedOptions>);
static_assert(numberOfEnumerators<AdmittedCode> <= 8 * sizeof(RecordedOptions::admittedCode));

struct LoggingOptions {
    bool logsPhaseTimes { false };
    UTF8CString mapFilePath;
};

struct CompileOptions {
    RecordedOptions recorded;
    std::array<uint8_t, 16> executableIdentity { };
    std::array<uint64_t, 8> removedFeatures { };
    std::array<uint64_t, 8> featuresRetainedByName { };
    double sizeClassProgression { 0 };
    unsigned threadCount { 1 };
    CompileAction action { CompileAction::Compile };
    bool verifiesCode { false };
    bool verifiesMachineCode { false };
    bool checksFlowFixpoint { false };
    bool replaysFinalLoopVisits { false };
    bool repeatsFittingLoopVisits { false };
    bool checksFlowMonotonicity { false };
    ReportSet reports { };
    TestingSwitch testing { };
    FaultInjection injectedFaults { };
    uint32_t understatedEffects { 0 };
    uint32_t shuffleSeed { 0 };
    uint32_t speculationFailurePeriod { 0 };
    uint32_t forcedWalkMode { 0 };
    uint32_t forcedWalkModePeriod { 0 };
    uint32_t loopVisitAbortPeriod { 0 };
    uint32_t loopVisitAbortPhase { 0 };
    ExcludedCode excludedCode { };
    OutputOptions output { };
    LoggingOptions logging;

    bool admitsUnseenCode() const { return hasFlag(recorded.admitted(), AdmittedCode::Inspector); }
    bool admitsReplacedBuiltins() const { return hasFlag(recorded.admitted(), AdmittedCode::ReplacedBuiltins); }
    bool excludesEvaluatedText() const { return hasFlag(excludedCode, ExcludedCode::EvaluatedText); }
    bool excludesHostEvaluatedText() const { return hasFlag(excludedCode, ExcludedCode::HostEvaluatedText); }
    bool excludesForeignCalls() const { return hasFlag(excludedCode, ExcludedCode::ForeignCalls); }
    bool excludesForeignCallsOfUnseenCode() const { return hasFlag(excludedCode, ExcludedCode::ForeignCallsOfUnseenCode); }
    bool excludesCodeOutsideImage() const { return isHardenedBuild || hasFlag(excludedCode, ExcludedCode::CodeOutsideImage); }
    bool excludesLibraryText() const { return hasFlag(excludedCode, ExcludedCode::LibraryText); }
    bool excludesFeaturesNamedAtRunTime() const { return hasFlag(excludedCode, ExcludedCode::FeaturesNamedAtRunTime); }
    bool obfuscates() const { return hasFlag(output, OutputOptions::Obfuscate); }
    bool hidesFunctionNames() const { return obfuscates() && !hasFlag(output, OutputOptions::KeepsFunctionNames); }
    bool runsInSeveralRealms() const { return hasFlag(output, OutputOptions::RunsInSeveralRealms); }

    JS_EXPORT_PRIVATE static CompileOptions fromWire(const Wire::CompileOptions&, Diagnostics&);
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
