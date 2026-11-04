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
using Wire::TestingSwitch;

template<typename Flags>
constexpr bool hasFlag(Flags set, Flags flag)
{
    return static_cast<uint32_t>(set) & static_cast<uint32_t>(flag);
}

// A hardened run time loads no code but that of the image, whatever the image was built to allow.
#if defined(BUN_HARDENED)
constexpr bool runsOnlyCodeOfImage = true;
#else
constexpr bool runsOnlyCodeOfImage = false;
#endif

constexpr ImageForm imageFormOfThisRuntime()
{
#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
    return ImageForm::PositionIndependent;
#else
    return ImageForm::Absolute;
#endif
}

// ImageHeader holds one, byte for byte, and the members behind it are found by offset.
struct RecordedOptions {
    uint64_t targetFeatures { 0 };
    bool validates { false };
    // Built with --no-compile-function-source.
    bool omitsFunctionSource { false };
    uint8_t architecture { 0 };
    uint8_t referenceWidth { 0 };
    uint8_t admittedCode { 0 };
    uint8_t imageForm { static_cast<uint8_t>(imageFormOfThisRuntime()) };
    uint16_t reserved { 0 };

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
    // Options::aotMapFilePath(). Null: the build writes no map of the image.
    UTF8CString mapFilePath;
};

struct CompileOptions {
    RecordedOptions recorded;
    std::array<uint8_t, 16> executableIdentity { };
    // A bit for each RemovableFeature that the executable is built without, whether or not the program uses it.
    std::array<uint64_t, 8> removedFeatures { };
    // A bit for each RemovableFeature that the executable is built without unless a use of it is FeatureUseKind::Named.
    std::array<uint64_t, 8> featuresKeptOnlyWhenNamed { };
    double sizeClassProgression { 0 };
    unsigned threadCount { 1 };
    CompileAction action { CompileAction::Compile };
    // CheckSet::Validate or Options::verifyAOTMachineCode(): the checks of machine/AOTMachineChecker.h run. With the option alone they run on the code that an ordinary build makes.
    bool verifiesMachineCode { false };
    // Debug and ASAN builds alone have these checks. Elsewhere each is a constant, so what hangs on it is not in the binary.
#if ASSERT_ENABLED || ASAN_ENABLED
    bool verifiesAfterEveryStage { false };
    bool verifiesProgram { false };
    bool validatesReferences { false };
    bool validatesReferenceGuards { false };
#else
    static constexpr bool verifiesAfterEveryStage = false;
    static constexpr bool verifiesProgram = false;
    static constexpr bool validatesReferences = false;
    static constexpr bool validatesReferenceGuards = false;
#endif
    ReportSet reports { };
    TestingSwitch testing { };
    ExcludedCode excludedCode { };
    OutputOptions output { };
    LoggingOptions logging;

    bool isVerifyOnly() const { return action == CompileAction::VerifyOnly; }
    bool isCheckOnly() const { return action == CompileAction::CheckOnly; }
    // Code that nobody has read comes in wherever unknown code may run, names every binding and holds every object of the heap.
    bool admitsUnseenCode() const { return hasFlag(recorded.admitted(), AdmittedCode::Inspector); }
    // A test run: what a test replaces on a built-in object is unlocked, and the build is told of what it still finds locked.
    bool admitsReplacedBuiltins() const { return hasFlag(recorded.admitted(), AdmittedCode::ReplacedBuiltins); }
    bool excludesEvaluatedText() const { return hasFlag(excludedCode, ExcludedCode::EvaluatedText); }
    bool excludesHostEvaluatedText() const { return hasFlag(excludedCode, ExcludedCode::HostEvaluatedText); }
    bool excludesForeignCalls() const { return hasFlag(excludedCode, ExcludedCode::ForeignCalls); }
    bool excludesForeignCallsOfUnseenCode() const { return hasFlag(excludedCode, ExcludedCode::ForeignCallsOfUnseenCode); }
    // The image holds every module of the library that the run can load, so none runs from its text.
    bool excludesLibraryText() const { return hasFlag(excludedCode, ExcludedCode::LibraryText); }
    bool excludesCodeOutsideImage() const { return runsOnlyCodeOfImage || hasFlag(excludedCode, ExcludedCode::CodeOutsideImage); }
    bool obfuscates() const { return hasFlag(output, OutputOptions::Obfuscate); }
    // The host runs the image in one realm of the VM after another, each with an instance of its own, and what it carries from one realm to the next may hold a function of the earlier one.
    bool runsInSeveralRealms() const { return hasFlag(output, OutputOptions::RunsInSeveralRealms); }
    // `bun run --aot`, `bun test --aot`: no executable. The build folds no read of BuiltinHostObject::lockedNames there, and the host locks none of them.
    bool runsInPlaceOfFiles() const { return hasFlag(output, OutputOptions::RunsInPlaceOfFiles); }

    JS_EXPORT_PRIVATE static CompileOptions fromWire(const Wire::CompileOptions&, Diagnostics&);
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
