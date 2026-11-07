/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinCells.h"
#include "AOTBuiltinTable.h"
#include "AOTChildProcess.h"
#include "AOTCompileOptionWords.h"
#include "AOTDriver.h"
#include "AOTEffectsNet.h"
#include "AOTIRParser.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTProgramFile.h"
#include "AOTProgramStatics.h"
#include "AOTState.h"
#include "AOTTemporaryFile.h"
#include "AOTTestHarness.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include "AbstractSlotVisitorInlines.h"
#include "HeapInlines.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"
#include "Options.h"
#include <wtf/ConcurrentPtrHashSet.h>
#include <wtf/DataLog.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT {

std::span<const NativeTest> programStaticsTests();

static constexpr unsigned numberOfVariables = 600;

static String makeProgramText()
{
    StringBuilder text;
    text.append("module m0 path=\"/main.js\" displayPath=\"main.js\" body=f0\nentry module=m0\n"_s);
    for (unsigned number = 0; number < numberOfVariables; ++number)
        text.append("variable g"_s, number, " name=\"v"_s, number, "\" module=m0 declared={type=anything} initialization=Undefined flags=[IsReplaceable]\n"_s);
    text.append(
        "function f0 name=\"main\" kind=Module results=[{type=anything}] {\n"
        "  b0():\n"
        "    v0 = Integer integer=1\n"_s);
    for (unsigned number = 0; number < numberOfVariables; ++number)
        text.append("    PutModuleVariable v0 variable=g"_s, number, '\n');
    text.append("    Return\n}\n"_s);
    return text.toString();
}

static Vector<uint8_t> compileProgramWithVariables()
{
    Vector<uint8_t> image;
    runOnBuildThread([&] {
        Wire::CompileOptions options = makeOptionsOfTestedProgram();
        BuildContext build(options);
        std::unique_ptr<Typed::Program> program = Typed::parseProgram(Typed::withStandInLibrary(makeProgramText()), build);
        AOT_TEST_CHECK(program);
        std::unique_ptr<CompileOutput> output = Typed::compileWithMechanisms(WTF::move(program), build, Typed::mechanismsOfProcess());
        if (!output->hasImage())
            Typed::printDiagnosticsOf(*output, WTF::dataFile());
        AOT_TEST_CHECK(output->hasImage());
        image = Vector<uint8_t>(output->section(Wire::ResultSection::Image));
    });
    return image;
}

static void runWithVariables(ASCIILiteral testName, LoadedProgramTest body)
{
    if (Options::aotImagePath()) {
        runWithLoadedProgram(testName, ""_s, body);
        return;
    }

    TemporaryFile image(".image"_s, compileProgramWithVariables().span());
    ChildProcessLauncher launcher(String(programPath()), { });
    Vector<String> arguments { String(nativeSubcommandName), String(singleProcessFlag), String(testName), makeString("--aotImagePath="_s, image.path()) };
    ChildProcessResult result = launcher.run(arguments.span());
    if (!result.exitedWith(0)) {
        dataLogLn(result.output);
        dataLogLn("The process that uses the image ", result.description());
        failNativeTest(__FILE__, __LINE__, testName.characters());
    }
}

namespace {

using CellList = Vector<JSCell*>;

class RecordingVisitor final : public AbstractSlotVisitor {
public:
    explicit RecordingVisitor(VM& vm)
        : AbstractSlotVisitor(vm.heap.collector(), "Recording"_s, m_opaqueRootStorage)
    {
    }

    const CellList& cells() const { return m_cells; }

    void appendUnbarriered(JSCell* cell) final
    {
        if (cell)
            m_cells.append(cell);
    }

    void append(const ConservativeRoots&) final { RELEASE_ASSERT_NOT_REACHED(); }
    void appendHiddenUnbarriered(JSCell*) final { RELEASE_ASSERT_NOT_REACHED(); }
    void markAuxiliary(const void*) final { RELEASE_ASSERT_NOT_REACHED(); }
    void visitAsConstraint(const JSCell*) final { RELEASE_ASSERT_NOT_REACHED(); }
    void addParallelConstraintTask(RefPtr<SharedTask<void(AbstractSlotVisitor&)>>) final { RELEASE_ASSERT_NOT_REACHED(); }
    void addParallelConstraintTask(RefPtr<SharedTask<void(SlotVisitor&)>>) final { RELEASE_ASSERT_NOT_REACHED(); }

    bool isFirstVisit() const final { return true; }
    bool isMarked(const void*) const final { return false; }
    bool isMarked(MarkedBlock&, HeapCell*) const final { return false; }
    bool isMarked(PreciseAllocation&, HeapCell*) const final { return false; }
    bool mutatorsAreStopped() const final { return true; }
    void reportExtraMemoryVisited(size_t) final { }
#if ENABLE(RESOURCE_USAGE)
    void reportExternalMemoryVisited(size_t) final { }
#endif
    void didRace(const VisitRaceKey&) final { }

private:
    ConcurrentPtrHashSet m_opaqueRootStorage;
    CellList m_cells;
};

} // anonymous namespace

static CellList visitedCells(LoadedProgram& program)
{
    RecordingVisitor visitor(program.vm());
    AbstractSlotVisitor& abstractVisitor = visitor;
    program.state().statics->visitRoots(abstractVisitor);
    return visitor.cells();
}

static bool isRoot(const ProgramTables& tables, uint32_t offset)
{
    return std::ranges::any_of(tables.staticsRoots(), [&](const StaticsRange& range) {
        return range.contains(offset);
    });
}

static SlotData variable(LoadedProgram& program, unsigned number)
{
    const ProgramTables& tables = program.state().tables;
    std::span<const StaticsRun> runs = tables.staticsRuns();
    AOT_TEST_CHECK(runs.size() == 1 && runs[0].numberOfWords == numberOfVariables && runs[0].bits() == JSValue::ValueUndefined);
    uint32_t offset = (runs[0].firstWord + number) * sizeof(EncodedJSValue);
    AOT_TEST_CHECK(isRoot(tables, offset));
    return SlotData::create(offset, SlotRepresentation::JSValue);
}

static uint8_t& cardOf(LoadedProgram& program, SlotData slot)
{
    return program.instance().cards()[slot.offset >> ProgramStatics::cardShift];
}

static void testNumberInWrittenCardIsNoCell(LoadedProgram& program)
{
    SlotData slot = variable(program, 40);
    ProgramStatics::writeVariable(program.instance(), slot, jsNumber(5));
    AOT_TEST_CHECK(visitedCells(program).isEmpty());

    program.state().statics->didStoreRootInStatics(slot.offset);
    AOT_TEST_CHECK(cardOf(program, slot) == ProgramStatics::cardIsDirty);
    AOT_TEST_CHECK(visitedCells(program).isEmpty());
}

static void testOnlyWriteOfAnotherValueIsCounted(LoadedProgram& program)
{
    SlotData slot = variable(program, 41);
    const uint64_t& count = program.state().writesCountedForEffectsNet[static_cast<size_t>(Typed::CountInEffectsNet::WordsOfStaticsWritten)];
    uint64_t inFront = count;
    ProgramStatics::writeVariable(program.instance(), slot, jsNumber(5));
    AOT_TEST_CHECK(count == inFront + 1);
    ProgramStatics::writeVariable(program.instance(), slot, jsNumber(5));
    AOT_TEST_CHECK(count == inFront + 1);
    ProgramStatics::writeVariable(program.instance(), slot, jsNumber(6));
    AOT_TEST_CHECK(count == inFront + 2);
}

static void testOnlyFirstNameAddedAboveInstancesIsCounted(LoadedProgram& program)
{
    const BuiltinTable& table = BuiltinTable::singleton();
    BuiltinID holder;
    for (unsigned index = 0; index < table.classCount() && !holder; ++index)
        holder = table.at(BuiltinClassID(index)).prototype;
    AOT_TEST_CHECK(holder);
    const uint64_t& count = program.state().writesCountedForEffectsNet[static_cast<size_t>(Typed::CountInEffectsNet::WatchOverBuiltinsEnded)];
    uint64_t inFront = count;
    program.state().builtinCells->willAddNameTo(holder);
    uint64_t behindFirst = count;
    AOT_TEST_CHECK(behindFirst > inFront);
    program.state().builtinCells->willAddNameTo(holder);
    AOT_TEST_CHECK(count == behindFirst);
}

template<LoadedProgramTest body>
static void runNativeTestWithVariables()
{
    for (const NativeTest& test : programStaticsTests()) {
        if (test.run == &runNativeTestWithVariables<body>) {
            runWithVariables(test.name, body);
            return;
        }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::span<const NativeTest> programStaticsTests()
{
    static constexpr NativeTest tests[] = {
        { "program-statics/number-in-written-card-is-no-cell"_s, runNativeTestWithVariables<testNumberInWrittenCardIsNoCell> },
        { "program-statics/only-a-write-of-another-value-is-counted-for-the-net-of-effects"_s, runNativeTestWithVariables<testOnlyWriteOfAnotherValueIsCounted> },
        { "program-statics/only-the-first-name-added-above-instances-is-counted-for-the-net-of-effects"_s, runNativeTestWithVariables<testOnlyFirstNameAddedAboveInstancesIsCounted> },
    };
    return tests;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
