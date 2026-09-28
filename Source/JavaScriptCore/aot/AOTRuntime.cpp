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
    size_t environmentsSize { 0 }; // Rounded up to whole pages.
    size_t sizeFromInstance { 0 };
};

static_assert(Instance::offsetOfVM() == 16, "JSWebAssemblyInstance::offsetOfVM()");

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
    globalObject->setAOTInstance(instance);
    vm.m_aotInstances.append(instance);
    return *instance;
}

void Instance::destroy(Instance* instance)
{
    instance->vm->m_aotInstances.removeFirst(instance);
    while (!instance->collections->all.isEmpty())
        Data::destroy(instance->collections->all.last());
    size_t environmentsSize = instance->collections->environmentsSize;
    size_t size = instance->collections->sizeFromInstance;
    delete instance->collections;
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
    return dataOf(callFrame)->ensureCodeBlock();
}

JSObject* calleeOf(const CallFrame* callFrame)
{
    return callFrame->registers()[CodeHeader::fromCallee(callFrame->rawCallee())->calleeSlot].object();
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
    data->numSlots = numSlots;
    data->slotEpoch = 1;

    RELEASE_ASSERT(sizeof(Instance) + (code.header().index + 1) * sizeof(Data*) <= instance.collections->sizeFromInstance);
    Data*& place = instance.data[code.header().index];
    RELEASE_ASSERT(!place);
    place = data;
    instance.collections->all.append(data);
    data->noteFilled(); // It is new.

    if (codeBlock)
        data->constants = codeBlock->constantRegisters().span().data();
    else if (!linkConstants(vm, *data)) {
        destroy(data);
        return nullptr;
    }
    return data;
}

void Data::destroy(Data* data)
{
    Instance& instance = *data->instance;
    Data*& place = instance.data[data->code->header().index];
    RELEASE_ASSERT(place == data);
    place = nullptr;
    instance.collections->all.removeFirst(data);
    if (data->hasBeenFilledSinceLastCollection)
        instance.collections->filledSinceLastCollection.removeFirst(data);
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

LineColumn Data::lineColumnFor(BytecodeIndex bytecodeIndex) const
{
    RELEASE_ASSERT(bytecodeIndex.offset() < unlinkedCodeBlock->instructions().size());
    auto lineColumn = unlinkedCodeBlock->lineColumnForBytecodeIndex(bytecodeIndex);
    lineColumn.column += lineColumn.line ? 1 : executable->startColumn();
    lineColumn.line += executable->firstLine();
    return lineColumn;
}

static FunctionExecutable* functionOf(Data& data, unsigned index, UnlinkedFunctionExecutable* unlinkedExecutable)
{
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

bool install(VM& vm, FunctionExecutable* executable, CodeSpecializationKind kind, UnlinkedCodeBlock* unlinkedCodeBlock, JSGlobalObject* globalObject, Ref<JITCode>&& code)
{
    Instance& instance = Instance::ensure(globalObject);
    code->setInstance(instance);
    Data* data = instance.data[code->header().index];
    if (!data) {
        if (!Data::create(instance, executable, unlinkedCodeBlock, code.get()))
            return false;
    } else
        RELEASE_ASSERT(data->executable == executable);
    executable->installAOTCode(vm, kind, WTF::move(code));
    return true;
}

void Data::noteFilled()
{
    hasBeenFilledSinceLastCollection = true;
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
