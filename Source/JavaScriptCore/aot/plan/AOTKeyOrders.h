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

struct BeginningsOfKeyOrder {
    friend bool operator==(const BeginningsOfKeyOrder&, const BeginningsOfKeyOrder&) = default;

    uint32_t orderWordOfShortest { 0 };
    uint32_t numberOfLongerOnes { 0 };
};

enum class KeyOrderSource : uint8_t {
    StoresInSequence,
    Declaration,
};

struct KeyOrderParts {
    Vector<uint32_t> firstOrderOfLayout;
    Vector<uint32_t> firstFieldOfOrder;
    Vector<FieldIndex> fields;
    Vector<KeyOrderSource> sourceOfField;
    Vector<uint8_t> someMakerStopsBehindField;
};

class KeyOrders {
    friend class FactMisstater;
public:
    KeyOrders() = default;
    explicit KeyOrders(KeyOrderParts&& parts)
        : m_parts(WTF::move(parts))
    {
        RELEASE_ASSERT(m_parts.sourceOfField.size() == m_parts.fields.size());
        RELEASE_ASSERT(m_parts.someMakerStopsBehindField.size() == m_parts.fields.size());
        indexFirstContinuations();
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
    std::span<const KeyOrderSource> sourcesOfFields(LayoutID layout, uint32_t order) const LIFETIME_BOUND
    {
        uint32_t index = m_parts.firstOrderOfLayout[layout.index()] + order;
        return m_parts.sourceOfField.span().subspan(m_parts.firstFieldOfOrder[index], m_parts.firstFieldOfOrder[index + 1] - m_parts.firstFieldOfOrder[index]);
    }
    std::span<const uint8_t> whereMakersStop(LayoutID layout, uint32_t order) const LIFETIME_BOUND
    {
        uint32_t index = m_parts.firstOrderOfLayout[layout.index()] + order;
        return m_parts.someMakerStopsBehindField.span().subspan(m_parts.firstFieldOfOrder[index], m_parts.firstFieldOfOrder[index + 1] - m_parts.firstFieldOfOrder[index]);
    }

    JS_EXPORT_PRIVATE std::optional<uint32_t> findOrderWord(LayoutID, std::span<const FieldIndex> fieldsWithStorage) const;
    JS_EXPORT_PRIVATE size_t lengthOfLongestBeginningWithOrderWord(LayoutID, std::span<const FieldIndex> fieldsWithStorage) const;
    JS_EXPORT_PRIVATE std::optional<KeyOrderContinuation> firstContinuationCreating(LayoutID, std::span<const FieldIndex> createdFieldsWithStorage) const;
    JS_EXPORT_PRIVATE std::optional<KeyOrderContinuation> continuationCreatingBehindFewestKeys(LayoutID, std::span<const FieldIndex> createdFieldsWithStorage) const;
    JS_EXPORT_PRIVATE std::optional<KeyOrderContinuation> continuationCreatingBehindFewestKeys(LayoutID, std::span<const FieldIndex> createdFieldsWithStorage, std::span<const FieldIndex> fieldsInFront) const;
    JS_EXPORT_PRIVATE std::optional<uint32_t> findOrderWordOfShortestBeginningThatLists(LayoutID, std::span<const FieldIndex> fieldsWithStorage) const;
    JS_EXPORT_PRIVATE Vector<BeginningsOfKeyOrder, 2> beginningsThatList(LayoutID, std::span<const FieldIndex> fieldsWithStorage) const;

private:
    JS_EXPORT_PRIVATE void indexFirstContinuations();

    KeyOrderParts m_parts;
    Vector<uint32_t> m_firstContinuationOfLayout;
    Vector<KeyOrderContinuation> m_firstContinuationCreatingField;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
