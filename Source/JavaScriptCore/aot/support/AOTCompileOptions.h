/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include "AOTLimits.h"
#include "AOTPassID.h"
#include "AOTSchemaEnums.h"
#include "AOTWire.h"
#include "JSExportMacros.h"
#include <array>
#include <optional>
#include <type_traits>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/CString.h>

namespace JSC {

namespace B3 {
enum class PolicySwitch : uint8_t;
}

namespace AOT {

class Diagnostics;

using Wire::AdmittedCode;
using Wire::CheckSet;
using Wire::CompileAction;
using Wire::ExcludedCode;
using Wire::FlowResultKind;
using Wire::HeatOverride;
using Wire::ImageForm;
using Wire::Instrumentation;
using Wire::MismatchPolicy;
using Wire::OutputOptions;
using Wire::ReferenceWidth;
using Wire::ReportSet;
using Wire::SelectionRuleID;
using Wire::Sabotage;
using Wire::TestingSwitch;

template<typename Flags>
constexpr bool hasFlag(Flags set, Flags flag)
{
    return static_cast<uint32_t>(set) & static_cast<uint32_t>(flag);
}

struct DroppedGuard {
    Typed::Opcode opcode;
    uint32_t ordinal;
};

struct RecordedSwitches {
    uint64_t disabledFlowResults { 0 };
    std::array<uint64_t, 4> disabledPasses { };
    uint64_t disabledSelectionRules { 0 };
    uint64_t disabledBackEndSwitches { 0 };
    uint32_t firstID { 0 };
    uint32_t idCount { 0 };

    friend bool operator==(const RecordedSwitches&, const RecordedSwitches&) = default;
};

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

struct RecordedOptions {
    RecordedSwitches switches;
    uint64_t loweredLimitValue { 0 };
    uint64_t targetFeatures { 0 };
    LimitID loweredLimit { LimitID::None };
    uint32_t layoutSeed { 0 };
    uint32_t droppedGuardOrdinal { 0 };
    uint16_t droppedGuardOpcode { 0 };
    bool validates { false };
    bool countsBranches { false };
    bool countsArrivals { false };
    // Built with --no-compile-function-source.
    bool omitsFunctionSource { false };
    uint8_t architecture { 0 };
    uint8_t referenceWidth { 0 };
    uint8_t mismatchPolicy { 0 };
    uint8_t heatOverride { 0 };
    uint16_t testing { 0 };
    uint8_t admittedCode { 0 };
    uint8_t sabotage { 0 };
    uint8_t imageForm { static_cast<uint8_t>(imageFormOfThisRuntime()) };
    uint8_t unused { 0 };
    uint16_t predictionFailurePeriod { 0 };
    uint16_t predictionFailureSeed { 0 };

    bool useWideReferences() const { return referenceWidth == static_cast<uint8_t>(ReferenceWidth::Wide); }
    ImageForm form() const { return static_cast<ImageForm>(imageForm); }
    MismatchPolicy policy() const { return static_cast<MismatchPolicy>(mismatchPolicy); }
    bool has(TestingSwitch flag) const { return testing & static_cast<uint16_t>(flag); }
    bool isSabotaged(Sabotage part) const { return sabotage == static_cast<uint8_t>(part); }
    bool usesExpectedTypes() const { return !has(TestingSwitch::IgnoreExpectedTypes); }
    HeatOverride heat() const { return static_cast<HeatOverride>(heatOverride); }
    AdmittedCode admitted() const { return static_cast<AdmittedCode>(admittedCode); }
    JS_EXPORT_PRIVATE std::optional<DroppedGuard> droppedGuard() const;

    JS_EXPORT_PRIVATE bool isEnabled(FlowResultKind, uint32_t subject) const;
    JS_EXPORT_PRIVATE bool isEnabledForProgram(FlowResultKind) const;
    JS_EXPORT_PRIVATE bool isEnabledEverywhere(FlowResultKind) const;
    JS_EXPORT_PRIVATE bool isEnabled(PassID, Typed::FunctionID) const;
    JS_EXPORT_PRIVATE bool isEnabled(SelectionRuleID, Typed::FunctionID) const;
    JS_EXPORT_PRIVATE bool isEnabled(B3::PolicySwitch, Typed::FunctionID) const;

    JS_EXPORT_PRIVATE bool isCompatibleWithProcess() const;

    friend bool operator==(const RecordedOptions&, const RecordedOptions&) = default;

private:
    bool isInSwitchRange(std::optional<uint32_t> id) const;
};

static_assert(sizeof(RecordedOptions) == 112);
static_assert(std::is_trivially_copyable_v<RecordedOptions>);
static_assert(std::has_unique_object_representations_v<RecordedOptions>);
static_assert(numberOfEnumerators<AdmittedCode> <= 8 * sizeof(RecordedOptions::admittedCode));

struct LoggingOptions {
    bool logsPhaseTimes { false };
    bool dumpsB3Graph { false };
    bool dumpsDisassembly { false };
    UTF8CString functionToDump;
    UTF8CString mapFilePath;
    std::optional<PassID> dumpedPass;
};

struct CompileOptions {
    RecordedOptions recorded;
    LimitValues limits { hostArchitecture() };
    std::array<uint8_t, 16> executableIdentity { };
    // A bit for each RemovableFeature that the executable is built without, whether or not the program uses it.
    std::array<uint64_t, 8> removedFeatures { };
    double sizeClassProgression { 0 };
    unsigned threadCount { 1 };
    CompileAction action { CompileAction::Compile };
    bool verifiesAfterEveryPass { false };
    bool verifiesAfterEveryStage { false };
    bool verifiesProgram { false };
    bool validatesBackEnd { false };
    bool validatesReferences { false };
    bool checksClaimsToBackEnd { false };
    bool checksMovedLoads { false };
    // Once the flow analysis is at rest every rule runs once more over the solved sets. Whatever that adds is an update that the solve has lost, and an error.
    bool checksFlowFixpoint { false };
    // A function that is reached and cannot get past its first block but by a throw of its own is dead code that nobody has called dead, or a value there has lost a source: an error.
    bool checksThatFunctionsGetPastEntry { false };
    bool checksThatUsedValuesAreListed { false };
    bool validatesReferenceGuards { false };
    ReportSet reports { };
    ExcludedCode excludedCode { };
    OutputOptions output { };
    LoggingOptions logging;

    bool isVerifyOnly() const { return action == CompileAction::VerifyOnly; }
    bool isCheckOnly() const { return action == CompileAction::CheckOnly; }
    bool collectsDeclines() const { return hasFlag(reports, ReportSet::Declines); }
    bool collectsAllocationRemarks() const { return hasFlag(reports, ReportSet::Allocations); }
    // Code comes in at any place and at any time, so nothing holds for the program as a whole.
    bool admitsUnseenCode() const { return recorded.admitted() != AdmittedCode { }; }
    bool excludesEvaluatedText() const { return hasFlag(excludedCode, ExcludedCode::EvaluatedText); }
    bool excludesCodeOutsideImage() const { return runsOnlyCodeOfImage || hasFlag(excludedCode, ExcludedCode::CodeOutsideImage); }
    bool obfuscates() const { return hasFlag(output, OutputOptions::Obfuscate); }
    bool writesCodeSizeMap() const { return !logging.mapFilePath.isNull(); }

    JS_EXPORT_PRIVATE static CompileOptions fromWire(const Wire::CompileOptions&, Diagnostics&);
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
