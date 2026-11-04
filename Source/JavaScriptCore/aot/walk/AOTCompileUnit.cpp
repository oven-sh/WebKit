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
        .checksProofs = AOT::hasFlag(options.testing, Wire::TestingSwitch::CheckProofsAtRunTime),
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

static CalleesOfIndirectCalls calleesOfIndirectCallsKeptBySweep(const Code& code)
{
    CalleesOfIndirectCalls callees;
    for (const Code::CalleesOfIndirectCall& entry : code.calleesOfIndirectCalls()) {
        if (entry.call && !code.isNeeded(entry.call))
            continue;
        if (entry.list)
            callees.lists.append(entry.list.index());
        else if (entry.function)
            callees.functions.append(entry.function);
        else
            ++callees.numberOfCallsThatMayEnterAnyClosure;
    }
    std::ranges::sort(callees.lists);
    removeRepeatedElements(callees.lists);
    std::ranges::sort(callees.functions);
    removeRepeatedElements(callees.functions);
    return callees;
}

namespace {

// A closure of the function may be made, and its word names this unit, but the build has followed the function and found no indirect call of it. So only a fault of the compiler comes here, and it ends in an error that says so.
// One thing comes here by right: no row lists the constructor of a class for a call, which throws in front of the body.
// Walker::walk() alone makes the Walker ready for a unit, and it goes on to the function. So the unit is built straight into the Walker's Code, each record by the rules of its row.
class ClosedGenericEntryOfCompileUnit {
public:
    ClosedGenericEntryOfCompileUnit(const WalkInputs& inputs, Walker& walker, CodeUnit unit)
        : m_walker(walker)
        , m_program(inputs.program)
        , m_conventions(inputs.plan.conventions())
        , m_function(inputs.program.at(unit.function))
        , m_typing { inputs.program, walker.types(), walker.positions() }
        , m_forms { inputs.program, inputs.plan, walker.types(), unit }
        , m_types(walker.types())
        , m_code(walker.code())
        , m_unit(unit)
        , m_origin { m_function.position, 0, false, false }
        , m_name(inputs.options.hidesNames ? StringID() : m_function.name)
    {
        RELEASE_ASSERT(isGenericEntry(unit.role) && !isBoundRole(unit.role) && m_conventions.indirectCallersAreAllListed(unit.function));
    }

    void build()
    {
        m_code.reset(m_unit);
        m_block = m_code.addBlock();
        m_code.addEntry(m_block);

        ArgumentLocations locations = m_conventions.locationsOf(m_program, m_unit);
        Vector<ValueID, 12> parameters;
        for (const ArgumentLocation& location : locations.parameters)
            parameters.append(m_code.appendParameter(m_block, widestTypeOf(location.representation), location.representation, m_origin));
        m_code.setParameters(m_block, parameters.span());
        RELEASE_ASSERT(parameters.size() >= firstArgumentInRegisters);
        m_closure = parameters[0];
        m_thisValue = parameters[1];

        GenericEntryPlan entry = m_conventions.genericEntry(m_unit);
        if (m_unit.role == CodeRole::GenericCallEntry && isClassConstructorOf(m_function.kind)) {
            // Constructions enter another unit, so whoever comes here calls.
            if (entry.constructEntry) {
                refuseCall();
                return;
            }
            if (entry.takesNewTarget)
                refuseUnlessConstructing(locations.convention == EntryConvention::GenericBuffer ? readFirstArgumentOfBuffer() : parameters[firstArgumentInRegisters]);
        }
        endInError();
    }

private:
    // Behind the closure and `this`.
    static constexpr size_t firstArgumentInRegisters = 2;

    struct FormsOfOperands {
        Vector<TypeID, 8> types;
        Vector<Representation, 8> representations;

        OperandTypes operandTypes() const LIFETIME_BOUND { return { types.span() }; }
        OperandForms operandForms() const LIFETIME_BOUND { return { types.span(), representations.span() }; }
    };

    FormsOfOperands formsOfOperandsOf(const DraftWords& draft) const
    {
        FormsOfOperands forms;
        for (Reference operand : draft.operands()) {
            forms.types.append(operand ? m_code.at(operand.id()).type : TypeID());
            forms.representations.append(operand ? m_code.at(operand.id()).representation : Representation::Unchosen);
        }
        return forms;
    }

    ValueID appendInFormsGiven(Draft& draft)
    {
        FormsOfOperands forms = formsOfOperandsOf(draft);
        TypeID proven = resultTypeOf(m_typing, draft, forms.operandTypes());
        if (draft.representation == Representation::Unchosen)
            draft.representation = resultRepresentationOf(m_forms, draft, forms.operandForms(), proven);
        return m_code.append(m_block, draft, proven, declaredEffectsOfOperation(m_program, draft.opcode, draft));
    }

    void bringOperandsIntoWantedForms(auto& draft)
    {
        FormsOfOperands forms = formsOfOperandsOf(draft);
        for (uint32_t index = 0; index < draft.numberOfOperands(); ++index) {
            if (!draft.operand(index))
                continue;
            Representation held = forms.representations[index];
            Representation wanted = wantedRepresentationOf(m_forms, draft, forms.operandForms(), index);
            if (wanted == Representation::Unchosen || wanted == held)
                continue;
            std::optional<ConversionPath> path = findConversion(m_types, forms.types[index], held, wanted);
            RELEASE_ASSERT(path);
            ValueID converted = draft.operand(index).id();
            for (ConversionStep step : *path) {
                Draft conversion(step.opcode, m_origin);
                conversion.setValueOperand(Reference::value(converted));
                conversion.representation = step.to;
                converted = appendInFormsGiven(conversion);
            }
            static_cast<DraftWords&>(draft).setOperand(index, Reference::value(converted));
        }
    }

    ValueID append(Draft& draft)
    {
        m_walker.chooseConservativeFields(draft);
        bringOperandsIntoWantedForms(draft);
        return appendInFormsGiven(draft);
    }

    void end(TerminatorDraft& terminator, uint32_t numberOfEdges = 0)
    {
        bringOperandsIntoWantedForms(terminator);
        m_code.end(m_block, terminator, numberOfEdges);
    }

    ValueID readFirstArgumentOfBuffer()
    {
        Draft read(Opcode::GetArgument, m_origin);
        read.setArgumentIndex(0);
        return append(read);
    }

    // The word of the closure of a class constructor says that it is not callable, and whoever calls by value goes by that word. So the call by value throws what every such call throws, and never comes back.
    void refuseCall()
    {
        Draft call(Opcode::Call, m_origin);
        call.setCalleeOperand(Reference::value(m_closure));
        call.setThisValueOperand(Reference::value(m_thisValue));
        call.setIsNeverInlined(true);
        append(call);
        TerminatorDraft unreachable(TerminatorKind::Unreachable, m_origin);
        end(unreachable);
    }

    // It goes on in the block of whoever constructs.
    void refuseUnlessConstructing(ValueID newTarget)
    {
        Draft test(Opcode::TypeTest, m_origin);
        test.setValueOperand(Reference::value(newTarget));
        test.setTestedType(TypeTable::undefined);
        ValueID isCalled = append(test);

        TerminatorDraft branch(TerminatorKind::Branch, m_origin);
        branch.setConditionOperand(Reference::value(isCalled));
        end(branch, 2);
        const Terminator& ended = m_code.at(m_block).terminator;
        EdgeID whereCalled = ended.taken();
        EdgeID whereConstructing = ended.notTaken();

        m_block = m_code.addBlock(BlockRole::Ordinary, BlockFrequency::Rare);
        m_code.closeEdge(whereCalled, m_block, { });
        refuseCall();

        m_block = m_code.addBlock();
        m_code.closeEdge(whereConstructing, m_block, { });
    }

    void endInError()
    {
        Draft error(Opcode::NewRuntimeError, m_origin);
        error.setRuntimeError(RuntimeErrorKind::LeftOutOfExecutable);
        if (m_name)
            error.setName(m_name);
        TerminatorDraft terminator(TerminatorKind::Throw, m_origin);
        terminator.setValueOperand(Reference::value(append(error)));
        end(terminator);
    }

    Walker& m_walker;
    const Program& m_program;
    const CallingConventions& m_conventions;
    const Function& m_function;
    TypingContext m_typing;
    RepresentationContext m_forms;
    TypeArena& m_types;
    Code& m_code;
    const CodeUnit m_unit;
    const Origin m_origin;
    const StringID m_name;
    BlockID m_block;
    ValueID m_closure;
    ValueID m_thisValue;
};

} // anonymous namespace

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
    if (m_compilesClosedGenericEntry)
        ClosedGenericEntryOfCompileUnit(inputs, *m_walker, unit).build();
    else if (!m_walker->walk(unit)) {
        m_build.diagnostics().append(m_walker->takeReports());
        reportFailureOfCompileUnit(m_build, unit, InternalErrorKind::Unknown, m_walker->failure());
        return false;
    } else if (uint32_t guesses = m_walker->numberOfGuessesOpened()) {
        m_build.statistics().count(StatisticID::TestsThatRejoin, guesses);
        m_build.statistics().count(StatisticID::LoopsBuiltTwice, m_walker->numberOfLoopsBuiltTwice());
        m_build.statistics().count(StatisticID::TurnsThatBeginAgain, m_walker->numberOfTurnsThatBeginAgain());
    }
    if (inputs.options.checksProofs && !m_compilesClosedGenericEntry) {
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

    if (inputs.options.takesNotes && notesAreTakenOfCompileUnit(unit.role)) {
        WalkNotes notes = notesOfSweptCode(m_types, code, AOT::hasFlag(m_build.options().reports, Wire::ReportSet::AccessCounts));
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
    // A slow body runs each time that a guard turns something away, and a loop of the caller may see to that for as long as it likes.
    if (unit.role != CodeRole::Body)
        return false;
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
    compiled->calleesOfIndirectCalls.numberOfCallsThatMayEnterAnyClosure = 1;
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
    code.calleesOfIndirectCalls = calleesOfIndirectCallsKeptBySweep(swept);
    code.isClosedGenericEntry = m_compilesClosedGenericEntry;

    Statistics& statistics = m_build.statistics();
    statistics.count(StatisticID::UnitsCompiled);
    if (m_inputs.walk.program.at(unit.function).role == FunctionRole::Parser)
        statistics.count(StatisticID::ParsersCreated);
    if (uint32_t selected = m_walker->numberOfTypedResultsSelected())
        statistics.count(StatisticID::TypedResultsSelected, selected);
    statistics.count(StatisticID::IndirectCallsThatMayEnterAnyClosure, code.calleesOfIndirectCalls.numberOfCallsThatMayEnterAnyClosure);
    return code;
}

std::optional<CompiledCode> CompileWorker::compileClosedGenericEntry(CodeUnit unit)
{
    SetForScope endsInPlaceOfGoingOn(m_compilesClosedGenericEntry, true);
    return compile(unit);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
