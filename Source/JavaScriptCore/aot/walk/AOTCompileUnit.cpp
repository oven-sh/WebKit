/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCompileUnit.h"

#if ENABLE(AOT)

#include "AOTCurrentWork.h"
#include "AOTDiagnostics.h"
#include "AOTEffectsDeclared.h"
#include "AOTMachineArena.h"
#include "AOTNumbering.h"
#include "AOTPhaseTimes.h"
#include "AOTRepresentation.h"
#include "AOTRepresentationRules.h"
#include "AOTResultRules.h"
#include "AOTScratchRealm.h"
#include "AOTStatistics.h"
#include "AOTTypeArena.h"
#include "AOTTypeIDs.h"
#include "AOTBodyIndex.h"
#include "AOTEffectsOfOperations.h"
#include "AOTFacts.h"
#include "AOTOpcodeSets.h"
#include <algorithm>
#include <optional>
#include <span>
#include <wtf/SetForScope.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

WalkOptions walkOptionsOf(const BuildContext& build)
{
#if ASSERT_ENABLED || ASAN_ENABLED
    constexpr bool isCheckingBuild = true;
#else
    constexpr bool isCheckingBuild = false;
#endif
    const AOT::CompileOptions& options = build.options();
    return {
        .takesNotes = AOT::hasFlag(options.reports, Wire::ReportSet::Flow) || AOT::hasFlag(options.reports, Wire::ReportSet::AccessCounts) || AOT::hasFlag(options.reports, Wire::ReportSet::Allocations),
        .verifiesCode = isCheckingBuild || build.validates(),
        .hidesNames = options.obfuscates(),
        .checksProofs = !!sourcesOfProofsCheckedUnder(options.testing),
        .sourcesOfProofsChecked = sourcesOfProofsCheckedUnder(options.testing),
        .writesBodiesStraight = AOT::hasFlag(options.testing, Wire::TestingSwitch::WritesBodiesStraight),
    };
}

// The key words that hold a TypeID or a PropertyKeyID, the ids that a TypeArena makes. Every other word is a program, Facts or Plan id, or plain content.
static std::span<const uint32_t> arenaIDsInUnitMentionKey(NumberSpace space, std::span<const uint32_t> key)
{
    switch (space) {
    case NumberSpace::Name:
    case NumberSpace::KeyCell:
    case NumberSpace::PropertyRun:
    case NumberSpace::Type:
        return key;
    case NumberSpace::GlobalBinding:
        return key.first(1);
    case NumberSpace::SiteCache:
        return key.size() < 2 ? std::span<const uint32_t>() : key.subspan(key.size() - 2, 1);
    case NumberSpace::NameOfText:
    case NumberSpace::String:
    case NumberSpace::StringCell:
    case NumberSpace::BuiltinCell:
    case NumberSpace::BigInt:
    case NumberSpace::RequireEdge:
    case NumberSpace::Variable:
    case NumberSpace::CardOfVariable:
    case NumberSpace::CardOfWord:
    case NumberSpace::ModuleValue:
    case NumberSpace::ModuleNamespace:
    case NumberSpace::ModuleBuiltinCell:
    case NumberSpace::FilledFiles:
    case NumberSpace::DataCell:
    case NumberSpace::DataStorage:
    case NumberSpace::DataElements:
    case NumberSpace::RegExpLiteralCell:
    case NumberSpace::TemplateObjectCell:
    case NumberSpace::ArrayProfile:
    case NumberSpace::EnvironmentHandedBack:
    case NumberSpace::ClassGuard:
    case NumberSpace::ClassRank:
    case NumberSpace::ClosureFunction:
    case NumberSpace::ClosureCell:
    case NumberSpace::Function:
    case NumberSpace::Allocation:
    case NumberSpace::Request:
    case NumberSpace::AtomOfComparedText:
    case NumberSpace::ShownString:
    case NumberSpace::AtomOfString:
    case NumberSpace::MethodSelector:
    case NumberSpace::RegExp:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool unitMentionsThreadArenaID(const CodeBody& body)
{
    for (const Mention& mention : body.mentions) {
        std::span<const uint32_t> key = AOT::keyOf(mention, body.mentionWords.span());
        for (uint32_t word : arenaIDsInUnitMentionKey(mention.space, key)) {
            // TypeID and PropertyKeyID share one encoding, so one check covers both.
            if (liesInThread(TypeID::fromBits(word)))
                return true;
        }
    }
    return false;
}

static void reportFailureOfCompileUnit(BuildContext& build, CodeUnit unit, InternalErrorKind kind, const String& detail)
{
    build.diagnostics().add(InternalError { .kind = kind, .function = unit.function, .detail = detail });
}

static std::optional<uint64_t> instructionsOfCompileThreadSoFar(const BuildContext& build)
{
    if (!build.options().logging.logsPhaseTimes) [[likely]]
        return std::nullopt;
    return InstructionCounts::ofCurrentThread();
}

static bool verifyCodeOfCompileUnit(const WalkInputs& inputs, BuildContext& build, TypeArena& types, const PositionTypes& positions, const Code& code)
{
    InternalErrorKind kindOfBrokenRule { InternalErrorKind::OpcodeNotInForm };
    String brokenRule = verifyCode(CodeTextContext { inputs.program, types, &inputs.plan }, code, &kindOfBrokenRule);
    if (brokenRule.isNull()) {
        kindOfBrokenRule = InternalErrorKind::TypeOutOfDate;
        brokenRule = verifyTypesOfCode(TypingContext { inputs.program, types, positions }, inputs.plan.conventions(), inputs.facts, inputs.arrivals, code);
    }
    if (brokenRule.isNull())
        return true;
    reportFailureOfCompileUnit(build, code.unit(), kindOfBrokenRule, brokenRule);
    return false;
}

// The operations of the other units stand at no place that the program wrote.
static bool notesAreTakenOfCompileUnit(CodeRole role)
{
    switch (role) {
    case CodeRole::Body:
    case CodeRole::CoroutineBody:
        return true;
    // Whoever reads the notes of a function reads those of its fast version.
    case CodeRole::SlowBody:
    case CodeRole::ParameterGuard:
    case CodeRole::GenericCallEntry:
    case CodeRole::GenericConstructEntry:
    case CodeRole::AllocatingConstructEntry:
    case CodeRole::ModuleStart:
    case CodeRole::BoundFunction:
    case CodeRole::GenericEntryOfBoundFunction:
    case CodeRole::TableEntry:
    case CodeRole::TableLookup:
    case CodeRole::Serializer:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static WalkInputs withRealmOfCompileWorker(const WalkInputs& inputs, ScratchRealm* realm)
{
    WalkInputs ofWorker = inputs;
    ofWorker.scratchRealm = realm;
    return ofWorker;
}

CompileWorker::CompileWorker(const CompileInputs& inputs, BuildContext& build, TypeTable& types)
    : m_inputs(inputs)
    , m_build(build)
    , m_types(types, ArenaKeeps::UntilReset)
    , m_arena(makeUniqueWithoutFastMallocCheck<Machine::Arena>())
    , m_scratchRealm(makeUniqueWithoutFastMallocCheck<ScratchRealm>())
    , m_walkInputs(withRealmOfCompileWorker(inputs.walk, m_scratchRealm.get()))
    , m_walker(makeUniqueWithoutFastMallocCheck<Walker>(m_walkInputs, m_types, inputs.walk.arrivals))
{
}

CompileWorker::~CompileWorker() = default;

bool CompileWorker::walkAndSweep(CodeUnit unit)
{
    const WalkInputs& inputs = m_inputs.walk;
    CurrentWorkScope work(inputs.program, unit.function, "walk"_s);

    m_types.reset();
    Code& code = m_walker->code();
    if (!m_walker->walk(unit)) {
        m_build.diagnostics().append(m_walker->takeReports());
        reportFailureOfCompileUnit(m_build, unit, InternalErrorKind::Unknown, m_walker->failure());
        return false;
    } else if (uint32_t guesses = m_walker->numberOfGuessesOpened()) {
        m_build.statistics().count(StatisticID::TestsThatRejoin, guesses);
        m_build.statistics().count(StatisticID::LoopsBuiltTwice, m_walker->numberOfLoopsBuiltTwice());
        m_build.statistics().count(StatisticID::TurnsThatBeginAgain, m_walker->numberOfTurnsThatBeginAgain());
        m_build.statistics().count(StatisticID::EmptySlotsTakenAsCases, m_walker->numberOfEmptySlotsTakenAsCases());
    }
    if (uint32_t assignments = m_walker->numberOfAssignmentsWrittenOut())
        m_build.statistics().count(StatisticID::AssignmentsWrittenOutFieldByField, assignments);
    if (uint32_t joins = m_walker->numberOfJoinsWalkedForEachWay())
        m_build.statistics().count(StatisticID::JoinsWalkedOnceForEachWay, joins);
    if (inputs.options.checksProofs) {
        static_assert(static_cast<unsigned>(StatisticID::ProofsThatNoTestTells) - static_cast<unsigned>(StatisticID::ProofsCheckedOfEntryParameters) == numberOfEnumerators<ProofSource>);
        for (unsigned source = 0; source < numberOfEnumerators<ProofSource>; ++source)
            m_build.statistics().count(static_cast<StatisticID>(static_cast<unsigned>(StatisticID::ProofsCheckedOfEntryParameters) + source), m_walker->numberOfProofsChecked(static_cast<ProofSource>(source)));
        m_build.statistics().count(StatisticID::ProofsThatNoTestTells, m_walker->numberOfProofsThatNoTestTells());
    }
    RELEASE_ASSERT(code.unit() == unit);
    m_build.diagnostics().append(m_walker->takeReports());

    if (inputs.options.verifiesCode) {
        work.setStep("verify"_s);
        if (!verifyCodeOfCompileUnit(inputs, m_build, m_types, m_walker->positions(), code))
            return false;
    }

    work.setStep("sweep"_s);
    SweepTotals swept = m_sweep.run(SweepInputs { inputs.program, inputs.plan, m_types }, code);
    m_build.statistics().count(StatisticID::OperationsSwept, swept.operationsSwept);
    m_build.statistics().count(StatisticID::ValuesWalked, m_walker->numberOfValuesWalked());
    m_build.statistics().count(StatisticID::OperationsBuilt, code.numberOfValues());
    m_build.statistics().count(StatisticID::CallsInlined, code.inlineFrames().size() - 1);

    if (inputs.options.takesNotes && notesAreTakenOfCompileUnit(unit.role)) {
        WalkNotes notes = notesOfSweptCode(m_types, code, AOT::hasFlag(m_build.options().reports, Wire::ReportSet::AccessCounts));
        m_build.statistics().count(StatisticID::ValuesInDouble, notes.numberOfValuesInDouble);
        if (!notes.notes.isEmpty())
            m_notes.append(WTF::move(notes));
    }
    return true;
}

// A body that runs at most once, without a loop, gains nothing from a walk where each of its operations does the same work whatever is known of its operands. No report, check or kind of build has a word in it.
static bool isWrittenStraight(const WalkInputs& inputs, CodeUnit unit, const BodyIndex& index)
{
    static constexpr OpcodeSet operationsThatTypesMakeNoCheaper = inputOperationsThatHandOn | OpcodeSet {
        Opcode::Undefined, Opcode::Null, Opcode::Boolean, Opcode::Integer, Opcode::Double, Opcode::String, Opcode::BigInt, Opcode::Builtin, Opcode::NativeBinding,
        Opcode::GetModuleVariable, Opcode::PutModuleVariable, Opcode::TypeTest, Opcode::ThrowModuleFailure, Opcode::EvaluateModule, Opcode::RecordRequire, Opcode::RequireModule,
        Opcode::NewClosure, Opcode::NewRegExp, Opcode::NewClass, Opcode::NewClassToken, Opcode::ResolveHeritage, Opcode::DefineMethod, Opcode::FinishClass,
    };
    if (unit.role != CodeRole::SlowBody && unit.role != CodeRole::Body)
        return false;
    if (inputs.options.writesBodiesStraight)
        return true;
    // What a guard turns away was expected by nobody. Whether straight code has a form for the call of a built-in hangs on the build's table of built-ins, which a test run fills otherwise than an executable: the two make the same code.
    if (unit.role != CodeRole::Body)
        return !index.opcodes().contains(Opcode::Builtin);
    if (!inputs.facts.runsAtMostOnce(unit.function) && !inputs.program.at(unit.function).isModuleBody())
        return false;
    return !index.hasLoops() && (index.opcodes() | operationsThatTypesMakeNoCheaper) == operationsThatTypesMakeNoCheaper;
}

std::optional<CompiledCode> CompileWorker::compileStraight(CodeUnit unit)
{
    const BodyIndex* index = m_inputs.walk.facts.bodies().find(unit.function);
    if (!index || !isWrittenStraight(m_inputs.walk, unit, *index) || !hasStraightForm(m_inputs.generation, unit, *index))
        return std::nullopt;
    Code& code = m_walker->code();
    code.reset(unit);
    std::optional<CompiledCode> compiled = generateStraightCode(CodeGenerationWorker { m_build, m_types, *m_arena }, m_inputs.generation, code, *index, m_totals);
    if (!compiled || !compiled->unit) {
        m_arena->reset();
        return std::nullopt;
    }
    // AOTArrivals.h: every call that may enter a function leaves something.
    if (const LinkTables& linked = m_inputs.walk.facts.linked(); !linked.calls.isEmpty()) {
        static const OpcodeSet opcodesThatMayRunOtherCode = opcodesWhere([](Opcode opcode) {
            return declaredBoundInTable(opcode).mayRunOtherCode() || operandsMayRunCode(opcode);
        });
        if (index->opcodes().intersects(opcodesThatMayRunOtherCode)) {
            index->body().forEachBlock([&](BodyBlock block) {
                for (BodyValue value : block.values()) {
                    if (!opcodesThatMayRunOtherCode.contains(value.opcode()))
                        continue;
                    if (FunctionID callee = linked.calleeAt(unit.function, value.id()))
                        m_inputs.walk.arrivals.leaveAnything(callee);
                }
            });
        }
    }
    m_build.statistics().count(StatisticID::UnitsCompiled);
    m_build.statistics().count(StatisticID::UnitsWrittenStraight);
    return compiled;
}

std::optional<CompiledCode> CompileWorker::compile(CodeUnit unit)
{
    CurrentWorkScope work(m_inputs.walk.program, unit.function, "walk"_s);

    m_arena->reset();
    if (std::optional<CompiledCode> straight = compileStraight(unit))
        return straight;
    std::optional<uint64_t> instructionsAtStart = instructionsOfCompileThreadSoFar(m_build);
    if (!walkAndSweep(unit))
        return std::nullopt;
    std::optional<uint64_t> instructionsAfterSweep = instructionsOfCompileThreadSoFar(m_build);

    work.setStep("emit"_s);
    const Code& swept = m_walker->code();
    CompiledCode code = generateCode(CodeGenerationWorker { m_build, m_types, *m_arena }, m_inputs.generation, swept, m_totals);
    std::optional<uint64_t> instructionsAfterEncoding = instructionsOfCompileThreadSoFar(m_build);
    if (instructionsAtStart && instructionsAfterSweep && instructionsAfterEncoding) {
        m_instructionsOfSteps.walkAndSweep += *instructionsAfterSweep - *instructionsAtStart;
        m_instructionsOfSteps.emitToEncode += *instructionsAfterEncoding - *instructionsAfterSweep;
    }
    if (!code.unit) {
        RELEASE_ASSERT(m_build.diagnostics().hasErrors());
        return std::nullopt;
    }
    RELEASE_ASSERT(code.unit == unit);
    if (unitMentionsThreadArenaID(code.body)) {
        reportFailureOfCompileUnit(m_build, unit, InternalErrorKind::Unknown, "the code mentions an id from the thread's arena, which the code outlives"_s);
        return std::nullopt;
    }

    Statistics& statistics = m_build.statistics();
    statistics.count(StatisticID::UnitsCompiled);
    if (m_inputs.walk.program.at(unit.function).role == FunctionRole::Parser)
        statistics.count(StatisticID::ParsersCreated);
    if (unit.role == CodeRole::Serializer)
        statistics.count(StatisticID::SerializersCreated);
    if (uint32_t selected = m_walker->numberOfTypedResultsSelected())
        statistics.count(StatisticID::TypedResultsSelected, selected);
    if (uint32_t slices = m_walker->numberOfSlicesMadeOfElementsInSight())
        statistics.count(StatisticID::SlicesMadeOfElementsInSight, slices);
    for (const Walker::CountOfUnit& count : m_walker->countsOfUnit())
        statistics.count(count.statistic, count.amount);
    return code;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
