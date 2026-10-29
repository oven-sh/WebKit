/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTEffectsDeclared.h"
#include "AOTInstance.h"
#include "AOTObjectWords.h"
#include "AOTProgramIdentifiers.h"
#include "AOTRuntimeTable.h"
#include "AOTRuntimeValidation.h"
#include "AOTUnwinding.h"
#include "DeferGC.h"
#include "DeferTermination.h"
#include "FrameTracers.h"
#include "FreeList.h"
#include "JSCInlines.h"
#include "LocalAllocator.h"
#include "TopExceptionScope.h"
#include <bit>
#include <optional>
#include <type_traits>
#include <utility>

namespace JSC { namespace AOT {

class OperationValidationScope {
    WTF_FORBID_HEAP_ALLOCATION;
    WTF_MAKE_NONCOPYABLE(OperationValidationScope);
public:
    ALWAYS_INLINE OperationValidationScope(VM& vm, Entry entry)
        : m_vm(vm)
    {
#if ASSERT_ENABLED
        if (!permissionsOf(entry).mayCollect)
            m_assertNoGC.emplace();
#endif
        if (validatesRuntime(vm)) [[unlikely]]
            open(entry);
    }

    ALWAYS_INLINE ~OperationValidationScope()
    {
        if (m_isOpen) [[unlikely]]
            close();
    }

private:
    void open(Entry);
    void close();

    VM& m_vm;
    CollectionVersions m_collectionVersions;
    Entry m_entry;
    bool m_mayNotEnterVM { false };
    bool m_mayNotCollect { false };
    bool m_didEnterVMBefore { false };
    bool m_isOpen { false };
#if ASSERT_ENABLED
    std::optional<AssertNoGC> m_assertNoGC;
#endif
};

#if OS(WINDOWS) && CPU(X86_64)
// There __builtin_frame_address(0) is the low end of the frame. DECLARE_CALL_FRAME says why rbp is named.
#define AOT_FRAME_RECORD_OF_THIS_FUNCTION() \
    ({ \
        asm volatile("" : : : "rbp"); \
        std::bit_cast<JSC::CallerFrameAndPC*>(static_cast<void**>(_AddressOfReturnAddress()) - 1); \
    })
#else
#define AOT_FRAME_RECORD_OF_THIS_FUNCTION() std::bit_cast<JSC::CallerFrameAndPC*>(__builtin_frame_address(0))
#endif

// An operation finds its caller's frame directly above its own, so a wrapper enters the operation with a jump, not a call.
// MUST_TAIL_CALL expands to nothing on Windows, but clang guarantees the tail call there too when caller and callee have the same signature.
#define AOT_MUST_TAIL_CALL [[clang::musttail]]

#if ASSERT_ENABLED
// Where assertions are on, the run-time table calls an operation that is declared not to collect through a function that holds one of these.
// The frame of that function lies between the frame of the operation and the frame of the caller.
class NoCollectionGuard {
    WTF_FORBID_HEAP_ALLOCATION;
    WTF_MAKE_NONCOPYABLE(NoCollectionGuard);
public:
    explicit NoCollectionGuard(CallerFrameAndPC* frameRecord)
        : m_outerFrameRecord(std::exchange(s_frameRecord, frameRecord))
    {
    }

    ~NoCollectionGuard() { s_frameRecord = m_outerFrameRecord; }

    static ALWAYS_INLINE CallerFrameAndPC* frameRecordThatNamesCaller(CallerFrameAndPC* frameRecordOfOperation)
    {
        CallerFrameAndPC* guard = s_frameRecord;
        return guard && std::bit_cast<CallerFrameAndPC*>(frameRecordOfOperation->callerFrame) == guard ? guard : frameRecordOfOperation;
    }

private:
    static thread_local CallerFrameAndPC* s_frameRecord;

    CallerFrameAndPC* m_outerFrameRecord;
    AssertNoGC m_assertNoGC;
};

#define AOT_OPERATION_FRAME_RECORD() JSC::AOT::NoCollectionGuard::frameRecordThatNamesCaller(AOT_FRAME_RECORD_OF_THIS_FUNCTION())
#else
#define AOT_OPERATION_FRAME_RECORD() AOT_FRAME_RECORD_OF_THIS_FUNCTION()
#endif

constexpr bool isDeclaredNotToCollect(Entry entry)
{
    std::optional<RuntimeOperation> operation = runtimeOperationOf(entry);
    return operation && !Typed::declaredBoundOf(*operation).mayCollect;
}

constexpr bool isDeclaredNotToThrow(Entry entry)
{
    std::optional<RuntimeOperation> operation = runtimeOperationOf(entry);
    return operation && !Typed::declaredBoundOf(*operation).mayThrow;
}

template<auto operation, typename Function = std::remove_pointer_t<decltype(operation)>>
struct CallWithoutCollection {
    static constexpr auto call = operation;
};

#if ASSERT_ENABLED
template<auto operation, typename Result, typename... Arguments, bool isNoexcept>
struct CallWithoutCollection<operation, Result(Arguments...) noexcept(isNoexcept)> {
    static NEVER_INLINE Result JIT_OPERATION_ATTRIBUTES call(Arguments... arguments) noexcept(isNoexcept)
    {
        NoCollectionGuard guard(AOT_FRAME_RECORD_OF_THIS_FUNCTION());
        return operation(arguments...);
    }
};
#endif

// OPERATION_RETURN() throws the out-of-memory error that the operation itself made pending: it has allocated, so its caller expects a collection here.
// An error that was already pending waits for a place that may collect.
class AllocatingOperationScope final : public ThrowScope {
public:
    template<typename... Arguments>
    ALWAYS_INLINE AllocatingOperationScope(Instance* instance, CallerFrameAndPC* frameRecordOfOperation, bool returnsException, Arguments&&... arguments)
        : ThrowScope(std::forward<Arguments>(arguments)...)
        , m_instance(*instance)
        , m_frameRecordOfOperation(frameRecordOfOperation)
        , m_returnsException(returnsException)
        , m_wasPending(instance->vm->traps().needsOutOfMemoryError())
    {
    }

    // The stub reads the exception from the VM, so the error can be thrown last, however the operation returns.
    ALWAYS_INLINE ~AllocatingOperationScope()
    {
        if (!m_returnsException)
            throwOwedOutOfMemoryError();
    }

    ALWAYS_INLINE void willMakeResult()
    {
        if (m_returnsException)
            throwOwedOutOfMemoryError();
    }

private:
    ALWAYS_INLINE void throwOwedOutOfMemoryError()
    {
        if (m_instance.vm->traps().needsOutOfMemoryError() && !m_wasPending) [[unlikely]]
            throwOwedOutOfMemoryErrorBeforeReturn(m_instance, m_frameRecordOfOperation, *this);
    }

    Instance& m_instance;
    CallerFrameAndPC* m_frameRecordOfOperation;
    bool m_returnsException;
    bool m_wasPending;
};

template<typename T>
ALWAYS_INLINE auto makeOperationResult(AllocatingOperationScope& scope, T result)
{
    scope.willMakeResult();
    return JSC::makeOperationResult(static_cast<ThrowScope&>(scope), result);
}

ALWAYS_INLINE auto makeOperationResult(AllocatingOperationScope& scope)
{
    scope.willMakeResult();
    return JSC::makeOperationResult(static_cast<ThrowScope&>(scope));
}

#if ENABLE(EXCEPTION_SCOPE_VERIFICATION)
#define AOT_DECLARE_ALLOCATING_OPERATION_SCOPE(vm, instance, frameRecordOfOperation, returnsException) \
    JSC::AOT::AllocatingOperationScope(instance, frameRecordOfOperation, returnsException, vm, JSC::ExceptionEventLocation(EXCEPTION_SCOPE_POSITION_FOR_ASAN(vm), __FUNCTION__, __FILE__, __LINE__))
#else
#define AOT_DECLARE_ALLOCATING_OPERATION_SCOPE(vm, instance, frameRecordOfOperation, returnsException) \
    JSC::AOT::AllocatingOperationScope(instance, frameRecordOfOperation, returnsException, vm)
#endif

#define AOT_OPERATION_BEGIN_WITH_SCOPE(instance, frameRecordOfOperation, entry, declaredScope) \
    JSGlobalObject* globalObject = (instance)->globalObject; \
    UNUSED_VARIABLE(globalObject); \
    VM& vm = *(instance)->vm; \
    AOTOperationPrologueCallFrameTracer tracer(vm, frameRecordOfOperation); \
    JSC::AOT::OperationValidationScope validationScope(vm, entry); \
    auto scope = declaredScope; \
    UNUSED_VARIABLE(scope)

#define AOT_OPERATION_BEGIN_IN_FRAME(instance, frameRecordOfOperation, entry) \
    AOT_OPERATION_BEGIN_WITH_SCOPE(instance, frameRecordOfOperation, entry, DECLARE_THROW_SCOPE(vm))

#define AOT_OPERATION_FIND_OWN_FRAME() \
    JSC::CallerFrameAndPC* frameRecord = AOT_OPERATION_FRAME_RECORD(); \
    CallFrame* callFrame = frameRecord->callerFrame; \
    UNUSED_VARIABLE(callFrame)

#define AOT_OPERATION_BEGIN_IN_OWN_FRAME(instance, entry) \
    AOT_OPERATION_FIND_OWN_FRAME(); \
    AOT_OPERATION_BEGIN_IN_FRAME(instance, frameRecord, entry)

#define AOT_OPERATION_BEGIN(instance, entry) \
    static_assert(!JSC::AOT::isDeclaredNotToCollect(entry), "an operation that is declared not to collect begins with AOT_OPERATION_BEGIN_WITHOUT_COLLECTION"); \
    constexpr bool returnsException = !JSC::AOT::isDeclaredNotToThrow(entry); \
    AOT_OPERATION_FIND_OWN_FRAME(); \
    AOT_OPERATION_BEGIN_WITH_SCOPE(instance, frameRecord, entry, AOT_DECLARE_ALLOCATING_OPERATION_SCOPE(vm, instance, frameRecord, returnsException))

// The operation puts collections off while it runs. An error is a new object, and what takes the exception runs the collection.
#define AOT_OPERATION_BEGIN_WITHOUT_COLLECTION(instance, entry) \
    static_assert(JSC::AOT::isDeclaredNotToCollect(entry)); \
    AOT_OPERATION_BEGIN_IN_OWN_FRAME(instance, entry); \
    DeferGCForAWhile deferGC(vm)

// A value that the program throws exists already. Only where somebody inspects exceptions is one made for it, and that road puts collections off itself.
#define AOT_OPERATION_BEGIN_THROW_OF_VALUE(instance, entry) \
    static_assert((entry) == JSC::AOT::Entry::operationAOTThrow || (entry) == JSC::AOT::Entry::operationAOTThrowAfterCleanup); \
    AOT_OPERATION_BEGIN_IN_OWN_FRAME(instance, entry)

ALWAYS_INLINE Identifier identifierAt(Instance* instance, unsigned number) { return instance->vm->aotState().identifiers->identifier(number); }

template<typename CellType>
ALWAYS_INLINE void prepareInlineAllocation(Instance* instance, InlineAllocation kind, Structure* structure)
{
    auto& data = instance->allocation.inlineAllocations[static_cast<unsigned>(kind)];
    if (data.allocator) [[likely]]
        return;
    data.header = cellHeaderFor(structure);
    data.allocator = subspaceFor<CellType>(*instance->vm)->allocatorFor(sizeof(CellType), AllocatorForMode::EnsureAllocator).localAllocator();
}

// A stub that finds no cell for a rope leaves its work to an operation. An operation that can answer without making a rope calls this, so that the stub finds cells the next time.
ALWAYS_INLINE void leaveCellsForRopes(Instance* instance)
{
    void* allocator = instance->prefix().ropeStringAllocator;
    if (!allocator || !std::bit_cast<FreeList*>(static_cast<char*>(allocator) + LocalAllocator::offsetOfFreeList())->allocationWillFail()) [[likely]]
        return;
    VM& vm = *instance->vm;
    DeferTerminationForAWhile deferTermination(vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSString* character = vm.smallStrings.singleCharacterString('a');
    jsString(instance->globalObject, character, character);
    scope.assertNoException();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
