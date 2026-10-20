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

static void resetShrinkHistory(ArrayAllocationProfile& profile)
{
    profile.reviewsWithRoomToSpare = 0;
    profile.largestLengthSinceChange = 0;
}

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
    if (length > inUse) {
        profile.set(std::min<unsigned>(length | 1, BASE_CONTIGUOUS_VECTOR_LEN_MAX), learnedShape);
        resetShrinkHistory(profile);
        return;
    }

    profile.largestLengthSinceChange = std::max(profile.largestLengthSinceChange, static_cast<uint8_t>(length));
    if (++profile.reviewsWithRoomToSpare < ArrayAllocationProfile::reviewsBeforeShrinking) {
        profile.set(profile.capacity(), learnedShape);
        return;
    }

    unsigned wanted = profile.largestLengthSinceChange | 1;
    profile.set(wanted < inUse ? wanted : profile.capacity(), learnedShape);
    resetShrinkHistory(profile);
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewArrayAtReviewedSite, JSCell*, (Instance* instance, void* profileAddress, uint32_t seedIndexingType))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewArrayAtReviewedSite);
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
