/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTSchemaEnums.h"
#include "AOTTaggedLayouts.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include "AOTWireSections.h"
#include "AOTWireTypes.h"
#include <algorithm>
#include <span>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT {

struct ExternalFunctionBodies;

namespace Typed {

class TellingFields {
public:
    TellingFields(const ParallelLoops&, const Wire::Sections& checkedSections);

    bool tellApart(uint32_t layout, uint32_t otherLayout) const;
    bool tellsNothingApartFrom(uint32_t otherLayout) const { return fieldsOf(otherLayout).empty(); }
    void forEachKeyOfOwnTellingField(uint32_t layout, NOESCAPE const Invocable<void(uint32_t)> auto& functor) const
    {
        for (const Field& field : fieldsOf(layout))
            functor(field.key);
    }
    bool excludes(uint32_t layout, uint32_t key, const LayoutTag&) const;

    bool declaresOnlyLiterals(uint32_t layout, uint32_t key, size_t maxLiterals, NOESCAPE const Invocable<bool(const LayoutTag&)> auto& isAdmitted) const
    {
        for (const Field& field : fieldsOf(layout)) {
            if (field.key != key)
                continue;
            if (field.nullish || field.holdsStrings)
                return false;
            uint64_t numberAboveMinimum = static_cast<uint64_t>(field.maximum) - static_cast<uint64_t>(field.minimum);
            if (numberAboveMinimum >= maxLiterals)
                return false;
            for (uint64_t offset = 0; offset <= numberAboveMinimum; ++offset) {
                if (!isAdmitted(LayoutTag { static_cast<int64_t>(static_cast<uint64_t>(field.minimum) + offset) }))
                    return false;
            }
            return true;
        }
        return false;
    }

private:
    struct Field {
        uint32_t key;
        uint32_t nullish;
        bool holdsStrings;
        int64_t minimum;
        int64_t maximum;
        std::span<const uint32_t> strings;
    };

    std::span<const Field> fieldsOf(uint32_t layout) const { return m_fields.subspan(m_firstField[layout], m_firstField[layout + 1] - m_firstField[layout]); }
    static bool haveNothingInCommon(const Field&, const Field&);

    Vector<Field> m_fields;
    Vector<uint32_t> m_firstField;
    Vector<uint32_t> m_bases;
    Vector<uint32_t> m_nearestWithFields;
};

inline bool has(const Wire::Layout& layout, LayoutFlags flag)
{
    return layout.flags & static_cast<uint32_t>(flag);
}

inline bool has(const Wire::Field& field, FieldFlags flag)
{
    return field.flags & static_cast<uint32_t>(flag);
}

constexpr uint64_t pairOfWireIndexes(uint32_t first, uint32_t second) { return static_cast<uint64_t>(first) << 32 | second; }

template<typename Listed>
void listSecondsByFirst(const ParallelLoops&, std::span<const Vector<uint64_t>> pairsInOrderOfSeconds, Vector<Vector<Listed>>& lists);
template<typename Listed>
void destroyLists(const ParallelLoops&, Vector<Vector<Listed>>&);

Vector<TaggedFamily> planTaggedFamilies(const ParallelLoops&, const Wire::Sections& checkedSections, const ExternalFunctionBodies*, ArenasOfWorkers&, Vector<LayoutSetID>& members, const TellingFields&, uint32_t firstLayout);

bool isSignature(const Wire::Function&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
