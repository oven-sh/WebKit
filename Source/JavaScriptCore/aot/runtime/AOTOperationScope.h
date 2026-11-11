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

#if OS(WINDOWS) && CPU(X86_64)
#define AOT_FRAME_RECORD_OF_THIS_FUNCTION() \
    ({ \
        asm volatile("" : : : "rbp"); \
        std::bit_cast<JSC::CallerFrameAndPC*>(static_cast<void**>(_AddressOfReturnAddress()) - 1); \
    })
#else
#define AOT_FRAME_RECORD_OF_THIS_FUNCTION() std::bit_cast<JSC::CallerFrameAndPC*>(__builtin_frame_address(0))
#endif

#define AOT_MUST_TAIL_CALL [[clang::musttail]]

#if ASSERT_ENABLED
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
    static inline thread_local CallerFrameAndPC* s_frameRecord { nullptr };

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

#define AOT_OPERATION_BEGIN_WITH_SCOPE(instance, frameRecordOfOperation, entry, declaredScope) \
    JSGlobalObject* globalObject = (instance)->globalObject; \
    UNUSED_VARIABLE(globalObject); \
    VM& vm = *(instance)->vm; \
    AOTOperationPrologueCallFrameTracer tracer(vm, frameRecordOfOperation); \
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
    AOT_OPERATION_BEGIN_IN_OWN_FRAME(instance, entry)

#define AOT_OPERATION_BEGIN_WITHOUT_COLLECTION(instance, entry) \
    static_assert(JSC::AOT::isDeclaredNotToCollect(entry)); \
    AOT_OPERATION_BEGIN_IN_OWN_FRAME(instance, entry); \
    DeferGCForAWhile deferGC(vm)

#define AOT_OPERATION_BEGIN_THROW_OF_VALUE(instance, entry) \
    static_assert((entry) == JSC::AOT::Entry::operationAOTThrow || (entry) == JSC::AOT::Entry::operationAOTThrowAfterCleanup); \
    AOT_OPERATION_BEGIN_IN_OWN_FRAME(instance, entry)

void collectInPlaceOfPollBehindOperation(VM&, JSCell* madeByOperation);

ALWAYS_INLINE JSCell* newCellForCompiledCode(VM& vm, JSCell* cell)
{
    if (vm.traps().needsOutOfMemoryError()) [[unlikely]]
        collectInPlaceOfPollBehindOperation(vm, cell);
    return cell;
}

ALWAYS_INLINE Identifier identifierAt(Instance* instance, unsigned number) { return identifiersOf(*instance).identifier(number); }

template<typename CellType>
ALWAYS_INLINE void prepareInlineAllocation(Instance* instance, InlineAllocation kind, Structure* structure)
{
    auto& data = instance->allocation.inlineAllocations[static_cast<unsigned>(kind)];
    if (data.allocator) [[likely]]
        return;
    data.header = cellHeaderFor(structure);
    data.allocator = subspaceFor<CellType>(*instance->vm)->allocatorFor(sizeof(CellType), AllocatorForMode::EnsureAllocator).localAllocator();
}

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
