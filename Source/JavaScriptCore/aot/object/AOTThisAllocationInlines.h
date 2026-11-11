/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTClosureWord.h"
#include "AOTCompiledCallee.h"
#include "FreeListInlines.h"
#include "FunctionRareData.h"
#include "JSFunction.h"
#include "LocalAllocator.h"
#include <wtf/Atomics.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static constexpr uint32_t constructorIsNewTarget = FunctionRareData::maxCompiledAllocationConstructor;
static constexpr uint32_t bitOfConstructorWithBody = 1u << 16;
static constexpr uint32_t trivialConstructorIsNewTarget = constructorIsNewTarget & ~bitOfConstructorWithBody;
static_assert(ClosureWord::functionMask < trivialConstructorIsNewTarget && ClosureWord::functionMask >> 16 < trivialConstructorIsNewTarget >> 16);
constexpr bool isConstructorThatIsNewTarget(uint32_t rememberedConstructor) { return (rememberedConstructor | bitOfConstructorWithBody) == constructorIsNewTarget; }

namespace Interpreter {

inline constexpr uint8_t trivialConstructorHighBits = trivialConstructorIsNewTarget >> 16;
inline constexpr size_t defaultFinalObjectSize = sizeof(JSObjectWithButterfly) + JSFinalObject::defaultInlineCapacity * sizeof(EncodedJSValue);
static_assert(JSFinalObject::defaultInlineCapacity == 6, "llint_aot_op_construct clears them one by one");
inline constexpr ptrdiff_t offsetOfDefaultFinalObjectAllocator = OBJECT_OFFSETOF(VM, heap) + OBJECT_OFFSETOF(Heap, cellSpace) + CompleteSubspace::offsetOfAllocatorForSizeStep() + MarkedSpace::sizeClassToIndex(defaultFinalObjectSize) * sizeof(Allocator);

} // namespace Interpreter

ALWAYS_INLINE Structure* rememberedStructureOfThis(VM&, JSCell* constructor, JSCell* newTarget)
{
    auto* function = dynamicDowncast<JSFunction>(*newTarget);
    FunctionRareData* rareData = function ? function->rareData() : nullptr;
    Structure* remembered = rareData ? rareData->compiledAllocationStructure() : nullptr;
    if (!remembered) [[unlikely]]
        return nullptr;
    uint32_t rememberedConstructor = rareData->compiledAllocationConstructor();
    if (constructor == newTarget)
        return isConstructorThatIsNewTarget(rememberedConstructor) ? remembered : nullptr;
    return closureNumberOf(constructor) == rememberedConstructor && constructor->structure()->realm() == remembered->realm() ? remembered : nullptr;
}

static constexpr size_t cellChunkSize = 2 * sizeof(uint64_t);
static_assert(MarkedSpace::sizeStep == cellChunkSize);

ALWAYS_INLINE uint8_t* tryAllocateCellInline(VM& vm, size_t size)
{
    Allocator allocator = vm.cellSpace().allocatorFor(size, AllocatorForMode::AllocatorIfExists);
    if (!allocator) [[unlikely]]
        return nullptr;
    FreeList& freeList = *reinterpret_cast<FreeList*>(reinterpret_cast<uint8_t*>(allocator.localAllocator()) + LocalAllocator::offsetOfFreeList());
    return reinterpret_cast<uint8_t*>(freeList.allocateWithCellSize([]() -> HeapCell* {
        return nullptr;
    }, freeList.cellSize()));
}

// The fence keeps the loop from becoming a call of memset.

ALWAYS_INLINE JSObject* tryCreateFinalObjectInline(VM& vm, Structure* structure)
{
    static_assert(JSObject::offsetOfInlineStorage() == cellChunkSize);
    unsigned inlineCapacity = structure->inlineCapacity();
    size_t size = JSFinalObject::allocationSize(inlineCapacity);
    uint8_t* cell = tryAllocateCellInline(vm, size);
    if (!cell) [[unlikely]]
        return nullptr;
    *reinterpret_cast<uint32_t*>(cell + JSCell::structureIDOffset()) = structure->id().bits();
    *reinterpret_cast<uint32_t*>(cell + JSCell::indexingTypeAndMiscOffset()) = structure->typeInfoBlob();
    *reinterpret_cast<uint64_t*>(cell + JSObject::butterflyOffset()) = 0;
    if (inlineCapacity == JSFinalObject::defaultInlineCapacity) [[likely]]
        memset(cell + cellChunkSize, 0, JSFinalObject::defaultInlineCapacity * sizeof(EncodedJSValue));
    else {
        for (size_t offset = cellChunkSize; offset < size; offset += cellChunkSize) {
            memset(cell + offset, 0, cellChunkSize);
            WTF::compilerFence();
        }
    }
    vm.mutatorFence();
    return reinterpret_cast<JSObject*>(cell);
}

ALWAYS_INLINE JSObject* tryCreateObjectWithoutFieldsInline(VM& vm, Structure* structure, size_t size, const uint64_t* wordsBehindHeader)
{
    uint8_t* cell = tryAllocateCellInline(vm, size);
    if (!cell) [[unlikely]]
        return nullptr;
    *reinterpret_cast<uint32_t*>(cell + JSCell::structureIDOffset()) = structure->id().bits();
    *reinterpret_cast<uint32_t*>(cell + JSCell::indexingTypeAndMiscOffset()) = structure->typeInfoBlob();
    memcpy(cell + sizeof(JSCell), wordsBehindHeader, size - sizeof(JSCell));
    vm.mutatorFence();
    return reinterpret_cast<JSObject*>(cell);
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
