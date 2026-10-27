/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3InductionRecords.h"

#if ENABLE(B3_JIT)

#include "B3ValueInlines.h"

namespace JSC { namespace B3 {

static Value* findLiveReplacement(const Value* deleted, const SparseCollection<Value>& liveValues)
{
    if (deleted->opcode() != Identity)
        return nullptr;
    // A sweep deletes values in program order, so the operand of a dead Identity may already be freed.
    Value* replacement = deleted->child(0);
    for (unsigned index = liveValues.size(); index--;) {
        if (liveValues.at(index) == replacement)
            return replacement;
    }
    return nullptr;
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

void InductionRecords::add(const InductionRecord& record)
{
    m_records.removeAllMatching([&](const InductionRecord& other) {
        return other.phi == record.phi;
    });
    m_records.append(record);
    recomputeReferences();
}

std::optional<InductionRecord> InductionRecords::find(const Value* phi) const
{
    if (!m_isReferenced.get(phi->index()))
        return std::nullopt;
    for (const InductionRecord& record : m_records) {
        if (record.phi == phi)
            return resolve(record);
    }
    return std::nullopt;
}

void InductionRecords::remove(const Value* phi)
{
    bool didRemove = m_records.removeAllMatching([&](const InductionRecord& record) {
        return record.phi == phi;
    });
    if (didRemove)
        recomputeReferences();
}

void InductionRecords::clear()
{
    m_records.clear();
    m_demoted.clear();
    m_isReferenced.clearAll();
}

void InductionRecords::addDemoted(const DemotedInduction& demoted)
{
    m_demoted.append(demoted);
    recomputeReferences();
}

void InductionRecords::didCreatePhi(Variable* variable, BasicBlock* block, Value* phi)
{
    for (unsigned i = 0; i < m_demoted.size(); ++i) {
        DemotedInduction demoted = m_demoted[i];
        if (demoted.variable != variable || demoted.header != block)
            continue;
        m_demoted.removeAt(i);
        add(InductionRecord { phi, demoted.next, demoted.test, demoted.step, demoted.doesNotWrap, false });
        return;
    }
}

void InductionRecords::clearDemoted()
{
    if (m_demoted.isEmpty())
        return;
    m_demoted.clear();
    recomputeReferences();
}

void InductionRecords::willDeleteReferencedValue(const Value* value, const SparseCollection<Value>& liveValues)
{
    Value* replacement = findLiveReplacement(value, liveValues);
    auto redirect = [&](Value*& reference) {
        if (reference == value)
            reference = replacement;
    };
    m_records.removeAllMatching([&](InductionRecord& record) {
        redirect(record.next);
        redirect(record.test);
        return record.phi == value || !record.next;
    });
    m_demoted.removeAllMatching([&](DemotedInduction& demoted) {
        redirect(demoted.next);
        redirect(demoted.test);
        return !demoted.next;
    });
    recomputeReferences();
}

void InductionRecords::recomputeReferences()
{
    m_isReferenced.clearAll();
    auto mark = [&](const Value* value) {
        if (value)
            m_isReferenced.set(value->index());
    };
    for (const InductionRecord& record : m_records) {
        mark(record.phi);
        mark(record.next);
        mark(record.test);
    }
    for (const DemotedInduction& demoted : m_demoted) {
        mark(demoted.next);
        mark(demoted.test);
    }
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
