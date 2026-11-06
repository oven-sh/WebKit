/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3CheckKnownProperties.h"

#if ENABLE(B3_JIT)

#include "B3BasicBlockInlines.h"
#include "B3CheckValue.h"
#include "B3CountedLoops.h"
#include "B3InsertionSetInlines.h"
#include "B3IntRange.h"
#include "B3MemoryValue.h"
#include "B3NaturalLoops.h"
#include "B3PatchpointValue.h"
#include "B3PhaseScope.h"
#include "B3PhiChildren.h"
#include "B3ProcedureInlines.h"
#include "B3StackmapGenerationParams.h"
#include "B3UpsilonValue.h"
#include "B3ValueInlines.h"
#include "CCallHelpers.h"

namespace JSC { namespace B3 {

namespace {

class CheckKnownProperties {
public:
    explicit CheckKnownProperties(Procedure& proc)
        : m_proc(proc)
        , m_insertionSet(proc)
    {
    }

    bool run()
    {
        m_proc.resetValueOwners();
        planInductionChecks();
        for (BasicBlock* block : m_proc) {
            for (unsigned index = 0; index < block->size(); ++index)
                checkValue(block, index);
            m_insertionSet.execute(block);
        }
        return m_changed;
    }

private:
    struct PlannedInduction {
        InductionRecord record;
        const NaturalLoop* loop;
    };

    struct EntryCheck {
        BasicBlock* preHeader;
        Opcode relation;
        Value* start;
        Value* bound;
    };

    void planInductionChecks()
    {
        if (m_proc.knownInductions().isEmpty())
            return;
        m_loops = &m_proc.naturalLoops();
        PhiChildren phiChildren(m_proc);
        m_proc.knownInductions().forEach([&](const InductionRecord& record) {
            const NaturalLoop* loop = m_loops->headerOf(record.phi->owner);
            if (!loop)
                return;
            m_inductions.append({ record, loop });
            if (!record.relationHoldsOnEntry)
                return;
            auto counted = analyzeCountedLoop({ m_proc, *m_loops, *loop, phiChildren, [&](Value* value) { return !m_loops->belongsTo(value->owner, *loop); }, std::nullopt });
            if (counted && counted->inductionVariable == record.phi && counted->relationHoldsOnEntry)
                m_entryChecks.append({ counted->preHeader, counted->relation, counted->start, counted->bound });
        });
    }

    void insertTrap(unsigned index, Origin origin, Value* isViolated, KnownProperty property = KnownProperty::Induction)
    {
        CheckValue* check = m_insertionSet.insert<CheckValue>(index, Check, origin, isViolated);
        check->setGenerator([property, origin, report = m_proc.policy().reportsBrokenKnownProperty](CCallHelpers& jit, const StackmapGenerationParams&) {
            if (report)
                report->run(jit, property, origin);
            else
                jit.breakpoint();
        });
        m_changed = true;
    }

    Value* insertComparison(unsigned index, Origin origin, Opcode opcode, Value* left, Value* right)
    {
        return m_insertionSet.insert<Value>(index, opcode, origin, left, right);
    }

    void checkValue(BasicBlock* block, unsigned index)
    {
        Value* value = block->at(index);
        if (auto* upsilon = value->as<UpsilonValue>())
            checkUpsilon(block, index, upsilon);
        if (value == block->last()) {
            checkEntryRelations(block, index);
            return;
        }
        checkRange(index + 1, value);
        checkDereferenceableBytes(index + 1, value);
    }

    void checkRange(unsigned index, Value* value)
    {
        std::optional<IntRange> range = m_proc.knownRange(value);
        if (!range || *range == IntRange::top(value->type()))
            return;
        Origin origin = value->origin();
        Type type = value->type();
        Value* min = m_insertionSet.insertIntConstant(index, origin, type, range->min());
        Value* width = m_insertionSet.insertIntConstant(index, origin, type, static_cast<int64_t>(static_cast<uint64_t>(range->max()) - static_cast<uint64_t>(range->min())));
        Value* opaque = m_insertionSet.insert<Value>(index, Opaque, origin, value);
        Value* distance = m_insertionSet.insert<Value>(index, Sub, origin, opaque, min);
        insertTrap(index, origin, insertComparison(index, origin, Above, distance, width), KnownProperty::Range);
    }

    void checkDereferenceableBytes(unsigned index, Value* value)
    {
        uint32_t bytes = m_proc.knownDereferenceableBytes(value);
        if (!bytes)
            return;
        Origin origin = value->origin();
        PatchpointValue* keepAlive = m_proc.add<PatchpointValue>(Void, origin);
        keepAlive->effects = Effects::none();
        keepAlive->effects.writesLocalState = true;
        keepAlive->setGenerator([](CCallHelpers&, const StackmapGenerationParams&) { });
        keepAlive->append(m_insertionSet.insert<MemoryValue>(index, Load8Z, Int32, origin, value, 0), ValueRep::ColdAny);
        if (bytes > 1 && isRepresentableAs<int32_t>(static_cast<int64_t>(bytes - 1)))
            keepAlive->append(m_insertionSet.insert<MemoryValue>(index, Load8Z, Int32, origin, value, static_cast<int32_t>(bytes - 1)), ValueRep::ColdAny);
        m_insertionSet.insertValue(index, keepAlive);
        m_changed = true;
    }

    void checkUpsilon(BasicBlock* block, unsigned index, UpsilonValue* upsilon)
    {
        for (const PlannedInduction& induction : m_inductions) {
            const InductionRecord& record = induction.record;
            if (upsilon->phi() != record.phi || !m_loops->belongsTo(block, *induction.loop))
                continue;
            Origin origin = upsilon->origin();
            Value* input = upsilon->child(0);
            Value* step = m_insertionSet.insertIntConstant(index, origin, record.phi->type(), record.step);
            Value* expected = m_insertionSet.insert<Value>(index, Add, origin, record.phi, step);
            insertTrap(index, origin, insertComparison(index, origin, NotEqual, input, expected));
            if (record.doesNotWrap)
                insertTrap(index, origin, insertComparison(index, origin, record.step > 0 ? LessThan : GreaterThan, input, record.phi));
        }
    }

    void checkEntryRelations(BasicBlock* block, unsigned index)
    {
        for (const EntryCheck& entryCheck : m_entryChecks) {
            if (entryCheck.preHeader != block)
                continue;
            Origin origin = block->last()->origin();
            Value* relationHolds = insertComparison(index, origin, entryCheck.relation, entryCheck.start, entryCheck.bound);
            Value* zero = m_insertionSet.insertIntConstant(index, origin, Int32, 0);
            insertTrap(index, origin, insertComparison(index, origin, Equal, relationHolds, zero));
        }
    }

    Procedure& m_proc;
    InsertionSet m_insertionSet;
    NaturalLoops* m_loops { nullptr };
    Vector<PlannedInduction, 4> m_inductions;
    Vector<EntryCheck, 4> m_entryChecks;
    bool m_changed { false };
};

} // anonymous namespace

bool checkKnownProperties(Procedure& proc)
{
    PhaseScope phaseScope(proc, "checkKnownProperties"_s);
    return CheckKnownProperties(proc).run();
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
