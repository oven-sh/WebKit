/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRuntime.h"

#include "DeferTermination.h"
#include "FrameTracers.h"
#include "FunctionCodeBlock.h"
#include "JSTemplateObjectDescriptor.h"
#include "JSWebAssemblyInstance.h"

#if ENABLE(FTL_JIT)

#include "AOTImage.h"
#include "StaticHeap.h"
#include "AOTOperations.h"
#include "AOTThunks.h"
#include "CCallHelpers.h"
#include "CallLinkInfo.h"
#include "CodeBlock.h"
#include "JITOperations.h"
#include "JITThunks.h"
#include "JSCInlines.h"
#include "ArrayPrototype.h"
#include "MathObject.h"
#include "StringPrototype.h"
#include "LLIntEntrypoint.h"
#include "LLIntSlowPaths.h"
#include "LLIntThunks.h"
#include "LinkBuffer.h"
#include "ThunkGenerators.h"
#include <wtf/TZoneMallocInlines.h>

namespace JSC { namespace AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(RuntimeTable);
WTF_MAKE_STRUCT_TZONE_ALLOCATED_IMPL(Data);
WTF_MAKE_STRUCT_TZONE_ALLOCATED_IMPL(VirtualCallInfo);

unsigned hashOfBytecode(UnlinkedCodeBlock* codeBlock)
{
    const auto& instructions = codeBlock->instructions();
    unsigned hash = 2166136261u ^ codeBlock->numParameters();
    for (uint8_t byte : std::span { static_cast<const uint8_t*>(instructions.rawPointer()), instructions.sizeInBytes() })
        hash = (hash ^ byte) * 16777619u;
    hash = (hash ^ codeBlock->constantRegisters().size()) * 16777619u;
    hash = (hash ^ codeBlock->numberOfIdentifiers()) * 16777619u;
    return hash;
}

void* addressOfStub(Stub stub)
{
    if (const void* inImage = Image::addressOfStub(stub))
        return const_cast<void*>(inImage);
    const StubBlob& blob = stubBlob();
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

RuntimeTable::RuntimeTable(VM& vm)
{
#define AOT_FILL_OPERATION(name) \
    m_entries[static_cast<unsigned>(Entry::name)] = tagCFunctionPtr<void*, OperationPtrTag>(name);
    FOR_EACH_AOT_OPERATION(AOT_FILL_OPERATION)
#undef AOT_FILL_OPERATION

    auto set = [&](Entry entry, void* pointer) {
        m_entries[static_cast<unsigned>(entry)] = pointer;
    };
    if constexpr (usesStubs) {
        set(Entry::HandleException, tagCodePtr<JITThunkPtrTag>(addressOfStub(Stub::HandleException)));
        set(Entry::ThrowStackOverflowAtPrologue, tagCodePtr<JITThunkPtrTag>(addressOfStub(Stub::ThrowStackOverflowAtPrologue)));
        set(Entry::VirtualCall, tagCodePtr<JITThunkPtrTag>(addressOfStub(Stub::VirtualCall)));
        set(Entry::VirtualConstruct, tagCodePtr<JITThunkPtrTag>(addressOfStub(Stub::VirtualConstruct)));
        set(Entry::VirtualTailCall, tagCodePtr<JITThunkPtrTag>(addressOfStub(Stub::VirtualTailCall)));
    } else {
        set(Entry::HandleException, vm.getCTIStub(CommonJITThunkID::HandleException).code().taggedPtr());
        set(Entry::ThrowStackOverflowAtPrologue, vm.getCTIStub(CommonJITThunkID::ThrowStackOverflowAtPrologue).code().taggedPtr());
        set(Entry::VirtualCall, vm.getCTIVirtualCall(CallMode::Regular).code().taggedPtr());
        set(Entry::VirtualConstruct, vm.getCTIVirtualCall(CallMode::Construct).code().taggedPtr());
        set(Entry::VirtualTailCall, vm.getCTIVirtualCall(CallMode::Tail).code().taggedPtr());
    }
    set(Entry::ArityFixup, LLInt::arityFixup().taggedPtr());
    set(Entry::LookupExceptionHandler, tagCFunctionPtr<void*, OperationPtrTag>(operationLookupExceptionHandler));
    set(Entry::LookupExceptionHandlerFromCallerFrame, tagCFunctionPtr<void*, OperationPtrTag>(operationLookupExceptionHandlerFromCallerFrame));
    set(Entry::ThrowStackOverflowError, tagCFunctionPtr<void*, OperationPtrTag>(operationAOTThrowStackOverflowError));

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
    installOperationFrontEnds(vm, m_entries);
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
    Vector<ScriptExecutable*> executablesWithoutData; // That the collector has to be told of.
    UncheckedKeyHashMap<String, Structure*> shapes; // By inline capacity and the addresses of the names. Null: there is no such structure.
    UncheckedKeyHashMap<uint32_t, Structure*> knownShapes; // By number: the ones that have been made.
    size_t environmentsSize { 0 }; // Rounded up to whole pages.
    size_t sizeFromInstance { 0 };
    size_t sizeOfInfos { 0 }; // If they are the Instance's own.
    size_t sizeOfMisses { 0 };
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

static std::atomic<uint64_t> s_startedCold;
static std::atomic<uint64_t> s_gotDataLater;
static void didStartCold()
{
    if (!Options::aotReportStats()) [[likely]]
        return;
    static std::once_flag once;
    std::call_once(once, [] {
        atexit([] {
            dataLogLn("AOT: ", s_startedCold.load(), " functions started with no Data, of which ", s_gotDataLater.load(), " got one");
        });
    });
    s_startedCold++;
}

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
    if (Image::environmentsSize() && StaticHeap::canPlaceCellsOf(vm)) {
        // Where cells can be that the collector did not allocate.
        environmentsSize = roundUpToMultipleOf(WTF::pageSize(), Image::environmentsSize());
        size = roundUpToMultipleOf(WTF::pageSize(), sizeof(Instance) + Image::numberOfFunctionsOfImageWithEnvironments() * sizeof(Data*));
        instance = reinterpret_cast<Instance*>(static_cast<char*>(StaticHeap::allocateBlock(environmentsSize + size)) + environmentsSize);
    } else {
        size = roundUpToMultipleOf(WTF::pageSize(), sizeof(Instance) + maxFunctions * sizeof(Data*));
        instance = static_cast<Instance*>(OSAllocator::reserveAndCommit(size, OSAllocator::FastMallocPages));
    }
    instance->runtimeTable = AOT::runtimeTable(vm).entries();
    instance->globalObject = globalObject;
    instance->vm = &vm;
    instance->collections = new Collections;
    instance->collections->environmentsSize = environmentsSize;
    instance->collections->sizeFromInstance = size;
    // (Addresses, again.)
    size_t numberOfFunctions = (size - sizeof(Instance)) / sizeof(Data*);
    instance->infos = environmentsSize ? StaticHeap::infosOfFunctions(vm) : nullptr;
    if (!instance->infos) {
        instance->collections->sizeOfInfos = roundUpToMultipleOf(WTF::pageSize(), numberOfFunctions * sizeof(FunctionInfo));
        instance->infos = static_cast<FunctionInfo*>(OSAllocator::reserveAndCommit(instance->collections->sizeOfInfos, OSAllocator::FastMallocPages));
    }
    instance->sharedData = SharedData::get();
    instance->collections->sizeOfMisses = roundUpToMultipleOf(WTF::pageSize(), numberOfFunctions * sizeof(uint16_t));
    instance->misses = static_cast<uint16_t*>(OSAllocator::reserveAndCommit(instance->collections->sizeOfMisses, OSAllocator::FastMallocPages));
    instance->structureIDBase = JSC::structureIDBase();
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
        instance->selectorsOnObjectPrototype = static_cast<uint8_t*>(fastZeroedMalloc(image->header().numberOfSelectors / 8 + 1));
    }
    globalObject->setAOTInstance(instance);
    vm.m_aotInstances.append(instance);
    if (environmentsSize) {
        RELEASE_ASSERT(!vm.m_aotInstanceOfProgram);
        vm.m_aotInstanceOfProgram = instance;
    }
    return *instance;
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
    OSAllocator::decommitAndRelease(instance->misses, instance->collections->sizeOfMisses);
    delete instance->collections;
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

static std::atomic<uint32_t> s_nextFunctionIndex { 0 };

uint32_t allocateFunctionIndex()
{
    uint32_t index = s_nextFunctionIndex++;
    RELEASE_ASSERT(index < Instance::maxFunctions);
    return index;
}

bool reserveFunctionIndicesForImage(uint32_t count)
{
    uint32_t expected = 0;
    return count < Instance::maxFunctions && s_nextFunctionIndex.compare_exchange_strong(expected, count);
}

bool isCodeHeader(const void* pointer)
{
    if (std::bit_cast<uintptr_t>(pointer) % alignof(CodeHeader))
        return false;
    if (!Image::containsCode(pointer) && !isJITPC(const_cast<void*>(pointer)))
        return false;
    return static_cast<const CodeHeader*>(pointer)->category == NativeCallee::Category::AOT;
}

Data* dataOf(const CallFrame* callFrame)
{
    auto* instance = std::bit_cast<Instance*>(callFrame->unsafeCodeBlock());
    return instance->data[CodeHeader::fromCallee(callFrame->rawCallee())->index];
}

CodeBlock* codeBlockOf(const CallFrame* callFrame)
{
    return FunctionRef::of(callFrame).ensureData()->ensureCodeBlock();
}

JSObject* calleeOf(const CallFrame* callFrame)
{
    int calleeSlot = CodeHeader::fromCallee(callFrame->rawCallee())->calleeSlot;
    return calleeSlot ? callFrame->registers()[calleeSlot].object() : nullptr;
}

VM& vmOf(const CallFrame* callFrame)
{
    return *std::bit_cast<Instance*>(callFrame->unsafeCodeBlock())->vm;
}

JSGlobalObject* globalObjectOf(const CallFrame* callFrame)
{
    return std::bit_cast<Instance*>(callFrame->unsafeCodeBlock())->globalObject;
}

bool constantsAreOfNoRealm(UnlinkedCodeBlock* unlinkedCodeBlock, SymbolTablesWillDo symbolTablesWillDo)
{
    auto& constants = unlinkedCodeBlock->constantRegisters();
    auto& representations = unlinkedCodeBlock->constantsSourceCodeRepresentation();
    for (unsigned i = 0; i < constants.size(); ++i) {
        if (representations[i] == SourceCodeRepresentation::LinkTimeConstant)
            return false;
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
    info.function = code.imageFunction();
    info.executableAndKind = std::bit_cast<uintptr_t>(executable) | (unlinkedCodeBlock->isConstructor() && unlinkedCodeBlock->codeType() == FunctionCode);
    info.numSlots = code.numSlots();
    info.missesToPutUpWith = std::min<uint32_t>(info.numSlots + info.numSlots / 2 + 8, std::numeric_limits<uint16_t>::max());
    info.flags = code.isFromImage() ? FunctionInfo::hasSiteConstants : 0;
}

Data* Data::create(Instance& instance, ScriptExecutable* executable, UnlinkedCodeBlock* unlinkedCodeBlock, JITCode& code, CodeBlock* codeBlock)
{
    VM& vm = *instance.vm;
    DeferGCForAWhile deferGC(vm);
    unsigned numSlots = code.numSlots();
    Data* data = static_cast<Data*>(fastZeroedMalloc(sizeof(Data) + numSlots * sizeof(Slot)));
    data->codeBlock = codeBlock;
    data->instance = &instance;
    data->executable = executable;
    data->unlinkedCodeBlock = unlinkedCodeBlock;
    code.ref();
    data->code = &code;
    data->identifiers = unlinkedCodeBlock->identifiers().span().data();
    data->sites = code.sites();
    data->hasSiteConstants = code.isFromImage();
    data->numSlots = numSlots;
    data->slotEpoch = 1;

    RELEASE_ASSERT(sizeof(Instance) + (code.header().index + 1) * sizeof(Data*) <= instance.collections->sizeFromInstance);
    Data*& place = instance.data[code.header().index];
    RELEASE_ASSERT(!place);
    place = data;
    data->indexAmongAll = instance.collections->all.size();
    instance.collections->all.append(data);
    data->noteFilled(); // It is new.

    if (codeBlock)
        data->constants = codeBlock->constantRegisters().span().data();
    else if (!linkConstants(vm, *data)) {
        destroy(data);
        return nullptr;
    }
    // (Those of a program that was put together with all it needs say so already, but for what nobody thought would be run.)
    FunctionInfo& info = instance.infos[code.header().index];
    if (!info.sites) {
        if (!instance.collections->sizeOfInfos && Options::aotVerbose()) [[unlikely]]
            dataLogLn("AOT: nothing was known of function ", code.header().index, " when the program was built");
        fillInfo(info, executable, unlinkedCodeBlock, code, data->constants);
    }
    RELEASE_ASSERT(info.identifiers == data->identifiers && info.sites == data->sites && info.numSlots == numSlots && (!info.executableAndKind || info.executable() == executable));
    return data;
}

Data* Instance::ensureData(uint32_t index)
{
    Data* data = this->data[index];
    RELEASE_ASSERT(data);
    if (data != sharedData)
        return data;
    const FunctionInfo& info = infos[index];
    auto* executable = uncheckedDowncast<FunctionExecutable>(info.executable());
    UnlinkedFunctionCodeBlock* unlinkedCodeBlock = executable->unlinkedExecutable()->codeBlockIfThereIsOne(info.kind());
    // (An executable that was made when the program was built has no way of holding on to one.)
    Ref<JITCode> code = executable->hasJITCodeFor(info.kind()) ? Ref { static_cast<JITCode&>(executable->generatedJITCodeFor(info.kind()).get()) } : codeFromImage({ &Image::of(*info.function), info.function }, unlinkedCodeBlock);
    RELEASE_ASSERT(code->header().index == index);
    code->setInstance(*this);
    this->data[index] = nullptr;
    s_gotDataLater++;
    data = Data::create(*this, executable, unlinkedCodeBlock, code.get());
    RELEASE_ASSERT(data); // Nothing that it is made of is made now.
    return data;
}

FunctionRef FunctionRef::of(const CallFrame* callFrame)
{
    return { std::bit_cast<Instance*>(callFrame->unsafeCodeBlock()), CodeHeader::fromCallee(callFrame->rawCallee())->index };
}

Data* FunctionRef::dataIfItHasAny() const
{
    Data* data = instance->data[index];
    return data == instance->sharedData ? nullptr : data;
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
    if (!(info.flags & FunctionInfo::hasSiteConstants))
        return 0;
    return reinterpret_cast<const uint32_t*>(info.sites + info.numSlots)[slot - (SharedData::contains(slot) ? instance->sharedData : instance->data[index])->slots];
}

UnlinkedCodeBlock* FunctionRef::unlinkedCodeBlock() const
{
    if (Data* data = dataIfItHasAny())
        return data->unlinkedCodeBlock;
    return uncheckedDowncast<FunctionExecutable>(executable())->unlinkedExecutable()->codeBlockIfThereIsOne(info().kind());
}

void Data::destroy(Data* data)
{
    Instance& instance = *data->instance;
    Data*& place = instance.data[data->code->header().index];
    RELEASE_ASSERT(place == data);
    place = nullptr;
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
    if (data->ownsConstants)
        fastFree(const_cast<void*>(data->constants));
    if (data->functions)
        fastFree(data->functions);
    data->code->deref();
    fastFree(data);
}

CodeBlock* Data::ensureCodeBlock()
{
    if (codeBlock)
        return codeBlock;
    VM& vm = *instance->vm;
    RELEASE_ASSERT(unlinkedCodeBlock->codeType() == FunctionCode);
    DeferGCForAWhile deferGC(vm);
    // Whoever asks may well be dealing with an exception, and this is no place to find out that the VM has been told to stop.
    DeferTerminationForAWhile deferTermination(vm);
    SuspendExceptionScope suspendExceptions(vm);
    FunctionCodeBlock* result = FunctionCodeBlock::create(vm, uncheckedDowncast<FunctionExecutable>(executable), uncheckedDowncast<UnlinkedFunctionCodeBlock>(unlinkedCodeBlock), instance->globalObject, CodeBlock::LinkMode::ForCodeFromImage);
    RELEASE_ASSERT(result);
    result->adoptAOTCode(*code, this);
    codeBlock = result;
    if (Options::aotReportStats()) [[unlikely]] {
        // TEMPORARY-FUNCTION-STATS
        static std::atomic<unsigned> count;
        unsigned now = ++count;
        if (!(now & (now - 1)) || !(now % 500))
            dataLogLn("AOT: ", now, " CodeBlocks made because somebody asked");
    }
    if (!hasBeenFilledSinceLastCollection)
        noteFilled();
    return result;
}

LineColumn FunctionRef::lineColumnFor(BytecodeIndex bytecodeIndex) const
{
    UnlinkedCodeBlock* unlinkedCodeBlock = this->unlinkedCodeBlock();
    ScriptExecutable* executable = this->executable();
    RELEASE_ASSERT(bytecodeIndex.offset() < unlinkedCodeBlock->instructions().size());
    auto lineColumn = unlinkedCodeBlock->lineColumnForBytecodeIndex(bytecodeIndex);
    lineColumn.column += lineColumn.line ? 1 : executable->startColumn();
    lineColumn.line += executable->firstLine();
    return lineColumn;
}

static FunctionExecutable* functionOf(Data& data, unsigned index, UnlinkedFunctionExecutable* unlinkedExecutable)
{
    if (FunctionExecutable* result = unlinkedExecutable->staticExecutable(); result && StaticHeap::contains(data.executable))
        return result;
    UnlinkedCodeBlock* unlinkedCodeBlock = data.unlinkedCodeBlock;
    if (!data.functions)
        data.functions = static_cast<FunctionExecutable**>(fastZeroedMalloc((unlinkedCodeBlock->numberOfFunctionDecls() + unlinkedCodeBlock->numberOfFunctionExprs()) * sizeof(FunctionExecutable*)));
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
    if (unlinkedCodeBlock->codeType() != FunctionCode)
        return codeBlock->functionDecl(index);
    return functionOf(*this, index, unlinkedCodeBlock->functionDecl(index));
}

FunctionExecutable* Data::functionExpr(unsigned index)
{
    if (unlinkedCodeBlock->codeType() != FunctionCode)
        return codeBlock->functionExpr(index);
    return functionOf(*this, unlinkedCodeBlock->numberOfFunctionDecls() + index, unlinkedCodeBlock->functionExpr(index));
}

FunctionExecutable* FunctionRef::functionDecl(unsigned index) const
{
    if (!dataIfItHasAny()) {
        if (FunctionExecutable* result = unlinkedCodeBlock()->functionDecl(index)->staticExecutable())
            return result;
    }
    return ensureData()->functionDecl(index);
}

FunctionExecutable* FunctionRef::functionExpr(unsigned index) const
{
    if (!dataIfItHasAny()) {
        if (FunctionExecutable* result = unlinkedCodeBlock()->functionExpr(index)->staticExecutable())
            return result;
    }
    return ensureData()->functionExpr(index);
}

bool install(VM& vm, FunctionExecutable* executable, CodeSpecializationKind kind, UnlinkedCodeBlock* unlinkedCodeBlock, JSGlobalObject* globalObject, Ref<JITCode>&& code)
{
    Instance& instance = Instance::ensure(globalObject);
    code->setInstance(instance);
    uint32_t index = code->header().index;
    if (instance.data[index])
        RELEASE_ASSERT((FunctionRef { &instance, index }.executable() == executable));
    else if (code->imageFunction() && code->imageFunction()->startsCold && Options::aotStartFunctionsCold() && !instance.infos[index].sites && constantsAreOfNoRealm(unlinkedCodeBlock)) {
        if (!instance.collections->sizeOfInfos && Options::aotVerbose()) [[unlikely]]
            dataLogLn("AOT: nothing was known of function ", index, " when the program was built");
        fillInfo(instance.infos[index], executable, unlinkedCodeBlock, code.get(), unlinkedCodeBlock->constantRegisters().span().data());
        instance.infos[index].flags |= FunctionInfo::startsCold;
        instance.collections->executablesWithoutData.append(executable);
        instance.data[index] = instance.sharedData;
        didStartCold();
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
    if (instance->data[index])
        return true;
    if (const FunctionInfo& info = instance->infos[index]; info.flags & FunctionInfo::startsCold && Options::aotStartFunctionsCold()) {
        RELEASE_ASSERT(info.executable() == executable && info.kind() == kind);
        if (info.function->usesStaticImports && !moduleIsLinkedAsCompiled(scope))
            return false;
        instance->data[index] = instance->sharedData;
        didStartCold();
        return true;
    }
    UnlinkedFunctionCodeBlock* unlinkedCodeBlock = executable->unlinkedExecutable()->codeBlockIfThereIsOne(kind);
    RELEASE_ASSERT(unlinkedCodeBlock);
    ImageCode found = findInImage(executable, kind, unlinkedCodeBlock, scope);
    if (!found)
        return false;
    Ref<JITCode> code = codeFromImage(found, unlinkedCodeBlock);
    RELEASE_ASSERT(code->header().index == executable->aotIndexFor(kind));
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
    visitor.appendUnbarriered(unlinkedCodeBlock);
    if (codeBlock)
        visitor.appendUnbarriered(codeBlock);
    if (functions) {
        for (unsigned i = unlinkedCodeBlock->numberOfFunctionDecls() + unlinkedCodeBlock->numberOfFunctionExprs(); i--;) {
            if (functions[i])
                visitor.appendUnbarriered(functions[i]);
        }
    }
    if (ownsConstants) {
        auto* values = static_cast<const WriteBarrier<Unknown>*>(constants);
        for (unsigned i = unlinkedCodeBlock->constantRegisters().size(); i--;)
            visitor.appendUnbarriered(values[i].get());
    }
    // Code that has cached a transition can put an object that has already been visited in the new structure.
    for (unsigned i = 0; i < numSlots; ++i) {
        Slot& slot = slots[i];
        StructureID oldStructureID = slot.structureID;
        StructureID newStructureID = slot.newStructureID;
        if (!oldStructureID || !newStructureID || slot.unused || slot.hasPointer())
            continue;
        if (visitor.isMarked(oldStructureID.decode()))
            visitor.appendUnbarriered(newStructureID.decode());
    }
}

// What was there at the last collection and has not been filled since refers to nothing that is young.
template<typename Visitor>
void Instance::visit(Visitor& visitor, bool onlyWhatIsNew)
{
    for (Data* data : onlyWhatIsNew ? collections->filledSinceLastCollection : collections->all)
        data->visit(visitor);
    for (Structure* structure : collections->shapes.values()) {
        if (structure)
            visitor.appendUnbarriered(structure);
    }
    for (Structure* structure : collections->knownShapes.values())
        visitor.appendUnbarriered(structure);
    for (ScriptExecutable* executable : collections->executablesWithoutData)
        visitor.appendUnbarriered(executable);
}

// TEMPORARY-SLOT-STATS
void Instance::dumpSlotStatistics()
{
    uint64_t total[numOpcodeIDs] = { }, filled[numOpcodeIDs] = { }, withKnownShape[numOpcodeIDs] = { }, withPointer[numOpcodeIDs] = { };
    uint64_t functions = 0, functionsWithNothingFilled = 0, slots = 0;
    for (Data* data : collections->all) {
        functions++;
        slots += data->numSlots;
        bool any = false;
        OpcodeID last = op_nop;
        auto& instructions = data->unlinkedCodeBlock->instructions();
        for (unsigned i = 0; i < data->numSlots; ++i) {
            uint32_t bits = data->sites[i].callSiteBits;
            OpcodeID opcode = bits || !i ? instructions.at(CallSiteIndex(bits).bytecodeIndex().offset())->opcodeID() : last;
            last = opcode;
            total[opcode]++;
            Slot& slot = data->slots[i];
            auto* words = reinterpret_cast<uint64_t*>(&slot);
            if (!words[0] && !words[1])
                continue;
            any = true;
            filled[opcode]++;
            if ((opcode == op_get_by_id || opcode == op_put_by_id) && slot.structureID && slot.structureID.decode()->knownShape())
                withKnownShape[opcode]++;
            if (slot.hasPointer())
                withPointer[opcode]++;
        }
        functionsWithNothingFilled += !any;
    }
    {
        // TEMPORARY-SLOT-STATS: who has a Data.
        uint64_t count[4] = { }, slotsOf[4] = { }, filledOf[4] = { };
        for (Data* data : collections->all) {
            const FunctionInfo& info = infos[data->code->header().index];
            bool hasLoop = false;
            if (data->unlinkedCodeBlock->codeType() == FunctionCode && !(info.flags & FunctionInfo::startsCold)) {
                for (const auto& instruction : data->unlinkedCodeBlock->instructions())
                    hasLoop |= instruction->opcodeID() == op_loop_hint;
            }
            unsigned kind = data->unlinkedCodeBlock->codeType() != FunctionCode ? 0 : info.flags & FunctionInfo::startsCold ? 1 : hasLoop ? 2 : 3;
            count[kind]++;
            slotsOf[kind] += data->numSlots;
            for (unsigned i = 0; i < data->numSlots; ++i) {
                auto* words = reinterpret_cast<uint64_t*>(&data->slots[i]);
                filledOf[kind] += words[0] || words[1];
            }
        }
        static constexpr ASCIILiteral names[] = { "the code of a module"_s, "started cold"_s, "has a loop"_s, "other"_s };
        for (unsigned i = 0; i < 4; ++i)
            dataLogLn("WHOHASDATA ", names[i], ": functions=", count[i], " slots=", slotsOf[i], " filled=", filledOf[i]);
    }
    dataLogLn("SLOTS functions=", functions, " ofWhichNothingFilled=", functionsWithNothingFilled, " slots=", slots, " bytes=", slots * sizeof(Slot), " knownShapesMade=", collections->knownShapes.size(), " otherLiteralShapes=", collections->shapes.size());
    for (unsigned i = 0; i < numOpcodeIDs; ++i) {
        if (total[i])
            dataLogLn("SLOTS ", opcodeNames[i], " total=", total[i], " filled=", filled[i], " knownShape=", withKnownShape[i], " withPointer=", withPointer[i]);
    }
}

// TEMPORARY-SHAPE-STATS
static UncheckedKeyHashMap<Structure*, uint8_t>& knownShapes()
{
    static NeverDestroyed<UncheckedKeyHashMap<Structure*, uint8_t>> shapes;
    return shapes;
}
void noteKnownShape(Structure* structure, uint8_t kind)
{
    if (Options::aotReportSlowPaths()) [[unlikely]]
        knownShapes().add(structure, kind);
}
uint8_t kindOfKnownShape(Structure* structure) { return knownShapes().get(structure); }

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
    Structure* result = Structure::createWithProperties(*vm, empty, names);
    RELEASE_ASSERT(result);
    result->setKnownShape(*vm, safeCast<uint16_t>(shape));
    collections->knownShapes.add(shape, result);
    noteKnownShape(result, 1);
    return result;
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
    noteKnownShape(result, 1);
    collections->shapes.add(WTF::move(key), result); // (The structure keeps the names.)
    return result;
}
template void Instance::visit(AbstractSlotVisitor&, bool);
template void Instance::visit(SlotVisitor&, bool);

void Instance::finalizeUnconditionally(bool onlyWhatIsNew)
{
    for (Data* data : onlyWhatIsNew ? collections->filledSinceLastCollection : collections->all)
        data->finalizeUnconditionally(*vm);
    for (Data* data : collections->filledSinceLastCollection)
        data->hasBeenFilledSinceLastCollection = false;
    collections->filledSinceLastCollection.shrink(0);
}

void Data::finalizeUnconditionally(VM& vm)
{
    for (unsigned i = 0; i < numSlots; ++i) {
        Slot& slot = slots[i];
        if (!slot.structureID)
            continue;
        bool dead = !vm.heap.isMarked(slot.structureID.decode());
        if (!dead) {
            if (slot.offset & Slot::pointerIsCell)
                dead = !vm.heap.isMarked(static_cast<JSCell*>(slot.pointer));
            else if (!slot.hasPointer() && slot.newStructureID && !slot.unused) // A scope cache may have an untagged address here.
                dead = !vm.heap.isMarked(slot.newStructureID.decode());
        }
        if (dead) {
            slot.clear();
            slotEpoch++;
        }
    }

    if (watchpoints) {
        watchpoints->removeIf([&](auto& entry) {
            Slot& slot = slots[entry.key];
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

JITCode::JITCode(void* code, RefPtr<ExecutableMemoryHandle>&& handle, CompiledFunctionInfo&& info, Way way)
    : JSC::JITCode(JITType::AOTJIT, CodePtr<JSEntryPtrTag>::fromTaggedPtr(tagCodePtr<JSEntryPtrTag>(addressOfStub(stubFor(way)))), handle ? ShareAttribute::NotShared : ShareAttribute::Shared)
    , m_code(code)
    , m_entry(tagCodePtr<JSEntryPtrTag>(static_cast<uint8_t*>(code) + info.arityCheckOffset))
    , m_owned(makeUnique<Owned>(WTF::move(handle), WTF::move(info)))
{
    m_calleeSaveRegisters = &m_owned->info.calleeSaveRegisters;
}

// There are as many of these as there are ways to pick the first few of the registers that a callee saves.
static const RegisterAtOffsetList* calleeSaveRegistersOf(const ImageFunction& function)
{
    static Lock lock;
    static NeverDestroyed<UncheckedKeyHashMap<uint64_t, Vector<std::unique_ptr<RegisterAtOffsetList>, 1>>> lists;

    auto* saves = function.calleeSaves();
    uint64_t key = function.numberOfCalleeSaves ? static_cast<uint32_t>(saves[0].offset) : 0;
    for (unsigned i = 0; i < function.numberOfCalleeSaves; ++i)
        key ^= 1ULL << (saves[i].reg & 63);
    key = key * 2 + 1; // Not one of the two that a table has a use for.
    auto isThat = [&](const RegisterAtOffsetList& list) {
        if (list.registerCount() != function.numberOfCalleeSaves)
            return false;
        for (unsigned i = 0; i < function.numberOfCalleeSaves; ++i) {
            if (list.at(i).reg().index() != saves[i].reg || list.at(i).offset() != saves[i].offset)
                return false;
        }
        return true;
    };

    Locker locker { lock };
    auto& candidates = lists->add(key, Vector<std::unique_ptr<RegisterAtOffsetList>, 1>()).iterator->value;
    for (auto& candidate : candidates) {
        if (isThat(*candidate))
            return candidate.get();
    }
    // Made the way Air::Code makes it: the registers in their order, next to each other, somewhere in the frame.
    RegisterSet registers;
    for (unsigned i = 0; i < function.numberOfCalleeSaves; ++i)
        registers.add(Reg::fromIndex(saves[i].reg), IgnoreVectors);
    auto list = makeUnique<RegisterAtOffsetList>(registers);
    if (function.numberOfCalleeSaves)
        list->adjustOffsets(saves[0].offset - list->at(0).offset());
    RELEASE_ASSERT(isThat(*list));
    candidates.append(WTF::move(list));
    return candidates.last().get();
}

JITCode::JITCode(void* code, const ImageFunction& function, Way way)
    // Code in an image is nobody's memory: the collector, which paces itself by what the heap holds on to, is not to count it.
    : JSC::JITCode(JITType::AOTJIT, CodePtr<JSEntryPtrTag>::fromTaggedPtr(tagCodePtr<JSEntryPtrTag>(addressOfStub(stubFor(way)))), ShareAttribute::Shared)
    , m_code(code)
    , m_entry(tagCodePtr<JSEntryPtrTag>(static_cast<uint8_t*>(code) + function.arityCheckOffset))
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
size_t JITCode::size() { return codeSize(); }

bool JITCode::contains(void* address)
{
    return address >= m_code && address < static_cast<uint8_t*>(m_code) + codeSize();
}

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)
