/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTInstance.h"
#include "AOTObjectWords.h"
#include "AOTProgramIdentifiers.h"
#include "AOTRuntimeTable.h"
#include "AOTRuntimeValidation.h"
#include "DeferGC.h"
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

#define AOT_OPERATION_BEGIN_IN_FRAME(instance, frameRecordOfOperation, entry) \
    JSGlobalObject* globalObject = (instance)->globalObject; \
    UNUSED_VARIABLE(globalObject); \
    VM& vm = *(instance)->vm; \
    AOTOperationPrologueCallFrameTracer tracer(vm, frameRecordOfOperation); \
    JSC::AOT::OperationValidationScope validationScope(vm, entry); \
    auto scope = DECLARE_THROW_SCOPE(vm); \
    UNUSED_VARIABLE(scope)

#define AOT_OPERATION_BEGIN(instance, entry) \
    JSC::CallerFrameAndPC* frameRecord = AOT_OPERATION_FRAME_RECORD(); \
    CallFrame* callFrame = frameRecord->callerFrame; \
    UNUSED_VARIABLE(callFrame); \
    AOT_OPERATION_BEGIN_IN_FRAME(instance, frameRecord, entry)

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
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSString* character = vm.smallStrings.singleCharacterString('a');
    jsString(instance->globalObject, character, character);
    scope.assertNoException();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
