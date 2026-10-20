/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFlowObjectOperations.h"
#include <span>
#include <tuple>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/FastMalloc.h>
#include <wtf/HashMap.h>
#include <wtf/HashTraits.h>
#include <wtf/Hasher.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

inline void add(Hasher& hasher, const FlowSiteDescription& description)
{
    add(hasher, static_cast<uint8_t>(description.kind), static_cast<uint8_t>(description.origin), description.numberOfPositions, description.isCreatedDense, description.isCompletedByPieces, description.value, description.layout, description.builtinClass, description.closureFunction, description.ordinal);
}

struct FlowSolveSiteKey {
    FunctionID function;
    FlowSiteDescription description;

    friend bool operator==(const FlowSolveSiteKey&, const FlowSolveSiteKey&) = default;
};

inline void add(Hasher& hasher, const FlowSolveSiteKey& key)
{
    add(hasher, key.function, key.description);
}

} } } // namespace JSC::AOT::Typed

namespace WTF {

template<> struct HashTraits<JSC::AOT::Typed::FlowSolveSiteKey> : GenericHashTraits<JSC::AOT::Typed::FlowSolveSiteKey> {
    using FlowSolveSiteKey = JSC::AOT::Typed::FlowSolveSiteKey;
    using FunctionID = JSC::AOT::Typed::FunctionID;

    static void constructDeletedValue(FlowSolveSiteKey& slot)
    {
        slot = FlowSolveSiteKey { };
        HashTraits<FunctionID>::constructDeletedValue(slot.function);
    }

    static bool isDeletedValue(const FlowSolveSiteKey& key) { return HashTraits<FunctionID>::isDeletedValue(key.function); }
};

} // namespace WTF

namespace JSC { namespace AOT { namespace Typed {

struct FlowObjectState {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(FlowObjectState);
    WTF_MAKE_NONCOPYABLE(FlowObjectState);

    FlowObjectState(const Program&, BuildContext&);

    uint64_t maxSitesPerLayout;
    BitVector functionsUsingSharedObjects;
    HashMap<FlowSolveSiteKey, AllocationSiteID> solveSites;
    HashMap<std::pair<AllocationSiteID, FlowInstanceID>, FlowObjectID> solveObjects;
    HashMap<LayoutID, AllocationClassID> typedResultClasses;
    HashMap<std::tuple<FlowInstanceID, ValueID, FlowInstanceID>, FlowObjectID> coroutineResults;
    HashMap<DataID, Vector<const DataNode*>> dataNodesInPreorder;
};

FlowObjectID findCallSiteAllocation(const FlowSolution&, FlowInstanceID, const FlowSiteDescription&);
FlowObjectID findCoroutineResult(const FlowSolution&, FlowInstanceID caller, ValueID origin, FlowInstanceID calleeInstance);

inline void forEachObjectOfClass(const FlowObjectTable& objects, AllocationClassID allocationClass, NOESCAPE const Invocable<void(FlowObjectID)> auto& functor)
{
    const FlowAllocationClassMembers& members = objects.at(allocationClass);
    functor(members.sharedObject);
    for (FlowObjectID copy : members.copies)
        functor(copy);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
