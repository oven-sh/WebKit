/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTArrivals.h"
#include "AOTCompileFunctions.h"
#include "AOTCompileOptions.h"
#include "AOTFacts.h"
#include "AOTHostFunctionSources.h"
#include "AOTMechanisms.h"
#include "AOTPlan.h"
#include "AOTSweep.h"
#include "AOTTypeArena.h"
#include "AOTWalkNotes.h"
#include <memory>
#include <optional>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT {

class RegExpSummaries;

namespace Typed {

class Walker;
struct KeptBetweenUnits;

struct EndsWalkerInRoom {
    void operator()(Walker*) const;

    bool* roomIsTaken { nullptr };
};
using WalkerInRoom = std::unique_ptr<Walker, EndsWalkerInRoom>;

class FormatPatternPlans;
class ScratchRealm;

inline constexpr uint16_t everySourceOfProofs = (1u << numberOfEnumerators<ProofSource>) - 1;
constexpr uint16_t bitOfSourceOfProofs(ProofSource source) { return static_cast<uint16_t>(1u << static_cast<unsigned>(source)); }
constexpr bool isEnforced(ProofSource source)
{
    switch (source) {
    case ProofSource::ResultOfCall:
    case ProofSource::EntryParameter:
    case ProofSource::Field:
    case ProofSource::SlotOfScope:
    case ProofSource::Variable:
    case ProofSource::Capture:
    case ProofSource::ResultOfOtherCall:
        return true;
    case ProofSource::Contents:
    case ProofSource::OtherOperation:
    case ProofSource::Test:
    case ProofSource::RememberedFact:
    case ProofSource::HoistedCall:
    case ProofSource::Lifetime:
        return false;
    }
    return false;
}
template<typename Switches>
uint16_t sourcesOfProofsCheckedUnder(Switches testing)
{
    if (AOT::hasFlag(testing, Wire::TestingSwitch::CheckProofsAtRunTime))
        return everySourceOfProofs;
    uint16_t atEntries = bitOfSourceOfProofs(ProofSource::EntryParameter) | bitOfSourceOfProofs(ProofSource::ResultOfCall);
    if (AOT::hasFlag(testing, Wire::TestingSwitch::CheckProofsAtEntriesAndFields))
        return atEntries | bitOfSourceOfProofs(ProofSource::Field);
    return AOT::hasFlag(testing, Wire::TestingSwitch::CheckProofsAtEntries) ? atEntries : 0;
}

template<typename Switches>
bool checksBoundsUnder(Switches testing)
{
    return AOT::hasFlag(testing, Wire::TestingSwitch::CheckBoundsAtRunTime) || AOT::hasFlag(testing, Wire::TestingSwitch::CheckProofsAtRunTime);
}

struct WalkOptions {
    bool takesNotes { false };
    bool verifiesCode { false };
    bool hidesNames { false };
    Mechanisms mechanisms { allMechanisms };
    bool checksProofs { false };
    uint16_t sourcesOfProofsChecked { 0 };
    bool checksBounds { false };
    bool shouldAssumeIndexesInBounds { false };
    bool shouldVerifyAssumedBounds { false };
    bool shouldDropLengthCheckedReads { false };
    bool shouldAssumeInvariantLoopBounds { false };
    bool shouldAssumeInvariantArrayStorage { false };
    bool shouldMisidentifyHoistedReadOperands { false };
    bool shouldAssumeUInt32Dividends { false };
    bool shouldMisroundLoopBounds { false };
    bool shouldAssumeGuardsAndFacts { false };
    bool checksHoistedCalls { false };
    bool shouldAssumeInlineBuiltinsPure { false };
    bool shouldAssumeNoNaNStores { false };
    bool shouldSkipCapacityChecks { false };
    bool shouldSkipLengthChecks { false };
    bool shouldIgnoreLengthStores { false };
    bool shouldIgnorePrototypeReads { false };
    bool shouldAssumeEnumeratorCurrent { false };
    bool shouldIgnoreAlternativeOverloads { false };
    bool shouldIgnoreFreshArrayEscapes { false };
    bool shouldIgnoreLoopArrayStores { false };
    bool shouldOverstateBoundArgumentCount { false };
    bool shouldSkipBoundsProof { false };
    bool shouldMisfoldBranchInSweep { false };
    bool shouldDropEdgeInSweep { false };
    bool handlerEdgesTakeWhatStandsIn { false };
    bool everyClosureStandsBehindCopies { false };
    bool shouldAssumeCalleesRetainNothing { false };
    bool forcesStraightCompilation { false };
    bool checksEffects { false };
    bool checksWalk { false };
    bool replaysFinalLoopVisits { false };
    bool repeatsFittingLoopVisits { false };
    bool shouldDisableLaterUnitSpeculation { false };
    uint32_t understatedEffects { 0 };
    bool ignoresBranchHints { false };
    bool shufflesBlockFrequencies { false };
    bool ignoresExpectedTypes { false };
    bool shufflesExpectedTypes { false };
    bool shouldFailSpeculations { false };
    bool shouldIgnoreIterationEffects { false };
    bool shouldUnderstateLoopEffects { false };
    bool netOfEffectsWatchesTurns { false };
    bool shufflesChoices { false };
    bool judgesInt32Promises { true };
    uint32_t shuffleSeed { 0 };
    WalkMode forcedWalkMode { WalkMode::AsBuilt };
    uint32_t forcedWalkModePeriod { 0 };
    uint32_t loopVisitAbortPeriod { 0 };
    uint32_t loopVisitAbortPhase { 0 };
    bool shouldAssumeLaterValuesUnaliased { false };
    bool shouldAssumeLocalArrayReceivers { false };
    bool shouldAssumeNumericAddends { false };
    bool shouldAssumeArithmeticUsers { false };
    bool shouldAssumePermanentlyPristineIterables { false };
    bool shouldAssumePristineDestructuring { false };
    bool shouldAssumeIterationsRestartable { false };
    bool shouldAllowMarkedObjects { false };
    bool shouldSkipPairGuard { false };
    bool shouldAssumePristineReceivers { false };
    bool shouldSkipStorageChecks { false };
    bool shouldAssumeVariablesInitialized { false };
    bool shouldAssumeSlotsInitialized { false };
    bool shouldIgnoreVariableChecks { false };
    bool shouldDropLastCase { false };
    bool shouldSkipSuperMethodGuard { false };
    bool shouldSkipHierarchyGuards { false };
    bool shouldSkipClassGuards { false };
    bool shouldIgnoreSiblingPrototypes { false };
    bool shouldSkipPrototypeChecks { false };
    bool shouldAssumeLayoutPrototype { false };
    bool shouldSkipSuperclassCheck { false };
    bool shouldSkipSuperPrototypeGuard { false };
    bool shouldIgnoreLoopEscapes { false };
    bool shouldIgnoreLoopWrites { false };
    bool shouldIgnoreLoopUses { false };
    bool shouldAssumeInvariantOperands { false };
    PrintStream* explainsClosedAccessesTo { nullptr };
    const HostStringEncoders* stringEncoders { nullptr };
};

struct WalkInputs {
    const Program& program;
    const Facts& facts;
    const Plan& plan;
    WalkOptions options;
    Arrivals& arrivals;
    RegExpSummaries* regExpSummaries { nullptr };
    FormatPatternPlans* formatPatternPlans { nullptr };
    ScratchRealm* scratchRealm { nullptr };
};

WalkOptions walkOptionsOf(const BuildContext&);

struct FunctionsNamedByFieldsOfOperation {
    template<typename IDType, typename... Role>
    void operator()(IDType id, Role... role)
    {
        if constexpr (std::is_same_v<IDType, FunctionID>) {
            if (id)
                functions.append(id);
        } else if constexpr (std::is_same_v<IDType, ModuleID>) {
            if (id && ((role == ModuleReferenceKind::Target) || ...) && program.at(id).body)
                functions.append(program.at(id).body);
        }
    }
    const Program& program;
    Vector<FunctionID, 4>& functions;
};

template<typename OperationOrTerminator>
void listFunctionsUsedBy(const Program& program, const BodyIndex& index, const OperationOrTerminator& user, bool namesFunctionsInFields, bool usesOperands, VariableID variableRead, Vector<FunctionID, 4>& functions)
{
    functions.shrink(0);
    if (namesFunctionsInFields)
        user.forEachReference(FunctionsNamedByFieldsOfOperation { program, functions });
    if (variableRead && program.at(variableRead).function)
        functions.append(program.at(variableRead).function);
    if (!usesOperands)
        return;
    user.forEachOperand([&](ValueID made, uint32_t) {
        while (made && !index.isParameter(made) && inputPassThroughOpcodes.contains(index.value(made).opcode()))
            made = index.value(made).operand(0);
        if (made && !index.isParameter(made) && index.value(made).opcode() == Opcode::NewClosure)
            functions.append(index.value(made).template as<Opcode::NewClosure>().function());
    });
}

inline void listFunctionsUsedByOperation(const Program& program, const BodyIndex& index, const BodyValue& value, Vector<FunctionID, 4>& functions)
{
    Opcode opcode = value.opcode();
    listFunctionsUsedBy(program, index, value, opcode != Opcode::NewClosure, !inputPassThroughOpcodes.contains(opcode), opcode == Opcode::GetModuleVariable ? value.variable() : VariableID(), functions);
}

struct CompileInputs {
    WalkInputs walk;
    CodeGenerationInputs generation;
};

WalkNotes notesOfSweptCode(TypeReader, const Code&, bool notesResolvedOperations);

class CompileWorker {
    WTF_MAKE_NONCOPYABLE(CompileWorker);
public:
    CompileWorker(const CompileInputs&, BuildContext&, TypeTable&);
    std::optional<CompiledCode> compileStraight(CodeUnit);
    std::optional<CompiledCode> compileEntryByHand(CodeUnit);
    ~CompileWorker();

    std::optional<CompiledCode> compile(CodeUnit);
    bool walkAndSweep(CodeUnit);
    bool walkAndSweep(CodeUnit, WalkMode);

    bool compile(CodeUnit unit, Vector<CompiledCode, 2>& finished) { return append(compile(unit), finished); }

    CodeGenerationTotals& totals() LIFETIME_BOUND { return m_totals; }
    Vector<WalkNotes>& notes() LIFETIME_BOUND { return m_notes; }
    Vector<FunctionID, 8>& functionsThatUnitMayEnterByValue() LIFETIME_BOUND { return m_functionsThatUnitMayEnterByValue; }
    Walker& walker() LIFETIME_BOUND { return *m_walker; }

    struct InstructionsOfSteps {
        uint64_t walkAndSweep { 0 };
        uint64_t emitToEncode { 0 };
    };
    const InstructionsOfSteps& instructionsOfSteps() const LIFETIME_BOUND { return m_instructionsOfSteps; }

private:
    enum class RoomOfFormerWalker : bool { IsLeft, IsTaken };
    WalkerInRoom makeWalker(RoomOfFormerWalker);
    static bool append(std::optional<CompiledCode>&& code, Vector<CompiledCode, 2>& finished)
    {
        if (!code)
            return false;
        finished.append(WTF::move(*code));
        return true;
    }

    const CompileInputs& m_inputs;
    BuildContext& m_build;
    TypeArena m_types;
    std::unique_ptr<Machine::Arena> m_arena;
    std::unique_ptr<ScratchRealm> m_scratchRealm;
    WalkInputs m_walkInputs;
    std::unique_ptr<KeptBetweenUnits> m_keptBetweenUnits;
    WalkerInRoom m_walker;
    RoomsOfSweep m_roomsOfSweep;
    CodeGenerationTotals m_totals;
    Vector<WalkNotes> m_notes;
    Vector<FunctionID, 8> m_functionsThatUnitMayEnterByValue;
    InstructionsOfSteps m_instructionsOfSteps;
};

} } } // namespace JSC::AOT::Typed

#include "AOTWalk.h"

#endif // ENABLE(AOT)
