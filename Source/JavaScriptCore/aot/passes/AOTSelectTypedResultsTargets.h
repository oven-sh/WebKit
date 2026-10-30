/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include "AOTVector.h"
#include <memory>
#include <wtf/HashMap.h>
#include <wtf/IterationStatus.h>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class FlowResults;
class Heat;
class UseLists;

struct TypedResultTarget {
    explicit operator bool() const { return !!type; }

    TypeID type;
    Vector<LayoutID, 4> layouts;
};

TypedResultForm typedResultFormOf(const Program&, const Value& call);
ValueID typedResultSourceOf(const Program&, const Value& call);
std::optional<uint32_t> indexOfAwaitedOperand(const Value&);

class HeadLayoutsOfFamilies {
    WTF_MAKE_NONCOPYABLE(HeadLayoutsOfFamilies);
public:
    HeadLayoutsOfFamilies() = default;

    std::span<const LayoutID> of(const Program&, const Type&) const LIFETIME_BOUND;

private:
    mutable Lock m_lock;
    mutable HashMap<LayoutSetID, std::unique_ptr<Vector<LayoutID, 4>>> m_headsOfFamily WTF_GUARDED_BY_LOCK(m_lock);
};

TypeID findTypeOfTypedResultTarget(const Program&, const Function&, const UseLists&, ValueID call);
TypedResultTarget findTypedResultTarget(const Program&, const Function&, const UseLists&, ValueID call, size_t maxNumberOfLayouts, const HeadLayoutsOfFamilies*);
void forEachLayoutReachedBy(const Program&, TypeID, const HeadLayoutsOfFamilies*, const ScopedLambda<IterationStatus(LayoutID)>&);
void markUnprofitableTypedResults(const Program&, FlowResults&, const Heat&, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
