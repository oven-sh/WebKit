/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLinkTables.h"
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

class LinkFlow;
class TypeArena;
struct LinkType;

// WHOSE OWN KEYS SOMEBODY LISTS: for-in, Object.keys | values | entries | assign, a spread, a rest, JSON.stringify of the object or of a holder of it. Nothing in the flow reads it: no node, no edge, no observer.
struct KeysListedInLink {
    // By layout.
    BitVector layouts;
    // By family: each member. Resolved behind the flow.
    BitVector families;
    // By layout: some member of a family of lost layouts through which keys were listed. Resolved behind the flow: LinkFlow::unitedLayouts.
    BitVector membersOfLostFamilies;
};

struct AccessorLaidInField {
    FieldReference field;
    FunctionID function;
    bool isGetter { true };

    friend constexpr auto operator<=>(const AccessorLaidInField&, const AccessorLaidInField&) = default;
};

// While the flow runs, for whoever applies a row or inspects. By parts, monotone, idempotent: a layout, a family, a member of Lost. It lays no edge and wakes nobody.
void noteKeysListedOf(LinkFlow&, const LinkType&);

// What the one visitor gathers behind the flow. WriterOfLinkLayouts::write() has one as a local.
struct GatheredForKeyOrders {
    // writeWhatLinkKnowsOfKeyOrders() lays LinkFlow::keysListed over it.
    KeysListedInLink keysListed;
    // By function.
    BitVector functionsWhoseClosuresMayChange;
    // The number of the layout above the key. Bit 0: some maker gives the field the constant undefined. Bit 1: some maker gives it anything else.
    // A field may be appended, and those have no place yet while the summaries are gone through.
    HashMap<uint64_t, uint8_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> whatMakersGive;
};
// In the loop of WriterOfLinkLayouts::write() over the reached summaries, once for each. It reads the types that the flow ended with.
void lookAtSummaryForKeyOrders(const LinkFlow&, TypeArena&, FunctionID, const ThinSummary&, GatheredForKeyOrders&);
// Behind writeAppendedFields(), which makes the cells of the appended fields. It writes LinkedLayout::keysMayBeListed, LinkedFunction::closuresStayAsMade, LinkedCell::isOnlyGivenUndefinedByMakers and LinkTables::accessorsExpectedInFields, and no other answer.
void writeWhatLinkKnowsOfKeyOrders(const LinkFlow&, const GatheredForKeyOrders&, LinkTables&);
// Behind writeAppendedFields() too, and by itself: LinkTables::accessorsExpectedInFields and keysOfAccessorsExpectedInFields, out of LinkFlow::accessorsLaidInFields alone.
void writeAccessorsExpectedInFields(const LinkFlow&, LinkTables&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
