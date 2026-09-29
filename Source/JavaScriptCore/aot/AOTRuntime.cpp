/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRuntime.h"

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
    set(Entry::NativeCallTrampoline, LLInt::getCodePtr<JSEntryPtrTag>(llint_native_call_trampoline).taggedPtr());

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
    if (Image::environmentsSize() && !vm.m_aotInstanceOfProgram && StaticHeap::canPlaceCellsOf(vm)) {
        // Where cells can be that the collector did not allocate.
        environmentsSize = roundUpToMultipleOf(WTF::pageSize(), Image::environmentsSize());
        size = roundUpToMultipleOf(WTF::pageSize(), sizeof(Instance) + Image::numberOfFunctionsOfImageWithEnvironments() * sizeof(Data*));
        instance = reinterpret_cast<Instance*>(static_cast<char*>(StaticHeap::allocateBlock(vm, environmentsSize + size)) + environmentsSize);
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
    instance->factsOfFunctions = environmentsSize ? StaticHeap::factsOfFunctions(vm) : nullptr;
    instance->constantsOfProgram = environmentsSize ? StaticHeap::constantsOfProgram(vm) : nullptr;
    instance->sharedData = SharedData::get();
    instance->missesForEightSlots = Options::aotMissesForEightSlots();
    instance->missesToSpare = Options::aotMissesToSpare();
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
    info.flags = (!code.isFromImage() ? 0 : code.imageFunction()->hasSiteConstants ? FunctionInfo::hasSiteConstants : FunctionInfo::sitesHaveTheirConstants) | FunctionInfo::slotsAmongFlags(code.numSlots());
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
    // (Of a program that was put together with all it needs, they are what its FunctionInfo says. The unlinked code, if there is
    // any, may have been decoded since, and has the same in a place of its own.)
    FunctionInfo& info = instance.infos[code.header().index];
    data->identifiers = info.sites ? info.identifiers : unlinkedCodeBlock->identifiers().span().data();
    data->sites = code.sites();
    data->hasSiteConstants = code.isFromImage() && code.imageFunction()->hasSiteConstants;
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
    if (!info.sites) {
        if (!instance.collections->sizeOfInfos && Options::aotVerbose()) [[unlikely]]
            dataLogLn("AOT: nothing was known of function ", code.header().index, " when the program was built");
        fillInfo(info, executable, unlinkedCodeBlock, code, data->constants);
    }
    RELEASE_ASSERT(info.identifiers == data->identifiers && info.sites == data->sites && (info.flags >> FunctionInfo::numberOfFlagBits) == std::min<uint32_t>(numSlots, FunctionInfo::mostSlotsSaid) && (!info.executable() || info.executable() == executable));
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
    Ref<JITCode> code = executable->hasJITCodeFor(info.kind()) ? Ref { static_cast<JITCode&>(executable->generatedJITCodeFor(info.kind()).get()) } : codeOfFunctionFromImage({ &Image::of(*info.function()), info.function() }, info.kind());
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

FunctionRef FunctionRef::of(VM& vm, FunctionExecutable* executable, CodeSpecializationKind kind)
{
    if (StaticHeap::contains(executable) && executable->aotIndexFor(kind) != FunctionExecutable::aotIndexOfWhatConstructsByCalling)
        return { vm.m_aotInstanceOfProgram, executable->aotIndexFor(kind) };
    if (!executable->hasJITCodeFor(kind) || executable->generatedJITCodeFor(kind)->jitType() != JITType::AOTJIT)
        return { };
    auto& code = static_cast<JITCode&>(executable->generatedJITCodeFor(kind).get());
    return { code.instance(), code.header().index };
}

CodeBlock* FunctionRef::ensureCodeBlock() const
{
    return ensureData()->ensureCodeBlock();
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
    size_t which = slot - (SharedData::contains(slot) ? instance->sharedData : instance->data[index])->slots;
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
    auto* code = static_cast<JITCode*>(codeBlock->jitCode().get());
    return code->instance() ? FunctionRef { code->instance(), code->header().index } : FunctionRef { };
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

// TEMPORARY-FUNCTION-STATS
static void reportWhoAsks(ASCIILiteral what)
{
#if OS(DARWIN)
    if (!getenv("BUN_AOT_WHO_ASKS"))
        return;
    void* stack[14];
    int depth = backtrace(stack, 14);
    char** symbols = backtrace_symbols(stack, depth);
    StringPrintStream out;
    out.print("WHOASKS ", what);
    for (int i = 2; i < depth; ++i) {
        // "3   cli   0x0000000104f2c1a4 symbol + 123"
        const char* symbol = symbols[i];
        for (unsigned field = 0; field < 3 && *symbol; ++field) {
            while (*symbol && *symbol != ' ')
                ++symbol;
            while (*symbol == ' ')
                ++symbol;
        }
        const char* end = strchr(symbol, ' ');
        out.print(" ", String::fromUTF8(std::span { symbol, end ? static_cast<size_t>(end - symbol) : strlen(symbol) }));
    }
    free(symbols);
    dataLogLn(out.toCString());
#else
    UNUSED_PARAM(what);
#endif
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
    reportWhoAsks("unlinked"_s);
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
    if (Options::aotReportStats()) [[unlikely]] {
        // TEMPORARY-FUNCTION-STATS
        static std::atomic<unsigned> count;
        unsigned now = ++count;
        if (!(now & (now - 1)) || !(now % 500))
            dataLogLn("AOT: the unlinked code of ", now, " functions decoded because somebody asked");
    }
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
    // (Where the function starts.)
    readVarint(at);
    readVarint(at);
    ReportedPosition result;
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

FunctionRef Data::function() const
{
    return { instance, code->header().index };
}

CodeBlock* Data::ensureCodeBlock()
{
    if (codeBlock)
        return codeBlock;
    VM& vm = *instance->vm;
    reportWhoAsks("codeblock"_s);
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
    if (index == FunctionExecutable::aotIndexOfWhatConstructsByCalling)
        return true;
    if (instance->data[index])
        return true;
    if (const FunctionInfo& info = instance->infos[index]; info.flags & FunctionInfo::startsCold && Options::aotStartFunctionsCold()) {
        RELEASE_ASSERT(info.executable() == executable && info.kind() == kind);
        if (info.function()->usesStaticImports && !moduleIsLinkedAsCompiled(scope))
            return false;
        instance->data[index] = instance->sharedData;
        didStartCold();
        return true;
    }
    UnlinkedFunctionCodeBlock* unlinkedCodeBlock = executable->unlinkedExecutable()->codeBlockIfThereIsOne(kind);
    ImageCode found = findInImage(executable, kind, unlinkedCodeBlock, scope);
    if (!found)
        return false;
    Ref<JITCode> code = codeOfFunctionFromImage(found, kind);
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

    uint64_t mask = ImageFunction::unpackRegisters(function.calleeSaveRegisters);
    ptrdiff_t offsetOfFirst = -static_cast<ptrdiff_t>(function.whereCalleeSavesStart * sizeof(CPURegister));
    uint64_t key = (static_cast<uint64_t>(function.whereCalleeSavesStart) << 32 | function.calleeSaveRegisters) * 2 + 1; // Not one of the two that a table has a use for.
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
    , m_entry(tagCodePtr<JSEntryPtrTag>(static_cast<uint8_t*>(code) + function.arityCheckOffset()))
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
