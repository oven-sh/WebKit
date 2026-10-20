/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class UseLists;

struct TypedResultTarget {
    explicit operator bool() const { return !!type; }

    TypeID type;
    KnowledgeLevel level { KnowledgeLevel::Unknown };
    Vector<LayoutID, 4> layouts;
};

TypedResultForm typedResultFormOf(const Program&, const Value& call);
ValueID typedResultSourceOf(const Program&, const Value& call);

TypedResultTarget findTypedResultTarget(const Program&, const Function&, const UseLists&, ValueID call);

Vector<LayoutID, 4> layoutsReachedBy(const Program&, TypeID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
