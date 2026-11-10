/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTestHarness.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTCodeRecords.h"
#include "AOTCurrentWork.h"
#include "AOTEntryEmitter.h"
#include "AOTFunctionCodeGenerator.h"
#include "AOTMachinePrinter.h"
#include "AOTMechanismWords.h"
#include "AOTSort.h"
#include "AOTStackCheckEmitter.h"
#include "AOTStages.h"
#include <wtf/ASCIICType.h>
#include <wtf/BitVector.h>
#include <wtf/StdLibExtras.h>
#include <wtf/StringPrintStream.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringToIntegerConversion.h>

namespace JSC { namespace AOT { namespace Typed {

static thread_local bool t_testRunsAsUsersBuild = false;

ScopeOfTestThatRunsAsUsersBuild::ScopeOfTestThatRunsAsUsersBuild(bool runsAsUsersBuild)
    : m_before(t_testRunsAsUsersBuild)
{
    t_testRunsAsUsersBuild = runsAsUsersBuild;
}

ScopeOfTestThatRunsAsUsersBuild::~ScopeOfTestThatRunsAsUsersBuild()
{
    t_testRunsAsUsersBuild = m_before;
}

Vector<TypeID> typesNamedBy(const Program& program, std::optional<FunctionID> functionOfTypesLine)
{
    FunctionID function = functionOfTypesLine.value_or(FunctionID(0u));
    Vector<TypeID> types;
    if (function.index() >= program.functions.size())
        return types;
    for (const DeclaredType& parameter : program.at(function).parameters)
        types.append(parameter.written());
    return types;
}

static constinit Mechanisms mechanismsThatProcessAllows = allMechanisms;

void printMechanisms(PrintStream& out, Mechanisms mechanisms)
{
    if (mechanisms == allMechanisms) {
        out.print("all"_s);
        return;
    }
    if (mechanisms.isEmpty()) {
        out.print("none"_s);
        return;
    }
    ASCIILiteral separator = ""_s;
    for (Mechanism mechanism : mechanisms) {
        out.print(separator, nameOfMechanism(mechanism));
        separator = ","_s;
    }
}

std::optional<String> applyMechanismsWordOfTest(Mechanisms& mechanisms, StringView word)
{
    if (std::optional<String> sentence = applyMechanismsWord(mechanisms, word))
        return sentence;
    mechanisms = withoutMechanismsMissingDependencies(mechanisms & mechanismsThatProcessAllows);
    return std::nullopt;
}

std::optional<String> applyMechanismsWordAskedFor(std::optional<Mechanisms>& askedFor, StringView word)
{
    Mechanisms mechanisms = askedFor.value_or(allMechanisms);
    if (std::optional<String> sentence = applyMechanismsWord(mechanisms, word))
        return sentence;
    askedFor = mechanisms;
    return std::nullopt;
}

std::optional<String> takeMechanismsWords(Vector<String>& words, std::optional<Mechanisms>& askedFor)
{
    std::optional<String> sentence;
    words.removeAllMatching([&](const String& word) {
        if (!isMechanismsWord(word))
            return false;
        std::optional<String> sentenceOfWord = applyMechanismsWordAskedFor(askedFor, word);
        if (sentenceOfWord && !sentence)
            sentence = WTF::move(sentenceOfWord);
        return true;
    });
    return sentence;
}

Mechanisms mechanismsOfProcess()
{
    return mechanismsThatProcessAllows;
}

void setMechanismsOfProcess(Mechanisms mechanisms)
{
    mechanismsThatProcessAllows = withoutMechanismsMissingDependencies(mechanisms);
}

Mechanisms mechanismsOfWay(ASCIILiteral way)
{
    Mechanisms mechanisms = mechanismsOfProcess();
    if (!way.isEmpty())
        RELEASE_ASSERT(!applyMechanismsWordOfTest(mechanisms, way));
    return mechanisms;
}

template<typename Number>
static std::optional<Number> numberInNameOfCodeUnit(StringView digits)
{
    if (digits.isEmpty() || !isASCIIDigit(digits[0]))
        return std::nullopt;
    return parseInteger<Number>(digits, 10, WTF::ParseIntegerWhitespacePolicy::Disallow);
}

static std::optional<FunctionID> functionInNameOfCodeUnit(StringView word)
{
    if (!word.startsWith('f'))
        return std::nullopt;
    std::optional<uint32_t> index = numberInNameOfCodeUnit<uint32_t>(word.substring(1));
    if (!index || *index > FunctionID::largestIndex)
        return std::nullopt;
    return FunctionID(*index);
}

std::optional<CodeUnit> parseCodeUnit(StringView text)
{
    Vector<StringView, 3> parts;
    for (StringView part : text.splitAllowingEmptyEntries(':'))
        parts.append(part);
    if (parts.isEmpty() || parts.size() > 2)
        return std::nullopt;

    std::optional<FunctionID> function = functionInNameOfCodeUnit(parts[0]);
    if (!function)
        return std::nullopt;
    if (parts.size() == 1)
        return CodeUnit(*function, CodeRole::Body);

    std::optional<CodeRole> role = parseEnum<CodeRole>(parts[1]);
    if (!role)
        return std::nullopt;
    return CodeUnit(*function, *role);
}

void printCodeUnit(PrintStream& out, CodeUnit unit)
{
    out.print("f"_s, unit.function.index(), ":"_s, nameOf(unit.role));
}

static String nameOfStagedUnit(CodeUnit unit)
{
    StringPrintStream out;
    printCodeUnit(out, unit);
    return out.toString();
}

std::optional<Vector<FunctionID>> functionsNamedBy(const Program& program, std::span<const String> arguments, PrintStream& out)
{
    Vector<FunctionID> functions;
    if (arguments.empty()) {
        for (size_t index = 0; index < program.functions.size(); ++index) {
            if (program.functions[index].hasBody())
                functions.append(FunctionID(index));
        }
        return functions;
    }

    for (const String& word : arguments) {
        std::optional<FunctionID> function = functionInNameOfCodeUnit(word);
        if (!function || function->index() >= program.functions.size()) {
            out.println("bad argument "_s, word);
            return std::nullopt;
        }
        functions.append(*function);
    }
    AOT::sort(functions);
    removeRepeatedElements(functions);
    return functions;
}

std::optional<Vector<CodeUnit>> unitsNamedBy(const Program& program, std::span<const String> arguments, PrintStream& out)
{
    Vector<CodeUnit> units;
    if (arguments.empty()) {
        for (size_t index = 0; index < program.functions.size(); ++index) {
            if (program.functions[index].hasBody())
                units.append(CodeUnit(FunctionID(index), CodeRole::Body));
        }
        return units;
    }

    for (const String& word : arguments) {
        std::optional<CodeUnit> unit = parseCodeUnit(word);
        if (!unit || unit->function.index() >= program.functions.size()) {
            out.println("bad argument "_s, word);
            return std::nullopt;
        }
        units.append(*unit);
    }
    AOT::sort(units);
    removeRepeatedElements(units);
    return units;
}

std::optional<Vector<CodeUnit>> unitsInOrderOfWords(const Program& program, std::span<const String> arguments, PrintStream& out)
{
    if (arguments.empty())
        return unitsNamedBy(program, arguments, out);

    Vector<CodeUnit> units;
    for (const String& word : arguments) {
        std::optional<CodeUnit> unit = parseCodeUnit(word);
        if (!unit || unit->function.index() >= program.functions.size()) {
            out.println("bad argument "_s, word);
            return std::nullopt;
        }
        units.append(*unit);
    }
    return units;
}

static InternalErrorKind errorKindOfStagedMachineFailure(Machine::FailureKind kind)
{
    switch (kind) {
    case Machine::FailureKind::MalformedProcedure:
    case Machine::FailureKind::OutOfRegisters:
    case Machine::FailureKind::MissingFrameSetup:
        return InternalErrorKind::Unknown;
    case Machine::FailureKind::AccessOutsideDeclaredEffects:
        return InternalErrorKind::StoreOutsideDeclaredEffects;
    case Machine::FailureKind::WrongValueInRegister:
        return InternalErrorKind::WrongValueInRegister;
    case Machine::FailureKind::WrongValueInSpillSlot:
        return InternalErrorKind::WrongValueInSpillSlot;
    case Machine::FailureKind::FrameAddressAcrossSuspension:
        return InternalErrorKind::FrameAddressAcrossSuspension;
    case Machine::FailureKind::NarrowReferenceWithoutPointer:
        return InternalErrorKind::NarrowReferenceWithoutPointer;
    case Machine::FailureKind::InnerPointerWithoutOwner:
        return InternalErrorKind::InnerPointerWithoutOwner;
    case Machine::FailureKind::UpperHalfIsNotClear:
        return InternalErrorKind::UpperHalfNotClear;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

struct MachineOperationsOfStagedBuild {
    WTF_MAKE_NONCOPYABLE(MachineOperationsOfStagedBuild);

    MachineOperationsOfStagedBuild(BuildContext& build, const CodeGenerationInputs& inputs, Machine::Arena& arena, const Machine::ProcedureTraits& traits, CodeGenerationTotals& totals)
        : relocations(jit, build.assemblerPolicy())
        , records(inputs.stubCatalog, relocations, totals)
        , procedure(arena, inputs.heaps.tree(), traits)
    {
    }

    CCallHelpers jit;
    RelocationRecorder relocations;
    CodeRecords records;
    StackCheckState stackChecks;
    Machine::Procedure procedure;
};

struct StagedBuild::State {
    WTF_MAKE_NONCOPYABLE(State);

    State(std::unique_ptr<Program>&&, BuildContext&, Mechanisms, OrderOfWorklist);

    void report(FunctionID, String&& detail);
    void reportFailureOf(const Machine::Procedure&, const Code&);

    FactsRequest requestForFacts() const;

    bool ensureFacts();
    bool ensurePrepared();
    bool ensureCompiled();
    bool ensureArrivals();
    CompileWorker* ensureWorker();
    CompileWorker* workerFor(CodeUnit);
    const Code* codeOf(CodeUnit);

    BuildContext& build;
    const Mechanisms mechanisms;
    const OrderOfWorklist orderOfWorklist;
    bool hasFailed;
    bool isPrepared { false };
    bool isCompiled { false };

    PreparedProgram prepared;
    std::unique_ptr<TypeArena> typesWithoutWorker;
    std::unique_ptr<CompileWorker> compileWorker;
    Vector<CompiledCode, 2> finished;
    Vector<CodeUnit> unitsWithCode;
    Machine::Arena machineArena;
    std::unique_ptr<MachineOperationsOfStagedBuild> machine;
};

StagedBuild::State::State(std::unique_ptr<Program>&& program, BuildContext& build, Mechanisms mechanisms, OrderOfWorklist orderOfWorklist)
    : build(build)
    , mechanisms(withoutMechanismsMissingDependencies(withoutMechanismsThatOptionsWithhold(mechanisms, build.options().testing)))
    , orderOfWorklist(orderOfWorklist)
    , hasFailed(!program)
{
    prepared.program = WTF::move(program);
    prepared.mechanisms = this->mechanisms;
}

void StagedBuild::State::report(FunctionID function, String&& detail)
{
    if (function.index() >= prepared.program->functions.size())
        function = { };
    build.diagnostics().add(InternalError { .kind = InternalErrorKind::Unknown, .function = function, .detail = WTF::move(detail) });
    hasFailed = true;
}

void StagedBuild::State::reportFailureOf(const Machine::Procedure& procedure, const Code& code)
{
    const Machine::Failure& failure = *procedure.failure();
    InternalError error { .kind = errorKindOfStagedMachineFailure(failure.kind), .function = code.function() };
    ValueID value = valueOfOrigin(failure.origin);
    if (value && value.index() < code.numberOfValues() && code.at(value).hasPosition()) {
        const Operation& operation = code.at(value);
        Wire::SourceLocation location = code.locationOf(*prepared.program, operation.origin());
        if (hasModule(location)) {
            error.value = value;
            error.location = location;
            error.operation = nameOf(operation.opcode);
        }
    }
    error.detail = failure.operation == UINT32_MAX ? WTF::toString(failure.kind) : makeString(WTF::toString(failure.kind), " at machine operation @"_s, failure.operation);
    build.diagnostics().add(error);
    hasFailed = true;
}

FactsRequest StagedBuild::State::requestForFacts() const
{
    FactsRequest request;
    request.mechanisms = mechanisms;
    request.orderOfWorklist = orderOfWorklist;
    return request;
}

bool StagedBuild::State::ensureFacts()
{
    if (hasFailed)
        return false;
    if (prepared.facts)
        return true;

    prepared.facts = computeFacts(*prepared.program, build, requestForFacts());
    if (!prepared.facts || build.diagnostics().hasErrors())
        hasFailed = true;
    return !hasFailed;
}

bool StagedBuild::State::ensurePrepared()
{
    if (hasFailed)
        return false;
    if (isPrepared)
        return true;
    if (!ensureFacts())
        return false;

    if (!prepareProgram(prepared, build) || build.diagnostics().hasErrors()) {
        hasFailed = true;
        return false;
    }
    isPrepared = true;
    return true;
}

bool StagedBuild::State::ensureCompiled()
{
    if (!ensurePrepared())
        return false;
    if (isCompiled)
        return true;

    CompiledProgram compiled = compileQueuedUnits(*prepared.compileInputs, prepared.program->types, build);
    if (!compiled.succeeded || build.diagnostics().hasErrors()) {
        hasFailed = true;
        return false;
    }

    for (const CompiledCode& code : compiled.functions)
        unitsWithCode.append(code.unit);
    AOT::sort(unitsWithCode);
    isCompiled = true;
    return true;
}

bool StagedBuild::State::ensureArrivals()
{
    return !mechanisms.contains(Mechanism::Arrivals) || ensureCompiled();
}

CompileWorker* StagedBuild::State::ensureWorker()
{
    if (!ensurePrepared())
        return nullptr;
    if (!compileWorker)
        compileWorker = makeUniqueWithoutFastMallocCheck<CompileWorker>(*prepared.compileInputs, build, prepared.program->types);
    return compileWorker.get();
}

CompileWorker* StagedBuild::State::workerFor(CodeUnit unit)
{
    machine = nullptr;
    finished.shrink(0);
    CompileWorker* worker = ensureWorker();
    if (!worker)
        return nullptr;
    if (!unit || !prepared.plan->conventions().exists(unit)) {
        report(unit.function, makeString("the plan has no unit "_s, nameOfStagedUnit(unit)));
        return nullptr;
    }
    return worker;
}

const Code* StagedBuild::State::codeOf(CodeUnit unit)
{
    CompileWorker* worker = workerFor(unit);
    if (!worker || !ensureArrivals())
        return nullptr;

    CurrentWorkScope work(*prepared.program, unit.function, "walk"_s);
    if (!worker->walkAndSweep(unit)) {
        hasFailed = true;
        return nullptr;
    }
    return &worker->walker().code();
}

StagedBuild::StagedBuild(std::unique_ptr<Program>&& program, BuildContext& build, Mechanisms mechanisms, OrderOfWorklist orderOfWorklist)
    : m_build(build)
    , m_state(makeUniqueWithoutFastMallocCheck<State>(WTF::move(program), build, mechanisms, orderOfWorklist))
{
}

StagedBuild::~StagedBuild() = default;

Mechanisms StagedBuild::mechanisms() const
{
    return m_state->mechanisms;
}

Program& StagedBuild::program()
{
    RELEASE_ASSERT(m_state->prepared.program);
    return *m_state->prepared.program;
}

const Facts* StagedBuild::facts()
{
    return m_state->ensureFacts() ? m_state->prepared.facts.get() : nullptr;
}

const Plan* StagedBuild::plan()
{
    return m_state->ensurePrepared() ? m_state->prepared.plan.get() : nullptr;
}

const PreparedProgram* StagedBuild::prepared()
{
    return m_state->ensurePrepared() ? &m_state->prepared : nullptr;
}

std::unique_ptr<CompileInputs> StagedBuild::prepareInputsForVerification()
{
    if (!m_state->isPrepared)
        m_state->prepared.isVerifiedByCaller = true;
    if (!prepared())
        return nullptr;
    RELEASE_ASSERT(willVerifyCode(m_state->prepared, m_build));
    auto inputs = makeUniqueWithoutFastMallocCheck<CompileInputs>(*m_state->prepared.compileInputs);
    inputs->walk.options.verifiesCode = true;
    return inputs;
}

TypeArena& StagedBuild::types()
{
    State& state = *m_state;
    if (state.compileWorker)
        return state.compileWorker->walker().types();
    if (CompileWorker* worker = state.ensureWorker())
        return worker->walker().types();

    if (!state.typesWithoutWorker) {
        TypeTable& table = program().types;
        state.typesWithoutWorker = makeUniqueWithoutFastMallocCheck<TypeArena>(table, table.isFrozen() ? ArenaKeeps::UntilReset : ArenaKeeps::InProgram);
    }
    return *state.typesWithoutWorker;
}

const Code* StagedBuild::code(CodeUnit unit)
{
    return m_state->codeOf(unit);
}

Machine::Procedure* StagedBuild::machineOperations(CodeUnit unit, MachineState wanted)
{
    const Code* code = m_state->codeOf(unit);
    return code ? emitMachineOperations(*code, wanted) : nullptr;
}

Machine::Procedure* StagedBuild::machineOperationsOf(const Code& code, MachineState wanted)
{
    RELEASE_ASSERT(code.isSwept());
    return m_state->workerFor(code.unit()) ? emitMachineOperations(code, wanted) : nullptr;
}

Machine::Procedure* StagedBuild::emitMachineOperations(const Code& codeToEmit, MachineState wanted)
{
    State& state = *m_state;
    const Code* code = &codeToEmit;
    CodeUnit unit = code->unit();

    CurrentWorkScope work(*state.prepared.program, unit.function, "emit"_s);
    CompileWorker& worker = *state.compileWorker;
    const CodeGenerationInputs& inputs = state.prepared.compileInputs->generation;
    Machine::ProcedureTraits traits = machineTraitsOf(m_build, inputs, *code);
    if (!t_testRunsAsUsersBuild)
        traits.checksAllocation = true;

    state.machineArena.reset();
    state.machine = makeUniqueWithoutFastMallocCheck<MachineOperationsOfStagedBuild>(m_build, inputs, state.machineArena, traits, worker.totals());
    MachineOperationsOfStagedBuild& machine = *state.machine;
    machine.records.frame.readsCallerFrame = readsCallerFrame(inputs, *code);
    setStackCheckGenerator(machine.procedure, code->holdsCallThatPlanHasNotCounted() ? StackCheck::Required : inputs.plan.conventions().stackCheckOf(unit), machine.records.frame, machine.stackChecks);

    CodeGenerator generator(FunctionInputs { m_build, worker.walker().types(), inputs, *code }, machine.procedure, machine.records);
    if (generator.run() == GenerationResult::Failed) {
        state.hasFailed = true;
        return nullptr;
    }

    switch (wanted) {
    case MachineState::Appended:
        break;
    case MachineState::Prepared:
        work.setStep("allocate"_s);
        if (machine.stackChecks.stackCheck == StackCheck::CoveredByCallers && !machine.records.frame.needsStackCheckForTailCalls)
            machine.procedure.setLargestFrameSetUpWithoutStackCheck(maxFrameSizeWithoutStackCheck);
        if (!machine.procedure.failure())
            Machine::prepareForGeneration(machine.procedure);
        break;
    }
    if (machine.procedure.failure()) {
        state.reportFailureOf(machine.procedure, *code);
        return nullptr;
    }
    return &machine.procedure;
}

const Machine::Procedure* StagedBuild::refusedMachineOperations() const
{
    const State& state = *m_state;
    return state.machine && state.machine->procedure.failure() ? &state.machine->procedure : nullptr;
}

const CompiledCode* StagedBuild::compiledCode(CodeUnit unit)
{
    State& state = *m_state;
    CompileWorker* worker = state.workerFor(unit);
    if (!worker || !state.ensureArrivals())
        return nullptr;

    if (!worker->compile(unit, state.finished)) {
        state.hasFailed = true;
        return nullptr;
    }
    for (const CompiledCode& code : state.finished) {
        if (code.unit == unit)
            return &code;
    }
    state.report(unit.function, makeString("no code was made of "_s, nameOfStagedUnit(unit)));
    return nullptr;
}

Vector<CodeUnit> StagedBuild::unitsToCompile()
{
    State& state = *m_state;
    state.machine = nullptr;
    state.finished.shrink(0);
    if (!state.ensureCompiled())
        return { };
    return state.unitsWithCode;
}

std::unique_ptr<CompileOutput> compileWithMechanisms(std::unique_ptr<Program> program, BuildContext& build, Mechanisms mechanisms)
{
    return compileParsedProgramWithMechanisms(WTF::move(program), build, mechanisms);
}

std::unique_ptr<Program> createEmptyProgram(BuildContext& build)
{
    auto program = makeUnique<Program>(BuiltinTable::singleton());
    program->strings.initialize(BuiltinTable::singleton(), build.parallelLoops(), { });
    return program;
}

static String messageInTextOfStagedOutput(std::span<const uint8_t> text, Wire::TextRange range)
{
    if (range.offset > text.size() || range.length > text.size() - range.offset)
        return { };
    return String::fromUTF8ReplacingInvalidSequences(text.subspan(range.offset, range.length));
}

void printDiagnosticsOf(const CompileOutput& output, PrintStream& out)
{
    std::span<const uint8_t> text = output.section(Wire::ResultSection::Text);
    for (const Wire::UserDiagnosticRecord& record : output.records<Wire::UserDiagnosticRecord>(Wire::ResultSection::UserDiagnostics))
        out.println(nameOf(record.kind), ": "_s, messageInTextOfStagedOutput(text, record.message));
    for (const Wire::InternalErrorRecord& record : output.records<Wire::InternalErrorRecord>(Wire::ResultSection::InternalErrors))
        out.println(nameOf(record.kind), ": "_s, messageInTextOfStagedOutput(text, record.message));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
