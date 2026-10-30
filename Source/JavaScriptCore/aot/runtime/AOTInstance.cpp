/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTInstance.h"

#if ENABLE(AOT)

#include "AOTAllocationCounts.h"
#include "AOTArrayAllocationProfile.h"
#include "AOTBuiltinCells.h"
#include "AOTBuiltinFamilyState.h"
#include "AOTBuiltinModules.h"
#include "AOTBuiltinTable.h"
#include "AOTBuiltinTableBindings.h"
#include "AOTCallSiteCache.h"
#include "AOTCellFormat.h"
#include "AOTChainDependents.h"
#include "AOTCheckedFrameCells.h"
#include "AOTClassPrototype.h"
#include "AOTClosure.h"
#include "AOTCompactTable.h"
#include "AOTCompileOptions.h"
#include "AOTCoroutineTable.h"
#include "AOTEffectsDeclared.h"
#include "AOTFunctionTables.h"
#include "AOTGenerator.h"
#include "AOTGlobalBindings.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTHeapHooks.h"
#include "AOTHost.h"
#include "AOTImage.h"
#include "AOTImageOfCellScope.h"
#include "AOTImageLoading.h"
#include "AOTLimits.h"
#include "AOTMembersLeftOut.h"
#include "AOTModuleHost.h"
#include "AOTModuleNamespace.h"
#include "AOTModuleRecord.h"
#include "AOTModules.h"
#include "AOTNativeCallValidation.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTOperationData.h"
#include "AOTOperationScope.h"
#include "AOTProgramIdentifiers.h"
#include "AOTProgramStatics.h"
#include "AOTProgramStrings.h"
#include "AOTPropertyCaches.h"
#include "AOTPropertyCachesStringKeys.h"
#include "AOTRealmStartup.h"
#include "AOTRegExpRecords.h"
#include "AOTRuntimeCounters.h"
#include "AOTRuntimeFunctions.h"
#include "AOTRuntimeHooks.h"
#include "AOTRuntimeReporter.h"
#include "AOTRuntimeTable.h"
#include "AOTRuntimeValidation.h"
#include "AOTSavedFrame.h"
#include "AOTSlimClosure.h"
#include "AOTSlotAccess.h"
#include "AOTState.h"
#include "AOTStringBuilderStorage.h"
#include "AOTStubBlob.h"
#include "AOTStubEntrypoints.h"
#include "AOTStubFrontEnds.h"
#include "AOTStubsExceptions.h"
#include "AOTThreshold.h"
#include "AOTUnwinding.h"
#include "AOTVMState.h"
#include "AbstractSlotVisitorInlines.h"
#include "AddressSpaceBudget.h"
#include "DeferGC.h"
#include "ErrorConstructor.h"
#include "FastMarkAppender.h"
#include "IntlObject.h"
#include "JSAsyncGenerator.h"
#include "JSBoundFunction.h"
#include "JSCInlines.h"
#include "JSCJSValueInlines.h"
#include "JSFunction.h"
#include "JSPromise.h"
#include "LLIntData.h"
#include "LLIntThunks.h"
#include "LocalAllocator.h"
#include "MarkedBlockCage.h"
#include "NativeExecutable.h"
#include "ObjectPrototype.h"
#include "Opcode.h"
#include "Options.h"
#include "ReservedRangeHeap.h"
#include "SlotVisitorInlines.h"
#include "Symbol.h"
#include "VM.h"
#include "VMTrapsInlines.h"
#include "WebAssemblyFunction.h"
#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <wtf/Atomics.h>
#include <wtf/CryptographicallyRandomNumber.h>
#include <wtf/DataLog.h>
#include <wtf/HashFunctions.h>
#include <wtf/MathExtras.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/OSAllocator.h>
#include <wtf/PageBlock.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/ThreadSpecific.h>
#include <wtf/WTFConfig.h>

#if ENABLE(WEBASSEMBLY)
#include "JSWebAssemblyInstance.h"
#endif

#if OS(LINUX)
#include <errno.h>
#include <sys/mman.h>
#endif

#if USE(MIMALLOC)
WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
#include <bmalloc/mimalloc.h>
WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
#endif

namespace JSC { namespace AOT {

static std::pair<size_t, size_t> reservationSizes(const Image& image)
{
    InstanceSizes sizes = image.programTables().instanceSizes;
    RELEASE_ASSERT(InstanceLayout::isAddressable(sizes));
    return { InstanceLayout::bytesReservedBelowInstance(sizes), InstanceLayout::bytesReservedFromInstance(sizes) };
}

// The table of the image is never written. An instance changes its own copy where something changes what its objects find under the name of a method.
static void copyMethodTable(Instance* instance, const Image& image)
{
    std::span<const uint32_t> table = image.methodTable();
    if (table.empty())
        return;
    InstanceSizes sizes = image.programTables().instanceSizes;
    RELEASE_ASSERT(table.size_bytes() == sizes.sizeOfMethodTable);
    auto* copy = reinterpret_cast<char*>(instance) + InstanceLayout::offsetOfMethodTable(sizes);
    size_t size = InstanceLayout::bytesReservedForMethodTable(sizes);
    if (mapPrivateCloneOfImagePages({ reinterpret_cast<const uint8_t*>(table.data()), size }, copy))
        return;
    memcpySpan(std::span { reinterpret_cast<uint32_t*>(copy), table.size() }, table);
    OSAllocator::protect(copy, size, true, false);
}

static Instance* reserveMemory(PagesOfVM& pages, const Image& image)
{
    RELEASE_ASSERT(!(InstanceLayout::granuleBelowInstance % WTF::pageSize()) && !(InstanceLayout::granuleAboveInstance % WTF::pageSize()));
    auto [below, above] = reservationSizes(image);
    auto* instance = reinterpret_cast<Instance*>(static_cast<char*>(pages.allocate(below + above)) + below);
    copyMethodTable(instance, image);
    return instance;
}

static void releaseMemory(Instance* instance)
{
    VMState& vmState = instance->vm->aotState();
    auto [below, above] = reservationSizes(*instance->program);
    vmState.pages->release(reinterpret_cast<char*>(instance) - below, below + above);
}

static void fillHeaderAndPrefix(Instance& instance, VM& vm, JSGlobalObject* realm)
{
    instance.runtimeTable = vm.aotState().runtimeTable->entries();
    instance.shapeSets = vm.aotState().image->programTables().shapeSetBits().data();
    instance.vm = &vm;
    instance.globalObject = realm;
    instance.numberTag = JSValue::NumberTag;
    instance.notCellMask = JSValue::NotCellMask;
    instance.singleCharacterStrings = vm.smallStrings.singleCharacterStrings();
    instance.collation.ducetLevel1Weights = ducetLevel1Weights;

    InstancePrefix& prefix = instance.prefix();
    prefix.structureIDBase = JSC::structureIDBase();
    prefix.notUndefinedOrNullMask = ~static_cast<int64_t>(JSValue::ValueUndefined);
    prefix.boxedNaN = JSValue::encode(jsNaN());
    prefix.one = 1;
    prefix.pureNaN = PNaN;
    prefix.minInt64 = std::numeric_limits<int64_t>::min();
    prefix.minIntegerAddedTo = minIntegerAddedTo;
    prefix.maxIntegerAddedTo = maxIntegerAddedTo;
    prefix.distanceOfIntegersAddedToFromZero = -minIntegerAddedTo;
    prefix.spanOfIntegersAddedTo = maxIntegerAddedTo - minIntegerAddedTo;
    prefix.doubleEncodeOffset = JSValue::DoubleEncodeOffset;
    prefix.boxedOne = JSValue::encode(jsNumber(1));
    prefix.undefinedInteger = undefinedInteger;
    prefix.nullInteger = nullInteger;
    prefix.absentInteger = absentInteger;
    prefix.undefinedDouble = undefinedDouble;
    prefix.nullDouble = nullDouble;
    prefix.absentDouble = absentDouble;
    prefix.arrayAllocator = subspaceFor<JSArray>(vm)->allocatorFor(sizeof(JSArray), AllocatorForMode::EnsureAllocator).localAllocator();
    prefix.ropeStringAllocator = subspaceFor<JSRopeString>(vm)->allocatorFor(sizeof(JSRopeString), AllocatorForMode::EnsureAllocator).localAllocator();
    RELEASE_ASSERT(static_cast<LocalAllocator*>(prefix.arrayAllocator)->cellSize() == sizeof(JSArray));
    RELEASE_ASSERT(static_cast<LocalAllocator*>(prefix.ropeStringAllocator)->cellSize() == sizeof(JSRopeString));
    prefix.emptyString = vm.smallStrings.emptyString();
    prefix.stringHeader = cellHeaderFor(vm.stringStructure.get());
}

static void fillStubReturnAddresses(Instance& instance, const Image& image)
{
    for (unsigned kind = 0; kind < numberOfStubFrameKinds; ++kind) {
        if (uint32_t offset = image.header().singleReturnSites[kind])
            instance.stubs.returnAddresses[kind] = image.sharedCodeCopy(0) + offset;
    }
}

static void fillImageWords(Instance& instance, VM& vm, const Image& image)
{
    ImageWords& words = instance.image;
    words.code = image.code().data();
    words.codeGranules = image.section<uint32_t>(ImageSection::CodeGranules).data();
    words.subsequentFunctionStarts = image.section<uint32_t>(ImageSection::FunctionStarts).data() + 1;
    words.tables = &image.programTables();
    words.fieldTableEntries = image.programTables().fieldTableEntries().data();
    words.fieldRecordIndices = image.programTables().fieldRecordIndices().data();
    words.classAccessors = image.programTables().classAccessors().data();
    words.data = image.programTables().data().data();
    words.closureFunctionKinds = image.programTables().closureFunctionKinds().data() - image.programTables().closureFunctions.numbersBelow;
#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
    words.stringRecords = image.text().stringRecords().data();
    words.lazyStringHeaderOffsets = image.text().lazyHeaderOffsets();
    instance.lazyStrings = { image.text().lazyHeaders(), image.text().lazyHeadersCapacity() };
#else
    words.programIdentifiers = vm.aotState().identifiers->identifiers();
    words.sizeOfProgramIdentifiers = static_cast<uint64_t>(vm.aotState().identifiers->count()) * sizeof(UniquedStringImpl);
#endif
    static_assert(sizeof(ImageStringWord) == sizeof(uint32_t));
    words.stringWords = image.section<uint32_t>(ImageSection::Strings).data();
    words.stringConstants = image.section<uint32_t>(ImageSection::StringConstants).data();
    words.listedStrings = image.section<uint32_t>(ImageSection::LongStrings).data();
    words.text = image.section<uint8_t>(ImageSection::Text).data();
    words.programSymbols = &vm.aotState().identifiers->symbols();
    fillStubReturnAddresses(instance, image);
}

static constexpr auto powersOfTen = [] {
    std::array<double, 309> result { };
    for (int32_t k = 0; k < static_cast<int32_t>(result.size()); ++k) {
        double power = 1;
        double square = 10;
        for (int32_t bits = k; bits; bits >>= 1) {
            if (bits & 1)
                power *= square;
            if (bits >> 1)
                square *= square;
        }
        result[k] = power;
    }
    return result;
}();
static_assert(powersOfTen.size() == static_cast<size_t>(std::numeric_limits<double>::max_exponent10) + 1);

static void fillNumberWords(Instance& instance)
{
    NumberWords& words = instance.numbers;
    words.powersOfTen = powersOfTen.data();
    static_assert(std::size(WTF::intHashSecrets) == std::extent_v<decltype(NumberWords::hashSecrets)>);
    for (size_t i = 0; i < std::size(WTF::intHashSecrets); ++i)
        words.hashSecrets[i] = WTF::intHashSecrets[i];
}

static void fillAllocationSpaces(Instance& instance, VM& vm)
{
    instance.allocation.auxiliarySpace = &vm.auxiliarySpace();
    instance.allocation.cagedAuxiliarySpace = vm.heap.cagedAuxiliarySpace<SubspaceAccess::OnMainThread>();
    instance.allocation.cellSpace = &vm.cellSpace();
    instance.allocation.primitiveAuxiliarySpace = &vm.primitiveGigacageAuxiliarySpace();
    instance.allocation.compactTableHeader = cellHeaderFor(vm.aotState().runtimeTable->compactTableStructure());
}

static void fillArrayHeaders(Instance& instance, JSGlobalObject* realm)
{
    instance.allocation.frameArrayHeader = 0;
    if (realm->isHavingABadTime())
        return;
    InstancePrefix& prefix = instance.prefix();
    prefix.newArrayWithInt32Header = cellHeaderFor(realm->arrayStructureForIndexingTypeDuringAllocation(ArrayWithInt32));
    prefix.newArrayWithDoubleHeader = cellHeaderFor(realm->arrayStructureForIndexingTypeDuringAllocation(ArrayWithDouble));
    prefix.newArrayWithContiguousHeader = cellHeaderFor(realm->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous));
    instance.allocation.newCopyOnWriteArrayHeaders[0] = cellHeaderFor(realm->arrayStructureForIndexingTypeDuringAllocation(CopyOnWriteArrayWithInt32));
    instance.allocation.newCopyOnWriteArrayHeaders[1] = cellHeaderFor(realm->arrayStructureForIndexingTypeDuringAllocation(CopyOnWriteArrayWithDouble));
    prefix.newCopyOnWriteArrayWithContiguousHeader = cellHeaderFor(realm->arrayStructureForIndexingTypeDuringAllocation(CopyOnWriteArrayWithContiguous));

    Structure* arraysOfValues = realm->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous);
    if (arraysOfValues->isPristine() && arraysOfValues->hasPristineElements()) {
        constexpr unsigned cellStateShift = JSCell::cellStateOffset() * 8;
        instance.allocation.frameArrayHeader = (prefix.newArrayWithContiguousHeader & ~(0xffull << cellStateShift)) | static_cast<uint64_t>(frameCellState) << cellStateShift;
    }
}

static void fillRealmWords(Instance& instance, VM& vm, JSGlobalObject* realm)
{
    // No cell has the number zero. didCreateInstanceStructure() fills the word of a class that the realm creates later.
    if (Structure* structure = realm->mapStructureConcurrently())
        instance.receiverStructureID(Receiver::Map) = structure->id().bits();
    if (Structure* structure = realm->setStructureConcurrently())
        instance.receiverStructureID(Receiver::Set) = structure->id().bits();
    instance.receiverStructureID(Receiver::RegExp) = realm->regExpStructure()->id().bits();
    for (const BuiltinClassBinding& binding : BuiltinBindings::classesOfHost()) {
        if (Structure* structure = binding.findInstanceStructure ? binding.findInstanceStructure(realm) : nullptr)
            instance.didCreateInstanceStructure(binding.builtinClass, structure);
    }

    RealmWords& words = instance.realm;
    words.boundFunctionStructureID = realm->boundFunctionStructure()->id().bits();
    words.hostFunctionStructureID = realm->hostFunctionStructure()->id().bits();
    // Compiled code binds compiled closures, which are no JSFunctions: a slim one ends before JSFunction's executable. JSBoundFunction::create picks the same for them.
    constexpr bool targetIsJSFunction = false;
    words.boundFunctionExecutable = vm.getBoundFunction(targetIsJSFunction, SourceTaintedOrigin::Untainted);
    // One for the realm: whichever image runs when an object gets its first extra property, the object's own image knows the structure.
    Instance* first = realm->aotInstance();
    words.emptyExtraPropertiesStructure = first && first != &instance ? first->realm.emptyExtraPropertiesStructure : JSFinalObject::createStructure(vm, realm, jsNull(), extraPropertiesInlineCapacity);
    words.extraPropertiesWithElementsStructure = Structure::nonPropertyTransition(vm, words.emptyExtraPropertiesStructure, TransitionKind::AllocateContiguous, nullptr);
    words.headerOfExtraPropertiesWithElements = cellHeaderFor(words.extraPropertiesWithElementsStructure);
    words.boundFunctionClassInfo = JSBoundFunction::info();
    words.objectPrototype = realm->objectPrototype();
    words.functionPrototypeCall = realm->linkTimeConstant(LinkTimeConstant::callFunction);
    words.smallIntStrings = vm.numericStrings.smallIntCache();
    words.intStrings = vm.numericStrings.intCache();
    words.keysOfPairs = StringKeyCache::noKeysOfPairs();
    words.keyOfPairMask = 0;
}

static void fillInstanceClassInfos(Instance& instance)
{
    unsigned numberOfClasses = BuiltinTable::singleton().classCount();
    std::span classInfos = unsafeMakeSpan(instance.state->tableArena.allocate<const ClassInfo*>(numberOfClasses), numberOfClasses);
    BuiltinBindings::fillInstanceClassInfos(classInfos);
    instance.realm.instanceClassInfos = classInfos.data();
}

CONCURRENT_SAFE void setPollsEnabled(Instance& instance, bool enabled)
{
    WTF::atomicStore(&instance.pollIsDue, static_cast<uintptr_t>(enabled), std::memory_order_relaxed);
#if CPU(X86_64) && (OS(LINUX) || OS(WINDOWS))
    OSAllocator::protect(reinterpret_cast<char*>(&instance) + InstanceLayout::offsetOfLoopPollPage(), WTF::pageSize(), !enabled, !enabled);
#endif
}

void makeTailCallsByValuePoll(Instance& instance, const uint8_t* isRenamedAtRunTime)
{
    StackManager::Mirror& limit = instance.callsByValue.limit.get();
    if (!limit.isOnList())
        return;
    instance.callsByValue.isRenamedAtRunTime = isRenamedAtRunTime + instance.image.tables->closureFunctions.first - instance.image.tables->closureFunctions.numbersBelow;
    // No change of the limit of the stack reaches the word any more.
    instance.vm->traps().unregisterMirror(limit);
    *std::bit_cast<uintptr_t*>(std::bit_cast<uintptr_t>(&limit) + StackManager::Mirror::offsetOfTrapAwareSoftStackLimit()) = std::numeric_limits<uintptr_t>::max();
}

static void registerStackMirror(Instance* instance, VM& vm)
{
    vm.traps().registerMirror(instance->stackMirror.get());
    vm.traps().registerMirror(instance->jsonLevels.limit.get());
    vm.traps().registerMirror(instance->callsByValue.limit.get());
    if (FunctionTables* tables = vm.aotState().functionTables.get(); tables && tables->recordsCalleesOfCallsByValue())
        makeTailCallsByValuePoll(*instance, tables->isRenamedAtRunTime.load(std::memory_order_acquire));
    VMState::PolledInstances& polled = vm.aotState().polledInstances;
    // The list grows outside the lock: a request to malloc may tell this thread that it owes an error, which takes the lock. This thread alone changes the list.
    decltype(polled.instances) instances;
    size_t numberOfInstances;
    {
        Locker locker { polled.lock };
        numberOfInstances = polled.instances.size();
    }
    instances.reserveInitialCapacity(numberOfInstances + 1);
    Locker locker { polled.lock };
    instances.appendVector(polled.instances);
    instances.append(instance);
    polled.instances.swap(instances);
    if (vm.traps().stopsAtEveryPoll() || vm.traps().needsOutOfMemoryError())
        setPollsEnabled(*instance, true);
}

static void unregisterStackMirror(Instance* instance, VM& vm)
{
    {
        VMState::PolledInstances& polled = vm.aotState().polledInstances;
        Locker locker { polled.lock };
        polled.instances.removeFirst(instance);
    }
    if (instance->callsByValue.limit.get().isOnList())
        vm.traps().unregisterMirror(instance->callsByValue.limit.get());
    vm.traps().unregisterMirror(instance->jsonLevels.limit.get());
    vm.traps().unregisterMirror(instance->stackMirror.get());
}

static void initializeRuntimeWords(Instance& instance, VM& vm)
{
    State& state = *instance.state;
    RuntimeWords& words = instance.runtime;
    std::span propertyRuns = state.tables.propertyRuns();
    size_t numberOfPropertyRuns = propertyRuns.empty() ? 0 : propertyRuns[0] - 1;
    words.literals.propertyRuns = state.tableArena.allocate<const PropertyRun*>(numberOfPropertyRuns);

    // Never zero: the generator would stay there.
    if (instance.options().has(TestingSwitch::FailPredictions))
        words.exceptions.predictionFailureState = 2 * instance.options().predictionFailureSeed + 1;

    if (instance.options().countsBranches) {
        words.counters.branchCounts = static_cast<uint64_t*>(state.pages.allocate(runtimeLimitValue(vm, LimitID::BranchCounters) * sizeof(uint64_t)));
        words.counters.allocationCounts = static_cast<uint64_t*>(state.pages.allocate(ExactAllocationCounts::sizeInBytes(state.tables.allocations().size())));
    }

    if (size_t numberOfGlobalBindings = state.tables.globalBindings().size()) {
        words.counters.globalBindings = InstanceLayout::holdsGlobalBindings(numberOfGlobalBindings * sizeof(GlobalBinding))
            ? instance.tableAt<GlobalBinding>(InstanceLayout::offsetOfGlobalBindings)
            : state.tableArena.allocate<GlobalBinding>(numberOfGlobalBindings);
        state.globalBindings = makeUnique<GlobalBindings>(state);
    }
}

size_t numberOfRunningInstances(const VMState& state)
{
    return std::ranges::count_if(state.instances, [](const Instance* instance) {
        return !instance->globalObject || instance->globalObject->microtaskRunnability() != QueuedTaskResult::Discard;
    });
}

static Instance* reserveInstanceOfRunningImage(VM& vm, JSGlobalObject* realm)
{
    VMState& vmState = vm.aotState();
    const Image& image = *vmState.image;
    vmState.runtimeTable->finishInitialization(vm);

    Instance* instance = reserveMemory(*vmState.pages, image);
    instance->program = &image;
    instance->partsOfImageInVM = vmState.partsOfImageThatRuns;
    instance->endOfClosureNumbers = image.header().numbers.endOfClosureNumbers;
    instance->lastShape = image.header().numbers.lastShape;
    fillHeaderAndPrefix(*instance, vm, realm);
    fillImageWords(*instance, vm, image);
    fillNumberWords(*instance);
    fillAllocationSpaces(*instance, vm);
    vmState.propertyCaches->publishWords(*instance);
    return instance;
}

Instance* reserveInstance(JSGlobalObject* realm)
{
    VM& vm = realm->vm();
    RELEASE_ASSERT(vm.hasCompiledCode() && !realm->aotInstance());
#if ENABLE(WEBASSEMBLY)
    RELEASE_ASSERT(static_cast<ptrdiff_t>(AOT_INSTANCE_OFFSET(vm)) == JSWebAssemblyInstance::offsetOfVM());
#endif
    Instance::destroyDeadInstances(vm);
    VMState& vmState = vm.aotState();
    RunningImageScope firstImage(vmState, &vmState.image->first());
    if (!(vmState.image->programTables().flags & ProgramTables::mayRunInMultipleInstances) && numberOfRunningInstances(vmState))
        return nullptr;
    vmState.identifiers->createEngineSymbols();

    Instance* first = reserveInstanceOfRunningImage(vm, realm);
    Instance* last = first;
    for (const Image* image = first->program->above(); image; image = image->above()) {
        RunningImageScope scope(vmState, image);
        last->above = reserveInstanceOfRunningImage(vm, realm);
        last = last->above;
    }
    realm->setAOTInstance(first);
    return first;
}

Instance* Instance::findInstanceOfRunningImage()
{
    const Image* running = vm->aotState().image;
    Instance* instance = this;
    while (instance->program != running) {
        instance = instance->above;
        RELEASE_ASSERT(instance);
    }
    return instance;
}

void Instance::forEachOf(JSGlobalObject* realm, const ScopedLambda<void(Instance&)>& functor)
{
    for (Instance* instance = realm->aotInstance(); instance; instance = instance->above) {
        RunningImageScope scope(instance->vm->aotState(), instance->program);
        functor(*instance);
    }
}

static std::optional<uint32_t> closureNumberOfCell(const JSCell* cell)
{
    switch (cell->type()) {
    case CompiledClosureType:
        return uncheckedDowncast<Closure>(cell)->closureNumber();
    case SlimClosureType:
        return uncheckedDowncast<SlimClosure>(cell)->closureNumber();
    case SlimClosureWithPropertiesType:
        return uncheckedDowncast<ReifiedSlimClosure>(cell)->closureNumber();
    default:
        return std::nullopt;
    }
}

void ImageOfCellScope::runImageOf(const JSCell* cell)
{
    JSGlobalObject* realm = cell->structure()->realm();
    if (!realm || !realm->vm().aotState().hasSeveralImages)
        return;
    if (Instance* owner = Instance::owning(cell))
        m_scope.emplace(realm->vm().aotState(), owner->program);
}

bool belongsToOtherImageSlow(VM& vm, const JSCell* cell)
{
    const Image& running = *vm.aotState().image;
    if (cell->type() == CompiledObjectType) {
        uint32_t shape = uncheckedDowncast<Object>(cell)->shape();
        return shape <= running.header().numbers.shapesBelow || shape > running.header().numbers.lastShape;
    }
    if (std::optional<uint32_t> closureNumber = closureNumberOfCell(cell))
        return !running.programTables().hasClosureNumber(*closureNumber);
    const ClassInfo* info = cell->classInfo();
    if (info == ClassPrototype::info())
        return uncheckedDowncast<ClassPrototype>(cell)->classNumber() - running.layoutIndicesBelow() >= running.programTables().numberOfLayoutIndices;
    if (info == ModuleNamespace::info())
        return uncheckedDowncast<ModuleNamespace>(cell)->imagePosition() != running.position();
    return false;
}

// Never in line: a caller that had the rare call below in its own body would keep slots for what it saves around that call and write them on that road alone,
// and the collector reads every word of the frames between a job and the body of an async generator.
NEVER_INLINE Instance* Instance::owning(const JSCell* cell)
{
    Instance* first = cell->structure()->realm()->aotInstance();
    if (!first || !first->above) [[likely]]
        return first;
    return first->findInstanceOwning(cell);
}

// Apart, so that a realm with one instance saves no registers for it.
NEVER_INLINE Instance* Instance::findInstanceOwning(const JSCell* cell)
{
    Instance* first = this;
    if (cell->type() == CompiledObjectType)
        return &first->instanceWithShape(uncheckedDowncast<Object>(cell)->shape());
    if (std::optional<uint32_t> closureNumber = closureNumberOfCell(cell))
        return &first->instanceWithClosureNumber(*closureNumber);
    auto atPosition = [&](unsigned position) {
        Instance* instance = first;
        while (position--)
            instance = instance->above;
        return instance;
    };
    const ClassInfo* info = cell->classInfo();
    if (info == ClassPrototype::info()) {
        uint32_t classNumber = uncheckedDowncast<ClassPrototype>(cell)->classNumber();
        Instance* instance = first;
        while (classNumber - instance->program->layoutIndicesBelow() >= instance->program->programTables().numberOfLayoutIndices) {
            instance = instance->above;
            RELEASE_ASSERT(instance);
        }
        return instance;
    }
    if (info == ModuleNamespace::info())
        return atPosition(uncheckedDowncast<ModuleNamespace>(cell)->imagePosition());
    if (info == SavedFrame::info()) {
        if (const Image* image = first->program->findInListByCodeAddress(removeCodePtrTag(uncheckedDowncast<SavedFrame>(cell)->resumeAddress())))
            return atPosition(image->position());
    }
    return first->findInstanceOfRunningImage();
}

Instance& Instance::instanceWithClosureNumber(uint32_t number)
{
    // The images of a realm hand the numbers out one behind the other.
    Instance* instance = globalObject->aotInstance();
    while (number >= instance->endOfClosureNumbers) {
        instance = instance->above;
        RELEASE_ASSERT(instance);
    }
    return *instance;
}

Instance& Instance::instanceWithShape(uint32_t shape)
{
    Instance* instance = globalObject->aotInstance();
    while (shape > instance->lastShape) {
        instance = instance->above;
        RELEASE_ASSERT(instance);
    }
    return *instance;
}

// A header holds the pristine bit of its Structure. The other inline allocations are prepared again by their first allocation.
void Instance::didCreateInstanceStructure(BuiltinClassID builtinClass, Structure* structure)
{
    if (builtinClass == BuiltinClasses::Map)
        receiverStructureID(Receiver::Map) = structure->id().bits();
    else if (builtinClass == BuiltinClasses::Set)
        receiverStructureID(Receiver::Set) = structure->id().bits();
    else if (builtinClass == BuiltinClasses::Buffer)
        receiverStructureID(Receiver::Buffer) = structure->id().bits();
}

void Instance::findShortArrayAllocators()
{
    static_assert(LocalAllocator::offsetOfFreeList() + sizeof(FreeList) <= ShortArrayWords::wordsOfExhaustedAllocator * sizeof(uint64_t));
    bool hasHeaders = prefix().newArrayWithContiguousHeader;
    // Nothing is allocated from it, so nothing reads the size of its cells.
    uint8_t* exhausted = reinterpret_cast<uint8_t*>(shortArrays.exhaustedAllocator);
    new (NotNull, exhausted + LocalAllocator::offsetOfFreeList()) FreeList(static_cast<unsigned>(MarkedSpace::sizeStep));
    for (unsigned i = 0; i < ShortArrayWords::numberOfAllocators; ++i) {
        size_t cellSize = sizeOfArrayWithStorage(2 * i + 1);
        static_assert(sizeOfArrayWithStorage(2 * ShortArrayWords::numberOfAllocators - 1) <= MarkedSpace::largeCutoff);
        RELEASE_ASSERT(ShortArrayWords::indexOfAllocator(2 * i + 1) == i && !(cellSize % MarkedSpace::sizeStep));
        LocalAllocator* allocator = hasHeaders ? vm->cellSpace().allocatorFor(cellSize, AllocatorForMode::AllocatorIfExists).localAllocator() : nullptr;
        shortArrays.allocators[i] = allocator ? static_cast<void*>(allocator) : exhausted;
    }
}

void Instance::captureCellHeaders()
{
    fillArrayHeaders(*this, globalObject);
    findShortArrayAllocators();
    for (auto& inlineAllocation : allocation.inlineAllocations)
        inlineAllocation = { };
    for (auto& typedArrayAllocation : allocation.typedArrayAllocations)
        typedArrayAllocation = { };
    prepareInlineAllocation<JSPromise>(this, InlineAllocation::Promise, globalObject->promiseStructure());
    // A cell has the pristine bit that its Structure had when the cell was made.
    realm.emptyArgumentList = JSArray::create(*vm, globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous));
}

Instance* Instance::create(JSGlobalObject* realm)
{
    VM& vm = realm->vm();
    RELEASE_ASSERT(vm.hasCompiledCode());
    DeferGCForAWhile deferGC(vm);
    destroyDeadInstances(vm);
    if (auto& functionTables = vm.aotState().functionTables)
        functionTables->forgetRecordedCalleesIfNotEntered(vm);

    if (!realm->aotInstance())
        RELEASE_ASSERT(reserveInstance(realm));
    Instance* instance = Instance::of(realm);
    RELEASE_ASSERT(!instance->state);

    instance->captureCellHeaders();
    fillRealmWords(*instance, vm, realm);
    if (std::span staticsLayout = instance->tables().staticsLayout(); !staticsLayout.empty())
        instance->arrays.distanceFromProfileToLastArray = staticsLayout[0].distanceFromArrayProfileToLastArray;
    initializeCollectorWords(vm, *instance);
    initializeStubWords(*instance);
    initializeIterationWords(*instance);

    State::create(*instance);
    instance->state->caches->addClearedEachCollection("short young arrays converted"_s, asMutableByteSpan(instance->arrays.shortYoungArraysConverted));
    instance->state->caches->addClearedEachCollection("last array looked at in vain"_s, asMutableByteSpan(instance->arrays.lastArrayLookedAtInVain));
    instance->state->caches->addClearedEachCollection("chains above lists without names"_s, asMutableByteSpan(instance->arrays.chainsAboveListsWithoutNames));
    instance->state->caches->addClearedEachCollection("structures of lists without names"_s, asMutableByteSpan(instance->arrays.structuresOfListsWithoutNames));
    instance->state->caches->addClearedEachCollection("copies without frame"_s, asWritableBytes(instance->copiesWithoutFrame()));
    // A place names the cell of a key and a Structure by their addresses, and the address of one that has ended goes to another.
    instance->state->caches->addClearedEachCollection("keyed value places"_s, asWritableBytes(instance->keyedValuePlaces()));
    fillInstanceClassInfos(*instance);
    initializeRuntimeWords(*instance, vm);

    registerStackMirror(instance, vm);
    VMState& vmState = vm.aotState();
    {
        Locker locker { vmState.instancesLock };
        vmState.instances.append(instance);
        vmState.didChangeInstances();
    }
    vmState.propertyCaches->didCreateInstance(*instance);
    RuntimeHooks::didChangeMicrotaskQueueing(vm);
#if defined(BUN_HARDENED)
    OSAllocator::protect(instance->tableAt<uint8_t>(InstanceLayout::offsetOfWordsWrittenOnce), InstanceLayout::offsetOfKeyOrderSteps - InstanceLayout::offsetOfWordsWrittenOnce, true, false);
    OSAllocator::protect(instance->boxedCalls().data(), InstanceLayout::bytesReservedForBoxedCalls, true, false);
#endif
    releaseUnusedPagesOfThread();
    return instance;
}

void Instance::destroy(Instance* instance)
{
    VM& vm = *instance->vm;
    VMState& vmState = vm.aotState();
    RunningImageScope scope(vmState, instance->program);
    {
        Locker locker { vmState.instancesLock };
        vmState.instances.removeFirst(instance);
        vmState.didChangeInstances();
        vmState.instancesToDestroy.removeFirst(instance);
    }
    vmState.counters->add(instance->runtime.counters);
    if (instance->state) {
        delete instance->state;
        unregisterStackMirror(instance, vm);
    }
    releaseMemory(instance);
}

void Instance::destroyDeadInstances(VM& vm)
{
    while (!vm.aotState().instancesToDestroy.isEmpty())
        destroy(vm.aotState().instancesToDestroy.last());
}

void Instance::destroyAll(VM& vm)
{
    destroyDeadInstances(vm);
    while (!vm.aotState().instances.isEmpty())
        destroy(vm.aotState().instances.last());
}

const RecordedOptions& Instance::options() const
{
    return program->header().options;
}

JSCell* livenessToken(Instance* instance)
{
    return instance->globalObject;
}

#if OS(LINUX)
#if defined(MADV_POPULATE_WRITE)
static constexpr int populateStateForWriting = MADV_POPULATE_WRITE;
#else
static constexpr int populateStateForWriting = 23;
#endif
#endif

void* ZeroedPages::allocate(size_t bytes, FirstUse firstUse)
{
    size_t size = roundUpToMultipleOf(pageSize(), std::max<size_t>(bytes, 1));
    void* pages = m_pagesOfVM.allocate(size);
#if OS(LINUX)
    // A kernel older than 5.14 refuses, and the pages fault in one by one.
    if (firstUse == FirstUse::AllAtOnce)
        madvise(pages, size, populateStateForWriting);
#else
    UNUSED_PARAM(firstUse);
#endif
    m_ranges.append({ pages, size });
    return pages;
}

void ZeroedPages::release(void* pages)
{
    size_t index = m_ranges.findIf([&](const auto& range) {
        return range.first == pages;
    });
    RELEASE_ASSERT(index != notFound);
    m_pagesOfVM.release(pages, m_ranges[index].second);
    m_ranges.removeAt(index);
}

ZeroedPages::~ZeroedPages()
{
    for (auto& [pages, size] : m_ranges)
        m_pagesOfVM.release(pages, size);
}

static size_t sizeOfZeroedTable(size_t count, size_t size)
{
    return roundUpToMultipleOf(pageSize(), std::max<size_t>(count, 1) * size);
}

static size_t arenaCapacity(const ProgramTables& tables)
{
    constexpr size_t sizeOfFixedSizeTables = 2 * MB;
    bool usesWideReferences = Image::current()->header().options.useWideReferences();
    size_t sizeOfCellReference = usesWideReferences ? sizeof(JSCell*) : sizeof(uint32_t);
    std::span propertyRuns = tables.propertyRuns();
    size_t numberOfPropertyRuns = propertyRuns.empty() ? 0 : propertyRuns[0] - 1;
    size_t capacity = sizeOfFixedSizeTables
        + sizeOfZeroedTable(tables.firstEnvironmentShape, sizeof(uint32_t))
        + sizeOfZeroedTable(tables.allocations().size() + 1, sizeof(AllocationSiteRecord) + sizeof(int32_t))
        + sizeOfZeroedTable(tables.numberOfClasses(), sizeof(JSCell*))
        + sizeOfZeroedTable(tables.numberOfClosureCells, sizeOfCellReference)
        + sizeOfZeroedTable(tables.literalStrings().size(), sizeOfCellReference)
        + sizeOfZeroedTable(tables.keyCellIdentifiers().size(), sizeOfCellReference)
        + sizeOfZeroedTable(tables.bigIntConstants().size(), sizeof(JSCell*))
        + sizeOfZeroedTable(InstanceLayout::holdsBuiltinCells(tables.builtinCells().size()) ? 0 : tables.builtinCells().size(), sizeof(JSCell*))
        + sizeOfZeroedTable(numberOfPropertyRuns, sizeof(const PropertyRun*))
        + sizeOfZeroedTable(SiteCacheState::numberOfStates, sizeof(SiteCacheState))
        + sizeOfZeroedTable(SiteCacheState::numberOfStates, sizeof(uint8_t))
        + sizeOfZeroedTable(SiteCaches::numberOfMissWords(tables.instanceSizes.numberOfSiteCaches), sizeof(uint64_t))
        + sizeOfZeroedTable(RememberedSlotSet::numberOfCardsFor(tables.instanceSizes.numberOfSiteCaches * sizeof(Slot), SiteCaches::regionShift), sizeof(uint8_t))
        + sizeOfZeroedTable(InstanceLayout::holdsGlobalBindings(tables.globalBindings().size() * sizeof(GlobalBinding)) ? 0 : tables.globalBindings().size(), sizeof(GlobalBinding))
        + sizeOfZeroedTable(tables.modules().size(), sizeof(uint8_t));
    if (usesWideReferences)
        capacity += sizeOfZeroedTable(tables.instanceSizes.numberOfStringCells, sizeof(JSCell*));
    return capacity;
}

ZeroedTableArena::ZeroedTableArena(ZeroedPages& pages, const ProgramTables& tables)
    : ZeroedTableArena(pages, arenaCapacity(tables))
{
}

ZeroedTableArena::ZeroedTableArena(ZeroedPages& pages, size_t capacity)
    : m_pages(pages)
    , m_start(static_cast<char*>(pages.allocate(capacity)))
    , m_capacity(capacity)
{
}

State::State(Instance& instance)
    : instance(instance)
    , tables(instance.tables())
    , sharesRealm(instance.vm->aotState().hasSeveralImages)
    , pages(*instance.vm->aotState().pages)
    , tableArena(pages, tables)
    , propertyCaches(*this)
    , stringKeys(*this)
{
    instance.state = this;
    caches = makeUnique<CacheRegistry>();
    immortalCells = makeUnique<ImmortalCells>(*this);
    strings = makeUnique<ProgramStrings>(*this);
    statics = makeUnique<ProgramStatics>(*this);
    builtinCells = makeUnique<BuiltinCells>(*this);
    chainDependents = makeUnique<ChainDependents>(*this);
    siteCaches = makeUniqueWithoutFastMallocCheck<SiteCaches>(*this);
    initializePropertyCacheWords(instance);
    modules = makeUnique<Modules>(*this);
    builtinModules = makeUnique<BuiltinModuleTable>(*this);
    objects = makeUnique<ObjectModel>(*this);
    coroutines = makeUnique<CoroutineTable>(*this);
    builtinFamilies = makeUnique<BuiltinFamilyState>(*this);
    caches->add(CacheKind::StrongRoots, "realm cells"_s, instance);
}

State::~State() = default;

State* State::create(Instance& instance)
{
    RELEASE_ASSERT(instance.vm->heap.isDeferred());
    RELEASE_ASSERT(instance.options().useWideReferences() || MarkedBlockCage::isRequired());
    return new State(instance);
}

WTF_MAKE_TZONE_ALLOCATED_IMPL(RuntimeTable);

#define AOT_CHECK_SCHEMA_ENTRY(name) static_assert(entryOf(RuntimeOperation::name) == Entry::operationAOT##name);
#define AOT_COUNT_SCHEMA_ENTRY(name) + 1
FOR_EACH_AOT_RUNTIME_OPERATION(AOT_CHECK_SCHEMA_ENTRY)
static_assert(numberOfRuntimeOperations == 0 FOR_EACH_AOT_RUNTIME_OPERATION(AOT_COUNT_SCHEMA_ENTRY));
#undef AOT_COUNT_SCHEMA_ENTRY
#undef AOT_CHECK_SCHEMA_ENTRY

static void JIT_OPERATION_ATTRIBUTES operationNotUsedByImage()
{
    dataLogLn("AOT: compiled code called an operation that its image does not list as used.");
    RELEASE_ASSERT_NOT_REACHED();
}

template<RemovableFeature feature>
static void JIT_OPERATION_ATTRIBUTES operationOfRemovedFeature()
{
    Bun__abortForRemovedFeature(static_cast<uint16_t>(feature));
}

template<Entry entry, auto operation>
static ALWAYS_INLINE void* operationAddress()
{
    if constexpr (featureOf(entry).has_value()) {
        auto replacement = reinterpret_cast<decltype(operation)>(&operationOfRemovedFeature<*featureOf(entry)>);
        return tagCFunctionPtr<void*, OperationPtrTag>(addressUnlessRemoved<*featureOf(entry), operation>(replacement));
    } else
        return tagCFunctionPtr<void*, OperationPtrTag>(operation);
}

static ALWAYS_INLINE VM& vmFor(VM* vm) { return *vm; }
static ALWAYS_INLINE VM& vmFor(Instance* instance) { return *instance->vm; }
static ALWAYS_INLINE VM& vmFor(JSGlobalObject* globalObject) { return globalObject->vm(); }
static ALWAYS_INLINE VM& vmFor(const JSCell* cell) { return cell->vm(); }

template<typename First>
concept CanFindVM = requires(First first) { vmFor(first); };

// No primary definition: an operation whose calling convention matches no specialization fails to compile instead of going uncounted.
template<Entry entry, auto operation, typename Function>
struct CountedCall;

template<Entry entry, auto operation, typename Result, typename... Parameters, bool isNoexcept>
struct CountedCall<entry, operation, Result JIT_OPERATION_ATTRIBUTES (Parameters...) noexcept(isNoexcept)> {
    static constexpr bool isCounted = false;
    static constexpr auto call = operation;
};

#if AOT_LEAVES_PRESERVE_MOST
template<Entry entry, auto operation, typename Result, typename... Parameters, bool isNoexcept>
struct CountedCall<entry, operation, Result AOT_PRESERVE_MOST_LEAF_ATTRIBUTES (Parameters...) noexcept(isNoexcept)> {
    static constexpr bool isCounted = false;
    static constexpr auto call = operation;
};
#endif

template<Entry entry, auto operation, typename Result, typename First, typename... Rest, bool isNoexcept>
    requires CanFindVM<First>
struct CountedCall<entry, operation, Result JIT_OPERATION_ATTRIBUTES (First, Rest...) noexcept(isNoexcept)> {
    static constexpr bool isCounted = true;
    static Result JIT_OPERATION_ATTRIBUTES call(First first, Rest... rest) noexcept(isNoexcept)
    {
        vmFor(first).aotState().runtimeTable->didCall(entry);
        if constexpr (std::is_same_v<First, Instance*>) {
            if (ArrivalCounts* arrivals = first->state ? first->state->arrivalCounts.get() : nullptr)
                arrivals->didCallOperation(entry);
        }
        AOT_MUST_TAIL_CALL return operation(first, rest...);
    }
};

// A leaf that preserves most registers is handed characters and numbers and no instance.
static constexpr bool isCalledWithoutCollection(Entry entry)
{
    std::optional<RuntimeOperation> operation = runtimeOperationOf(entry);
    return operation && !Typed::declaredBoundOf(*operation).mayCollect && !preservesMostRegisters(*operation) && !featureOf(entry);
}

// What takes the pending exception runs the collection that throwing put off.
static_assert(!isCalledWithoutCollection(Entry::operationAOTCatch) && !isCalledWithoutCollection(Entry::operationAOTCatchException) && !isCalledWithoutCollection(Entry::operationAOTCatchValueAndException));

template<Entry entry, auto operation>
static constexpr auto functionInTable = [] {
    if constexpr (isCalledWithoutCollection(entry))
        return CallWithoutCollection<operation>::call;
    else
        return operation;
}();

template<Entry entry, auto operation>
using CountedCallInTable = CountedCall<entry, functionInTable<entry, operation>, std::remove_pointer_t<decltype(operation)>>;

static constexpr bool isHandedWhatLeadsToVM(Entry entry)
{
    if (std::optional<RuntimeOperation> operation = runtimeOperationOf(entry))
        return !isLeaf(*operation);
    return entry != Entry::operationAOTReportBrokenClaim;
}

template<bool countsCalls>
static void fillOperations(void** entries)
{
#define AOT_FILL_OPERATION(name) \
    static_assert(CountedCallInTable<Entry::name, &name>::isCounted || !isHandedWhatLeadsToVM(Entry::name)); \
    entries[static_cast<unsigned>(Entry::name)] = operationAddress<Entry::name, countsCalls ? CountedCallInTable<Entry::name, &name>::call : functionInTable<Entry::name, &name>>();
#define AOT_FILL_SCHEMA_OPERATION(name) AOT_FILL_OPERATION(operationAOT##name)
#define AOT_TRAP_ENTRY(name) \
    entries[static_cast<unsigned>(Entry::name)] = tagCFunctionPtr<void*, OperationPtrTag>(operationNotUsedByImage);
    FOR_EACH_AOT_RUNTIME_OPERATION(AOT_FILL_SCHEMA_OPERATION)
    FOR_EACH_AOT_OPERATION_OUTSIDE_SCHEMA(AOT_FILL_OPERATION, AOT_TRAP_ENTRY)
#undef AOT_TRAP_ENTRY
#undef AOT_FILL_SCHEMA_OPERATION
#undef AOT_FILL_OPERATION
}

static void fillThunkAndPointers(void** entries, const Image& image)
{
    auto set = [&](Entry entry, void* pointer) {
        entries[static_cast<unsigned>(entry)] = pointer;
    };
    const void* handleExceptionStub = image.stubAddress(handleException(false));
    RELEASE_ASSERT(handleExceptionStub);
    set(Entry::HandleException, tagCodePtr<JITThunkPtrTag>(const_cast<void*>(handleExceptionStub)));
    set(Entry::VirtualCallTrampoline, LLInt::getCodePtr<JSEntryPtrTag>(llint_virtual_call_trampoline).taggedPtr());
    set(Entry::NonConstructorHostFunction, std::bit_cast<void*>(&callHostFunctionAsConstructor));
    set(Entry::CallNativeFunctionUnderValidation, std::bit_cast<void*>(&callNativeFunctionUnderValidation));
    set(Entry::ConstructNativeFunctionUnderValidation, std::bit_cast<void*>(&constructNativeFunctionUnderValidation));
    set(Entry::CheckThatThrowingCallHasSite, std::bit_cast<void*>(&checkThatThrowingCallHasSite));
    set(Entry::ErrorConstructorClassInfo, const_cast<ClassInfo*>(ErrorConstructor::info()));
    set(Entry::GeneratorClassInfo, const_cast<ClassInfo*>(Generator::info()));
    set(Entry::AsyncGeneratorClassInfo, const_cast<ClassInfo*>(JSAsyncGenerator::info()));
#if ENABLE(WEBASSEMBLY)
    set(Entry::WebAssemblyFunctionClassInfo, const_cast<ClassInfo*>(WebAssemblyFunction::info()));
#endif
}

static void installOperationFrontEnds(void** entries, const Image& image)
{
    for (const OperationFrontEnd& row : operationFrontEnds()) {
        entries[static_cast<unsigned>(row.raw)] = entries[static_cast<unsigned>(row.operation)];
        if (const void* frontEnd = image.stubAddress(row.stub))
            entries[static_cast<unsigned>(row.operation)] = tagCodePtr<OperationPtrTag>(const_cast<void*>(frontEnd));
    }
}

static constexpr auto entriesThatAreCalled = [] {
    std::array<uint32_t, std::extent_v<decltype(ImageHeader::entriesUsed)>> bits { };
    for (unsigned index = 0; index < numberOfEntries; ++index) {
        Entry entry = static_cast<Entry>(index);
        if (isOperation(entry) || isRawOperation(entry))
            bits[index / 32] |= 1u << index % 32;
    }
    return bits;
}();

static void trapUnusedEntries(void** entries, const Image& image)
{
    void* trap = tagCFunctionPtr<void*, OperationPtrTag>(operationNotUsedByImage);
    const uint32_t* entriesUsed = image.header().entriesUsed;
    for (unsigned word = 0; word < entriesThatAreCalled.size(); ++word) {
        for (uint32_t unused = entriesThatAreCalled[word] & ~entriesUsed[word]; unused; unused &= unused - 1)
            entries[word * 32 + std::countr_zero(unused)] = trap;
    }
}

#if defined(BUN_HARDENED)
static size_t entryPagesSize()
{
    return roundUpToMultipleOf(pageSize(), numberOfEntries * sizeof(void*));
}
#endif

RuntimeTable::RuntimeTable(VM& vm)
#if defined(BUN_HARDENED)
    : m_entries(static_cast<void**>(reserveGuardedPages(entryPagesSize(), OSAllocator::UnknownUsage)))
    , m_countsCalls(!!Options::aotCountsPath())
#else
    : m_countsCalls(!!Options::aotCountsPath())
#endif
{
    const Image& image = *vm.aotState().image;
    (m_countsCalls ? fillOperations<true> : fillOperations<false>)(m_entries);
    if (image.sharesRealms()) {
        constexpr Entry entry = Entry::operationAOTLookupExceptionHandler;
        m_entries[static_cast<unsigned>(entry)] = m_countsCalls ? operationAddress<entry, CountedCallInTable<entry, &operationAOTLookupExceptionHandlerAmongImages>::call>() : operationAddress<entry, functionInTable<entry, &operationAOTLookupExceptionHandlerAmongImages>>();
    }
    fillThunkAndPointers(m_entries, image);
    installOperationFrontEnds(m_entries, image);
    trapUnusedEntries(m_entries, image);

    m_compactTableStructure = CompactTable::createStructure(vm);
    vm.aotState().caches->addStrongRoots("compact table structure"_s, { reinterpret_cast<JSCell* const*>(&m_compactTableStructure), 1 });
}

RuntimeTable::~RuntimeTable()
{
#if defined(BUN_HARDENED)
    releaseGuardedPages(m_entries, entryPagesSize());
#endif
}

void RuntimeTable::finishInitialization(VM& vm)
{
    void*& entry = m_entries[static_cast<unsigned>(Entry::CallLinkInfoForCall)];
    if (entry)
        return;
    m_callLinkInfoForCall.initialize(vm, nullptr, CallLinkInfo::Call, CodeOrigin { });
    m_callLinkInfoForCall.setVirtualCall(vm);
    entry = static_cast<CallLinkInfo*>(&m_callLinkInfoForCall);
#if defined(BUN_HARDENED)
    OSAllocator::protect(m_entries, entryPagesSize(), true, false);
#endif
}

ASCIILiteral nameOf(Entry entry)
{
    switch (entry) {
#define AOT_NAME_OF_ENTRY(name) case Entry::name: return #name ""_s;
#define AOT_NAME_OF_SCHEMA_ENTRY(name) case Entry::operationAOT##name: return "operationAOT" #name ""_s;
    FOR_EACH_AOT_RUNTIME_OPERATION(AOT_NAME_OF_SCHEMA_ENTRY)
    FOR_EACH_AOT_OPERATION_OUTSIDE_SCHEMA(AOT_NAME_OF_ENTRY, AOT_NAME_OF_ENTRY)
    FOR_EACH_AOT_THUNK(AOT_NAME_OF_ENTRY)
    FOR_EACH_AOT_POINTER(AOT_NAME_OF_ENTRY)
#undef AOT_NAME_OF_SCHEMA_ENTRY
#undef AOT_NAME_OF_ENTRY
#define AOT_STUB_FRONT_END(Operation, Family) case Entry::Raw##Operation: return "Raw" #Operation ""_s;
#include "AOTStubFamilies.def"
#undef AOT_STUB_FRONT_END
    case Entry::NumberOfEntries:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

uint64_t operationCallCount(VM& vm, Entry entry)
{
    const RuntimeTable* runtimeTable = vm.aotState().runtimeTable.get();
    return runtimeTable ? runtimeTable->callCount(entry) : 0;
}

const Identifier& RuntimeTable::nameOfApplyTrap(VM& vm)
{
    if (m_nameOfApplyTrap.isNull()) [[unlikely]]
        m_nameOfApplyTrap = Identifier::fromString(vm, "apply"_s);
    return m_nameOfApplyTrap;
}

size_t StructuresWithFastIndexing::removeDeadEntries(VM& vm, CollectionScope)
{
    return m_structures.removeAllMatching([&](Structure* structure) {
        return !vm.heap.isMarked(structure);
    });
}

static size_t sizeOfPagesAtStart(const Image&);

VMState::VMState() = default;
VMState::PartsOfImage::PartsOfImage() = default;
VMState::PartsOfImage::PartsOfImage(PartsOfImage&&) = default;
VMState::PartsOfImage::~PartsOfImage() = default;

// Only a VM that runs several images writes it. Not a thread_local: the block of those has a bound, and an executable would pay for the word.
static VMState*& stateOfVMOnThisThread()
{
    static NeverDestroyed<ThreadSpecific<VMState*, CanBeGCThread::True>> state;
    return *state.get();
}

void runImageOnThisThread(const Image& image)
{
    if (VMState* state = image.sharesRealms() ? stateOfVMOnThisThread() : nullptr; state && state->hasSeveralImages)
        state->run(image);
    else
        Image::setCurrentOfThread(&image);
}

HeapHooks::WorkForStoppedVMScope::WorkForStoppedVMScope(VM& vm)
    : m_hasSeveralImages(vm.aotState().hasSeveralImages)
{
    if (!m_hasSeveralImages) [[likely]]
        return;
    m_stateOfThread = std::exchange(stateOfVMOnThisThread(), &vm.aotState());
    m_imageOfThread = Image::current();
    Image::setCurrentOfThread(vm.aotState().image);
}

HeapHooks::WorkForStoppedVMScope::~WorkForStoppedVMScope()
{
    if (!m_hasSeveralImages) [[likely]]
        return;
    stateOfVMOnThisThread() = m_stateOfThread;
    Image::setCurrentOfThread(m_imageOfThread);
}

VMState::PartsOfImage& VMState::partsOfRunningImage()
{
    return parts[image->position()];
}

void VMState::run(const Image& next)
{
    PartsOfImage& partsOfNext = parts[next.position()];
    RELEASE_ASSERT(partsOfNext.image == &next);
    partsOfImageThatRuns = &partsOfNext;
    image = &next;
    runtimeTable = partsOfNext.runtimeTable.get();
    propertyCaches = partsOfNext.propertyCaches.get();
    callSiteCache = partsOfNext.callSiteCache.get();
    functionTables = partsOfNext.functionTables.get();
    regExpLiterals = partsOfNext.regExpLiterals.get();
    Image::setCurrentOfThread(&next);
}

void VMState::makeRoomForImagesBelowAdapters()
{
    size_t size = endOfImagesBelowAdapters - imagesBelowAdapters.get();
    size_t room = std::max<size_t>(64, 2 * size);
    auto larger = makeUniqueArray<ImageBelowAdapter>(room);
    if (size)
        std::copy_n(imagesBelowAdapters.get(), size, larger.get());
    else {
        larger[size++] = { aboveEveryAdapterFrame(), nullptr };
        adapterFrameThatRestoresImage = aboveEveryAdapterFrame();
    }
    imagesBelowAdapters = WTF::move(larger);
    endOfImagesBelowAdapters = imagesBelowAdapters.get() + size;
    endOfRoomForImagesBelowAdapters = imagesBelowAdapters.get() + room;
}

// emitEnterAdapter() does the same where there is room and every frame that is noted is still there.
void VMState::runImageOfAdapter(const Image& next, const void* adapterFrame)
{
    // The stack grows down: a frame that is still there lies above the new one. A frame that was left by no way that restores (none is known) would leave the image of it
    // running in its caller for good: what ran below the outermost of such frames runs now.
    const PartsOfImage* below = partsOfImageThatRuns;
    while (endOfImagesBelowAdapters[-1].adapterFrame <= adapterFrame) [[unlikely]] {
        ASSERT_NOT_REACHED();
        below = (--endOfImagesBelowAdapters)->parts;
    }
    if (endOfImagesBelowAdapters == endOfRoomForImagesBelowAdapters)
        makeRoomForImagesBelowAdapters();
    *endOfImagesBelowAdapters++ = { adapterFrame, below };
    adapterFrameThatRestoresImage = adapterFrame;
    run(next);
}

// As emitAdapterEpilogue() does.
void VMState::restoreImageBelowAdapter()
{
    run(*(--endOfImagesBelowAdapters)->parts->image);
    adapterFrameThatRestoresImage = endOfImagesBelowAdapters[-1].adapterFrame;
}

void VMState::restoreImagesBelowAdaptersBelow(const void* frame)
{
    // No adapter has changed the image yet.
    if (!endOfImagesBelowAdapters)
        return;
    while (endOfImagesBelowAdapters[-1].adapterFrame < frame) {
        if (ASSERT_ENABLED) {
            dataLogLn("AOT: a throw passed the adapter's frame ", RawPointer(endOfImagesBelowAdapters[-1].adapterFrame), " without restoring the image below it; it lands at ", RawPointer(frame));
            WTFReportBacktrace();
        }
        restoreImageBelowAdapter();
    }
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTRunImageOfAdapter, void, (void* instance, void* adapterFrame))
{
    auto* entered = static_cast<Instance*>(instance);
    entered->vm->aotState().runImageOfAdapter(*entered->program, adapterFrame);
}

VMState::~VMState()
{
    RELEASE_ASSERT(!hasCompiledCode);
}

void VMState::attachImage(VM& vm)
{
    RELEASE_ASSERT(!hasCompiledCode && !image);
    image = Image::current();
    RELEASE_ASSERT(image && image == &image->first());
    const Image& first = *image;
    size_t sizeOfPages = 0;
    size_t sizeOfRegExpMatchOutput = 2 * (maxGroupsInRegExpMatchOutput + 1);
    for (const Image* each = image; each; each = each->above()) {
        parts.append(PartsOfImage());
        parts.last().image = each;
        sizeOfPages += sizeOfPagesAtStart(*each);
        sizeOfRegExpMatchOutput = std::max<size_t>(sizeOfRegExpMatchOutput, each->header().counts.maxRegExpOutputSize);
    }
    hasSeveralImages = parts.size() > 1;
    if (hasSeveralImages) {
        stateOfVMOnThisThread() = this;
        currentImageOfThread = Image::addressOfCurrentOfThread();
        makeRoomForImagesBelowAdapters();
    }
    RELEASE_ASSERT(vm.structureStructure && !vm.structureRareDataStructure);

    structureIDBase = JSC::structureIDBase();
    validatesRuntime = image->header().options.validates || Options::validateAOTRuntime();
    if (image->header().options.has(TestingSwitch::CheckProofsAtRunTime)) [[unlikely]]
        imageChecksProofs.store(true, std::memory_order_relaxed);
    vm.updateSoftReservedZoneSize(Options::softReservedZoneSize() + BuiltinCells::stackKeptFromProgram);

    pages = makeUnique<PagesOfVM>(sizeOfPages);
    caches = makeUnique<CacheRegistry>();
    caches->add(CacheKind::WeakByKey, "structures with fast indexing"_s, structuresWithFastIndexing);
    counters = makeUniqueWithoutFastMallocCheck<RuntimeCounters>();
    hasCompiledCode = true;
    vm.setMayHaveUncacheableTransitions();

    vm.heap.setValidatesCellVisits(validatesRuntime || Options::verifyGC());

    reporter = makeUniqueWithoutFastMallocCheck<RuntimeReporter>(vm);
    identifiers = makeUnique<ProgramIdentifiers>(vm, first);
    for (PartsOfImage& partsOfImage : parts) {
        run(*partsOfImage.image);
        partsOfImage.runtimeTable = makeUnique<RuntimeTable>(vm);
        partsOfImage.callSiteCache = makeUnique<CallSiteCache>();
        run(*partsOfImage.image);
        partsOfImage.propertyCaches = makeUnique<VMPropertyCaches>(vm);
        partsOfImage.regExpLiterals = makeUnique<RegExpLiterals>(*caches);
    }
    run(first);
    regExpMatchOutput = makeUniqueArray<int>(sizeOfRegExpMatchOutput);

    moduleRecordStructure.setWithoutWriteBarrier(ModuleRecord::createStructure(vm, nullptr, jsNull()));
    stringBuilderStorageStructure.setWithoutWriteBarrier(StringBuilderStorage::createStructure(vm, nullptr, jsNull()));

    if (checksClaimsAboutObjects(vm))
        caches->addObserver("validation after collection"_s, collectionValidator);
}

// The rows of Structures are words of one table of the process, so they are given back while the Structure IDs still belong to this VM.
void VMState::willShutDownHeap()
{
    if (propertyCaches)
        propertyCaches->front().clear();
    if (AllocationCounts::isEnabled()) [[unlikely]]
        AllocationCounts::willShutDownHeap(*this);
}

void VMState::releaseImage(VM& vm)
{
    if (!hasCompiledCode)
        return;
    ASSERT(vm.heap.isShuttingDown());

    reporter->writeAtExit();
    forgetErrorOfCodeLeftOut(vm);
    pages->willReleaseEverything();
    Instance::destroyAll(vm);
    RELEASE_ASSERT(instances.isEmpty() && instancesToDestroy.isEmpty());
    if (CheckedFrameCells::isInUse()) [[unlikely]]
        CheckedFrameCells::willDestroy(vm);

    regExpLiterals = nullptr;
    regExpMatchOutput = nullptr;
    propertyCaches = nullptr;
    functionTables = nullptr;
    callSiteCache = nullptr;
    runtimeTable = nullptr;
    for (PartsOfImage& partsOfImage : parts) {
        partsOfImage.regExpLiterals = nullptr;
        partsOfImage.propertyCaches = nullptr;
        partsOfImage.functionTables = nullptr;
        partsOfImage.callSiteCache = nullptr;
    }
    identifiers = nullptr;
    Image::setCurrentOfThread(&image->first());
    parts.clear();
    partsOfImageThatRuns = nullptr;
    if (hasSeveralImages && stateOfVMOnThisThread() == this)
        stateOfVMOnThisThread() = nullptr;
    hasSeveralImages = false;
    currentImageOfThread = nullptr;
    adapterFrameThatRestoresImage = nullptr;
    endOfImagesBelowAdapters = nullptr;
    endOfRoomForImagesBelowAdapters = nullptr;
    imagesBelowAdapters = nullptr;
    reporter = nullptr;
    counters = nullptr;
    caches = nullptr;
    pages = nullptr;

    moduleRecordStructure.clear();
    stringBuilderStorageStructure.clear();
    image = nullptr;
    structureIDBase = 0;
    validatesRuntime = false;
    hasCompiledCode = false;
}

#if ENABLE(AOT_GUARDED_PAGES)
static constexpr unsigned guardPagesOnEachEnd = 1;

// What a check of the file can tell such a build by.
extern "C" __attribute__((used, retain)) const char JSC__AOT__mapsGuardedPages[] = "guarded pages: what a compiled program maps for good has an address of its own";
#else
static constexpr unsigned guardPagesOnEachEnd = 0;
#endif

#if ENABLE(AOT_GUARDED_PAGES) && OS(LINUX) && CPU(ADDRESS64)
// What the system places by itself shares one random base: an address of one mapping gives away the others, the C library's among them.
static void* tryReserveGuardedPagesAtRandomAddress(size_t bytes, OSAllocator::Usage usage)
{
    static constexpr uintptr_t lowestRandomAddress = 64 * GB;
    static constexpr uintptr_t highestRandomAddress = 124 * 1024 * GB;
    AOT_THRESHOLD(unsigned, maxDraws, 16);
    if (bytes >= highestRandomAddress - lowestRandomAddress)
        return nullptr;
    if (WTF::isAddressRandomizationDisabled())
        return nullptr;
    uintptr_t numberOfSlots = (highestRandomAddress - lowestRandomAddress - bytes) / pageSize();
    for (unsigned draw = 0; draw < maxDraws; ++draw) {
        void* requestedAddress = std::bit_cast<void*>(lowestRandomAddress + cryptographicallyRandomNumber<uint64_t>() % numberOfSlots * pageSize());
        void* pages = OSAllocator::tryReserveAndCommit(bytes, usage, requestedAddress, true, false, false, guardPagesOnEachEnd);
        if (!pages)
            return nullptr;
        if (pages == requestedAddress)
            return pages;
        OSAllocator::releaseDecommitted(pages, bytes, guardPagesOnEachEnd);
    }
    return nullptr;
}
#endif

#if OS(LINUX)
static bool canMap(size_t bytes)
{
    void* pages = mmap(nullptr, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (pages == MAP_FAILED)
        return false;
    munmap(pages, bytes);
    return true;
}

AddressSpaceToStart askForAddressSpaceToStart(size_t sizeOfProgram, std::span<const size_t> stepsOfAllocator)
{
    AddressSpaceToStart result;
    result.left = addressSpaceLeft();
    if (result.left == std::numeric_limits<size_t>::max())
        return result;
    result.needed = addressSpaceToStart(sizeOfProgram);
    result.isRefused = !canMap(result.needed);
    if (result.isRefused)
        return result;
    // Beside a step of the allocator there is room for as much again: the cells, tables and stacks of threads.
    for (size_t step : stepsOfAllocator) {
        if (canMap(step + std::max(step, addressSpaceToStartBesideAllocations()))) {
            result.largestStepOfAllocator = step;
            return result;
        }
    }
    result.largestStepOfAllocator = 0;
    return result;
}
#else
AddressSpaceToStart askForAddressSpaceToStart(size_t, std::span<const size_t>) { return { }; }
#endif

// The memory that a VM's tables need before the program's first statement runs. Anything beyond that gets its own pages.
static size_t sizeOfPagesAtStart(const Image& image)
{
    AOT_THRESHOLD(size_t, sizeOfSmallTables, 512 * KB);
    auto [below, above] = reservationSizes(image);
    return roundUpToMultipleOf(pageSize(), VMPropertyCaches::maxSizeOfPagesAtStart(image) + below + above + arenaCapacity(image.programTables()) + sizeOfSmallTables);
}

#if OS(LINUX) && !ENABLE(AOT_GUARDED_PAGES)
static thread_local uint8_t* t_pagesOfThread;
static thread_local size_t t_sizeOfPagesOfThread;
static thread_local bool t_isStarting;
static std::atomic<unsigned> s_numberOfThreadsStarting;

// The allocator keeps pieces of different sizes in pages of different sizes, and a thread shares no page with another thread. So does a heap.
// Other threads allocate meanwhile, so it stops where less than this much address space is left, and it reads what is left: to map as much would take it from them.
static bool tryEnsureRoomInAllocator(unsigned numberOfThreads, size_t sizeToLeaveFree)
{
#if USE(MIMALLOC)
    struct Pages {
        size_t size;
        unsigned count;
    };
    // What a thread takes until the first statement of a small program runs. The largest come last: by then the small ones have divided what is free.
    AOT_THRESHOLD(Pages, pagesOfThread[], { { 64 * KB, 40 }, { 512 * KB, 8 }, { 4 * MB, 2 } });
    AOT_THRESHOLD(unsigned, piecesInPage, 7);
    AOT_THRESHOLD(size_t, alignmentOfPages, 64 * KB);
    // It asks the system for an arena whenever its arenas are full, and each time it is refused it goes on at addresses far from the last.
    AOT_THRESHOLD(size_t, smallestStep, 128 * MB);
    static const size_t largestStep = mi_option_get_size(mi_option_arena_reserve);
    size_t left = addressSpaceLeft();
    size_t step = largestStep + sizeToLeaveFree <= left ? largestStep : largestStep > smallestStep && smallestStep + sizeToLeaveFree <= left ? smallestStep : 0;
    mi_option_set(mi_option_arena_reserve, step / KB);

    size_t sizeOfPagesOfThread = 0;
    for (auto [size, count] : pagesOfThread)
        sizeOfPagesOfThread += count * (size + alignmentOfPages);
    if (numberOfThreads * sizeOfPagesOfThread + sizeToLeaveFree <= left)
        return true;

    mi_heap_t* heap = mi_heap_new();
    if (!heap)
        return false;
    bool hasRoom = true;
    for (unsigned thread = 0; thread < numberOfThreads; ++thread) {
        for (auto [size, count] : pagesOfThread) {
            for (unsigned page = 0; hasRoom && page < count; ++page) {
                for (unsigned piece = 0; hasRoom && piece < piecesInPage; ++piece)
                    hasRoom = !!mi_heap_malloc(heap, size / (piecesInPage + 1));
                hasRoom = hasRoom && addressSpaceLeft() >= sizeToLeaveFree;
            }
        }
    }
    mi_heap_destroy(heap);
    return hasRoom;
#else
    UNUSED_PARAM(numberOfThreads);
    UNUSED_PARAM(sizeToLeaveFree);
    return true;
#endif
}

// Until a thread adopts them the pages begin with their size.
static void* tryReservePagesForTables(const Image& image, unsigned numberOfThreads, size_t sizeToLeaveFree)
{
    if (!MarkedBlockCage::tryEnsureRoomForNewVMs(numberOfThreads))
        return nullptr;
    // The thread may run a different image of the executable than the thread that started it.
    size_t size = sizeOfPagesAtStart(image);
    if (addressSpaceLeft() < size + sizeToLeaveFree)
        return nullptr;
    void* pages = OSAllocator::tryReserveAndCommit(size, OSAllocator::FastMallocPages);
    if (pages)
        *static_cast<size_t*>(pages) = size;
    return pages;
}

PagesForNewThread tryReservePagesForFirstThread()
{
    const Image* image = Image::current();
    if (!hasAddressSpaceLimit() || !image)
        return { };
    void* pages = tryReservePagesForTables(*image, ++s_numberOfThreadsStarting, 0);
    if (!pages)
        --s_numberOfThreadsStarting;
    return { pages, !pages };
}

PagesForNewThread tryReservePagesForNewThread(bool threadsAreEnding)
{
    // The stack of the thread, and what the threads that run take meanwhile.
    AOT_THRESHOLD(size_t, sizeToLeaveFreeForThread, 10 * MB);
    const Image* image = Image::current();
    if (!hasAddressSpaceLimit() || !image)
        return { };
    unsigned numberOfThreads = ++s_numberOfThreadsStarting;
    size_t sizeToLeaveFree = sizeToLeaveFreeForThread;
    // Once every thread that is starting has its stack, the range of the cells can still take more.
    if (threadsAreEnding)
        sizeToLeaveFree = numberOfThreads * sizeToLeaveFreeForThread + ReservedRangeHeap::addressSpaceToGrow();
    void* pages = tryReservePagesForTables(*image, numberOfThreads, sizeToLeaveFree);
    if (!pages) {
        --s_numberOfThreadsStarting;
        return { nullptr, true };
    }
    if (!tryEnsureRoomInAllocator(numberOfThreads, sizeToLeaveFree)) {
        releasePagesForNewThread(pages);
        return { nullptr, true };
    }
    return { pages, false };
}

void releasePagesForNewThread(void* pages)
{
    if (!pages)
        return;
    OSAllocator::releaseDecommitted(pages, *static_cast<size_t*>(pages), 0);
    --s_numberOfThreadsStarting;
}

void adoptPagesForNewThread(void* pages)
{
    RELEASE_ASSERT(!t_pagesOfThread);
    t_pagesOfThread = static_cast<uint8_t*>(pages);
    t_sizeOfPagesOfThread = pages ? std::exchange(*static_cast<size_t*>(pages), 0) : 0;
    t_isStarting = !!pages;
}

void releaseUnusedPagesOfThread()
{
    if (t_sizeOfPagesOfThread)
        OSAllocator::releaseDecommitted(t_pagesOfThread, t_sizeOfPagesOfThread, 0);
    t_pagesOfThread = nullptr;
    t_sizeOfPagesOfThread = 0;
}

void didStartThread(VM* vm)
{
    if (!std::exchange(t_isStarting, false)) [[likely]]
        return;
    if (vm)
        vm->heap.cage().willRunProgram();
    --s_numberOfThreadsStarting;
}

// What the starting thread reserved for this thread.
static std::pair<uint8_t*, size_t> takePagesOfThread()
{
    return { std::exchange(t_pagesOfThread, nullptr), std::exchange(t_sizeOfPagesOfThread, 0) };
}
#else
PagesForNewThread tryReservePagesForFirstThread() { return { }; }
PagesForNewThread tryReservePagesForNewThread(bool) { return { }; }
void releasePagesForNewThread(void*) { }
void adoptPagesForNewThread(void*) { }
void releaseUnusedPagesOfThread() { }
void didStartThread(VM*) { }
#if !ENABLE(AOT_GUARDED_PAGES)
static std::pair<uint8_t*, size_t> takePagesOfThread() { return { }; }
#endif
#endif

void* tryReserveGuardedPages(size_t bytes, OSAllocator::Usage usage)
{
#if ENABLE(AOT_GUARDED_PAGES) && OS(LINUX) && CPU(ADDRESS64)
    if (void* pages = tryReserveGuardedPagesAtRandomAddress(bytes, usage))
        return pages;
#endif
    return OSAllocator::tryReserveAndCommit(bytes, usage, nullptr, true, false, false, guardPagesOnEachEnd);
}

void* reserveGuardedPages(size_t bytes, OSAllocator::Usage usage)
{
    void* pages = tryReserveGuardedPages(bytes, usage);
    RELEASE_ASSERT(pages, bytes);
    return pages;
}

void releaseGuardedPages(void* pages, size_t bytes)
{
    OSAllocator::releaseDecommitted(pages, bytes, guardPagesOnEachEnd);
}

static void keepSmallPages(void* pages, size_t bytes)
{
#if OS(LINUX)
    while (madvise(pages, bytes, MADV_NOHUGEPAGE) == -1 && errno == EAGAIN) { }
#else
    UNUSED_PARAM(pages);
    UNUSED_PARAM(bytes);
#endif
}

WTF_MAKE_TZONE_ALLOCATED_IMPL(PagesOfVM);

PagesOfVM::PagesOfVM(size_t sizeOfMapping)
{
#if !ENABLE(AOT_GUARDED_PAGES)
    auto [pagesOfThread, sizeOfPagesOfThread] = takePagesOfThread();
    m_start = pagesOfThread;
    m_size = sizeOfPagesOfThread;
    if (!m_start) {
        // If there is no room for all at once, each table requests its own.
        m_start = static_cast<uint8_t*>(OSAllocator::tryReserveAndCommit(sizeOfMapping, OSAllocator::FastMallocPages));
        m_size = m_start ? sizeOfMapping : 0;
    }
    if (m_start)
        keepSmallPages(m_start, m_size);
#else
    UNUSED_PARAM(sizeOfMapping);
#endif
}

PagesOfVM::~PagesOfVM()
{
    if (!m_start)
        return;
    // Something else may be mapped by now where a piece was released early.
    std::ranges::sort(m_gaps);
    uint8_t* start = m_start;
    for (auto [gap, size] : m_gaps) {
        if (gap > start)
            OSAllocator::releaseDecommitted(start, gap - start, 0);
        start = std::max(start, gap + size);
    }
    if (m_start + m_size > start)
        OSAllocator::releaseDecommitted(start, m_start + m_size - start, 0);
}

void* PagesOfVM::tryAllocate(size_t bytes)
{
    bytes = roundUpToMultipleOf(pageSize(), bytes);
    if (bytes <= m_size - m_used)
        return m_start + std::exchange(m_used, m_used + bytes);
    void* pages = tryReserveGuardedPages(bytes);
    if (pages)
        keepSmallPages(pages, bytes);
    return pages;
}

void* PagesOfVM::allocate(size_t bytes)
{
    bytes = roundUpToMultipleOf(pageSize(), bytes);
    if (bytes <= m_size - m_used)
        return m_start + std::exchange(m_used, m_used + bytes);
    void* pages = reserveGuardedPages(bytes);
    keepSmallPages(pages, bytes);
    return pages;
}

void PagesOfVM::release(void* pages, size_t bytes)
{
    if (!contains(pages)) {
        releaseGuardedPages(pages, bytes);
        return;
    }
    if (m_releasesEverything && m_gaps.isEmpty())
        return;
    bytes = roundUpToMultipleOf(pageSize(), bytes);
#if OS(WINDOWS)
    // Only a whole reservation can be released.
    OSAllocator::decommit(pages, bytes);
#else
    OSAllocator::releaseDecommitted(pages, bytes, 0);
    didMoveAway(pages, bytes);
#endif
}

void PagesOfVM::didMoveAway(void* pages, size_t bytes)
{
    if (contains(pages))
        m_gaps.append({ static_cast<uint8_t*>(pages), roundUpToMultipleOf(pageSize(), bytes) });
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

WTF_MAKE_TZONE_ALLOCATED_IMPL(ProgramStatics);

static uint32_t sizeFilledAtProgramEntry(const ProgramTables& tables)
{
    std::span<const StaticsLayout> layout = tables.staticsLayout();
    return layout.empty() ? 0 : layout[0].sizeFilledAtProgramEntry;
}

static std::optional<FunctionID> findFunctionVariable(const ProgramTables& tables, uint32_t offset)
{
    std::span<const FunctionVariableData> variables = tables.functionVariables();
    auto found = std::ranges::lower_bound(variables, offset, { }, &FunctionVariableData::offset);
    if (found == variables.end() || found->offset != offset)
        return std::nullopt;
    return FunctionID(found->function);
}

// The literal string in the slot, if reading the slot would create its cell.
static std::optional<uint32_t> literalStringWithoutCell(const Instance& instance, SlotRepresentation representation, const void* slot)
{
    if (traitsOf(representation).valueKind != SlotValueKind::LiteralString)
        return std::nullopt;
    uint64_t word = readSlotBits(representation, slot);
    if (!isLiteralStringWord(word))
        return std::nullopt;
    uint32_t number = literalStringIndex(static_cast<uint32_t>(word));
    RELEASE_ASSERT(number < instance.tables().literalStrings().size());
    const ConstantCellWords& constants = instance.runtime.constants;
    bool hasCell = instance.options().useWideReferences() ? !!constants.wideLiteralStrings[number] : !!constants.literalStrings[number];
    return hasCell ? std::nullopt : std::optional(number);
}

void ProgramStatics::initializeStatics(uint32_t firstWord, uint32_t endWord)
{
    auto* words = reinterpret_cast<uint64_t*>(m_instance.statics());
    std::span<const StaticsRun> runs = m_tables.staticsRuns();
    auto run = std::ranges::upper_bound(runs, firstWord, { }, &StaticsRun::firstWord);
    if (run != runs.begin())
        --run;
    for (; run != runs.end() && run->firstWord < endWord; ++run) {
        uint32_t first = std::max(run->firstWord, firstWord);
        uint32_t end = std::min(run->firstWord + run->numberOfWords, endWord);
        if (first < end)
            std::ranges::fill(std::span { words + first, end - first }, run->bits());
    }
}

uint32_t ProgramStatics::groupWithWord(uint32_t word) const
{
    std::span<const StaticsOfFiles> groups = m_tables.staticsOfFiles();
    auto next = std::ranges::upper_bound(groups, word, { }, &StaticsOfFiles::firstWord);
    RELEASE_ASSERT(next != groups.begin());
    return static_cast<uint32_t>(next - groups.begin()) - 1;
}

bool ProgramStatics::isInitialized(uint32_t offset) const
{
    return (m_programStaticsInitialized || offset >= sizeFilledAtProgramEntry(m_tables)) && isFilled(offset);
}

bool ProgramStatics::isFilled(uint32_t offset) const
{
    uint32_t word = offset / sizeof(uint64_t);
    return word - m_firstWordFilledWithFiles >= m_numberOfWordsFilledWithFiles || m_filledFiles[groupWithWord(word)];
}

void ProgramStatics::fillVariablesOfFiles(uint32_t first)
{
    std::span<const StaticsOfFiles> groups = m_tables.staticsOfFiles();
    std::span<const uint32_t> importedGroups = m_tables.staticsOfImportedFiles();
    RELEASE_ASSERT(first + 1 < groups.size());
    if (m_filledFiles[first])
        return;
    m_filledFiles[first] = true;
    Vector<uint32_t, 32> pending { first };
    while (!pending.isEmpty()) {
        uint32_t group = pending.takeLast();
        initializeStatics(groups[group].firstWord, groups[group + 1].firstWord);
        for (uint32_t imported : importedGroups.subspan(groups[group].firstImportedGroup, groups[group + 1].firstImportedGroup - groups[group].firstImportedGroup)) {
            if (!std::exchange(m_filledFiles[imported], true))
                pending.append(imported);
        }
    }
}

void ProgramStatics::fillVariablesOfAllFiles()
{
    std::span<const StaticsOfFiles> groups = m_tables.staticsOfFiles();
    for (uint32_t group = 0; group + 1 < groups.size(); ++group) {
        if (!std::exchange(m_filledFiles[group], true))
            initializeStatics(groups[group].firstWord, groups[group + 1].firstWord);
    }
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTFillVariablesOfFiles, void, (Instance* instance, uint32_t group))
{
    instance->state->statics->fillVariablesOfFiles(group);
}

void ProgramStatics::initializeProgramStatics()
{
    if (m_readOnlyStaticsSize)
        OSAllocator::protect(m_instance.statics(), m_readOnlyStaticsSize, true, true);
    initializeStatics(0, sizeFilledAtProgramEntry(m_tables) / sizeof(uint64_t));
    WTF::storeStoreFence();
    m_readOnlyStaticsSize = 0;
    m_programStaticsInitialized = true;
}

JSValue ProgramStatics::readVariable(Instance& instance, SlotData slot)
{
    VM& vm = *instance.vm;
    State& state = *instance.state;
    ClosureTable& closures = state.objects->closures;
    uint32_t offset = slot.offset;
    if (slot.representation() == SlotRepresentation::DeferredClosure)
        return closures.closure(vm, FunctionID(offset));
    ProgramStatics& statics = *state.statics;
    statics.ensureProgramStaticsInitialized();
    statics.ensureVariableIsFilled(offset);
    if (JSValue value = readSlot(slot.representation(), instance.statics() + offset, statics.m_strings))
        return value;
    if (std::optional<FunctionID> function = findFunctionVariable(statics.m_tables, offset)) {
        // Every reader takes an empty word for the one closure of the function, so the word may as well hold it.
        // A stub that answers for the variable reads the word alone: code of another image of the realm comes by one for each function that it imports.
        JSCell* closure = closures.closure(vm, *function);
        writeVariable(instance, slot, closure);
        return closure;
    }
    return { };
}

JSValue ProgramStatics::readVariableWithoutAllocating(const Instance& instance, SlotData slot)
{
    const State& state = *instance.state;
    const ClosureTable& closures = state.objects->closures;
    uint32_t offset = slot.offset;
    if (slot.representation() == SlotRepresentation::DeferredClosure)
        return closures.closureIfCreated(FunctionID(offset));
    const ProgramStatics& statics = *state.statics;
    if (!statics.isInitialized(offset))
        return { };
    const uint8_t* address = instance.statics() + offset;
    if (literalStringWithoutCell(instance, slot.representation(), address))
        return { };
    if (JSValue value = readSlot(slot.representation(), address, statics.m_strings))
        return value;
    if (std::optional<FunctionID> function = findFunctionVariable(statics.m_tables, offset))
        return closures.closureIfCreated(*function);
    return { };
}

String ProgramStatics::readStringWithoutAllocating(const Instance& instance, SlotData slot)
{
    if (JSValue value = readVariableWithoutAllocating(instance, slot))
        return value.isString() ? asString(value)->tryGetValueWithoutGC() : String();
    const ProgramStatics& statics = *instance.state->statics;
    if (slot.representation() == SlotRepresentation::DeferredClosure || !statics.isInitialized(slot.offset))
        return String();
    std::optional<uint32_t> literal = literalStringWithoutCell(instance, slot.representation(), instance.statics() + slot.offset);
    return literal ? ProgramStrings::text(statics.m_tables, StringID(statics.m_tables.literalStrings()[*literal])) : String();
}

void ProgramStatics::writeVariable(Instance& instance, SlotData slot, JSValue value)
{
    SlotRepresentation representation = slot.representation();
    RELEASE_ASSERT(representation != SlotRepresentation::DeferredClosure);
    ProgramStatics& statics = *instance.state->statics;
    statics.ensureProgramStaticsInitialized();
    statics.ensureVariableIsFilled(slot.offset);
    uint8_t* address = instance.statics() + slot.offset;
    if (!value) {
        RELEASE_ASSERT(traitsOf(representation).hasAbsent);
        clearSlot(representation, address);
        return;
    }
    bool didWrite = tryWriteSlot(statics.m_strings, representation, address, value);
    RELEASE_ASSERT(didWrite);
    if (traitsOf(representation).holdsCell)
        statics.didStoreRootInStatics(slot.offset);
}

template<typename Word, typename Functor>
static ALWAYS_INLINE void forEachRootInRegion(const uint8_t* statics, std::span<const StaticsRange> roots, size_t begin, size_t end, const Functor& functor)
{
    auto first = std::ranges::partition_point(roots, [&](const StaticsRange& range) {
        return range.end() <= begin;
    });
    for (const StaticsRange& range : std::span { first, roots.end() }) {
        if (range.offset >= end)
            break;
        size_t endInRange = std::min<size_t>(range.end(), end);
        for (size_t offset = std::max<size_t>(range.offset, begin); offset < endInRange; offset += sizeof(Word))
            functor(*reinterpret_cast<const Word*>(statics + offset));
    }
}

template<typename Appender>
ALWAYS_INLINE void ProgramStatics::visitRegion(Appender& appender, size_t begin, size_t end)
{
    const uint8_t* statics = m_instance.statics();
    begin = std::max<size_t>(begin, m_readOnlyStaticsSize);
    forEachRootInRegion<EncodedJSValue>(statics, m_tables.staticsRoots(), begin, end, [&](EncodedJSValue value) ALWAYS_INLINE_LAMBDA {
        appender.appendUnbarriered(JSValue::decode(value));
    });
    // A marker or the word of a literal string lies below every reference.
    uintptr_t cageBase = m_instance.cellCageBase;
    forEachRootInRegion<uint32_t>(statics, m_tables.staticsNarrowRoots(), begin, end, [&](uint32_t word) ALWAYS_INLINE_LAMBDA {
        if (word >= minCell32)
            appender.appendUnbarriered(MarkedBlockCage::decode(cageBase, word));
    });
}

template<typename Visitor>
void ProgramStatics::visitRoots(Visitor& visitor)
{
    visitor.appendUnbarriered(JSValue::decode(m_instance.runtime.exceptions.thrownValue));
    bool isDeferred = m_slots.tryDeferFullVisit(visitor, [](SlotVisitor& visitor, void* owner, size_t begin, size_t end) {
        appendRoots(visitor, [&](auto& appender) ALWAYS_INLINE_LAMBDA {
            static_cast<ProgramStatics*>(owner)->visitRegion(appender, begin, end);
        });
    }, this);
    if (isDeferred)
        return;
    appendRoots(visitor, [&](auto& appender) ALWAYS_INLINE_LAMBDA {
        m_slots.forEachRegionToVisit(visitor, [&](size_t begin, size_t end) ALWAYS_INLINE_LAMBDA {
            visitRegion(appender, begin, end);
        });
    });
}

template void ProgramStatics::visitRoots(AbstractSlotVisitor&);
template void ProgramStatics::visitRoots(SlotVisitor&);

ProgramStatics::ProgramStatics(State& state)
    : m_instance(state.instance)
    , m_tables(state.tables)
    , m_strings(*state.strings)
    , m_slots({ state.instance.statics(), state.tables.instanceSizes.sizeOfStatics }, InstanceLayout::cardShift, { state.instance.cards(), RememberedSlotSet::numberOfCardsFor(state.tables.instanceSizes.sizeOfStatics, InstanceLayout::cardShift) })
{
    m_slots.setCardsAreWrittenWithoutRecord();
    uint8_t* statics = m_instance.statics();
    uint32_t filled = sizeFilledAtProgramEntry(m_tables);
    uint32_t endOfWordsFilledNow = std::numeric_limits<uint32_t>::max();
    if (std::span<const StaticsOfFiles> files = m_tables.staticsOfFiles(); !files.empty()) {
        // Nothing behind them starts as anything but zero.
        endOfWordsFilledNow = m_firstWordFilledWithFiles = files.front().firstWord;
        m_numberOfWordsFilledWithFiles = files.back().firstWord - files.front().firstWord;
        m_filledFiles = statics + files.back().firstWord * sizeof(uint64_t);
    }
    initializeStatics(filled / sizeof(uint64_t), endOfWordsFilledNow);
    // Compiled code may load from anywhere in the statics ahead of the branch that needs the value, so only stores are caught until the program is entered.
    m_readOnlyStaticsSize = static_cast<uint32_t>(WTF::roundDownToMultipleOf(pageSize(), static_cast<size_t>(filled)));
    if (m_readOnlyStaticsSize)
        OSAllocator::protect(statics, m_readOnlyStaticsSize, true, false);
    state.caches->add(CacheKind::StrongRoots, "statics"_s, *this, RootVolatility::GreyedByExecution);
    for (const StaticsRange& range : m_tables.staticsClearedEachCollection()) {
        RELEASE_ASSERT(range.offset >= filled);
        state.caches->addClearedEachCollection("statics cleared each collection"_s, { statics + range.offset, range.size });
    }
}

ProgramStatics::~ProgramStatics()
{
    if (m_readOnlyStaticsSize)
        OSAllocator::protect(m_instance.statics(), m_readOnlyStaticsSize, true, true);
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
