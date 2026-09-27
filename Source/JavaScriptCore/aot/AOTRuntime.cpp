/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRuntime.h"

#if ENABLE(FTL_JIT)

#include "AOTImage.h"
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
    set(Entry::ThrowStackOverflowError, tagCFunctionPtr<void*, OperationPtrTag>(operationThrowStackOverflowError));

    auto addCallLinkInfo = [&](Entry entry, CallLinkInfo::CallType type) {
        auto info = makeUnique<VirtualCallInfo>();
        static_assert(!OBJECT_OFFSETOF(VirtualCallInfo, callLinkInfo));
        info->callLinkInfo.initialize(vm, nullptr, type, CodeOrigin { });
        info->callLinkInfo.setVirtualCall(vm);
        info->findTarget = tagCFunctionPtr<void*, OperationPtrTag>(LLInt::llint_virtual_call);
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

Data* Data::create(VM& vm, CodeBlock* codeBlock, unsigned numSlots, const Site* sites)
{
    size_t size = sizeof(Data) + numSlots * sizeof(Slot);
    Data* data = static_cast<Data*>(fastZeroedMalloc(size));
    data->runtimeTable = AOT::runtimeTable(vm).entries();
    data->vm = &vm;
    data->globalObject = codeBlock->globalObject();
    data->constants = codeBlock->constantRegisters().span().data();
    data->identifiers = codeBlock->unlinkedCodeBlock()->identifiers().span().data();
    data->sites = sites;
    data->numSlots = numSlots;
    data->slotEpoch = 1;
    return data;
}

void Data::destroy(Data* data)
{
    delete data->watchpoints;
    fastFree(data);
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

JITCode::JITCode(void* code, RefPtr<ExecutableMemoryHandle>&& handle, CompiledFunctionInfo&& info)
    : JSC::JITCode(JITType::AOTJIT, CodePtr<JSEntryPtrTag>::fromTaggedPtr(tagCodePtr<JSEntryPtrTag>(static_cast<uint8_t*>(code) + info.entryOffset)), handle ? ShareAttribute::NotShared : ShareAttribute::Shared)
    , m_code(code)
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

JITCode::JITCode(void* code, const ImageFunction& function)
    // Code in an image is nobody's memory: the collector, which paces itself by what the heap holds on to, is not to count it.
    : JSC::JITCode(JITType::AOTJIT, CodePtr<JSEntryPtrTag>::fromTaggedPtr(tagCodePtr<JSEntryPtrTag>(static_cast<uint8_t*>(code) + function.entryOffset)), ShareAttribute::Shared)
    , m_code(code)
    , m_function(&function)
    , m_calleeSaveRegisters(calleeSaveRegistersOf(function))
{
}

JITCode::~JITCode() = default;

CodePtr<JSEntryPtrTag> JITCode::addressForCall(ArityCheckMode arity)
{
    unsigned offset = arity == ArityCheckMode::ArityCheckNotRequired ? entryOffset() : arityCheckOffset();
    return CodePtr<JSEntryPtrTag>::fromTaggedPtr(tagCodePtr<JSEntryPtrTag>(static_cast<uint8_t*>(m_code) + offset));
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
