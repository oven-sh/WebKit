/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTImage.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "JSCInlines.h"
#include "MarkedBlockInlines.h"
#include "StrongInlines.h"
#include <array>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT {

std::span<const NativeTest> innerObjectTests();

static constexpr ASCIILiteral programText = R"IR(layout L0 name="Outer" fields=[{key="b" declared={type=number} flags=[IsRequired]}, {key="c" declared={type=L1} flags=[IsRequired]}, {key="t" declared={type=anything} flags=[IsRequired]}]
layout L1 name="Inner" fields=[{key="d" declared={type=number} flags=[IsRequired]}, {key="s" declared={type=anything} flags=[IsRequired]}]
module m0 path="/main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="publish" [isFreeName]
    v2 = NewClosure function=f1
    v3 = Call v1, -, v2
    Return
}
function f1 name="make" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    v2 = Check v1 castType=number
    v3 = NewObject -, v2, v0 layout=L1 fields=[0, 1]
    v4 = NewObject -, v2, v3, v0 layout=L0 fields=[0, 1, 2]
    v5 = Call v0, -, v4
    Return
}
)IR"_s;

static const LayoutData& layoutNamed(LoadedProgram& program, ASCIILiteral name)
{
    const ProgramTables& tables = program.state().tables;
    for (uint32_t shape = 1; shape < tables.firstEnvironmentShape; ++shape) {
        if (tables.hasLayoutForShape(shape) && Image::current()->text().equals(StringID(tables.layoutForShape(shape).name), name))
            return tables.layoutForShape(shape);
    }
    failNativeTest(__FILE__, __LINE__, name.characters());
}

static uint32_t allocationOf(LoadedProgram& program, const LayoutData& layout, bool holdsInnerObjects)
{
    std::span<const AllocationData> allocations = program.state().tables.allocations();
    for (uint32_t number = 0; number < allocations.size(); ++number) {
        if (allocations[number].shape == layout.shape && (allocations[number].size() > layout.size) == holdsInnerObjects)
            return number;
    }
    failNativeTest(__FILE__, __LINE__, "the image has the allocation");
}

static const FieldData& field(LoadedProgram& program, Object* object, ASCIILiteral name)
{
    const FieldData* result = object->fieldNamed(program.state(), Identifier::fromString(program.vm(), name));
    AOT_TEST_CHECK(result);
    return *result;
}

struct Pair {
    Object* outer;
    Object* inner;
};

// What the operations of the two objects do between them.
static NEVER_INLINE Pair makePair(LoadedProgram& program)
{
    VM& vm = program.vm();
    DeferGC deferGC(vm);
    StructureTable& structures = program.state().objects->structures;
    const LayoutData& outerLayout = layoutNamed(program, "Outer"_s);
    const LayoutData& innerLayout = layoutNamed(program, "Inner"_s);
    AOT_TEST_CHECK(outerLayout.hasInnerObjects && !outerLayout.mayBeInnerObject && innerLayout.mayBeInnerObject && !innerLayout.hasInnerObjects);
    AOT_TEST_CHECK(outerLayout.offsetsOfInnerObjects().size() == 1 && outerLayout.offsetsOfInnerObjects()[0] == outerLayout.size);

    uint32_t number = allocationOf(program, outerLayout, true);
    uint32_t size = program.state().tables.allocations()[number].size();
    AOT_TEST_CHECK(size == outerLayout.size + innerLayout.size);
    auto* bytes = reinterpret_cast<uint8_t*>(structures.allocateAsCompiledCodeDoes(vm, number));
    // Compiled code copies the header of the inner object without a look at the record.
    uint32_t innerNumber = allocationOf(program, innerLayout, false);
    AOT_TEST_CHECK(outerLayout.allocationsOfInnerObjects().size() == 1 && outerLayout.allocationsOfInnerObjects()[0] == innerNumber);
    AOT_TEST_CHECK(program.instance().objects.allocationSite(innerNumber) && program.instance().objects.allocationSite(innerNumber)->header);
    const AllocationSiteRecord innerRecord = *program.instance().objects.allocationSite(innerNumber);
    zeroSpan(std::span { bytes + 2 * sizeof(uint64_t), size - 2 * sizeof(uint64_t) });
    auto* innerWords = reinterpret_cast<uint64_t*>(bytes + outerLayout.size);
    innerWords[0] = innerRecord.header;
    innerWords[1] = innerRecord.orderAndShapeWords;

    Pair pair { reinterpret_cast<Object*>(bytes), reinterpret_cast<Object*>(innerWords) };
    for (Object* object : { pair.outer, pair.inner }) {
        for (const FieldData& each : object->layout().fields())
            object->clear(each);
    }
    AOT_TEST_CHECK(pair.outer->tryStore(vm, field(program, pair.outer, "b"_s), jsNumber(1)) && pair.outer->tryStore(vm, field(program, pair.outer, "c"_s), pair.inner));
    AOT_TEST_CHECK(pair.outer->tryStore(vm, field(program, pair.outer, "t"_s), jsNull()));
    AOT_TEST_CHECK(pair.inner->tryStore(vm, field(program, pair.inner, "d"_s), jsNumber(2)) && pair.inner->tryStore(vm, field(program, pair.inner, "s"_s), jsNull()));
    return pair;
}

// The collector reads the stack for what looks like a reference, so the tests keep the cells they ask about in another form and off the stack.
struct Hidden {
    uintptr_t outer { 0 };
    uintptr_t inner { 0 };
    uintptr_t other { 0 };
};

static NEVER_INLINE uintptr_t hide(JSCell* cell)
{
    return ~std::bit_cast<uintptr_t>(cell);
}

static NEVER_INLINE bool isMarked(VM& vm, uintptr_t hidden)
{
    return vm.heap.isMarked(std::bit_cast<JSCell*>(~hidden));
}

static NEVER_INLINE bool hasHeader(uintptr_t hidden)
{
    return !!*std::bit_cast<const uint64_t*>(~hidden);
}

static NEVER_INLINE bool isLive(uintptr_t hidden)
{
    auto* cell = std::bit_cast<JSCell*>(~hidden);
    return cell->markedBlock().handle().isLive(cell);
}

enum class Kept : uint8_t { Outer, Inner };

static NEVER_INLINE void makeHiddenPair(LoadedProgram& program, Hidden& hidden, Strong<JSCell>& root, Kept kept)
{
    Pair pair = makePair(program);
    hidden.outer = hide(pair.outer);
    hidden.inner = hide(pair.inner);
    root.set(program.vm(), kept == Kept::Outer ? pair.outer : pair.inner);
}

// A block without a live cell is given back, and then nothing can be asked of it. So a pair that lasts lies in the block of the pair that a test lets go of.
static NEVER_INLINE void makeHiddenPairInBlockThatLasts(LoadedProgram& program, Hidden& hidden, Strong<JSCell>& root, Kept kept, Strong<JSCell>& anchor)
{
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        Pair lasting = makePair(program);
        anchor.set(program.vm(), lasting.outer);
        makeHiddenPair(program, hidden, root, kept);
        if (&lasting.outer->markedBlock() == &std::bit_cast<JSCell*>(~hidden.outer)->markedBlock())
            return;
    }
    failNativeTest(__FILE__, __LINE__, "two pairs in a row lie in one block");
}

static constexpr std::array ordersOfCollections {
    std::array { CollectionScope::Eden, CollectionScope::Eden, CollectionScope::Full, CollectionScope::Eden },
    std::array { CollectionScope::Full, CollectionScope::Eden, CollectionScope::Full, CollectionScope::Full },
    std::array { CollectionScope::Eden, CollectionScope::Full, CollectionScope::Eden, CollectionScope::Full },
};

static void testCellLiesInBlockOfItsOwnKind(LoadedProgram& program)
{
    Pair pair = makePair(program);
    MarkedBlock& block = pair.outer->markedBlock();
    AOT_TEST_CHECK(block.hasInnerCells() && &pair.inner->markedBlock() == &block);
    AOT_TEST_CHECK(!block.isInnerCell(pair.outer) && block.isInnerCell(pair.inner));
    AOT_TEST_CHECK(block.outerCellOf(pair.inner) == pair.outer && block.outerCellOf(pair.outer) == pair.outer);
    AOT_TEST_CHECK(pair.outer->read(field(program, pair.outer, "c"_s)) == JSValue(pair.inner));

    // An object that the run time makes by itself lies among the other objects, where nobody looks for an inner object.
    const LayoutData& layout = pair.outer->layout();
    JSCell* alone = program.state().objects->structures.allocate(program.vm(), allocationOf(program, layout, true));
    AOT_TEST_CHECK(!alone->markedBlock().hasInnerCells());
}

// Whichever of the two somebody holds, both last, and neither is given back while the other is in use.
static void checkBothLast(LoadedProgram& program, Kept kept)
{
    VM& vm = program.vm();
    auto hidden = makeUniqueWithoutFastMallocCheck<Hidden>();
    for (const auto& order : ordersOfCollections) {
        vm.heap.collectNow(Sync, CollectionScope::Full);
        Strong<JSCell> root;
        Strong<JSCell> anchor;
        makeHiddenPairInBlockThatLasts(program, *hidden, root, kept, anchor);
        for (CollectionScope scope : order) {
            vm.heap.collectNow(Sync, scope);
            AOT_TEST_CHECK(isMarked(vm, hidden->outer) && isMarked(vm, hidden->inner));
            AOT_TEST_CHECK(isLive(hidden->outer) && isLive(hidden->inner));
        }
        root.clear();
        vm.heap.collectNow(Sync, CollectionScope::Full);
        AOT_TEST_CHECK(!isMarked(vm, hidden->outer) && !isMarked(vm, hidden->inner));
        AOT_TEST_CHECK(!isLive(hidden->outer) && !isLive(hidden->inner));
    }
}

static void testOuterObjectKeepsInnerObject(LoadedProgram& program)
{
    checkBothLast(program, Kept::Outer);
}

static void testInnerObjectKeepsOuterObject(LoadedProgram& program)
{
    checkBothLast(program, Kept::Inner);
}

static NEVER_INLINE void pointFieldElsewhere(LoadedProgram& program, Hidden& hidden)
{
    VM& vm = program.vm();
    Pair other = makePair(program);
    auto* outer = std::bit_cast<Object*>(~hidden.outer);
    auto* inner = std::bit_cast<Object*>(~hidden.inner);
    JSCell* string = jsNontrivialString(vm, "held by the old inner object alone"_s);
    hidden.other = hide(string);
    AOT_TEST_CHECK(inner->tryStore(vm, field(program, inner, "s"_s), string));
    AOT_TEST_CHECK(outer->tryStore(vm, field(program, outer, "c"_s), other.inner));
}

// Nothing but a reference keeps an inner object in use. Its bytes last as long as the cell, what it refers to does not: so it loses its header at the end of the marking that finds no mark for it.
static void testInnerObjectNobodyHoldsIsNotVisitedAndLosesItsHeader(LoadedProgram& program)
{
    VM& vm = program.vm();
    auto hidden = makeUniqueWithoutFastMallocCheck<Hidden>();
    Strong<JSCell> root;
    makeHiddenPair(program, *hidden, root, Kept::Outer);
    vm.heap.collectNow(Sync, CollectionScope::Full);
    AOT_TEST_CHECK(hasHeader(hidden->inner));
    pointFieldElsewhere(program, *hidden);
    vm.heap.collectNow(Sync, CollectionScope::Eden);
    vm.heap.collectNow(Sync, CollectionScope::Full);
    vm.heap.collectNow(Sync, CollectionScope::Full);
    AOT_TEST_CHECK(isMarked(vm, hidden->outer) && !isMarked(vm, hidden->inner) && !isMarked(vm, hidden->other));
    AOT_TEST_CHECK(isLive(hidden->outer) && !hasHeader(hidden->inner));
}

static NEVER_INLINE void collectWithWordOnStack(VM& vm, uintptr_t hiddenCell, ptrdiff_t distance)
{
    volatile uintptr_t word = ~hiddenCell + distance;
    vm.heap.collectNow(Sync, CollectionScope::Full);
    AOT_TEST_CHECK(word);
}

// The fault: the field points elsewhere, collections pass and free what the old inner object referred to, and only then a word of the stack names it.
static void testStaleWordOfStackNamesInnerObjectNobodyHeld(LoadedProgram& program)
{
    VM& vm = program.vm();
    auto hidden = makeUniqueWithoutFastMallocCheck<Hidden>();
    for (ptrdiff_t distance : { 0, 8, 24 }) {
        Strong<JSCell> root;
        makeHiddenPair(program, *hidden, root, Kept::Outer);
        vm.heap.collectNow(Sync, CollectionScope::Full);
        pointFieldElsewhere(program, *hidden);
        vm.heap.collectNow(Sync, CollectionScope::Full);
        vm.heap.collectNow(Sync, CollectionScope::Full);
        vm.heap.sweepSynchronously();
        AOT_TEST_CHECK(!isLive(hidden->other));
        collectWithWordOnStack(vm, hidden->inner, distance);
        AOT_TEST_CHECK(isMarked(vm, hidden->outer) && !isMarked(vm, hidden->inner) && !isMarked(vm, hidden->other));
    }
}

// The field points elsewhere, so nothing in the heap names the old inner object. An address on the stack, of the object or of a place in it, holds the object, what it refers to, and the cell.
static void testWordOfStackHoldsInnerObject(LoadedProgram& program)
{
    VM& vm = program.vm();
    auto hidden = makeUniqueWithoutFastMallocCheck<Hidden>();
    for (bool outerObjectIsHeld : { true, false }) {
        for (ptrdiff_t distance : { 0, 8, 24 }) {
            Strong<JSCell> root;
            makeHiddenPair(program, *hidden, root, Kept::Outer);
            vm.heap.collectNow(Sync, CollectionScope::Full);
            pointFieldElsewhere(program, *hidden);
            if (!outerObjectIsHeld)
                root.clear();
            collectWithWordOnStack(vm, hidden->inner, distance);
            AOT_TEST_CHECK(isMarked(vm, hidden->inner) && isMarked(vm, hidden->other) && isMarked(vm, hidden->outer));
        }
    }
}

static NEVER_INLINE void collectWithComplementOnStack(VM& vm, uint64_t complement)
{
    volatile uint64_t word = ~complement;
    vm.heap.collectNow(Sync, CollectionScope::Full);
    AOT_TEST_CHECK(word);
}

static NEVER_INLINE uint64_t complementOfNarrowReference(uintptr_t hiddenCell, unsigned shift)
{
    auto* cell = std::bit_cast<JSCell*>(~hiddenCell);
    return ~(static_cast<uint64_t>(MarkedBlockCage::encode(cageBaseOf(cell), cell)) << shift);
}

// Compiled code keeps references of 32 bits in registers and frames: either half of a word may be one.
static void testHalfOfStackWordHoldsInnerObject(LoadedProgram& program)
{
    VM& vm = program.vm();
    // The scan looks at halves of words only for a program that has such references.
    AOT_TEST_CHECK(MarkedBlockCage::require().has_value() && MarkedBlockCage::isRequired());
    auto hidden = makeUniqueWithoutFastMallocCheck<Hidden>();
    for (bool outerObjectIsHeld : { true, false }) {
        for (unsigned shift : { 0, 32 }) {
            Strong<JSCell> root;
            makeHiddenPair(program, *hidden, root, Kept::Outer);
            vm.heap.collectNow(Sync, CollectionScope::Full);
            pointFieldElsewhere(program, *hidden);
            if (!outerObjectIsHeld)
                root.clear();
            collectWithComplementOnStack(vm, complementOfNarrowReference(hidden->inner, shift));
            AOT_TEST_CHECK(isMarked(vm, hidden->inner) && isMarked(vm, hidden->other) && isMarked(vm, hidden->outer));
        }
    }
}

static NEVER_INLINE void storeYoungCell(LoadedProgram& program, Hidden& hidden, bool throughInnerObject)
{
    VM& vm = program.vm();
    JSCell* young = jsNontrivialString(vm, "a young string"_s);
    hidden.other = hide(young);
    auto* object = std::bit_cast<Object*>(throughInnerObject ? ~hidden.inner : ~hidden.outer);
    AOT_TEST_CHECK(object->tryStore(vm, field(program, object, throughInnerObject ? "s"_s : "t"_s), young));
}

// Each of the two is remembered by itself.
static void testYoungCellStoredIntoOldCell(LoadedProgram& program)
{
    VM& vm = program.vm();
    auto hidden = makeUniqueWithoutFastMallocCheck<Hidden>();
    for (Kept kept : { Kept::Outer, Kept::Inner }) {
        for (bool throughInnerObject : { true, false }) {
            Strong<JSCell> root;
            makeHiddenPair(program, *hidden, root, kept);
            vm.heap.collectNow(Sync, CollectionScope::Full);
            vm.heap.collectNow(Sync, CollectionScope::Eden);
            storeYoungCell(program, *hidden, throughInnerObject);
            for (CollectionScope scope : { CollectionScope::Eden, CollectionScope::Eden, CollectionScope::Full }) {
                vm.heap.collectNow(Sync, scope);
                AOT_TEST_CHECK(isMarked(vm, hidden->other));
            }
        }
    }
}

static NEVER_INLINE size_t markCountOfBlockOf(uintptr_t hidden)
{
    return std::bit_cast<JSCell*>(~hidden)->markedBlock().markCount();
}

// A block is as full as its cells say, however many objects lie in them.
static void testInnerCellsAreNotCounted(LoadedProgram& program)
{
    VM& vm = program.vm();
    auto hidden = makeUniqueWithoutFastMallocCheck<Hidden>();
    vm.heap.collectNow(Sync, CollectionScope::Full);
    Strong<JSCell> first;
    makeHiddenPair(program, *hidden, first, Kept::Inner);
    vm.heap.collectNow(Sync, CollectionScope::Full);
    size_t before = markCountOfBlockOf(hidden->outer);
    uintptr_t cellOfBlock = hidden->outer;
    Strong<JSCell> second;
    makeHiddenPair(program, *hidden, second, Kept::Outer);
    AOT_TEST_CHECK(&std::bit_cast<JSCell*>(~cellOfBlock)->markedBlock() == &std::bit_cast<JSCell*>(~hidden->outer)->markedBlock());
    vm.heap.collectNow(Sync, CollectionScope::Full);
    AOT_TEST_CHECK(markCountOfBlockOf(cellOfBlock) == before + 1);
}

// A cell that was given back is handed out whole.
static void testCellIsUsedAgain(LoadedProgram& program)
{
    VM& vm = program.vm();
    auto hidden = makeUniqueWithoutFastMallocCheck<Hidden>();
    Strong<JSCell> root;
    Strong<JSCell> anchor;
    makeHiddenPairInBlockThatLasts(program, *hidden, root, Kept::Outer, anchor);
    uintptr_t freed = hidden->outer;
    // What the making of the pair has left on the stack is overwritten by the first collection.
    vm.heap.collectNow(Sync, CollectionScope::Full);
    root.clear();
    vm.heap.collectNow(Sync, CollectionScope::Full);
    AOT_TEST_CHECK(!isLive(freed));
    bool wasUsedAgain = false;
    for (unsigned attempt = 0; attempt < 4096 && !wasUsedAgain; ++attempt) {
        makeHiddenPair(program, *hidden, root, Kept::Inner);
        wasUsedAgain = hidden->outer == freed;
    }
    AOT_TEST_CHECK(wasUsedAgain);
    vm.heap.collectNow(Sync, CollectionScope::Full);
    AOT_TEST_CHECK(isLive(hidden->outer) && isLive(hidden->inner));
}

// THE TESTS ABOVE HAVE TEETH: each part of the collector's side is left out by --aot-sabotage, and what the test above it demands is seen to fail. What is asserted here is the damage.
static void testSabotagedStackScanNamesNoInnerObject(LoadedProgram& program)
{
    VM& vm = program.vm();
    auto hidden = makeUniqueWithoutFastMallocCheck<Hidden>();
    Strong<JSCell> root;
    makeHiddenPair(program, *hidden, root, Kept::Outer);
    vm.heap.collectNow(Sync, CollectionScope::Full);
    pointFieldElsewhere(program, *hidden);
    collectWithWordOnStack(vm, hidden->inner, 0);
    AOT_TEST_CHECK(isMarked(vm, hidden->outer) && !isMarked(vm, hidden->inner) && !isMarked(vm, hidden->other));
}

static void testSabotagedScanOfHalvesNamesNoInnerObject(LoadedProgram& program)
{
    VM& vm = program.vm();
    AOT_TEST_CHECK(MarkedBlockCage::require().has_value() && MarkedBlockCage::isRequired());
    auto hidden = makeUniqueWithoutFastMallocCheck<Hidden>();
    Strong<JSCell> root;
    makeHiddenPair(program, *hidden, root, Kept::Outer);
    vm.heap.collectNow(Sync, CollectionScope::Full);
    pointFieldElsewhere(program, *hidden);
    collectWithComplementOnStack(vm, complementOfNarrowReference(hidden->inner, 32));
    AOT_TEST_CHECK(isMarked(vm, hidden->outer) && !isMarked(vm, hidden->inner) && !isMarked(vm, hidden->other));
}

static void testSabotagedVisitInPlaceLeavesOutFields(LoadedProgram& program)
{
    VM& vm = program.vm();
    auto hidden = makeUniqueWithoutFastMallocCheck<Hidden>();
    Strong<JSCell> root;
    makeHiddenPair(program, *hidden, root, Kept::Outer);
    storeYoungCell(program, *hidden, true);
    vm.heap.collectNow(Sync, CollectionScope::Full);
    AOT_TEST_CHECK(isMarked(vm, hidden->outer) && isMarked(vm, hidden->inner) && !isMarked(vm, hidden->other));
}

static void testSabotagedVisitInPlaceLeavesCellWhite(LoadedProgram& program)
{
    VM& vm = program.vm();
    auto hidden = makeUniqueWithoutFastMallocCheck<Hidden>();
    Strong<JSCell> root;
    makeHiddenPair(program, *hidden, root, Kept::Outer);
    vm.heap.collectNow(Sync, CollectionScope::Full);
    AOT_TEST_CHECK(isMarked(vm, hidden->inner) && std::bit_cast<JSCell*>(~hidden->inner)->cellState() == CellState::DefinitelyWhite);
    AOT_TEST_CHECK(std::bit_cast<JSCell*>(~hidden->outer)->cellState() == CellState::PossiblyBlack);
}

static void testSabotagedMarkerLeavesOuterCellUnmarked(LoadedProgram& program)
{
    VM& vm = program.vm();
    auto hidden = makeUniqueWithoutFastMallocCheck<Hidden>();
    Strong<JSCell> root;
    Strong<JSCell> anchor;
    makeHiddenPairInBlockThatLasts(program, *hidden, root, Kept::Inner, anchor);
    // A register may still hold the outer object. Another pair takes its place there.
    auto other = makeUniqueWithoutFastMallocCheck<Hidden>();
    Strong<JSCell> otherRoot;
    makeHiddenPair(program, *other, otherRoot, Kept::Outer);
    vm.heap.collectNow(Sync, CollectionScope::Full);
    AOT_TEST_CHECK(isMarked(vm, hidden->inner) && !isMarked(vm, hidden->outer));
}

static void testSabotagedInnerObjectLetGoKeepsItsHeader(LoadedProgram& program)
{
    VM& vm = program.vm();
    auto hidden = makeUniqueWithoutFastMallocCheck<Hidden>();
    Strong<JSCell> root;
    makeHiddenPair(program, *hidden, root, Kept::Outer);
    vm.heap.collectNow(Sync, CollectionScope::Full);
    pointFieldElsewhere(program, *hidden);
    // As in testInnerObjectNobodyHoldsIsNotVisitedAndLosesItsHeader(), whose last line this sabotage turns false. One collection is not enough: a slot of the stack that nobody uses any more may still hold the inner object.
    vm.heap.collectNow(Sync, CollectionScope::Eden);
    vm.heap.collectNow(Sync, CollectionScope::Full);
    vm.heap.collectNow(Sync, CollectionScope::Full);
    AOT_TEST_CHECK(isMarked(vm, hidden->outer) && !isMarked(vm, hidden->inner) && !isMarked(vm, hidden->other) && hasHeader(hidden->inner));
}

template<LoadedProgramTest body>
static constexpr void (*withProgram)() = runNativeTestWithLoadedProgram<innerObjectTests, programText, body>;

template<LoadedProgramTest body, const ASCIILiteral& optionWord>
static void runWithSabotagedProgram()
{
    for (const NativeTest& test : innerObjectTests()) {
        if (test.run == &runWithSabotagedProgram<body, optionWord>) {
            runWithLoadedProgram(test.name, programText, body, { optionWord });
            return;
        }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static constexpr ASCIILiteral stackNamesNoInnerCell = "sabotage=StackNamesNoInnerCell"_s;
static constexpr ASCIILiteral narrowReferenceOnStackNamesNoInnerCell = "sabotage=NarrowReferenceOnStackNamesNoInnerCell"_s;
static constexpr ASCIILiteral innerObjectIsVisitedWithoutItsFields = "sabotage=InnerObjectIsVisitedWithoutItsFields"_s;
static constexpr ASCIILiteral innerCellVisitedInPlaceStaysWhite = "sabotage=InnerCellVisitedInPlaceStaysWhite"_s;
static constexpr ASCIILiteral innerCellIsMarkedWithoutItsOuterCell = "sabotage=InnerCellIsMarkedWithoutItsOuterCell"_s;
static constexpr ASCIILiteral innerCellLetGoKeepsItsHeader = "sabotage=InnerCellLetGoKeepsItsHeader"_s;

std::span<const NativeTest> innerObjectTests()
{
    static constexpr NativeTest tests[] = {
        { "inner-objects/cell-lies-in-a-block-of-its-own-kind"_s, withProgram<testCellLiesInBlockOfItsOwnKind> },
        { "inner-objects/outer-object-keeps-inner-object"_s, withProgram<testOuterObjectKeepsInnerObject> },
        { "inner-objects/inner-object-keeps-outer-object"_s, withProgram<testInnerObjectKeepsOuterObject> },
        { "inner-objects/inner-object-nobody-holds-is-not-visited-and-loses-its-header"_s, withProgram<testInnerObjectNobodyHoldsIsNotVisitedAndLosesItsHeader> },
        { "inner-objects/stale-word-of-the-stack-names-inner-object-nobody-held"_s, withProgram<testStaleWordOfStackNamesInnerObjectNobodyHeld> },
        { "inner-objects/word-of-the-stack-holds-inner-object"_s, withProgram<testWordOfStackHoldsInnerObject> },
        { "inner-objects/half-of-a-stack-word-holds-inner-object"_s, withProgram<testHalfOfStackWordHoldsInnerObject> },
        { "inner-objects/young-cell-stored-into-an-old-cell"_s, withProgram<testYoungCellStoredIntoOldCell> },
        { "inner-objects/inner-cells-are-not-counted"_s, withProgram<testInnerCellsAreNotCounted> },
        { "inner-objects/cell-is-used-again"_s, withProgram<testCellIsUsedAgain> },
        { "inner-objects/sabotaged/stack-names-no-inner-cell"_s, runWithSabotagedProgram<testSabotagedStackScanNamesNoInnerObject, stackNamesNoInnerCell> },
        { "inner-objects/sabotaged/narrow-reference-on-stack-names-no-inner-cell"_s, runWithSabotagedProgram<testSabotagedScanOfHalvesNamesNoInnerObject, narrowReferenceOnStackNamesNoInnerCell> },
        { "inner-objects/sabotaged/inner-object-is-visited-without-its-fields"_s, runWithSabotagedProgram<testSabotagedVisitInPlaceLeavesOutFields, innerObjectIsVisitedWithoutItsFields> },
        { "inner-objects/sabotaged/inner-cell-visited-in-place-stays-white"_s, runWithSabotagedProgram<testSabotagedVisitInPlaceLeavesCellWhite, innerCellVisitedInPlaceStaysWhite> },
        { "inner-objects/sabotaged/inner-cell-is-marked-without-its-outer-cell"_s, runWithSabotagedProgram<testSabotagedMarkerLeavesOuterCellUnmarked, innerCellIsMarkedWithoutItsOuterCell> },
        { "inner-objects/sabotaged/inner-cell-let-go-keeps-its-header"_s, runWithSabotagedProgram<testSabotagedInnerObjectLetGoKeepsItsHeader, innerCellLetGoKeepsItsHeader> },
    };
    return tests;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
