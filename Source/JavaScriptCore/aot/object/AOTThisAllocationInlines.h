/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTClosureWord.h"
#include "AOTCompiledStructure.h"
#include "AOTInstance.h"
#include "AOTObjectModel.h"
#include "AOTObjectWords.h"
#include "AOTRuntimeValidation.h"
#include "AOTState.h"
#include "FreeListInlines.h"
#include "FunctionRareData.h"
#include "JSFunction.h"
#include "LocalAllocator.h"
#include <wtf/Atomics.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

// What FunctionRareData::compiledAllocationConstructor() holds for a function that was constructed with itself as new.target. Any other constructor is remembered by its closure number.
static constexpr uint32_t constructorIsNewTarget = FunctionRareData::maxCompiledAllocationConstructor;
static_assert(ClosureWord::functionMask < constructorIsNewTarget);

// The fast paths of ClassTable::createThis(). They call nothing, so they cannot collect and their callers need no frame.

ALWAYS_INLINE Structure* rememberedStructureOfThis(VM& vm, JSCell* constructor, JSCell* newTarget)
{
    if (constructor != newTarget || validatesRuntime(vm)) [[unlikely]]
        return nullptr;
    auto* function = dynamicDowncast<JSFunction>(*newTarget);
    FunctionRareData* rareData = function ? function->rareData() : nullptr;
    if (!rareData || rareData->compiledAllocationConstructor() != constructorIsNewTarget) [[unlikely]]
        return nullptr;
    return rareData->compiledAllocationStructure();
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

// In both, the fence keeps the loop from becoming a call of memset or memcpy.

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

ALWAYS_INLINE JSObject* tryCreateCompiledObjectInline(Instance& instance, Structure* structure, const LayoutData& layout)
{
    const RuntimeLayoutTable::BodyAtBirth* body = instance.state->objects->layouts.bodyAtBirthIfKnown(layout);
    if (!body) [[unlikely]]
        return nullptr;
    VM& vm = *instance.vm;
    std::span<const uint8_t> cellAtBirth = body->cell.span();
    uint8_t* cell = tryAllocateCellInline(vm, cellAtBirth.size());
    if (!cell) [[unlikely]]
        return nullptr;
    for (size_t offset = 0; offset < cellAtBirth.size(); offset += cellChunkSize) {
        memcpy(cell + offset, cellAtBirth.data() + offset, cellChunkSize);
        WTF::compilerFence();
    }
    static_assert(JSCell::typeInfoFlagsOffset() == 6);
    *reinterpret_cast<uint64_t*>(cell) = cellHeaderFor(structure) | (body->isBornUnfinished ? static_cast<uint64_t>(TypeInfoPerCellBit) << 48 : 0);
    vm.mutatorFence();
    return reinterpret_cast<JSObject*>(cell);
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
