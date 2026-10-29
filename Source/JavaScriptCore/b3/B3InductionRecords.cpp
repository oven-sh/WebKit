/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3InductionRecords.h"

#if ENABLE(B3_JIT)

#include "B3ValueInlines.h"

namespace JSC { namespace B3 {

Value* InductionRecords::findLiveReplacement(const Value* deleted, const SparseCollection<Value>& liveValues)
{
    if (deleted->opcode() != Identity)
        return nullptr;
    // A sweep deletes values in program order, so the operand of a dead Identity may already be freed.
    Value* replacement = deleted->child(0);
    // To make the set costs as much as many walks.
    constexpr unsigned walksBeforeSet = 16;
    if (!m_knowsLiveValues && m_numberOfWalksOverLiveValues++ < walksBeforeSet) {
        for (unsigned index = liveValues.size(); index--;) {
            if (liveValues.at(index) == replacement)
                return replacement;
        }
        return nullptr;
    }
    if (!m_knowsLiveValues) {
        m_liveValues.clear();
        for (Value* value : liveValues)
            m_liveValues.add(value);
        m_knowsLiveValues = true;
    }
    return m_liveValues.contains(replacement) ? replacement : nullptr;
}

std::optional<InductionRecord> InductionRecords::resolve(const InductionRecord& record)
{
    if (record.phi->opcode() != Phi)
        return std::nullopt;
    InductionRecord result = record;
    result.next = record.next->foldIdentity();
    if (result.next->type() != record.phi->type())
        return std::nullopt;
    if (record.test)
        result.test = record.test->foldIdentity();
    if (result.test && result.test->numChildren() != 2)
        result.test = nullptr;
    if (!result.test)
        result.relationHoldsOnEntry = false;
    return result;
}

void InductionRecords::name(const Value* value, Place place)
{
    if (!value)
        return;
    Places& places = m_placesOf.add(value, Places()).iterator->value;
    if (!places.contains(place))
        places.append(place);
    m_isReferenced.set(value->index());
}

void InductionRecords::forget(const Value* value, Place place)
{
    if (!value)
        return;
    auto places = m_placesOf.find(value);
    if (places == m_placesOf.end())
        return;
    places->value.removeFirst(place);
    if (!places->value.isEmpty())
        return;
    m_placesOf.remove(places);
    m_isReferenced.clear(value->index());
}

void InductionRecords::removeRecordAt(unsigned index)
{
    InductionRecord& record = m_records[index];
    Place place { index, false };
    forget(record.phi, place);
    forget(record.next, place);
    forget(record.test, place);
    record = { };
    --m_numberOfRecords;
}

void InductionRecords::removeDemotedAt(unsigned index)
{
    DemotedInduction& demoted = m_demoted[index];
    Place place { index, true };
    forget(demoted.next, place);
    forget(demoted.test, place);
    auto at = m_demotedAt.find({ demoted.variable, demoted.header });
    at->value.removeFirst(index);
    if (at->value.isEmpty())
        m_demotedAt.remove(at);
    demoted = { };
    --m_numberOfDemoted;
}

void InductionRecords::dropGapsIfMany()
{
    if (m_records.size() + m_demoted.size() <= 2 * (m_numberOfRecords + m_numberOfDemoted) + 8)
        return;
    Vector<InductionRecord, 2> records = std::exchange(m_records, { });
    Vector<DemotedInduction, 2> allDemoted = std::exchange(m_demoted, { });
    clear();
    for (const InductionRecord& record : records) {
        if (record.phi)
            add(record);
    }
    for (const DemotedInduction& demoted : allDemoted) {
        if (demoted.next)
            addDemoted(demoted);
    }
}

void InductionRecords::add(const InductionRecord& record)
{
    RELEASE_ASSERT(record.phi && record.next);
    remove(record.phi);
    Place place { static_cast<unsigned>(m_records.size()), false };
    m_records.append(record);
    ++m_numberOfRecords;
    name(record.phi, place);
    name(record.next, place);
    name(record.test, place);
}

std::optional<InductionRecord> InductionRecords::find(const Value* phi) const
{
    if (!m_isReferenced.get(phi->index()))
        return std::nullopt;
    auto places = m_placesOf.find(phi);
    if (places == m_placesOf.end())
        return std::nullopt;
    for (Place place : places->value) {
        if (!place.isDemoted && m_records[place.index].phi == phi)
            return resolve(m_records[place.index]);
    }
    return std::nullopt;
}

void InductionRecords::remove(const Value* phi)
{
    auto places = m_placesOf.find(phi);
    if (places == m_placesOf.end())
        return;
    for (Place place : Places(places->value)) {
        if (!place.isDemoted && m_records[place.index].phi == phi)
            removeRecordAt(place.index);
    }
    dropGapsIfMany();
}

void InductionRecords::clear()
{
    m_records.clear();
    m_demoted.clear();
    m_numberOfRecords = 0;
    m_numberOfDemoted = 0;
    m_placesOf.clear();
    m_demotedAt.clear();
    m_isReferenced.clearAll();
    m_liveValues.clear();
    m_knowsLiveValues = false;
    m_numberOfWalksOverLiveValues = 0;
}

void InductionRecords::addDemoted(const DemotedInduction& demoted)
{
    unsigned index = m_demoted.size();
    RELEASE_ASSERT(demoted.next);
    m_demoted.append(demoted);
    ++m_numberOfDemoted;
    m_demotedAt.add({ demoted.variable, demoted.header }, Vector<unsigned, 1>()).iterator->value.append(index);
    name(demoted.next, { index, true });
    name(demoted.test, { index, true });
}

void InductionRecords::didCreatePhi(Variable* variable, BasicBlock* block, Value* phi)
{
    auto at = m_demotedAt.find({ variable, block });
    if (at == m_demotedAt.end())
        return;
    unsigned index = at->value.first();
    DemotedInduction demoted = m_demoted[index];
    removeDemotedAt(index);
    add(InductionRecord { phi, demoted.next, demoted.test, demoted.step, demoted.doesNotWrap, false });
}

void InductionRecords::clearDemoted()
{
    if (!m_numberOfDemoted)
        return;
    for (unsigned index = 0; index < m_demoted.size(); ++index) {
        if (m_demoted[index].next)
            removeDemotedAt(index);
    }
    m_demoted.clear();
}

void InductionRecords::willDeleteReferencedValue(const Value* value, const SparseCollection<Value>& liveValues)
{
    auto found = m_placesOf.find(value);
    if (found == m_placesOf.end())
        return;
    Value* replacement = findLiveReplacement(value, liveValues);
    for (Place place : Places(found->value)) {
        if (place.isDemoted) {
            DemotedInduction& demoted = m_demoted[place.index];
            if (demoted.next == value)
                demoted.next = replacement;
            if (demoted.test == value)
                demoted.test = replacement;
            forget(value, place);
            if (demoted.next)
                name(replacement, place);
            else
                removeDemotedAt(place.index);
            continue;
        }
        InductionRecord& record = m_records[place.index];
        if (record.phi == value) {
            removeRecordAt(place.index);
            continue;
        }
        if (record.next == value)
            record.next = replacement;
        if (record.test == value)
            record.test = replacement;
        forget(value, place);
        if (!record.next) {
            removeRecordAt(place.index);
            continue;
        }
        name(replacement, place);
    }
    dropGapsIfMany();
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
