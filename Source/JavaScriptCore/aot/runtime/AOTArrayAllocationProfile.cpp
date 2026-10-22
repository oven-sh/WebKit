/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTArrayAllocationProfile.h"

#if ENABLE(AOT)

#include "AOTConvention.h"
#include "AOTInstance.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTUnwinding.h"
#include "ExceptionHelpers.h"
#include "JSArrayBufferViewInlines.h"
#include "JSArrayInlines.h"
#include "JSCInlines.h"
#include <algorithm>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static unsigned capacityInUse(const ArrayAllocationProfile& profile)
{
    return profile.capacity() ? profile.capacity() : arrayLiteralVectorLength(0);
}

static void startWindow(ArrayAllocationProfile& profile)
{
    profile.reviews = 0;
    std::ranges::fill(profile.largestLengths, 0);
}

// Every array of the site is born with the room, and only the ones that outgrow it pay for that. Lengths are skewed: most arrays of a site are short and a few are many times as long.
// So the room follows the smallest of the lengths that are kept: in one window that many arrays less one may be longer without a change.
void reviewArrayAllocationProfile(ArrayAllocationProfile& profile, JSArray* lastArrayOrNull)
{
    if (!lastArrayOrNull)
        return;
    IndexingType shape = lastArrayOrNull->indexingType() & IndexingShapeMask;
    if (shape != Int32Shape && shape != DoubleShape && shape != ContiguousShape)
        return;

    IndexingType learnedShape = std::max(profile.shape(), shape);
    unsigned length = std::min<unsigned>(lastArrayOrNull->butterfly()->publicLength(), BASE_CONTIGUOUS_VECTOR_LEN_MAX);
    unsigned inUse = capacityInUse(profile);
    uint8_t entering = static_cast<uint8_t>(length);
    for (uint8_t& kept : profile.largestLengths) {
        if (entering > kept)
            std::swap(entering, kept);
    }

    unsigned wanted = profile.largestLengths[ArrayAllocationProfile::lengthsKept - 1] | 1;
    if (wanted > inUse) {
        profile.set(std::min<unsigned>(wanted, BASE_CONTIGUOUS_VECTOR_LEN_MAX), learnedShape);
        startWindow(profile);
        return;
    }
    if (++profile.reviews < ArrayAllocationProfile::reviewsInWindow) {
        profile.set(profile.capacity(), learnedShape);
        return;
    }

    profile.set(wanted < inUse ? wanted : profile.capacity(), learnedShape);
    startWindow(profile);
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewArrayAtReviewedSite, JSCell*, (Instance* instance, void* profileAddress, uint32_t seedIndexingType))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewArrayAtReviewedSite);
    ThrowsOwedOutOfMemoryErrorAtReturn owedError(instance, frameRecord);
    uint8_t* statics = instance->statics();
    uint8_t* address = static_cast<uint8_t*>(profileAddress);
    uint64_t distanceToLastArray = instance->arrays.distanceFromProfileToLastArray;
    RELEASE_ASSERT(address >= statics && distanceToLastArray >= sizeof(ArrayAllocationProfile));
    RELEASE_ASSERT(address + distanceToLastArray + ArrayAllocationProfile::sizeOfLastArrayWord <= statics + instance->tables().instanceSizes.sizeOfStatics);
    RELEASE_ASSERT(seedIndexingType == ArrayWithInt32 || seedIndexingType == ArrayWithDouble || seedIndexingType == ArrayWithContiguous);
    auto& profile = *reinterpret_cast<ArrayAllocationProfile*>(address);
    auto& lastArray = *reinterpret_cast<JSArray**>(address + distanceToLastArray);
    ASSERT(profile.isDueForReview());

    reviewArrayAllocationProfile(profile, lastArray);

    IndexingType shape = std::max<IndexingType>(profile.shape(), seedIndexingType & IndexingShapeMask);
    JSArray* array = JSArray::tryCreate(vm, globalObject->arrayStructureForIndexingTypeDuringAllocation(IsArray | shape), 0, capacityInUse(profile));
    if (!array) {
        throwOutOfMemoryError(globalObject, scope);
        OPERATION_RETURN(scope, nullptr);
    }
    // Not a root: every collection zeroes this word, so it neither keeps the array alive nor dangles.
    lastArray = array;
    OPERATION_RETURN(scope, array);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedArrayLength, uint64_t, (JSCell* view))
{
    return cellArgument<JSArrayBufferView>(view, Entry::operationAOTTypedArrayLength)->length();
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
