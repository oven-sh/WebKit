/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

#include "B3SparseCollection.h"
#include "B3Value.h"
#include <wtf/BitVector.h>
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

    bool isEmpty() const { return m_records.isEmpty(); }
    unsigned size() const { return m_records.size(); }

    JS_EXPORT_PRIVATE void add(const InductionRecord&);
    JS_EXPORT_PRIVATE std::optional<InductionRecord> find(const Value* phi) const;
    JS_EXPORT_PRIVATE void remove(const Value* phi);
    JS_EXPORT_PRIVATE void clear();

    bool hasDemoted() const { return !m_demoted.isEmpty(); }
    JS_EXPORT_PRIVATE void addDemoted(const DemotedInduction&);
    JS_EXPORT_PRIVATE void didCreatePhi(Variable*, BasicBlock*, Value* phi);
    JS_EXPORT_PRIVATE void clearDemoted();

    void willDelete(const Value* value, const SparseCollection<Value>& liveValues)
    {
        if (m_isReferenced.get(value->index())) [[unlikely]]
            willDeleteReferencedValue(value, liveValues);
    }

    template<typename Functor>
    void forEach(const Functor& functor) const
    {
        for (const InductionRecord& record : m_records) {
            if (std::optional<InductionRecord> resolved = resolve(record))
                functor(*resolved);
        }
    }

private:
    JS_EXPORT_PRIVATE static std::optional<InductionRecord> resolve(const InductionRecord&);
    JS_EXPORT_PRIVATE void willDeleteReferencedValue(const Value*, const SparseCollection<Value>& liveValues);
    void recomputeReferences();

    Vector<InductionRecord, 2> m_records;
    Vector<DemotedInduction, 2> m_demoted;
    BitVector m_isReferenced;
};

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
