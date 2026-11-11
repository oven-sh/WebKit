/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTEnvironment.h"
#include "AOTFunctionNames.h"
#include "AOTFunctionTables.h"
#include "AOTImage.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTOperationCallScope.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTSlimClosure.h"
#include "AOTVector.h"
#include "BrandedStructure.h"
#include "HeapIterationScope.h"
#include "JSCInlines.h"
#include "MarkedSpaceInlines.h"
#include "ObjectConstructor.h"
#include "StrongInlines.h"
#include "WeakInlines.h"
#include <array>
#include <bit>
#include <wtf/text/MakeString.h>

#if ENABLE(WEBASSEMBLY)
#include "WebAssemblyGCStructure.h"
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT {

std::span<const NativeTest> compiledStructureTests();

static_assert(sizeof(Structure) == 112);

static constexpr ASCIILiteral programText = R"IR(layout L0 name="Circle" fields=[{key="radius" declared={type=number} flags=[IsRequired]}]
layout L1 name="Tagged" fields=[{key="name" declared={type=anything} flags=[IsRequired]}, {key=symbol("tag")@m0:120 declared={type=anything} flags=[IsRequired]}]
layout L2 kind=Environment name="scope" fields=[{key="count" declared={type=number}}]
module m0 path="/main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="publish" [isFreeName]
    v2 = NewClosure function=f1
    v3 = Call v1, -, v2
    Return
}
function f1 name="make" parameters=[{type=anything}, {type=anything}, {type=anything}] results=[{type=anything}] {
    v3 = Check v1 expectedType=number
    v4 = Check v2 expectedType=string
    v5 = NewObject -, v3 layout=L0 fields=[0]
    ElemSet v5, v4, v3
    v7 = Call v0, -, v5
    v8 = NewObject -, v0, v0 layout=L1 fields=[0, 1]
    ElemSet v8, v4, v3
    v10 = Call v0, -, v8
    v11 = NewObject -, v3 layout=L2 fields=[0]
    v12 = NewClosure v11 function=f2
    v13 = Call v0, -, v12
    v14 = NewClosure v11 function=f3
    v15 = Call v0, -, v14
    Return
}
function f2 name="increment" kind=Arrow captures=[{type=env L2}] results=[{type=anything}] {
  b0(v0: env L2):
    v1 = GetEnvironmentField v0 layout=L2 field=0
    v2 = Integer integer=1
    v3 = Add v1, v2
    PutEnvironmentField v0, v3 layout=L2 field=0
    Return v3
}
function f3 name="read" kind=Arrow captures=[{type=env L2}] results=[{type=anything}] {
  b0(v0: env L2):
    v1 = GetEnvironmentField v0 layout=L2 field=0
    Return v1
}
)IR"_s;

static const LayoutData& layoutNamed(LoadedProgram& program, ASCIILiteral name)
{
    const ProgramTables& tables = program.state().tables;
    for (uint32_t shape = 1; shape < tables.firstEnvironmentShape; ++shape) {
        if (tables.hasLayoutForShape(shape) && isNamedInTextOfLoadedProgram(tables.layoutForShape(shape), name))
            return tables.layoutForShape(shape);
    }
    failNativeTest(__FILE__, __LINE__, name.characters());
}

static uint32_t shapeOfEnvironment(LoadedProgram& program)
{
    const ProgramTables& tables = program.state().tables;
    for (uint32_t shape = tables.firstEnvironmentShape; shape <= tables.numberOfShapes; ++shape) {
        if (tables.environmentForShape(shape).kind == LayoutKind::Environment)
            return shape;
    }
    failNativeTest(__FILE__, __LINE__, "the image has an environment");
}

static StructureTable& structures(LoadedProgram& program)
{
    return program.state().objects->structures;
}

static CompiledStructure* usualStructure(LoadedProgram& program, ASCIILiteral layoutName)
{
    return structures(program).objectStructure(program.vm(), layoutNamed(program, layoutName).shape);
}

static JSObject* allocateObject(LoadedProgram& program, ASCIILiteral layoutName)
{
    const ProgramTables& tables = program.state().tables;
    uint32_t shape = layoutNamed(program, layoutName).shape;
    for (uint32_t number = 0; number < tables.allocations().size(); ++number) {
        if (tables.allocations()[number].shape == shape)
            return asObject(structures(program).allocate(program.vm(), number));
    }
    failNativeTest(__FILE__, __LINE__, layoutName.characters());
}

static Structure* createOrdinaryStructure(LoadedProgram& program, unsigned numberOfProperties)
{
    VM& vm = program.vm();
    Structure* structure = JSFinalObject::createStructure(vm, program.realm(), program.realm()->objectPrototype(), JSFinalObject::defaultInlineCapacity);
    for (unsigned i = 0; i < numberOfProperties; ++i) {
        PropertyOffset offset;
        structure = Structure::addPropertyTransition(vm, structure, Identifier::fromString(vm, makeString("extra"_s, i)), 0, offset);
    }
    return structure;
}

static OptionSet<CompiledStructure::Flag> flagsOfVariant(const CompiledStructure* structure)
{
    return structure->flags() - OptionSet { CompiledStructure::Flag::HasFieldsWithSymbolKeys, CompiledStructure::Flag::HasOnlyPlainSlots, CompiledStructure::Flag::IsCopiedByWords, CompiledStructure::Flag::HasOnlyConfigurableFields };
}

template<typename T>
static T wordAt(const CompiledStructure* structure, ptrdiff_t offset)
{
    return *reinterpret_cast<const T*>(reinterpret_cast<const uint8_t*>(structure) + offset);
}

static void testUsualStructureIsCreatedInItsSpace(LoadedProgram& program)
{
    VM& vm = program.vm();
    const LayoutData& layout = layoutNamed(program, "Circle"_s);
    CompiledStructure* structure = structures(program).objectStructure(vm, layout.shape);
    AOT_TEST_CHECK(structure->subspace() == vm.heap.compiledStructureSpace<SubspaceAccess::OnMainThread>());
    AOT_TEST_CHECK(program.realm()->objectPrototype()->structure()->subspace() != vm.heap.compiledStructureSpace<SubspaceAccess::OnMainThread>());
    AOT_TEST_CHECK(structure->structure() == vm.structureStructure.get() && static_cast<JSCell*>(structure)->type() == StructureType);

    AOT_TEST_CHECK(structure->isCompiledStructure() && structure->describesObject() && !structure->describesEnvironment());
    AOT_TEST_CHECK(structure->typeInfo().type() == CompiledObjectType);
    AOT_TEST_CHECK(&structure->layout() == &layout);
    AOT_TEST_CHECK(structure->realm() == program.realm());
    AOT_TEST_CHECK(structure->storedPrototype() == JSValue(program.realm()->objectPrototype()));
    AOT_TEST_CHECK(flagsOfVariant(structure) == OptionSet { CompiledStructure::Flag::HasOnlyFieldsBelowObjectPrototype } && !structure->mayBePrototype());
    AOT_TEST_CHECK(!structure->extraPropertiesStructure() && !structure->extraPropertiesCellSize());

    AOT_TEST_CHECK(structures(program).objectStructure(vm, layout.shape) == structure);
    AOT_TEST_CHECK(structures(program).structure(vm, layout.shape) == structure);
    AOT_TEST_CHECK(usualStructure(program, "Tagged"_s) != structure);
    AOT_TEST_CHECK(allocateObject(program, "Circle"_s)->structure() == structure);
}

static void testWordsCompiledCodeReads(LoadedProgram& program)
{
    VM& vm = program.vm();
    const LayoutData& layout = layoutNamed(program, "Circle"_s);
    AOT_TEST_CHECK(layout.extraPropertiesSlot() && !layout.extensionSlot());
    CompiledStructure* usual = usualStructure(program, "Circle"_s);
    Structure* extra = createOrdinaryStructure(program, 2);
    CompiledStructure* named = structures(program).withExtraProperties(vm, *usual, extra);

    for (const CompiledStructure* structure : { usual, named }) {
        AOT_TEST_CHECK(wordAt<const LayoutData*>(structure, CompiledStructure::offsetOfLayout()) == &layout);
        AOT_TEST_CHECK(wordAt<SlotData>(structure, CompiledStructure::offsetOfExtraPropertiesSlot()) == *layout.extraPropertiesSlot());
        AOT_TEST_CHECK(structure->extraPropertiesSlot() == layout.extraPropertiesSlot());
        AOT_TEST_CHECK(!wordAt<SlotData>(structure, CompiledStructure::offsetOfExtensionSlot()).encoded() && !structure->extensionSlot());
        AOT_TEST_CHECK(wordAt<uint8_t>(structure, CompiledStructure::offsetOfFlags()) == structure->flags().toRaw());
    }
    AOT_TEST_CHECK(!usual->hasExtraProperties() && !usual->extraPropertiesStructureID() && !usual->extraPropertiesCellSize());
    AOT_TEST_CHECK(named->hasExtraProperties() && named->extraPropertiesStructureID() == extra->id());
    AOT_TEST_CHECK(wordAt<uint32_t>(named, CompiledStructure::offsetOfExtraPropertiesStructureID()) == extra->id().bits());
    AOT_TEST_CHECK(wordAt<uint16_t>(named, CompiledStructure::offsetOfExtraPropertiesCellSize()) == named->extraPropertiesCellSize());
    AOT_TEST_CHECK(wordAt<uint16_t>(named, CompiledStructure::offsetOfExtraPropertiesCellSize()) == roundUpToMultipleOf<MarkedSpace::sizeStep>(JSFinalObject::allocationSize(extra->inlineCapacity())));
}

static void testStructureOfEnvironments(LoadedProgram& program)
{
    VM& vm = program.vm();
    uint32_t shape = shapeOfEnvironment(program);
    Structure* structure = structures(program).structure(vm, shape);
    auto* compiled = dynamicDowncast<CompiledStructure>(structure);
    AOT_TEST_CHECK(compiled && compiled->describesEnvironment() && !compiled->describesObject());
    AOT_TEST_CHECK(compiled->subspace() == vm.heap.compiledStructureSpace<SubspaceAccess::OnMainThread>());
    AOT_TEST_CHECK(compiled->typeInfo().type() == CompiledEnvironmentType);
    AOT_TEST_CHECK(compiled->storedPrototype().isNull() && compiled->flags().isEmpty());
    AOT_TEST_CHECK(compiled->environmentShapePage() == shape >> environmentShapesPerPageShift);
    AOT_TEST_CHECK(wordAt<uint16_t>(compiled, CompiledStructure::offsetOfEnvironmentShapePage()) == compiled->environmentShapePage());
    AOT_TEST_CHECK(structures(program).structure(vm, shape) == structure);
    AOT_TEST_CHECK(!compiledObjectStructureOrNull(structure) && !compiledLayoutOrNull(structure));
}

static Environment* allocateEnvironment(LoadedProgram& program)
{
    const ProgramTables& tables = program.state().tables;
    uint32_t shape = shapeOfEnvironment(program);
    for (uint32_t number = 0; number < tables.allocations().size(); ++number) {
        if (tables.allocations()[number].shape == shape)
            return uncheckedDowncast<Environment>(structures(program).allocate(program.vm(), number));
    }
    failNativeTest(__FILE__, __LINE__, "the image allocates an environment");
}

static NEVER_INLINE Weak<JSObject> storeNewObjectInFirstVariable(LoadedProgram& program, Environment* environment)
{
    JSObject* object = constructEmptyObject(program.realm());
    environment->at<uint64_t>(offsetOfFirstEnvironmentVariable) = static_cast<uint64_t>(JSValue::encode(object));
    program.vm().writeBarrier(environment, object);
    return Weak<JSObject>(object);
}

static void testMarkInHeaderOfEnvironmentLeavesItsShape(LoadedProgram& program)
{
    VM& vm = program.vm();
    uint32_t shape = shapeOfEnvironment(program);
    Environment* environment = allocateEnvironment(program);
    const EnvironmentData& layout = environment->layout();
    uint64_t header = Environment::headerFor(environment->structure(), shape);
    AOT_TEST_CHECK(layout.shape == shape && !environment->isEscaped() && environment->at<uint64_t>(0) == header);

    environment->markEscaped();
    AOT_TEST_CHECK(environment->isEscaped() && &environment->layout() == &layout);
    AOT_TEST_CHECK(environment->at<uint64_t>(0) == (header | static_cast<uint64_t>(environmentIsEscapedBit) << 8 * offsetOfEnvironmentShapeHighByte));
    AOT_TEST_CHECK(environment->at<uint8_t>(offsetOfEnvironmentShapeLowByte) == lowByteOfEnvironmentShape(shape));

    Weak<JSObject> held = storeNewObjectInFirstVariable(program, environment);
    vm.heap.collectSync(CollectionScope::Full);
    vm.heap.collectSync(CollectionScope::Full);
    AOT_TEST_CHECK(held && environment->isEscaped() && &environment->layout() == &layout);
    ensureStillAliveHere(environment);
}

static void testCollectorTakesLinkBetweenEnvironmentsForNoCell(LoadedProgram& program)
{
    VM& vm = program.vm();
    Environment* environment = allocateEnvironment(program);
    constexpr uint64_t addressOfNoCell = 3 * MarkedBlock::atomSize;
    for (uint64_t next : { static_cast<uint64_t>(0), addressOfNoCell }) {
        environment->at<uint64_t>(offsetOfFirstEnvironmentVariable) = next + tagOfLinkBetweenEnvironmentsHandedBack;
        vm.heap.collectSync(CollectionScope::Full);
        AOT_TEST_CHECK(environment->at<uint64_t>(offsetOfFirstEnvironmentVariable) == next + tagOfLinkBetweenEnvironmentsHandedBack);
    }
    ensureStillAliveHere(environment);
}

static FunctionID slimFunctionNamed(LoadedProgram& program, ASCIILiteral name)
{
    const ProgramTables& tables = program.state().tables;
    const uint64_t* closureWords = FunctionTables::of(program.vm()).words;
    for (uint32_t index = tables.closureFunctions.first; index < tables.closureFunctions.end; ++index) {
        FunctionID function(index);
        if (closureWords[closureNumberOf(function)] && functionKindData(function).isSlim && writtenFunctionName(function) == name)
            return function;
    }
    failNativeTest(__FILE__, __LINE__, name.characters());
}

static uint64_t headerOfSlimClosureInFrame(LoadedProgram& program, FunctionID function)
{
    return program.instance().slimClosureHeaders[functionKindData(function).slimClosureKind()] | static_cast<uint64_t>(frameCellState) << 8 * JSCell::cellStateOffset();
}

namespace {

struct alignas(16) ClosureInFrameWithOneCapture {
    static constexpr size_t placeOfCapture = offsetOfSlimClosureCaptures / sizeof(uint64_t);
    static constexpr size_t placeOfCopyInHeap = offsetOfCopyInHeapOfClosureInFrame(offsetOfSlimClosureCaptures, 1) / sizeof(uint64_t);

    void make(LoadedProgram& program, FunctionID function, uint64_t capture)
    {
        words = { headerOfSlimClosureInFrame(program, function), FunctionTables::of(program.vm()).words[closureNumberOf(function)], capture, 0 };
    }

    JSCell* moveToHeap(LoadedProgram& program) { return callOperationFromTest(program.vm(), operationAOTMoveClosureToHeap, &program.instance(), reinterpret_cast<JSCell*>(words.data())); }
    uint64_t address() const { return std::bit_cast<uintptr_t>(words.data()); }
    JSCell* copyInHeap() const { return std::bit_cast<JSCell*>(words[placeOfCopyInHeap]); }

    std::array<uint64_t, sizeOfClosureInFrame(offsetOfSlimClosureCaptures, 1) / sizeof(uint64_t)> words { };
};
static_assert(ClosureInFrameWithOneCapture::placeOfCopyInHeap == ClosureInFrameWithOneCapture::placeOfCapture + 1 && sizeof(ClosureInFrameWithOneCapture) == (ClosureInFrameWithOneCapture::placeOfCopyInHeap + 1) * sizeof(uint64_t));

} // anonymous namespace

static uint64_t captureOf(JSCell* closureInHeap)
{
    return uncheckedDowncast<SlimClosure>(closureInHeap)->captures()[0];
}

static unsigned numberOfClosuresInHeap(LoadedProgram& program, FunctionID function)
{
    VM& vm = program.vm();
    uint64_t word = FunctionTables::of(vm).words[closureNumberOf(function)];
    unsigned count = 0;
    HeapIterationScope iterationScope(vm.heap);
    vm.heap.objectSpace().forEachLiveCell(iterationScope, [&](HeapCell* cell, HeapCell::Kind kind) {
        count += isJSCellKind(kind) && static_cast<JSCell*>(cell)->type() == SlimClosureType && reinterpret_cast<uint64_t*>(cell)[offsetOfSlimClosureWord / sizeof(uint64_t)] == word;
        return IterationStatus::Continue;
    });
    return count;
}

static void testClosureMovedToHeapMarksEnvironmentThatItHolds(LoadedProgram& program)
{
    FunctionID function = slimFunctionNamed(program, "increment"_s);
    CaptureCounts counts = captureCountsOf(function);
    AOT_TEST_CHECK(counts.references == 1 && counts.captures == 1);
    Environment* environment = allocateEnvironment(program);
    Environment* other = allocateEnvironment(program);
    const EnvironmentData& layout = environment->layout();

    struct {
        ClosureInFrameWithOneCapture closure;
        uint64_t wordBehindClosure;
    } inFrame;
    inFrame.closure.make(program, function, std::bit_cast<uint64_t>(environment));
    inFrame.wordBehindClosure = std::bit_cast<uint64_t>(other);
    JSCell* moved = inFrame.closure.moveToHeap(program);
    AOT_TEST_CHECK(moved->type() == SlimClosureType && moved->cellState() == CellState::DefinitelyWhite && uncheckedDowncast<SlimClosure>(moved)->function() == function);
    AOT_TEST_CHECK(captureOf(moved) == std::bit_cast<uint64_t>(environment) && inFrame.closure.copyInHeap() == moved);
    AOT_TEST_CHECK(environment->isEscaped() && &environment->layout() == &layout && !other->isEscaped() && inFrame.wordBehindClosure == std::bit_cast<uint64_t>(other));

    JSObject* object = constructEmptyObject(program.realm());
    uint8_t flagsOfObject = object->inlineTypeFlags();
    for (JSValue held : { JSValue(object), JSValue(), jsUndefined(), jsNumber(7), jsNumber(0.5) }) {
        inFrame.closure.make(program, function, static_cast<uint64_t>(JSValue::encode(held)));
        moved = inFrame.closure.moveToHeap(program);
        AOT_TEST_CHECK(captureOf(moved) == static_cast<uint64_t>(JSValue::encode(held)));
    }
    AOT_TEST_CHECK(object->inlineTypeFlags() == flagsOfObject && !other->isEscaped());
}

static void testClosureMovedToHeapTakesAlongClosuresOfFramesThatItHolds(LoadedProgram& program)
{
    VM& vm = program.vm();
    FunctionID function = slimFunctionNamed(program, "increment"_s);
    Environment* environment = allocateEnvironment(program);

    constexpr size_t depth = 2000;
    std::array<ClosureInFrameWithOneCapture, depth> inFrames;
    inFrames[0].make(program, function, std::bit_cast<uint64_t>(environment));
    for (size_t level = 1; level < depth; ++level)
        inFrames[level].make(program, function, inFrames[level - 1].address());

    JSCell* moved = inFrames[depth - 1].moveToHeap(program);
    inFrames.fill(ClosureInFrameWithOneCapture { });
    vm.heap.collectSync(CollectionScope::Full);
    JSCell* closure = moved;
    for (size_t level = depth; --level;) {
        AOT_TEST_CHECK(closure->type() == SlimClosureType && static_cast<uint8_t>(closure->cellState()) != frameCellState && uncheckedDowncast<SlimClosure>(closure)->function() == function);
        closure = std::bit_cast<JSCell*>(captureOf(closure));
    }
    AOT_TEST_CHECK(closure->type() == SlimClosureType && captureOf(closure) == std::bit_cast<uint64_t>(environment) && environment->isEscaped());
    ensureStillAliveHere(moved);
}

static void testClosureMovedToHeapTwiceIsOneCell(LoadedProgram& program)
{
    VM& vm = program.vm();
    FunctionID function = slimFunctionNamed(program, "increment"_s);
    Environment* environment = allocateEnvironment(program);

    ClosureInFrameWithOneCapture inFrame;
    inFrame.make(program, function, std::bit_cast<uint64_t>(environment));
    AOT_TEST_CHECK(!inFrame.copyInHeap());
    JSCell* moved = inFrame.moveToHeap(program);
    AOT_TEST_CHECK(inFrame.copyInHeap() == moved && static_cast<uint8_t>(reinterpret_cast<JSCell*>(inFrame.words.data())->cellState()) == frameCellState);
    vm.heap.collectSync(CollectionScope::Full);
    AOT_TEST_CHECK(inFrame.moveToHeap(program) == moved && inFrame.copyInHeap() == moved && captureOf(moved) == std::bit_cast<uint64_t>(environment));

    std::array<ClosureInFrameWithOneCapture, 2> holders;
    for (ClosureInFrameWithOneCapture& holder : holders) {
        holder.make(program, function, inFrame.address());
        AOT_TEST_CHECK(captureOf(holder.moveToHeap(program)) == std::bit_cast<uint64_t>(moved));
    }
    AOT_TEST_CHECK(holders[0].copyInHeap() != holders[1].copyInHeap() && inFrame.copyInHeap() == moved);
    ensureStillAliveHere(moved);
}

static void testChainOfClosuresMovedToHeapManyTimesIsCopiedOnce(LoadedProgram& program)
{
    VM& vm = program.vm();
    DeferGC deferGC(vm);
    FunctionID function = slimFunctionNamed(program, "increment"_s);
    Environment* environment = allocateEnvironment(program);
    unsigned numberOfClosuresBefore = numberOfClosuresInHeap(program, function);

    constexpr size_t depth = 2000;
    std::array<ClosureInFrameWithOneCapture, depth> inFrames;
    inFrames[0].make(program, function, std::bit_cast<uint64_t>(environment));
    for (size_t level = 1; level < depth; ++level)
        inFrames[level].make(program, function, inFrames[level - 1].address());

    JSCell* moved = inFrames[depth - 1].moveToHeap(program);
    AOT_TEST_CHECK(numberOfClosuresInHeap(program, function) == numberOfClosuresBefore + depth);
    for (size_t turn = 1; turn < depth; ++turn)
        AOT_TEST_CHECK(inFrames[depth - 1].moveToHeap(program) == moved);
    JSCell* closure = moved;
    for (size_t level = depth; level--;) {
        AOT_TEST_CHECK(inFrames[level].moveToHeap(program) == closure);
        closure = std::bit_cast<JSCell*>(captureOf(closure));
    }
    AOT_TEST_CHECK(closure == environment && numberOfClosuresInHeap(program, function) == numberOfClosuresBefore + depth);

    std::array<ClosureInFrameWithOneCapture, depth> movedFromBottom;
    movedFromBottom[0].make(program, function, std::bit_cast<uint64_t>(environment));
    for (size_t level = 1; level < depth; ++level)
        movedFromBottom[level].make(program, function, movedFromBottom[level - 1].address());
    for (size_t level = 0; level < depth; ++level) {
        JSCell* copy = movedFromBottom[level].moveToHeap(program);
        AOT_TEST_CHECK(captureOf(copy) == (level ? std::bit_cast<uint64_t>(movedFromBottom[level - 1].copyInHeap()) : std::bit_cast<uint64_t>(environment)));
    }
    AOT_TEST_CHECK(numberOfClosuresInHeap(program, function) == numberOfClosuresBefore + 2 * depth);
}

static void testSlotOfFrameThatIsUsedAgainIsCopiedAgain(LoadedProgram& program)
{
    FunctionID function = slimFunctionNamed(program, "increment"_s);
    Environment* first = allocateEnvironment(program);
    Environment* second = allocateEnvironment(program);

    ClosureInFrameWithOneCapture slot;
    slot.make(program, function, std::bit_cast<uint64_t>(first));
    JSCell* copyOfFirst = slot.moveToHeap(program);
    slot.make(program, function, std::bit_cast<uint64_t>(second));
    AOT_TEST_CHECK(!slot.copyInHeap() && !second->isEscaped());
    JSCell* copyOfSecond = slot.moveToHeap(program);
    AOT_TEST_CHECK(copyOfSecond != copyOfFirst && slot.copyInHeap() == copyOfSecond && second->isEscaped());
    AOT_TEST_CHECK(captureOf(copyOfFirst) == std::bit_cast<uint64_t>(first) && captureOf(copyOfSecond) == std::bit_cast<uint64_t>(second));
    ensureStillAliveHere(copyOfFirst);
}

static void testComponentsOfVariantAreIndependent(LoadedProgram& program)
{
    VM& vm = program.vm();
    StructureTable& table = structures(program);
    CompiledStructure* usual = usualStructure(program, "Tagged"_s);
    JSObject* prototype = constructEmptyObject(program.realm());
    Structure* extra = createOrdinaryStructure(program, 1);
    const std::array<uint16_t, 2> entries { 1, 0 };

    CompiledStructure* oneWay = table.withOutOfLineKeyOrder(vm, *table.withExtraProperties(vm, *table.asPrototype(vm, *table.withPrototype(vm, *usual, prototype)), extra), entries);
    CompiledStructure* otherWay = table.withPrototype(vm, *table.asPrototype(vm, *table.withExtraProperties(vm, *table.withOutOfLineKeyOrder(vm, *usual, entries), extra)), prototype);
    AOT_TEST_CHECK(oneWay == otherWay);
    AOT_TEST_CHECK(oneWay->storedPrototype() == JSValue(prototype) && oneWay->mayBePrototype() && oneWay->extraPropertiesStructure() == extra);
    AOT_TEST_CHECK(oneWay->hasOnlyConfigurableFields());
    AOT_TEST_CHECK(flagsOfVariant(oneWay) == OptionSet({ CompiledStructure::Flag::HasExtraProperties, CompiledStructure::Flag::HasOtherPrototype, CompiledStructure::Flag::HasOutOfLineKeyOrder }));

    CompiledStructure* numbered = table.withNumberedKeyOrder(vm, *oneWay);
    AOT_TEST_CHECK(numbered != oneWay && !numbered->hasOutOfLineKeyOrder());
    AOT_TEST_CHECK(numbered->storedPrototype() == JSValue(prototype) && numbered->mayBePrototype() && numbered->extraPropertiesStructure() == extra);
}

static constexpr unsigned numberOfShortLivedVariants = 64;

static NEVER_INLINE void createVariants(LoadedProgram& program, MarkedArgumentBuffer& prototypes, Vector<Weak<Structure>>& variants)
{
    VM& vm = program.vm();
    CompiledStructure* usual = usualStructure(program, "Circle"_s);
    for (unsigned i = 0; i < numberOfShortLivedVariants; ++i) {
        prototypes.append(constructEmptyObject(program.realm()));
        variants.append(Weak<Structure>(structures(program).withPrototype(vm, *usual, prototypes.at(i))));
    }
}

static NEVER_INLINE void checkVariantsAreThoseMadeBefore(LoadedProgram& program, const MarkedArgumentBuffer& prototypes, const Vector<Weak<Structure>>& variants)
{
    VM& vm = program.vm();
    CompiledStructure* usual = usualStructure(program, "Circle"_s);
    AOT_TEST_CHECK(vm.heap.isMarked(usual));
    for (unsigned i = 0; i < numberOfShortLivedVariants; ++i) {
        CompiledStructure* variant = structures(program).withPrototype(vm, *usual, prototypes.at(i));
        AOT_TEST_CHECK(variant != usual && variant->storedPrototype() == prototypes.at(i) && variant->hasOtherPrototype());
        AOT_TEST_CHECK(variants[i] && variants[i].get() == variant);
    }
}

static void testVariantNobodyHoldsLivesAsLongAsItsPrototype(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSObject* object = allocateObject(program, "Circle"_s);
    MarkedArgumentBuffer prototypes;
    Vector<Weak<Structure>> variants;
    createVariants(program, prototypes, variants);
    vm.heap.collectSync(CollectionScope::Full);
    vm.heap.collectSync(CollectionScope::Full);
    checkVariantsAreThoseMadeBefore(program, prototypes, variants);

    AOT_TEST_CHECK(!prototypes.hasOverflowed());
    prototypes.clear();
    vm.heap.collectSync(CollectionScope::Full);
    vm.heap.collectSync(CollectionScope::Full);
    unsigned numberCollected = 0;
    for (const Weak<Structure>& variant : variants)
        numberCollected += !variant;
    AOT_TEST_CHECK(numberCollected > numberOfShortLivedVariants / 2);
    ensureStillAliveHere(object);
}

template<LoadedProgramTest body>
static constexpr void (*withProgram)() = runNativeTestWithLoadedProgram<compiledStructureTests, programText, body>;

static uint32_t allocationOf(LoadedProgram& program, ASCIILiteral layoutName)
{
    const ProgramTables& tables = program.state().tables;
    uint32_t shape = layoutNamed(program, layoutName).shape;
    for (uint32_t number = 0; number < tables.allocations().size(); ++number) {
        if (tables.allocations()[number].shape == shape)
            return number;
    }
    failNativeTest(__FILE__, __LINE__, layoutName.characters());
}

static void testHeaderOfAnAllocationComesWithoutACell(LoadedProgram& program)
{
    VM& vm = program.vm();
    DeferGC deferGC(vm);
    uint32_t number = allocationOf(program, "Tagged"_s);
    AOT_TEST_CHECK(!program.instance().objects.allocationSite(number));
    auto numberOfCellsWithHeader = [&](uint64_t header) {
        unsigned count = 0;
        HeapIterationScope iterationScope(vm.heap);
        vm.heap.objectSpace().forEachLiveCell(iterationScope, [&](HeapCell* cell, HeapCell::Kind kind) {
            count += isJSCellKind(kind) && *reinterpret_cast<uint64_t*>(cell) == header;
            return IterationStatus::Continue;
        });
        return count;
    };

    uint64_t header = callOperationFromTest(vm, operationAOTGetHeaderOfAllocation, &program.instance(), number);
    const AllocationSiteRecord* record = program.instance().objects.allocationSite(number);
    AOT_TEST_CHECK(record && record->header == header && header == cellHeaderFor(usualStructure(program, "Tagged"_s)));
    AOT_TEST_CHECK(!numberOfCellsWithHeader(header));
    AOT_TEST_CHECK(callOperationFromTest(vm, operationAOTGetHeaderOfAllocation, &program.instance(), number) == header && !numberOfCellsWithHeader(header));

    JSCell* cell = callOperationFromTest(vm, operationAOTAllocateSlow, &program.instance(), number);
    AOT_TEST_CHECK(*reinterpret_cast<uint64_t*>(cell) == header && numberOfCellsWithHeader(header) == 1);
}

std::span<const NativeTest> compiledStructureTests()
{
    static constexpr NativeTest tests[] = {
        { "compiled-structures/usual-structure-is-created-in-its-space"_s, withProgram<testUsualStructureIsCreatedInItsSpace> },
        { "compiled-structures/words-compiled-code-reads"_s, withProgram<testWordsCompiledCodeReads> },
        { "compiled-structures/structure-of-environments"_s, withProgram<testStructureOfEnvironments> },
        { "compiled-structures/mark-in-the-header-of-an-environment-leaves-its-shape"_s, withProgram<testMarkInHeaderOfEnvironmentLeavesItsShape> },
        { "compiled-structures/collector-takes-a-link-between-environments-for-no-cell"_s, withProgram<testCollectorTakesLinkBetweenEnvironmentsForNoCell> },
        { "compiled-structures/closure-moved-to-the-heap-marks-the-environment-that-it-holds"_s, withProgram<testClosureMovedToHeapMarksEnvironmentThatItHolds> },
        { "compiled-structures/closure-moved-to-the-heap-takes-along-the-closures-of-frames-that-it-holds"_s, withProgram<testClosureMovedToHeapTakesAlongClosuresOfFramesThatItHolds> },
        { "compiled-structures/closure-moved-to-the-heap-twice-is-one-cell"_s, withProgram<testClosureMovedToHeapTwiceIsOneCell> },
        { "compiled-structures/chain-of-closures-moved-to-the-heap-many-times-is-copied-once"_s, withProgram<testChainOfClosuresMovedToHeapManyTimesIsCopiedOnce> },
        { "compiled-structures/slot-of-the-frame-that-is-used-again-is-copied-again"_s, withProgram<testSlotOfFrameThatIsUsedAgainIsCopiedAgain> },
        { "compiled-structures/components-of-variant-are-independent"_s, withProgram<testComponentsOfVariantAreIndependent> },
        { "compiled-structures/variant-nobody-holds-lives-as-long-as-its-prototype"_s, withProgram<testVariantNobodyHoldsLivesAsLongAsItsPrototype> },
        { "compiled-structures/header-of-an-allocation-comes-without-a-cell"_s, withProgram<testHeaderOfAnAllocationComesWithoutACell> },
    };
    return tests;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
