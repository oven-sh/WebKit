/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3RegionCloner.h"

#if ENABLE(B3_JIT)

#include "B3FixSSA.h"
#include "B3Procedure.h"
#include "B3UpsilonValue.h"
#include "B3ValueInlines.h"

namespace JSC { namespace B3 {

RegionCloner::RegionCloner(Procedure& proc)
    : m_proc(proc)
{
}

void RegionCloner::addBlock(BasicBlock* block)
{
    RELEASE_ASSERT(m_step == Step::Declaring);
    RELEASE_ASSERT(block->last()->type() == Void);
    m_sources.add(block);
    m_declaredBlocks.add(block);
}

void RegionCloner::demoteEscapingValues(PhiTreatment treatment, ValueOwners owners)
{
    RELEASE_ASSERT(m_step == Step::Declaring);
    m_step = Step::Copying;
    m_phiTreatment = treatment;
    if (m_declaredBlocks.isEmpty())
        return;

    if (owners == ValueOwners::MayBeStale)
        m_proc.resetValueOwners();

    IndexSet<Value*> valuesToDemote;
    for (BasicBlock* block : m_proc) {
        for (Value* value : *block) {
            switch (treatment) {
            case PhiTreatment::Demote:
                if (value->opcode() == Phi && m_declaredBlocks.contains(block))
                    valuesToDemote.add(value);
                break;
            case PhiTreatment::KeepIfJumpFed: {
                auto* upsilon = value->as<UpsilonValue>();
                if (upsilon && m_declaredBlocks.contains(upsilon->phi()->owner) && (block->last()->opcode() != Jump || block->successorBlock(0) != upsilon->phi()->owner))
                    valuesToDemote.add(upsilon->phi());
                break;
            }
            }
            for (Value* child : value->children()) {
                if (child->owner != block && m_declaredBlocks.contains(child->owner))
                    valuesToDemote.add(child);
            }
        }
    }
    if (valuesToDemote.isEmpty())
        return;

    demoteValues(m_proc, valuesToDemote);
    m_didChange = true;
}

Value* RegionCloner::appendClone(Value* original, BasicBlock* destination, ValueMap& map)
{
    RELEASE_ASSERT(m_step == Step::Copying);
    RELEASE_ASSERT(original->opcode() != Phi);
    RELEASE_ASSERT(m_declaredBlocks.contains(original->owner));
    ASSERT(!original->kind().isCloningForbidden());

    Value* clone = m_proc.clone(original);
    for (Value*& child : clone->children()) {
        if (Value* replacement = map.get(child))
            child = replacement;
        else
            RELEASE_ASSERT(child->opcode() != Phi || child->owner != original->owner);
    }
    if (original->type() != Void)
        map.add(original, clone);
    destination->append(clone);
    m_didChange = true;
    return clone;
}

void RegionCloner::substitute(ValueMap& map, Value* original, Value* replacement)
{
    RELEASE_ASSERT(m_step == Step::Copying);
    ASSERT(original->type() == replacement->type());
    map.set(original, replacement);
}

void RegionCloner::substitutePhisFedBy(BasicBlock* predecessor, BasicBlock* block, ValueMap& map)
{
    RELEASE_ASSERT(m_step == Step::Copying);
    for (Value* value : *predecessor) {
        auto* upsilon = value->as<UpsilonValue>();
        if (!upsilon || upsilon->phi()->owner != block)
            continue;
        map.set(upsilon->phi(), upsilon->child(0));
        upsilon->replaceWithNop();
        m_didChange = true;
    }
}

RegionCloner::BlockMap RegionCloner::cloneBlocks(std::span<BasicBlock* const> blocks)
{
    RELEASE_ASSERT(m_step == Step::Copying);
    RELEASE_ASSERT(m_phiTreatment == PhiTreatment::Demote);

    BlockMap copies;
    for (BasicBlock* block : blocks) {
        auto result = copies.add(block, m_proc.addBlock());
        RELEASE_ASSERT(result.isNewEntry);
    }
    for (BasicBlock* block : blocks) {
        BasicBlock* copy = copies.get(block);
        ValueMap map;
        for (Value* value : *block)
            appendClone(value, copy, map);
        copy->successors() = block->successors();
        for (BasicBlock*& successor : copy->successorBlocks()) {
            if (BasicBlock* successorCopy = copies.get(successor))
                successor = successorCopy;
        }
    }
    return copies;
}

bool RegionCloner::finish()
{
    RELEASE_ASSERT(m_step != Step::Finished);
    m_step = Step::Finished;
    if (!m_didChange)
        return false;

    m_proc.resetReachability();
    m_proc.invalidateCFG();
    return true;
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
