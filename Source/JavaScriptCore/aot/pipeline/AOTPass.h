/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTDerivedFunctions.h"
#include "AOTDiagnostics.h"
#include "AOTIR.h"
#include "AOTLimits.h"
#include "AOTLosses.h"
#include "AOTPassID.h"
#include "AOTPassList.h"
#include "AOTTypePropagation.h"
#include <optional>
#include <span>
#include <wtf/OptionSet.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class CallGraph;
class CallingConventions;
class EffectSummaries;
class FlowResults;
class FunctionAnalyses;
class Heat;
class InliningSummaries;
class Placement;
class SlotPlan;
class StringArena;
class TypeArena;

enum class PassScope : uint8_t { Program, Function };
enum class PassNecessity : uint8_t { Required, Optional };
enum class PassOutcome : uint8_t { Unchanged, Changed };
enum class InvalidatedAnalyses : uint8_t { None, Values, All };

constexpr PassOutcome operator|(PassOutcome a, PassOutcome b) { return a == PassOutcome::Changed ? a : b; }
constexpr PassOutcome& operator|=(PassOutcome& a, PassOutcome b) { return a = a | b; }

enum class ProgramResult : uint32_t {
    Flow = 1 << 0,
    CallGraph = 1 << 1,
    EffectSummaries = 1 << 2,
    Heat = 1 << 3,
    InliningSummaries = 1 << 4,
    CallGroups = 1 << 5,
    CallingConventions = 1 << 6,
    FrameAllocation = 1 << 7,
    EnvironmentReuse = 1 << 8,
    LiteralStrings = 1 << 9,
    ConstantCollections = 1 << 10,
    SlotPlan = 1 << 11,
    RegExpRequests = 1 << 12,
    StackChecks = 1 << 13,
    Placement = 1 << 14,
    SiteCaches = 1 << 15,
};

enum class FunctionSchedule : uint8_t { Type, Simplify, Prepare, Optimize, Select, Represent };
enum class ScheduleMode : uint8_t { InPlace, OnCopies };
enum class ScheduledFunctions : uint8_t { OneAtATime, All, Reached };

inline constexpr unsigned numberOfFunctionSchedules = static_cast<unsigned>(FunctionSchedule::Represent) + 1;

struct OptimizationInputs {
    const FlowResults& flow;
    const CallGraph& callGraph;
    const Heat& heat;
    const InliningSummaries& inlining;
    const EffectSummaries& effects;
    DerivedFunctions* derivedFunctions { nullptr };
    const CallingConventions* callingConventions { nullptr };
    const SlotPlan* slots { nullptr };
    const Placement* placement { nullptr };
};

struct FunctionPassContext {
    const Program& program;
    const OptimizationInputs& inputs;
    FunctionID id;
    Function& function;
    FunctionAnalyses& analyses;
    TypeArena& types;
    StringArena& strings;
    BuildContext& build;
    TypedDefinitions typedDefinitions { };
    FieldReadTypes fieldReadTypes { };

    FunctionID derivedFunction(const DerivedFunctionKey&);

    double heatOf(BlockID);
    bool runsAtMostOnce(BlockID);

    Subject subject() const;
    Subject subject(BlockID) const;
    Subject subject(ValueID) const;

    uint64_t limit(LimitID) const;
    bool exceeds(LimitID, uint64_t amount, ValueID = { });
    GaveUp gaveUp(LimitID, ValueID = { });
    void addDecline(Wire::DeclineKind, ValueID, const DeclineReason& = { }, LimitID = LimitID::None, uint32_t count = 0);
};

using FunctionPass = PassOutcome (*)(FunctionPassContext&);

#define AOT_DECLARE_FUNCTION_PASS(id, function) PassOutcome function(FunctionPassContext&);
FOR_EACH_AOT_FUNCTION_PASS(AOT_DECLARE_FUNCTION_PASS)
#undef AOT_DECLARE_FUNCTION_PASS

struct PassDescriptor {
    PassID id;
    PassScope scope;
    PassNecessity necessity;
    IRForm firstForm;
    IRForm lastForm;
    std::optional<IRForm> produces;
    OptionSet<ProgramResult> inputs;
    OptionSet<ProgramResult> outputs;
    OptionSet<ProgramResult> invalidatedResults;
    InvalidatedAnalyses invalidatedAnalyses;
    bool readsOtherBodies;
    bool derivesFunctions;
    bool countsOwnChanges;
    bool isVerifiedAfterNextPass;
    FunctionPass run;
};

const PassDescriptor& descriptorOf(PassID);

inline constexpr OptionSet<ProgramResult> resultsAboutBodies { ProgramResult::CallGraph, ProgramResult::EffectSummaries, ProgramResult::InliningSummaries };
inline constexpr OptionSet<ProgramResult> resultsComputedOnDemand = resultsAboutBodies | ProgramResult::Heat;
inline constexpr OptionSet<ProgramResult> simplifyInputs { ProgramResult::Flow, ProgramResult::EffectSummaries };

struct ScheduleStep {
    enum class Kind : uint8_t { RunPass, RunSchedule, BeginFixpoint, EndFixpoint };

    static constexpr ScheduleStep run(PassID pass) { return { Kind::RunPass, pass }; }
    static constexpr ScheduleStep include(FunctionSchedule schedule) { return { Kind::RunSchedule, PassID::None, schedule }; }
    static constexpr ScheduleStep beginFixpoint(LimitID limit = LimitID::MaxOptimizationRounds) { return { Kind::BeginFixpoint, PassID::None, FunctionSchedule::Simplify, limit }; }
    static constexpr ScheduleStep endFixpoint() { return { Kind::EndFixpoint }; }

    Kind kind;
    PassID pass { PassID::None };
    FunctionSchedule schedule { FunctionSchedule::Simplify };
    LimitID limit { LimitID::None };
};

struct ScheduleDescriptor {
    FunctionSchedule id;
    ASCIILiteral name;
    ScheduleMode mode;
    ScheduledFunctions functions;
    IRForm form;
    std::span<const ScheduleStep> steps;
    OptionSet<ProgramResult> inputs;
};

const ScheduleDescriptor& descriptorOf(FunctionSchedule);

inline std::span<const ScheduleStep> stepsOf(FunctionSchedule schedule) { return descriptorOf(schedule).steps; }
inline OptionSet<ProgramResult> inputsOf(FunctionSchedule schedule) { return descriptorOf(schedule).inputs; }

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
