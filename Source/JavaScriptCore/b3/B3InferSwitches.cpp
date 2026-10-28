/*
 * Copyright (C) 2016-2019 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#include "config.h"
#include "B3InferSwitches.h"

#if ENABLE(B3_JIT)

#include "B3BasicBlockInlines.h"
#include "B3CaseCollectionInlines.h"
#include "B3InsertionSet.h"
#include "B3Limits.h"
#include "B3PhaseScope.h"
#include "B3ProcedureInlines.h"
#include "B3SwitchValue.h"
#include "B3UpsilonValue.h"
#include "B3UseCounts.h"
#include "B3ValueInlines.h"
#include <wtf/HashSet.h>
#include <wtf/IndexSet.h>
#include <wtf/ListDump.h>

namespace JSC { namespace B3 {

namespace {

namespace B3InferSwitchesInternal {
static constexpr bool verbose = false;
}

class InferSwitches {
public:
    InferSwitches(Procedure& proc)
        : m_proc(proc)
        , m_insertionSet(proc)
    {
    }
    
    bool run()
    {
        if (B3InferSwitchesInternal::verbose)
            dataLog("B3 before inferSwitches:\n", m_proc);
        
        bool changed = true;
        bool everChanged = false;
        while (changed) {
            changed = false;
            
            if (B3InferSwitchesInternal::verbose)
                dataLog("Performing fixpoint iteration:\n");
            
            for (BasicBlock* block : m_proc)
                changed |= attemptToMergeWithPredecessor(block) || attemptToMergeComparisonThatIsOnlyValue(block);

            everChanged |= changed;
        }
        
        if (everChanged) {
            m_proc.resetReachability();
            m_proc.invalidateCFG();
            
            m_proc.deleteOrphans();
            
            if (B3InferSwitchesInternal::verbose)
                dataLog("B3 after inferSwitches:\n", m_proc);
            return true;
        }
        
        return false;
    }
    
private:
    bool attemptToMergeWithPredecessor(BasicBlock* block)
    {
        // No point in considering the root block. We also don't consider blocks with multiple
        // predecessors, but we could handle this if we made this code a bit more general and we were
        // not afraid of code bloat.
        if (block->numPredecessors() != 1)
            return false;
        
        SwitchDescription description = describe(block);
        if (B3InferSwitchesInternal::verbose)
            dataLog("Description of primary block ", *block, ": ", description, "\n");
        if (!description) {
            if (B3InferSwitchesInternal::verbose)
                dataLog("    Bailing because not switch-like.\n");
            return false;
        }
        
        // We know that this block behaves like a switch. But we need to verify that it doesn't also
        // perform any effects or do expensive things. We don't want to create a switch if that will
        // make expensive things execute unconditionally. We're very conservative about how we define
        // "expensive".
        for (Value* value : *block) {
            if (value->isFree())
                continue;
            if (value == description.extra)
                continue;
            if (value == description.branch)
                continue;
            if (B3InferSwitchesInternal::verbose)
                dataLog("    Bailing because of ", deepDump(m_proc, value), "\n");
            return false;
        }
        
        BasicBlock* predecessor = block->predecessor(0);
        if (m_proc.policy().optimization.stopsSwitchInferenceAtRareEdges) {
            for (const FrequentedBlock& successor : predecessor->successors()) {
                if (successor.block() == block && successor.isRare())
                    return false;
            }
        }
        if (SwitchValue* grown = predecessor->last()->as<SwitchValue>(); grown && grown->numCaseValues() >= Limits::minCasesToGrowSwitchInPlace) {
            switch (tryGrowInPlace(block, description, predecessor, grown)) {
            case GrowthOutcome::Merged:
                return true;
            case GrowthOutcome::NotMergeable:
                return false;
            case GrowthOutcome::UseGeneralPath:
                break;
            }
        }
        SwitchDescription predecessorDescription = describe(predecessor);
        if (B3InferSwitchesInternal::verbose)
            dataLog("    Description of predecessor block ", *predecessor, ": ", predecessorDescription, "\n");
        if (!predecessorDescription) {
            if (B3InferSwitchesInternal::verbose)
                dataLog("    Bailing because not switch-like.\n");
            return false;
        }
        
        // Both us and the predecessor are switch-like, but that doesn't mean that we're compatible.
        // We may be switching on different values!
        if (description.source != predecessorDescription.source) {
            if (B3InferSwitchesInternal::verbose)
                dataLog("    Bailing because sources don't match.\n");
            return false;
        }
        
        // We expect that we are the fall-through destination of the predecessor. This is a bit of a
        // goofy condition. If we were not the fall-through destination then our switch is probably
        // just totally redundant and we should be getting rid of it. But we don't handle that here,
        // yet.
        if (predecessorDescription.fallThrough.block() != block) {
            if (B3InferSwitchesInternal::verbose)
                dataLog("    Bailing because fall-through of predecessor is not the primary block.\n");
            return false;
        }
        
        // Make sure that there ain't no loops.
        if (description.fallThrough.block() == block
            || description.fallThrough.block() == predecessor) {
            if (B3InferSwitchesInternal::verbose)
                dataLog("    Bailing because of fall-through loop.\n");
            return false;
        }
        for (SwitchCase switchCase : description.cases) {
            if (switchCase.targetBlock() == block
                || switchCase.targetBlock() == predecessor) {
                if (B3InferSwitchesInternal::verbose)
                    dataLog("    Bailing because of loop in primary cases.\n");
                return false;
            }
        }
        for (SwitchCase switchCase : predecessorDescription.cases) {
            if (switchCase.targetBlock() == block
                || switchCase.targetBlock() == predecessor) {
                if (B3InferSwitchesInternal::verbose)
                    dataLog("    Bailing because of loop in predecessor cases.\n");
                return false;
            }
        }
        
        if (B3InferSwitchesInternal::verbose)
            dataLog("    Doing it!\n");
        // We're committed to doing the thing.
        if (!m_useCounts)
            m_useCounts.emplace(m_proc);
        m_grown = nullptr;

        // Delete the extra value from the predecessor, since that would break downstream inference
        // on the next fixpoint iteration. We would think that this block is too expensive to merge
        // because of the Equal or NotEqual value even though that value is dead! We know it's dead
        // so we kill it ourselves.
        for (Value* value : *predecessor) {
            if (value == predecessorDescription.extra)
                value->replaceWithNopIgnoringType();
        }
        
        // Insert all non-terminal values from our block into our predecessor. We definitely need to
        // do this for constants. We must not do it for the extra value, since that would break
        // downstream inference on the next fixpoint iteration. As a bonus, we don't do it for nops,
        // so that we limit how big blocks get in this phase.
        for (unsigned i = 0; i < block->size() - 1; ++i) {
            Value* value = block->at(i);
            if (value != description.extra && value->opcode() != Nop)
                m_insertionSet.insertValue(predecessor->size() - 1, value);
        }
        m_insertionSet.execute(predecessor);
        block->values().shrink(0);
        block->appendNew<Value>(m_proc, Oops, description.branch->origin());
        block->removePredecessor(predecessor);
        
        for (BasicBlock* successorBlock : description.block->successorBlocks())
            successorBlock->replacePredecessor(block, predecessor);

        block->clearSuccessors();
        
        SwitchValue* switchValue = predecessor->replaceLastWithNew<SwitchValue>(
            m_proc, predecessor->last()->origin(), description.source);
        predecessor->clearSuccessors();
        switchValue->setFallThrough(description.fallThrough);
        
        Vector<int64_t> predecessorCases;
        for (SwitchCase switchCase : predecessorDescription.cases) {
            switchValue->appendCase(switchCase);
            predecessorCases.append(switchCase.caseValue());
        }
        std::ranges::sort(predecessorCases);
        auto isPredecessorCase = [&] (int64_t value) -> bool {
            return !!tryBinarySearch<int64_t>(
                predecessorCases, predecessorCases.size(), value,
                [] (int64_t* element) -> int64_t { return *element; });
        };
        
        for (SwitchCase switchCase : description.cases) {
            if (!isPredecessorCase(switchCase.caseValue()))
                switchValue->appendCase(switchCase);
        }
        return true;
    }

    // a == 1 || a == 2 || a == 3 || a == 4 as a value: three branches to a block that says true, and where none is taken the last comparison is the value.
    // The last comparison becomes a case that leads to that block too, where the cases lie close enough for one test of a bit.
    bool attemptToMergeComparisonThatIsOnlyValue(BasicBlock* block)
    {
        if (!m_proc.policy().switchLowering.usesBitTests || block->numPredecessors() != 1 || block->last()->opcode() != Jump)
            return false;
        BasicBlock* predecessor = block->predecessor(0);
        SwitchValue* switchValue = predecessor->last()->as<SwitchValue>();
        if (!switchValue || predecessor->fallThrough().block() != block || switchValue->numCaseValues() + 1 < Limits::minCasesForBitTests)
            return false;

        UpsilonValue* upsilon = nullptr;
        for (Value* value : *block) {
            if (value->isFree() || value == block->last())
                continue;
            if (auto* candidate = value->as<UpsilonValue>(); candidate && !upsilon) {
                upsilon = candidate;
                continue;
            }
            if ((value->opcode() == Equal || value->opcode() == NotEqual) && value->child(0) == switchValue->child(0) && value->child(1)->hasInt())
                continue;
            return false;
        }
        if (!upsilon)
            return false;
        Value* comparison = upsilon->child(0);
        if ((comparison->opcode() != Equal && comparison->opcode() != NotEqual) || comparison->child(0) != switchValue->child(0) || !comparison->child(1)->hasInt() || useCounts().numUses(comparison) != 1)
            return false;
        if (!block->values().contains(comparison))
            return false;

        int32_t valueOfMatch = comparison->opcode() == Equal;
        int64_t matched = comparison->child(1)->asInt();
        int64_t smallest = matched;
        int64_t largest = matched;
        std::optional<FrequentedBlock> target;
        for (SwitchCase switchCase : switchValue->cases(predecessor)) {
            if (switchCase.caseValue() == matched)
                return false;
            smallest = std::min(smallest, switchCase.caseValue());
            largest = std::max(largest, switchCase.caseValue());
            if (!target && saysOnly(switchCase.targetBlock(), upsilon->phi(), valueOfMatch, block->successorBlock(0)))
                target = switchCase.target();
        }
        if (!target || static_cast<uint64_t>(largest) - static_cast<uint64_t>(smallest) >= 64)
            return false;

        m_grown = nullptr;
        Value* otherwise = m_proc.add<Const32Value>(comparison->origin(), !valueOfMatch);
        block->values().insert(0, otherwise);
        otherwise->owner = block;
        upsilon->child(0) = otherwise;
        comparison->replaceWithNopIgnoringType();
        switchValue->appendCase(predecessor, SwitchCase(matched, *target));
        return true;
    }

    static bool saysOnly(BasicBlock* block, Value* phi, int32_t constant, BasicBlock* successor)
    {
        if (block->last()->opcode() != Jump || block->successorBlock(0) != successor)
            return false;
        bool says = false;
        for (Value* value : *block) {
            if (value->isFree() || value == block->last())
                continue;
            auto* upsilon = value->as<UpsilonValue>();
            if (!upsilon || says || upsilon->phi() != phi || !upsilon->child(0)->isInt32(constant))
                return false;
            says = true;
        }
        return says;
    }

    struct SwitchDescription {
        SwitchDescription()
        {
        }
        
        explicit operator bool() { return !!block; }
        
        void dump(PrintStream& out) const
        {
            out.print(
                "{block = ", pointerDump(block),
                ", branch = ", pointerDump(branch),
                ", extra = ", pointerDump(extra),
                ", source = ", pointerDump(source),
                ", cases = ", listDump(cases),
                ", fallThrough = ", fallThrough, "}");
        }

        BasicBlock* block { nullptr };
        Value* branch { nullptr };
        Value* extra { nullptr }; // This is the Equal or NotEqual value, if applicable.
        Value* source { nullptr };
        Vector<SwitchCase, 1> cases;
        FrequentedBlock fallThrough;
    };
    
    enum class GrowthOutcome : uint8_t { UseGeneralPath, NotMergeable, Merged };

    GrowthOutcome tryGrowInPlace(BasicBlock* block, SwitchDescription& description, BasicBlock* predecessor, SwitchValue* grown)
    {
        auto isRepresentableCase = [](int64_t value) {
            return value != std::numeric_limits<int64_t>::min() && value != std::numeric_limits<int64_t>::max();
        };
        if (m_grown != grown || m_numberOfGrownCases != grown->numCaseValues()) {
            m_grown = nullptr;
            m_grownValues.clear();
            m_grownTargets = IndexSet<BasicBlock*>();
            for (SwitchCase switchCase : grown->cases(predecessor)) {
                if (!isRepresentableCase(switchCase.caseValue()))
                    return GrowthOutcome::UseGeneralPath;
                m_grownValues.add(switchCase.caseValue());
                m_grownTargets.add(switchCase.targetBlock());
            }
            m_grown = grown;
            m_numberOfGrownCases = grown->numCaseValues();
        }
        for (SwitchCase switchCase : description.cases) {
            if (!isRepresentableCase(switchCase.caseValue()))
                return GrowthOutcome::UseGeneralPath;
        }
        if (description.source != grown->child(0) || predecessor->fallThrough().block() != block)
            return GrowthOutcome::NotMergeable;
        if (description.fallThrough.block() == block || description.fallThrough.block() == predecessor)
            return GrowthOutcome::NotMergeable;
        for (SwitchCase switchCase : description.cases) {
            if (switchCase.targetBlock() == block || switchCase.targetBlock() == predecessor)
                return GrowthOutcome::NotMergeable;
        }
        if (m_grownTargets.contains(block) || m_grownTargets.contains(predecessor))
            return GrowthOutcome::NotMergeable;

        useCounts();
        auto& values = predecessor->values();
        for (unsigned i = 0; i < block->size() - 1; ++i) {
            Value* value = block->at(i);
            if (value == description.extra || value->opcode() == Nop)
                continue;
            values.append(values.last());
            values[values.size() - 2] = value;
        }
        block->values().shrink(0);
        block->appendNew<Value>(m_proc, Oops, description.branch->origin());
        block->removePredecessor(predecessor);
        for (BasicBlock* successorBlock : description.block->successorBlocks())
            successorBlock->replacePredecessor(block, predecessor);
        block->clearSuccessors();
        grown->owner = predecessor;
        grown->setFallThrough(predecessor, description.fallThrough);
        for (SwitchCase switchCase : description.cases) {
            if (!m_grownValues.add(switchCase.caseValue()).isNewEntry)
                continue;
            grown->appendCase(predecessor, switchCase);
            m_grownTargets.add(switchCase.targetBlock());
        }
        m_numberOfGrownCases = grown->numCaseValues();
        return GrowthOutcome::Merged;
    }

    SwitchDescription describe(BasicBlock* block)
    {
        SwitchDescription result;
        result.block = block;
        result.branch = block->last();
        
        switch (result.branch->opcode()) {
        case Branch: {
            Value* predicate = result.branch->child(0);
            FrequentedBlock taken = result.block->taken();
            FrequentedBlock notTaken = result.block->notTaken();
            bool handled = false;
            // NOTE: This uses UseCounts that we computed before any transformation. This is fine
            // because although we may have mutated the IR, we would not have added any new
            // predicates.
            if (predicate->numChildren() == 2
                && predicate->child(1)->hasInt()
                && useCounts().numUses(predicate) == 1) {
                switch (predicate->opcode()) {
                case Equal:
                    result.source = predicate->child(0);
                    result.extra = predicate;
                    result.cases.append(SwitchCase(predicate->child(1)->asInt(), taken));
                    result.fallThrough = notTaken;
                    handled = true;
                    break;
                case NotEqual:
                    result.source = predicate->child(0);
                    result.extra = predicate;
                    result.cases.append(SwitchCase(predicate->child(1)->asInt(), notTaken));
                    result.fallThrough = taken;
                    handled = true;
                    break;
                default:
                    break;
                }
            }
            if (handled)
                break;
            result.source = predicate;
            result.cases.append(SwitchCase(0, notTaken));
            result.fallThrough = taken;
            break;
        }
            
        case Switch: {
            SwitchValue* switchValue = result.branch->as<SwitchValue>();
            result.source = switchValue->child(0);
            for (SwitchCase switchCase : switchValue->cases(result.block))
                result.cases.append(switchCase);
            result.fallThrough = result.block->fallThrough();
            break;
        }
            
        default:
            result.block = nullptr;
            result.branch = nullptr;
            break;
        }
        
        return result;
    }
    
    Procedure& m_proc;
    InsertionSet m_insertionSet;
    UseCountsWithoutUsingInstructions& useCounts()
    {
        if (!m_useCounts)
            m_useCounts.emplace(m_proc);
        return *m_useCounts;
    }

    std::optional<UseCountsWithoutUsingInstructions> m_useCounts;
    SwitchValue* m_grown { nullptr };
    unsigned m_numberOfGrownCases { 0 };
    UncheckedKeyHashSet<int64_t, IntHash<int64_t>, WTF::SignedWithZeroKeyHashTraits<int64_t>> m_grownValues;
    IndexSet<BasicBlock*> m_grownTargets;
};

} // anonymous namespace

bool inferSwitches(Procedure& proc)
{
    PhaseScope phaseScope(proc, "inferSwitches"_s);
    InferSwitches inferSwitches(proc);
    return inferSwitches.run();
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)

