/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTInstance.h"

#if ENABLE(AOT)

#include "AOTBuiltinTableBindings.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTHeapHooks.h"
#include "AOTImage.h"
#include "AOTIteration.h"
#include "AOTLimits.h"
#include "AOTOperationScope.h"
#include "AOTProgramIdentifiers.h"
#include "AOTPropertyCaches.h"
#include "AOTRealmStartup.h"
#include "AOTRuntimeCounters.h"
#include "AOTRuntimeHooks.h"
#include "AOTRuntimeTable.h"
#include "AOTState.h"
#include "AOTStubBlob.h"
#include "AOTStubEntrypoints.h"
#include "DeferGC.h"
#include "JSBoundFunction.h"
#include "JSCInlines.h"
#include "JSPromise.h"
#include "LocalAllocator.h"
#include "NativeExecutable.h"
#include "ObjectPrototype.h"
#include "Symbol.h"
#include "VMTrapsInlines.h"
#include <array>
#include <limits>
#include <wtf/HashFunctions.h>
#include <wtf/OSAllocator.h>
#include <wtf/PageBlock.h>
#include <wtf/StdLibExtras.h>

#if ENABLE(WEBASSEMBLY)
#include "JSWebAssemblyInstance.h"
#endif

#if OS(LINUX)
#include <errno.h>
#include <sys/mman.h>
#endif

namespace JSC { namespace AOT {

struct ReservationSizes {
    size_t below;
    size_t above;
};

static ReservationSizes reservationSizes(const Image& image)
{
    InstanceSizes sizes = image.programTables().instanceSizes;
    RELEASE_ASSERT(InstanceLayout::isAddressable(sizes));
    return {
        InstanceLayout::bytesReservedBelowInstance(sizes),
        InstanceLayout::bytesReservedFromInstance(sizes),
    };
}

static void* reserveZeroedPages(size_t bytes)
{
    void* pages = OSAllocator::reserveAndCommit(bytes, OSAllocator::FastMallocPages);
#if OS(LINUX)
    while (madvise(pages, bytes, MADV_NOHUGEPAGE) == -1 && errno == EAGAIN) { }
#endif
    return pages;
}

static Instance* reserveMemory(const Image& image)
{
    RELEASE_ASSERT(!(InstanceLayout::granuleBelowInstance % WTF::pageSize()) && !(InstanceLayout::granuleAboveInstance % WTF::pageSize()));
    auto [below, above] = reservationSizes(image);
    return reinterpret_cast<Instance*>(static_cast<char*>(reserveZeroedPages(below + above)) + below);
}

static void releaseMemory(Instance* instance)
{
    auto [below, above] = reservationSizes(*instance->vm->aotState().image);
    OSAllocator::decommitAndRelease(reinterpret_cast<char*>(instance) - below, below + above);
}

static uint32_t idOf(Structure* structure)
{
    return structure->id().bits();
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
    words.programIdentifiers = vm.aotState().identifiers->identifiers();
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
    instance.allocation.cagedAuxiliarySpace = &vm.heap.cagedAuxiliarySpace;
    instance.allocation.cellSpace = &vm.cellSpace();
    instance.allocation.primitiveAuxiliarySpace = &vm.primitiveGigacageAuxiliarySpace();
    instance.allocation.compactTableHeader = cellHeaderFor(vm.aotState().runtimeTable->compactTableStructure());
}

static void fillArrayHeaders(Instance& instance, JSGlobalObject* realm)
{
    if (realm->isHavingABadTime())
        return;
    InstancePrefix& prefix = instance.prefix();
    prefix.newArrayWithInt32Header = cellHeaderFor(realm->arrayStructureForIndexingTypeDuringAllocation(ArrayWithInt32));
    prefix.newArrayWithDoubleHeader = cellHeaderFor(realm->arrayStructureForIndexingTypeDuringAllocation(ArrayWithDouble));
    prefix.newArrayWithContiguousHeader = cellHeaderFor(realm->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous));
    instance.allocation.newCopyOnWriteArrayHeaders[0] = cellHeaderFor(realm->arrayStructureForIndexingTypeDuringAllocation(CopyOnWriteArrayWithInt32));
    instance.allocation.newCopyOnWriteArrayHeaders[1] = cellHeaderFor(realm->arrayStructureForIndexingTypeDuringAllocation(CopyOnWriteArrayWithDouble));
    prefix.newCopyOnWriteArrayWithContiguousHeader = cellHeaderFor(realm->arrayStructureForIndexingTypeDuringAllocation(CopyOnWriteArrayWithContiguous));
}

static void fillRealmWords(Instance& instance, VM& vm, JSGlobalObject* realm)
{
    // No cell has the number zero. didCreateInstanceStructure() fills the word of a class that the realm creates later.
    if (Structure* structure = realm->mapStructureConcurrently())
        instance.receiverStructureID(Receiver::Map) = idOf(structure);
    if (Structure* structure = realm->setStructureConcurrently())
        instance.receiverStructureID(Receiver::Set) = idOf(structure);
    instance.receiverStructureID(Receiver::RegExp) = idOf(realm->regExpStructure());

    RealmWords& words = instance.realm;
    words.boundFunctionStructureID = idOf(realm->boundFunctionStructure());
    words.hostFunctionStructureID = idOf(realm->hostFunctionStructure());
    words.boundFunctionExecutable = vm.getBoundFunction(true, SourceTaintedOrigin::Untainted);
    words.emptyExtraPropertiesStructure = JSFinalObject::createStructure(vm, realm, jsNull(), extraPropertiesInlineCapacity);
    words.boundFunctionClassInfo = JSBoundFunction::info();
    words.objectPrototype = realm->objectPrototype();
    words.functionPrototypeCall = realm->linkTimeConstant(LinkTimeConstant::callFunction);
    words.smallIntStrings = vm.numericStrings.smallIntCache();
    words.intStrings = vm.numericStrings.intCache();
}

static void fillInstanceClassInfos(Instance& instance)
{
    std::span classInfos = unsafeMakeSpan(instance.state->tableArena.allocate<const ClassInfo*>(numberOfBuiltinClasses), numberOfBuiltinClasses);
    for (unsigned index = 0; index < numberOfBuiltinClasses; ++index)
        classInfos[index] = BuiltinBindings::instanceClassInfoOf(BuiltinClassID(index));
    instance.realm.instanceClassInfos = classInfos.data();
}

#if CPU(X86_64) && OS(LINUX)
static void* loopPollPage(Instance* instance)
{
    return reinterpret_cast<char*>(instance) + InstanceLayout::offsetOfLoopPollPage();
}
#endif

static void registerStackMirror(Instance* instance, VM& vm)
{
    vm.traps().registerMirror(instance->stackMirror.get());
#if CPU(X86_64) && OS(LINUX)
    VMState::LoopPollPages& loopPollPages = vm.aotState().loopPollPages;
    Locker locker { loopPollPages.lock };
    void* page = loopPollPage(instance);
    loopPollPages.pages.append(page);
    if (instance->stackMirror.get().trapAwareSoftStackLimit() == reinterpret_cast<void*>(StackManager::StopRequestMarkerValue))
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
    vm.traps().unregisterMirror(instance->stackMirror.get());
}

static void initializeRuntimeWords(Instance& instance, VM& vm)
{
    State& state = *instance.state;
    RuntimeWords& words = instance.runtime;
    std::span propertyRuns = state.tables.propertyRuns();
    size_t numberOfPropertyRuns = propertyRuns.empty() ? 0 : propertyRuns[0] - 1;
    words.literals.propertyRuns = state.tableArena.allocate<const PropertyRun*>(numberOfPropertyRuns);

    if (instance.options().countsBranches)
        words.counters.branchCounts = static_cast<uint64_t*>(state.pages.allocate(runtimeLimitValue(vm, LimitID::BranchCounters) * sizeof(uint64_t)));
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
        receiverStructureID(Receiver::Map) = idOf(structure);
    else if (builtinClass == BuiltinClasses::Set)
        receiverStructureID(Receiver::Set) = idOf(structure);
}

void Instance::captureCellHeaders()
{
    fillArrayHeaders(*this, globalObject);
    for (auto& inlineAllocation : allocation.inlineAllocations)
        inlineAllocation = { };
    for (auto& typedArrayAllocation : allocation.typedArrayAllocations)
        typedArrayAllocation = { };
    prepareInlineAllocation<JSPromise>(this, InlineAllocation::Promise, globalObject->promiseStructure());
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
    fillInstanceClassInfos(*instance);
    initializeRuntimeWords(*instance, vm);

    registerStackMirror(instance, vm);
    VMState& vmState = vm.aotState();
    {
        Locker locker { vmState.instancesLock };
        vmState.instances.append(instance);
    }
    vmState.propertyCaches->didCreateInstance(*instance);
    RuntimeHooks::didChangeMicrotaskQueueing(vm);
    return instance;
}

void Instance::destroy(Instance* instance)
{
    VM& vm = *instance->vm;
    VMState& vmState = vm.aotState();
    {
        Locker locker { vmState.instancesLock };
        vmState.instances.removeFirst(instance);
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

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
