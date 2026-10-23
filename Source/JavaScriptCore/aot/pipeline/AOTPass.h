/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTDerivedFunctions.h"
#include "AOTDiagnostics.h"
#include "AOTEffects.h"
#include "AOTIR.h"
#include "AOTLimits.h"
#include "AOTLosses.h"
#include "AOTOpenSites.h"
#include "AOTPassID.h"
#include "AOTPassList.h"
#include "AOTRefusedSites.h"
#include "AOTTypePropagation.h"
#include "AOTVector.h"
#include <array>
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

enum class FunctionAnalysis : uint16_t;
using AnalysisSet = OptionSet<FunctionAnalysis>;

using PassSet = EnumeratorBitSet<PassID, numberOfPasses>;

ChangeLog& changeLogOf(FunctionAnalyses&);

enum class PassScope : uint8_t { Program, Function };
enum class PassNecessity : uint8_t { Required, Optional };
enum class PassOutcome : uint8_t { Unchanged, Changed };
enum class InvalidatedAnalyses : uint8_t { None, Values, All };
// What a program pass says of the functions that it edits and of those that see a record that it changes.
enum class EditedFunctions : uint8_t { DoAnything, DoNoMore, DoLess };

constexpr PassOutcome operator|(PassOutcome a, PassOutcome b) { return a == PassOutcome::Changed ? a : b; }
constexpr PassOutcome& operator|=(PassOutcome& a, PassOutcome b) { return a = a | b; }

enum class ProgramResult : uint32_t {
    Flow = 1 << 0,
    CallGraph = 1 << 1,
    EffectSummaries = 1 << 2,
    Heat = 1 << 3,
    InliningSummaries = 1 << 4,
    CallingConventions = 1 << 5,
    FrameAllocation = 1 << 6,
    EnvironmentReuse = 1 << 7,
    LiteralStrings = 1 << 8,
    ConstantCollections = 1 << 9,
    SlotPlan = 1 << 10,
    RegExpRequests = 1 << 11,
    StackChecks = 1 << 12,
    Placement = 1 << 13,
    SiteCaches = 1 << 14,
};

enum class FunctionSchedule : uint8_t { Type, Simplify, Prepare, Resolve, Optimize, Select, Represent };
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

    OptimizationInputs withEffects(const EffectSummaries& otherEffects LIFETIME_BOUND) const { return { flow, callGraph, heat, inlining, otherEffects, derivedFunctions, callingConventions, slots, placement }; }
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
    PendingTyping pendingTyping { };
    TypingMemo typingMemo { &pendingTyping };
    RefusedSites refusedSites { };
    // What has been edited in the function since the analyses were set up. Function::edit() and its neighbors write it.
    ChangeLog& changes { changeLogOf(analyses) };
    OpenSites openSites { };
    size_t numberOfBlocksWhenSinkingGaveUp { 0 };
    // Where in a schedule the running pass stands: in the rounds of a fixpoint, at a step for what was left for later, or neither.
    bool isInRounds { false };
    bool isBehindRounds { false };
    bool hasUsesAwaitingExpansion { false };
    // Set by a pass in a run that changes the function: the analyses that describe the function as it is behind the run. The runner empties it in front of every run and drops what the row of the pass names, less these.
    AnalysisSet keptAnalyses { };
    Vector<uint64_t> tracesOfAllocationsNotWorthSinking;
    // Set by a pass whose run changed the function: running it again right away would change and report nothing. The runner clears it before every run.
    bool reachedFixpoint { false };
    PassSet passesLeftIdle { };
    PassSet passesThatCanWait { };
    bool foldingAskedForUses { true };
    bool isLastStepOfSchedule { false };
    bool mayDecideTestsAcrossBlocks { true };
    bool lookedOnlyNearby { false };
    // Set by the planner of a site where its answer follows from more than the site, the types of its operands and the program: from where the site stands, or from who else uses a value.
    bool planAsksAboutPlace { false };

    FunctionID derivedFunction(const DerivedFunctionKey&);

    // Good until the function is edited.
    std::span<const ValueSite> sitesIn(SiteList list) LIFETIME_BOUND { return openSites.sitesIn(list, changes, analyses, function); }
    // The functor may not edit the function.
    void forEachSiteIn(SiteList list, NOESCAPE const Invocable<void(const ValueSite&)> auto& functor)
    {
        for (const ValueSite& site : sitesIn(list))
            functor(site);
    }
    bool hasRequiredTailCall() { return openSites.hasRequiredTailCall(changes, analyses, function); }
    bool hasConstantListSource() { return openSites.hasConstantListSource(changes, analyses, function); }

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
    EditedFunctions editedFunctions;
    Effects effectsAddedByEdits;
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
    // The pass does here what it has left for the time when the rounds in front of it have come to rest.
    static constexpr ScheduleStep runBehindRounds(PassID pass) { return { Kind::RunPass, pass, FunctionSchedule::Simplify, LimitID::None, true, true }; }
    static constexpr ScheduleStep include(FunctionSchedule schedule) { return { Kind::RunSchedule, PassID::None, schedule }; }
    // All but the last step of the schedule, which renumbers blocks and values. Every analysis is made again behind that step, so a schedule that goes on leaves it to its own end.
    static constexpr ScheduleStep includeWithoutCompacting(FunctionSchedule schedule) { return { Kind::RunSchedule, PassID::None, schedule, LimitID::None, false }; }
    static constexpr ScheduleStep beginFixpoint(LimitID limit = LimitID::MaxOptimizationRounds) { return { Kind::BeginFixpoint, PassID::None, FunctionSchedule::Simplify, limit }; }
    static constexpr ScheduleStep endFixpoint() { return { Kind::EndFixpoint }; }

    Kind kind;
    PassID pass { PassID::None };
    FunctionSchedule schedule { FunctionSchedule::Simplify };
    LimitID limit { LimitID::None };
    bool compacts { true };
    bool isBehindRounds { false };
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
