/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRuntime.h"
#include "CompilerHooks.h"

#include "AOTBuiltins.h"
#include "AOTGraph.h"
#include <bmalloc/StaticRegion.h>
#include <sys/mman.h>

#include "ArrayConstructor.h"
#include "ArrayPrototype.h"
#include "MapPrototype.h"
#include "SetPrototype.h"
#include "StringPrototype.h"

#include "DeferTermination.h"
#include "FrameTracers.h"
#include "FunctionCodeBlock.h"
#include "JSTemplateObjectDescriptor.h"
#include "JSWebAssemblyInstance.h"
#include "ParserError.h"

#if ENABLE(FTL_JIT)

#include "AOTProgram.h"
#include "AOTImage.h"
#include "StaticHeap.h"
#include "AOTOperations.h"
#include "AOTThunks.h"
#include "CCallHelpers.h"
#include "CallLinkInfo.h"
#include "CodeBlock.h"
#include "DFGOperations.h"
#include "JITOperations.h"
#include "JITThunks.h"
#include "JSCInlines.h"
#include "MathObject.h"
#include "LLIntData.h"
#include "LLIntEntrypoint.h"
#include "LLIntSlowPaths.h"
#include "LLIntThunks.h"
#include "LinkBuffer.h"
#include "ObjectConstructorInlines.h"
#include "ThunkGenerators.h"
#include <wtf/TZoneMallocInlines.h>

#if OS(DARWIN)
#include <execinfo.h>
#endif

namespace JSC { namespace AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(RuntimeTable);
WTF_MAKE_STRUCT_TZONE_ALLOCATED_IMPL(Data);
WTF_MAKE_STRUCT_TZONE_ALLOCATED_IMPL(VirtualCallInfo);

void* addressOfStub(Stub stub)
{
    const void* inImage = Image::addressOfStub(stub);
    RELEASE_ASSERT_WITH_MESSAGE(inImage, "No AOT image with code is registered.");
    return const_cast<void*>(inImage);
}

void* catchThunk()
{
    return tagCodePtr<ExceptionHandlerPtrTag>(addressOfStub(Stub::Catch));
}

void* nearCallTargetFor(void* code)
{
    if (isJITPC(code))
        return code;
    // Like the LLInt's entry points, code in an image is out of range of a near call from JIT memory, so the call goes through a
    // thunk.
    static Lock lock;
    static NeverDestroyed<UncheckedKeyHashMap<void*, MacroAssemblerCodeRef<JSEntryPtrTag>>> thunks;
    Locker locker { lock };
    auto result = thunks.get().add(code, MacroAssemblerCodeRef<JSEntryPtrTag>());
    if (result.isNewEntry) {
        CCallHelpers jit;
        jit.move(CCallHelpers::TrustedImmPtr(code), GPRInfo::nonArgGPR0);
        jit.farJump(GPRInfo::nonArgGPR0, JSEntryPtrTag);
        LinkBuffer patchBuffer(jit, GLOBAL_THUNK_ID, LinkBuffer::Profile::Thunk);
        result.iterator->value = FINALIZE_THUNK(patchBuffer, JSEntryPtrTag, "AOTFarJump"_s, "Jump to code in an image");
    }
    return result.iterator->value.code().untaggedPtr();
}

extern "C" void* g_aotStaticFunctionEntrypoints[2]; // FunctionExecutable.cpp

RuntimeTable::RuntimeTable(VM& vm)
{
    using namespace DFG; // FOR_EACH_AOT_OPERATION_OF_THE_OTHER_TIERS
#define AOT_FILL_OPERATION(name) \
    m_entries[static_cast<unsigned>(Entry::name)] = tagCFunctionPtr<void*, OperationPtrTag>(name);
    FOR_EACH_AOT_OPERATION(AOT_FILL_OPERATION)
#undef AOT_FILL_OPERATION

    auto set = [&](Entry entry, void* pointer) {
        m_entries[static_cast<unsigned>(entry)] = pointer;
    };
    set(Entry::HandleException, tagCodePtr<JITThunkPtrTag>(addressOfStub(Stub::HandleException)));
    set(Entry::ThrowStackOverflowAtPrologue, tagCodePtr<JITThunkPtrTag>(addressOfStub(Stub::ThrowStackOverflowAtPrologue)));
    set(Entry::VirtualCall, tagCodePtr<JITThunkPtrTag>(addressOfStub(Stub::VirtualCall)));
    set(Entry::VirtualConstruct, tagCodePtr<JITThunkPtrTag>(addressOfStub(Stub::VirtualConstruct)));
    set(Entry::VirtualTailCall, tagCodePtr<JITThunkPtrTag>(addressOfStub(Stub::VirtualTailCall)));
    set(Entry::LookupExceptionHandler, tagCFunctionPtr<void*, OperationPtrTag>(operationLookupExceptionHandler));
    set(Entry::ThrowStackOverflowError, tagCFunctionPtr<void*, OperationPtrTag>(operationAOTThrowStackOverflowError));
    set(Entry::NativeCallTrampoline, LLInt::getCodePtr<JSEntryPtrTag>(llint_native_call_trampoline).taggedPtr());
    // A FunctionExecutable in the short form has no room for its entry points, so they are read from here:
    // ExecutableBase::entrypointOfShortForm().
    set(Entry::EnterStaticFunctionForCall, tagCodePtr<JSEntryPtrTag>(addressOfStub(Stub::EnterStaticFunctionForCall)));
    set(Entry::EnterStaticFunctionForConstruct, tagCodePtr<JSEntryPtrTag>(addressOfStub(Stub::EnterStaticFunctionForConstruct)));
    g_aotStaticFunctionEntrypoints[0] = m_entries[static_cast<unsigned>(Entry::EnterStaticFunctionForCall)];
    g_aotStaticFunctionEntrypoints[1] = m_entries[static_cast<unsigned>(Entry::EnterStaticFunctionForConstruct)];

    auto addCallLinkInfo = [&](Entry entry, CallLinkInfo::CallType type) {
        auto info = makeUnique<VirtualCallInfo>();
        static_assert(!OBJECT_OFFSETOF(VirtualCallInfo, callLinkInfo));
        info->callLinkInfo.initialize(vm, nullptr, type, CodeOrigin { });
        info->callLinkInfo.setVirtualCall(vm);
        info->findTarget = tagCFunctionPtr<void*, OperationPtrTag>(findCallTarget);
        info->lookupExceptionHandler = tagCFunctionPtr<void*, OperationPtrTag>(operationLookupExceptionHandler);
        set(entry, &info->callLinkInfo);
        m_callLinkInfos.append(WTF::move(info));
    };
    addCallLinkInfo(Entry::CallLinkInfoForCall, CallLinkInfo::Call);
    addCallLinkInfo(Entry::CallLinkInfoForConstruct, CallLinkInfo::Construct);
    addCallLinkInfo(Entry::CallLinkInfoForTailCall, CallLinkInfo::TailCall);
    set(Entry::StructureIDBase, std::bit_cast<void*>(structureIDBase()));
    for (unsigned slot = 0; slot < Structure::numberOfSlotsWithFieldIDs; ++slot)
        set(static_cast<Entry>(static_cast<unsigned>(Entry::LayoutIDsOfFieldsInSlot0) + slot), const_cast<uint16_t*>(TypedLayoutTable::layoutIDsOfFieldsInSlot(slot)));
    auto setHostFunction = [&](Entry entry, NativeFunction::Ptr function) {
        set(entry, TaggedNativeFunction(function).taggedPtr());
    };
    setHostFunction(Entry::HostMathSqrt, mathProtoFuncSqrt);
    setHostFunction(Entry::HostMathAbs, mathProtoFuncAbs);
    setHostFunction(Entry::HostMathFloor, mathProtoFuncFloor);
    setHostFunction(Entry::HostMathCeil, mathProtoFuncCeil);
    setHostFunction(Entry::HostMathTrunc, mathProtoFuncTrunc);
    setHostFunction(Entry::HostMathFround, mathProtoFuncFround);
    setHostFunction(Entry::HostMathMin, mathProtoFuncMin);
    setHostFunction(Entry::HostMathMax, mathProtoFuncMax);
    setHostFunction(Entry::HostMathIMul, mathProtoFuncIMul);
    setHostFunction(Entry::HostStringCharCodeAt, stringProtoFuncCharCodeAt);
    setHostFunction(Entry::HostArrayPush, arrayProtoFuncPush);
    setHostFunction(Entry::HostStringCodePointAt, stringProtoFuncCodePointAt);
    setHostFunction(Entry::HostStringCharAt, stringProtoFuncCharAt);
    setHostFunction(Entry::HostArrayPop, arrayProtoFuncPop);
    setHostFunction(Entry::HostArrayIsArray, arrayConstructorIsArray);
    setHostFunction(Entry::HostMapGet, mapProtoFuncGet);
    setHostFunction(Entry::HostMapHas, mapProtoFuncHas);
    setHostFunction(Entry::HostMapSet, mapProtoFuncSet);
    setHostFunction(Entry::HostSetHas, setProtoFuncHas);
    setHostFunction(Entry::HostSetAdd, setProtoFuncAdd);

    installOperationFrontEnds(vm, m_entries);

    // (With the JIT, bound functions use the thunk from boundFunctionCallGenerator().)
    if (usesStubs && !Options::useJIT())
        vm.getBoundFunction(true, SourceTaintedOrigin::Untainted)->setCallEntrypoint(CodePtr<JSEntryPtrTag>::fromTaggedPtr(tagCodePtr<JSEntryPtrTag>(addressOfStub(Stub::CallBoundFunction))));
}

RuntimeTable::~RuntimeTable() = default;

RuntimeTable& runtimeTable(VM& vm)
{
    if (!vm.m_aotRuntimeTable)
        vm.m_aotRuntimeTable = makeUnique<RuntimeTable>(vm);
    return *vm.m_aotRuntimeTable;
}

struct Instance::Collections {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Collections);
    Vector<Data*> all;
    Vector<Data*> filledSinceLastCollection;
    bool hasFieldAdditions { false }; // See Instance::fieldAdditions.
    // Functions linked without a Data, which would otherwise mark these. Their FunctionInfo points into the UnlinkedCodeBlock, which the
    // executable does not keep alive: an UnlinkedFunctionExecutable drops code that has aged (UnlinkedCodeBlock::maxAge).
    struct FunctionWithoutData {
        ScriptExecutable* executable;
        UnlinkedCodeBlock* unlinkedCodeBlock;
    };
    Vector<FunctionWithoutData> functionsWithoutData;
    // The slots that cache, or have cached, a structure transition. The collector revisits them repeatedly while marking. There are
    // few.
    Vector<Slot*> transitions;
    Vector<Slot*> transitionsSinceLastCollection;
    Vector<PolymorphicSlots*> slotsOfSites;
    UncheckedKeyHashMap<String, Structure*> shapes; // Keyed by inline capacity and the addresses of the names. Null: no such structure exists.
    UncheckedKeyHashMap<uint32_t, Structure*> knownShapes; // Keyed by shape number. Holds the ones created so far.
    // For Instance::tryCopySlotsForSpread(): the Structure of a copy of an object with a given Structure. Null: the slots cannot be
    // copied. (Both structures are kept alive.)
    UncheckedKeyHashMap<Structure*, Structure*> structuresOfCopies;
    // For Instance::adopt(): the Structure that an object with a given Structure gets when it is converted to a typed layout. Null:
    // it cannot be converted. (Both structures are kept alive.)
    UncheckedKeyHashMap<std::pair<Structure*, uint16_t>, Structure*> convertedStructures;
    // For each conversion that is possible: which property offset moves to which slot, and which field of the typed layout it
    // becomes (null: none).
    struct LayoutConversionPlan {
        Vector<std::pair<PropertyOffset, uint16_t>> moves;
        Vector<const TypedLayoutTable::Field*> fields;
    };
    UncheckedKeyHashMap<std::pair<Structure*, uint16_t>, LayoutConversionPlan> conversionPlans;
    // Conversions that were rejected because of the Structure. (The Structure is not kept alive. If another Structure is later
    // allocated at the same address, its objects only take the slow path.)
    UncheckedKeyHashMap<std::pair<Structure*, uint16_t>, ASCIILiteral> rejectedConversions; // The value is the reason.
    UncheckedKeyHashMap<uint32_t, Structure*> emptyStructures;
    size_t environmentsSize { 0 }; // Rounded up to whole pages.
    size_t sizeFromInstance { 0 };
    size_t sizeOfInfos { 0 }; // Zero unless the Instance owns them.
    size_t numberOfFunctions { 0 };
    // For Instance::allocateForData(). Offsets from the Instance, in units of 16 bytes.
    size_t startOfDatas { 0 };
    size_t endOfDatasUsed { 0 };
    size_t endOfDatas { 0 };
    Vector<std::pair<size_t, size_t>> freeAmongDatas; // Offset and size, sorted by offset.
};

static_assert(Instance::offsetOfVM() == 16, "JSWebAssemblyInstance::offsetOfVM()");

static_assert(!(sizeof(Data) % sizeof(uint64_t)) && OBJECT_OFFSETOF(Data, slots) == sizeof(Data));
static constexpr auto s_sharedData = [] {
    std::array<uint64_t, SharedData::size / sizeof(uint64_t)> words { };
    static_assert(!OBJECT_OFFSETOF(Slot, structureID) && OBJECT_OFFSETOF(Slot, offset) == 4 && sizeof(Slot) == 16);
    for (size_t i = sizeof(Data) / sizeof(uint64_t); i < words.size(); i += 2)
        words[i] = static_cast<uint64_t>(Slot::attemptsMask) << 32;
    return words;
}();

Data* SharedData::get()
{
    return std::bit_cast<Data*>(s_sharedData.data());
}

Instance& Instance::ensure(JSGlobalObject* globalObject)
{
    if (Instance* instance = globalObject->aotInstance())
        return *instance;
#if ENABLE(WEBASSEMBLY)
    RELEASE_ASSERT(Instance::offsetOfVM() == JSWebAssemblyInstance::offsetOfVM());
#endif
    VM& vm = globalObject->vm();
    RELEASE_ASSERT(vm.useImmutableIntrinsics);
    Instance* instance;
    size_t environmentsSize = 0;
    size_t size;
    size_t numberOfFunctions;
    // (Only address space is reserved. Pages are committed when they are first touched.)
    constexpr size_t roomForDatas = 256 * MB;
    auto startOfDatasFor = [](size_t numberOfFunctions) {
        return std::max<size_t>(roundUpToMultipleOf(WTF::pageSize(), sizeof(Instance) + numberOfFunctions * sizeof(uint32_t)), static_cast<size_t>(leastStateWithData) << shiftOfStateWithData);
    };
    auto sizeFor = [&](size_t numberOfFunctions) { return startOfDatasFor(numberOfFunctions) + roomForDatas; };
    if (Image::environmentsSize() && !vm.m_aotInstanceOfProgram && StaticHeap::canPlaceCellsOf(vm)) {
        // The environments are cells that the collector did not allocate, so they have to be in a block of the static heap.
        environmentsSize = roundUpToMultipleOf(WTF::pageSize(), Image::environmentsSize());
        numberOfFunctions = Image::numberOfFunctionsOfImageWithEnvironments();
        size = sizeFor(numberOfFunctions);
        instance = reinterpret_cast<Instance*>(static_cast<char*>(StaticHeap::allocateBlock(vm, environmentsSize + size)) + environmentsSize);
    } else {
        numberOfFunctions = maxFunctions;
        size = sizeFor(numberOfFunctions);
        instance = static_cast<Instance*>(OSAllocator::reserveAndCommit(size, OSAllocator::FastMallocPages));
    }
    instance->runtimeTable = AOT::runtimeTable(vm).entries();
    instance->globalObject = globalObject;
    instance->vm = &vm;
    instance->collections = new Collections;
    instance->collections->environmentsSize = environmentsSize;
    instance->collections->sizeFromInstance = size;
    instance->collections->numberOfFunctions = numberOfFunctions;
    instance->collections->startOfDatas = startOfDatasFor(numberOfFunctions) >> shiftOfStateWithData;
    instance->collections->endOfDatasUsed = instance->collections->startOfDatas;
    instance->collections->endOfDatas = size >> shiftOfStateWithData;
    instance->infos = environmentsSize ? StaticHeap::infosOfFunctions(vm) : nullptr;
    if (!instance->infos) {
        instance->collections->sizeOfInfos = roundUpToMultipleOf(WTF::pageSize(), numberOfFunctions * sizeof(FunctionInfo));
        instance->infos = static_cast<FunctionInfo*>(OSAllocator::reserveAndCommit(instance->collections->sizeOfInfos, OSAllocator::FastMallocPages));
    }
    instance->functionMetadataOffsets = environmentsSize ? StaticHeap::functionMetadataOffsets(vm) : nullptr;
    instance->constantsOfProgram = environmentsSize ? StaticHeap::constantsOfProgram(vm) : nullptr;
    instance->sharedData = SharedData::get();
    // (Pages are committed when they are first touched.)
    instance->fieldsWithObservableReads = static_cast<uint8_t*>(OSAllocator::reserveAndCommit(sizeOfFieldsWithObservableReads, OSAllocator::FastMallocPages));
    if (Image* image = Image::withCode()) {
        instance->code = static_cast<const uint8_t*>(image->code());
        instance->granulesOfCode = image->at<uint32_t>(image->header().granulesOfCodeOffset);
        instance->startsOfFunctionsAfterFirst = image->at<uint32_t>(image->header().startsOfFunctionsOffset) + 1;
    }
    instance->missesForEightSlots = Options::aotCacheMissesPerEightSlotsBeforeOwnData();
    instance->missesToSpare = Options::aotExtraCacheMissesBeforeOwnData();
    instance->structureIDBase = JSC::structureIDBase();
    RELEASE_ASSERT_WITH_MESSAGE(!Image::withCode() || instance->structureIDBase == structureIDBaseOfImages, "Structures are not where the program's code takes them to be: the addresses were taken.");
    {
        auto idOf = [](Structure* structure) { return structure->id().bits(); };
        auto ofReceiver = [&](Receiver receiver) -> uint32_t& { return instance->structureIDsOfReceivers[static_cast<unsigned>(receiver)]; };
        ofReceiver(Receiver::Map) = idOf(globalObject->mapStructure());
        ofReceiver(Receiver::Set) = idOf(globalObject->setStructure());
        ofReceiver(Receiver::WeakMap) = idOf(globalObject->weakMapStructure());
        ofReceiver(Receiver::WeakSet) = idOf(globalObject->weakSetStructure());
        ofReceiver(Receiver::RegExp) = idOf(globalObject->regExpStructure());
        ofReceiver(Receiver::Date) = idOf(globalObject->dateStructure());
        for (IndexingType type : { ArrayWithUndecided, ArrayWithInt32, ArrayWithDouble, ArrayWithContiguous, ArrayWithArrayStorage, CopyOnWriteArrayWithInt32, CopyOnWriteArrayWithDouble, CopyOnWriteArrayWithContiguous })
            instance->structureIDsOfOriginalArrays[(type & (IndexingShapeMask | CopyOnWrite)) >> Instance::shiftOfKindOfArray] = idOf(globalObject->originalArrayStructureForIndexingType(type));
        if (!globalObject->isHavingABadTime()) {
            instance->structureIDOfNewArrayWithInt32 = idOf(globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithInt32));
            instance->structureIDOfNewArrayWithContiguous = idOf(globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous));
            instance->structureIDsOfNewCopyOnWriteArrays[0] = idOf(globalObject->arrayStructureForIndexingTypeDuringAllocation(CopyOnWriteArrayWithInt32));
            instance->structureIDsOfNewCopyOnWriteArrays[1] = idOf(globalObject->arrayStructureForIndexingTypeDuringAllocation(CopyOnWriteArrayWithDouble));
            instance->structureIDsOfNewCopyOnWriteArrays[2] = idOf(globalObject->arrayStructureForIndexingTypeDuringAllocation(CopyOnWriteArrayWithContiguous));
        }
        // (The JIT tiers have to be notified of the first and second activation created for each SymbolTable. Inline allocation
        // skips that, so it is only enabled without the JIT.)
        if (!Options::useJIT())
            instance->structureIDOfActivation = idOf(globalObject->activationStructure());
        instance->auxiliarySpace = &vm.auxiliarySpace();
        instance->spaceOfActivations = subspaceFor<JSLexicalEnvironment>(vm);
        instance->allocatorOfArrays = subspaceFor<JSArray>(vm)->allocatorFor(sizeof(JSArray), AllocatorForMode::EnsureAllocator).localAllocator();
        instance->allocatorOfRopeStrings = subspaceFor<JSRopeString>(vm)->allocatorFor(sizeof(JSRopeString), AllocatorForMode::EnsureAllocator).localAllocator();
        instance->singleCharacterStrings = vm.smallStrings.singleCharacterStrings();
        instance->emptyString = vm.smallStrings.emptyString();
        instance->sentinelString = vm.smallStrings.sentinelString();
        instance->sentinelOfArrayIteration = vm.fastArrayUnboxedSentinel();
        instance->structureIDOfStrings = idOf(vm.stringStructure.get());
    }
    // Link-time constants, which only builtins can refer to. The image uses a handful, so they are initialized eagerly, and
    // compiled code reads them without a check.
    if (Image* image = Image::withCode()) {
        MonotonicTime before = MonotonicTime::now();
        unsigned count = 0;
        for (unsigned which = 0; which < numberOfLinkTimeConstants; ++which) {
            if (!(image->header().linkTimeConstantsUsed[which / 64] >> which % 64 & 1))
                continue;
            instance->linkTimeConstants[which] = JSValue::encode(globalObject->linkTimeConstant(static_cast<LinkTimeConstant>(which)));
            ++count;
        }
        if (Options::verboseAOTCompilation()) [[unlikely]]
            dataLogLn("AOT: ", count, " link-time constants made ready in ", (MonotonicTime::now() - before).microseconds(), " us");
    }
    memcpySpan(std::span { instance->intrinsics }, globalObject->immutableIntrinsics());
    // (putDirect() ignores the read-only attribute, so check that nothing has replaced an intrinsic on the global object.)
    for (unsigned number = 1; number < globalObject->immutableIntrinsics().size(); ++number) {
        const ImmutableIntrinsics::Entry& entry = ImmutableIntrinsics::shared()->at(number);
        if (entry.holder != ImmutableIntrinsics::globalObject)
            break;
        RELEASE_ASSERT(JSValue::encode(globalObject->getDirect(vm, Identifier::fromString(vm, entry.name))) == instance->intrinsics[number]);
    }
    if (Image* image = Image::withShapes()) {
        instance->dispatch = image->at<uint32_t>(image->header().dispatchOffset);
        instance->rowsOfSelectors = image->at<uint32_t>(image->header().rowsOfSelectorsOffset);
        RELEASE_ASSERT(!image->header().hashOfIntrinsics || image->header().hashOfIntrinsics == ImmutableIntrinsics::shared()->hash());
        instance->objectPrototype = globalObject->objectPrototype();
        if (JSValue call = globalObject->linkTimeConstant(LinkTimeConstant::callFunction); call.isCell()) {
            instance->functionPrototypeCall = call.asCell();
            instance->structureIDOfBoundFunctions = globalObject->boundFunctionStructure()->id().bits();
        }
        instance->selectorsOnObjectPrototype = static_cast<uint8_t*>(fastZeroedMalloc(image->header().numberOfSelectors / 8 + 1));
    }
    globalObject->setAOTInstance(instance);
    vm.m_aotInstances.append(instance);
    if (environmentsSize) {
        RELEASE_ASSERT(!vm.m_aotInstanceOfProgram);
        // (StaticHeap::engineBuiltinFor() assumed that the first realm is the program's.)
        RELEASE_ASSERT(!vm.m_firstRealmHasBuiltinsOfStaticHeap || vm.m_firstRealm == globalObject);
        vm.m_aotInstanceOfProgram = instance;
    }
    return *instance;
}

void* Instance::allocateForData(size_t size)
{
    size_t units = roundUpToMultipleOf<1 << shiftOfStateWithData>(size) >> shiftOfStateWithData;
    auto at = [&](size_t where) { return std::bit_cast<void*>(std::bit_cast<uintptr_t>(this) + (where << shiftOfStateWithData)); };
    auto& free = collections->freeAmongDatas;
    for (unsigned i = 0; i < free.size(); ++i) {
        if (free[i].second < units)
            continue;
        size_t where = free[i].first;
        if (free[i].second == units)
            free.removeAt(i);
        else {
            free[i].first += units;
            free[i].second -= units;
        }
        memset(at(where), 0, units << shiftOfStateWithData);
        return at(where);
    }
    size_t where = collections->endOfDatasUsed;
    RELEASE_ASSERT(units <= collections->endOfDatas - where);
    collections->endOfDatasUsed += units;
    return at(where);
}

void Instance::freeOfData(void* pointer, size_t size)
{
    size_t units = roundUpToMultipleOf<1 << shiftOfStateWithData>(size) >> shiftOfStateWithData;
    size_t where = (std::bit_cast<uintptr_t>(pointer) - std::bit_cast<uintptr_t>(this)) >> shiftOfStateWithData;
    auto& free = collections->freeAmongDatas;
    unsigned i = 0;
    while (i < free.size() && free[i].first < where)
        ++i;
    free.insert(i, std::pair<size_t, size_t> { where, units });
    if (i + 1 < free.size() && free[i].first + free[i].second == free[i + 1].first) {
        free[i].second += free[i + 1].second;
        free.removeAt(i + 1);
    }
    if (i && free[i - 1].first + free[i - 1].second == free[i].first) {
        free[i - 1].second += free[i].second;
        free.removeAt(i);
    }
}

void Instance::destroy(Instance* instance)
{
    instance->vm->m_aotInstances.removeFirst(instance);
    if (instance->vm->m_aotInstanceOfProgram == instance)
        instance->vm->m_aotInstanceOfProgram = nullptr;
    while (!instance->collections->all.isEmpty())
        Data::destroy(instance->collections->all.last());
    size_t environmentsSize = instance->collections->environmentsSize;
    size_t size = instance->collections->sizeFromInstance;
    if (instance->collections->sizeOfInfos)
        OSAllocator::decommitAndRelease(instance->infos, instance->collections->sizeOfInfos);
    delete instance->collections;
    OSAllocator::decommitAndRelease(instance->fieldsWithObservableReads, sizeOfFieldsWithObservableReads);
    fastFree(instance->selectorsOnObjectPrototype);
    if (environmentsSize)
        StaticHeap::freeBlock(reinterpret_cast<char*>(instance) - environmentsSize, environmentsSize + size);
    else
        OSAllocator::decommitAndRelease(instance, size);
}

void* Instance::placeForEnvironment(ImageEnvironment environment) const
{
    if (!environment.distance || environment.distance > collections->environmentsSize)
        return nullptr;
    return const_cast<char*>(reinterpret_cast<const char*>(this)) - environment.distance;
}

SUPPRESS_ASAN void* returnAddressForFrame(const void* frame, const void* startingFrom)
{
    struct Record {
        const Record* previous;
        void* returnAddress;
    };
    for (auto* record = static_cast<const Record*>(startingFrom); record && record < frame; record = record->previous) {
        if (record->previous == frame)
            return removeCodePtrTag(record->returnAddress);
    }
    return nullptr;
}

namespace {
struct FrameRecord {
    const FrameRecord* previous;
    void* returnAddress;
};
}

SUPPRESS_ASAN std::optional<FrameAndPC> innermostFrame(void* machineFrame, void* machinePC, void* machineLinkRegister, void* topCallFrame, const StackBounds& stack)
{
    // (The thread may have been stopped anywhere, so the frame pointer may be anything.)
    auto isValid = [&](const FrameRecord* record) {
        return stack.contains(const_cast<FrameRecord*>(record)) && !(std::bit_cast<uintptr_t>(record) % sizeof(void*));
    };
    auto* record = static_cast<const FrameRecord*>(machineFrame);
    // A function that calls nothing makes no frame, and no function has made one yet when it begins. Then the frame is still the
    // caller's, and only the link register says where the caller will resume.
    if (machineLinkRegister && machineFrame != topCallFrame && isValid(record) && removeCodePtrTag(record->returnAddress) != machineLinkRegister
        && classifyAddress(machineLinkRegister).kind != ImageAddressInfo::NotInImage)
        return FrameAndPC { machineFrame, machineLinkRegister };
    void* pc = machinePC;
    while (isValid(record)) {
        if (record == topCallFrame)
            return FrameAndPC { topCallFrame, pc };
        pc = removeCodePtrTag(record->returnAddress);
        if (classifyAddress(pc).kind != ImageAddressInfo::NotInImage)
            return FrameAndPC { const_cast<FrameRecord*>(record->previous), pc };
        if (record->previous <= record)
            break;
        record = record->previous;
    }
    return std::nullopt;
}

SUPPRESS_ASAN Instance* instanceForFrame(const void* frame)
{
    for (auto* record = static_cast<const FrameRecord*>(frame);; record = record->previous) {
        ImageAddressInfo::Kind kind = classifyAddress(removeCodePtrTag(record->returnAddress)).kind;
        RELEASE_ASSERT(kind != ImageAddressInfo::NotInImage);
        if (kind == ImageAddressInfo::Adapter)
            return *reinterpret_cast<Instance* const*>(reinterpret_cast<const char*>(record->previous) + offsetOfInstanceInAdapter);
    }
}

SUPPRESS_ASAN bool canFindInstanceForFrame(const void* frame)
{
    for (auto* record = static_cast<const FrameRecord*>(frame); record; record = record->previous) {
        ImageAddressInfo::Kind kind = classifyAddress(removeCodePtrTag(record->returnAddress)).kind;
        if (kind == ImageAddressInfo::NotInImage)
            return false;
        if (kind == ImageAddressInfo::Adapter)
            return true;
    }
    return false;
}

NEVER_INLINE bool topCallFrameIsAOTFrame(const void* frame)
{
    if (!hasCode())
        return false;
    void* returnAddress = returnAddressForFrame(frame, __builtin_frame_address(0));
    return returnAddress && classifyAddress(returnAddress).kind != ImageAddressInfo::NotInImage;
}

SUPPRESS_ASAN FunctionRef callerFunction(const CallFrame* callFrame)
{
    if (!hasCode())
        return { };
    for (auto* record = reinterpret_cast<const FrameRecord*>(callFrame);; record = record->previous) {
        ImageAddressInfo what = classifyAddress(removeCodePtrTag(record->returnAddress));
        if (what.kind == ImageAddressInfo::Function) {
            FunctionRef function { instanceForFrame(record->previous), what.index };
            // (The return address may be in code that was inlined from another function.)
            if (function.info().function()->hasInlineFrames) [[unlikely]]
                return function.locationForReturnAddress(removeCodePtrTag(record->returnAddress)).function;
            return function;
        }
        if (what.kind != ImageAddressInfo::Stub)
            return { };
    }
}

CodeBlock* codeBlockOfCaller(const CallFrame* callFrame)
{
    FunctionRef function = callerFunction(callFrame);
    return function ? function.ensureCodeBlock() : nullptr;
}

const RegisterAtOffsetList& adapterSavedRegisters()
{
    static LazyNeverDestroyed<RegisterAtOffsetList> list;
    static std::once_flag once;
    std::call_once(once, [] {
        RegisterSet registers;
        registers.add(instanceGPR, IgnoreVectors);
        registers.add(GPRInfo::numberTagRegister, IgnoreVectors);
        registers.add(GPRInfo::notCellMaskRegister, IgnoreVectors);
        list.construct(registers);
        // (Registers are saved in increasing order of register number.)
        list->adjustOffsets(offsetOfInstanceRegisterInAdapter - list->find(instanceGPR)->offset());
        RELEASE_ASSERT(list->find(GPRInfo::numberTagRegister)->offset() == offsetOfNumberTagRegisterInAdapter && list->find(GPRInfo::notCellMaskRegister)->offset() == offsetOfNotCellMaskRegisterInAdapter);
    });
    return list.get();
}

bool hasOnlyRealmIndependentConstants(UnlinkedCodeBlock* unlinkedCodeBlock, SymbolTablesAreShared symbolTablesAreShared)
{
    auto& constants = unlinkedCodeBlock->constantRegisters();
    auto& representations = unlinkedCodeBlock->constantsSourceCodeRepresentation();
    for (unsigned i = 0; i < constants.size(); ++i) {
        // (Compiled code reads link-time constants from the Instance: NodeKind::LinkTimeConstant.)
        if (representations[i] == SourceCodeRepresentation::LinkTimeConstant)
            continue;
        JSValue constant = constants[i].get();
        if (!constant || !constant.isCell())
            continue;
        if (auto* symbolTable = dynamicDowncast<SymbolTable>(constant.asCell())) {
            if (symbolTablesAreShared == SymbolTablesAreShared::No && !symbolTable->isSharedAcrossRealms())
                return false;
        } else if (constant.asCell()->inherits<JSTemplateObjectDescriptor>())
            return false;
    }
    return true;
}

// The constants of the unlinked code can be used as they are, except for the ones that depend on the realm.
static bool linkConstants(VM& vm, Data& data)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!data.unlinkedCodeBlock) {
        // This was determined when the program was built.
        const FunctionInfo& info = data.function().info();
        if (info.hasOnlyRealmIndependentConstants()) {
            data.constants = info.constants;
            return true;
        }
        const uint32_t* list = StaticHeap::inData<uint32_t>(*data.function().metadata()->find(FunctionMetadata::RealmConstants));
        std::span constants { static_cast<const WriteBarrier<Unknown>*>(info.constants), list[0] };
        JSGlobalObject* globalObject = data.instance->globalObject;
        auto* copy = static_cast<WriteBarrier<Unknown>*>(fastZeroedMalloc(constants.size_bytes()));
        data.constants = copy;
        data.ownsConstants = true;
        data.numberOfOwnConstants = constants.size();
        for (unsigned i = 0; i < constants.size(); ++i) {
            JSValue constant = constants[i].get();
            if (constant && constant.isCell()) {
                if (auto* descriptor = dynamicDowncast<JSTemplateObjectDescriptor>(constant.asCell())) {
                    constant = data.executable->topLevelExecutable()->createTemplateObject(globalObject, descriptor);
                    RETURN_IF_EXCEPTION(scope, false);
                }
            }
            copy[i].setWithoutWriteBarrier(constant);
        }
        for (uint32_t i : std::span { list + 2, list[1] })
            copy[i].setWithoutWriteBarrier(globalObject->linkTimeConstant(static_cast<LinkTimeConstant>(constants[i].get().asInt32AsAnyInt())));
        return true;
    }
    UnlinkedCodeBlock* unlinkedCodeBlock = data.unlinkedCodeBlock;
    auto& constants = unlinkedCodeBlock->constantRegisters();
    auto& representations = unlinkedCodeBlock->constantsSourceCodeRepresentation();
    bool hasRealmDependentConstants = false;
    for (unsigned i = 0; i < constants.size(); ++i) {
        if (representations[i] == SourceCodeRepresentation::LinkTimeConstant) {
            hasRealmDependentConstants = true;
            continue;
        }
        JSValue constant = constants[i].get();
        if (!constant || !constant.isCell())
            continue;
        if (auto* symbolTable = dynamicDowncast<SymbolTable>(constant.asCell())) {
            // A SymbolTable that is not shared has to be cloned for the JIT tiers, which CodeBlock::setConstantRegisters() does.
            if (!symbolTable->isSharedAcrossRealms()) {
                data.constants = data.ensureCodeBlock()->constantRegisters().span().data();
                return true;
            }
        } else if (constant.asCell()->inherits<JSTemplateObjectDescriptor>())
            hasRealmDependentConstants = true;
    }
    if (!hasRealmDependentConstants) {
        data.constants = constants.span().data();
        return true;
    }

    JSGlobalObject* globalObject = data.instance->globalObject;
    auto* copy = static_cast<WriteBarrier<Unknown>*>(fastZeroedMalloc(constants.size() * sizeof(WriteBarrier<Unknown>)));
    data.constants = copy;
    data.ownsConstants = true;
    data.numberOfOwnConstants = constants.size();
    for (unsigned i = 0; i < constants.size(); ++i) {
        JSValue constant = constants[i].get();
        if (representations[i] == SourceCodeRepresentation::LinkTimeConstant)
            constant = globalObject->linkTimeConstant(static_cast<LinkTimeConstant>(constant.asInt32AsAnyInt()));
        else if (constant && constant.isCell()) {
            if (auto* descriptor = dynamicDowncast<JSTemplateObjectDescriptor>(constant.asCell())) {
                constant = data.executable->topLevelExecutable()->createTemplateObject(globalObject, descriptor);
                RETURN_IF_EXCEPTION(scope, false);
            }
        }
        copy[i].setWithoutWriteBarrier(constant);
    }
    return true;
}

static void fillInfo(FunctionInfo& info, ScriptExecutable* executable, UnlinkedCodeBlock* unlinkedCodeBlock, JITCode& code, const void* constants)
{
    info.constants = constants;
    info.identifiers = unlinkedCodeBlock->identifiers().span().data();
    info.sites = code.sites();
    info.setExecutable(executable, unlinkedCodeBlock->isConstructor() && unlinkedCodeBlock->codeType() == FunctionCode ? CodeSpecializationKind::CodeForConstruct : CodeSpecializationKind::CodeForCall, false);
    info.flags = (code.imageFunction()->hasSiteConstants ? FunctionInfo::hasSiteConstants : FunctionInfo::sitesHaveTheirConstants) | FunctionInfo::slotsAmongFlags(code.numSlots());
}

Data* Data::create(Instance& instance, ScriptExecutable* executable, UnlinkedCodeBlock* unlinkedCodeBlock, JITCode& code, CodeBlock* codeBlock)
{
    VM& vm = *instance.vm;
    DeferGCForAWhile deferGC(vm);
    unsigned numSlots = code.numSlots();
    Data* data = static_cast<Data*>(instance.allocateForData(sizeof(Data) + numSlots * sizeof(Slot)));
    data->codeBlock = codeBlock;
    data->instance = &instance;
    data->executable = executable;
    data->unlinkedCodeBlock = unlinkedCodeBlock;
    code.ref();
    data->code = &code;
    // (With a static heap, use the identifiers from the FunctionInfo. The unlinked code, if it exists, may have been decoded later,
    // and has its own copy of the same identifiers.)
    FunctionInfo& info = instance.infos[code.index()];
    data->identifiers = info.sites ? info.identifiers : unlinkedCodeBlock->identifiers().span().data();
    data->sites = code.sites();
    data->hasSiteConstants = code.imageFunction()->hasSiteConstants;
    data->numSlots = numSlots;
    data->slotEpoch = 1;

    RELEASE_ASSERT(code.index() < instance.collections->numberOfFunctions);
    RELEASE_ASSERT(!instance.dataIfExists(code.index()));
    instance.setData(code.index(), data);
    data->indexAmongAll = instance.collections->all.size();
    instance.collections->all.append(data);
    data->noteFilled(); // New, so the next collection has to visit it.

    if (codeBlock)
        data->constants = codeBlock->constantRegisters().span().data();
    else if (!linkConstants(vm, *data)) {
        destroy(data);
        return nullptr;
    }
    if (!info.sites) {
        if (!instance.collections->sizeOfInfos && Options::verboseAOTCompilation()) [[unlikely]]
            dataLogLn("AOT: nothing was known of function ", code.index(), " when the program was built");
        fillInfo(info, executable, unlinkedCodeBlock, code, data->constants);
    }
    RELEASE_ASSERT(info.identifiers == data->identifiers && info.sites == data->sites && (info.flags >> FunctionInfo::numberOfFlagBits) == std::min<uint32_t>(numSlots, FunctionInfo::maxEncodedSlots) && (!info.executable() || info.executable() == executable));
    return data;
}

Data* Instance::ensureData(uint32_t index)
{
    // (A function that starts cold and has only been called directly has no Data yet.)
    Data* data = dataIfExists(index);
    if (data)
        return data;
    const FunctionInfo& info = infos[index];
    auto* executable = uncheckedDowncast<FunctionExecutable>(info.executable());
    UnlinkedFunctionCodeBlock* unlinkedCodeBlock = executable->unlinkedExecutable()->codeBlockIfExists(info.kind());
    // (An executable in the static heap is read-only, so it cannot hold a reference to its JITCode.)
    Ref<JITCode> code = executable->hasJITCodeFor(info.kind()) ? Ref { static_cast<JITCode&>(executable->generatedJITCodeFor(info.kind()).get()) } : codeOfFunctionFromImage({ &Image::of(*info.function()), info.function() }, info.kind());
    RELEASE_ASSERT(code->index() == index);
    code->setInstance(*this);
    data = Data::create(*this, executable, unlinkedCodeBlock, code.get());
    RELEASE_ASSERT(data); // Cannot fail, because everything it links to already exists.
    return data;
}

FunctionRef FunctionRef::at(Instance* instance, const void* address)
{
    auto& asked = instance->cachedAddressInfo(address);
    if (asked.address == address) [[likely]]
        return { instance, asked.function };
    ImageAddressInfo what = classifyAddress(address);
    RELEASE_ASSERT(what.kind == ImageAddressInfo::Function);
    asked = { address, what.index, Instance::CachedAddressInfo::siteNotLookedFor };
    return { instance, what.index };
}

CallSiteOverride::CallSiteOverride(Instance& instance, const void* returnAddress, uint32_t site)
    : m_instance(instance)
{
    RELEASE_ASSERT(!instance.overriddenReturnAddress);
    instance.overriddenReturnAddress = returnAddress;
    instance.overridingSite = site;
}

CallSiteOverride::~CallSiteOverride()
{
    m_instance.overriddenReturnAddress = nullptr;
}

static FunctionRef::Location locationForSite(FunctionRef function, uint32_t site)
{
    const ImageFunction& record = *function.info().function();
    if (!record.hasInlineFrames) [[likely]]
        return { function, CallSiteIndex(site).bytecodeIndex(), 0 };
    unsigned frame = PackedSite::inlineFrame(site);
    BytecodeIndex bytecodeIndex = CallSiteIndex(PackedSite::bits(site)).bytecodeIndex();
    if (!frame)
        return { function, bytecodeIndex, 0 };
    return { FunctionRef { function.instance, inlineFrameOf(record, frame).function }, bytecodeIndex, frame, false, PackedSite::isOfTailCall(site) };
}

FunctionRef::Location FunctionRef::locationForReturnAddress(const void* returnAddress) const
{
    if (returnAddress == instance->overriddenReturnAddress) [[unlikely]]
        return locationForSite(*this, instance->overridingSite);
    using Asked = Instance::CachedAddressInfo;
    auto& asked = instance->cachedAddressInfo(returnAddress);
    if (asked.address != returnAddress || asked.site == Asked::siteNotLookedFor) [[unlikely]] {
        ImageAddressInfo what = classifyAddress(returnAddress);
        RELEASE_ASSERT(what.kind == ImageAddressInfo::Function && what.index == index);
        auto site = tryCallSiteAt(*info().function(), what.offset);
        RELEASE_ASSERT(!site || *site < Asked::hasNoSite);
        asked = { returnAddress, index, site.value_or(Asked::hasNoSite) };
    }
    ASSERT(asked.function == index);
    // Not every call has a recorded site: the ones whose callee neither throws nor inspects the stack do not. But the stack can be
    // walked at any time, for example by an allocation profiler. In that case, report the function without a position.
    if (asked.site == Asked::hasNoSite) [[unlikely]]
        return { *this, BytecodeIndex(), 0 };
    return locationForSite(*this, asked.site);
}

FunctionRef::Location FunctionRef::inlineCallSiteLocation(unsigned inlineFrame) const
{
    ImageInlineFrame frame = inlineFrameOf(*info().function(), inlineFrame);
    Location location = locationForSite(*this, PackedSite::pack(frame.parent, frame.callSite));
    location.isTailDeleted = frame.isTailCall;
    return location;
}

BytecodeIndex FunctionRef::bytecodeIndexAt(const void* returnAddress) const
{
    return locationForReturnAddress(returnAddress).bytecodeIndex;
}

FunctionRef FunctionRef::of(VM& vm, FunctionExecutable* executable, CodeSpecializationKind kind)
{
    if (StaticHeap::contains(executable) && executable->aotIndexFor(kind) != FunctionExecutable::aotIndexOfWhatConstructsByCalling)
        return { vm.m_aotInstanceOfProgram, executable->aotIndexFor(kind) };
    if (!executable->hasJITCodeFor(kind) || executable->generatedJITCodeFor(kind)->jitType() != JITType::AOTJIT)
        return { };
    Ref generated = executable->generatedJITCodeFor(kind);
    auto& code = static_cast<JITCode&>(generated.get());
    return { code.instance(), code.index() };
}

CodeBlock* FunctionRef::ensureCodeBlock() const
{
    return ensureData()->ensureCodeBlock();
}

Data* FunctionRef::dataIfExists() const
{
    return instance->dataIfExists(index);
}

Data* FunctionRef::ensureData() const
{
    return instance->ensureData(index);
}

ScriptExecutable* FunctionRef::executable() const
{
    // (The executable of a module's top-level code is created at run time, so the FunctionInfo cannot refer to it.)
    if (Data* data = dataIfExists())
        return data->executable;
    return info().executable();
}

CodeBlock* FunctionRef::codeBlockIfExists() const
{
    Data* data = dataIfExists();
    return data ? data->codeBlock : nullptr;
}

uint32_t FunctionRef::siteConstantOf(const Slot* slot) const
{
    const FunctionInfo& info = this->info();
    size_t which = slot - (SharedData::contains(slot) ? instance->sharedData : instance->dataIfExists(index))->slots;
    if (info.flags & FunctionInfo::sitesHaveTheirConstants)
        return info.sites[which].identifierAndExtra;
    if (!(info.flags & FunctionInfo::hasSiteConstants))
        return 0;
    return info.function()->siteConstants()[which];
}

std::optional<std::pair<String, bool>> FunctionRef::quoteAt(BytecodeIndex bytecodeIndex) const
{
    const ImageFunction* function = info().function();
    if (!function)
        return std::nullopt;
    return Image::of(*function).quoteAt(*function, bytecodeIndex.offset());
}

FunctionRef FunctionRef::of(CodeBlock* codeBlock)
{
    // (Not through its Data, which a CodeBlock that will not run again has released: CodeBlock::releaseAOTData().)
    if (codeBlock->jitType() != JITType::AOTJIT)
        return { };
    RefPtr generated = codeBlock->jitCode();
    auto* code = static_cast<JITCode*>(generated.get());
    return code->instance() ? FunctionRef { code->instance(), code->index() } : FunctionRef { };
}

bool FunctionRef::constructsAt(BytecodeIndex bytecodeIndex) const
{
    const ImageFunction* function = info().function();
    return function && Image::of(*function).constructsAt(*function, bytecodeIndex.offset());
}

AllocationPlan FunctionRef::planOf(const Slot* firstOfSite) const
{
    uint32_t constant = siteConstantOf(firstOfSite + 1);
    if (!constant)
        return { };
    return { info().function()->plans() + constant - 1 };
}

UnlinkedCodeBlock* FunctionRef::unlinkedCodeBlockIfExists() const
{
    if (Data* data = dataIfExists(); data && data->unlinkedCodeBlock)
        return data->unlinkedCodeBlock;
    return uncheckedDowncast<FunctionExecutable>(executable())->unlinkedExecutable()->codeBlockIfExists(info().kind());
}

// A buffer of zeros as large as any function's instruction stream. Its pages are only committed if they are read.
static std::span<const uint8_t> zerosForInstructions(size_t size)
{
    static constexpr size_t most = static_cast<size_t>(1) << (32 - FunctionMetadata::shiftOfInstructionsSize);
    static const uint8_t* zeros;
    static std::once_flag once;
    std::call_once(once, [] {
        void* result = mmap(nullptr, most, PROT_READ, MAP_PRIVATE | MAP_ANON, -1, 0);
        RELEASE_ASSERT(result != MAP_FAILED);
        zeros = static_cast<const uint8_t*>(result);
    });
    RELEASE_ASSERT(size <= most);
    return { zeros, size };
}

// The result is never interpreted, and its users do not read the instructions.
UnlinkedCodeBlock* FunctionRef::makeUnlinkedCodeBlockFromMetadata() const
{
    auto* metadata = this->metadata();
    const uint32_t* scalars = metadata ? metadata->find(FunctionMetadata::Scalars) : nullptr;
    if (!scalars)
        return nullptr;
    PartsOfFunctionCode parts { };
    parts.scalars = StaticHeap::inData<uint8_t>(*scalars);
    parts.instructions = zerosForInstructions(metadata->instructionsSize());
    // (When the program has one identifier table, compiled code refers to a name by its index in that table, and the function's own
    // identifier list is not recorded. As with the instructions, users of the result do not read it.)
    parts.identifiers = StaticHeap::hasIdentifiersOfProgram() ? nullptr : static_cast<const Identifier*>(info().identifiers);
    parts.constants = static_cast<const WriteBarrier<Unknown>*>(info().constants);
    if (const uint32_t* word = metadata->find(FunctionMetadata::RealmConstants)) {
        const uint32_t* list = StaticHeap::inData<uint32_t>(*word);
        parts.linkTimeConstants = { list + 2, list[1] };
    }
    // (The nested functions are not needed either. They are looked up through functionDecl() and functionExpr().)
    if (const uint32_t* words = metadata->find(FunctionMetadata::Handlers))
        parts.handlers = { StaticHeap::inData<UnlinkedHandlerInfo>(words[0]), words[1] };
    if (const uint32_t* word = metadata->find(FunctionMetadata::ExpressionInfo); word && !StaticHeap::hasPositionsOfCallSites())
        parts.expressionInfo = StaticHeap::inData<uint8_t>(*word);
    return makeFunctionCodeFromParts(*instance->vm, parts);
}

UnlinkedCodeBlock* FunctionRef::ensureUnlinkedCodeBlock() const
{
    if (UnlinkedCodeBlock* existing = unlinkedCodeBlockIfExists())
        return existing;
    VM& vm = *instance->vm;
    DeferGCForAWhile deferGC(vm);
    DeferTerminationForAWhile deferTermination(vm);
    SuspendExceptionScope suspendExceptions(vm);
    // The result is stored in the Data. (The executable is in the static heap. Storing one word to it would dirty a whole
    // copy-on-write page.)
    Data* data = ensureData();
    UnlinkedCodeBlock* result = makeUnlinkedCodeBlockFromMetadata();
    if (!result)
        result = uncheckedDowncast<FunctionExecutable>(data->executable)->unlinkedExecutable()->decodeCodeFromKeptPayload(vm, info().kind(), instance->globalObject);
    RELEASE_ASSERT(result);
    data->unlinkedCodeBlock = result;
    if (!data->hasBeenFilledSinceLastCollection)
        data->noteFilled();
    return result;
}

const FunctionMetadata* FunctionRef::metadata() const
{
    if (!instance->functionMetadataOffsets)
        return nullptr;
    uint32_t at = instance->functionMetadataOffsets[index];
    // (For odd values, see reportedPositionFor().)
    return at && !(at & 1) ? StaticHeap::inData<FunctionMetadata>(at) : nullptr;
}

static uint64_t readVarint(const uint8_t*& at)
{
    uint64_t value = 0;
    for (unsigned shift = 0;; shift += 7) {
        uint8_t byte = *at++;
        value |= static_cast<uint64_t>(byte & 0x7f) << shift;
        if (!(byte & 0x80))
            return value;
    }
}

// Decodes what makePositions() in StaticHeap.cpp wrote.
auto FunctionRef::reportedPositionFor(BytecodeIndex bytecodeIndex, OfConstruction ofConstruction) const -> std::optional<ReportedPosition>
{
    if (!instance->functionMetadataOffsets || !StaticHeap::hasPositionsOfCallSites())
        return std::nullopt;
    const uint8_t* at = nullptr;
    if (uint32_t word = instance->functionMetadataOffsets[index]; word & 1)
        at = StaticHeap::inData<uint8_t>(word - 1);
    else if (auto* metadata = this->metadata()) {
        if (const uint32_t* where = metadata->find(FunctionMetadata::ExpressionInfo))
            at = StaticHeap::inData<uint8_t>(*where);
    }
    if (!at)
        return std::nullopt;
    // Start with the position of the function itself, which is the fallback. (A builtin has no other positions.)
    ReportedPosition result;
    result.lineColumn.line = static_cast<unsigned>(readVarint(at));
    result.lineColumn.column = static_cast<unsigned>(readVarint(at));
    uint64_t offset = 0;
    int64_t line = 0;
    int64_t column = 0;
    uint32_t source = 0;
    auto zigZagDecode = [](uint64_t value) {
        return static_cast<int64_t>(value >> 1) ^ -static_cast<int64_t>(value & 1);
    };
    auto readPosition = [&]() -> ReportedPosition {
        uint64_t word = readVarint(at);
        if (word & 1)
            column += zigZagDecode(word >> 1);
        else {
            line += zigZagDecode(word >> 2);
            if (word & 2)
                source = static_cast<uint32_t>(readVarint(at));
            column = static_cast<int64_t>(readVarint(at));
        }
        return { { static_cast<unsigned>(line), static_cast<unsigned>(column) }, source };
    };
    // Find the last entry that is not past the bytecode index. Every index that a frame can report has an entry, so this is an
    // exact match.
    for (uint64_t count = readVarint(at); count--;) {
        uint64_t word = readVarint(at);
        offset += word >> 1;
        if (offset > bytecodeIndex.offset())
            break;
        result = readPosition();
        if (word & 1) {
            ReportedPosition start = readPosition();
            if (ofConstruction == OfConstruction::WhereItStarts)
                result = start;
        }
    }
    return result;
}

CodeType FunctionRef::codeType() const
{
    return metadata() ? FunctionCode : unlinkedCodeBlockIfExists()->codeType();
}

bool FunctionRef::isBuiltinFunction() const
{
    if (auto* metadata = this->metadata())
        return metadata->flagsAndInstructionsSize & FunctionMetadata::isBuiltinFunction;
    return unlinkedCodeBlockIfExists()->isBuiltinFunction();
}

unsigned FunctionRef::instructionsSize() const
{
    if (auto* metadata = this->metadata())
        return metadata->instructionsSize();
    return unlinkedCodeBlockIfExists()->instructions().size();
}

void* FunctionRef::addressOfCatchEntrypoint(unsigned bytecodeOffset) const
{
    const ImageFunction& function = *info().function();
    for (unsigned i = 0; i < function.numberOfCatchEntrypoints; ++i) {
        if (function.catchEntrypoints()[i].bytecodeOffset == bytecodeOffset)
            return const_cast<uint8_t*>(Image::of(function).codeFor(function)) + function.catchEntrypoints()[i].codeOffset;
    }
    return nullptr;
}

const UnlinkedHandlerInfo* FunctionRef::handlerFor(unsigned bytecodeOffset) const
{
    auto* metadata = this->metadata();
    if (!metadata)
        return unlinkedCodeBlockIfExists()->handlerForIndex(bytecodeOffset, RequiredHandler::AnyHandler);
    const uint32_t* words = metadata->find(FunctionMetadata::Handlers);
    if (!words)
        return nullptr;
    std::span<const UnlinkedHandlerInfo> handlers { StaticHeap::inData<UnlinkedHandlerInfo>(words[0]), words[1] };
    return UnlinkedHandlerInfo::handlerForIndex<const UnlinkedHandlerInfo>(handlers, bytecodeOffset, RequiredHandler::AnyHandler);
}

const UnlinkedStringJumpTable& FunctionRef::stringSwitchJumpTable(unsigned tableIndex) const
{
    auto* metadata = this->metadata();
    if (!metadata)
        return unlinkedCodeBlockIfExists()->unlinkedStringSwitchJumpTable(tableIndex);
    return StaticHeap::inMalloc<UnlinkedStringJumpTable>(*metadata->find(FunctionMetadata::StringSwitchJumpTables))[tableIndex];
}

const IdentifierSet& FunctionRef::constantIdentifierSet(unsigned index) const
{
    auto* metadata = this->metadata();
    if (!metadata)
        return ensureUnlinkedCodeBlock()->constantIdentifierSets()[index];
    return StaticHeap::inMalloc<IdentifierSet>(*metadata->find(FunctionMetadata::ConstantIdentifierSets))[index];
}

BytecodeIndex FunctionRef::resumePointOf(int32_t state) const
{
    if (state <= 0)
        return BytecodeIndex(0);
    int32_t offset = 0;
    if (auto* metadata = this->metadata()) {
        if (const uint32_t* word = metadata->find(FunctionMetadata::ResumePoints)) {
            const int32_t* table = StaticHeap::inData<int32_t>(*word);
            if (state >= table[0] && static_cast<uint32_t>(state - table[0]) < static_cast<uint32_t>(table[1]))
                offset = table[2 + state - table[0]];
        }
    } else if (UnlinkedCodeBlock* codeBlock = unlinkedCodeBlockIfExists(); codeBlock && codeBlock->numberOfUnlinkedSwitchJumpTables())
        offset = codeBlock->unlinkedSwitchJumpTable(codeBlock->numberOfUnlinkedSwitchJumpTables() - 1).offsetForValue(state);
    return BytecodeIndex(std::max(offset, 0));
}

static std::span<const WriteBarrier<UnlinkedFunctionExecutable>> functionsIn(const FunctionMetadata& metadata, FunctionMetadata::Section which)
{
    const uint32_t* words = metadata.find(which);
    if (!words)
        return { };
    return { StaticHeap::inMalloc<WriteBarrier<UnlinkedFunctionExecutable>>(words[0]), words[1] };
}

std::span<const WriteBarrier<UnlinkedFunctionExecutable>> FunctionRef::functionDecls() const
{
    if (auto* metadata = this->metadata())
        return functionsIn(*metadata, FunctionMetadata::FunctionDecls);
    return unlinkedCodeBlockIfExists()->functionDecls();
}

std::span<const WriteBarrier<UnlinkedFunctionExecutable>> FunctionRef::functionExprs() const
{
    if (auto* metadata = this->metadata())
        return functionsIn(*metadata, FunctionMetadata::FunctionExprs);
    return unlinkedCodeBlockIfExists()->functionExprs();
}

void Data::destroy(Data* data)
{
    Instance& instance = *data->instance;
    // (Invalidates MegamorphicCache::ConstructionEntry::m_site.)
    if (auto* cache = instance.vm->megamorphicCache())
        cache->bumpEpoch();
    RELEASE_ASSERT(instance.dataIfExists(data->code->index()) == data);
    instance.setNotLinked(data->code->index());
    auto isOfThis = [&](Slot* slot) { return slot >= data->slots && slot < data->slots + data->numSlots; };
    instance.collections->transitions.removeAllMatching(isOfThis);
    instance.collections->transitionsSinceLastCollection.removeAllMatching(isOfThis);
    auto removeFrom = [&](Vector<Data*>& list, unsigned Data::*index) {
        RELEASE_ASSERT(list[data->*index] == data);
        Data* last = list.takeLast();
        if (last != data) {
            list[data->*index] = last;
            last->*index = data->*index;
        }
    };
    removeFrom(instance.collections->all, &Data::indexAmongAll);
    if (data->hasBeenFilledSinceLastCollection)
        removeFrom(instance.collections->filledSinceLastCollection, &Data::indexAmongFilled);
    delete data->watchpoints;
    instance.collections->slotsOfSites.removeAllMatching([&](PolymorphicSlots* several) {
        if (several->owner != data)
            return false;
        fastFree(several);
        return true;
    });
    if (data->ownsConstants)
        fastFree(const_cast<void*>(data->constants));
    if (data->functions)
        fastFree(data->functions);
    data->code->deref();
    instance.freeOfData(data, sizeof(Data) + data->numSlots * sizeof(Slot));
}

FunctionRef Data::function() const
{
    return { instance, code->index() };
}

CodeBlock* Data::ensureCodeBlock()
{
    if (codeBlock)
        return codeBlock;
    VM& vm = *instance->vm;
    if (!unlinkedCodeBlock)
        function().ensureUnlinkedCodeBlock();
    RELEASE_ASSERT(unlinkedCodeBlock->codeType() == FunctionCode);
    DeferGCForAWhile deferGC(vm);
    // The caller may be handling an exception, so this must not observe a pending termination request.
    DeferTerminationForAWhile deferTermination(vm);
    SuspendExceptionScope suspendExceptions(vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    FunctionCodeBlock* result = FunctionCodeBlock::create(vm, uncheckedDowncast<FunctionExecutable>(executable), uncheckedDowncast<UnlinkedFunctionCodeBlock>(unlinkedCodeBlock), instance->globalObject, CodeBlock::LinkMode::ForCodeFromImage);
    scope.releaseAssertNoException();
    RELEASE_ASSERT(result);
    result->adoptAOTCode(*code, this);
    codeBlock = result;
    if (!hasBeenFilledSinceLastCollection)
        noteFilled();
    return result;
}

LineColumn FunctionRef::lineColumnFor(BytecodeIndex bytecodeIndex) const
{
    if (auto position = reportedPositionFor(bytecodeIndex))
        return position->lineColumn;
    ScriptExecutable* executable = this->executable();
    RELEASE_ASSERT(bytecodeIndex.offset() < instructionsSize());
    SourceProvider& provider = *executable->sourceProvider();
    unsigned sourceOffset = executable->source().startOffset();
    LineColumn inText = provider.lineColumnInTextForOffset(sourceOffset);
    if (auto* metadata = this->metadata()) {
        if (const uint32_t* word = metadata->find(FunctionMetadata::ExpressionInfo))
            inText = decodeBorrowedExpressionInfo(StaticHeap::inData<uint8_t>(*word))->lineColumnInTextForInstPC(bytecodeIndex.offset(), provider, sourceOffset);
    } else
        inText = unlinkedCodeBlockIfExists()->lineColumnInTextForBytecodeIndex(bytecodeIndex, provider, sourceOffset);
    return provider.documentLineColumn(inText);
}

static FunctionExecutable* functionOf(Data& data, unsigned index, const WriteBarrier<UnlinkedFunctionExecutable>& entry)
{
    if (FunctionExecutable* result = UnlinkedCodeBlock::executableIn(entry))
        return result;
    UnlinkedFunctionExecutable* unlinkedExecutable = entry.get();
    if (FunctionExecutable* result = unlinkedExecutable->staticExecutable(); result && StaticHeap::contains(data.executable))
        return result;
    if (!data.functions)
        data.functions = static_cast<FunctionExecutable**>(fastZeroedMalloc((data.function().functionDecls().size() + data.function().functionExprs().size()) * sizeof(FunctionExecutable*)));
    FunctionExecutable*& function = data.functions[index];
    if (!function) {
        ScriptExecutable* executable = data.executable;
        function = unlinkedExecutable->link(*data.instance->vm, executable->topLevelExecutable(), executable->source(), std::nullopt, NoIntrinsic, executable->isInsideOrdinaryFunction());
        if (!data.hasBeenFilledSinceLastCollection)
            data.noteFilled();
    }
    return function;
}

FunctionExecutable* Data::functionDecl(unsigned index)
{
    // A module's code shares its nested functions with the other tiers that run it.
    if (function().codeType() != FunctionCode)
        return codeBlock->functionDecl(index);
    return functionOf(*this, index, function().functionDecls()[index]);
}

FunctionExecutable* Data::functionExpr(unsigned index)
{
    if (function().codeType() != FunctionCode)
        return codeBlock->functionExpr(index);
    return functionOf(*this, function().functionDecls().size() + index, function().functionExprs()[index]);
}

FunctionExecutable* FunctionRef::functionDecl(unsigned index) const
{
    if (!dataIfExists()) {
        auto& entry = functionDecls()[index];
        if (FunctionExecutable* result = UnlinkedCodeBlock::executableIn(entry))
            return result;
        if (FunctionExecutable* result = entry->staticExecutable())
            return result;
    }
    return ensureData()->functionDecl(index);
}

FunctionExecutable* FunctionRef::functionExpr(unsigned index) const
{
    if (!dataIfExists()) {
        auto& entry = functionExprs()[index];
        if (FunctionExecutable* result = UnlinkedCodeBlock::executableIn(entry))
            return result;
        if (FunctionExecutable* result = entry->staticExecutable())
            return result;
    }
    return ensureData()->functionExpr(index);
}

bool install(VM& vm, FunctionExecutable* executable, CodeSpecializationKind kind, UnlinkedCodeBlock* unlinkedCodeBlock, JSGlobalObject* globalObject, Ref<JITCode>&& code)
{
    Instance& instance = Instance::ensure(globalObject);
    code->setInstance(instance);
    uint32_t index = code->index();
    if (instance.isLinked(index))
        RELEASE_ASSERT((FunctionRef { &instance, index }.executable() == executable));
    else if (code->imageFunction()->startsCold && !instance.infos[index].sites && hasOnlyRealmIndependentConstants(unlinkedCodeBlock)) {
        if (!instance.collections->sizeOfInfos && Options::verboseAOTCompilation()) [[unlikely]]
            dataLogLn("AOT: nothing was known of function ", index, " when the program was built");
        fillInfo(instance.infos[index], executable, unlinkedCodeBlock, code.get(), unlinkedCodeBlock->constantRegisters().span().data());
        instance.infos[index].flags |= FunctionInfo::startsCold;
        instance.collections->functionsWithoutData.append({ executable, unlinkedCodeBlock });
        instance.setLinkedWithoutData(index);
    } else if (!Data::create(instance, executable, unlinkedCodeBlock, code.get()))
        return false;
    executable->installAOTCode(vm, kind, WTF::move(code));
    return true;
}

bool linkStaticFunction(VM& vm, FunctionExecutable* executable, CodeSpecializationKind kind, JSScope* scope)
{
    Instance* instance = vm.m_aotInstanceOfProgram;
    if (!instance || scope->realm() != instance->globalObject)
        return false;
    uint32_t index = executable->aotIndexFor(kind);
    if (index == FunctionExecutable::aotIndexOfWhatConstructsByCalling)
        return true;
    if (instance->isLinked(index))
        return true;
    if (const FunctionInfo& info = instance->infos[index]; info.flags & FunctionInfo::startsCold) {
        RELEASE_ASSERT(info.executable() == executable && info.kind() == kind);
        if (info.function()->usesStaticImports && !moduleIsLinkedAsCompiled(scope))
            return false;
        instance->setLinkedWithoutData(index);
        return true;
    }
    UnlinkedFunctionCodeBlock* unlinkedCodeBlock = executable->unlinkedExecutable()->codeBlockIfExists(kind);
    ImageCode found = findInImage(executable, kind, unlinkedCodeBlock, scope);
    if (!found)
        return false;
    Ref<JITCode> code = codeOfFunctionFromImage(found, kind);
    RELEASE_ASSERT(code->index() == executable->aotIndexFor(kind));
    code->setInstance(*instance);
    return !!Data::create(*instance, executable, unlinkedCodeBlock, code.get());
}

void Data::noteFilled()
{
    hasBeenFilledSinceLastCollection = true;
    indexAmongFilled = instance->collections->filledSinceLastCollection.size();
    instance->collections->filledSinceLastCollection.append(this);
}

template<typename Visitor>
void Data::visit(Visitor& visitor)
{
    visitor.appendUnbarriered(executable);
    if (unlinkedCodeBlock)
        visitor.appendUnbarriered(unlinkedCodeBlock);
    if (codeBlock)
        visitor.appendUnbarriered(codeBlock);
    if (functions) {
        for (unsigned i = function().functionDecls().size() + function().functionExprs().size(); i--;) {
            if (functions[i])
                visitor.appendUnbarriered(functions[i]);
        }
    }
    if (ownsConstants) {
        auto* values = static_cast<const WriteBarrier<Unknown>*>(constants);
        for (unsigned i = numberOfOwnConstants; i--;)
            visitor.appendUnbarriered(values[i].get());
    }
}

static ALWAYS_INLINE bool hasTransition(const Slot& slot)
{
    // (A scope cache may have an untagged address there.)
    return slot.structureID && slot.newStructureID && (!slot.fieldType || (slot.offset & Slot::hasFieldType)) && !slot.hasPointer();
}

PolymorphicSlots* Instance::makeSlotsOfSite(Data* owner, UniquedStringImpl* name)
{
    auto* several = static_cast<PolymorphicSlots*>(fastZeroedMalloc(sizeof(PolymorphicSlots)));
    several->name = name;
    several->owner = owner;
    several->timesLeftToLearnAtOnce = PolymorphicSlots::timesToLearnAtOnce;
    several->byName = PolymorphicSlots::initialByName;
    several->timesLeftToTolerateTableThatCannotBeFilledIn = PolymorphicSlots::timesToTolerateTableThatCannotBeFilledIn;
    collections->slotsOfSites.append(several);
    return several;
}

void Instance::noteTransitionCached(Slot* slot)
{
    collections->transitionsSinceLastCollection.append(slot);
}

// With onlyNew, a Data that existed at the last collection and has not been filled since is skipped, because it refers to no young
// objects.
template<typename Visitor>
void Instance::visit(Visitor& visitor, bool onlyNew)
{
    for (Data* data : onlyNew ? collections->filledSinceLastCollection : collections->all)
        data->visit(visitor);
    // Code with a cached transition can move an object that was already visited to the new structure, so the new structure has to
    // be marked.
    auto visitTransitions = [&](const Vector<Slot*>& slots) {
        for (Slot* slot : slots) {
            if (hasTransition(*slot) && visitor.isMarked(slot->structureID.decode()))
                visitor.appendUnbarriered(slot->newStructureID.decode());
        }
    };
    visitTransitions(collections->transitionsSinceLastCollection);
    if (!onlyNew)
        visitTransitions(collections->transitions);
    for (Structure* structure : collections->shapes.values()) {
        if (structure)
            visitor.appendUnbarriered(structure);
    }
    for (Structure* structure : collections->knownShapes.values())
        visitor.appendUnbarriered(structure);
    for (auto& [from, to] : collections->structuresOfCopies) {
        visitor.appendUnbarriered(from);
        if (to)
            visitor.appendUnbarriered(to);
    }
    for (Structure* structure : collections->emptyStructures.values())
        visitor.appendUnbarriered(structure);
    for (auto& [from, to] : collections->convertedStructures) {
        visitor.appendUnbarriered(from.first);
        if (to)
            visitor.appendUnbarriered(to);
    }
    for (auto& function : collections->functionsWithoutData) {
        visitor.appendUnbarriered(function.executable);
        visitor.appendUnbarriered(function.unlinkedCodeBlock);
    }
}

Structure* Instance::structureOfKnownShape(uint32_t shape, std::span<UniquedStringImpl* const> names)
{
    if (auto it = collections->knownShapes.find(shape); it != collections->knownShapes.end())
        return it->value;
    Image* image = Image::withShapes();
    RELEASE_ASSERT(shape && shape < image->header().numberOfShapes);
    const ImageShape& description = image->at<ImageShape>(image->header().shapesOffset)[shape];
    RELEASE_ASSERT(names.size() == description.numberOfProperties);
    DeferGC deferGC(*vm);
    Structure* empty = globalObject->structureCache().emptyObjectStructureForPrototype(globalObject, globalObject->objectPrototype(), description.inlineCapacity);
    RELEASE_ASSERT(empty->inlineCapacity() == description.inlineCapacity);
    auto slots = slotsOfKnownShape(shape);
    Structure* result = slots.empty() ? Structure::createWithProperties(*vm, empty, names) : Structure::createWithProperties(*vm, empty, names, slots, description.reserved, description.layoutID ? description.inlineSlots : std::numeric_limits<unsigned>::max());
    RELEASE_ASSERT(result);
    result->setKnownShape(*vm, safeCast<uint16_t>(shape));
    if (description.hasIds) {
        uint16_t fieldIDInSlot[Structure::numberOfSlotsWithFieldIDs] { };
        const uint16_t* ids = slots.data() + slots.size();
        for (unsigned i = 0; i < slots.size(); ++i) {
            if (ids[i])
                fieldIDInSlot[slots[i]] = ids[i];
        }
        result->setTypedLayoutID(description.layoutID, fieldIDInSlot);
    } else if (TypedLayoutTable::hasTypedFields())
        result->setTypedLayoutID(description.layoutID);
    collections->knownShapes.add(shape, result);
    return result;
}

Structure* Instance::emptyStructureForLayout(uint16_t layoutID)
{
    if (auto it = collections->emptyStructures.find(layoutID); it != collections->emptyStructures.end())
        return it->value;
    unsigned capacity = TypedLayoutTable::numberOfSlots(layoutID);
    RELEASE_ASSERT(capacity);
    DeferGC deferGC(*vm);
    unsigned inlineSlots = TypedLayoutTable::inlineSlots(layoutID);
    Structure* empty = globalObject->structureCache().emptyObjectStructureForPrototype(globalObject, globalObject->objectPrototype(), KnownShape::inlineCapacityFor(inlineSlots));
    RELEASE_ASSERT(empty->inlineCapacity() >= inlineSlots);
    Structure* result = Structure::createWithProperties(*vm, empty, { }, std::span<const uint16_t> { }, capacity, inlineSlots);
    RELEASE_ASSERT(result);
    result->setTypedLayoutID(layoutID);
    collections->emptyStructures.add(layoutID, result);
    return result;
}

Structure* Instance::emptyStructureForLayout(uint16_t layoutID, JSObject* prototype)
{
    unsigned capacity = TypedLayoutTable::numberOfSlots(layoutID);
    RELEASE_ASSERT(capacity);
    DeferGC deferGC(*vm);
    unsigned inlineSlots = TypedLayoutTable::inlineSlots(layoutID);
    Structure* empty = globalObject->structureCache().emptyObjectStructureForPrototype(globalObject, prototype, KnownShape::inlineCapacityFor(inlineSlots));
    RELEASE_ASSERT(empty->inlineCapacity() >= inlineSlots);
    Structure* result = Structure::createWithProperties(*vm, empty, { }, std::span<const uint16_t> { }, capacity, inlineSlots);
    RELEASE_ASSERT(result);
    result->setTypedLayoutID(layoutID);
    return result;
}

JSObject* Instance::newObjectOf(VM& vm, Structure* structure)
{
    unsigned outOfLineCapacity = structure->outOfLineCapacity();
    if (!outOfLineCapacity)
        return constructEmptyObject(vm, structure);
    DeferGC deferGC(vm);
    Butterfly* butterfly = Butterfly::create(vm, nullptr, 0, outOfLineCapacity, false, IndexingHeader(), 0);
    gcSafeZeroMemory(std::bit_cast<EncodedJSValue*>(butterfly->propertyStorage() - outOfLineCapacity), outOfLineCapacity * sizeof(EncodedJSValue));
    return JSFinalObject::createWithButterfly(vm, structure, butterfly);
}

bool Instance::convertToTypedLayout(VM& vm, JSObject* object, uint16_t layoutID)
{
    Structure* old = object->structure();
    unsigned capacity = TypedLayoutTable::numberOfSlots(layoutID);
    auto no = [](ASCIILiteral why) {
        TypedLayoutTable::s_lastConversionFailure = why;
        return false;
    };
    if (object->type() != FinalObjectType)
        return no("it is not a plain object"_s);
    if (old->typedLayoutID())
        return no("it already has the layout of another type"_s);
    if (!capacity)
        return no("the type has no layout"_s);
    unsigned inlineSlots = TypedLayoutTable::inlineSlots(layoutID);
    // With a typed layout that uses field IDs, an object has as many fields in their assigned slots as it has room for, and the
    // rest anywhere. Readers check the slot first (Structure::fieldIDInSlot()).
    bool usesFieldIDs = TypedLayoutTable::usesFieldIDs(layoutID);
    if (usesFieldIDs) {
        if (old->cannotConvertToTypedLayout())
            return no("objects with its structure cannot be converted"_s);
        capacity = inlineSlots = std::min<unsigned>(inlineSlots, old->inlineCapacity());
        if (!capacity) {
            old->setCannotConvertToTypedLayout();
            return no("it does not have enough inline capacity"_s);
        }
    }
    if (old->hasPolyProto())
        return no("its prototype is not supported"_s);
    if (old->mayBePrototype())
        return no("it is used as a prototype"_s);
    // (The private methods of an object are identified by its Structure, so the Structure cannot be replaced.)
    if (old->isBrandedStructure())
        return no("it has private methods"_s);
    if (old->inlineCapacity() < inlineSlots)
        return no("it does not have enough inline capacity"_s);
    if (!old->isStructureExtensible())
        return no("it is not extensible"_s);
    if (old->isDictionary() && old->isUncacheableDictionary())
        return no("it is a dictionary"_s);
    Instance& instance = ensure(old->globalObject());
    if (auto it = instance.collections->rejectedConversions.find({ old, layoutID }); it != instance.collections->rejectedConversions.end())
        return no(it->value);
    auto rejectStructure = [&](ASCIILiteral why) {
        if (!old->isDictionary() && instance.collections->rejectedConversions.size() < 4096)
            instance.collections->rejectedConversions.add({ old, layoutID }, why);
        return no(why);
    };
    // Properties without a slot in the typed layout come after the layout's slots: inline if all of the layout's slots are inline
    // and there is room, otherwise out of line.
    Vector<std::pair<PropertyOffset, uint16_t>, 16> moves; // From which offset to which slot, in property order.
    Vector<UniquedStringImpl*, 16> names;
    Vector<uint16_t, 16> slots;
    Vector<const TypedLayoutTable::Field*, 16> fields; // Null: the typed layout has no field with that name.
    Vector<unsigned, 16> attributes;
    Vector<const TypedLayoutTable::Field*, 4> accessors; // Fields of the typed layout that are accessors on this object.
    // (Slots are numbered as for an object with exactly `capacity` inline slots, so any extra inline capacity goes unused.)
    if (auto plan = instance.collections->conversionPlans.find({ old, layoutID }); plan != instance.collections->conversionPlans.end()) {
        moves = plan->value.moves;
        fields = plan->value.fields;
    } else {
        BitVector taken;
        unsigned next = capacity;
        bool isPlain = true;
        bool hasTwoForOneSlot = false;
        old->forEachProperty(vm, [&](const PropertyTableEntry& entry) {
            auto* field = TypedLayoutTable::findField(layoutID, entry.key());
            if (usesFieldIDs) {
                if (entry.attributes() & (PropertyAttribute::Accessor | PropertyAttribute::CustomAccessor | PropertyAttribute::CustomValue)) {
                    if (field)
                        accessors.append(field);
                    field = nullptr;
                }
            } else
                isPlain &= !entry.attributes();
            attributes.append(entry.attributes());
            // (Two names share a slot if no type has both. If an object had both, a read of one name would return the value of the
            // other.)
            hasTwoForOneSlot |= field && !usesFieldIDs && taken.get(field->slot);
            unsigned slot = field && field->slot < capacity && !taken.get(field->slot) ? field->slot : next++;
            taken.set(slot);
            fields.append(field);
            names.append(entry.key());
            slots.append(safeCast<uint16_t>(slot));
            moves.append({ entry.offset(), safeCast<uint16_t>(slot) });
            return true;
        });
        if (!isPlain)
            return rejectStructure("it has a property that is not a plain data property"_s);
        if (hasTwoForOneSlot)
            return rejectStructure("it has two properties that share a slot"_s);
        // (With field IDs, a read of a missing field detects that it is missing.)
        for (auto& field : TypedLayoutTable::fieldsOf(layoutID)) {
            if (!usesFieldIDs && !field.mayBeAbsent && !taken.get(field.slot))
                return rejectStructure("it lacks a required property"_s);
        }
        // A field that the object lacks must not be inherited either, because code that finds the slot empty does not search the
        // prototype chain.
        if (JSValue prototype = old->storedPrototype(); prototype.isObject() && asObject(prototype) != old->globalObject()->objectPrototype()) {
            // (With field IDs, the occupied slots do not identify which names are present.)
            if (usesFieldIDs) {
                if (!old->isDictionary())
                    old->setCannotConvertToTypedLayout();
                return no("its prototype is not supported"_s);
            }
            UniquedStringImpl* const* identifiers = StaticHeap::identifiersOfProgram();
            for (JSObject* holder = asObject(prototype); holder && holder != old->globalObject()->objectPrototype();) {
                if (holder->type() != FinalObjectType && holder->type() != ObjectType)
                    return no("its prototype is not supported"_s);
                for (auto& field : TypedLayoutTable::fieldsOf(layoutID)) {
                    if (!taken.get(field.slot) && isValidOffset(holder->structure()->get(vm, PropertyName(Identifier::fromUid(vm, identifiers[field.identifier])))))
                        return no("it inherits a property that the type declares"_s);
                }
                JSValue next = holder->structure()->storedPrototype(holder);
                holder = next.isObject() ? asObject(next) : nullptr;
            }
        }
    }
    // The values have to be valid for their fields. (Checking a value may convert another object to a typed layout.)
    Vector<JSValue, 16> values;
    for (unsigned i = 0; i < moves.size(); ++i) {
        JSValue value = object->getDirect(moves[i].first);
        // (A field's type applies wherever the value is stored, because every store by that name is checked against it.)
        if (fields[i] && TypedLayoutTable::checkStore(*fields[i], value) == TypedLayoutTable::StoreCheck::Rejected)
            return no("the value of a property does not match its declared type"_s);
        values.append(fields[i] ? TypedLayoutTable::toFieldRepresentation(*fields[i], value) : value);
    }
    if (object->structure() != old)
        return object->structure()->typedLayoutID() == layoutID; // The object refers to itself, and was converted by the check above.
    Structure* converted;
    if (auto it = instance.collections->convertedStructures.find({ old, layoutID }); it != instance.collections->convertedStructures.end())
        converted = it->value;
    else {
        DeferGC deferGC(vm);
        Structure* empty = old->storedPrototype().isObject()
            ? old->globalObject()->structureCache().emptyObjectStructureForPrototype(old->globalObject(), asObject(old->storedPrototype()), old->inlineCapacity())
            : Structure::create(vm, old->globalObject(), jsNull(), old->typeInfo(), old->classInfoForCells(), NonArray, old->inlineCapacity());
        converted = empty->inlineCapacity() == old->inlineCapacity() && empty->indexingType() == old->indexingType() ? Structure::createWithProperties(vm, empty, names.span(), slots.span(), capacity, inlineSlots, attributes.span()) : nullptr;
        if (converted)
            converted->copyAccessorAndReadOnlyFlagsFrom(*old);
        if (converted && usesFieldIDs) {
            // (The same result as calling Structure::noteFieldAdded() for each field.)
            uint16_t fieldIDInSlot[Structure::numberOfSlotsWithFieldIDs] { };
            for (auto* field : accessors) {
                if (field->slot < Structure::numberOfSlotsWithFieldIDs)
                    fieldIDInSlot[field->slot] = Structure::ambiguousFieldID;
            }
            for (unsigned i = 0; i < fields.size(); ++i) {
                if (auto* field = fields[i]; field && field->slot < Structure::numberOfSlotsWithFieldIDs)
                    fieldIDInSlot[field->slot] = !fieldIDInSlot[field->slot] && field->slot < capacity && slots[i] == field->slot ? field->id : Structure::ambiguousFieldID; // (Slot numbers from `capacity` up are out of line.)
            }
            converted->setTypedLayoutID(layoutID, fieldIDInSlot);
        } else if (converted)
            converted->setTypedLayoutID(layoutID);
        // (A dictionary Structure belongs to a single object, so there is nothing to cache.)
        if (!old->isDictionary()) {
            instance.collections->convertedStructures.add({ old, layoutID }, converted);
            if (converted) {
                Collections::LayoutConversionPlan plan;
                plan.moves = moves;
                plan.fields = fields;
                instance.collections->conversionPlans.add({ old, layoutID }, WTF::move(plan));
            }
        }
    }
    if (!converted)
        return no("it has indexed elements, or no structure could be created"_s);
    {
        DeferGC deferGC(vm);
        unsigned oldOutside = old->outOfLineCapacity();
        unsigned newOutside = converted->outOfLineCapacity();
        // (The object has no indexed elements. The out-of-line capacity is derived from the Structure, and the collector relies on
        // that.)
        if (newOutside != oldOutside) {
            Butterfly* butterfly = nullptr;
            if (newOutside) {
                butterfly = Butterfly::create(vm, nullptr, 0, newOutside, false, IndexingHeader(), 0);
                gcSafeZeroMemory(std::bit_cast<EncodedJSValue*>(butterfly->propertyStorage() - newOutside), newOutside * sizeof(EncodedJSValue));
            }
            object->nukeStructureAndSetButterfly(vm, object->structureID(), butterfly);
        } else {
            for (unsigned i = 0; i < newOutside; ++i)
                object->locationForOffset(firstOutOfLineOffset + i)->clear();
        }
        for (unsigned offset = 0; offset < old->inlineCapacity(); ++offset)
            object->locationForOffset(offset)->clear();
        for (unsigned i = 0; i < moves.size(); ++i)
            object->putDirectOffset(vm, TypedLayoutTable::offsetOfSlot(moves[i].second, inlineSlots), values[i]);
        object->setStructure(vm, converted);
        vm.writeBarrier(object);
    }
    return true;
}

std::span<const uint16_t> Instance::slotsOfKnownShape(uint32_t shape) const
{
    Image* image = Image::withShapes();
    const ImageShape& description = image->at<ImageShape>(image->header().shapesOffset)[shape];
    if (!description.slots)
        return { };
    return { image->at<uint16_t>(image->header().slotsOfShapesOffset) + description.slots - 1, description.numberOfProperties };
}

void Instance::lookAtObjectPrototype()
{
    if (!objectPrototype)
        return;
    Structure* structure = objectPrototype->structure();
    if (structure->id().bits() == structureIDOfObjectPrototype)
        return;
    structureIDOfObjectPrototype = 0;
    // (A dictionary can gain properties without changing its Structure.)
    if (structure->isDictionary() || !structure->propertyAccessesAreCacheable() || structure->typeInfo().overridesGetOwnPropertySlot()
        || structure->typeInfo().getOwnPropertySlotIsImpureForPropertyAbsence() || !structure->storedPrototype().isNull()
        || (structure->typeInfo().hasStaticPropertyTable() && !structure->staticPropertiesReified()))
        return;
    Image* image = Image::withShapes();
    memset(selectorsOnObjectPrototype, 0, image->header().numberOfSelectors / 8 + 1);
    structure->forEachProperty(*vm, [&](const PropertyTableEntry& entry) {
        if (uint32_t selector = image->selectorNamed(*entry.key()))
            selectorsOnObjectPrototype[selector / 8] |= 1 << (selector % 8);
        return true;
    });
    structureIDOfObjectPrototype = structure->id().bits();
}

Structure* Instance::structureOfLiteral(Structure* empty, std::span<UniquedStringImpl* const> names)
{
    ASSERT(empty->storedPrototype() == globalObject->objectPrototype());
    Vector<uintptr_t, 32> words;
    words.append(empty->inlineCapacity());
    for (UniquedStringImpl* name : names)
        words.append(std::bit_cast<uintptr_t>(name));
    String key { std::span { reinterpret_cast<const Latin1Character*>(words.span().data()), words.size() * sizeof(void*) } };
    if (auto it = collections->shapes.find(key); it != collections->shapes.end())
        return it->value;
    Structure* result = Structure::createWithProperties(*vm, empty, names);
    collections->shapes.add(WTF::move(key), result); // (The structure keeps the names alive.)
    return result;
}
template void Instance::visit(AbstractSlotVisitor&, bool);
template void Instance::visit(SlotVisitor&, bool);

JSObject* Instance::tryCopySlotsForSpread(JSObject* source)
{
    Structure* ofSource = source->structure();
    Structure* ofCopy = nullptr;
    if (auto it = collections->structuresOfCopies.find(ofSource); it != collections->structuresOfCopies.end())
        ofCopy = it->value;
    else {
        // The conditions of tryCreateObjectViaCloning(), except for the prototype, which the copy does not share.
        if (ofSource->canPerformFastPropertyEnumerationCommon() && checkStructureForClone(ofSource) && !ofSource->outOfLineCapacity()) {
            Vector<UniquedStringImpl*, 32> names;
            bool isInOrder = true;
            ofSource->forEachProperty(*vm, [&](const PropertyTableEntry& entry) {
                isInOrder &= !entry.attributes() && static_cast<size_t>(entry.offset()) == names.size();
                names.append(entry.key());
                return isInOrder;
            });
            if (isInOrder && !names.isEmpty()) {
                DeferGC deferGC(*vm);
                // With at least the capacity of the source, so that the copy has them all in the object too, and at least that of an
                // empty object literal, so that { ...small, more } has room.
                unsigned inlineCapacity = std::max<unsigned>(ofSource->inlineCapacity(), JSFinalObject::defaultInlineCapacity);
                Structure* empty = globalObject->structureCache().emptyObjectStructureForPrototype(globalObject, globalObject->objectPrototype(), inlineCapacity);
                if (empty->inlineCapacity() >= names.size())
                    ofCopy = structureOfLiteral(empty, names.span());
            }
        }
        collections->structuresOfCopies.add(ofSource, ofCopy);
    }
    if (!ofCopy)
        return nullptr;
    JSFinalObject* copy = JSFinalObject::create(*vm, ofCopy);
    // (In both, the properties are at the offsets from zero up.)
    for (PropertyOffset offset = 0; offset <= ofCopy->maxOffset(); ++offset)
        copy->putDirectOffset(*vm, offset, source->getDirect(offset));
    return copy;
}

void Instance::didHaveABadTime()
{
    structureIDOfNewArrayWithInt32 = 0;
    structureIDOfNewArrayWithContiguous = 0;
    zeroSpan(std::span { structureIDsOfNewCopyOnWriteArrays });
}

void Instance::noteFieldAddition(Structure* before, unsigned slot, Structure* afterwards)
{
    FieldAddition& entry = fieldAdditions[indexOfFieldAddition(before->id().bits(), slot)];
    entry.structureID = before->id().bits();
    entry.slot = slot;
    entry.structureIDAfterAddition = afterwards->id().bits();
    collections->hasFieldAdditions = true;
}

void Instance::finalizeUnconditionally(bool onlyNew)
{
    if (std::exchange(collections->hasFieldAdditions, false))
        zeroSpan(std::span { fieldAdditions });
    zeroSpan(std::span { customGetters });
    for (PolymorphicSlots* several : collections->slotsOfSites) {
        if (onlyNew && !several->owner->hasBeenFilledSinceLastCollection)
            continue;
        for (Slot& slot : several->slots)
            several->owner->finalizeSlot(*vm, slot);
    }
    for (Data* data : onlyNew ? collections->filledSinceLastCollection : collections->all)
        data->finalizeUnconditionally(*vm);
    for (Data* data : collections->filledSinceLastCollection)
        data->hasBeenFilledSinceLastCollection = false;
    collections->filledSinceLastCollection.shrink(0);
    collections->transitions.appendVector(collections->transitionsSinceLastCollection);
    collections->transitionsSinceLastCollection.shrink(0);
    if (!onlyNew) {
        // Remove duplicates and the slots that no longer have a transition.
        auto& transitions = collections->transitions;
        std::ranges::sort(transitions);
        transitions.shrink(std::ranges::unique(transitions).begin() - transitions.begin());
        transitions.removeAllMatching([](Slot* slot) { return !hasTransition(*slot); });
    }
}

void Data::finalizeSlot(VM& vm, Slot& slot)
{
    if (!slot.structureID)
        return;
    bool dead = !vm.heap.isMarked(slot.structureID.decode());
    if (!dead) {
        if (slot.offset & Slot::pointerIsCell)
            dead = !vm.heap.isMarked(static_cast<JSCell*>(slot.pointer));
        else if (!slot.hasPointer() && slot.newStructureID && (!slot.fieldType || (slot.offset & Slot::hasFieldType))) // A scope cache may have an untagged address here.
            dead = !vm.heap.isMarked(slot.newStructureID.decode());
    }
    if (dead) {
        slot.clear();
        slotEpoch++;
    }
}

void Data::finalizeUnconditionally(VM& vm)
{
    for (unsigned i = 0; i < numSlots; ++i)
        finalizeSlot(vm, slots[i]);

    if (watchpoints) {
        watchpoints->removeIf([&](auto& entry) {
            Slot& slot = *entry.key;
            if (!slot.structureID)
                return true;
            for (const SlotWatchpoint& watchpoint : entry.value) {
                if (!watchpoint.key().isStillLive(vm)) {
                    slot.clear();
                    slotEpoch++;
                    return true;
                }
            }
            return false;
        });
    }
}

static Stub stubFor(JITCode::Way way)
{
    switch (way) {
    case JITCode::Way::TopLevel:
        return Stub::Enter;
    case JITCode::Way::Call:
        return Stub::EnterFunctionForCall;
    case JITCode::Way::Construct:
        return Stub::EnterFunctionForConstruct;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JITCode::Way JITCode::entryBlockFor(UnlinkedCodeBlock* codeBlock)
{
    if (codeBlock->codeType() != FunctionCode)
        return Way::TopLevel;
    return codeBlock->isConstructor() ? Way::Construct : Way::Call;
}

// There are only as many of these as there are prefixes of the list of callee-saved registers.
const RegisterAtOffsetList* calleeSaveRegistersOf(const ImageFunction& function)
{
    static Lock lock;
    static NeverDestroyed<UncheckedKeyHashMap<uint64_t, Vector<std::unique_ptr<RegisterAtOffsetList>, 1>>> lists;

    const ImageFrame& frame = Image::of(function).frameOf(function);
    uint64_t mask = ImageFunction::unpackRegisters(frame.calleeSaveRegisters);
    ptrdiff_t offsetOfFirst = -static_cast<ptrdiff_t>(frame.whereCalleeSavesStart * sizeof(CPURegister));
    uint64_t key = (static_cast<uint64_t>(frame.whereCalleeSavesStart) << 32 | frame.calleeSaveRegisters) * 2 + 1; // Odd, so never the empty or deleted value of the hash table.
    auto isThat = [&](const RegisterAtOffsetList& list) {
        uint64_t registers = 0;
        for (unsigned i = 0; i < list.registerCount(); ++i)
            registers |= 1ULL << list.at(i).reg().index();
        return registers == mask && (!mask || list.at(0).offset() == offsetOfFirst);
    };

    Locker locker { lock };
    auto& candidates = lists->add(key, Vector<std::unique_ptr<RegisterAtOffsetList>, 1>()).iterator->value;
    for (auto& candidate : candidates) {
        if (isThat(*candidate))
            return candidate.get();
    }
    RegisterSet registers;
    for (unsigned index = 0; index < 64; ++index) {
        if (mask >> index & 1)
            registers.add(Reg::fromIndex(index), IgnoreVectors);
    }
    auto list = makeUnique<RegisterAtOffsetList>(registers);
    if (mask)
        list->adjustOffsets(offsetOfFirst - list->at(0).offset());
    RELEASE_ASSERT(isThat(*list));
    candidates.append(WTF::move(list));
    return candidates.last().get();
}

JITCode::JITCode(void* code, const ImageFunction& function, Way way)
    // Code in an image is file-backed memory, so it must not count toward the heap size that the collector uses to pace itself.
    : JSC::JITCode(JITType::AOTJIT, CodePtr<JSEntryPtrTag>::fromTaggedPtr(tagCodePtr<JSEntryPtrTag>(addressOfStub(stubFor(way)))), ShareAttribute::Shared)
    , m_code(code)
    , m_entry(EntryWord::encode(code, function.convention()))
    , m_function(&function)
    , m_calleeSaveRegisters(calleeSaveRegistersOf(function))
{
}

JITCode::~JITCode() = default;

CodePtr<JSEntryPtrTag> JITCode::addressForCall(ArityCheckMode)
{
    // Callers that use this address build frames in the interpreter's format.
    return m_addressForCall;
}

void* JITCode::executableAddressAtOffset(size_t offset) { return static_cast<uint8_t*>(m_code) + offset; }
void* JITCode::dataAddressAtOffset(size_t offset) { return static_cast<uint8_t*>(m_code) + offset; }
unsigned JITCode::offsetOf(void* pointer) { return static_cast<uint8_t*>(pointer) - static_cast<uint8_t*>(m_code); }
unsigned JITCode::codeSize() const { return Image::of(*m_function).sizeOfCodeOf(*m_function); }
unsigned JITCode::frameSizeInBytes() const { return Image::of(*m_function).frameOf(*m_function).frameSizeInBytes(); }
size_t JITCode::size() { return codeSize(); }

bool JITCode::contains(void* address)
{
    return address >= m_code && address < static_cast<uint8_t*>(m_code) + codeSize();
}

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)
