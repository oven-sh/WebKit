/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

#include "B3SparseCollection.h"
#include "B3Value.h"
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC { namespace B3 {

class BasicBlock;
class Variable;

struct InductionRecord {
    Value* phi { nullptr };
    Value* next { nullptr };
    Value* test { nullptr };
    int64_t step { 0 };
    bool doesNotWrap { false };
    bool relationHoldsOnEntry { false };
};

struct DemotedInduction {
    Variable* variable { nullptr };
    BasicBlock* header { nullptr };
    Value* next { nullptr };
    Value* test { nullptr };
    int64_t step { 0 };
    bool doesNotWrap { false };
};

class InductionRecords {
    WTF_MAKE_NONCOPYABLE(InductionRecords);
public:
    InductionRecords() = default;

    bool isEmpty() const { return !m_numberOfRecords; }
    unsigned size() const { return m_numberOfRecords; }

    JS_EXPORT_PRIVATE void add(const InductionRecord&);
    JS_EXPORT_PRIVATE std::optional<InductionRecord> find(const Value* phi) const;
    JS_EXPORT_PRIVATE void remove(const Value* phi);
    JS_EXPORT_PRIVATE void clear();

    bool hasDemoted() const { return m_numberOfDemoted; }
    JS_EXPORT_PRIVATE void addDemoted(const DemotedInduction&);
    JS_EXPORT_PRIVATE void didCreatePhi(Variable*, BasicBlock*, Value* phi);
    JS_EXPORT_PRIVATE void clearDemoted();

    void willDelete(const Value* value, const SparseCollection<Value>& liveValues)
    {
        if (m_isReferenced.get(value->index())) [[unlikely]]
            willDeleteReferencedValue(value, liveValues);
        if (m_knowsLiveValues) [[unlikely]]
            m_liveValues.remove(value);
    }

    void didAddValue()
    {
        m_knowsLiveValues = false;
        m_numberOfWalksOverLiveValues = 0;
    }

    template<typename Functor>
    void forEach(const Functor& functor) const
    {
        for (const InductionRecord& record : m_records) {
            if (!record.phi)
                continue;
            if (std::optional<InductionRecord> resolved = resolve(record))
                functor(*resolved);
        }
    }

private:
    // Where a value is named: a record or a demoted induction, by its place in the list.
    struct Place {
        unsigned index { 0 };
        bool isDemoted { false };

        friend bool operator==(const Place&, const Place&) = default;
    };
    using Places = Vector<Place, 1>;

    JS_EXPORT_PRIVATE static std::optional<InductionRecord> resolve(const InductionRecord&);
    JS_EXPORT_PRIVATE void willDeleteReferencedValue(const Value*, const SparseCollection<Value>& liveValues);
    Value* findLiveReplacement(const Value* deleted, const SparseCollection<Value>& liveValues);
    void name(const Value*, Place);
    void forget(const Value*, Place);
    void removeRecordAt(unsigned index);
    void removeDemotedAt(unsigned index);
    void dropGapsIfMany();

    // In the order in which they were added. What was removed leaves a gap: a record without phi, a demoted induction without next.
    Vector<InductionRecord, 2> m_records;
    Vector<DemotedInduction, 2> m_demoted;
    unsigned m_numberOfRecords { 0 };
    unsigned m_numberOfDemoted { 0 };
    UncheckedKeyHashMap<const Value*, Places> m_placesOf;
    UncheckedKeyHashMap<std::pair<Variable*, BasicBlock*>, Vector<unsigned, 1>> m_demotedAt;
    BitVector m_isReferenced;
    // Made where a sweep goes on asking, and right until a value is added.
    UncheckedKeyHashSet<const Value*> m_liveValues;
    unsigned m_numberOfWalksOverLiveValues { 0 };
    bool m_knowsLiveValues { false };
};

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
