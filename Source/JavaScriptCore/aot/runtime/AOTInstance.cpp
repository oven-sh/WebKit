/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTInstance.h"

#if ENABLE(AOT)

#include "AOTAllocationCounts.h"
#include "AOTBuiltinCells.h"
#include "AOTBuiltinFamilyState.h"
#include "AOTBuiltinModules.h"
#include "AOTBuiltinTable.h"
#include "AOTBuiltinTableBindings.h"
#include "AOTCallSiteCache.h"
#include "AOTCellFormat.h"
#include "AOTChainDependents.h"
#include "AOTCompactTable.h"
#include "AOTCompileOptions.h"
#include "AOTCoroutineTable.h"
#include "AOTFunctionTables.h"
#include "AOTGenerator.h"
#include "AOTGlobalBindings.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTHeapHooks.h"
#include "AOTHost.h"
#include "AOTImage.h"
#include "AOTImageLoading.h"
#include "AOTLimits.h"
#include "AOTMembersLeftOut.h"
#include "AOTModuleHost.h"
#include "AOTModuleRecord.h"
#include "AOTModules.h"
#include "AOTNativeCallValidation.h"
#include "AOTObjectModel.h"
#include "AOTOperationData.h"
#include "AOTOperationScope.h"
#include "AOTProgramIdentifiers.h"
#include "AOTProgramStatics.h"
#include "AOTProgramStrings.h"
#include "AOTPropertyCaches.h"
#include "AOTRealmStartup.h"
#include "AOTRegExpRecords.h"
#include "AOTRuntimeCounters.h"
#include "AOTRuntimeFunctions.h"
#include "AOTRuntimeHooks.h"
#include "AOTRuntimeReporter.h"
#include "AOTRuntimeTable.h"
#include "AOTRuntimeValidation.h"
#include "AOTSlotAccess.h"
#include "AOTState.h"
#include "AOTStringBuilderStorage.h"
#include "AOTStubBlob.h"
#include "AOTStubEntrypoints.h"
#include "AOTStubFrontEnds.h"
#include "AOTStubsExceptions.h"
#include "AOTThreshold.h"
#include "AOTVMState.h"
#include "AbstractSlotVisitorInlines.h"
#include "AddressSpaceBudget.h"
#include "DeferGC.h"
#include "ErrorConstructor.h"
#include "FastMarkAppender.h"
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
#include <wtf/Atomics.h>
#include <wtf/CryptographicallyRandomNumber.h>
#include <wtf/DataLog.h>
#include <wtf/HashFunctions.h>
#include <wtf/MathExtras.h>
#include <wtf/OSAllocator.h>
#include <wtf/PageBlock.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/WTFConfig.h>

#if ENABLE(WEBASSEMBLY)
#include "JSWebAssemblyInstance.h"
#endif

#if OS(LINUX)
#include <errno.h>
#include <sys/mman.h>
#endif

namespace JSC { namespace AOT {

static std::pair<size_t, size_t> reservationSizes(const Image& image)
{
    InstanceSizes sizes = image.programTables().instanceSizes;
    RELEASE_ASSERT(InstanceLayout::isAddressable(sizes));
    return { InstanceLayout::bytesReservedBelowInstance(sizes), InstanceLayout::bytesReservedFromInstance(sizes) };
}

static void* reserveZeroedPages(size_t bytes)
{
    void* pages = reserveGuardedPages(bytes);
#if OS(LINUX)
    while (madvise(pages, bytes, MADV_NOHUGEPAGE) == -1 && errno == EAGAIN) { }
#endif
    return pages;
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

static Instance* reserveMemory(const Image& image)
{
    RELEASE_ASSERT(!(InstanceLayout::granuleBelowInstance % WTF::pageSize()) && !(InstanceLayout::granuleAboveInstance % WTF::pageSize()));
    auto [below, above] = reservationSizes(image);
    auto* instance = reinterpret_cast<Instance*>(static_cast<char*>(reserveZeroedPages(below + above)) + below);
    copyMethodTable(instance, image);
    return instance;
}

static void releaseMemory(Instance* instance)
{
    auto [below, above] = reservationSizes(*instance->vm->aotState().image);
    releaseGuardedPages(reinterpret_cast<char*>(instance) - below, below + above);
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

    InstancePrefix& prefix = instance.prefix();
    prefix.structureIDBase = JSC::structureIDBase();
    prefix.notUndefinedOrNullMask = ~static_cast<int64_t>(JSValue::ValueUndefined);
    prefix.boxedNaN = JSValue::encode(jsNaN());
    prefix.one = 1;
    prefix.pureNaN = PNaN;
    prefix.minInt64 = std::numeric_limits<int64_t>::min();
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
    for (const StubReturnSite& site : image.stubReturnSites()) {
        if (!hasSingleReturnSite(site.kind))
            continue;
        const void*& slot = instance.stubs.returnAddresses[static_cast<unsigned>(site.kind)];
        RELEASE_ASSERT(!slot);
        slot = image.sharedCodeCopy(0) + site.offset;
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
    words.closureFunctionKindsOrigin = std::bit_cast<uintptr_t>(image.programTables().closureFunctionKinds().data()) - image.programTables().closureFunctions.first * sizeof(FunctionKindData);
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
    words.emptyExtraPropertiesStructure = JSFinalObject::createStructure(vm, realm, jsNull(), extraPropertiesInlineCapacity);
    words.boundFunctionClassInfo = JSBoundFunction::info();
    words.objectPrototype = realm->objectPrototype();
    words.functionPrototypeCall = realm->linkTimeConstant(LinkTimeConstant::callFunction);
    words.smallIntStrings = vm.numericStrings.smallIntCache();
    words.intStrings = vm.numericStrings.intCache();
}

static void fillInstanceClassInfos(Instance& instance)
{
    unsigned numberOfClasses = BuiltinTable::singleton().classCount();
    std::span classInfos = unsafeMakeSpan(instance.state->tableArena.allocate<const ClassInfo*>(numberOfClasses), numberOfClasses);
    BuiltinBindings::fillInstanceClassInfos(classInfos);
    instance.realm.instanceClassInfos = classInfos.data();
}

#if CPU(X86_64) && OS(LINUX)
static void* loopPollPage(Instance* instance)
{
    return reinterpret_cast<char*>(instance) + InstanceLayout::offsetOfLoopPollPage();
}
#endif

void makeCallsByValuePoll(Instance& instance, const uint8_t* isRenamedAtRunTime)
{
    StackManager::Mirror& limit = instance.callsByValue.limit.get();
    if (!limit.isOnList())
        return;
    instance.callsByValue.isRenamedAtRunTime = isRenamedAtRunTime;
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
        makeCallsByValuePoll(*instance, tables->isRenamedAtRunTime.load(std::memory_order_acquire));
#if CPU(X86_64) && OS(LINUX)
    VMState::LoopPollPages& loopPollPages = vm.aotState().loopPollPages;
    Locker locker { loopPollPages.lock };
    void* page = loopPollPage(instance);
    loopPollPages.pages.append(page);
    if (vm.traps().stopsAtEveryPoll() || vm.traps().needsOutOfMemoryError())
        RELEASE_ASSERT(!mprotect(page, WTF::pageSize(), PROT_NONE));
#endif
}

static void unregisterStackMirror(Instance* instance, VM& vm)
{
#if CPU(X86_64) && OS(LINUX)
    {
        VMState::LoopPollPages& loopPollPages = vm.aotState().loopPollPages;
        Locker locker { loopPollPages.lock };
        loopPollPages.pages.removeFirst(loopPollPage(instance));
    }
#endif
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

Instance* reserveInstance(JSGlobalObject* realm)
{
    VM& vm = realm->vm();
    RELEASE_ASSERT(vm.hasCompiledCode() && !realm->aotInstance());
#if ENABLE(WEBASSEMBLY)
    RELEASE_ASSERT(static_cast<ptrdiff_t>(AOT_INSTANCE_OFFSET(vm)) == JSWebAssemblyInstance::offsetOfVM());
#endif
    Instance::destroyDeadInstances(vm);
    VMState& vmState = vm.aotState();
    const Image& image = *vmState.image;
    if (!(image.programTables().flags & ProgramTables::mayRunInMultipleInstances) && !vmState.instances.isEmpty())
        return nullptr;
    vmState.runtimeTable->finishInitialization(vm);
    vmState.identifiers->createEngineSymbols();

    Instance* instance = reserveMemory(image);
    fillHeaderAndPrefix(*instance, vm, realm);
    fillImageWords(*instance, vm, image);
    fillNumberWords(*instance);
    fillAllocationSpaces(*instance, vm);
    vmState.propertyCaches->publishWords(*instance);
    realm->setAOTInstance(instance);
    return instance;
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

void Instance::captureCellHeaders()
{
    fillArrayHeaders(*this, globalObject);
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

    Instance* instance = realm->aotInstance();
    if (!instance) {
        instance = reserveInstance(realm);
        RELEASE_ASSERT(instance);
    }
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
    instance->state->caches->addClearedEachCollection("cloned structures"_s, asMutableByteSpan(instance->allocation.clonedStructures));
    instance->state->caches->addClearedEachCollection("copies without frame"_s, asMutableByteSpan(instance->allocation.copiesWithoutFrame));
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
    return vm->aotState().image->header().options;
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
    void* pages = reserveGuardedPages(size);
#if OS(LINUX)
    while (madvise(pages, size, MADV_NOHUGEPAGE) == -1 && errno == EAGAIN) { }
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
    releaseGuardedPages(pages, m_ranges[index].second);
    m_ranges.removeAt(index);
}

ZeroedPages::~ZeroedPages()
{
    for (auto& [pages, size] : m_ranges)
        releaseGuardedPages(pages, size);
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
        + sizeOfZeroedTable(SiteCaches::numberOfMissWords(tables.instanceSizes.numberOfSiteCaches), sizeof(uint64_t))
        + sizeOfZeroedTable(RememberedSlotSet::numberOfCardsFor(tables.instanceSizes.numberOfSiteCaches * sizeof(Slot), SiteCaches::regionShift), sizeof(uint8_t))
        + sizeOfZeroedTable(InstanceLayout::holdsGlobalBindings(tables.globalBindings().size() * sizeof(GlobalBinding)) ? 0 : tables.globalBindings().size(), sizeof(GlobalBinding))
        + sizeOfZeroedTable(tables.modules().size(), sizeof(uint8_t));
    if (usesWideReferences)
        capacity += sizeOfZeroedTable(tables.instanceSizes.numberOfStringCells, sizeof(JSCell*));
    return capacity;
}

ZeroedTableArena::ZeroedTableArena(ZeroedPages& pages, const ProgramTables& tables)
    : m_pages(pages)
{
    m_capacity = arenaCapacity(tables);
    m_start = static_cast<char*>(pages.allocate(m_capacity));
}

State::State(Instance& instance)
    : instance(instance)
    , tables(instance.tables())
    , tableArena(pages, tables)
    , propertyCaches(*this)
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

template<Entry entry, auto operation>
static ALWAYS_INLINE void* operationAddress()
{
    if constexpr (featureOf(entry).has_value()) {
        auto replacement = reinterpret_cast<decltype(operation)>(&operationNotUsedByImage);
        return tagCFunctionPtr<void*, OperationPtrTag>(addressUnlessRemoved<*featureOf(entry), operation>(replacement));
    } else
        return tagCFunctionPtr<void*, OperationPtrTag>(operation);
}

template<Entry entry, auto operation, typename Function>
struct CountedCall {
    static constexpr auto call = operation;
};

#if HAVE_MUST_TAIL_CALL
static ALWAYS_INLINE VM& vmFor(VM* vm) { return *vm; }
static ALWAYS_INLINE VM& vmFor(Instance* instance) { return *instance->vm; }
static ALWAYS_INLINE VM& vmFor(JSGlobalObject* globalObject) { return globalObject->vm(); }
static ALWAYS_INLINE VM& vmFor(const JSCell* cell) { return cell->vm(); }

template<typename First>
concept CanFindVM = requires(First first) { vmFor(first); };

template<Entry entry, auto operation, typename Result, typename First, typename... Rest, bool isNoexcept>
    requires CanFindVM<First>
struct CountedCall<entry, operation, Result(First, Rest...) noexcept(isNoexcept)> {
    static Result JIT_OPERATION_ATTRIBUTES call(First first, Rest... rest) noexcept(isNoexcept)
    {
        vmFor(first).aotState().runtimeTable->didCall(entry);
        if constexpr (std::is_same_v<First, Instance*>) {
            if (ArrivalCounts* arrivals = first->state ? first->state->arrivalCounts.get() : nullptr)
                arrivals->didCallOperation(entry);
        }
        MUST_TAIL_CALL return operation(first, rest...);
    }
};
#endif

template<bool countsCalls>
static void fillOperations(void** entries)
{
#define AOT_FILL_OPERATION(name) \
    entries[static_cast<unsigned>(Entry::name)] = operationAddress<Entry::name, countsCalls ? CountedCall<Entry::name, &name, std::remove_pointer_t<decltype(&name)>>::call : &name>();
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
    , m_countsCalls(HAVE_MUST_TAIL_CALL && Options::aotCountsPath())
#else
    : m_countsCalls(HAVE_MUST_TAIL_CALL && Options::aotCountsPath())
#endif
{
    const Image& image = *vm.aotState().image;
    (m_countsCalls ? fillOperations<true> : fillOperations<false>)(m_entries);
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

VMState::VMState() = default;

VMState::~VMState()
{
    RELEASE_ASSERT(!hasCompiledCode);
}

void VMState::attachImage(VM& vm)
{
    RELEASE_ASSERT(!hasCompiledCode && !image);
    image = Image::current();
    RELEASE_ASSERT(image);
    RELEASE_ASSERT(vm.structureStructure && !vm.structureRareDataStructure);

    structureIDBase = JSC::structureIDBase();
    validatesRuntime = image->header().options.validates || Options::validateAOTRuntime();
    vm.updateSoftReservedZoneSize(Options::softReservedZoneSize() + BuiltinCells::stackKeptFromProgram);

    caches = makeUnique<CacheRegistry>();
    caches->add(CacheKind::WeakByKey, "structures with fast indexing"_s, structuresWithFastIndexing);
    counters = makeUniqueWithoutFastMallocCheck<RuntimeCounters>();
    hasCompiledCode = true;
    vm.setMayHaveUncacheableTransitions();

    vm.heap.setValidatesCellVisits(validatesRuntime || Options::verifyGC());

    reporter = makeUniqueWithoutFastMallocCheck<RuntimeReporter>(vm);
    runtimeTable = makeUnique<RuntimeTable>(vm);
    identifiers = makeUnique<ProgramIdentifiers>(vm, *image);
    callSiteCache = makeUnique<CallSiteCache>();
    propertyCaches = makeUnique<VMPropertyCaches>(vm);
    regExpLiterals = makeUnique<RegExpLiterals>(*caches);
    regExpMatchOutput = makeUniqueArray<int>(std::max<size_t>(image->header().counts.maxRegExpOutputSize, 2 * (maxGroupsInRegExpMatchOutput + 1)));

    moduleRecordStructure.setWithoutWriteBarrier(ModuleRecord::createStructure(vm, nullptr, jsNull()));
    stringBuilderStorageStructure.setWithoutWriteBarrier(StringBuilderStorage::createStructure(vm, nullptr, jsNull()));

    if (validatesRuntime)
        caches->addObserver("validation after collection"_s, collectionValidator);
}

// The rows of Structures are words of one table of the process, so they are given back while the Structure IDs still belong to this VM.
void VMState::willShutDownHeap()
{
    if (propertyCaches)
        propertyCaches->front().clear();
}

void VMState::releaseImage(VM& vm)
{
    if (!hasCompiledCode)
        return;
    ASSERT(vm.heap.isShuttingDown());

    reporter->writeAtExit();
    forgetErrorOfCodeLeftOut(vm);
    Instance::destroyAll(vm);
    RELEASE_ASSERT(instances.isEmpty() && instancesToDestroy.isEmpty());

    regExpLiterals = nullptr;
    regExpMatchOutput = nullptr;
    propertyCaches = nullptr;
    functionTables = nullptr;
    callSiteCache = nullptr;
    identifiers = nullptr;
    runtimeTable = nullptr;
    reporter = nullptr;
    counters = nullptr;
    caches = nullptr;

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
    for (size_t step : stepsOfAllocator) {
        if (canMap(step + addressSpaceToStartBesideAllocations())) {
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

#if OS(LINUX) && !ENABLE(AOT_GUARDED_PAGES)
static thread_local uint8_t* t_pagesOfThread;
static thread_local size_t t_sizeOfPagesOfThread;
static thread_local bool t_isStarting;
static std::atomic<unsigned> s_numberOfThreadsStarting;

// The thread may run another image of the executable than the thread that starts it. What its tables need beyond these pages they get as they do without them.
static size_t sizeOfPagesForNewThread(const Image& image)
{
    AOT_THRESHOLD(size_t, sizeOfSmallTables, 512 * KB);
    auto [below, above] = reservationSizes(image);
    return roundUpToMultipleOf(pageSize(), VMPropertyCaches::maxSizeOfPagesAtStart(image) + below + above + arenaCapacity(image.programTables()) + sizeOfSmallTables);
}

// Until a thread adopts them the pages begin with their size.
PagesForNewThread tryReservePagesForNewThread()
{
    const Image* image = Image::current();
    if (!hasAddressSpaceLimit() || !image)
        return { };
    size_t size = sizeOfPagesForNewThread(*image);
    void* pages = MarkedBlockCage::tryEnsureRoomForNewVMs(++s_numberOfThreadsStarting) ? OSAllocator::tryReserveAndCommit(size, OSAllocator::FastMallocPages) : nullptr;
    if (!pages) {
        --s_numberOfThreadsStarting;
        return { nullptr, true };
    }
    *static_cast<size_t*>(pages) = size;
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
    if (std::exchange(t_isStarting, false))
        --s_numberOfThreadsStarting;
}

static void* tryTakePagesOfThread(size_t bytes)
{
    bytes = roundUpToMultipleOf(pageSize(), bytes);
    if (bytes > t_sizeOfPagesOfThread)
        return nullptr;
    t_sizeOfPagesOfThread -= bytes;
    return std::exchange(t_pagesOfThread, t_pagesOfThread + bytes);
}
#else
PagesForNewThread tryReservePagesForNewThread() { return { }; }
void releasePagesForNewThread(void*) { }
void adoptPagesForNewThread(void*) { }
void releaseUnusedPagesOfThread() { }
static void* tryTakePagesOfThread(size_t) { return nullptr; }
#endif

void* tryReserveGuardedPages(size_t bytes, OSAllocator::Usage usage)
{
    if (void* pages = tryTakePagesOfThread(bytes))
        return pages;
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

static bool readCreatesCell(const Instance& instance, SlotRepresentation representation, const void* slot)
{
    if (traitsOf(representation).valueKind != SlotValueKind::LiteralString)
        return false;
    uint64_t word = readSlotBits(representation, slot);
    if (!isLiteralStringWord(word))
        return false;
    uint32_t number = literalStringIndex(static_cast<uint32_t>(word));
    RELEASE_ASSERT(number < instance.tables().literalStrings().size());
    const ConstantCellWords& constants = instance.runtime.constants;
    return instance.options().useWideReferences() ? !constants.wideLiteralStrings[number] : !constants.literalStrings[number];
}

void ProgramStatics::initializeStatics(uint32_t firstWord, uint32_t endWord)
{
    auto* words = reinterpret_cast<uint64_t*>(m_instance.statics());
    std::span<const StaticsRun> runs = m_tables.staticsRuns();
    for (auto run = std::ranges::lower_bound(runs, firstWord, { }, &StaticsRun::firstWord); run != runs.end() && run->firstWord < endWord; ++run)
        std::ranges::fill(std::span { words + run->firstWord, run->numberOfWords }, run->bits());
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
    if (JSValue value = readSlot(slot.representation(), instance.statics() + offset, statics.m_strings))
        return value;
    if (std::optional<FunctionID> function = findFunctionVariable(statics.m_tables, offset))
        return closures.closure(vm, *function);
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
    if (!statics.m_programStaticsInitialized && offset < sizeFilledAtProgramEntry(statics.m_tables))
        return { };
    const uint8_t* address = instance.statics() + offset;
    if (readCreatesCell(instance, slot.representation(), address))
        return { };
    if (JSValue value = readSlot(slot.representation(), address, statics.m_strings))
        return value;
    if (std::optional<FunctionID> function = findFunctionVariable(statics.m_tables, offset))
        return closures.closureIfCreated(*function);
    return { };
}

void ProgramStatics::writeVariable(Instance& instance, SlotData slot, JSValue value)
{
    SlotRepresentation representation = slot.representation();
    RELEASE_ASSERT(representation != SlotRepresentation::DeferredClosure);
    ProgramStatics& statics = *instance.state->statics;
    statics.ensureProgramStaticsInitialized();
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
    initializeStatics(filled / sizeof(uint64_t), std::numeric_limits<uint32_t>::max());
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
