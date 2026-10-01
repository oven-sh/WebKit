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
    if (const void* inImage = Image::addressOfStub(stub))
        return const_cast<void*>(inImage);
    // (They are made now, which takes somewhere to put them.)
    RELEASE_ASSERT_WITH_MESSAGE(Options::useJIT(), "There is no image with code in it, and no JIT to do without one.");
    const StubBlob& blob = *static_cast<const StubBlob*>(g_compilerHooks.stubBlobOfAOT());
    return static_cast<uint8_t*>(blob.inJITMemory) + blob.offsets[static_cast<unsigned>(stub)];
}

void* catchThunk()
{
    return tagCodePtr<ExceptionHandlerPtrTag>(addressOfStub(Stub::Catch));
}

void* nearCallTargetFor(void* code)
{
    if (isJITPC(code))
        return code;
    // Like the interpreter's entry points, an image is out of reach of a call instruction in the JIT's memory.
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

extern "C" void* g_aotWaysIntoStaticFunctions[2]; // FunctionExecutable.cpp

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
    // What a FunctionExecutable in the short form has no room to say: ExecutableBase::wayIntoShortForm().
    set(Entry::EnterStaticFunctionForCall, tagCodePtr<JSEntryPtrTag>(addressOfStub(Stub::EnterStaticFunctionForCall)));
    set(Entry::EnterStaticFunctionForConstruct, tagCodePtr<JSEntryPtrTag>(addressOfStub(Stub::EnterStaticFunctionForConstruct)));
    g_aotWaysIntoStaticFunctions[0] = m_entries[static_cast<unsigned>(Entry::EnterStaticFunctionForCall)];
    g_aotWaysIntoStaticFunctions[1] = m_entries[static_cast<unsigned>(Entry::EnterStaticFunctionForConstruct)];

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

    // (With a JIT it has boundFunctionCallGenerator()'s thunk.)
    if (usesStubs && !Options::useJIT() && Options::aotCallsBoundFunctionsWithStub())
        vm.getBoundFunction(true, SourceTaintedOrigin::Untainted)->setCodeToBeCalledWith(CodePtr<JSEntryPtrTag>::fromTaggedPtr(tagCodePtr<JSEntryPtrTag>(addressOfStub(Stub::CallBoundFunction))));
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
    bool hasAddsOfFields { false }; // Instance::addsOfFields
    Vector<ScriptExecutable*> executablesWithoutData; // That the collector has to be told of.
    // The slots that have, or have had, a transition. The collector goes over them again and again while it marks, and they are few.
    Vector<Slot*> transitions;
    Vector<Slot*> transitionsSinceLastCollection;
    Vector<SlotsOfSite*> slotsOfSites;
    UncheckedKeyHashMap<String, Structure*> shapes; // By inline capacity and the addresses of the names. Null: there is no such structure.
    UncheckedKeyHashMap<uint32_t, Structure*> knownShapes; // By number: the ones that have been made.
    // Instance::adopt(): what an object of a Structure turns into when it is made one of a family. Null: it cannot be. (Both are kept.)
    UncheckedKeyHashMap<std::pair<Structure*, uint16_t>, Structure*> adoptions;
    // What goes where, for those of them that are not null: from which offset to which slot, and which field of the family it is (null: none).
    struct LayoutConversionPlan {
        Vector<std::pair<PropertyOffset, uint16_t>> moves;
        Vector<const TypedLayoutTable::Field*> fields;
    };
    UncheckedKeyHashMap<std::pair<Structure*, uint16_t>, LayoutConversionPlan> conversionPlans;
    // Those that were turned down for what their Structure says. (Not kept: if another Structure comes to be where one of these was, its objects are read the long way, is all.)
    UncheckedKeyHashMap<std::pair<Structure*, uint16_t>, ASCIILiteral> turnedDown; // And what for.
    UncheckedKeyHashMap<uint32_t, Structure*> emptyStructures;
    size_t environmentsSize { 0 }; // Rounded up to whole pages.
    size_t sizeFromInstance { 0 };
    size_t sizeOfInfos { 0 }; // If they are the Instance's own.
    size_t numberOfFunctions { 0 };
    // Instance::allocateForData(). All in units of sixteen bytes from the Instance.
    size_t startOfDatas { 0 };
    size_t endOfDatasUsed { 0 };
    size_t endOfDatas { 0 };
    Vector<std::pair<size_t, size_t>> freeAmongDatas; // Where and how much, in order.
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
    Instance* instance;
    size_t environmentsSize = 0;
    size_t size;
    size_t numberOfFunctions;
    // (Addresses. It is memory once it is touched.)
    constexpr size_t roomForDatas = 256 * MB;
    auto startOfDatasFor = [](size_t numberOfFunctions) {
        return std::max<size_t>(roundUpToMultipleOf(WTF::pageSize(), sizeof(Instance) + numberOfFunctions * sizeof(uint32_t)), static_cast<size_t>(leastStateWithData) << shiftOfStateWithData);
    };
    auto sizeFor = [&](size_t numberOfFunctions) { return startOfDatasFor(numberOfFunctions) + roomForDatas; };
    if (Image::environmentsSize() && !vm.m_aotInstanceOfProgram && StaticHeap::canPlaceCellsOf(vm)) {
        // Where cells can be that the collector did not allocate.
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
    instance->factsOfFunctions = environmentsSize ? StaticHeap::factsOfFunctions(vm) : nullptr;
    instance->constantsOfProgram = environmentsSize ? StaticHeap::constantsOfProgram(vm) : nullptr;
    instance->sharedData = SharedData::get();
    // (Nothing of it is there until it is looked at.)
    instance->fieldsNotJustRead = static_cast<uint8_t*>(OSAllocator::reserveAndCommit(sizeOfFieldsNotJustRead, OSAllocator::FastMallocPages));
    if (Image* image = Image::withCode()) {
        instance->code = static_cast<const uint8_t*>(image->code());
        instance->granulesOfCode = image->at<uint32_t>(image->header().granulesOfCodeOffset);
        instance->startsOfFunctionsAfterFirst = image->at<uint32_t>(image->header().startsOfFunctionsOffset) + 1;
    }
    instance->missesForEightSlots = Options::aotMissesForEightSlots();
    instance->missesToSpare = Options::aotMissesToSpare();
    instance->structureIDBase = JSC::structureIDBase();
    RELEASE_ASSERT_WITH_MESSAGE(!Image::withCode() || instance->structureIDBase == structureIDBaseOfImages, "Structures are not where the program's code takes them to be: the addresses were taken.");
    {
        auto idOf = [](Structure* structure) { return structure->id().bits(); };
        if (Options::useImmutableIntrinsics()) {
            auto ofReceiver = [&](Receiver receiver) -> uint32_t& { return instance->structureIDsOfReceivers[static_cast<unsigned>(receiver)]; };
            ofReceiver(Receiver::Map) = idOf(globalObject->mapStructure());
            ofReceiver(Receiver::Set) = idOf(globalObject->setStructure());
            ofReceiver(Receiver::WeakMap) = idOf(globalObject->weakMapStructure());
            ofReceiver(Receiver::WeakSet) = idOf(globalObject->weakSetStructure());
            ofReceiver(Receiver::RegExp) = idOf(globalObject->regExpStructure());
            ofReceiver(Receiver::Date) = idOf(globalObject->dateStructure());
            for (IndexingType type : { ArrayWithUndecided, ArrayWithInt32, ArrayWithDouble, ArrayWithContiguous, ArrayWithArrayStorage, CopyOnWriteArrayWithInt32, CopyOnWriteArrayWithDouble, CopyOnWriteArrayWithContiguous })
                instance->structureIDsOfOriginalArrays[(type & (IndexingShapeMask | CopyOnWrite)) >> Instance::shiftOfKindOfArray] = idOf(globalObject->originalArrayStructureForIndexingType(type));
        }
        if (!globalObject->isHavingABadTime()) {
            instance->structureIDOfNewArrayWithInt32 = idOf(globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithInt32));
            instance->structureIDOfNewArrayWithContiguous = idOf(globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous));
            instance->structureIDsOfNewCopyOnWriteArrays[0] = idOf(globalObject->arrayStructureForIndexingTypeDuringAllocation(CopyOnWriteArrayWithInt32));
            instance->structureIDsOfNewCopyOnWriteArrays[1] = idOf(globalObject->arrayStructureForIndexingTypeDuringAllocation(CopyOnWriteArrayWithDouble));
            instance->structureIDsOfNewCopyOnWriteArrays[2] = idOf(globalObject->arrayStructureForIndexingTypeDuringAllocation(CopyOnWriteArrayWithContiguous));
        }
        // (The other tiers want to be told of the first of each kind that is made, and the second. If there are none, nobody does.)
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
    // What the code has of the realm that only the engine's own functions can name. There are a handful, so they are made now, and the code has nothing to ask.
    if (Image* image = Image::withCode()) {
        MonotonicTime before = MonotonicTime::now();
        unsigned count = 0;
        for (unsigned which = 0; which < numberOfLinkTimeConstants; ++which) {
            if (!(image->header().linkTimeConstantsUsed[which / 64] >> which % 64 & 1))
                continue;
            instance->linkTimeConstants[which] = JSValue::encode(globalObject->linkTimeConstant(static_cast<LinkTimeConstant>(which)));
            ++count;
        }
        if (Options::aotVerbose()) [[unlikely]]
            dataLogLn("AOT: ", count, " link-time constants made ready in ", (MonotonicTime::now() - before).microseconds(), " us");
    }
    memcpySpan(std::span { instance->intrinsics }, globalObject->immutableIntrinsics());
    // (putDirect() does as it is told.)
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
        // (StaticHeap::builtinOfEngineFor() went by that.)
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
    OSAllocator::decommitAndRelease(instance->fieldsNotJustRead, sizeOfFieldsNotJustRead);
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

SUPPRESS_ASAN void* returnAddressInto(const void* frame, const void* startingFrom)
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

SUPPRESS_ASAN Instance* instanceOfFrame(const void* frame)
{
    for (auto* record = static_cast<const FrameRecord*>(frame);; record = record->previous) {
        WhatIsAt::Kind kind = whatIsAt(removeCodePtrTag(record->returnAddress)).kind;
        RELEASE_ASSERT(kind != WhatIsAt::SomethingElse);
        if (kind == WhatIsAt::Adapter)
            return *reinterpret_cast<Instance* const*>(reinterpret_cast<const char*>(record->previous) + offsetOfInstanceInAdapter);
    }
}

SUPPRESS_ASAN bool canTellInstanceOfFrame(const void* frame)
{
    for (auto* record = static_cast<const FrameRecord*>(frame); record; record = record->previous) {
        WhatIsAt::Kind kind = whatIsAt(removeCodePtrTag(record->returnAddress)).kind;
        if (kind == WhatIsAt::SomethingElse)
            return false;
        if (kind == WhatIsAt::Adapter)
            return true;
    }
    return false;
}

NEVER_INLINE bool topFrameIsNotTheEnginesOwn(const void* frame)
{
    if (!hasCode())
        return false;
    void* returnAddress = returnAddressInto(frame, __builtin_frame_address(0));
    return returnAddress && whatIsAt(returnAddress).kind != WhatIsAt::SomethingElse;
}

SUPPRESS_ASAN FunctionRef functionThatCalled(const CallFrame* callFrame)
{
    if (!hasCode())
        return { };
    for (auto* record = reinterpret_cast<const FrameRecord*>(callFrame);; record = record->previous) {
        WhatIsAt what = whatIsAt(removeCodePtrTag(record->returnAddress));
        if (what.kind == WhatIsAt::Function) {
            FunctionRef function { instanceOfFrame(record->previous), what.index };
            // (What it is in the middle of may be what another does, that was made part of it.)
            if (function.info().function()->hasInlineFrames) [[unlikely]]
                return function.placeAt(removeCodePtrTag(record->returnAddress)).function;
            return function;
        }
        if (what.kind != WhatIsAt::Stub)
            return { };
    }
}

CodeBlock* codeBlockOfFunctionThatCalled(const CallFrame* callFrame)
{
    FunctionRef function = functionThatCalled(callFrame);
    return function ? function.ensureCodeBlock() : nullptr;
}

const RegisterAtOffsetList& registersThatAdapterSaves()
{
    static LazyNeverDestroyed<RegisterAtOffsetList> list;
    static std::once_flag once;
    std::call_once(once, [] {
        RegisterSet registers;
        registers.add(instanceGPR, IgnoreVectors);
        registers.add(GPRInfo::numberTagRegister, IgnoreVectors);
        registers.add(GPRInfo::notCellMaskRegister, IgnoreVectors);
        list.construct(registers);
        // (In the order of their numbers, upwards.)
        list->adjustOffsets(offsetOfInstanceRegisterInAdapter - list->find(instanceGPR)->offset());
        RELEASE_ASSERT(list->find(GPRInfo::numberTagRegister)->offset() == offsetOfNumberTagRegisterInAdapter && list->find(GPRInfo::notCellMaskRegister)->offset() == offsetOfNotCellMaskRegisterInAdapter);
    });
    return list.get();
}

bool constantsAreOfNoRealm(UnlinkedCodeBlock* unlinkedCodeBlock, SymbolTablesWillDo symbolTablesWillDo)
{
    auto& constants = unlinkedCodeBlock->constantRegisters();
    auto& representations = unlinkedCodeBlock->constantsSourceCodeRepresentation();
    for (unsigned i = 0; i < constants.size(); ++i) {
        // (Code has those from the Instance: NodeKind::LinkTimeConstant.)
        if (representations[i] == SourceCodeRepresentation::LinkTimeConstant)
            continue;
        JSValue constant = constants[i].get();
        if (!constant || !constant.isCell())
            continue;
        if (auto* symbolTable = dynamicDowncast<SymbolTable>(constant.asCell())) {
            if (symbolTablesWillDo == SymbolTablesWillDo::No && !symbolTable->isItsOwnClone())
                return false;
        } else if (constant.asCell()->inherits<JSTemplateObjectDescriptor>())
            return false;
    }
    return true;
}

// The constants of unlinked code are good as they are, but for the ones that are of a realm.
static bool linkConstants(VM& vm, Data& data)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!data.unlinkedCodeBlock) {
        // Whoever built the program has looked.
        const FunctionInfo& info = data.function().info();
        if (info.constantsAreOfNoRealm()) {
            data.constants = info.constants;
            return true;
        }
        const uint32_t* list = StaticHeap::inData<uint32_t>(*data.function().facts()->find(FunctionFacts::RealmConstants));
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
    bool someAreOfTheRealm = false;
    for (unsigned i = 0; i < constants.size(); ++i) {
        if (representations[i] == SourceCodeRepresentation::LinkTimeConstant) {
            someAreOfTheRealm = true;
            continue;
        }
        JSValue constant = constants[i].get();
        if (!constant || !constant.isCell())
            continue;
        if (auto* symbolTable = dynamicDowncast<SymbolTable>(constant.asCell())) {
            // What becomes of these is a long story, about code from the other compilers (CodeBlock::setConstantRegisters()).
            if (!symbolTable->isItsOwnClone()) {
                data.constants = data.ensureCodeBlock()->constantRegisters().span().data();
                return true;
            }
        } else if (constant.asCell()->inherits<JSTemplateObjectDescriptor>())
            someAreOfTheRealm = true;
    }
    if (!someAreOfTheRealm) {
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
    // (Of a program that was put together with all it needs, they are what its FunctionInfo says. The unlinked code, if there is
    // any, may have been decoded since, and has the same in a place of its own.)
    FunctionInfo& info = instance.infos[code.index()];
    data->identifiers = info.sites ? info.identifiers : unlinkedCodeBlock->identifiers().span().data();
    data->sites = code.sites();
    data->hasSiteConstants = code.imageFunction()->hasSiteConstants;
    data->numSlots = numSlots;
    data->slotEpoch = 1;

    RELEASE_ASSERT(code.index() < instance.collections->numberOfFunctions);
    RELEASE_ASSERT(!instance.dataIfItHasAny(code.index()));
    instance.setData(code.index(), data);
    data->indexAmongAll = instance.collections->all.size();
    instance.collections->all.append(data);
    data->noteFilled(); // It is new.

    if (codeBlock)
        data->constants = codeBlock->constantRegisters().span().data();
    else if (!linkConstants(vm, *data)) {
        destroy(data);
        return nullptr;
    }
    if (!info.sites) {
        if (!instance.collections->sizeOfInfos && Options::aotVerbose()) [[unlikely]]
            dataLogLn("AOT: nothing was known of function ", code.index(), " when the program was built");
        fillInfo(info, executable, unlinkedCodeBlock, code, data->constants);
    }
    RELEASE_ASSERT(info.identifiers == data->identifiers && info.sites == data->sites && (info.flags >> FunctionInfo::numberOfFlagBits) == std::min<uint32_t>(numSlots, FunctionInfo::mostSlotsSaid) && (!info.executable() || info.executable() == executable));
    return data;
}

Data* Instance::ensureData(uint32_t index)
{
    // (One that starts cold, and that only those have called that know what they are calling, has nothing there at all.)
    Data* data = dataIfItHasAny(index);
    if (data)
        return data;
    const FunctionInfo& info = infos[index];
    auto* executable = uncheckedDowncast<FunctionExecutable>(info.executable());
    UnlinkedFunctionCodeBlock* unlinkedCodeBlock = executable->unlinkedExecutable()->codeBlockIfThereIsOne(info.kind());
    // (An executable that was made when the program was built has no way of holding on to one.)
    Ref<JITCode> code = executable->hasJITCodeFor(info.kind()) ? Ref { static_cast<JITCode&>(executable->generatedJITCodeFor(info.kind()).get()) } : codeOfFunctionFromImage({ &Image::of(*info.function()), info.function() }, info.kind());
    RELEASE_ASSERT(code->index() == index);
    code->setInstance(*this);
    data = Data::create(*this, executable, unlinkedCodeBlock, code.get());
    RELEASE_ASSERT(data); // Nothing that it is made of is made now.
    return data;
}

FunctionRef FunctionRef::at(Instance* instance, const void* address)
{
    auto& asked = instance->placeAskedAbout(address);
    if (asked.address == address) [[likely]]
        return { instance, asked.function };
    WhatIsAt what = whatIsAt(address);
    RELEASE_ASSERT(what.kind == WhatIsAt::Function);
    asked = { address, what.index, Instance::PlaceAskedAbout::siteNotLookedFor };
    return { instance, what.index };
}

SiteInPlaceOfCallSite::SiteInPlaceOfCallSite(Instance& instance, const void* returnAddress, uint32_t site)
    : m_instance(instance)
{
    RELEASE_ASSERT(!instance.returnAddressWithSiteInPlace);
    instance.returnAddressWithSiteInPlace = returnAddress;
    instance.siteInPlace = site;
}

SiteInPlaceOfCallSite::~SiteInPlaceOfCallSite()
{
    m_instance.returnAddressWithSiteInPlace = nullptr;
}

static FunctionRef::Place placeOfSite(FunctionRef function, uint32_t site)
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

FunctionRef::Place FunctionRef::placeAt(const void* returnAddress) const
{
    if (returnAddress == instance->returnAddressWithSiteInPlace) [[unlikely]]
        return placeOfSite(*this, instance->siteInPlace);
    using Asked = Instance::PlaceAskedAbout;
    auto& asked = instance->placeAskedAbout(returnAddress);
    if (asked.address != returnAddress || asked.site == Asked::siteNotLookedFor) [[unlikely]] {
        WhatIsAt what = whatIsAt(returnAddress);
        RELEASE_ASSERT(what.kind == WhatIsAt::Function && what.index == index);
        auto site = tryCallSiteAt(*info().function(), what.offset);
        RELEASE_ASSERT(!site || *site < Asked::hasNoSite);
        asked = { returnAddress, index, site.value_or(Asked::hasNoSite) };
    }
    ASSERT(asked.function == index);
    // Not every call is one that anybody was expected to ask about: what is called does not throw, and does not look at the stack. But
    // something may look at the stack at any time (a profiler of allocations does). Then it is the function, and nowhere in particular.
    if (asked.site == Asked::hasNoSite) [[unlikely]]
        return { *this, BytecodeIndex(), 0 };
    return placeOfSite(*this, asked.site);
}

FunctionRef::Place FunctionRef::placeOfInlinedCall(unsigned inlineFrame) const
{
    InlineFrameOfImage frame = inlineFrameOf(*info().function(), inlineFrame);
    Place place = placeOfSite(*this, PackedSite::pack(frame.parent, frame.callSite));
    place.hasBeenLeft = frame.isTailCall;
    return place;
}

BytecodeIndex FunctionRef::bytecodeIndexAt(const void* returnAddress) const
{
    return placeAt(returnAddress).bytecodeIndex;
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

Data* FunctionRef::dataIfItHasAny() const
{
    return instance->dataIfItHasAny(index);
}

Data* FunctionRef::ensureData() const
{
    return instance->ensureData(index);
}

ScriptExecutable* FunctionRef::executable() const
{
    // (That of the code of a module is made when the program runs, so nothing that was made before says which it is.)
    if (Data* data = dataIfItHasAny())
        return data->executable;
    return info().executable();
}

CodeBlock* FunctionRef::codeBlockIfThereIsOne() const
{
    Data* data = dataIfItHasAny();
    return data ? data->codeBlock : nullptr;
}

uint32_t FunctionRef::siteConstantOf(const Slot* slot) const
{
    const FunctionInfo& info = this->info();
    size_t which = slot - (SharedData::contains(slot) ? instance->sharedData : instance->dataIfItHasAny(index))->slots;
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
    // (Not by way of its Data, which code that is not going to run again has let go of: CodeBlock::releaseAOTData().)
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

UnlinkedCodeBlock* FunctionRef::unlinkedCodeBlockIfThereIsOne() const
{
    if (Data* data = dataIfItHasAny(); data && data->unlinkedCodeBlock)
        return data->unlinkedCodeBlock;
    return uncheckedDowncast<FunctionExecutable>(executable())->unlinkedExecutable()->codeBlockIfThereIsOne(info().kind());
}

// As many zeros as any function has bytes of instructions, in memory that is nobody's until somebody reads it.
static std::span<const uint8_t> zerosForInstructions(size_t size)
{
    static constexpr size_t most = static_cast<size_t>(1) << (32 - FunctionFacts::shiftOfInstructionsSize);
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

// It is not going to be interpreted, and whoever asks for one does not ask what the instructions are.
UnlinkedCodeBlock* FunctionRef::makeUnlinkedCodeBlockFromFacts() const
{
    auto* facts = this->facts();
    const uint32_t* scalars = facts ? facts->find(FunctionFacts::Scalars) : nullptr;
    if (!scalars)
        return nullptr;
    PartsOfFunctionCode parts { };
    parts.scalars = StaticHeap::inData<uint8_t>(*scalars);
    parts.instructions = zerosForInstructions(facts->instructionsSize());
    // (Where the program has one table of them, the code goes by where a name is in that: which of the function's own identifiers is
    // which is known to nobody. As with the instructions, whoever asks for one of these does not ask.)
    parts.identifiers = StaticHeap::hasIdentifiersOfProgram() ? nullptr : static_cast<const Identifier*>(info().identifiers);
    parts.constants = static_cast<const WriteBarrier<Unknown>*>(info().constants);
    if (const uint32_t* word = facts->find(FunctionFacts::RealmConstants)) {
        const uint32_t* list = StaticHeap::inData<uint32_t>(*word);
        parts.linkTimeConstants = { list + 2, list[1] };
    }
    // (Nor for the functions in it, which are asked for here: functionDecl(), functionExpr().)
    if (const uint32_t* words = facts->find(FunctionFacts::Handlers))
        parts.handlers = { StaticHeap::inData<UnlinkedHandlerInfo>(words[0]), words[1] };
    if (const uint32_t* word = facts->find(FunctionFacts::ExpressionInfo); word && !StaticHeap::hasPositionsOfCallSites())
        parts.expressionInfo = StaticHeap::inData<uint8_t>(*word);
    return makeFunctionCodeFromParts(*instance->vm, parts);
}

UnlinkedCodeBlock* FunctionRef::ensureUnlinkedCodeBlock() const
{
    if (UnlinkedCodeBlock* existing = unlinkedCodeBlockIfThereIsOne())
        return existing;
    VM& vm = *instance->vm;
    DeferGCForAWhile deferGC(vm);
    DeferTerminationForAWhile deferTermination(vm);
    SuspendExceptionScope suspendExceptions(vm);
    // It is the Data's. (The executable was there when the program was built, and to store to it is to have a page of one's own
    // for the sake of a word.)
    Data* data = ensureData();
    UnlinkedCodeBlock* result = makeUnlinkedCodeBlockFromFacts();
    if (!result)
        result = uncheckedDowncast<FunctionExecutable>(data->executable)->unlinkedExecutable()->decodeCodeLeftInPayload(vm, info().kind(), instance->globalObject);
    RELEASE_ASSERT(result);
    data->unlinkedCodeBlock = result;
    if (!data->hasBeenFilledSinceLastCollection)
        data->noteFilled();
    return result;
}

const FunctionFacts* FunctionRef::facts() const
{
    if (!instance->factsOfFunctions)
        return nullptr;
    uint32_t at = instance->factsOfFunctions[index];
    // (Odd: see reportedPositionFor().)
    return at && !(at & 1) ? StaticHeap::inData<FunctionFacts>(at) : nullptr;
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

// What StaticHeap's makePositions() wrote.
auto FunctionRef::reportedPositionFor(BytecodeIndex bytecodeIndex, OfConstruction ofConstruction) const -> std::optional<ReportedPosition>
{
    if (!instance->factsOfFunctions || !StaticHeap::hasPositionsOfCallSites())
        return std::nullopt;
    const uint8_t* at = nullptr;
    if (uint32_t word = instance->factsOfFunctions[index]; word & 1)
        at = StaticHeap::inData<uint8_t>(word - 1);
    else if (auto* facts = this->facts()) {
        if (const uint32_t* where = facts->find(FunctionFacts::ExpressionInfo))
            at = StaticHeap::inData<uint8_t>(*where);
    }
    if (!at)
        return std::nullopt;
    // Where the function starts: which is where it is, for want of anything better (a builtin has nothing that says where anything is).
    ReportedPosition result;
    result.lineColumn.line = static_cast<unsigned>(readVarint(at));
    result.lineColumn.column = static_cast<unsigned>(readVarint(at));
    uint64_t offset = 0;
    int64_t line = 0;
    int64_t column = 0;
    uint32_t source = 0;
    auto withSignAtTheBottom = [](uint64_t value) {
        return static_cast<int64_t>(value >> 1) ^ -static_cast<int64_t>(value & 1);
    };
    auto readPosition = [&]() -> ReportedPosition {
        uint64_t word = readVarint(at);
        if (word & 1)
            column += withSignAtTheBottom(word >> 1);
        else {
            line += withSignAtTheBottom(word >> 2);
            if (word & 2)
                source = static_cast<uint32_t>(readVarint(at));
            column = static_cast<int64_t>(readVarint(at));
        }
        return { { static_cast<unsigned>(line), static_cast<unsigned>(column) }, source };
    };
    // The last that is not past it: every place that a frame can say it is at is there, so that is the very one.
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
    return facts() ? FunctionCode : unlinkedCodeBlockIfThereIsOne()->codeType();
}

bool FunctionRef::isBuiltinFunction() const
{
    if (auto* facts = this->facts())
        return facts->flagsAndInstructionsSize & FunctionFacts::isBuiltinFunction;
    return unlinkedCodeBlockIfThereIsOne()->isBuiltinFunction();
}

unsigned FunctionRef::instructionsSize() const
{
    if (auto* facts = this->facts())
        return facts->instructionsSize();
    return unlinkedCodeBlockIfThereIsOne()->instructions().size();
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
    auto* facts = this->facts();
    if (!facts)
        return unlinkedCodeBlockIfThereIsOne()->handlerForIndex(bytecodeOffset, RequiredHandler::AnyHandler);
    const uint32_t* words = facts->find(FunctionFacts::Handlers);
    if (!words)
        return nullptr;
    std::span<const UnlinkedHandlerInfo> handlers { StaticHeap::inData<UnlinkedHandlerInfo>(words[0]), words[1] };
    return UnlinkedHandlerInfo::handlerForIndex<const UnlinkedHandlerInfo>(handlers, bytecodeOffset, RequiredHandler::AnyHandler);
}

const UnlinkedStringJumpTable& FunctionRef::stringSwitchJumpTable(unsigned tableIndex) const
{
    auto* facts = this->facts();
    if (!facts)
        return unlinkedCodeBlockIfThereIsOne()->unlinkedStringSwitchJumpTable(tableIndex);
    return StaticHeap::inMalloc<UnlinkedStringJumpTable>(*facts->find(FunctionFacts::StringSwitchJumpTables))[tableIndex];
}

const IdentifierSet& FunctionRef::constantIdentifierSet(unsigned index) const
{
    auto* facts = this->facts();
    if (!facts)
        return ensureUnlinkedCodeBlock()->constantIdentifierSets()[index];
    return StaticHeap::inMalloc<IdentifierSet>(*facts->find(FunctionFacts::ConstantIdentifierSets))[index];
}

BytecodeIndex FunctionRef::resumePointOf(int32_t state) const
{
    if (state <= 0)
        return BytecodeIndex(0);
    int32_t offset = 0;
    if (auto* facts = this->facts()) {
        if (const uint32_t* word = facts->find(FunctionFacts::ResumePoints)) {
            const int32_t* table = StaticHeap::inData<int32_t>(*word);
            if (state >= table[0] && static_cast<uint32_t>(state - table[0]) < static_cast<uint32_t>(table[1]))
                offset = table[2 + state - table[0]];
        }
    } else if (UnlinkedCodeBlock* codeBlock = unlinkedCodeBlockIfThereIsOne(); codeBlock && codeBlock->numberOfUnlinkedSwitchJumpTables())
        offset = codeBlock->unlinkedSwitchJumpTable(codeBlock->numberOfUnlinkedSwitchJumpTables() - 1).offsetForValue(state);
    return BytecodeIndex(std::max(offset, 0));
}

static std::span<const WriteBarrier<UnlinkedFunctionExecutable>> functionsIn(const FunctionFacts& facts, FunctionFacts::Fact which)
{
    const uint32_t* words = facts.find(which);
    if (!words)
        return { };
    return { StaticHeap::inMalloc<WriteBarrier<UnlinkedFunctionExecutable>>(words[0]), words[1] };
}

std::span<const WriteBarrier<UnlinkedFunctionExecutable>> FunctionRef::functionDecls() const
{
    if (auto* facts = this->facts())
        return functionsIn(*facts, FunctionFacts::FunctionDecls);
    return unlinkedCodeBlockIfThereIsOne()->functionDecls();
}

std::span<const WriteBarrier<UnlinkedFunctionExecutable>> FunctionRef::functionExprs() const
{
    if (auto* facts = this->facts())
        return functionsIn(*facts, FunctionFacts::FunctionExprs);
    return unlinkedCodeBlockIfThereIsOne()->functionExprs();
}

void Data::destroy(Data* data)
{
    Instance& instance = *data->instance;
    // (MegamorphicCache::ConstructionEntry::m_site)
    if (auto* cache = instance.vm->megamorphicCache())
        cache->bumpEpoch();
    RELEASE_ASSERT(instance.dataIfItHasAny(data->code->index()) == data);
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
    instance.collections->slotsOfSites.removeAllMatching([&](SlotsOfSite* several) {
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
    // Whoever asks may well be dealing with an exception, and this is no place to find out that the VM has been told to stop.
    DeferTerminationForAWhile deferTermination(vm);
    SuspendExceptionScope suspendExceptions(vm);
    FunctionCodeBlock* result = FunctionCodeBlock::create(vm, uncheckedDowncast<FunctionExecutable>(executable), uncheckedDowncast<UnlinkedFunctionCodeBlock>(unlinkedCodeBlock), instance->globalObject, CodeBlock::LinkMode::ForCodeFromImage);
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
    LineColumn lineColumn;
    if (auto* facts = this->facts()) {
        if (const uint32_t* word = facts->find(FunctionFacts::ExpressionInfo))
            lineColumn = decodeBorrowedExpressionInfo(StaticHeap::inData<uint8_t>(*word))->lineColumnForInstPC(bytecodeIndex.offset());
    } else
        lineColumn = unlinkedCodeBlockIfThereIsOne()->lineColumnForBytecodeIndex(bytecodeIndex);
    lineColumn.column += lineColumn.line ? 1 : executable->startColumn();
    lineColumn.line += executable->firstLine();
    return lineColumn;
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
    // A module shares them with whoever else runs its code.
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
    if (!dataIfItHasAny()) {
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
    if (!dataIfItHasAny()) {
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
    else if (code->imageFunction()->startsCold && Options::aotStartFunctionsCold() && !instance.infos[index].sites && constantsAreOfNoRealm(unlinkedCodeBlock)) {
        if (!instance.collections->sizeOfInfos && Options::aotVerbose()) [[unlikely]]
            dataLogLn("AOT: nothing was known of function ", index, " when the program was built");
        fillInfo(instance.infos[index], executable, unlinkedCodeBlock, code.get(), unlinkedCodeBlock->constantRegisters().span().data());
        instance.infos[index].flags |= FunctionInfo::startsCold;
        instance.collections->executablesWithoutData.append(executable);
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
    if (const FunctionInfo& info = instance->infos[index]; info.flags & FunctionInfo::startsCold && Options::aotStartFunctionsCold()) {
        RELEASE_ASSERT(info.executable() == executable && info.kind() == kind);
        if (info.function()->usesStaticImports && !moduleIsLinkedAsCompiled(scope))
            return false;
        instance->setLinkedWithoutData(index);
        return true;
    }
    UnlinkedFunctionCodeBlock* unlinkedCodeBlock = executable->unlinkedExecutable()->codeBlockIfThereIsOne(kind);
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

SlotsOfSite* Instance::makeSlotsOfSite(Data* owner, UniquedStringImpl* name)
{
    auto* several = static_cast<SlotsOfSite*>(fastZeroedMalloc(sizeof(SlotsOfSite)));
    several->name = name;
    several->owner = owner;
    several->timesLeftToLearnAtOnce = SlotsOfSite::timesToLearnAtOnce;
    collections->slotsOfSites.append(several);
    return several;
}

void Instance::noteTransitionCached(Slot* slot)
{
    collections->transitionsSinceLastCollection.append(slot);
}

// What was there at the last collection and has not been filled since refers to nothing that is young.
template<typename Visitor>
void Instance::visit(Visitor& visitor, bool onlyWhatIsNew)
{
    for (Data* data : onlyWhatIsNew ? collections->filledSinceLastCollection : collections->all)
        data->visit(visitor);
    // Code that has cached a transition can put an object that has already been visited in the new structure.
    auto visitTransitions = [&](const Vector<Slot*>& slots) {
        for (Slot* slot : slots) {
            if (hasTransition(*slot) && visitor.isMarked(slot->structureID.decode()))
                visitor.appendUnbarriered(slot->newStructureID.decode());
        }
    };
    visitTransitions(collections->transitionsSinceLastCollection);
    if (!onlyWhatIsNew)
        visitTransitions(collections->transitions);
    for (Structure* structure : collections->shapes.values()) {
        if (structure)
            visitor.appendUnbarriered(structure);
    }
    for (Structure* structure : collections->knownShapes.values())
        visitor.appendUnbarriered(structure);
    for (Structure* structure : collections->emptyStructures.values())
        visitor.appendUnbarriered(structure);
    for (auto& [from, to] : collections->adoptions) {
        visitor.appendUnbarriered(from.first);
        if (to)
            visitor.appendUnbarriered(to);
    }
    for (ScriptExecutable* executable : collections->executablesWithoutData)
        visitor.appendUnbarriered(executable);
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
        return no("it is no plain object"_s);
    if (old->typedLayoutID())
        return no("it is of another type's family"_s);
    if (!capacity)
        return no("there is no such family"_s);
    unsigned inlineSlots = TypedLayoutTable::inlineSlots(layoutID);
    // Of a family whose slots are verified an object has what there is room for where the family has it, and the rest wherever: whoever reads asks first (Structure::fieldIDInSlot()).
    bool usesFieldIDs = TypedLayoutTable::usesFieldIDs(layoutID);
    if (usesFieldIDs) {
        if (old->cannotConvertToTypedLayout())
            return no("its like is never adopted"_s);
        capacity = inlineSlots = std::min<unsigned>(inlineSlots, old->inlineCapacity());
        if (!capacity) {
            old->setCannotConvertToTypedLayout();
            return no("it has no room"_s);
        }
    }
    if (old->hasPolyProto())
        return no("of its prototype"_s);
    if (old->mayBePrototype())
        return no("it is a prototype"_s);
    // (Which private methods it has is for its Structure to say, and for no other.)
    if (old->isBrandedStructure())
        return no("it has private methods"_s);
    if (old->inlineCapacity() < inlineSlots)
        return no("it has no room"_s);
    if (!old->isStructureExtensible())
        return no("it is not extensible"_s);
    if (old->isDictionary() && old->isUncacheableDictionary())
        return no("it has been through too much (a dictionary)"_s);
    Instance& instance = ensure(old->globalObject());
    if (auto it = instance.collections->turnedDown.find({ old, layoutID }); it != instance.collections->turnedDown.end())
        return no(it->value);
    auto noneOfItsLike = [&](ASCIILiteral why) {
        if (!old->isDictionary() && instance.collections->turnedDown.size() < 4096)
            instance.collections->turnedDown.add({ old, layoutID }, why);
        return no(why);
    };
    // What the family has no slot for comes after the family's: in the object if all the family's are, and there is room; if not, outside.
    Vector<std::pair<PropertyOffset, uint16_t>, 16> moves; // From where to which slot, in the order the properties are in.
    Vector<UniquedStringImpl*, 16> names;
    Vector<uint16_t, 16> slots;
    Vector<const TypedLayoutTable::Field*, 16> fields; // Null: the family has no such name.
    Vector<unsigned, 16> attributes;
    Vector<const TypedLayoutTable::Field*, 4> accessors; // Names of the family that are no fields of this.
    // (The slots are numbered as for an object with room for just so many, so what is left of a bigger one goes unused.)
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
            // (Two names have one slot if no type has both. Whichever of them was given it, whoever reads the other would get that.)
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
            return noneOfItsLike("it has a property that is not plain"_s);
        if (hasTwoForOneSlot)
            return noneOfItsLike("it has two properties that no type has together"_s);
        // (Where the slots are verified, whoever reads what is not there finds that out.)
        for (auto& field : TypedLayoutTable::fieldsOf(layoutID)) {
            if (!usesFieldIDs && !field.mayBeAbsent && !taken.get(field.slot))
                return noneOfItsLike("it lacks a property that the type says it has"_s);
        }
        // What it has no property of its own for it must not have from anywhere else: code that finds nothing in the slot looks no further.
        if (JSValue prototype = old->storedPrototype(); prototype.isObject() && asObject(prototype) != old->globalObject()->objectPrototype()) {
            // (Which slots are taken does not say which names are.)
            if (usesFieldIDs) {
                if (!old->isDictionary())
                    old->setCannotConvertToTypedLayout();
                return no("of its prototype"_s);
            }
            UniquedStringImpl* const* identifiers = StaticHeap::identifiersOfProgram();
            for (JSObject* holder = asObject(prototype); holder && holder != old->globalObject()->objectPrototype();) {
                if (holder->type() != FinalObjectType && holder->type() != ObjectType)
                    return no("of its prototype"_s);
                for (auto& field : TypedLayoutTable::fieldsOf(layoutID)) {
                    if (!taken.get(field.slot) && isValidOffset(holder->structure()->get(vm, PropertyName(Identifier::fromUid(vm, identifiers[field.identifier])))))
                        return no("it inherits a property that the type has"_s);
                }
                JSValue next = holder->structure()->storedPrototype(holder);
                holder = next.isObject() ? asObject(next) : nullptr;
            }
        }
    }
    // What is in it has to be what the slots hold. (Which may take making something else what it has to be.)
    Vector<JSValue, 16> values;
    for (unsigned i = 0; i < moves.size(); ++i) {
        JSValue value = object->getDirect(moves[i].first);
        // (A name of the family holds what it holds wherever it is: whoever stores by it is held to that.)
        if (fields[i] && TypedLayoutTable::checkStore(*fields[i], value) == TypedLayoutTable::StoreCheck::Rejected)
            return no("a property of it is not what the type says"_s);
        values.append(fields[i] ? TypedLayoutTable::toFieldRepresentation(*fields[i], value) : value);
    }
    if (object->structure() != old)
        return object->structure()->typedLayoutID() == layoutID; // It has itself in it.
    Structure* converted;
    if (auto it = instance.collections->adoptions.find({ old, layoutID }); it != instance.collections->adoptions.end())
        converted = it->value;
    else {
        DeferGC deferGC(vm);
        Structure* empty = old->storedPrototype().isObject()
            ? old->globalObject()->structureCache().emptyObjectStructureForPrototype(old->globalObject(), asObject(old->storedPrototype()), old->inlineCapacity())
            : Structure::create(vm, old->globalObject(), jsNull(), old->typeInfo(), old->classInfoForCells(), NonArray, old->inlineCapacity());
        converted = empty->inlineCapacity() == old->inlineCapacity() && empty->indexingType() == old->indexingType() ? Structure::createWithProperties(vm, empty, names.span(), slots.span(), capacity, inlineSlots, attributes.span()) : nullptr;
        if (converted)
            converted->saysOfAccessorsAndReadOnlyPropertiesWhat(*old);
        if (converted && usesFieldIDs) {
            // (As Structure::noteFieldAdded() would have it.)
            uint16_t fieldIDInSlot[Structure::numberOfSlotsWithFieldIDs] { };
            for (auto* field : accessors) {
                if (field->slot < Structure::numberOfSlotsWithFieldIDs)
                    fieldIDInSlot[field->slot] = Structure::ambiguousFieldID;
            }
            for (unsigned i = 0; i < fields.size(); ++i) {
                if (auto* field = fields[i]; field && field->slot < Structure::numberOfSlotsWithFieldIDs)
                    fieldIDInSlot[field->slot] = !fieldIDInSlot[field->slot] && field->slot < capacity && slots[i] == field->slot ? field->id : Structure::ambiguousFieldID; // (From `capacity` on the numbers are of places outside the object.)
            }
            converted->setTypedLayoutID(layoutID, fieldIDInSlot);
        } else if (converted)
            converted->setTypedLayoutID(layoutID);
        // (A dictionary is one object's own.)
        if (!old->isDictionary()) {
            instance.collections->adoptions.add({ old, layoutID }, converted);
            if (converted) {
                Collections::LayoutConversionPlan plan;
                plan.moves = moves;
                plan.fields = fields;
                instance.collections->conversionPlans.add({ old, layoutID }, WTF::move(plan));
            }
        }
    }
    if (!converted)
        return no("it has elements, or no structure can be made for it"_s);
    {
        DeferGC deferGC(vm);
        unsigned oldOutside = old->outOfLineCapacity();
        unsigned newOutside = converted->outOfLineCapacity();
        // (It has no elements. How much room there is outside is for the Structure to say, and the collector goes by that.)
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
    // (One that is a dictionary gets more properties and stays the same Structure.)
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
    collections->shapes.add(WTF::move(key), result); // (The structure keeps the names.)
    return result;
}
template void Instance::visit(AbstractSlotVisitor&, bool);
template void Instance::visit(SlotVisitor&, bool);

void Instance::didHaveABadTime()
{
    structureIDOfNewArrayWithInt32 = 0;
    structureIDOfNewArrayWithContiguous = 0;
    zeroSpan(std::span { structureIDsOfNewCopyOnWriteArrays });
}

void Instance::noteAddOfField(Structure* before, unsigned slot, Structure* afterwards)
{
    AddOfField& entry = addsOfFields[indexOfAddOfField(before->id().bits(), slot)];
    entry.structureID = before->id().bits();
    entry.slot = slot;
    entry.structureIDAfterwards = afterwards->id().bits();
    collections->hasAddsOfFields = true;
}

void Instance::finalizeUnconditionally(bool onlyWhatIsNew)
{
    if (std::exchange(collections->hasAddsOfFields, false))
        zeroSpan(std::span { addsOfFields });
    zeroSpan(std::span { customGetters });
    for (SlotsOfSite* several : collections->slotsOfSites) {
        if (onlyWhatIsNew && !several->owner->hasBeenFilledSinceLastCollection)
            continue;
        for (Slot& slot : several->slots)
            several->owner->finalizeSlot(*vm, slot);
    }
    for (Data* data : onlyWhatIsNew ? collections->filledSinceLastCollection : collections->all)
        data->finalizeUnconditionally(*vm);
    for (Data* data : collections->filledSinceLastCollection)
        data->hasBeenFilledSinceLastCollection = false;
    collections->filledSinceLastCollection.shrink(0);
    collections->transitions.appendVector(collections->transitionsSinceLastCollection);
    collections->transitionsSinceLastCollection.shrink(0);
    if (!onlyWhatIsNew) {
        // Each once, and only those that still have one.
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

JITCode::Way JITCode::wayInto(UnlinkedCodeBlock* codeBlock)
{
    if (codeBlock->codeType() != FunctionCode)
        return Way::TopLevel;
    return codeBlock->isConstructor() ? Way::Construct : Way::Call;
}

// There are as many of these as there are ways to pick the first few of the registers that a callee saves.
const RegisterAtOffsetList* calleeSaveRegistersOf(const ImageFunction& function)
{
    static Lock lock;
    static NeverDestroyed<UncheckedKeyHashMap<uint64_t, Vector<std::unique_ptr<RegisterAtOffsetList>, 1>>> lists;

    const ImageFrame& frame = Image::of(function).frameOf(function);
    uint64_t mask = ImageFunction::unpackRegisters(frame.calleeSaveRegisters);
    ptrdiff_t offsetOfFirst = -static_cast<ptrdiff_t>(frame.whereCalleeSavesStart * sizeof(CPURegister));
    uint64_t key = (static_cast<uint64_t>(frame.whereCalleeSavesStart) << 32 | frame.calleeSaveRegisters) * 2 + 1; // Not one of the two that a table has a use for.
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
    // Code in an image is nobody's memory: the collector, which paces itself by what the heap holds on to, is not to count it.
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
    // Whoever asks makes frames the way the interpreter wants them.
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
