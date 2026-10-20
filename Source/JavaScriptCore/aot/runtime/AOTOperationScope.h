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
#include "FrameTracers.h"
#include "JSCInlines.h"
#include <bit>

namespace JSC { namespace AOT {

class OperationValidationScope {
    WTF_FORBID_HEAP_ALLOCATION;
    WTF_MAKE_NONCOPYABLE(OperationValidationScope);
public:
    ALWAYS_INLINE OperationValidationScope(VM& vm, Entry entry)
        : m_vm(vm)
    {
        if (validatesRuntime(vm)) [[unlikely]]
            open(entry);
    }

    ALWAYS_INLINE ~OperationValidationScope()
    {
        if (m_isOpen) [[unlikely]]
            close();
    }

private:
    struct CollectionVersions {
        friend bool operator==(const CollectionVersions&, const CollectionVersions&) = default;

        HeapVersion marking { 0 };
        HeapVersion eden { 0 };
        HeapVersion newlyAllocated { 0 };
    };

    CollectionVersions collectionVersions() const;
    void open(Entry);
    void close();

    VM& m_vm;
    CollectionVersions m_collectionVersions;
    Entry m_entry;
    bool m_mayNotEnterVM { false };
    bool m_mayNotCollect { false };
    bool m_didEnterVMBefore { false };
    bool m_isOpen { false };
};

#define AOT_OPERATION_FRAME_RECORD() std::bit_cast<JSC::CallerFrameAndPC*>(__builtin_frame_address(0))

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

ALWAYS_INLINE const Identifier& identifierAt(Instance* instance, unsigned number) { return instance->vm->aotState().identifiers->identifier(number); }

template<typename CellType>
ALWAYS_INLINE void prepareInlineAllocation(Instance* instance, InlineAllocation kind, Structure* structure)
{
    auto& data = instance->allocation.inlineAllocations[static_cast<unsigned>(kind)];
    if (data.allocator) [[likely]]
        return;
    data.header = cellHeaderFor(structure);
    data.allocator = subspaceFor<CellType>(*instance->vm)->allocatorFor(sizeof(CellType), AllocatorForMode::EnsureAllocator).localAllocator();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
