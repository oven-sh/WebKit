/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTID.h"
#include "AOTSlotPlan.h"
#include "AOTVector.h"
#include "JSExportMacros.h"
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

struct KeyOrderContinuation {
    friend bool operator==(const KeyOrderContinuation&, const KeyOrderContinuation&) = default;

    uint32_t orderWordBefore { 0 };
    uint32_t orderWordAfter { 0 };
};

// Who says that a field comes where an order lists it.
enum class KeyOrderSource : uint8_t {
    // Behind what the layout lists: what some body stores by name, one behind the other, on an object that it was just given. LinkTables::forEachRunOfMakers().
    StoresInSequence,
    Declaration,
};

struct KeyOrderParts {
    // One entry more than there are layouts. For each layout that holds compiled objects: the fields with storage as Layout::declaredOrder lists them,
    // and for each DataNode::Object anywhere in Program::data that has the layout, the members that SlotPlan::findField() finds and that have storage, as they are written.
    // AOTDataTable.cpp has no way to add a field behind the allocation. And for each run of a maker, the fields that the layout lists itself and behind them those of the run.
    // And for each constructor function that is reached, the fields that its body stores on `this`, each once, as the stores are written.
    // And for each allocation in a body that is reached, the fields with storage that it lists, as they are written: ThinSummary::fieldsListedByAllocations. Withheld with TypedSlots.
    // And for each copy into an allocation (`{ ...source, more }`) of whose source somebody says a layout, each order of that layout that lists all its fields, as fields of the target. Withheld with TypedSlots.
    // No empty order. Sorted, each once, none the beginning of another.
    // More than maxKeyOrdersPerLayout of one layout: InternalErrorKind::InvalidLayout, LimitID::KeyOrdersPerLayout.
    Vector<uint32_t> firstOrderOfLayout;
    // One entry more than there are orders.
    Vector<uint32_t> firstFieldOfOrder;
    Vector<FieldIndex> fields;
    // By entry of `fields`. It only ranks.
    Vector<KeyOrderSource> sourceOfField;
};

// The orders of a layout list fields with storage alone. They are sorted as lists of field indices, and none is the beginning of another.
// An object has the number of the first order that begins with its keys, and the count of them: AOTCellFormat.h, orderWordOf().
class KeyOrders {
public:
    static constexpr unsigned maxContinuations = 2;
    using Continuations = Vector<KeyOrderContinuation, maxContinuations>;

    KeyOrders() = default;
    explicit KeyOrders(KeyOrderParts&& parts)
        : m_parts(WTF::move(parts))
    {
        RELEASE_ASSERT(m_parts.sourceOfField.size() == m_parts.fields.size());
    }

    uint32_t numberOfOrdersOf(LayoutID layout) const
    {
        if (static_cast<size_t>(layout.index()) + 1 >= m_parts.firstOrderOfLayout.size())
            return 0;
        return m_parts.firstOrderOfLayout[layout.index() + 1] - m_parts.firstOrderOfLayout[layout.index()];
    }
    std::span<const FieldIndex> order(LayoutID layout, uint32_t order) const LIFETIME_BOUND
    {
        uint32_t index = m_parts.firstOrderOfLayout[layout.index()] + order;
        return m_parts.fields.span().subspan(m_parts.firstFieldOfOrder[index], m_parts.firstFieldOfOrder[index + 1] - m_parts.firstFieldOfOrder[index]);
    }
    // By field of the order.
    std::span<const KeyOrderSource> sourcesOfFields(LayoutID layout, uint32_t order) const LIFETIME_BOUND
    {
        uint32_t index = m_parts.firstOrderOfLayout[layout.index()] + order;
        return m_parts.sourceOfField.span().subspan(m_parts.firstFieldOfOrder[index], m_parts.firstFieldOfOrder[index + 1] - m_parts.firstFieldOfOrder[index]);
    }

    // None: the image has no order that begins so. Whoever makes such an object makes it with the longest beginning that has a word and adds the other fields by stores.
    // No fields: unorderedOrderWord, whether or not the layout has an order.
    JS_EXPORT_PRIVATE std::optional<uint32_t> findOrderWord(LayoutID, std::span<const FieldIndex> fieldsWithStorage) const;
    // How many of the fields, from the first on, have a word together. It may be 0, and Plan::allocation() has an answer for 0 fields. The walk asks in front of each NewObject.
    JS_EXPORT_PRIVATE size_t lengthOfLongestBeginningWithOrderWord(LayoutID, std::span<const FieldIndex> fieldsWithStorage) const;
    // The places in the orders of the layout where exactly these fields come next, one behind the other, by order. A function of the orders and the fields alone.
    JS_EXPORT_PRIVATE Continuations continuationsCreating(LayoutID, std::span<const FieldIndex> createdFieldsWithStorage) const;
    // Of all such places, the one with the fewest keys in front of it; of several, the first. What a constructor creates only on some way stands in some orders and not in others: the shortest beginning is what every way creates.
    JS_EXPORT_PRIVATE std::optional<KeyOrderContinuation> continuationCreatingBehindFewestKeys(LayoutID, std::span<const FieldIndex> createdFieldsWithStorage) const;
    // The same among the places in front of which the order lists every one of `fieldsInFront`, in whatever order.
    JS_EXPORT_PRIVATE std::optional<KeyOrderContinuation> continuationCreatingBehindFewestKeys(LayoutID, std::span<const FieldIndex> createdFieldsWithStorage, std::span<const FieldIndex> fieldsInFront) const;
    // The word of the fewest keys, from the first of some order on, among which all of the fields are. No fields: no word.
    JS_EXPORT_PRIVATE std::optional<uint32_t> findOrderWordOfShortestBeginningThatLists(LayoutID, std::span<const FieldIndex> fieldsWithStorage) const;

private:
    KeyOrderParts m_parts;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
