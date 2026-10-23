/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTChildProcess.h"
#include "AOTCompile.h"
#include "AOTCompileOptionWords.h"
#include "AOTIRParser.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTProgramFile.h"
#include "AOTProgramStatics.h"
#include "AOTTemporaryFile.h"
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
static constexpr uint32_t bytesPerCard = 1u << ProgramStatics::cardShift;

static String makeProgramText()
{
    StringBuilder text;
    text.append("module m0 path=\"/main.js\" displayPath=\"main.js\" body=f0\nentry module=m0\n"_s);
    for (unsigned number = 0; number < numberOfVariables; ++number)
        text.append("variable g"_s, number, " name=\"v"_s, number, "\" module=m0 declared={type=anything} initialization=Undefined\n"_s);
    text.append(
        "function f0 name=\"main\" kind=Module results=[{type=anything}] {\n"
        "  b0():\n"
        "    v0 = Builtin builtin=GlobalThis\n"
        "    v1 = PropGet v0 key=\"publish\" [isFreeName]\n"
        "    v2 = NewClosure function=f1\n"
        "    v3 = Call v1, -, v2\n"
        "    v4 = NewClosure function=f2\n"
        "    v5 = Call v1, -, v4\n"
        "    Return\n"
        "}\n"
        "function f1 name=\"store\" parameters=[{type=anything}] results=[{type=anything}] {\n"
        "  b0(v0: anything):\n"_s);
    for (unsigned number = 0; number < numberOfVariables; ++number)
        text.append("    PutModuleVariable v0 variable=g"_s, number, '\n');
    text.append(
        "    v"_s, numberOfVariables + 1, " = NewArray contents=anything @70\n"
        "    v"_s, numberOfVariables + 2, " = Call v0, -, v"_s, numberOfVariables + 1, "\n"
        "    Return v"_s, numberOfVariables + 1, "\n"
        "}\n"
        "function f2 name=\"load\" parameters=[{type=anything}] results=[{type=anything}] {\n"
        "  b0(v0: anything):\n"_s);
    for (unsigned number = 0; number < numberOfVariables; ++number) {
        text.append("    v"_s, 2 * number + 1, " = GetModuleVariable variable=g"_s, number, '\n');
        text.append("    v"_s, 2 * number + 2, " = Call v0, -, v"_s, 2 * number + 1, '\n');
    }
    text.append("    Return\n}\n"_s);
    return text.toString();
}

static Vector<uint8_t> compileProgram()
{
    Vector<uint8_t> image;
    runOnBuildThread([&] {
        // Only in fast code has an empty array literal a profile and a last array, which is the word that each collection clears.
        Wire::CompileOptions options = makeDefaultCompileOptions();
        AOT_TEST_CHECK(!applyOptionWord(options, "heat=EverythingIsHot"_s));
        BuildContext build(options);
        std::unique_ptr<Typed::Program> program = Typed::parseProgram(Typed::withStandInLibrary(makeProgramText()), Typed::IRForm::AsRead, build);
        AOT_TEST_CHECK(program);
        std::unique_ptr<CompileOutput> output = compileParsedProgram(WTF::move(program), build);
        if (output->image.isEmpty())
            Typed::printDiagnostics(output->diagnostics, WTF::dataFile());
        AOT_TEST_CHECK(!output->image.isEmpty());
        image = WTF::move(output->image);
    });
    return image;
}

static void runWithVariables(ASCIILiteral testName, LoadedProgramTest body)
{
    if (Options::aotImagePath()) {
        runWithLoadedProgram(testName, ""_s, body);
        return;
    }

    TemporaryFile image(".image"_s, compileProgram().span());
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

static uint64_t& wordAt(LoadedProgram& program, uint32_t offset)
{
    return program.state().statics->at<uint64_t>(offset);
}

static uint8_t& cardOf(LoadedProgram& program, SlotData slot)
{
    return program.instance().cards()[slot.offset >> ProgramStatics::cardShift];
}

static NEVER_INLINE void storeNewObject(LoadedProgram& program, SlotData slot, int32_t mark)
{
    VM& vm = program.vm();
    JSObject* object = constructEmptyObject(program.realm());
    object->putDirect(vm, Identifier::fromString(vm, "mark"_s), jsNumber(mark));
    ProgramStatics::writeVariable(program.instance(), slot, object);
}

static NEVER_INLINE void allocateGarbage(LoadedProgram& program)
{
    for (unsigned i = 0; i < 20000; ++i)
        constructEmptyObject(program.realm());
}

static bool holdsObjectWithMark(LoadedProgram& program, SlotData slot, int32_t mark)
{
    VM& vm = program.vm();
    JSValue value = ProgramStatics::readVariable(program.instance(), slot);
    return value.isObject() && vm.heap.isMarked(value.asCell()) && asObject(value)->getDirect(vm, Identifier::fromString(vm, "mark"_s)) == jsNumber(mark);
}

static void testReadingFillsTheRuns(LoadedProgram& program)
{
    Instance& instance = program.instance();
    const ProgramTables& tables = program.state().tables;
    AOT_TEST_CHECK(tables.staticsLayout().size() == 1);
    AOT_TEST_CHECK(tables.staticsLayout()[0].sizeFilledAtProgramEntry == numberOfVariables * sizeof(EncodedJSValue));

    for (unsigned number : { 0u, 1u, numberOfVariables - 1 }) {
        AOT_TEST_CHECK(!wordAt(program, variable(program, number).offset));
        AOT_TEST_CHECK(!ProgramStatics::readVariableWithoutAllocating(instance, variable(program, number)));
    }

    AOT_TEST_CHECK(ProgramStatics::readVariable(instance, variable(program, 3)).isUndefined());
    for (unsigned number = 0; number < numberOfVariables; ++number) {
        AOT_TEST_CHECK(wordAt(program, variable(program, number).offset) == JSValue::ValueUndefined);
        AOT_TEST_CHECK(ProgramStatics::readVariableWithoutAllocating(instance, variable(program, number)).isUndefined());
    }
    for (uint32_t offset = numberOfVariables * sizeof(EncodedJSValue); offset < tables.instanceSizes.sizeOfStatics; offset += sizeof(uint64_t))
        AOT_TEST_CHECK(!wordAt(program, offset));
}

static void testWritingFillsTheRunsFirst(LoadedProgram& program)
{
    Instance& instance = program.instance();
    AOT_TEST_CHECK(!wordAt(program, variable(program, 0).offset));
    ProgramStatics::writeVariable(instance, variable(program, 5), jsNumber(42));
    AOT_TEST_CHECK(ProgramStatics::readVariable(instance, variable(program, 5)) == jsNumber(42));
    AOT_TEST_CHECK(ProgramStatics::readVariable(instance, variable(program, 4)).isUndefined());
    AOT_TEST_CHECK(ProgramStatics::readVariable(instance, variable(program, 6)).isUndefined());
    AOT_TEST_CHECK(wordAt(program, variable(program, numberOfVariables - 1).offset) == JSValue::ValueUndefined);

    program.state().statics->ensureProgramStaticsInitialized();
    AOT_TEST_CHECK(ProgramStatics::readVariable(instance, variable(program, 5)) == jsNumber(42));
}

static void testEmptyValueClearsTheVariable(LoadedProgram& program)
{
    Instance& instance = program.instance();
    SlotData slot = variable(program, 7);
    ProgramStatics::writeVariable(instance, slot, jsNumber(1));
    ProgramStatics::writeVariable(instance, slot, JSValue());
    AOT_TEST_CHECK(!ProgramStatics::readVariable(instance, slot));
    AOT_TEST_CHECK(!ProgramStatics::readVariableWithoutAllocating(instance, slot));
    AOT_TEST_CHECK(wordAt(program, slot.offset) == traitsOf(SlotRepresentation::JSValue).absentBits);
    AOT_TEST_CHECK(ProgramStatics::readVariable(instance, variable(program, 8)).isUndefined());
}

static void testStoredCellSurvivesEdenCollection(LoadedProgram& program)
{
    VM& vm = program.vm();
    SlotData first = variable(program, 2);
    SlotData last = variable(program, numberOfVariables - 1);
    AOT_TEST_CHECK(&cardOf(program, first) != &cardOf(program, last));

    vm.heap.collectSync(CollectionScope::Full);
    storeNewObject(program, first, 11);
    storeNewObject(program, last, 12);
    AOT_TEST_CHECK(cardOf(program, first) == ProgramStatics::cardIsDirty && cardOf(program, last) == ProgramStatics::cardIsDirty);

    allocateGarbage(program);
    vm.heap.collectSync(CollectionScope::Eden);
    AOT_TEST_CHECK(vm.heap.lastCollectionScope() == CollectionScope::Eden);
    AOT_TEST_CHECK(holdsObjectWithMark(program, first, 11) && holdsObjectWithMark(program, last, 12));
    AOT_TEST_CHECK(cardOf(program, first) == RememberedSlotSet::scannedCard && cardOf(program, last) == RememberedSlotSet::scannedCard);

    storeNewObject(program, first, 13);
    AOT_TEST_CHECK(cardOf(program, first) == ProgramStatics::cardIsDirty);
    allocateGarbage(program);
    vm.heap.collectSync(CollectionScope::Eden);
    AOT_TEST_CHECK(holdsObjectWithMark(program, first, 13) && holdsObjectWithMark(program, last, 12));

    allocateGarbage(program);
    vm.heap.collectSync(CollectionScope::Full);
    AOT_TEST_CHECK(holdsObjectWithMark(program, first, 13) && holdsObjectWithMark(program, last, 12));
    vm.heap.collectSync(CollectionScope::Full);
    AOT_TEST_CHECK(holdsObjectWithMark(program, first, 13) && holdsObjectWithMark(program, last, 12));
}

static void testCardNeverWrittenIsNotScanned(LoadedProgram& program)
{
    Instance& instance = program.instance();
    JSObject* recorded = constructEmptyObject(program.realm());
    JSObject* neighbor = constructEmptyObject(program.realm());
    JSObject* unrecorded = constructEmptyObject(program.realm());
    SlotData inWrittenCard = variable(program, 1);
    SlotData inSameCard = variable(program, 2);
    SlotData inOtherCard = variable(program, 2 * bytesPerCard / sizeof(EncodedJSValue) + 1);
    AOT_TEST_CHECK(&cardOf(program, inWrittenCard) == &cardOf(program, inSameCard) && &cardOf(program, inWrittenCard) != &cardOf(program, inOtherCard));

    program.state().statics->ensureProgramStaticsInitialized();
    AOT_TEST_CHECK(visitedCells(program).isEmpty());
    for (uint32_t card = 0; card <= program.state().tables.instanceSizes.sizeOfStatics >> ProgramStatics::cardShift; ++card)
        AOT_TEST_CHECK(instance.cards()[card] == RememberedSlotSet::neverWrittenCard);

    wordAt(program, inOtherCard.offset) = JSValue::encode(unrecorded);
    wordAt(program, inSameCard.offset) = JSValue::encode(neighbor);
    AOT_TEST_CHECK(visitedCells(program).isEmpty());

    ProgramStatics::writeVariable(instance, inWrittenCard, recorded);
    AOT_TEST_CHECK(cardOf(program, inWrittenCard) == ProgramStatics::cardIsDirty);
    AOT_TEST_CHECK(cardOf(program, inOtherCard) == RememberedSlotSet::neverWrittenCard);
    AOT_TEST_CHECK(visitedCells(program) == CellList({ recorded, neighbor }));

    cardOf(program, inOtherCard) = ProgramStatics::cardIsDirty;
    AOT_TEST_CHECK(visitedCells(program) == CellList({ recorded, neighbor, unrecorded }));

    ensureStillAliveHere(recorded);
    ensureStillAliveHere(neighbor);
    ensureStillAliveHere(unrecorded);
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

static void testThrownValueIsARoot(LoadedProgram& program)
{
    JSObject* thrown = constructEmptyObject(program.realm());
    EncodedJSValue& thrownValue = program.instance().runtime.exceptions.thrownValue;
    AOT_TEST_CHECK(!thrownValue && visitedCells(program).isEmpty());
    thrownValue = JSValue::encode(thrown);
    AOT_TEST_CHECK(visitedCells(program) == CellList({ thrown }));
    thrownValue = JSValue::encode(JSValue());
    AOT_TEST_CHECK(visitedCells(program).isEmpty());
}

static void testClearedRangeIsZeroAfterCollection(LoadedProgram& program)
{
    VM& vm = program.vm();
    const ProgramTables& tables = program.state().tables;
    std::span<const StaticsRange> ranges = tables.staticsClearedEachCollection();
    AOT_TEST_CHECK(ranges.size() == 1 && ranges[0].size == sizeof(uint64_t));
    AOT_TEST_CHECK(ranges[0].offset >= tables.staticsLayout()[0].sizeFilledAtProgramEntry);
    AOT_TEST_CHECK(!isRoot(tables, ranges[0].offset));
    uint32_t cleared = ranges[0].offset;
    uint32_t kept = cleared - tables.staticsLayout()[0].distanceFromArrayProfileToLastArray;
    AOT_TEST_CHECK(kept != cleared && !isRoot(tables, kept));
    SlotData slot = variable(program, 9);

    for (CollectionScope scope : { CollectionScope::Eden, CollectionScope::Full }) {
        wordAt(program, cleared) = std::bit_cast<uint64_t>(constructEmptyObject(program.realm()));
        wordAt(program, kept) = 0x1234;
        ProgramStatics::writeVariable(program.instance(), slot, jsNumber(77));
        vm.heap.collectSync(scope);
        AOT_TEST_CHECK(!wordAt(program, cleared));
        AOT_TEST_CHECK(wordAt(program, kept) == 0x1234);
        AOT_TEST_CHECK(ProgramStatics::readVariable(program.instance(), slot) == jsNumber(77));
    }
    wordAt(program, kept) = 0;
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
        { "program-statics/reading-fills-the-runs"_s, runNativeTestWithVariables<testReadingFillsTheRuns> },
        { "program-statics/writing-fills-the-runs-first"_s, runNativeTestWithVariables<testWritingFillsTheRunsFirst> },
        { "program-statics/empty-value-clears-the-variable"_s, runNativeTestWithVariables<testEmptyValueClearsTheVariable> },
        { "program-statics/stored-cell-survives-eden-collection"_s, runNativeTestWithVariables<testStoredCellSurvivesEdenCollection> },
        { "program-statics/card-never-written-is-not-scanned"_s, runNativeTestWithVariables<testCardNeverWrittenIsNotScanned> },
        { "program-statics/number-in-written-card-is-no-cell"_s, runNativeTestWithVariables<testNumberInWrittenCardIsNoCell> },
        { "program-statics/thrown-value-is-a-root"_s, runNativeTestWithVariables<testThrownValueIsARoot> },
        { "program-statics/cleared-range-is-zero-after-collection"_s, runNativeTestWithVariables<testClearedRangeIsZeroAfterCollection> },
    };
    return tests;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
