/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// `function Node(kind, pos, end) { register(this); this.kind = kind; this.pos = pos; this.end = end; }`: WHICH FIELDS AN OBJECT HAS WHEN ITS MAKER IS THROUGH WITH IT.
// FOR THE MARK OF THE CELL, AND NO PROOF: the object is born with the mark, the mark is cleared where its maker is through with it and every such field lies in its slot, and whoever empties one sets it again.
// So what has seen the object in between does not count, and neither does what may be deleted or redefined later: that is what tells this from SummaryConstruction::Key::isFilled and ThinSummary::fieldsLeftEmpty, under which a read asks nothing.
// A body is through with an object where it returns: the object that it was handed or has made. With a literal: at its FinishObject.

#pragma once

#if ENABLE(AOT)

// For the foot of AOTScan.cpp alone, which declares ScanOfBody.

namespace JSC { namespace AOT { namespace Typed {

inline bool ScanOfBody::hasFilledInFrontOfEveryLeaving(uint32_t root, PropertyKeyID key)
{
    FillInScan fill { root, false, key.bits() };
    const ObjectBeingMadeInScan& object = objectBeingMade(root);
    return m_fills.contains(fill) && (!object.hasLeft || std::ranges::binary_search(object.filled, fill));
}

inline void ScanOfBody::noteFieldLeftEmptyForGood(FieldReference field)
{
    m_summary.fieldsLeftEmpty.append(field);
    m_leavesFieldsEmptyForGood = true;
}

inline void ScanOfBody::meetFillsWhereConstructionEnds(uint32_t root)
{
    ObjectBeingMadeInScan& object = objectBeingMade(root);
    if (object.isGivenUp)
        return;
    std::span<const FillInScan> fillsOfObject = fillsOf(root, m_fills.inOrder());
    if (!std::exchange(object.hasEnded, true)) {
        object.filledAtEnd.append(fillsOfObject);
        return;
    }
    object.filledAtEnd.removeAllMatching([&](const FillInScan& fill) {
        return !std::ranges::binary_search(fillsOfObject, fill);
    });
}

// Behind listConstructions(), which has listed one SummaryConstruction for each object that is not of one layout, in the order of m_objectsBeingMade.
inline void ScanOfBody::listBirths()
{
    uint32_t numberOfConstructionsSeen = 0;
    for (const ObjectBeingMadeInScan& object : m_objectsBeingMade) {
        // A body that never returns with it is through with it where it lets it go.
        const Vector<FillInScan, 8>& atEnd = object.hasEnded ? object.filledAtEnd : object.filled;
        if (object.kind == ObjectBeingMadeKindInScan::OfOneLayout) {
            // Where the body was never through with it, it is born as it leaves: fieldsLeftEmpty says it all.
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
    // Another object of the layout that the body makes may lack the field at its own end. A body may make thousands of objects, each of a layout of its own: `const b = { ...a, x: 1 }   const c = { ...b, y: 2 }   ..`.
    if (!m_summary.fieldsFilledAtEndAlone.isEmpty() && m_leavesFieldsEmptyForGood)
        m_summary.fieldsFilledAtEndAlone.shrink(0);
    if (!m_summary.fieldsFilledAtEndAlone.isEmpty()) {
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
        std::ranges::stable_sort(objectsByLayout, { }, &ObjectOfLayout::layout);
        m_summary.fieldsFilledAtEndAlone.removeAllMatching([&](FieldReference field) {
            PropertyKeyID key = m_program.at(field.layout).fields[field.field].key;
            return std::ranges::any_of(std::ranges::equal_range(objectsByLayout, field.layout, { }, &ObjectOfLayout::layout), [&](const ObjectOfLayout& listed) {
                const ObjectBeingMadeInScan& other = m_objectsBeingMade[listed.placeAmongObjectsBeingMade];
                const Vector<FillInScan, 8>& atEnd = other.hasEnded ? other.filledAtEnd : other.filled;
                return !std::ranges::binary_search(atEnd, FillInScan { other.root, false, key.bits() }) && (other.isGivenUp || !isListedByAllocation(other.root, key));
            });
        });
        sortWithoutRepeatsInScan(m_summary.fieldsFilledAtEndAlone);
    }
    if (!m_summary.fieldsEmptyAtEndAlone.isEmpty())
        sortWithoutRepeatsInScan(m_summary.fieldsEmptyAtEndAlone);
    if (!m_summary.keysFilledAtEnds.isEmpty())
        m_summary.keysFilledAtEnds.shrinkToFit();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
