/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTInstance.h"

#if ENABLE(AOT)

#include "AOTArrayAllocationProfile.h"
#include "AOTBuiltinCells.h"
#include "AOTBuiltinFamilyState.h"
#include "AOTBuiltinModules.h"
#include "AOTBuiltinTable.h"
#include "AOTBuiltinTableBindings.h"
#include "AOTCallSiteCache.h"
#include "AOTCellFormat.h"
#include "AOTChainDependents.h"
#include "AOTCheckedSpan.h"
#include "AOTClassGuards.h"
#include "AOTClosure.h"
#include "AOTCompactTable.h"
#include "AOTCompileOptions.h"
#include "AOTCoroutineTable.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsNet.h"
#include "AOTFaultHandler.h"
#include "AOTFunctionTables.h"
#include "AOTGenerator.h"
#include "AOTGlobalBindings.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTHeapHooks.h"
#include "AOTImage.h"
#include "AOTImageLoading.h"
#include "AOTMembersLeftOut.h"
#include "AOTModuleHost.h"
#include "AOTModuleRecord.h"
#include "AOTModules.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTOperationData.h"
#include "AOTOperationScope.h"
#include "AOTPrefixCode.h"
#include "AOTProgramIdentifiers.h"
#include "AOTProgramStatics.h"
#include "AOTProgramStrings.h"
#include "AOTPropertyCaches.h"
#include "AOTPropertyCachesStringKeys.h"
#include "AOTRealmStartup.h"
#include "AOTRegExpRecords.h"
#include "AOTRuntimeCheckFailure.h"
#include "AOTRuntimeFunctions.h"
#include "AOTRuntimeHooks.h"
#include "AOTRuntimeReporter.h"
#include "AOTRuntimeTable.h"
#include "AOTSlotAccess.h"
#include "AOTState.h"
#include "AOTStringBuilderStorage.h"
#include "AOTStubBlob.h"
#include "AOTStubEntrypoints.h"
#include "AOTStubFrontEnds.h"
#include "AOTStubsExceptions.h"
#include "AOTUnwinding.h"
#include "AOTVMState.h"
#include "AbstractSlotVisitorInlines.h"
#include "AddressSpaceBudget.h"
#include "BooleanConstructor.h"
#include "DeferGC.h"
#include "ArrayConstructor.h"
#include "ErrorConstructor.h"
#include "FastMarkAppender.h"
#include "GlobalObjectMethodTable.h"
#include "HeapAnalyzer.h"
#include "HeapIterationScope.h"
#include "IntlObject.h"
#include "JSAsyncGenerator.h"
#include "JSBoundFunction.h"
#include "JSCInlines.h"
#include "JSCJSValueInlines.h"
#include "JSCustomGetterFunction.h"
#include "JSArrayBufferView.h"
#include "JSFunction.h"
#include "JSModuleLoader.h"
#include "JSPromise.h"
#include "LLIntData.h"
#include "LLIntThunks.h"
#include "LocalAllocator.h"
#include "MarkedBlockCage.h"
#include "NativeExecutable.h"
#include "NumberConstructor.h"
#include "ObjectConstructor.h"
#include "ObjectPrototype.h"
#include "Opcode.h"
#include "Options.h"
#include "ParserArena.h"
#include "ReservedRangeHeap.h"
#include "SlotVisitorInlines.h"
#include "StringConstructor.h"
#include "SubspaceInlines.h"
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
#include <wtf/OSAllocator.h>
#include <wtf/PageBlock.h>
#include <wtf/Scope.h>
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
    return { InstanceLayout::bytesReservedBelowInstance(sizes), InstanceLayout::bytesReservedFromInstance(sizes) + AccessTable::alignment + sizeof(AccessTable) };
}

static AccessTable& accessTableOf(Instance& instance, const Image& image)
{
    uintptr_t end = std::bit_cast<uintptr_t>(&instance) + InstanceLayout::bytesReservedFromInstance(image.programTables().instanceSizes);
    return *std::bit_cast<AccessTable*>(roundUpToMultipleOf<AccessTable::alignment>(end));
}

static Instance* reserveMemory(PagesOfVM& pages, const Image& image)
{
    RELEASE_ASSERT(!(InstanceLayout::largestPageSize % WTF::pageSize()));
    auto [below, above] = reservationSizes(image);
    auto* instance = reinterpret_cast<Instance*>(static_cast<char*>(pages.allocate(below + above)) + below);
    return instance;
}

static void releaseMemory(Instance* instance)
{
    VMState& vmState = instance->vm->aotState();
    auto [below, above] = reservationSizes(*instance->program);
    if (std::span<uint32_t> methodTable = instance->methodTable(); !methodTable.empty())
        OSAllocator::protect(methodTable.data(), InstanceLayout::bytesReservedForMethodTable(instance->tables().instanceSizes), true, true);
    vmState.pages->release(reinterpret_cast<char*>(instance) - below, below + above);
}

static void fillMethodTable(Instance& instance, const Image& image)
{
    std::span<uint32_t> methodTable = instance.methodTable();
    if (methodTable.empty())
        return;
#if OS(DARWIN) || OS(LINUX)
    std::span<const uint8_t> pagesOfImage { reinterpret_cast<const uint8_t*>(image.methodTable().data()), roundUpToMultipleOf(pageSize(), methodTable.size_bytes()) };
    if (mapPrivateCloneOfImagePages(image, pagesOfImage, methodTable.data()))
        return;
#endif
    volatile uint32_t* to = methodTable.data();
    const volatile uint32_t* from = image.methodTable().data();
    for (size_t index = 0; index < methodTable.size(); index += pageSize() / sizeof(uint32_t)) {
        to[index] = 0;
        static_cast<void>(from[index]);
    }
    memcpySpan(methodTable, image.methodTable());
    OSAllocator::protect(methodTable.data(), InstanceLayout::bytesReservedForMethodTable(instance.tables().instanceSizes), true, false);
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
    static_assert(static_cast<unsigned>(TypeofType::Function) + 1 == InstanceHeader::numberOfTypeStrings);
    for (unsigned type = 0; type < InstanceHeader::numberOfTypeStrings; ++type)
        instance.typeStrings[type] = vm.smallStrings.typeString(static_cast<TypeofType>(type));
    instance.collation.ducetLevel1Weights = ducetLevel1Weights;

    InstancePrefix& prefix = instance.prefix();
    prefix.structureIDBase = JSC::structureIDBase();
    prefix.notUndefinedOrNullMask = ~static_cast<int64_t>(JSValue::ValueUndefined);
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

static void fillImageWords(Instance& instance, VM& vm, const Image& image, ProgramIdentifiers& identifiers)
{
    ImageWords& words = instance.image;
    words.code = image.code().data();
    words.tables = &image.programTables();
    words.fieldTableEntries = image.programTables().fieldTableEntries().data();
    words.fieldRecordIndices = image.programTables().fieldRecordIndices().data();
    words.classAccessors = image.programTables().classAccessors().data();
    words.data = image.programTables().data().data();
    words.closureFactNumbers = image.programTables().closureFactNumbers().data();
    words.closureFacts = image.programTables().closureFacts().data();
    words.regExps = image.section<ImageRegExp>(ImageSection::RegExps).data();
    words.regExpCode = image.regExpCode().data();
    words.functionSteps = image.functionStarts().steps();
    words.functionStartGroups = image.functionStarts().groups();
#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
    words.stringRecords = image.text().stringRecords().data();
    words.lazyStringHeaderOffsets = image.text().lazyHeaderOffsets();
    instance.lazyStrings = { image.text().lazyHeaders(), image.text().lazyHeadersCapacity() };
#else
    words.programIdentifiers = vm.aotState().identifiers->identifiers();
    words.sizeOfProgramIdentifiers = static_cast<uint64_t>(vm.aotState().identifiers->count()) * sizeof(UniquedStringImpl);
#endif
    static_assert(sizeof(ImageStringWord) == sizeof(uint32_t));
    words.endOfIdentifiers = image.header().numbers.endOfIdentifiers;
    words.stringWords = std::bit_cast<const uint32_t*>(std::bit_cast<uintptr_t>(image.section<uint32_t>(ImageSection::Strings).data()) - words.endOfIdentifiers * sizeof(uint32_t));
    words.stringConstants = image.section<uint32_t>(ImageSection::StringConstants).data();
    words.hashesOfPlacedConstants = image.text().hashesOfPlacedConstants().data();
    words.numberOfPlacedConstants = image.text().hashesOfPlacedConstants().size();
    static_assert(!(InstanceLayout::largestPageSize % InstanceLayout::sizeOfIndexOfSiteAnswers) && !(InstanceLayout::distanceToPageEdge % InstanceLayout::sizeOfIndexOfSiteAnswers));
    static_assert(sizeof(uint16_t) == InstanceLayout::sizeOfIndexOfSiteAnswers);
    RELEASE_ASSERT(InstanceLayout::offsetOfIndexOfSiteAnswers(image.programTables().instanceSizes, image.programTables().instanceSizes.numberOfSiteCaches) <= INT32_MAX);
    words.namesOfSitesByDistance = reinterpret_cast<const uint8_t*>(image.section<uint16_t>(ImageSection::NamesOfSites).data()) - InstanceLayout::offsetOfIndicesOfSiteAnswers(image.programTables().instanceSizes);
    words.listedStrings = image.section<uint32_t>(ImageSection::LongStrings).data();
    words.text = image.section<uint8_t>(ImageSection::Text).data();
    words.programSymbols = &identifiers.symbols();
    identifiers.didGainInstance();
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
    words.keyMultiplierOfTables = CompactTable::keyMultiplier;
}

static void fillAllocationSpaces(Instance& instance, VM& vm)
{
    instance.allocation.auxiliarySpace = &vm.auxiliarySpace();
    instance.allocation.cagedAuxiliarySpace = vm.heap.cagedAuxiliarySpace<SubspaceAccess::OnMainThread>();
    instance.allocation.cellSpace = &vm.cellSpace();
    instance.allocation.primitiveAuxiliarySpace = &vm.primitiveGigacageAuxiliarySpace();
    instance.allocation.compactTableHeader = cellHeaderFor(vm.aotState().runtimeTable->compactTableStructure());
    instance.allocation.textBufferStructureID = vm.aotState().runtimeTable->textBufferStructure()->id().bits();
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
    if (Structure* structure = realm->mapStructureConcurrently())
        instance.receiverStructureID(Receiver::Map) = structure->id().bits();
    if (Structure* structure = realm->setStructureConcurrently())
        instance.receiverStructureID(Receiver::Set) = structure->id().bits();
    instance.receiverStructureID(Receiver::RegExp) = realm->regExpStructure()->id().bits();
    BuiltinBindings::forEachInstanceStructureOfHost(realm, [&](const BuiltinClassBinding& binding, Structure* structure) {
        instance.didCreateInstanceStructure(binding.builtinClass, structure);
    });

    RealmWords& words = instance.realm;
    words.boundFunctionStructureID = realm->boundFunctionStructure()->id().bits();
    words.hostFunctionStructureID = realm->hostFunctionStructure()->id().bits();
    words.atomListStructureID = realm->vm().cellButterflyOnlyAtomStringsStructure->id().bits();
    constexpr bool targetIsJSFunction = false;
    words.boundFunctionExecutable = vm.getBoundFunction(targetIsJSFunction, SourceTaintedOrigin::Untainted);
    words.emptyExtraPropertiesStructure = JSFinalObject::createStructure(vm, realm, jsNull(), extraPropertiesInlineCapacity);
    words.extraPropertiesWithElementsStructure = Structure::nonPropertyTransition(vm, words.emptyExtraPropertiesStructure, TransitionKind::AllocateContiguous, nullptr);
    words.headerOfExtraPropertiesWithElements = cellHeaderFor(words.extraPropertiesWithElementsStructure);
    words.boundFunctionClassInfo = JSBoundFunction::info();
    words.objectPrototype = realm->objectPrototype();
    words.functionPrototypeCall = realm->linkTimeConstant(LinkTimeConstant::callFunction);
    words.smallIntStrings = vm.numericStrings.smallIntCache();
    words.intStrings = vm.numericStrings.intCache();
    words.keysOfPairs = StringKeyCache::noKeysOfPairs();
    words.keyOfPairMask = 0;
    words.stringsOfImmediates[JSValue::ValueNull] = vm.smallStrings.nullString();
    words.stringsOfImmediates[JSValue::ValueUndefined] = vm.smallStrings.undefinedString();
    words.stringsOfImmediates[JSValue::ValueTrue] = vm.smallStrings.trueString();
    words.stringsOfImmediates[JSValue::ValueFalse] = vm.smallStrings.falseString();
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
    WTF::atomicStore<MemoryOrder::Relaxed>(&instance.pollIsDue, static_cast<uintptr_t>(enabled));
    if constexpr (hasFaultingLoopPolls)
        OSAllocator::protect(reinterpret_cast<char*>(&instance) + InstanceLayout::offsetOfLoopPollPage(), WTF::pageSize(), !enabled, !enabled);
}

static void registerStackMirror(Instance* instance, VM& vm)
{
    vm.traps().registerMirror(instance->stackMirror.get());
    vm.traps().registerMirror(instance->callsByValue.limit.get());
    VMState::PolledInstances& polled = vm.aotState().polledInstances;
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
    vm.traps().unregisterMirror(instance->callsByValue.limit.get());
    vm.traps().unregisterMirror(instance->stackMirror.get());
}

static void initializeRuntimeWords(Instance& instance)
{
    State& state = *instance.state;
    RuntimeWords& words = instance.runtime;
    if (state.tables.instanceSizes.numberOfGlobalBindings) {
        words.bindings.globalBindings = instance.tableAt<GlobalBinding>(InstanceLayout::offsetOfGlobalBindings(state.tables.instanceSizes));
        state.globalBindings = makeUnique<GlobalBindings>(state);
    }
}

size_t numberOfRunningInstances(const VMState& state)
{
    return std::ranges::count_if(state.instances, [](const Instance* instance) {
        return !instance->globalObject || instance->globalObject->microtaskRunnability() != QueuedTaskResult::Discard;
    });
}

Instance* reserveInstance(JSGlobalObject* realm)
{
    VM& vm = realm->vm();
    RELEASE_ASSERT(vm.hasCompiledCode() && !realm->aotInstance());
#if ENABLE(WEBASSEMBLY)
    RELEASE_ASSERT(static_cast<ptrdiff_t>(AOT_INSTANCE_OFFSET(vm)) == JSWebAssemblyInstance::offsetOfVM());
#endif
    Instance::destroyDeadInstances(vm);
    forgetUnfinishedLoadsOfRetiredRealms(vm);
    VMState& vmState = vm.aotState();
    const Image& image = *vmState.image;
    bool mayRunInMultipleInstances = image.programTables().flags & ProgramTables::mayRunInMultipleInstances;
    if (!mayRunInMultipleInstances && numberOfRunningInstances(vmState))
        return nullptr;
    vmState.identifiers->createEngineSymbols();
    ProgramIdentifiers* identifiers = vmState.identifiers.get();
    if (mayRunInMultipleInstances)
        identifiers = new ProgramIdentifiers(vm, image, identifiers);
    vmState.runtimeTable->finishInitialization(vm);

    Instance* instance = reserveMemory(*vmState.pages, image);
    instance->program = &image;
    fillHeaderAndPrefix(*instance, vm, realm);
    fillImageWords(*instance, vm, image, *identifiers);
    fillMethodTable(*instance, image);
    instance->siteAnswers()[0] = SiteAnswers::forPlacesWithout();
    instance->propertyCaches.firstWordOfWideAnswers = InstanceLayout::bytesReservedForSiteAnswers / sizeof(uint64_t);
    fillNumberWords(*instance);
    fillAllocationSpaces(*instance, vm);
    vmState.propertyCaches->publishWords(*instance);
    instance->propertyCaches.accessTable.publish(accessTableOf(*instance, image));
    realm->setAOTInstance(instance);
    return instance;
}

Instance* Instance::owning(const JSCell* cell)
{
    return cell->structure()->realm()->aotInstance();
}

void Instance::didCreateInstanceStructure(BuiltinClassID builtinClass, Structure* structure)
{
    auto fill = [&](Receiver receiver) {
        uint32_t& word = receiverStructureID(receiver);
        ASSERT(!word || word == structure->id().bits());
        word = structure->id().bits();
    };
    if (builtinClass == BuiltinClasses::Map)
        fill(Receiver::Map);
    else if (builtinClass == BuiltinClasses::Set)
        fill(Receiver::Set);
    else if (builtinClass == BuiltinClasses::Buffer)
        fill(Receiver::Buffer);
}

void Instance::findShortArrayAllocators()
{
    static_assert(LocalAllocator::offsetOfFreeList() + sizeof(FreeList) <= ShortArrayWords::wordsOfExhaustedAllocator * sizeof(uint64_t));
    bool hasHeaders = prefix().newArrayWithContiguousHeader;
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
    realm.emptyArgumentList = JSArray::create(*vm, globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous));
}

Instance* Instance::create(JSGlobalObject* realm)
{
    VM& vm = realm->vm();
    RELEASE_ASSERT(vm.hasCompiledCode());
    if (const ModuleHost* host = realm->globalObjectMethodTable()->moduleHost)
        vm.aotState().image->setDecompressSource(host->decompressSource);
    takeFaultsOfThisThreadOnAnotherStack();
    DeferGCForAWhile deferGC(vm);
    destroyDeadInstances(vm);

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
    constexpr unsigned entryOfEffectsNet = static_cast<unsigned>(Entry::operationAOTStopAtFalseEffects);
    if (vm.aotState().image->header().entriesUsed[entryOfEffectsNet / 32] >> entryOfEffectsNet % 32 & 1) [[unlikely]]
        instance->effectsNet.exercised = makeEffectsNetExercised().data();

    State::create(*instance);
    instance->state->caches->addClearedEachCollection("growing flat text"_s, asMutableByteSpan(instance->state->growingFlatText));
    instance->state->caches->addClearedEachCollection("wide character strings"_s, asMutableByteSpan(instance->state->wideCharacterStrings));
    instance->state->caches->addClearedEachCollection("texts of few strings"_s, asMutableByteSpan(instance->state->textsOfFewStrings));
    instance->state->caches->addClearedEachCollection("released heap frames"_s, asMutableByteSpan(instance->coroutines.releasedHeapFrames));
    instance->state->caches->addClearedEachCollection("released array"_s, asMutableByteSpan(instance->arrays.releasedArray));
    instance->state->caches->addClearedEachCollection("released copy"_s, asMutableByteSpan(instance->arrays.releasedCopy));
    instance->state->caches->addClearedEachCollection("sort keys"_s, asMutableByteSpan(instance->arrays.sortKeys));
    instance->state->caches->addClearedEachCollection("chains above lists without names"_s, asMutableByteSpan(instance->arrays.chainsAboveListsWithoutNames));
    instance->state->caches->addClearedEachCollection("structures of lists without names"_s, asMutableByteSpan(instance->arrays.structuresOfListsWithoutNames));
    instance->state->caches->addClearedEachCollection("structures around the first shift"_s, asMutableByteSpan(instance->arrays.structuresAroundFirstShift));
    instance->state->caches->addClearedEachCollection("copies without frame"_s, asWritableBytes(instance->copiesWithoutFrame()));
    const uint8_t* hasRunSinceClear = &instance->propertyCaches.hasRunSinceClear;
    instance->state->caches->addClearedEachCollection("keyed value places"_s, asWritableBytes(instance->keyedValuePlaces()), hasRunSinceClear);
    instance->state->caches->addClearedEachCollection("places hold copies of slots"_s, asMutableByteSpan(instance->propertyCaches.hasCachedSlotValues), hasRunSinceClear);
    instance->state->caches->addClearedEachCollection("forwarding handlers of proxies"_s, asMutableByteSpan(instance->propertyCaches.forwardingProxyHandlers), hasRunSinceClear);
    instance->state->caches->addClearedEachCollection("accepted kinds of values"_s, asWritableBytes(instance->acceptedKindsOfValues()), hasRunSinceClear);
    instance->state->caches->addClearedEachCollection("access table"_s, asMutableByteSpan(accessTableOf(*instance, *instance->program)), hasRunSinceClear);
    instance->state->caches->addClearedEachCollection("keys of the access table that depend on chains"_s, asMutableByteSpan(instance->propertyCaches.accessTable.numberOfKeysThatDependOnChains), hasRunSinceClear);
    instance->state->caches->addClearedEachCollection("replaced members"_s, asMutableByteSpan(instance->propertyCaches.replacedMembers));
    fillInstanceClassInfos(*instance);
    initializeRuntimeWords(*instance);

    registerStackMirror(instance, vm);
    VMState& vmState = vm.aotState();
    {
        Locker locker { vmState.instancesLock };
        vmState.instances.append(instance);
        vmState.didChangeInstances();
        vmState.lendTablesOfLivingInstances();
        for (Instance* each : vmState.instances)
            each->propertyCaches.forgetReplacedMembers();
    }
    vmState.propertyCaches->didCreateInstance(*instance);
    RuntimeHooks::didChangeMicrotaskQueueing(vm);
#if defined(BUN_HARDENED)
    OSAllocator::protect(instance->tableAt<uint8_t>(InstanceLayout::offsetOfWordsWrittenOnce), InstanceLayout::offsetOfTablesAboveInstance - InstanceLayout::offsetOfWordsWrittenOnce, true, false);
    if constexpr (InstanceLayout::offsetOfBoxedCalls < 0)
        OSAllocator::protect(instance->boxedCalls().data(), InstanceLayout::bytesReservedForBoxedCalls, true, false);
#endif
    releaseUnusedPagesOfThread();
    return instance;
}

void VMState::didChangeInstances()
{
    onlyInstance = instances.size() == 1 ? instances.first() : nullptr;
    if (onlyInstance)
        onlyInstance->propertyCaches.hasRunSinceClear = true;
    instanceOfRecentRun = { };
}

Instance* VMState::findInstanceThatLendsTables() const
{
    for (Instance* instance : instances) {
        if (instance->state)
            return instance;
    }
    return nullptr;
}

void VMState::lendTablesOfLivingInstances()
{
    for (JSGlobalObject* realm : realmsWithBorrowedTables) {
        if (!realm->aotInstance() && !instances.contains(realm->aotTableInstance()))
            realm->setAOTTableInstance(findInstanceThatLendsTables());
    }
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
    if (instance->state) {
        if (instance->state->modules)
            instance->state->modules->noteTextsThatHostKeeps();
        delete instance->state;
        unregisterStackMirror(instance, vm);
    }
    ProgramIdentifiers& identifiers = identifiersOf(*instance);
    if (identifiers.didLoseLastInstance() && identifiers.isOfOneRealm())
        delete &identifiers;
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
    size_t capacity = sizeOfFixedSizeTables
        + sizeOfZeroedTable(tables.firstEnvironmentShape, sizeof(uint32_t))
        + sizeOfZeroedTable(tables.allocations().size() + 1, sizeof(AllocationSiteRecord) + sizeof(int32_t))
        + sizeOfZeroedTable(tables.numberOfClasses(), sizeof(JSCell*))
        + sizeOfZeroedTable(tables.numberOfClosureCells, sizeOfCellReference)
        + sizeOfZeroedTable(tables.keyCellIdentifiers().size(), sizeOfCellReference)
        + sizeOfZeroedTable(tables.bigIntConstants().size(), sizeof(JSCell*))
        + sizeOfZeroedTable(InstanceLayout::holdsBuiltinCells(tables.builtinCells().size()) ? 0 : tables.builtinCells().size(), sizeof(JSCell*))
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
    , pages(*instance.vm->aotState().pages)
    , tableArena(pages, tables)
    , propertyCaches(*this)
    , stringKeys(*this)
{
    instance.state = this;
    caches = makeUnique<CacheRegistry>();
    immortalCells = makeUnique<ImmortalCells>(*this);
    objectThatHoldsUndefined = JSFinalObject::create(*instance.vm, JSFinalObject::createStructure(*instance.vm, nullptr, jsNull(), 1));
    objectThatHoldsUndefined->inlineStorage()[0].setWithoutWriteBarrier(jsUndefined());
    immortalCells->add(objectThatHoldsUndefined);
    strings = makeUnique<ProgramStrings>(*this);
    statics = makeUnique<ProgramStatics>(*this);
    builtinCells = makeUnique<BuiltinCells>(*this);
    chainDependents = makeUnique<ChainDependents>(*this);
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
    RuntimeCheckFailure failure("operation table check failed"_s);
    failure.detail("compiled code called a runtime operation that its image does not list as used");
    failure.report();
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

static constexpr bool isCalledWithoutCollection(Entry entry)
{
    std::optional<RuntimeOperation> operation = runtimeOperationOf(entry);
    return operation && !Typed::declaredBoundOf(*operation).mayCollect && !preservesMostRegisters(*operation);
}

static_assert(!isCalledWithoutCollection(Entry::operationAOTCatch) && !isCalledWithoutCollection(Entry::operationAOTCatchException) && !isCalledWithoutCollection(Entry::operationAOTCatchValueAndException));

template<Entry entry, auto operation>
static constexpr auto functionInTable = [] {
    if constexpr (isCalledWithoutCollection(entry))
        return CallWithoutCollection<operation>::call;
    else
        return operation;
}();

static ALWAYS_INLINE VM& vmFor(VM* vm) { return *vm; }
static ALWAYS_INLINE VM& vmFor(Instance* instance) { return *instance->vm; }
static ALWAYS_INLINE VM& vmFor(JSGlobalObject* globalObject) { return globalObject->vm(); }
static ALWAYS_INLINE VM& vmFor(const JSCell* cell) { return cell->vm(); }

template<typename First>
concept LeadsToVM = requires(First first) { vmFor(first); };

template<LeadsToVM First>
static NEVER_INLINE AOT_PRESERVE_MOST_LEAF_ATTRIBUTES void didCallCountedOperation(First first, Entry entry)
{
    vmFor(first).aotState().counts->didCall(entry);
}

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
    requires LeadsToVM<First>
struct CountedCall<entry, operation, Result JIT_OPERATION_ATTRIBUTES (First, Rest...) noexcept(isNoexcept)> {
    static constexpr bool isCounted = true;
    static Result JIT_OPERATION_ATTRIBUTES call(First first, Rest... rest) noexcept(isNoexcept)
    {
        didCallCountedOperation(first, entry);
        AOT_MUST_TAIL_CALL return operation(first, rest...);
    }
};

template<Entry entry, auto operation>
using CountedCallInTable = CountedCall<entry, functionInTable<entry, operation>, std::remove_pointer_t<decltype(operation)>>;

static constexpr bool isHandedWhatLeadsToVM(Entry entry)
{
    std::optional<RuntimeOperation> operation = runtimeOperationOf(entry);
    return !operation || !isLeaf(*operation);
}

bool countsCallsOf(Entry entry)
{
    switch (entry) {
#define AOT_COUNTS_CALLS_OF(name) case Entry::name: return CountedCallInTable<Entry::name, &name>::isCounted;
#define AOT_COUNTS_CALLS_OF_SCHEMA_OPERATION(name) AOT_COUNTS_CALLS_OF(operationAOT##name)
#define AOT_COUNTS_NO_CALLS_OF(name)
    FOR_EACH_AOT_RUNTIME_OPERATION(AOT_COUNTS_CALLS_OF_SCHEMA_OPERATION)
    FOR_EACH_AOT_OPERATION_OUTSIDE_SCHEMA(AOT_COUNTS_CALLS_OF, AOT_COUNTS_NO_CALLS_OF)
#undef AOT_COUNTS_NO_CALLS_OF
#undef AOT_COUNTS_CALLS_OF_SCHEMA_OPERATION
#undef AOT_COUNTS_CALLS_OF
    default:
        return false;
    }
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
    set(Entry::ErrorConstructorClassInfo, const_cast<ClassInfo*>(ErrorConstructor::info()));
    set(Entry::ArrayConstructorClassInfo, const_cast<ClassInfo*>(ArrayConstructor::info()));
    set(Entry::ObjectConstructorClassInfo, const_cast<ClassInfo*>(ObjectConstructor::info()));
    set(Entry::BooleanConstructorClassInfo, const_cast<ClassInfo*>(BooleanConstructor::info()));
    set(Entry::CustomGetterClassInfo, const_cast<ClassInfo*>(JSCustomGetterFunction::info()));
    set(Entry::NumberConstructorClassInfo, const_cast<ClassInfo*>(NumberConstructor::info()));
    set(Entry::StringConstructorClassInfo, const_cast<ClassInfo*>(StringConstructor::info()));
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

static_assert(!isOperationFamily(static_cast<RemovableFeature>(0)));
static constexpr auto familiesOfEntries = [] {
    std::array<uint16_t, entriesThatAreCalled.size() * 32> families { };
    for (unsigned index = 0; index < numberOfEntries; ++index) {
        std::optional<RemovableFeature> feature = featureOf(static_cast<Entry>(index));
        if (feature && isOperationFamily(*feature))
            families[index] = static_cast<uint16_t>(*feature);
    }
    return families;
}();

static void checkThatFamiliesOfUsedEntriesAreThere(const Image& image)
{
    const uint32_t* entriesUsed = image.header().entriesUsed;
    for (unsigned word = 0; word < entriesThatAreCalled.size(); ++word) {
        for (uint32_t used = entriesUsed[word]; used; used &= used - 1) {
            uint16_t family = familiesOfEntries[word * 32 + std::countr_zero(used)];
            if (family && isFeatureRemoved(static_cast<RemovableFeature>(family))) [[unlikely]]
                Bun__abortForRemovedFeature(family);
        }
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
#endif
{
    const Image& image = *vm.aotState().image;
    checkThatFamiliesOfUsedEntriesAreThere(image);
    (vm.aotState().counts ? fillOperations<true> : fillOperations<false>)(m_entries);
    fillThunkAndPointers(m_entries, image);
    installOperationFrontEnds(m_entries, image);
    trapUnusedEntries(m_entries, image);

    m_compactTableStructure = CompactTable::createStructure(vm);
    vm.aotState().caches->addStrongRoots("compact table structure"_s, { reinterpret_cast<JSCell* const*>(&m_compactTableStructure), 1 });
    m_textBufferStructure = JSString::createStructure(vm, nullptr, jsNull());
    vm.aotState().caches->addStrongRoots("text buffer structure"_s, { reinterpret_cast<JSCell* const*>(&m_textBufferStructure), 1 });
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

size_t StructuresWithFastIndexing::removeDeadEntries(VM& vm, CollectionScope)
{
    return m_structures.removeAllMatching([&](Structure* structure) {
        return !vm.heap.isMarked(structure);
    });
}

static size_t sizeOfPagesAtStart(const Image&);

VMState::VMState() = default;

VMState::~VMState()
{
    RELEASE_ASSERT(!hasCompiledCode);
}

void VMState::didParseSource(VM& vm, size_t length, std::unique_ptr<IdentifierArena>&& identifiers)
{
    constexpr size_t lengthBetweenPrunes = 1 * MB;
    lengthOfSourcesSincePrune += length;
    if (lengthOfSourcesSincePrune >= lengthBetweenPrunes) {
        lengthOfSourcesSincePrune = 0;
        vm.pruneUnusedSourceProviderCaches();
    }
    constexpr size_t mostTimesAsLarge = 8;
    if (identifiers && (!identifiersOfLastSource || identifiersOfLastSource->size() <= mostTimesAsLarge * identifiers->size()))
        identifiersOfLastSource = WTF::move(identifiers);
}

#if OS(LINUX) && ENABLE(AOT_GUARDED_PAGES)
static void didMapPropertyCachesOfThread();
#endif

void VMState::attachImage(VM& vm)
{
    RELEASE_ASSERT(!hasCompiledCode && !image);
    image = Image::current();
    RELEASE_ASSERT(image);
    Image::setCurrentOfThread(image);
    verifiesFastPathAnswers = image->header().options.verifiesFastPathAnswers;
    size_t sizeOfPages = sizeOfPagesAtStart(*image);
    size_t sizeOfRegExpMatchOutput = 2 * (maxGroupsInRegExpMatchOutput + 1);
    sizeOfRegExpMatchOutput = std::max<size_t>(sizeOfRegExpMatchOutput, image->header().counts.maxRegExpOutputSize);
    RELEASE_ASSERT(vm.structureStructure && !vm.structureRareDataStructure);

    structureIDBase = JSC::structureIDBase();
    vm.updateSoftReservedZoneSize(Options::softReservedZoneSize() + BuiltinCells::stackKeptFromProgram);

    pages = makeUnique<PagesOfVM>(sizeOfPages);
    caches = makeUnique<CacheRegistry>();
    caches->add(CacheKind::WeakByKey, "structures with fast indexing"_s, structuresWithFastIndexing);
    hasCompiledCode = true;
    vm.setMayHaveUncacheableTransitions();

    vm.heap.setValidatesCellVisits(Options::verifyGC());

    identifiers = makeUnique<ProgramIdentifiers>(vm, *image);
    if (Options::aotCountsPath()) [[unlikely]]
        counts = makeUnique<RuntimeCounts>(vm);
    runtimeTable = makeUnique<RuntimeTable>(vm);
    callSiteCache = makeUnique<CallSiteCache>();
    propertyCaches = makeUnique<VMPropertyCaches>(vm);
#if OS(LINUX) && ENABLE(AOT_GUARDED_PAGES)
    didMapPropertyCachesOfThread();
#endif
    regExpLiterals = makeUnique<RegExpLiterals>(*caches);
    regExpMatchOutput = makeUniqueArray<int>(sizeOfRegExpMatchOutput);
    this->sizeOfRegExpMatchOutput = sizeOfRegExpMatchOutput;

    moduleRecordStructure.setWithoutWriteBarrier(ModuleRecord::createStructure(vm, nullptr, jsNull()));
    stringBuilderStorageStructure.setWithoutWriteBarrier(StringBuilderStorage::createStructure(vm, nullptr, jsNull()));
}

void VMState::willShutDownHeap()
{
    if (propertyCaches)
        propertyCaches->front().clear();
    formatPatternState = nullptr;
    computedFunctionNames.clear();
}

void VMState::countWithoutImage(VM& vm)
{
    RELEASE_ASSERT(!hasCompiledCode && !image);
    counts = makeUnique<RuntimeCounts>(vm);
}

void VMState::releaseImage(VM& vm)
{
    counts = nullptr;
    clearOmittedCodeError(vm);
    if (!hasCompiledCode)
        return;
    ASSERT(vm.heap.isShuttingDown());

    pages->willReleaseEverything();
    Instance::destroyAll(vm);
    RELEASE_ASSERT(instances.isEmpty() && instancesToDestroy.isEmpty());

    regExpLiterals = nullptr;
    regExpMatchOutput = nullptr;
    sizeOfRegExpMatchOutput = 0;
    propertyCaches = nullptr;
    functionTables = nullptr;
    cyclesOfImports = nullptr;
    importersOfModules = nullptr;
    modulesWithQuery = std::nullopt;
    reexportedNames = nullptr;
    callSiteCache = nullptr;
    runtimeTable = nullptr;
    identifiers = nullptr;
    countedFiles = { };
    caches = nullptr;
    pages = nullptr;

    moduleRecordStructure.clear();
    stringBuilderStorageStructure.clear();
    poisonedCellStructure.clear();
    image = nullptr;
    structureIDBase = 0;
    hasCompiledCode = false;
}

#if ENABLE(AOT_GUARDED_PAGES)
static constexpr unsigned guardPagesOnEachEnd = 1;

extern "C" __attribute__((used, retain)) const char JSC__AOT__mapsGuardedPages[] = "guarded pages: what a compiled program maps for good has an address of its own";
#else
static constexpr unsigned guardPagesOnEachEnd = 0;
#endif

#if ENABLE(AOT_GUARDED_PAGES) && OS(LINUX) && CPU(ADDRESS64)
static void* tryReserveGuardedPagesAtRandomAddress(size_t bytes, OSAllocator::Usage usage)
{
    static constexpr uintptr_t lowestRandomAddress = 64 * GB;
    static constexpr uintptr_t highestRandomAddress = 124 * 1024 * GB;
    static constexpr unsigned maxDraws = 16;
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

static size_t sizeOfPagesAtStart(const Image& image)
{
    static constexpr size_t sizeOfSmallTables = 512 * KB;
    auto [below, above] = reservationSizes(image);
    return roundUpToMultipleOf(pageSize(), VMPropertyCaches::maxSizeOfPagesAtStart(image) + below + above + arenaCapacity(image.programTables()) + sizeOfSmallTables);
}

#if OS(LINUX)
#if !ENABLE(AOT_GUARDED_PAGES)
static thread_local uint8_t* t_pagesOfThread;
static thread_local size_t t_sizeOfPagesOfThread;
#endif
static thread_local bool t_isStarting;
static std::atomic<unsigned> s_numberOfThreadsStarting;
#if ENABLE(AOT_GUARDED_PAGES)
static thread_local bool t_hasNoPropertyCaches;
static std::atomic<unsigned> s_numberOfThreadsWithoutPropertyCaches;

static void didMapPropertyCachesOfThread()
{
    if (std::exchange(t_hasNoPropertyCaches, false))
        --s_numberOfThreadsWithoutPropertyCaches;
}

static size_t sizeOfPagesThatStartingThreadsWillMap(const Image& image, unsigned numberOfThreads)
{
    size_t sizeOfCaches = VMPropertyCaches::maxSizeOfPagesAtStart(image);
    return s_numberOfThreadsWithoutPropertyCaches * sizeOfCaches + numberOfThreads * (sizeOfPagesAtStart(image) - sizeOfCaches);
}
#endif

static bool tryEnsureRoomInAllocator(unsigned numberOfThreads, size_t sizeToLeaveFree)
{
#if USE(MIMALLOC)
    struct Pages {
        size_t size;
        unsigned count;
    };
    static constexpr Pages pagesOfThread[] = { { 64 * KB, 40 }, { 512 * KB, 8 }, { 4 * MB, 2 } };
    static constexpr unsigned piecesInPage = 7;
    static constexpr size_t alignmentOfPages = 64 * KB;
    static constexpr size_t smallestStep = 128 * MB;
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

static void* tryReservePagesForTables(const Image& image, unsigned numberOfThreads, size_t sizeToLeaveFree)
{
    if (!MarkedBlockCage::tryEnsureRoomForNewVMs(numberOfThreads))
        return nullptr;
#if ENABLE(AOT_GUARDED_PAGES)
    ++s_numberOfThreadsWithoutPropertyCaches;
    size_t needed = sizeOfPagesThatStartingThreadsWillMap(image, numberOfThreads) + sizeToLeaveFree;
    if (addressSpaceLeft() >= needed)
        return &s_numberOfThreadsStarting;
    --s_numberOfThreadsWithoutPropertyCaches;
    return nullptr;
#else
    size_t size = sizeOfPagesAtStart(image);
    if (addressSpaceLeft() < size + sizeToLeaveFree)
        return nullptr;
    void* pages = OSAllocator::tryReserveAndCommit(size, OSAllocator::FastMallocPages);
    if (pages)
        *static_cast<size_t*>(pages) = size;
    return pages;
#endif
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
    static constexpr size_t sizeToLeaveFreeForThread = 10 * MB;
    const Image* image = Image::current();
    if (!hasAddressSpaceLimit() || !image)
        return { };
    unsigned numberOfThreads = ++s_numberOfThreadsStarting;
    size_t sizeToLeaveFree = sizeToLeaveFreeForThread;
    if (threadsAreEnding)
        sizeToLeaveFree = numberOfThreads * sizeToLeaveFreeForThread + ReservedRangeHeap::addressSpaceToGrow();
    void* pages = tryReservePagesForTables(*image, numberOfThreads, sizeToLeaveFree);
    if (!pages) {
        --s_numberOfThreadsStarting;
        return { nullptr, true };
    }
#if ENABLE(AOT_GUARDED_PAGES)
    sizeToLeaveFree += sizeOfPagesThatStartingThreadsWillMap(*image, numberOfThreads);
#endif
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
#if ENABLE(AOT_GUARDED_PAGES)
    --s_numberOfThreadsWithoutPropertyCaches;
#else
    OSAllocator::releaseDecommitted(pages, *static_cast<size_t*>(pages), 0);
#endif
    --s_numberOfThreadsStarting;
}

void adoptPagesForNewThread(void* pages)
{
#if ENABLE(AOT_GUARDED_PAGES)
    t_hasNoPropertyCaches = !!pages;
#else
    RELEASE_ASSERT(!t_pagesOfThread);
    t_pagesOfThread = static_cast<uint8_t*>(pages);
    t_sizeOfPagesOfThread = pages ? std::exchange(*static_cast<size_t*>(pages), 0) : 0;
#endif
    t_isStarting = !!pages;
}

void releaseUnusedPagesOfThread()
{
#if !ENABLE(AOT_GUARDED_PAGES)
    if (t_sizeOfPagesOfThread)
        OSAllocator::releaseDecommitted(t_pagesOfThread, t_sizeOfPagesOfThread, 0);
    t_pagesOfThread = nullptr;
    t_sizeOfPagesOfThread = 0;
#endif
}

void didStartThread(VM* vm)
{
    if (!std::exchange(t_isStarting, false)) [[likely]]
        return;
#if ENABLE(AOT_GUARDED_PAGES)
    didMapPropertyCachesOfThread();
#endif
    if (vm)
        vm->heap.cage().willRunProgram();
    --s_numberOfThreadsStarting;
}

#if !ENABLE(AOT_GUARDED_PAGES)
static std::pair<uint8_t*, size_t> takePagesOfThread()
{
    return { std::exchange(t_pagesOfThread, nullptr), std::exchange(t_sizeOfPagesOfThread, 0) };
}
#endif
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

static std::optional<FunctionID> findFunctionVariable(const ProgramTables& tables, uint32_t offset)
{
    std::span<const FunctionVariableData> variables = tables.functionVariables();
    auto found = std::ranges::lower_bound(variables, offset, { }, &FunctionVariableData::offset);
    if (found == variables.end() || found->offset != offset)
        return std::nullopt;
    return FunctionID(found->function);
}

void ProgramStatics::putBack(uint32_t offset, uint32_t size)
{
    m_instance.state->countWriteForEffectsNet(Typed::CountInEffectsNet::WordsOfStaticsWritten);
    std::span<const StaticsRun> runs = m_tables.staticsRuns();
    auto initialWordAt = [&](uint32_t word) -> uint64_t {
        auto run = std::ranges::upper_bound(runs, word, { }, &StaticsRun::firstWord);
        if (run == runs.begin())
            return 0;
        --run;
        return word - run->firstWord < run->numberOfWords ? run->bits() : 0;
    };
    auto putBackSlot = [&]<typename Slot>(Slot) {
        at<Slot>(offset) = static_cast<Slot>(initialWordAt(offset / sizeof(uint64_t)) >> (offset % sizeof(uint64_t) * 8));
        offset += sizeof(Slot);
    };
    auto refreshMirrors = makeScopeExit([&] {
        refreshViewMirrors();
    });
    for (uint32_t end = offset + size; offset < end;) {
        uint32_t left = end - offset;
        if (!(offset % sizeof(uint64_t)) && left >= sizeof(uint64_t))
            putBackSlot(uint64_t());
        else if (!(offset % sizeof(uint32_t)) && left >= sizeof(uint32_t))
            putBackSlot(uint32_t());
        else if (!(offset % sizeof(uint16_t)) && left >= sizeof(uint16_t))
            putBackSlot(uint16_t());
        else
            putBackSlot(uint8_t());
    }
}

std::span<ProgramStatics::VariableWithViewMirror> ProgramStatics::viewMirrorVariables(const StaticsRange& range)
{
    return { reinterpret_cast<VariableWithViewMirror*>(m_instance.statics() + range.offset), range.size / sizeof(VariableWithViewMirror) };
}

void ProgramStatics::refreshViewMirrors()
{
    bool mirrorAlone = true;
    for (const StaticsRange& range : m_tables.variablesWithViewMirror()) {
        std::span<VariableWithViewMirror> views = viewMirrorVariables(range);
        if (!std::exchange(mirrorAlone, false)) {
            VariableWithViewMirror::refreshViewsOfOneBuffer(views);
            continue;
        }
        for (VariableWithViewMirror& mirrored : views)
            mirrored.refresh();
    }
}

void ProgramStatics::VariableWithViewMirror::refresh()
{
    JSValue held = JSValue::decode(variable);
    auto* view = held && held.isCell() ? dynamicDowncast<JSArrayBufferView>(held.asCell()) : nullptr;
    bool hasFixedLength = view && isTypedView(typedArrayType(view->type())) && !isResizableOrGrowableShared(view->mode());
    vector = hasFixedLength ? view->vector() : nullptr;
    length = hasFixedLength ? std::min<uint64_t>(view->lengthRaw(), (1ull << 31) >> logElementSize(typedArrayType(view->type()))) : 0;
}

void ProgramStatics::VariableWithViewMirror::refreshViewsOfOneBuffer(std::span<VariableWithViewMirror> views)
{
    auto logElementSizeOf = [](const VariableWithViewMirror& mirrored) {
        return logElementSize(typedArrayType(JSValue::decode(mirrored.variable).asCell()->type()));
    };
    uint64_t size = UINT64_MAX;
    bool haveOneVector = true;
    for (VariableWithViewMirror& mirrored : views) {
        mirrored.refresh();
        haveOneVector &= mirrored.length && mirrored.vector == views.front().vector;
        if (haveOneVector)
            size = std::min(size, mirrored.length << logElementSizeOf(mirrored));
    }
    for (VariableWithViewMirror& mirrored : views)
        mirrored.lengthOfBuffer = haveOneVector ? size >> logElementSizeOf(mirrored) : 0;
}

void ProgramStatics::didWriteVariable(uint32_t offset)
{
    std::span<const StaticsRange> ranges = m_tables.variablesWithViewMirror();
    auto range = std::ranges::find_if(ranges, [&](const StaticsRange& range) {
        return range.contains(offset);
    });
    if (range == ranges.begin() && range != ranges.end())
        at<VariableWithViewMirror>(offset).refresh();
    else if (range != ranges.end())
        VariableWithViewMirror::refreshViewsOfOneBuffer(viewMirrorVariables(*range));
}

void ProgramStatics::didMoveStorageOf(JSCell* view)
{
    bool mirrorAlone = true;
    for (const StaticsRange& range : m_tables.variablesWithViewMirror()) {
        std::span<VariableWithViewMirror> views = viewMirrorVariables(range);
        bool areOfOneBuffer = !std::exchange(mirrorAlone, false);
        bool holdsView = false;
        for (VariableWithViewMirror& mirrored : views) {
            if (mirrored.variable != JSValue::encode(view))
                continue;
            holdsView = true;
            if (!areOfOneBuffer)
                mirrored.refresh();
        }
        if (areOfOneBuffer && holdsView)
            VariableWithViewMirror::refreshViewsOfOneBuffer(views);
    }
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
    if (JSValue value = readSlot(slot.representation(), instance.statics() + offset, instance))
        return value;
    if (std::optional<FunctionID> function = findFunctionVariable(statics.m_tables, offset)) {
        JSCell* closure = closures.closure(vm, *function);
        write(instance, slot, closure);
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
    const uint8_t* address = instance.statics() + offset;
    if (JSValue value = readSlot(slot.representation(), address, instance))
        return value;
    if (std::optional<FunctionID> function = findFunctionVariable(statics.m_tables, offset))
        return closures.closureIfCreated(*function);
    return { };
}

bool ProgramStatics::isVariableEmpty(const Instance& instance, SlotData slot)
{
    if (slot.representation() == SlotRepresentation::DeferredClosure)
        return false;
    const ProgramStatics& statics = *instance.state->statics;
    const uint8_t* address = instance.statics() + slot.offset;
    if (readSlot(slot.representation(), address, instance))
        return false;
    return !findFunctionVariable(statics.m_tables, slot.offset);
}

void ProgramStatics::writeVariable(Instance& instance, SlotData slot, JSValue value)
{
    if (readVariableWithoutAllocating(instance, slot) != value)
        instance.state->countWriteForEffectsNet(Typed::CountInEffectsNet::WordsOfStaticsWritten);
    write(instance, slot, value);
}

void ProgramStatics::emptyVariableWhoseValueHasMoved(Instance& instance, SlotData slot)
{
    write(instance, slot, JSValue());
}

void ProgramStatics::write(Instance& instance, SlotData slot, JSValue value)
{
    SlotRepresentation representation = slot.representation();
    RELEASE_ASSERT(representation != SlotRepresentation::DeferredClosure);
    ProgramStatics& statics = *instance.state->statics;
    uint8_t* address = instance.statics() + slot.offset;
    if (!value) {
        RELEASE_ASSERT(traitsOf(representation).hasAbsent);
        clearSlot(representation, address);
        statics.didWriteVariable(slot.offset);
        return;
    }
    bool didWrite = tryWriteSlot(instance, representation, address, value);
    RELEASE_ASSERT(didWrite);
    statics.didWriteVariable(slot.offset);
    if (traitsOf(representation).holdsCell)
        statics.didStoreRootInStatics(slot.offset);
}

namespace {

class ModuleEnvironment final : public JSCell {
public:
    using Base = JSCell;
    static constexpr unsigned StructureFlags = Base::StructureFlags;
    static constexpr DestructionMode needsDestruction = DoesNotNeedDestruction;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm) { return &vm.cellSpace(); }

    static Structure* createStructure(VM& vm, JSGlobalObject* globalObject) { return Structure::create(vm, globalObject, jsNull(), TypeInfo(CellType, StructureFlags), info()); }

    static ModuleEnvironment* create(VM& vm, Structure* structure, const ProgramStatics& statics, uint32_t module)
    {
        auto* environment = new (NotNull, allocateCell<ModuleEnvironment>(vm)) ModuleEnvironment(vm, structure, statics, module);
        environment->finishCreation(vm);
        return environment;
    }

    DECLARE_INFO;

    static void analyzeHeap(JSCell* cell, HeapAnalyzer& analyzer)
    {
        Base::analyzeHeap(cell, analyzer);
        auto* environment = uncheckedDowncast<ModuleEnvironment>(cell);
        environment->m_statics.analyzeVariablesOfModule(environment, environment->m_module, analyzer);
    }

private:
    ModuleEnvironment(VM& vm, Structure* structure, const ProgramStatics& statics, uint32_t module)
        : Base(vm, structure)
        , m_statics(statics)
        , m_module(module)
    {
    }

    const ProgramStatics& m_statics;
    uint32_t m_module;
};

const ClassInfo ModuleEnvironment::s_info = { "JSModuleEnvironment"_s, nullptr, nullptr, nullptr, CREATE_METHOD_TABLE(ModuleEnvironment) };

} // anonymous namespace

bool HeapHooks::representsModuleInHeapSnapshot(JSCell* cell)
{
    return cell->classInfo() == ModuleEnvironment::info();
}

std::span<const VariableData> ProgramStatics::namedVariablesOfModule(uint32_t module) const
{
    std::span<const uint32_t> first = m_firstNamedVariablesOfModules.span();
    return m_namedVariables.span().subspan(first[module], first[module + 1] - first[module]);
}

JSCell* ProgramStatics::cellIn(SlotData slot) const
{
    const ClosureTable& closures = m_instance.state->objects->closures;
    if (slot.representation() == SlotRepresentation::DeferredClosure)
        return closures.closureIfCreated(FunctionID(slot.offset));
    const uint8_t* word = m_instance.statics() + slot.offset;
    if (readSlotBits(slot.representation(), word))
        return cellInSlot(slot.representation(), word, m_instance.cellCageBase);
    std::optional<FunctionID> function = findFunctionVariable(m_tables, slot.offset);
    return function ? closures.closureIfCreated(*function) : nullptr;
}

void ProgramStatics::willAnalyzeHeap(VM& vm)
{
    if (m_tables.namedVariables().empty())
        return;
    m_scopesOfClosures.variablesShown.clear();
    if (m_firstNamedVariablesOfModules.isEmpty()) {
        m_placesOfModulesOfClosureNumbers.fill(UINT32_MAX, m_tables.closureFunctions.end - m_tables.closureFunctions.first);
        bool endsWhereModuleEnds = forEachNamedVariable(decodeVarintBytes(m_tables.namedVariables()).span(), [&](uint32_t module, size_t) {
            m_modulesInHeapSnapshots.append(module);
            m_firstNamedVariablesOfModules.append(static_cast<uint32_t>(m_namedVariables.size()));
        }, [&](const VariableData& variable) {
            m_namedVariables.append(variable);
        }, [&](uint32_t first, uint32_t length) {
            std::ranges::fill(checkedSubspan(m_placesOfModulesOfClosureNumbers.mutableSpan(), first, length), static_cast<uint32_t>(m_modulesInHeapSnapshots.size() - 1));
        });
        RELEASE_ASSERT(endsWhereModuleEnds);
        m_firstNamedVariablesOfModules.append(static_cast<uint32_t>(m_namedVariables.size()));
        ProgramIdentifiers& identifiers = *vm.aotState().identifiers;
        for (const VariableData& variable : m_namedVariables)
            identifiers.nameForHeapSnapshots(&variable, AtomString(ProgramStrings::text(m_tables, StringID(variable.name))));
    }
    size_t numberOfModules = m_firstNamedVariablesOfModules.size() - 1;
    MarkedBlockCage::InfallibleAllocationScope infallibleAllocation(vm.heap.cage());
    ImmortalCells& immortalCells = *m_instance.state->immortalCells;
    if (!m_structureOfModuleEnvironments) {
        m_structureOfModuleEnvironments = ModuleEnvironment::createStructure(vm, m_instance.globalObject);
        immortalCells.add(m_structureOfModuleEnvironments);
        m_environmentsOfModules.fill(nullptr, numberOfModules);
    }
    for (uint32_t module = 0; module < numberOfModules; ++module) {
        if (m_environmentsOfModules[module])
            continue;
        const Modules& modules = *m_instance.state->modules;
        ModuleID id(m_modulesInHeapSnapshots[module]);
        bool hasBegun = modules.status(id) >= ModuleStatus::Evaluating || (modules.status(id) == ModuleStatus::Linked && !modules.data(id).needsEvaluation());
        bool hasSomethingToList = hasBegun || std::ranges::any_of(namedVariablesOfModule(module), [&](const VariableData& variable) {
            return !!cellIn(variable.slot);
        });
        if (!hasSomethingToList)
            continue;
        m_environmentsOfModules[module] = ModuleEnvironment::create(vm, m_structureOfModuleEnvironments, *this, module);
        immortalCells.add(m_environmentsOfModules[module]);
    }

    if (!m_structureOfScopesOfClosures) {
        m_structureOfScopesOfClosures = createStructureOfScopesInHeapSnapshots(vm, m_instance.globalObject);
        immortalCells.add(m_structureOfScopesOfClosures);
    }
    DeferGCForAWhile deferGC(vm);
    Vector<std::pair<JSCell*, JSCell*>>& scopes = m_scopesOfClosures.list;
    scopes.shrink(0);
    {
        HeapIterationScope iterationScope(vm.heap);
        vm.heap.aotClosureSpace<SubspaceAccess::OnMainThread>()->forEachLiveCell([&](HeapCell* cell, HeapCell::Kind) {
            SUPPRESS_MEMORY_UNSAFE_CAST auto* closure = static_cast<JSCell*>(cell);
            if (closure->structure()->realm() == m_instance.globalObject && hasScopeInHeapSnapshots(closure))
                scopes.append({ closure, nullptr });
        });
    }
    std::ranges::sort(scopes);
    for (auto& [closure, scope] : scopes)
        scope = createScopeInHeapSnapshot(vm, m_structureOfScopesOfClosures, closure);
}

void ProgramStatics::ScopesOfClosures::clear()
{
    if (vm->isAnalyzingHeap())
        return;
    list.clear();
    variablesShown.clear();
}

JSCell* ProgramStatics::scopeInHeapSnapshotOf(JSCell* closure) const
{
    std::span<const std::pair<JSCell*, JSCell*>> scopes = m_scopesOfClosures.list.span();
    auto found = std::ranges::lower_bound(scopes, closure, { }, &std::pair<JSCell*, JSCell*>::first);
    return found != scopes.end() && found->first == closure ? found->second : nullptr;
}

JSCell* ProgramStatics::environmentOfModuleThatDefines(uint32_t closureNumber) const
{
    if (closureNumber >= m_placesOfModulesOfClosureNumbers.size())
        return nullptr;
    uint32_t place = m_placesOfModulesOfClosureNumbers[closureNumber];
    return place < m_environmentsOfModules.size() ? m_environmentsOfModules[place] : nullptr;
}

JSCell* ProgramStatics::environmentOfModule(uint32_t module) const
{
    std::span<const uint32_t> modules = m_modulesInHeapSnapshots.span();
    size_t place = std::ranges::lower_bound(modules, module) - modules.begin();
    return place < modules.size() && modules[place] == module && place < m_environmentsOfModules.size() ? m_environmentsOfModules[place] : nullptr;
}

void ProgramStatics::analyzeVariableOfModule(JSCell* standIn, JSCell* held, UniquedStringImpl* name, HeapAnalyzer& analyzer) const
{
    if (m_scopesOfClosures.variablesShown.add({ standIn, held, name }).isNewEntry)
        analyzer.analyzeVariableNameEdge(standIn, held, name);
}

void ProgramStatics::analyzeVariablesOfModule(JSCell* standIn, uint32_t module, HeapAnalyzer& analyzer) const
{
    analyzer.analyzeEdge(m_instance.globalObject->moduleLoader(), standIn, RootMarkReason::None);
    const ProgramIdentifiers& identifiers = *m_instance.vm->aotState().identifiers;
    for (const VariableData& variable : namedVariablesOfModule(module)) {
        JSCell* held = cellIn(variable.slot);
        UniquedStringImpl* name = held ? identifiers.nameInHeapSnapshots(&variable) : nullptr;
        if (name)
            analyzeVariableOfModule(standIn, held, name, analyzer);
    }
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
    forEachRootInRegion<EncodedJSValue>(statics, m_tables.staticsRoots(), begin, end, [&](EncodedJSValue value) ALWAYS_INLINE_LAMBDA {
        appender.appendUnbarriered(JSValue::decode(value));
    });
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
    , m_slots({ state.instance.statics(), state.tables.instanceSizes.sizeOfStatics }, InstanceLayout::cardShift, { state.instance.cards(), RememberedSlotSet::numberOfCardsFor(state.tables.instanceSizes.sizeOfStatics, InstanceLayout::cardShift) })
{
    m_slots.setCardsAreWrittenWithoutRecord();
    uint8_t* statics = m_instance.statics();
    auto* words = reinterpret_cast<uint64_t*>(statics);
    for (const StaticsRun& run : m_tables.staticsRuns())
        std::ranges::fill(std::span { words + run.firstWord, run.numberOfWords }, run.bits());
    state.caches->add(CacheKind::StrongRoots, "statics"_s, *this, RootVolatility::GreyedByExecution);
    m_scopesOfClosures.vm = m_instance.vm;
    state.caches->add(CacheKind::ClearedEachCollection, "scopes of closures in a heap snapshot"_s, m_scopesOfClosures);
    for (const StaticsRange& range : m_tables.staticsClearedEachCollection())
        state.caches->addClearedAndGivenBackEachCollection("statics cleared each collection"_s, { statics + range.offset, range.size });
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
