/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRuntimeTable.h"

#if ENABLE(AOT)

#include "AOTArrivalCounts.h"
#include "AOTCompactTable.h"
#include "AOTGenerator.h"
#include "AOTGuardedPages.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTHost.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTNativeCallValidation.h"
#include "AOTState.h"
#include "AOTStubFrontEnds.h"
#include "AOTStubsExceptions.h"
#include "AOTVMState.h"
#include "DFGOperations.h"
#include "ErrorConstructor.h"
#include "FrameTracers.h"
#include "JSAsyncGenerator.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "LLIntData.h"
#include "LLIntThunks.h"
#include "Opcode.h"
#include "Options.h"
#include "WebAssemblyFunction.h"
#include <array>
#include <bit>
#include <type_traits>
#include <wtf/DataLog.h>
#include <wtf/OSAllocator.h>
#include <wtf/PageBlock.h>
#include <wtf/TZoneMallocInlines.h>

#if !ENABLE(DFG_JIT)
// The two operations of the DFG that compiled code calls, for an engine that has no DFG.
namespace JSC {

JSC_DECLARE_JIT_OPERATION(operationArrayIncludesValueInt32OrContiguous, UCPUStrictInt32, (JSGlobalObject*, Butterfly*, EncodedJSValue, int32_t));
JSC_DECLARE_JIT_OPERATION(operationArrayIndexOfValueInt32OrContiguous, UCPUStrictInt32, (JSGlobalObject*, Butterfly*, EncodedJSValue, int32_t));

static ALWAYS_INLINE UCPUStrictInt32 arrayIncludesString(JSGlobalObject* globalObject, Butterfly* butterfly, JSString* searchElement, int32_t index)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    int32_t length = butterfly->publicLength();
    auto data = butterfly->contiguous().data();
    for (; index < length; ++index) {
        JSValue value = data[index].get();
        if (!value || !value.isString())
            continue;
        auto* string = asString(value);
        if (string == searchElement)
            return toUCPUStrictInt32(1);
        if (string->equalInline(globalObject, searchElement)) {
            scope.assertNoExceptionExceptTermination();
            return toUCPUStrictInt32(1);
        }
        RETURN_IF_EXCEPTION(scope, { });
    }
    return toUCPUStrictInt32(0);
}

JSC_DEFINE_JIT_OPERATION(operationArrayIncludesValueInt32OrContiguous, UCPUStrictInt32, (JSGlobalObject* globalObject, Butterfly* butterfly, EncodedJSValue encodedValue, int32_t index))
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSValue searchElement = JSValue::decode(encodedValue);

    if (searchElement.isString())
        OPERATION_RETURN(scope, arrayIncludesString(globalObject, butterfly, asString(searchElement), index));

    int32_t length = butterfly->publicLength();
    auto data = butterfly->contiguous().data();

    if (index >= length)
        OPERATION_RETURN(scope, toUCPUStrictInt32(0));

    if (searchElement.isObject()) {
        auto* result = std::bit_cast<const WriteBarrier<Unknown>*>(WTF::find64(std::bit_cast<const uint64_t*>(data + index), encodedValue, length - index));
        if (result)
            OPERATION_RETURN(scope, toUCPUStrictInt32(1));
        OPERATION_RETURN(scope, toUCPUStrictInt32(0));
    }

    if (searchElement.isInt32()) {
        for (; index < length; ++index) {
            JSValue value = data[index].get();
            if (!value || !value.isNumber())
                continue;
            if (searchElement.asInt32() == value.asNumber())
                OPERATION_RETURN(scope, toUCPUStrictInt32(1));
        }
        OPERATION_RETURN(scope, toUCPUStrictInt32(0));
    }

    bool searchElementIsUndefined = searchElement.isUndefined();
    for (; index < length; ++index) {
        JSValue value = data[index].get();
        if (!value) {
            if (searchElementIsUndefined)
                OPERATION_RETURN(scope, 1);
            continue;
        }
        bool isEqual = sameValueZero(globalObject, searchElement, value);
        OPERATION_RETURN_IF_EXCEPTION(scope, 0);
        if (isEqual)
            OPERATION_RETURN(scope, toUCPUStrictInt32(1));
    }
    OPERATION_RETURN(scope, toUCPUStrictInt32(0));
}

static ALWAYS_INLINE UCPUStrictInt32 arrayIndexOfString(JSGlobalObject* globalObject, Butterfly* butterfly, JSString* searchElement, int32_t index)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    int32_t length = butterfly->publicLength();
    auto data = butterfly->contiguous().data();
    for (; index < length; ++index) {
        JSValue value = data[index].get();
        if (!value || !value.isString())
            continue;
        auto* string = asString(value);
        if (string == searchElement)
            return toUCPUStrictInt32(index);
        if (string->equalInline(globalObject, searchElement)) {
            scope.assertNoExceptionExceptTermination();
            return toUCPUStrictInt32(index);
        }
        RETURN_IF_EXCEPTION(scope, { });
    }
    return toUCPUStrictInt32(-1);
}

JSC_DEFINE_JIT_OPERATION(operationArrayIndexOfValueInt32OrContiguous, UCPUStrictInt32, (JSGlobalObject* globalObject, Butterfly* butterfly, EncodedJSValue encodedValue, int32_t index))
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSValue searchElement = JSValue::decode(encodedValue);

    if (searchElement.isString())
        OPERATION_RETURN(scope, arrayIndexOfString(globalObject, butterfly, asString(searchElement), index));

    int32_t length = butterfly->publicLength();
    auto data = butterfly->contiguous().data();

    if (index >= length)
        OPERATION_RETURN(scope, toUCPUStrictInt32(-1));

    if (searchElement.isObject()) {
        auto* result = std::bit_cast<const WriteBarrier<Unknown>*>(WTF::find64(std::bit_cast<const uint64_t*>(data + index), encodedValue, length - index));
        if (result)
            OPERATION_RETURN(scope, toUCPUStrictInt32(result - data));
        OPERATION_RETURN(scope, toUCPUStrictInt32(-1));
    }

    if (searchElement.isInt32()) {
        for (; index < length; ++index) {
            JSValue value = data[index].get();
            if (!value || !value.isNumber())
                continue;
            if (searchElement.asInt32() == value.asNumber())
                OPERATION_RETURN(scope, toUCPUStrictInt32(index));
        }
        OPERATION_RETURN(scope, toUCPUStrictInt32(-1));
    }

    for (; index < length; ++index) {
        JSValue value = data[index].get();
        if (!value)
            continue;
        bool isEqual = JSValue::strictEqual(globalObject, searchElement, value);
        OPERATION_RETURN_IF_EXCEPTION(scope, 0);
        if (isEqual)
            OPERATION_RETURN(scope, toUCPUStrictInt32(index));
    }
    OPERATION_RETURN(scope, toUCPUStrictInt32(-1));
}

} // namespace JSC
#endif // !ENABLE(DFG_JIT)

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

template<typename First>
static ALWAYS_INLINE void countCall(First first, Entry entry)
{
    vmFor(first).aotState().runtimeTable->didCall(entry);
    if constexpr (std::is_same_v<First, Instance*>) {
        if (ArrivalCounts* arrivals = first->state ? first->state->arrivalCounts.get() : nullptr)
            arrivals->didCallOperation(entry);
    }
}

template<Entry entry, auto operation, typename Function>
struct CountedCall {
    static constexpr auto call = operation;
};

template<Entry entry, auto operation, typename Result, typename First, typename... Rest>
    requires CanFindVM<First>
struct CountedCall<entry, operation, Result(First, Rest...)> {
    static Result JIT_OPERATION_ATTRIBUTES call(First first, Rest... rest)
    {
        countCall(first, entry);
        MUST_TAIL_CALL return operation(first, rest...);
    }
};

template<Entry entry, auto operation, typename Result, typename First, typename... Rest>
    requires CanFindVM<First>
struct CountedCall<entry, operation, Result(First, Rest...) noexcept> {
    static Result JIT_OPERATION_ATTRIBUTES call(First first, Rest... rest) noexcept
    {
        countCall(first, entry);
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
    fillOperations(m_entries, m_countsCalls);
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

const Identifier& RuntimeTable::nameOfApplyTrap(VM& vm)
{
    if (m_nameOfApplyTrap.isNull()) [[unlikely]]
        m_nameOfApplyTrap = Identifier::fromString(vm, "apply"_s);
    return m_nameOfApplyTrap;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
