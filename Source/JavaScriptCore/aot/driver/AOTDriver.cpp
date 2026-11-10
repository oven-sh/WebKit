/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTDriver.h"

#if ENABLE(AOT)

#include "AOTBodyFactsForTables.h"
#include "AOTBodyVerifier.h"
#include "AOTBuildContext.h"
#include "AOTExternalFunctionBodies.h"
#include "AOTIRProgram.h"
#include "AOTImageBuilder.h"
#include "AOTImageFormat.h"
#include "AOTPhaseTimes.h"
#include "AOTProgramVerifier.h"
#include "AOTStages.h"
#include "AOTThreads.h"
#include <limits>
#include <optional>
#include <utility>
#include <wtf/DataLog.h>
#include <wtf/MonotonicTime.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>
#include <wtf/StringPrintStream.h>
#include <wtf/text/CString.h>

namespace JSC { namespace AOT {

Wire::TextRange CompileOutput::appendText(std::span<const uint8_t> utf8)
{
    static_assert(!WTF::isValidCapacityForVector<uint8_t>(std::numeric_limits<uint32_t>::max()), "whatever a Vector holds has an offset and a length that fit a TextRange");

    Vector<uint8_t>& text = m_sections[static_cast<unsigned>(Wire::ResultSection::Text)];
    size_t offset = text.size();
    if (!text.tryAppend(utf8))
        return { };
    return { static_cast<uint32_t>(offset), static_cast<uint32_t>(utf8.size()) };
}

Wire::TextRange CompileOutput::appendText(const String& text)
{
    return appendText(byteCast<uint8_t>(text.utf8().span()));
}

size_t imageAlignment()
{
    return imagePageSize;
}

size_t processPeakResidentMegabytes()
{
    return PhaseTimes::takePeakResidentMegabytes().ofProcess;
}

namespace {

class DriverInstructionLog {
    WTF_MAKE_NONCOPYABLE(DriverInstructionLog);
public:
    explicit DriverInstructionLog(const BuildContext& context)
        : m_isOn(context.options().logging.logsPhaseTimes)
    {
        if (!m_isOn) [[likely]]
            return;
        InstructionCounts::begin();
        InstructionCounts::countOnCurrentThread();
        m_countAtLastBoundary = InstructionCounts::ofAllThreads();
    }

    void didFinish(ASCIILiteral stages)
    {
        if (!m_isOn) [[likely]]
            return;
        std::optional<uint64_t> now = InstructionCounts::ofAllThreads();
        if (now && m_countAtLastBoundary)
            printLine(stages, *now - *m_countAtLastBoundary);
        m_countAtLastBoundary = now;
    }

    void didCountPart(ASCIILiteral part, uint64_t instructions)
    {
        if (m_isOn && m_countAtLastBoundary)
            printLine(part, instructions);
    }

private:
    static void printLine(ASCIILiteral label, uint64_t instructions)
    {
        dataLog(toUTF8CString("AOT instructions: ", label, " | ", instructions, "\n"));
    }

    const bool m_isOn;
    std::optional<uint64_t> m_countAtLastBoundary;
};

void logUseOfCompileThreads(const UseOfCompileThreads& use)
{
    if (!use.wall)
        return;
    StringPrintStream lines;
    lines.print("AOT compile threads: ", use.numberOfThreads, " | wall ", use.wall.milliseconds(), " ms | at units ", use.busy.milliseconds(), " ms | busy on average ", use.busy / use.wall, " | most at once ", use.mostThreadsBusyAtOnce, " | by twentieth of the wall");
    for (double threads : use.threadsBusyInPart)
        lines.print(" ", static_cast<unsigned>(threads + 0.5));
    lines.print("\n");
    for (const UseOfCompileThreads::Unit& unit : use.longestUnits) {
        lines.print("AOT longest units: f", unit.unit.function.index(), " ", nameOf(unit.unit.role), " | from ", unit.begin.milliseconds(), " ms | ", unit.duration.milliseconds(), " ms | ", unit.duration / use.wall * 100, " % of the wall | walk and sweep ", unit.instructionsOfWalkAndSweep, " | emit to encode ", unit.instructionsOfEmitToEncode, "\n");
    }
    dataLog(lines.toUTF8CString());
}

class DestructionLog {
public:
    explicit DestructionLog(bool isOn)
        : m_isOn(isOn)
        , m_start(MonotonicTime::now())
    {
    }

    void didDestroy(ASCIILiteral what)
    {
        if (!m_isOn) [[likely]]
            return;
        Seconds wallTime = MonotonicTime::now() - m_start;
        dataLog(toUTF8CString("AOT phase times: BuildImage | destroyed ", what, " | wall ", wallTime.milliseconds(), " ms | resident ", PhaseTimes::residentMegabytes(), " MB\n"));
        m_start = MonotonicTime::now();
    }

private:
    const bool m_isOn;
    MonotonicTime m_start;
};

template<typename T>
void destroyOnAllThreads(const ParallelLoops& loops, std::span<T> elements)
{
    forEachIndexInParallel(loops, static_cast<unsigned>(elements.size()), [&](unsigned index) {
        T element = WTF::move(elements[index]);
    });
}

struct StageResults {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(StageResults);

    void destroy(const ParallelLoops&, bool logsTimes);

    std::unique_ptr<Typed::Program> program;
    std::unique_ptr<Typed::Facts> facts;
    std::unique_ptr<Typed::Plan> plan;
    std::unique_ptr<const StubCatalog> stubCatalog;
    std::unique_ptr<const AOTHeaps> heaps;
    LinkedFunctions functions;
    std::optional<BuiltImage> built;
};

void StageResults::destroy(const ParallelLoops& loops, bool logsTimes)
{
    CurrentWorkScope work("destroy stage results"_s);
    DestructionLog log(logsTimes);
    functions.clear(loops);
    if (built)
        built->functions.clear(loops);
    log.didDestroy("the code of units"_s);

    auto destroyInTask = [&](auto& owner, ASCIILiteral what) {
        DestructionLog logOfTask(logsTimes);
        owner.reset();
        logOfTask.didDestroy(what);
    };
    runTasksInParallel(loops, {
        [&] { destroyInTask(facts, "facts"_s); },
        [&] { destroyInTask(plan, "the plan"_s); },
        [&] {
            stubCatalog = nullptr;
            heaps = nullptr;
        },
        [&] { destroyInTask(built, "the tables of the image"_s); },
    });
    log.didDestroy("facts, the plan and the tables of the image, side by side"_s);

    if (program) {
        destroyOnAllThreads(loops, program->functions.mutableSpan());
        destroyOnAllThreads(loops, program->layouts.mutableSpan());
        destroyOnAllThreads(loops, program->modules.mutableSpan());
        destroyOnAllThreads(loops, program->data.mutableSpan());
        log.didDestroy("the records of the program"_s);
        program = nullptr;
        log.didDestroy("the types, the strings and the rest of the program"_s);
    }
}

class BuildDriver {
    WTF_MAKE_NONCOPYABLE(BuildDriver);
public:
    BuildDriver(BuildContext& context, CompileOutput& output, const ProgramOwner& owner)
        : m_context(context)
        , m_output(output)
        , m_owner(owner)
        , m_instructions(context)
    {
        if (context.options().logging.logsPhaseTimes)
            PhaseTimes::takePeakResidentMegabytes();
    }

    void read(std::span<const uint8_t> bytes);
    void setProgram(std::unique_ptr<Typed::Program>&& program) { m_prepared.program = WTF::move(program); }
    void setChangeOfPlan(void (*changePlan)(Typed::Plan&)) { m_prepared.changePlan = changePlan; }
    void setMechanisms(Typed::Mechanisms mechanisms) { m_prepared.mechanisms = mechanisms; }
    void run();

private:
    static constexpr Wire::ResultSection sectionsRequiringImage[] = {
        Wire::ResultSection::Image,
        Wire::ResultSection::Symbols,
        Wire::ResultSection::ObfuscatedSites,
        Wire::ResultSection::BytesByForm,
        Wire::ResultSection::BytesByModule,
    };

    bool hasErrors() const { return m_context.diagnostics().hasErrors(); }

    void runStagesAfterRead();
    void verify();
    bool prepare();
    bool compile();
    void explain();
    void buildImageFromCode();
    void releaseProgram();
    void letGoOfWhatCompileRead();
    void finishOutput();
    void destroyNotes();
    void destroyStageResults();

    BuildContext& m_context;
    CompileOutput& m_output;
    ProgramOwner m_owner;
    DriverInstructionLog m_instructions;
    PreparedProgram m_prepared;
    CompiledProgram m_compiled;
    std::optional<BuiltImage> m_built;
    bool m_planIsChecked { false };
    std::unique_ptr<EarlyWasmModules> m_earlyWasmModules;
};

void BuildDriver::read(std::span<const uint8_t> bytes)
{
    if (hasErrors())
        return;
    {
        PhaseTimes times(m_context, Stage::ReadAndVerify);
        m_prepared.program = Typed::readProgramWithExternalBodies(bytes, m_owner.externalBodies, m_context);
        times.didFinish("read"_s);
        if (auto releaseBytes = std::exchange(m_owner.releaseBytes, nullptr); releaseBytes && m_owner.externalBodies) {
            if (m_prepared.program)
                m_prepared.program->borrowBuffer({ });
            releaseBytes(m_owner.owner);
            times.didFinish("release the bytes of the program"_s);
        }
    }
    RELEASE_ASSERT(m_prepared.program || hasErrors());
    m_instructions.didFinish("ReadAndVerify"_s);
}

void BuildDriver::run()
{
    runStagesAfterRead();
    releaseProgram();
    finishOutput();
    m_instructions.didFinish("BuildImage"_s);
    destroyStageResults();
    writeStatisticSection(m_context.statistics(), m_output);
}

static bool holdsInt32PromiseForDriver(const Typed::Function& function)
{
    bool holds = false;
    function.body.forEachBlock([&](Typed::BodyBlock block) {
        if (holds)
            return;
        for (Typed::BodyValue value : block.values()) {
            if (value.opcode() == Typed::Opcode::Convert && value.as<Typed::Opcode::Convert>().declared().has(Typed::PositionFlags::IsInt32Promise)) {
                holds = true;
                return;
            }
        }
        Typed::BodyTerminator terminator = block.terminator();
        if (terminator.kind() != Typed::TerminatorKind::ReturnCall)
            return;
        for (Typed::TailCast cast : terminator.as<Typed::TerminatorKind::ReturnCall>().casts())
            holds |= cast.declared.has(Typed::PositionFlags::IsInt32Promise);
    });
    return holds;
}

static bool walkJudgesSomethingOfProgramForDriver(const Typed::Program& program, const ParallelLoops& loops)
{
    return !findIndicesInParallel(loops, static_cast<unsigned>(program.functions.size()), [&](unsigned index) {
        const Typed::Function& function = program.at(FunctionID(index));
        return function.hasBody() && holdsInt32PromiseForDriver(function);
    }).isEmpty();
}

void BuildDriver::runStagesAfterRead()
{
    if (!m_prepared.program || hasErrors())
        return;

    bool makesImage = false;
    switch (m_context.options().action) {
    case Wire::CompileAction::VerifyOnly:
        verify();
        return;
    case Wire::CompileAction::CheckOnly:
        break;
    case Wire::CompileAction::Compile:
        makesImage = true;
        break;
    }

    if (makesImage && m_context.threadCount() > 1)
        m_prepared.checkOfPlan = CheckOfPlan::LeftToCaller;

    auto analyzeAndCompile = [&] {
        if (!prepare())
            return false;
        if (!makesImage && !m_prepared.compileInputs->walk.options.takesNotes && !walkJudgesSomethingOfProgramForDriver(*m_prepared.program, m_context.parallelLoops()))
            return false;
        if (!compile())
            return false;
        if (m_prepared.compileInputs->walk.options.takesNotes)
            explain();
        return true;
    };
    bool isCompiled = false;
    if (makesImage && !m_prepared.program->webAssemblyModules.isEmpty()) {
        runLanes(m_context.parallelLoops(), {
            [&] { isCompiled = analyzeAndCompile(); },
            [&] { m_earlyWasmModules = compileWebAssemblyFiles(*m_prepared.program, m_context); },
        });
    } else
        isCompiled = analyzeAndCompile();
    if (isCompiled && makesImage && !hasErrors())
        buildImageFromCode();
}

void BuildDriver::verify()
{
    {
        PhaseTimes times(m_context, Stage::ReadAndVerify);
        const Typed::Program& program = *m_prepared.program;
        if (Typed::verifyRecords(program, m_context))
            Typed::verifyBodies(program, m_context);
        times.didFinish("verify"_s);
    }
    m_instructions.didFinish("ReadAndVerify"_s);
}

bool BuildDriver::prepare()
{
    bool isPrepared = prepareProgram(m_prepared, m_context);
    RELEASE_ASSERT(isPrepared || hasErrors());
    if (m_prepared.facts) {
        PhaseTimes times(m_context, Stage::AnalyzeFlow);
        writeFactSections(*m_prepared.program, *m_prepared.facts, m_output);
        times.didFinish("write what Facts found"_s);
    }
    m_instructions.didFinish("AnalyzeFlow PlanSlots"_s);
    return isPrepared && !hasErrors();
}

bool BuildDriver::compile()
{
    {
        PhaseTimes times(m_context, Stage::GenerateCode);
        auto compileUnits = [&] {
            m_compiled = compileQueuedUnits(*m_prepared.compileInputs, m_prepared.program->types, m_context);
            times.didFinish("compile units"_s);
            m_instructions.didFinish("GenerateCode"_s);
        };
        switch (m_prepared.checkOfPlan) {
        case CheckOfPlan::InFrontOfCompile:
            compileUnits();
            break;
        case CheckOfPlan::LeftToCaller: {
            std::optional<StepTimes> timesOfWait;
            runLanes(m_context.parallelLoops(), {
                [&] {
                    compileUnits();
                    timesOfWait.emplace(m_context, nameOf(Stage::PlanSlots));
                },
                [&] { checkPlanOfPreparedProgram(m_prepared, m_context); },
            });
            timesOfWait->didFinish("check the plan"_s);
            m_instructions.didFinish("AnalyzeFlow PlanSlots"_s);
            break;
        }
        }
        m_planIsChecked = true;
    }
    RELEASE_ASSERT(m_compiled.succeeded || hasErrors());
    m_instructions.didCountPart("walk and sweep"_s, m_compiled.instructionsOfWalkAndSweep);
    m_instructions.didCountPart("emit to encode"_s, m_compiled.instructionsOfEmitToEncode);
    if (m_context.options().logging.logsPhaseTimes)
        logUseOfCompileThreads(m_compiled.useOfThreads);
    if (const Typed::SharedAnswers* answers = m_prepared.program->types.sharedAnswers(); answers && m_context.options().logging.logsPhaseTimes) {
        StringPrintStream counts;
        answers->dumpCounts(counts);
        dataLog(counts.toUTF8CString());
    }
    return m_compiled.succeeded && !hasErrors();
}

void BuildDriver::explain()
{
    {
        PhaseTimes times(m_context, Stage::GenerateCode);
        writeExplainSections(*m_prepared.program, m_compiled.notes.span(), m_context, m_output);
        times.didFinish("write the records of --explain"_s);
        destroyNotes();
        times.didFinish("destroy notes"_s);
    }
    m_instructions.didFinish("GenerateCode"_s);
}

void BuildDriver::buildImageFromCode()
{
    RELEASE_ASSERT(m_planIsChecked && !hasErrors());
    Typed::Program& program = *m_prepared.program;
    BodyFactsForTables bodyFacts;
    {
        PhaseTimes times(m_context, Stage::BuildImage);
        bodyFacts = collectBodyFactsForTables(program, *m_prepared.plan, m_compiled.functions, m_context);
        times.didFinish("collect facts of bodies for the tables"_s);
        releaseProgram();
        times.didFinish("release the program"_s);
        letGoOfWhatCompileRead();
        times.didFinish("let go of what Compile read"_s);
    }
    if (hasErrors())
        return;

    m_built.emplace(buildImage(ImageInputs {
        .program = program,
        .facts = *m_prepared.facts,
        .plan = *m_prepared.plan,
        .stubCatalog = *m_prepared.stubCatalog,
        .functions = WTF::move(m_compiled.functions),
        .bodyFacts = WTF::move(bodyFacts),
        .entriesUsedByFunctions = m_compiled.entriesUsed,
        .earlyWasmModules = WTF::move(m_earlyWasmModules),
    }, m_context));
    if (hasErrors())
        return;

    BuiltImage& built = *m_built;
    RELEASE_ASSERT(!built.image.isEmpty() && !(built.image.size() % imagePageSize));
    PhaseTimes times(m_context, Stage::BuildImage);
    writeCodeReportSections(program, built, m_context, m_output);
    writeObfuscatedSiteSection(program, built, m_context, m_output);
    if (const UTF8CString& mapFilePath = m_context.options().logging.mapFilePath; !mapFilePath.isNull())
        writeMapFile(mapFilePath, program, built, m_context);
    m_output.setSection(Wire::ResultSection::Image, WTF::move(built.image));
    times.didFinish("write reports of the code"_s);
}

void BuildDriver::releaseProgram()
{
    if (const Typed::Program* program = m_prepared.program.get())
        m_context.diagnostics().resolveLocations(*program);
    if (auto release = std::exchange(m_owner.release, nullptr))
        release(m_owner.owner);
}

void BuildDriver::letGoOfWhatCompileRead()
{
    m_prepared.compileInputs = nullptr;
    m_prepared.arrivals = nullptr;
    m_context.backgroundJobs().add([read = m_prepared.facts->takeWhatMiddleAloneReads()](const ParallelLoops&) mutable {
        read = { };
        WTF::releaseFastMallocFreeMemory();
    });
}

void BuildDriver::finishOutput()
{
    PhaseTimes times(m_context, Stage::BuildImage);
    Diagnostics& diagnostics = m_context.diagnostics();
    diagnostics.finish(m_prepared.program.get());
    if (diagnostics.hasErrors()) {
        for (Wire::ResultSection section : sectionsRequiringImage)
            m_output.setSection(section, { });
    }
    if (m_prepared.facts)
        writeFeatureSection(*m_prepared.facts, m_output.hasImage() ? &*m_built : nullptr, m_output);
    writeDiagnosticSections(diagnostics, m_output);
    times.didFinish("write features and diagnostics"_s);
}

void BuildDriver::destroyNotes()
{
    if (m_compiled.notes.isEmpty())
        return;
    m_context.backgroundJobs().add([notes = std::exchange(m_compiled.notes, { })](const ParallelLoops& loops) mutable {
        destroyOnAllThreads(loops, notes.mutableSpan());
    });
}

void BuildDriver::destroyStageResults()
{
    PhaseTimes times(m_context, Stage::BuildImage);
    m_prepared.compileInputs = nullptr;
    m_prepared.arrivals = nullptr;
    destroyNotes();
    auto results = makeUnique<StageResults>();
    results->program = WTF::move(m_prepared.program);
    results->facts = WTF::move(m_prepared.facts);
    results->plan = WTF::move(m_prepared.plan);
    results->stubCatalog = WTF::move(m_prepared.stubCatalog);
    results->heaps = WTF::move(m_prepared.heaps);
    results->functions = WTF::move(m_compiled.functions);
    results->built = std::exchange(m_built, std::nullopt);
    m_context.backgroundJobs().add([results = WTF::move(results), logsTimes = m_context.options().logging.logsPhaseTimes](const ParallelLoops& loops) {
        results->destroy(loops, logsTimes);
    });
    times.didFinish("destroy stage results"_s);
}

void logTimeToEnterEngine(const BuildContext& context, MonotonicTime entered)
{
    if (!context.options().logging.logsPhaseTimes) [[likely]]
        return;
    dataLog(toUTF8CString("AOT phase times: ReadAndVerify | enter the engine | wall ", (MonotonicTime::now() - entered).milliseconds(), " ms\n"));
}

std::unique_ptr<CompileOutput> compileBytesWithBuildDriver(std::span<const uint8_t> program, const Wire::CompileOptions& options, const HostThreadPool* hostPool, const ProgramOwner& owner, Typed::Mechanisms mechanisms)
{
    MonotonicTime entered = MonotonicTime::now();
    auto output = makeUnique<CompileOutput>();
    runOnBuildThread([&] {
        RefPtr<BackgroundJobs> jobs;
        {
            BuildContext context(options, hostPool);
            jobs = &context.backgroundJobs();
            context.setFunctionSources(owner.functionSources);
            context.setStringEncoders(owner.stringEncoders);
            BuildDriver driver(context, *output, owner);
            driver.setMechanisms(withoutMechanismsThatOptionsWithhold(mechanisms, options.testing));
            logTimeToEnterEngine(context, entered);
            driver.read(program);
            driver.run();
        }
        if constexpr (ASAN_ENABLED)
            jobs->waitUntilAllHaveRun();
    });
    return output;
}

std::unique_ptr<CompileOutput> compileRecordsWithBuildDriver(std::unique_ptr<Typed::Program>&& program, BuildContext& context, void (*changePlan)(Typed::Plan&), Typed::Mechanisms mechanisms)
{
    auto output = makeUnique<CompileOutput>();
    runOnBuildThread([&] {
        BuildDriver driver(context, *output, { });
        driver.setProgram(WTF::move(program));
        driver.setChangeOfPlan(changePlan);
        driver.setMechanisms(withoutMechanismsThatOptionsWithhold(mechanisms, context.options().testing));
        driver.run();
    });
    return output;
}

} // anonymous namespace

std::unique_ptr<CompileOutput> compileProgram(std::span<const uint8_t> program, const Wire::CompileOptions& options, const HostThreadPool* hostPool, ProgramOwner owner)
{
    return compileBytesWithBuildDriver(program, options, hostPool, owner, Typed::allMechanisms);
}

std::unique_ptr<CompileOutput> compileProgramWithMechanisms(std::span<const uint8_t> program, const Wire::CompileOptions& options, Typed::Mechanisms mechanisms)
{
    return compileBytesWithBuildDriver(program, options, nullptr, { }, mechanisms);
}

std::unique_ptr<CompileOutput> compileParsedProgramWithMechanisms(std::unique_ptr<Typed::Program> program, BuildContext& context, Typed::Mechanisms mechanisms)
{
    return compileRecordsWithBuildDriver(WTF::move(program), context, nullptr, mechanisms);
}

std::unique_ptr<CompileOutput> compileParsedProgramWithChangedPlan(std::unique_ptr<Typed::Program> program, BuildContext& context, void (*changePlan)(Typed::Plan&), Typed::Mechanisms mechanisms)
{
    return compileRecordsWithBuildDriver(WTF::move(program), context, changePlan, mechanisms);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
