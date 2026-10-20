/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRuntimeTable.h"

#if ENABLE(AOT)

#include "AOTCompactTable.h"
#include "AOTGenerator.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTHost.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTNativeCallValidation.h"
#include "AOTStubFrontEnds.h"
#include "AOTStubsExceptions.h"
#include "AOTVMState.h"
#include "DFGOperations.h"
#include "ErrorConstructor.h"
#include "JSAsyncGenerator.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "LLIntData.h"
#include "LLIntThunks.h"
#include "Opcode.h"
#include "Options.h"
#include "WebAssemblyFunction.h"
#include <bit>
#include <type_traits>
#include <wtf/DataLog.h>
#include <wtf/TZoneMallocInlines.h>

namespace JSC { namespace AOT {

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

#if HAVE_MUST_TAIL_CALL
static ALWAYS_INLINE VM& vmFor(VM* vm) { return *vm; }
static ALWAYS_INLINE VM& vmFor(Instance* instance) { return *instance->vm; }
static ALWAYS_INLINE VM& vmFor(JSGlobalObject* globalObject) { return globalObject->vm(); }
static ALWAYS_INLINE VM& vmFor(const JSCell* cell) { return cell->vm(); }

template<typename First>
concept CanFindVM = requires(First first) { vmFor(first); };

template<Entry entry, auto operation, typename Function>
struct CountedCall {
    static constexpr auto call = operation;
};

template<Entry entry, auto operation, typename Result, typename First, typename... Rest>
    requires CanFindVM<First>
struct CountedCall<entry, operation, Result(First, Rest...)> {
    static Result JIT_OPERATION_ATTRIBUTES call(First first, Rest... rest)
    {
        vmFor(first).aotState().runtimeTable->didCall(entry);
        MUST_TAIL_CALL return operation(first, rest...);
    }
};

template<Entry entry, auto operation, typename Result, typename First, typename... Rest>
    requires CanFindVM<First>
struct CountedCall<entry, operation, Result(First, Rest...) noexcept> {
    static Result JIT_OPERATION_ATTRIBUTES call(First first, Rest... rest) noexcept
    {
        vmFor(first).aotState().runtimeTable->didCall(entry);
        MUST_TAIL_CALL return operation(first, rest...);
    }
};

template<Entry entry, auto operation>
using CountedOperation = CountedCall<entry, operation, std::remove_pointer_t<decltype(operation)>>;
#endif

static void fillOperations(void** entries, bool countsCalls)
{
    using namespace DFG;
#define AOT_FILL_OPERATION(name) \
    entries[static_cast<unsigned>(Entry::name)] = operationAddress<Entry::name, &name>();
#define AOT_FILL_SCHEMA_OPERATION(name) AOT_FILL_OPERATION(operationAOT##name)
    FOR_EACH_AOT_RUNTIME_OPERATION(AOT_FILL_SCHEMA_OPERATION)
    FOR_EACH_AOT_OPERATION_OUTSIDE_SCHEMA(AOT_FILL_OPERATION)
#undef AOT_FILL_SCHEMA_OPERATION
#undef AOT_FILL_OPERATION
#if HAVE_MUST_TAIL_CALL
    if (countsCalls) [[unlikely]] {
#define AOT_COUNT_OPERATION(name) \
        entries[static_cast<unsigned>(Entry::name)] = operationAddress<Entry::name, CountedOperation<Entry::name, &name>::call>();
#define AOT_COUNT_SCHEMA_OPERATION(name) AOT_COUNT_OPERATION(operationAOT##name)
        FOR_EACH_AOT_RUNTIME_OPERATION(AOT_COUNT_SCHEMA_OPERATION)
        FOR_EACH_AOT_OPERATION_OUTSIDE_SCHEMA(AOT_COUNT_OPERATION)
#undef AOT_COUNT_SCHEMA_OPERATION
#undef AOT_COUNT_OPERATION
    }
#else
    UNUSED_PARAM(countsCalls);
#endif
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

static void trapUnusedEntries(void** entries, const Image& image)
{
    const uint32_t* entriesUsed = image.header().entriesUsed;
    auto trapUnlessUsed = [&](Entry entry) {
        unsigned index = static_cast<unsigned>(entry);
        if (!(entriesUsed[index / 32] >> index % 32 & 1))
            entries[index] = tagCFunctionPtr<void*, OperationPtrTag>(operationNotUsedByImage);
    };
    for (unsigned index = 0; index < numberOfEntries; ++index) {
        Entry entry = static_cast<Entry>(index);
        if (isOperation(entry))
            trapUnlessUsed(entry);
    }
    for (const OperationFrontEnd& row : operationFrontEnds())
        trapUnlessUsed(row.raw);
}

RuntimeTable::RuntimeTable(VM& vm)
    : m_countsCalls(HAVE_MUST_TAIL_CALL && Options::aotCountsPath())
{
    const Image& image = *vm.aotState().image;
    fillOperations(m_entries, m_countsCalls);
    fillThunkAndPointers(m_entries, image);
    installOperationFrontEnds(m_entries, image);
    trapUnusedEntries(m_entries, image);

    m_compactTableStructure = CompactTable::createStructure(vm);
    vm.aotState().caches->addStrongRoots("compact table structure"_s, { reinterpret_cast<JSCell* const*>(&m_compactTableStructure), 1 });
}

RuntimeTable::~RuntimeTable() = default;

void RuntimeTable::finishInitialization(VM& vm)
{
    void*& entry = m_entries[static_cast<unsigned>(Entry::CallLinkInfoForCall)];
    if (entry)
        return;
    m_callLinkInfoForCall.initialize(vm, nullptr, CallLinkInfo::Call, CodeOrigin { });
    m_callLinkInfoForCall.setVirtualCall(vm);
    entry = static_cast<CallLinkInfo*>(&m_callLinkInfoForCall);
}

uint64_t RuntimeTable::callCount(Entry entry) const
{
    return m_callCounts[static_cast<unsigned>(entry)];
}

ASCIILiteral nameOf(Entry entry)
{
    switch (entry) {
#define AOT_NAME_OF_ENTRY(name) case Entry::name: return #name ""_s;
#define AOT_NAME_OF_SCHEMA_ENTRY(name) case Entry::operationAOT##name: return "operationAOT" #name ""_s;
    FOR_EACH_AOT_RUNTIME_OPERATION(AOT_NAME_OF_SCHEMA_ENTRY)
    FOR_EACH_AOT_OPERATION_OUTSIDE_SCHEMA(AOT_NAME_OF_ENTRY)
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

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
