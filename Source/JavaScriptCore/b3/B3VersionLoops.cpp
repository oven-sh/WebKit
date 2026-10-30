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
#include "B3FixSSA.h"
#include "B3InsertionSetInlines.h"
#include "B3Limits.h"
#include "B3LoopInvariance.h"
#include "B3LoopRangeChecks.h"
#include "B3LoopRegions.h"
#include "B3LoopUtilities.h"
#include "B3LoopVersionPlan.h"
#include "B3MovedLoadCheck.h"
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

        for (auto& version : m_versions)
            version->returnsFromSlowPaths = canReturnFromSlowPaths(version->plan);
        for (unsigned i = m_versions.size(); i--;)
            buildFastPreHeader(*m_versions[i], i);
        storeValuesThatOnlyPreHeadersName();
        for (auto& version : m_versions)
            copyFastLoop(*version);
        for (auto& version : m_versions)
            returnFromSlowPaths(*version);
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
        // An iteration of the original loop ends in front of the guards of the fast loop.
        bool returnsFromSlowPaths { false };
        struct HeaderPhi {
            Value* phi;
            Value* valueInPreHeader;
        };
        Vector<HeaderPhi, 2> invariantHeaderPhis;
        RegionCloner::ValueMap headerPhisEqualToHoistedValues;
        std::optional<FastPreHeader> front;
        RegionCloner::ValueMap hoistedMap;
        RegionCloner::ValueMap peeledMap;
        FastLoopBounds bounds;
        Value* start { nullptr };
        RegionCloner::BlockMap hotCopies;
        RegionCloner::BlockMap excursionCopies;
        Value* testedCopy { nullptr };
        Value* testedCopyInExcursion { nullptr };
        StridedIndexVariables stridedIndexVariables { };
        unsigned numberOfReducedIndicesCopied { 0 };
        // Only where decided comparisons are logged: what stands for a phi in the fast loop, by the variable that the phi has become.
        UncheckedKeyHashMap<Variable*, Value*> valuesStandingForVariables;
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
        Value* valueOfCopiedBlock = version.front->last()->appendNew<VariableValue>(m_proc, Get, value->origin(), variableHolding(value));
        m_cloner.substitute(version.hoistedMap, value, valueOfCopiedBlock);
        return valueOfCopiedBlock;
    }

    Variable* variableHolding(Value* value)
    {
        if (value->opcode() == Get)
            return value->as<VariableValue>()->variable();
        return m_definitionVariables.ensure(value, [&] {
            RELEASE_ASSERT(!m_didStoreValuesThatOnlyPreHeadersName);
            Variable* variable = addVariableForDemotedValue(m_proc, value);
            m_singleDefinitions.add(variable, value);
            m_blocksOfValuesThatOnlyPreHeadersName.add(value->owner);
            m_valuesThatOnlyPreHeadersName.add(value);
            return variable;
        }).iterator->value;
    }

    void storeValuesThatOnlyPreHeadersName()
    {
        m_didStoreValuesThatOnlyPreHeadersName = true;
        InsertionSet insertionSet(m_proc);
        for (BasicBlock* block : m_blocksOfValuesThatOnlyPreHeadersName.values(m_proc)) {
            for (unsigned index = 0; index < block->size(); ++index) {
                Value* value = block->at(index);
                if (m_valuesThatOnlyPreHeadersName.contains(value))
                    insertionSet.insert<VariableValue>(index + 1, Set, value->origin(), m_definitionVariables.get(value), value);
            }
            insertionSet.execute(block);
        }
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

    static Variable* variableOfHeaderPhi(const LoopVersionPlan& plan, Value* value)
    {
        Value* read = value->foldIdentity();
        if (value->owner != plan.region.header || read == value || read->opcode() != Get)
            return nullptr;
        return read->as<VariableValue>()->variable();
    }

    template<typename Functor>
    void forEachValueOfNextIteration(const LoopVersionPlan& plan, Variable* variableOfPhi, const Functor& functor) const
    {
        for (BasicBlock* block : plan.region.blocks) {
            for (Value* value : *block) {
                if (value->opcode() == Set && value->as<VariableValue>()->variable() == variableOfPhi)
                    functor(definitionOf(value->child(0)));
            }
        }
    }

    // The guards in front of the fast loop read variables, so they hold for the next iteration, whichever it is.
    // The guards of range checks start from the values that enter the loop, and hold for the first iteration only.
    bool canReturnFromSlowPaths(const LoopVersionPlan& plan) const
    {
        const RangeCheckPlan& rangeChecks = plan.rangeChecks;
        if (!plan.isEnteredFromPreHeader || plan.unswitched || rangeChecks.clampsBound() || rangeChecks.strided || !rangeChecks.entryGuards.isEmpty() || !rangeChecks.decidedComparisons.isEmpty())
            return false;
        // A loop that may be entered again on any turn keeps its poll, its turns are not written out, and more values live around it. Beside a call that costs little.
        // A call that writes, such as an allocation, takes a slow path of its own every so often, so a loop around one is sure to leave its fast version.
        bool callsAndWritesInEveryTurn = false;
        for (BasicBlock* block : plan.region.blocks) {
            for (Value* value : *block)
                callsAndWritesInEveryTurn |= isCall(value) && !isLoopPoll(value) && value->effects().writes;
        }
        if (!callsAndWritesInEveryTurn)
            return false;
        bool canCompareHeaderPhis = true;
        for (const HoistStep& step : plan.hoisted.steps) {
            Variable* variable = step.kind == HoistStep::Kind::Value ? variableOfHeaderPhi(plan, step.value) : nullptr;
            if (!variable)
                continue;
            canCompareHeaderPhis &= step.value->type().isInt();
            forEachValueOfNextIteration(plan, variable, [&](Value* next) {
                canCompareHeaderPhis &= next == step.value || plan.hoisted.isHoisted(next);
            });
        }
        return canCompareHeaderPhis;
    }

    void hoistValue(Version& version, Value* value)
    {
        if (Variable* variable = version.returnsFromSlowPaths ? variableOfHeaderPhi(version.plan, value) : nullptr) {
            Value* current = version.front->last()->appendNew<VariableValue>(m_proc, Get, value->origin(), variable);
            m_cloner.substitute(version.hoistedMap, value, current);
            version.invariantHeaderPhis.append({ value, current });
            return;
        }
        if (Value* repeated = version.plan.hoisted.repeatedValues.get(value)) {
            // A slow path may have changed what was computed before the loop.
            bool mayBeStale = version.returnsFromSlowPaths && !m_cloner.contains(definitionOf(repeated)->owner);
            // A phi has no value in front of the loop. What it repeats is named by the first computation of its kind, which may be a load in front of the loop
            // where the phi is given a load of the same word in the loop: that one has just been made again here.
            if (Value* given = mayBeStale && value->opcode() == Identity ? valueGivenInFastVersion(version, value) : nullptr) {
                m_cloner.substitute(version.hoistedMap, value, given);
                return;
            }
            if (!mayBeStale || value->opcode() == Identity) {
                m_cloner.substitute(version.hoistedMap, value, hoistedValueOrSelf(version, repeated));
                return;
            }
        }
        cloneIntoPreHeader(version, value, IsPeeled::No);
    }

    // For a phi outside the header that the fast version gives one value: that value as the pre-header has it. Null where the pre-header has none of its own.
    Value* valueGivenInFastVersion(const Version& version, Value* phi) const
    {
        Value* read = phi->foldIdentity();
        if (read == phi || read->opcode() != Get)
            return nullptr;
        Value* result = nullptr;
        forEachValueOfNextIteration(version.plan, read->as<VariableValue>()->variable(), [&](Value* given) {
            if (!result)
                result = valueInPreHeader(version, given, IsPeeled::No);
        });
        return result;
    }

    // In the fast loop such a phi stands for what each iteration computes again. After a slow path it may still hold what the last iteration computed earlier.
    void guardInvariantHeaderPhis(Version& version)
    {
        for (auto [phi, current] : version.invariantHeaderPhis) {
            forEachValueOfNextIteration(version.plan, variableOfHeaderPhi(version.plan, phi), [&, phi = phi, current = current](Value* next) {
                Value* computedAgain = version.hoistedMap.get(next);
                RELEASE_ASSERT(computedAgain);
                if (computedAgain == current)
                    return;
                version.front->appendGuard(phi->origin(), Equal, current, computedAgain);
                m_cloner.substitute(version.hoistedMap, next, current);
            });
        }
    }

    // A phi that every iteration of the fast loop gives one hoisted value is that value from the second iteration on. Behind a guard it is that value in the first one too.
    void guardHeaderPhisGivenHoistedValues(Version& version)
    {
        const LoopVersionPlan& plan = version.plan;
        if (!version.returnsFromSlowPaths)
            return;
        for (Value* phi : *plan.region.header) {
            Variable* variable = variableOfHeaderPhi(plan, phi);
            if (!variable || plan.hoisted.isHoisted(phi) || !phi->type().isInt())
                continue;
            Value* given = nullptr;
            bool isGivenOneValue = true;
            forEachValueOfNextIteration(plan, variable, [&](Value* next) {
                Value* hoisted = plan.hoisted.isHoisted(next) ? version.hoistedMap.get(next) : nullptr;
                isGivenOneValue &= hoisted && (!given || given == hoisted);
                given = hoisted;
            });
            // A rare path that is copied into the fast version comes back to its header with whatever it has given the phi.
            for (BasicBlock* block : plan.excursion.blocks) {
                for (Value* value : *block) {
                    if (value->opcode() != Set || value->as<VariableValue>()->variable() != variable)
                        continue;
                    Value* next = definitionOf(value->child(0));
                    isGivenOneValue &= plan.hoisted.valuesValidInExcursion.contains(next) && version.hoistedMap.get(next) == given;
                }
            }
            if (!given || !isGivenOneValue)
                continue;
            version.front->appendGuard(phi->origin(), Equal, version.front->last()->appendNew<VariableValue>(m_proc, Get, phi->origin(), variable), given);
            version.headerPhisEqualToHoistedValues.add(phi, given);
        }
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
        guardInvariantHeaderPhis(version);
        guardHeaderPhisGivenHoistedValues(version);
        appendEntryGuards(*version.front, plan.region.header->at(0)->origin(), plan.rangeChecks.entryGuards.span(), [&](Value* value) {
            return hoistedValueOrSelf(version, value);
        });
        if (plan.rangeChecks.strided) {
            appendStridedRangeCheckGuards(*version.front, *plan.rangeChecks.strided, *plan.countedLoop, [&](Value* value) {
                return hoistedValueOrSelf(version, value);
            }, version.stridedIndexVariables);
        }
        if (!plan.rewritesLoopTest())
            return;
        version.bounds = appendEntryGuards(*version.front, plan.rangeChecks, *plan.countedLoop, [&](Value* value) {
            return hoistedValueOrSelf(version, value);
        });
        version.start = hoistedValueOrSelf(version, plan.countedLoop->start);
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
        if (Value* given = kind == CopyKind::FastLoop ? version.headerPhisEqualToHoistedValues.get(value) : nullptr) {
            m_cloner.substitute(map, value, given);
            return;
        }
        if (isReplaced(version, value, kind)) {
            if (value->type() == Void)
                return;
            Value* hoisted = version.hoistedMap.get(value);
            RELEASE_ASSERT(hoisted);
            if (std::optional<MovedLoad> moved = MovedLoad::of(m_proc, value)) {
                Value* clone = m_cloner.appendClone(value, copy, map);
                Value* address = clone->lastChild();
                copy->values().removeLast();
                m_proc.deleteValue(clone);
                m_proc.movedLoadLog().add(*moved, copy, copy->size(), address, hoisted);
            }
            m_cloner.substitute(map, value, hoisted);
            return;
        }
        if (kind == CopyKind::FastLoop) {
            if (auto outcome = plan.rangeChecks.decidedComparisons.find(value); outcome != plan.rangeChecks.decidedComparisons.end()) {
                if (m_proc.policy().validation.logsDecidedComparisons && value->numChildren() == 2) {
                    Value* clone = m_cloner.appendClone(value, copy, map);
                    Value* left = clone->child(0);
                    Value* right = clone->child(1);
                    copy->values().removeLast();
                    m_proc.deleteValue(clone);
                    m_proc.movedLoadLog().addDecidedComparison(m_proc, value, copy, copy->size(), left, right, outcome->value);
                }
                m_cloner.substitute(map, value, copy->appendNew<Const32Value>(m_proc, value->origin(), outcome->value));
                return;
            }
            if (const StridedRangeChecks* strided = plan.rangeChecks.strided.get(); strided && version.numberOfReducedIndicesCopied < strided->numberOfReducedIndices) {
                const ReducedIndex& reduced = strided->reducedIndices[version.numberOfReducedIndicesCopied];
                if (reduced.value == value) {
                    ++version.numberOfReducedIndicesCopied;
                    m_cloner.substitute(map, value, appendStridedIndexPlus(copy, value->origin(), version.stridedIndexVariables[reduced.checkIndex].variable, copy->appendNew<Const64Value>(m_proc, value->origin(), reduced.offset)));
                    return;
                }
            }
        }
        Value* clone = m_cloner.appendClone(value, copy, map);
        if (kind == CopyKind::FastLoop && value->opcode() == Set) {
            if (Value* standingForVariable = version.valuesStandingForVariables.get(value->as<VariableValue>()->variable()))
                m_proc.movedLoadLog().addValuesTakenForEqual(m_proc, value->origin(), copy, copy->size(), clone->child(0), standingForVariable);
        }
        if (!plan.rewritesLoopTest())
            return;
        if (value != plan.countedLoop->testedValue())
            return;
        if (kind == CopyKind::FastLoop)
            version.testedCopy = clone;
        else
            version.testedCopyInExcursion = clone;
    }

    Value* appendStridedIndexPlus(BasicBlock* block, Origin origin, Variable* variable, Value* addend)
    {
        return block->appendNew<Value>(m_proc, Add, origin, block->appendNew<VariableValue>(m_proc, Get, origin, variable), addend);
    }

    void appendStridedIndexSteps(Version& version, BasicBlock* block)
    {
#pragma clang loop unroll(disable)
        for (auto [variable, stride] : version.stridedIndexVariables) {
            if (variable)
                block->appendNew<VariableValue>(m_proc, Set, stride->origin(), variable, appendStridedIndexPlus(block, stride->origin(), variable, stride));
        }
    }

    static bool stepsStridedIndices(const LoopVersionPlan& plan, BasicBlock* block)
    {
        if (!plan.rangeChecks.strided)
            return false;
        if (BasicBlock* stepBlock = plan.rangeChecks.strided->indexStepBlock)
            return block == stepBlock;
        return block->last()->opcode() == Jump && block->successorBlock(0) == plan.region.header;
    }

    void fillCopies(Version& version, const Vector<BasicBlock*, 8>& blocks, const RegionCloner::BlockMap& copies, BasicBlock* nextIterationHeader, const RegionCloner::BlockMap* otherCopies, CopyKind kind)
    {
        const LoopVersionPlan& plan = version.plan;
        MovedLoadLog::RegionCopy regionCopy(m_proc.movedLoadLog());
        for (BasicBlock* block : blocks) {
            BasicBlock* copy = copies.get(block);
            m_proc.movedLoadLog().didCopy(block, copy);
            Value* terminal = block->last();
            RegionCloner::ValueMap map;
            for (Value* value : *block) {
                if (value != terminal)
                    copyValue(version, value, copy, map, kind);
            }
            if (kind == CopyKind::FastLoop && stepsStridedIndices(plan, block))
                appendStridedIndexSteps(version, copy);
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
        if (m_proc.policy().validation.logsDecidedComparisons) {
            for (Value* repeated : plan.hoisted.repeatedValues.keys()) {
                Value* read = repeated->foldIdentity();
                if (repeated != read && read->opcode() == Get && plan.hoisted.isHoisted(repeated))
                    version.valuesStandingForVariables.add(read->as<VariableValue>()->variable(), version.hoistedMap.get(repeated));
            }
        }
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
            appendStridedIndexSteps(version, excursionEnd);
            excursionEnd->appendNew<Value>(m_proc, Jump, header->at(0)->origin());
            excursionEnd->setSuccessors(FrequentedBlock(fastHeader));
        }
        if (plan.countedLoop && plan.countedLoop->isTopTested)
            m_topTestedVersionHeaders.append(fastHeader);
        if (plan.isEnteredFromPreHeader)
            plan.region.preHeader->replaceSuccessor(header, version.front->first());
    }

    void returnFromSlowPaths(Version& version)
    {
        if (!version.returnsFromSlowPaths)
            return;
        const HotRegion& region = version.plan.region;
        auto redirect = [&](BasicBlock* block) {
            for (BasicBlock*& successor : block->successorBlocks()) {
                if (successor == region.header)
                    successor = version.front->first();
            }
        };
        for (BasicBlock* block : region.blocks)
            redirect(block);
        for (BasicBlock* block : region.slowPathBlocks)
            redirect(block);
    }

    CountedLoop fastLoopOf(Version& version)
    {
        const CountedLoop& original = *version.plan.countedLoop;
        CountedLoop fast = original;
        fast.preHeader = version.front->last();
        fast.start = version.start;
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
    IndexSet<Value*> m_valuesThatOnlyPreHeadersName;
    IndexSet<BasicBlock*> m_blocksOfValuesThatOnlyPreHeadersName;
    bool m_didStoreValuesThatOnlyPreHeadersName { false };
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
