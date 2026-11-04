/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIDSets.h"
#include "AOTTypeKinds.h"
#include <span>

namespace JSC { namespace AOT { namespace Typed {

// The layouts of the objects that a type names, that a test asks for, or that arrive at an access.
// A LayoutSetID is an interned set: LayoutSets in AOTIDSets.h makes them, TypeReader::at() reads them, and membersOf() hands one out as a sorted list. The null id is the empty set.
// Every set lists every layout: there are no open sets. Whether the objects of a type all have one of its layouts is Type::hasOnlyLayouts().

// Whoever has tested an object for the set, or has read the set off a type that hasOnlyLayouts(), may say that no other layout arrives.
inline ProvenValue<std::span<const LayoutID>> provenMembers(std::span<const LayoutID> members) { return Proven::byClosedSet(members); }

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
