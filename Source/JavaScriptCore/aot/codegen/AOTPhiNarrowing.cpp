/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPhiNarrowing.h"

#if ENABLE(AOT)

#include "AOTVector.h"
#include "B3BasicBlockInlines.h"
#include "B3Const32Value.h"
#include "B3InsertionSetInlines.h"
#include "B3Procedure.h"
#include "B3UpsilonValue.h"
#include "B3ValueInlines.h"
#include <wtf/BitVector.h>
#include <wtf/SharedTask.h>

namespace JSC { namespace AOT {

static bool isExtension(B3::Value* value)
{
    return value->opcode() == B3::SExt32 || value->opcode() == B3::ZExt32;
}

bool narrowTruncatedPhis(B3::Procedure& proc)
{
    BitVector isCandidate;
    for (B3::Value* value : proc.values()) {
        if (auto* upsilon = value->as<B3::UpsilonValue>(); upsilon && isExtension(upsilon->child(0)))
            isCandidate.set(upsilon->phi()->index());
    }
    if (isCandidate.isEmpty())
        return false;

    BitVector hasTruncation;
    for (B3::Value* value : proc.values()) {
        for (B3::Value* child : value->children()) {
            if (!isCandidate.get(child->index()))
                continue;
            if (value->opcode() == B3::Trunc)
                hasTruncation.set(child->index());
            else
                isCandidate.clear(child->index());
        }
    }
    isCandidate.filter(hasTruncation);
    if (isCandidate.isEmpty())
        return false;

    Vector<B3::Value*> narrowPhis;
    narrowPhis.fill(nullptr, proc.values().size());
    B3::InsertionSet insertionSet(proc);
    for (B3::BasicBlock* block : proc) {
        for (unsigned index = 0; index < block->size(); ++index) {
            B3::Value* value = block->at(index);
            if (value->opcode() == B3::Phi && isCandidate.get(value->index()))
                narrowPhis[value->index()] = insertionSet.insert<B3::Value>(index, B3::Phi, B3::Int32, value->origin());
        }
        insertionSet.execute(block);
    }

    for (B3::BasicBlock* block : proc) {
        for (unsigned index = 0; index < block->size(); ++index) {
            B3::Value* value = block->at(index);
            if (auto* upsilon = value->as<B3::UpsilonValue>()) {
                B3::Value* narrowPhi = upsilon->phi()->index() < narrowPhis.size() ? narrowPhis[upsilon->phi()->index()] : nullptr;
                if (!narrowPhi)
                    continue;
                B3::Value* input = upsilon->child(0);
                B3::Value* narrowInput = nullptr;
                if (isExtension(input))
                    narrowInput = input->child(0);
                else if (input->hasInt64())
                    narrowInput = insertionSet.insert<B3::Const32Value>(index, upsilon->origin(), static_cast<int32_t>(input->asInt64()));
                else
                    narrowInput = insertionSet.insert<B3::Value>(index, B3::Trunc, upsilon->origin(), input);
                insertionSet.insert<B3::UpsilonValue>(index, upsilon->origin(), narrowInput, narrowPhi);
                upsilon->replaceWithNop();
                continue;
            }
            if (value->opcode() != B3::Trunc || value->child(0)->index() >= narrowPhis.size())
                continue;
            if (B3::Value* narrowPhi = narrowPhis[value->child(0)->index()])
                value->replaceWithIdentity(narrowPhi);
        }
        insertionSet.execute(block);
    }
    for (B3::Value* value : proc.values()) {
        value->performSubstitution();
        if (value->opcode() == B3::Phi && value->index() < narrowPhis.size() && narrowPhis[value->index()])
            value->replaceWithNopIgnoringType();
    }
    return true;
}

B3::ClientPhase narrowTruncatedPhisAfterOptimization()
{
    return {
        B3::ClientPhase::Point::AfterOptimization,
        "narrowTruncatedPhis"_s,
        createSharedTask<bool(B3::Procedure&)>([](B3::Procedure& proc) {
            return narrowTruncatedPhis(proc);
        })
    };
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
