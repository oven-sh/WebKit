/*
 * Copyright (C) 2015-2019 Apple Inc. All rights reserved.
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
#include "B3EliminateDeadCode.h"

#if ENABLE(B3_JIT)

#include "B3BasicBlock.h"
#include "B3PhaseScope.h"
#include "B3ValueInlines.h"
#include "B3Variable.h"
#include "B3VariableValue.h"
#include <wtf/GraphNodeWorklist.h>
#include <wtf/IndexSet.h>
#include <wtf/Vector.h>

namespace JSC { namespace B3 {

// FIXME: this pass currently only eliminates values and variables, it does not seem to eliminate dead blocks.

#if USE(BUN_JSC_ADDITIONS)
static ALWAYS_INLINE bool mustExecuteWhereItStands(Value* value, Opcode opcode)
{
    switch (opcode) {
    case Load8Z:
    case Load8S:
    case Load16Z:
    case Load16S:
    case Load:
        return static_cast<MemoryValue*>(value)->hasFence() || value->traps();
    case Store8:
    case Store16:
    case Store:
        return static_cast<MemoryValue*>(value)->range() || static_cast<MemoryValue*>(value)->hasFence() || value->traps();
    case Patchpoint:
        return static_cast<PatchpointValue*>(value)->effects.mustExecute();
    case CCall:
        return static_cast<CCallValue*>(value)->effects.mustExecute();
    default:
        return value->mustExecute();
    }
}
#endif

#if USE(BUN_JSC_ADDITIONS)
bool eliminateDeadCodeImpl(Procedure& proc, Vector<UpsilonValue*>* liveUpsilons)
#else
bool eliminateDeadCodeImpl(Procedure& proc)
#endif
{
    bool changed = false;
#if USE(BUN_JSC_ADDITIONS)
    enum State : uint8_t { NotReached, Wanted, PassedAsDead, Live };
    struct {
        bool saw(Value* value) const { return states[value->index()] == Live; }

        Vector<uint8_t, 1024> states;
    } worklist;
    worklist.states.fill(NotReached, proc.values().size());
    uint8_t* states = worklist.states.mutableSpan().data();
    Vector<Value*, 64> stack;
    Vector<UpsilonValue*, 64> upsilons;
    Vector<BasicBlock*, 64> blocksThatMayHoldDeadValues;
    auto want = [&](Value* value) ALWAYS_INLINE_LAMBDA {
        uint8_t& state = states[value->index()];
        if (state == NotReached)
            state = Wanted;
        else if (state == PassedAsDead) {
            state = Live;
            stack.append(value);
        }
    };
    for (unsigned blockIndex = proc.size(); blockIndex--;) {
        BasicBlock* block = proc[blockIndex];
        if (!block)
            continue;
        bool mayHoldDeadValues = false;
        Value* const* values = block->values().span().data();
        for (unsigned index = block->size(); index--;) {
            Value* value = values[index];
            uint8_t& state = states[value->index()];
            if (state != Wanted) {
                Opcode opcode = value->opcode();
                bool isLive = false;
                if (opcode == Upsilon) {
                    uint8_t stateOfPhi = states[static_cast<UpsilonValue*>(value)->phi()->index()];
                    isLive = stateOfPhi == Wanted || stateOfPhi == Live;
                    if (!isLive)
                        upsilons.append(static_cast<UpsilonValue*>(value));
                    else if (liveUpsilons)
                        liveUpsilons->append(static_cast<UpsilonValue*>(value));
                } else if (opcode != Phi)
                    isLive = mustExecuteWhereItStands(value, opcode);
                if (!isLive) {
                    state = PassedAsDead;
                    mayHoldDeadValues = true;
                    continue;
                }
            }
            state = Live;
            for (Value* child : value->children())
                want(child);
        }
        if (mayHoldDeadValues)
            blocksThatMayHoldDeadValues.append(block);
    }
    for (;;) {
        while (!stack.isEmpty()) {
            for (Value* child : stack.takeLast()->children())
                want(child);
        }

        bool didPush = false;
        for (size_t upsilonIndex = 0; upsilonIndex < upsilons.size(); ++upsilonIndex) {
            UpsilonValue* upsilon = upsilons[upsilonIndex];
            uint8_t stateOfPhi = states[upsilon->phi()->index()];
            if (stateOfPhi == Wanted || stateOfPhi == Live) {
                want(upsilon);
                if (liveUpsilons)
                    liveUpsilons->append(upsilon);
                upsilons[upsilonIndex--] = upsilons.last();
                upsilons.takeLast();
                didPush = true;
            }
        }
        if (!didPush)
            break;
    }
#else
    GraphNodeWorklist<Value*, IndexSet<Value*>> worklist;
    Vector<UpsilonValue*, 128> upsilons;
    for (BasicBlock* block : proc) {
        for (Value* value : *block) {
            // We don't care about effects of SSA operations, since we model them more
            // accurately than the effects() method does.
            if (value->opcode() != Phi && value->opcode() != Upsilon) {
                if (value->mustExecute())
                    worklist.push(value);
            }

            if (UpsilonValue* upsilon = value->as<UpsilonValue>())
                upsilons.append(upsilon);
        }
    }
    for (;;) {
        while (Value* value = worklist.pop()) {
            for (Value* child : value->children())
                worklist.push(child);
        }
        
        bool didPush = false;
        for (size_t upsilonIndex = 0; upsilonIndex < upsilons.size(); ++upsilonIndex) {
            UpsilonValue* upsilon = upsilons[upsilonIndex];
            if (worklist.saw(upsilon->phi())) {
                worklist.push(upsilon);
                upsilons[upsilonIndex--] = upsilons.last();
                upsilons.takeLast();
                didPush = true;
            }
        }
        if (!didPush)
            break;
    }
#endif

#if USE(BUN_JSC_ADDITIONS)
    if (proc.variables().isEmpty()) {
        for (BasicBlock* block : blocksThatMayHoldDeadValues | std::views::reverse) {
            size_t sourceIndex = 0;
            size_t targetIndex = 0;
            size_t size = block->size();
            Value** values = block->values().mutableSpan().data();
            while (sourceIndex < size) {
                Value* value = values[sourceIndex++];
                if (states[value->index()] == Live)
                    values[targetIndex++] = value;
                else {
                    proc.deleteValue(value);
                    changed = true;
                }
            }
            block->values().shrink(targetIndex);
        }
        return changed;
    }
#endif

    IndexSet<Variable*> liveVariables;
    
    for (BasicBlock* block : proc) {
        size_t sourceIndex = 0;
        size_t targetIndex = 0;
        while (sourceIndex < block->size()) {
            Value* value = block->at(sourceIndex++);
            if (worklist.saw(value)) {
                if (VariableValue* variableValue = value->as<VariableValue>())
                    liveVariables.add(variableValue->variable());
                block->at(targetIndex++) = value;
            } else {
                proc.deleteValue(value);
                changed = true;
            }
        }
        block->values().shrink(targetIndex);
    }

    if (liveVariables.isEmpty())
        proc.deleteAllVariables();
    else {
        for (Variable* variable : proc.variables()) {
            if (!liveVariables.contains(variable))
                proc.deleteVariable(variable);
        }
    }

    return changed;
}

bool eliminateDeadCode(Procedure& proc)
{
    PhaseScope phaseScope(proc, "eliminateDeadCode"_s);
    return eliminateDeadCodeImpl(proc);
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
