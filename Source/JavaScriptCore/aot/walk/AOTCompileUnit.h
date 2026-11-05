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
class ScratchRealm;

inline constexpr uint16_t everySourceOfProofs = (1u << numberOfEnumerators<ProofSource>) - 1;
constexpr uint16_t bitOfSourceOfProofs(ProofSource source) { return static_cast<uint16_t>(1u << static_cast<unsigned>(source)); }
constexpr bool isEnforced(ProofSource source)
{
    switch (source) {
    case ProofSource::ResultOfCall:
        return true;
    case ProofSource::EntryParameter:
    case ProofSource::Field:
    case ProofSource::Variable:
    case ProofSource::Capture:
    case ProofSource::Contents:
    case ProofSource::ResultOfOtherCall:
    case ProofSource::OtherOperation:
    case ProofSource::Test:
    case ProofSource::WhatIsRemembered:
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

struct WalkOptions {
    bool takesNotes { false };
    bool verifiesCode { false };
    bool hidesNames { false };
    Mechanisms mechanisms { allMechanisms };
    bool checksProofs { false };
    uint16_t sourcesOfProofsChecked { 0 };
    bool testsEveryPosition { false };
    bool takesPositionsToLieInside { false };
    // TestingSwitch::NoCalleeKeepsWhatItIsHanded. No option of a build.
    bool takesCalleesToKeepNothing { false };
    bool writesBodiesStraight { false };
    bool checksEffects { false };
    uint32_t rowThatSaysLess { 0 };
    PrintStream* explainsClosedAccessesTo { nullptr };
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
    void operator()(IDType id, Role...)
    {
        if constexpr (std::is_same_v<IDType, FunctionID>) {
            if (id)
                functions.append(id);
        }
    }
    Vector<FunctionID, 4>& functions;
};

template<typename OperationOrTerminator>
void listFunctionsUsedBy(const Program& program, const BodyIndex& index, const OperationOrTerminator& user, bool namesFunctionsInFields, bool usesOperands, VariableID variableRead, Vector<FunctionID, 4>& functions)
{
    functions.shrink(0);
    if (namesFunctionsInFields)
        user.forEachReference(FunctionsNamedByFieldsOfOperation { functions });
    if (variableRead && program.at(variableRead).function)
        functions.append(program.at(variableRead).function);
    if (!usesOperands)
        return;
    user.forEachOperand([&](ValueID made, uint32_t) {
        while (made && !index.isParameter(made) && inputOperationsThatHandOn.contains(index.value(made).opcode()))
            made = index.value(made).operand(0);
        if (made && !index.isParameter(made) && index.value(made).opcode() == Opcode::NewClosure)
            functions.append(index.value(made).template as<Opcode::NewClosure>().function());
    });
}

inline void listFunctionsUsedByOperation(const Program& program, const BodyIndex& index, const BodyValue& value, Vector<FunctionID, 4>& functions)
{
    Opcode opcode = value.opcode();
    listFunctionsUsedBy(program, index, value, opcode != Opcode::NewClosure, !inputOperationsThatHandOn.contains(opcode), opcode == Opcode::GetModuleVariable ? value.variable() : VariableID(), functions);
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
    ~CompileWorker();

    std::optional<CompiledCode> compile(CodeUnit);
    bool walkAndSweep(CodeUnit);

    bool compile(CodeUnit unit, Vector<CompiledCode, 2>& finished) { return append(compile(unit), finished); }

    CodeGenerationTotals& totals() LIFETIME_BOUND { return m_totals; }
    Vector<WalkNotes>& notes() LIFETIME_BOUND { return m_notes; }
    Walker& walker() LIFETIME_BOUND { return *m_walker; }

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
    TypeArena m_types;
    std::unique_ptr<Machine::Arena> m_arena;
    std::unique_ptr<ScratchRealm> m_scratchRealm;
    WalkInputs m_walkInputs;
    std::unique_ptr<Walker> m_walker;
    Sweep m_sweep;
    CodeGenerationTotals m_totals;
    Vector<WalkNotes> m_notes;
    InstructionsOfSteps m_instructionsOfSteps;
};

} } } // namespace JSC::AOT::Typed

#include "AOTWalk.h"

#endif // ENABLE(AOT)
