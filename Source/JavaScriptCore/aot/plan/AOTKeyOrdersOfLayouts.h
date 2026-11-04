/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class Facts;
class Plan;
class Program;
struct PlannedLayout;

// A PROOF. Facts::linked() says that no object of the layout reaches unseen code and that nobody in sight lists its keys, and the layout is a record or an instance whose objects only compiled code makes.
// False claims nothing. TypedSlots withheld: false.
bool keysOfLayoutAreNeverListed(const Program&, const Facts&, LayoutID);
// In planLayoutForSlotPlan(), of a layout with cells, when every slot has its place. It alone writes PlannedLayout::keysAreNeverEnumerated: without its call every layout keeps its orders.
// Only a key order tells a field without a granule from one that the object lacks, so no slot of a layout without orders may exist without storage.
void dropKeyOrdersIfKeysAreNeverListed(const Program&, const Facts&, LayoutID, PlannedLayout&);
// The last step of planLayoutForSlotPlan(), of a layout with cells. It writes PlannedLayout::granulesOfFirstExtension and PlacedSlot::mayExistWithoutStorage, each anew from the slots as they lie, and nothing else.
// Whoever lays the slots of the layout again calls both again, in this order. TypedSlots withheld: nothing.
void finishLayoutForKeyOrders(const Program&, const Facts&, LayoutID, PlannedLayout&);
// For checkPlan(). A layout without an order has the proof, its shapes say the same, none of its slots may exist without storage, and KeyOrders has no order of it. Else InternalErrorKind::InvalidLayout.
void checkKeyOrdersOfLayouts(const Program&, const Facts&, const Plan&, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
