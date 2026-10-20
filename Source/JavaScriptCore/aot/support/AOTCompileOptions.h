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
using Wire::Instrumentation;
using Wire::MismatchPolicy;
using Wire::OutputOptions;
using Wire::ReferenceWidth;
using Wire::ReportSet;
using Wire::SelectionRuleID;
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

struct RecordedOptions {
    RecordedSwitches switches;
    uint64_t loweredLimitValue { 0 };
    uint64_t targetFeatures { 0 };
    LimitID loweredLimit { LimitID::None };
    uint32_t layoutSeed { 0 };
    uint32_t droppedGuardOrdinal { 0 };
    uint16_t droppedGuardOpcode { 0 };
    bool dropsGuard { false };
    bool validates { false };
    bool countsBranches { false };
    uint8_t architecture { 0 };
    uint8_t referenceWidth { 0 };
    uint8_t mismatchPolicy { 0 };
    uint8_t testing { 0 };
    uint8_t heatOverride { 0 };
    uint8_t admittedCode { 0 };
    uint8_t reserved { 0 };

    bool useWideReferences() const { return referenceWidth == static_cast<uint8_t>(ReferenceWidth::Wide); }
    MismatchPolicy policy() const { return static_cast<MismatchPolicy>(mismatchPolicy); }
    bool has(TestingSwitch flag) const { return testing & static_cast<uint8_t>(flag); }
    bool usesPredictions() const { return !has(TestingSwitch::IgnorePredictions); }
    bool analyzesValueFlow() const { return has(TestingSwitch::AnalyzeValueFlow); }
    HeatOverride heat() const { return static_cast<HeatOverride>(heatOverride); }
    AdmittedCode admitted() const { return static_cast<AdmittedCode>(admittedCode); }
    JS_EXPORT_PRIVATE std::optional<DroppedGuard> droppedGuard() const;

    JS_EXPORT_PRIVATE bool isEnabled(FlowResultKind, uint32_t subject) const;
    JS_EXPORT_PRIVATE bool isEnabledForProgram(FlowResultKind) const;
    JS_EXPORT_PRIVATE bool isEnabledEverywhere(FlowResultKind) const;
    JS_EXPORT_PRIVATE bool isEnabled(PassID, Typed::FunctionID) const;
    JS_EXPORT_PRIVATE bool isEnabled(SelectionRuleID, Typed::FunctionID) const;
    JS_EXPORT_PRIVATE bool isEnabled(B3::PolicySwitch, Typed::FunctionID) const;

    JS_EXPORT_PRIVATE uint64_t hash() const;
    JS_EXPORT_PRIVATE bool isCompatibleWithProcess() const;

    friend bool operator==(const RecordedOptions&, const RecordedOptions&) = default;

private:
    bool isInSwitchRange(std::optional<uint32_t> id) const;
};

static_assert(sizeof(RecordedOptions) == 104);
static_assert(std::is_trivially_copyable_v<RecordedOptions>);
static_assert(std::has_unique_object_representations_v<RecordedOptions>);
static_assert(numberOfEnumerators<AdmittedCode> <= 8 * sizeof(RecordedOptions::admittedCode));

struct LoggingOptions {
    bool logsPhaseTimes { false };
    bool dumpsB3Graph { false };
    bool dumpsDisassembly { false };
    UTF8CString functionToDump;
    UTF8CString mapFilePath;
    uint8_t dumpedForms { 0 };
    std::optional<PassID> dumpedPass;

    bool dumps(Typed::IRForm form) const { return dumpedForms & (1u << static_cast<unsigned>(form)); }

    JS_EXPORT_PRIVATE static LoggingOptions fromJSCOptions();
};

struct CompileOptions {
    RecordedOptions recorded;
    LimitValues limits { hostArchitecture() };
    std::array<uint8_t, 16> executableIdentity { };
    double sizeClassProgression { 0 };
    unsigned threadCount { 1 };
    uint32_t flowOrderSeed { 0 };
    CompileAction action { CompileAction::Compile };
    bool verifiesAfterEveryPass { false };
    bool verifiesAfterEveryStage { false };
    bool validatesBackEnd { false };
    bool validatesReferences { false };
    bool validatesReferenceGuards { false };
    bool checksFlowFixpoint { false };
    ReportSet reports { };
    ExcludedCode excludedCode { };
    OutputOptions output { };
    LoggingOptions logging;

    bool isVerifyOnly() const { return action == CompileAction::VerifyOnly; }
    bool collectsDeclines() const { return hasFlag(reports, ReportSet::Declines); }
    bool collectsAllocationRemarks() const { return hasFlag(reports, ReportSet::Allocations); }
    bool admitsUnseenCode() const { return recorded.admitted() != AdmittedCode { }; }
    bool admitsRuntimeCode() const { return hasFlag(recorded.admitted(), AdmittedCode::RuntimeCode); }
    bool excludesEvaluatedText() const { return hasFlag(excludedCode, ExcludedCode::EvaluatedText); }
    bool obfuscates() const { return hasFlag(output, OutputOptions::Obfuscate); }
    bool writesCodeSizeMap() const { return !logging.mapFilePath.isNull(); }

    JS_EXPORT_PRIVATE static CompileOptions fromWire(const Wire::CompileOptions&, Diagnostics&);
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
