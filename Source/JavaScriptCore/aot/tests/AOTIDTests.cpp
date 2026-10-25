/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTID.h"
#include "AOTNativeTest.h"
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(DefaultHash<ValueID>::safeToCompareToEmptyOrDeleted);
static_assert(ValueID::hashTableDeletedIndex != ValueID().index());

template<typename IDType>
static void checkReservedValues()
{
    static_assert(sizeof(IDType) == sizeof(uint32_t) && DefaultHash<IDType>::safeToCompareToEmptyOrDeleted);
    AOT_TEST_CHECK(!IDType() && !IDType(nullptr) && !HashTraits<IDType>::emptyValue());
    IDType deleted;
    HashTraits<IDType>::constructDeletedValue(deleted);
    AOT_TEST_CHECK(deleted && deleted.index() == IDType::hashTableDeletedIndex);
    AOT_TEST_CHECK(HashTraits<IDType>::isDeletedValue(deleted) && !HashTraits<IDType>::isDeletedValue(IDType()) && !HashTraits<IDType>::isDeletedValue(IDType(0u)));
}

template<typename... IDTypes>
static void checkReservedValuesOfEach()
{
    (checkReservedValues<IDTypes>(), ...);
}

static void idReservesAbsentAndDeletedValues()
{
    checkReservedValuesOfEach<ValueID, BlockID>();
    checkReservedValuesOfEach<StringID, PropertyKeyID, FunctionID, LayoutID, VariableID, ModuleID, ModuleFailureID, TypeID, TypeSiteID, EvalScopeID, DataID, ExpectedResultID>();
    checkReservedValuesOfEach<LayoutSetID, CalleeSetID>();
    checkReservedValuesOfEach<AccessSiteID, AllocationSiteID, AllocationClassID>();
    checkReservedValuesOfEach<FieldAccessID, AllocationID, RegExpRecordID, SiteCacheID, GlobalBindingID, GuardBitID>();
}

static void idHashIsIntegerHashOfIndex()
{
    for (uint32_t index : { 0u, 1u, 350000u, ValueID::hashTableDeletedIndex - 1 })
        AOT_TEST_CHECK(DefaultHash<ValueID>::hash(ValueID(index)) == IntHash<uint32_t>::hash(index));
}

static void idKeyedMapHoldsIndexZero()
{
    HashMap<ValueID, unsigned> map;
    AOT_TEST_CHECK(map.add(ValueID(0u), 10).isNewEntry && map.add(ValueID(7u), 17).isNewEntry && !map.add(ValueID(0u), 20).isNewEntry);
    AOT_TEST_CHECK(map.size() == 2 && map.get(ValueID(0u)) == 10 && map.get(ValueID(7u)) == 17 && !map.contains(ValueID(1u)));
}

static void idKeyedSetFindsKeysAfterRemoval()
{
    HashSet<FunctionID> set;
    for (uint32_t index = 0; index < 64; ++index)
        set.add(FunctionID(index));
    for (uint32_t index = 0; index < 64; index += 2)
        AOT_TEST_CHECK(set.remove(FunctionID(index)));
    for (uint32_t index = 0; index < 64; ++index)
        AOT_TEST_CHECK(set.contains(FunctionID(index)) == !!(index & 1));
    AOT_TEST_CHECK(set.size() == 32);
}

#define FOR_EACH_AOT_ID_TEST(v) \
    v(idReservesAbsentAndDeletedValues) v(idHashIsIntegerHashOfIndex) v(idKeyedMapHoldsIndexZero) v(idKeyedSetFindsKeysAfterRemoval)

std::span<const NativeTest> idTests()
{
#define AOT_ID_TEST_ENTRY(name) { #name ""_s, name },
    static constexpr NativeTest tests[] = { FOR_EACH_AOT_ID_TEST(AOT_ID_TEST_ENTRY) };
#undef AOT_ID_TEST_ENTRY
    return tests;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
