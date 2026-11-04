/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTArrivals.h"
#include "AOTCompileFunctions.h"
#include "AOTFacts.h"
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
class FormatPatternPlans;
class LeavePoints;
class ScratchRealm;

// Compile: walk > sweep > emit > allocate > encode, one unit from start to finish on one thread. A pure function of (function, facts, plan).

inline constexpr uint16_t everySourceOfProofs = (1u << numberOfEnumerators<ProofSource>) - 1;
constexpr uint16_t bitOfSourceOfProofs(ProofSource source) { return static_cast<uint16_t>(1u << static_cast<unsigned>(source)); }
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

struct WalkOptions {
    // --explain.
    bool takesNotes { false };
    // verifyCode() and verifyTypesOfCode() behind each walk. Neither changes what is compiled.
    bool verifiesCode { false };
    // CompileOptions::obfuscates(): an error that the code makes names no function.
    bool hidesNames { false };
    // Set only by tests and by testaot. No option of a build.
    Mechanisms mechanisms { allMechanisms };
    // TestingSwitch::CheckProofsAtRunTime. No option of a build.
    bool checksProofs { false };
    // A bit for each ProofSource that is checked. TestingSwitch::CheckProofsAtRunTime: all. CheckProofsAtEntries, CheckProofsAtEntriesAndFields: two, three. checksProofs says whether there is any.
    uint16_t sourcesOfProofsChecked { 0 };
    // TestingSwitch::WritesBodiesStraight: every body that has a straight form takes it. No option of a build.
    bool writesBodiesStraight { false };
    // Set only by `testaot why-closed`: explainClosedAccess() writes there.
    PrintStream* explainsClosedAccessesTo { nullptr };
};

// Made once, in front of Compile. Every thread reads it and nothing changes it, but for what the walks leave in `arrivals` and note in `leavePoints`.
struct WalkInputs {
    const Program& program;
    const Facts& facts;
    const Plan& plan;
    WalkOptions options;
    Arrivals& arrivals;
    // BuildContext::regExpSummaries(), which Image reads too. Null: the walk claims nothing of the text of any RegExp.
    RegExpSummaries* regExpSummaries { nullptr };
    // BuildContext::formatPatternPlans(). Null: no formatter is laid down as a pattern.
    FormatPatternPlans* formatPatternPlans { nullptr };
    // BuildContext::leavePoints(). Null: no fast version is left.
    LeavePoints* leavePoints { nullptr };
    // Of one worker: CompileWorker hands its Walker a copy of these inputs that names its own. Null: nothing is asked of the engine.
    ScratchRealm* scratchRealm { nullptr };
};

WalkOptions walkOptionsOf(const BuildContext&);

struct CompileInputs {
    WalkInputs walk;
    CodeGenerationInputs generation;
};

// AOTWalkNotes.cpp. The walk itself notes nothing: the notes of a unit are read off its swept Code, in the order of its records, by the columns of the schema and the engine fields that the walk chose.
//   an access or a call, by accessClass: a slot or a direct call is resolved, one by name or by value is generic, and one that testsItsOwnGuess() is a test that rejoins
//   an operator, by its OperatorForm . a record of the family Guard that stands for no access of the program is a test that rejoins
//   an allocation that the sweep kept is an object built, and one that it dropped an object that stayed virtual
// `notesResolvedOperations`: ReportSet::AccessCounts was asked for. No other report reads them.
WalkNotes notesOfSweptCode(TypeReader, const Code&, bool notesResolvedOperations);

// What a thread of Compile owns: its arenas, one Walker, one Sweep. Nothing of it outlives the unit but the bytes and the notes.
class CompileWorker {
    WTF_MAKE_NONCOPYABLE(CompileWorker);
public:
    CompileWorker(const CompileInputs&, BuildContext&, TypeTable&);
    // codegen/AOTStraightEmitter.h. Nothing: the unit is walked.
    std::optional<CompiledCode> compileStraight(CodeUnit);
    ~CompileWorker();

    // One unit gives one piece of code. Nothing after a failure, which the build's diagnostics hold.
    std::optional<CompiledCode> compile(CodeUnit);
    // Walk and sweep alone. walker().code() holds the Code until the next call.
    bool walkAndSweep(CodeUnit);

    // The same, appended.
    bool compile(CodeUnit unit, Vector<CompiledCode, 2>& finished) { return append(compile(unit), finished); }

    CodeGenerationTotals& totals() LIFETIME_BOUND { return m_totals; }
    Vector<WalkNotes>& notes() LIFETIME_BOUND { return m_notes; }
    Walker& walker() LIFETIME_BOUND { return *m_walker; }

    // What this thread has retired inside walkAndSweep() and inside generateCode(), over all its units.
    // Counted where LoggingOptions::logsPhaseTimes is set and InstructionCounts::ofCurrentThread() answers. Zero elsewhere.
    struct InstructionsOfSteps {
        uint64_t walkAndSweep { 0 };
        uint64_t emitToEncode { 0 };
    };
    const InstructionsOfSteps& instructionsOfSteps() const LIFETIME_BOUND { return m_instructionsOfSteps; }

private:
    static bool append(std::optional<CompiledCode>&& code, Vector<CompiledCode, 2>& finished)
    {
        if (!code)
            return false;
        finished.append(WTF::move(*code));
        return true;
    }

    const CompileInputs& m_inputs;
    BuildContext& m_build;
    // ArenaKeeps::UntilReset.
    TypeArena m_types;
    std::unique_ptr<Machine::Arena> m_arena;
    std::unique_ptr<ScratchRealm> m_scratchRealm;
    // CompileInputs::walk, with the realm of this worker.
    WalkInputs m_walkInputs;
    std::unique_ptr<Walker> m_walker;
    Sweep m_sweep;
    CodeGenerationTotals m_totals;
    Vector<WalkNotes> m_notes;
    InstructionsOfSteps m_instructionsOfSteps;
};

} } } // namespace JSC::AOT::Typed

// Whoever asks walker() goes on to ask the Walker. It takes WalkInputs, so it comes behind them.
#include "AOTWalk.h"

#endif // ENABLE(AOT)
