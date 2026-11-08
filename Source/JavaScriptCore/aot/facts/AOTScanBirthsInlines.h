/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTSort.h"

namespace JSC { namespace AOT { namespace Typed {

inline bool ScanOfBody::hasFilledInFrontOfEveryLeaving(uint32_t root, PropertyKeyID key)
{
    FillInScan fill { root, false, key.bits() };
    const ObjectBeingMadeInScan& object = objectBeingMade(root);
    return m_setsOfFills.contains(m_fills, fill) && (!object.hasLeft || std::ranges::binary_search(object.filled, fill));
}

inline void ScanOfBody::meetFillsWhereConstructionEnds(uint32_t root)
{
    ObjectBeingMadeInScan& object = objectBeingMade(root);
    if (object.isGivenUp)
        return;
    std::span<const FillInScan> fillsOfObject = fillsOf(root, m_fills);
    if (!std::exchange(object.hasEnded, true)) {
        object.filledAtEnd.append(fillsOfObject);
        return;
    }
    object.filledAtEnd.removeAllMatching([&](const FillInScan& fill) {
        return !std::ranges::binary_search(fillsOfObject, fill);
    });
}

inline void ScanOfBody::listBirths()
{
    uint32_t numberOfConstructionsSeen = 0;
    for (const ObjectBeingMadeInScan& object : m_objectsBeingMade) {
        const Vector<FillInScan, 8>& atEnd = object.hasEnded ? object.filledAtEnd : object.filled;
        if (object.kind == ObjectBeingMadeKindInScan::OfOneLayout) {
            if (!object.hasLeft || !object.hasEnded || object.isGivenUp)
                continue;
            LayoutID id = layoutOfAllocation(definitionOf(object.root));
            const Layout& layout = m_program.at(id);
            auto noteWhatOneListHasAlone = [&](const Vector<FillInScan, 8>& has, const Vector<FillInScan, 8>& lacks, Vector<FieldReference>& fields) {
                for (const FillInScan& fill : has) {
                    if (fill.isPart || std::ranges::binary_search(lacks, fill))
                        continue;
                    PropertyKeyID key = PropertyKeyID::fromBits(fill.number);
                    if (std::optional<uint32_t> field = layout.findField(key); field && !isListedByAllocation(object.root, key))
                        fields.append(FieldReference { id, *field });
                }
            };
            noteWhatOneListHasAlone(object.filledAtEnd, object.filled, m_summary.fieldsFilledAtEndAlone);
            noteWhatOneListHasAlone(object.filled, object.filledAtEnd, m_summary.fieldsEmptyAtEndAlone);
            continue;
        }

        RELEASE_ASSERT(numberOfConstructionsSeen < m_summary.constructions.size());
        SummaryConstruction& construction = m_summary.constructions[numberOfConstructionsSeen++];
        construction.firstKeyFilledAtEnd = static_cast<uint32_t>(m_summary.keysFilledAtEnds.size());
        for (const FillInScan& fill : atEnd) {
            if (!fill.isPart)
                m_summary.keysFilledAtEnds.append(PropertyKeyID::fromBits(fill.number));
        }
        if (BodyValue made = definitionOf(object.root); made && made.opcode() == Opcode::NewObject && !object.isGivenUp) {
            auto allocation = made.as<Opcode::NewObject>();
            const Layout& layout = m_program.at(allocation.layout());
            for (uint32_t field : allocation.fields()) {
                if (field < layout.fields.size() && layout.fields[field].key && !std::ranges::binary_search(atEnd, FillInScan { object.root, false, layout.fields[field].key.bits() }))
                    m_summary.keysFilledAtEnds.append(layout.fields[field].key);
            }
        }
        construction.numberOfKeysFilledAtEnd = static_cast<uint32_t>(m_summary.keysFilledAtEnds.size()) - construction.firstKeyFilledAtEnd;

        RELEASE_ASSERT(construction.numberOfParts == object.parts.size());
        for (uint32_t part = 0; part < construction.numberOfParts; ++part)
            m_summary.partsOfConstructions[construction.firstPart + part].runsOnEveryWayToEnd = std::ranges::binary_search(atEnd, FillInScan { object.root, true, object.parts[part].index() });
    }
    if (!m_summary.fieldsFilledAtEndAlone.isEmpty() && m_leavesFieldsEmptyForGood)
        m_summary.fieldsFilledAtEndAlone.shrink(0);
    if (!m_summary.fieldsFilledAtEndAlone.isEmpty()) {
        sortWithoutRepeatsInScan(m_summary.fieldsFilledAtEndAlone);
        struct ObjectOfLayout {
            LayoutID layout;
            uint32_t placeAmongObjectsBeingMade;
        };
        Vector<ObjectOfLayout, 8> objectsByLayout;
        for (uint32_t place = 0; place < m_objectsBeingMade.size(); ++place) {
            const ObjectBeingMadeInScan& object = m_objectsBeingMade[place];
            if (object.kind == ObjectBeingMadeKindInScan::OfOneLayout && object.hasLeft)
                objectsByLayout.append({ layoutOfAllocation(definitionOf(object.root)), place });
        }
        AOT::stableSort(objectsByLayout, { }, &ObjectOfLayout::layout);
        m_summary.fieldsFilledAtEndAlone.removeAllMatching([&](FieldReference field) {
            PropertyKeyID key = m_program.at(field.layout).fields[field.field].key;
            return std::ranges::any_of(std::ranges::equal_range(objectsByLayout, field.layout, { }, &ObjectOfLayout::layout), [&](const ObjectOfLayout& listed) {
                const ObjectBeingMadeInScan& other = m_objectsBeingMade[listed.placeAmongObjectsBeingMade];
                const Vector<FillInScan, 8>& atEnd = other.hasEnded ? other.filledAtEnd : other.filled;
                return !std::ranges::binary_search(atEnd, FillInScan { other.root, false, key.bits() }) && (other.isGivenUp || !isListedByAllocation(other.root, key));
            });
        });
    }
    if (!m_summary.fieldsEmptyAtEndAlone.isEmpty())
        sortWithoutRepeatsInScan(m_summary.fieldsEmptyAtEndAlone);

}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
