/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3VersionLoops.h"

#if ENABLE(B3_JIT)

#include "B3BasicBlockInlines.h"
#include "B3ClampLoopBound.h"
#include "B3CountedLoops.h"
#include "B3EnsureLoopPreHeaders.h"
#include "B3Limits.h"
#include "B3LoopInvariance.h"
#include "B3LoopRangeChecks.h"
#include "B3LoopRegions.h"
#include "B3LoopVersionPlan.h"
#include "B3NaturalLoops.h"
#include "B3PhaseScope.h"
#include "B3ProcedureInlines.h"
#include "B3RegionCloner.h"
#include "B3ValueInlines.h"
#include <wtf/IndexSet.h>

namespace JSC { namespace B3 {

namespace {

class VersionLoops {
public:
    VersionLoops(Procedure& proc, Vector<BasicBlock*>& topTestedVersionHeaders)
        : m_proc(proc)
        , m_topTestedVersionHeaders(topTestedVersionHeaders)
        , m_cloner(proc)
    {
    }

    bool run()
    {
        ensureLoopPreHeaders(m_proc);
        NaturalLoops& loops = m_proc.naturalLoops();
        m_proc.resetValueOwners();
        LoopVersionPlanner planner(m_proc);
        for (unsigned loopIndex = loops.numLoops(); loopIndex--;) {
            for (LoopVersionPlan& plan : planner.plan(loops.loop(loopIndex)))
                m_versions.append(makeUnique<Version>(WTF::move(plan)));
        }
        if (m_versions.isEmpty())
            return false;

        for (auto& version : m_versions) {
            for (BasicBlock* block : version->plan.region.blocks)
                m_cloner.addBlock(block);
            for (BasicBlock* block : version->plan.excursion.blocks)
                m_cloner.addBlock(block);
        }
        m_cloner.demoteEscapingValues();
        m_proc.resetValueOwners();
        findSingleDefinitions();

        for (unsigned i = m_versions.size(); i--;)
            buildFastPreHeader(*m_versions[i], i);
        for (auto& version : m_versions)
            copyFastLoop(*version);
        splitEdgesIntoSlowPaths();
        return m_cloner.finish();
    }

private:
    enum class CopyKind : uint8_t { FastLoop, Excursion };
    enum class IsPeeled : bool { No, Yes };

    struct Version {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Version);

        explicit Version(LoopVersionPlan&& plan)
            : plan(WTF::move(plan))
        {
        }

        LoopVersionPlan plan;
        std::optional<FastPreHeader> front;
        RegionCloner::ValueMap hoistedMap;
        RegionCloner::ValueMap peeledMap;
        FastLoopBounds bounds;
        RegionCloner::BlockMap hotCopies;
        RegionCloner::BlockMap excursionCopies;
        Value* testedCopy { nullptr };
        Value* testedCopyInExcursion { nullptr };
    };

    void findSingleDefinitions()
    {
        for (Value* value : m_proc.values()) {
            if (value->opcode() != Set)
                continue;
            auto result = m_singleDefinitions.add(value->as<VariableValue>()->variable(), value->child(0));
            if (!result.isNewEntry)
                result.iterator->value = nullptr;
        }
        for (Value* value : m_proc.values()) {
            if (value->opcode() != Set || value->owner != value->child(0)->owner)
                continue;
            Variable* variable = value->as<VariableValue>()->variable();
            if (m_singleDefinitions.get(variable))
                m_definitionVariables.add(value->child(0), variable);
        }
    }

    Value* definitionOf(Value* operand) const
    {
        if (operand->opcode() != Get)
            return operand;
        Value* definition = m_singleDefinitions.get(operand->as<VariableValue>()->variable());
        return definition ? definition : operand;
    }

    Value* valueInPreHeader(const Version& version, Value* operand, IsPeeled isPeeled) const
    {
        Value* source = definitionOf(operand);
        if (isPeeled == IsPeeled::Yes) {
            if (Value* copy = version.peeledMap.get(source))
                return copy;
        }
        return version.hoistedMap.get(source);
    }

    Value* hoistedValueOrSelf(Version& version, Value* value)
    {
        if (Value* replacement = valueInPreHeader(version, value, IsPeeled::No))
            return replacement;
        if (!m_cloner.contains(value->owner))
            return value;
        Variable* variable = m_definitionVariables.get(value);
        RELEASE_ASSERT(variable);
        Value* siblingLoopValue = version.front->last()->appendNew<VariableValue>(m_proc, Get, value->origin(), variable);
        m_cloner.substitute(version.hoistedMap, value, siblingLoopValue);
        return siblingLoopValue;
    }

    void mapOperands(Version& version, Value* value, RegionCloner::ValueMap& map, IsPeeled isPeeled)
    {
        for (Value* child : value->children()) {
            if (map.contains(child))
                continue;
            if (Value* replacement = valueInPreHeader(version, child, isPeeled)) {
                m_cloner.substitute(map, child, replacement);
                continue;
            }
            if (child->opcode() == Get && m_cloner.contains(child->owner))
                m_cloner.appendClone(child, version.front->last(), map);
        }
    }

    void cloneIntoPreHeader(Version& version, Value* value, IsPeeled isPeeled)
    {
        RegionCloner::ValueMap& map = isPeeled == IsPeeled::Yes ? version.peeledMap : version.hoistedMap;
        mapOperands(version, value, map, isPeeled);
        Value* clone = m_cloner.appendClone(value, version.front->last(), map);
        if (isPeeled == IsPeeled::No)
            return;
        if (auto* memory = clone->as<MemoryValue>())
            memory->setControlDependent(true);
        else if (auto* patchpoint = clone->as<PatchpointValue>())
            patchpoint->effects.controlDependent = true;
    }

    void hoistValue(Version& version, Value* value)
    {
        if (Value* repeated = version.plan.hoisted.repeatedValues.get(value)) {
            m_cloner.substitute(version.hoistedMap, value, hoistedValueOrSelf(version, repeated));
            return;
        }
        cloneIntoPreHeader(version, value, IsPeeled::No);
    }

    void buildFastPreHeader(Version& version, unsigned index)
    {
        const LoopVersionPlan& plan = version.plan;
        version.front.emplace(m_proc, plan.region.header);
        for (const HoistStep& step : plan.hoisted.steps) {
            switch (step.kind) {
            case HoistStep::Kind::Value:
                hoistValue(version, step.value);
                break;
            case HoistStep::Kind::Guard:
                cloneIntoPreHeader(version, step.value, IsPeeled::No);
                closeWithGuard(version, index, step.value);
                break;
            case HoistStep::Kind::Check:
                closeWithCheck(version, step.value, IsPeeled::No);
                break;
            case HoistStep::Kind::PeeledValue:
                cloneIntoPreHeader(version, step.value, IsPeeled::Yes);
                break;
            case HoistStep::Kind::PeeledGuard:
                cloneIntoPreHeader(version, step.value, IsPeeled::Yes);
                closeWithGuard(version, index, step.value);
                break;
            case HoistStep::Kind::PeeledCheck:
                closeWithCheck(version, step.value, IsPeeled::Yes);
                break;
            }
        }
        if (!plan.rewritesLoopTest())
            return;
        version.bounds = appendEntryGuards(*version.front, plan.rangeChecks, *plan.countedLoop, [&](Value* value) {
            return hoistedValueOrSelf(version, value);
        });
    }

    void closeWithGuard(Version& version, unsigned index, Value* terminal)
    {
        const LoopVersionPlan& plan = version.plan;
        BasicBlock* closed = version.front->startNextBlock();
        closed->successors() = terminal->owner->successors();
        BasicBlock* likelySuccessor = plan.hoisted.likelySuccessors.get(terminal);
        for (FrequentedBlock& successor : closed->successors()) {
            if (successor.block() == likelySuccessor)
                successor = FrequentedBlock(version.front->last());
            else if (plan.unswitched && terminal == plan.unswitched->terminal && successor.block() == plan.unswitched->otherSuccessor)
                successor = FrequentedBlock(m_versions[index + 1]->front->first());
            else
                successor = FrequentedBlock(plan.region.header, FrequencyClass::Rare);
        }
    }

    void closeWithCheck(Version& version, Value* check, IsPeeled isPeeled)
    {
        RegionCloner::ValueMap& map = isPeeled == IsPeeled::Yes ? version.peeledMap : version.hoistedMap;
        Value* condition = check->child(0);
        Value* conditionInPreHeader = valueInPreHeader(version, condition, isPeeled);
        if (!conditionInPreHeader && condition->opcode() == Get && m_cloner.contains(condition->owner))
            conditionInPreHeader = m_cloner.appendClone(condition, version.front->last(), map);
        if (!conditionInPreHeader)
            conditionInPreHeader = condition;
        Value* zero = version.front->appendIntConstant(check->origin(), condition->type(), 0);
        version.front->appendGuard(check->origin(), Equal, conditionInPreHeader, zero);
    }

    static bool isReplaced(const Version& version, Value* value, CopyKind kind)
    {
        switch (kind) {
        case CopyKind::FastLoop:
            return version.plan.hoisted.isHoisted(value);
        case CopyKind::Excursion:
            return version.plan.hoisted.valuesValidInExcursion.contains(value);
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    void copyValue(Version& version, Value* value, BasicBlock* copy, RegionCloner::ValueMap& map, CopyKind kind)
    {
        const LoopVersionPlan& plan = version.plan;
        if (isReplaced(version, value, kind)) {
            if (value->type() == Void)
                return;
            Value* hoisted = version.hoistedMap.get(value);
            RELEASE_ASSERT(hoisted);
            m_cloner.substitute(map, value, hoisted);
            return;
        }
        if (kind == CopyKind::FastLoop) {
            if (auto outcome = plan.rangeChecks.decidedComparisons.find(value); outcome != plan.rangeChecks.decidedComparisons.end()) {
                m_cloner.substitute(map, value, copy->appendNew<Const32Value>(m_proc, value->origin(), outcome->value));
                return;
            }
        }
        Value* clone = m_cloner.appendClone(value, copy, map);
        if (!plan.rewritesLoopTest())
            return;
        if (value != plan.countedLoop->testedValue())
            return;
        if (kind == CopyKind::FastLoop)
            version.testedCopy = clone;
        else
            version.testedCopyInExcursion = clone;
    }

    void fillCopies(Version& version, const Vector<BasicBlock*, 8>& blocks, const RegionCloner::BlockMap& copies, BasicBlock* nextIterationHeader, const RegionCloner::BlockMap* otherCopies, CopyKind kind)
    {
        const LoopVersionPlan& plan = version.plan;
        for (BasicBlock* block : blocks) {
            BasicBlock* copy = copies.get(block);
            Value* terminal = block->last();
            RegionCloner::ValueMap map;
            for (Value* value : *block) {
                if (value != terminal)
                    copyValue(version, value, copy, map, kind);
            }
            if (isReplaced(version, terminal, kind)) {
                copy->appendNew<Value>(m_proc, Jump, terminal->origin());
                copy->setSuccessors(FrequentedBlock(plan.hoisted.likelySuccessors.get(terminal)));
            } else {
                m_cloner.appendClone(terminal, copy, map);
                copy->successors() = block->successors();
            }
            for (BasicBlock*& successor : copy->successorBlocks()) {
                if (successor == plan.region.header)
                    successor = nextIterationHeader;
                else if (BasicBlock* successorCopy = copies.get(successor))
                    successor = successorCopy;
                else if (BasicBlock* successorCopy = otherCopies ? otherCopies->get(successor) : nullptr)
                    successor = successorCopy;
            }
        }
    }

    void copyFastLoop(Version& version)
    {
        const LoopVersionPlan& plan = version.plan;
        BasicBlock* header = plan.region.header;
        for (BasicBlock* block : plan.region.blocks)
            version.hotCopies.add(block, m_proc.addBlock());
        BasicBlock* fastHeader = version.hotCopies.get(header);
        BasicBlock* excursionEnd = nullptr;
        if (!plan.excursion.isEmpty()) {
            for (BasicBlock* block : plan.excursion.blocks)
                version.excursionCopies.add(block, m_proc.addBlock());
            excursionEnd = m_proc.addBlock();
            fillCopies(version, plan.excursion.blocks, version.excursionCopies, excursionEnd, nullptr, CopyKind::Excursion);
        }
        fillCopies(version, plan.region.blocks, version.hotCopies, fastHeader, &version.excursionCopies, CopyKind::FastLoop);
        if (plan.rewritesLoopTest())
            rewriteLoopTest(version);
        version.front->last()->appendNew<Value>(m_proc, Jump, header->at(0)->origin());
        version.front->last()->setSuccessors(FrequentedBlock(fastHeader));
        if (excursionEnd) {
            excursionEnd->appendNew<Value>(m_proc, Jump, header->at(0)->origin());
            excursionEnd->setSuccessors(FrequentedBlock(fastHeader));
        }
        if (plan.countedLoop && plan.countedLoop->isTopTested)
            m_topTestedVersionHeaders.append(fastHeader);
        if (plan.isEnteredFromPreHeader)
            plan.region.preHeader->replaceSuccessor(header, version.front->first());
    }

    CountedLoop fastLoopOf(Version& version)
    {
        const CountedLoop& original = *version.plan.countedLoop;
        CountedLoop fast = original;
        fast.preHeader = version.front->last();
        fast.start = hoistedValueOrSelf(version, original.start);
        fast.header = version.hotCopies.get(original.header);
        fast.test = version.hotCopies.get(original.test);
        fast.latch = version.hotCopies.get(original.latch);
        fast.bound = version.bounds.bound;
        fast.boundIsSafe = true;
        if (fast.isTopTested)
            fast.inductionVariable = version.testedCopy;
        else
            fast.next = version.testedCopy;
        return fast;
    }

    void rewriteLoopTest(Version& version)
    {
        const LoopVersionPlan& plan = version.plan;
        const CountedLoop& original = *plan.countedLoop;
        RELEASE_ASSERT(version.testedCopy);
        CountedLoop fast = fastLoopOf(version);

        ClampTargets targets;
        targets.limit = version.bounds.limit;
        targets.beyondLimit = fast.isTopTested ? original.header : copyOriginalLatch(original.latch);
        if (BasicBlock* test = version.excursionCopies.get(original.test))
            targets.otherTests.append(LoopTestCopy { test, test->successorBlock(fast.latchSuccessorIndex), version.testedCopyInExcursion, fast.isTopTested ? original.test : nullptr });

        clampLoopBound(m_proc, fast, targets);
    }

    BasicBlock* copyOriginalLatch(BasicBlock* latch)
    {
        return m_cloner.cloneBlocks(singleElementSpan(latch)).get(latch);
    }

    void splitEdgesIntoSlowPaths()
    {
        for (auto& version : m_versions) {
            for (BasicBlock* block : version->plan.region.slowPathBlocks)
                m_slowPathBlocks.add(block);
        }
        unsigned numberOfBlocks = m_proc.size();
        for (unsigned i = 0; i < numberOfBlocks; ++i) {
            BasicBlock* block = m_proc[i];
            if (!block || block->numSuccessors() < 2 || m_slowPathBlocks.contains(block))
                continue;
            for (FrequentedBlock& successor : block->successors()) {
                if (!m_slowPathBlocks.contains(successor.block()))
                    continue;
                BasicBlock* edge = m_proc.addBlock();
                edge->appendNew<Value>(m_proc, Jump, block->last()->origin());
                edge->setSuccessors(FrequentedBlock(successor.block()));
                successor.block() = edge;
            }
        }
    }

    Procedure& m_proc;
    Vector<BasicBlock*>& m_topTestedVersionHeaders;
    RegionCloner m_cloner;
    Vector<std::unique_ptr<Version>> m_versions;
    UncheckedKeyHashMap<Variable*, Value*> m_singleDefinitions;
    UncheckedKeyHashMap<Value*, Variable*> m_definitionVariables;
    IndexSet<BasicBlock*> m_slowPathBlocks;
};

} // anonymous namespace

bool versionLoops(Procedure& proc, Vector<BasicBlock*>& topTestedVersionHeaders)
{
    PhaseScope phaseScope(proc, "versionLoops"_s);
    VersionLoops versionLoops(proc, topTestedVersionHeaders);
    return versionLoops.run();
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
