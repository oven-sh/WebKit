/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTArrayAllocationProfile.h"
#include "AOTObjectWords.h"
#include "AllocatorInlines.h"
#include "ButterflyInlines.h"
#include "CompleteSubspaceInlines.h"
#include "JSArray.h"

namespace JSC { namespace AOT {

// Arrays of one shape, one behind the other, each one cell as tryCreateArrayWithStorage() makes it: what that asks of the structure and looks up in the heap for every array is asked and looked up once.
class ArraysMadeInARow {
public:
    ArraysMadeInARow(VM& vm, Structure* structure, unsigned length, unsigned vectorLength)
        : m_vm(vm)
        , m_allocator(vm.cellSpace().allocatorFor(sizeOfArrayWithStorage(vectorLength), AllocatorForMode::EnsureAllocator))
        , m_header(cellHeaderFor(structure))
        , m_length(length)
        , m_vectorLength(vectorLength)
    {
        RELEASE_ASSERT(length <= vectorLength && vectorLength && sizeOfArrayWithStorage(vectorLength) <= MarkedSpace::largeCutoff && m_allocator);
        RELEASE_ASSERT(hasContiguous(structure->indexingType()) && !structure->outOfLineCapacity() && !structure->hasElementsStartOffset() && structure->classInfoForCells() == JSArray::info());
        static_assert(sizeOfArrayWithStorage(0) == sizeof(JSArray) + sizeof(IndexingHeader));
    }

    // functor(the elements): it fills the first `length` of them, and runs nothing that allocates. The others are holes.
    template<typename Functor>
    ALWAYS_INLINE JSArray* make(NOESCAPE const Functor& functor)
    {
        char* cell = static_cast<char*>(m_allocator.allocate(m_vm.heap, m_allocator.cellSize(), nullptr, AllocationFailureMode::Assert));
        Butterfly* butterfly = Butterfly::fromBase(cell + sizeof(JSArray), 0, 0);
        butterfly->setVectorLength(m_vectorLength);
        butterfly->setPublicLength(m_length);
        EncodedJSValue* elements = butterfly->indexingPayload<EncodedJSValue>();
        functor(elements);
        for (unsigned index = m_length; index < m_vectorLength; ++index)
            elements[index] = JSValue::encode(JSValue());
        *std::bit_cast<Butterfly**>(cell + JSObject::butterflyOffset()) = butterfly;
        *std::bit_cast<uint64_t*>(cell) = m_header;
        m_vm.mutatorFence();
        return std::bit_cast<JSArray*>(cell);
    }

private:
    VM& m_vm;
    Allocator m_allocator;
    uint64_t m_header;
    unsigned m_length;
    unsigned m_vectorLength;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
